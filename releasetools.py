#
# SPDX-FileCopyrightText: The LineageOS Project
# SPDX-License-Identifier: Apache-2.0
#

# A recovery without dynamic partitions looks for a partition named super instead of using
# androidboot.super_partition (SYSTEM and HIDDEN) and would fail halfway through the install
SUPER_PARTITION_CHECK = (
    'getprop("ro.boot.super_partition") == "SYSTEM" || '
    'abort("This package needs a recovery with dynamic partitions. '
    'Flash the recovery image from this build first.");'
)


def FullOTA_InstallBegin(info):
  info.script.AppendExtra(SUPER_PARTITION_CHECK)


def IncrementalOTA_InstallBegin(info):
  info.script.AppendExtra(SUPER_PARTITION_CHECK)
