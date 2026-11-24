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

// One capture from the biometrics camera daemon (biometrics/camera/Iris.cpp).
class IrisCapture {
  public:
    using FrameCallback = std::function<void(int fd)>;
    using EndCallback = std::function<void(int error)>;
    struct Callbacks {
        FrameCallback on_ir_frame;
        FrameCallback on_front_frame;  // the front camera's iris data
        EndCallback on_end;
    };

    ~IrisCapture();

    // Opens the cameras and returns the IR camera's parameters in *params; frames then go to
    // the callbacks on the capture thread until Stop(). on_end runs on that thread if the IR
    // camera fails or goes away.
    bool Start(std::map<std::string, std::string>* params, Callbacks callbacks);
    void SetParameters(const std::string& flattened);
    // Closes the camera; returns once no callback runs any more. Not from a callback.
    void Stop();

  private:
    void Loop(Callbacks callbacks);

    ::android::base::unique_fd socket_;
    std::atomic<bool> stopping_ = false;
    std::thread thread_;
};

}  // namespace aidl::android::hardware::biometrics::face
