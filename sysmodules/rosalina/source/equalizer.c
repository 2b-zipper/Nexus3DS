
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

#include <3ds.h>
#include <math.h>
#include "equalizer.h"
#include "ifile.h"
#include "utils.h"

#define EQ_SAMPLE_RATE  32728.0f // DSP output rate
#define EQ_BASS_FREQ    200.0f   // low shelf corner
#define EQ_MIDS_FREQ    1000.0f  // peaking center
#define EQ_MIDS_Q       0.7071f
#define EQ_HIGHS_FREQ   4000.0f  // high shelf corner
#define EQ_SHELF_S      1.0f     // shelf slope

s8 equalizerGains[EQ_BAND_COUNT];

void Equalizer_SetGain(EqBand band, int gainDb)
{
    if (band < 0 || band >= EQ_BAND_COUNT)
        return;
    equalizerGains[band] = (s8)CLAMP(gainDb, EQ_GAIN_MIN, EQ_GAIN_MAX);
}

bool Equalizer_IsFlat(void)
{
    for (int i = 0; i < EQ_BAND_COUNT; i++)
        if (equalizerGains[i] != 0)
            return false;
    return true;
}

void Equalizer_Reset(void)
{
    for (int i = 0; i < EQ_BAND_COUNT; i++)
        equalizerGains[i] = 0;
}

// Coefficients from the RBJ Audio EQ Cookbook
EqBiquad Equalizer_GetBiquad(EqBand band)
{
    static const EqBiquad flat = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f};

    if (band < 0 || band >= EQ_BAND_COUNT || equalizerGains[band] == 0)
        return flat;

    float A = powf(10.0f, equalizerGains[band] / 40.0f);
    float b0, b1, b2, a0, a1, a2;

    if (band == EQ_BAND_MIDS)
    {
        float w0 = 2.0f * (float)M_PI * EQ_MIDS_FREQ / EQ_SAMPLE_RATE;
        float alpha = sinf(w0) / (2.0f * EQ_MIDS_Q);
        float cw = cosf(w0);

        b0 = 1.0f + alpha * A;
        b1 = -2.0f * cw;
        b2 = 1.0f - alpha * A;
        a0 = 1.0f + alpha / A;
        a1 = -2.0f * cw;
        a2 = 1.0f - alpha / A;
    }
    else
    {
        bool low = band == EQ_BAND_BASS;
        float w0 = 2.0f * (float)M_PI * (low ? EQ_BASS_FREQ : EQ_HIGHS_FREQ) / EQ_SAMPLE_RATE;
        float cw = cosf(w0);
        float alpha = sinf(w0) / 2.0f * sqrtf((A + 1.0f / A) * (1.0f / EQ_SHELF_S - 1.0f) + 2.0f);
        float tsa = 2.0f * sqrtf(A) * alpha;

        if (low)
        {
            b0 = A * ((A + 1.0f) - (A - 1.0f) * cw + tsa);
            b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cw);
            b2 = A * ((A + 1.0f) - (A - 1.0f) * cw - tsa);
            a0 = (A + 1.0f) + (A - 1.0f) * cw + tsa;
            a1 = -2.0f * ((A - 1.0f) + (A + 1.0f) * cw);
            a2 = (A + 1.0f) + (A - 1.0f) * cw - tsa;
        }
        else
        {
            b0 = A * ((A + 1.0f) + (A - 1.0f) * cw + tsa);
            b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cw);
            b2 = A * ((A + 1.0f) + (A - 1.0f) * cw - tsa);
            a0 = (A + 1.0f) - (A - 1.0f) * cw + tsa;
            a1 = 2.0f * ((A - 1.0f) - (A + 1.0f) * cw);
            a2 = (A + 1.0f) - (A - 1.0f) * cw - tsa;
        }
    }

    EqBiquad out = {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
    return out;
}

#define EQ_CONFIG_PATH  "/luma/equalizer.bin"
#define EQ_CONFIG_MAGIC 0x31514545 // "EEQ1"

typedef struct EqConfigFile {
    u32 magic;
    s8 gains[EQ_BAND_COUNT];
    u8 pad;
} EqConfigFile;

Result Equalizer_SaveConfig(void)
{
    EqConfigFile cfg = { .magic = EQ_CONFIG_MAGIC };
    for (int i = 0; i < EQ_BAND_COUNT; i++)
        cfg.gains[i] = equalizerGains[i];

    IFile file;
    u64 total;
    Result res = IFile_Open(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""), fsMakePath(PATH_ASCII, EQ_CONFIG_PATH), FS_OPEN_CREATE | FS_OPEN_WRITE);
    if (R_SUCCEEDED(res))
        res = IFile_SetSize(&file, sizeof(cfg));
    if (R_SUCCEEDED(res))
        res = IFile_Write(&file, &total, &cfg, sizeof(cfg), 0);
    IFile_Close(&file);
    return res;
}

void Equalizer_LoadConfig(void)
{
    Equalizer_Reset();

    IFile file;
    u64 total;
    EqConfigFile cfg;
    Result res = IFile_Open(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""), fsMakePath(PATH_ASCII, EQ_CONFIG_PATH), FS_OPEN_READ);
    if (R_FAILED(res))
        return;

    res = IFile_Read(&file, &total, &cfg, sizeof(cfg));
    IFile_Close(&file);

    if (R_SUCCEEDED(res) && total == sizeof(cfg) && cfg.magic == EQ_CONFIG_MAGIC)
        for (int i = 0; i < EQ_BAND_COUNT; i++)
            Equalizer_SetGain((EqBand)i, cfg.gains[i]);
}
