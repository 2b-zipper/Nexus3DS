#include <3ds.h>
#include "menus/screen_toggle.h"
#include "draw.h"
#include "fmt.h"
#include "luma_config.h"
#include "menu.h"

u8 screenToggleTarget = 0;
u32 screenToggleCombo = 0;

static void updateMenuTitles(void)
{
    switch(screenToggleTarget) {
        case 0:
            screenToggleMenu.items[0].title = "Screen to toggle: None";
            break;
        case 1:
            screenToggleMenu.items[0].title = "Screen to toggle: Bottom";
            break;
        case 2:
            screenToggleMenu.items[0].title = "Screen to toggle: Top";
            break;
        case 3:
            screenToggleMenu.items[0].title = "Screens to toggle: Both";
            break;
        default:
            screenToggleMenu.items[0].title = "Screen to toggle: Unknown"; // should never happen
            break;
    }
}

void ScreenToggleMenu_LoadConfig(void)
{
    s64 out = 0;
    Result res;

    res = svcGetSystemInfo(&out, 0x10000, 0x186);
    
    if (R_SUCCEEDED(res))
    {
        screenToggleTarget = (u32)out;
    }

    res = svcGetSystemInfo(&out, 0x10000, 0x187);
    if (R_SUCCEEDED(res))
    {
        screenToggleCombo = (u32)out;
    }
    
    updateMenuTitles();
}

Menu screenToggleMenu = {
    "Screen toggle options",
    {
        {"", METHOD, .method = &ScreenToggleMenu_ChangeScreenToggle},
        {"Change screen toggle combo", METHOD, .method = &ScreenToggleMenu_ChangeCombo},
        {},
    }};

void ScreenToggleMenu_ChangeScreenToggle(void)
{
    screenToggleTarget = (screenToggleTarget + 1) % 4;
    updateMenuTitles();
    LumaConfig_RequestSaveSettings();
}

void ScreenToggleMenu_ChangeCombo(void)
{
    char comboStrOrig[128], comboStr[128];

    Draw_Lock();
    Draw_ClearFramebuffer();
    Draw_FlushFramebuffer();
    Draw_Unlock();
    
    LumaConfig_ConvertComboToString(comboStrOrig, screenToggleCombo);
    
    Draw_Lock();
    Draw_DrawMenuFrame("Screen toggle options");
    Draw_DrawFormattedString(20, 40, COLOR_WHITE, "The current screen toggle combo is:  %s", comboStrOrig);
    Draw_DrawString(20, 50, COLOR_WHITE, "Please enter the new combo:");
    screenToggleCombo = waitCombo();
    LumaConfig_ConvertComboToString(comboStr, screenToggleCombo);
    
    do {
        Draw_Lock();
        Draw_DrawMenuFrame("Screen toggle options");
        Draw_DrawFormattedString(20, 40, COLOR_WHITE, "The current screen toggle combo is:  %s", comboStrOrig);
        Draw_DrawFormattedString(20, 50, COLOR_WHITE, "Please enter the new combo: %s", comboStr);
        Draw_DrawString(20, 70, COLOR_GREEN, "Successfully changed the Screen toggle combo.");
        Draw_DrawString(20, 90, COLOR_GRAY, "Press B to go back.");
        Draw_FlushFramebuffer();
        Draw_Unlock();
    } while (!(waitInput() & KEY_B) && !menuShouldExit);
    
    LumaConfig_RequestSaveSettings();
}
