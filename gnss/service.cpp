/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "android.hardware.gnss@1.1-service.samsung"

#include <android-base/logging.h>
#include <android/hardware/gnss/1.1/IGnss.h>
#include <hidl/LegacySupport.h>

using ::android::OK;
using ::android::hardware::configureRpcThreadpool;
using ::android::hardware::joinRpcThreadpool;
using ::android::hardware::registerPassthroughServiceImplementation;
using ::android::hardware::gnss::V1_1::IGnss;

int main(int /* argc */, char* /* argv */[]) {
    configureRpcThreadpool(2, true /* will join */);
    if (registerPassthroughServiceImplementation<IGnss>("default") != OK) {
        LOG(ERROR) << "Could not register GNSS 1.1 service.";
        return 1;
    }
    joinRpcThreadpool();

    return 1;
}
