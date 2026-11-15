/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

namespace aidl::android::hardware::biometrics::face {

// Runs tasks one at a time on its own thread, in time order. The session callbacks must not run
// on binder threads, and the library callbacks arrive on the capture thread.
class Worker {
  public:
    using Clock = std::chrono::steady_clock;

    Worker() : mThread(&Worker::loop, this) {}

    void post(std::function<void()> task) { postAt(Clock::now(), std::move(task)); }

    void postDelayed(std::chrono::milliseconds delay, std::function<void()> task) {
        postAt(Clock::now() + delay, std::move(task));
    }

  private:
    void postAt(Clock::time_point when, std::function<void()> task) {
        std::lock_guard<std::mutex> lock(mLock);
        mTasks.emplace(when, std::move(task));
        mCond.notify_one();
    }

    [[noreturn]] void loop() {
        std::unique_lock<std::mutex> lock(mLock);
        for (;;) {
            if (mTasks.empty()) {
                mCond.wait(lock);
                continue;
            }
            auto next = mTasks.begin();
            if (next->first > Clock::now()) {
                mCond.wait_until(lock, next->first);
                continue;
            }
            auto task = std::move(next->second);
            mTasks.erase(next);
            lock.unlock();
            task();
            lock.lock();
        }
    }

    std::mutex mLock;
    std::condition_variable mCond;
    std::multimap<Clock::time_point, std::function<void()>> mTasks;
    std::thread mThread;
};

}  // namespace aidl::android::hardware::biometrics::face
