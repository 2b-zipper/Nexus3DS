
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
#define DSPEQ_LOAD_ATTEMPTS 1200                 // 2 minutes of retries
#define DSPEQ_GIVEUP_EVENTS 5                    // this many installs / lost parameter blocks ...
#define DSPEQ_GIVEUP_WINDOW (30LL * SYSCLOCK_ARM11) // ... within this many ticks make Rosalina stop patching
#define DSPEQ_PARAM_COEFS   0x10
#define DSPEQ_PARAM_STATES  0x40
#define DSPEQ_STATE_WORDS   48                   // 3 bands x 2 channels x 8

static MyThread dspEqThread;
static u8 CTR_ALIGN(8) dspEqThreadStack[0x2000];

static volatile bool dspEqDirty = true;

// Safety net: if the DSP keeps reloading or the parameter block keeps getting lost (i.e. the patch is not
// behaving), stop patching instead of disturbing the audio over and over. Changing the gains re-arms it.
static volatile bool dspEqGaveUp;
static u32 dspEqEvents;
static u64 dspEqEventWindowStart;
static u32 dspEqInstalls, dspEqFixes;

static void DspEq_CountEvent(void)
{
    u64 now = svcGetSystemTick();

    if (dspEqEvents == 0 || now - dspEqEventWindowStart > (u64)DSPEQ_GIVEUP_WINDOW)
    {
        dspEqEventWindowStart = now;
        dspEqEvents = 0;
    }

    if (++dspEqEvents >= DSPEQ_GIVEUP_EVENTS)
        dspEqGaveUp = true;
}

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

// Firmware as loaded: the code around the two hooked calls is exactly what the patch was written for
static bool DspEq_FirmwareMatches(volatile u16 *prog)
{
    for (u32 i = 0; i < sizeof(dspEqFingerprint) / sizeof(dspEqFingerprint[0]); i++)
        if (prog[DSPEQ_FINGERPRINT_ADDR + i] != dspEqFingerprint[i])
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
    u16 hookA = prog[DSPEQ_HOOK_A_ADDR];
    u16 hookB = prog[DSPEQ_HOOK_B_ADDR];

    if (hookA == DSPEQ_HOOK_A_ORIG && hookB == DSPEQ_HOOK_B_ORIG)
    {
        // Firmware freshly (re)loaded, not patched
        if (!wantEq || dspEqGaveUp || !DspEq_FirmwareMatches(prog) || !DspEq_CodeAreaIsFree(prog))
            return;

        DspEq_CountEvent();
        dspEqInstalls++;

        for (u32 i = 0; i < DSPEQ_CODE_WORDS; i++)
            prog[DSPEQ_CODE_BASE + i] = dspEqCode[i];
        __dsb();
        DspEq_WriteParams(data, true, true);
        // Each hook is a single 16-bit write to the operand of a call, so the running DSP never sees a half-patched instruction
        prog[DSPEQ_HOOK_A_ADDR] = DSPEQ_HOOK_A_NEW;
        prog[DSPEQ_HOOK_B_ADDR] = DSPEQ_HOOK_B_NEW;
        __dsb();
        dspEqDirty = false;
    }
    else if (hookA == DSPEQ_HOOK_A_NEW && hookB == DSPEQ_HOOK_B_NEW && (dirty || data[DSPEQ_DATA_BASE] != (wantEq ? DSPEQ_MAGIC : 0)))
    {
        // Patched, settings changed (or the parameter block was lost)
        bool lost = wantEq && data[DSPEQ_DATA_BASE] != DSPEQ_MAGIC;
        if (lost)
        {
            DspEq_CountEvent();
            dspEqFixes++;
            if (dspEqGaveUp)
            {
                data[DSPEQ_DATA_BASE] = 0; // leave the DSP in bypass
                __dsb();
                return;
            }
        }
        DspEq_WriteParams(data, wantEq, lost);
        dspEqDirty = false;
    }
}

static void DspEq_ThreadMain(void)
{
    // The SD card may not be available yet this early in boot: keep trying to load the saved settings for a while
    u32 loadAttempts = 0;

    while (!preTerminationRequested)
    {
        svcSleepThread(DSPEQ_POLL_NS);
        Sleep__Status(); // waits while the console sleeps

        if (!Equalizer_ConfigDone() && loadAttempts++ < DSPEQ_LOAD_ATTEMPTS)
        {
            Equalizer_LoadConfig();
            if (Equalizer_ConfigDone())
                dspEqDirty = true;
        }

        DspEq_Tick();
    }
}

void DspEq_GetStatus(DspEqStatus *status)
{
    status->pdnDspCnt = PDN_DSP_CNT;
    status->dspRunning = DspEq_IsDspRunning();
    status->gaveUp = dspEqGaveUp;
    status->installs = dspEqInstalls;
    status->fixes = dspEqFixes;
    status->hookA = 0;
    status->hookB = 0;
    status->magic = 0;

    if (status->dspRunning)
    {
        volatile u16 *prog = (volatile u16 *)PA_PTR(DSP_RAM_BASE);
        volatile u16 *data = (volatile u16 *)PA_PTR(DSP_RAM_BASE + DSP_DATA_OFFSET);
        status->hookA = prog[DSPEQ_HOOK_A_ADDR];
        status->hookB = prog[DSPEQ_HOOK_B_ADDR];
        status->magic = data[DSPEQ_DATA_BASE];
    }
}

void DspEq_NotifyChanged(void)
{
    dspEqDirty = true;
    dspEqGaveUp = false;
    dspEqEvents = 0;
}

MyThread *DspEq_CreateThread(void)
{
    if (R_FAILED(MyThread_Create(&dspEqThread, DspEq_ThreadMain, dspEqThreadStack, sizeof(dspEqThreadStack), 0x30, CORE_SYSTEM)))
        svcBreak(USERBREAK_PANIC);
    return &dspEqThread;
}
