/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <memory>
#include <mutex>

#include <aidl/google/hardware/power/extension/pixel/IPowerExt.h>

namespace aidl::android::hardware::biometrics::face {

// Raises the CPU minimum frequency while the IR camera runs, through the IRIS_CAPTURE power hint
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
