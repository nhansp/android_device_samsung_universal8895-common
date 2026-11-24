/*
 * SPDX-FileCopyrightText: The Android Open Source Project
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "android.hardware.keymaster@3.0-impl"

#include <string.h>

#include <android/hardware/keymaster/3.0/IKeymasterDevice.h>
#include <hardware/hardware.h>
#include <hardware/keymaster1.h>
#include <hardware/keymaster2.h>
#include <log/log.h>

using ::android::hardware::keymaster::V3_0::IKeymasterDevice;

// Defined by the stock libskeymaster3device.so.
namespace skeymaster::ng {
IKeymasterDevice* CreateSKeymasterDevice(keymaster1_device_t* dev);
IKeymasterDevice* CreateSKeymasterDevice(keymaster2_device_t* dev);
}  // namespace skeymaster::ng

namespace {

IKeymasterDevice* OpenKeymaster1(const hw_module_t* mod) {
    keymaster1_device_t* dev = nullptr;
    int rc = keymaster1_open(mod, &dev);
    if (rc) {
        ALOGE("Error %d opening keystore keymaster1 device", rc);
        return nullptr;
    }
    return skeymaster::ng::CreateSKeymasterDevice(dev);
}

IKeymasterDevice* OpenKeymaster2(const hw_module_t* mod) {
    keymaster2_device_t* dev = nullptr;
    int rc = keymaster2_open(mod, &dev);
    if (rc) {
        ALOGE("Error %d opening keystore keymaster2 device", rc);
        return nullptr;
    }
    return skeymaster::ng::CreateSKeymasterDevice(dev);
}

IKeymasterDevice* CreateKeymaster3Device() {
    const hw_module_t* mod = nullptr;

    int rc = hw_get_module_by_class(KEYSTORE_HARDWARE_MODULE_ID, nullptr, &mod);
    if (rc) {
        ALOGE("Could not find the keystore module: %d", rc);
        return nullptr;
    }

    switch (mod->module_api_version) {
        case KEYMASTER_MODULE_API_VERSION_1_0:
            return OpenKeymaster1(mod);
        case KEYMASTER_MODULE_API_VERSION_2_0:
            return OpenKeymaster2(mod);
        default:
            ALOGE("Unsupported keystore module API version 0x%x", mod->module_api_version);
            return nullptr;
    }
}

}  // namespace

extern "C" IKeymasterDevice* HIDL_FETCH_IKeymasterDevice(const char* name) {
    ALOGI("Fetching keymaster device name %s", name);

    if (name && strcmp(name, "default") == 0) {
        return CreateKeymaster3Device();
    }
    return nullptr;
}
