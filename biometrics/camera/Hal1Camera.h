/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>

#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>

#include <hardware/camera.h>

using CameraParams = std::map<std::string, std::string>;

// One opened camera. The camera HAL calls back on its own threads.
struct Hal1Camera {
    using DataCallback = std::function<void(int32_t msgType, const uint8_t* data, size_t size)>;
    using ErrorCallback = std::function<void(int32_t error)>;

    Hal1Camera(const char* id, DataCallback onData, ErrorCallback onError = {})
        : id(id), onData(std::move(onData)), onError(std::move(onError)) {}

    const char* id;
    DataCallback onData;
    ErrorCallback onError;
    camera_device_t* device = nullptr;

    void logFirst(int32_t msgType, const uint8_t* data, size_t size);

  private:
    std::mutex mSeenLock;
    std::set<int32_t> mSeen;
};

// Opens a camera with `overrides` merged into its default parameters; returns them in *params.
bool Hal1Open(Hal1Camera* camera, const CameraParams& overrides, CameraParams* params);
// Same with parameters chosen from the defaults (the camera's supported values).
bool Hal1Open(Hal1Camera* camera, const std::function<CameraParams(const CameraParams&)>& choose,
              CameraParams* params);
bool Hal1SetParameters(Hal1Camera* camera, const CameraParams& params);
void Hal1Close(Hal1Camera* camera, int32_t msgTypes);

// CameraParameters is not available to vendor code; these keep its flattened format.
CameraParams Unflatten(const std::string& flat);
std::string Flatten(const CameraParams& params);

// Sends one CameraMsg (and an fd with SCM_RIGHTS). `block` waits for socket space.
bool Send(int fd, uint32_t type, const void* data, uint32_t len, int passFd = -1,
          bool block = false);
// Same with a 32-bit prefix before `data` (one message, no copy of `data`).
bool SendPrefixed(int fd, uint32_t type, uint32_t prefix, const void* data, uint32_t len);
