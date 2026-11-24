/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "IrisPreview"

#include "Preview.h"

#include <algorithm>

#include <android-base/logging.h>
#include <android/native_window_aidl.h>
#include <vndk/window.h>

namespace aidl::android::hardware::biometrics::face {

namespace {

// BT.601 limited range, in 8.8 fixed point
uint32_t Rgba(uint8_t y, uint8_t v, uint8_t u) {
    int c = (static_cast<int>(y) - 16) * 298;  // 1.1643
    int d = static_cast<int>(u) - 128;
    int e = static_cast<int>(v) - 128;
    auto clamp = [](int x) { return static_cast<uint32_t>(std::clamp((x + 128) >> 8, 0, 255)); };
    uint32_t r = clamp(c + 409 * e);            // 1.5958
    uint32_t g = clamp(c - 100 * d - 208 * e);  // 0.39173, 0.81290
    uint32_t b = clamp(c + 516 * d);            // 2.017
    return 0xff000000u | b << 16 | g << 8 | r;
}

}  // namespace

void Preview::Window::reset(ANativeWindow* w) {
    if (w) {
        ANativeWindow_acquire(w);
        // Async mode: a new image replaces one the app has not latched yet, so render() on the
        // capture thread never waits for a view that stopped drawing (e.g. with the screen off).
        if (ANativeWindow_setSwapInterval(w, 0)) LOG(WARNING) << "No async preview surface";
    }
    if (window) ANativeWindow_release(window);
    window = w;
    width = height = 0;
}

Preview::~Preview() {
    mEnroll.reset(nullptr);
    mCard.reset(nullptr);
}

ndk::ScopedAStatus Preview::setPreviewSurface(
        const std::optional<::aidl::android::view::Surface>& surface) {
    std::lock_guard<std::mutex> lock(mLock);
    mCard.reset(surface ? surface->get() : nullptr);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Preview::setListener(const std::shared_ptr<IIrisPreviewListener>& listener) {
    std::lock_guard<std::mutex> lock(mLock);
    mListener = listener;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Preview::resumeCapture() {
    std::function<void(bool)> control;
    {
        std::lock_guard<std::mutex> lock(mLock);
        control = mControl;
    }
    if (control) control(true);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Preview::endCapture() {
    std::function<void(bool)> control;
    {
        std::lock_guard<std::mutex> lock(mLock);
        control = mControl;
    }
    if (control) control(false);
    return ndk::ScopedAStatus::ok();
}

void Preview::setControl(std::function<void(bool resume)> control) {
    std::lock_guard<std::mutex> lock(mLock);
    mControl = std::move(control);
}

bool Preview::hasListener() {
    std::lock_guard<std::mutex> lock(mLock);
    return mListener != nullptr;
}

void Preview::setEnrollSurface(ANativeWindow* window) {
    std::lock_guard<std::mutex> lock(mLock);
    mEnroll.reset(window);
}

void Preview::render(const uint8_t* nv21, int32_t width, int32_t height) {
    std::lock_guard<std::mutex> lock(mLock);
    Window& target = mEnroll.window ? mEnroll : mCard;
    if (!target.window) return;

    if (target.width != width || target.height != height) {
        if (ANativeWindow_setBuffersGeometry(target.window, width, height,
                                             AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM)) {
            LOG(ERROR) << "Failed to size the preview surface";
            target.reset(nullptr);
            return;
        }
        target.width = width;
        target.height = height;
    }

    ANativeWindow_Buffer buffer;
    if (ANativeWindow_lock(target.window, &buffer, nullptr)) {
        // The surface went away; the app sets a new one.
        target.reset(nullptr);
        return;
    }
    const uint8_t* vu = nv21 + static_cast<size_t>(width) * height;
    for (int32_t row = 0; row < height; row++) {
        const uint8_t* y = nv21 + static_cast<size_t>(row) * width;
        const uint8_t* c = vu + static_cast<size_t>(row / 2) * width;
        uint32_t* out = static_cast<uint32_t*>(buffer.bits) + static_cast<size_t>(row) * buffer.stride;
        for (int32_t x = 0; x < width; x++) out[x] = Rgba(y[x], c[x & ~1], c[x | 1]);
    }
    ANativeWindow_unlockAndPost(target.window);
}

void Preview::notifyCaptureStarted(int32_t faceReason) {
    std::shared_ptr<IIrisPreviewListener> listener;
    {
        std::lock_guard<std::mutex> lock(mLock);
        listener = mListener;
    }
    if (listener) listener->onCaptureStarted(faceReason);
}

void Preview::notifyCaptureHeld(bool timedOut) {
    std::shared_ptr<IIrisPreviewListener> listener;
    {
        std::lock_guard<std::mutex> lock(mLock);
        listener = mListener;
    }
    if (listener) listener->onCaptureHeld(timedOut);
}

void Preview::notifyAcquired(int32_t code) {
    std::shared_ptr<IIrisPreviewListener> listener;
    {
        std::lock_guard<std::mutex> lock(mLock);
        listener = mListener;
    }
    if (listener) listener->onAcquired(code);
}

}  // namespace aidl::android::hardware::biometrics::face
