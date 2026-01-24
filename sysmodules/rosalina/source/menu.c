/*
*   This file is part of Luma3DS
*   Copyright (C) 2016-2022 Aurora Wright, TuxSH
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
#include <stdio.h>
#include "menu.h"
#include "draw.h"
#include "fmt.h"
#include "memory.h"
#include "ifile.h"
#include "menus.h"
#include "utils.h"
#include "luma_config.h"
#include "menus/n3ds.h"
#include "menus/cheats.h"
#include "menus/config_extra.h"
#include "menus/home_button_sim.h"
#include "menus/screen_toggle.h"
#include "redshift/redshift.h"
#include "menus/plugin_options.h"
#include "menus/sysconfig.h"
#include "minisoc.h"
#include "plugin.h"
#include "menus/screen_filters.h"
#include "shell.h"
#include "volume.h"
#include "pmdbgext.h"

// Temperature conversion macros
#define CELSIUS_TO_FAHRENHEIT(c) ((c) * 9 / 5 + 32)

//#define ROSALINA_MENU_SELF_SCREENSHOT 1 // uncomment this to enable the feature

u32 menuCombo = 0;
bool instantReboot = false;
bool isHidInitialized = false;
bool isQtmInitialized = false;
u32 mcuFwVersion = 0;
u8 mcuInfoTable[10] = {0};
bool mcuInfoTableRead = false;

const char *topScreenType = NULL;
const char *bottomScreenType = NULL;
bool areScreenTypesInitialized = false;

// libctru redefinition:

bool hidShouldUseIrrst(void)
{
    // ir:rst exposes only two sessions :(
    return false;
}

static inline u32 convertHidKeys(u32 keys)
{
    // No actual conversion done
    return keys;
}

void scanInputHook(void)
{
    hidScanInput();

#ifdef ROSALINA_MENU_SELF_SCREENSHOT
    // Ugly hack but should work. For self-documentation w/o capture card purposes only.
    u32 selfScreenshotCombo = KEY_L | KEY_DUP | KEY_SELECT;
    if ((hidKeysHeld() & selfScreenshotCombo) == selfScreenshotCombo && (hidKeysDown() & selfScreenshotCombo) != 0)
        menuTakeSelfScreenshot();
#endif
}

u32 waitInputWithTimeout(s32 msec)
{
    s32 n = 0;
    u32 keys;

    do
    {
        svcSleepThread(1 * 1000 * 1000LL);
        Draw_Lock();
        if (!isHidInitialized || menuShouldExit)
        {
            keys = 0;
            Draw_Unlock();
            break;
        }
        n++;

        scanInputHook();
        keys = convertHidKeys(hidKeysDown()) | (convertHidKeys(hidKeysDownRepeat()) & DIRECTIONAL_KEYS);
        Draw_Unlock();
    } while (keys == 0 && !menuShouldExit && isHidInitialized && (msec < 0 || n < msec));


    return keys;
}

u32 waitInputWithTimeoutEx(u32 *outHeldKeys, s32 msec)
{
    s32 n = 0;
    u32 keys;

    do
    {
        svcSleepThread(1 * 1000 * 1000LL);
        Draw_Lock();
        if (!isHidInitialized || menuShouldExit)
        {
            keys = 0;
            Draw_Unlock();
            break;
        }
        n++;

        scanInputHook();
        keys = convertHidKeys(hidKeysDown()) | (convertHidKeys(hidKeysDownRepeat()) & DIRECTIONAL_KEYS);
        *outHeldKeys = convertHidKeys(hidKeysHeld());
        Draw_Unlock();
    } while (keys == 0 && !menuShouldExit && isHidInitialized && (msec < 0 || n < msec));


    return keys;
}

u32 waitInput(void)
{
    return waitInputWithTimeout(-1);
}

static u32 scanHeldKeys(void)
{
    u32 keys;

    Draw_Lock();

    if (!isHidInitialized || menuShouldExit)
        keys = 0;
    else
    {
        scanInputHook();
        keys = convertHidKeys(hidKeysHeld());
    }

    Draw_Unlock();
    return keys;
}

u32 waitComboWithTimeout(s32 msec)
{
    s32 n = 0;
    u32 keys = 0;
    u32 tempKeys = 0;

    // Wait for nothing to be pressed
    while (scanHeldKeys() != 0 && !menuShouldExit && isHidInitialized && (msec < 0 || n < msec))
    {
        svcSleepThread(1 * 1000 * 1000LL);
        n++;
    }

    if (menuShouldExit || !isHidInitialized || !(msec < 0 || n < msec))
        return 0;

    do
    {
        svcSleepThread(1 * 1000 * 1000LL);
        n++;

        tempKeys = scanHeldKeys();

        for (u32 i = 0x10000; i > 0; i--)
        {
            if (tempKeys != scanHeldKeys()) break;
            if (i == 1) keys = tempKeys;
        }
    }
    while((keys == 0 || scanHeldKeys() != 0) && !menuShouldExit && isHidInitialized && (msec < 0 || n < msec));

    return keys;
}

u32 waitCombo(void)
{
    return waitComboWithTimeout(-1);
}

static MyThread menuThread;
static u8 CTR_ALIGN(8) menuThreadStack[0x3000];

static float batteryPercentage;
static float batteryVoltage;
static u8 batteryTemperature;
// volume
static u8 volumeSlider[2];
static u8 dspVolumeSlider[2];

static Result menuUpdateMcuInfo(void)
{
    Result res = 0;
    u8 data[4];

    if (!isServiceUsable("mcu::HWC"))
        return -1;

    Handle *mcuHwcHandlePtr = mcuHwcGetSessionHandle();
    *mcuHwcHandlePtr = 0;

    res = srvGetServiceHandle(mcuHwcHandlePtr, "mcu::HWC");
    // Try to steal the handle if some other process is using the service (custom SVC)
    if (R_FAILED(res))
        res = svcControlService(SERVICEOP_STEAL_CLIENT_SESSION, mcuHwcHandlePtr, "mcu::HWC");
    if (res != 0)
        return res;

    // Read single-byte mcu regs 0x0A to 0x0D directly
    res = MCUHWC_ReadRegister(0xA, data, 4);

    if (R_SUCCEEDED(res))
    {
        batteryTemperature = data[0];

        // The battery percentage isn't very precise... its precision ranges from 0.09% to 0.14% approx
        // Round to 0.1%
        batteryPercentage = data[1] + data[2] / 256.0f;
        batteryPercentage = (u32)((batteryPercentage + 0.05f) * 10.0f) / 10.0f;

        // Round battery voltage to 0.01V
        batteryVoltage = 0.02f * data[3];
        batteryVoltage = (u32)((batteryVoltage + 0.005f) * 100.0f) / 100.0f;
    }

    // Read mcu fw version if not already done
    if (mcuFwVersion == 0)
    {
        u8 minor = 0, major = 0;
        MCUHWC_GetFwVerHigh(&major);
        MCUHWC_GetFwVerLow(&minor);

        // If it has failed, mcuFwVersion will be set to 0 again
        mcuFwVersion = SYSTEM_VERSION(major - 0x10, minor, 0);
    }
    
        // https://www.3dbrew.org/wiki/I2C_Registers#Device_3
    MCUHWC_ReadRegister(0x58, dspVolumeSlider, 2); // Register-mapped ADC register
    MCUHWC_ReadRegister(0x27, volumeSlider + 0, 1); // Raw volume slider state
    MCUHWC_ReadRegister(0x09, volumeSlider + 1, 1); // Volume slider state

    if (!mcuInfoTableRead)
        mcuInfoTableRead = R_SUCCEEDED(MCUHWC_ReadRegister(0x7F, mcuInfoTable, sizeof(mcuInfoTable)));

    svcCloseHandle(*mcuHwcHandlePtr);
    return res;
}

static const char *menuGetScreenTypeStr(u8 vendorId)
{
    switch (vendorId)
    {
        case 1:  return "IPS"; // SHARP
        case 12: return "TN";  // JDN
        default: return "unknown";
    }
}

static void menuReadScreenTypes(void)
{
    if (areScreenTypesInitialized)
        return;

    if (!isN3DS)
    {
        // Old3DS never have IPS screens and GetVendors is not implemented
        topScreenType = "TN";
        bottomScreenType = "TN";
        areScreenTypesInitialized = true;
    }
    else
    {
        srvSetBlockingPolicy(false);

        Result res = gspLcdInit();
        if (R_SUCCEEDED(res))
        {
            u8 vendors = 0;
            if (R_SUCCEEDED(GSPLCD_GetVendors(&vendors)))
            {
                topScreenType = menuGetScreenTypeStr(vendors >> 4);
                bottomScreenType = menuGetScreenTypeStr(vendors & 0xF);
                areScreenTypesInitialized = true;
            }

            gspLcdExit();
        }

        srvSetBlockingPolicy(true);
    }
}

static void menuInitializeQtm(void)
{
    if (isQtmInitialized)
        return;

    // Steal QTM handle from GSP, because there is a limit of 3 sessions (or 2 before 9.3) for ALL qtm services
    Handle qtmHandle = 0;
    for (int i = 0; i < 30 && !qtmIsInitialized(); i++)
    {
        if (R_SUCCEEDED(svcControlService(SERVICEOP_STEAL_CLIENT_SESSION, &qtmHandle, "qtm:sp")))
            *qtmGetSessionHandle() = qtmHandle;
        else
            svcSleepThread(100 * 100 * 1000LL);
    }

    isQtmInitialized = qtmIsInitialized();
}

static inline u32 menuAdvanceCursor(u32 pos, u32 numItems, s32 displ)
{
    return (pos + numItems + displ) % numItems;
}

static inline bool menuItemIsHidden(const MenuItem *item)
{
    return item->visibility != NULL && !item->visibility();
}

bool menuCheckN3ds(void)
{
    return isN3DS;
}

u32 menuCountItems(const Menu *menu)
{
    u32 n;
    for (n = 0; menu->items[n].action_type != MENU_END; n++);
    return n;
}

MyThread *menuCreateThread(void)
{
    if(R_FAILED(MyThread_Create(&menuThread, menuThreadMain, menuThreadStack, 0x3000, 52, CORE_SYSTEM)))
        svcBreak(USERBREAK_PANIC);
    return &menuThread;
}

u32 menuCombo;
u32 g_blockMenuOpen = 0;

void menuThreadMain(void)
{
    while (!isServiceUsable("ac:u") || !isServiceUsable("hid:USER") || !isServiceUsable("gsp::Gpu") || !isServiceUsable("gsp::Lcd") || !isServiceUsable("cdc:CHK"))
        svcSleepThread(250 * 1000 * 1000LL);

    if (isN3DS)
    {
        while (!isServiceUsable("qtm:u"))
            svcSleepThread(250 * 1000 * 1000LL);
        menuInitializeQtm();
        N3DSMenu_UpdateStatus();
    }

    handleShellOpened();

    hidInit(); // assume this doesn't fail
    isHidInitialized = true;

    menuReadScreenTypes();

    while(!preTerminationRequested)
    {
        svcSleepThread(50 * 1000 * 1000LL);
        if (menuShouldExit)
            continue;

        Cheat_ApplyCheats();

        u32 kHeld = scanHeldKeys();

        if(((kHeld & menuCombo) == menuCombo) && !g_blockMenuOpen)
        {
            menuEnter();
            if(isN3DS) N3DSMenu_UpdateStatus();
            PluginLoader__UpdateMenu();
            PluginLoaderOptions__UpdateMenu();
            PluginWatcher__UpdateMenu();
            PluginConverter__UpdateMenu();
            nightLightSettingsRead = Redshift_ReadNightLightSettings();
            Redshift_UpdateNightLightStatuses();
            menuShow(&rosalinaMenu);
            menuLeave();
        }

        // instant reboot combo key
        if(instantReboot & ((scanHeldKeys() & (KEY_A | KEY_B | KEY_X | KEY_Y | KEY_START)) == (KEY_A | KEY_B | KEY_X | KEY_Y | KEY_START)))
        {
            svcKernelSetState(7);
            __builtin_unreachable();
        }

        // HOME button simulation combo
        if(enableHomeButtonCombo && homeButtonCombo != 0 && ((kHeld & homeButtonCombo) == homeButtonCombo))
        {
            srvPublishToSubscriber(0x204, 0);
        }

        // screen toggle
        if (screenToggleTarget != 0 && screenToggleCombo != 0 && ((kHeld & screenToggleCombo) == screenToggleCombo))
        {
            u8 result;
            char sysInfo[10] = {0};

            mcuHwcInit();
            MCUHWC_ReadRegister(0x0F, &result, 1);
            MCUHWC_ReadRegister(0x7F, sysInfo, 10);
            mcuHwcExit();

            if (sysInfo[9] != 3) // Check the model, o2ds (3) don't have a top screen
            {
                gspLcdInit();

                if (screenToggleTarget == 1 || screenToggleTarget == 3)
                {
                    if ((result >> 5) & 1) // bottom screen state
                        GSPLCD_PowerOffBacklight(BIT(GSP_SCREEN_BOTTOM));
                    else
                        GSPLCD_PowerOnBacklight(BIT(GSP_SCREEN_BOTTOM));
                }

                if (screenToggleTarget == 2 || screenToggleTarget == 3)
                {
                    if ((result >> 6) & 1) // top screen state
                        GSPLCD_PowerOffBacklight(BIT(GSP_SCREEN_TOP));
                    else
                        GSPLCD_PowerOnBacklight(BIT(GSP_SCREEN_TOP));
                }

                gspLcdExit();
            }

            while (!(waitInput() & screenToggleCombo));
        }

        if (saveSettingsRequest) {
            LumaConfig_SaveSettings();
            saveSettingsRequest = false;
        }
    }
}

static s32 menuRefCount = 0;
void menuEnter(void)
{
    Draw_Lock();
    if(!menuShouldExit && menuRefCount == 0)
    {
        menuRefCount++;
        svcKernelSetState(0x10000, 2 | 1);
        svcSleepThread(5 * 1000 * 100LL);
        if (R_FAILED(Draw_AllocateFramebufferCache(FB_BOTTOM_SIZE)))
        {
            // Oops
            menuRefCount = 0;
            svcKernelSetState(0x10000, 2 | 1);
            svcSleepThread(5 * 1000 * 100LL);
        }
        else
            Draw_SetupFramebuffer();
    }
    Draw_Unlock();
}

void menuLeave(void)
{
    svcSleepThread(50 * 1000 * 1000);

    Draw_Lock();
    if(--menuRefCount == 0)
    {
        Draw_RestoreFramebuffer();
        Draw_FreeFramebufferCache();
        svcKernelSetState(0x10000, 2 | 1);
    }
    Draw_Unlock();
}


u32 Get_TitleID(u64* titleId)
{
    FS_ProgramInfo programInfo;
    u32 pid;
    u32 launchFlags;
    Result res = PMDBG_GetCurrentAppInfo(&programInfo, &pid, &launchFlags);
    if (R_FAILED(res)) {
        *titleId = 0;
        return 0xFFFFFFFF;
    }

    *titleId = programInfo.programId;
    return pid;
}


static void menuDraw(Menu *menu, u32 selected)
{
    char versionString[16];
    char nexusVersionString[16];
    s64 out;
    u32 version, commitHash, seconds, minutes, hours, days, year, month;
    u64 milliseconds = osGetTime();
    bool isRelease;

    seconds = milliseconds / 1000;
    milliseconds %= 1000;
    minutes = seconds / 60;
    seconds %= 60;
    hours = minutes / 60;
    minutes %= 60;
    days = hours / 24;
    hours %= 24;

    year = 1900; // osGetTime starts in 1900

    while (true)
    {
        bool leapYear = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
        u16 daysInYear = leapYear ? 366 : 365;
        if (days >= daysInYear)
        {
            days -= daysInYear;
            ++year;
        }
        else
        {
            static const u8 daysInMonth[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
            for (month = 0; month < 12; ++month)
            {
                u8 dim = daysInMonth[month];

                if (month == 1 && leapYear)
                    ++dim;

                if (days >= dim)
                    days -= dim;
                else
                    break;
            }
            break;
        }
    }
    days++;
    month++;

    u64 titleId;
    Get_TitleID(&titleId);

    Result mcuInfoRes = menuUpdateMcuInfo();

    svcGetSystemInfo(&out, 0x10000, 0);
    version = (u32)out;

    svcGetSystemInfo(&out, 0x10000, 1);
    commitHash = (u32)out;

    svcGetSystemInfo(&out, 0x10000, 0x200);
    isRelease = (bool)out;

    if (GET_VERSION_REVISION(version) == 0)
        sprintf(versionString, "v%lu.%lu", GET_VERSION_MAJOR(version), GET_VERSION_MINOR(version));
    else
        sprintf(versionString, "v%lu.%lu.%lu", GET_VERSION_MAJOR(version), GET_VERSION_MINOR(version), GET_VERSION_REVISION(version));

    sprintf(nexusVersionString, "v%d.%d.%d", NEXUS_VERSION_MAJOR, NEXUS_VERSION_MINOR, NEXUS_VERSION_BUILD);

    Draw_DrawMenuFrame(menu->title);
    
    u32 numItems = menuCountItems(menu);
    u32 dispY = 0;

    for (u32 i = 0; i < numItems; i++)
    {
        if (menuItemIsHidden(&menu->items[i]))
            continue;

        u32 yPos = 40 + dispY;
        Draw_DrawMenuCursor(yPos, (i == selected), menu->items[i].title, NULL);
        dispY += SPACING_Y;
    }

    {
        float coe = Volume_ExtractVolume(dspVolumeSlider[0], dspVolumeSlider[1], volumeSlider[0]);
        u32 volInt = (u32)((coe * 100.0F) + (1 / 256.0F));
        Draw_DrawFormattedString(SCREEN_BOT_WIDTH - 10 - SPACING_X * 12, 10, COLOR_LIGHT_BLUE, "Volume: %3lu%%", volInt);
    }

    if (miniSocEnabled) {
        u32 ip = socGethostid();
        if (ip != 0) {
            char ipBuffer[17];
            u8 *addr = (u8 *)&ip;
            int n = sprintf(ipBuffer, "%hhu.%hhu.%hhu.%hhu", addr[0], addr[1], addr[2], addr[3]);
            Draw_DrawString(SCREEN_BOT_WIDTH - 10 - SPACING_X * n, SCREEN_BOT_HEIGHT - 40, COLOR_GREEN, ipBuffer);
        } else {
            Draw_DrawFormattedString(SCREEN_BOT_WIDTH - 10 - SPACING_X * 15, SCREEN_BOT_HEIGHT - 40, COLOR_WHITE, "%15s", "");
        }
    } else {
        Draw_DrawFormattedString(SCREEN_BOT_WIDTH - 10 - SPACING_X * 15, SCREEN_BOT_HEIGHT - 40, COLOR_WHITE, "%15s", "");
    }

    if (mcuInfoRes == 0)
    {
        u32 voltageInt = (u32)batteryVoltage;
        u32 voltageFrac = (u32)(batteryVoltage * 100.0f) % 100u;
        u32 percentageInt = (u32)batteryPercentage;
        u32 percentageFrac = (u32)(batteryPercentage * 10.0f) % 10u;

        char buf[32];
        int n;
        if (configExtra.temperatureUnit) {
            u8 tempFahrenheit = CELSIUS_TO_FAHRENHEIT(batteryTemperature);
            n = sprintf(
                buf, "   %02hhu\xF8""F  %lu.%02luV  %lu.%lu%%", tempFahrenheit, // CP437
                voltageInt, voltageFrac,
                percentageInt, percentageFrac
            );
        } else {
            n = sprintf(
                buf, "   %02hhu\xF8""C  %lu.%02luV  %lu.%lu%%", batteryTemperature, // CP437
                voltageInt, voltageFrac,
                percentageInt, percentageFrac
            );
        }
        Draw_DrawString(SCREEN_BOT_WIDTH - 10 - SPACING_X * n, SCREEN_BOT_HEIGHT - 30, COLOR_CYAN, buf);
    }

    if (isRelease) {
        Draw_DrawFormattedString(10, SCREEN_BOT_HEIGHT - 30, COLOR_ORANGE, "Nexus3DS %s", nexusVersionString);
        Draw_DrawFormattedString(10, SCREEN_BOT_HEIGHT - 20, COLOR_ORANGE, "Based on Luma3DS %s", versionString);
    } else {
        Draw_DrawFormattedString(10, SCREEN_BOT_HEIGHT - 30, COLOR_ORANGE, "Nexus3DS %s", nexusVersionString);
        Draw_DrawFormattedString(10, SCREEN_BOT_HEIGHT - 20, COLOR_ORANGE, "Based on Luma3DS %s-%08lx", versionString, commitHash);
    }

    if (configExtra.use12HourClock) {
        const char *ampm = (hours >= 12) ? "PM" : "AM";
        u32 displayHours = (hours % 12 == 0) ? 12 : (hours % 12);
        Draw_DrawFormattedString(SCREEN_BOT_WIDTH - 30 - SPACING_X * 18.6, SCREEN_BOT_HEIGHT - 20, COLOR_CYAN, "%04lu-%02lu-%02lu", year, month, days);
        Draw_DrawFormattedString(SCREEN_BOT_WIDTH - 30 - SPACING_X * 7.6, SCREEN_BOT_HEIGHT - 20, COLOR_CYAN, "%02lu:%02lu:%02lu %s", displayHours, minutes, seconds, ampm);
    } else {
        Draw_DrawFormattedString(SCREEN_BOT_WIDTH - 30 - SPACING_X * 15.6, SCREEN_BOT_HEIGHT - 20, COLOR_CYAN, "%04lu-%02lu-%02lu", year, month, days);
        Draw_DrawFormattedString(SCREEN_BOT_WIDTH - 30 - SPACING_X * 4.6, SCREEN_BOT_HEIGHT - 20, COLOR_CYAN, "%02lu:%02lu:%02lu", hours, minutes, seconds);
    }

    Draw_FlushFramebuffer();
}

void menuShow(Menu *root)
{
    u32 selectedItem = 0;
    Menu *currentMenu = root;
    u32 nbPreviousMenus = 0;
    Menu *previousMenus[0x80];
    u32 previousSelectedItems[0x80];

    u32 numItems = menuCountItems(currentMenu);
    if (menuItemIsHidden(&currentMenu->items[selectedItem]))
        selectedItem = menuAdvanceCursor(selectedItem, numItems, 1);

    Draw_Lock();
    Draw_ClearFramebuffer();
    Draw_FlushFramebuffer();
    hidSetRepeatParameters(0, 0);
    menuDraw(currentMenu, selectedItem);
    Draw_Unlock();

    bool menuComboReleased = false;

    u8 prevVolumeSlider[2] = {0};
    s8 prevVolumeOverride = currVolumeSliderOverride;
    bool firstRun = true;
    u32 updateCounter = 0;

    do
    {
        u32 pressed = waitInputWithTimeout(30);
        numItems = menuCountItems(currentMenu);
        updateCounter++;

        if (firstRun || currVolumeSliderOverride != prevVolumeOverride)
        {
            prevVolumeOverride = currVolumeSliderOverride;
            firstRun = false;
        }
        else if (pressed == 0)
        {
            if (isServiceUsable("mcu::HWC"))
            {
                u8 currentVolumeSlider[2];
                
                Handle *mcuHwcHandlePtr = mcuHwcGetSessionHandle();
                Handle oldHandle = *mcuHwcHandlePtr;
                *mcuHwcHandlePtr = 0;
                
                if (R_SUCCEEDED(srvGetServiceHandle(mcuHwcHandlePtr, "mcu::HWC")) ||
                    R_SUCCEEDED(svcControlService(SERVICEOP_STEAL_CLIENT_SESSION, mcuHwcHandlePtr, "mcu::HWC")))
                {
                    if (R_SUCCEEDED(MCUHWC_ReadRegister(0x09, currentVolumeSlider + 1, 1)))
                    {
                        if (currentVolumeSlider[1] != prevVolumeSlider[1])
                        {
                            prevVolumeSlider[1] = currentVolumeSlider[1];
                        }
                    }
                    svcCloseHandle(*mcuHwcHandlePtr);
                }
                *mcuHwcHandlePtr = oldHandle;
            }
        }

        if(!menuComboReleased && (scanHeldKeys() & menuCombo) != menuCombo)
        {
            menuComboReleased = true;
            Draw_Lock();
            hidSetRepeatParameters(200, 100);
            Draw_Unlock();
        }

        if(pressed & KEY_A)
        {
            Draw_Lock();
            Draw_ClearFramebuffer();
            Draw_FlushFramebuffer();
            Draw_Unlock();

            switch(currentMenu->items[selectedItem].action_type)
            {
                case METHOD:
                    if(currentMenu->items[selectedItem].method != NULL)
                        currentMenu->items[selectedItem].method();
                    break;
                case MENU:
                    previousSelectedItems[nbPreviousMenus] = selectedItem;
                    previousMenus[nbPreviousMenus++] = currentMenu;
                    currentMenu = currentMenu->items[selectedItem].menu;
                    selectedItem = 0;
                    break;
                default:
                    __builtin_trap(); // oops
                    break;
            }

            Draw_Lock();
            Draw_ClearFramebuffer();
            Draw_FlushFramebuffer();
            Draw_Unlock();
        }
        else if(pressed & KEY_B)
        {
            while (nbPreviousMenus == 0 && (scanHeldKeys() & KEY_B)); // wait a bit before exiting rosalina

            Draw_Lock();
            Draw_ClearFramebuffer();
            Draw_FlushFramebuffer();
            Draw_Unlock();

            if(nbPreviousMenus > 0)
            {
                currentMenu = previousMenus[--nbPreviousMenus];
                selectedItem = previousSelectedItems[nbPreviousMenus];
            }
            else
                break;
        }
        else if(pressed & (KEY_DOWN | KEY_UP))
        {
            s32 n = (pressed & KEY_DOWN) != 0 ? 1 : -1;
            do {
                selectedItem = menuAdvanceCursor(selectedItem, numItems, n);
            } while (menuItemIsHidden(&currentMenu->items[selectedItem])); // assume at least one item is visible
        }
        else if(pressed & KEY_START)
        {
            if (isServiceUsable("nwm::EXT"))
            {
                u8 wireless = (*(vu8 *)((0x10140000 | (1u << 31)) + 0x180));
                nwmExtInit();
                NWMEXT_ControlWirelessEnabled(!wireless);
                nwmExtExit();
            }
        }
        else if(pressed & KEY_SELECT)
        {
            // Toggle LEDs
            mcuHwcInit();
            u8 result;
            MCUHWC_ReadRegister(0x28, &result, 1);
            result = ~result;
            MCUHWC_WriteRegister(0x28, &result, 1);
            mcuHwcExit();
        }

        Draw_Lock();
        menuDraw(currentMenu, selectedItem);
        Draw_Unlock();
    }
    while(!menuShouldExit);
}
