/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

package org.lineageos.iris

import android.content.Intent
import android.hardware.face.FaceEnrollOptions
import android.hardware.face.FaceManager
import android.os.Bundle
import android.os.CancellationSignal
import android.os.UserHandle
import android.util.Log
import android.view.Surface
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.material3.Button
import androidx.compose.material3.ExperimentalMaterial3ExpressiveApi
import androidx.compose.material3.LinearWavyProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.dimensionResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp

/**
 * Iris enrollment, launched from Settings' face enrollment through config_face_enroll. Settings
 * passes the hardware auth token and finishes its own flow on RESULT_FINISHED.
 */
class IrisEnrollActivity : ComponentActivity() {
    private enum class Phase {
        WAITING,
        ENROLLING,
        DONE,
        FAILED
    }

    private var phase by mutableStateOf(Phase.WAITING)
    private var percent by mutableIntStateOf(0)
    private var cancel: CancellationSignal? = null
    private var surface: Surface? = null
    private var token: ByteArray? = null
    private var userId = 0

    override fun onCreate(savedInstanceState: Bundle?) {
        enableEdgeToEdge()
        super.onCreate(savedInstanceState)
        token = intent.getByteArrayExtra(EXTRA_KEY_CHALLENGE_TOKEN)
        userId = intent.getIntExtra(Intent.EXTRA_USER_ID, UserHandle.myUserId())
        if (token == null) {
            Log.e(TAG, "No hardware auth token")
            finish()
            return
        }
        setContent { IrisTheme { EnrollScreen() } }
    }

    override fun onStop() {
        super.onStop()
        // Leaving the screen ends the attempt
        if (phase != Phase.DONE && !isChangingConfigurations) {
            cancel?.cancel()
            finish()
        }
    }

    private fun onSurface(newSurface: Surface?) {
        // FaceManager.enroll() parcels the surface right away, so the old one can go now.
        surface?.release()
        surface = newSurface
        if (newSurface == null) {
            cancel?.cancel()
        } else if (phase == Phase.WAITING) {
            enroll()
        }
    }

    private fun enroll() {
        val previewSurface = surface ?: return
        val faceManager = getSystemService(FaceManager::class.java) ?: return
        IrisHal.clearHint()
        percent = 0
        phase = Phase.ENROLLING
        cancel = CancellationSignal()
        faceManager.enroll(
            userId,
            token,
            cancel,
            object : FaceManager.EnrollmentCallback() {
                override fun onEnrollmentProgress(remaining: Int) {
                    percent = (100 - remaining).coerceIn(0, 100)
                    if (remaining == 0) {
                        phase = Phase.DONE
                        setResult(
                            RESULT_FINISHED,
                            Intent().putExtra(EXTRA_FINISHED_ENROLL_FACE, true)
                        )
                    }
                }

                override fun onEnrollmentError(errMsgId: Int, errString: CharSequence?) {
                    if (phase == Phase.ENROLLING) phase = Phase.FAILED
                }
            },
            IntArray(0),
            previewSurface,
            false,
            FaceEnrollOptions.Builder().build()
        )
    }

    @OptIn(ExperimentalMaterial3ExpressiveApi::class)
    @Composable
    private fun EnrollScreen() {
        val acquired by IrisHal.hint.collectAsState()
        val shownProgress by
            animateFloatAsState(
                percent / 100f,
                MaterialTheme.motionScheme.defaultSpatialSpec(),
                label = "progress"
            )
        Surface(Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.surface) {
            IrisField(
                acquired = acquired,
                failed = phase == Phase.FAILED,
                showMessage = false,
                onSurface = ::onSurface
            )
            Column(
                Modifier.fillMaxSize()
                    .safeDrawingPadding()
                    .padding(top = dimensionResource(R.dimen.iris_field_bottom) - statusBarHeight())
                    .padding(horizontal = 24.dp),
                horizontalAlignment = Alignment.CenterHorizontally
            ) {
                Text(
                    stringResource(R.string.iris_enroll_title),
                    modifier = Modifier.fillMaxWidth().padding(top = 24.dp, bottom = 8.dp),
                    style = MaterialTheme.typography.headlineSmall,
                    color = MaterialTheme.colorScheme.onSurface
                )
                Spacer(Modifier.height(24.dp))
                val description = stringResource(R.string.iris_enroll_progress_description)
                LinearWavyProgressIndicator(
                    progress = { shownProgress },
                    modifier =
                    Modifier.fillMaxWidth().semantics { contentDescription = description }
                )
                Text(
                    "$percent%",
                    modifier = Modifier.padding(top = 16.dp),
                    style = MaterialTheme.typography.displaySmallEmphasized,
                    color = MaterialTheme.colorScheme.onSurface
                )
                Text(
                    stringResource(message(acquired)),
                    modifier = Modifier.fillMaxWidth().padding(top = 8.dp),
                    style = MaterialTheme.typography.bodyLarge,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    textAlign = TextAlign.Center
                )
                Spacer(Modifier.weight(1f))
                Row(
                    Modifier.fillMaxWidth().padding(bottom = 16.dp),
                    horizontalArrangement = Arrangement.SpaceBetween
                ) {
                    when (phase) {
                        Phase.DONE -> {
                            Spacer(Modifier)
                            Button(onClick = { finish() }) {
                                Text(stringResource(R.string.iris_enroll_done))
                            }
                        }
                        Phase.FAILED -> {
                            TextButton(onClick = { finish() }) {
                                Text(stringResource(android.R.string.cancel))
                            }
                            Button(onClick = { enroll() }) {
                                Text(stringResource(R.string.iris_enroll_try_again))
                            }
                        }
                        else ->
                            TextButton(onClick = { finish() }) {
                                Text(stringResource(android.R.string.cancel))
                            }
                    }
                }
            }
        }
    }

    private fun message(acquired: Int?): Int =
        when (phase) {
            Phase.DONE -> R.string.iris_enroll_complete
            Phase.FAILED -> R.string.iris_enroll_failed
            Phase.WAITING -> R.string.iris_enroll_guide
            Phase.ENROLLING -> {
                val fallback =
                    if (percent > 0) R.string.iris_enroll_progress else R.string.iris_enroll_guide
                hintText(acquired) ?: fallback
            }
        }

    private companion object {
        const val TAG = "IrisEnroll"

        // Extras of Settings' face enrollment
        const val EXTRA_KEY_CHALLENGE_TOKEN = "hw_auth_token"
        const val EXTRA_FINISHED_ENROLL_FACE = "finished_enrolling_face"
        const val RESULT_FINISHED = RESULT_FIRST_USER
    }
}
