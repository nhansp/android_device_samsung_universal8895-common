/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define ATRACE_TAG ATRACE_TAG_GRAPHICS

#include <errno.h>

#include <sync/sync.h>
#include <utils/Errors.h>
#include <utils/LightRefBase.h>
#include <utils/Trace.h>

/*
 * android::Fence as of Android 9, which libexynoscamera3 was built against:
 * the reference count at offset 0 and a plain descriptor at offset 4. The HAL
 * allocates the object, counts references and closes the descriptor inline,
 * and only calls the constructor and wait(). Its imports of those two are
 * renamed to this namespace at extraction, because libui now has a vtable in
 * front of both fields and hands the descriptor to fdsan.
 */
namespace libui28 {

class Fence : public android::LightRefBase<Fence> {
  public:
    explicit Fence(int fenceFd);
    android::status_t wait(int timeout);

  private:
    int mFenceFd;
};

static_assert(sizeof(Fence) == 8);

Fence::Fence(int fenceFd) : mFenceFd(fenceFd) {}

android::status_t Fence::wait(int timeout) {
    ATRACE_CALL();
    if (mFenceFd == -1) {
        return android::NO_ERROR;
    }
    int err = sync_wait(mFenceFd, timeout);
    return err < 0 ? -errno : android::NO_ERROR;
}

}  // namespace libui28
