/*
 * SPDX-FileCopyrightText: The Android Open Source Project
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "android.hardware.graphics.composer@2.1-service"

#include <dlfcn.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/ioctl.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <android-base/unique_fd.h>
#include <android/hardware/graphics/composer/2.1/IComposer.h>

#include <binder/ProcessState.h>
#include <composer-passthrough/2.1/HwcLoader.h>
#include <hidl/LegacySupport.h>
#include <hwc2on1adapter/HWC2On1Adapter.h>

using android::HWC2On1Adapter;
using android::hardware::hidl_vec;
using android::hardware::graphics::composer::V2_1::Config;
using android::hardware::graphics::composer::V2_1::Display;
using android::hardware::graphics::composer::V2_1::Error;
using android::hardware::graphics::composer::V2_1::IComposer;
using android::hardware::graphics::composer::V2_1::IComposerCallback;
using android::hardware::graphics::composer::V2_1::IComposerClient;
using android::hardware::graphics::composer::V2_1::Layer;
using android::hardware::graphics::composer::V2_1::hal::ComposerHal;
using android::hardware::graphics::composer::V2_1::passthrough::HwcHal;
using android::hardware::graphics::composer::V2_1::passthrough::HwcLoader;

namespace {

// Display scaler (DSU) of the stock HWC: the panel shows a lower resolution, scaled up by the DECON
// and the DDI. The modes are the HWC's mDSUInfo (count at +0xc48, {width, height} pairs from
// +0xc4c) and the ExynosDevice is stored at +0xe8 of the HWC1 device; neither is exported.
class ExynosScaler {
  public:
    struct Mode {
        int32_t width;
        int32_t height;
    };

    bool Init(hwc_composer_device_1* hwc1) {
        void* lib = dlopen("libexynosdisplay.so", RTLD_NOW | RTLD_NOLOAD);
        if (lib == nullptr) {
            ALOGE("scaler: libexynosdisplay.so is not loaded");
            return false;
        }
        auto get_display = reinterpret_cast<void* (*)(void*, uint32_t)>(
                dlsym(lib, "_ZN12ExynosDevice10getDisplayEj"));
        set_scaler_ = reinterpret_cast<void (*)(void*, int, int)>(
                dlsym(lib, "_ZN13ExynosDisplay18setDDIScalerEnableEii"));
        if (get_display == nullptr || set_scaler_ == nullptr) {
            ALOGE("scaler: libexynosdisplay.so lacks the scaler symbols");
            return false;
        }

        void* device = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(hwc1) + 0xe8);
        display_ = device != nullptr ? get_display(device, HWC_DISPLAY_PRIMARY) : nullptr;
        if (display_ == nullptr) {
            ALOGE("scaler: no primary ExynosDisplay");
            return false;
        }
        return true;
    }

    // mDSUInfo is filled from psr_info by the HWC, read at each use
    std::vector<Mode> Modes() const {
        std::vector<Mode> modes;
        if (display_ == nullptr) {
            return modes;
        }
        const uint8_t* display = static_cast<const uint8_t*>(display_);
        uint32_t count = *reinterpret_cast<const uint32_t*>(display + 0xc48);
        for (uint32_t i = 0; i < count && i < kMaxModes; i++) {
            const int32_t* mode = reinterpret_cast<const int32_t*>(display + 0xc4c + 8 * i);
            if (mode[0] > 0 && mode[1] > 0) {
                modes.push_back({mode[0], mode[1]});
            }
        }
        return modes;
    }

    void Set(const Mode& mode) const { set_scaler_(display_, mode.width, mode.height); }

  private:
    static constexpr uint32_t kMaxModes = 3;  // DSU_MODE_1..3, the HWC's own bound
    void* display_ = nullptr;
    void (*set_scaler_)(void*, int, int) = nullptr;
};

// Records the first connected display as the primary, which HWC2On1Adapter connects first
class HotplugTracker : public ComposerHal::EventCallback {
  public:
    void SetTarget(ComposerHal::EventCallback* target) { target_ = target; }
    Display Primary() const { return primary_.load(); }

    void onHotplug(Display display, IComposerCallback::Connection connected) override {
        Display none = kNone;
        if (connected == IComposerCallback::Connection::CONNECTED) {
            primary_.compare_exchange_strong(none, display);
        }
        target_->onHotplug(display, connected);
    }
    void onRefresh(Display display) override { target_->onRefresh(display); }
    void onVsync(Display display, int64_t timestamp) override {
        target_->onVsync(display, timestamp);
    }

    static constexpr Display kNone = ~Display(0);

  private:
    ComposerHal::EventCallback* target_ = nullptr;
    std::atomic<Display> primary_{kNone};
};

// Works around the stock HWC1 behind HWC2On1Adapter:
// - SOLID_COLOR layers are composed by the client: the HWC dereferences the color, which the
//   adapter keeps in the union that holds the buffer handle
// - the retire fence of frame N-1 is the present fence of N: the HWC1 retire fence of N signals one
//   refresh late and SurfaceFlinger would count every frame as missed
// - DOZE_SUSPEND waits for a frame presented in DOZE, then goes to the framebuffer directly: from
//   an off display it would stay off, and the DECON drops every frame in DOZE_SUSPEND
// - the scaler modes are offered as display configs, as the HWC1 reports one config and ignores
//   setActiveConfig on the primary display
class SolidColorHwcHal : public HwcHal {
  public:
    void InitScaler(hwc_composer_device_1* hwc1) { scaler_ready_ = scaler_.Init(hwc1); }

    void InitDozeSuspend() {
        framebuffer_.reset(open("/dev/graphics/fb0", O_RDWR | O_CLOEXEC));
        if (framebuffer_ < 0) {
            ALOGE("doze: cannot open /dev/graphics/fb0: %s", strerror(errno));
        }
    }

    void registerEventCallback(ComposerHal::EventCallback* callback) override {
        hotplug_.SetTarget(callback);
        HwcHal::registerEventCallback(&hotplug_);
    }

    Error getDisplayConfigs(Display display, hidl_vec<Config>* out_configs) override {
        Error err = HwcHal::getDisplayConfigs(display, out_configs);
        if (err != Error::NONE || !IsScaled(display) || out_configs->size() != 1) {
            return err;
        }

        std::vector<ExynosScaler::Mode> modes = scaler_.Modes();
        if (modes.size() < 2) {
            return err;
        }

        std::lock_guard<std::mutex> lock(scaler_mutex_);
        base_config_ = (*out_configs)[0];
        std::vector<Config> configs{base_config_};
        for (size_t i = 1; i < modes.size(); i++) {
            configs.push_back(static_cast<Config>(base_config_ + i));
        }
        modes_ = modes;
        *out_configs = configs;
        return err;
    }

    Error getDisplayAttribute(Display display, Config config, IComposerClient::Attribute attribute,
                              int32_t* out_value) override {
        size_t index;
        if (!IsScaled(display) || !ScalerIndex(config, &index) || index == 0) {
            return HwcHal::getDisplayAttribute(display, config, attribute, out_value);
        }

        ExynosScaler::Mode base, mode;
        {
            std::lock_guard<std::mutex> lock(scaler_mutex_);
            base = modes_[0];
            mode = modes_[index];
        }
        Error err = HwcHal::getDisplayAttribute(display, base_config_, attribute, out_value);
        if (err != Error::NONE) {
            return err;
        }
        switch (attribute) {
            case IComposerClient::Attribute::WIDTH:
                *out_value = mode.width;
                break;
            case IComposerClient::Attribute::HEIGHT:
                *out_value = mode.height;
                break;
            case IComposerClient::Attribute::DPI_X:
                *out_value = static_cast<int32_t>(int64_t(*out_value) * mode.width / base.width);
                break;
            case IComposerClient::Attribute::DPI_Y:
                *out_value = static_cast<int32_t>(int64_t(*out_value) * mode.height / base.height);
                break;
            default:
                break;
        }
        return err;
    }

    Error getActiveConfig(Display display, Config* out_config) override {
        if (IsScaled(display)) {
            std::lock_guard<std::mutex> lock(scaler_mutex_);
            if (!modes_.empty()) {
                *out_config = active_config_.value_or(base_config_);
                return Error::NONE;
            }
        }
        return HwcHal::getActiveConfig(display, out_config);
    }

    Error setActiveConfig(Display display, Config config) override {
        size_t index;
        if (!IsScaled(display) || !ScalerIndex(config, &index)) {
            return HwcHal::setActiveConfig(display, config);
        }

        std::lock_guard<std::mutex> lock(scaler_mutex_);
        scaler_.Set(modes_[index]);
        active_config_ = config;
        return Error::NONE;
    }

    Error setPowerMode(Display display, IComposerClient::PowerMode mode) override {
        if (display != hotplug_.Primary()) {
            return HwcHal::setPowerMode(display, mode);
        }

        std::lock_guard<std::mutex> lock(power_mutex_);
        PowerState& state = power_[display];
        state.suspend_pending = false;
        switch (mode) {
            case IComposerClient::PowerMode::DOZE_SUSPEND:
                if (state.mode == IComposerClient::PowerMode::DOZE_SUSPEND) {
                    return Error::NONE;
                }
                if (state.mode != IComposerClient::PowerMode::DOZE) {
                    Error err = HwcHal::setPowerMode(display, IComposerClient::PowerMode::DOZE);
                    if (err != Error::NONE) {
                        return err;
                    }
                    state.mode = IComposerClient::PowerMode::DOZE;
                    state.doze_frame_presented = false;
                }
                if (!state.doze_frame_presented) {
                    state.suspend_pending = true;
                    RequestDozeFrame(display);
                    return Error::NONE;
                }
                return SuspendDoze(state);
            case IComposerClient::PowerMode::DOZE:
                if (state.mode == IComposerClient::PowerMode::DOZE_SUSPEND) {
                    return ResumeDoze(state);
                }
                break;
            case IComposerClient::PowerMode::OFF:
                // the HWC blanks from DOZE; a blank in DOZE_SUSPEND would leave the panel in ALPM
                if (state.mode == IComposerClient::PowerMode::DOZE_SUSPEND) {
                    Error err = ResumeDoze(state);
                    if (err != Error::NONE) {
                        return err;
                    }
                }
                break;
            default:
                break;
        }

        Error err = HwcHal::setPowerMode(display, mode);
        if (err == Error::NONE) {
            state.mode = mode;
            state.doze_frame_presented = false;
        }
        return err;
    }

    Error presentDisplay(Display display, int32_t* out_present_fence,
                         std::vector<Layer>* out_layers,
                         std::vector<int32_t>* out_release_fences) override {
        Error err =
                HwcHal::presentDisplay(display, out_present_fence, out_layers, out_release_fences);
        if (err != Error::NONE) {
            return err;
        }

        IComposerClient::DisplayType type;
        if (getDisplayType(display, &type) != Error::NONE ||
            type != IComposerClient::DisplayType::PHYSICAL) {
            return err;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        android::base::unique_fd& previous_retire = previous_retire_[display];
        int32_t retire = *out_present_fence;
        // -1 for the first frame: no fence means presented
        *out_present_fence = previous_retire.release();
        previous_retire.reset(retire);
        OnDozeFramePresented(display);
        return err;
    }

    Error setLayerCompositionType(Display display, Layer layer, int32_t type) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (type == HWC2_COMPOSITION_SOLID_COLOR) {
                solid_color_layers_[display].insert(layer);
                type = HWC2_COMPOSITION_CLIENT;
            } else {
                solid_color_layers_[display].erase(layer);
            }
        }
        return HwcHal::setLayerCompositionType(display, layer, type);
    }

  protected:
    int32_t getChangedCompositionTypes(
            Display display, uint32_t* out_types_count, Layer* out_changed_layers,
            IComposerClient::Composition* out_composition_types) override {
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
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = solid_color_layers_.find(display);
            if (it != solid_color_layers_.end()) {
                for (Layer layer : it->second) {
                    if (std::find(layers.begin(), layers.end(), layer) == layers.end()) {
                        layers.push_back(layer);
                        types.push_back(IComposerClient::Composition::CLIENT);
                    }
                }
            }
        }

        if (out_changed_layers == nullptr || out_composition_types == nullptr) {
            *out_types_count = layers.size();
            return HWC2_ERROR_NONE;
        }

        uint32_t n = std::min<uint32_t>(*out_types_count, layers.size());
        std::copy_n(layers.begin(), n, out_changed_layers);
        std::copy_n(types.begin(), n, out_composition_types);
        *out_types_count = n;
        return HWC2_ERROR_NONE;
    }

    void onLayerDestroyed(Display display, Layer layer) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = solid_color_layers_.find(display);
        if (it != solid_color_layers_.end()) {
            it->second.erase(layer);
        }
    }

  private:
    bool IsScaled(Display display) const { return scaler_ready_ && display == hotplug_.Primary(); }

    bool ScalerIndex(Config config, size_t* out_index) {
        std::lock_guard<std::mutex> lock(scaler_mutex_);
        if (modes_.empty() || config < base_config_ || config - base_config_ >= modes_.size()) {
            return false;
        }
        *out_index = config - base_config_;
        return true;
    }

    ExynosScaler scaler_;
    bool scaler_ready_ = false;
    HotplugTracker hotplug_;
    std::mutex scaler_mutex_;
    std::vector<ExynosScaler::Mode> modes_;
    Config base_config_ = 0;
    std::optional<Config> active_config_;

    // SurfaceFlinger has nothing new to draw after the screen-off animation or a power mode
    // change (IComposerCallback.onRefresh is oneway)
    void RequestDozeFrame(Display display) {
        if (mEventCallback != nullptr) {
            mEventCallback->onRefresh(display);
        }
    }

    // S3CFB_POWER_MODE and the DECON's doze modes
    static constexpr unsigned int kS3cfbPowerMode = 0x400446df;
    static constexpr uint32_t kDeconPowerDoze = 1;
    static constexpr uint32_t kDeconPowerDozeSuspend = 3;

    struct PowerState {
        IComposerClient::PowerMode mode = IComposerClient::PowerMode::OFF;
        // DOZE_SUSPEND waits for a frame presented in DOZE
        bool suspend_pending = false;
        bool doze_frame_presented = false;
    };

    Error SetDeconPowerMode(uint32_t mode) {
        if (framebuffer_ < 0 || ioctl(framebuffer_.get(), kS3cfbPowerMode, &mode) != 0) {
            ALOGE("doze: S3CFB_POWER_MODE(%u) failed: %s", mode, strerror(errno));
            return Error::NO_RESOURCES;
        }
        return Error::NONE;
    }

    Error SuspendDoze(PowerState& state) {
        Error err = SetDeconPowerMode(kDeconPowerDozeSuspend);
        if (err == Error::NONE) {
            state.mode = IComposerClient::PowerMode::DOZE_SUSPEND;
        }
        return err;
    }

    Error ResumeDoze(PowerState& state) {
        Error err = SetDeconPowerMode(kDeconPowerDoze);
        if (err == Error::NONE) {
            state.mode = IComposerClient::PowerMode::DOZE;
            state.doze_frame_presented = false;
        }
        return err;
    }

    void OnDozeFramePresented(Display display) {
        if (display != hotplug_.Primary()) {
            return;
        }

        std::lock_guard<std::mutex> lock(power_mutex_);
        auto it = power_.find(display);
        if (it == power_.end()) {
            return;
        }

        PowerState& state = it->second;
        if (state.mode == IComposerClient::PowerMode::DOZE_SUSPEND) {
            if (ResumeDoze(state) == Error::NONE) {
                state.suspend_pending = true;
                RequestDozeFrame(display);
            }
            return;
        }
        if (state.mode != IComposerClient::PowerMode::DOZE) {
            return;
        }

        state.doze_frame_presented = true;
        if (state.suspend_pending) {
            state.suspend_pending = false;
            SuspendDoze(state);
        }
    }

    android::base::unique_fd framebuffer_;
    std::mutex power_mutex_;
    std::unordered_map<Display, PowerState> power_;
    std::mutex mutex_;
    std::unordered_map<Display, std::unordered_set<Layer>> solid_color_layers_;
    std::unordered_map<Display, android::base::unique_fd> previous_retire_;
};

// HwcLoader::load() with SolidColorHwcHal, opening the HWC1 device here to keep it for the scaler
class SolidColorHwcLoader : public HwcLoader {
  public:
    static IComposer* load() {
        const hw_module_t* module = loadModule();
        if (!module) {
            return nullptr;
        }

        hw_device_t* device;
        int error = module->methods->open(module, HWC_HARDWARE_COMPOSER, &device);
        if (error) {
            ALOGE("failed to open hwcomposer device: %s", strerror(-error));
            return nullptr;
        }

        hwc_composer_device_1* hwc1 = nullptr;
        hwc2_device_t* hwc2;
        if (((device->version >> 24) & 0xf) != 2) {
            hwc1 = reinterpret_cast<hwc_composer_device_1*>(device);
            if (((hwc1->common.version >> 16) & 0xf) < 1) {
                ALOGE("hwcomposer 1.0 is not supported");
                hwc1->common.close(&hwc1->common);
                return nullptr;
            }
            hwc2 = new HWC2On1Adapter(hwc1);
        } else {
            hwc2 = reinterpret_cast<hwc2_device_t*>(device);
        }

        auto hal = std::make_unique<SolidColorHwcHal>();
        if (!hal->initWithDevice(hwc2, hwc1 == nullptr)) {
            return nullptr;
        }
        if (hwc1 != nullptr) {
            hal->InitScaler(hwc1);
            hal->InitDozeSuspend();
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
