/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <memory>
#include <mutex>

#include <aidl/google/hardware/power/extension/pixel/IPowerExt.h>

namespace aidl::android::hardware::biometrics::face {

// Stock IrisService.startIRCamera() raises the CPU minimum frequency while the IR camera runs
// (acquireDVFS); here that is the IRIS_CAPTURE hint of the power HAL (powerhint.json).
class PowerBoost {
  public:
    void start();
    void end();

  private:
    std::shared_ptr<::aidl::google::hardware::power::extension::pixel::IPowerExt> ext();

    std::mutex mLock;
    std::shared_ptr<::aidl::google::hardware::power::extension::pixel::IPowerExt> mExt;
};

}  // namespace aidl::android::hardware::biometrics::face
