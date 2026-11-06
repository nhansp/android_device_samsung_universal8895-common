/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string>

#include <utils/RefBase.h>
#include <utils/StrongPointer.h>

namespace android::frameworks::schedulerservice::V1_0 {

struct ISchedulingPolicyService : public virtual RefBase {
    static sp<ISchedulingPolicyService> getService(const std::string& serviceName = "default",
                                                   bool getStub = false);
};

// frameworks/hardware/interfaces/schedulerservice is gone and no service
// registers it any more, so there is no instance to hand out.
sp<ISchedulingPolicyService> ISchedulingPolicyService::getService(const std::string&, bool) {
    return nullptr;
}

}  // namespace android::frameworks::schedulerservice::V1_0
