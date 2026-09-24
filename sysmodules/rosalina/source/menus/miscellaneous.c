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
#include "menus/miscellaneous.h"
#include "menus/home_button_sim.h"
#include "luma_config.h"
#include "input_redirection.h"
#include "ntp.h"
#include "memory.h"
#include "draw.h"
#include "fmt.h"
#include "utils.h" // for makeArmBranch
#include "minisoc.h"
#include "ifile.h"
#include "pmdbgext.h"
#include "plugin.h"
#include "process_patches.h"

typedef struct DspFirmSegmentHeader
{
    u32 offset;
    u32 loadAddrHalfwords;
    u32 size;
    u8 _0x0C[3];
    u8 memType;
    u8 hash[0x20];
} DspFirmSegmentHeader;

typedef struct DspFirm
{
    u8 signature[0x100];
    char magic[4];
    u32 totalSize; // no more than 0x10000
    u16 layoutBitfield;
    u8 _0x10A[3];
    u8 surroundSegmentMemType;
    u8 numSegments; // no more than 10
    u8 flags;
    u32 surroundSegmentLoadAddrHalfwords;
    u32 surroundSegmentSize;
    u8 _0x118[8];
    DspFirmSegmentHeader segmentHdrs[10];
    u8 data[];
} DspFirm;

Menu miscellaneousMenu = {
    "Miscellaneous options menu",
    {
        {"Switch the hb. title to the current app.", METHOD, .method = &MiscellaneousMenu_SwitchBoot3dsxTargetTitle},
        {"Change the menu combo", METHOD, .method = &MiscellaneousMenu_ChangeMenuCombo},
        {"HOME button simulation options...", MENU, .menu = &homeButtonSimMenu},
        {"Start InputRedirection", METHOD, .method = &MiscellaneousMenu_InputRedirection},
        {"Update time and date via NTP", METHOD, .method = &MiscellaneousMenu_UpdateTimeDateNtp},
        {"Nullify user time offset", METHOD, .method = &MiscellaneousMenu_NullifyUserTimeOffset},
        {"Dump DSP firmware", METHOD, .method = &MiscellaneousMenu_DumpDspFirm},
        {"Set the number of Play Coins", METHOD, .method = &MiscellaneousMenu_EditPlayCoins},
        {},
    }};

int lastNtpTzOffset = 0;

static inline bool compareTids(u64 tidA, u64 tidB)
{
    // Just like p9 clears them, ignore platform/N3DS bits
    return ((tidA ^ tidB) & ~0xF0000000ull) == 0;
}

void MiscellaneousMenu_SwitchBoot3dsxTargetTitle(void)
{
    Result res;
    char failureReason[64];
    u64 currentTid = Luma_SharedConfig->selected_hbldr_3dsx_tid;
    u64 newTid = currentTid;

    FS_ProgramInfo progInfo;
    u32 pid;
    u32 launchFlags;
    res = PMDBG_GetCurrentAppInfo(&progInfo, &pid, &launchFlags);
    bool appRunning = R_SUCCEEDED(res);

    if (compareTids(currentTid, HBLDR_DEFAULT_3DSX_TID))
    {
        if (appRunning)
            newTid = progInfo.programId;
        else
        {
            res = -1;
            strcpy(failureReason, "no suitable process found");
        }
    }
    else
    {
        res = 0;
        newTid = HBLDR_DEFAULT_3DSX_TID;
    }

    Luma_SharedConfig->selected_hbldr_3dsx_tid = newTid;

    // Move "selected" field to "current" if no app is currently running.
    // Otherwise, PM will do it on app exit.
    // There's a small possibility of race condition but it shouldn't matter
    // here.
    // We need to do that to ensure that the ExHeader at init matches the ExHeader
    // at termination at all times, otherwise the process refcounts of sysmodules
    // get all messed up.
    if (!appRunning)
        Luma_SharedConfig->hbldr_3dsx_tid = newTid;

    if (compareTids(newTid, HBLDR_DEFAULT_3DSX_TID))
        miscellaneousMenu.items[0].title = "Switch the hb. title to the current app.";
    else
        miscellaneousMenu.items[0].title = "Switch the hb. title to " HBLDR_DEFAULT_3DSX_TITLE_NAME;

    Draw_Lock();
    Draw_ClearFramebuffer();
    Draw_FlushFramebuffer();
    Draw_Unlock();
    do {
        Draw_Lock();
        Draw_DrawMenuFrame("Miscellaneous options menu");
        if (R_SUCCEEDED(res))
            Draw_DrawString(20, 40, COLOR_WHITE, "Operation succeeded.");
        else
            Draw_DrawFormattedString(20, 40, COLOR_WHITE, "Operation failed (%s).", failureReason);
        Draw_DrawString(20, 60, COLOR_GRAY, "Press B to go back.");
        Draw_FlushFramebuffer();
        Draw_Unlock();
    } while (!(waitInput() & KEY_B) && !menuShouldExit);
}

void MiscellaneousMenu_ChangeMenuCombo(void)
{
    char comboStrOrig[128], comboStr[128];
    u32 posY;

    Draw_Lock();
    Draw_ClearFramebuffer();
    Draw_FlushFramebuffer();
    Draw_Unlock();
    LumaConfig_ConvertComboToString(comboStrOrig, menuCombo);
    Draw_Lock();
    Draw_DrawMenuFrame("Miscellaneous options menu");
    posY = Draw_DrawFormattedString(20, 40, COLOR_WHITE, "The current menu combo is:  %s", comboStrOrig);
    posY = Draw_DrawString(20, posY + SPACING_Y, COLOR_WHITE, "Please enter the new combo:");
    menuCombo = waitCombo();
    LumaConfig_ConvertComboToString(comboStr, menuCombo);
    do {
        Draw_Lock();
        Draw_DrawMenuFrame("Miscellaneous options menu");
        posY = Draw_DrawFormattedString(20, 40, COLOR_WHITE, "The current menu combo is:  %s", comboStrOrig);
        posY = Draw_DrawFormattedString(20, posY + SPACING_Y, COLOR_WHITE, "Please enter the new combo: %s", comboStr) + SPACING_Y;
        posY = Draw_DrawString(20, posY + SPACING_Y, COLOR_GREEN, "Successfully changed the menu combo.");
        Draw_DrawString(20, posY + SPACING_Y * 2, COLOR_GRAY, "Press B to go back.");
        Draw_FlushFramebuffer();
        Draw_Unlock();
    } while (!(waitInput() & KEY_B) && !menuShouldExit);
}

void MiscellaneousMenu_InputRedirection(void)
{
    bool done = false;

    Result res;
    char buf[65];
    bool wasEnabled = inputRedirectionEnabled;
    bool cantStart = false;

    if (wasEnabled)
    {
        res = InputRedirection_Disable(5 * 1000 * 1000 * 1000LL);
        if (res != 0)
            sprintf(buf, "Failed to stop InputRedirection (0x%08lx).", (u32)res);
        else
            miscellaneousMenu.items[3].title = "Start InputRedirection";
    }
    else
    {
        s64 dummyInfo;
        bool isN3DS = svcGetSystemInfo(&dummyInfo, 0x10001, 0) == 0;
        bool isSocURegistered;

        res = srvIsServiceRegistered(&isSocURegistered, "soc:U");
        cantStart = R_FAILED(res) || !isSocURegistered;

        if (!cantStart && isN3DS)
        {
            bool isIrRstRegistered;

            res = srvIsServiceRegistered(&isIrRstRegistered, "ir:rst");
            cantStart = R_FAILED(res) || !isIrRstRegistered;
        }
    }

    Draw_Lock();
    Draw_ClearFramebuffer();
    Draw_FlushFramebuffer();
    Draw_Unlock();

    do {
        Draw_Lock();
        Draw_DrawMenuFrame("Miscellaneous options menu");

        u32 posY = 40;
        if (!wasEnabled && cantStart)
            Draw_DrawString(20, posY, COLOR_WHITE, "Can't start the input redirection before the system has finished loading.");
        else if (!wasEnabled) {
            Draw_DrawString(20, posY, COLOR_WHITE, "Starting InputRedirection...");
            if (!done) {
                res = InputRedirection_DoOrUndoPatches();
                if (R_SUCCEEDED(res)) {
                    res = svcCreateEvent(&inputRedirectionThreadStartedEvent, RESET_STICKY);
                    if (R_SUCCEEDED(res)) {
                        inputRedirectionCreateThread();
                        res = svcWaitSynchronization(inputRedirectionThreadStartedEvent, 10 * 1000 * 1000 * 1000LL);
                        if (res == 0)
                            res = (Result)inputRedirectionStartResult;
                        if (res != 0) {
                            svcCloseHandle(inputRedirectionThreadStartedEvent);
                            InputRedirection_DoOrUndoPatches();
                            inputRedirectionEnabled = false;
                        }
                        inputRedirectionStartResult = 0;
                    }
                }
                if (res != 0)
                    sprintf(buf, "Starting InputRedirection... failed (0x%08lx).", (u32)res);
                else
                    miscellaneousMenu.items[3].title = "Stop InputRedirection";
                done = true;
            }
            if (res == 0)
                posY = Draw_DrawString(20, posY + SPACING_Y, COLOR_GREEN, "Starting InputRedirection... OK.");
            else
                posY = Draw_DrawString(20, posY + SPACING_Y, COLOR_RED, buf);
        } else {
            if (res == 0) {
                posY = Draw_DrawString(20, posY, COLOR_GREEN, "InputRedirection stopped successfully.\n\n");
                if (isN3DS) {
                    posY = Draw_DrawString(20, posY, COLOR_WHITE,
                        "This might cause a key press to be repeated\nin Home Menu for no reason.\n\nJust pressing ZL/ZR on the console is\nenough to fix this.\n");
                }
            } else {
                posY = Draw_DrawString(20, posY, COLOR_RED, buf);
            }
        }
        Draw_DrawString(20, posY + SPACING_Y * 2, COLOR_GRAY, "Press B to go back.");
        Draw_FlushFramebuffer();
        Draw_Unlock();
    } while (!(waitInput() & KEY_B) && !menuShouldExit);
}

void MiscellaneousMenu_UpdateTimeDateNtp(void)
{
    u32 posY;
    u32 input = 0;

    Result res;
    bool cantStart = false;

    bool isSocURegistered;

    u64 msSince1900, samplingTick;

    res = srvIsServiceRegistered(&isSocURegistered, "soc:U");
    cantStart = R_FAILED(res) || !isSocURegistered;

    int dt = 12 * 60 + lastNtpTzOffset;
    int utcOffset = dt / 60;
    int utcOffsetMinute = dt % 60;
    int absOffset;

    Draw_Lock();
    Draw_ClearFramebuffer();
    Draw_FlushFramebuffer();
    Draw_Unlock();

    do {
        Draw_Lock();
        Draw_DrawMenuFrame("Miscellaneous options menu");

        absOffset = utcOffset - 12;
        absOffset = absOffset < 0 ? -absOffset : absOffset;
        posY = Draw_DrawFormattedString(20, 40, COLOR_WHITE, "Current UTC offset:  %c%02d%02d", utcOffset < 12 ? '-' : '+', absOffset, utcOffsetMinute);
        posY = Draw_DrawFormattedString(20, posY + SPACING_Y, COLOR_WHITE, "Use DPAD Left/Right to change hour offset.\nUse DPAD Up/Down to change minute offset.\nPress A when done.") + SPACING_Y;
        Draw_DrawString(20, posY + SPACING_Y * 3, COLOR_GRAY, "Press B to go back.");

        Draw_FlushFramebuffer();
        Draw_Unlock();

        input = waitInput();
        if (input & KEY_LEFT) utcOffset = (27 + utcOffset - 1) % 27; // ensure utcOffset >= 0
        if (input & KEY_RIGHT) utcOffset = (utcOffset + 1) % 27;
        if (input & KEY_UP) utcOffsetMinute = (utcOffsetMinute + 1) % 60;
        if (input & KEY_DOWN) utcOffsetMinute = (60 + utcOffsetMinute - 1) % 60;
    } while (!(input & (KEY_A | KEY_B)) && !menuShouldExit);

    if (input & KEY_B)
        return;

    utcOffset -= 12;
    lastNtpTzOffset = 60 * utcOffset + utcOffsetMinute;

    res = srvIsServiceRegistered(&isSocURegistered, "soc:U");
    cantStart = R_FAILED(res) || !isSocURegistered;
    res = 0;
    if (!cantStart)
    {
        res = ntpGetTimeStamp(&msSince1900, &samplingTick);
        if (R_SUCCEEDED(res))
        {
            msSince1900 += 1000 * (3600 * utcOffset + 60 * utcOffsetMinute);
            res = ntpSetTimeDate(msSince1900, samplingTick);
        }
    }

    do {
        Draw_Lock();
        Draw_DrawMenuFrame("Miscellaneous options menu");

        absOffset = utcOffset;
        absOffset = absOffset < 0 ? -absOffset : absOffset;
        posY = Draw_DrawFormattedString(20, 40, COLOR_WHITE, "Current UTC offset:  %c%02d%02d", utcOffset < 0 ? '-' : '+', absOffset, utcOffsetMinute);
        if (cantStart)
            posY = Draw_DrawString(20, posY + SPACING_Y * 5, COLOR_RED, "Can't sync time/date before the system has finished loading.");
        else if (R_FAILED(res))
            posY = Draw_DrawFormattedString(20, posY + SPACING_Y * 5, COLOR_RED, "Operation failed (%08lx).", (u32)res);
        else
            posY = Draw_DrawString(20, posY + SPACING_Y * 5, COLOR_GREEN, "Time/date updated successfully.");;
        Draw_FlushFramebuffer();
        Draw_Unlock();
    } while (!(waitInput() & KEY_B) && !menuShouldExit);
}

void MiscellaneousMenu_NullifyUserTimeOffset(void)
{
    Result res = ntpNullifyUserTimeOffset();

    Draw_Lock();
    Draw_ClearFramebuffer();
    Draw_FlushFramebuffer();
    Draw_Unlock();

    do {
        Draw_Lock();
        Draw_DrawMenuFrame("Miscellaneous options menu");
        if (R_SUCCEEDED(res))
            Draw_DrawString(20, 40, COLOR_GREEN, "Operation succeeded.\n\nPlease reboot to finalize the changes.");
        else
            Draw_DrawFormattedString(20, 40, COLOR_RED, "Operation failed (0x%08lx).", res);
        Draw_DrawString(20, 80, COLOR_GRAY, "Press B to go back.");
        Draw_FlushFramebuffer();
        Draw_Unlock();
    } while (!(waitInput() & KEY_B) && !menuShouldExit);
}

static Result MiscellaneousMenu_DumpDspFirmCallback(Handle procHandle, u32 textSz, u32 roSz, u32 rwSz)
{
    (void)procHandle;
    Result res = 0;

    // NOTE: we suppose .text, .rodata, .data+.bss are contiguous & in that order
    u32 rwStart = 0x00100000 + textSz + roSz;
    u32 rwEnd = rwStart + rwSz;

    // Locate the DSP firm (it's in .data, not .rodata, suprisingly)
    u32 magic;
    memcpy(&magic, "DSP1", 4);
    const u32 *off = (u32 *)rwStart;

    for (; off < (u32 *)rwEnd && *off != magic; off++);

    if (off >= (u32 *)rwEnd || off < (u32 *)(rwStart + 0x100))
        return -2;

    // Do some sanity checks
    const DspFirm *firm = (const DspFirm *)((u32)off - 0x100);
    if (firm->totalSize > 0x10000 || firm->numSegments > 10)
        return -3;
    if ((u32)firm + firm->totalSize >= rwEnd)
        return -3;

    // Dump to SD card (no point in dumping to CTRNAND as 3dsx stuff doesn't work there)
    IFile file;
    FS_Archive archive;

    // Create sdmc:/3ds directory if it doesn't exist yet
    res = FSUSER_OpenArchive(&archive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""));
    if (R_SUCCEEDED(res))
    {
        res = FSUSER_CreateDirectory(archive, fsMakePath(PATH_ASCII, "/3ds"), 0);
        if ((u32)res == 0xC82044BE) // directory already exists
            res = 0;
        FSUSER_CloseArchive(archive);
    }

    if (R_SUCCEEDED(res))
        res = IFile_Open(
            &file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""),
            fsMakePath(PATH_ASCII, "/3ds/dspfirm.cdc"), FS_OPEN_CREATE | FS_OPEN_WRITE
        );

    u64 total;
    if (R_SUCCEEDED(res))
        res = IFile_Write(&file, &total, firm, firm->totalSize, 0);
    if (R_SUCCEEDED(res))
        res = IFile_SetSize(&file, firm->totalSize); // truncate accordingly

    IFile_Close(&file);

    return res;
}
void MiscellaneousMenu_DumpDspFirm(void)
{
    Result res = OperateOnProcessByName("menu", MiscellaneousMenu_DumpDspFirmCallback);

    Draw_Lock();
    Draw_ClearFramebuffer();
    Draw_FlushFramebuffer();
    Draw_Unlock();

    do {
        Draw_Lock();
        Draw_DrawMenuFrame("Miscellaneous options menu");
        if (R_SUCCEEDED(res))
            Draw_DrawString(20, 40, COLOR_GREEN, "DSP firm. successfully written to\n/3ds/dspfirm.cdc on the SD card.");
        else
            Draw_DrawFormattedString(20, 40, COLOR_RED,
                "Operation failed (0x%08lx).\n\nMake sure that Home Menu is running and that\nyour SD card is inserted.",
                res);
        Draw_DrawString(20, 100, COLOR_GRAY, "Press B to go back.");
        Draw_FlushFramebuffer();
        Draw_Unlock();
    } while (!(waitInput() & KEY_B) && !menuShouldExit);
}



static Result MiscellaneousMenu_SetPlayCoins(u16 amount)
{
    FS_Archive archive; //extdata archive
    Handle file; //gamecoin file handle
    Result res; //result variable
    FS_Path pathData;
    //its on nand so mediatype nand and extdata id is 0xf000000b, for some reason the low comes before high here,
    //high is always 00048000 for nand extdata https://www.3dbrew.org/wiki/Extdata
    u32 extdataPath[3] = {MEDIATYPE_NAND, 0xf000000b, 0x00048000};  //type low high
    pathData.type = PATH_BINARY; //binary path because titleid
    pathData.size = 12; //3*sizeof(u32)
    pathData.data = (const void*)extdataPath; //data
    //shared extdata archive https://www.3dbrew.org/wiki/Extdata#NAND_Shared_Extdata has the f000000b archive
    res = FSUSER_OpenArchive(&archive, ARCHIVE_SHARED_EXTDATA, pathData);
    if (R_FAILED(res)) { //return if error
        return res;
    }
    // open /gamecoin.dat in extdata archive
    // https://www.3dbrew.org/wiki/Extdata#Shared_Extdata_0xf000000b_gamecoin.dat

    res = FSUSER_OpenFile(&file, archive, fsMakePath(PATH_ASCII, "/gamecoin.dat"), FS_OPEN_WRITE, 0); //open for writing, no attributes necessary
    if (R_FAILED(res)) { //return if error
        FSUSER_CloseArchive(archive); //dont care about error, just close archive since it opened without error
        return res;
    }
    // from 3dbrew
    // offset: 0x4 size: 0x2 desc: Number of Play Coins, (note: size 0x2 so its a u16 value)
    //I think we dont care about the amount of bytes written, so NULL, as buffer we use the provided u16 argument, size is sizeof(u16) which should be 0x2, as u8 (one byte) * 2 is u16
    res = FSFILE_Write(file, NULL, 0x4, &amount, sizeof(u16), 0); 
    if (R_FAILED(res)) {
        FSFILE_Close(file); //dont care about error, just close file since it opened without error
        FSUSER_CloseArchive(archive); //dont care about error, just close archive since it opened without error
        return res;
    }

    res = FSFILE_Close(file);
    if (R_FAILED(res)) { //return if error
        FSUSER_CloseArchive(archive); //dont care about error, just close archive since it opened without error
        return res;
    }
    res = FSUSER_CloseArchive(archive);
    return res;
}


void MiscellaneousMenu_EditPlayCoins(void)
{
    u16 playCoins = 300;
    Result res = 0;
    u32 pressed = 0;

    void updateDisplay(bool showResult)
    {
        Draw_Lock();
        Draw_ClearFramebuffer();
        Draw_DrawMenuFrame("Miscellaneous options menu");
        Draw_DrawFormattedString(20, 40, COLOR_WHITE, "Set Play Coins: %d", playCoins);
        
        Draw_DrawString(20, 60, COLOR_WHITE, "DPAD Up/Down: +-1\nDPAD Right/Left: +-10\nA: Apply");
        if (showResult) {
            if (R_SUCCEEDED(res))
                Draw_DrawString(20, 100, COLOR_GREEN, "Play Coins successfully set.");
            else
                Draw_DrawFormattedString(20, 100, COLOR_RED, "Error: 0x%08lx", res);
        }

        Draw_DrawString(20, 120, COLOR_GRAY, "Press B to go back.");
        Draw_DrawString(20, 140, COLOR_TITLE, "Note:");
        
        Draw_DrawString(20, 150, COLOR_WHITE, "Changes may not be applied immediately.\nIt will be applied once the app reloads the\nplay coins count.");
        Draw_FlushFramebuffer();
        Draw_Unlock();
    }

    updateDisplay(false);

    do {
        pressed = waitInputWithTimeout(50);

        if (pressed & KEY_A) {
            res = MiscellaneousMenu_SetPlayCoins(playCoins);
            updateDisplay(true);
        } else if (pressed & KEY_B) {
            return;
        } else {
            bool updated = false;
            if (pressed & KEY_DUP) {
                if (playCoins < 300)
                    playCoins++;
                else
                    playCoins = 0;
                updated = true;
            } else if (pressed & KEY_DDOWN) {
                if (playCoins > 0) {
                    playCoins--;
                } else {
                    playCoins = 300;
                }
                updated = true;
            } else if (pressed & KEY_DRIGHT) {
                if (playCoins + 10 > 300)
                    playCoins = 300;
                else
                    playCoins += 10;
                updated = true;
            } else if (pressed & KEY_DLEFT) {
                if (playCoins < 10)
                    playCoins = 0;
                else
                    playCoins -= 10;
                updated = true;
            }

            if (updated) {
                Draw_Lock();
                Draw_DrawString(20, 40, COLOR_WHITE, "Set Play Coins:         ");
                Draw_DrawFormattedString(20, 40, COLOR_WHITE, "Set Play Coins: %d", playCoins);
                Draw_DrawString(20, 100, COLOR_WHITE, "                            ");
                Draw_FlushFramebuffer();
                Draw_Unlock();
            }
        }
    } while (!menuShouldExit);
}
