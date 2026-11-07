#include <3ds.h>
#include "menus/screenshot_settings.h"
#include "draw.h"
#include "fmt.h"
#include "luma_config.h"
#include "menu.h"

u8 screenshotFolder = 0;
bool screenshotCombined = false;

static void updateMenuTitles(void)
{
    switch(screenshotFolder) {
        case 0:
            screenshotSettingsMenu.items[0].title = "Screenshots output folder: Default";
            break;
        case 1:
            screenshotSettingsMenu.items[0].title = "Screenshots output folder: Date";
            break;
        case 2:
            screenshotSettingsMenu.items[0].title = "Screenshots output folder: Title ID";
            break;
        default:
            screenshotSettingsMenu.items[0].title = "Screenshots output folder: Default"; // should never happen
            break;
    }

    screenshotSettingsMenu.items[1].title = screenshotCombined ? 
        "Combine top/bottom screens: [Enabled] " : 
        "Combine top/bottom screens: [Disabled]";
}

void ScreenshotSettingsMenu_LoadConfig(void)
{
    s64 out = 0;
    Result res;

    res = svcGetSystemInfo(&out, 0x10000, 0x183);
    
    if (R_SUCCEEDED(res))
    {
        screenshotFolder = (u32)out;
    }

    res = svcGetSystemInfo(&out, 0x10000, 0x184);
    
    if (R_SUCCEEDED(res))
    {
        u32 flags = (u32)out;
        screenshotCombined = (flags >> 0) & 1;
    }
    
    updateMenuTitles();
}

Menu screenshotSettingsMenu = {
    "Screenshot settings options",
    {
        {"", METHOD, .method = &ScreenshotSettingsMenu_ChangeFolderOutput},
        {"", METHOD, .method = &ScreenshotSettingsMenu_ToggleCombine},
        {},
    }};

void ScreenshotSettingsMenu_ChangeFolderOutput(void)
{
    screenshotFolder = (screenshotFolder + 1) % 3;
    updateMenuTitles();
    LumaConfig_RequestSaveSettings();
}

void ScreenshotSettingsMenu_ToggleCombine(void)
{
    screenshotCombined = !screenshotCombined;
    updateMenuTitles();
    LumaConfig_RequestSaveSettings();
}
