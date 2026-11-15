/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include "IrisLib.h"

#include <dlfcn.h>

#include <type_traits>

#include <android-base/logging.h>

namespace aidl::android::hardware::biometrics::face {

bool IrisLib::load() {
    void* handle = dlopen("libIrisService.so", RTLD_NOW);
    if (!handle) {
        LOG(ERROR) << "Failed to load libIrisService.so: " << dlerror();
        return false;
    }

    bool ok = true;
    auto sym = [&](auto& fn, const char* name) {
        fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(handle, name));
        if (!fn) {
            LOG(ERROR) << "libIrisService.so has no " << name;
            ok = false;
        }
    };
    sym(ss_iris_open, "ss_iris_open");
    sym(ss_iris_close, "ss_iris_close");
    sym(ss_set_notify_callback, "ss_set_notify_callback");
    sym(ss_iris_pre_enroll, "ss_iris_pre_enroll");
    sym(ss_iris_enroll, "ss_iris_enroll");
    sym(ss_iris_post_enroll, "ss_iris_post_enroll");
    sym(ss_iris_get_auth_id, "ss_iris_get_auth_id");
    sym(ss_iris_cancel, "ss_iris_cancel");
    sym(ss_iris_remove, "ss_iris_remove");
    sym(ss_iris_set_active_group, "ss_iris_set_active_group");
    sym(ss_iris_authenticate, "ss_iris_authenticate");
    sym(ss_iris_request, "ss_iris_request");
    sym(ss_iris_processIRImage, "ss_iris_processIRImage");
    sym(ss_iris_processFrontImage, "ss_iris_processFrontImage");
    sym(ss_iris_sendIRProperty, "ss_iris_sendIRProperty");
    return ok;
}

}  // namespace aidl::android::hardware::biometrics::face
