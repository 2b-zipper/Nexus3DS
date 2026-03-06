#include <3ds.h>
#include "fmt.h"
#include "draw.h"
#include "ifile.h"
#include "menu.h"
#include "menus.h"
#include "menus/config_extra.h"
#include "luma_config.h"

config_extra configExtra;

typedef struct {
    const char *displayText;
    bool *configValue;
    bool (*visibility)(void);
} ConfigMenuEntry;

static inline const char* ConfigExtra_GetCheckboxDisplay(bool value)
{
    return value ? "(x)" : "( )";
}

void ConfigExtra_DrawDetailedMenu(void)
{
    static const ConfigMenuEntry configMenuEntries[] = {
        {"Automatically suppress LEDs", &configExtra.suppressLeds, NULL},
        {"Cut power to TWL Flashcards", &configExtra.cutSlotPower, NULL},
        {"Cut 3DS Wifi in sleep mode", &configExtra.cutSleepWifi, NULL},
        {"Save screenshots in date folders", &configExtra.screenshotDateFolders, NULL},
        {"Combine top/bottom screenshots", &configExtra.screenshotCombined, NULL},
        {"Use Fahrenheit for temperature display", &configExtra.temperatureUnit, NULL},
        {"Use 12-hour clock format in Rosalina menu", &configExtra.use12HourClock, NULL}
    };

    s32 selected = 0;
    s32 nbOptions = sizeof(configMenuEntries) / sizeof(ConfigMenuEntry);

    s32 visibleCount = 0;
    s32 visibleMapping[sizeof(configMenuEntries) / sizeof(ConfigMenuEntry)];
    
    for (s32 i = 0; i < nbOptions; i++) {
        if (configMenuEntries[i].visibility == NULL || configMenuEntries[i].visibility()) {
            visibleMapping[visibleCount] = i;
            visibleCount++;
        }
    }

    if (visibleCount == 0) return;

    do {
        Draw_Lock();
        Draw_DrawMenuFrame("Extra config menu");

        s32 displayIndex = 0;
        for (s32 i = 0; i < nbOptions; i++) {
            const ConfigMenuEntry *entry = &configMenuEntries[i];

            if (entry->visibility != NULL && !entry->visibility()) {
                continue;
            }

            u32 yPos = 40 + displayIndex * SPACING_Y;
            const char *checkbox = ConfigExtra_GetCheckboxDisplay(*(entry->configValue));

            Draw_DrawMenuCursor(yPos, displayIndex == selected, entry->displayText, checkbox);

            displayIndex++;
        }
        
        Draw_FlushFramebuffer();
        Draw_Unlock();
        
        u32 pressed = waitInputWithTimeout(30);
        
        if(pressed & KEY_B)
            break;
        else if(pressed & KEY_A)
        {
            s32 actualIndex = visibleMapping[selected];
            bool *selectedValue = configMenuEntries[actualIndex].configValue;
            *selectedValue = !(*selectedValue);
            LumaConfig_RequestSaveSettings();
        }
        else if(pressed & KEY_DOWN)
            selected++;
        else if(pressed & KEY_UP)
            selected--;
            
        if(selected < 0)
            selected = visibleCount - 1;
        else if(selected >= visibleCount)
            selected = 0;
            
    } while(!menuShouldExit);
}

void ConfigExtra_ReadConfigExtra(void)
{
    s64 extraConfigFlags = 0;
    Result res = svcGetSystemInfo(&extraConfigFlags, 0x10000, 0x183);
    
    if (R_SUCCEEDED(res)) {
        configExtra.suppressLeds = (extraConfigFlags >> 0) & 1;
        configExtra.cutSlotPower = (extraConfigFlags >> 1) & 1;
        configExtra.cutSleepWifi = (extraConfigFlags >> 2) & 1;
        configExtra.screenshotDateFolders = (extraConfigFlags >> 3) & 1;
        configExtra.screenshotCombined = (extraConfigFlags >> 4) & 1;
        configExtra.temperatureUnit = (extraConfigFlags >> 5) & 1;
        configExtra.use12HourClock = (extraConfigFlags >> 6) & 1;
    } else {
        configExtra.suppressLeds = false;
        configExtra.cutSlotPower = false;
        configExtra.cutSleepWifi = false;
        configExtra.screenshotDateFolders = true;
        configExtra.screenshotCombined = true;
        configExtra.temperatureUnit = false;
        configExtra.use12HourClock = false;
    }
}
