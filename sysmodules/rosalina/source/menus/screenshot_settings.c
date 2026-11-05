#include <3ds.h>
#include "menus/screenshot_settings.h"
#include "draw.h"
#include "fmt.h"
#include "luma_config.h"
#include "menu.h"

bool screenshotDateFolders = false;
bool screenshotCombined = false;

static void updateMenuTitles(void)
{
    screenshotSettingsMenu.items[0].title = screenshotDateFolders ? 
        "Save screenshots in date folders: [Enabled] " : 
        "Save screenshots in date folders: [Disabled]";

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
        u32 flags = (u32)out;
        screenshotDateFolders = (flags >> 0) & 1;
        screenshotCombined = (flags >> 1) & 1;
    }
    
    updateMenuTitles();
}

Menu screenshotSettingsMenu = {
    "Screenshot settings options",
    {
        {"", METHOD, .method = &ScreenshotSettingsMenu_ToggleDateFolders},
        {"", METHOD, .method = &ScreenshotSettingsMenu_ToggleCombine},
        {},
    }};

void ScreenshotSettingsMenu_ToggleDateFolders(void)
{
    screenshotDateFolders = !screenshotDateFolders;
    updateMenuTitles();
    LumaConfig_RequestSaveSettings();
}

void ScreenshotSettingsMenu_ToggleCombine(void)
{
    screenshotCombined = !screenshotCombined;
    updateMenuTitles();
    LumaConfig_RequestSaveSettings();
}
