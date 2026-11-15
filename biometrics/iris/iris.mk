#
# SPDX-FileCopyrightText: The LineageOS Project
# SPDX-License-Identifier: Apache-2.0
#

# Iris unlock, served to the framework as the face sensor. The iris trustlets and the sepolicy
# ship with the common tree; everything else is here.

IRIS_PATH := device/samsung/universal8895-common/biometrics/iris

# The face HAL (APEX) and the iris camera host it talks to
PRODUCT_PACKAGES += \
    com.samsung.android.biometrics.iris.camera \
    com.samsung.android.biometrics.iris.helper

# The APEX loads the stock libIrisService.so from /vendor
PRODUCT_VENDOR_LINKER_CONFIG_FRAGMENTS += \
    $(IRIS_PATH)/linker.config.json

# The iris card, enrollment and their overlays
PRODUCT_PACKAGES += \
    IrisPreview \
    SettingsOverlayVendorUniversal8895 \
    SystemUIOverlayVendorUniversal8895

# Permissions
PRODUCT_PACKAGES += \
    android.hardware.biometrics.face.prebuilt.xml
