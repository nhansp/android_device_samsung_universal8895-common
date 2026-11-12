#
# SPDX-FileCopyrightText: The LineageOS Project
# SPDX-License-Identifier: Apache-2.0
#

COMMON_PATH := device/samsung/universal8895-common

# Architecture
# The big cluster is Exynos M2, which clang no longer accepts as -mcpu
# (build/soong/cc/config/arm64_device.go:98-99 would pass -mcpu=exynos-m2);
# tune for the Cortex-A53 little cluster, which also links with the A53
# erratum 843419 workaround this SoC needs.
TARGET_ARCH := arm64
TARGET_ARCH_VARIANT := armv8-a
TARGET_CPU_ABI := arm64-v8a
TARGET_CPU_VARIANT := cortex-a53

TARGET_2ND_ARCH := arm
TARGET_2ND_ARCH_VARIANT := armv8-a
TARGET_2ND_CPU_ABI := armeabi-v7a
TARGET_2ND_CPU_ABI2 := armeabi
TARGET_2ND_CPU_VARIANT := cortex-a53

# Bluetooth
BOARD_HAVE_BLUETOOTH := true
BOARD_HAVE_BLUETOOTH_BCM := true

# Boot image
AB_OTA_UPDATER := false

BOARD_CUSTOM_BOOTIMG := true
BOARD_CUSTOM_BOOTIMG_MK := $(COMMON_PATH)/mkbootimg.mk
BOARD_KERNEL_BASE := 0x10000000
BOARD_KERNEL_PAGESIZE := 2048
BOARD_MKBOOTIMG_ARGS := \
    --kernel_offset 0x00008000 \
    --ramdisk_offset 0x01000000 \
    --second_offset 0x00f00000 \
    --tags_offset 0x00000100
BOARD_DTBH_MKBOOTIMG_ARGS := \
    --dtbh_chip 0x22bf \
    --dtbh_platform 0x50a6 \
    --dtbh_subtype 0x217584da \
    --dtbh_hw_rev $(BOARD_DTBH_HW_REV) \
    --dtbh_hw_rev_end $(BOARD_DTBH_HW_REV_END)
BOARD_RAMDISK_USE_XZ := true

# DTB
BOARD_INCLUDE_DTB_IN_BOOTIMG := true

# Filesystem
BOARD_CACHEIMAGE_FILE_SYSTEM_TYPE := ext4
BOARD_SYSTEMIMAGE_FILE_SYSTEM_TYPE := ext4
TARGET_COPY_OUT_VENDOR := system/vendor
TARGET_USERIMAGES_USE_EXT4 := true
TARGET_USERIMAGES_USE_F2FS := true

# Firmware
TARGET_NO_BOOTLOADER := true
TARGET_NO_RADIOIMAGE := true

# Kernel
BOARD_KERNEL_IMAGE_NAME := Image
TARGET_KERNEL_ARCH := arm64
TARGET_KERNEL_CLANG_VERSION := r584948b
TARGET_KERNEL_RUST_VERSION := 1.91.1
TARGET_KERNEL_SOURCE := kernel/samsung/universal8895
TARGET_KERNEL_CONFIG := \
    gki_defconfig \
    greatlte_base.config \
    greatlte.config \
    greatlte_l1.config \
    greatlte_l2.config \
    greatlte_l3.config \
    greatlte_l4.config \
    greatlte_l4b.config \
    greatlte_l5.config \
    greatlte_l6com.config \
    greatlte_l6t.config \
    greatlte_l6a.config \
    greatlte_l6f.config \
    greatlte_l6d.config \
    greatlte_l6e.config \
    greatlte_l6c.config \
    greatlte_l6b.config \
    greatlte_l6g.config \
    greatlte_l6h.config \
    greatlte_l6i.config \
    greatlte_l6k.config \
    greatlte_l6l.config \
    greatlte_l6m.config \
    greatlte_l2b.config \
    greatlte_l7.config \
    greatlte_release.config

# Partitions
# Block counts of the GREATLTE_KOR_SINGLE PIT x 4096. USERDATA has no fixed
# size (it takes the rest of LUN 0).
BOARD_BOOTIMAGE_PARTITION_SIZE := 41943040
BOARD_CACHEIMAGE_PARTITION_SIZE := 314572800
BOARD_RECOVERYIMAGE_PARTITION_SIZE := 48234496
BOARD_SYSTEMIMAGE_PARTITION_SIZE := 4508876800
BOARD_FLASH_BLOCK_SIZE := 4096
BOARD_ROOT_EXTRA_FOLDERS := cpefs efs

# A17 keeps the aconfig flag storage, apex sessions and vold state in /metadata: the empty OMR
# partition serves as one (fstab).
BOARD_USES_METADATA_PARTITION := true

# Platform
BOARD_VENDOR := samsung
TARGET_BOARD_PLATFORM := exynos5
TARGET_BOOTLOADER_BOARD_NAME := universal8895
TARGET_SOC := exynos8895

# Properties
TARGET_SYSTEM_PROP += $(COMMON_PATH)/system.prop
TARGET_VENDOR_PROP += $(COMMON_PATH)/vendor.prop

# Recovery
BOARD_HAS_DOWNLOAD_MODE := true
BOARD_USES_FULL_RECOVERY_IMAGE := true
TARGET_RECOVERY_FSTAB := $(COMMON_PATH)/init/fstab.samsungexynos8895
TARGET_RECOVERY_PIXEL_FORMAT := RGBX_8888

# Security
VENDOR_SECURITY_PATCH := 2021-10-01

# RIL
ENABLE_VENDOR_RIL_SERVICE := true

# SELinux
include device/lineage/sepolicy/libion/sepolicy.mk
include device/lineage/sepolicy/libperfmgr/sepolicy.mk
include device/lineage/sepolicy/exynos/sepolicy.mk
include device/samsung_slsi/sepolicy/sepolicy.mk
BOARD_VENDOR_SEPOLICY_DIRS += device/samsung_slsi/sepolicy/tee/mobicore/common
BOARD_VENDOR_SEPOLICY_DIRS += $(COMMON_PATH)/sepolicy/vendor

# VINTF
DEVICE_MANIFEST_FILE := $(COMMON_PATH)/vintf/manifest.xml
DEVICE_MATRIX_FILE := $(COMMON_PATH)/vintf/compatibility_matrix.xml
DEVICE_FRAMEWORK_COMPATIBILITY_MATRIX_FILE += \
    $(COMMON_PATH)/vintf/framework_compatibility_matrix.xml \
    hardware/samsung/vintf/samsung_framework_compatibility_matrix.xml

# Wi-Fi
BOARD_WLAN_DEVICE := bcmdhd
BOARD_HOSTAPD_DRIVER := NL80211
BOARD_HOSTAPD_PRIVATE_LIB := lib_driver_cmd_$(BOARD_WLAN_DEVICE)
BOARD_WPA_SUPPLICANT_DRIVER := NL80211
BOARD_WPA_SUPPLICANT_PRIVATE_LIB := lib_driver_cmd_$(BOARD_WLAN_DEVICE)
# The dhd driver appends the chip revision (_b0, _b2) to the path; stock ships
# one STA image per revision and uses it for SoftAP as well.
WIFI_DRIVER_FW_PATH_AP := "/vendor/etc/wifi/bcmdhd_sta.bin"
WIFI_DRIVER_FW_PATH_PARAM := "/sys/module/dhd/parameters/firmware_path"
WIFI_DRIVER_FW_PATH_STA := "/vendor/etc/wifi/bcmdhd_sta.bin"
WIFI_HIDL_UNIFIED_SUPPLICANT_SERVICE_RC_ENTRY := true
WPA_SUPPLICANT_VERSION := VER_0_8_X

# Proprietary files
include vendor/samsung/universal8895-common/BoardConfigVendor.mk
