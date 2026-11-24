/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>

#include <mutex>

#include <android/native_window.h>

namespace aidl::android::hardware::biometrics::face {

// Draws the camera frames into the surface Settings passes with the enrollment
class Preview {
  public:
    // The side of the square drawn: the upright frame's width.
    static constexpr int32_t kSize = 480;

    ~Preview();

    // The enrollment's surface, or null when it ends.
    void SetSurface(ANativeWindow* window);
    // One 640x480 NV21 camera frame; drawn upright, mirrored like a selfie view and cropped to
    // the middle square.
    void Render(const uint8_t* nv21);

  private:
    void Reset(ANativeWindow* window);

    std::mutex lock_;
    ANativeWindow* window_ = nullptr;
    bool sized_ = false;
};

}  // namespace aidl::android::hardware::biometrics::face
