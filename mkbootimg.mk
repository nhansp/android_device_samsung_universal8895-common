#
# SPDX-FileCopyrightText: The LineageOS Project
# SPDX-License-Identifier: Apache-2.0
#
# BOARD_CUSTOM_BOOTIMG_MK: S-Boot loads boot and recovery images with a header v0 and a DTBH table after the
# ramdisk, which AOSP mkbootimg cannot write (see mkbootimg/mkdtbhbootimg.py).

MKDTBHBOOTIMG := $(HOST_OUT_EXECUTABLES)/mkdtbhbootimg$(HOST_EXECUTABLE_SUFFIX)

# build/make/core/Makefile adds the kernel dependency of the boot image only when it builds the image itself.
$(foreach b,$(INSTALLED_BOOTIMAGE_TARGET),\
    $(eval $(call add-dependency,$(b),$(call bootimage-to-kernel,$(b)))))

$(INSTALLED_BOOTIMAGE_TARGET): $(MKDTBHBOOTIMG) $(INTERNAL_BOOTIMAGE_FILES)
	$(call pretty,"Target boot image: $@")
	$(MKDTBHBOOTIMG) --kernel $(call bootimage-to-kernel,$@) $(INTERNAL_BOOTIMAGE_ARGS) \
	    $(BOARD_MKBOOTIMG_ARGS) --output $@
	$(call assert-max-image-size,$@,$(call get-bootimage-partition-size,$@,boot))

$(INSTALLED_RECOVERYIMAGE_TARGET): $(MKDTBHBOOTIMG) $(recoveryimage-deps)
	$(call pretty,"Target recovery image: $@")
	$(MKDTBHBOOTIMG) $(INTERNAL_RECOVERYIMAGE_ARGS) $(BOARD_MKBOOTIMG_ARGS) --output $@
	$(call assert-max-image-size,$@,$(BOARD_RECOVERYIMAGE_PARTITION_SIZE))
