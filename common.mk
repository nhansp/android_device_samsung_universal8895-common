#
# SPDX-FileCopyrightText: The LineageOS Project
# SPDX-License-Identifier: Apache-2.0
#

COMMON_PATH := device/samsung/universal8895-common

# Dalvik heap: 6 GB of RAM
$(call inherit-product, frameworks/native/build/phone-xhdpi-6144-dalvik-heap.mk)

# Emulated storage with project quotas and casefolding
$(call inherit-product, $(SRC_TARGET_DIR)/product/emulated_storage.mk)

# Audio
# The primary HAL (audio.primary.exynos8895, codeberg universal8895 lineage-23.2) exists only as a
# 32-bit module, so the HIDL audio service stays on its default prefer32 build.
PRODUCT_PACKAGES += \
    android.hardware.audio.effect@7.0-impl \
    android.hardware.audio.service \
    android.hardware.audio@7.1-impl \
    android.hardware.bluetooth.audio-impl \
    audio.bluetooth.default \
    audio.r_submix.default \
    audio.usbv2.default \
    libaudioparams_shim

PRODUCT_COPY_FILES += \
    $(COMMON_PATH)/configs/audio/audio_policy_configuration.xml:$(TARGET_COPY_OUT_VENDOR)/etc/audio_policy_configuration.xml \
    $(COMMON_PATH)/configs/audio/mixer_paths.xml:$(TARGET_COPY_OUT_VENDOR)/etc/mixer_paths.xml \
    $(COMMON_PATH)/configs/audio/audio_effects.xml:$(TARGET_COPY_OUT_VENDOR)/etc/audio_effects.xml \
    frameworks/av/services/audiopolicy/config/a2dp_in_audio_policy_configuration_7_0.xml:$(TARGET_COPY_OUT_VENDOR)/etc/a2dp_in_audio_policy_configuration_7_0.xml \
    frameworks/av/services/audiopolicy/config/audio_policy_volumes.xml:$(TARGET_COPY_OUT_VENDOR)/etc/audio_policy_volumes.xml \
    frameworks/av/services/audiopolicy/config/bluetooth_audio_policy_configuration_7_0.xml:$(TARGET_COPY_OUT_VENDOR)/etc/bluetooth_audio_policy_configuration_7_0.xml \
    frameworks/av/services/audiopolicy/config/default_volume_tables.xml:$(TARGET_COPY_OUT_VENDOR)/etc/default_volume_tables.xml \
    frameworks/av/services/audiopolicy/config/r_submix_audio_policy_configuration.xml:$(TARGET_COPY_OUT_VENDOR)/etc/r_submix_audio_policy_configuration.xml \
    frameworks/av/services/audiopolicy/config/usb_audio_policy_configuration.xml:$(TARGET_COPY_OUT_VENDOR)/etc/usb_audio_policy_configuration.xml

# Bluetooth
PRODUCT_PACKAGES += \
    android.hardware.bluetooth@1.0-impl \
    android.hardware.bluetooth@1.0-service

# Boot image
PRODUCT_HOST_PACKAGES += \
    mkdtbhbootimg

# Camera
PRODUCT_PACKAGES += \
    android.hardware.camera.provider-service_32.samsung \
    camera.universal8895_wrapper \
    libcamera_compat \
    libexynoscamera_shim \
    libion

$(call soong_config_set_bool,libion,legacy_impl,true)
$(call soong_config_set_bool,samsungCameraVars,usage_64bit,true)

# DRM
PRODUCT_PACKAGES += \
    android.hardware.drm-service.clearkey

# Fastbootd
PRODUCT_PACKAGES += \
    fastbootd

# Fingerprint
PRODUCT_PACKAGES += \
    android.hardware.biometrics.fingerprint-service.samsung

# Face unlock: face (default) or iris; only one can serve the face sensor
UNIVERSAL8895_FACE_UNLOCK ?= face
ifeq ($(UNIVERSAL8895_FACE_UNLOCK),iris)
$(call inherit-product, $(COMMON_PATH)/biometrics/iris/iris.mk)
else ifeq ($(UNIVERSAL8895_FACE_UNLOCK),face)
$(call inherit-product, $(COMMON_PATH)/biometrics/face/face.mk)
else
$(error UNIVERSAL8895_FACE_UNLOCK must be face or iris, not $(UNIVERSAL8895_FACE_UNLOCK))
endif

# Gatekeeper
PRODUCT_PACKAGES += \
    android.hardware.gatekeeper-service.samsung

# GNSS
PRODUCT_PACKAGES += \
    android.hardware.gnss@1.1-service.samsung

# Graphics
PRODUCT_PACKAGES += \
    android.hardware.graphics.allocator@2.0-impl \
    android.hardware.graphics.allocator@2.0-service \
    android.hardware.graphics.composer@2.1-service.exynos8895 \
    android.hardware.graphics.mapper@2.0-impl

$(call soong_config_set_bool,libui,legacy_gralloc,true)

# Health
PRODUCT_PACKAGES += \
    android.hardware.health-service.samsung \
    android.hardware.health-service.samsung-recovery

# IMS
$(call inherit-product, hardware/lineage/generic-ims/ims.mk)

# Init
PRODUCT_PACKAGES += \
    fstab.samsungexynos8895 \
    fstab.samsungexynos8895.ramdisk \
    init.recovery.samsungexynos8895.rc \
    init.samsungexynos8895.rc \
    init.samsungexynos8895.usb.rc \
    ueventd.samsungexynos8895.rc

# Kernel
PRODUCT_OTA_ENFORCE_VINTF_KERNEL_REQUIREMENTS := false

# Shipping API level
PRODUCT_SHIPPING_API_LEVEL := 25
PRODUCT_SET_DEBUGFS_RESTRICTIONS := true

# Keymaster
# TEE-backed by default. TARGET_KEYMASTER_TEE := false falls back to the AOSP software keymaster;
# switching either way invalidates every key keystore holds.
TARGET_KEYMASTER_TEE ?= true
ifeq ($(TARGET_KEYMASTER_TEE),true)
PRODUCT_PACKAGES += \
    android.hardware.keymaster@3.0-impl.exynos8895 \
    keymaster@3.0-override.rc
PRODUCT_VENDOR_PROPERTIES += \
    ro.hardware.keystore=mdfpp
else
PRODUCT_PACKAGES += \
    android.hardware.keymaster@3.0-impl
endif
PRODUCT_PACKAGES += \
    android.hardware.keymaster@3.0-service

# Lights
PRODUCT_PACKAGES += \
    android.hardware.light-service.samsung

# LiveDisplay
PRODUCT_PACKAGES += \
    vendor.lineage.livedisplay-service.samsung-exynos

# Media
PRODUCT_PACKAGES += \
    android.hardware.media.omx@1.0-service

PRODUCT_COPY_FILES += \
    $(COMMON_PATH)/configs/media/media_codecs.xml:$(TARGET_COPY_OUT_VENDOR)/etc/media_codecs.xml \
    $(COMMON_PATH)/configs/media/media_codecs_performance.xml:$(TARGET_COPY_OUT_VENDOR)/etc/media_codecs_performance.xml \
    frameworks/av/media/libstagefright/data/media_codecs_google_audio.xml:$(TARGET_COPY_OUT_VENDOR)/etc/media_codecs_google_audio.xml \
    frameworks/av/media/libstagefright/data/media_codecs_google_telephony.xml:$(TARGET_COPY_OUT_VENDOR)/etc/media_codecs_google_telephony.xml \
    frameworks/av/media/libstagefright/data/media_codecs_google_video.xml:$(TARGET_COPY_OUT_VENDOR)/etc/media_codecs_google_video.xml \
    $(COMMON_PATH)/configs/media/media_profiles.xml:$(TARGET_COPY_OUT_VENDOR)/etc/media_profiles_V1_0.xml

# Memtrack
PRODUCT_PACKAGES += \
    android.hardware.memtrack-service.samsung-mali

# NFC
PRODUCT_PACKAGES += \
    android.hardware.nfc-service.sec \
    com.android.nfc_extras \
    nfc_nci_sec \
    Tag

# OpenCL
PRODUCT_COPY_FILES += \
    $(COMMON_PATH)/configs/public.libraries.txt:$(TARGET_COPY_OUT_VENDOR)/etc/public.libraries.txt

# Overlays
PRODUCT_PACKAGES += \
    CarrierSettingsOverlayCommon \
    FrameworkResOverlayCommon \
    LineageSDKOverlayCommon \
    SystemUIOverlayCommon \
    WifiOverlayCommon

PRODUCT_ENFORCE_RRO_TARGETS := *

# Partitions
PRODUCT_USE_DYNAMIC_PARTITIONS := true

# Permissions
PRODUCT_PACKAGES += \
    android.hardware.bluetooth.prebuilt.xml \
    android.hardware.bluetooth_le.prebuilt.xml \
    android.hardware.camera.flash-autofocus.prebuilt.xml \
    android.hardware.camera.front.prebuilt.xml \
    android.hardware.camera.full.prebuilt.xml \
    android.hardware.camera.raw.prebuilt.xml \
    android.hardware.fingerprint.prebuilt.xml \
    android.hardware.location.gps.prebuilt.xml \
    android.hardware.nfc.prebuilt.xml \
    android.hardware.nfc.hce.prebuilt.xml \
    android.hardware.nfc.hcef.prebuilt.xml \
    android.hardware.sensor.accelerometer.prebuilt.xml \
    android.hardware.sensor.barometer.prebuilt.xml \
    android.hardware.sensor.compass.prebuilt.xml \
    android.hardware.sensor.gyroscope.prebuilt.xml \
    android.hardware.sensor.hifi_sensors.prebuilt.xml \
    android.hardware.sensor.light.prebuilt.xml \
    android.hardware.sensor.proximity.prebuilt.xml \
    android.hardware.sensor.stepcounter.prebuilt.xml \
    android.hardware.sensor.stepdetector.prebuilt.xml \
    android.hardware.telephony.gsm.prebuilt.xml \
    android.hardware.usb.accessory.prebuilt.xml \
    android.hardware.usb.host.prebuilt.xml \
    android.hardware.vulkan.compute-0.prebuilt.xml \
    android.hardware.vulkan.level-0.prebuilt.xml \
    android.hardware.vulkan.version-1_0_3.prebuilt.xml \
    android.hardware.wifi.prebuilt.xml \
    android.hardware.wifi.direct.prebuilt.xml \
    android.software.ipsec_tunnels.prebuilt.xml \
    android.software.sip.voip.prebuilt.xml \
    com.nxp.mifare.prebuilt.xml

PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.opengles.aep.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.opengles.aep.xml \
    frameworks/native/data/etc/android.hardware.touchscreen.multitouch.jazzhand.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.touchscreen.multitouch.jazzhand.xml \
    frameworks/native/data/etc/android.software.freeform_window_management.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.software.freeform_window_management.xml \
    frameworks/native/data/etc/android.software.midi.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.software.midi.xml \
    frameworks/native/data/etc/com.android.nfc_extras.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/com.android.nfc_extras.xml

# Display and Dolby settings (hardware/samsung)
PRODUCT_PACKAGES += \
    AdvancedDisplay \
    AdvancedDisplayOverlayCommon \
    SamsungDAP

# Power: LineageOS's maintained libperfmgr service (hardware/lineage/interfaces), with PowerExt
PRODUCT_PACKAGES += \
    android.hardware.power-service.lineage-libperfmgr

PRODUCT_COPY_FILES += \
    $(COMMON_PATH)/configs/power/powerhint.json:$(TARGET_COPY_OUT_VENDOR)/etc/powerhint.json \
    $(COMMON_PATH)/configs/power/task_profiles.json:$(TARGET_COPY_OUT_VENDOR)/etc/task_profiles.json

# PowerShare
PRODUCT_PACKAGES += \
    vendor.lineage.powershare-service.samsung

# RIL
PRODUCT_PACKAGES += \
    libsec-ril \
    secril_config_svc

# Sensors
PRODUCT_PACKAGES += \
    android.hardware.sensors@1.0-impl.samsung \
    android.hardware.sensors@1.0-service

PRODUCT_COPY_FILES += \
    $(COMMON_PATH)/configs/sensors/hals.conf:$(TARGET_COPY_OUT_VENDOR)/etc/sensors/hals.conf

# Shims
PRODUCT_PACKAGES += \
    android.frameworks.schedulerservice@1.0 \
    libutils-v32

# Soong namespaces
PRODUCT_SOONG_NAMESPACES += \
    $(COMMON_PATH) \
    hardware/google/interfaces \
    hardware/google/pixel/power-libperfmgr \
    hardware/lineage/interfaces/power-libperfmgr \
    hardware/google/pixel/thermal \
    hardware/google/pixel/pixelstats \
    hardware/samsung

# Thermal
PRODUCT_PACKAGES += \
    android.hardware.thermal-service.pixel \
    pixelatoms-cpp \
    thermal_symlinks

PRODUCT_COPY_FILES += \
    $(COMMON_PATH)/configs/thermal/thermal_info_config.json:$(TARGET_COPY_OUT_VENDOR)/etc/thermal_info_config.json

# Touch
PRODUCT_PACKAGES += \
    vendor.lineage.touch-service.samsung

# USB
PRODUCT_PACKAGES += \
    android.hardware.usb-service.samsung \
    android.hardware.usb.gadget-service.samsung

$(call soong_config_set,samsungUsbGadgetVars,gadget_name,10c00000.usb)

# Vibrator
PRODUCT_PACKAGES += \
    android.hardware.vibrator-service.universal8895

# Wi-Fi
PRODUCT_PACKAGES += \
    android.hardware.wifi-service \
    hostapd \
    wpa_supplicant \
    wpa_supplicant.conf

# LiveDisplay
PRODUCT_COPY_FILES += \
    $(COMMON_PATH)/init/zz-livedisplay-ondemand.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/zz-livedisplay-ondemand.rc

# Input
PRODUCT_COPY_FILES += \
    $(COMMON_PATH)/configs/idc/sec_e-pen.idc:$(TARGET_COPY_OUT_VENDOR)/usr/idc/sec_e-pen.idc

# Display
PRODUCT_COPY_FILES += \
    $(COMMON_PATH)/configs/display/display_port_0.xml:$(TARGET_COPY_OUT_VENDOR)/etc/displayconfig/display_port_0.xml

# Proprietary files
$(call inherit-product, vendor/samsung/universal8895-common/universal8895-common-vendor.mk)
