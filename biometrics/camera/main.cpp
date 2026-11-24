/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "BiometricsCamera"

#include <poll.h>
#include <sys/socket.h>

#include <android-base/logging.h>
#include <android-base/unique_fd.h>
#include <cutils/sockets.h>

#include <biometrics_camera/CameraProtocol.h>

using android::base::unique_fd;

void ServeIris(int client);
void ServeFace(int client);

namespace {

// Returns the init socket listening, or -1 (poll() skips it).
int Listen(const char* name) {
    int server = android_get_control_socket(name);
    if (server < 0) return -1;
    if (listen(server, 1)) {
        PLOG(ERROR) << "listen " << name;
        return -1;
    }
    return server;
}

}  // namespace

// One capture at a time: the iris and the face capture both use the front camera
int main() {
    pollfd fds[] = {
            {Listen(iris_camera::kSocketName), POLLIN, 0},
            {Listen(face_camera::kSocketName), POLLIN, 0},
    };
    void (*const serve[])(int) = {ServeIris, ServeFace};
    if (fds[0].fd < 0 && fds[1].fd < 0) {
        LOG(FATAL) << "No init socket";
    }
    for (;;) {
        if (TEMP_FAILURE_RETRY(poll(fds, 2, -1)) < 0) {
            PLOG(FATAL) << "poll";
        }
        for (int i = 0; i < 2; i++) {
            if (fds[i].fd < 0 || !(fds[i].revents & POLLIN)) continue;
            unique_fd client(TEMP_FAILURE_RETRY(accept4(fds[i].fd, nullptr, nullptr,
                                                        SOCK_CLOEXEC)));
            if (client < 0) {
                PLOG(ERROR) << "accept";
                continue;
            }
            serve[i](client.get());
        }
    }
}
