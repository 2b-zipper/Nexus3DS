/*
 *   This file is part of Luma3DS
 *   Copyright (C) 2016-2021 Aurora Wright, TuxSH
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
#include "menus/nexus_config.h"
#include "menus/home_button_sim.h"
#include "luma_config.h"
#include "draw.h"
#include "utils.h" // for makeArmBranch

Menu nexusConfigMenu = {
    "Nexus config options menu",
    {
        { "Change the menu combo", METHOD, .method = &NexusConfigMenu_ChangeMenuCombo },
        { "HOME button simulation options...", MENU, .menu = &homeButtonSimMenu },
        { "Show temperature in Fahrenheit", METHOD, .method = &NexusConfigMenu_ToggleTemperatureUnit },
        { "Use 12 hours clock format", METHOD, .method = &NexusConfigMenu_ToggleClockFormat },
        { "Save Settings", METHOD, .method = &NexusConfigMenu_SaveSettings },
        {}, // [TODO] screenshot settings
    }
};

void NexusConfigMenu_UpdateTemperatureUnitEntry(void)
{
    if (lumaMcuConfig.flags & MCUCFG_FLAG_TEMPERATURE_IN_FAHRENHEIT)
        nexusConfigMenu.items[2].title = "Show temperature in Celsius   ";
    else
        nexusConfigMenu.items[2].title = "Show temperature in Fahrenheit";
}

void NexusConfigMenu_ToggleTemperatureUnit(void)
{
    // Stored in MCU, saved immediately
    LumaConfig_SetMcuConfigFlags(MCUCFG_FLAG_TEMPERATURE_IN_FAHRENHEIT, lumaMcuConfig.flags ^ MCUCFG_FLAG_TEMPERATURE_IN_FAHRENHEIT);
    NexusConfigMenu_UpdateTemperatureUnitEntry();
}

void NexusConfigMenu_UpdateClockFormatEntry(void)
{
    if (lumaMcuConfig.flags & MCUCFG_FLAG_CLOCK_AM_PM)
        nexusConfigMenu.items[3].title = "Use 24 hours clock format";
    else
        nexusConfigMenu.items[3].title = "Use 12 hours clock format";
}

void NexusConfigMenu_ToggleClockFormat(void)
{
    // Stored in MCU, saved immediately
    LumaConfig_SetMcuConfigFlags(MCUCFG_FLAG_CLOCK_AM_PM, lumaMcuConfig.flags ^ MCUCFG_FLAG_CLOCK_AM_PM);
    NexusConfigMenu_UpdateClockFormatEntry();
}

void NexusConfigMenu_ChangeMenuCombo(void)
{
    char comboStrOrig[128], comboStr[128];
    u32 posY;

    Draw_Lock();
    Draw_ClearFramebuffer();
    Draw_FlushFramebuffer();
    Draw_Unlock();
    LumaConfig_ConvertComboToString(comboStrOrig, menuCombo);
    Draw_Lock();
    Draw_DrawMenuFrame("Nexus config options menu");
    posY = Draw_DrawFormattedString(20, 40, COLOR_WHITE, "The current menu combo is:  %s", comboStrOrig);
    posY = Draw_DrawString(20, posY + SPACING_Y, COLOR_WHITE, "Please enter the new combo:");
    menuCombo = waitCombo();
    LumaConfig_ConvertComboToString(comboStr, menuCombo);
    do {
        Draw_Lock();
        Draw_DrawMenuFrame("Nexus config options menu");
        posY = Draw_DrawFormattedString(20, 40, COLOR_WHITE, "The current menu combo is:  %s", comboStrOrig);
        posY = Draw_DrawFormattedString(20, posY + SPACING_Y, COLOR_WHITE, "Please enter the new combo: %s", comboStr) + SPACING_Y;
        posY = Draw_DrawString(20, posY + SPACING_Y, COLOR_GREEN, "Successfully changed the menu combo.");
        Draw_DrawString(20, posY + SPACING_Y * 2, COLOR_GRAY, "Press B to go back.");
        Draw_FlushFramebuffer();
        Draw_Unlock();
    } while (!(waitInput() & KEY_B) && !menuShouldExit);
}

void NexusConfigMenu_SaveSettings(void)
{
    Result res = LumaConfig_SaveSettings();
    Draw_Lock();
    Draw_ClearFramebuffer();
    Draw_FlushFramebuffer();
    Draw_Unlock();

    do
    {
        Draw_Lock();
        Draw_DrawMenuFrame("Save settings");
        if (R_SUCCEEDED(res))
            Draw_DrawString(10, 40, COLOR_WHITE, "Operation succeeded.");
        else
            Draw_DrawFormattedString(10, 40, COLOR_WHITE, "Operation failed (0x%08lx).", res);
        Draw_FlushFramebuffer();
        Draw_Unlock();
    }
    while (!(waitInput() & KEY_B) && !menuShouldExit);
}