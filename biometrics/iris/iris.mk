#
# SPDX-FileCopyrightText: The LineageOS Project
# SPDX-License-Identifier: Apache-2.0
#

# Iris unlock, served to the framework as the face sensor. The iris trustlets and the sepolicy
# ship with the common tree; everything else is here.

IRIS_PATH := device/samsung/universal8895-common/biometrics/iris

# The iris HAL (APEX) and the biometrics camera host it talks to
PRODUCT_PACKAGES += \
    com.android.hardware.biometrics.iris.samsung \
    vendor.lineage.biometrics.camera.samsung

# The APEX loads the stock libIrisService.so from /vendor
PRODUCT_VENDOR_LINKER_CONFIG_FRAGMENTS += \
    $(IRIS_PATH)/linker.config.json

# The iris card, enrollment and their overlays
PRODUCT_PACKAGES += \
    IrisPreview \
    SettingsOverlayIris \
    SystemUIOverlayIris

# Permissions
PRODUCT_PACKAGES += \
    android.hardware.biometrics.face.prebuilt.xml

# The face slot is taken: keep PixelOS from adding ParanoidSense as a second face sensor
# (vendor/custom/config/common.mk)
TARGET_PROVIDES_FACE_UNLOCK_HAL := true
