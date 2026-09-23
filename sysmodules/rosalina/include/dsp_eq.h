
/*
*   This file is part of Luma3DS
*   Copyright (C) 2016-2020 Aurora Wright, TuxSH
*
*   This program is free software: you can redistribute it and/or modify
*   it under the terms of the GNU General Public License as published by
*   the Free Software Foundation, either version 3 of the License, or
*   (at your option) any later version.
*
*   This program is distributed in the hope that it will be useful,
*   but WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*   GNU General Public License for more details.
*
*   You should have received a copy of the GNU General Public License
*   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*   Additional Terms 7.b and 7.c of GPLv3 apply to this file:
*       * Requiring preservation of specified reasonable legal notices or
*         author attributions in that material or in the Appropriate Legal
*         Notices displayed by works containing it.
*       * Prohibiting misrepresentation of the origin of that material,
*         or requiring that modified versions of such material be marked in
*         reasonable ways as different from the original version.
*/

#pragma once

#include "MyThread.h"

// Applies the equalizer settings (see equalizer.h) to the DSP by patching the running DSP firmware.
MyThread *DspEq_CreateThread(void);

// Call after the gains changed
void DspEq_NotifyChanged(void);

// Short description of the DSP patch state, for the equalizer menu
typedef struct DspEqStatus {
    u8 pdnDspCnt;       // raw PDN_DSP_CNT (bit 0: not in reset, bit 1: clock on)
    bool dspRunning;
    u16 hookA, hookB;   // operands of the two hooked calls in the firmware (valid if dspRunning)
    u16 magic;          // parameter block magic (valid if dspRunning)
} DspEqStatus;

void DspEq_GetStatus(DspEqStatus *status);
