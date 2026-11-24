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

std::mutex Face::session_lock_;
std::shared_ptr<Session> Face::session_;

Face::Face() {
    preview_->SetControl([](bool resume) {
        std::shared_ptr<Session> session;
        {
            std::lock_guard<std::mutex> lock(session_lock_);
            session = session_;
        }
        if (session) session->OnUserAction(resume);
    });
    loaded_ = lib_.Load();
    if (loaded_) {
        lib_.ss_iris_open(nullptr);
        lib_.ss_set_notify_callback(Notify);
    }
}

void Face::Notify(const IrisMsg* msg) {
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
            .componentInfo = {{"irisSensor", "Samsung S5K5E6", "", "", ""},
                              {"matchingAlgorithm", "", "", "", "Samsung iris trustlet"}},
    };
    SensorProps props;
    props.commonProps = common_props;
    props.sensorType = FaceSensorType::IR;
    props.halControlsPreview = true;
    props.enrollPreviewWidth = iris_camera::kWidth;
    props.enrollPreviewHeight = iris_camera::kHeight;
    props.supportsDetectInteraction = false;
    out->push_back(props);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Face::createSession(int32_t /*sensor_id*/, int32_t user_id,
                                       const std::shared_ptr<ISessionCallback>& cb,
                                       std::shared_ptr<ISession>* out) {
    auto session = SharedRefBase::make<Session>(&lib_, &worker_, &lockout_, preview_.get(), &boost_,
                                                user_id, cb);
    session->LinkToDeath(cb->asBinder().get());
    {
        std::lock_guard<std::mutex> lock(session_lock_);
        session_ = session;
    }
    *out = session;
    return ndk::ScopedAStatus::ok();
}

}  // namespace aidl::android::hardware::biometrics::face
