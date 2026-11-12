#
# SPDX-FileCopyrightText: The LineageOS Project
# SPDX-License-Identifier: Apache-2.0
#

# /omr is no longer a root mount point (it hosts /metadata).
$(call add-clean-step, rm -rf $(PRODUCT_OUT)/root/omr)

# The OMX service no longer gets an extra group from a vendor init override.
$(call add-clean-step, rm -f $(PRODUCT_OUT)/$(TARGET_COPY_OUT_VENDOR)/etc/init/zz-media-omx.rc)
