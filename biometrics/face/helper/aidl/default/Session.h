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
            PowerBoost* boost, int32_t userId, std::shared_ptr<ISessionCallback> cb);
    ~Session();

    ndk::ScopedAStatus generateChallenge() override;
    ndk::ScopedAStatus revokeChallenge(int64_t challenge) override;
    ndk::ScopedAStatus getEnrollmentConfig(EnrollmentType type,
                                           std::vector<EnrollmentStageConfig>* out) override;
    ndk::ScopedAStatus enroll(const HardwareAuthToken& hat, EnrollmentType type,
                              const std::vector<Feature>& features,
                              const std::optional<NativeHandle>& previewSurface,
                              std::shared_ptr<ICancellationSignal>* out) override;
    ndk::ScopedAStatus authenticate(int64_t operationId,
                                    std::shared_ptr<ICancellationSignal>* out) override;
    ndk::ScopedAStatus detectInteraction(std::shared_ptr<ICancellationSignal>* out) override;
    ndk::ScopedAStatus enumerateEnrollments() override;
    ndk::ScopedAStatus removeEnrollments(const std::vector<int32_t>& enrollmentIds) override;
    ndk::ScopedAStatus getFeatures() override;
    ndk::ScopedAStatus setFeature(const HardwareAuthToken& hat, Feature feature,
                                  bool enabled) override;
    ndk::ScopedAStatus getAuthenticatorId() override;
    ndk::ScopedAStatus invalidateAuthenticatorId() override;
    ndk::ScopedAStatus resetLockout(const HardwareAuthToken& hat) override;
    ndk::ScopedAStatus close() override;
    ndk::ScopedAStatus authenticateWithContext(int64_t operationId, const OperationContext& context,
                                               std::shared_ptr<ICancellationSignal>* out) override;
    ndk::ScopedAStatus enrollWithContext(const HardwareAuthToken& hat, EnrollmentType type,
                                         const std::vector<Feature>& features,
                                         const std::optional<NativeHandle>& previewSurface,
                                         const OperationContext& context,
                                         std::shared_ptr<ICancellationSignal>* out) override;
    ndk::ScopedAStatus detectInteractionWithContext(
            const OperationContext& context, std::shared_ptr<ICancellationSignal>* out) override;
    ndk::ScopedAStatus onContextChanged(const OperationContext& context) override;
    ndk::ScopedAStatus enrollWithOptions(const FaceEnrollOptions& options,
                                         std::shared_ptr<ICancellationSignal>* out) override;

    void cancel(uint64_t op);
    bool isClosed() const { return mClosed; }
    void linkToDeath(AIBinder* binder);
    // From the face library, in whatever thread called into it (with the engine lock held).
    void notify(const FaceMsg* msg);

  private:
    enum class State { IDLE, ENROLLING, AUTHENTICATING };

    std::shared_ptr<ICancellationSignal> cancellationSignal();
    void enroll(const HardwareAuthToken& hat, ANativeWindow* previewWindow);
    bool startCapture(State state, std::chrono::milliseconds timeout);
    void endCapture(bool cancelLibrary);
    bool onFrame(const uint8_t* nv21);
    void onAcquired(uint64_t op, int32_t code);
    void onTemplateEnrolling(uint64_t op, uint32_t fid, uint32_t progress);
    void onAuthenticated(uint64_t op, uint32_t fid);
    void onError(uint64_t op, int32_t error);
    void onTimeout(uint64_t op);
    bool checkLockout();
    std::string storePath() const;
    std::vector<int32_t> enrollments() const;

    FaceEngine mEngine;
    Worker* mWorker;
    LockoutTracker* mLockout;
    Preview* mPreview;
    PowerBoost* mBoost;
    int32_t mUserId;
    std::shared_ptr<ISessionCallback> mCb;
    AIBinder_DeathRecipient* mDeathRecipient;
    bool mClosed = false;

    // Worker thread only, except mOp, which the cancellation signal and the capture read.
    State mState = State::IDLE;
    std::atomic<uint64_t> mOp = 0;
    int64_t mOperationId = 0;
    int32_t mLastAcquired = -1;
    FaceCapture mCapture;
};

}  // namespace aidl::android::hardware::biometrics::face
