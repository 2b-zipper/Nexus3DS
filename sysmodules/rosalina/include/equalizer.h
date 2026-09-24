
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

// Separate settings for the two audio outputs; the one matching the output in use (headphones plugged in or not) is applied
typedef enum EqProfile {
    EQ_PROFILE_SPEAKERS = 0,
    EQ_PROFILE_HEADPHONES = 1,
    EQ_PROFILE_COUNT
} EqProfile;

// The profile of the audio output in use right now
EqProfile Equalizer_CurrentOutput(void);
const char *Equalizer_ProfileName(EqProfile profile);

// Per-band gain in dB, each in [EQ_GAIN_MIN, EQ_GAIN_MAX]
s8 Equalizer_GetGain(EqProfile profile, EqBand band);
void Equalizer_SetGain(EqProfile profile, EqBand band, int gainDb);
bool Equalizer_IsFlat(EqProfile profile);
void Equalizer_Reset(EqProfile profile);

// Computes the filter coefficients of a band for the profile's gain
EqBiquad Equalizer_GetBiquad(EqProfile profile, EqBand band);

// Coefficients in the layout the DSP routine expects: per band [b0, b1, b2, -a1, -a2, -a1, -a2], Q12
#define EQ_DSP_WORDS_PER_BAND   7
void Equalizer_GetDspWords(EqProfile profile, u16 out[EQ_BAND_COUNT][EQ_DSP_WORDS_PER_BAND]);

// Gains come from the kernel extension (parsed from /luma/nexusconfig.ini at boot) ...
void Equalizer_LoadConfig(void);
// ... and are written back with the other settings. Call after changing gains.
Result Equalizer_SaveConfig(void);

// One-time import of the old /luma/equalizer.bin (deleted afterwards). Returns true if the file was found. May be retried
// until the SD card is available.
bool Equalizer_ImportLegacyConfig(void);
