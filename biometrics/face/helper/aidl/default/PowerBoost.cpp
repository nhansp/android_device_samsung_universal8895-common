/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "FacePowerBoost"

#include "PowerBoost.h"

#include <android-base/logging.h>
#include <android/binder_manager.h>

namespace aidl::android::hardware::biometrics::face {

using ::aidl::google::hardware::power::extension::pixel::IPowerExt;

namespace {

constexpr char kPowerHal[] = "android.hardware.power.IPower/default";
constexpr char kHint[] = "FACE_CAPTURE";
// At most the enrollment timeout; every capture ends the hint itself
constexpr int32_t kDurationMs = 15000;

}  // namespace

std::shared_ptr<IPowerExt> PowerBoost::Ext() {
    std::lock_guard<std::mutex> lock(lock_);
    if (ext_) return ext_;
    ndk::SpAIBinder power(AServiceManager_checkService(kPowerHal));
    ndk::SpAIBinder extension;
    if (power.get() == nullptr ||
        AIBinder_getExtension(power.get(), extension.getR()) != STATUS_OK ||
        extension.get() == nullptr) {
        LOG(WARNING) << "No power HAL extension";
        return nullptr;
    }
    ext_ = IPowerExt::fromBinder(extension);
    return ext_;
}

void PowerBoost::Start() {
    if (auto power = Ext()) power->setBoost(kHint, kDurationMs);
}

void PowerBoost::End() {
    // A negative duration ends the hint
    if (auto power = Ext()) power->setBoost(kHint, -1);
}

}  // namespace aidl::android::hardware::biometrics::face
