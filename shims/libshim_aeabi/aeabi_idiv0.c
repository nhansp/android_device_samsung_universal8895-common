/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include <signal.h>

/* Stock libc exported __aeabi_idiv0, libcompiler_rt does not; it raises SIGFPE */
int __aeabi_idiv0(int return_value) {
    raise(SIGFPE);
    return return_value;
}
