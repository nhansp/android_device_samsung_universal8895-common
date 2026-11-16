/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

package org.lineageos.iris

import android.os.IBinder
import android.os.RemoteException
import android.os.ServiceManager
import android.util.Log
import android.view.Surface
import java.util.concurrent.Executors
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import vendor.lineage.biometrics.iris.IIrisPreview
import vendor.lineage.biometrics.iris.IIrisPreviewListener

/** vendor.lineage.biometrics.iris: the iris face HAL draws the preview and sends hints. */
object IrisHal {
    private const val TAG = "IrisHal"
    private const val INSTANCE = "vendor.lineage.biometrics.iris.IIrisPreview/default"

    private val executor = Executors.newSingleThreadExecutor()
    private val _hint = MutableStateFlow<Int?>(null)

    /** The latest IrisAcquired hint of the current capture, or null. */
    val hint: StateFlow<Int?> = _hint

    /** Called on a binder thread when an authentication capture starts, with its face reason. */
    @Volatile var onCaptureStarted: ((faceReason: Int) -> Unit)? = null

    /** Called on a binder thread when the HAL holds a bouncer attempt after a no match. */
    @Volatile var onCaptureHeld: ((timedOut: Boolean) -> Unit)? = null

    @Volatile private var service: IIrisPreview? = null
    private var surface: Surface? = null

    private val listener =
        object : IIrisPreviewListener.Stub() {
            override fun onAcquired(acquired: Int) {
                _hint.value = acquired
            }

            override fun onCaptureStarted(faceReason: Int) {
                onCaptureStarted?.invoke(faceReason)
            }

            override fun onCaptureHeld(timedOut: Boolean) {
                onCaptureHeld?.invoke(timedOut)
            }

            override fun getInterfaceVersion(): Int = IIrisPreviewListener.VERSION

            override fun getInterfaceHash(): String = IIrisPreviewListener.HASH
        }

    private val death = IBinder.DeathRecipient {
        service = null
        // The HAL restarted: hand it the surface again.
        executor.execute { connect()?.let { apply(it) } }
    }

    /** Receive the capture callbacks from now on, before any preview surface exists. */
    fun listen() {
        executor.execute { connect()?.let { apply(it) } }
    }

    fun clearHint() {
        _hint.value = null
    }

    /**
     * Draw the preview of the coming captures into [surface], or stop with null. Takes ownership:
     * the previous surface is released only once the HAL has the new one, so a queued call never
     * parcels a released Surface.
     */
    fun setPreviewSurface(surface: Surface?) {
        executor.execute {
            val old = this.surface
            this.surface = surface
            connect()?.let { apply(it) }
            if (old !== surface) old?.release()
        }
    }

    /** Captures again in the held attempt (board 4: tap to retry). */
    fun resumeCapture() {
        executor.execute { call { it.resumeCapture() } }
    }

    /** Ends the current keyguard attempt: "Use PIN", a tap outside the card, the screen going off. */
    fun endCapture() {
        executor.execute { call { it.endCapture() } }
    }

    private fun call(action: (IIrisPreview) -> Unit) {
        val hal = connect() ?: return
        try {
            action(hal)
        } catch (e: RemoteException) {
            Log.w(TAG, "IIrisPreview failed", e)
        }
    }

    private fun apply(hal: IIrisPreview) {
        try {
            hal.setListener(listener)
            hal.setPreviewSurface(surface)
        } catch (e: RemoteException) {
            Log.w(TAG, "IIrisPreview failed", e)
        }
    }

    private fun connect(): IIrisPreview? {
        service?.let { return it }
        val binder = ServiceManager.waitForDeclaredService(INSTANCE) ?: return null
        return try {
            binder.linkToDeath(death, 0)
            IIrisPreview.Stub.asInterface(binder).also { service = it }
        } catch (e: RemoteException) {
            Log.w(TAG, "IIrisPreview died", e)
            null
        }
    }
}
