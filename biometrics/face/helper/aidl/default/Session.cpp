/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "FaceSession"

#include "Session.h"

#include <aidl/android/hardware/biometrics/common/BnCancellationSignal.h>

#include <endian.h>
#include <stdio.h>
#include <unistd.h>

#include <algorithm>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/strings.h>

#include <android/native_window_aidl.h>

#include <biometrics_camera/CameraProtocol.h>

namespace aidl::android::hardware::biometrics::face {

using ::aidl::android::hardware::biometrics::common::BnCancellationSignal;
using namespace std::chrono_literals;

namespace {

// Frame rotation for the natural (portrait) display orientation
constexpr int kRotation = 270;
// SECURE_LEVEL_HIGH
constexpr uint32_t kSecureLevel = 2;
constexpr auto kAuthenticationTimeout = 6000ms;
constexpr auto kEnrollmentTimeout = 15000ms;
// /data/vendor/biometrics/face is created by the APEX's init rc. The library creates the store
// directory and the one above it, which it takes as the path minus its last three characters
// (setStorePath)
constexpr char kStoreDir[] = "/data/vendor/biometrics/face/%d/fr";
// The library numbers its templates but keeps no enrollment ids; the id it reported at
// enrollment is kept here.
constexpr char kEnrollmentFile[] = "enrollment";

class CancellationSignal : public BnCancellationSignal {
  public:
    CancellationSignal(std::shared_ptr<Session> session, uint64_t op)
        : mSession(session), mOp(op) {}

    ndk::ScopedAStatus cancel() override {
        if (auto session = mSession.lock()) session->cancel(mOp);
        return ndk::ScopedAStatus::ok();
    }

  private:
    std::weak_ptr<Session> mSession;
    uint64_t mOp;
};

hw_auth_token_t toLegacy(const HardwareAuthToken& token) {
    hw_auth_token_t hat = {};
    hat.version = HW_AUTH_TOKEN_VERSION;
    hat.challenge = token.challenge;
    hat.user_id = token.userId;
    hat.authenticator_id = token.authenticatorId;
    hat.authenticator_type = htobe32(static_cast<uint32_t>(token.authenticatorType));
    hat.timestamp = htobe64(token.timestamp.milliSeconds);
    std::copy_n(token.mac.begin(), std::min(token.mac.size(), sizeof(hat.hmac)), hat.hmac);
    return hat;
}

void onClientDeath(void* cookie) {
    LOG(INFO) << "FaceService has died";
    auto* session = static_cast<Session*>(cookie);
    if (!session->isClosed()) session->close();
}

}  // namespace

Session::Session(FaceEngine engine, Worker* worker, LockoutTracker* lockout, Preview* preview,
                 PowerBoost* boost, int32_t userId, std::shared_ptr<ISessionCallback> cb)
    : mEngine(engine),
      mWorker(worker),
      mLockout(lockout),
      mPreview(preview),
      mBoost(boost),
      mUserId(userId),
      mCb(std::move(cb)) {
    mDeathRecipient = AIBinder_DeathRecipient_new(onClientDeath);
    std::lock_guard<std::mutex> lock(*mEngine.lock);
    mEngine.lib->ss_face_set_active_group(mUserId, storePath().c_str());
}

Session::~Session() {
    AIBinder_DeathRecipient_delete(mDeathRecipient);
}

void Session::linkToDeath(AIBinder* binder) {
    AIBinder_linkToDeath(binder, mDeathRecipient, this);
}

std::string Session::storePath() const {
    char path[64];
    snprintf(path, sizeof(path), kStoreDir, mUserId);
    return path;
}

std::vector<int32_t> Session::enrollments() const {
    std::string id;
    int32_t fid;
    if (::android::base::ReadFileToString(storePath() + "/" + kEnrollmentFile, &id) &&
        ::android::base::ParseInt(::android::base::Trim(id), &fid)) {
        return {fid};
    }
    return {};
}

std::shared_ptr<ICancellationSignal> Session::cancellationSignal() {
    return SharedRefBase::make<CancellationSignal>(ref<Session>(), mOp.load());
}

ndk::ScopedAStatus Session::generateChallenge() {
    mWorker->post([self = ref<Session>()] {
        uint64_t challenge;
        {
            std::lock_guard<std::mutex> lock(*self->mEngine.lock);
            challenge = self->mEngine.lib->ss_face_pre_enroll();
        }
        LOG(INFO) << "generateChallenge";
        self->mCb->onChallengeGenerated(challenge);
    });
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::revokeChallenge(int64_t challenge) {
    mWorker->post([self = ref<Session>(), challenge] {
        {
            std::lock_guard<std::mutex> lock(*self->mEngine.lock);
            self->mEngine.lib->ss_face_post_enroll();
        }
        self->mCb->onChallengeRevoked(challenge);
    });
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::getEnrollmentConfig(EnrollmentType /*type*/,
                                                std::vector<EnrollmentStageConfig>* out) {
    *out = {};
    return ndk::ScopedAStatus::ok();
}

bool Session::onFrame(const uint8_t* nv21) {
    std::lock_guard<std::mutex> lock(*mEngine.lock);
    // Returns the engine's keepProcessing: 1 once the enrollment or match is over (the library
    // closes its session itself; its result was sent to notify() before), any other value to
    // keep feeding frames; -2 when the library is not open.
    int ret = mEngine.lib->ss_face_processFrontImage(nv21, face_camera::kFrameBytes,
                                                      face_camera::kWidth, face_camera::kHeight,
                                                      kRotation, FACE_IMAGE_FORMAT_NV21);
    return ret != 1 && ret >= 0;
}

bool Session::startCapture(State state, std::chrono::milliseconds timeout) {
    std::weak_ptr<Session> weak = ref<Session>();
    uint64_t op = mOp.load();
    FaceCapture::Callbacks callbacks = {
            .onPreview = [this](const uint8_t* nv21) { mPreview->render(nv21); },
            .onFrame = [this](const uint8_t* nv21) { return onFrame(nv21); },
            .onEnd =
                    [this, weak, op](int error) {
                        mWorker->post([weak, op, error] {
                            if (auto self = weak.lock()) self->onError(op, -error);
                        });
                    },
    };
    mBoost->start();
    if (!mCapture.start(std::move(callbacks))) {
        mBoost->end();
        return false;
    }
    mState = state;
    mLastAcquired = -1;
    mWorker->postDelayed(timeout, [weak, op] {
        if (auto self = weak.lock()) self->onTimeout(op);
    });
    return true;
}

void Session::endCapture(bool cancelLibrary) {
    mOp++;
    mState = State::IDLE;
    // First, as the capture thread may wait for the engine lock.
    mCapture.stop();
    {
        std::lock_guard<std::mutex> lock(*mEngine.lock);
        if (cancelLibrary) mEngine.lib->ss_face_cancel();
        mEngine.lib->ss_face_request(FACE_REQUEST_SESSION_CLOSE, nullptr, 0, nullptr, 0, 0);
    }
    mPreview->setSurface(nullptr);
    mBoost->end();
}

ndk::ScopedAStatus Session::enroll(const HardwareAuthToken& hat, EnrollmentType /*type*/,
                                   const std::vector<Feature>& /*features*/,
                                   const std::optional<NativeHandle>& /*previewSurface*/,
                                   std::shared_ptr<ICancellationSignal>* out) {
    // The framework passes the preview as a Surface (enrollWithOptions), not a NativeHandle.
    *out = cancellationSignal();
    enroll(hat, nullptr);
    return ndk::ScopedAStatus::ok();
}

void Session::enroll(const HardwareAuthToken& hat, ANativeWindow* previewWindow) {
    if (previewWindow) ANativeWindow_acquire(previewWindow);
    std::shared_ptr<ANativeWindow> window(previewWindow, [](ANativeWindow* w) {
        if (w) ANativeWindow_release(w);
    });
    mWorker->post([self = ref<Session>(), hat, window] {
        LOG(INFO) << "enroll";
        if (self->mState != State::IDLE) {
            self->mCb->onError(Error::UNABLE_TO_PROCESS, 0);
            return;
        }
        int ret;
        {
            std::lock_guard<std::mutex> lock(*self->mEngine.lock);
            FaceLib* lib = self->mEngine.lib;
            lib->ss_face_set_active_group(self->mUserId, self->storePath().c_str());
            lib->ss_face_request(FACE_REQUEST_SESSION_OPEN, nullptr, 0, nullptr, 0, 0);
            hw_auth_token_t authToken = toLegacy(hat);
            ret = lib->ss_face_enroll(&authToken, self->mUserId, 0, 0);
        }
        if (ret) {
            LOG(ERROR) << "ss_face_enroll: " << ret;
            self->endCapture(true);
            self->mCb->onError(Error::UNABLE_TO_PROCESS, ret);
            return;
        }
        self->mPreview->setSurface(window.get());
        if (!self->startCapture(State::ENROLLING, kEnrollmentTimeout)) {
            self->endCapture(true);
            self->mCb->onError(Error::HW_UNAVAILABLE, 0);
        }
    });
}

ndk::ScopedAStatus Session::authenticate(int64_t operationId,
                                         std::shared_ptr<ICancellationSignal>* out) {
    *out = cancellationSignal();
    mWorker->post([self = ref<Session>(), operationId] {
        LOG(INFO) << "authenticate";
        if (self->mState != State::IDLE) {
            self->mCb->onError(Error::UNABLE_TO_PROCESS, 0);
            return;
        }
        if (self->checkLockout()) return;
        int ret;
        {
            std::lock_guard<std::mutex> lock(*self->mEngine.lock);
            FaceLib* lib = self->mEngine.lib;
            lib->ss_face_set_active_group(self->mUserId, self->storePath().c_str());
            lib->ss_face_request(FACE_REQUEST_SESSION_OPEN, nullptr, 0, nullptr, 0, 0);
            ret = lib->ss_face_authenticate(operationId, self->mUserId, kSecureLevel, nullptr, 0);
        }
        if (ret) {
            LOG(ERROR) << "ss_face_authenticate: " << ret;
            self->endCapture(true);
            self->mCb->onError(Error::UNABLE_TO_PROCESS, ret);
            return;
        }
        self->mOperationId = operationId;
        if (!self->startCapture(State::AUTHENTICATING, kAuthenticationTimeout)) {
            self->endCapture(true);
            self->mCb->onError(Error::HW_UNAVAILABLE, 0);
        }
    });
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::detectInteraction(std::shared_ptr<ICancellationSignal>* out) {
    *out = cancellationSignal();
    mWorker->post([self = ref<Session>()] { self->mCb->onError(Error::UNABLE_TO_PROCESS, 0); });
    return ndk::ScopedAStatus::ok();
}

void Session::cancel(uint64_t op) {
    mWorker->post([self = ref<Session>(), op] {
        if (self->mState != State::IDLE && self->mOp.load() == op) {
            LOG(INFO) << "cancel";
            self->endCapture(true);
        }
        self->mCb->onError(Error::CANCELED, 0);
    });
}

void Session::notify(const FaceMsg* msg) {
    uint64_t op = mOp.load();
    std::weak_ptr<Session> weak = ref<Session>();
    switch (msg->type) {
        case FACE_MSG_ACQUIRED: {
            int32_t code = msg->data.acquired;
            mWorker->post([weak, op, code] {
                if (auto self = weak.lock()) self->onAcquired(op, code);
            });
        } break;
        case FACE_MSG_TEMPLATE_ENROLLING: {
            uint32_t fid = msg->data.enroll.fid, progress = msg->data.enroll.progress;
            mWorker->post([weak, op, fid, progress] {
                if (auto self = weak.lock()) self->onTemplateEnrolling(op, fid, progress);
            });
        } break;
        case FACE_MSG_AUTHENTICATED: {
            // The token the library sends is not filled in (FaceLib.h); only the result counts.
            uint32_t fid = msg->data.authenticated.fid;
            mWorker->post([weak, op, fid] {
                if (auto self = weak.lock()) self->onAuthenticated(op, fid);
            });
        } break;
        case FACE_MSG_ERROR: {
            int32_t error = msg->data.error;
            mWorker->post([weak, op, error] {
                if (auto self = weak.lock()) self->onError(op, error);
            });
        } break;
        default:
            break;
    }
}

void Session::onAcquired(uint64_t op, int32_t code) {
    if (mOp.load() != op || mState == State::IDLE) return;
    // A code equal to the previous one is skipped
    if (code == mLastAcquired) return;
    mLastAcquired = code;
    // Live face, process fail and fake face show nothing; the core migration notices are not
    // about the user.
    AcquiredInfo info;
    switch (code) {
        case FACE_ACQUIRED_NONE_FACE:
        case FACE_ACQUIRED_MISALIGNED_FACE:
            info = AcquiredInfo::NOT_DETECTED;
            break;
        case FACE_ACQUIRED_LOW_QUALITY:
            info = AcquiredInfo::SENSOR_DIRTY;
            break;
        case FACE_ACQUIRED_BIG_FACE:
            info = AcquiredInfo::TOO_CLOSE;
            break;
        case FACE_ACQUIRED_SMALL_FACE:
            info = AcquiredInfo::TOO_FAR;
            break;
        default:
            return;
    }
    BaseFrame frame;
    frame.acquiredInfo = info;
    frame.isCancellable = true;
    if (mState == State::ENROLLING) {
        EnrollmentFrame enrollment;
        enrollment.data = frame;
        mCb->onEnrollmentFrame(enrollment);
    } else {
        AuthenticationFrame authentication;
        authentication.data = frame;
        mCb->onAuthenticationFrame(authentication);
    }
}

void Session::onTemplateEnrolling(uint64_t op, uint32_t fid, uint32_t progress) {
    if (mOp.load() != op || mState != State::ENROLLING) return;
    int32_t remaining = 100 - static_cast<int32_t>(std::min(progress, 100u));
    LOG(INFO) << "onEnrollmentProgress fid " << fid << " progress " << progress;
    if (remaining == 0) {
        if (!::android::base::WriteStringToFile(std::to_string(fid),
                                                storePath() + "/" + kEnrollmentFile)) {
            PLOG(ERROR) << "Failed to record enrollment " << fid;
        }
        endCapture(false);
    }
    mCb->onEnrollmentProgress(fid, remaining);
}

void Session::onAuthenticated(uint64_t op, uint32_t fid) {
    if (mOp.load() != op || mState != State::AUTHENTICATING) return;
    endCapture(false);
    if (fid != 0) {
        LOG(INFO) << "onAuthenticationSucceeded";
        mLockout->reset(true);
        // The library gives no signed token, so the token carries only the challenge
        HardwareAuthToken token;
        token.challenge = mOperationId;
        mCb->onAuthenticationSucceeded(static_cast<int32_t>(fid), token);
    } else {
        // Five frames without a match
        LOG(INFO) << "onAuthenticationFailed";
        mLockout->addFailedAttempt();
        mCb->onAuthenticationFailed();
        checkLockout();
    }
}

void Session::onError(uint64_t op, int32_t error) {
    if (mOp.load() != op || mState == State::IDLE) return;
    LOG(ERROR) << "Face error " << error;
    endCapture(true);
    // 1004 is the broken template database; everything else is a processing failure
    if (error == kFaceErrorBrokenDatabase) {
        mCb->onError(Error::REENROLL_REQUIRED, 0);
    } else {
        mCb->onError(Error::UNABLE_TO_PROCESS, 0);
    }
}

void Session::onTimeout(uint64_t op) {
    if (mOp.load() != op || mState == State::IDLE) return;
    LOG(INFO) << "Timeout";
    // A frame the library is still processing may complete the enrollment or match; its
    // result is queued behind this task, so decide after it.
    mCapture.stop();
    mWorker->post([weak = std::weak_ptr<Session>(ref<Session>()), op] {
        auto self = weak.lock();
        if (!self || self->mOp.load() != op || self->mState == State::IDLE) return;
        self->endCapture(true);
        self->mCb->onError(Error::TIMEOUT, 0);
    });
}

bool Session::checkLockout() {
    switch (mLockout->mode()) {
        case LockoutTracker::Mode::PERMANENT:
            mCb->onLockoutPermanent();
            return true;
        case LockoutTracker::Mode::TIMED: {
            auto left = mLockout->timeLeft();
            mCb->onLockoutTimed(left.count());
            mWorker->postDelayed(left, [weak = std::weak_ptr<Session>(ref<Session>())] {
                auto self = weak.lock();
                if (self && self->mLockout->mode() == LockoutTracker::Mode::NONE) {
                    self->mCb->onLockoutCleared();
                }
            });
            return true;
        }
        case LockoutTracker::Mode::NONE:
            return false;
    }
    return false;
}

ndk::ScopedAStatus Session::enumerateEnrollments() {
    mWorker->post([self = ref<Session>()] {
        self->mCb->onEnrollmentsEnumerated(self->enrollments());
    });
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::removeEnrollments(const std::vector<int32_t>& enrollmentIds) {
    mWorker->post([self = ref<Session>(), enrollmentIds] {
        LOG(INFO) << "removeEnrollments";
        {
            std::lock_guard<std::mutex> lock(*self->mEngine.lock);
            for (int32_t fid : enrollmentIds) {
                int ret = self->mEngine.lib->ss_face_remove(self->mUserId, fid);
                if (ret) LOG(ERROR) << "ss_face_remove " << fid << ": " << ret;
            }
        }
        unlink((self->storePath() + "/" + kEnrollmentFile).c_str());
        self->mCb->onEnrollmentsRemoved(enrollmentIds);
    });
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::getFeatures() {
    // No attention detection: config_face_attention_supported is false.
    mWorker->post([self = ref<Session>()] { self->mCb->onFeaturesRetrieved({}); });
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::setFeature(const HardwareAuthToken& /*hat*/, Feature /*feature*/,
                                       bool /*enabled*/) {
    mWorker->post([self = ref<Session>()] { self->mCb->onError(Error::UNABLE_TO_PROCESS, 0); });
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::getAuthenticatorId() {
    mWorker->post([self = ref<Session>()] {
        uint64_t id;
        {
            std::lock_guard<std::mutex> lock(*self->mEngine.lock);
            id = self->mEngine.lib->ss_face_get_auth_id();
        }
        self->mCb->onAuthenticatorIdRetrieved(id);
    });
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::invalidateAuthenticatorId() {
    // The authenticator id lives in the trustlet's sid.dat and only changes with a new enrollment.
    mWorker->post([self = ref<Session>()] {
        uint64_t id;
        {
            std::lock_guard<std::mutex> lock(*self->mEngine.lock);
            id = self->mEngine.lib->ss_face_get_auth_id();
        }
        self->mCb->onAuthenticatorIdInvalidated(id);
    });
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::resetLockout(const HardwareAuthToken& /*hat*/) {
    mWorker->post([self = ref<Session>()] {
        self->mLockout->reset(true);
        self->mCb->onLockoutCleared();
    });
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::close() {
    mWorker->post([self = ref<Session>()] {
        if (self->mState != State::IDLE) self->endCapture(true);
        self->mCb->onSessionClosed();
    });
    mClosed = true;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::authenticateWithContext(int64_t operationId,
                                                    const OperationContext& /*context*/,
                                                    std::shared_ptr<ICancellationSignal>* out) {
    return authenticate(operationId, out);
}

ndk::ScopedAStatus Session::enrollWithContext(const HardwareAuthToken& hat, EnrollmentType type,
                                              const std::vector<Feature>& features,
                                              const std::optional<NativeHandle>& previewSurface,
                                              const OperationContext& /*context*/,
                                              std::shared_ptr<ICancellationSignal>* out) {
    return enroll(hat, type, features, previewSurface, out);
}

ndk::ScopedAStatus Session::detectInteractionWithContext(
        const OperationContext& /*context*/, std::shared_ptr<ICancellationSignal>* out) {
    return detectInteraction(out);
}

ndk::ScopedAStatus Session::onContextChanged(const OperationContext& /*context*/) {
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::enrollWithOptions(const FaceEnrollOptions& options,
                                              std::shared_ptr<ICancellationSignal>* out) {
    *out = cancellationSignal();
    enroll(options.hardwareAuthToken,
           options.surfacePreview ? options.surfacePreview->get() : nullptr);
    return ndk::ScopedAStatus::ok();
}

}  // namespace aidl::android::hardware::biometrics::face
