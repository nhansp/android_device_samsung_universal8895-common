/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <hardware/hw_auth_token.h>

namespace aidl::android::hardware::biometrics::face {

// Messages of the stock iris library (iris.default.so passes them to irisd unchanged).
enum IrisMsgType : int32_t {
    kIrisMsgError = -1,
    kIrisMsgAcquired = 1,
    kIrisMsgTemplateEnrolling = 3,
    kIrisMsgTemplateRemoved = 4,
    kIrisMsgAuthenticated = 5,
    kIrisMsgIrPropertyChanged = 6,
    kIrisMsgBroadcastEvent = 7,
    kIrisMsgTemplateEnumerating = 8,
    kIrisMsgImageProcessed = 9,
};

struct IrisMsg {
    int32_t type;
    int32_t pad;
    union {
        int32_t error;
        // Sent for every enrollment frame. progress reaches 100 when the template is stored.
        struct {
            uint32_t gid;
            uint32_t fid;
            uint32_t progress;
        } enroll;
        struct {
            uint32_t gid;
            uint32_t fid;
        } removed;
        struct {
            uint32_t gid;
            uint32_t fid;
            hw_auth_token_t hat;
        } authenticated;
        struct {
            const char* key;
            uint64_t key_len;
            const char* value;
            uint64_t value_len;
        } property;
        // The preview of the frame being processed, valid during the callback only. id 1 is the
        // RGB preview from the front camera's iris data (192x108 NV21); id 0 is the IR one.
        struct {
            int32_t id;
            int32_t pad;
            const uint8_t* data;
            int32_t width;
            int32_t height;
        } image;
    } data;
};
static_assert(offsetof(IrisMsg, data.authenticated.hat) == 0x10);
static_assert(offsetof(IrisMsg, data.property.value) == 0x18);
static_assert(offsetof(IrisMsg, data.image.data) == 0x10);
static_assert(offsetof(IrisMsg, data.image.width) == 0x18);
static_assert(offsetof(IrisMsg, data.image.height) == 0x1c);

constexpr int32_t kRgbPreviewImageId = 1;

// kIrisMsgAcquired carries a 0x810-byte _EyeInfos after the type; the acquired code is its int
// at +0x808.
constexpr size_t kAcquiredCodeOffset = 8 + 0x808;

// ss_iris_request() commands
enum IrisRequest : uint32_t {
    // param 0: RGB preview from the front camera; 1: IR preview
    kIrisRequestIrPreviewEnable = 2005,
    kIrisRequestSecureEnable = 20000,  // opens the trustlet session for a capture
    kIrisRequestSecureDisable = 20001,
};

using IrisNotify = void (*)(const IrisMsg* msg);

// libIrisService.so, the stock iris library behind iris.default.so
struct IrisLib {
    int (*ss_iris_open)(void* id);
    int (*ss_iris_close)();
    int (*ss_set_notify_callback)(IrisNotify notify);
    uint64_t (*ss_iris_pre_enroll)();
    int (*ss_iris_enroll)(const hw_auth_token_t* hat, uint32_t gid, uint32_t timeout_sec);
    int (*ss_iris_post_enroll)();
    uint64_t (*ss_iris_get_auth_id)();
    int (*ss_iris_cancel)();
    int (*ss_iris_remove)(uint32_t gid, uint32_t fid);
    int (*ss_iris_set_active_group)(uint32_t gid, const char* store_path);
    int (*ss_iris_authenticate)(uint64_t operation_id, uint32_t gid, uint8_t* fido, uint32_t len);
    int (*ss_iris_request)(uint32_t cmd, const uint8_t* in, uint32_t in_len, uint8_t* out,
                           uint32_t out_len, uint32_t param);
    void (*ss_iris_processIRImage)(int fd, int width, int height, int rotation);
    // The front camera's iris data
    void (*ss_iris_processFrontImage)(int fd, int width, int height, int rotation);
    void (*ss_iris_sendIRProperty)(const char* key, const char* value, const char* min,
                                   const char* max);

    bool Load();
};

}  // namespace aidl::android::hardware::biometrics::face
