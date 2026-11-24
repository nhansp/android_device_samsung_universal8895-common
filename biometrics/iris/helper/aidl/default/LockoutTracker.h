/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <chrono>

namespace aidl::android::hardware::biometrics::face {

class LockoutTracker {
  public:
    enum class Mode { kNone, kTimed, kPermanent };

    static constexpr int kTimedThreshold = 5;
    static constexpr std::chrono::milliseconds kTimedDuration{30000};
    static constexpr int kPermanentThreshold = 20;

    void Reset(bool clear_attempts) {
        if (clear_attempts) failed_ = 0;
        timed_end_ = {};
    }

    void AddFailedAttempt() {
        if (++failed_ >= kPermanentThreshold) return;
        if (failed_ % kTimedThreshold == 0) timed_end_ = Clock::now() + kTimedDuration;
    }

    Mode GetMode() const {
        if (failed_ >= kPermanentThreshold) return Mode::kPermanent;
        return TimeLeft().count() > 0 ? Mode::kTimed : Mode::kNone;
    }

    std::chrono::milliseconds TimeLeft() const {
        auto left =
                std::chrono::duration_cast<std::chrono::milliseconds>(timed_end_ - Clock::now());
        return left.count() > 0 ? left : std::chrono::milliseconds(0);
    }

  private:
    using Clock = std::chrono::steady_clock;

    int failed_ = 0;
    Clock::time_point timed_end_;
};

}  // namespace aidl::android::hardware::biometrics::face
