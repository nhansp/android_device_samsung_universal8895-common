/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

package org.lineageos.iris

import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.ExperimentalMaterial3ExpressiveApi
import androidx.compose.material3.MaterialExpressiveTheme
import androidx.compose.material3.MotionScheme
import androidx.compose.material3.dynamicDarkColorScheme
import androidx.compose.material3.dynamicLightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.platform.LocalContext

/** M3 Expressive with the phone's dynamic colours. */
@OptIn(ExperimentalMaterial3ExpressiveApi::class)
@Composable
fun IrisTheme(dark: Boolean = isSystemInDarkTheme(), content: @Composable () -> Unit) {
    val context = LocalContext.current
    MaterialExpressiveTheme(
        colorScheme =
        if (dark) dynamicDarkColorScheme(context) else dynamicLightColorScheme(context),
        motionScheme = MotionScheme.expressive(),
        content = content
    )
}
