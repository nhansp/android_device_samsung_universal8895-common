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
 * android::Fence as libexynoscamera3 expects it: refcount at offset 0, descriptor at offset 4.
 * The HAL manages both inline and only imports the constructor and wait(), which are renamed to
 * this namespace at extraction because libui now has a vtable in front and uses fdsan.
 */
namespace libui28 {

class Fence : public android::LightRefBase<Fence> {
  public:
    explicit Fence(int fence_fd);
    android::status_t wait(int timeout);

  private:
    int fence_fd_;
};

static_assert(sizeof(Fence) == 8);

Fence::Fence(int fence_fd) : fence_fd_(fence_fd) {}

android::status_t Fence::wait(int timeout) {
    ATRACE_CALL();
    if (fence_fd_ == -1) {
        return android::NO_ERROR;
    }
    int err = sync_wait(fence_fd_, timeout);
    return err < 0 ? -errno : android::NO_ERROR;
}

}  // namespace libui28
