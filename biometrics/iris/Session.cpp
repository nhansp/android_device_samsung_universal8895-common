/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "IrisSession"

#include "Session.h"

#include <aidl/android/hardware/biometrics/common/AuthenticateReason.h>
#include <aidl/android/hardware/biometrics/common/BnCancellationSignal.h>
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

#include "CameraProtocol.h"

namespace aidl::android::hardware::biometrics::face {

using ::aidl::android::hardware::biometrics::common::AuthenticateReason;
using ::aidl::android::hardware::biometrics::common::BnCancellationSignal;
using ::aidl::vendor::lineage::biometrics::iris::IrisAcquired;
using ::aidl::android::hardware::keymaster::HardwareAuthenticatorType;
using namespace std::chrono_literals;

namespace {

// Stock IRV1Controller.getIRRotationPhone() for the natural (portrait) display rotation.
constexpr int kIrRotation = 270;
// Stock IrisService eye safety timeout: the IR LED never runs longer than this per capture.
constexpr auto kEyeSafetyTimeout = 9000ms;
// /data/vendor/biometrics is created by init; the library creates the per-user directory.
constexpr char kStoreDir[] = "/data/vendor/biometrics/iris/User_%d";
// The library keeps one template per user (<gid>tmpl.dat) and no enrollment ids; the id it
// reported at enrollment is kept here.
constexpr char kEnrollmentFile[] = "enrollment";
// Stock IRController: the IR camera parameters the iris library steers, with their limits.
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

HardwareAuthToken fromLegacy(const hw_auth_token_t& hat) {
    HardwareAuthToken token;
    token.challenge = hat.challenge;
    token.userId = hat.user_id;
    token.authenticatorId = hat.authenticator_id;
    token.authenticatorType = static_cast<HardwareAuthenticatorType>(be32toh(hat.authenticator_type));
    token.timestamp.milliSeconds = be64toh(hat.timestamp);
    token.mac.assign(std::begin(hat.hmac), std::end(hat.hmac));
    return token;
}

Error toError(int32_t error, int32_t* vendorCode) {
    *vendorCode = 0;
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
            *vendorCode = error;
            return Error::VENDOR;
    }
}

void onClientDeath(void* cookie) {
    LOG(INFO) << "FaceService has died";
    auto* session = static_cast<Session*>(cookie);
    if (!session->isClosed()) session->close();
}

}  // namespace

Session::Session(IrisLib* lib, Worker* worker, LockoutTracker* lockout, Preview* preview,
                 PowerBoost* boost, int32_t userId, std::shared_ptr<ISessionCallback> cb)
    : mLib(lib),
      mWorker(worker),
      mLockout(lockout),
      mPreview(preview),
      mBoost(boost),
      mUserId(userId),
      mCb(std::move(cb)) {
    mDeathRecipient = AIBinder_DeathRecipient_new(onClientDeath);
    mLib->ss_iris_set_active_group(mUserId, storePath().c_str());
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
        uint64_t challenge = self->mLib->ss_iris_pre_enroll();
        LOG(INFO) << "generateChallenge";
        self->mCb->onChallengeGenerated(challenge);
    });
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::revokeChallenge(int64_t challenge) {
    mWorker->post([self = ref<Session>(), challenge] {
        self->mLib->ss_iris_post_enroll();
        self->mCb->onChallengeRevoked(challenge);
    });
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::getEnrollmentConfig(EnrollmentType /*type*/,
                                                std::vector<EnrollmentStageConfig>* out) {
    *out = {};
    return ndk::ScopedAStatus::ok();
}

bool Session::startCapture(State state) {
    std::map<std::string, std::string> params;
    std::weak_ptr<Session> weak = ref<Session>();
    uint64_t op = mOp.load();
    IrisCapture::Callbacks callbacks = {
            .onIrFrame =
                    [this](int fd) {
                        mLib->ss_iris_processIRImage(fd, iris_camera::kWidth,
                                                     iris_camera::kHeight, kIrRotation);
                    },
            // Stock IrisPreviewDataCallback() passes the IR rotation for the front data too.
            .onFrontFrame =
                    [this](int fd) {
                        mLib->ss_iris_processFrontImage(fd, iris_camera::kFrontWidth,
                                                        iris_camera::kFrontHeight, kIrRotation);
                    },
            .onEnd =
                    [this, weak, op](int error) {
                        mWorker->post([weak, op, error] {
                            if (auto self = weak.lock()) self->onError(op, -error);
                        });
                    },
    };
    mBoost->start();
    if (!mCapture.start(&params, std::move(callbacks))) {
        mBoost->end();
        return false;
    }

    // Stock IRV1Controller.startIRCamera(): seed the library with the camera's IR settings.
    for (const auto& prop : kIrProperties) {
        mLib->ss_iris_sendIRProperty(prop[0], params[prop[0]].c_str(), params[prop[1]].c_str(),
                                     params[prop[2]].c_str());
    }
    mLib->ss_iris_request(IRIS_REQUEST_IR_PREVIEW_ENABLE, nullptr, 0, nullptr, 0, 0);
    mLib->ss_iris_request(IRIS_REQUEST_SECURE_ENABLE, nullptr, 0, nullptr, 0, 0);
    mState = state;

    mWorker->postDelayed(kEyeSafetyTimeout, [weak, op] {
        if (auto self = weak.lock()) self->onTimeout(op);
    });
    return true;
}

void Session::endCapture(bool cancelLibrary) {
    mOp++;
    mState = State::IDLE;
    mCapture.stop();
    if (cancelLibrary) mLib->ss_iris_cancel();
    mLib->ss_iris_request(IRIS_REQUEST_SECURE_DISABLE, nullptr, 0, nullptr, 0, 0);
    mPreview->setEnrollSurface(nullptr);
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
        self->mLib->ss_iris_set_active_group(self->mUserId, self->storePath().c_str());
        self->mPreview->setEnrollSurface(window.get());
        if (!self->startCapture(State::ENROLLING)) {
            self->mPreview->setEnrollSurface(nullptr);
            self->mCb->onError(Error::HW_UNAVAILABLE, 0);
            return;
        }
        hw_auth_token_t authToken = toLegacy(hat);
        int ret = self->mLib->ss_iris_enroll(&authToken, self->mUserId, 0);
        if (ret) {
            LOG(ERROR) << "ss_iris_enroll: " << ret;
            self->endCapture(true);
            self->mCb->onError(Error::UNABLE_TO_PROCESS, ret);
        }
    });
}

ndk::ScopedAStatus Session::authenticate(int64_t operationId,
                                         std::shared_ptr<ICancellationSignal>* out) {
    *out = cancellationSignal();
    authenticate(operationId, static_cast<int32_t>(AuthenticateReason::Face::UNKNOWN));
    return ndk::ScopedAStatus::ok();
}

void Session::authenticate(int64_t operationId, int32_t faceReason) {
    mWorker->post([self = ref<Session>(), operationId, faceReason] {
        LOG(INFO) << "authenticate";
        if (self->mState != State::IDLE) {
            self->mCb->onError(Error::UNABLE_TO_PROCESS, 0);
            return;
        }
        if (self->checkLockout()) return;
        self->mLib->ss_iris_set_active_group(self->mUserId, self->storePath().c_str());
        if (!self->startCapture(State::AUTHENTICATING)) {
            self->mCb->onError(Error::HW_UNAVAILABLE, 0);
            return;
        }
        int ret = self->mLib->ss_iris_authenticate(operationId, self->mUserId, nullptr, 0);
        if (ret) {
            LOG(ERROR) << "ss_iris_authenticate: " << ret;
            self->endCapture(true);
            self->mCb->onError(Error::UNABLE_TO_PROCESS, ret);
            return;
        }
        self->mPreview->notifyCaptureStarted(faceReason);
    });
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

void Session::notify(const IrisMsg* msg) {
    uint64_t op = mOp.load();
    std::weak_ptr<Session> weak = ref<Session>();
    switch (msg->type) {
        case IRIS_MSG_IR_PROPERTY_CHANGED:
            // Stock IRV1Controller.onIRPropertyChanged(): the library's exposure and IR LED
            // settings for the next frames.
            if (msg->data.property.value) mCapture.setParameters(msg->data.property.value);
            break;
        case IRIS_MSG_IMAGE_PROCESSED:
            // Sent from ss_iris_processIRImage() on the capture thread; the image is the
            // library's and only valid until we return.
            if (msg->data.image.id == kRgbPreviewImageId && msg->data.image.data &&
                msg->data.image.width > 0 && msg->data.image.height > 0) {
                mPreview->render(msg->data.image.data, msg->data.image.width,
                                 msg->data.image.height);
            }
            break;
        case IRIS_MSG_ACQUIRED: {
            int32_t code;
            memcpy(&code, reinterpret_cast<const uint8_t*>(msg) + kAcquiredCodeOffset,
                   sizeof(code));
            mWorker->post([weak, op, code] {
                if (auto self = weak.lock()) self->onAcquired(op, code);
            });
        } break;
        case IRIS_MSG_TEMPLATE_ENROLLING: {
            uint32_t fid = msg->data.enroll.fid, progress = msg->data.enroll.progress;
            mWorker->post([weak, op, fid, progress] {
                if (auto self = weak.lock()) self->onTemplateEnrolling(op, fid, progress);
            });
        } break;
        case IRIS_MSG_AUTHENTICATED: {
            uint32_t fid = msg->data.authenticated.fid;
            hw_auth_token_t hat = msg->data.authenticated.hat;
            mWorker->post([weak, op, fid, hat] {
                if (auto self = weak.lock()) self->onAuthenticated(op, fid, hat);
            });
        } break;
        case IRIS_MSG_ERROR: {
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
    switch (static_cast<IrisAcquired>(code)) {
        case IrisAcquired::MOVE_CLOSER:
        case IrisAcquired::MOVE_FARTHER:
        case IrisAcquired::OPEN_WIDER:
        case IrisAcquired::MOVE_SOMEWHERE_DARKER:
        case IrisAcquired::CHANGE_POSITION:
            mPreview->notifyAcquired(code);
            break;
        default:
            return;
    }
    BaseFrame frame;
    frame.acquiredInfo = AcquiredInfo::VENDOR;
    frame.vendorCode = code;
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

void Session::onAuthenticated(uint64_t op, uint32_t fid, const hw_auth_token_t& hat) {
    if (mOp.load() != op || mState != State::AUTHENTICATING) return;
    endCapture(false);
    if (fid != 0) {
        LOG(INFO) << "onAuthenticationSucceeded";
        mLockout->reset(true);
        mCb->onAuthenticationSucceeded(fid, fromLegacy(hat));
    } else {
        LOG(INFO) << "onAuthenticationFailed";
        mLockout->addFailedAttempt();
        mCb->onAuthenticationFailed();
        checkLockout();
    }
}

void Session::onError(uint64_t op, int32_t error) {
    if (mOp.load() != op || mState == State::IDLE) return;
    LOG(ERROR) << "Iris error " << error;
    endCapture(true);
    int32_t vendorCode;
    Error result = toError(error, &vendorCode);
    mCb->onError(result, vendorCode);
}

void Session::onTimeout(uint64_t op) {
    if (mOp.load() != op || mState == State::IDLE) return;
    LOG(INFO) << "Eye safety timeout";
    // The IR LED goes off now. A frame the library is still processing may complete the
    // enrollment or match; its result is queued behind this task, so decide after it.
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
        for (int32_t fid : enrollmentIds) {
            int ret = self->mLib->ss_iris_remove(self->mUserId, fid);
            if (ret) LOG(ERROR) << "ss_iris_remove " << fid << ": " << ret;
        }
        unlink((self->storePath() + "/" + kEnrollmentFile).c_str());
        self->mCb->onEnrollmentsRemoved(enrollmentIds);
    });
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::getFeatures() {
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
        self->mCb->onAuthenticatorIdRetrieved(self->mLib->ss_iris_get_auth_id());
    });
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::invalidateAuthenticatorId() {
    // The authenticator id lives in the trustlet's sid.dat and only changes with a new enrollment.
    mWorker->post([self = ref<Session>()] {
        self->mCb->onAuthenticatorIdInvalidated(self->mLib->ss_iris_get_auth_id());
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
                                                    const OperationContext& context,
                                                    std::shared_ptr<ICancellationSignal>* out) {
    *out = cancellationSignal();
    // Keyguard tells a bouncer retry from a lockscreen attempt by this (OperationContextExt).
    AuthenticateReason::Face reason = AuthenticateReason::Face::UNKNOWN;
    if (context.authenticateReason &&
        context.authenticateReason->getTag() == AuthenticateReason::faceAuthenticateReason) {
        reason = context.authenticateReason->get<AuthenticateReason::faceAuthenticateReason>();
    }
    authenticate(operationId, static_cast<int32_t>(reason));
    return ndk::ScopedAStatus::ok();
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
