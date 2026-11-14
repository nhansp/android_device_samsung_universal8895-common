/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

package org.lineageos.iris

import android.app.Application

class IrisApp : Application() {
    override fun onCreate() {
        super.onCreate()
        IrisCardController(this).start()
    }
}
