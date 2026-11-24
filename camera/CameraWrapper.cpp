/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "CameraWrapper"

#include <dlfcn.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <android-base/file.h>
#include <android-base/parseint.h>
#include <android-base/strings.h>
#include <hardware/camera3.h>
#include <hardware/camera_common.h>
#include <hardware/hardware.h>
#include <log/log.h>
#include <system/camera_metadata.h>
#include <system/graphics.h>

extern camera_module_t HAL_MODULE_INFO_SYM __attribute__((visibility("default")));

namespace {

#ifdef __LP64__
constexpr char kVendorModulePath[] = "/vendor/lib64/hw/camera.universal8895.so";
#else
constexpr char kVendorModulePath[] = "/vendor/lib/hw/camera.universal8895.so";
#endif

constexpr char kTorchBrightnessPath[] = "/sys/class/leds/torch-sec1/brightness";
constexpr char kTorchMaxBrightnessPath[] = "/sys/class/leds/torch-sec1/max_brightness";

// torch-sec1 counts 20 mA steps; level 1 is the 60 mA of Samsung's HAL, the others 80 to 200 mA,
// about 1.35x apart so the steps look even
constexpr int32_t kTorchSteps[] = {3, 4, 6, 8, 10};
constexpr int32_t kDefaultTorchLevel = 1;

std::atomic<const camera_module_callbacks_t*> framework_callbacks{nullptr};

std::mutex torch_status_mutex;
std::map<std::string, int> torch_status;

std::mutex torch_mutex;

std::mutex static_info_mutex;
std::map<int, camera_metadata_t*> static_info;

camera_module_t* VendorModule() {
    static camera_module_t* const module = []() -> camera_module_t* {
        void* handle = dlopen(kVendorModulePath, RTLD_NOW);
        if (handle == nullptr) {
            ALOGE("Failed to load %s: %s", kVendorModulePath, dlerror());
            return nullptr;
        }
        auto* hmi = static_cast<camera_module_t*>(dlsym(handle, HAL_MODULE_INFO_SYM_AS_STR));
        if (hmi == nullptr || strcmp(hmi->common.id, CAMERA_HARDWARE_MODULE_ID) != 0) {
            ALOGE("%s is not a camera HAL module", kVendorModulePath);
            dlclose(handle);
            return nullptr;
        }
        hmi->common.dso = handle;
        return hmi;
    }();
    return module;
}

int32_t TorchMaxLevel() {
    static const int32_t level = [] {
        std::string value;
        int32_t max = 0;
        if (!android::base::ReadFileToString(kTorchMaxBrightnessPath, &value) ||
            !android::base::ParseInt(android::base::Trim(value), &max, 1) ||
            access(kTorchBrightnessPath, W_OK) != 0) {
            ALOGW("No torch strength control through %s", kTorchBrightnessPath);
            return 0;
        }
        return static_cast<int32_t>(std::count_if(std::begin(kTorchSteps), std::end(kTorchSteps),
                                                  [max](int32_t step) { return step <= max; }));
    }();
    return level;
}

int SetEntry(camera_metadata_t* metadata, uint32_t tag, const void* data, size_t count) {
    camera_metadata_entry_t entry;
    if (find_camera_metadata_entry(metadata, tag, &entry) == 0) {
        return update_camera_metadata_entry(metadata, entry.index, data, count, nullptr);
    }
    return add_camera_metadata_entry(metadata, tag, data, count);
}

template <typename T>
std::vector<T> GetEntry(const camera_metadata_t* metadata, uint32_t tag) {
    camera_metadata_ro_entry_t entry;
    if (find_camera_metadata_ro_entry(metadata, tag, &entry) != 0) return {};
    const T* data = reinterpret_cast<const T*>(entry.data.u8);
    return std::vector<T>(data, data + entry.count);
}

// libexynoscamera3 caps every YUV/PRIVATE output at 33.3 ms and its visible fps ranges at 30. With
// the 16:9 row of its preview size table on the sensor's 16:9 size (extract-files.py), a 16:9
// session runs the sensor's 4032x2268 60 fps mode, so 1080p and 720p can take 1/60 s
constexpr int kBackCameraId = 0;
constexpr int64_t kFrameDuration60Fps = 1000000000LL / 60;
constexpr int32_t kVideoFormats[] = {HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED,
                                     HAL_PIXEL_FORMAT_YCbCr_420_888};
constexpr int32_t kSixtyFpsSizes[][2] = {{1920, 1080}, {1280, 720}};

void AddBackVideoModes(std::vector<int64_t>* durations, std::vector<int32_t>* fps_ranges) {
    for (size_t i = 0; i + 3 < durations->size(); i += 4) {
        const int64_t* d = durations->data() + i;
        bool size = std::any_of(std::begin(kSixtyFpsSizes), std::end(kSixtyFpsSizes),
                                [d](const int32_t* s) { return d[1] == s[0] && d[2] == s[1]; });
        if (size && std::find(std::begin(kVideoFormats), std::end(kVideoFormats), d[0]) !=
                            std::end(kVideoFormats)) {
            (*durations)[i + 3] = kFrameDuration60Fps;
        }
    }
    fps_ranges->insert(fps_ranges->end(), {60, 60});
}

// Returns a copy of src with the changes for this camera, or nullptr if there are none.
camera_metadata_t* PatchStaticInfo(int camera_id, const camera_metadata_t* src,
                                   int32_t max_torch_level) {
    camera_metadata_ro_entry_t entry;
    bool torch = max_torch_level > 0 &&
                 find_camera_metadata_ro_entry(src, ANDROID_FLASH_INFO_AVAILABLE, &entry) == 0 &&
                 entry.count == 1 && entry.data.u8[0] == ANDROID_FLASH_INFO_AVAILABLE_TRUE;
    bool video = camera_id == kBackCameraId;
    if (!torch && !video) return nullptr;

    auto keys = GetEntry<int32_t>(src, ANDROID_REQUEST_AVAILABLE_CHARACTERISTICS_KEYS);
    auto durations = GetEntry<int64_t>(src, ANDROID_SCALER_AVAILABLE_MIN_FRAME_DURATIONS);
    auto fps_ranges = GetEntry<int32_t>(src, ANDROID_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES);
    const int32_t default_level = std::min(kDefaultTorchLevel, max_torch_level);
    if (torch) {
        for (int32_t tag : {ANDROID_FLASH_INFO_STRENGTH_MAXIMUM_LEVEL,
                            ANDROID_FLASH_INFO_STRENGTH_DEFAULT_LEVEL}) {
            if (std::find(keys.begin(), keys.end(), tag) == keys.end()) keys.push_back(tag);
        }
    }
    if (video) AddBackVideoModes(&durations, &fps_ranges);

    size_t extra_data = calculate_camera_metadata_entry_data_size(TYPE_INT32, keys.size()) +
                        calculate_camera_metadata_entry_data_size(TYPE_INT64, durations.size()) +
                        calculate_camera_metadata_entry_data_size(TYPE_INT32, fps_ranges.size());
    camera_metadata_t* dst =
            allocate_camera_metadata(get_camera_metadata_entry_count(src) + 8,
                                     get_camera_metadata_data_count(src) + extra_data);
    if (dst == nullptr) return nullptr;
    int ret = append_camera_metadata(dst, src);
    if (!ret && torch) {
        ret = SetEntry(dst, ANDROID_FLASH_INFO_STRENGTH_MAXIMUM_LEVEL, &max_torch_level, 1) ||
              SetEntry(dst, ANDROID_FLASH_INFO_STRENGTH_DEFAULT_LEVEL, &default_level, 1) ||
              SetEntry(dst, ANDROID_REQUEST_AVAILABLE_CHARACTERISTICS_KEYS, keys.data(),
                       keys.size());
    }
    if (!ret && video) {
        ret = SetEntry(dst, ANDROID_SCALER_AVAILABLE_MIN_FRAME_DURATIONS, durations.data(),
                       durations.size()) ||
              SetEntry(dst, ANDROID_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES, fps_ranges.data(),
                       fps_ranges.size());
    }
    if (ret) {
        ALOGE("Failed to extend the static info of camera %d", camera_id);
        free_camera_metadata(dst);
        return nullptr;
    }
    sort_camera_metadata(dst);
    return dst;
}

bool IsTorchOn(const char* camera_id) {
    std::lock_guard<std::mutex> lock(torch_status_mutex);
    auto it = torch_status.find(camera_id);
    return it != torch_status.end() && it->second == TORCH_MODE_STATUS_AVAILABLE_ON;
}

void CameraDeviceStatusChange(const camera_module_callbacks_t*, int camera_id, int new_status) {
    const camera_module_callbacks_t* callbacks = framework_callbacks.load();
    if (callbacks != nullptr) {
        callbacks->camera_device_status_change(callbacks, camera_id, new_status);
    }
}

void TorchModeStatusChange(const camera_module_callbacks_t*, const char* camera_id,
                           int new_status) {
    if (camera_id != nullptr) {
        std::lock_guard<std::mutex> lock(torch_status_mutex);
        torch_status[camera_id] = new_status;
    }
    const camera_module_callbacks_t* callbacks = framework_callbacks.load();
    if (callbacks != nullptr) {
        callbacks->torch_mode_status_change(callbacks, camera_id, new_status);
    }
}

const camera_module_callbacks_t kCallbacks = {
        .camera_device_status_change = CameraDeviceStatusChange,
        .torch_mode_status_change = TorchModeStatusChange,
};

// libexynoscamera3 cannot flush a session with a ZSL (PRIVATE reprocessing) stream while ZSL
// requests are in flight: their buffers never return and the provider aborts on close. So with an
// input stream, flush() first waits for the requests in flight to complete, then flushes
constexpr auto kZslDrainTimeout = std::chrono::milliseconds(1500);

struct Pending {
    uint32_t buffers = 0;
    bool metadata = false;
};

struct WrappedDevice {
    camera3_device_t base;
    camera3_device_ops_t ops;
    camera3_callback_ops_t callbacks;
    camera3_device_t* vendor = nullptr;
    const camera3_callback_ops_t* framework = nullptr;
    uint32_t partial_result_count = 1;

    std::mutex lock;
    std::condition_variable drained;
    bool has_input_stream = false;
    std::map<uint32_t, Pending> in_flight;
};

WrappedDevice* FromDevice(const camera3_device_t* dev) {
    return reinterpret_cast<WrappedDevice*>(const_cast<camera3_device_t*>(dev));
}

WrappedDevice* FromCallbacks(const camera3_callback_ops_t* ops) {
    return reinterpret_cast<WrappedDevice*>(reinterpret_cast<uintptr_t>(ops) -
                                            offsetof(WrappedDevice, callbacks));
}

// Called with w->lock held.
void Settle(WrappedDevice* w, std::map<uint32_t, Pending>::iterator it) {
    if (it->second.buffers == 0 && it->second.metadata) {
        w->in_flight.erase(it);
        if (w->in_flight.empty()) w->drained.notify_all();
    }
}

void ProcessCaptureResult(const camera3_callback_ops_t* ops, const camera3_capture_result_t* r) {
    WrappedDevice* w = FromCallbacks(ops);
    {
        std::lock_guard<std::mutex> lock(w->lock);
        auto it = w->in_flight.find(r->frame_number);
        if (it != w->in_flight.end()) {
            uint32_t returned = r->num_output_buffers + (r->input_buffer != nullptr ? 1 : 0);
            it->second.buffers -= std::min(returned, it->second.buffers);
            if (r->result != nullptr && r->partial_result >= w->partial_result_count) {
                it->second.metadata = true;
            }
            Settle(w, it);
        }
    }
    w->framework->process_capture_result(w->framework, r);
}

void Notify(const camera3_callback_ops_t* ops, const camera3_notify_msg_t* msg) {
    WrappedDevice* w = FromCallbacks(ops);
    if (msg->type == CAMERA3_MSG_ERROR) {
        std::lock_guard<std::mutex> lock(w->lock);
        const camera3_error_msg_t& e = msg->message.error;
        if (e.error_code == CAMERA3_MSG_ERROR_DEVICE) {
            w->in_flight.clear();
            w->drained.notify_all();
        } else if (e.error_code == CAMERA3_MSG_ERROR_REQUEST ||
                   e.error_code == CAMERA3_MSG_ERROR_RESULT) {
            auto it = w->in_flight.find(e.frame_number);
            if (it != w->in_flight.end()) {
                it->second.metadata = true;
                Settle(w, it);
            }
        }
    }
    w->framework->notify(w->framework, msg);
}

int Initialize(const camera3_device_t* dev, const camera3_callback_ops_t* callbacks) {
    WrappedDevice* w = FromDevice(dev);
    w->framework = callbacks;
    return w->vendor->ops->initialize(w->vendor, &w->callbacks);
}

int ConfigureStreams(const camera3_device_t* dev, camera3_stream_configuration_t* list) {
    WrappedDevice* w = FromDevice(dev);
    bool input = std::any_of(list->streams, list->streams + list->num_streams,
                             [](const camera3_stream_t* s) {
                                 return s->stream_type == CAMERA3_STREAM_INPUT ||
                                        s->stream_type == CAMERA3_STREAM_BIDIRECTIONAL;
                             });
    {
        std::lock_guard<std::mutex> lock(w->lock);
        w->has_input_stream = input;
    }
    return w->vendor->ops->configure_streams(w->vendor, list);
}

int ProcessCaptureRequest(const camera3_device_t* dev, camera3_capture_request_t* request) {
    WrappedDevice* w = FromDevice(dev);
    {
        std::lock_guard<std::mutex> lock(w->lock);
        w->in_flight[request->frame_number] = {
                request->num_output_buffers + (request->input_buffer != nullptr ? 1 : 0), false};
    }
    int ret = w->vendor->ops->process_capture_request(w->vendor, request);
    if (ret != 0) {
        std::lock_guard<std::mutex> lock(w->lock);
        w->in_flight.erase(request->frame_number);
        if (w->in_flight.empty()) w->drained.notify_all();
    }
    return ret;
}

int RegisterStreamBuffers(const camera3_device_t* dev, const camera3_stream_buffer_set_t* set) {
    WrappedDevice* w = FromDevice(dev);
    return w->vendor->ops->register_stream_buffers(w->vendor, set);
}

const camera_metadata_t* ConstructDefaultRequestSettings(const camera3_device_t* dev, int type) {
    WrappedDevice* w = FromDevice(dev);
    return w->vendor->ops->construct_default_request_settings(w->vendor, type);
}

void GetMetadataVendorTagOps(const camera3_device_t* dev, vendor_tag_query_ops_t* ops) {
    WrappedDevice* w = FromDevice(dev);
    w->vendor->ops->get_metadata_vendor_tag_ops(w->vendor, ops);
}

void Dump(const camera3_device_t* dev, int fd) {
    WrappedDevice* w = FromDevice(dev);
    w->vendor->ops->dump(w->vendor, fd);
}

int Flush(const camera3_device_t* dev) {
    WrappedDevice* w = FromDevice(dev);
    {
        std::unique_lock<std::mutex> lock(w->lock);
        if (w->has_input_stream &&
            !w->drained.wait_for(lock, kZslDrainTimeout, [w] { return w->in_flight.empty(); })) {
            ALOGW("%zu requests still in flight after %lld ms, flushing anyway",
                  w->in_flight.size(), static_cast<long long>(kZslDrainTimeout.count()));
        }
    }
    return w->vendor->ops->flush(w->vendor);
}

int CloseDevice(hw_device_t* device) {
    WrappedDevice* w = reinterpret_cast<WrappedDevice*>(device);
    int ret = w->vendor->common.close(&w->vendor->common);
    delete w;
    return ret;
}

uint32_t PartialResultCount(camera_module_t* vendor, int camera_id) {
    camera_info info = {};
    if (vendor->get_camera_info(camera_id, &info) != 0 ||
        info.static_camera_characteristics == nullptr) {
        return 1;
    }
    auto count = GetEntry<int32_t>(info.static_camera_characteristics,
                                   ANDROID_REQUEST_PARTIAL_RESULT_COUNT);
    return count.empty() || count[0] < 1 ? 1 : count[0];
}

int Open(const hw_module_t*, const char* id, hw_device_t** device) {
    camera_module_t* vendor = VendorModule();
    if (vendor == nullptr) {
        return -ENODEV;
    }
    int ret = vendor->common.methods->open(&vendor->common, id, device);
    if (ret != 0 || *device == nullptr || (*device)->version < CAMERA_DEVICE_API_VERSION_3_0) {
        return ret;
    }

    int camera_id = 0;
    auto* w = new WrappedDevice();
    w->vendor = reinterpret_cast<camera3_device_t*>(*device);
    if (android::base::ParseInt(id, &camera_id, 0)) {
        w->partial_result_count = PartialResultCount(vendor, camera_id);
    }
    // Only the device 3.3 operations, the blob's ops table predates the later members
    const camera3_device_ops_t* v = w->vendor->ops;
    w->ops.initialize = Initialize;
    w->ops.configure_streams = ConfigureStreams;
    w->ops.register_stream_buffers = v->register_stream_buffers ? RegisterStreamBuffers : nullptr;
    w->ops.construct_default_request_settings = ConstructDefaultRequestSettings;
    w->ops.process_capture_request = ProcessCaptureRequest;
    w->ops.get_metadata_vendor_tag_ops =
            v->get_metadata_vendor_tag_ops ? GetMetadataVendorTagOps : nullptr;
    w->ops.dump = v->dump ? Dump : nullptr;
    w->ops.flush = Flush;
    w->callbacks.process_capture_result = ProcessCaptureResult;
    w->callbacks.notify = Notify;
    w->base.common = w->vendor->common;
    w->base.common.module = &HAL_MODULE_INFO_SYM.common;
    w->base.common.close = CloseDevice;
    w->base.ops = &w->ops;
    w->base.priv = w->vendor->priv;
    *device = &w->base.common;
    return 0;
}

int GetNumberOfCameras() {
    camera_module_t* vendor = VendorModule();
    return vendor != nullptr ? vendor->get_number_of_cameras() : 0;
}

int GetCameraInfo(int camera_id, struct camera_info* info) {
    camera_module_t* vendor = VendorModule();
    if (vendor == nullptr) {
        return -ENODEV;
    }
    int ret = vendor->get_camera_info(camera_id, info);
    if (ret != 0 || info->device_version < CAMERA_DEVICE_API_VERSION_3_0 ||
        info->static_camera_characteristics == nullptr) {
        return ret;
    }

    std::lock_guard<std::mutex> lock(static_info_mutex);
    auto it = static_info.find(camera_id);
    if (it == static_info.end()) {
        it = static_info
                     .emplace(camera_id,
                              PatchStaticInfo(camera_id, info->static_camera_characteristics,
                                              TorchMaxLevel()))
                     .first;
    }
    if (it->second != nullptr) {
        info->static_camera_characteristics = it->second;
    }
    return ret;
}

int SetCallbacks(const camera_module_callbacks_t* callbacks) {
    camera_module_t* vendor = VendorModule();
    if (vendor == nullptr) {
        return -ENODEV;
    }
    framework_callbacks.store(callbacks);
    return vendor->set_callbacks(callbacks != nullptr ? &kCallbacks : nullptr);
}

void GetVendorTagOps(vendor_tag_ops_t* ops) {
    camera_module_t* vendor = VendorModule();
    if (vendor != nullptr && vendor->get_vendor_tag_ops != nullptr) {
        vendor->get_vendor_tag_ops(ops);
    }
}

int OpenLegacy(const hw_module_t*, const char* id, uint32_t hal_version, hw_device_t** device) {
    camera_module_t* vendor = VendorModule();
    if (vendor == nullptr || vendor->open_legacy == nullptr) {
        return -ENOSYS;
    }
    return vendor->open_legacy(&vendor->common, id, hal_version, device);
}

int SetTorchMode(const char* camera_id, bool enabled) {
    camera_module_t* vendor = VendorModule();
    if (vendor == nullptr) {
        return -ENODEV;
    }
    std::lock_guard<std::mutex> lock(torch_mutex);
    return vendor->set_torch_mode(camera_id, enabled);
}

int SetTorchModeStrength(const char* camera_id, bool enabled, int32_t strength) {
    if (!enabled) {
        return SetTorchMode(camera_id, false);
    }

    camera_module_t* vendor = VendorModule();
    if (vendor == nullptr) {
        return -ENODEV;
    }
    const int32_t max_level = TorchMaxLevel();
    if (max_level == 0) {
        return -ENOSYS;
    }
    if (strength < 1 || strength > max_level) {
        return -EINVAL;
    }

    std::lock_guard<std::mutex> lock(torch_mutex);
    const bool was_on = IsTorchOn(camera_id);
    if (!was_on) {
        int ret = vendor->set_torch_mode(camera_id, true);
        if (ret != 0) {
            return ret;
        }
    }
    if (!android::base::WriteStringToFile(std::to_string(kTorchSteps[strength - 1]),
                                          kTorchBrightnessPath)) {
        int ret = errno != 0 ? -errno : -EIO;
        ALOGE("Failed to set torch level %d: %s", strength, strerror(-ret));
        if (!was_on) {
            vendor->set_torch_mode(camera_id, false);
        }
        return ret;
    }
    return 0;
}

int Init() {
    camera_module_t* vendor = VendorModule();
    if (vendor == nullptr) {
        return -ENODEV;
    }
    if (TorchMaxLevel() == 0) {
        HAL_MODULE_INFO_SYM.set_torch_mode_strength = nullptr;
    }
    return vendor->init != nullptr ? vendor->init() : 0;
}

hw_module_methods_t module_methods = {
        .open = Open,
};

}  // namespace

camera_module_t HAL_MODULE_INFO_SYM __attribute__((visibility("default"))) = {
        .common =
                {
                        .tag = HARDWARE_MODULE_TAG,
                        .module_api_version = CAMERA_MODULE_API_VERSION_2_4,
                        .hal_api_version = HARDWARE_HAL_API_VERSION,
                        .id = CAMERA_HARDWARE_MODULE_ID,
                        .name = "Exynos 8895 camera wrapper",
                        .author = "The LineageOS Project",
                        .methods = &module_methods,
                        .dso = nullptr,
                        .reserved = {},
                },
        .get_number_of_cameras = GetNumberOfCameras,
        .get_camera_info = GetCameraInfo,
        .set_callbacks = SetCallbacks,
        .get_vendor_tag_ops = GetVendorTagOps,
        .open_legacy = OpenLegacy,
        .set_torch_mode = SetTorchMode,
        .init = Init,
        .get_physical_camera_info = nullptr,
        .is_stream_combination_supported = nullptr,
        .notify_device_state_change = nullptr,
        .set_torch_mode_strength = SetTorchModeStrength,
        .reserved1 = {},
};
