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
// Stock acquireDVFS(mInactivityTimeOut): at most the enrollment timeout, 15 s; every capture ends
// the hint itself.
constexpr int32_t kDurationMs = 15000;

}  // namespace

std::shared_ptr<IPowerExt> PowerBoost::ext() {
    std::lock_guard<std::mutex> lock(mLock);
    if (mExt) return mExt;
    ndk::SpAIBinder power(AServiceManager_checkService(kPowerHal));
    ndk::SpAIBinder extension;
    if (power.get() == nullptr ||
        AIBinder_getExtension(power.get(), extension.getR()) != STATUS_OK ||
        extension.get() == nullptr) {
        LOG(WARNING) << "No power HAL extension";
        return nullptr;
    }
    mExt = IPowerExt::fromBinder(extension);
    return mExt;
}

void PowerBoost::start() {
    if (auto power = ext()) power->setBoost(kHint, kDurationMs);
}

void PowerBoost::end() {
    // A negative duration ends the hint (power-libperfmgr PowerExt::setBoost).
    if (auto power = ext()) power->setBoost(kHint, -1);
}

}  // namespace aidl::android::hardware::biometrics::face
