/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "IrisSession"

#include "Session.h"

#include <aidl/android/hardware/biometrics/common/AuthenticateReason.h>
#include <aidl/android/hardware/biometrics/common/BnCancellationSignal.h>
#include <aidl/vendor/lineage/biometrics/iris/IIrisPreview.h>
#include <aidl/vendor/lineage/biometrics/iris/IrisAcquired.h>

#include <endian.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/strings.h>

#include <android/native_window_aidl.h>

#include <biometrics_camera/CameraProtocol.h>

namespace aidl::android::hardware::biometrics::face {

using ::aidl::android::hardware::biometrics::common::AuthenticateReason;
using ::aidl::android::hardware::biometrics::common::BnCancellationSignal;
using ::aidl::android::hardware::keymaster::HardwareAuthenticatorType;
using ::aidl::vendor::lineage::biometrics::iris::IIrisPreview;
using ::aidl::vendor::lineage::biometrics::iris::IrisAcquired;
using namespace std::chrono_literals;

namespace {

// IR camera rotation for the natural (portrait) display rotation
constexpr int kIrRotation = 270;
// Eye safety: the IR LED never runs longer than this per capture
constexpr auto kEyeSafetyTimeout = 9000ms;
constexpr std::chrono::milliseconds kHoldTimeout{IIrisPreview::HOLD_TIMEOUT_MS};
// /data/vendor/biometrics is created by init; the library creates the per-user directory.
constexpr char kStoreDir[] = "/data/vendor/biometrics/iris/User_%d";
// The library keeps one template per user (<gid>tmpl.dat) and no enrollment ids; the id it
// reported at enrollment is kept here.
constexpr char kEnrollmentFile[] = "enrollment";
// The IR camera parameters the iris library steers, with their limits
constexpr const char* kIrProperties[][3] = {
        {"gain", "min-gain", "max-gain"},
        {"shutterspeed", "min-shutterspeed", "max-shutterspeed"},
        {"dac", "min-dac", "max-dac"},
        {"pulse-delay", "min-pulse-delay", "max-pulse-delay"},
        {"pulse-width", "min-pulse-width", "max-pulse-width"},
};

class CancellationSignal : public BnCancellationSignal {
  public:
    CancellationSignal(std::shared_ptr<Session> session, uint64_t op)
        : session_(session), op_(op) {}

    ndk::ScopedAStatus cancel() override {
        if (auto session = session_.lock()) session->Cancel(op_);
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

HardwareAuthToken FromLegacy(const hw_auth_token_t& hat) {
    HardwareAuthToken token;
    token.challenge = hat.challenge;
    token.userId = hat.user_id;
    token.authenticatorId = hat.authenticator_id;
    token.authenticatorType =
            static_cast<HardwareAuthenticatorType>(be32toh(hat.authenticator_type));
    token.timestamp.milliSeconds = be64toh(hat.timestamp);
    token.mac.assign(std::begin(hat.hmac), std::end(hat.hmac));
    return token;
}

Error ToError(int32_t error, int32_t* vendor_code) {
    *vendor_code = 0;
    switch (error) {
        case 1:
            return Error::HW_UNAVAILABLE;
        case 2:
            return Error::UNABLE_TO_PROCESS;
        case 3:
            return Error::TIMEOUT;
        case 4:
            return Error::NO_SPACE;
        case 5:
            return Error::CANCELED;
        case 6:
            return Error::UNABLE_TO_REMOVE;
        default:
            *vendor_code = error;
            return Error::VENDOR;
    }
}

// The keyguard attempts the iris card shows on: the bouncer coming up and the swipe up on it.
bool IsBouncerReason(int32_t face_reason) {
    auto reason = static_cast<AuthenticateReason::Face>(face_reason);
    return reason == AuthenticateReason::Face::PRIMARY_BOUNCER_SHOWN ||
           reason == AuthenticateReason::Face::SWIPE_UP_ON_BOUNCER;
}

void OnClientDeath(void* cookie) {
    LOG(INFO) << "FaceService has died";
    auto* session = static_cast<Session*>(cookie);
    if (!session->IsClosed()) session->close();
}

}  // namespace

Session::Session(IrisLib* lib, Worker* worker, LockoutTracker* lockout, Preview* preview,
                 PowerBoost* boost, int32_t user_id, std::shared_ptr<ISessionCallback> cb)
    : lib_(lib),
      worker_(worker),
      lockout_(lockout),
      preview_(preview),
      boost_(boost),
      user_id_(user_id),
      cb_(std::move(cb)) {
    death_recipient_ = AIBinder_DeathRecipient_new(OnClientDeath);
    lib_->ss_iris_set_active_group(user_id_, StorePath().c_str());
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

std::shared_ptr<ICancellationSignal> Session::NewCancellationSignal() {
    return SharedRefBase::make<CancellationSignal>(ref<Session>(), op_.load());
}

ndk::ScopedAStatus Session::generateChallenge() {
    worker_->Post([self = ref<Session>()] {
        uint64_t challenge = self->lib_->ss_iris_pre_enroll();
        LOG(INFO) << "generateChallenge";
        self->cb_->onChallengeGenerated(challenge);
    });
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::revokeChallenge(int64_t challenge) {
    worker_->Post([self = ref<Session>(), challenge] {
        self->lib_->ss_iris_post_enroll();
        self->cb_->onChallengeRevoked(challenge);
    });
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::getEnrollmentConfig(EnrollmentType /*type*/,
                                                std::vector<EnrollmentStageConfig>* out) {
    *out = {};
    return ndk::ScopedAStatus::ok();
}

bool Session::StartCapture(State state) {
    std::map<std::string, std::string> params;
    std::weak_ptr<Session> weak = ref<Session>();
    uint64_t gen = ++gen_;
    IrisCapture::Callbacks callbacks = {
            .on_ir_frame =
                    [this](int fd) {
                        lib_->ss_iris_processIRImage(fd, iris_camera::kWidth, iris_camera::kHeight,
                                                     kIrRotation);
                    },
            // The front data takes the IR rotation too
            .on_front_frame =
                    [this](int fd) {
                        lib_->ss_iris_processFrontImage(fd, iris_camera::kFrontWidth,
                                                        iris_camera::kFrontHeight, kIrRotation);
                    },
            .on_end =
                    [this, weak, gen](int error) {
                        worker_->Post([weak, gen, error] {
                            if (auto self = weak.lock()) self->OnError(gen, -error);
                        });
                    },
    };
    boost_->Start();
    if (!capture_.Start(&params, std::move(callbacks))) {
        boost_->End();
        return false;
    }

    // Seed the library with the camera's IR settings
    for (const auto& prop : kIrProperties) {
        lib_->ss_iris_sendIRProperty(prop[0], params[prop[0]].c_str(), params[prop[1]].c_str(),
                                     params[prop[2]].c_str());
    }
    lib_->ss_iris_request(kIrisRequestIrPreviewEnable, nullptr, 0, nullptr, 0, 0);
    lib_->ss_iris_request(kIrisRequestSecureEnable, nullptr, 0, nullptr, 0, 0);
    state_ = state;

    worker_->PostDelayed(kEyeSafetyTimeout, [weak, gen] {
        if (auto self = weak.lock()) self->OnTimeout(gen);
    });
    return true;
}

void Session::EndCapture(bool cancel_library) {
    op_++;
    gen_++;
    state_ = State::kIdle;
    capture_.Stop();
    if (cancel_library) lib_->ss_iris_cancel();
    lib_->ss_iris_request(kIrisRequestSecureDisable, nullptr, 0, nullptr, 0, 0);
    preview_->SetEnrollSurface(nullptr);
    boost_->End();
}

ndk::ScopedAStatus Session::enroll(const HardwareAuthToken& hat, EnrollmentType /*type*/,
                                   const std::vector<Feature>& /*features*/,
                                   const std::optional<NativeHandle>& /*preview_surface*/,
                                   std::shared_ptr<ICancellationSignal>* out) {
    // The framework passes the preview as a Surface (enrollWithOptions), not a NativeHandle.
    *out = NewCancellationSignal();
    Enroll(hat, nullptr);
    return ndk::ScopedAStatus::ok();
}

void Session::Enroll(const HardwareAuthToken& hat, ANativeWindow* preview_window) {
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
        self->lib_->ss_iris_set_active_group(self->user_id_, self->StorePath().c_str());
        self->preview_->SetEnrollSurface(window.get());
        if (!self->StartCapture(State::kEnrolling)) {
            self->preview_->SetEnrollSurface(nullptr);
            self->cb_->onError(Error::HW_UNAVAILABLE, 0);
            return;
        }
        hw_auth_token_t auth_token = ToLegacy(hat);
        int ret = self->lib_->ss_iris_enroll(&auth_token, self->user_id_, 0);
        if (ret) {
            LOG(ERROR) << "ss_iris_enroll: " << ret;
            self->EndCapture(true);
            self->cb_->onError(Error::UNABLE_TO_PROCESS, ret);
        }
    });
}

ndk::ScopedAStatus Session::authenticate(int64_t operation_id,
                                         std::shared_ptr<ICancellationSignal>* out) {
    *out = NewCancellationSignal();
    Authenticate(operation_id, static_cast<int32_t>(AuthenticateReason::Face::UNKNOWN));
    return ndk::ScopedAStatus::ok();
}

void Session::Authenticate(int64_t operation_id, int32_t face_reason) {
    worker_->Post([self = ref<Session>(), operation_id, face_reason] {
        LOG(INFO) << "authenticate";
        if (self->state_ != State::kIdle) {
            self->cb_->onError(Error::UNABLE_TO_PROCESS, 0);
            return;
        }
        if (self->CheckLockout()) return;
        self->operation_id_ = operation_id;
        self->face_reason_ = face_reason;
        self->lib_->ss_iris_set_active_group(self->user_id_, self->StorePath().c_str());
        if (!self->StartCapture(State::kAuthenticating)) {
            self->cb_->onError(Error::HW_UNAVAILABLE, 0);
            return;
        }
        int ret = self->lib_->ss_iris_authenticate(operation_id, self->user_id_, nullptr, 0);
        if (ret) {
            LOG(ERROR) << "ss_iris_authenticate: " << ret;
            self->EndCapture(true);
            self->cb_->onError(Error::UNABLE_TO_PROCESS, ret);
            return;
        }
        self->preview_->NotifyCaptureStarted(face_reason);
    });
}

ndk::ScopedAStatus Session::detectInteraction(std::shared_ptr<ICancellationSignal>* out) {
    *out = NewCancellationSignal();
    worker_->Post([self = ref<Session>()] { self->cb_->onError(Error::UNABLE_TO_PROCESS, 0); });
    return ndk::ScopedAStatus::ok();
}

void Session::Cancel(uint64_t op) {
    worker_->Post([self = ref<Session>(), op] {
        if (self->op_.load() == op) {
            if (self->state_ == State::kHeld) {
                LOG(INFO) << "cancel held";
                self->ReleaseHeld();
            } else if (self->state_ != State::kIdle) {
                LOG(INFO) << "cancel";
                self->EndCapture(true);
            }
        }
        self->cb_->onError(Error::CANCELED, 0);
    });
}

void Session::Notify(const IrisMsg* msg) {
    uint64_t gen = gen_.load();
    std::weak_ptr<Session> weak = ref<Session>();
    switch (msg->type) {
        case kIrisMsgIrPropertyChanged:
            // The library's exposure and IR LED settings for the next frames
            if (msg->data.property.value) capture_.SetParameters(msg->data.property.value);
            break;
        case kIrisMsgImageProcessed:
            // Sent on the capture thread; the image is the library's and only valid until we
            // return
            if (msg->data.image.id == kRgbPreviewImageId && msg->data.image.data &&
                msg->data.image.width > 0 && msg->data.image.height > 0) {
                preview_->Render(msg->data.image.data, msg->data.image.width,
                                 msg->data.image.height);
            }
            break;
        case kIrisMsgAcquired: {
            int32_t code;
            memcpy(&code, reinterpret_cast<const uint8_t*>(msg) + kAcquiredCodeOffset,
                   sizeof(code));
            worker_->Post([weak, gen, code] {
                if (auto self = weak.lock()) self->OnAcquired(gen, code);
            });
        } break;
        case kIrisMsgTemplateEnrolling: {
            uint32_t fid = msg->data.enroll.fid, progress = msg->data.enroll.progress;
            worker_->Post([weak, gen, fid, progress] {
                if (auto self = weak.lock()) self->OnTemplateEnrolling(gen, fid, progress);
            });
        } break;
        case kIrisMsgAuthenticated: {
            uint32_t fid = msg->data.authenticated.fid;
            hw_auth_token_t hat = msg->data.authenticated.hat;
            worker_->Post([weak, gen, fid, hat] {
                if (auto self = weak.lock()) self->OnAuthenticated(gen, fid, hat);
            });
        } break;
        case kIrisMsgError: {
            int32_t error = msg->data.error;
            worker_->Post([weak, gen, error] {
                if (auto self = weak.lock()) self->OnError(gen, error);
            });
        } break;
        default:
            break;
    }
}

void Session::OnAcquired(uint64_t gen, int32_t code) {
    if (gen_.load() != gen || (state_ != State::kEnrolling && state_ != State::kAuthenticating)) {
        return;
    }
    switch (static_cast<IrisAcquired>(code)) {
        case IrisAcquired::MOVE_CLOSER:
        case IrisAcquired::MOVE_FARTHER:
        case IrisAcquired::OPEN_WIDER:
        case IrisAcquired::MOVE_SOMEWHERE_DARKER:
        case IrisAcquired::CHANGE_POSITION:
            preview_->NotifyAcquired(code);
            break;
        default:
            return;
    }
    BaseFrame frame;
    frame.acquiredInfo = AcquiredInfo::VENDOR;
    frame.vendorCode = code;
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

void Session::OnTemplateEnrolling(uint64_t gen, uint32_t fid, uint32_t progress) {
    if (gen_.load() != gen || state_ != State::kEnrolling) return;
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

void Session::OnAuthenticated(uint64_t gen, uint32_t fid, const hw_auth_token_t& hat) {
    if (gen_.load() != gen || state_ != State::kAuthenticating) return;
    if (fid != 0) {
        LOG(INFO) << "onAuthenticationSucceeded";
        EndCapture(false);
        lockout_->Reset(true);
        cb_->onAuthenticationSucceeded(fid, FromLegacy(hat));
        return;
    }
    // Every no match counts at once, so retrying a held attempt never gets around lockout.
    lockout_->AddFailedAttempt();
    if (lockout_->GetMode() == LockoutTracker::Mode::kNone && CanHold()) {
        Hold(false);
        return;
    }
    LOG(INFO) << "onAuthenticationFailed";
    EndCapture(false);
    cb_->onAuthenticationFailed();
    CheckLockout();
}

void Session::OnError(uint64_t gen, int32_t error) {
    if (gen_.load() != gen || (state_ != State::kEnrolling && state_ != State::kAuthenticating)) {
        return;
    }
    LOG(ERROR) << "Iris error " << error;
    int32_t vendor_code;
    if (ToError(error, &vendor_code) == Error::TIMEOUT && CanHold()) {
        Hold(true);
        return;
    }
    EndCapture(true);
    Error result = ToError(error, &vendor_code);
    cb_->onError(result, vendor_code);
}

void Session::OnTimeout(uint64_t gen) {
    if (gen_.load() != gen || (state_ != State::kEnrolling && state_ != State::kAuthenticating)) {
        return;
    }
    LOG(INFO) << "Eye safety timeout";
    // The IR LED goes off now. A frame the library is still processing may complete the
    // enrollment or match; its result is queued behind this task, so decide after it.
    capture_.Stop();
    worker_->Post([weak = std::weak_ptr<Session>(ref<Session>()), gen] {
        auto self = weak.lock();
        if (!self || self->gen_.load() != gen ||
            (self->state_ != State::kEnrolling && self->state_ != State::kAuthenticating)) {
            return;
        }
        if (self->CanHold()) {
            self->Hold(true);
            return;
        }
        self->EndCapture(true);
        self->cb_->onError(Error::TIMEOUT, 0);
    });
}

bool Session::CanHold() const {
    return state_ == State::kAuthenticating && IsBouncerReason(face_reason_) &&
           preview_->HasListener();
}

void Session::Hold(bool timed_out) {
    LOG(INFO) << "Holding the attempt, timed_out " << timed_out;
    gen_++;
    state_ = State::kHeld;
    held_timed_out_ = timed_out;
    capture_.Stop();
    if (timed_out) lib_->ss_iris_cancel();
    lib_->ss_iris_request(kIrisRequestSecureDisable, nullptr, 0, nullptr, 0, 0);
    boost_->End();
    worker_->PostDelayed(kHoldTimeout,
                         [weak = std::weak_ptr<Session>(ref<Session>()), gen = gen_.load()] {
                             auto self = weak.lock();
                             if (self && self->state_ == State::kHeld && self->gen_.load() == gen) {
                                 self->FinishHeld();
                             }
                         });
    preview_->NotifyCaptureHeld(timed_out);
}

void Session::ReleaseHeld() {
    op_++;
    gen_++;
    state_ = State::kIdle;
}

void Session::FinishHeld() {
    LOG(INFO) << "Ending the held attempt";
    ReleaseHeld();
    if (held_timed_out_) {
        cb_->onError(Error::TIMEOUT, 0);
    } else {
        cb_->onAuthenticationFailed();
    }
}

void Session::OnUserAction(bool resume) {
    worker_->Post([self = ref<Session>(), resume] {
        if (!resume) {
            if (self->state_ == State::kHeld) {
                self->FinishHeld();
            } else if (self->state_ == State::kAuthenticating &&
                       IsBouncerReason(self->face_reason_)) {
                LOG(INFO) << "Attempt ended from the iris card";
                self->EndCapture(true);
                self->cb_->onError(Error::CANCELED, 0);
            }
            return;
        }
        if (self->state_ != State::kHeld) return;
        LOG(INFO) << "Resuming the held attempt";
        self->lib_->ss_iris_set_active_group(self->user_id_, self->StorePath().c_str());
        if (!self->StartCapture(State::kAuthenticating)) {
            self->FinishHeld();
            return;
        }
        int ret = self->lib_->ss_iris_authenticate(self->operation_id_, self->user_id_, nullptr, 0);
        if (ret) {
            LOG(ERROR) << "ss_iris_authenticate: " << ret;
            self->EndCapture(true);
            if (self->held_timed_out_) {
                self->cb_->onError(Error::TIMEOUT, 0);
            } else {
                self->cb_->onAuthenticationFailed();
            }
            return;
        }
        self->preview_->NotifyCaptureStarted(self->face_reason_);
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
        for (int32_t fid : enrollment_ids) {
            int ret = self->lib_->ss_iris_remove(self->user_id_, fid);
            if (ret) LOG(ERROR) << "ss_iris_remove " << fid << ": " << ret;
        }
        unlink((self->StorePath() + "/" + kEnrollmentFile).c_str());
        self->cb_->onEnrollmentsRemoved(enrollment_ids);
    });
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::getFeatures() {
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
        self->cb_->onAuthenticatorIdRetrieved(self->lib_->ss_iris_get_auth_id());
    });
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::invalidateAuthenticatorId() {
    // The authenticator id lives in the trustlet's sid.dat and only changes with a new enrollment.
    worker_->Post([self = ref<Session>()] {
        self->cb_->onAuthenticatorIdInvalidated(self->lib_->ss_iris_get_auth_id());
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
        if (self->state_ == State::kHeld) {
            self->ReleaseHeld();
        } else if (self->state_ != State::kIdle) {
            self->EndCapture(true);
        }
        self->cb_->onSessionClosed();
    });
    closed_ = true;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::authenticateWithContext(int64_t operation_id,
                                                    const OperationContext& context,
                                                    std::shared_ptr<ICancellationSignal>* out) {
    *out = NewCancellationSignal();
    AuthenticateReason::Face reason = AuthenticateReason::Face::UNKNOWN;
    if (context.authenticateReason &&
        context.authenticateReason->getTag() == AuthenticateReason::faceAuthenticateReason) {
        reason = context.authenticateReason->get<AuthenticateReason::faceAuthenticateReason>();
    }
    Authenticate(operation_id, static_cast<int32_t>(reason));
    return ndk::ScopedAStatus::ok();
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
    *out = NewCancellationSignal();
    Enroll(options.hardwareAuthToken,
           options.surfacePreview ? options.surfacePreview->get() : nullptr);
    return ndk::ScopedAStatus::ok();
}

}  // namespace aidl::android::hardware::biometrics::face
