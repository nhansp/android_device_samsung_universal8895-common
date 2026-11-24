/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <aidl/android/hardware/biometrics/face/BnFace.h>

#include <mutex>

#include "IrisLib.h"
#include "LockoutTracker.h"
#include "PowerBoost.h"
#include "Preview.h"
#include "Session.h"
#include "Worker.h"

namespace aidl::android::hardware::biometrics::face {

class Face : public BnFace {
  public:
    Face();
    ndk::ScopedAStatus getSensorProps(std::vector<SensorProps>* out) override;
    ndk::ScopedAStatus createSession(int32_t sensor_id, int32_t user_id,
                                     const std::shared_ptr<ISessionCallback>& cb,
                                     std::shared_ptr<ISession>* out) override;

    std::shared_ptr<Preview> GetPreview() const { return preview_; }

  private:
    static void Notify(const IrisMsg* msg);

    IrisLib lib_ = {};
    bool loaded_ = false;
    Worker worker_;
    LockoutTracker lockout_;
    std::shared_ptr<Preview> preview_ = SharedRefBase::make<Preview>();
    PowerBoost boost_;

    static std::mutex session_lock_;
    static std::shared_ptr<Session> session_;
};

}  // namespace aidl::android::hardware::biometrics::face
