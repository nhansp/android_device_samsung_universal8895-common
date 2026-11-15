/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "BiometricsCamera"

#include <sys/socket.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <string>
#include <vector>

#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/strings.h>

#include <biometrics_camera/CameraProtocol.h>

#include "Hal1Camera.h"

using namespace face_camera;

namespace {

constexpr char kFrontCameraId[] = "1";

std::atomic<int> gClient{-1};
// A frame is out until the HAL sends MSG_RELEASE; later preview frames are dropped meanwhile.
std::atomic<bool> gInFlight{false};

// Stock FrCamCtrl.h1OpenCamera(): the lowest minimum among the fps ranges that reach 30 fps.
std::string FpsRange(const CameraParams& defaults) {
    auto it = defaults.find("preview-fps-range-values");
    if (it == defaults.end()) return {};
    int bestMin = INT_MAX;
    std::string best;
    // "(min,max),(min,max)"
    for (std::string range : android::base::Split(it->second, "),")) {
        range.erase(std::remove(range.begin(), range.end(), '('), range.end());
        range.erase(std::remove(range.begin(), range.end(), ')'), range.end());
        auto values = android::base::Split(range, ",");
        int min, max;
        if (values.size() != 2 || !android::base::ParseInt(values[0], &min) ||
            !android::base::ParseInt(values[1], &max)) {
            continue;
        }
        if (max == 30000 && min < bestMin) {
            bestMin = min;
            best = values[0] + "," + values[1];
        }
    }
    return best;
}

// Stock FrCamCtrl.h1OpenCamera() for the front camera of the face service.
CameraParams FaceParams(const CameraParams& defaults) {
    CameraParams params = {
            {"preview-size", std::to_string(kWidth) + "x" + std::to_string(kHeight)},
            // setPreviewFormat(ImageFormat.NV21)
            {"preview-format", "yuv420sp"},
            // No SurfaceTexture: the preview the user sees is drawn by the face HAL
            {"no-display-mode", "1"},
            // "set night shot mode" for great* and dream*: libexynoscamera m_setShotMode case
            // 10 -> setMetaCtlSceneMode(AA_CONTROL_USE_SCENE_MODE, AA_SCENE_MODE_LLS), the
            // ISP's low light mode (other products use 40, AA_SCENE_MODE_FACE_LOCK)
            {"shot-mode", "10"},
    };
    // mIsAvailableFocusMode: only when the camera has autofocus
    auto focus = defaults.find("focus-mode-values");
    if (focus != defaults.end()) {
        auto modes = android::base::Split(focus->second, ",");
        if (std::find(modes.begin(), modes.end(), "auto") != modes.end()) {
            params["focus-mode"] = "fixed-face";
        }
    }
    std::string fps = FpsRange(defaults);
    if (!fps.empty()) params["preview-fps-range"] = fps;
    return params;
}

void OnPreviewFrame(int32_t msgType, const uint8_t* data, size_t size) {
    int client = gClient.load();
    if (client < 0 || msgType != CAMERA_MSG_PREVIEW_FRAME || !data) return;
    if (size < kFrameBytes) {
        LOG(ERROR) << "Short preview frame: " << size;
        return;
    }
    bool idle = false;
    if (!gInFlight.compare_exchange_strong(idle, true)) return;
    // The frame is only valid in this callback; it goes out before the buffer returns.
    for (size_t offset = 0; offset < kFrameBytes; offset += kChunkBytes) {
        uint32_t len = static_cast<uint32_t>(std::min(kChunkBytes, kFrameBytes - offset));
        if (!SendPrefixed(client, MSG_CHUNK, static_cast<uint32_t>(offset), data + offset, len)) {
            PLOG(ERROR) << "Failed to send a face frame";
            return;
        }
    }
}

}  // namespace

void ServeFace(int client) {
    Hal1Camera front(kFrontCameraId, OnPreviewFrame, [](int32_t error) {
        int c = gClient.load();
        if (c >= 0) Send(c, MSG_ERROR, &error, sizeof(error));
    });
    gInFlight.store(false);
    gClient.store(client);

    CameraParams params;
    if (!Hal1Open(&front, FaceParams, &params)) {
        int32_t err = -ENODEV;
        Send(client, MSG_ERROR, &err, sizeof(err));
    } else {
        LOG(INFO) << "Face camera: preview-fps-range " << params["preview-fps-range"]
                  << " focus-mode " << params["focus-mode"];
        front.device->ops->enable_msg_type(front.device, CAMERA_MSG_PREVIEW_FRAME);
        int ret = front.device->ops->start_preview(front.device);
        if (ret) {
            LOG(ERROR) << "Face camera start_preview: " << ret;
            int32_t err = ret;
            Send(client, MSG_ERROR, &err, sizeof(err));
        } else {
            Send(client, MSG_STARTED, nullptr, 0);
            LOG(INFO) << "Face camera streaming";
            std::vector<char> buf(kMaxMsgSize);
            ssize_t n;
            while ((n = TEMP_FAILURE_RETRY(recv(client, buf.data(), buf.size(), 0))) > 0) {
                auto* hdr = reinterpret_cast<CameraMsg*>(buf.data());
                if (n >= static_cast<ssize_t>(sizeof(*hdr)) && hdr->type == MSG_RELEASE) {
                    gInFlight.store(false);
                }
            }
        }
    }
    gClient.store(-1);
    Hal1Close(&front, CAMERA_MSG_PREVIEW_FRAME);
    LOG(INFO) << "Face camera closed";
}
