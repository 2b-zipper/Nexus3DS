
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

#include <3ds/types.h>

#define EQ_BAND_COUNT   3
#define EQ_GAIN_MIN     (-24)
#define EQ_GAIN_MAX     24

typedef enum EqBand {
    EQ_BAND_BASS = 0,
    EQ_BAND_MIDS = 1,
    EQ_BAND_HIGHS = 2,
} EqBand;

// Normalized (a0 = 1) biquad coefficients, direct form I: y = b0 x + b1 x1 + b2 x2 - a1 y1 - a2 y2
typedef struct EqBiquad {
    float b0, b1, b2, a1, a2;
} EqBiquad;

// Per-band gain in dB, each in [EQ_GAIN_MIN, EQ_GAIN_MAX]
extern s8 equalizerGains[EQ_BAND_COUNT];

void Equalizer_SetGain(EqBand band, int gainDb);
bool Equalizer_IsFlat(void);
void Equalizer_Reset(void);

// Computes the filter coefficients of a band for the current gain
EqBiquad Equalizer_GetBiquad(EqBand band);

// Coefficients in the layout the DSP routine expects: per band [b0, b1, b2, -a1, -a2, -a1, -a2], Q12
#define EQ_DSP_WORDS_PER_BAND   7
void Equalizer_GetDspWords(u16 out[EQ_BAND_COUNT][EQ_DSP_WORDS_PER_BAND]);

Result Equalizer_SaveConfig(void);

// Loads the saved gains unless they were loaded/changed already. Does nothing if the file cannot be read (yet).
void Equalizer_LoadConfig(void);
// true once saved settings were loaded or the gains were changed by the user
bool Equalizer_ConfigDone(void);
