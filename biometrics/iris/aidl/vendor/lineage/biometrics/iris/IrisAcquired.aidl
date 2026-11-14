/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

package vendor.lineage.biometrics.iris;

/** The iris library's acquired codes that stock SecIrisService shows a hint for. */
@VintfStability
@Backing(type="int")
enum IrisAcquired {
    MOVE_CLOSER = 3,
    MOVE_FARTHER = 4,
    OPEN_WIDER = 9,
    MOVE_SOMEWHERE_DARKER = 11,
    CHANGE_POSITION = 12,
}
