/*
 * SPDX-FileCopyrightText: The Android Open Source Project
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 *
 * hardware/interfaces/graphics/composer/2.1/default/service.cpp, loading the HWC through a
 * HwcHal that keeps solid-color layers away from the stock HWC1 (see SolidColorHwcHal).
 */

#define LOG_TAG "android.hardware.graphics.composer@2.1-service"

#include <sched.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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
class SolidColorHwcHal : public HwcHal {
  public:
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
    std::mutex mMutex;
    std::unordered_map<Display, std::unordered_set<Layer>> mSolidColorLayers;
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
