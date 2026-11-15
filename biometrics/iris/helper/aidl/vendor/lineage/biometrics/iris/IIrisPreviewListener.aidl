/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

package vendor.lineage.biometrics.iris;

@VintfStability
oneway interface IIrisPreviewListener {
    /** Guidance for the current frame: one of IrisAcquired. */
    void onAcquired(int acquired);

    /**
     * An authentication capture started. faceReason is the AuthenticateReason.Face the framework
     * passed in its OperationContext (UNKNOWN without one), so keyguard can tell a bouncer retry
     * from a lockscreen attempt.
     */
    void onCaptureStarted(int faceReason);
}
