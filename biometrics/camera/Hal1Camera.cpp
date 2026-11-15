/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "BiometricsCamera"

#include "Hal1Camera.h"

#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>

#include <vector>

#include <android-base/logging.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <hardware/camera_common.h>

#include <biometrics_camera/CameraProtocol.h>

namespace {

constexpr char kModulePath[] = "/vendor/lib/hw/camera.universal8895.so";

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

// camera_memory_t of get_memory(fd, size, count): `count` buffers of `size` bytes; a data
// callback's index selects one (stock libcameraservice CameraHardwareInterface keeps the same).
struct Memory {
    camera_memory_t mem;
    size_t bufSize;
    unsigned int count;
};

void ReleaseMemory(camera_memory_t* mem) {
    auto* memory = reinterpret_cast<Memory*>(mem);
    if (mem->handle) {
        munmap(mem->data, mem->size);
    } else {
        free(mem->data);
    }
    delete memory;
}

camera_memory_t* GetMemory(int fd, size_t size, unsigned int count, void*) {
    auto* memory = new Memory{{nullptr, size * count, nullptr, ReleaseMemory}, size, count};
    camera_memory_t* mem = &memory->mem;
    if (fd >= 0) {
        void* data = mmap(nullptr, mem->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (data == MAP_FAILED) {
            PLOG(ERROR) << "mmap of a " << mem->size << " byte camera buffer";
            delete memory;
            return nullptr;
        }
        mem->data = data;
        mem->handle = mem;
    } else {
        mem->data = calloc(1, mem->size);
    }
    return mem;
}

void Notify(int32_t msgType, int32_t ext1, int32_t ext2, void* user) {
    auto* camera = static_cast<Hal1Camera*>(user);
    if (msgType != CAMERA_MSG_ERROR) return;
    LOG(ERROR) << "Camera " << camera->id << " error " << ext1 << " " << ext2;
    if (camera->onError) camera->onError(ext1);
}

void Data(int32_t msgType, const camera_memory_t* mem, unsigned int index,
          camera_frame_metadata_t*, void* user) {
    auto* camera = static_cast<Hal1Camera*>(user);
    const uint8_t* data = nullptr;
    size_t size = 0;
    if (mem && mem->data) {
        const auto* memory = reinterpret_cast<const Memory*>(mem);
        // Single-buffer memory (the iris data messages) is read as is, whatever the index.
        if (memory->count <= 1) {
            data = static_cast<const uint8_t*>(mem->data);
            size = mem->size;
        } else if (index < memory->count) {
            data = static_cast<const uint8_t*>(mem->data) + index * memory->bufSize;
            size = memory->bufSize;
        }
    }
    camera->logFirst(msgType, data, size);
    if (camera->onData) camera->onData(msgType, data, size);
}

void DataTimestamp(int64_t, int32_t, const camera_memory_t*, unsigned int, void*) {}

bool OpenDevice(Hal1Camera* camera, CameraParams* defaults) {
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
    *defaults = Unflatten(flat ? flat : "");
    if (device->ops->put_parameters) {
        device->ops->put_parameters(device, flat);
    } else {
        free(flat);
    }
    return true;
}

}  // namespace

void Hal1Camera::logFirst(int32_t msgType, const uint8_t* data, size_t size) {
    std::lock_guard<std::mutex> lock(mSeenLock);
    if (!mSeen.insert(msgType).second) return;
    uint32_t words[2] = {};
    if (data && size >= sizeof(words)) memcpy(words, data, sizeof(words));
    LOG(INFO) << "Camera " << id << " first message 0x" << std::hex << msgType << std::dec
              << " size " << size << " words "
              << android::base::StringPrintf("%08x %08x", words[0], words[1]);
}

bool Hal1Open(Hal1Camera* camera, const std::function<CameraParams(const CameraParams&)>& choose,
              CameraParams* params) {
    CameraParams defaults;
    if (!OpenDevice(camera, &defaults)) return false;
    *params = defaults;
    for (const auto& [key, value] : choose(defaults)) (*params)[key] = value;
    Hal1SetParameters(camera, *params);
    return true;
}

bool Hal1Open(Hal1Camera* camera, const CameraParams& overrides, CameraParams* params) {
    return Hal1Open(
            camera, [&overrides](const CameraParams&) { return overrides; }, params);
}

bool Hal1SetParameters(Hal1Camera* camera, const CameraParams& params) {
    int ret = camera->device->ops->set_parameters(camera->device, Flatten(params).c_str());
    if (ret) LOG(ERROR) << "Camera " << camera->id << " set_parameters: " << ret;
    return ret == 0;
}

void Hal1Close(Hal1Camera* camera, int32_t msgTypes) {
    camera_device_t* device = camera->device;
    if (!device) return;
    device->ops->stop_preview(device);
    device->ops->disable_msg_type(device, msgTypes);
    device->ops->release(device);
    device->common.close(&device->common);
    camera->device = nullptr;
}

CameraParams Unflatten(const std::string& flat) {
    CameraParams params;
    for (const auto& item : android::base::Split(flat, ";")) {
        size_t eq = item.find('=');
        if (eq != std::string::npos) params[item.substr(0, eq)] = item.substr(eq + 1);
    }
    return params;
}

std::string Flatten(const CameraParams& params) {
    std::vector<std::string> items;
    for (const auto& [key, value] : params) items.push_back(key + "=" + value);
    return android::base::Join(items, ";");
}

namespace {

bool SendIov(int fd, iovec* iov, int iovcnt, int passFd, bool block) {
    msghdr msg = {};
    msg.msg_iov = iov;
    msg.msg_iovlen = iovcnt;
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
    int flags = MSG_NOSIGNAL | (block ? 0 : MSG_DONTWAIT);
    return TEMP_FAILURE_RETRY(sendmsg(fd, &msg, flags)) >= 0;
}

}  // namespace

bool Send(int fd, uint32_t type, const void* data, uint32_t len, int passFd, bool block) {
    CameraMsg hdr = {type, len};
    iovec iov[2] = {{&hdr, sizeof(hdr)}, {const_cast<void*>(data), len}};
    return SendIov(fd, iov, len ? 2 : 1, passFd, block);
}

bool SendPrefixed(int fd, uint32_t type, uint32_t prefix, const void* data, uint32_t len) {
    CameraMsg hdr = {type, static_cast<uint32_t>(sizeof(prefix) + len)};
    iovec iov[3] = {{&hdr, sizeof(hdr)}, {&prefix, sizeof(prefix)}, {const_cast<void*>(data), len}};
    return SendIov(fd, iov, 3, -1, true);
}
