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
    # __aeabi_idiv, __aeabi_ldivmod, __aeabi_f2lz and __aeabi_l2f are not exported by bionic
    'vendor/lib/lib_SamsungRec_06006.so': blob_fixup()
        .add_needed('libcompiler_rt-samsung.so'),
    # BT SCO shim, no SoundTrigger HAL; the Samsung libaudioroute and libtinyalsa are renamed away
    # from the AOSP ones
    # patchelf 0.17.2 and 0.18 put the program headers inside the text LOAD (vaddr 0x1a034); 0.9
    # maps them in a new LOAD
    'vendor/lib/hw/audio.primary.exynos8895.so': blob_fixup()
        .patchelf_version('0_9')
        .add_needed('libaudioparams_shim.so')
        .binary_regex_replace(b'str_parms_get_str', b'str_parms_get_mod')
        .remove_needed('libaudio_soundtrigger.so')
        .replace_needed('libaudioroute.so', 'libaudioroute-samsung.so')
        .replace_needed('libtinyalsa.so', 'libtinyalsa-samsung.so'),
    'vendor/lib/libaboxpcmdump.so': blob_fixup()
        .replace_needed('libaudioroute.so', 'libaudioroute-samsung.so')
        .replace_needed('libtinyalsa.so', 'libtinyalsa-samsung.so'),
    'vendor/lib/libaudioroute-samsung.so': blob_fixup()
        .replace_needed('libtinyalsa.so', 'libtinyalsa-samsung.so'),
    (
        'vendor/lib/libaudioroute-samsung.so',
        'vendor/lib/libtinyalsa-samsung.so',
        'vendor/lib64/libtinyalsa-samsung.so',
        'vendor/lib/libcompiler_rt-samsung.so',
    ): blob_fixup()
        .fix_soname(),
    (
        'vendor/lib/egl/libGLES_mali.so',
        'vendor/lib64/egl/libGLES_mali.so',
    ): blob_fixup()
        .clear_symbol_version('AHardwareBuffer_acquire')
        .clear_symbol_version('AHardwareBuffer_allocate')
        .clear_symbol_version('AHardwareBuffer_describe')
        .clear_symbol_version('AHardwareBuffer_getNativeHandle')
        .clear_symbol_version('AHardwareBuffer_release'),
    # Only libc, libc++, liblog and libIrisTlc are used; the rest are A9 framework libraries
    'vendor/lib64/libIrisService.so': blob_fixup()
        .remove_needed('libbinder.so')
        .remove_needed('libexif.so')
        .remove_needed('libexpat.so')
        .remove_needed('libgui.so')
        .remove_needed('libsavscmn.so')
        .remove_needed('libstagefright_foundation.so')
        .remove_needed('libui.so')
        .remove_needed('libutils.so'),
    'vendor/lib64/libIrisTlc.so': blob_fixup()
        .remove_needed('libutils.so'),
    # Neither is used; libFaceService needs it by file name, not by the stock soname
    'vendor/lib64/libsecfr_engine.so': blob_fixup()
        .remove_needed('libcutils.so')
        .remove_needed('libutils.so')
        .fix_soname(),
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
    # sensors.bio.so dlopens it as libflicker.so; its DT_SONAME is libflicker_sec.so
    'vendor/lib64/libflicker.so': blob_fixup()
        .fix_soname(),
    (
        'vendor/lib64/libkeymaster_helper.so',
        'vendor/lib64/libkeymaster_mdfpp.so',
    ): blob_fixup()
        .replace_needed('libcrypto.so', 'libcrypto-v28.so'),
    'vendor/lib/libExynosOMX_Core.so': blob_fixup()
        .binary_regex_replace(b'/system/lib/omx/', b'/vendor/lib/omx/'),
    # dlopened by the libsec-ril shim (shims/libsec-ril), which rild loads in its place
    'vendor/lib64/libsec-ril-impl.so': blob_fixup()
        .fix_soname(),
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
