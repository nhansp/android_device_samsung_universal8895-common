#
# SPDX-FileCopyrightText: The LineageOS Project
# SPDX-License-Identifier: Apache-2.0
#

# The super partition is on SYSTEM and HIDDEN, and the recovery finds it through
# androidboot.super_partition in its kernel command line. A recovery from before dynamic
# partitions looks for a partition named super instead and would fail halfway through the
# install, so refuse to start.
SUPER_PARTITION_CHECK = (
    'getprop("ro.boot.super_partition") == "SYSTEM" || '
    'abort("This package needs a recovery with dynamic partitions. '
    'Flash the recovery image from this build first.");'
)


def FullOTA_InstallBegin(info):
  info.script.AppendExtra(SUPER_PARTITION_CHECK)


def IncrementalOTA_InstallBegin(info):
  info.script.AppendExtra(SUPER_PARTITION_CHECK)
