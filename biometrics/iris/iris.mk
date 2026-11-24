#
# SPDX-FileCopyrightText: The LineageOS Project
# SPDX-License-Identifier: Apache-2.0
#

IRIS_PATH := device/samsung/universal8895-common/biometrics/iris

# HAL
PRODUCT_PACKAGES += \
    com.android.hardware.biometrics.iris.samsung \
    vendor.lineage.biometrics.camera.samsung

# Linker
PRODUCT_VENDOR_LINKER_CONFIG_FRAGMENTS += \
    $(IRIS_PATH)/linker.config.json

# Overlays
PRODUCT_PACKAGES += \
    IrisPreview \
    SettingsOverlayIris \
    SystemUIOverlayIris

# Permissions
PRODUCT_PACKAGES += \
    android.hardware.biometrics.face.prebuilt.xml
