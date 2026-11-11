/*
 * SPDX-FileCopyrightText: The Android Open Source Project
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 *
 * hardware/interfaces/graphics/composer/2.1/default/service.cpp, loading the HWC through a
 * HwcHal that keeps solid-color layers away from the stock HWC1, reports HWC2 present fences and
 * enters DOZE_SUSPEND through DOZE (see SolidColorHwcHal).
 */

#define LOG_TAG "android.hardware.graphics.composer@2.1-service"

#include <sched.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <android-base/unique_fd.h>
#include <android/hardware/graphics/composer/2.1/IComposer.h>

#include <binder/ProcessState.h>
#include <composer-passthrough/2.1/HwcLoader.h>
#include <hidl/LegacySupport.h>

using android::hardware::graphics::composer::V2_1::Display;
using android::hardware::graphics::composer::V2_1::Error;
using android::hardware::graphics::composer::V2_1::IComposer;
using android::hardware::graphics::composer::V2_1::IComposerClient;
using android::hardware::graphics::composer::V2_1::Layer;
using android::hardware::graphics::composer::V2_1::passthrough::HwcHal;
using android::hardware::graphics::composer::V2_1::passthrough::HwcLoader;

namespace {

// The stock libexynosdisplay create_layers() passes hwc_layer_1.handle of every layer to
// ExynosLayer::setLayerBuffer(), which dereferences it. For a SOLID_COLOR layer the HWC1 adapter
// keeps the color in the union that also holds the handle, so the HWC read through the color
// (fault address 0xff5b5288 = color 0xff5b5250 + 0x38). The adapter composes solid colors as
// HWC_FRAMEBUFFER | HWC_SKIP_LAYER anyway, so ask it for CLIENT composition instead, which leaves
// the handle slot to the (null) buffer, and report the layer as changed to CLIENT so that
// SurfaceFlinger draws it.
//
// HWC1 has no present fence. For frame N the adapter returns the HWC1 retire fence, which signals
// when N is replaced on the panel: the DECON timeline of a DSI display starts one ahead
// (kernel exynos8895_s3cfb.c, stock dpu/helper.c:326-339), so it signals a refresh after N is
// shown. SurfaceFlinger takes it as the HWC2 present fence and counts each frame as missed
// (FrameTargeter::beginFrame: the fence signalled more than half a refresh after its expected
// present time), then composes every other vsync. The retire fence of frame N-1 signals when N
// replaces it on the panel, which is what the HWC2 present fence of N means; return that one.
// Release fences are not touched.
//
// The stock HWC blanks the display (FBIOBLANK POWERDOWN) when it leaves ON for DOZE or
// DOZE_SUSPEND, then sets the doze mode with S3CFB_POWER_MODE; from a doze mode it sets the new
// mode only (libexynosdisplay ExynosPrimaryDisplay::setPowerMode, 0x26bfc-0x26c84). DOZE then
// brings the panel back up in ALPM with the next frame, but DOZE_SUSPEND from a blanked display
// leaves the panel off (kernel decon_set_doze_suspend() with the DECON off). The framework asks
// for DOZE before DOZE_SUSPEND (DisplayPowerController.animateScreenStateChange), but the two
// requests reach the HAL as one, ON to DOZE_SUSPEND. So enter DOZE_SUSPEND from ON as DOZE, ask
// for a frame, and suspend after it is presented: the HWC presents it in DOZE and the panel shows
// it in ALPM.
class SolidColorHwcHal : public HwcHal {
  public:
    Error setPowerMode(Display display, IComposerClient::PowerMode mode) override {
        std::lock_guard<std::mutex> lock(mPowerMutex);
        PowerState& state = mPower[display];
        state.suspendPending = false;
        if (mode == IComposerClient::PowerMode::DOZE_SUSPEND &&
            state.mode == IComposerClient::PowerMode::ON) {
            Error err = HwcHal::setPowerMode(display, IComposerClient::PowerMode::DOZE);
            if (err == Error::NONE) {
                state.mode = IComposerClient::PowerMode::DOZE;
                state.suspendPending = true;
                // the doze frame: SurfaceFlinger has nothing new to draw after the screen-off
                // animation (IComposerCallback.onRefresh is oneway)
                if (mEventCallback != nullptr) {
                    mEventCallback->onRefresh(display);
                }
            }
            return err;
        }

        Error err = HwcHal::setPowerMode(display, mode);
        if (err == Error::NONE) {
            state.mode = mode;
        }
        return err;
    }

    Error presentDisplay(Display display, int32_t* outPresentFence, std::vector<Layer>* outLayers,
                         std::vector<int32_t>* outReleaseFences) override {
        Error err = HwcHal::presentDisplay(display, outPresentFence, outLayers, outReleaseFences);
        if (err != Error::NONE) {
            return err;
        }

        IComposerClient::DisplayType type;
        if (getDisplayType(display, &type) != Error::NONE ||
            type != IComposerClient::DisplayType::PHYSICAL) {
            return err;
        }

        std::lock_guard<std::mutex> lock(mMutex);
        android::base::unique_fd& previousRetire = mPreviousRetire[display];
        int32_t retire = *outPresentFence;
        // -1 for the first frame: no fence means presented
        *outPresentFence = previousRetire.release();
        previousRetire.reset(retire);
        enterPendingDozeSuspend(display);
        return err;
    }

    Error setLayerCompositionType(Display display, Layer layer, int32_t type) override {
        {
            std::lock_guard<std::mutex> lock(mMutex);
            if (type == HWC2_COMPOSITION_SOLID_COLOR) {
                mSolidColorLayers[display].insert(layer);
                type = HWC2_COMPOSITION_CLIENT;
            } else {
                mSolidColorLayers[display].erase(layer);
            }
        }
        return HwcHal::setLayerCompositionType(display, layer, type);
    }

  protected:
    int32_t getChangedCompositionTypes(Display display, uint32_t* outTypesCount,
                                       Layer* outChangedLayers,
                                       IComposerClient::Composition* outCompositionTypes) override {
        uint32_t count = 0;
        int32_t err = getChangedCompositionTypesInternal(display, &count, nullptr, nullptr);
        if (err != HWC2_ERROR_NONE) {
            return err;
        }

        std::vector<Layer> layers(count);
        std::vector<IComposerClient::Composition> types(count);
        if (count > 0) {
            err = getChangedCompositionTypesInternal(display, &count, layers.data(), types.data());
            if (err != HWC2_ERROR_NONE) {
                return err;
            }
            layers.resize(count);
            types.resize(count);
        }

        {
            std::lock_guard<std::mutex> lock(mMutex);
            auto it = mSolidColorLayers.find(display);
            if (it != mSolidColorLayers.end()) {
                for (Layer layer : it->second) {
                    if (std::find(layers.begin(), layers.end(), layer) == layers.end()) {
                        layers.push_back(layer);
                        types.push_back(IComposerClient::Composition::CLIENT);
                    }
                }
            }
        }

        if (outChangedLayers == nullptr || outCompositionTypes == nullptr) {
            *outTypesCount = layers.size();
            return HWC2_ERROR_NONE;
        }

        uint32_t n = std::min<uint32_t>(*outTypesCount, layers.size());
        std::copy_n(layers.begin(), n, outChangedLayers);
        std::copy_n(types.begin(), n, outCompositionTypes);
        *outTypesCount = n;
        return HWC2_ERROR_NONE;
    }

    void onLayerDestroyed(Display display, Layer layer) override {
        std::lock_guard<std::mutex> lock(mMutex);
        auto it = mSolidColorLayers.find(display);
        if (it != mSolidColorLayers.end()) {
            it->second.erase(layer);
        }
    }

  private:
    void enterPendingDozeSuspend(Display display) {
        std::lock_guard<std::mutex> lock(mPowerMutex);
        auto it = mPower.find(display);
        if (it == mPower.end() || !it->second.suspendPending) {
            return;
        }
        it->second.suspendPending = false;
        if (HwcHal::setPowerMode(display, IComposerClient::PowerMode::DOZE_SUSPEND) == Error::NONE) {
            it->second.mode = IComposerClient::PowerMode::DOZE_SUSPEND;
        }
    }

    struct PowerState {
        IComposerClient::PowerMode mode = IComposerClient::PowerMode::OFF;
        bool suspendPending = false;
    };

    std::mutex mPowerMutex;
    std::unordered_map<Display, PowerState> mPower;
    std::mutex mMutex;
    std::unordered_map<Display, std::unordered_set<Layer>> mSolidColorLayers;
    std::unordered_map<Display, android::base::unique_fd> mPreviousRetire;
};

// HwcLoader::load() with SolidColorHwcHal in place of HwcHal.
class SolidColorHwcLoader : public HwcLoader {
  public:
    static IComposer* load() {
        const hw_module_t* module = loadModule();
        if (!module) {
            return nullptr;
        }

        bool adapted = false;
        hwc2_device_t* device = openDeviceWithAdapter(module, &adapted);
        if (!device) {
            return nullptr;
        }

        auto hal = std::make_unique<SolidColorHwcHal>();
        if (!hal->initWithDevice(device, !adapted)) {
            return nullptr;
        }

        return createComposer(std::move(hal));
    }
};

}  // namespace

int main() {
    // the conventional HAL might start binder services
    android::ProcessState::initWithDriver("/dev/vndbinder");
    android::ProcessState::self()->setThreadPoolMaxThreadCount(4);
    android::ProcessState::self()->startThreadPool();

    // same as SF main thread
    struct sched_param param = {0};
    param.sched_priority = 2;
    if (sched_setscheduler(0, SCHED_FIFO | SCHED_RESET_ON_FORK, &param) != 0) {
        ALOGE("Couldn't set SCHED_FIFO: %d", errno);
    }

    android::hardware::configureRpcThreadpool(4, true /* will join */);

    android::sp<IComposer> composer = SolidColorHwcLoader::load();
    if (composer == nullptr) {
        return 1;
    }
    if (composer->registerAsService() != android::NO_ERROR) {
        ALOGE("failed to register service");
        return 1;
    }

    android::hardware::joinRpcThreadpool();

    ALOGE("service is terminating");
    return 1;
}
