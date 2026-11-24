/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "BiometricsCamera"

#include <sys/socket.h>

#include <atomic>
#include <string>
#include <vector>

#include <android-base/logging.h>
#include <android-base/properties.h>

#include <biometrics_camera/CameraProtocol.h>

#include "Hal1Camera.h"

using namespace iris_camera;

namespace {

// Id 90 is the secure iris sensor, opened through the module's HAL1 path
constexpr char kIrisCameraId[] = "90";
constexpr char kFrontCameraId[] = "1";
// Each IR frame arrives as an 8-byte buffer holding kFrameMagic and the frame's dma-buf fd
constexpr int32_t kMsgIrisFrame = 0x10000;
constexpr uint32_t kFrameMagic = 0x00fd42fd;
constexpr int32_t kMsgIrisPreviewData = 0x8000;
// Vendor send_command code that enables the iris data callback
constexpr int32_t kCmdIrisPreviewData = 1832;
// Makes the HAL open the 3H1 directly: the C3 companion has no 752x1328 mode. The blob reads
// this name after extract-files.py renames it.
constexpr char kCompanionDisableProp[] = "vendor.camera.CC.disable";

std::atomic<int> gClient{-1};

// Returns the fd of an iris data message ({kFrameMagic, fd}), or -1.
int IrisDataFd(const char* id, const uint8_t* data, size_t size) {
    if (!data || size < 2 * sizeof(uint32_t)) return -1;
    const auto* payload = reinterpret_cast<const uint32_t*>(data);
    if (payload[0] != kFrameMagic) {
        LOG(WARNING) << "Camera " << id << ": unexpected iris data " << payload[0];
        return -1;
    }
    return static_cast<int>(payload[1]);
}

void StartFront(Hal1Camera* front) {
    CameraParams params;
    android::base::SetProperty(kCompanionDisableProp, "1");
    bool opened = Hal1Open(front,
                           {{"focus-mode", "fixed-face"},
                            {"no-display-mode", "1"},
                            {"preview-size", std::to_string(kFrontWidth) + "x" +
                                                     std::to_string(kFrontHeight)},
                            {"preview-fps-range", "15000,15000"},
                            {"shot-mode", "39"}},
                           &params);
    if (!opened) return;
    front->device->ops->enable_msg_type(front->device, kMsgIrisPreviewData);
    int ret = front->device->ops->start_preview(front->device);
    if (ret) {
        LOG(ERROR) << "Front camera start_preview: " << ret;
        Hal1Close(front, kMsgIrisPreviewData);
        return;
    }
    // The iris data callback is enabled only after start_preview
    ret = front->device->ops->send_command(front->device, kCmdIrisPreviewData, 1, 0);
    if (ret) LOG(WARNING) << "Front camera command " << kCmdIrisPreviewData << ": " << ret;
}

}  // namespace

void ServeIris(int client) {
    // The iris library turns the front camera's iris data into the preview (IMAGE_PROCESSED
    // id 1); the display frames of the front camera are not used. A frame the HAL is too slow
    // for is dropped: the buffer goes back to the camera.
    Hal1Camera front(kFrontCameraId, [](int32_t msgType, const uint8_t* data, size_t size) {
        int c = gClient.load();
        if (c < 0 || msgType != kMsgIrisPreviewData) return;
        int fd = IrisDataFd(kFrontCameraId, data, size);
        if (fd >= 0) Send(c, MSG_FRONT_FRAME, nullptr, 0, fd);
    });
    // Without the IR camera there is no capture; the front camera only adds the preview.
    Hal1Camera iris(
            kIrisCameraId,
            [](int32_t msgType, const uint8_t* data, size_t size) {
                int c = gClient.load();
                if (c < 0 || msgType != kMsgIrisFrame) return;
                int fd = IrisDataFd(kIrisCameraId, data, size);
                if (fd >= 0) Send(c, MSG_FRAME, nullptr, 0, fd);
            },
            [](int32_t error) {
                int c = gClient.load();
                if (c >= 0) Send(c, MSG_ERROR, &error, sizeof(error));
            });
    gClient.store(client);
    StartFront(&front);

    CameraParams params;
    if (!Hal1Open(&iris,
                  {{"preview-size", std::to_string(kWidth) + "x" + std::to_string(kHeight)},
                   {"no-display-mode", "1"},
                   {"shot-mode", "39"}},
                  &params)) {
        int32_t err = -ENODEV;
        Send(client, MSG_ERROR, &err, sizeof(err));
    } else {
        std::string flattened = Flatten(params);
        Send(client, MSG_PARAMS, flattened.data(), flattened.size());
        iris.device->ops->enable_msg_type(iris.device, kMsgIrisFrame);
        int ret = iris.device->ops->start_preview(iris.device);
        if (ret) {
            LOG(ERROR) << "start_preview: " << ret;
            int32_t err = ret;
            Send(client, MSG_ERROR, &err, sizeof(err));
        } else {
            LOG(INFO) << "Iris camera streaming";
            std::vector<char> buf(kMaxMsgSize);
            ssize_t n;
            while ((n = TEMP_FAILURE_RETRY(recv(client, buf.data(), buf.size(), 0))) > 0) {
                auto* hdr = reinterpret_cast<CameraMsg*>(buf.data());
                if (n < static_cast<ssize_t>(sizeof(*hdr)) || hdr->type != MSG_SET_PARAMS ||
                    hdr->len > n - sizeof(*hdr)) {
                    continue;
                }
                for (const auto& [key, value] :
                     Unflatten(std::string(buf.data() + sizeof(*hdr), hdr->len))) {
                    params[key] = value;
                }
                Hal1SetParameters(&iris, params);
            }
        }
    }
    gClient.store(-1);
    Hal1Close(&iris, kMsgIrisFrame);
    Hal1Close(&front, kMsgIrisPreviewData);
    // Other users of the front camera get the companion again.
    android::base::SetProperty(kCompanionDisableProp, "0");
    LOG(INFO) << "Iris camera closed";
}
