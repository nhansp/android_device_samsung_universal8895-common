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

    /**
     * A bouncer attempt found no match. The camera is off and the attempt stays open, without a
     * result, until IIrisPreview.resumeCapture() or endCapture(), or HOLD_TIMEOUT_MS. timedOut is
     * set when the capture ended on the eye safety timeout rather than on a rejected match.
     */
    void onCaptureHeld(boolean timedOut);
}
