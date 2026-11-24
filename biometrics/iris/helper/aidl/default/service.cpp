/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include <android-base/logging.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>

#include "Face.h"

using ::aidl::android::hardware::biometrics::face::Face;
using ::aidl::android::hardware::biometrics::face::Preview;

int main() {
    ABinderProcess_setThreadPoolMaxThreadCount(0);
    std::shared_ptr<Face> face = ndk::SharedRefBase::make<Face>();

    const std::string instance = std::string(Face::descriptor) + "/default";
    binder_status_t status = AServiceManager_addService(face->asBinder().get(), instance.c_str());
    CHECK_EQ(status, STATUS_OK);

    const std::string preview = std::string(Preview::descriptor) + "/default";
    status = AServiceManager_addService(face->preview()->asBinder().get(), preview.c_str());
    CHECK_EQ(status, STATUS_OK);

    ABinderProcess_joinThreadPool();
    return EXIT_FAILURE;
}
