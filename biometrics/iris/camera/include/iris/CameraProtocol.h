/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

// The face HAL (64-bit, it hosts the 64-bit-only iris libraries) and the iris camera daemon
// (32-bit, it hosts the 32-bit camera HAL) talk over the init socket below. One connection is one
// capture: the daemon opens the iris camera when a client connects and closes it on disconnect.
// Each SOCK_SEQPACKET message is an IrisCameraMsg header followed by `len` payload bytes.
namespace iris_camera {

constexpr char kSocketName[] = "iris_camera";

// Stock SecIrisService IRV1Controller: camera 90, 1920x1920.
constexpr int kWidth = 1920;
constexpr int kHeight = 1920;
// Stock IRV1Controller.startFrontCamera(): camera 1 at 752x1328 (CAMERA_CONFIG_IRIS_OPERATION=RGB).
constexpr int kFrontWidth = 752;
constexpr int kFrontHeight = 1328;

enum MsgType : uint32_t {
    MSG_PARAMS = 1,     // daemon -> HAL: the flattened camera parameters, once per connection
    MSG_FRAME = 2,      // daemon -> HAL: one IR frame, its dma-buf passed with SCM_RIGHTS
    MSG_ERROR = 3,      // daemon -> HAL: int32_t error, the capture is over
    MSG_SET_PARAMS = 4, // HAL -> daemon: "key=value;..." to merge into the camera parameters
    MSG_FRONT_FRAME = 5, // daemon -> HAL: the front camera's iris data, its dma-buf with SCM_RIGHTS
};

struct IrisCameraMsg {
    uint32_t type;
    uint32_t len;
};

constexpr size_t kMaxMsgSize = 64 * 1024;

}  // namespace iris_camera
