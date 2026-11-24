/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "FacePreview"

#include "Preview.h"

#include <algorithm>

#include <android-base/logging.h>
#include <vndk/window.h>

#include <biometrics_camera/CameraProtocol.h>

namespace aidl::android::hardware::biometrics::face {

namespace {

using face_camera::kHeight;
using face_camera::kWidth;

// BT.601 limited range in 8.8 fixed point
uint32_t Rgba(uint8_t y, uint8_t v, uint8_t u) {
    int c = (static_cast<int>(y) - 16) * 298;
    int d = static_cast<int>(u) - 128;
    int e = static_cast<int>(v) - 128;
    auto clamp = [](int x) { return static_cast<uint32_t>(std::clamp((x + 128) >> 8, 0, 255)); };
    uint32_t r = clamp(c + 409 * e);
    uint32_t g = clamp(c - 100 * d - 208 * e);
    uint32_t b = clamp(c + 516 * d);
    return 0xff000000u | b << 16 | g << 8 | r;
}

}  // namespace

Preview::~Preview() {
    Reset(nullptr);
}

void Preview::Reset(ANativeWindow* window) {
    if (window) {
        ANativeWindow_acquire(window);
        // A new frame replaces one the app has not latched yet, so the capture thread never
        // waits for a view that stopped drawing.
        if (ANativeWindow_setSwapInterval(window, 0)) LOG(WARNING) << "No async preview surface";
    }
    if (window_) ANativeWindow_release(window_);
    window_ = window;
    sized_ = false;
}

void Preview::SetSurface(ANativeWindow* window) {
    std::lock_guard<std::mutex> lock(lock_);
    Reset(window);
}

void Preview::Render(const uint8_t* nv21) {
    std::lock_guard<std::mutex> lock(lock_);
    if (!window_) return;
    if (!sized_) {
        if (ANativeWindow_setBuffersGeometry(window_, kSize, kSize,
                                             AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM)) {
            LOG(ERROR) << "Failed to size the preview surface";
            Reset(nullptr);
            return;
        }
        sized_ = true;
    }

    ANativeWindow_Buffer buffer;
    if (ANativeWindow_lock(window_, &buffer, nullptr)) {
        // The surface went away with the enrollment screen.
        Reset(nullptr);
        return;
    }
    // The frame turned by kRotation (the 270 degrees the face library gets too) is kHeight wide
    // and kWidth tall; mirrored, its middle kSize rows are drawn. For output pixel (x, y):
    // upright mirrored (x, y + top) <- upright (kHeight - 1 - x, y + top)
    // <- sensor (kWidth - 1 - (y + top), kHeight - 1 - x).
    const uint8_t* vu = nv21 + static_cast<size_t>(kWidth) * kHeight;
    const int32_t top = (kWidth - kSize) / 2;
    for (int32_t y = 0; y < kSize; y++) {
        uint32_t* out =
                static_cast<uint32_t*>(buffer.bits) + static_cast<size_t>(y) * buffer.stride;
        const int32_t sx = kWidth - 1 - (y + top);
        for (int32_t x = 0; x < kSize; x++) {
            const int32_t sy = kHeight - 1 - x;
            const uint8_t* c = vu + static_cast<size_t>(sy / 2) * kWidth;
            out[x] = Rgba(nv21[static_cast<size_t>(sy) * kWidth + sx], c[sx & ~1], c[sx | 1]);
        }
    }
    ANativeWindow_unlockAndPost(window_);
}

}  // namespace aidl::android::hardware::biometrics::face
