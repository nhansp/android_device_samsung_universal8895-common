/*
 * SPDX-FileCopyrightText: The Android Open Source Project
 * SPDX-License-Identifier: Apache-2.0
 *
 * From hardware/interfaces/graphics/composer/2.1/default/service.cpp.
 */

#define LOG_TAG "android.hardware.graphics.composer@2.1-service"

#include <sched.h>

#include <android/hardware/graphics/composer/2.1/IComposer.h>

#include <binder/ProcessState.h>
#include <composer-passthrough/2.1/HwcLoader.h>
#include <hidl/LegacySupport.h>

using android::hardware::graphics::composer::V2_1::IComposer;
using android::hardware::graphics::composer::V2_1::passthrough::HwcLoader;

int main() {
    // the conventional HAL might start binder services
    android::ProcessState::initWithDriver("/dev/vndbinder");
    android::ProcessState::self()->setThreadPoolMaxThreadCount(4);
    android::ProcessState::self()->startThreadPool();

    // same as SF main thread
    struct sched_param param = {0};
    param.sched_priority = 2;
    if (sched_setscheduler(0, SCHED_FIFO | SCHED_RESET_ON_FORK,
                &param) != 0) {
        ALOGE("Couldn't set SCHED_FIFO: %d", errno);
    }

    android::hardware::configureRpcThreadpool(4, true /* will join */);

    android::sp<IComposer> composer = HwcLoader::load();
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
