/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <hardware/hw_auth_token.h>

namespace aidl::android::hardware::biometrics::face {

// Messages of the stock face library (face.default.so hands them to faced unchanged;
// faced hal_notify_callback 0x7b8c-0x7ec4 switches on the type with sub w8,w3,#1; cmp #6).
enum FaceMsgType : int32_t {
    FACE_MSG_ERROR = -1,
    FACE_MSG_ACQUIRED = 1,
    // type 2 is rejected by faced (hal_notify_callback 0x7c64, "invalid msg type")
    FACE_MSG_TEMPLATE_ENROLLING = 3,
    FACE_MSG_TEMPLATE_REMOVED = 4,
    FACE_MSG_AUTHENTICATED = 5,
    // No code in libFaceService ever sends 6 or 7 (FaceServiceCallback::sendBroadCastEvent 0x66c0
    // and sendonEnumerate 0x64e0 have no caller); faced would map 6 to the
    // BIOMETRIC_AUTHENTICATION_START/STOP intents and 7 to onEnumerate.
    FACE_MSG_BROADCAST_EVENT = 6,
    FACE_MSG_TEMPLATE_ENUMERATING = 7,
};

struct FaceMsg {
    int32_t type;
    int32_t pad;
    union {
        // FaceServiceCallback::sendError 0x6270: 1004 for face_error 0xe and 0x3ec
        // (broken database), 1 for every other internal error (8, 9, 0xc, 0xd, 0xf)
        int32_t error;
        // FaceServiceCallback::sendAcquired 0x6660. 0 live face .. 7, see FaceAcquired.
        int32_t acquired;
        // FaceServiceCallback::sendTemplateEnrolling 0x65a0, called from processFrontImage
        // 0x59dc after every enroll frame. progress is 0..100; the template is stored when it
        // reaches 100, fid is then (number of stored templates before it) + 1.
        // faced onEnrollResult(fid=+0xc, gid=+8, remaining=+0x10) (hal_notify_callback 0x7e08).
        struct {
            uint32_t gid;
            uint32_t fid;
            uint32_t progress;
            uint32_t pad;
            uint64_t unused;  // processFrontImage passes 0
        } enroll;
        // FaceServiceCallback::sendRemoved 0x6430; fid 0 when everything was removed
        struct {
            uint32_t gid;
            uint32_t fid;
        } removed;
        // FaceServiceCallback::sendAuthenticated 0x6330. fid 0 is a failed match (sent after 5
        // non-matching frames, processFrontImage 0x5984). The hat is only copied when the caller
        // passes one, and processFrontImage always passes NULL (0x58a8 mov x3,#0), so on a match
        // the 69 bytes at +0x10 are uninitialised stack. The signed token that
        // sec_fr_engine_authenticated returns stays in a private FaceService field (+0x428).
        struct {
            uint32_t gid;
            uint32_t fid;
            hw_auth_token_t hat;
            const uint8_t* result;  // "wrapped object" from the trustlet, at most 0x200 bytes
            uint32_t resultLen;
        } authenticated;
    } data;
};
static_assert(offsetof(FaceMsg, data.enroll.gid) == 0x8);
static_assert(offsetof(FaceMsg, data.enroll.fid) == 0xc);
static_assert(offsetof(FaceMsg, data.enroll.progress) == 0x10);
static_assert(offsetof(FaceMsg, data.removed.fid) == 0xc);
static_assert(offsetof(FaceMsg, data.authenticated.hat) == 0x10);
static_assert(offsetof(FaceMsg, data.authenticated.result) == 0x58);
static_assert(offsetof(FaceMsg, data.authenticated.resultLen) == 0x60);
static_assert(sizeof(hw_auth_token_t) == 0x45);

// Acquired codes: engine status of on_enroll_frame/on_authenticate_frame passed straight to
// sendAcquired (processFrontImage 0x5640, 0x58dc); names from FaceUtils.FaceDaemonValue.
enum FaceAcquired : int32_t {
    FACE_ACQUIRED_LIVE_FACE = 0,
    FACE_ACQUIRED_PROCESS_FAIL = 1,
    FACE_ACQUIRED_NONE_FACE = 2,
    FACE_ACQUIRED_LOW_QUALITY = 3,
    FACE_ACQUIRED_FAKE_FACE = 4,
    FACE_ACQUIRED_BIG_FACE = 5,
    FACE_ACQUIRED_SMALL_FACE = 6,
    FACE_ACQUIRED_MISALIGNED_FACE = 7,
    // sec_fr_engine_use_ssrm 0x53d0 is hard-wired to 1, so init/deinit of every session send
    // these two (FaceService::initialize 0x3c58, enroll 0x3efc, deinitialize 0x3ce4). They only
    // announce the stock CPU-core migration and mean nothing to AIDL.
    FACE_ACQUIRED_LSI_CORE_MIGRATION_ON = 1001,
    FACE_ACQUIRED_LSI_CORE_MIGRATION_OFF = 1002,
};

constexpr int32_t kFaceErrorBrokenDatabase = 1004;

// ss_face_request() commands (FaceService::request 0x4b70: sub w8,w1,#0xb; cmp w8,#5 jump table
// at 0x8640, plus cmp w1,#0x3ec at 0x4cbc). Anything else logs "request Default" and returns 0
// without touching the output buffer (10000/10001/2003/3000/50001/50002 are Java-only).
enum FaceRequest : uint32_t {
    // out[i] = first character of the decimal fid ("%d" via 0x4eb0), i < min(outLen, count);
    // returns the number of templates. Java checkIntegrity() reads the digits back.
    FACE_REQUEST_ENUMERATE = 11,
    // Opens the trustlet session (sec_fr_engine_init + init_library) outside of enroll/auth
    FACE_REQUEST_SESSION_OPEN = 12,
    // Returns 1 when no session is open (state at +0x87c == 0), else 0
    FACE_REQUEST_SESSION_STATUS = 13,
    // sec_fr_engine_deinit, state back to 0, returns 0
    FACE_REQUEST_SESSION_CLOSE = 14,
    // in = 69-byte hat; moves the interim template (enroll flags 1) to template 1 and rewrites
    // sid.dat via sec_fr_engine_migrate_interim_template + enrolled. Returns 0.
    // Java: FACE_REQUEST_UPDATE_SECURE_ID_WITH_CHALLENGE
    FACE_REQUEST_MIGRATE_INTERIM_TEMPLATE = 15,
    // in = 69-byte hat; re-encrypts sid.dat with the hat's user id
    // (sec_fr_engine_update_secure_id). Returns 0, -9 for a bad token length.
    FACE_REQUEST_UPDATE_SECURE_ID = 16,
    // sec_fr_engine_get_cpufreq 0x53a0 is a stub that returns 5
    FACE_REQUEST_DVFS_FREQUENCY = 1004,
};

// Samsung image type of processFrontImage's last argument (CamCtrl.FACE_IMAGE_FORMAT_*), not an
// android.graphics.ImageFormat value.
enum FaceImageFormat : int32_t {
    FACE_IMAGE_FORMAT_NV21 = 0,
    FACE_IMAGE_FORMAT_YU12 = 1,
    FACE_IMAGE_FORMAT_YUV_420_888 = 2,
};

// processFrontImage (FaceService 0x54d0) hands the engine a fixed 640x480 frame whatever
// width/height the caller passes (mov w1,#0x280; orr w2,wzr,#0x1e0 at 0x55a8 and 0x5694), and
// sec_fr_engine_on_*_frame copies 0x70800 bytes from the pointer without looking at the length
// (libsecfr_engine 0x34a4 movk w8,#0x804 pair, memcpy at 0x34a0).
constexpr int kFrameWidth = 640;
constexpr int kFrameHeight = 480;
constexpr size_t kFrameBytes = 0x70800;  // 640 * 480 * 3 / 2

typedef void (*face_notify_t)(const FaceMsg* msg);

// libFaceService.so, the stock face library behind face.default.so. None of the calls takes a
// lock or starts a thread and every callback runs in the thread that made the call, so the HAL
// has to serialize them.
struct FaceLib {
    // ss_face_open 0x61b0 ignores x0, allocates the 0x880-byte FaceService once, returns 0
    int (*ss_face_open)(void* unused);
    // 0x5a90, always 0
    int (*ss_face_close)();
    // 0x5f90; -2 when the library is not open. Set after open and before anything else.
    int (*ss_set_notify_callback)(face_notify_t notify);
    // 0x5b00; returns the challenge, 0 on failure (sendError 9), -2 when not open
    uint64_t (*ss_face_pre_enroll)();
    // 0x5b60; timeoutSec is never read, flags 0 = normal enroll (hat required), 1 = interim
    // enroll (hat may be NULL, template kept in a hidden file until request 15), >1 is -2.
    // gid must equal the active group, else -2.
    int (*ss_face_enroll)(const hw_auth_token_t* hat, uint32_t gid, uint32_t timeoutSec,
                          uint32_t flags);
    // 0x5c00, clears the cached encrypted challenge, 0
    int (*ss_face_post_enroll)();
    // 0x5c60; the authenticator id, 0 when nothing is enrolled, -2 when not open
    uint64_t (*ss_face_get_auth_id)();
    // 0x5cc0; deinit of the session if one is open, 0
    int (*ss_face_cancel)();
    // 0x5d20, 0x5d80: stubs that return 0 and never call back
    int (*ss_face_enumerate)();
    int (*ss_face_cancelEnumeration)();
    // 0x5de0; fid 0 removes every template and sid.dat. gid != active group is -2, a failed
    // delete sends error 0xe. Sends FACE_MSG_TEMPLATE_REMOVED.
    int (*ss_face_remove)(uint32_t gid, uint32_t fid);
    // 0x5e60; storePath is at most 100 bytes, the directory and its parent (path minus the
    // last 3 characters) are created with mode 0770. Also reloads the authenticator id.
    int (*ss_face_set_active_group)(uint32_t gid, const char* storePath);
    // 0x5ee0; faced FaceDaemonProxy::authenticate passes (sid, gid, secureLevel, fidoChallenge,
    // fidoLen). challenge is optional, at most 0x100 bytes. secureLevel goes to the trustlet.
    int (*ss_face_authenticate)(uint64_t operationId, uint32_t gid, uint32_t secureLevel,
                                const uint8_t* challenge, uint32_t challengeLen);
    // 0x6010; same shape as ss_iris_request
    int (*ss_face_request)(uint32_t cmd, const uint8_t* in, uint32_t inLen, uint8_t* out,
                           uint32_t outLen, uint32_t param);
    // 0x60d0; returns 1 when the session is over (or none exists), 0 to keep feeding frames
    // (Java also treats 2 as "drop queued frames"; the engine can return it, UNCONFIRMED).
    int (*ss_face_processFrontImage)(const uint8_t* data, int dataLen, int width, int height,
                                     int rotation, int format);

    bool load();
};

}  // namespace aidl::android::hardware::biometrics::face
