/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include <signal.h>

/* Stock bionic/libc++ __aeabi_idiv0: raise SIGFPE on division by zero. */
int __aeabi_idiv0(int return_value) {
    raise(SIGFPE);
    return return_value;
}
