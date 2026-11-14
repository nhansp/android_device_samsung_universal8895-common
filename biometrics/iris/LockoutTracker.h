/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <chrono>

namespace aidl::android::hardware::biometrics::face {

// Stock SecIrisService: MAX_FAILED_ATTEMPTS_LOCKOUT_TIMED 5, FAIL_LOCKOUT_TIMEOUT_MS 30000,
// MAX_FAILED_ATTEMPTS_LOCKOUT_PERMANENT 20.
class LockoutTracker {
  public:
    enum class Mode { NONE, TIMED, PERMANENT };

    static constexpr int kTimedThreshold = 5;
    static constexpr std::chrono::milliseconds kTimedDuration{30000};
    static constexpr int kPermanentThreshold = 20;

    void reset(bool clearAttempts) {
        if (clearAttempts) mFailed = 0;
        mTimedEnd = {};
    }

    void addFailedAttempt() {
        if (++mFailed >= kPermanentThreshold) return;
        if (mFailed % kTimedThreshold == 0) mTimedEnd = Clock::now() + kTimedDuration;
    }

    Mode mode() const {
        if (mFailed >= kPermanentThreshold) return Mode::PERMANENT;
        return timeLeft().count() > 0 ? Mode::TIMED : Mode::NONE;
    }

    std::chrono::milliseconds timeLeft() const {
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(mTimedEnd - Clock::now());
        return left.count() > 0 ? left : std::chrono::milliseconds(0);
    }

  private:
    using Clock = std::chrono::steady_clock;

    int mFailed = 0;
    Clock::time_point mTimedEnd;
};

}  // namespace aidl::android::hardware::biometrics::face
