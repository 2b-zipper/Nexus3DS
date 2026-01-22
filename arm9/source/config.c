/*
*   This file is part of Nexus3DS
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

#define _GNU_SOURCE // for strchrnul

#include <assert.h>
#include <strings.h>
#include "config.h"
#include "memory.h"
#include "fs.h"
#include "utils.h"
#include "screen.h"
#include "draw.h"
#include "emunand.h"
#include "buttons.h"
#include "pin.h"
#include "i2c.h"
#include "ini.h"
#include "firm.h"

#include "config_template_ini.h" // note that it has an extra NUL byte inserted

#define MAKE_LUMA_VERSION_MCU(major, minor, build) (u16)(((major) & 0xFF) << 8 | ((minor) & 0x1F) << 5 | ((build) & 7))

#define FLOAT_CONV_MULT 100000000ll
#define FLOAT_CONV_PRECISION 8u

CfgData configData;
ConfigurationStatus needConfig;
static CfgData oldConfig;

static CfgDataMcu configDataMcu;
static_assert(sizeof(CfgDataMcu) > 0, "wrong data size");

// INI parsing
// ===========================================================

static const char *singleOptionIniNamesBoot[] = {
    "autoboot_emunand",
    "enable_external_firm_and_modules",
    "enable_game_patching",
    "app_syscore_threads_on_core_2",
    "show_system_settings_string",
    "show_gba_boot_screen",
    "use_dev_unitinfo",
    "disable_arm11_exception_handlers",
    "enable_safe_firm_rosalina",
    "instant_reboot_no_errdisp",
    "enable_sd_boot_time_patch",
};

static const char *keyNames[] = {
    "A", "B", "Select", "Start", "Right", "Left", "Up", "Down", "R", "L", "X", "Y",
    "?", "?",
    "ZL", "ZR",
    "?", "?", "?", "?",
    "Touch",
    "?", "?", "?",
    "CStick Right", "CStick Left", "CStick Up", "CStick Down",
    "CPad Right", "CPad Left", "CPad Up", "CPad Down",
};

static int parseBoolOption(bool *out, const char *val)
{
    *out = false;
    if (strlen(val) != 1) {
        return -1;
    }

    if (val[0] == '0') {
        return 0;
    } else if (val[0] == '1') {
        *out = true;
        return 0;
    } else {
        return -1;
    }
}

static int parseDecIntOptionImpl(s64 *out, const char *val, size_t numDigits, s64 minval, s64 maxval)
{
    *out = 0;
    s64 res = 0;
    size_t i = 0;

    s64 sign = 1;
    if (numDigits >= 2) {
        if (val[0] == '+') {
            ++i;
        } else if (val[0] == '-') {
            sign = -1;
            ++i;
        }
    }

    for (; i < numDigits; i++) {
        u64 n = (u64)(val[i] - '0');
        if (n > 9) {
            return -1;
        }

        res = 10*res + n;
    }

    res *= sign;
    if (res <= maxval && res >= minval) {
        *out = res;
        return 0;
    } else {
        return -1;
    }
}

static int parseDecIntOption(s64 *out, const char *val, s64 minval, s64 maxval)
{
    return parseDecIntOptionImpl(out, val, strlen(val), minval, maxval);
}

static int parseDecFloatOption(s64 *out, const char *val, s64 minval, s64 maxval)
{
    s64 sign = 1;// intPart < 0 ? -1 : 1;

    switch (val[0]) {
        case '\0':
            return -1;
        case '+':
            ++val;
            break;
        case '-':
            sign = -1;
            ++val;
            break;
        default:
            break;
    }

    // Reject "-" and "+"
    if (val[0] == '\0') {
        return -1;
    }

    char *point = strchrnul(val, '.');

    // Parse integer part, then fractional part
    s64 intPart = 0;
    s64 fracPart = 0;
    int rc = 0;

    if (point == val) {
        // e.g. -.5
        if (val[1] == '\0')
            return -1;
    }
    else {
        rc = parseDecIntOptionImpl(&intPart, val, point - val, INT64_MIN, INT64_MAX);
    }

    if (rc != 0) {
        return -1;
    }

    s64 intPartAbs = sign == -1 ? -intPart : intPart;
    s64 res = 0;
    bool of = __builtin_mul_overflow(intPartAbs, FLOAT_CONV_MULT, &res);

    if (of) {
        return -1;
    }

    s64 mul = FLOAT_CONV_MULT / 10;

    // Check if there's a fractional part
    if (point[0] != '\0' && point[1] != '\0') {
        for (char *pos = point + 1; *pos != '\0' && mul > 0; pos++) {
            if (*pos < '0' || *pos > '9') {
                return -1;
            }

            res += (*pos - '0') * mul;
            mul /= 10;
        }
    }


    res = sign * (res + fracPart);

    if (res <= maxval && res >= minval && !of) {
        *out = res;
        return 0;
    } else {
        return -1;
    }
}

static int parseHexIntOption(u64 *out, const char *val, u64 minval, u64 maxval)
{
    *out = 0;
    size_t numDigits = strlen(val);
    u64 res = 0;

    for (size_t i = 0; i < numDigits; i++) {
        char c = val[i];
        if ((u64)(c - '0') <= 9) {
            res = 16*res + (u64)(c - '0');
        } else if ((u64)(c - 'a') <= 5) {
            res = 16*res + (u64)(c - 'a' + 10);
        } else if ((u64)(c - 'A') <= 5) {
            res = 16*res + (u64)(c - 'A' + 10);
        } else {
            return -1;
        }
    }

    if (res <= maxval && res >= minval) {
        *out = res;
        return 0;
    } else {
        return -1;
    }
}

static int parseKeyComboOption(u32 *out, const char *val)
{
    const char *startpos = val;
    const char *endpos;

    *out = 0;
    u32 keyCombo = 0;
    do {
        // Copy the button name (note that 16 chars is longer than any of the key names)
        char name[17];
        endpos = strchr(startpos, '+');
        size_t n = endpos == NULL ? 16 : endpos - startpos;
        n = n > 16 ? 16 : n;
        strncpy(name, startpos, n);
        name[n] = '\0';

        if (strcmp(name, "?") == 0) {
            // Lol no, bail out
            return -1;
        }

        bool found = false;
        for (size_t i = 0; i < sizeof(keyNames)/sizeof(keyNames[0]); i++) {
            if (strcasecmp(keyNames[i], name) == 0) {
                found = true;
                keyCombo |= 1u << i;
            }
        }

        if (!found) {
            return -1;
        }

        if (endpos != NULL) {
            startpos = endpos + 1;
        }
    } while(endpos != NULL && *startpos != '\0');

    if (*startpos == '\0') {
        // Trailing '+'
        return -1;
    } else {
        *out = keyCombo;
        return 0;
    }
}

static void menuComboToString(char *out, u32 combo)
{
    char *outOrig = out;
    out[0] = 0;
    for(int i = 31; i >= 0; i--)
    {
        if(combo & (1 << i))
        {
            strcpy(out, keyNames[i]);
            out += strlen(keyNames[i]);
            *out++ = '+';
        }
    }

    if (out != outOrig)
        out[-1] = 0;
}

static int encodedFloatToString(char *out, s64 val)
{
    s64 sign = val >= 0 ? 1 : -1;

    s64 intPart = (sign * val) / FLOAT_CONV_MULT;
    s64 fracPart = (sign * val) % FLOAT_CONV_MULT;

    while (fracPart % 10 != 0) {
        // Remove trailing zeroes
        fracPart /= 10;
    }

    int n = sprintf(out, "%lld", sign * intPart);
    if (fracPart != 0) {
        n += sprintf(out + n, ".%0*lld", (int)FLOAT_CONV_PRECISION, fracPart);

        // Remove trailing zeroes
        int n2 = n - 1;
        while (out[n2] == '0') {
            out[n2--] = '\0';
        }

        n = n2;
    }

    return n;
}

static bool hasIniParseError = false;
static int iniParseErrorLine = 0;

#define CHECK_PARSE_OPTION(res) do { if((res) < 0) { hasIniParseError = true; iniParseErrorLine = lineno; return 0; } } while(false)

static int configIniHandler(void* user, const char* section, const char* name, const char* value, int lineno)
{
    CfgData *cfg = (CfgData *)user;
    if (strcmp(section, "meta") == 0) {
        if (strcmp(name, "config_version_major") == 0) {
            s64 opt;
            CHECK_PARSE_OPTION(parseDecIntOption(&opt, value, 0, 0xFFFF));
            cfg->formatVersionMajor = (u16)opt;
            return 1;
        } else if (strcmp(name, "config_version_minor") == 0) {
            s64 opt;
            CHECK_PARSE_OPTION(parseDecIntOption(&opt, value, 0, 0xFFFF));
            cfg->formatVersionMinor = (u16)opt;
            return 1;
        } else {
            CHECK_PARSE_OPTION(-1);
        }
    } else if (strcmp(section, "boot") == 0) {
        // Simple options displayed on the Nexus3DS boot screen
        for (size_t i = 0; i < sizeof(singleOptionIniNamesBoot)/sizeof(singleOptionIniNamesBoot[0]); i++) {
            if (strcmp(name, singleOptionIniNamesBoot[i]) == 0) {
                bool opt;
                CHECK_PARSE_OPTION(parseBoolOption(&opt, value));
                cfg->config |= (u32)opt << i;
                return 1;
            }
        }

        // Multi-choice options displayed on the Nexus3DS boot screen

        if (strcmp(name, "default_emunand_number") == 0) {
            s64 opt;
            CHECK_PARSE_OPTION(parseDecIntOption(&opt, value, 1, 4));
            cfg->multiConfig |= (opt - 1) << (2 * (u32)DEFAULTEMU);
            return 1;
        } else if (strcmp(name, "brightness_level") == 0) {
            s64 opt;
            CHECK_PARSE_OPTION(parseDecIntOption(&opt, value, 1, 4));
            cfg->multiConfig |= (4 - opt) << (2 * (u32)BRIGHTNESS);
            return 1;
        } else if (strcmp(name, "splash_position") == 0) {
            if (strcasecmp(value, "off") == 0) {
                cfg->multiConfig |= 0 << (2 * (u32)SPLASH);
                return 1;
            } else if (strcasecmp(value, "before payloads") == 0) {
                cfg->multiConfig |= 1 << (2 * (u32)SPLASH);
                return 1;
            } else if (strcasecmp(value, "after payloads") == 0) {
                cfg->multiConfig |= 2 << (2 * (u32)SPLASH);
                return 1;
            } else {
                CHECK_PARSE_OPTION(-1);
            }
        } else if (strcmp(name, "splash_duration_preset") == 0) {
            if (strcasecmp(value, "1s") == 0) {
                cfg->multiConfig |= 0 << (2 * (u32)SPLASHDURATION);
                return 1;
            } else if (strcasecmp(value, "3s") == 0) {
                cfg->multiConfig |= 1 << (2 * (u32)SPLASHDURATION);
                return 1;
            } else if (strcasecmp(value, "5s") == 0) {
                cfg->multiConfig |= 2 << (2 * (u32)SPLASHDURATION);
                return 1;
            } else if (strcasecmp(value, "custom") == 0) {
                cfg->multiConfig |= 3 << (2 * (u32)SPLASHDURATION);
                return 1;
            } else {
                CHECK_PARSE_OPTION(-1);
            }
        } else if (strcmp(name, "splash_duration_ms") == 0) {
            s64 opt;
            CHECK_PARSE_OPTION(parseDecIntOption(&opt, value, 0, 0xFFFFFFFFu));
            cfg->splashDurationMsec = (u32)opt;
            return 1;
        }
        else if (strcmp(name, "pin_lock_num_digits") == 0) {
            s64 opt;
            u32 encodedOpt;
            CHECK_PARSE_OPTION(parseDecIntOption(&opt, value, 0, 8));
            // Only allow for 0 (off), 4, 6 or 8 'digits'
            switch (opt) {
                case 0: encodedOpt = 0; break;
                case 4: encodedOpt = 1; break;
                case 6: encodedOpt = 2; break;
                case 8: encodedOpt = 3; break;
                default: {
                    CHECK_PARSE_OPTION(-1);
                }
            }
            cfg->multiConfig |= encodedOpt << (2 * (u32)PIN);
            return 1;
        } else if (strcmp(name, "app_launch_new_3ds_cpu") == 0) {
            if (strcasecmp(value, "off") == 0) {
                cfg->multiConfig |= 0 << (2 * (u32)NEWCPU);
                return 1;
            } else if (strcasecmp(value, "clock") == 0) {
                cfg->multiConfig |= 1 << (2 * (u32)NEWCPU);
                return 1;
            } else if (strcasecmp(value, "l2") == 0) {
                cfg->multiConfig |= 2 << (2 * (u32)NEWCPU);
                return 1;
            } else if (strcasecmp(value, "clock+l2") == 0) {
                cfg->multiConfig |= 3 << (2 * (u32)NEWCPU);
                return 1;
            } else {
                CHECK_PARSE_OPTION(-1);
            }
        } else if (strcmp(name, "autoboot_mode") == 0) {
            if (strcasecmp(value, "off") == 0) {
                cfg->multiConfig |= 0 << (2 * (u32)AUTOBOOTMODE);
                return 1;
            } else if (strcasecmp(value, "3ds") == 0) {
                cfg->multiConfig |= 1 << (2 * (u32)AUTOBOOTMODE);
                return 1;
            } else if (strcasecmp(value, "dsi") == 0) {
                cfg->multiConfig |= 2 << (2 * (u32)AUTOBOOTMODE);
                return 1;
            } else {
                CHECK_PARSE_OPTION(-1);
            }
        } else {
            CHECK_PARSE_OPTION(-1);
        }
    } else if (strcmp(section, "rosalina") == 0) {
        // Rosalina options
        if (strcmp(name, "hbldr_3dsx_titleid") == 0) {
            u64 opt;
            CHECK_PARSE_OPTION(parseHexIntOption(&opt, value, 0, 0xFFFFFFFFFFFFFFFFull));
            cfg->hbldr3dsxTitleId = opt;
            return 1;
        } else if (strcmp(name, "rosalina_menu_combo") == 0) {
            u32 opt;
            CHECK_PARSE_OPTION(parseKeyComboOption(&opt, value));
            cfg->rosalinaMenuCombo = opt;
            return 1;
        } else if (strcmp(name, "plugin_loader_enabled") == 0) {
            bool opt;
            CHECK_PARSE_OPTION(parseBoolOption(&opt, value));
            cfg->pluginLoaderFlags = opt ? cfg->pluginLoaderFlags | 1 : cfg->pluginLoaderFlags & ~1;
            return 1;
        } else if (strcmp(name, "plugin_watcher_enabled") == 0) {
            bool opt;
            CHECK_PARSE_OPTION(parseBoolOption(&opt, value));
            cfg->pluginLoaderFlags = opt ? cfg->pluginLoaderFlags | (1 << 1) : cfg->pluginLoaderFlags & ~(1 << 1);
            return 1;
        } else if (strcmp(name, "plugin_watcher_level") == 0) {
            s64 opt;
            CHECK_PARSE_OPTION(parseDecIntOption(&opt, value, 0, 0xFFFFFFFF));
            cfg->pluginWatcherLevel = (u32)opt;
            return 1;
        } else if (strcmp(name, "use_cache_in_plugin_converter") == 0) {
            bool opt;
            CHECK_PARSE_OPTION(parseBoolOption(&opt, value));
            cfg->pluginLoaderFlags = opt ? cfg->pluginLoaderFlags | (1 << 2) : cfg->pluginLoaderFlags & ~(1 << 2);
            return 1;
        } else if (strcmp(name, "ntp_tz_offset_min") == 0) {
            s64 opt;
            CHECK_PARSE_OPTION(parseDecIntOption(&opt, value, -779, 899));
            cfg->ntpTzOffetMinutes = (s16)opt;
            return 1;
        } else if (strcmp(name, "suppress_leds") == 0) {
            bool opt;
            CHECK_PARSE_OPTION(parseBoolOption(&opt, value));
            cfg->extraConfigFlags = opt ? cfg->extraConfigFlags | (1 << 0) : cfg->extraConfigFlags & ~(1 << 0);
            return 1;
        } else if (strcmp(name, "cut_slot_power") == 0) {
            bool opt;
            CHECK_PARSE_OPTION(parseBoolOption(&opt, value));
            cfg->extraConfigFlags = opt ? cfg->extraConfigFlags | (1 << 1) : cfg->extraConfigFlags & ~(1 << 1);
            return 1;
        } else if (strcmp(name, "cut_sleep_wifi") == 0) {
            bool opt;
            CHECK_PARSE_OPTION(parseBoolOption(&opt, value));
            cfg->extraConfigFlags = opt ? cfg->extraConfigFlags | (1 << 2) : cfg->extraConfigFlags & ~(1 << 2);
            return 1;
        } else if (strcmp(name, "screenshot_date_folders") == 0) {
            bool opt;
            CHECK_PARSE_OPTION(parseBoolOption(&opt, value));
            cfg->extraConfigFlags = opt ? cfg->extraConfigFlags | (1 << 3) : cfg->extraConfigFlags & ~(1 << 3);
            return 1;
        } else if (strcmp(name, "screenshot_combined") == 0) {
            bool opt;
            CHECK_PARSE_OPTION(parseBoolOption(&opt, value));
            cfg->extraConfigFlags = opt ? cfg->extraConfigFlags | (1 << 4) : cfg->extraConfigFlags & ~(1 << 4);
            return 1;
        } else if (strcmp(name, "toggle_lcd_combo") == 0) {
            bool opt;
            CHECK_PARSE_OPTION(parseBoolOption(&opt, value));
            cfg->extraConfigFlags = opt ? cfg->extraConfigFlags | (1 << 5) : cfg->extraConfigFlags & ~(1 << 5);
            return 1;
        } else if (strcmp(name, "temperature_unit_fahrenheit") == 0) {
            bool opt;
            CHECK_PARSE_OPTION(parseBoolOption(&opt, value));
            cfg->extraConfigFlags = opt ? cfg->extraConfigFlags | (1 << 6) : cfg->extraConfigFlags & ~(1 << 6);
            return 1;
        } else if (strcmp(name, "use_12_hour_clock") == 0) {
            bool opt;
            CHECK_PARSE_OPTION(parseBoolOption(&opt, value));
            cfg->extraConfigFlags = opt ? cfg->extraConfigFlags | (1 << 7) : cfg->extraConfigFlags & ~(1 << 7);
            return 1;
        } else {
            CHECK_PARSE_OPTION(-1);
        }
    } else if (strcmp(section, "screen_filters") == 0) {
        if (strcmp(name, "screen_filters_top_cct") == 0) {
            s64 opt;
            CHECK_PARSE_OPTION(parseDecIntOption(&opt, value, 1000, 25100));
            cfg->topScreenFilter.cct = (u32)opt;
            return 1;
        } else if (strcmp(name, "screen_filters_top_gamma") == 0) {
            s64 opt;
            CHECK_PARSE_OPTION(parseDecFloatOption(&opt, value, 0, 8 * FLOAT_CONV_MULT));
            cfg->topScreenFilter.gammaEnc = opt;
            return 1;
        } else if (strcmp(name, "screen_filters_top_contrast") == 0) {
            s64 opt;
            CHECK_PARSE_OPTION(parseDecFloatOption(&opt, value, 0, 255 * FLOAT_CONV_MULT));
            cfg->topScreenFilter.contrastEnc = opt;
            return 1;
        } else if (strcmp(name, "screen_filters_top_brightness") == 0) {
            s64 opt;
            CHECK_PARSE_OPTION(parseDecFloatOption(&opt, value, -1 * FLOAT_CONV_MULT, 1 * FLOAT_CONV_MULT));
            cfg->topScreenFilter.brightnessEnc = opt;
            return 1;
        } else if (strcmp(name, "screen_filters_top_invert") == 0) {
            bool opt;
            CHECK_PARSE_OPTION(parseBoolOption(&opt, value));
            cfg->topScreenFilter.invert = opt;
            return 1;
        } else if (strcmp(name, "screen_filters_top_color_curve_adj") == 0) {
            s64 opt;
            CHECK_PARSE_OPTION(parseDecIntOption(&opt, value, 0, 2));
            cfg->topScreenFilter.colorCurveCorrection = (u8)opt;
            return 1;
        } else if (strcmp(name, "screen_filters_bot_cct") == 0) {
            s64 opt;
            CHECK_PARSE_OPTION(parseDecIntOption(&opt, value, 1000, 25100));
            cfg->bottomScreenFilter.cct = (u32)opt;
            return 1;
        } else if (strcmp(name, "screen_filters_bot_gamma") == 0) {
            s64 opt;
            CHECK_PARSE_OPTION(parseDecFloatOption(&opt, value, 0, 8 * FLOAT_CONV_MULT));
            cfg->bottomScreenFilter.gammaEnc = opt;
            return 1;
        } else if (strcmp(name, "screen_filters_bot_contrast") == 0) {
            s64 opt;
            CHECK_PARSE_OPTION(parseDecFloatOption(&opt, value, 0, 255 * FLOAT_CONV_MULT));
            cfg->bottomScreenFilter.contrastEnc = opt;
            return 1;
        } else if (strcmp(name, "screen_filters_bot_brightness") == 0) {
            s64 opt;
            CHECK_PARSE_OPTION(parseDecFloatOption(&opt, value, -1 * FLOAT_CONV_MULT, 1 * FLOAT_CONV_MULT));
            cfg->bottomScreenFilter.brightnessEnc = opt;
            return 1;
        } else if (strcmp(name, "screen_filters_bot_invert") == 0) {
            bool opt;
            CHECK_PARSE_OPTION(parseBoolOption(&opt, value));
            cfg->bottomScreenFilter.invert = opt;
            return 1;
        } else if (strcmp(name, "screen_filters_bot_color_curve_adj") == 0) {
            s64 opt;
            CHECK_PARSE_OPTION(parseDecIntOption(&opt, value, 0, 2));
            cfg->bottomScreenFilter.colorCurveCorrection = (u8)opt;
            return 1;
        } else {
            CHECK_PARSE_OPTION(-1);
        }
    } else if (strcmp(section, "autoboot") == 0) {
        if (strcmp(name, "autoboot_dsi_titleid") == 0) {
            u64 opt;
            CHECK_PARSE_OPTION(parseHexIntOption(&opt, value, 0, 0xFFFFFFFFFFFFFFFFull));
            cfg->autobootTwlTitleId = opt;
            return 1;
        } else if (strcmp(name, "autoboot_3ds_app_mem_type") == 0) {
            s64 opt;
            CHECK_PARSE_OPTION(parseDecIntOption(&opt, value, 0, 4));
            cfg->autobootCtrAppmemtype = (u8)opt;
            return 1;
        } else {
            CHECK_PARSE_OPTION(-1);
        }
    } else if (strcmp(section, "misc") == 0) {
        if (strcmp(name, "force_audio_output") == 0) {
            if (strcasecmp(value, "off") == 0) {
                cfg->multiConfig |= 0 << (2 * (u32)FORCEAUDIOOUTPUT);
                return 1;
            } else if (strcasecmp(value, "headphones") == 0) {
                cfg->multiConfig |= 1 << (2 * (u32)FORCEAUDIOOUTPUT);
                return 1;
            } else if (strcasecmp(value, "speakers") == 0) {
                cfg->multiConfig |= 2 << (2 * (u32)FORCEAUDIOOUTPUT);
                return 1;
            } else {
                CHECK_PARSE_OPTION(-1);
            }
        } else if (strcmp(name, "volume_slider_override") == 0) {
            s64 opt;
            CHECK_PARSE_OPTION(parseDecIntOption(&opt, value, -1, 100));
            cfg->volumeSliderOverride = (s8)opt;
            return 1;
        } else if (strcmp(name, "hide_return_to_home_menu") == 0) {
            bool opt;
            CHECK_PARSE_OPTION(parseBoolOption(&opt, value));
            cfg->homeButtonSimFlags = opt ? cfg->homeButtonSimFlags | (1 << 0) : cfg->homeButtonSimFlags & ~(1 << 0);
            return 1;
        } else if (strcmp(name, "enable_home_button_combo") == 0) {
            bool opt;
            CHECK_PARSE_OPTION(parseBoolOption(&opt, value));
            cfg->homeButtonSimFlags = opt ? cfg->homeButtonSimFlags | (1 << 1) : cfg->homeButtonSimFlags & ~(1 << 1);
            return 1;
        } else if (strcmp(name, "home_button_combo") == 0) {
            u32 opt;
            CHECK_PARSE_OPTION(parseKeyComboOption(&opt, value));
            cfg->homeButtonCombo = opt;
            return 1;
        } else {
            CHECK_PARSE_OPTION(-1);
        }
    } else {
        CHECK_PARSE_OPTION(-1);
    }
}

static size_t saveLumaIniConfigToStr(char *out)
{
    const CfgData *cfg = &configData;

    char lumaVerStr[64];
    char lumaRevSuffixStr[16];
    char rosalinaMenuComboStr[128];
    char homeButtonComboStr[128];

    const char *splashPosStr;
    const char *splashDurationPresetStr;
    const char *n3dsCpuStr;
    const char *autobootModeStr;
    const char *forceAudioOutputStr;

    switch (MULTICONFIG(SPLASH)) {
        default: case 0: splashPosStr = "off"; break;
        case 1: splashPosStr = "before payloads"; break;
        case 2: splashPosStr = "after payloads"; break;
    }

    switch (MULTICONFIG(SPLASHDURATION)) {
        default: case 0: splashDurationPresetStr = "1s"; break;
        case 1: splashDurationPresetStr = "3s"; break;
        case 2: splashDurationPresetStr = "5s"; break;
        case 3: splashDurationPresetStr = "custom"; break;
    }

    switch (MULTICONFIG(NEWCPU)) {
        default: case 0: n3dsCpuStr = "off"; break;
        case 1: n3dsCpuStr = "clock"; break;
        case 2: n3dsCpuStr = "l2"; break;
        case 3: n3dsCpuStr = "clock+l2"; break;
    }

    switch (MULTICONFIG(AUTOBOOTMODE)) {
        default: case 0: autobootModeStr = "off"; break;
        case 1: autobootModeStr = "3ds"; break;
        case 2: autobootModeStr = "dsi"; break;
    }

    switch (MULTICONFIG(FORCEAUDIOOUTPUT)) {
        default: case 0: forceAudioOutputStr = "off"; break;
        case 1: forceAudioOutputStr = "headphones"; break;
        case 2: forceAudioOutputStr = "speakers"; break;
    }

    if (NEXUS_VERSION_BUILD != 0) {
        sprintf(lumaVerStr, "Nexus3DS v%d.%d.%d", (int)NEXUS_VERSION_MAJOR, (int)NEXUS_VERSION_MINOR, (int)NEXUS_VERSION_BUILD);
    } else {
        sprintf(lumaVerStr, "Nexus3DS v%d.%d", (int)NEXUS_VERSION_MAJOR, (int)NEXUS_VERSION_MINOR);
    }

    if (ISRELEASE) {
        strcpy(lumaRevSuffixStr, "");
    } else {
        sprintf(lumaRevSuffixStr, "-%08lx", (u32)COMMIT_HASH);
    }

    menuComboToString(rosalinaMenuComboStr, cfg->rosalinaMenuCombo);
    menuComboToString(homeButtonComboStr, cfg->homeButtonCombo);

    static const int pinOptionToDigits[] = { 0, 4, 6, 8 };
    int pinNumDigits = pinOptionToDigits[MULTICONFIG(PIN)];

    char topScreenFilterGammaStr[32];
    char topScreenFilterContrastStr[32];
    char topScreenFilterBrightnessStr[32];
    encodedFloatToString(topScreenFilterGammaStr, cfg->topScreenFilter.gammaEnc);
    encodedFloatToString(topScreenFilterContrastStr, cfg->topScreenFilter.contrastEnc);
    encodedFloatToString(topScreenFilterBrightnessStr, cfg->topScreenFilter.brightnessEnc);

    char bottomScreenFilterGammaStr[32];
    char bottomScreenFilterContrastStr[32];
    char bottomScreenFilterBrightnessStr[32];
    encodedFloatToString(bottomScreenFilterGammaStr, cfg->bottomScreenFilter.gammaEnc);
    encodedFloatToString(bottomScreenFilterContrastStr, cfg->bottomScreenFilter.contrastEnc);
    encodedFloatToString(bottomScreenFilterBrightnessStr, cfg->bottomScreenFilter.brightnessEnc);

    int n = sprintf(
        out, (const char *)config_template_ini,
        lumaVerStr, lumaRevSuffixStr,

        (int)CONFIG_VERSIONMAJOR, (int)CONFIG_VERSIONMINOR,
        (int)CONFIG(AUTOBOOTEMU), (int)CONFIG(LOADEXTFIRMSANDMODULES),
        (int)CONFIG(PATCHGAMES), (int)CONFIG(REDIRECTAPPTHREADS),
        (int)CONFIG(PATCHVERSTRING), (int)CONFIG(SHOWGBABOOT),
        (int)CONFIG(PATCHUNITINFO), (int)CONFIG(DISABLEARM11EXCHANDLERS),
        (int)CONFIG(ENABLESAFEFIRMROSALINA), (int)CONFIG(INSTANTREBOOTNOERRDISP),
        (int)CONFIG(ENABLESDBOOTTIMEPATCH),

        1 + (int)MULTICONFIG(DEFAULTEMU), 4 - (int)MULTICONFIG(BRIGHTNESS),
        splashPosStr, splashDurationPresetStr, (unsigned int)cfg->splashDurationMsec,
        pinNumDigits, n3dsCpuStr,
        autobootModeStr,

        cfg->hbldr3dsxTitleId, rosalinaMenuComboStr, (int)(cfg->pluginLoaderFlags & 1),
        (int)((cfg->pluginLoaderFlags & (1 << 1)) >> 1), (int)cfg->pluginWatcherLevel,
        (int)((cfg->pluginLoaderFlags & (1 << 2)) >> 2),
        (int)cfg->ntpTzOffetMinutes,

        (int)((cfg->extraConfigFlags >> 0) & 1),
        (int)((cfg->extraConfigFlags >> 1) & 1),
        (int)((cfg->extraConfigFlags >> 2) & 1),
        (int)((cfg->extraConfigFlags >> 3) & 1),
        (int)((cfg->extraConfigFlags >> 4) & 1),
        (int)((cfg->extraConfigFlags >> 5) & 1),
        (int)((cfg->extraConfigFlags >> 6) & 1),
        (int)((cfg->extraConfigFlags >> 7) & 1),

        (int)cfg->topScreenFilter.cct, (int)cfg->bottomScreenFilter.cct,
        (int)cfg->topScreenFilter.colorCurveCorrection, (int)cfg->bottomScreenFilter.colorCurveCorrection,
        topScreenFilterGammaStr, bottomScreenFilterGammaStr,
        topScreenFilterContrastStr, bottomScreenFilterContrastStr,
        topScreenFilterBrightnessStr, bottomScreenFilterBrightnessStr,
        (int)cfg->topScreenFilter.invert, (int)cfg->bottomScreenFilter.invert,

        cfg->autobootTwlTitleId, (int)cfg->autobootCtrAppmemtype,

        forceAudioOutputStr,
        cfg->volumeSliderOverride,
        (int)((cfg->homeButtonSimFlags >> 0) & 1),
        (int)((cfg->homeButtonSimFlags >> 1) & 1),
        homeButtonComboStr
    );

    return n < 0 ? 0 : (size_t)n;
}

static char tmpIniBuffer[0x2500];

static bool readLumaIniConfig(void)
{
    u32 rd = fileRead(tmpIniBuffer, "nexusconfig.ini", sizeof(tmpIniBuffer) - 1);
    if (rd == 0) return false;

    tmpIniBuffer[rd] = '\0';

    return ini_parse_string(tmpIniBuffer, &configIniHandler, &configData) >= 0 && !hasIniParseError;
}

static bool writeLumaIniConfig(void)
{
    size_t n = saveLumaIniConfigToStr(tmpIniBuffer);
    return n != 0 && fileWrite(tmpIniBuffer, "nexusconfig.ini", n);
}

// ===========================================================

static void writeConfigMcu(void)
{
    u8 data[sizeof(CfgDataMcu)];

    // Set Luma version
    configDataMcu.lumaVersion = MAKE_LUMA_VERSION_MCU(LUMA_VERSION_MAJOR, LUMA_VERSION_MINOR, LUMA_VERSION_BUILD);

    // Set bootconfig from CfgData
    configDataMcu.bootCfg = configData.bootConfig;

    memcpy(data, &configDataMcu, sizeof(CfgDataMcu));

    // Fix checksum
    u8 checksum = 0;
    for (u32 i = 0; i < sizeof(CfgDataMcu) - 1; i++)
        checksum += data[i];
    checksum = ~checksum;
    data[sizeof(CfgDataMcu) - 1] = checksum;
    configDataMcu.checksum = checksum;

    I2C_writeReg(I2C_DEV_MCU, 0x60, 200 - sizeof(CfgDataMcu));
    I2C_writeRegBuf(I2C_DEV_MCU, 0x61, data, sizeof(CfgDataMcu));
}

static bool readConfigMcu(void)
{
    u8 data[sizeof(CfgDataMcu)];
    u16 curVer = MAKE_LUMA_VERSION_MCU(LUMA_VERSION_MAJOR, LUMA_VERSION_MINOR, LUMA_VERSION_BUILD);

    // Select free reg id, then access the data regs
    I2C_writeReg(I2C_DEV_MCU, 0x60, 200 - sizeof(CfgDataMcu));
    I2C_readRegBuf(I2C_DEV_MCU, 0x61, data, sizeof(CfgDataMcu));
    memcpy(&configDataMcu, data, sizeof(CfgDataMcu));

    u8 checksum = 0;
    for (u32 i = 0; i < sizeof(CfgDataMcu) - 1; i++)
        checksum += data[i];
    checksum = ~checksum;

    if (checksum != configDataMcu.checksum || configDataMcu.lumaVersion < MAKE_LUMA_VERSION_MCU(10, 3, 0))
    {
        // Invalid data stored in MCU...
        memset(&configDataMcu, 0, sizeof(CfgDataMcu));
        configData.bootConfig = 0;
        // Perform upgrade process (ignoring failures)
        askForUpgradeProcess();
        writeConfigMcu();

        return false;
    }

    if (configDataMcu.lumaVersion < curVer)
    {
        // Perform upgrade process (ignoring failures)
        askForUpgradeProcess();
        writeConfigMcu();
    }

    return true;
}

bool readConfig(void)
{
    bool retMcu, ret;

    retMcu = readConfigMcu();
    ret = readLumaIniConfig();
    if(!retMcu || !ret ||
       configData.formatVersionMajor != CONFIG_VERSIONMAJOR ||
       configData.formatVersionMinor != CONFIG_VERSIONMINOR)
    {
        memset(&configData, 0, sizeof(CfgData));
        configData.formatVersionMajor = CONFIG_VERSIONMAJOR;
        configData.formatVersionMinor = CONFIG_VERSIONMINOR;
        configData.config |= 1u << PATCHVERSTRING;
        configData.multiConfig |= 3 << (2 * (u32)NEWCPU); // Default NEWCPU to Clock+L2
        configData.multiConfig |= 1 << (2 * (u32)SPLASHDURATION); // Default splash duration to 3s
        configData.splashDurationMsec = 7000;
        configData.volumeSliderOverride = -1;
        configData.hbldr3dsxTitleId = HBLDR_DEFAULT_3DSX_TID;
        configData.rosalinaMenuCombo = 1u << 9 | 1u << 7 | 1u << 2; // L+Start+Select
        configData.topScreenFilter.cct = 6500; // default temp, no-op
        configData.topScreenFilter.gammaEnc = 1 * FLOAT_CONV_MULT; // 1.0f
        configData.topScreenFilter.contrastEnc = 1 * FLOAT_CONV_MULT; // 1.0f
        configData.bottomScreenFilter = configData.topScreenFilter;
        configData.autobootTwlTitleId = AUTOBOOT_DEFAULT_TWL_TID;

        configData.extraConfigFlags = 0;
        configData.extraConfigFlags |= 1 << 3; // screenshot_date_folders  
        configData.extraConfigFlags |= 1 << 4; // screenshot_combined
        configData.homeButtonCombo = 1u << 2 | 1u << 8; // Select+R
        ret = false;
    }
    else
        ret = true;

    configData.bootConfig = configDataMcu.bootCfg;
    oldConfig = configData;

    return ret;
}

void askForUpgradeProcess(void)
{
    initScreens();

    drawString(true, 10, 10, COLOR_ORANGE, "Nexus3DS backup confirmation");
    drawString(true, 10, 10 + SPACING_Y * 2, COLOR_WHITE, "Do you want to install Nexus3DS to CTRNAND?\nThis enables you to boot without an sd card.");
    drawString(true, 10, 10 + SPACING_Y * 5, COLOR_WHITE, "Doing so will also backup essential files.");
    drawString(true, 10, 10 + SPACING_Y * 7, COLOR_ORANGE, "Press A to confirm, X to cancel.\nIf you're unsure, press A.");

    while (true) {
        u32 pressed = waitInput(false);

        if (pressed & (BUTTON_A | BUTTON_X)) {
            if (pressed & BUTTON_A)
                doLumaUpgradeProcess() ? drawString(true, 10, 10 + SPACING_Y * 10, COLOR_GREEN, "Backup complete!") :
                                         drawString(true, 10, 10 + SPACING_Y * 10, COLOR_RED, "Backup failed! Is your SD card corrupted?");
            break;
        }
    }
    wait(2000ULL);
}

u32 getSplashDurationMs(void)
{
    switch (MULTICONFIG(SPLASHDURATION)) {
        case 0: return 1000;
        case 1: return 3000;
        case 2: return 5000;
        case 3: return configData.splashDurationMsec;
        default: return 3000;
    }
}

void writeConfig(bool isConfigOptions)
{
    bool updateMcu, updateIni;

    if (needConfig == CREATE_CONFIGURATION)
    {
        updateMcu = !isConfigOptions; // We've already committed it once (if it wasn't initialized)
        updateIni = isConfigOptions;
        needConfig = MODIFY_CONFIGURATION;
    }
    else
    {
        updateMcu = !isConfigOptions && configData.bootConfig != oldConfig.bootConfig;
        updateIni = isConfigOptions && (configData.config != oldConfig.config || configData.multiConfig != oldConfig.multiConfig);
    }

    if (updateMcu)
        writeConfigMcu();

    if(updateIni && !writeLumaIniConfig())
        error("Error writing the configuration file");
}

static void drawConfigMenu(u32 *selectedOption, u32 *singleSelected,
                           u32 multiOptionsAmount, u32 singleOptionsAmount, u32 currentPage, 
                           struct multiOption *multiOptions, struct singleOption *singleOptions,
                           const char **multiOptionsText, const char **singleOptionsText,
                           const char **optionsDescription, const char *bootType)
{
    clearScreens(false);
    drawString(true, 10, 10, COLOR_ORANGE, CONFIG_TITLE);
    if(currentPage == 1) {
        drawString(true, 10, 10 + SPACING_Y, COLOR_ORANGE, "Press B to save and go back");
        drawString(true, 10, 20 + SPACING_Y, COLOR_RED, "These are expert options, use carefully!");
    } else {
        drawString(true, 10, 10 + SPACING_Y, COLOR_ORANGE, "Use the DPAD and A to change settings");
    }
    drawFormattedString(false, 10, SCREEN_HEIGHT - 2 * SPACING_Y, COLOR_YELLOW, "Booted from %s via %s", isSdMode ? "SD" : "CTRNAND", bootType);

    u32 endPos = 10 + 2 * SPACING_Y;

    //Display all the multiple choice options in white
    for(u32 i = 0; i < multiOptionsAmount; i++)
    {
        if(!multiOptions[i].visible || multiOptions[i].page != currentPage) continue;

        multiOptions[i].posY = endPos + SPACING_Y;
        endPos = drawString(true, 10, multiOptions[i].posY, COLOR_WHITE, multiOptionsText[i]);
        drawCharacter(true, 10 + multiOptions[i].posXs[multiOptions[i].enabled] * SPACING_X, multiOptions[i].posY, COLOR_WHITE, 'x');
    }

    endPos += SPACING_Y / 2;

    //Find and set the first available option, display all the normal options in white except for the first one
    for(u32 i = 0, color = COLOR_CYAN; i < singleOptionsAmount; i++)
    {
        if(!singleOptions[i].visible || singleOptions[i].page != currentPage) continue;

        singleOptions[i].posY = endPos + SPACING_Y;
        endPos = drawString(true, 10, singleOptions[i].posY, color, singleOptionsText[i]);
        if(singleOptions[i].enabled && singleOptionsText[i][0] == '(') drawCharacter(true, 10 + SPACING_X, singleOptions[i].posY, color, 'x');

        if(color == COLOR_CYAN) // first option found
        {
            color = COLOR_WHITE;
            *singleSelected = i;
            *selectedOption = i + multiOptionsAmount;
        }
    }

    drawString(false, 10, 10, COLOR_WHITE, optionsDescription[*selectedOption]);
}

void configMenu(bool oldPinStatus, u32 oldPinMode)
{
    static const char *multiOptionsText[]  = { "Default EmuNAND: 1( ) 2( ) 3( ) 4( )",
                                               "Screen brightness: 4( ) 3( ) 2( ) 1( )",
                                               "Splash: Off( ) Before( ) After( ) payloads",
                                               "Splash duration: 1s( ) 3s( ) 5s( ) Custom( )",
                                               "PIN lock: Off( ) 4( ) 6( ) 8( ) digits",
                                               "New 3DS CPU: Off( ) Clock( ) L2( ) Clock+L2( )",
                                               "Hbmenu autoboot: Off( ) 3DS( ) DSi( )",
                                             };

    static const char *singleOptionsText[] = { "( ) Autoboot EmuNAND",
                                               "( ) Enable loading external FIRMs and modules",
                                               "( ) Enable game patching",
                                               "( ) Redirect app. syscore threads to core2",
                                               "( ) Show NAND or user string in System Settings",
                                               "( ) Show GBA boot screen in patched AGB_FIRM",
                                               "( ) Enable development UNITINFO",
                                               "( ) Disable arm11 exception handlers",
                                               "( ) Enable Rosalina on SAFE_FIRM",
                                               "( ) Enable instant reboot + disable Errdisp",
                                               "( ) Enable SD card boot time patch",

                                               // Should always be the last 2 entries
                                               "\nBoot chainloader",
                                               "Save and exit"
                                             };

    // Dynamic description for splash duration
    static char splashDurationDescription[256];
    sprintf(splashDurationDescription, 
            "Select splash screen duration.\n\n"
            "Choose preset: 1s, 3s, 5s or custom.\n"
            "Custom reads from splash_duration_ms\n"
            "setting in nexusconfig.ini.\n\n"
            "Current custom value: %lu ms", 
            configData.splashDurationMsec);

    static const char *optionsDescription[]  = { "Select the default EmuNAND.\n\n"
                                                 "It will be booted when no directional\n"
                                                 "pad buttons are pressed (Up/Right/Down\n"
                                                 "/Left equal EmuNANDs 1/2/3/4).",

                                                 "Select the screen brightness.",

                                                 "Enable splash screen support.\n\n"
                                                 "\t* 'Before payloads' displays it\n"
                                                 "before booting payloads\n"
                                                 "(intended for splashes that display\n"
                                                 "button hints).\n\n"
                                                 "\t* 'After payloads' displays it\n"
                                                 "afterwards.",

                                                 splashDurationDescription,

                                                 "Activate a PIN lock.\n\n"
                                                 "The PIN will be asked each time\n"
                                                 "Nexus3DS boots.\n\n"
                                                 "4, 6 or 8 digits can be selected.\n\n"
                                                 "The ABXY buttons and the directional\n"
                                                 "pad buttons can be used as keys.\n\n"
                                                 "A message can also be displayed\n"
                                                 "(refer to the wiki for instructions).",

                                                 "Select the New 3DS CPU mode.\n\n"
                                                 "This won't apply to\n"
                                                 "New 3DS exclusive/enhanced games.\n\n"
                                                 "'Clock+L2' can cause issues with some\n"
                                                 "games.",

                                                 "Enable autobooting into homebrew menu,\n"
                                                 "either into 3DS or DSi mode.\n\n"
                                                 "Autobooting into a gamecard title is\n"
                                                 "not supported.\n\n"
                                                 "Refer to the \"autoboot\" section in the\n"
                                                 "configuration file to configure\n"
                                                 "this feature.",

                                                 "If enabled, an EmuNAND\n"
                                                 "will be launched on boot.\n\n"
                                                 "Otherwise, SysNAND will.\n\n"
                                                 "Hold L on boot to switch NAND.\n\n"
                                                 "To use a different EmuNAND from the\n"
                                                 "default, hold a directional pad button\n"
                                                 "(Up/Right/Down/Left equal EmuNANDs\n"
                                                 "1/2/3/4).",

                                                 "Enable loading external FIRMs and\n"
                                                 "system modules.\n\n"
                                                 "This isn't needed in most cases.\n\n"
                                                 "Refer to the wiki for instructions.",

                                                 "Enable overriding the region and\n"
                                                 "language configuration and the usage\n"
                                                 "of patched code binaries, exHeaders,\n"
                                                 "IPS code patches and LayeredFS\n"
                                                 "for specific games.\n\n"
                                                 "Also makes certain DLCs for out-of-\n"
                                                 "region games work.\n\n"
                                                 "Refer to the wiki for instructions.",

                                                 "Redirect app. threads that would spawn\n"
                                                 "on core1, to core2 (which is an extra\n"
                                                 "CPU core for applications that usually\n"
                                                 "remains unused).\n\n"
                                                 "This improves the performance of very\n"
                                                 "demanding games (like Pok\x82mon US/UM)\n" // CP437
                                                 "by about 10%. Can break some games\n"
                                                 "and other applications.\n",

                                                 "Enable showing the current NAND:\n\n"
                                                 "\t* Sys  = SysNAND\n"
                                                 "\t* Emu  = EmuNAND 1\n"
                                                 "\t* EmuX = EmuNAND X\n\n"
                                                 "or a user-defined custom string in\n"
                                                 "System Settings.\n\n"
                                                 "Refer to the wiki for instructions.",

                                                 "Enable showing the GBA boot screen\n"
                                                 "when booting GBA games.",

                                                 "Make the console be always detected\n"
                                                 "as a development unit, and conversely.\n"
                                                 "This is meant to install and boot\n"
                                                 "some developer software.\n\n"
                                                 "!YOU WILL GET ISSUES such as online\n"
                                                 "features and Amiibos not working and\n"
                                                 "retail CIAs installation may fail.\n\n"
                                                 "Only select this if you know what you\n"
                                                 "are doing!",

                                                 "Disables the fatal error exception\n"
                                                 "handlers for the Arm11 CPU.\n\n"
                                                 "Note: Disabling the exception handlers\n"
                                                 "will disqualify you from submitting\n"
                                                 "issues or bug reports to the Luma3DS\n"
                                                 "GitHub repository!\n\n"
                                                 "Only select this if you know what you\n"
                                                 "are doing!",

                                                 "Enables Rosalina, the kernel ext.\n"
                                                 "and sysmodule reimplementations on\n"
                                                 "SAFE_FIRM (New 3DS only).\n\n"
                                                 "Also suppresses QTM error 0xF96183FE,\n"
                                                 "allowing to use 8.1-11.3 N3DS on\n"
                                                 "New 2DS XL consoles.\n\n"
                                                 "Only select this if you know what you\n"
                                                 "are doing!",

                                                 "Disable rebooting after an Errdisp\n"
                                                 "error occurs. Also enable instant\n"
                                                 "reboot combo (A + B + X + Y + Start).\n\n"
                                                 "!WARNING! Using instant reboot may\n"
                                                 "corrupt your SD card!\n\n"
                                                 "Only select this if you know what you\n"
                                                 "are doing!",

                                                 "Enable SD card boot time patch.\n"
                                                 "This patch will speed up boot by NOT\n"
                                                 "calculating free space on SD card.\n"
                                                 "Only enable this option if you know\n"
                                                 "what you are doing!!!!!\n"
                                                 "As well as at your own risk!!!!!",

                                                // Should always be the last 2 entries
                                                "Boot to the Nexus3DS chainloader menu.",

                                                 "Save the changes and exit. To discard\n"
                                                 "any changes press the POWER button.\n"
                                                 "Use START as a shortcut to this entry."
                                               };

    FirmwareSource nandType = FIRMWARE_SYSNAND;
    if(isSdMode)
    {
        // Check if there is at least one emuNAND
        u32 emuIndex = 0;
        nandType = FIRMWARE_EMUNAND;
        locateEmuNand(&nandType, &emuIndex, false);
    }

    struct multiOption multiOptions[] = {
        { .visible = nandType == FIRMWARE_EMUNAND, .page = 0 }, // Default emunand
        { .visible = true, .page = 0 }, // Screen brightness
        { .visible = true, .page = 0 }, // Splash
        { .visible = true, .page = 0 }, // Splash duration
        { .visible = true, .page = 0 }, // PIN
        { .visible = ISN3DS, .page = 0 }, // n3ds CPU
        { .visible = true, .page = 1 }, // Autoboot
        // { .visible = true }, audio rerouting, hidden
    };

    struct singleOption singleOptions[] = {
        { .visible = nandType == FIRMWARE_EMUNAND, .page = 0 }, // Autoboot EmuNAND
        { .visible = true, .page = 0 }, // Enable external firms and modules
        { .visible = true, .page = 0 }, // Enable game patching
        { .visible = ISN3DS, .page = 1 }, // Redirect app thrreads to core2
        { .visible = true, .page = 0 }, // Show nand or user string in system settings
        { .visible = true, .page = 0 }, // show GBA boot screen
        { .visible = true, .page = 1 }, // Enable dev UNITINFO
        { .visible = true, .page = 1 }, // disable arm11 exception handlers
        { .visible = true, .page = 1 }, // Enable Rosalina on SAFE_FIRM
        { .visible = true, .page = 1 }, // Enable instant reboot + disable Errdisp
        { .visible = true, .page = 1 }, // Enable SD card boot time patch 
        // Should always be visible
        { .visible = true, .page = 0 }, // Boot chainloader
        { .visible = true, .page = 0 }, // Save and exit
    };

    //Calculate the amount of the various kinds of options and pre-select the first single one
    u32 multiOptionsAmount = sizeof(multiOptions) / sizeof(struct multiOption),
        singleOptionsAmount = sizeof(singleOptions) / sizeof(struct singleOption),
        totalIndexes = multiOptionsAmount + singleOptionsAmount - 1,
        selectedOption = 0,
        singleSelected = 0,
        currentPage = 0;
    bool isMultiOption = false;

    //Parse the existing options
    for(u32 i = 0; i < multiOptionsAmount; i++)
    {
        //Detect the positions where the "x" should go
        u32 optionNum = 0;
        for(u32 j = 0; optionNum < 4 && j < strlen(multiOptionsText[i]); j++)
            if(multiOptionsText[i][j] == '(') multiOptions[i].posXs[optionNum++] = j + 1;
        while(optionNum < 4) multiOptions[i].posXs[optionNum++] = 0;

        multiOptions[i].enabled = MULTICONFIG(i);
    }
    for(u32 i = 0; i < singleOptionsAmount; i++)
        singleOptions[i].enabled = CONFIG(i);

    initScreens();

    static const char *bootTypes[] = { "B9S",
                                       "B9S (ntrboot)",
                                       "FIRM0",
                                       "FIRM1" };

    // Initial menu draw
    drawConfigMenu(&selectedOption, &singleSelected, multiOptionsAmount, singleOptionsAmount, currentPage, multiOptions, singleOptions,
                   multiOptionsText, singleOptionsText, optionsDescription, bootTypes[(u32)bootType]);

    bool startPressed = false;

    // konami code setup
    const u32 konamiCode[] = { BUTTON_UP, BUTTON_UP, BUTTON_DOWN, BUTTON_DOWN, BUTTON_LEFT, BUTTON_RIGHT, BUTTON_LEFT, BUTTON_RIGHT, BUTTON_B, BUTTON_A };
	const u32 konami = sizeof(konamiCode) / sizeof(u32);
	u32 konamiState = 0;

    //Boring configuration menu
    while(true)
    {
        u32 pressed = 0;
        if (!startPressed)
        do
        {
            pressed = waitInput(true) & MENU_BUTTONS;
        }
        while(!pressed);

        // Check konami
        konamiState = (pressed & konamiCode[konamiState]) ? konamiState + 1 : 0;

        if(konamiState == konami)
        {
            // Switch to advanced page
            konamiState = 0;
            if(currentPage != 1)
            {
                currentPage = 1;
                isMultiOption = false;

                // Redraw the menu, reset selected position
                drawConfigMenu(&selectedOption, &singleSelected, multiOptionsAmount, singleOptionsAmount, currentPage, multiOptions, singleOptions,
                               multiOptionsText, singleOptionsText, optionsDescription, bootTypes[(u32)bootType]);
            }
            continue;
        }
        else if(pressed & BUTTON_B)
        {
            // Switch to main page
            if(currentPage != 0)
            {
                currentPage = 0;
                isMultiOption = false;

                // Redraw the menu, reset selected position
                drawConfigMenu(&selectedOption, &singleSelected, multiOptionsAmount, singleOptionsAmount, currentPage, multiOptions, singleOptions,
                               multiOptionsText, singleOptionsText, optionsDescription, bootTypes[(u32)bootType]);
            }
            continue;
        }

        // Force the selection of "save and exit" and trigger it.
        if(pressed & BUTTON_START && currentPage == 0)
        {
            startPressed = true;
            // This moves the cursor to the last entry
            pressed = BUTTON_RIGHT;
        }

        if(pressed & DPAD_BUTTONS)
        {
            //Remember the previously selected option
            u32 oldSelectedOption = selectedOption;

            while(true)
            {
                switch(pressed & DPAD_BUTTONS)
                {
                    case BUTTON_UP:
                        selectedOption = !selectedOption ? totalIndexes : selectedOption - 1;
                        break;
                    case BUTTON_DOWN:
                        selectedOption = selectedOption == totalIndexes ? 0 : selectedOption + 1;
                        break;
                    case BUTTON_LEFT:
                        pressed = BUTTON_DOWN;
                        selectedOption = 0;
                        break;
                    case BUTTON_RIGHT:
                        pressed = BUTTON_UP;
                        selectedOption = totalIndexes;
                        break;
                    default:
                        break;
                }

                if(selectedOption < multiOptionsAmount)
                {
                    if(!multiOptions[selectedOption].visible || multiOptions[selectedOption].page != currentPage) continue;

                    isMultiOption = true;
                    break;
                }
                else
                {
                    singleSelected = selectedOption - multiOptionsAmount;

                    if(!singleOptions[singleSelected].visible || singleOptions[singleSelected].page != currentPage) continue;

                    isMultiOption = false;
                    break;
                }
            }

            if(selectedOption == oldSelectedOption && !startPressed) continue;

            //The user moved to a different option, print the old option in white and the new one in red. Only print 'x's if necessary
            if(oldSelectedOption < multiOptionsAmount)
            {
                drawString(true, 10, multiOptions[oldSelectedOption].posY, COLOR_WHITE, multiOptionsText[oldSelectedOption]);
                drawCharacter(true, 10 + multiOptions[oldSelectedOption].posXs[multiOptions[oldSelectedOption].enabled] * SPACING_X, multiOptions[oldSelectedOption].posY, COLOR_WHITE, 'x');
            }
            else
            {
                u32 singleOldSelected = oldSelectedOption - multiOptionsAmount;
                drawString(true, 10, singleOptions[singleOldSelected].posY, COLOR_WHITE, singleOptionsText[singleOldSelected]);
                if(singleOptions[singleOldSelected].enabled) drawCharacter(true, 10 + SPACING_X, singleOptions[singleOldSelected].posY, COLOR_WHITE, 'x');
            }

            if(isMultiOption) drawString(true, 10, multiOptions[selectedOption].posY, COLOR_CYAN, multiOptionsText[selectedOption]);
            else drawString(true, 10, singleOptions[singleSelected].posY, COLOR_CYAN, singleOptionsText[singleSelected]);

            drawString(false, 10, 10, COLOR_BLACK, optionsDescription[oldSelectedOption]);
            drawString(false, 10, 10, COLOR_WHITE, optionsDescription[selectedOption]);
        }
        else if (pressed & BUTTON_A || startPressed)
        {
            //The selected option's status changed, print the 'x's accordingly
            if(isMultiOption)
            {
                u32 oldEnabled = multiOptions[selectedOption].enabled;
                drawCharacter(true, 10 + multiOptions[selectedOption].posXs[oldEnabled] * SPACING_X, multiOptions[selectedOption].posY, COLOR_BLACK, 'x');
                multiOptions[selectedOption].enabled = (oldEnabled == 3 || !multiOptions[selectedOption].posXs[oldEnabled + 1]) ? 0 : oldEnabled + 1;

                if(selectedOption == BRIGHTNESS) updateBrightness(multiOptions[BRIGHTNESS].enabled);
            }
            else
            {
                // Save and exit was selected.
                if (singleSelected == singleOptionsAmount - 1)
                {
                    drawString(true, 10, singleOptions[singleSelected].posY, COLOR_GREEN, singleOptionsText[singleSelected]);
                    startPressed = false;
                    break;
                }
                else if (singleSelected == singleOptionsAmount - 2) {
                    loadHomebrewFirm(0);
                    break;
                }
                else
                {
                    bool oldEnabled = singleOptions[singleSelected].enabled;
                    singleOptions[singleSelected].enabled = !oldEnabled;
                    if(oldEnabled) drawCharacter(true, 10 + SPACING_X, singleOptions[singleSelected].posY, COLOR_BLACK, 'x');
                }
            }
        }

        //In any case, if the current option is enabled (or a multiple choice option is selected) we must display a cyan 'x'
        if(isMultiOption) drawCharacter(true, 10 + multiOptions[selectedOption].posXs[multiOptions[selectedOption].enabled] * SPACING_X, multiOptions[selectedOption].posY, COLOR_CYAN, 'x');
        else if(singleOptions[singleSelected].enabled && singleOptionsText[singleSelected][0] == '(') drawCharacter(true, 10 + SPACING_X, singleOptions[singleSelected].posY, COLOR_CYAN, 'x');
    }

    //Parse and write the new configuration
    configData.multiConfig = 0;
    for(u32 i = 0; i < multiOptionsAmount; i++)
        configData.multiConfig |= multiOptions[i].enabled << (i * 2);

    configData.config = 0;
    for(u32 i = 0; i < singleOptionsAmount; i++)
        configData.config |= (singleOptions[i].enabled ? 1 : 0) << i;

    writeConfig(true);

    u32 newPinMode = MULTICONFIG(PIN);

    if(newPinMode != 0) newPin(oldPinStatus && newPinMode == oldPinMode, newPinMode);
    else if(oldPinStatus)
    {
        if(!fileDelete(PIN_FILE))
            error("Unable to delete PIN file");
    }

    while(HID_PAD & PIN_BUTTONS);
    wait(2000ULL);
}
