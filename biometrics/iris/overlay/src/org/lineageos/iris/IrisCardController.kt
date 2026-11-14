/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

package org.lineageos.iris

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.graphics.PixelFormat
import android.hardware.biometrics.AuthenticationStateListener
import android.hardware.biometrics.BiometricManager
import android.hardware.biometrics.BiometricRequestConstants
import android.hardware.biometrics.BiometricSourceType
import android.hardware.biometrics.events.AuthenticationAcquiredInfo
import android.hardware.biometrics.events.AuthenticationErrorInfo
import android.hardware.biometrics.events.AuthenticationFailedInfo
import android.hardware.biometrics.events.AuthenticationHelpInfo
import android.hardware.biometrics.events.AuthenticationStartedInfo
import android.hardware.biometrics.events.AuthenticationStoppedInfo
import android.hardware.biometrics.events.AuthenticationSucceededInfo
import android.hardware.face.FaceManager
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.Gravity
import android.view.ViewGroup
import android.view.WindowManager
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.platform.ComposeView
import androidx.compose.ui.platform.ViewCompositionStrategy

/**
 * Shows the iris card while the face sensor (the iris HAL) authenticates: on the bouncer over a
 * second blur of the PIN pad, in a BiometricPrompt as an island growing out of a pill at the top.
 * On the lockscreen itself iris still authenticates, but without the card: it would cover the
 * lockscreen. Enrollment has its own activity (IrisEnrollActivity). Face authentication reaches
 * AuthenticationStateListener only: FaceProvider gives BiometricStateCallback to enrollment.
 */
class IrisCardController(private val context: Context) {
    private val handler = Handler(Looper.getMainLooper())
    private val windowManager = context.getSystemService(WindowManager::class.java)

    private var mode = IrisCardMode.NONE
    private var view: ComposeView? = null
    private var owner: WindowOwner? = null
    private var failed by mutableStateOf(false)
    private var params: WindowManager.LayoutParams? = null

    fun start() {
        val faceManager = context.getSystemService(FaceManager::class.java)
        if (faceManager == null || !faceManager.isHardwareDetected) {
            Log.i(TAG, "No face sensor")
            return
        }
        context.getSystemService(BiometricManager::class.java)
            ?.registerAuthenticationStateListener(authListener)
        // The HAL reports why keyguard started a capture; only a bouncer retry shows the card.
        IrisHal.onCaptureStarted = { reason ->
            if (reason in BOUNCER_REASONS) {
                handler.post {
                    failed = false
                    show(IrisCardMode.BOUNCER)
                }
            }
        }
        IrisHal.listen()
        // A no-match card stays up over the bouncer; it goes with the keyguard.
        context.registerReceiver(
            object : BroadcastReceiver() {
                override fun onReceive(context: Context, intent: Intent) {
                    hide()
                }
            },
            IntentFilter().apply {
                addAction(Intent.ACTION_USER_PRESENT)
                addAction(Intent.ACTION_SCREEN_OFF)
            },
            Context.RECEIVER_NOT_EXPORTED,
        )
    }

    private val authListener =
        object : AuthenticationStateListener.Stub() {
            override fun onAuthenticationStarted(info: AuthenticationStartedInfo) {
                if (info.biometricSourceType != BiometricSourceType.FACE) return
                // Keyguard captures show the card from onCaptureStarted, by their reason.
                if (info.requestReason != BiometricRequestConstants.REASON_AUTH_BP) return
                handler.post {
                    failed = false
                    show(IrisCardMode.ISLAND)
                }
            }

            override fun onAuthenticationFailed(info: AuthenticationFailedInfo) {
                if (info.biometricSourceType == BiometricSourceType.FACE) {
                    handler.post { failed = true }
                }
            }

            override fun onAuthenticationSucceeded(info: AuthenticationSucceededInfo) {
                handler.post { hide() }
            }

            override fun onAuthenticationAcquired(info: AuthenticationAcquiredInfo) {}

            override fun onAuthenticationError(info: AuthenticationErrorInfo) {}

            override fun onAuthenticationHelp(info: AuthenticationHelpInfo) {}

            override fun onAuthenticationStopped(info: AuthenticationStoppedInfo) {
                if (info.biometricSourceType != BiometricSourceType.FACE) return
                // After a failed match the bouncer keeps the no-match card until a tap.
                handler.post { if (!(mode == IrisCardMode.BOUNCER && failed)) hide() }
            }
        }

    private fun show(newMode: IrisCardMode) {
        if (mode == newMode && !failed) return
        hide()
        mode = newMode
        failed = false
        IrisHal.clearHint()

        val res = context.resources
        val lp =
            WindowManager.LayoutParams(
                    // Above the notification shade (the bouncer) and the prompt's sub panel, and
                    // unlike TYPE_SECURE_SYSTEM_OVERLAY it takes touches ("Use PIN"; DisplayPolicy
                    // .adjustWindowParamsLw makes secure overlays untouchable).
                    WindowManager.LayoutParams.TYPE_KEYGUARD_DIALOG,
                    WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or
                        WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN or
                        WindowManager.LayoutParams.FLAG_HARDWARE_ACCELERATED,
                    PixelFormat.TRANSLUCENT,
                )
                .apply {
                    title = TAG
                    fitInsetsTypes = 0
                    layoutInDisplayCutoutMode =
                        WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS
                    width = ViewGroup.LayoutParams.MATCH_PARENT
                    if (newMode == IrisCardMode.BOUNCER) {
                        // Board 7: a second blur over the PIN pad, faded in by the card
                        height = ViewGroup.LayoutParams.MATCH_PARENT
                        flags = flags or WindowManager.LayoutParams.FLAG_BLUR_BEHIND
                        blurBehindRadius = 0
                    } else {
                        // Board 8: over the prompt, untouchable, down to the field's bottom
                        height = res.getDimensionPixelSize(R.dimen.iris_field_bottom)
                        gravity = Gravity.TOP
                        flags = flags or WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE
                    }
                }

        val windowOwner = WindowOwner()
        val composeView =
            ComposeView(context).apply {
                setViewCompositionStrategy(ViewCompositionStrategy.DisposeOnDetachedFromWindow)
                setContent {
                    IrisTheme(dark = true) {
                        IrisCard(
                            mode = newMode,
                            failed = failed,
                            onSurface = IrisHal::setPreviewSurface,
                            onBlur = { fraction -> setBlur(fraction, res.getDimensionPixelSize(R.dimen.iris_blur_radius)) },
                            onDismiss = { hide() },
                        )
                    }
                }
            }
        windowOwner.attach(composeView)
        owner = windowOwner
        view = composeView
        params = lp
        windowManager.addView(composeView, lp)
    }

    private fun setBlur(fraction: Float, radius: Int) {
        val v = view ?: return
        val lp = params ?: return
        val r = (fraction * radius).toInt()
        if (lp.blurBehindRadius == r) return
        lp.blurBehindRadius = r
        windowManager.updateViewLayout(v, lp)
    }

    private fun hide() {
        if (mode == IrisCardMode.NONE) return
        mode = IrisCardMode.NONE
        failed = false
        IrisHal.setPreviewSurface(null)
        view?.let { windowManager.removeViewImmediate(it) }
        owner?.destroy()
        view = null
        owner = null
        params = null
    }

    private companion object {
        const val TAG = "IrisCard"

        // android.hardware.biometrics.common.AuthenticateReason.Face: the bouncer showing, and
        // the swipe up on it that retries face authentication.
        const val PRIMARY_BOUNCER_SHOWN = 2
        const val SWIPE_UP_ON_BOUNCER = 9
        val BOUNCER_REASONS = setOf(PRIMARY_BOUNCER_SHOWN, SWIPE_UP_ON_BOUNCER)
    }
}
