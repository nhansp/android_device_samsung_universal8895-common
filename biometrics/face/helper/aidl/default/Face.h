/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <aidl/android/hardware/biometrics/face/BnFace.h>

#include <mutex>

#include "FaceLib.h"
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
    ndk::ScopedAStatus createSession(int32_t sensorId, int32_t userId,
                                     const std::shared_ptr<ISessionCallback>& cb,
                                     std::shared_ptr<ISession>* out) override;

  private:
    static void notify(const FaceMsg* msg);

    FaceLib mLib = {};
    std::mutex mLibLock;
    bool mLoaded = false;
    Worker mWorker;
    LockoutTracker mLockout;
    Preview mPreview;
    PowerBoost mBoost;

    static std::mutex sSessionLock;
    static std::shared_ptr<Session> sSession;
};

}  // namespace aidl::android::hardware::biometrics::face
