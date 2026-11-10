#
# SPDX-FileCopyrightText: The LineageOS Project
# SPDX-License-Identifier: Apache-2.0
#

# /omr is no longer a root mount point (it hosts /metadata).
$(call add-clean-step, rm -rf $(PRODUCT_OUT)/root/omr)
