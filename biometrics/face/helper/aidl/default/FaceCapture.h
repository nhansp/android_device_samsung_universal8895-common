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
        PreviewCallback onPreview;
        FrameCallback onFrame;
        EndCallback onEnd;
    };

    ~FaceCapture();

    // Opens the camera. Every frame goes to onPreview on the capture thread; the engine thread
    // takes the newest one to onFrame whenever the last call returned, as stock FrCamCtrl draws
    // the camera's preview at its own rate and feeds the engine one callback buffer. onEnd runs
    // on the capture thread if the camera fails or goes away.
    bool start(Callbacks callbacks);
    // Closes the camera; returns once no callback runs any more. Not from a callback.
    void stop();

  private:
    void loop(Callbacks callbacks);
    void engineLoop(FrameCallback onFrame);

    ::android::base::unique_fd mSocket;
    std::atomic<bool> mStopping = false;
    std::thread mThread;

    std::mutex mFrameLock;
    std::condition_variable mFrameCv;
    std::vector<uint8_t> mLatest;
    bool mHaveFrame = false;
    std::thread mEngineThread;
};

}  // namespace aidl::android::hardware::biometrics::face
