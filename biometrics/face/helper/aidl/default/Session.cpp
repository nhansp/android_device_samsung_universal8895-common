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
        : session_(session), op_(op) {}

    ndk::ScopedAStatus cancel() override {
        if (auto session = session_.lock()) session->cancel(op_);
        return ndk::ScopedAStatus::ok();
    }

  private:
    std::weak_ptr<Session> session_;
    uint64_t op_;
};

hw_auth_token_t ToLegacy(const HardwareAuthToken& token) {
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

void OnClientDeath(void* cookie) {
    LOG(INFO) << "FaceService has died";
    auto* session = static_cast<Session*>(cookie);
    if (!session->IsClosed()) session->close();
}

}  // namespace

Session::Session(FaceEngine engine, Worker* worker, LockoutTracker* lockout, Preview* preview,
                 PowerBoost* boost, int32_t user_id, std::shared_ptr<ISessionCallback> cb)
    : engine_(engine),
      worker_(worker),
      lockout_(lockout),
      preview_(preview),
      boost_(boost),
      user_id_(user_id),
      cb_(std::move(cb)) {
    death_recipient_ = AIBinder_DeathRecipient_new(OnClientDeath);
    std::lock_guard<std::mutex> lock(*engine_.lock);
    engine_.lib->ss_face_set_active_group(user_id_, StorePath().c_str());
}

Session::~Session() {
    AIBinder_DeathRecipient_delete(death_recipient_);
}

void Session::LinkToDeath(AIBinder* binder) {
    AIBinder_linkToDeath(binder, death_recipient_, this);
}

std::string Session::StorePath() const {
    char path[64];
    snprintf(path, sizeof(path), kStoreDir, user_id_);
    return path;
}

std::vector<int32_t> Session::Enrollments() const {
    std::string id;
    int32_t fid;
    if (::android::base::ReadFileToString(StorePath() + "/" + kEnrollmentFile, &id) &&
        ::android::base::ParseInt(::android::base::Trim(id), &fid)) {
        return {fid};
    }
    return {};
}

std::shared_ptr<ICancellationSignal> Session::MakeCancellationSignal() {
    return SharedRefBase::make<CancellationSignal>(ref<Session>(), op_.load());
}

ndk::ScopedAStatus Session::generateChallenge() {
    worker_->Post([self = ref<Session>()] {
        uint64_t challenge;
        {
            std::lock_guard<std::mutex> lock(*self->engine_.lock);
            challenge = self->engine_.lib->ss_face_pre_enroll();
        }
        LOG(INFO) << "generateChallenge";
        self->cb_->onChallengeGenerated(challenge);
    });
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::revokeChallenge(int64_t challenge) {
    worker_->Post([self = ref<Session>(), challenge] {
        {
            std::lock_guard<std::mutex> lock(*self->engine_.lock);
            self->engine_.lib->ss_face_post_enroll();
        }
        self->cb_->onChallengeRevoked(challenge);
    });
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::getEnrollmentConfig(EnrollmentType /*type*/,
                                                std::vector<EnrollmentStageConfig>* out) {
    *out = {};
    return ndk::ScopedAStatus::ok();
}

bool Session::OnFrame(const uint8_t* nv21) {
    std::lock_guard<std::mutex> lock(*engine_.lock);
    // Returns the engine's keepProcessing: 1 once the enrollment or match is over (the library
    // closes its session itself; its result was sent to Notify() before), any other value to
    // keep feeding frames; -2 when the library is not open.
    int ret = engine_.lib->ss_face_processFrontImage(nv21, face_camera::kFrameBytes,
                                                     face_camera::kWidth, face_camera::kHeight,
                                                     kRotation, kFaceImageFormatNv21);
    return ret != 1 && ret >= 0;
}

bool Session::StartCapture(State state, std::chrono::milliseconds timeout) {
    std::weak_ptr<Session> weak = ref<Session>();
    uint64_t op = op_.load();
    FaceCapture::Callbacks callbacks = {
            .on_preview = [this](const uint8_t* nv21) { preview_->Render(nv21); },
            .on_frame = [this](const uint8_t* nv21) { return OnFrame(nv21); },
            .on_end =
                    [this, weak, op](int error) {
                        worker_->Post([weak, op, error] {
                            if (auto self = weak.lock()) self->OnError(op, -error);
                        });
                    },
    };
    boost_->Start();
    if (!capture_.Start(std::move(callbacks))) {
        boost_->End();
        return false;
    }
    state_ = state;
    last_acquired_ = -1;
    worker_->PostDelayed(timeout, [weak, op] {
        if (auto self = weak.lock()) self->OnTimeout(op);
    });
    return true;
}

void Session::EndCapture(bool cancel_library) {
    op_++;
    state_ = State::kIdle;
    // First, as the capture thread may wait for the engine lock.
    capture_.Stop();
    {
        std::lock_guard<std::mutex> lock(*engine_.lock);
        if (cancel_library) engine_.lib->ss_face_cancel();
        engine_.lib->ss_face_request(kFaceRequestSessionClose, nullptr, 0, nullptr, 0, 0);
    }
    preview_->SetSurface(nullptr);
    boost_->End();
}

ndk::ScopedAStatus Session::enroll(const HardwareAuthToken& hat, EnrollmentType /*type*/,
                                   const std::vector<Feature>& /*features*/,
                                   const std::optional<NativeHandle>& /*preview_surface*/,
                                   std::shared_ptr<ICancellationSignal>* out) {
    // The framework passes the preview as a Surface (enrollWithOptions), not a NativeHandle.
    *out = MakeCancellationSignal();
    StartEnroll(hat, nullptr);
    return ndk::ScopedAStatus::ok();
}

void Session::StartEnroll(const HardwareAuthToken& hat, ANativeWindow* preview_window) {
    if (preview_window) ANativeWindow_acquire(preview_window);
    std::shared_ptr<ANativeWindow> window(preview_window, [](ANativeWindow* w) {
        if (w) ANativeWindow_release(w);
    });
    worker_->Post([self = ref<Session>(), hat, window] {
        LOG(INFO) << "enroll";
        if (self->state_ != State::kIdle) {
            self->cb_->onError(Error::UNABLE_TO_PROCESS, 0);
            return;
        }
        int ret;
        {
            std::lock_guard<std::mutex> lock(*self->engine_.lock);
            FaceLib* lib = self->engine_.lib;
            lib->ss_face_set_active_group(self->user_id_, self->StorePath().c_str());
            lib->ss_face_request(kFaceRequestSessionOpen, nullptr, 0, nullptr, 0, 0);
            hw_auth_token_t auth_token = ToLegacy(hat);
            ret = lib->ss_face_enroll(&auth_token, self->user_id_, 0, 0);
        }
        if (ret) {
            LOG(ERROR) << "ss_face_enroll: " << ret;
            self->EndCapture(true);
            self->cb_->onError(Error::UNABLE_TO_PROCESS, ret);
            return;
        }
        self->preview_->SetSurface(window.get());
        if (!self->StartCapture(State::kEnrolling, kEnrollmentTimeout)) {
            self->EndCapture(true);
            self->cb_->onError(Error::HW_UNAVAILABLE, 0);
        }
    });
}

ndk::ScopedAStatus Session::authenticate(int64_t operation_id,
                                         std::shared_ptr<ICancellationSignal>* out) {
    *out = MakeCancellationSignal();
    worker_->Post([self = ref<Session>(), operation_id] {
        LOG(INFO) << "authenticate";
        if (self->state_ != State::kIdle) {
            self->cb_->onError(Error::UNABLE_TO_PROCESS, 0);
            return;
        }
        if (self->CheckLockout()) return;
        int ret;
        {
            std::lock_guard<std::mutex> lock(*self->engine_.lock);
            FaceLib* lib = self->engine_.lib;
            lib->ss_face_set_active_group(self->user_id_, self->StorePath().c_str());
            lib->ss_face_request(kFaceRequestSessionOpen, nullptr, 0, nullptr, 0, 0);
            ret = lib->ss_face_authenticate(operation_id, self->user_id_, kSecureLevel, nullptr, 0);
        }
        if (ret) {
            LOG(ERROR) << "ss_face_authenticate: " << ret;
            self->EndCapture(true);
            self->cb_->onError(Error::UNABLE_TO_PROCESS, ret);
            return;
        }
        self->operation_id_ = operation_id;
        if (!self->StartCapture(State::kAuthenticating, kAuthenticationTimeout)) {
            self->EndCapture(true);
            self->cb_->onError(Error::HW_UNAVAILABLE, 0);
        }
    });
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::detectInteraction(std::shared_ptr<ICancellationSignal>* out) {
    *out = MakeCancellationSignal();
    worker_->Post([self = ref<Session>()] { self->cb_->onError(Error::UNABLE_TO_PROCESS, 0); });
    return ndk::ScopedAStatus::ok();
}

void Session::cancel(uint64_t op) {
    worker_->Post([self = ref<Session>(), op] {
        if (self->state_ != State::kIdle && self->op_.load() == op) {
            LOG(INFO) << "cancel";
            self->EndCapture(true);
        }
        self->cb_->onError(Error::CANCELED, 0);
    });
}

void Session::Notify(const FaceMsg* msg) {
    uint64_t op = op_.load();
    std::weak_ptr<Session> weak = ref<Session>();
    switch (msg->type) {
        case kFaceMsgAcquired: {
            int32_t code = msg->data.acquired;
            worker_->Post([weak, op, code] {
                if (auto self = weak.lock()) self->OnAcquired(op, code);
            });
        } break;
        case kFaceMsgTemplateEnrolling: {
            uint32_t fid = msg->data.enroll.fid, progress = msg->data.enroll.progress;
            worker_->Post([weak, op, fid, progress] {
                if (auto self = weak.lock()) self->OnTemplateEnrolling(op, fid, progress);
            });
        } break;
        case kFaceMsgAuthenticated: {
            // The token the library sends is not filled in (FaceLib.h); only the result counts.
            uint32_t fid = msg->data.authenticated.fid;
            worker_->Post([weak, op, fid] {
                if (auto self = weak.lock()) self->OnAuthenticated(op, fid);
            });
        } break;
        case kFaceMsgError: {
            int32_t error = msg->data.error;
            worker_->Post([weak, op, error] {
                if (auto self = weak.lock()) self->OnError(op, error);
            });
        } break;
        default:
            break;
    }
}

void Session::OnAcquired(uint64_t op, int32_t code) {
    if (op_.load() != op || state_ == State::kIdle) return;
    // A code equal to the previous one is skipped
    if (code == last_acquired_) return;
    last_acquired_ = code;
    // Live face, process fail and fake face show nothing; the core migration notices are not
    // about the user.
    AcquiredInfo info;
    switch (code) {
        case kFaceAcquiredNoneFace:
        case kFaceAcquiredMisalignedFace:
            info = AcquiredInfo::NOT_DETECTED;
            break;
        case kFaceAcquiredLowQuality:
            info = AcquiredInfo::SENSOR_DIRTY;
            break;
        case kFaceAcquiredBigFace:
            info = AcquiredInfo::TOO_CLOSE;
            break;
        case kFaceAcquiredSmallFace:
            info = AcquiredInfo::TOO_FAR;
            break;
        default:
            return;
    }
    BaseFrame frame;
    frame.acquiredInfo = info;
    frame.isCancellable = true;
    if (state_ == State::kEnrolling) {
        EnrollmentFrame enrollment;
        enrollment.data = frame;
        cb_->onEnrollmentFrame(enrollment);
    } else {
        AuthenticationFrame authentication;
        authentication.data = frame;
        cb_->onAuthenticationFrame(authentication);
    }
}

void Session::OnTemplateEnrolling(uint64_t op, uint32_t fid, uint32_t progress) {
    if (op_.load() != op || state_ != State::kEnrolling) return;
    int32_t remaining = 100 - static_cast<int32_t>(std::min(progress, 100u));
    LOG(INFO) << "onEnrollmentProgress fid " << fid << " progress " << progress;
    if (remaining == 0) {
        if (!::android::base::WriteStringToFile(std::to_string(fid),
                                                StorePath() + "/" + kEnrollmentFile)) {
            PLOG(ERROR) << "Failed to record enrollment " << fid;
        }
        EndCapture(false);
    }
    cb_->onEnrollmentProgress(fid, remaining);
}

void Session::OnAuthenticated(uint64_t op, uint32_t fid) {
    if (op_.load() != op || state_ != State::kAuthenticating) return;
    EndCapture(false);
    if (fid != 0) {
        LOG(INFO) << "onAuthenticationSucceeded";
        lockout_->Reset(true);
        // The library gives no signed token, so the token carries only the challenge
        HardwareAuthToken token;
        token.challenge = operation_id_;
        cb_->onAuthenticationSucceeded(static_cast<int32_t>(fid), token);
    } else {
        // Five frames without a match
        LOG(INFO) << "onAuthenticationFailed";
        lockout_->AddFailedAttempt();
        cb_->onAuthenticationFailed();
        CheckLockout();
    }
}

void Session::OnError(uint64_t op, int32_t error) {
    if (op_.load() != op || state_ == State::kIdle) return;
    LOG(ERROR) << "Face error " << error;
    EndCapture(true);
    // 1004 is the broken template database; everything else is a processing failure
    if (error == kFaceErrorBrokenDatabase) {
        cb_->onError(Error::REENROLL_REQUIRED, 0);
    } else {
        cb_->onError(Error::UNABLE_TO_PROCESS, 0);
    }
}

void Session::OnTimeout(uint64_t op) {
    if (op_.load() != op || state_ == State::kIdle) return;
    LOG(INFO) << "Timeout";
    // A frame the library is still processing may complete the enrollment or match; its
    // result is queued behind this task, so decide after it.
    capture_.Stop();
    worker_->Post([weak = std::weak_ptr<Session>(ref<Session>()), op] {
        auto self = weak.lock();
        if (!self || self->op_.load() != op || self->state_ == State::kIdle) return;
        self->EndCapture(true);
        self->cb_->onError(Error::TIMEOUT, 0);
    });
}

bool Session::CheckLockout() {
    switch (lockout_->GetMode()) {
        case LockoutTracker::Mode::kPermanent:
            cb_->onLockoutPermanent();
            return true;
        case LockoutTracker::Mode::kTimed: {
            auto left = lockout_->TimeLeft();
            cb_->onLockoutTimed(left.count());
            worker_->PostDelayed(left, [weak = std::weak_ptr<Session>(ref<Session>())] {
                auto self = weak.lock();
                if (self && self->lockout_->GetMode() == LockoutTracker::Mode::kNone) {
                    self->cb_->onLockoutCleared();
                }
            });
            return true;
        }
        case LockoutTracker::Mode::kNone:
            return false;
    }
    return false;
}

ndk::ScopedAStatus Session::enumerateEnrollments() {
    worker_->Post(
            [self = ref<Session>()] { self->cb_->onEnrollmentsEnumerated(self->Enrollments()); });
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::removeEnrollments(const std::vector<int32_t>& enrollment_ids) {
    worker_->Post([self = ref<Session>(), enrollment_ids] {
        LOG(INFO) << "removeEnrollments";
        {
            std::lock_guard<std::mutex> lock(*self->engine_.lock);
            for (int32_t fid : enrollment_ids) {
                int ret = self->engine_.lib->ss_face_remove(self->user_id_, fid);
                if (ret) LOG(ERROR) << "ss_face_remove " << fid << ": " << ret;
            }
        }
        unlink((self->StorePath() + "/" + kEnrollmentFile).c_str());
        self->cb_->onEnrollmentsRemoved(enrollment_ids);
    });
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::getFeatures() {
    // No attention detection: config_face_attention_supported is false.
    worker_->Post([self = ref<Session>()] { self->cb_->onFeaturesRetrieved({}); });
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::setFeature(const HardwareAuthToken& /*hat*/, Feature /*feature*/,
                                       bool /*enabled*/) {
    worker_->Post([self = ref<Session>()] { self->cb_->onError(Error::UNABLE_TO_PROCESS, 0); });
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::getAuthenticatorId() {
    worker_->Post([self = ref<Session>()] {
        uint64_t id;
        {
            std::lock_guard<std::mutex> lock(*self->engine_.lock);
            id = self->engine_.lib->ss_face_get_auth_id();
        }
        self->cb_->onAuthenticatorIdRetrieved(id);
    });
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::invalidateAuthenticatorId() {
    // The authenticator id lives in the trustlet's sid.dat and only changes with a new enrollment.
    worker_->Post([self = ref<Session>()] {
        uint64_t id;
        {
            std::lock_guard<std::mutex> lock(*self->engine_.lock);
            id = self->engine_.lib->ss_face_get_auth_id();
        }
        self->cb_->onAuthenticatorIdInvalidated(id);
    });
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::resetLockout(const HardwareAuthToken& /*hat*/) {
    worker_->Post([self = ref<Session>()] {
        self->lockout_->Reset(true);
        self->cb_->onLockoutCleared();
    });
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::close() {
    worker_->Post([self = ref<Session>()] {
        if (self->state_ != State::kIdle) self->EndCapture(true);
        self->cb_->onSessionClosed();
    });
    closed_ = true;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::authenticateWithContext(int64_t operation_id,
                                                    const OperationContext& /*context*/,
                                                    std::shared_ptr<ICancellationSignal>* out) {
    return authenticate(operation_id, out);
}

ndk::ScopedAStatus Session::enrollWithContext(const HardwareAuthToken& hat, EnrollmentType type,
                                              const std::vector<Feature>& features,
                                              const std::optional<NativeHandle>& preview_surface,
                                              const OperationContext& /*context*/,
                                              std::shared_ptr<ICancellationSignal>* out) {
    return enroll(hat, type, features, preview_surface, out);
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
    *out = MakeCancellationSignal();
    StartEnroll(options.hardwareAuthToken,
                options.surfacePreview ? options.surfacePreview->get() : nullptr);
    return ndk::ScopedAStatus::ok();
}

}  // namespace aidl::android::hardware::biometrics::face
