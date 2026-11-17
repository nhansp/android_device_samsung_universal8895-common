/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

package org.lineageos.iris

import android.view.Surface
import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.AnimationVector1D
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBars
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.ExperimentalMaterial3ExpressiveApi
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.res.dimensionResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.Dp
import kotlinx.coroutines.joinAll
import kotlinx.coroutines.launch

enum class IrisCardMode {
    NONE,
    BOUNCER,
    ISLAND,
}

@Composable
fun statusBarHeight(): Dp =
    with(LocalDensity.current) { WindowInsets.statusBars.getTop(this).toDp() }

/** The preview field's top: a status bar's height of space under the status bar. */
@Composable
fun irisFieldTop(): Dp = statusBarHeight() * 2

/**
 * How far the field has slid down into place: 0 above the screen, 1 in place. It slides like
 * SystemUI's screen record menu (SmallScreenCaptureRecordContent: slideInVertically on the
 * motion scheme's fast spatial spring).
 */
@Composable
fun rememberFieldReveal(): Animatable<Float, AnimationVector1D> {
    val reveal = remember { Animatable(0f) }
    val spatial = MaterialTheme.motionScheme.fastSpatialSpec<Float>()
    LaunchedEffect(Unit) { reveal.animateTo(1f, spatial) }
    return reveal
}

/**
 * The preview field at its fixed place, slid down from above the screen by [reveal]. The same in
 * every mode: bouncer, BiometricPrompt and enrollment. [onRetry] makes the failed field the
 * board 4 retry target.
 */
@Composable
fun IrisField(
    modifier: Modifier = Modifier,
    acquired: Int?,
    failed: Boolean,
    showMessage: Boolean,
    onSurface: (Surface?) -> Unit,
    reveal: Animatable<Float, AnimationVector1D> = rememberFieldReveal(),
    onRetry: (() -> Unit)? = null,
) {
    val top = irisFieldTop()
    val height = dimensionResource(R.dimen.iris_field_bottom) - top
    Box(modifier.fillMaxSize()) {
        IrisPreviewCard(
            Modifier.align(Alignment.TopCenter)
                .offset(y = top)
                .size(dimensionResource(R.dimen.iris_card_width), height)
                .graphicsLayer { translationY = (reveal.value - 1f) * (top + height).toPx() }
                .then(
                    if (failed && onRetry != null) {
                        Modifier.clickable(
                            interactionSource = remember { MutableInteractionSource() },
                            indication = null,
                            onClick = onRetry,
                        )
                    } else {
                        Modifier
                    }
                ),
            acquired = acquired,
            failed = failed,
            showMessage = showMessage,
            showRetry = onRetry != null,
            onSurface = onSurface,
        )
    }
}

/**
 * The iris card window. On the bouncer (board 7) it fades a blur and the dynamic tint over the
 * PIN pad, like keyguard's own blur, with "Use PIN" under the field; a tap outside the field goes
 * back to the PIN pad, and after a no match a tap on the field retries (board 4). In a
 * BiometricPrompt (board 8) it is the field alone. When [visible] turns false the field slides
 * back up and the rest fades out on the effects spring, then [onExited] runs.
 */
@OptIn(ExperimentalMaterial3ExpressiveApi::class)
@Composable
fun IrisCard(
    mode: IrisCardMode,
    visible: Boolean,
    failed: Boolean,
    onSurface: (Surface?) -> Unit,
    onBlur: (Float) -> Unit,
    onRetry: () -> Unit,
    onDismiss: () -> Unit,
    onExited: () -> Unit,
) {
    val acquired by IrisHal.hint.collectAsState()
    val exited by rememberUpdatedState(onExited)
    val alpha = remember { Animatable(1f) }
    val reveal = rememberFieldReveal()
    val fastSpatial = MaterialTheme.motionScheme.fastSpatialSpec<Float>()
    // Full-screen effect: the slow effects spring (M3 motion: damping 1.0, stiffness 800).
    val blur = remember { Animatable(0f) }
    val slowEffects = MaterialTheme.motionScheme.slowEffectsSpec<Float>()
    val effects = MaterialTheme.motionScheme.defaultEffectsSpec<Float>()
    LaunchedEffect(visible) {
        if (visible) {
            if (mode == IrisCardMode.BOUNCER) blur.animateTo(1f, slowEffects) { onBlur(value) }
            return@LaunchedEffect
        }
        joinAll(
            launch { if (mode == IrisCardMode.BOUNCER) blur.animateTo(0f, effects) { onBlur(value) } },
            launch { alpha.animateTo(0f, effects) },
            launch { reveal.animateTo(0f, fastSpatial) },
        )
        exited()
    }

    Box(Modifier.fillMaxSize()) {
        if (mode != IrisCardMode.BOUNCER) {
            IrisField(
                acquired = acquired,
                failed = failed,
                showMessage = true,
                onSurface = onSurface,
                reveal = reveal,
            )
            return@Box
        }

        // "Use PIN" springs in on the spatial spring, fading on the effects spring.
        val button = remember { Animatable(0f) }
        val buttonAlpha = remember { Animatable(0f) }
        val spatial = MaterialTheme.motionScheme.defaultSpatialSpec<Float>()
        LaunchedEffect(Unit) {
            launch { button.animateTo(1f, spatial) }
            launch { buttonAlpha.animateTo(1f, effects) }
        }

        Box(
            Modifier.fillMaxSize()
                .graphicsLayer { this.alpha = blur.value * alpha.value }
                .background(IrisColors.tint())
                .clickable(interactionSource = remember { MutableInteractionSource() }, indication = null) {
                    onDismiss()
                }
        )
        IrisField(
            acquired = acquired,
            failed = failed,
            showMessage = true,
            onSurface = onSurface,
            reveal = reveal,
            onRetry = onRetry,
        )
        Box(Modifier.fillMaxSize()) {
            Button(
                onClick = onDismiss,
                modifier =
                    Modifier.align(Alignment.TopCenter)
                        .offset(y = dimensionResource(R.dimen.iris_field_bottom) + dimensionResource(R.dimen.iris_pin_button_gap))
                        .size(
                            dimensionResource(R.dimen.iris_pin_button_width),
                            dimensionResource(R.dimen.iris_pin_button_height),
                        )
                        .graphicsLayer {
                            scaleX = 0.6f + 0.4f * button.value
                            scaleY = 0.6f + 0.4f * button.value
                            this.alpha = buttonAlpha.value * alpha.value
                        },
                // M3 Expressive buttons morph their shape on press
                shapes = ButtonDefaults.shapes(),
                colors =
                    ButtonDefaults.buttonColors(
                        containerColor = MaterialTheme.colorScheme.secondaryContainer,
                        contentColor = MaterialTheme.colorScheme.onSecondaryContainer,
                    ),
            ) {
                Text(stringResource(R.string.iris_use_pin), style = MaterialTheme.typography.labelLarge)
            }
        }
    }
}
