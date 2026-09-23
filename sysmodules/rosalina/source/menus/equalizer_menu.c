
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

    do
    {
        Draw_Lock();
        Draw_DrawMenuFrame("Equalizer");

        u32 posY = 40;
        posY = Draw_DrawString(20, posY, COLOR_WHITE, "Up/down: select band, left/right: +-1 dB.\n");
        posY = Draw_DrawString(20, posY, COLOR_WHITE, "Hold R for +-6 dB steps, X to reset all bands.\n") + SPACING_Y;

        for (int i = 0; i < EQ_BAND_COUNT; i++)
        {
            u32 color = i == pos ? COLOR_CYAN : COLOR_WHITE;
            char bar[3 + 2 * EQ_BAR_CELLS + 1];
            EqualizerMenu_FormatBar(bar, equalizerGains[i]);
            posY = Draw_DrawFormattedString(20, posY, color, "%s %+3d dB %s\n", bandNames[i], equalizerGains[i], bar) + SPACING_Y;
        }

        posY += SPACING_Y;
        Draw_DrawString(20, posY, COLOR_WHITE, "Range: -24 dB to +24 dB. Press B to exit.\n");

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
