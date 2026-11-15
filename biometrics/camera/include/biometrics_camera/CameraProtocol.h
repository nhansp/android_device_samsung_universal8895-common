/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

// The biometrics HALs are 64-bit (they host the 64-bit-only iris and face libraries); the camera
// HAL is 32-bit only. The biometrics camera daemon hosts it and talks to the HALs over one init
// socket per HAL. One connection is one capture: the daemon opens the cameras when a client
// connects and closes them on disconnect. Each SOCK_SEQPACKET message is a CameraMsg header
// followed by `len` payload bytes.

struct CameraMsg {
    uint32_t type;
    uint32_t len;
};

constexpr size_t kMaxMsgSize = 64 * 1024;

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

}  // namespace iris_camera

namespace face_camera {

constexpr char kSocketName[] = "face_camera";

// Stock BioFaceService FrCamCtrl: camera 1 at 640x480 NV21; the face library takes exactly this
// size (libFaceService processFrontImage 0x55a8: mov w1,#0x280; orr w2,wzr,#0x1e0).
constexpr int kWidth = 640;
constexpr int kHeight = 480;
constexpr size_t kFrameBytes = kWidth * kHeight * 3 / 2;

enum MsgType : uint32_t {
    MSG_STARTED = 1,   // daemon -> HAL: the camera streams, once per connection
    MSG_CHUNK = 2,     // daemon -> HAL: uint32_t offset, then bytes of the current frame
    MSG_ERROR = 3,     // daemon -> HAL: int32_t error, the capture is over
    MSG_RELEASE = 4,   // HAL -> daemon: the frame is consumed, the next one may come
};

// A frame goes out in chunks that fit kMaxMsgSize; the chunk that reaches kFrameBytes completes
// it. Only one frame is in flight: the daemon drops preview frames until MSG_RELEASE, like stock
// FrCamCtrl, which hands the camera only buffers its consumer has returned.
constexpr size_t kChunkBytes = 60 * 1024;

}  // namespace face_camera
