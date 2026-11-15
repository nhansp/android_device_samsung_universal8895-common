/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include "FaceCapture.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <algorithm>
#include <vector>

#include <android-base/logging.h>
#include <cutils/sockets.h>

#include <biometrics_camera/CameraProtocol.h>

using namespace face_camera;

namespace aidl::android::hardware::biometrics::face {

namespace {

// Opening the front camera and getting its first frame takes about 0.5 s through the camera
// service (camera2, measured 2026-11-16); the HAL1 path should not be slower.
constexpr int kStartTimeoutSec = 5;

// Receives one message. Returns its type, or -1 on EOF or error.
int Receive(int fd, std::vector<uint8_t>* buf, size_t* len) {
    ssize_t n = TEMP_FAILURE_RETRY(recv(fd, buf->data(), buf->size(), 0));
    if (n < static_cast<ssize_t>(sizeof(CameraMsg))) return -1;
    CameraMsg hdr;
    memcpy(&hdr, buf->data(), sizeof(hdr));
    *len = std::min<size_t>(hdr.len, n - sizeof(hdr));
    return hdr.type;
}

}  // namespace

FaceCapture::~FaceCapture() {
    stop();
}

bool FaceCapture::start(Callbacks callbacks) {
    mSocket.reset(socket_local_client(kSocketName, ANDROID_SOCKET_NAMESPACE_RESERVED,
                                      SOCK_SEQPACKET));
    if (mSocket < 0) {
        PLOG(ERROR) << "Failed to connect to the face camera";
        return false;
    }

    timeval timeout = {kStartTimeoutSec, 0};
    setsockopt(mSocket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    std::vector<uint8_t> buf(kMaxMsgSize);
    size_t len;
    int type = Receive(mSocket, &buf, &len);
    if (type != MSG_STARTED) {
        LOG(ERROR) << "The face camera did not start (" << type << ")";
        mSocket.reset();
        return false;
    }
    timeout = {0, 0};
    setsockopt(mSocket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    mStopping = false;
    mThread = std::thread(&FaceCapture::loop, this, std::move(callbacks));
    return true;
}

void FaceCapture::loop(Callbacks callbacks) {
    std::vector<uint8_t> buf(kMaxMsgSize);
    std::vector<uint8_t> frame(kFrameBytes);
    size_t filled = 0;
    bool taking = true;
    int error = -EPIPE;
    for (;;) {
        size_t len;
        int type = Receive(mSocket, &buf, &len);
        if (type == MSG_CHUNK && len > sizeof(uint32_t)) {
            uint32_t offset;
            memcpy(&offset, buf.data() + sizeof(CameraMsg), sizeof(offset));
            size_t bytes = len - sizeof(offset);
            if (offset != filled || offset + bytes > kFrameBytes) {
                LOG(ERROR) << "Out of order frame chunk at " << offset;
                filled = 0;
                continue;
            }
            memcpy(frame.data() + offset, buf.data() + sizeof(CameraMsg) + sizeof(offset), bytes);
            filled += bytes;
            if (filled < kFrameBytes) continue;
            filled = 0;
            if (taking) taking = callbacks.onFrame(frame.data());
            // The next frame may come; after the last one this only drops frames in the daemon.
            CameraMsg release = {MSG_RELEASE, 0};
            TEMP_FAILURE_RETRY(send(mSocket, &release, sizeof(release), MSG_NOSIGNAL));
            continue;
        }
        if (type == MSG_ERROR && len >= sizeof(int32_t)) {
            memcpy(&error, buf.data() + sizeof(CameraMsg), sizeof(int32_t));
        }
        if (type == MSG_ERROR || type < 0) break;
    }
    if (!mStopping) {
        LOG(ERROR) << "The face camera stopped: " << error;
        callbacks.onEnd(error);
    }
}

void FaceCapture::stop() {
    if (mSocket < 0) return;
    mStopping = true;
    shutdown(mSocket, SHUT_RDWR);
    if (mThread.joinable()) mThread.join();
    mSocket.reset();
}

}  // namespace aidl::android::hardware::biometrics::face
