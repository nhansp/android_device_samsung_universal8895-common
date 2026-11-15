#
# SPDX-FileCopyrightText: The LineageOS Project
# SPDX-License-Identifier: Apache-2.0
#
# The compressed bounce stub, built from source with the platform's clang. It is not a module: the boot
# and recovery image rules depend on UNIVERSAL8895_BOUNCE_STUB, so it is built for any goal that needs
# one of them.

UNIVERSAL8895_BOUNCE_SRC := device/samsung/universal8895-common/mkbootimg/stub
UNIVERSAL8895_BOUNCE_OUT := $(call intermediates-dir-for,PACKAGING,sboot-bounce-lz4)
UNIVERSAL8895_BOUNCE_STUB := $(UNIVERSAL8895_BOUNCE_OUT)/sboot-bounce-lz4.bin

# Freestanding code that runs with the MMU off at an unknown load address: no libc, no FP/SIMD (CPACR may
# trap it), no PAC/BTI, no jump tables or outlined atomics (rodata and helpers a flat binary would have to
# carry), no stack protector
UNIVERSAL8895_BOUNCE_CFLAGS := \
    -O2 -ffreestanding -fno-builtin -nostdlib -mgeneral-regs-only -fno-stack-protector \
    -fno-pic -fno-pie -fno-asynchronous-unwind-tables -fno-unwind-tables -mbranch-protection=none \
    -mno-outline-atomics -fno-jump-tables -Wall -Wextra -Werror

UNIVERSAL8895_BOUNCE_LDS := $(UNIVERSAL8895_BOUNCE_SRC)/sboot-bounce-lz4.ld
UNIVERSAL8895_BOUNCE_HEADERS := \
    $(UNIVERSAL8895_BOUNCE_SRC)/sboot-bounce-lz4.h \
    $(UNIVERSAL8895_BOUNCE_SRC)/lz4dec.h
UNIVERSAL8895_BOUNCE_OBJS := $(addprefix $(UNIVERSAL8895_BOUNCE_OUT)/, \
    sboot-bounce-lz4.S.o sboot-bounce-lz4.c.o lz4dec.c.o)

$(UNIVERSAL8895_BOUNCE_OUT)/sboot-bounce-lz4.S.o: \
        $(UNIVERSAL8895_BOUNCE_SRC)/sboot-bounce-lz4.S $(UNIVERSAL8895_BOUNCE_HEADERS) $(CLANG)
	@mkdir -p $(dir $@)
	$(CLANG) --target=aarch64-linux-gnu -I$(UNIVERSAL8895_BOUNCE_SRC) -c -o $@ $<

$(UNIVERSAL8895_BOUNCE_OUT)/%.c.o: \
        $(UNIVERSAL8895_BOUNCE_SRC)/%.c $(UNIVERSAL8895_BOUNCE_HEADERS) $(CLANG)
	@mkdir -p $(dir $@)
	$(CLANG) --target=aarch64-linux-gnu $(UNIVERSAL8895_BOUNCE_CFLAGS) -I$(UNIVERSAL8895_BOUNCE_SRC) \
	    -c -o $@ $<

$(UNIVERSAL8895_BOUNCE_STUB): $(UNIVERSAL8895_BOUNCE_OBJS) $(UNIVERSAL8895_BOUNCE_LDS) $(LLVM_OBJCOPY)
	$(LLVM_PREBUILTS_PATH)/ld.lld --image-base=0 -T $(UNIVERSAL8895_BOUNCE_LDS) \
	    -o $(basename $@).elf $(UNIVERSAL8895_BOUNCE_OBJS)
	$(LLVM_OBJCOPY) -O binary $(basename $@).elf $@
