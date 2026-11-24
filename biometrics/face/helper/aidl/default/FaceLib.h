/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <hardware/hw_auth_token.h>

namespace aidl::android::hardware::biometrics::face {

// Messages of the stock face library
enum FaceMsgType : int32_t {
    kFaceMsgError = -1,
    kFaceMsgAcquired = 1,
    // type 2 is invalid
    kFaceMsgTemplateEnrolling = 3,
    kFaceMsgTemplateRemoved = 4,
    kFaceMsgAuthenticated = 5,
    // The library never sends 6 or 7
    kFaceMsgBroadcastEvent = 6,
    kFaceMsgTemplateEnumerating = 7,
};

struct FaceMsg {
    int32_t type;
    int32_t pad;
    union {
        // 1004 for face_error 0xe and 0x3ec (broken database), 1 for every other internal
        // error (8, 9, 0xc, 0xd, 0xf)
        int32_t error;
        // 0 live face .. 7, see FaceAcquired
        int32_t acquired;
        // Sent after every enroll frame. progress is 0..100; the template is stored when it
        // reaches 100, fid is then (number of stored templates before it) + 1.
        struct {
            uint32_t gid;
            uint32_t fid;
            uint32_t progress;
            uint32_t pad;
            uint64_t unused;  // always 0
        } enroll;
        // fid 0 when everything was removed
        struct {
            uint32_t gid;
            uint32_t fid;
        } removed;
        // fid 0 is a failed match (sent after 5 non-matching frames). The library never fills
        // in the hat: on a match its 69 bytes are uninitialised stack, and the signed token
        // stays in a private field of the library.
        struct {
            uint32_t gid;
            uint32_t fid;
            hw_auth_token_t hat;
            const uint8_t* result;  // "wrapped object" from the trustlet, at most 0x200 bytes
            uint32_t result_len;
        } authenticated;
    } data;
};
static_assert(offsetof(FaceMsg, data.enroll.gid) == 0x8);
static_assert(offsetof(FaceMsg, data.enroll.fid) == 0xc);
static_assert(offsetof(FaceMsg, data.enroll.progress) == 0x10);
static_assert(offsetof(FaceMsg, data.removed.fid) == 0xc);
static_assert(offsetof(FaceMsg, data.authenticated.hat) == 0x10);
static_assert(offsetof(FaceMsg, data.authenticated.result) == 0x58);
static_assert(offsetof(FaceMsg, data.authenticated.result_len) == 0x60);
static_assert(sizeof(hw_auth_token_t) == 0x45);

// Acquired codes: the engine status of an enroll or authenticate frame, passed through
enum FaceAcquired : int32_t {
    kFaceAcquiredLiveFace = 0,
    kFaceAcquiredProcessFail = 1,
    kFaceAcquiredNoneFace = 2,
    kFaceAcquiredLowQuality = 3,
    kFaceAcquiredFakeFace = 4,
    kFaceAcquiredBigFace = 5,
    kFaceAcquiredSmallFace = 6,
    kFaceAcquiredMisalignedFace = 7,
    // Sent on init and deinit of every session; they only announce the stock CPU-core
    // migration and mean nothing to AIDL
    kFaceAcquiredLsiCoreMigrationOn = 1001,
    kFaceAcquiredLsiCoreMigrationOff = 1002,
};

constexpr int32_t kFaceErrorBrokenDatabase = 1004;

// ss_face_request() commands; any other command returns 0 without touching the output buffer
enum FaceRequest : uint32_t {
    // out[i] = first character of the decimal fid, i < min(out_len, count); returns the number
    // of templates
    kFaceRequestEnumerate = 11,
    // Opens the trustlet session outside of enroll/auth
    kFaceRequestSessionOpen = 12,
    // Returns 1 when no session is open, else 0
    kFaceRequestSessionStatus = 13,
    // Closes the trustlet session, returns 0
    kFaceRequestSessionClose = 14,
    // in = 69-byte hat; moves the interim template (enroll flags 1) to template 1 and rewrites
    // sid.dat. Returns 0.
    kFaceRequestMigrateInterimTemplate = 15,
    // in = 69-byte hat; re-encrypts sid.dat with the hat's user id. Returns 0, -9 for a bad
    // token length.
    kFaceRequestUpdateSecureId = 16,
    // Stub that returns 5
    kFaceRequestDvfsFrequency = 1004,
};

// Samsung image type of ss_face_processFrontImage's last argument, not an ImageFormat value
enum FaceImageFormat : int32_t {
    kFaceImageFormatNv21 = 0,
    kFaceImageFormatYu12 = 1,
    kFaceImageFormatYuv420888 = 2,
};

// The engine takes a fixed 640x480 frame whatever width/height the caller passes, and copies
// 0x70800 bytes from the pointer without looking at the length
constexpr int kFrameWidth = 640;
constexpr int kFrameHeight = 480;
constexpr size_t kFrameBytes = 0x70800;  // 640 * 480 * 3 / 2

typedef void (*face_notify_t)(const FaceMsg* msg);

// libFaceService.so, the stock face library. None of the calls takes a lock or starts a thread
// and every callback runs in the thread that made the call, so the HAL has to serialize them.
struct FaceLib {
    // Ignores the argument, allocates the library state once, returns 0
    int (*ss_face_open)(void* unused);
    // Always 0
    int (*ss_face_close)();
    // -2 when the library is not open. Set after open and before anything else.
    int (*ss_set_notify_callback)(face_notify_t notify);
    // Returns the challenge, 0 on failure (error 9), -2 when not open
    uint64_t (*ss_face_pre_enroll)();
    // timeout_sec is never read, flags 0 = normal enroll (hat required), 1 = interim
    // enroll (hat may be NULL, template kept in a hidden file until request 15), >1 is -2.
    // gid must equal the active group, else -2.
    int (*ss_face_enroll)(const hw_auth_token_t* hat, uint32_t gid, uint32_t timeout_sec,
                          uint32_t flags);
    // Clears the cached encrypted challenge, returns 0
    int (*ss_face_post_enroll)();
    // The authenticator id, 0 when nothing is enrolled, -2 when not open
    uint64_t (*ss_face_get_auth_id)();
    // Closes the session if one is open, returns 0
    int (*ss_face_cancel)();
    // Stubs that return 0 and never call back
    int (*ss_face_enumerate)();
    int (*ss_face_cancelEnumeration)();
    // fid 0 removes every template and sid.dat. gid != active group is -2, a failed
    // delete sends error 0xe. Sends kFaceMsgTemplateRemoved.
    int (*ss_face_remove)(uint32_t gid, uint32_t fid);
    // store_path is at most 100 bytes, the directory and its parent (path minus the
    // last 3 characters) are created with mode 0770. Also reloads the authenticator id.
    int (*ss_face_set_active_group)(uint32_t gid, const char* store_path);
    // challenge is optional, at most 0x100 bytes. secure_level goes to the trustlet.
    int (*ss_face_authenticate)(uint64_t operation_id, uint32_t gid, uint32_t secure_level,
                                const uint8_t* challenge, uint32_t challenge_len);
    int (*ss_face_request)(uint32_t cmd, const uint8_t* in, uint32_t in_len, uint8_t* out,
                           uint32_t out_len, uint32_t param);
    // Returns the engine's keepProcessing: 1 when the session is over, any other value (0, 2)
    // to keep feeding frames; -2 when the library is not open.
    int (*ss_face_processFrontImage)(const uint8_t* data, int data_len, int width, int height,
                                     int rotation, int format);

    bool Load();
};

}  // namespace aidl::android::hardware::biometrics::face
