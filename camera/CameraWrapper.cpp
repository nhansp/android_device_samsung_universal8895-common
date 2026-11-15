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
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <android-base/file.h>
#include <android-base/parseint.h>
#include <android-base/strings.h>
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

// torch-sec1 counts 20 mA steps. The Samsung camera provider offers five strength
// levels. Level 1 is the 60 mA (step 3) that Samsung's HAL lights through
// rear_torch_flash; the others are 80, 120, 160 and 200 mA from the greatlte
// torch_table, about 1.35x apart so the steps look even.
constexpr int32_t kTorchSteps[] = {3, 4, 6, 8, 10};
constexpr int32_t kDefaultTorchLevel = 1;

std::atomic<const camera_module_callbacks_t*> gCallbacks{nullptr};

std::mutex gStatusLock;
std::map<std::string, int> gTorchStatus;

std::mutex gTorchLock;

std::mutex gInfoLock;
std::map<int, camera_metadata_t*> gStaticInfo;

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

// 1080p and 720p at 60 fps on the back camera, as stock One UI records them
// (cameradata/camera-feature.xml BACK_CAMCORDER_RESOLUTION_FEATURE_MAP_1920X1080_60FPS).
// libexynoscamera3 caps every YUV/PRIVATE output at 33.3 ms and its visible fps ranges at 30
// (the (60,60) range is only in the IMX333/2L2 hidden list). With the 16:9 row of its preview
// size table on the sensor's 16:9 size (extract-files.py), a 16:9 session runs the sensor's
// 4032x2268 60 fps mode (fimc-is-device-module-2l2.c mode 3), so 16:9 outputs up to 1080p can
// take 1/60 s.
constexpr int kBackCameraId = 0;
constexpr int64_t kFrameDuration60Fps = 1000000000LL / 60;
constexpr int32_t kVideoFormats[] = {HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED,
                                     HAL_PIXEL_FORMAT_YCbCr_420_888};
constexpr int32_t kSixtyFpsSizes[][2] = {{1920, 1080}, {1280, 720}};

void AddBackVideoModes(std::vector<int64_t>* durations, std::vector<int32_t>* fpsRanges) {
    for (size_t i = 0; i + 3 < durations->size(); i += 4) {
        const int64_t* d = durations->data() + i;
        bool size = std::any_of(std::begin(kSixtyFpsSizes), std::end(kSixtyFpsSizes),
                                [d](const int32_t* s) { return d[1] == s[0] && d[2] == s[1]; });
        if (size && std::find(std::begin(kVideoFormats), std::end(kVideoFormats), d[0]) !=
                            std::end(kVideoFormats)) {
            (*durations)[i + 3] = kFrameDuration60Fps;
        }
    }
    fpsRanges->insert(fpsRanges->end(), {60, 60});
}

// Returns a copy of src with the additions for this camera, or nullptr if there are none.
camera_metadata_t* PatchStaticInfo(int cameraId, const camera_metadata_t* src, int32_t maxTorchLevel) {
    camera_metadata_ro_entry_t entry;
    bool torch = maxTorchLevel > 0 &&
                 find_camera_metadata_ro_entry(src, ANDROID_FLASH_INFO_AVAILABLE, &entry) == 0 &&
                 entry.count == 1 && entry.data.u8[0] == ANDROID_FLASH_INFO_AVAILABLE_TRUE;
    bool video = cameraId == kBackCameraId;
    if (!torch && !video) return nullptr;

    auto keys = GetEntry<int32_t>(src, ANDROID_REQUEST_AVAILABLE_CHARACTERISTICS_KEYS);
    auto durations = GetEntry<int64_t>(src, ANDROID_SCALER_AVAILABLE_MIN_FRAME_DURATIONS);
    auto fpsRanges = GetEntry<int32_t>(src, ANDROID_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES);
    const int32_t defaultLevel = std::min(kDefaultTorchLevel, maxTorchLevel);
    if (torch) {
        for (int32_t tag : {ANDROID_FLASH_INFO_STRENGTH_MAXIMUM_LEVEL,
                            ANDROID_FLASH_INFO_STRENGTH_DEFAULT_LEVEL}) {
            if (std::find(keys.begin(), keys.end(), tag) == keys.end()) keys.push_back(tag);
        }
    }
    if (video) AddBackVideoModes(&durations, &fpsRanges);

    size_t extraData = calculate_camera_metadata_entry_data_size(TYPE_INT32, keys.size()) +
                       calculate_camera_metadata_entry_data_size(TYPE_INT64, durations.size()) +
                       calculate_camera_metadata_entry_data_size(TYPE_INT32, fpsRanges.size());
    camera_metadata_t* dst = allocate_camera_metadata(get_camera_metadata_entry_count(src) + 8,
                                                      get_camera_metadata_data_count(src) + extraData);
    if (dst == nullptr) return nullptr;
    int ret = append_camera_metadata(dst, src);
    if (!ret && torch) {
        ret = SetEntry(dst, ANDROID_FLASH_INFO_STRENGTH_MAXIMUM_LEVEL, &maxTorchLevel, 1) ||
              SetEntry(dst, ANDROID_FLASH_INFO_STRENGTH_DEFAULT_LEVEL, &defaultLevel, 1) ||
              SetEntry(dst, ANDROID_REQUEST_AVAILABLE_CHARACTERISTICS_KEYS, keys.data(),
                       keys.size());
    }
    if (!ret && video) {
        ret = SetEntry(dst, ANDROID_SCALER_AVAILABLE_MIN_FRAME_DURATIONS, durations.data(),
                       durations.size()) ||
              SetEntry(dst, ANDROID_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES, fpsRanges.data(),
                       fpsRanges.size());
    }
    if (ret) {
        ALOGE("Failed to extend the static info of camera %d", cameraId);
        free_camera_metadata(dst);
        return nullptr;
    }
    sort_camera_metadata(dst);
    return dst;
}

bool IsTorchOn(const char* camera_id) {
    std::lock_guard<std::mutex> lock(gStatusLock);
    auto it = gTorchStatus.find(camera_id);
    return it != gTorchStatus.end() && it->second == TORCH_MODE_STATUS_AVAILABLE_ON;
}

void CameraDeviceStatusChange(const camera_module_callbacks_t*, int camera_id, int new_status) {
    const camera_module_callbacks_t* callbacks = gCallbacks.load();
    if (callbacks != nullptr) {
        callbacks->camera_device_status_change(callbacks, camera_id, new_status);
    }
}

void TorchModeStatusChange(const camera_module_callbacks_t*, const char* camera_id,
                           int new_status) {
    if (camera_id != nullptr) {
        std::lock_guard<std::mutex> lock(gStatusLock);
        gTorchStatus[camera_id] = new_status;
    }
    const camera_module_callbacks_t* callbacks = gCallbacks.load();
    if (callbacks != nullptr) {
        callbacks->torch_mode_status_change(callbacks, camera_id, new_status);
    }
}

const camera_module_callbacks_t kCallbacks = {
        .camera_device_status_change = CameraDeviceStatusChange,
        .torch_mode_status_change = TorchModeStatusChange,
};

int Open(const hw_module_t*, const char* id, hw_device_t** device) {
    camera_module_t* vendor = VendorModule();
    if (vendor == nullptr) {
        return -ENODEV;
    }
    return vendor->common.methods->open(&vendor->common, id, device);
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

    std::lock_guard<std::mutex> lock(gInfoLock);
    auto it = gStaticInfo.find(camera_id);
    if (it == gStaticInfo.end()) {
        it = gStaticInfo
                     .emplace(camera_id, PatchStaticInfo(camera_id,
                                                         info->static_camera_characteristics,
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
    gCallbacks.store(callbacks);
    return vendor->set_callbacks(callbacks != nullptr ? &kCallbacks : nullptr);
}

void GetVendorTagOps(vendor_tag_ops_t* ops) {
    camera_module_t* vendor = VendorModule();
    if (vendor != nullptr && vendor->get_vendor_tag_ops != nullptr) {
        vendor->get_vendor_tag_ops(ops);
    }
}

int OpenLegacy(const hw_module_t*, const char* id, uint32_t halVersion, hw_device_t** device) {
    camera_module_t* vendor = VendorModule();
    if (vendor == nullptr || vendor->open_legacy == nullptr) {
        return -ENOSYS;
    }
    return vendor->open_legacy(&vendor->common, id, halVersion, device);
}

int SetTorchMode(const char* camera_id, bool enabled) {
    camera_module_t* vendor = VendorModule();
    if (vendor == nullptr) {
        return -ENODEV;
    }
    std::lock_guard<std::mutex> lock(gTorchLock);
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
    const int32_t maxLevel = TorchMaxLevel();
    if (maxLevel == 0) {
        return -ENOSYS;
    }
    if (strength < 1 || strength > maxLevel) {
        return -EINVAL;
    }

    std::lock_guard<std::mutex> lock(gTorchLock);
    const bool wasOn = IsTorchOn(camera_id);
    if (!wasOn) {
        int ret = vendor->set_torch_mode(camera_id, true);
        if (ret != 0) {
            return ret;
        }
    }
    if (!android::base::WriteStringToFile(std::to_string(kTorchSteps[strength - 1]),
                                          kTorchBrightnessPath)) {
        int ret = errno != 0 ? -errno : -EIO;
        ALOGE("Failed to set torch level %d: %s", strength, strerror(-ret));
        if (!wasOn) {
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

hw_module_methods_t gModuleMethods = {
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
                        .methods = &gModuleMethods,
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
