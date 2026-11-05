#pragma once

#include <3ds/types.h>
#include "menu.h"

extern Menu screenshotSettingsMenu;
extern bool screenshotDateFolders;
extern bool screenshotCombined;

void ScreenshotSettingsMenu_ToggleDateFolders(void);
void ScreenshotSettingsMenu_ToggleCombine(void);
void ScreenshotSettingsMenu_LoadConfig(void);
