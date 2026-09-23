
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
#include "menus/equalizer_menu.h"
#include "equalizer.h"
#include "dsp_eq.h"
#include "dsp_eq_code.h"
#include "draw.h"
#include "menu.h"

#define EQ_BAR_CELLS    12 // cells per side of the 0 dB marker (2 dB each)

static const char *const bandNames[EQ_BAND_COUNT] = { "Bass ", "Mids ", "Highs" };

static void EqualizerMenu_FormatBar(char *out, int gainDb)
{
    // "[   ####|      ]": cut fills left of the marker, boost fills right
    int cells = (gainDb < 0 ? -gainDb : gainDb) * EQ_BAR_CELLS / EQ_GAIN_MAX;

    out[0] = '[';
    for (int i = 0; i < EQ_BAR_CELLS; i++)
        out[1 + i] = (gainDb < 0 && i >= EQ_BAR_CELLS - cells) ? '#' : ' ';
    out[1 + EQ_BAR_CELLS] = '|';
    for (int i = 0; i < EQ_BAR_CELLS; i++)
        out[2 + EQ_BAR_CELLS + i] = (gainDb > 0 && i < cells) ? '#' : ' ';
    out[2 + 2 * EQ_BAR_CELLS] = ']';
    out[3 + 2 * EQ_BAR_CELLS] = '\0';
}

static void EqualizerMenu_MemTest(void)
{
    u32 input, held;
    if (!DspEq_MemTestActive()) // opening the screen again must not reset a running test
        DspEq_MemTestArm();

    do
    {
        Draw_Lock();
        Draw_DrawMenuFrame("DSP memory test");

        u32 posY = 40;
        posY = Draw_DrawString(20, posY, COLOR_WHITE, "Marker patterns are written to DSP memory.\n");
        posY = Draw_DrawString(20, posY, COLOR_WHITE, "Leave (B), use the console for a minute\n");
        posY = Draw_DrawString(20, posY, COLOR_WHITE, "(sounds, volume, apps), then come back here.\n");
        posY = Draw_DrawString(20, posY, COLOR_WHITE, "Y: restart (resets counts). Changes per block:\n") + SPACING_Y;

        for (u32 r = 0; r < DSPEQ_MEMTEST_REGIONS; r++)
        {
            u32 x = 20 + (r % 2) * 150;
            u32 y = posY + (r / 2) * SPACING_Y;
            u32 changes = DspEq_MemTestChanges(r);
            Draw_DrawFormattedString(x, y, changes ? COLOR_ORANGE : COLOR_GREEN, "%s%04x %s%lu", DspEq_MemTestRegionIsProgram(r) ? "P" : "D", DspEq_MemTestRegionAddr(r), changes ? "changed " : "ok ", changes);
        }

        Draw_FlushFramebuffer();
        Draw_Unlock();

        input = waitInputWithTimeoutEx(&held, 300);
        (void)held;
        if (input & KEY_Y)
            DspEq_MemTestArm();
    }
    while (!(input & KEY_B) && !menuShouldExit);
}

void EqualizerMenu_Show(void)
{
    int pos = 0;
    u32 input = 0, held = 0;
    bool dirty = false;

    do
    {
        Draw_Lock();
        Draw_DrawMenuFrame("Equalizer");

        u32 posY = 40;
        posY = Draw_DrawString(20, posY, COLOR_WHITE, "Up/down: select band, left/right: +-1 dB.\n");
        posY = Draw_DrawString(20, posY, COLOR_WHITE, "R: +-6 dB steps, X: reset, Y: DSP memory test.\n") + SPACING_Y;

        for (int i = 0; i < EQ_BAND_COUNT; i++)
        {
            u32 color = i == pos ? COLOR_CYAN : COLOR_WHITE;
            char bar[3 + 2 * EQ_BAR_CELLS + 1];
            EqualizerMenu_FormatBar(bar, equalizerGains[i]);
            posY = Draw_DrawFormattedString(20, posY, color, "%s %+3d dB %s\n", bandNames[i], equalizerGains[i], bar) + SPACING_Y;
        }

        posY += SPACING_Y;
        posY = Draw_DrawString(20, posY, COLOR_WHITE, "Range: -24 dB to +24 dB. Press B to exit.\n") + SPACING_Y;

        DspEqStatus st;
        DspEq_GetStatus(&st);
        const char *state = st.gaveUp ? "stopped (DSP kept undoing it)" : !st.dspRunning ? "DSP not running" :
            (st.hookA == DSPEQ_HOOK_A_NEW && st.hookB == DSPEQ_HOOK_B_NEW) ? (st.magic == DSPEQ_MAGIC ? "active" : "patched, bypassed") :
            (st.hookA == DSPEQ_HOOK_A_ORIG && st.hookB == DSPEQ_HOOK_B_ORIG) ? "not applied yet" : "unsupported DSP firmware";
        posY = Draw_DrawFormattedString(20, posY, COLOR_GRAY, "DSP: %-24s\n", state);
        Draw_DrawFormattedString(20, posY, COLOR_GRAY, "pdn %02x hook %04x/%04x prm %04x i%lu f%lu\n", st.pdnDspCnt, st.hookA, st.hookB, st.magic, st.installs, st.fixes);
        posY = Draw_DrawFormattedString(20, posY, COLOR_GRAY, "dsp: calls %u (%lu/s) r4 %04x A%u B%u\n", st.diagCalls, st.callsPerSec, st.diagR4, st.diagA, st.diagB);

        Draw_FlushFramebuffer();
        Draw_Unlock();

        input = waitInputWithTimeoutEx(&held, -1);
        int step = (held & KEY_R) ? 6 : 1;

        if (input & KEY_UP)
            pos = (pos + EQ_BAND_COUNT - 1) % EQ_BAND_COUNT;
        if (input & KEY_DOWN)
            pos = (pos + 1) % EQ_BAND_COUNT;
        if (input & KEY_LEFT)
        {
            Equalizer_SetGain((EqBand)pos, equalizerGains[pos] - step);
            dirty = true;
            DspEq_NotifyChanged();
        }
        if (input & KEY_RIGHT)
        {
            Equalizer_SetGain((EqBand)pos, equalizerGains[pos] + step);
            dirty = true;
            DspEq_NotifyChanged();
        }
        if (input & KEY_Y)
            EqualizerMenu_MemTest();
        if (input & KEY_X)
        {
            Equalizer_Reset();
            dirty = true;
            DspEq_NotifyChanged();
        }
    }
    while (!(input & (KEY_A | KEY_B)) && !menuShouldExit);

    if (dirty)
        Equalizer_SaveConfig();
}
