#pragma once

#include <3ds/types.h>
#include "menu.h"

extern Menu screenshotSettingsMenu;
extern u8 screenshotFolder;
extern bool screenshotCombined;

void ScreenshotSettingsMenu_ChangeFolderOutput(void);
void ScreenshotSettingsMenu_ToggleCombine(void);
void ScreenshotSettingsMenu_LoadConfig(void);
