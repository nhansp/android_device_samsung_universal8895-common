/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>

#include <atomic>
#include <functional>
#include <thread>

#include <android-base/unique_fd.h>

namespace aidl::android::hardware::biometrics::face {

// One capture of the front camera from the biometrics camera daemon (biometrics/camera/Face.cpp).
class FaceCapture {
  public:
    // One 640x480 NV21 frame, valid during the call. Returns false to stop taking frames.
    using FrameCallback = std::function<bool(const uint8_t* nv21)>;
    using EndCallback = std::function<void(int error)>;
    struct Callbacks {
        FrameCallback onFrame;
        EndCallback onEnd;
    };

    ~FaceCapture();

    // Opens the camera; frames then go to onFrame on the capture thread until stop(). onEnd
    // runs on that thread if the camera fails or goes away.
    bool start(Callbacks callbacks);
    // Closes the camera; returns once no callback runs any more. Not from a callback.
    void stop();

  private:
    void loop(Callbacks callbacks);

    ::android::base::unique_fd mSocket;
    std::atomic<bool> mStopping = false;
    std::thread mThread;
};

}  // namespace aidl::android::hardware::biometrics::face
