
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
#define DSPEQ_LOAD_ATTEMPTS 120                  // once a second for 2 minutes
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
static u32 dspEqCallsPerSec, dspEqPeakCallsPerSec;
static bool dspEqMagicSet; // we last left the parameter block enabled (so a missing magic word means it was lost)

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

static void DspEq_WriteParams(volatile u16 *data, EqProfile profile, bool wantEq, bool resetStates)
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
    Equalizer_GetDspWords(profile, coefs);
    for (u32 band = 0; band < EQ_BAND_COUNT; band++)
        for (u32 i = 0; i < EQ_DSP_WORDS_PER_BAND; i++)
            block[DSPEQ_PARAM_COEFS + 8 * band + i] = coefs[band][i];

    __dsb();
    block[0] = wantEq ? DSPEQ_MAGIC : 0;
    __dsb();
}

// Where the two hooked calls are in the firmware that is currently loaded (found by searching for the call sequence, because
// the address differs between firmware versions)
typedef struct DspEqHooks {
    u32 patternAddr;    // program address of the matched call sequence
    u32 addrA, addrB;   // program addresses of the two call operands
    u16 origA, origB;   // their original values (the routines the calls target)
} DspEqHooks;

static DspEqHooks dspEqHooks;
static bool dspEqHooksValid;
static bool dspEqScanFailed;      // "unsupported": the search failed several times in a row
static u32 dspEqScanFailures;
static bool dspEqCodeBusy;        // the code area is occupied by something else, the patch cannot be installed
#define DSPEQ_SCAN_FAILURES_UNSUPPORTED 3 // a search can fail while the firmware is still being loaded
static u64 dspEqLastScan;

static bool DspEq_PatternAt(volatile u16 *prog, u32 addr)
{
    for (u32 i = 0; i < DSPEQ_PATTERN_WORDS; i++)
        if (dspEqPattern[i] != DSPEQ_PATTERN_ANY && prog[addr + i] != dspEqPattern[i])
            return false;
    return true;
}

// Closest partial match seen by the last failed search (diagnostics)
static u32 dspEqBestAddr, dspEqBestScore;
static u16 dspEqBestWords[DSPEQ_PATTERN_WORDS];
static u32 dspEqNonZeroWords; // program words 0x2000..0x7FFF that are not 0 (is a firmware loaded there at all?)

static bool DspEq_FindHooks(volatile u16 *prog, DspEqHooks *out)
{
    dspEqBestScore = 0;
    dspEqNonZeroWords = 0;

    for (u32 addr = DSPEQ_SCAN_FIRST; addr + DSPEQ_PATTERN_WORDS < DSPEQ_SCAN_LAST; addr++)
    {
        u16 first = prog[addr];
        if (first)
            dspEqNonZeroWords++;
        if (first != dspEqPattern[0])
            continue;

        // score every place that starts like the sequence, remember the closest one
        u32 score = 0;
        for (u32 i = 0; i < DSPEQ_PATTERN_WORDS; i++)
            if (dspEqPattern[i] == DSPEQ_PATTERN_ANY || prog[addr + i] == dspEqPattern[i])
                score++;
        if (score > dspEqBestScore)
        {
            dspEqBestScore = score;
            dspEqBestAddr = addr;
            for (u32 i = 0; i < DSPEQ_PATTERN_WORDS; i++)
                dspEqBestWords[i] = prog[addr + i];
        }

        if (score != DSPEQ_PATTERN_WORDS)
            continue;

        out->patternAddr = addr;
        out->addrA = addr + DSPEQ_PATTERN_A;
        out->addrB = addr + DSPEQ_PATTERN_B;
        out->origA = prog[out->addrA];
        out->origB = prog[out->addrB];
        return true;
    }
    return false;
}

// Both calls already redirected to the equalizer
static bool DspEq_HooksPatched(volatile u16 *prog)
{
    return dspEqHooksValid && prog[dspEqHooks.addrA] == DSPEQ_HOOK_A_NEW && prog[dspEqHooks.addrB] == DSPEQ_HOOK_B_NEW;
}

// The firmware as loaded, not patched (same place, same call sequence, same targets as when it was found)
static bool DspEq_HooksOriginal(volatile u16 *prog)
{
    return dspEqHooksValid && DspEq_PatternAt(prog, dspEqHooks.patternAddr)
        && prog[dspEqHooks.addrA] == dspEqHooks.origA && prog[dspEqHooks.addrB] == dspEqHooks.origB;
}

// The code area must be empty (or hold this routine from an earlier patch - the two call targets differ between firmware versions)
static bool DspEq_CodeAreaIsFree(volatile u16 *prog)
{
    for (u32 i = 0; i < DSPEQ_CODE_WORDS; i++)
    {
        if (i == DSPEQ_ENTRY_A_OPERAND || i == DSPEQ_ENTRY_B_OPERAND)
            continue;
        if (prog[DSPEQ_CODE_BASE + i] != 0 && prog[DSPEQ_CODE_BASE + i] != dspEqCode[i])
            return false;
    }
    return true;
}

static void DspEq_Tick(void)
{
    // The equalizer settings follow the audio output: plugging in / removing headphones switches the profile
    static EqProfile lastProfile = EQ_PROFILE_COUNT;
    EqProfile profile = Equalizer_CurrentOutput();
    bool wantEq = !Equalizer_IsFlat(profile);
    bool dirty = dspEqDirty;

    if (profile != lastProfile)
        dirty = true; // lastProfile is updated once the DSP has been given the new coefficients

    if (!DspEq_IsDspRunning())
        return;

    volatile u16 *prog = (volatile u16 *)PA_PTR(DSP_RAM_BASE);
    volatile u16 *data = (volatile u16 *)PA_PTR(DSP_RAM_BASE + DSP_DATA_OFFSET);

    bool patched = DspEq_HooksPatched(prog);
    bool original = !patched && DspEq_HooksOriginal(prog);

    if (!wantEq)
    {
        dspEqScanFailures = 0; // nothing is being tried, so nothing is "unsupported"
        dspEqScanFailed = false;
        dspEqCodeBusy = false;
    }

    if (!patched && !original)
    {
        // Nothing known about the firmware that is loaded now (first look, or another firmware was loaded): look for
        // the call sequence, but only if the equalizer is needed, and not more often than every 2 seconds
        dspEqHooksValid = false;
        u64 now = svcGetSystemTick();
        if (wantEq && !dspEqGaveUp && now - dspEqLastScan > 2 * (u64)SYSCLOCK_ARM11)
        {
            dspEqLastScan = now;
            dspEqHooksValid = DspEq_FindHooks(prog, &dspEqHooks);
            dspEqScanFailures = dspEqHooksValid ? 0 : dspEqScanFailures + 1;
            dspEqScanFailed = dspEqScanFailures >= DSPEQ_SCAN_FAILURES_UNSUPPORTED;
            original = dspEqHooksValid;
        }
    }

    if (original)
    {
        // Firmware freshly (re)loaded, not patched
        if (!wantEq || dspEqGaveUp)
            return;

        dspEqCodeBusy = !DspEq_CodeAreaIsFree(prog);
        if (dspEqCodeBusy)
            return;

        DspEq_CountEvent();
        dspEqInstalls++;

        for (u32 i = 0; i < DSPEQ_CODE_WORDS; i++)
            prog[DSPEQ_CODE_BASE + i] = dspEqCode[i];
        prog[DSPEQ_CODE_BASE + DSPEQ_ENTRY_A_OPERAND] = dspEqHooks.origA; // the entries call the original routines
        prog[DSPEQ_CODE_BASE + DSPEQ_ENTRY_B_OPERAND] = dspEqHooks.origB;
        __dsb();
        DspEq_WriteParams(data, profile, true, true);
        dspEqMagicSet = true;
        lastProfile = profile;
        // Each hook is a single 16-bit write to the operand of a call, so the running DSP never sees a half-patched instruction
        prog[dspEqHooks.addrA] = DSPEQ_HOOK_A_NEW;
        prog[dspEqHooks.addrB] = DSPEQ_HOOK_B_NEW;
        __dsb();
        dspEqDirty = false;
    }
    else if (patched && (dirty || data[DSPEQ_DATA_BASE] != (wantEq ? DSPEQ_MAGIC : 0)))
    {
        // Patched, settings changed (or the parameter block was lost)
        bool lost = wantEq && dspEqMagicSet && data[DSPEQ_DATA_BASE] != DSPEQ_MAGIC;
        bool enabling = wantEq && !dspEqMagicSet; // e.g. switching from a flat profile to an equalized one
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
        DspEq_WriteParams(data, profile, wantEq, lost || enabling);
        dspEqMagicSet = wantEq;
        lastProfile = profile;
        dspEqDirty = false;
    }
}

// Measures how fast the DSP routine's call counter advances (should be about 204 per second)
static void DspEq_MeasureRate(void)
{
    static u64 lastTick;
    static u16 lastCalls;
    u64 now = svcGetSystemTick();

    if (now - lastTick < (u64)SYSCLOCK_ARM11)
        return;

    dspEqCallsPerSec = 0;
    if (DspEq_IsDspRunning())
    {
        volatile u16 *prog = (volatile u16 *)PA_PTR(DSP_RAM_BASE);
        volatile u16 *data = (volatile u16 *)PA_PTR(DSP_RAM_BASE + DSP_DATA_OFFSET);
        if (DspEq_HooksPatched(prog))
        {
            u16 calls = data[DSPEQ_DATA_BASE + DSPEQ_DIAG_CALLS];
            dspEqCallsPerSec = (u16)(calls - lastCalls);
            if (dspEqCallsPerSec > dspEqPeakCallsPerSec && dspEqCallsPerSec < 1000)
                dspEqPeakCallsPerSec = dspEqCallsPerSec;
            lastCalls = calls;
        }
    }
    lastTick = now;
}

static void DspEq_ThreadMain(void)
{
    // The old settings file may only be readable once the SD card is available: keep trying for a while
    u32 legacyAttempts = 0, tick = 0;
    bool legacyDone = false;

    while (!preTerminationRequested)
    {
        svcSleepThread(DSPEQ_POLL_NS);
        Sleep__Status(); // waits while the console sleeps

        if (!legacyDone && tick++ % 10 == 0 && legacyAttempts++ < DSPEQ_LOAD_ATTEMPTS)
        {
            legacyDone = Equalizer_ImportLegacyConfig();
            if (legacyDone)
                dspEqDirty = true;
        }

        DspEq_Tick();
        DspEq_MeasureRate();
    }
}

void DspEq_GetStatus(DspEqStatus *status)
{
    status->searchBestAddr = dspEqBestAddr;
    status->searchBestScore = dspEqBestScore;
    status->searchNonZero = dspEqNonZeroWords;
    for (u32 i = 0; i < DSPEQ_PATTERN_WORDS; i++)
        status->searchBestWords[i] = dspEqBestWords[i];
    status->pdnDspCnt = PDN_DSP_CNT;
    status->dspRunning = DspEq_IsDspRunning();
    status->callsPerSec = dspEqCallsPerSec;
    status->peakCallsPerSec = dspEqPeakCallsPerSec;
    status->diagIdx0 = status->diagIdx1 = status->droppedA = status->droppedB = 0;
    for (u32 i = 0; i < 7; i++)
        status->diagCf[i] = status->diagSt[i] = 0;
    for (u32 i = 0; i < 8; i++)
        status->diagMod[i] = 0;
    for (u32 i = 0; i < 10; i++)
        status->diagT[i] = 0;
    status->gaveUp = dspEqGaveUp;
    status->codeBusy = dspEqCodeBusy;
    status->installs = dspEqInstalls;
    status->fixes = dspEqFixes;
    status->hookA = 0;
    status->hookB = 0;
    status->hookState = DSPEQ_HOOKS_UNKNOWN;
    status->hookAddrA = dspEqHooksValid ? dspEqHooks.addrA : 0;
    status->magic = 0;
    status->diagCalls = status->diagR4 = status->diagA = status->diagB = 0;

    if (status->dspRunning)
    {
        volatile u16 *prog = (volatile u16 *)PA_PTR(DSP_RAM_BASE);
        volatile u16 *data = (volatile u16 *)PA_PTR(DSP_RAM_BASE + DSP_DATA_OFFSET);
        if (dspEqHooksValid)
        {
            status->hookA = prog[dspEqHooks.addrA];
            status->hookB = prog[dspEqHooks.addrB];
        }
        status->hookState = DspEq_HooksPatched(prog) ? DSPEQ_HOOKS_PATCHED : DspEq_HooksOriginal(prog) ? DSPEQ_HOOKS_ORIGINAL :
            dspEqScanFailed ? DSPEQ_HOOKS_UNSUPPORTED : DSPEQ_HOOKS_UNKNOWN;
        status->magic = data[DSPEQ_DATA_BASE];
        status->diagCalls = data[DSPEQ_DATA_BASE + DSPEQ_DIAG_CALLS];
        status->diagR4 = data[DSPEQ_DATA_BASE + DSPEQ_DIAG_R4];
        status->diagA = data[DSPEQ_DATA_BASE + DSPEQ_DIAG_A];
        status->diagB = data[DSPEQ_DATA_BASE + DSPEQ_DIAG_B];
        status->diagIdx0 = data[DSPEQ_DATA_BASE + DSPEQ_DIAG_IDX0];
        status->diagIdx1 = data[DSPEQ_DATA_BASE + DSPEQ_DIAG_IDX1];
        status->droppedA = data[0x8401];    // DspStatus.dropped_frames of the two shared frame buffers
        status->droppedB = data[0x18401];
        for (u32 i = 0; i < 8; i++)
            status->diagMod[i] = data[DSPEQ_DATA_BASE + DSPEQ_DIAG_MOD + i];
        for (u32 i = 0; i < 10; i++)
            status->diagT[i] = data[DSPEQ_DATA_BASE + DSPEQ_DIAG_T + i];
        for (u32 i = 0; i < 7; i++)
        {
            status->diagCf[i] = data[DSPEQ_DATA_BASE + DSPEQ_DIAG_CF + i];
            status->diagSt[i] = data[DSPEQ_DATA_BASE + DSPEQ_DIAG_ST + i];
        }
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
