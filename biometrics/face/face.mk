#
# SPDX-FileCopyrightText: The LineageOS Project
# SPDX-License-Identifier: Apache-2.0
#

# Face unlock with the stock face library and trustlet, served to the framework as the face
# sensor. The face trustlet and the sepolicy ship with the common tree; everything else is here.

FACE_PATH := device/samsung/universal8895-common/biometrics/face

# The face HAL (APEX) and the camera host it talks to
PRODUCT_PACKAGES += \
    com.android.hardware.biometrics.face.samsung \
    vendor.lineage.biometrics.camera.samsung

# The APEX loads the stock libFaceService.so from /vendor
PRODUCT_VENDOR_LINKER_CONFIG_FRAGMENTS += \
    $(FACE_PATH)/linker.config.json

# Enrollment is Settings' own; the HAL draws its preview
PRODUCT_PACKAGES += \
    SettingsOverlayFace \
    SystemUIOverlayFace

# Permissions
PRODUCT_PACKAGES += \
    android.hardware.biometrics.face.prebuilt.xml
