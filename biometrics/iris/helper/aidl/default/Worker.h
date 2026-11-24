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

    Worker() : thread_(&Worker::Loop, this) {}

    void Post(std::function<void()> task) { PostAt(Clock::now(), std::move(task)); }

    void PostDelayed(std::chrono::milliseconds delay, std::function<void()> task) {
        PostAt(Clock::now() + delay, std::move(task));
    }

  private:
    void PostAt(Clock::time_point when, std::function<void()> task) {
        std::lock_guard<std::mutex> lock(lock_);
        tasks_.emplace(when, std::move(task));
        cond_.notify_one();
    }

    [[noreturn]] void Loop() {
        std::unique_lock<std::mutex> lock(lock_);
        for (;;) {
            if (tasks_.empty()) {
                cond_.wait(lock);
                continue;
            }
            auto next = tasks_.begin();
            if (next->first > Clock::now()) {
                cond_.wait_until(lock, next->first);
                continue;
            }
            auto task = std::move(next->second);
            tasks_.erase(next);
            lock.unlock();
            task();
            lock.lock();
        }
    }

    std::mutex lock_;
    std::condition_variable cond_;
    std::multimap<Clock::time_point, std::function<void()>> tasks_;
    std::thread thread_;
};

}  // namespace aidl::android::hardware::biometrics::face
