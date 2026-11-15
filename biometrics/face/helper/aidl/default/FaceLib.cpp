/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include "FaceLib.h"

#include <dlfcn.h>

#include <type_traits>

#include <android-base/logging.h>

namespace aidl::android::hardware::biometrics::face {

bool FaceLib::load() {
    void* handle = dlopen("libFaceService.so", RTLD_NOW);
    if (!handle) {
        LOG(ERROR) << "Failed to load libFaceService.so: " << dlerror();
        return false;
    }

    bool ok = true;
    auto sym = [&](auto& fn, const char* name) {
        fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(handle, name));
        if (!fn) {
            LOG(ERROR) << "libFaceService.so has no " << name;
            ok = false;
        }
    };
    sym(ss_face_open, "ss_face_open");
    sym(ss_face_close, "ss_face_close");
    sym(ss_set_notify_callback, "ss_set_notify_callback");
    sym(ss_face_pre_enroll, "ss_face_pre_enroll");
    sym(ss_face_enroll, "ss_face_enroll");
    sym(ss_face_post_enroll, "ss_face_post_enroll");
    sym(ss_face_get_auth_id, "ss_face_get_auth_id");
    sym(ss_face_cancel, "ss_face_cancel");
    sym(ss_face_enumerate, "ss_face_enumerate");
    sym(ss_face_cancelEnumeration, "ss_face_cancelEnumeration");
    sym(ss_face_remove, "ss_face_remove");
    sym(ss_face_set_active_group, "ss_face_set_active_group");
    sym(ss_face_authenticate, "ss_face_authenticate");
    sym(ss_face_request, "ss_face_request");
    sym(ss_face_processFrontImage, "ss_face_processFrontImage");
    return ok;
}

}  // namespace aidl::android::hardware::biometrics::face
