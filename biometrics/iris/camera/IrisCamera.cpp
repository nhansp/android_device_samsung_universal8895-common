/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "IrisCamera"

#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>
#include <cutils/sockets.h>
#include <hardware/camera.h>
#include <hardware/camera_common.h>

#include <iris/CameraProtocol.h>

using android::base::unique_fd;
using namespace iris_camera;

namespace {

constexpr char kModulePath[] = "/vendor/lib/hw/camera.universal8895.so";
// The camera module opens the secure iris sensor for id "90" through its HAL1 path
// (HAL_open_legacy maps 90 to its internal secure camera).
constexpr char kIrisCameraId[] = "90";
constexpr char kFrontCameraId[] = "1";
// libexynoscamera's secure vision thread delivers each IR frame as this message: an 8-byte
// buffer holding kFrameMagic and the frame's dma-buf fd, valid in this process.
constexpr int32_t kMsgIrisFrame = 0x10000;
constexpr uint32_t kFrameMagic = 0x00fd42fd;
// Stock SemCamera.setIrisPreviewDataCallbackEnabled() -> sendCommand(1832), which the stock
// 32-bit cameraserver turns into this HAL message type on the front camera.
constexpr int32_t kMsgIrisPreviewData = 0x8000;
constexpr int32_t kCmdIrisPreviewData = 1832;
// Stock libcameraservice sets system.camera.CC.disable for com.samsung.android.server.iris
// ("setCompanionDisableSysFs", "is Need DisableCC") before the camera HAL opens camera 1, so
// libexynoscamera reports use_companion(0) and opens the direct 3H1 (sensor mode 8,
// 752x1328@15); the C3 companion has no mode for that size. The blob reads it under the vendor
// name (extract-files.py).
constexpr char kCompanionDisableProp[] = "vendor.camera.CC.disable";

std::atomic<int> gClient{-1};

camera_module_t* Module() {
    static camera_module_t* const module = []() -> camera_module_t* {
        void* handle = dlopen(kModulePath, RTLD_NOW);
        if (!handle) {
            LOG(ERROR) << "Failed to load " << kModulePath << ": " << dlerror();
            return nullptr;
        }
        auto* hmi = static_cast<camera_module_t*>(dlsym(handle, HAL_MODULE_INFO_SYM_AS_STR));
        if (!hmi || !hmi->open_legacy) {
            LOG(ERROR) << kModulePath << " has no HAL1 open";
            return nullptr;
        }
        hmi->common.dso = handle;
        // The module reports torch availability through these on every open.
        static const camera_module_callbacks_t callbacks = {
                .camera_device_status_change = [](const camera_module_callbacks*, int, int) {},
                .torch_mode_status_change = [](const camera_module_callbacks*, const char*,
                                               int) {},
        };
        hmi->set_callbacks(&callbacks);
        return hmi;
    }();
    return module;
}

bool Send(int fd, uint32_t type, const void* data, uint32_t len, int passFd = -1) {
    IrisCameraMsg hdr = {type, len};
    iovec iov[2] = {{&hdr, sizeof(hdr)}, {const_cast<void*>(data), len}};
    msghdr msg = {};
    msg.msg_iov = iov;
    msg.msg_iovlen = len ? 2 : 1;
    char control[CMSG_SPACE(sizeof(int))];
    if (passFd >= 0) {
        msg.msg_control = control;
        msg.msg_controllen = sizeof(control);
        cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cmsg), &passFd, sizeof(int));
    }
    return TEMP_FAILURE_RETRY(sendmsg(fd, &msg, MSG_NOSIGNAL | MSG_DONTWAIT)) >= 0;
}

// CameraParameters is not available to vendor code; these keep its flattened format.
std::map<std::string, std::string> Unflatten(const std::string& flat) {
    std::map<std::string, std::string> params;
    for (const auto& item : android::base::Split(flat, ";")) {
        size_t eq = item.find('=');
        if (eq != std::string::npos) params[item.substr(0, eq)] = item.substr(eq + 1);
    }
    return params;
}

std::string Flatten(const std::map<std::string, std::string>& params) {
    std::vector<std::string> items;
    for (const auto& [key, value] : params) items.push_back(key + "=" + value);
    return android::base::Join(items, ";");
}

void ReleaseMemory(camera_memory_t* mem) {
    if (mem->handle) {
        munmap(mem->data, mem->size);
    } else {
        free(mem->data);
    }
    delete mem;
}

camera_memory_t* GetMemory(int fd, size_t size, unsigned int count, void*) {
    auto* mem = new camera_memory_t{nullptr, size * count, nullptr, ReleaseMemory};
    if (fd >= 0) {
        void* data = mmap(nullptr, mem->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (data == MAP_FAILED) {
            PLOG(ERROR) << "mmap of a " << mem->size << " byte camera buffer";
            delete mem;
            return nullptr;
        }
        mem->data = data;
        mem->handle = mem;
    } else {
        mem->data = calloc(1, mem->size);
    }
    return mem;
}

// One opened camera; the camera HAL hands this back as the callbacks' user pointer.
struct Camera {
    const char* id;
    bool ir;
    camera_device_t* device = nullptr;
    std::mutex seenLock;
    std::set<int32_t> seen;

    void logFirst(int32_t msgType, const camera_memory_t* mem) {
        std::lock_guard<std::mutex> lock(seenLock);
        if (!seen.insert(msgType).second) return;
        const auto* words = mem && mem->size >= 8 ? static_cast<const uint32_t*>(mem->data)
                                                  : nullptr;
        LOG(INFO) << "Camera " << id << " first message 0x" << std::hex << msgType << std::dec
                  << " size " << (mem ? mem->size : 0) << " words "
                  << (words ? android::base::StringPrintf("%08x %08x", words[0], words[1])
                            : std::string("-"));
    }
};


void Notify(int32_t msgType, int32_t ext1, int32_t ext2, void* user) {
    auto* camera = static_cast<Camera*>(user);
    if (msgType != CAMERA_MSG_ERROR) return;
    LOG(ERROR) << "Camera " << camera->id << " error " << ext1 << " " << ext2;
    // Without the IR camera there is no capture; the front camera only adds the preview.
    if (!camera->ir) return;
    int client = gClient.load();
    if (client >= 0) Send(client, MSG_ERROR, &ext1, sizeof(ext1));
}

// Returns the fd of an iris data message ({kFrameMagic, fd}), or -1.
int IrisDataFd(const Camera* camera, const camera_memory_t* mem) {
    if (!mem || mem->size < 2 * sizeof(uint32_t)) return -1;
    const uint32_t* payload = static_cast<const uint32_t*>(mem->data);
    if (payload[0] != kFrameMagic) {
        LOG(WARNING) << "Camera " << camera->id << ": unexpected iris data " << payload[0];
        return -1;
    }
    return static_cast<int>(payload[1]);
}

void Data(int32_t msgType, const camera_memory_t* mem, unsigned int /*index*/,
          camera_frame_metadata_t*, void* user) {
    auto* camera = static_cast<Camera*>(user);
    camera->logFirst(msgType, mem);
    int client = gClient.load();
    // A frame the face HAL is too slow for is dropped: the buffer goes back to the camera.
    if (client < 0) return;

    if (camera->ir) {
        int fd = msgType == kMsgIrisFrame ? IrisDataFd(camera, mem) : -1;
        if (fd >= 0) Send(client, MSG_FRAME, nullptr, 0, fd);
        return;
    }
    // The iris library turns this into the preview (IMAGE_PROCESSED id 1); the display frames
    // of the front camera are not used.
    if (msgType == kMsgIrisPreviewData) {
        int fd = IrisDataFd(camera, mem);
        if (fd >= 0) Send(client, MSG_FRONT_FRAME, nullptr, 0, fd);
    }
}

void DataTimestamp(int64_t, int32_t, const camera_memory_t*, unsigned int, void*) {}

// Opens a camera with the parameters below merged into its defaults; returns the parameters.
bool Open(Camera* camera, const std::map<std::string, std::string>& overrides,
          std::map<std::string, std::string>* params) {
    camera_module_t* module = Module();
    int ret = module ? module->open_legacy(&module->common, camera->id,
                                           CAMERA_DEVICE_API_VERSION_1_0,
                                           reinterpret_cast<hw_device_t**>(&camera->device))
                     : -ENODEV;
    if (ret || !camera->device) {
        LOG(ERROR) << "Failed to open camera " << camera->id << ": " << ret;
        camera->device = nullptr;
        return false;
    }
    camera_device_t* device = camera->device;
    device->ops->set_callbacks(device, Notify, Data, DataTimestamp, GetMemory, camera);
    char* flat = device->ops->get_parameters(device);
    *params = Unflatten(flat ? flat : "");
    if (device->ops->put_parameters) {
        device->ops->put_parameters(device, flat);
    } else {
        free(flat);
    }
    for (const auto& [key, value] : overrides) (*params)[key] = value;
    ret = device->ops->set_parameters(device, Flatten(*params).c_str());
    if (ret) LOG(ERROR) << "Camera " << camera->id << " set_parameters: " << ret;
    return true;
}

void Close(Camera* camera, int32_t msgTypes) {
    camera_device_t* device = camera->device;
    if (!device) return;
    device->ops->stop_preview(device);
    device->ops->disable_msg_type(device, msgTypes);
    device->ops->release(device);
    device->common.close(&device->common);
    camera->device = nullptr;
}

// Stock IRV1Controller.startFrontCamera(): the front camera runs before the IR camera, in
// the iris shot mode, without a display, and hands the iris library its own data callback.
void StartFront(Camera* front) {
    std::map<std::string, std::string> params;
    android::base::SetProperty(kCompanionDisableProp, "1");
    bool opened = Open(front, {{"focus-mode", "fixed-face"},
                      {"no-display-mode", "1"},
                      {"preview-size", std::to_string(kFrontWidth) + "x" +
                                               std::to_string(kFrontHeight)},
                      {"preview-fps-range", "15000,15000"},
                      {"shot-mode", "39"}},
              &params);
    if (!opened) return;
    front->device->ops->enable_msg_type(front->device, kMsgIrisPreviewData);
    int ret = front->device->ops->start_preview(front->device);
    if (ret) {
        LOG(ERROR) << "Front camera start_preview: " << ret;
        Close(front, kMsgIrisPreviewData);
        return;
    }
    // Stock IRV1Controller sets the iris data callback after startPreview().
    ret = front->device->ops->send_command(front->device, kCmdIrisPreviewData, 1, 0);
    if (ret) LOG(WARNING) << "Front camera command " << kCmdIrisPreviewData << ": " << ret;
}

void Serve(int client) {
    Camera front{.id = kFrontCameraId, .ir = false};
    Camera iris{.id = kIrisCameraId, .ir = true};
    gClient.store(client);
    StartFront(&front);

    // Stock IRV1Controller.startIRCamera()
    std::map<std::string, std::string> params;
    if (!Open(&iris,
              {{"preview-size", std::to_string(kWidth) + "x" + std::to_string(kHeight)},
               {"no-display-mode", "1"},
               {"shot-mode", "39"}},
              &params)) {
        int32_t err = -ENODEV;
        Send(client, MSG_ERROR, &err, sizeof(err));
    } else {
        std::string flattened = Flatten(params);
        Send(client, MSG_PARAMS, flattened.data(), flattened.size());
        iris.device->ops->enable_msg_type(iris.device, kMsgIrisFrame);
        int ret = iris.device->ops->start_preview(iris.device);
        if (ret) {
            LOG(ERROR) << "start_preview: " << ret;
            int32_t err = ret;
            Send(client, MSG_ERROR, &err, sizeof(err));
        } else {
            LOG(INFO) << "Iris camera streaming";
            std::vector<char> buf(kMaxMsgSize);
            ssize_t n;
            while ((n = TEMP_FAILURE_RETRY(recv(client, buf.data(), buf.size(), 0))) > 0) {
                auto* hdr = reinterpret_cast<IrisCameraMsg*>(buf.data());
                if (n < static_cast<ssize_t>(sizeof(*hdr)) || hdr->type != MSG_SET_PARAMS ||
                    hdr->len > n - sizeof(*hdr)) {
                    continue;
                }
                // Stock IRV1Controller.onIRPropertyChanged(): merge and set the parameters
                for (const auto& [key, value] :
                     Unflatten(std::string(buf.data() + sizeof(*hdr), hdr->len))) {
                    params[key] = value;
                }
                ret = iris.device->ops->set_parameters(iris.device, Flatten(params).c_str());
                if (ret) LOG(ERROR) << "set_parameters: " << ret;
            }
        }
    }
    gClient.store(-1);
    Close(&iris, kMsgIrisFrame);
    Close(&front, kMsgIrisPreviewData);
    // Other users of the front camera get the companion again.
    android::base::SetProperty(kCompanionDisableProp, "0");
    LOG(INFO) << "Iris camera closed";
}

}  // namespace

int main() {
    int server = android_get_control_socket(kSocketName);
    if (server < 0) {
        LOG(FATAL) << "No init socket " << kSocketName;
    }
    if (listen(server, 1)) {
        PLOG(FATAL) << "listen";
    }
    for (;;) {
        unique_fd client(TEMP_FAILURE_RETRY(accept4(server, nullptr, nullptr, SOCK_CLOEXEC)));
        if (client < 0) {
            PLOG(ERROR) << "accept";
            continue;
        }
        Serve(client.get());
    }
}
