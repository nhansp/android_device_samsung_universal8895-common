#
# SPDX-FileCopyrightText: The LineageOS Project
# SPDX-License-Identifier: Apache-2.0
#

FACE_PATH := device/samsung/universal8895-common/biometrics/face

# HAL
PRODUCT_PACKAGES += \
    com.android.hardware.biometrics.face.samsung \
    vendor.lineage.biometrics.camera.samsung

# Linker
PRODUCT_VENDOR_LINKER_CONFIG_FRAGMENTS += \
    $(FACE_PATH)/linker.config.json

# Overlays
PRODUCT_PACKAGES += \
    SettingsOverlayFace \
    SystemUIOverlayFace

# Permissions
PRODUCT_PACKAGES += \
    android.hardware.biometrics.face.prebuilt.xml
