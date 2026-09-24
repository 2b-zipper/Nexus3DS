
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
#include <math.h>
#include "equalizer.h"
#include "ifile.h"
#include "luma_config.h"
#include "utils.h"

#define EQ_SAMPLE_RATE  32728.0f // DSP output rate
#define EQ_BASS_MID_HZ  200.0f   // bass shelf midpoint (half of the total gain, in dB)
#define EQ_MIDS_HZ      1000.0f  // peaking filter center
#define EQ_MIDS_Q       0.7071f
#define EQ_HIGH_MID_HZ  4000.0f  // high shelf midpoint
#define EQ_DSP_QBITS    12       // fractional bits of the DSP coefficients

static s8 equalizerGains[EQ_PROFILE_COUNT][EQ_BAND_COUNT];
static bool equalizerUserChanged; // the gains were changed from the menu (so an old settings file must not overwrite them)

EqProfile Equalizer_CurrentOutput(void)
{
    return osIsHeadsetConnected() ? EQ_PROFILE_HEADPHONES : EQ_PROFILE_SPEAKERS;
}

const char *Equalizer_ProfileName(EqProfile profile)
{
    return profile == EQ_PROFILE_HEADPHONES ? "Headphones" : "Speakers";
}

s8 Equalizer_GetGain(EqProfile profile, EqBand band)
{
    if (profile >= EQ_PROFILE_COUNT || band >= EQ_BAND_COUNT)
        return 0;
    return equalizerGains[profile][band];
}

static void Equalizer_StoreGain(EqProfile profile, EqBand band, int gainDb)
{
    if (profile >= EQ_PROFILE_COUNT || band >= EQ_BAND_COUNT)
        return;
    equalizerGains[profile][band] = (s8)CLAMP(gainDb, EQ_GAIN_MIN, EQ_GAIN_MAX);
}

void Equalizer_SetGain(EqProfile profile, EqBand band, int gainDb)
{
    Equalizer_StoreGain(profile, band, gainDb);
    equalizerUserChanged = true;
}

bool Equalizer_IsFlat(EqProfile profile)
{
    for (int i = 0; i < EQ_BAND_COUNT; i++)
        if (Equalizer_GetGain(profile, (EqBand)i) != 0)
            return false;
    return true;
}

void Equalizer_Reset(EqProfile profile)
{
    for (int i = 0; i < EQ_BAND_COUNT; i++)
        Equalizer_SetGain(profile, (EqBand)i, 0);
}

// First-order shelf, expressed as a biquad with b2 = a2 = 0. The pole/zero pair is placed symmetrically around the
// midpoint frequency, so a shelf of X dB has X/2 dB of gain at the midpoint. Cuts are the inverse of the matching boost.
// (A second-order low shelf at 200 Hz would need more coefficient precision than the 16-bit DSP arithmetic gives.)
static EqBiquad Equalizer_DesignShelf(bool low, int gainDb, float midHz)
{
    float v0 = powf(10.0f, (gainDb < 0 ? -gainDb : gainDb) / 20.0f);
    float kMid = tanf((float)M_PI * midHz / EQ_SAMPLE_RATE);
    float k = low ? kMid / sqrtf(v0) : kMid * sqrtf(v0);
    float a1 = (k - 1.0f) / (k + 1.0f);
    float b0, b1;

    if (low)
    {
        b0 = (1.0f + v0 * k) / (1.0f + k);
        b1 = (v0 * k - 1.0f) / (1.0f + k);
    }
    else
    {
        b0 = (v0 + k) / (1.0f + k);
        b1 = (k - v0) / (1.0f + k);
    }

    EqBiquad out;
    if (gainDb < 0)
        out = (EqBiquad){1.0f / b0, a1 / b0, 0.0f, b1 / b0, 0.0f};
    else
        out = (EqBiquad){b0, b1, 0.0f, a1, 0.0f};
    return out;
}

// RBJ Audio EQ Cookbook peaking filter
static EqBiquad Equalizer_DesignPeak(int gainDb, float hz, float q)
{
    float a = powf(10.0f, gainDb / 40.0f);
    float w0 = 2.0f * (float)M_PI * hz / EQ_SAMPLE_RATE;
    float alpha = sinf(w0) / (2.0f * q);
    float c = cosf(w0);
    float a0 = 1.0f + alpha / a;

    return (EqBiquad){(1.0f + alpha * a) / a0, -2.0f * c / a0, (1.0f - alpha * a) / a0, -2.0f * c / a0, (1.0f - alpha / a) / a0};
}

EqBiquad Equalizer_GetBiquad(EqProfile profile, EqBand band)
{
    static const EqBiquad flat = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    int gain = Equalizer_GetGain(profile, band);

    if (band >= EQ_BAND_COUNT || gain == 0)
        return flat;

    switch (band)
    {
        case EQ_BAND_BASS:  return Equalizer_DesignShelf(true, gain, EQ_BASS_MID_HZ);
        case EQ_BAND_MIDS:  return Equalizer_DesignPeak(gain, EQ_MIDS_HZ, EQ_MIDS_Q);
        default:            return Equalizer_DesignShelf(false, gain, EQ_HIGH_MID_HZ);
    }
}

static u16 Equalizer_ToQ(float v)
{
    s32 r = (s32)floorf(v * (float)(1 << EQ_DSP_QBITS) + 0.5f);
    return (u16)(s16)CLAMP(r, -32768, 32767);
}

void Equalizer_GetDspWords(EqProfile profile, u16 out[EQ_BAND_COUNT][EQ_DSP_WORDS_PER_BAND])
{
    for (int band = 0; band < EQ_BAND_COUNT; band++)
    {
        EqBiquad f = Equalizer_GetBiquad(profile, (EqBand)band);
        out[band][0] = Equalizer_ToQ(f.b0);
        out[band][1] = Equalizer_ToQ(f.b1);
        out[band][2] = Equalizer_ToQ(f.b2);
        out[band][3] = out[band][5] = Equalizer_ToQ(-f.a1); // the DSP sums products, so it stores -a1/-a2 ...
        out[band][4] = out[band][6] = Equalizer_ToQ(-f.a2); // ... once for the high and once for the low half of y
    }
}

// The gains are stored in /luma/nexusconfig.ini ([equalizer] section) together with the other settings: the boot code parses
// them and hands them to the kernel extension, Rosalina reads them from there (svcGetSystemInfo) and writes the file back
// with LumaConfig_SaveSettings().

static void Equalizer_UnpackGains(EqProfile profile, s64 packed)
{
    for (int band = 0; band < EQ_BAND_COUNT; band++)
        Equalizer_StoreGain(profile, (EqBand)band, (s8)((packed >> (8 * band)) & 0xFF));
}

void Equalizer_LoadConfig(void)
{
    s64 speakers = 0, headphones = 0;

    if (R_SUCCEEDED(svcGetSystemInfo(&speakers, 0x10000, 0x188)))
        Equalizer_UnpackGains(EQ_PROFILE_SPEAKERS, speakers);
    if (R_SUCCEEDED(svcGetSystemInfo(&headphones, 0x10000, 0x189)))
        Equalizer_UnpackGains(EQ_PROFILE_HEADPHONES, headphones);
}

Result Equalizer_SaveConfig(void)
{
    LumaConfig_RequestSaveSettings(); // saved when the menu is left
    return 0;
}

// Earlier versions kept the gains in /luma/equalizer.bin. Import that file once, unless the ini already has settings.
#define EQ_LEGACY_PATH   "/luma/equalizer.bin"
#define EQ_LEGACY_MAGIC1 0x31514545 // "EEQ1": one set of gains
#define EQ_LEGACY_MAGIC2 0x32514545 // "EEQ2": one set per output

typedef struct EqLegacyFile {
    u32 magic;
    s8 gains[EQ_PROFILE_COUNT][EQ_BAND_COUNT];
    u8 pad[2];
} EqLegacyFile;

bool Equalizer_ImportLegacyConfig(void)
{
    IFile file;
    u64 total = 0;
    EqLegacyFile cfg = {0};
    Result res = IFile_Open(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""), fsMakePath(PATH_ASCII, EQ_LEGACY_PATH), FS_OPEN_READ);
    if (R_FAILED(res))
        return false; // no such file, or the SD card is not available yet

    res = IFile_Read(&file, &total, &cfg, sizeof(cfg));
    IFile_Close(&file);
    if (R_FAILED(res))
        return false;

    bool valid = (total == sizeof(cfg) && cfg.magic == EQ_LEGACY_MAGIC2) || (total >= 4 + EQ_BAND_COUNT && cfg.magic == EQ_LEGACY_MAGIC1);
    if (valid && !equalizerUserChanged && Equalizer_IsFlat(EQ_PROFILE_SPEAKERS) && Equalizer_IsFlat(EQ_PROFILE_HEADPHONES))
    {
        for (int p = 0; p < EQ_PROFILE_COUNT; p++)
            for (int i = 0; i < EQ_BAND_COUNT; i++)
                Equalizer_StoreGain((EqProfile)p, (EqBand)i, cfg.magic == EQ_LEGACY_MAGIC1 ? cfg.gains[0][i] : cfg.gains[p][i]);
        LumaConfig_RequestSaveSettings();
    }

    FS_Archive archive;
    if (R_SUCCEEDED(FSUSER_OpenArchive(&archive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""))))
    {
        FSUSER_DeleteFile(archive, fsMakePath(PATH_ASCII, EQ_LEGACY_PATH));
        FSUSER_CloseArchive(archive);
    }
    return valid;
}
