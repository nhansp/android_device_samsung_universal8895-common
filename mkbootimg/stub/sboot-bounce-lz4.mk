#
# SPDX-FileCopyrightText: The LineageOS Project
# SPDX-License-Identifier: Apache-2.0
#

UNIVERSAL8895_BOUNCE_SRC := device/samsung/universal8895-common/mkbootimg/stub
UNIVERSAL8895_BOUNCE_OUT := $(call intermediates-dir-for,PACKAGING,sboot-bounce-lz4)
UNIVERSAL8895_BOUNCE_STUB := $(UNIVERSAL8895_BOUNCE_OUT)/sboot-bounce-lz4.bin

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

UNIVERSAL8895_BOUNCE_CC = $(CLANG) --target=aarch64-linux-gnu $(UNIVERSAL8895_BOUNCE_CFLAGS) \
    -I$(UNIVERSAL8895_BOUNCE_SRC) -c -o $@ $<

$(UNIVERSAL8895_BOUNCE_OUT)/sboot-bounce-lz4.c.o: \
        $(UNIVERSAL8895_BOUNCE_SRC)/sboot-bounce-lz4.c $(UNIVERSAL8895_BOUNCE_HEADERS) $(CLANG)
	@mkdir -p $(dir $@)
	$(UNIVERSAL8895_BOUNCE_CC)

$(UNIVERSAL8895_BOUNCE_OUT)/lz4dec.c.o: \
        $(UNIVERSAL8895_BOUNCE_SRC)/lz4dec.c $(UNIVERSAL8895_BOUNCE_HEADERS) $(CLANG)
	@mkdir -p $(dir $@)
	$(UNIVERSAL8895_BOUNCE_CC)

$(UNIVERSAL8895_BOUNCE_STUB): $(UNIVERSAL8895_BOUNCE_OBJS) $(UNIVERSAL8895_BOUNCE_LDS) $(LLVM_OBJCOPY)
	$(LLVM_PREBUILTS_PATH)/ld.lld --image-base=0 -T $(UNIVERSAL8895_BOUNCE_LDS) \
	    -o $(basename $@).elf $(UNIVERSAL8895_BOUNCE_OBJS)
	$(LLVM_OBJCOPY) -O binary $(basename $@).elf $@
