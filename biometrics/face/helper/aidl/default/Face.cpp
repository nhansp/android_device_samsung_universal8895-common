/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "SamsungFace"

#include "Face.h"

#include <android-base/logging.h>

namespace aidl::android::hardware::biometrics::face {

namespace {

// The fingerprint HAL is sensor 0.
constexpr int kSensorId = 1;
// Stock FaceService DEFAULT_MAX_TEMPLATES_PER_USER
constexpr int kMaxEnrollmentsPerUser = 1;
// Matching runs in the face trustlet. The library returns no signed token, so keystore-bound
// operations (BiometricPrompt with a CryptoObject) cannot use this sensor; unlock and plain
// BiometricPrompt work.
constexpr common::SensorStrength kSensorStrength = common::SensorStrength::STRONG;

}  // namespace

std::mutex Face::sSessionLock;
std::shared_ptr<Session> Face::sSession;

Face::Face() {
    mLoaded = mLib.load();
    if (mLoaded) {
        std::lock_guard<std::mutex> lock(mLibLock);
        mLib.ss_face_open(nullptr);
        mLib.ss_set_notify_callback(notify);
    }
}

void Face::notify(const FaceMsg* msg) {
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
            .componentInfo = {{"faceSensor", "Samsung S5K3H1", "", "", ""},
                              {"matchingAlgorithm", "", "", "", "Samsung face trustlet"}},
    };
    SensorProps props;
    props.commonProps = commonProps;
    props.sensorType = FaceSensorType::RGB;
    // The HAL opens the camera and draws the enrollment preview (Preview.cpp).
    props.halControlsPreview = true;
    props.enrollPreviewWidth = Preview::kSize;
    props.enrollPreviewHeight = Preview::kSize;
    props.supportsDetectInteraction = false;
    out->push_back(props);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Face::createSession(int32_t /*sensorId*/, int32_t userId,
                                       const std::shared_ptr<ISessionCallback>& cb,
                                       std::shared_ptr<ISession>* out) {
    auto session = SharedRefBase::make<Session>(FaceEngine{&mLib, &mLibLock}, &mWorker, &mLockout,
                                                &mPreview, &mBoost, userId, cb);
    session->linkToDeath(cb->asBinder().get());
    {
        std::lock_guard<std::mutex> lock(sSessionLock);
        sSession = session;
    }
    *out = session;
    return ndk::ScopedAStatus::ok();
}

}  // namespace aidl::android::hardware::biometrics::face
