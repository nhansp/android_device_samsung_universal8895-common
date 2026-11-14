/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

package org.lineageos.iris

import android.view.Surface
import androidx.compose.animation.core.Animatable
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
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.res.dimensionResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.lerp
import kotlinx.coroutines.launch

enum class IrisCardMode {
    NONE,
    BOUNCER,
    ISLAND,
}

/** The preview field's top: right under the status bar, as the user specified. */
@Composable
fun irisFieldTop(): Dp = with(LocalDensity.current) { WindowInsets.statusBars.getTop(this).toDp() }

/**
 * The preview field at its fixed place, growing out of a pill on the expressive spatial spring;
 * the preview and its message fade in on the effects spring once it is mostly open. The same in
 * every mode: bouncer, BiometricPrompt and enrollment.
 */
@Composable
fun IrisField(
    modifier: Modifier = Modifier,
    acquired: Int?,
    failed: Boolean,
    showMessage: Boolean,
    onSurface: (Surface?) -> Unit,
) {
    val expand = remember { Animatable(0f) }
    val content = remember { Animatable(0f) }
    val spatial = MaterialTheme.motionScheme.defaultSpatialSpec<Float>()
    val effects = MaterialTheme.motionScheme.defaultEffectsSpec<Float>()
    LaunchedEffect(Unit) {
        expand.animateTo(1f, spatial)
        content.animateTo(1f, effects)
    }
    val e = expand.value
    val top = irisFieldTop()
    val width = dimensionResource(R.dimen.iris_card_width)
    val height = dimensionResource(R.dimen.iris_field_bottom) - top
    Box(modifier.fillMaxSize()) {
        IrisPreviewCard(
            Modifier.align(Alignment.TopCenter)
                .offset(y = top)
                .size(
                    lerp(dimensionResource(R.dimen.iris_island_pill_width), width, e),
                    lerp(dimensionResource(R.dimen.iris_island_pill_height), height, e),
                ),
            acquired = acquired,
            failed = failed,
            showMessage = showMessage,
            contentAlpha = content.value,
            onSurface = onSurface,
        )
    }
}

/**
 * The iris card window. On the bouncer (board 7) it fades a blur and the dynamic tint over the
 * PIN pad, like keyguard's own blur, with "Use PIN" under the field; any tap goes back to the
 * PIN pad. In a BiometricPrompt (board 8) it is the field alone.
 */
@OptIn(ExperimentalMaterial3ExpressiveApi::class)
@Composable
fun IrisCard(
    mode: IrisCardMode,
    failed: Boolean,
    onSurface: (Surface?) -> Unit,
    onBlur: (Float) -> Unit,
    onDismiss: () -> Unit,
) {
    val acquired by IrisHal.hint.collectAsState()
    if (mode != IrisCardMode.BOUNCER) {
        IrisField(acquired = acquired, failed = failed, showMessage = true, onSurface = onSurface)
        return
    }

    // Full-screen effect: the slow effects spring (M3 motion: damping 1.0, stiffness 800).
    val blur = remember { Animatable(0f) }
    val slowEffects = MaterialTheme.motionScheme.slowEffectsSpec<Float>()
    // "Use PIN" springs in on the spatial spring, fading on the effects spring.
    val button = remember { Animatable(0f) }
    val buttonAlpha = remember { Animatable(0f) }
    val spatial = MaterialTheme.motionScheme.defaultSpatialSpec<Float>()
    val effects = MaterialTheme.motionScheme.defaultEffectsSpec<Float>()
    LaunchedEffect(Unit) {
        launch { blur.animateTo(1f, slowEffects) { onBlur(value) } }
        launch { button.animateTo(1f, spatial) }
        launch { buttonAlpha.animateTo(1f, effects) }
    }

    Box(
        Modifier.fillMaxSize()
            .alpha(blur.value)
            .background(IrisColors.tint())
            .clickable(interactionSource = remember { MutableInteractionSource() }, indication = null) {
                onDismiss()
            }
    )
    IrisField(acquired = acquired, failed = failed, showMessage = true, onSurface = onSurface)
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
                        alpha = buttonAlpha.value
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
