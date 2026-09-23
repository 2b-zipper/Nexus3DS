
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
#include <string.h>
#include "menus/dsp_probe.h"
#include "csvc.h"
#include "draw.h"
#include "menu.h"
#include "fmt.h"
#include "ifile.h"
#include "utils.h"

#define PROBE_DIR           "/luma/dsp_probe"
#define PROBE_MAP_ADDR      0x00100000   // where snapshots get mapped in our own process
#define PROBE_MIN_REGION    0x1000
#define PROBE_MAX_REGION    0x80000      // DSP RAM is 512KB
#define PROBE_MAX_DUMPS     8
#define PROBE_MAX_PROCS     0x40
#define PROBE_MAPTXT_SIZE   0x3000

typedef struct ProbeProc {
    u32 pid;
    char name[9];
} ProbeProc;

static ProbeProc probeProcs[PROBE_MAX_PROCS];
static s32 probeProcCount;
static u32 probeSnapshotId;

static char probeMapText[PROBE_MAPTXT_SIZE];

static void DspProbe_RefreshProcs(void)
{
    u32 pids[PROBE_MAX_PROCS];
    s32 count = 0;
    probeProcCount = 0;

    if (R_FAILED(svcGetProcessList(&count, pids, PROBE_MAX_PROCS)))
        return;

    for (s32 i = 0; i < count; i++)
    {
        Handle h;
        if (R_FAILED(svcOpenProcess(&h, pids[i])))
            continue;

        ProbeProc *p = &probeProcs[probeProcCount++];
        p->pid = pids[i];
        memset(p->name, 0, sizeof(p->name));
        svcGetProcessInfo((s64 *)p->name, h, 0x10000);
        svcCloseHandle(h);
    }
}

static const char *DspProbe_StateName(u32 state)
{
    switch (state)
    {
        case MEMSTATE_FREE:       return "FREE";
        case MEMSTATE_RESERVED:   return "RESERVED";
        case MEMSTATE_IO:         return "IO";
        case MEMSTATE_STATIC:     return "STATIC";
        case MEMSTATE_CODE:       return "CODE";
        case MEMSTATE_PRIVATE:    return "PRIVATE";
        case MEMSTATE_SHARED:     return "SHARED";
        case MEMSTATE_CONTINUOUS: return "CONTINUOUS";
        case MEMSTATE_ALIAS:      return "ALIAS";
        case MEMSTATE_ALIASCODE:  return "ALIASCODE";
        case MEMSTATE_LOCKED:     return "LOCKED";
        default:                  return "?";
    }
}

static Result DspProbe_WriteFile(const char *path, const void *data, u32 size)
{
    IFile file;
    u64 total;
    Result res = IFile_Open(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""), fsMakePath(PATH_ASCII, path), FS_OPEN_CREATE | FS_OPEN_WRITE);
    if (R_SUCCEEDED(res))
        res = IFile_SetSize(&file, size);
    if (R_SUCCEEDED(res))
        res = IFile_Write(&file, &total, data, size, 0);
    IFile_Close(&file);
    return res;
}

static void DspProbe_MakeDir(void)
{
    FS_Archive archive;
    if (R_FAILED(FSUSER_OpenArchive(&archive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""))))
        return;
    FSUSER_CreateDirectory(archive, fsMakePath(PATH_ASCII, "/luma"), 0);
    FSUSER_CreateDirectory(archive, fsMakePath(PATH_ASCII, PROBE_DIR), 0);
    FSUSER_CloseArchive(archive);
}

// Writes the memory map to <name>_<pid>_<snap>_map.txt and dumps candidate regions next to it.
// Returns the number of region dumps written, or a negative value if the process cannot be opened.
static int DspProbe_DumpProcess(const ProbeProc *proc, char *status)
{
    Handle h;
    Result res = svcOpenProcess(&h, proc->pid);
    if (R_FAILED(res))
    {
        sprintf(status, "svcOpenProcess failed: %08lx", res);
        return -1;
    }

    DspProbe_MakeDir();
    probeSnapshotId++;

    char prefix[64];
    sprintf(prefix, PROBE_DIR "/%s_%lu_s%lu", proc->name, proc->pid, probeSnapshotId);

    char path[96];
    int dumps = 0;
    size_t len = 0;
    u32 address = 0;
    MemInfo memi;
    PageInfo pagei;

    len += sprintf(probeMapText + len, "process %s pid %lu\n", proc->name, proc->pid);

    while (len < PROBE_MAPTXT_SIZE - 160 && R_SUCCEEDED(svcQueryProcessMemory(&memi, &pagei, h, address)))
    {
        address = memi.base_addr + memi.size;
        if (memi.state == MEMSTATE_FREE)
            continue;

        len += sprintf(probeMapText + len, "%08lx-%08lx %-10s perm %lu",
            memi.base_addr, memi.base_addr + memi.size, DspProbe_StateName(memi.state), (u32)memi.perm);

        bool candidate = (memi.state == MEMSTATE_SHARED || memi.state == MEMSTATE_IO || memi.state == MEMSTATE_ALIAS)
            && memi.size >= PROBE_MIN_REGION && memi.size <= PROBE_MAX_REGION;

        if (candidate && dumps < PROBE_MAX_DUMPS)
        {
            Result mres = svcMapProcessMemoryEx(CUR_PROCESS_HANDLE, PROBE_MAP_ADDR, h, memi.base_addr, memi.size, 0);
            if (R_SUCCEEDED(mres))
            {
                sprintf(path, "%s_%08lx.bin", prefix, memi.base_addr);
                Result wres = DspProbe_WriteFile(path, (const void *)PROBE_MAP_ADDR, memi.size);
                svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE, PROBE_MAP_ADDR, memi.size);
                len += sprintf(probeMapText + len, " dump %s", R_SUCCEEDED(wres) ? "ok" : "write-failed");
                if (R_SUCCEEDED(wres))
                    dumps++;
            }
            else
                len += sprintf(probeMapText + len, " map-failed %08lx", (u32)mres);
        }
        len += sprintf(probeMapText + len, "\n");
    }

    svcCloseHandle(h);

    sprintf(path, "%s_map.txt", prefix);
    res = DspProbe_WriteFile(path, probeMapText, len);
    if (R_FAILED(res))
    {
        sprintf(status, "writing map failed: %08lx", res);
        return -1;
    }

    sprintf(status, "snapshot %lu: map + %d dumps saved", probeSnapshotId, dumps);
    return dumps;
}

void DspProbeMenu_Show(void)
{
    char status[64] = "";
    s32 pos = 0;
    u32 input = 0, held = 0;

    DspProbe_RefreshProcs();

    do
    {
        Draw_Lock();
        Draw_DrawMenuFrame("DSP probe");

        u32 posY = 40;
        posY = Draw_DrawString(20, posY, COLOR_WHITE, "A: dump selected process, Y: refresh list.\n");
        posY = Draw_DrawString(20, posY, COLOR_WHITE, "Pick 'dsp' and your running game/app.\n");
        posY = Draw_DrawString(20, posY, COLOR_WHITE, "Output: " PROBE_DIR "\n") + SPACING_Y;

        // Show a window of the list around the cursor
        s32 first = pos - 6 < 0 ? 0 : pos - 6;
        for (s32 i = first; i < probeProcCount && i < first + 12; i++)
            posY = Draw_DrawFormattedString(30, posY, i == pos ? COLOR_CYAN : COLOR_WHITE, "%s%3lu %s\n", i == pos ? ">" : " ", probeProcs[i].pid, probeProcs[i].name);

        Draw_DrawString(20, SCREEN_BOT_HEIGHT - 20, COLOR_YELLOW, status);

        Draw_FlushFramebuffer();
        Draw_Unlock();

        input = waitInputWithTimeoutEx(&held, -1);
        (void)held;

        if (input & KEY_DOWN)
            pos++;
        if (input & KEY_UP)
            pos--;
        if (probeProcCount > 0)
            pos = (pos + probeProcCount) % probeProcCount;
        if (input & KEY_Y)
        {
            DspProbe_RefreshProcs();
            pos = 0;
            status[0] = '\0';
        }
        if ((input & KEY_A) && probeProcCount > 0)
        {
            strcpy(status, "dumping...");
            DspProbe_DumpProcess(&probeProcs[pos], status);
        }
    }
    while (!(input & KEY_B) && !menuShouldExit);
}
