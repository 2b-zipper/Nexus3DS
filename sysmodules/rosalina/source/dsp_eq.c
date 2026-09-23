
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

#include <3ds.h>
#include "dsp_eq.h"
#include "dsp_eq_code.h"
#include "equalizer.h"
#include "csvc.h"
#include "menu.h"
#include "sleep.h"
#include "utils.h"

// The DSP mixes and outputs all audio itself, so the equalizer runs inside the DSP firmware (see tools/dsp_eq/):
// a small routine is written into free DSP program memory and the firmware's final-mix call is redirected to it.
// The dsp module rewrites DSP memory whenever it (re)loads the firmware, so this is checked periodically.
// Nothing is touched while all gains are 0 dB and no patch is installed.

#define DSP_RAM_BASE        0x1FF00000u
#define DSP_DATA_OFFSET     0x40000u
// Physical memory is accessible through PA_PTR() (the kernel extension maps it uncached at PA | 1 << 31)
#define PDN_DSP_CNT         (*(vu8 *)PA_PTR(0x10141230)) // bit 0: 0 = reset, bit 1: clock enable

#define DSPEQ_POLL_NS       100000000LL
#define DSPEQ_PARAM_COEFS   0x10
#define DSPEQ_PARAM_STATES  0x40
#define DSPEQ_STATE_WORDS   48                   // 3 bands x 2 channels x 8

static MyThread dspEqThread;
static u8 CTR_ALIGN(8) dspEqThreadStack[0x1000];

static volatile bool dspEqDirty = true;

// The DSP is running (clock on, not in reset) - only then it is safe to access DSP memory
static bool DspEq_IsDspRunning(void)
{
    return (PDN_DSP_CNT & 3) == 3;
}

static void DspEq_WriteParams(volatile u16 *data, bool wantEq, bool resetStates)
{
    volatile u16 *block = data + DSPEQ_DATA_BASE;

    if (resetStates)
    {
        block[0] = 0; // bypass while the block is rewritten
        __dsb();
        for (u32 i = 0; i < DSPEQ_STATE_WORDS; i++)
            block[DSPEQ_PARAM_STATES + i] = 0;
    }

    u16 coefs[EQ_BAND_COUNT][EQ_DSP_WORDS_PER_BAND];
    Equalizer_GetDspWords(coefs);
    for (u32 band = 0; band < EQ_BAND_COUNT; band++)
        for (u32 i = 0; i < EQ_DSP_WORDS_PER_BAND; i++)
            block[DSPEQ_PARAM_COEFS + 8 * band + i] = coefs[band][i];

    __dsb();
    block[0] = wantEq ? DSPEQ_MAGIC : 0;
    __dsb();
}

static bool DspEq_FirmwareMatches(volatile u16 *prog)
{
    for (u32 i = 0; i < 9; i++)
        if (prog[DSPEQ_HOOK_ADDR - 8 + i] != dspEqHookFingerprint[i])
            return false;
    return true;
}

static bool DspEq_CodeAreaIsFree(volatile u16 *prog)
{
    for (u32 i = 0; i < DSPEQ_CODE_WORDS; i++)
        if (prog[DSPEQ_CODE_BASE + i] != 0 && prog[DSPEQ_CODE_BASE + i] != dspEqCode[i])
            return false;
    return true;
}

static void DspEq_Tick(void)
{
    bool wantEq = !Equalizer_IsFlat();
    bool dirty = dspEqDirty;

    if (!DspEq_IsDspRunning())
        return;

    volatile u16 *prog = (volatile u16 *)PA_PTR(DSP_RAM_BASE);
    volatile u16 *data = (volatile u16 *)PA_PTR(DSP_RAM_BASE + DSP_DATA_OFFSET);
    u16 hook = prog[DSPEQ_HOOK_ADDR];

    if (hook == DSPEQ_ORIG_TARGET)
    {
        // Firmware freshly (re)loaded, not patched
        if (!wantEq || !DspEq_FirmwareMatches(prog) || !DspEq_CodeAreaIsFree(prog))
            return;

        for (u32 i = 0; i < DSPEQ_CODE_WORDS; i++)
            prog[DSPEQ_CODE_BASE + i] = dspEqCode[i];
        __dsb();
        DspEq_WriteParams(data, true, true);
        prog[DSPEQ_HOOK_ADDR] = DSPEQ_CODE_BASE; // activates the routine
        __dsb();
        dspEqDirty = false;
    }
    else if (hook == DSPEQ_CODE_BASE && (dirty || data[DSPEQ_DATA_BASE] != (wantEq ? DSPEQ_MAGIC : 0)))
    {
        // Patched, settings changed (or the parameter block was lost)
        DspEq_WriteParams(data, wantEq, wantEq && data[DSPEQ_DATA_BASE] != DSPEQ_MAGIC);
        dspEqDirty = false;
    }
}

static void DspEq_ThreadMain(void)
{
    while (!preTerminationRequested)
    {
        svcSleepThread(DSPEQ_POLL_NS);
        Sleep__Status(); // waits while the console sleeps
        DspEq_Tick();
    }
}

void DspEq_GetStatus(DspEqStatus *status)
{
    status->pdnDspCnt = PDN_DSP_CNT;
    status->dspRunning = DspEq_IsDspRunning();
    status->hookWord = 0;
    status->magic = 0;

    if (status->dspRunning)
    {
        volatile u16 *prog = (volatile u16 *)PA_PTR(DSP_RAM_BASE);
        volatile u16 *data = (volatile u16 *)PA_PTR(DSP_RAM_BASE + DSP_DATA_OFFSET);
        status->hookWord = prog[DSPEQ_HOOK_ADDR];
        status->magic = data[DSPEQ_DATA_BASE];
    }
}

void DspEq_NotifyChanged(void)
{
    dspEqDirty = true;
}

MyThread *DspEq_CreateThread(void)
{
    if (R_FAILED(MyThread_Create(&dspEqThread, DspEq_ThreadMain, dspEqThreadStack, sizeof(dspEqThreadStack), 0x30, CORE_SYSTEM)))
        svcBreak(USERBREAK_PANIC);
    return &dspEqThread;
}
