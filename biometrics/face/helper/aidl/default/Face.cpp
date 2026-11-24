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
constexpr int kMaxEnrollmentsPerUser = 1;
// The library returns no signed token, so keystore-bound operations (BiometricPrompt with a
// CryptoObject) cannot use this sensor
constexpr common::SensorStrength kSensorStrength = common::SensorStrength::STRONG;

}  // namespace

std::mutex Face::session_lock_;
std::shared_ptr<Session> Face::session_;

Face::Face() {
    loaded_ = lib_.Load();
    if (loaded_) {
        std::lock_guard<std::mutex> lock(lib_lock_);
        lib_.ss_face_open(nullptr);
        lib_.ss_set_notify_callback(Notify);
    }
}

void Face::Notify(const FaceMsg* msg) {
    std::shared_ptr<Session> session;
    {
        std::lock_guard<std::mutex> lock(session_lock_);
        session = session_;
    }
    if (session) session->Notify(msg);
}

ndk::ScopedAStatus Face::getSensorProps(std::vector<SensorProps>* out) {
    *out = {};
    if (!loaded_) return ndk::ScopedAStatus::ok();

    common::CommonProps common_props = {
            .sensorId = kSensorId,
            .sensorStrength = kSensorStrength,
            .maxEnrollmentsPerUser = kMaxEnrollmentsPerUser,
            .componentInfo = {{"faceSensor", "Samsung S5K3H1", "", "", ""},
                              {"matchingAlgorithm", "", "", "", "Samsung face trustlet"}},
    };
    SensorProps props;
    props.commonProps = common_props;
    props.sensorType = FaceSensorType::RGB;
    props.halControlsPreview = true;
    props.enrollPreviewWidth = Preview::kSize;
    props.enrollPreviewHeight = Preview::kSize;
    props.supportsDetectInteraction = false;
    out->push_back(props);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Face::createSession(int32_t /*sensor_id*/, int32_t user_id,
                                       const std::shared_ptr<ISessionCallback>& cb,
                                       std::shared_ptr<ISession>* out) {
    auto session = SharedRefBase::make<Session>(FaceEngine{&lib_, &lib_lock_}, &worker_, &lockout_,
                                                &preview_, &boost_, user_id, cb);
    session->LinkToDeath(cb->asBinder().get());
    {
        std::lock_guard<std::mutex> lock(session_lock_);
        session_ = session;
    }
    *out = session;
    return ndk::ScopedAStatus::ok();
}

}  // namespace aidl::android::hardware::biometrics::face
