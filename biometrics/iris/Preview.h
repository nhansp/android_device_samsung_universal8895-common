/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>

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

    ndk::ScopedAStatus setPreviewSurface(const std::optional<::aidl::android::view::Surface>& surface)
            override;
    ndk::ScopedAStatus setListener(const std::shared_ptr<IIrisPreviewListener>& listener) override;

    // The surface from FaceEnrollOptions for the current enrollment, or null when it ends.
    void setEnrollSurface(ANativeWindow* window);
    // One NV21 preview image, stride = width.
    void render(const uint8_t* nv21, int32_t width, int32_t height);
    void notifyAcquired(int32_t code);
    void notifyCaptureStarted(int32_t faceReason);

  private:
    struct Window {
        ANativeWindow* window = nullptr;
        int32_t width = 0;
        int32_t height = 0;

        void reset(ANativeWindow* w);
    };

    std::mutex mLock;
    Window mEnroll;
    Window mCard;
    std::shared_ptr<IIrisPreviewListener> mListener;
};

}  // namespace aidl::android::hardware::biometrics::face
