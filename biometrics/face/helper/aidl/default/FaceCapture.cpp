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

// Opening the front camera and getting its first frame takes about 0.5 s
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
    Stop();
}

bool FaceCapture::Start(Callbacks callbacks) {
    socket_.reset(
            socket_local_client(kSocketName, ANDROID_SOCKET_NAMESPACE_RESERVED, SOCK_SEQPACKET));
    if (socket_ < 0) {
        PLOG(ERROR) << "Failed to connect to the face camera";
        return false;
    }

    timeval timeout = {kStartTimeoutSec, 0};
    setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    std::vector<uint8_t> buf(kMaxMsgSize);
    size_t len;
    int type = Receive(socket_, &buf, &len);
    if (type != MSG_STARTED) {
        LOG(ERROR) << "The face camera did not start (" << type << ")";
        socket_.reset();
        return false;
    }
    timeout = {0, 0};
    setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    stopping_ = false;
    have_frame_ = false;
    latest_.resize(kFrameBytes);
    engine_thread_ = std::thread(&FaceCapture::EngineLoop, this, callbacks.on_frame);
    thread_ = std::thread(&FaceCapture::Loop, this, std::move(callbacks));
    return true;
}

void FaceCapture::Loop(Callbacks callbacks) {
    std::vector<uint8_t> buf(kMaxMsgSize);
    std::vector<uint8_t> frame(kFrameBytes);
    size_t filled = 0;
    int error = -EPIPE;
    for (;;) {
        size_t len;
        int type = Receive(socket_, &buf, &len);
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
            // The frame is copied out; the daemon may send the next one.
            CameraMsg release = {MSG_RELEASE, 0};
            TEMP_FAILURE_RETRY(send(socket_, &release, sizeof(release), MSG_NOSIGNAL));
            callbacks.on_preview(frame.data());
            {
                std::lock_guard<std::mutex> lock(frame_lock_);
                latest_.swap(frame);
                have_frame_ = true;
            }
            frame_cv_.notify_one();
            continue;
        }
        if (type == MSG_ERROR && len >= sizeof(int32_t)) {
            memcpy(&error, buf.data() + sizeof(CameraMsg), sizeof(int32_t));
        }
        if (type == MSG_ERROR || type < 0) break;
    }
    if (!stopping_) {
        LOG(ERROR) << "The face camera stopped: " << error;
        callbacks.on_end(error);
    }
}

void FaceCapture::EngineLoop(FrameCallback on_frame) {
    std::vector<uint8_t> frame(kFrameBytes);
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(frame_lock_);
            frame_cv_.wait(lock, [this] { return have_frame_ || stopping_; });
            if (stopping_) return;
            frame.swap(latest_);
            have_frame_ = false;
        }
        if (!on_frame(frame.data())) return;
    }
}

void FaceCapture::Stop() {
    if (socket_ < 0) return;
    {
        std::lock_guard<std::mutex> lock(frame_lock_);
        stopping_ = true;
    }
    frame_cv_.notify_all();
    shutdown(socket_, SHUT_RDWR);
    if (thread_.joinable()) thread_.join();
    if (engine_thread_.joinable()) engine_thread_.join();
    socket_.reset();
}

}  // namespace aidl::android::hardware::biometrics::face
