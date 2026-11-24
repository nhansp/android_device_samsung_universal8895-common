/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

package org.lineageos.iris

import android.app.ActivityManager
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.content.res.Resources
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
import android.os.RemoteException
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
 * Shows the iris card while the iris HAL authenticates: on the bouncer over a second blur of the
 * PIN pad, in a BiometricPrompt as an island at the top. The lockscreen gets no card, it would
 * cover it. Face authentication reaches AuthenticationStateListener only: FaceProvider gives
 * BiometricStateCallback to enrollment.
 */
class IrisCardController(private val context: Context) {
    private val handler = Handler(Looper.getMainLooper())
    private val windowManager = context.getSystemService(WindowManager::class.java)

    private var mode = IrisCardMode.NONE
    private var view: ComposeView? = null
    private var owner: WindowOwner? = null
    private var params: WindowManager.LayoutParams? = null
    private var failed by mutableStateOf(false)
    private var visible by mutableStateOf(false)
    private var pendingShow: Runnable? = null

    // SystemUI holds the bouncer back this long when face may pass first; the card shows with it
    private var bouncerDelayMs = DEFAULT_BOUNCER_DELAY_MS.toLong()

    fun start() {
        val faceManager = context.getSystemService(FaceManager::class.java)
        if (faceManager == null || !faceManager.isHardwareDetected) {
            Log.i(TAG, "No face sensor")
            return
        }
        bouncerDelayMs =
            systemUiResources()
                ?.let { res ->
                    val id = res.getIdentifier("primary_bouncer_passive_auth_delay", "integer", SYSTEMUI)
                    if (id != 0) res.getInteger(id) else null
                }
                ?.toLong() ?: bouncerDelayMs
        context.getSystemService(BiometricManager::class.java)
            ?.registerAuthenticationStateListener(authListener)
        // The HAL reports why keyguard started a capture; only a bouncer attempt shows the card.
        IrisHal.onCaptureStarted = { reason ->
            if (reason in BOUNCER_REASONS) handler.post { onBouncerCapture() }
        }
        IrisHal.onCaptureHeld = { handler.post { if (mode == IrisCardMode.BOUNCER) onFailed() } }
        IrisHal.listen()
        context.registerReceiver(
            object : BroadcastReceiver() {
                override fun onReceive(context: Context, intent: Intent) {
                    if (intent.action == Intent.ACTION_SCREEN_OFF) IrisHal.endCapture()
                    cancelPendingShow()
                    remove()
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
                handler.post { show(IrisCardMode.ISLAND) }
            }

            override fun onAuthenticationFailed(info: AuthenticationFailedInfo) {
                if (info.biometricSourceType != BiometricSourceType.FACE) return
                // A bouncer attempt reports its no match only once it is over; the prompt owns
                // its retry, so the island just shows the no match.
                handler.post { if (mode == IrisCardMode.ISLAND) onFailed() }
            }

            override fun onAuthenticationSucceeded(info: AuthenticationSucceededInfo) {
                handler.post { dismiss() }
            }

            override fun onAuthenticationAcquired(info: AuthenticationAcquiredInfo) {}

            override fun onAuthenticationError(info: AuthenticationErrorInfo) {}

            override fun onAuthenticationHelp(info: AuthenticationHelpInfo) {}

            override fun onAuthenticationStopped(info: AuthenticationStoppedInfo) {
                if (info.biometricSourceType != BiometricSourceType.FACE) return
                handler.post {
                    cancelPendingShow()
                    dismiss()
                }
            }
        }

    /**
     * Keyguard asks for the bouncer as soon as a swipe up starts, and starts iris with it, even
     * when the swipe is let go and the bouncer never shows. The card waits as long as SystemUI
     * holds the bouncer back for face; an attempt that ends meanwhile cancels it.
     */
    private fun onBouncerCapture() {
        if (mode == IrisCardMode.BOUNCER) {
            // The held attempt resumed.
            failed = false
            IrisHal.clearHint()
            setKeepScreenOn(true)
            return
        }
        cancelPendingShow()
        val show = Runnable {
            pendingShow = null
            show(IrisCardMode.BOUNCER)
        }
        pendingShow = show
        handler.postDelayed(show, bouncerDelayMs)
    }

    private fun cancelPendingShow() {
        pendingShow?.let { handler.removeCallbacks(it) }
        pendingShow = null
    }

    private fun show(newMode: IrisCardMode) {
        remove()
        mode = newMode
        failed = false
        visible = true
        IrisHal.clearHint()

        val res = context.resources
        val lp =
            WindowManager.LayoutParams(
                    if (newMode == IrisCardMode.BOUNCER) {
                        // Above the notification shade; unlike secure overlays it takes touches
                        WindowManager.LayoutParams.TYPE_KEYGUARD_DIALOG
                    } else {
                        // Part of the prompt, so above its dim
                        WindowManager.LayoutParams.TYPE_SECURE_SYSTEM_OVERLAY
                    },
                    WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or
                        // The scan holds the screen on; a no match hands it back to the timeout.
                        WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON or
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
                        // A second blur over the PIN pad, faded in by the card
                        height = ViewGroup.LayoutParams.MATCH_PARENT
                        flags = flags or WindowManager.LayoutParams.FLAG_BLUR_BEHIND
                        blurBehindRadius = 0
                    } else {
                        // Over the prompt, untouchable, down to the field's bottom
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
                            visible = visible,
                            failed = failed,
                            onSurface = IrisHal::setPreviewSurface,
                            onBlur = { fraction -> setBlur(fraction, res.getDimensionPixelSize(R.dimen.iris_blur_radius)) },
                            onRetry = ::retry,
                            onDismiss = ::usePin,
                            onExited = ::remove,
                        )
                    }
                }
            }
        windowOwner.attach(composeView)
        owner = windowOwner
        view = composeView
        params = lp
        windowManager.addView(composeView, lp)
        // Schedules the card's render thread like SystemUI's own windows
        setHasTopUi(true)
    }

    private fun retry() {
        if (!failed) return
        failed = false
        IrisHal.clearHint()
        setKeepScreenOn(true)
        IrisHal.resumeCapture()
    }

    private fun onFailed() {
        failed = true
        setKeepScreenOn(false)
    }

    private fun setKeepScreenOn(on: Boolean) {
        val v = view ?: return
        val lp = params ?: return
        val flags =
            if (on) lp.flags or WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON
            else lp.flags and WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON.inv()
        if (flags == lp.flags) return
        lp.flags = flags
        windowManager.updateViewLayout(v, lp)
    }

    private fun usePin() {
        IrisHal.endCapture()
        dismiss()
    }

    private fun setBlur(fraction: Float, radius: Int) {
        val v = view ?: return
        val lp = params ?: return
        val r = (fraction * radius).toInt()
        if (lp.blurBehindRadius == r) return
        lp.blurBehindRadius = r
        windowManager.updateViewLayout(v, lp)
    }

    /** Plays the card's exit; the window goes once it ends. */
    private fun dismiss() {
        if (mode == IrisCardMode.NONE) return
        visible = false
    }

    private fun remove() {
        if (mode == IrisCardMode.NONE) return
        mode = IrisCardMode.NONE
        failed = false
        visible = false
        IrisHal.setPreviewSurface(null)
        view?.let { windowManager.removeViewImmediate(it) }
        owner?.destroy()
        view = null
        owner = null
        params = null
        setHasTopUi(false)
    }

    private fun setHasTopUi(hasTopUi: Boolean) {
        try {
            ActivityManager.getService().setHasTopUi(hasTopUi)
        } catch (e: RemoteException) {
            Log.w(TAG, "setHasTopUi failed", e)
        }
    }

    private fun systemUiResources(): Resources? =
        try {
            context.packageManager.getResourcesForApplication(SYSTEMUI)
        } catch (e: PackageManager.NameNotFoundException) {
            null
        }

    private companion object {
        const val TAG = "IrisCard"
        const val SYSTEMUI = "com.android.systemui"

        // SystemUI's primary_bouncer_passive_auth_delay
        const val DEFAULT_BOUNCER_DELAY_MS = 500

        // AuthenticateReason.Face: the bouncer showing, and the swipe up on it that retries
        const val PRIMARY_BOUNCER_SHOWN = 2
        const val SWIPE_UP_ON_BOUNCER = 9
        val BOUNCER_REASONS = setOf(PRIMARY_BOUNCER_SHOWN, SWIPE_UP_ON_BOUNCER)
    }
}
