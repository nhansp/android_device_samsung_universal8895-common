/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <thread>

#include <android-base/unique_fd.h>

namespace aidl::android::hardware::biometrics::face {

// One capture from the iris camera daemon (camera/IrisCamera.cpp).
class IrisCapture {
  public:
    using FrameCallback = std::function<void(int fd)>;
    using EndCallback = std::function<void(int error)>;
    struct Callbacks {
        FrameCallback onIrFrame;
        FrameCallback onFrontFrame;  // the front camera's iris data
        EndCallback onEnd;
    };

    ~IrisCapture();

    // Opens the cameras and returns the IR camera's parameters in *params; frames then go to
    // the callbacks on the capture thread until stop(). onEnd runs on that thread if the IR
    // camera fails or goes away.
    bool start(std::map<std::string, std::string>* params, Callbacks callbacks);
    void setParameters(const std::string& flattened);
    // Closes the camera; returns once no callback runs any more. Not from a callback.
    void stop();

  private:
    void loop(Callbacks callbacks);

    ::android::base::unique_fd mSocket;
    std::atomic<bool> mStopping = false;
    std::thread mThread;
};

}  // namespace aidl::android::hardware::biometrics::face
