/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

package vendor.lineage.biometrics.iris;

import android.view.Surface;
import vendor.lineage.biometrics.iris.IIrisPreviewListener;

/**
 * The iris preview outside enrollment: the lockscreen and BiometricPrompt card. During
 * enrollment the face HAL draws into the surface from FaceEnrollOptions instead.
 */
@VintfStability
interface IIrisPreview {
    /**
     * The IR preview of every capture (240x135 grayscale) is drawn into this surface until it is
     * replaced or cleared with null.
     */
    void setPreviewSurface(in @nullable Surface surface);

    /** Receives the guidance of every capture until it is replaced or cleared with null. */
    void setListener(in @nullable IIrisPreviewListener listener);
}
