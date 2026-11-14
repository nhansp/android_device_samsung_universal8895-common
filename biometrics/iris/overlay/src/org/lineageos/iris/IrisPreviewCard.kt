/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

package org.lineageos.iris

import android.graphics.Matrix
import android.graphics.SurfaceTexture
import android.view.Surface
import android.view.TextureView
import androidx.annotation.StringRes
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.blur
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Shadow
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.res.dimensionResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import vendor.lineage.biometrics.iris.IrisAcquired

// The RGB preview the HAL draws: the iris library's 192x108 image of both eyes
// (libIrisService processFrontImage, IrisServiceCallback::sendImageProcessed id 1).
private const val PREVIEW_WIDTH = 192f
private const val PREVIEW_HEIGHT = 108f

// Stock SecIrisService eye guides (IrisGLPreview EYE_POS from values/dimens.xml): two 121 dp
// circles 30 dp apart, centred in the 360 x 202.5 dp preview, so the eyes sit at 29.0 % and
// 71.0 % of its width, at half its height, and a circle spans 59.8 % of its height.
private const val EYE_LEFT_X = (44f + 60.5f) / 360f
private const val EYE_RIGHT_X = (44f + 121f + 30f + 60.5f) / 360f
private const val EYE_DIAMETER = 121f / 202.5f

// The hint sits 12 dp below the card's top edge (design decision, not in M3).
private val HINT_TOP = 12.dp

/** Stock IrisViewManager.processAcquiredInfo(): the hint of each acquired code. */
@StringRes
fun hintText(acquired: Int?): Int? =
    when (acquired) {
        IrisAcquired.MOVE_CLOSER -> R.string.iris_acquired_move_closer
        IrisAcquired.MOVE_FARTHER -> R.string.iris_acquired_move_farther
        IrisAcquired.OPEN_WIDER -> R.string.iris_acquired_open_wider
        IrisAcquired.MOVE_SOMEWHERE_DARKER -> R.string.iris_acquired_move_somewhere_darker
        IrisAcquired.CHANGE_POSITION -> R.string.iris_acquired_change_your_position
        else -> null
    }

/**
 * The preview field: the HAL's colour preview cropped to fill, the stock eye guides on top, a
 * light Monet border and shadow, and the guidance when [showMessage] is set. On failure the
 * preview blurs under the dynamic tint.
 */
@Composable
fun IrisPreviewCard(
    modifier: Modifier = Modifier,
    acquired: Int?,
    failed: Boolean,
    showMessage: Boolean,
    contentAlpha: Float = 1f,
    onSurface: (Surface?) -> Unit,
) {
    val shape = RoundedCornerShape(dimensionResource(R.dimen.iris_card_radius))
    val monet = MaterialTheme.colorScheme.primaryFixedDim
    Box(
        modifier
            .shadow(dimensionResource(R.dimen.iris_card_elevation), shape, ambientColor = monet, spotColor = monet)
            .border(dimensionResource(R.dimen.iris_card_border), monet, shape)
            .clip(shape)
            .background(Color.Black)
    ) {
        PreviewTexture(
            Modifier.fillMaxSize()
                .alpha(contentAlpha)
                .then(if (failed) Modifier.blur(dimensionResource(R.dimen.iris_blur_radius)) else Modifier),
            onSurface,
        )
        if (failed) {
            Box(Modifier.fillMaxSize().background(IrisColors.tint()))
        } else {
            EyeGuides(Modifier.fillMaxSize().alpha(contentAlpha), monet)
        }
        if (showMessage) {
            val message =
                when {
                    failed -> R.string.iris_no_match
                    else -> hintText(acquired) ?: R.string.iris_guide
                }
            CardMessage(stringResource(message), contentAlpha)
        }
    }
}

/** The stock eye circles, placed on the cropped preview the same way as the image. */
@Composable
private fun EyeGuides(modifier: Modifier, color: Color) {
    val thickness = dimensionResource(R.dimen.iris_eye_ring_thickness)
    Canvas(modifier) {
        val scale = maxOf(size.width / PREVIEW_WIDTH, size.height / PREVIEW_HEIGHT)
        val shownWidth = PREVIEW_WIDTH * scale
        val shownHeight = PREVIEW_HEIGHT * scale
        val left = (size.width - shownWidth) / 2f
        val top = (size.height - shownHeight) / 2f
        val radius = EYE_DIAMETER * shownHeight / 2f
        for (x in floatArrayOf(EYE_LEFT_X, EYE_RIGHT_X)) {
            drawCircle(
                color = color,
                radius = radius,
                center = Offset(left + x * shownWidth, top + shownHeight / 2f),
                style = Stroke(thickness.toPx()),
            )
        }
    }
}

@Composable
private fun CardMessage(text: String, alpha: Float) {
    Text(
        text = text,
        modifier = Modifier.fillMaxWidth().padding(top = HINT_TOP, start = 16.dp, end = 16.dp).alpha(alpha),
        color = Color.White,
        textAlign = TextAlign.Center,
        style =
            MaterialTheme.typography.titleMedium.copy(
                // Board 7: the guide carries a soft shadow over the preview
                shadow = Shadow(Color(0xB3000000), blurRadius = 3f),
            ),
    )
}

@Composable
private fun PreviewTexture(modifier: Modifier, onSurface: (Surface?) -> Unit) {
    AndroidView(
        modifier = modifier,
        factory = { context ->
            TextureView(context).apply {
                surfaceTextureListener =
                    object : TextureView.SurfaceTextureListener {
                        override fun onSurfaceTextureAvailable(
                            texture: SurfaceTexture,
                            width: Int,
                            height: Int,
                        ) {
                            cropToFill(this@apply, width, height)
                            onSurface(Surface(texture))
                        }

                        override fun onSurfaceTextureSizeChanged(
                            texture: SurfaceTexture,
                            width: Int,
                            height: Int,
                        ) = cropToFill(this@apply, width, height)

                        override fun onSurfaceTextureDestroyed(texture: SurfaceTexture): Boolean {
                            // onSurface owns the Surfaces it gets and releases them when replaced.
                            onSurface(null)
                            return true
                        }

                        override fun onSurfaceTextureUpdated(texture: SurfaceTexture) {}
                    }
            }
        },
    )
}

/** Scales the preview to fill the view, centred, cropping the rest. */
private fun cropToFill(view: TextureView, width: Int, height: Int) {
    if (width == 0 || height == 0) return
    val scale = maxOf(width / PREVIEW_WIDTH, height / PREVIEW_HEIGHT)
    view.setTransform(
        Matrix().apply {
            setScale(PREVIEW_WIDTH * scale / width, PREVIEW_HEIGHT * scale / height, width / 2f, height / 2f)
        }
    )
}

object IrisColors {
    /**
     * Board 4/7 tint: the dynamic secondary tone 20 (the dark scheme's onSecondary) at 55 %,
     * like SystemUI tints its blurred backgrounds.
     */
    @Composable
    fun tint(): Color =
        androidx.compose.material3.dynamicDarkColorScheme(androidx.compose.ui.platform.LocalContext.current)
            .onSecondary
            .copy(alpha = 0.55f)
}
