#pragma once

#include <3ds/types.h>
#include "menu.h"

extern Menu screenToggleMenu;
extern u8 screenToggleTarget;
extern u32 screenToggleCombo;

void ScreenToggleMenu_ChangeScreenToggle(void);
void ScreenToggleMenu_LoadConfig(void);
void ScreenToggleMenu_ChangeCombo(void);
