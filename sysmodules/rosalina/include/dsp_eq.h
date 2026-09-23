
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
    bool gaveUp;        // patching was stopped because it kept being undone (see dsp_eq.c)
    u32 installs;       // how many times the patch was applied
    u32 fixes;          // how many times the parameter block had to be restored
    u16 hookA, hookB;   // operands of the two hooked calls in the firmware (valid if dspRunning)
    u16 diagCalls;      // how many times the DSP routine ran since the patch was installed
    u16 diagR4;         // buffer pointer it received the last time
    u16 diagA, diagB;   // calls through the plain copy / soft clipping output path
    u32 callsPerSec;    // measured rate of diagCalls (about 204 when everything runs normally); 0 while the system is paused
    u32 peakCallsPerSec;// highest rate seen so far
    u16 diagIdx0, diagIdx1; // output read index when the routine started / finished (16 words = 0.24 ms)
    u16 droppedA, droppedB; // firmware's own dropped-frame counter in the two shared frame buffers
    u16 magic;          // parameter block magic (valid if dspRunning)
} DspEqStatus;

void DspEq_GetStatus(DspEqStatus *status);

// Diagnostic: writes marker patterns into candidate DSP memory blocks and counts how often each one gets changed
// (by the DSP or the audio library) while the console is used. See DspEq_MemTest*.
#define DSPEQ_MEMTEST_REGIONS 13
void DspEq_MemTestArm(void);
u16  DspEq_MemTestRegionAddr(u32 index);      // word address; the last region is in program memory
bool DspEq_MemTestRegionIsProgram(u32 index);
u32  DspEq_MemTestChanges(u32 index);         // number of times the marker was found changed (and restored)
bool DspEq_MemTestActive(void);
