#include <3ds.h>
#include "menus/home_button_sim.h"
#include "draw.h"
#include "fmt.h"
#include "luma_config.h"
#include "menu.h"

bool hideReturnToHomeMenu = false;
bool enableHomeButtonCombo = false;
u32 homeButtonCombo = 0;

static void updateMenuTitles(void)
{
    homeButtonSimMenu.items[0].title = hideReturnToHomeMenu ? 
        "Show 'Return to HOME Menu': [Hidden]" : 
        "Show 'Return to HOME Menu': [Shown] ";

    homeButtonSimMenu.items[1].title = enableHomeButtonCombo ? 
        "HOME button combo: [Enabled] " : 
        "HOME button combo: [Disabled]";
}

void HomeButtonSimMenu_LoadConfig(void)
{
    s64 out = 0;
    Result res;

    res = svcGetSystemInfo(&out, 0x10000, 0x185);
    
    if (R_SUCCEEDED(res))
    {
        u32 flags = (u32)out;
        hideReturnToHomeMenu = (flags >> 0) & 1;
        enableHomeButtonCombo = (flags >> 1) & 1;
    }

    res = svcGetSystemInfo(&out, 0x10000, 0x186);
    if (R_SUCCEEDED(res))
    {
        homeButtonCombo = (u32)out;
    }
    
    updateMenuTitles();
}

Menu homeButtonSimMenu = {
    "HOME button simulation options",
    {
        {"", METHOD, .method = &HomeButtonSimMenu_ToggleReturnToHomeMenu},
        {"", METHOD, .method = &HomeButtonSimMenu_ToggleEnableCombo},
        {"Change HOME button combo", METHOD, .method = &HomeButtonSimMenu_ChangeCombo},
        {},
    }};

void HomeButtonSimMenu_ToggleReturnToHomeMenu(void)
{
    hideReturnToHomeMenu = !hideReturnToHomeMenu;
    updateMenuTitles();
    LumaConfig_RequestSaveSettings();
}

void HomeButtonSimMenu_ToggleEnableCombo(void)
{
    enableHomeButtonCombo = !enableHomeButtonCombo;
    updateMenuTitles();
    LumaConfig_RequestSaveSettings();
}

bool HomeButtonSimMenu_ShouldShowReturnToHomeMenu(void)
{
    return !hideReturnToHomeMenu;
}

void HomeButtonSimMenu_ChangeCombo(void)
{
    char comboStrOrig[128], comboStr[128];

    Draw_Lock();
    Draw_ClearFramebuffer();
    Draw_FlushFramebuffer();
    Draw_Unlock();
    
    LumaConfig_ConvertComboToString(comboStrOrig, homeButtonCombo);
    
    Draw_Lock();
    Draw_DrawMenuFrame("HOME button simulation options");
    Draw_DrawFormattedString(20, 40, COLOR_WHITE, "The current HOME button combo is:  %s", comboStrOrig);
    Draw_DrawString(20, 50, COLOR_WHITE, "Please enter the new combo:");
    Draw_DrawString(20, 70, COLOR_YELLOW, "Note: Only works inside applications,");
    Draw_DrawString(20, 80, COLOR_YELLOW, "      not on HOME Menu.");
    homeButtonCombo = waitCombo();
    LumaConfig_ConvertComboToString(comboStr, homeButtonCombo);
    
    do {
        Draw_Lock();
        Draw_DrawMenuFrame("HOME button simulation options");
        Draw_DrawFormattedString(20, 40, COLOR_WHITE, "The current HOME button combo is:  %s", comboStrOrig);
        Draw_DrawFormattedString(20, 50, COLOR_WHITE, "Please enter the new combo: %s", comboStr);
        Draw_DrawString(20, 70, COLOR_YELLOW, "Note: Only works inside applications,");
        Draw_DrawString(20, 80, COLOR_YELLOW, "      not on HOME Menu.");
        Draw_DrawString(20, 100, COLOR_GREEN, "Successfully changed the HOME button combo.");
        Draw_DrawString(20, 120, COLOR_GRAY, "Press B to go back.");
        Draw_FlushFramebuffer();
        Draw_Unlock();
    } while (!(waitInput() & KEY_B) && !menuShouldExit);
    
    LumaConfig_RequestSaveSettings();
}
