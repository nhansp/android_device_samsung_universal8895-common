/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "IrisFace"

#include "Face.h"

#include <android-base/logging.h>

#include <biometrics_camera/CameraProtocol.h>

namespace aidl::android::hardware::biometrics::face {

namespace {

// The fingerprint HAL is sensor 0.
constexpr int kSensorId = 1;
// The library keeps one template per user
constexpr int kMaxEnrollmentsPerUser = 1;
// Matching, liveness and the auth token all happen in the iris trustlet.
constexpr common::SensorStrength kSensorStrength = common::SensorStrength::STRONG;

}  // namespace

std::mutex Face::sSessionLock;
std::shared_ptr<Session> Face::sSession;

Face::Face() {
    mPreview->setControl([](bool resume) {
        std::shared_ptr<Session> session;
        {
            std::lock_guard<std::mutex> lock(sSessionLock);
            session = sSession;
        }
        if (session) session->onUserAction(resume);
    });
    mLoaded = mLib.load();
    if (mLoaded) {
        mLib.ss_iris_open(nullptr);
        mLib.ss_set_notify_callback(notify);
    }
}

void Face::notify(const IrisMsg* msg) {
    std::shared_ptr<Session> session;
    {
        std::lock_guard<std::mutex> lock(sSessionLock);
        session = sSession;
    }
    if (session) session->notify(msg);
}

ndk::ScopedAStatus Face::getSensorProps(std::vector<SensorProps>* out) {
    *out = {};
    if (!mLoaded) return ndk::ScopedAStatus::ok();

    common::CommonProps commonProps = {
            .sensorId = kSensorId,
            .sensorStrength = kSensorStrength,
            .maxEnrollmentsPerUser = kMaxEnrollmentsPerUser,
            .componentInfo = {{"irisSensor", "Samsung S5K5E6", "", "", ""},
                              {"matchingAlgorithm", "", "", "", "Samsung iris trustlet"}},
    };
    SensorProps props;
    props.commonProps = commonProps;
    props.sensorType = FaceSensorType::IR;
    props.halControlsPreview = true;
    props.enrollPreviewWidth = iris_camera::kWidth;
    props.enrollPreviewHeight = iris_camera::kHeight;
    props.supportsDetectInteraction = false;
    out->push_back(props);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Face::createSession(int32_t /*sensorId*/, int32_t userId,
                                       const std::shared_ptr<ISessionCallback>& cb,
                                       std::shared_ptr<ISession>* out) {
    auto session = SharedRefBase::make<Session>(&mLib, &mWorker, &mLockout, mPreview.get(),
                                                &mBoost, userId, cb);
    session->linkToDeath(cb->asBinder().get());
    {
        std::lock_guard<std::mutex> lock(sSessionLock);
        sSession = session;
    }
    *out = session;
    return ndk::ScopedAStatus::ok();
}

}  // namespace aidl::android::hardware::biometrics::face
