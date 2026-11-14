/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include "IrisCapture.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <vector>

#include <android-base/logging.h>
#include <android-base/strings.h>
#include <cutils/sockets.h>

#include "CameraProtocol.h"

using namespace iris_camera;

namespace aidl::android::hardware::biometrics::face {

namespace {

// Opening the camera and starting the secure stream takes about 100 ms (camera daemon log).
constexpr int kStartTimeoutSec = 5;

// Receives one message. Returns its type, or -1 on EOF or error.
int Receive(int fd, std::vector<char>* buf, std::string* payload, int* passedFd) {
    IrisCameraMsg hdr;
    iovec iov[2] = {{&hdr, sizeof(hdr)}, {buf->data(), buf->size()}};
    char control[CMSG_SPACE(sizeof(int))];
    msghdr msg = {};
    msg.msg_iov = iov;
    msg.msg_iovlen = 2;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);
    *passedFd = -1;
    ssize_t n = TEMP_FAILURE_RETRY(recvmsg(fd, &msg, MSG_CMSG_CLOEXEC));
    if (n < static_cast<ssize_t>(sizeof(hdr))) return -1;

    for (cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
            memcpy(passedFd, CMSG_DATA(cmsg), sizeof(int));
        }
    }
    size_t len = std::min<size_t>(hdr.len, n - sizeof(hdr));
    payload->assign(buf->data(), len);
    return hdr.type;
}

}  // namespace

IrisCapture::~IrisCapture() {
    stop();
}

bool IrisCapture::start(std::map<std::string, std::string>* params, Callbacks callbacks) {
    mSocket.reset(socket_local_client(kSocketName, ANDROID_SOCKET_NAMESPACE_RESERVED,
                                      SOCK_SEQPACKET));
    if (mSocket < 0) {
        PLOG(ERROR) << "Failed to connect to the iris camera";
        return false;
    }

    timeval timeout = {kStartTimeoutSec, 0};
    setsockopt(mSocket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    std::vector<char> buf(kMaxMsgSize);
    std::string payload;
    int fd;
    int type = Receive(mSocket, &buf, &payload, &fd);
    if (fd >= 0) close(fd);
    if (type != MSG_PARAMS) {
        LOG(ERROR) << "The iris camera did not start (" << type << ")";
        mSocket.reset();
        return false;
    }
    timeout = {0, 0};
    setsockopt(mSocket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    params->clear();
    for (const auto& item : ::android::base::Split(payload, ";")) {
        size_t eq = item.find('=');
        if (eq != std::string::npos) (*params)[item.substr(0, eq)] = item.substr(eq + 1);
    }

    mStopping = false;
    mThread = std::thread(&IrisCapture::loop, this, std::move(callbacks));
    return true;
}

void IrisCapture::loop(Callbacks callbacks) {
    std::vector<char> buf(kMaxMsgSize);
    std::string payload;
    int fd;
    int error = -EPIPE;
    for (;;) {
        int type = Receive(mSocket, &buf, &payload, &fd);
        if ((type == MSG_FRAME || type == MSG_FRONT_FRAME) && fd >= 0) {
            (type == MSG_FRAME ? callbacks.onIrFrame : callbacks.onFrontFrame)(fd);
            close(fd);
            continue;
        }
        if (fd >= 0) close(fd);
        if (type == MSG_ERROR && payload.size() >= sizeof(int32_t)) {
            memcpy(&error, payload.data(), sizeof(int32_t));
        }
        if (type == MSG_ERROR || type < 0) break;
    }
    if (!mStopping) {
        LOG(ERROR) << "The iris camera stopped: " << error;
        callbacks.onEnd(error);
    }
}

void IrisCapture::setParameters(const std::string& flattened) {
    if (mSocket < 0) return;
    IrisCameraMsg hdr = {MSG_SET_PARAMS, static_cast<uint32_t>(flattened.size())};
    iovec iov[2] = {{&hdr, sizeof(hdr)}, {const_cast<char*>(flattened.data()), flattened.size()}};
    msghdr msg = {};
    msg.msg_iov = iov;
    msg.msg_iovlen = 2;
    if (TEMP_FAILURE_RETRY(sendmsg(mSocket, &msg, MSG_NOSIGNAL)) < 0) {
        PLOG(ERROR) << "Failed to send camera parameters";
    }
}

void IrisCapture::stop() {
    if (mSocket < 0) return;
    mStopping = true;
    shutdown(mSocket, SHUT_RDWR);
    if (mThread.joinable()) mThread.join();
    mSocket.reset();
}

}  // namespace aidl::android::hardware::biometrics::face
