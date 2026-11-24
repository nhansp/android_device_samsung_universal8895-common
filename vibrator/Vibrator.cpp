/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include "Vibrator.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/stringprintf.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <thread>

namespace aidl {
namespace android {
namespace hardware {
namespace vibrator {

namespace {

constexpr char kEnable[] = "/sys/class/timed_output/vibrator/enable";
constexpr char kIntensity[] = "/sys/class/timed_output/vibrator/intensity";
constexpr char kMultiFreq[] = "/sys/class/timed_output/vibrator/multi_freq";
constexpr char kHapticEngine[] = "/sys/class/timed_output/vibrator/haptic_engine";

constexpr int kIntensityMax = 10000;
// below this the LRA does not move noticeably
constexpr float kLevelFloor = 0.15f;
// one sysfs write
constexpr size_t kEngineBufMax = 4095;

constexpr int kComposeDelayMax = 1000;
constexpr int kComposeSizeMax = 16;
constexpr int kRampSteps = 5;

// multi_freq: table entry 0 is the motor's resonance; 0.1 Hz values for the rest
constexpr int kFreqResonance = 0;
constexpr int kFreqShort = 2000;
constexpr int kFreqLow = 1200;

// amplitudes match the stock HAL's CLICK
const std::map<EffectStrength, float> kStrength = {
        {EffectStrength::LIGHT, 36.0f / 255.0f},
        {EffectStrength::MEDIUM, 128.0f / 255.0f},
        {EffectStrength::STRONG, 1.0f},
};

void Ramp(std::vector<Packet>* out, float from, float to, int ms, int freq, bool od) {
    int left = ms;
    for (int i = 0; i < kRampSteps; i++) {
        int step = left / (kRampSteps - i);
        float level = from + (to - from) * (i + 0.5f) / kRampSteps;
        out->push_back({step, level, freq, od});
        left -= step;
    }
}

// Short primitives follow the stock 7 ms, 200 Hz overdrive tick, long ones the length and envelope
// of the Pixel primitives at resonance; frequency does not change how a short pulse feels on this
// LRA, so only level and length differ
std::vector<Packet> PrimitivePackets(CompositePrimitive primitive) {
    std::vector<Packet> p;
    switch (primitive) {
        case CompositePrimitive::CLICK:
            p.push_back({7, 1.0f, kFreqShort, true});
            break;
        case CompositePrimitive::LIGHT_TICK:
            p.push_back({5, 0.6f, kFreqShort, true});
            break;
        case CompositePrimitive::LOW_TICK:
            p.push_back({11, 0.5f, kFreqLow, false});
            break;
        case CompositePrimitive::THUD:
            Ramp(&p, 1.0f, 0.0f, 300, kFreqLow, false);
            break;
        case CompositePrimitive::SPIN:
            Ramp(&p, 0.5f, 1.0f, 78, kFreqResonance, false);
            Ramp(&p, 1.0f, 0.5f, 54, kFreqResonance, false);
            break;
        case CompositePrimitive::QUICK_RISE:
            Ramp(&p, 0.0f, 1.0f, 150, kFreqResonance, false);
            break;
        case CompositePrimitive::SLOW_RISE:
            Ramp(&p, 0.0f, 1.0f, 500, kFreqResonance, false);
            break;
        case CompositePrimitive::QUICK_FALL:
            Ramp(&p, 0.5f, 1.0f, 51, kFreqShort, false);
            Ramp(&p, 1.0f, 0.0f, 50, kFreqShort, false);
            break;
        default:
            break;
    }
    return p;
}

std::vector<Packet> EffectPackets(Effect effect) {
    switch (effect) {
        case Effect::CLICK:
            return PrimitivePackets(CompositePrimitive::CLICK);
        case Effect::TICK:
            return PrimitivePackets(CompositePrimitive::LIGHT_TICK);
        case Effect::TEXTURE_TICK:
            return {{5, 0.4f, kFreqShort, true}};
        case Effect::HEAVY_CLICK:
            return {{10, 1.0f, kFreqShort, true}};
        case Effect::DOUBLE_CLICK:
            // two clicks 107 ms apart, the first lighter
            return {{7, 0.65f, kFreqShort, true},
                    {100, 0.0f, kFreqShort, false},
                    {7, 1.0f, kFreqShort, true}};
        default:
            return {};
    }
}

int DurationMs(const std::vector<Packet>& packets) {
    int ms = 0;
    for (const auto& p : packets) ms += p.ms;
    return ms;
}

int ToIntensity(float level) {
    if (level == 0.0f) return 0;
    float mag = kLevelFloor + (1.0f - kLevelFloor) * std::min(std::fabs(level), 1.0f);
    int intensity = std::lround(mag * kIntensityMax);
    return level < 0 ? -intensity : intensity;
}

bool WriteNode(const char* path, const std::string& value) {
    if (!::android::base::WriteStringToFile(value, path)) {
        PLOG(ERROR) << "Failed to write " << value.size() << " bytes to " << path;
        return false;
    }
    return true;
}

void CompleteAfter(int ms, const std::shared_ptr<IVibratorCallback>& callback) {
    if (callback == nullptr) return;
    std::thread([ms, callback] {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        callback->onComplete();
    }).detach();
}

}  // namespace

ndk::ScopedAStatus Vibrator::getCapabilities(int32_t* _aidl_return) {
    *_aidl_return = IVibrator::CAP_ON_CALLBACK | IVibrator::CAP_PERFORM_CALLBACK |
                    IVibrator::CAP_AMPLITUDE_CONTROL | IVibrator::CAP_COMPOSE_EFFECTS;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::off() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!WriteNode(kEnable, "0")) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_SERVICE_SPECIFIC);
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::on(int32_t timeout_ms,
                                const std::shared_ptr<IVibratorCallback>& callback) {
    if (timeout_ms <= 0) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    // 0 first: it drops any packets an effect left loaded
    if (!WriteNode(kEnable, "0") || !WriteNode(kMultiFreq, std::to_string(kFreqResonance)) ||
        !WriteNode(kIntensity, std::to_string(std::lround(amplitude_ * kIntensityMax))) ||
        !WriteNode(kEnable, std::to_string(timeout_ms))) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_SERVICE_SPECIFIC);
    }
    CompleteAfter(timeout_ms, callback);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::play(const std::vector<Packet>& packets,
                                  const std::shared_ptr<IVibratorCallback>& callback) {
    std::string buf;
    for (const auto& p : packets) {
        buf += ::android::base::StringPrintf(" %d %d %d %d", p.ms, ToIntensity(p.level), p.freq,
                                             p.od ? 1 : 0);
    }
    buf = std::to_string(packets.size() * 4) + buf;
    if (buf.size() > kEngineBufMax) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!WriteNode(kHapticEngine, buf) || !WriteNode(kEnable, "1")) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_SERVICE_SPECIFIC);
    }
    CompleteAfter(DurationMs(packets), callback);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::perform(Effect effect, EffectStrength strength,
                                     const std::shared_ptr<IVibratorCallback>& callback,
                                     int32_t* _aidl_return) {
    auto packets = EffectPackets(effect);
    if (packets.empty()) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }
    auto it = kStrength.find(strength);
    if (it == kStrength.end()) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }
    for (auto& p : packets) p.level *= it->second;

    auto status = play(packets, callback);
    if (status.isOk()) *_aidl_return = DurationMs(packets);
    return status;
}

ndk::ScopedAStatus Vibrator::getSupportedEffects(std::vector<Effect>* _aidl_return) {
    *_aidl_return = {Effect::CLICK, Effect::TICK, Effect::TEXTURE_TICK, Effect::HEAVY_CLICK,
                     Effect::DOUBLE_CLICK};
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::setAmplitude(float amplitude) {
    if (amplitude <= 0.0f || amplitude > 1.0f) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    amplitude_ = amplitude;
    if (!WriteNode(kIntensity, std::to_string(std::lround(amplitude * kIntensityMax)))) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_SERVICE_SPECIFIC);
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::setExternalControl(bool /*enabled*/) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus Vibrator::getCompositionDelayMax(int32_t* _aidl_return) {
    *_aidl_return = kComposeDelayMax;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getCompositionSizeMax(int32_t* _aidl_return) {
    *_aidl_return = kComposeSizeMax;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getSupportedPrimitives(std::vector<CompositePrimitive>* _aidl_return) {
    *_aidl_return = {CompositePrimitive::NOOP,       CompositePrimitive::CLICK,
                     CompositePrimitive::THUD,       CompositePrimitive::SPIN,
                     CompositePrimitive::QUICK_RISE, CompositePrimitive::SLOW_RISE,
                     CompositePrimitive::QUICK_FALL, CompositePrimitive::LIGHT_TICK,
                     CompositePrimitive::LOW_TICK};
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getPrimitiveDuration(CompositePrimitive primitive,
                                                  int32_t* _aidl_return) {
    if (primitive == CompositePrimitive::NOOP) {
        *_aidl_return = 0;
        return ndk::ScopedAStatus::ok();
    }
    auto packets = PrimitivePackets(primitive);
    if (packets.empty()) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }
    *_aidl_return = DurationMs(packets);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::compose(const std::vector<CompositeEffect>& composite,
                                     const std::shared_ptr<IVibratorCallback>& callback) {
    if (composite.empty() || composite.size() > static_cast<size_t>(kComposeSizeMax)) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }

    std::vector<Packet> packets;
    for (const auto& e : composite) {
        if (e.delayMs < 0 || e.delayMs > kComposeDelayMax || e.scale < 0.0f || e.scale > 1.0f) {
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        }
        if (e.delayMs > 0) packets.push_back({e.delayMs, 0.0f, kFreqResonance, false});
        if (e.primitive == CompositePrimitive::NOOP) continue;

        auto p = PrimitivePackets(e.primitive);
        if (p.empty()) {
            return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
        }
        for (auto& q : p) q.level *= e.scale;
        packets.insert(packets.end(), p.begin(), p.end());
    }
    if (packets.empty()) {
        CompleteAfter(0, callback);
        return ndk::ScopedAStatus::ok();
    }
    return play(packets, callback);
}

ndk::ScopedAStatus Vibrator::getSupportedAlwaysOnEffects(std::vector<Effect>* /*_aidl_return*/) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus Vibrator::alwaysOnEnable(int32_t /*id*/, Effect /*effect*/,
                                            EffectStrength /*strength*/) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus Vibrator::alwaysOnDisable(int32_t /*id*/) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus Vibrator::getResonantFrequency(float* /*_aidl_return*/) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus Vibrator::getQFactor(float* /*_aidl_return*/) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus Vibrator::getFrequencyResolution(float* /*_aidl_return*/) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus Vibrator::getFrequencyMinimum(float* /*_aidl_return*/) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus Vibrator::getBandwidthAmplitudeMap(std::vector<float>* /*_aidl_return*/) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus Vibrator::getPwlePrimitiveDurationMax(int32_t* /*_aidl_return*/) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus Vibrator::getPwleCompositionSizeMax(int32_t* /*_aidl_return*/) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus Vibrator::getSupportedBraking(std::vector<Braking>* /*_aidl_return*/) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus Vibrator::composePwle(const std::vector<PrimitivePwle>& /*composite*/,
                                         const std::shared_ptr<IVibratorCallback>& /*callback*/) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

}  // namespace vibrator
}  // namespace hardware
}  // namespace android
}  // namespace aidl
