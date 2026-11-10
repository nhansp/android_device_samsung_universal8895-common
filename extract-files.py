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
    'vendor/lib/lib_SamsungRec_06006.so': blob_fixup()
        .add_needed('libcompiler_rt-samsung.so'),
    'vendor/lib/hw/audio.primary.universal8895.so': blob_fixup()
        .add_needed('libprocessgroup.so')
        .replace_needed('libaudioroute.so', 'libaudioroute-samsung.so')
        .replace_needed('libtinyalsa.so', 'libtinyalsa-samsung.so'),
    (
        'vendor/lib/hw/sound_trigger.primary.universal8895.so',
        'vendor/lib/libaboxpcmdump.so',
        'vendor/lib/libaudio_soundtrigger.so',
        'vendor/lib/libaudioroute-samsung.so',
    ): blob_fixup()
        .replace_needed('libtinyalsa.so', 'libtinyalsa-samsung.so'),
    (
        'vendor/lib/libaudioroute-samsung.so',
        'vendor/lib/libtinyalsa-samsung.so',
        'vendor/lib/libcompiler_rt-samsung.so',
    ): blob_fixup()
        .fix_soname(),
    'vendor/lib/libaudioroute-samsung.so': blob_fixup()
        .binary_regex_replace(b'/system/etc/mixer_paths.xml', b'/vendor/etc/mixer_paths.xml'),
    'vendor/etc/mixer_paths.xml': blob_fixup()
        .regex_replace('/system/etc/mixer_gains.xml', '/vendor/etc/mixer_gains.xml'),
    (
        'vendor/lib/egl/libGLES_mali.so',
        'vendor/lib64/egl/libGLES_mali.so',
    ): blob_fixup()
        .clear_symbol_version('AHardwareBuffer_acquire')
        .clear_symbol_version('AHardwareBuffer_allocate')
        .clear_symbol_version('AHardwareBuffer_describe')
        .clear_symbol_version('AHardwareBuffer_getNativeHandle')
        .clear_symbol_version('AHardwareBuffer_release'),
    'vendor/bin/hw/gpsd': blob_fixup()
        .binary_regex_replace(b'SSLv3_client_method', b'SSLv23_method\x00\x00\x00\x00\x00\x00'),
    (
        'vendor/lib64/hw/android.hardware.gnss@1.1-impl.so',
        'vendor/lib64/hw/gps.default.so',
    ): blob_fixup()
        .fix_soname(),
    (
        'vendor/lib/libskeymaster3device.so',
        'vendor/lib64/libskeymaster3device.so',
    ): blob_fixup()
        .add_needed('libshim_keymaster_ctx.so'),
    'vendor/lib64/libcrypto-v28.so': blob_fixup()
        .fix_soname(),
    (
        'vendor/lib64/libkeymaster_helper.so',
        'vendor/lib64/libkeymaster_mdfpp.so',
    ): blob_fixup()
        .replace_needed('libcrypto.so', 'libcrypto-v28.so'),
    'vendor/lib/libExynosOMX_Core.so': blob_fixup()
        .binary_regex_replace(b'/system/lib/omx/', b'/vendor/lib/omx/'),
    'vendor/lib64/libsec-ril.so': blob_fixup()
        .remove_needed('libhidltransport.so')
        .remove_needed('libhwbinder.so'),
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
