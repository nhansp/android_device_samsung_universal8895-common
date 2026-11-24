#
# SPDX-FileCopyrightText: The LineageOS Project
# SPDX-License-Identifier: Apache-2.0
#

# /omr is no longer a root mount point
$(call add-clean-step, rm -rf $(PRODUCT_OUT)/root/omr)

# OMX service group override dropped
$(call add-clean-step, rm -f $(PRODUCT_OUT)/$(TARGET_COPY_OUT_VENDOR)/etc/init/zz-media-omx.rc)

# keymaster service now defined in init.samsungexynos8895.rc
$(call add-clean-step, rm -f $(PRODUCT_OUT)/$(TARGET_COPY_OUT_VENDOR)/etc/init/keymaster@3.0-override.rc)
