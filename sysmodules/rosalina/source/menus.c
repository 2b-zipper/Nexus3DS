/*
 *   This file is part of Luma3DS
 *   Copyright (C) 2016-2020 Aurora Wright, TuxSH
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
#include <3ds/os.h>
#include "menu.h"
#include "menus.h"
#include "draw.h"
#include "menus/process_list.h"
#include "menus/n3ds.h"
#include "menus/debugger.h"
#include "menus/miscellaneous.h"
#include "menus/home_button_sim.h"
#include "menus/sysconfig.h"
#include "menus/screen_filters.h"
#include "menus/plugin_options.h"
#include "menus/config_extra.h"
#include "plugin.h"
#include "pmdbgext.h"
#include "ifile.h"
#include "memory.h"
#include "fmt.h"
#include "process_patches.h"
#include "luma_config.h"

extern config_extra configExtra;

Menu rosalinaMenu = {
    "Rosalina menu",
    {
        { "Take screenshot", METHOD, .method = &RosalinaMenu_TakeScreenshot },
        { "Screen filters...", MENU, .menu = &screenFiltersMenu },
        { "Cheats...", METHOD, .method = &RosalinaMenu_Cheats },
        { "", MENU, .menu = &pluginOptionsMenu}, // Plugin loader
        { "New 3DS settings:", MENU, .menu = &N3DSMenu, .visibility = &menuCheckN3ds },
        { "Process list", METHOD, .method = &RosalinaMenu_ProcessList },
        { "Debugger options...", MENU, .menu = &debuggerMenu },
        { "System configuration...", MENU, .menu = &sysconfigMenu },
        { "Miscellaneous options...", MENU, .menu = &miscellaneousMenu },
        { "Save settings", METHOD, .method = &RosalinaMenu_SaveSettings },
        { "Return to HOME Menu", METHOD, .method = &RosalinaMenu_ReturnToHomeMenu, .visibility = &HomeButtonSimMenu_ShouldShowReturnToHomeMenu },
        { "Power off / reboot", METHOD, .method = &RosalinaMenu_PowerOffOrReboot },
        { "System info", METHOD, .method = &RosalinaMenu_ShowSystemInfo },
        { "Credits", METHOD, .method = &RosalinaMenu_ShowCredits },
        { "Debug info", METHOD, .method = &RosalinaMenu_ShowDebugInfo, .visibility = &rosalinaMenuShouldShowDebugInfo },
        {},
    }};

Result CopyFileInSdmc(const char *src, const char *dst)
{
    FS_Archive sdmcArchive;
    u64 remaining;
    u64 total;
    Result res;
    IFile srcFile;
    IFile dstFile;
    const u32 bufferSize = 2048;
    u8 buffer[bufferSize];

    // Open the sdmc archive
    if (R_FAILED(res = FSUSER_OpenArchive(&sdmcArchive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""))))
        return res;

    // Open the source file
    if (R_FAILED(res = IFile_OpenFromArchive(&srcFile, sdmcArchive, fsMakePath(PATH_ASCII, src), FS_OPEN_READ)))
    {
        FSUSER_CloseArchive(sdmcArchive);
        return res;
    }

    // Open the destination file
    if (R_FAILED(res = IFile_OpenFromArchive(&dstFile, sdmcArchive, fsMakePath(PATH_ASCII, dst), FS_OPEN_WRITE)))
    {
        IFile_Close(&srcFile);
        FSUSER_CloseArchive(sdmcArchive);
        return res;
    }

    // Close the sdmc archive
    FSUSER_CloseArchive(sdmcArchive);

    // Clear the destination file
    if (R_FAILED(res = IFile_SetSize(&dstFile, 0)))
    {
        IFile_Close(&srcFile);
        IFile_Close(&dstFile);
        return res;
    }

    // Get the file size
    if (R_FAILED(IFile_GetSize(&srcFile, &remaining)))
    {
        IFile_Close(&srcFile);
        IFile_Close(&dstFile);
        return res;
    }

    // Copy the file
    while (remaining != 0)
    {
        u64 size = remaining > bufferSize ? bufferSize : remaining;

        if (R_FAILED(IFile_Read(&srcFile, &total, (void *)buffer, size)) || R_FAILED(IFile_Write(&dstFile, &total, (void *)buffer, total, 0)))
        {
            IFile_Close(&srcFile);
            IFile_Close(&dstFile);
            return res;
        }
        remaining -= total;
    }

    // Close the files
    IFile_Close(&srcFile);
    IFile_Close(&dstFile);

    return 0;
}

bool rosalinaMenuShouldShowDebugInfo(void)
{
    // Don't show on release builds

    s64 out;
    svcGetSystemInfo(&out, 0x10000, 0x200);
    return out == 0;
}

void RosalinaMenu_SaveSettings(void)
{
    Result res = LumaConfig_SaveSettings();
    Draw_Lock();
    Draw_ClearFramebuffer();
    Draw_FlushFramebuffer();
    Draw_Unlock();

    do
    {
        Draw_Lock();
        Draw_DrawMenuFrame("Save settings");
        if (R_SUCCEEDED(res))
            Draw_DrawString(10, 40, COLOR_WHITE, "Operation succeeded.");
        else
            Draw_DrawFormattedString(10, 40, COLOR_WHITE, "Operation failed (0x%08lx).", res);
        Draw_FlushFramebuffer();
        Draw_Unlock();
    }
    while (!(waitInput() & KEY_B) && !menuShouldExit);
}

void RosalinaMenu_ReturnToHomeMenu(void)
{
    Draw_Lock();
    Draw_ClearFramebuffer();
    Draw_FlushFramebuffer();
    Draw_Unlock();

    do
    {
        Draw_Lock();
        Draw_DrawMenuFrame("Return to HOME Menu");
        Draw_DrawString(10, 40, COLOR_WHITE, "Simulate pressing the HOME button.");
        Draw_DrawString(10, 50, COLOR_WHITE, "Useful if your HOME button is broken.");
        Draw_DrawString(10, 70, COLOR_WHITE, "Press A to proceed.");
        Draw_DrawString(10, 80, COLOR_WHITE, "Press B to go back.");
        Draw_FlushFramebuffer();
        Draw_Unlock();

        u32 pressed = waitInputWithTimeout(1000);

        if (pressed & KEY_A)
        {
            // Simulate HOME button press
            srvPublishToSubscriber(0x204, 0);
            return;
        }
        else if (pressed & KEY_B)
            return;
    }
    while (!menuShouldExit);
}

void RosalinaMenu_PowerOffOrReboot(void)
{
    Draw_Lock();
    Draw_ClearFramebuffer();
    Draw_FlushFramebuffer();
    Draw_Unlock();

    do
    {
        Draw_Lock();
        Draw_DrawMenuFrame("Power Off / Reboot");
        Draw_DrawString(10, 40, COLOR_WHITE, "Press A to power off.");
        Draw_DrawString(10, 50, COLOR_WHITE, "Press Y to reboot.");
        Draw_DrawString(10, 60, COLOR_WHITE, "Press X to force reboot.");
        Draw_DrawString(10, 70, COLOR_WHITE, "Press B to go back.");
        Draw_DrawString(10, 90, COLOR_WHITE, "Note: Force reboot may corrupt your SD card.");
        Draw_FlushFramebuffer();
        Draw_Unlock();

        u32 pressed = waitInputWithTimeout(1000);

        if (pressed & KEY_Y)
        {
            menuLeave();
            APT_HardwareResetAsync();
            return;
        }
        else if (pressed & KEY_A)
        {
            // Soft shutdown
            menuLeave();
            srvPublishToSubscriber(0x203, 0);
            return;
        }
        else if(pressed & KEY_X)
        {
            // Force reboot
            svcKernelSetState(7);
            __builtin_unreachable();
            return;
        }
        else if (pressed & KEY_B)
            return;
    }
    while (!menuShouldExit);
}

void RosalinaMenu_ShowSystemInfo(void)
{
    u32 kver = osGetKernelVersion();

    do
    {
        Draw_Lock();
        Draw_DrawMenuFrame("Rosalina -- System info");

        u32 posY = 40;

        if (areScreenTypesInitialized)
        {
            posY = Draw_DrawFormattedString(10, posY, COLOR_WHITE, "Top screen type:    %s\n", topScreenType);
            posY = Draw_DrawFormattedString(10, posY, COLOR_WHITE, "Bottom screen type: %s\n\n", bottomScreenType);
        }

        posY = Draw_DrawFormattedString(10, posY, COLOR_WHITE, "Kernel version:     %lu.%lu-%lu\n\n", GET_VERSION_MAJOR(kver), GET_VERSION_MINOR(kver), GET_VERSION_REVISION(kver));
        if (mcuFwVersion != 0 && mcuInfoTableRead)
        {
            posY = Draw_DrawFormattedString(10, posY, COLOR_WHITE, "MCU FW version:     %lu.%lu\n", GET_VERSION_MAJOR(mcuFwVersion), GET_VERSION_MINOR(mcuFwVersion));
            posY = Draw_DrawFormattedString(10, posY, COLOR_WHITE, "PMIC vendor:        %hhu\n", mcuInfoTable[1]);
            posY = Draw_DrawFormattedString(10, posY, COLOR_WHITE, "Battery vendor:     %hhu\n", mcuInfoTable[2]);
            posY = Draw_DrawString(10, posY, COLOR_WHITE, "\n");
        }

        u64 titleId = 0;
        Get_TitleID(&titleId);
        if (titleId != 0)
            posY = Draw_DrawFormattedString(10, posY, COLOR_WHITE, "Title ID:           %016llX\n", titleId);
        else
            posY = Draw_DrawString(10, posY, COLOR_WHITE, "Title ID:           Not Found\n");

        Draw_FlushFramebuffer();
        Draw_Unlock();
    }
    while (!(waitInput() & KEY_B) && !menuShouldExit);
}

void RosalinaMenu_ShowDebugInfo(void)
{
    Draw_Lock();
    Draw_ClearFramebuffer();
    Draw_FlushFramebuffer();
    Draw_Unlock();

    char memoryMap[512];
    formatMemoryMapOfProcess(memoryMap, 511, CUR_PROCESS_HANDLE);

    s64 kextAddrSize;
    svcGetSystemInfo(&kextAddrSize, 0x10000, 0x300);
    u32 kextPa = (u32)((u64)kextAddrSize >> 32);
    u32 kextSize = (u32)kextAddrSize;

    FS_SdMmcSpeedInfo speedInfo;

    do
    {
        Draw_Lock();
        Draw_DrawMenuFrame("Rosalina -- Debug info");

        u32 posY = 40;

        posY = Draw_DrawString(10, posY, COLOR_WHITE, memoryMap);
        posY = Draw_DrawFormattedString(10, posY, COLOR_WHITE, "Kernel ext PA: %08lx - %08lx\n\n", kextPa, kextPa + kextSize);
        if (R_SUCCEEDED(FSUSER_GetSdmcSpeedInfo(&speedInfo)))
        {
            u32 clkDiv = 1 << (1 + (speedInfo.sdClkCtrl & 0xFF));
            posY = Draw_DrawFormattedString(
                10, posY, COLOR_WHITE, "SDMC speed: HS=%d %lukHz\n",
                (int)speedInfo.highSpeedModeEnabled, SYSCLOCK_SDMMC / (1000 * clkDiv)
            );
        }
        if (R_SUCCEEDED(FSUSER_GetNandSpeedInfo(&speedInfo)))
        {
            u32 clkDiv = 1 << (1 + (speedInfo.sdClkCtrl & 0xFF));
            posY = Draw_DrawFormattedString(
                10, posY, COLOR_WHITE, "NAND speed: HS=%d %lukHz\n",
                (int)speedInfo.highSpeedModeEnabled, SYSCLOCK_SDMMC / (1000 * clkDiv)
            );
        }
        {
            posY = Draw_DrawFormattedString(
                10, posY, COLOR_WHITE, "APPMEMTYPE: %lu\n",
                OS_KernelConfig->app_memtype
            );
        }
        Draw_FlushFramebuffer();
        Draw_Unlock();
    }
    while (!(waitInput() & KEY_B) && !menuShouldExit);
}

void RosalinaMenu_ShowCredits(void)
{
    Draw_Lock();
    Draw_ClearFramebuffer();
    Draw_FlushFramebuffer();
    Draw_Unlock();

    do
    {
        Draw_Lock();
        Draw_DrawMenuFrame("Rosalina -- Nexus3DS credits");

        u32 posY = Draw_DrawString(10, 40, COLOR_WHITE, "Nexus3DS (c) 2024-2025 Nexus3DS Team") + SPACING_Y;
        posY = Draw_DrawString(10, posY, COLOR_WHITE, "Based on Luma3DS (c) 2016-2025 AuroraWright, TuxSH") + SPACING_Y;

        posY = Draw_DrawString(10, posY + SPACING_Y, COLOR_WHITE, "3DSX loading code by fincs");
        posY = Draw_DrawString(10, posY + SPACING_Y, COLOR_WHITE, "Networking code & basic GDB functionality by Stary");
        posY = Draw_DrawString(10, posY + SPACING_Y, COLOR_WHITE, "InputRedirection by Stary (PoC by ShinyQuagsire)");

        posY += 2 * SPACING_Y;

        Draw_DrawString(10, posY, COLOR_WHITE,
            (
                "Special thanks to:\n"
                "  fincs, WinterMute, mtheall, piepie62,\n"
                "  Luma3DS contributors, libctru contributors,\n"
                "  other people\n\n"
                "People who made this fork possible:\n"
                "  DullPointer, Tekito-256, Gruetzig, cooolgamer,\n"
                "  suprdratts/hide0123, Pixel-Pop, 2b-zipper"
            ));

        Draw_FlushFramebuffer();
        Draw_Unlock();
    }
    while (!(waitInput() & KEY_B) && !menuShouldExit);
}

#define TRY(expr) if(R_FAILED(res = (expr))) goto end;

static s64 timeSpentConvertingScreenshot = 0;
static s64 timeSpentWritingScreenshot = 0;

static u64 GetCurrentTitleId(void)
{
    FS_ProgramInfo programInfo;
    u32 pid;
    u32 launchFlags;
    Result res = PMDBG_GetCurrentAppInfo(&programInfo, &pid, &launchFlags);
    if (R_FAILED(res)) {
        return 0;
    }
    return programInfo.programId;
}

static Result RosalinaMenu_WriteScreenshot(IFile *file, u32 width, bool top, bool left)
{
    u64 total;
    Result res = 0;
    u32 lineSize = 3 * width;

    // When dealing with 800px mode (800x240 with half-width pixels), duplicate each line
    // to restore aspect ratio and obtain faithful 800x480 screenshots
    u32 scaleFactorY = width > 400 ? 2 : 1;
    u32 numLinesScaled = 240 * scaleFactorY;
    u32 remaining = lineSize * numLinesScaled;

    TRY(Draw_AllocateFramebufferCacheForScreenshot(remaining));

    u8 *framebufferCache = (u8 *)Draw_GetFramebufferCache();
    u8 *framebufferCacheEnd = framebufferCache + Draw_GetFramebufferCacheSize();

    u8 *buf = framebufferCache;
    Draw_CreateBitmapHeader(framebufferCache, width, numLinesScaled);
    const u32 headerSize = 0x40;
    buf += headerSize;

    u32 y = 0;
    // Our buffer might be smaller than the size of the screenshot...
    while (remaining != 0)
    {
        s64 t0 = svcGetSystemTick();
        u32 available = (u32)(framebufferCacheEnd - buf);
        u32 size = available < remaining ? available : remaining;
        u32 nlines = size / (lineSize * scaleFactorY);
        Draw_ConvertFrameBufferLines(buf, width, y, nlines, scaleFactorY, top, left);

        s64 t1 = svcGetSystemTick();
        timeSpentConvertingScreenshot += t1 - t0;
        TRY(IFile_Write(file, &total, framebufferCache, (y == 0 ? headerSize : 0) + lineSize * nlines * scaleFactorY, 0)); // don't forget to write the header
        timeSpentWritingScreenshot += svcGetSystemTick() - t1;

        y += nlines;
        remaining -= lineSize * nlines * scaleFactorY;
        buf = framebufferCache;
    }
end:

    Draw_FreeFramebufferCache();
    return res;
}

static Result RosalinaMenu_WriteCombinedScreenshot(IFile *file, u32 topWidth, u32 bottomWidth, bool is3d)
{
    (void)is3d;
    u64 total;
    Result res = 0;
    
    u32 topScaleFactorY = topWidth > 400 ? 2 : 1;
    u32 topHeightScaled = 240 * topScaleFactorY;
    u32 bottomHeightScaled = 240;
    u32 combinedHeight = topHeightScaled + bottomHeightScaled;
    
    u32 combinedWidth = topWidth;
    u32 combinedLineSize = 3 * combinedWidth;
    u32 totalImageSize = combinedLineSize * combinedHeight;
    
    TRY(Draw_AllocateFramebufferCacheForScreenshot(totalImageSize + 0x40));
    
    u8 *framebufferCache = (u8 *)Draw_GetFramebufferCache();
    u8 *buf = framebufferCache;
    
    Draw_CreateCombinedBitmapHeader(buf, combinedWidth, combinedHeight);
    buf += 0x40;
    
    s64 t0 = svcGetSystemTick();

    // Convert and center bottom screen
    u32 bottomLineSize = 3 * bottomWidth;
    Draw_ConvertFrameBufferLines(buf, bottomWidth, 0, 240, 1, false, true);

    u32 offset = (combinedWidth - bottomWidth) / 2;
    for (int line = bottomHeightScaled - 1; line >= 0; line--) {
        u8 *srcLine = buf + line * bottomLineSize;
        u8 *dstLine = buf + line * combinedLineSize + offset * 3;
        memmove(dstLine, srcLine, bottomLineSize);
        memset(buf + line * combinedLineSize, 0, offset * 3);
        memset(dstLine + bottomLineSize, 0, (combinedWidth - bottomWidth - offset) * 3);
    }

    // Convert top screen
    u8 *topBuf = buf + bottomHeightScaled * combinedLineSize;
    Draw_ConvertFrameBufferLines(topBuf, topWidth, 0, 240, topScaleFactorY, true, true);
    
    s64 t1 = svcGetSystemTick();
    timeSpentConvertingScreenshot += t1 - t0;
    
    TRY(IFile_Write(file, &total, framebufferCache, 0x40 + totalImageSize, 0));
    
    timeSpentWritingScreenshot += svcGetSystemTick() - t1;
    
end:
    Draw_FreeFramebufferCache();
    return res;
}

void RosalinaMenu_TakeScreenshot(void)
{
    IFile file = {0};
    Result res = 0;

    char filename[256];
    char dateTimeStr[32];

    FS_Archive archive;
    FS_ArchiveID archiveId;
    s64 out;
    bool isSdMode;

    timeSpentConvertingScreenshot = 0;
    timeSpentWritingScreenshot = 0;

    if (R_FAILED(svcGetSystemInfo(&out, 0x10000, 0x203))) svcBreak(USERBREAK_ASSERT);
    isSdMode = (bool)out;

    archiveId = isSdMode ? ARCHIVE_SDMC : ARCHIVE_NAND_RW;
    Draw_Lock();
    Draw_RestoreFramebuffer();
    Draw_FreeFramebufferCache();

    svcFlushEntireDataCache();

    bool is3d;
    u32 topWidth, bottomWidth; // actually Y-dim

    Draw_GetCurrentScreenInfo(&bottomWidth, &is3d, false);
    Draw_GetCurrentScreenInfo(&topWidth, &is3d, true);

    res = FSUSER_OpenArchive(&archive, archiveId, fsMakePath(PATH_EMPTY, ""));
    if (R_SUCCEEDED(res))
    {
        res = FSUSER_CreateDirectory(archive, fsMakePath(PATH_ASCII, "/luma/screenshots"), 0);
        if ((u32)res == 0xC82044BE) // directory already exists
            res = 0;
        FSUSER_CloseArchive(archive);
    }
    else
    {
        archive = 0;
        goto end;
    }

    dateTimeToString(dateTimeStr, osGetTime(), true);

    if (configExtra.screenshotDateFolders) {
        res = FSUSER_OpenArchive(&archive, archiveId, fsMakePath(PATH_EMPTY, ""));
        if (R_SUCCEEDED(res)) {
            char dateFolderPath[64];
            char dateOnlyStr[16];
            
            strncpy(dateOnlyStr, dateTimeStr, 10);
            dateOnlyStr[10] = '\0';
            
            sprintf(dateFolderPath, "/luma/screenshots/%s", dateOnlyStr);
            res = FSUSER_CreateDirectory(archive, fsMakePath(PATH_ASCII, dateFolderPath), 0);
            if ((u32)res == 0xC82044BE) // directory already exists
                res = 0;
            FSUSER_CloseArchive(archive);
        }
        else
        {
            archive = 0;
            goto end;
        }
    }

    char titleIdStr[18] = {0};
    u64 titleId = GetCurrentTitleId();
    if (titleId != 0) {
        sprintf(titleIdStr, "_%016llX", titleId);
    }

    char folderPath[64];
    if (configExtra.screenshotDateFolders) {
        char dateOnlyStr[16];
        strncpy(dateOnlyStr, dateTimeStr, 10);
        dateOnlyStr[10] = '\0';
        sprintf(folderPath, "/luma/screenshots/%s", dateOnlyStr);
    } else {
        strcpy(folderPath, "/luma/screenshots");
    }

    if (configExtra.screenshotCombined) {
        sprintf(filename, "%s/%s%s_cmb.bmp", folderPath, dateTimeStr, titleIdStr);
        TRY(IFile_Open(&file, archiveId, fsMakePath(PATH_EMPTY, ""), fsMakePath(PATH_ASCII, filename), FS_OPEN_CREATE | FS_OPEN_WRITE));
        TRY(RosalinaMenu_WriteCombinedScreenshot(&file, topWidth, bottomWidth, is3d));
        TRY(IFile_Close(&file));
    } else {
        sprintf(filename, "%s/%s%s_top.bmp", folderPath, dateTimeStr, titleIdStr);
        TRY(IFile_Open(&file, archiveId, fsMakePath(PATH_EMPTY, ""), fsMakePath(PATH_ASCII, filename), FS_OPEN_CREATE | FS_OPEN_WRITE));
        TRY(RosalinaMenu_WriteScreenshot(&file, topWidth, true, true));
        TRY(IFile_Close(&file));

        sprintf(filename, "%s/%s%s_bot.bmp", folderPath, dateTimeStr, titleIdStr);
        TRY(IFile_Open(&file, archiveId, fsMakePath(PATH_EMPTY, ""), fsMakePath(PATH_ASCII, filename), FS_OPEN_CREATE | FS_OPEN_WRITE));
        TRY(RosalinaMenu_WriteScreenshot(&file, bottomWidth, false, true));
        TRY(IFile_Close(&file));
    }

    if (is3d && (Draw_GetCurrentFramebufferAddress(true, true) != Draw_GetCurrentFramebufferAddress(true, false)))
    {
        sprintf(filename, "%s/%s%s_top_right.bmp", folderPath, dateTimeStr, titleIdStr);
        TRY(IFile_Open(&file, archiveId, fsMakePath(PATH_EMPTY, ""), fsMakePath(PATH_ASCII, filename), FS_OPEN_CREATE | FS_OPEN_WRITE));
        TRY(RosalinaMenu_WriteScreenshot(&file, topWidth, true, false));
        TRY(IFile_Close(&file));
    }

end:
    IFile_Close(&file);

    if (archive != 0)
        FSUSER_CloseArchive(archive);

    if (R_FAILED(Draw_AllocateFramebufferCache(FB_BOTTOM_SIZE)))
        __builtin_trap(); // We're f***ed if this happens

    svcFlushEntireDataCache();
    Draw_SetupFramebuffer();
    Draw_Unlock();

    do
    {
        Draw_Lock();
        Draw_DrawMenuFrame("Screenshot");
        if (R_FAILED(res))
            Draw_DrawFormattedString(10, 40, COLOR_WHITE, "Operation failed (0x%08lx).", (u32)res);
        else
        {
            u32 t1 = (u32)(1000 * timeSpentConvertingScreenshot / SYSCLOCK_ARM11);
            u32 t2 = (u32)(1000 * timeSpentWritingScreenshot / SYSCLOCK_ARM11);
            u32 posY = 40;
            posY = Draw_DrawString(10, posY, COLOR_WHITE, "Operation succeeded.\n\n");
            posY = Draw_DrawFormattedString(10, posY, COLOR_WHITE, "Time spent converting:    %5lums\n", t1);
            posY = Draw_DrawFormattedString(10, posY, COLOR_WHITE, "Time spent writing files: %5lums\n", t2);
        }

        Draw_FlushFramebuffer();
        Draw_Unlock();
    }
    while (!(waitInput() & KEY_B) && !menuShouldExit);
}

static Result menuWriteSelfScreenshot(IFile *file)
{
    u64 total;
    Result res = 0;

    u32 width = 320;
    u32 lineSize = 3 * width;

    u32 scaleFactorY = 1;
    u32 numLinesScaled = 240 * scaleFactorY;

    u32 addr = 0x0D800000; // keep this in check
    u32 tmp;

    u32 size = ((54 + lineSize * numLinesScaled * scaleFactorY) + 0xFFF) >> 12 << 12; // round-up
    u8 *buffer = NULL;

    TRY(svcControlMemoryEx(&tmp, addr, 0, size, MEMOP_ALLOC | MEMOP_REGION_SYSTEM, MEMPERM_READWRITE, true));
    buffer = (u8 *)addr;

    Draw_CreateBitmapHeader(buffer, width, numLinesScaled);

    Draw_ConvertFrameBufferLines(buffer + 54, width, 0, numLinesScaled, scaleFactorY, false, false);
    TRY(IFile_Write(file, &total, buffer, 54 + lineSize * numLinesScaled * scaleFactorY, 0)); // don't forget to write the header

end:
    if (buffer)
        svcControlMemoryEx(&tmp, addr, 0, size, MEMOP_FREE, MEMPERM_DONTCARE, false);

    return res;
}

void menuTakeSelfScreenshot(void)
{
    // Optimized for N3DS. May fail due to OOM.

    IFile file = {0};
    Result res = 0;

    char filename[256];
    char dateTimeStr[64];

    FS_Archive archive;
    FS_ArchiveID archiveId;
    s64 out;
    bool isSdMode;

    timeSpentConvertingScreenshot = 0;
    timeSpentWritingScreenshot = 0;

    if (R_FAILED(svcGetSystemInfo(&out, 0x10000, 0x203))) svcBreak(USERBREAK_ASSERT);
    isSdMode = (bool)out;

    archiveId = isSdMode ? ARCHIVE_SDMC : ARCHIVE_NAND_RW;
    Draw_Lock();
    svcFlushEntireDataCache();

    res = FSUSER_OpenArchive(&archive, archiveId, fsMakePath(PATH_EMPTY, ""));
    if (R_SUCCEEDED(res))
    {
        res = FSUSER_CreateDirectory(archive, fsMakePath(PATH_ASCII, "/luma/screenshots"), 0);
        if ((u32)res == 0xC82044BE) // directory already exists
            res = 0;
        FSUSER_CloseArchive(archive);
    }
    else
    {
        archive = 0;
        goto end;
    }

    dateTimeToString(dateTimeStr, osGetTime(), true);

    if (configExtra.screenshotDateFolders) {
        res = FSUSER_OpenArchive(&archive, archiveId, fsMakePath(PATH_EMPTY, ""));
        if (R_SUCCEEDED(res)) {
            char dateFolderPath[64];
            char dateOnlyStr[16];
            
            strncpy(dateOnlyStr, dateTimeStr, 10);
            dateOnlyStr[10] = '\0';
            
            sprintf(dateFolderPath, "/luma/screenshots/%s", dateOnlyStr);
            res = FSUSER_CreateDirectory(archive, fsMakePath(PATH_ASCII, dateFolderPath), 0);
            if ((u32)res == 0xC82044BE) // directory already exists
                res = 0;
            FSUSER_CloseArchive(archive);
        }
        else
        {
            archive = 0;
            goto end;
        }
    }

    char titleIdStr[18] = {0};
    u64 titleId = GetCurrentTitleId();
    if (titleId != 0) {
        sprintf(titleIdStr, "_%016llX", titleId);
    }

    char folderPath[64];
    if (configExtra.screenshotDateFolders) {
        char dateOnlyStr[16];
        strncpy(dateOnlyStr, dateTimeStr, 10);
        dateOnlyStr[10] = '\0';
        sprintf(folderPath, "/luma/screenshots/%s", dateOnlyStr);
    } else {
        strcpy(folderPath, "/luma/screenshots");
    }

    sprintf(filename, "%s/rosalina_menu_%s%s.bmp", folderPath, dateTimeStr, titleIdStr);

    TRY(IFile_Open(&file, archiveId, fsMakePath(PATH_EMPTY, ""), fsMakePath(PATH_ASCII, filename), FS_OPEN_CREATE | FS_OPEN_WRITE));
    TRY(menuWriteSelfScreenshot(&file));

end:
    IFile_Close(&file);

    if (archive != 0)
        FSUSER_CloseArchive(archive);
}

#undef TRY
