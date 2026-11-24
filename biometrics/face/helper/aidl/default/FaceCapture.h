/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include <android-base/unique_fd.h>

namespace aidl::android::hardware::biometrics::face {

// One capture of the front camera from the biometrics camera daemon (biometrics/camera/Face.cpp).
class FaceCapture {
  public:
    // One 640x480 NV21 frame, valid during the call.
    using PreviewCallback = std::function<void(const uint8_t* nv21)>;
    // The same for the engine; returns false to stop taking frames.
    using FrameCallback = std::function<bool(const uint8_t* nv21)>;
    using EndCallback = std::function<void(int error)>;
    struct Callbacks {
        PreviewCallback on_preview;
        FrameCallback on_frame;
        EndCallback on_end;
    };

    ~FaceCapture();

    // Opens the camera. Every frame goes to on_preview on the capture thread; the engine thread
    // takes the newest one to on_frame whenever the last call returned. on_end runs on the
    // capture thread if the camera fails or goes away.
    bool Start(Callbacks callbacks);
    // Closes the camera; returns once no callback runs any more. Not from a callback.
    void Stop();

  private:
    void Loop(Callbacks callbacks);
    void EngineLoop(FrameCallback on_frame);

    ::android::base::unique_fd socket_;
    std::atomic<bool> stopping_ = false;
    std::thread thread_;

    std::mutex frame_lock_;
    std::condition_variable frame_cv_;
    std::vector<uint8_t> latest_;
    bool have_frame_ = false;
    std::thread engine_thread_;
};

}  // namespace aidl::android::hardware::biometrics::face
