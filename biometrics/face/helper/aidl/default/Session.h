/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <aidl/android/hardware/biometrics/face/BnSession.h>
#include <aidl/android/hardware/biometrics/face/ISessionCallback.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>

#include "FaceCapture.h"
#include "FaceLib.h"
#include "LockoutTracker.h"
#include "PowerBoost.h"
#include "Preview.h"
#include "Worker.h"

namespace aidl::android::hardware::biometrics::face {

using ::aidl::android::hardware::biometrics::common::ICancellationSignal;
using ::aidl::android::hardware::biometrics::common::OperationContext;
using ::aidl::android::hardware::common::NativeHandle;
using ::aidl::android::hardware::keymaster::HardwareAuthToken;

// The face library, which takes no lock itself; every call goes through `lock`.
struct FaceEngine {
    FaceLib* lib;
    std::mutex* lock;
};

class Session : public BnSession {
  public:
    Session(FaceEngine engine, Worker* worker, LockoutTracker* lockout, Preview* preview,
            PowerBoost* boost, int32_t user_id, std::shared_ptr<ISessionCallback> cb);
    ~Session();

    ndk::ScopedAStatus generateChallenge() override;
    ndk::ScopedAStatus revokeChallenge(int64_t challenge) override;
    ndk::ScopedAStatus getEnrollmentConfig(EnrollmentType type,
                                           std::vector<EnrollmentStageConfig>* out) override;
    ndk::ScopedAStatus enroll(const HardwareAuthToken& hat, EnrollmentType type,
                              const std::vector<Feature>& features,
                              const std::optional<NativeHandle>& preview_surface,
                              std::shared_ptr<ICancellationSignal>* out) override;
    ndk::ScopedAStatus authenticate(int64_t operation_id,
                                    std::shared_ptr<ICancellationSignal>* out) override;
    ndk::ScopedAStatus detectInteraction(std::shared_ptr<ICancellationSignal>* out) override;
    ndk::ScopedAStatus enumerateEnrollments() override;
    ndk::ScopedAStatus removeEnrollments(const std::vector<int32_t>& enrollment_ids) override;
    ndk::ScopedAStatus getFeatures() override;
    ndk::ScopedAStatus setFeature(const HardwareAuthToken& hat, Feature feature,
                                  bool enabled) override;
    ndk::ScopedAStatus getAuthenticatorId() override;
    ndk::ScopedAStatus invalidateAuthenticatorId() override;
    ndk::ScopedAStatus resetLockout(const HardwareAuthToken& hat) override;
    ndk::ScopedAStatus close() override;
    ndk::ScopedAStatus authenticateWithContext(int64_t operation_id,
                                               const OperationContext& context,
                                               std::shared_ptr<ICancellationSignal>* out) override;
    ndk::ScopedAStatus enrollWithContext(const HardwareAuthToken& hat, EnrollmentType type,
                                         const std::vector<Feature>& features,
                                         const std::optional<NativeHandle>& preview_surface,
                                         const OperationContext& context,
                                         std::shared_ptr<ICancellationSignal>* out) override;
    ndk::ScopedAStatus detectInteractionWithContext(
            const OperationContext& context, std::shared_ptr<ICancellationSignal>* out) override;
    ndk::ScopedAStatus onContextChanged(const OperationContext& context) override;
    ndk::ScopedAStatus enrollWithOptions(const FaceEnrollOptions& options,
                                         std::shared_ptr<ICancellationSignal>* out) override;

    void cancel(uint64_t op);
    bool IsClosed() const { return closed_; }
    void LinkToDeath(AIBinder* binder);
    // From the face library, in whatever thread called into it (with the engine lock held).
    void Notify(const FaceMsg* msg);

  private:
    enum class State { kIdle, kEnrolling, kAuthenticating };

    std::shared_ptr<ICancellationSignal> MakeCancellationSignal();
    void StartEnroll(const HardwareAuthToken& hat, ANativeWindow* preview_window);
    bool StartCapture(State state, std::chrono::milliseconds timeout);
    void EndCapture(bool cancel_library);
    bool OnFrame(const uint8_t* nv21);
    void OnAcquired(uint64_t op, int32_t code);
    void OnTemplateEnrolling(uint64_t op, uint32_t fid, uint32_t progress);
    void OnAuthenticated(uint64_t op, uint32_t fid);
    void OnError(uint64_t op, int32_t error);
    void OnTimeout(uint64_t op);
    bool CheckLockout();
    std::string StorePath() const;
    std::vector<int32_t> Enrollments() const;

    FaceEngine engine_;
    Worker* worker_;
    LockoutTracker* lockout_;
    Preview* preview_;
    PowerBoost* boost_;
    int32_t user_id_;
    std::shared_ptr<ISessionCallback> cb_;
    AIBinder_DeathRecipient* death_recipient_;
    bool closed_ = false;

    // Worker thread only, except op_, which the cancellation signal and the capture read.
    State state_ = State::kIdle;
    std::atomic<uint64_t> op_ = 0;
    int64_t operation_id_ = 0;
    int32_t last_acquired_ = -1;
    FaceCapture capture_;
};

}  // namespace aidl::android::hardware::biometrics::face
