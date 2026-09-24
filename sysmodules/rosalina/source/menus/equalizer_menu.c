
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
#include <stdarg.h>
#include <string.h>
#include "menus/equalizer_menu.h"
#include "equalizer.h"
#include "dsp_eq.h"
#include "dsp_eq_code.h"
#include "draw.h"
#include "menu.h"
#include "fmt.h"

#define EQ_BAR_CELLS    12 // cells per side of the 0 dB marker (2 dB each)

// Fixed screen layout. Every line is always redrawn at the same place and padded with spaces to the full width of the text area,
// so a shorter text completely replaces a longer one (each character cell is drawn whole, glyph and background) and nothing has to
// be cleared first - clearing the screen makes it flash.
#define EQ_TEXT_X       20
#define EQ_LINE_CHARS   ((SCREEN_BOT_WIDTH - EQ_TEXT_X) / SPACING_X)
#define EQ_Y_PROFILE    40
#define EQ_Y_HELP       51   // three lines: help, or the firmware search result when the firmware is not recognised
#define EQ_Y_BANDS      95   // 22 pixels per band
#define EQ_Y_STATE      172
#define EQ_Y_DIAG       183  // four lines, only used when something is wrong

static void EqualizerMenu_Line(u32 y, u32 color, const char *fmt, ...)
{
    char buf[DRAW_MAX_FORMATTED_STRING_SIZE + 1];
    va_list args;
    va_start(args, fmt);
    vsprintf(buf, fmt, args);
    va_end(args);

    size_t len = strlen(buf);
    if (len > EQ_LINE_CHARS)
        len = EQ_LINE_CHARS;
    memset(buf + len, ' ', EQ_LINE_CHARS - len);
    buf[EQ_LINE_CHARS] = '\0';
    Draw_DrawString(EQ_TEXT_X, y, color, buf);
}

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
    EqProfile editProfile = Equalizer_CurrentOutput();

    do
    {
        EqProfile inUse = Equalizer_CurrentOutput();

        DspEqStatus st;
        DspEq_GetStatus(&st);
        // Diagnostics are only shown when something is wrong (the patch cannot be applied or keeps being undone)
        bool failed = st.gaveUp || st.codeBusy || (st.dspRunning && st.hookState == DSPEQ_HOOKS_UNSUPPORTED);

        Draw_Lock();
        Draw_DrawMenuFrame("Equalizer");

        EqualizerMenu_Line(EQ_Y_PROFILE, COLOR_CYAN, "Settings for: %s%s", Equalizer_ProfileName(editProfile), editProfile == inUse ? " (in use)" : "");

        if (!failed)
        {
            EqualizerMenu_Line(EQ_Y_HELP, COLOR_WHITE, "L: switch speakers/headphones settings.");
            EqualizerMenu_Line(EQ_Y_HELP + SPACING_Y, COLOR_WHITE, "Up/down: band, left/right: +-1 dB, R: +-6 dB.");
            EqualizerMenu_Line(EQ_Y_HELP + 2 * SPACING_Y, COLOR_WHITE, "X: reset this output.");
        }
        else if (st.hookState == DSPEQ_HOOKS_UNSUPPORTED)
        {
            EqualizerMenu_Line(EQ_Y_HELP, COLOR_GRAY, "search: best %04lx %lu/16 nz %lu", st.searchBestAddr, st.searchBestScore, st.searchNonZero);
            EqualizerMenu_Line(EQ_Y_HELP + SPACING_Y, COLOR_GRAY, "%04x %04x %04x %04x %04x %04x %04x %04x", st.searchBestWords[0], st.searchBestWords[1], st.searchBestWords[2], st.searchBestWords[3], st.searchBestWords[4], st.searchBestWords[5], st.searchBestWords[6], st.searchBestWords[7]);
            EqualizerMenu_Line(EQ_Y_HELP + 2 * SPACING_Y, COLOR_GRAY, "%04x %04x %04x %04x %04x %04x %04x %04x", st.searchBestWords[8], st.searchBestWords[9], st.searchBestWords[10], st.searchBestWords[11], st.searchBestWords[12], st.searchBestWords[13], st.searchBestWords[14], st.searchBestWords[15]);
        }
        else
        {
            for (u32 i = 0; i < 3; i++)
                EqualizerMenu_Line(EQ_Y_HELP + i * SPACING_Y, COLOR_GRAY, "");
        }

        for (int i = 0; i < EQ_BAND_COUNT; i++)
        {
            int gain = Equalizer_GetGain(editProfile, (EqBand)i);
            char bar[3 + 2 * EQ_BAR_CELLS + 1];
            EqualizerMenu_FormatBar(bar, gain);
            EqualizerMenu_Line(EQ_Y_BANDS + i * 2 * SPACING_Y, i == pos ? COLOR_CYAN : COLOR_WHITE, "%s %+3d dB %s", bandNames[i], gain, bar);
        }

        const char *state = st.gaveUp ? "stopped (DSP kept undoing it)" : st.codeBusy ? "DSP code area in use" : !st.dspRunning ? "DSP not running" :
            st.hookState == DSPEQ_HOOKS_PATCHED ? (st.magic == DSPEQ_MAGIC ? "active" : "patched, bypassed") :
            st.hookState == DSPEQ_HOOKS_ORIGINAL ? "not applied yet" :
            st.hookState == DSPEQ_HOOKS_UNSUPPORTED ? "unsupported DSP firmware" : "idle";
        EqualizerMenu_Line(EQ_Y_STATE, failed ? COLOR_ORANGE : COLOR_GRAY, "DSP: %s", state);

        if (failed)
        {
            EqualizerMenu_Line(EQ_Y_DIAG, COLOR_GRAY, "pdn %02x hook@%04lx %04x/%04x prm %04x i%lu f%lu", st.pdnDspCnt, st.hookAddrA, st.hookA, st.hookB, st.magic, st.installs, st.fixes);
            EqualizerMenu_Line(EQ_Y_DIAG + SPACING_Y, COLOR_GRAY, "calls %u pk %lu/s r4 %04x A%u B%u  d%u/%u", st.diagCalls, st.peakCallsPerSec, st.diagR4, st.diagA, st.diagB, st.droppedA, st.droppedB);
            EqualizerMenu_Line(EQ_Y_DIAG + 2 * SPACING_Y, COLOR_GRAY, "t %04x %04x %04x %04x %04x %04x %04x %04x %04x %04x", st.diagT[0], st.diagT[1], st.diagT[2], st.diagT[3], st.diagT[4], st.diagT[5], st.diagT[6], st.diagT[7], st.diagT[8], st.diagT[9]);
            EqualizerMenu_Line(EQ_Y_DIAG + 3 * SPACING_Y, COLOR_GRAY, "cf %04x %04x %04x %04x %04x %04x %04x", st.diagCf[0], st.diagCf[1], st.diagCf[2], st.diagCf[3], st.diagCf[4], st.diagCf[5], st.diagCf[6]);
        }
        else
        {
            for (u32 i = 0; i < 4; i++)
                EqualizerMenu_Line(EQ_Y_DIAG + i * SPACING_Y, COLOR_GRAY, "");
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
