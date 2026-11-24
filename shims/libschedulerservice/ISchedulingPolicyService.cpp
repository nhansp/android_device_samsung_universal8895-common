/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string>

#include <utils/RefBase.h>
#include <utils/StrongPointer.h>

namespace android::frameworks::schedulerservice::V1_0 {

struct ISchedulingPolicyService : public virtual RefBase {
    static sp<ISchedulingPolicyService> getService(const std::string& service_name = "default",
                                                   bool get_stub = false);
};

// the schedulerservice HAL is gone, so there is no instance to hand out
sp<ISchedulingPolicyService> ISchedulingPolicyService::getService(const std::string&, bool) {
    return nullptr;
}

}  // namespace android::frameworks::schedulerservice::V1_0
