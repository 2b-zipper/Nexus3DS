
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

void EqualizerMenu_Show(void)
{
    int pos = 0;
    u32 input = 0, held = 0;
    bool dirty = false;
    bool showDiag = false;
    EqProfile editProfile = Equalizer_CurrentOutput();

    do
    {
        EqProfile inUse = Equalizer_CurrentOutput();

        Draw_Lock();
        Draw_DrawMenuFrame("Equalizer");

        u32 posY = 40;
        posY = Draw_DrawFormattedString(20, posY, COLOR_CYAN, "Settings for: %s%s\n", Equalizer_ProfileName(editProfile), editProfile == inUse ? " (in use)" : "");
        if (!showDiag)
        {
            posY = Draw_DrawString(20, posY, COLOR_WHITE, "L: switch speakers/headphones settings.\n");
            posY = Draw_DrawString(20, posY, COLOR_WHITE, "Up/down: band, left/right: +-1 dB, R: +-6 dB.\n");
            posY = Draw_DrawString(20, posY, COLOR_WHITE, "X: reset this output.  START: diagnostics.\n");
        }
        posY += SPACING_Y;

        for (int i = 0; i < EQ_BAND_COUNT; i++)
        {
            int gain = Equalizer_GetGain(editProfile, (EqBand)i);
            u32 color = i == pos ? COLOR_CYAN : COLOR_WHITE;
            char bar[3 + 2 * EQ_BAR_CELLS + 1];
            EqualizerMenu_FormatBar(bar, gain);
            posY = Draw_DrawFormattedString(20, posY, color, "%s %+3d dB %s\n", bandNames[i], gain, bar) + SPACING_Y;
        }

        posY += SPACING_Y;

        DspEqStatus st;
        DspEq_GetStatus(&st);
        const char *state = st.gaveUp ? "stopped (DSP kept undoing it)" : !st.dspRunning ? "DSP not running" :
            st.hookState == DSPEQ_HOOKS_PATCHED ? (st.magic == DSPEQ_MAGIC ? "active" : "patched, bypassed") :
            st.hookState == DSPEQ_HOOKS_ORIGINAL ? "not applied yet" :
            st.hookState == DSPEQ_HOOKS_UNSUPPORTED ? "unsupported DSP firmware" : "idle";
        posY = Draw_DrawFormattedString(20, posY, COLOR_GRAY, "DSP: %-24s\n", state);

        if (showDiag)
        {
            posY = Draw_DrawFormattedString(20, posY, COLOR_GRAY, "pdn %02x hook@%04lx %04x/%04x prm %04x i%lu f%lu\n", st.pdnDspCnt, st.hookAddrA, st.hookA, st.hookB, st.magic, st.installs, st.fixes);
            posY = Draw_DrawFormattedString(20, posY, COLOR_GRAY, "calls %u pk %lu/s r4 %04x A%u B%u\n", st.diagCalls, st.peakCallsPerSec, st.diagR4, st.diagA, st.diagB);
            posY = Draw_DrawFormattedString(20, posY, COLOR_GRAY, "t %04x %04x %04x %04x %04x\n", st.diagT[0], st.diagT[1], st.diagT[2], st.diagT[3], st.diagT[4]);
            posY = Draw_DrawFormattedString(20, posY, COLOR_GRAY, "t %04x %04x %04x %04x %04x  d%u/%u\n", st.diagT[5], st.diagT[6], st.diagT[7], st.diagT[8], st.diagT[9], st.droppedA, st.droppedB);
            posY = Draw_DrawFormattedString(20, posY, COLOR_GRAY, "cf %04x %04x %04x %04x %04x %04x %04x\n", st.diagCf[0], st.diagCf[1], st.diagCf[2], st.diagCf[3], st.diagCf[4], st.diagCf[5], st.diagCf[6]);
            Draw_DrawFormattedString(20, posY, COLOR_GRAY, "st %04x %04x %04x %04x %04x %04x %04x\n", st.diagSt[0], st.diagSt[1], st.diagSt[2], st.diagSt[3], st.diagSt[4], st.diagSt[5], st.diagSt[6]);
        }

        Draw_FlushFramebuffer();
        Draw_Unlock();

        input = waitInputWithTimeoutEx(&held, -1);
        int step = (held & KEY_R) ? 6 : 1;

        if (input & KEY_UP)
            pos = (pos + EQ_BAND_COUNT - 1) % EQ_BAND_COUNT;
        if (input & KEY_DOWN)
            pos = (pos + 1) % EQ_BAND_COUNT;
        if (input & KEY_L)
            editProfile = editProfile == EQ_PROFILE_SPEAKERS ? EQ_PROFILE_HEADPHONES : EQ_PROFILE_SPEAKERS;
        if (input & KEY_START)
            showDiag = !showDiag;
        if (input & (KEY_LEFT | KEY_RIGHT))
        {
            int dir = (input & KEY_RIGHT) ? 1 : -1;
            Equalizer_SetGain(editProfile, (EqBand)pos, Equalizer_GetGain(editProfile, (EqBand)pos) + dir * step);
            dirty = true;
            DspEq_NotifyChanged();
        }
        if (input & KEY_X)
        {
            Equalizer_Reset(editProfile);
            dirty = true;
            DspEq_NotifyChanged();
        }
    }
    while (!(input & (KEY_A | KEY_B)) && !menuShouldExit);

    if (dirty)
        Equalizer_SaveConfig();
}
