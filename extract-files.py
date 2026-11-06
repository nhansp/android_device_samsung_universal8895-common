#!/usr/bin/env -S PYTHONPATH=../../../tools/extract-utils python3
#
# SPDX-FileCopyrightText: The LineageOS Project
# SPDX-License-Identifier: Apache-2.0
#

from extract_utils.fixups_blob import (
    blob_fixup,
    blob_fixups_user_type,
)
from extract_utils.fixups_lib import (
    lib_fixups,
    lib_fixups_user_type,
)
from extract_utils.main import (
    ExtractUtils,
    ExtractUtilsModule,
)

namespace_imports = [
    'device/samsung/universal8895-common',
    'hardware/samsung',
    'vendor/samsung/universal8895-common',
]

lib_fixups: lib_fixups_user_type = {
    **lib_fixups,
}

blob_fixups: blob_fixups_user_type = {
    'vendor/bin/hw/gpsd': blob_fixup()
        .binary_regex_replace(b'SSLv3_client_method', b'SSLv23_method\x00\x00\x00\x00\x00\x00'),
    (
        'vendor/lib64/hw/android.hardware.gnss@1.1-impl.so',
        'vendor/lib64/hw/gps.default.so',
    ): blob_fixup()
        .fix_soname(),
    'vendor/lib/libExynosOMX_Core.so': blob_fixup()
        .binary_regex_replace(b'/system/lib/omx/', b'/vendor/lib/omx/'),
    'vendor/lib64/libsec-ril.so': blob_fixup()
        .sig_replace('80 0E 40 F9 E1 03 16 AA 82 0C 80 52 E3 03 15 AA',
            '80 0E 40 F9 E1 03 16 AA 82 0C 80 52 08 00 80 D2'),
    (
        'vendor/lib/libexynosdisplay.so',
        'vendor/lib64/libexynosdisplay.so',
        'vendor/lib64/sensors.sensorhub.so',
    ): blob_fixup()
        .add_needed('libutils-v32.so')
        .binary_regex_replace(b'_ZN7android6Thread3runEPKci', b'_ZN7utils326Thread3runEPKci'),
}  # fmt: skip

module = ExtractUtilsModule(
    'universal8895-common',
    'samsung',
    blob_fixups=blob_fixups,
    lib_fixups=lib_fixups,
    namespace_imports=namespace_imports,
)

if __name__ == '__main__':
    utils = ExtractUtils.device(module)
    utils.run()
