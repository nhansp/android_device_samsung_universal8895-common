/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>

#include <functional>
#include <memory>
#include <mutex>

#include <aidl/vendor/lineage/biometrics/iris/BnIrisPreview.h>
#include <android/native_window.h>

namespace aidl::android::hardware::biometrics::face {

using ::aidl::vendor::lineage::biometrics::iris::BnIrisPreview;
using ::aidl::vendor::lineage::biometrics::iris::IIrisPreviewListener;

// Draws the RGB preview of a capture (the iris library's processed front image) into the
// enrollment surface, or else into the surface of the iris card (the bouncer and
// BiometricPrompt), and forwards guidance to it.
class Preview : public BnIrisPreview {
  public:
    ~Preview();

    ndk::ScopedAStatus setPreviewSurface(
            const std::optional<::aidl::android::view::Surface>& surface) override;
    ndk::ScopedAStatus setListener(const std::shared_ptr<IIrisPreviewListener>& listener) override;
    ndk::ScopedAStatus resumeCapture() override;
    ndk::ScopedAStatus endCapture() override;

    // Where resumeCapture() and endCapture() go: the current session.
    void SetControl(std::function<void(bool resume)> control);
    // Whether the iris card listens, so a held attempt has someone to resume or end it.
    bool HasListener();

    // The surface from FaceEnrollOptions for the current enrollment, or null when it ends.
    void SetEnrollSurface(ANativeWindow* window);
    // One NV21 preview image, stride = width.
    void Render(const uint8_t* nv21, int32_t width, int32_t height);
    void NotifyAcquired(int32_t code);
    void NotifyCaptureStarted(int32_t face_reason);
    void NotifyCaptureHeld(bool timed_out);

  private:
    struct Window {
        ANativeWindow* window = nullptr;
        int32_t width = 0;
        int32_t height = 0;

        void Reset(ANativeWindow* w);
    };

    std::mutex lock_;
    Window enroll_;
    Window card_;
    std::shared_ptr<IIrisPreviewListener> listener_;
    std::function<void(bool resume)> control_;
};

}  // namespace aidl::android::hardware::biometrics::face
