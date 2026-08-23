#include "svc/SysPluginLoader.h"
#include "svc/SysPlugin3nrGenerated.h"
#include "sysplugin_entry.h"

extern u32 convertVAToPA(const void *address, bool writeCheck);

#define PLUGIN_HEADER_SIZE       0x30u
#define SYSPLUGIN_3NR_PAIR_OFF   0x04u
#define SYSPLUGIN_NAME_SIZE      256u
#define ARCHIVE_SDMC             0x00000009u
#define FS_OPEN_READ             1u

#define MEMSTATE_FREE            0u
#define MEMOP_FREE               1u
#define MEMOP_ALLOC              3u
#define MEMOP_PROT               6u
#define MEMOP_REGION_SYSTEM      0x200u
#define MEMPERM_READWRITE        3u
#define MEMPERM_READEXECUTE      5u

#define SP_RESULT_OUT_OF_MEMORY  ((Result)0xD900182F)
#define SP_PLUGIN_MARKED         2u

#define ALIGN_PAGE_DOWN(x)       ((x) & ~0xFFFu)

#define SP_FAILED(x)             ((s32)(x) < 0)

typedef struct
{
    u32 base_addr;
    u32 size;
    u32 perm;
    u32 state;
} SysPlgMemInfo;

typedef struct
{
    u32 flags;
} SysPlgPageInfo;

extern Result svcQueryMemory(
    SysPlgMemInfo *memoryInfo,
    SysPlgPageInfo *pageInfo,
    u32 address
);
extern Result svcCloseHandle(Handle handle);
extern void svcSleepThread(s64 nanoseconds);
extern Result svcOpenProcess(Handle *process, u32 processId);
extern Result svcGetProcessId(u32 *processId, Handle process);
extern Result svcControlProcessMemory(
    Handle process,
    u32 addr0,
    u32 addr1,
    u32 size,
    u32 operation,
    u32 permission
);
extern Result svcControlMemoryUnsafe(
    u32 *out,
    u32 address,
    u32 size,
    u32 operation,
    u32 permission
);
extern void svcFlushEntireDataCache(void);
extern void svcInvalidateEntireInstructionCache(void);

typedef struct
{
    u32 magic;
    u32 plgid;
    u32 pluginCodeSize;
    u32 pluginDataSize;
    u32 pluginBssSize;
    u32 pluginPtrSize;
    u32 repairSize;
    u32 abiLo;
    u32 abiHi;
    u32 expectedEnvLo;
    u32 expectedEnvHi;
    u32 metadataSize;
} SysPluginHeader;

typedef SysPluginEntry SysPlugin;


static const char SP_PLUGINS_DIR[] = "/luma/plugins";
static const char SP_THREE_NR_PATH[] = "/boot.3nr";
static const char SP_THREE_NR_KEYED_PATH[] = SYSPLUGIN_3NR_KEYED_PATH;
static volatile bool spLocked = true;

__attribute__((noinline)) static Result SP_Unlock(Result result)
{
    spLocked = false;
    return result;
}

static bool SP_AddChecked(u32 a, u32 b, u32 *out)
{
    u32 value = a + b;

    if (value < a)
        return false;

    *out = value;
    return true;
}

static bool SP_AlignPageChecked(u32 value, u32 *out)
{
    if (value > 0xFFFFF000u)
        return false;

    *out = (value + 0xFFFu) & ~0xFFFu;
    return true;
}

static u32 SP_StringLength(const char *string)
{
    u32 length = 0;

    while (string[length])
        length++;

    return length;
}

static void SP_CopyString(char *dst, const char *src)
{
    u32 i = 0;

    while (i + 1 < SYSPLUGIN_NAME_SIZE && src[i])
    {
        dst[i] = src[i];
        i++;
    }

    dst[i] = 0;
}

static s32 SP_StringCompare(const char *a, const char *b)
{
    while (*a && *a == *b)
    {
        a++;
        b++;
    }

    return (s32)(u8)*a - (s32)(u8)*b;
}


static void SP_MakePluginPath(const char *name, char *path)
{
    u32 pos = 0;
    u32 i = 0;

    while (SP_PLUGINS_DIR[pos])
    {
        path[pos] = SP_PLUGINS_DIR[pos];
        pos++;
    }

    path[pos++] = '/';

    while (name[i])
        path[pos++] = name[i++];

    path[pos] = 0;
}

static SysPlgPath SP_MakeAsciiPath(const char *string)
{
    SysPlgPath path;

    path.type = SYSPLG_PATH_ASCII;
    path.size = SP_StringLength(string) + 1;
    path.data = string;

    return path;
}

static bool SP_ReadExact(
    const SysPluginHost *host,
    Handle file,
    u64 offset,
    void *dst,
    u32 size
)
{
    u32 bytesRead = 0;

    if (!size)
        return true;

    if (SP_FAILED(host->FSFILE_Read(file, &bytesRead, offset, dst, size)))
        return false;

    return bytesRead == size;
}

static bool SP_ReadRelocPair(
    const SysPluginHost *host,
    Handle file,
    u32 ptrDataStart,
    u32 pluginPtrSize,
    u32 *totalRead,
    u32 out[2]
)
{
    if (pluginPtrSize - *totalRead < 8)
        return false;

    if (!SP_ReadExact(host, file, ptrDataStart + *totalRead, out, 8))
        return false;

    *totalRead += 8;
    return true;
}

static bool SP_ReadHeader(
    const SysPluginHost *host,
    Handle file,
    u32 fileOffset,
    SysPluginHeader *header,
    u32 *nextOffset
)
{
    u32 end;

    if (!SP_ReadExact(host, file, fileOffset, header, sizeof(*header)) ||
        !SP_AddChecked(fileOffset, PLUGIN_HEADER_SIZE, &end) ||
        !SP_AddChecked(end, header->pluginPtrSize, &end) ||
        !SP_AddChecked(end, header->pluginCodeSize, &end) ||
        !SP_AddChecked(end, header->pluginDataSize, &end) ||
        !SP_AddChecked(end, header->repairSize, &end) ||
        end > 0xFFFFFFF0u)
    {
        return false;
    }

    end = (end + 0xFu) & ~0xFu;
    return SP_AddChecked(end, header->metadataSize, nextOffset) && *nextOffset > fileOffset;
}

static Result SP_AllocPages(
    u32 size,
    u32 rangeLow,
    u32 rangeHigh,
    bool downward,
    u32 *outAddress
)
{
    SysPlgMemInfo memoryInfo;
    SysPlgPageInfo pageInfo;
    Result lastResult = SP_RESULT_OUT_OF_MEMORY;
    u32 alignedSize = (size + 0xFFFu) & ~0xFFFu;

    if (alignedSize > rangeHigh - rangeLow)
        return SP_RESULT_OUT_OF_MEMORY;

    u32 scan = downward ? rangeHigh - 1 : rangeLow;

    while (scan >= rangeLow && scan < rangeHigh)
    {
        Result queryResult = svcQueryMemory(&memoryInfo, &pageInfo, scan);
        u32 regionEnd;

        if (SP_FAILED(queryResult))
            return queryResult;

        if (!SP_AddChecked(memoryInfo.base_addr, memoryInfo.size, &regionEnd))
            regionEnd = 0xFFFFFFFFu;

        if (memoryInfo.state == MEMSTATE_FREE)
        {
            u32 overlapStart = memoryInfo.base_addr > rangeLow ? memoryInfo.base_addr : rangeLow;
            u32 overlapEnd = regionEnd < rangeHigh ? regionEnd : rangeHigh;

            if (overlapEnd >= overlapStart && alignedSize <= overlapEnd - overlapStart)
            {
                u32 candidate = downward ?
                    ALIGN_PAGE_DOWN(overlapEnd - alignedSize) :
                    (overlapStart + 0xFFFu) & ~0xFFFu;

                if (candidate >= overlapStart && candidate <= overlapEnd &&
                    alignedSize <= overlapEnd - candidate)
                {
                    Result allocResult = svcControlMemoryUnsafe(
                        outAddress,
                        candidate,
                        alignedSize,
                        MEMOP_ALLOC | MEMOP_REGION_SYSTEM,
                        MEMPERM_READWRITE
                    );

                    if (!SP_FAILED(allocResult))
                        return 0;

                    lastResult = allocResult;
                }
            }
        }

        if (downward)
        {
            if (memoryInfo.base_addr <= rangeLow)
                break;

            scan = memoryInfo.base_addr - 1;
        }
        else
        {
            if (regionEnd <= scan)
                break;

            scan = regionEnd;
        }
    }

    return lastResult;
}

static Result SP_FreePages(u32 address, u32 size)
{
    u32 out;

    return svcControlMemoryUnsafe(
        &out,
        address,
        (size + 0xFFFu) & ~0xFFFu,
        MEMOP_FREE | MEMOP_REGION_SYSTEM,
        0
    );
}

__attribute__((noinline)) static Result SP_Protect(
    Handle process,
    u32 address,
    u32 size,
    u32 permission
)
{
    return svcControlProcessMemory(process, address, 0, size, MEMOP_PROT, permission);
}

static bool SP_PluginEntryEarlier(
    const SysPlugin *current,
    const char *name,
    u32 priority,
    u32 fileOffset
)
{
    s32 comparison;

    if (priority != current->priority)
        return priority < current->priority;

    comparison = SP_StringCompare(name, current->name);

    if (comparison != 0)
        return comparison < 0;

    return fileOffset < current->fileOffset;
}

static void SP_AddPluginEntry(
    SysPlugin *plugins,
    u32 *pluginCount,
    const char *name,
    u32 priority,
    u32 fileOffset,
    const SysPluginHeader *header
)
{
    u32 pos;

    for (pos = 0; pos < *pluginCount; pos++)
    {
        if (plugins[pos].plgid != header->plgid)
            continue;

        if (!SP_PluginEntryEarlier(&plugins[pos], name, priority, fileOffset))
            return;

        break;
    }

    if (pos == *pluginCount)
    {
        if (*pluginCount < SYSPLUGIN_MAX_PLUGINS)
            pos = (*pluginCount)++;
        else
        {
            pos = SYSPLUGIN_MAX_PLUGINS - 1;

            if (!SP_PluginEntryEarlier(&plugins[pos], name, priority, fileOffset))
                return;
        }
    }

    while (pos > 0 && SP_PluginEntryEarlier(&plugins[pos - 1], name, priority, fileOffset))
    {
        SP_CopyString(plugins[pos].name, plugins[pos - 1].name);
        plugins[pos].priority = plugins[pos - 1].priority;
        plugins[pos].fileOffset = plugins[pos - 1].fileOffset;
        plugins[pos].plgid = plugins[pos - 1].plgid;
        plugins[pos].allocatedAddr = plugins[pos - 1].allocatedAddr;
        plugins[pos].totalSize = plugins[pos - 1].totalSize;
        plugins[pos].codeSize = plugins[pos - 1].codeSize;
        plugins[pos].mainAddr = plugins[pos - 1].mainAddr;
        pos--;
    }

    SP_CopyString(plugins[pos].name, name);
    plugins[pos].priority = priority;
    plugins[pos].fileOffset = fileOffset;
    plugins[pos].plgid = header->plgid;
    plugins[pos].allocatedAddr = header->abiLo;
    plugins[pos].totalSize = header->abiHi;
    plugins[pos].codeSize = header->expectedEnvLo;
    plugins[pos].mainAddr = header->expectedEnvHi;
}

static bool SP_ScanPlugins(
    const SysPluginHost *host,
    SysPlgArchive archive,
    u32 pluginMagic,
    SysPlugin *plugins,
    u32 *pluginCount
)
{
    char directoryPath[14];
    Handle directory;

    SP_CopyString(directoryPath, SP_PLUGINS_DIR);

    if (SP_FAILED(host->FSUSER_OpenDirectory(
        &directory,
        archive,
        SP_MakeAsciiPath(directoryPath)
    )))
    {
        return false;
    }

    *pluginCount = 0;

    while (1)
    {
        SysPlgDirectoryEntry entry;
        u32 entriesRead = 0;
        char name[SYSPLUGIN_NAME_SIZE];
        u32 length = 0;
        char path[272];
        Handle file;

        if (SP_FAILED(host->FSDIR_Read(directory, &entriesRead, 1, &entry)) || !entriesRead)
            break;

        while (length + 1 < sizeof(name) && entry.name[length])
        {
            name[length] = (char)entry.name[length];
            length++;
        }
        name[length] = 0;

        if (length < 7 ||
            name[length - 4] != '.' ||
            name[length - 3] != '3' ||
            name[length - 2] != 'n' ||
            name[length - 1] != 'x')
        {
            continue;
        }

        {
            char *extension = &name[length - 4];
            char *priorityDot = extension - 1;
            u32 priority = 0;
            bool valid = true;

            while (priorityDot > name && *priorityDot != '.')
                priorityDot--;

            if (*priorityDot != '.' || priorityDot + 1 == extension)
                continue;

            for (char *character = priorityDot + 1; character < extension; character++)
            {
                u32 digit;

                if (*character < '0' || *character > '9')
                {
                    valid = false;
                    break;
                }

                digit = (u32)(*character - '0');

                if (priority > (0xFFFFFFFFu - digit) / 10u)
                {
                    valid = false;
                    break;
                }

                priority = priority * 10u + digit;
            }

            if (!valid)
                continue;

            SP_MakePluginPath(name, path);

            if (SP_FAILED(host->FSUSER_OpenFile(
                &file,
                archive,
                SP_MakeAsciiPath(path),
                FS_OPEN_READ,
                0
            )))
            {
                continue;
            }

            {
                u32 fileOffset = 0;

                while (1)
                {
                    SysPluginHeader header;
                    u32 nextOffset;

                    if (!SP_ReadHeader(
                        host,
                        file,
                        fileOffset,
                        &header,
                        &nextOffset
                    ))
                    {
                        break;
                    }

                    if (header.magic != ROSALINA_PLUGIN_MAGIC &&
                        header.magic != LOADER_PLUGIN_MAGIC)
                    {
                        break;
                    }

                    if (header.magic == pluginMagic)
                    {
                        SP_AddPluginEntry(
                            plugins,
                            pluginCount,
                            name,
                            priority,
                            fileOffset,
                            &header
                        );
                    }

                    fileOffset = nextOffset;
                }
            }

            host->FSFILE_Close(file);
        }
    }

    host->FSDIR_Close(directory);

    return *pluginCount != 0;
}

static s32 SP_FindPluginById(SysPlugin *plugins, u32 pluginCount, u32 pluginId)
{
    for (u32 i = 0; i < pluginCount; i++)
    {
        if (plugins[i].plgid == pluginId)
            return (s32)i;
    }

    return -1;
}

static void SP_DiscardPlugin(SysPlugin *plugin, Handle selfProcess)
{
    if (plugin->codeProtected)
    {
        (void)SP_Protect(selfProcess, plugin->allocatedAddr, plugin->codeSize, MEMPERM_READWRITE);
    }

    (void)SP_FreePages(plugin->allocatedAddr, plugin->totalSize);
    plugin->loaded = 0;
}

static bool SP_CallPluginMain(u32 mainAddress)
{
    u32 returned = 0;

    __asm__ volatile(
        "push {r0-r12, lr}\n"
        "mrs r12, cpsr\n"
        "push {r12}\n"
        "sub sp, sp, #12\n"
        "str %[returnPointer], [sp]\n"
        "mov r12, %[address]\n"
        "blx r12\n"
        "ldr r12, [sp]\n"
        "str r0, [r12]\n"
        "add sp, sp, #12\n"
        "pop {r12}\n"
        "msr cpsr_f, r12\n"
        "pop {r0-r12, lr}\n"
        :
        : [address] "r"(mainAddress), [returnPointer] "r"(&returned)
        : "r12", "memory"
    );

    return returned != 0;
}

static bool SP_RunFixer(
    const SysPluginHost *host,
    Handle file,
    const char *threeNrPath,
    u32 pluginMagic,
    u32 rangeLow,
    u32 rangeHigh,
    bool downward,
    Handle selfProcess,
    SysPlugin *plugins,
    bool ignorePairMismatch
)
{
    u32 pair[2];
    const u32 codeSize = (SYSPLUGIN_3NR_CODE_SIZE + 0xFFFu) & ~0xFFFu;
    const u32 dataSize = (SYSPLUGIN_3NR_DATA_SIZE + 0xFFFu) & ~0xFFFu;
    const u32 totalSize = codeSize + dataSize;
    u32 address = 0;
    bool result = false;
    bool loaded;
    const bool pairMatches = SP_ReadExact(host, file, SYSPLUGIN_3NR_PAIR_OFF, pair, sizeof(pair)) &&
                             pair[0] == SYSPLUGIN_3NR_BUILD_PAIR_LO &&
                             pair[1] == SYSPLUGIN_3NR_BUILD_PAIR_HI;

    if (!pairMatches || SP_FAILED(SP_AllocPages(totalSize, rangeLow, rangeHigh, downward, &address)))
    {
        host->FSFILE_Close(file);
        return !pairMatches && ignorePairMismatch;
    }

    loaded = SP_ReadExact(host, file, SYSPLUGIN_3NR_CODE_OFFSET, (void *)address, SYSPLUGIN_3NR_CODE_SIZE);
    loaded = !SP_FAILED(host->FSFILE_Close(file)) && loaded;

    if (loaded)
    {
        svcFlushEntireDataCache();

        if (!SP_FAILED(SP_Protect(selfProcess, address, codeSize, MEMPERM_READEXECUTE)))
        {
            svcInvalidateEntireInstructionCache();
            result = ((bool (*)(const SysPluginHost *, u32, SysPlugin *, const char *))address)(
                host, pluginMagic, plugins, threeNrPath);
            (void)SP_Protect(selfProcess, address, codeSize, MEMPERM_READWRITE);
        }
    }

    (void)SP_FreePages(address, totalSize);
    return result;
}

static void SP_PropagateFailures(
    SysPlugin *plugins,
    u32 pluginCount,
    Handle selfProcess
)
{
    u32 failedMask = 0;
    bool changed;

    for (u32 i = 0; i < pluginCount; i++)
    {
        if (!plugins[i].loaded)
            failedMask |= 1u << i;
    }

    do
    {
        changed = false;

        for (u32 i = 0; i < pluginCount; i++)
        {
            if (plugins[i].loaded == SP_PLUGIN_MARKED ||
                (plugins[i].loaded && (plugins[i].depsMask & failedMask)))
            {
                SP_DiscardPlugin(&plugins[i], selfProcess);
                failedMask |= 1u << i;
                changed = true;
            }
        }
    }
    while (changed);
}

static bool SP_RelocatePlugin(
    const SysPluginHost *host,
    SysPlgArchive archive,
    SysPlugin *plugins,
    u32 pluginCount,
    u32 pluginIndex,
    bool optionalPass
)
{
    SysPlugin *plugin = &plugins[pluginIndex];
    char path[272];
    Handle file;
    u32 ptrDataStart = plugin->fileOffset + PLUGIN_HEADER_SIZE;
    u32 totalRead = 0;
    bool failed = false;

    SP_MakePluginPath(plugin->name, path);
    if (SP_FAILED(host->FSUSER_OpenFile(&file, archive, SP_MakeAsciiPath(path), FS_OPEN_READ, 0)))
        return false;

    while (totalRead < plugin->pluginPtrSize && !failed)
    {
        u32 group[2];
        s32 dependencyIndex;
        SysPlugin *dependency = 0;
        bool selfReference = false;
        bool managedDependency = false;

        if (!SP_ReadRelocPair(host, file, ptrDataStart, plugin->pluginPtrSize, &totalRead, group))
        {
            failed = true;
            break;
        }

        dependencyIndex = SP_FindPluginById(plugins, pluginCount, group[0]);
        if (dependencyIndex >= 0)
        {
            dependency = &plugins[(u32)dependencyIndex];
            selfReference = dependency == plugin;
            managedDependency = !selfReference && (u32)dependencyIndex < pluginIndex;
            if (!optionalPass && managedDependency)
            {
                plugin->depsMask |= 1u << (u32)dependencyIndex;
                if (!dependency->loaded)
                {
                    failed = true;
                    break;
                }
            }
        }

        if (!optionalPass && !selfReference && !managedDependency)
            plugin->reserved[0] = 1;

        for (u32 i = 0; i < group[1]; i++)
        {
            u32 pair[2];
            bool patch = optionalPass ? (!selfReference && !managedDependency) : (selfReference || managedDependency);

            if (!SP_ReadRelocPair(host, file, ptrDataStart, plugin->pluginPtrSize, &totalRead, pair) ||
                pair[0] > plugin->totalSize - 4)
            {
                failed = true;
                break;
            }

            if (patch)
            {
                if (dependency && dependency->loaded)
                {
                    if (pair[1] >= dependency->totalSize)
                    {
                        failed = true;
                        break;
                    }
                    *(u32 *)(plugin->allocatedAddr + pair[0]) = dependency->allocatedAddr + pair[1];
                }
                else
                    *(u32 *)(plugin->allocatedAddr + pair[0]) = 0;
            }
        }
    }

    host->FSFILE_Close(file);
    return !failed && totalRead == plugin->pluginPtrSize;
}

Result SysPluginLoader_Main(
    const SysPluginHost *host,
    u32 pluginMagic,
    u32 rangeLow,
    u32 rangeHigh,
    bool unlock
)
{
    const bool downward = pluginMagic == LOADER_PLUGIN_MAGIC;
    const u32 workspaceSize = sizeof(SysPlugin) * SYSPLUGIN_MAX_PLUGINS;
    SysPlgArchive archive = 0;
    SysPlgPath emptyPath;
    char emptyPathData[1];
    SysPlugin *plugins;
    u32 workspaceAddress = 0;
    u32 pluginCount = 0;
    Handle selfProcess = 0;

    if (!unlock)
    {
        while (spLocked)
            svcSleepThread(1000000LL);
    }

    {
        Result allocResult = SP_AllocPages(
            workspaceSize,
            rangeLow,
            rangeHigh,
            downward,
            &workspaceAddress
        );

        if (SP_FAILED(allocResult))
            return SP_Unlock(allocResult);
    }

    plugins = (SysPlugin *)workspaceAddress;

    emptyPathData[0] = 0;
    emptyPath.type = SYSPLG_PATH_EMPTY;
    emptyPath.size = 1;
    emptyPath.data = emptyPathData;

    if (SP_FAILED(host->FSUSER_OpenArchive(&archive, ARCHIVE_SDMC, emptyPath)))
    {
        Result freeResult = SP_FreePages(workspaceAddress, workspaceSize);
        return SP_Unlock(SP_FAILED(freeResult) ? freeResult : 0);
    }

    if (!SP_ScanPlugins(host, archive, pluginMagic, plugins, &pluginCount))
    {
        host->FSUSER_CloseArchive(archive);
        return SP_Unlock(SP_FreePages(workspaceAddress, workspaceSize));
    }

    {
        u32 envLo = SYSPLUGIN_3NR_BUILD_PAIR_LO;
        u32 envHi = SYSPLUGIN_3NR_BUILD_PAIR_HI;
        const u32 expectedLo = plugins[0].codeSize & ~1u;
        const u32 expectedHi = plugins[0].mainAddr;
        const bool needsFix = expectedLo == 0 && expectedHi == 0;
        u32 mismatch = 0;
        bool environmentMismatch;

        for (u32 i = 0; i < pluginCount; i++)
        {
            envLo ^= plugins[i].allocatedAddr;
            envHi ^= plugins[i].totalSize;
            mismatch |= (plugins[i].codeSize & ~1u) ^ expectedLo;
            mismatch |= plugins[i].mainAddr ^ expectedHi;
        }

        environmentMismatch = needsFix || mismatch || expectedLo != envLo || expectedHi != envHi;

        {
            u32 processId = 0;

            if (SP_FAILED(svcGetProcessId(&processId, CUR_PROCESS_HANDLE)) ||
                SP_FAILED(svcOpenProcess(&selfProcess, processId)))
            {
                host->FSUSER_CloseArchive(archive);
                return SP_Unlock(SP_FreePages(workspaceAddress, workspaceSize));
            }
        }

        {
            /* Keep this local K11 path buffer: do not pass direct SP_THREE_NR_* string-literal pointers to mapped 3nr code. */
            char threeNrPath[19];
            Handle file = 0;
            Result openResult;
            bool keyed = false;

            SP_CopyString(threeNrPath, SP_THREE_NR_PATH);

        retryThreeNr:
            openResult = host->FSUSER_OpenFile(
                &file,
                archive,
                SP_MakeAsciiPath(threeNrPath),
                FS_OPEN_READ,
                0
            );

            if ((!SP_FAILED(openResult) || environmentMismatch) &&
                (SP_FAILED(openResult) ||
                 !SP_RunFixer(host, file, threeNrPath, pluginMagic, rangeLow, rangeHigh, downward, selfProcess, plugins, !environmentMismatch)))
            {
                if (!keyed)
                {
                    SP_CopyString(threeNrPath, SP_THREE_NR_KEYED_PATH);
                    keyed = true;
                    environmentMismatch = true;
                    goto retryThreeNr;
                }

                if (SP_FAILED(openResult) && file)
                    host->FSFILE_Close(file);
                svcCloseHandle(selfProcess);
                host->FSUSER_CloseArchive(archive);
                return SP_Unlock(SP_FreePages(workspaceAddress, workspaceSize));
            }
        }
    }

    (void)SP_Unlock(0);

    // load every image as RW
    for (u32 pluginIndex = 0; pluginIndex < pluginCount; pluginIndex++)
    {
        SysPlugin *plugin = &plugins[pluginIndex];
        SysPluginHeader header;
        if (!plugin->plgid || (plugin->codeSize & 1u))
            continue;
        char path[272];
        Handle file;
        u32 payloadOffset;
        u32 nextOffset;
        u32 codeSize;
        u32 dataAndBssSize;
        u32 dataSize;
        u32 totalSize;
        u32 codeAddress = 0;
        u32 dataAddress;

        SP_MakePluginPath(plugin->name, path);

        if (SP_FAILED(host->FSUSER_OpenFile(
            &file,
            archive,
            SP_MakeAsciiPath(path),
            FS_OPEN_READ,
            0
        )))
        {
            continue;
        }

        if (!SP_ReadHeader(
                host,
                file,
                plugin->fileOffset,
                &header,
                &nextOffset
            ) ||
            header.magic != pluginMagic ||
            header.plgid != plugin->plgid ||
            !header.pluginCodeSize ||
            !SP_AlignPageChecked(header.pluginCodeSize, &codeSize) ||
            !SP_AddChecked(header.pluginDataSize, header.pluginBssSize, &dataAndBssSize) ||
            !SP_AlignPageChecked(dataAndBssSize, &dataSize) ||
            !SP_AddChecked(codeSize, dataSize, &totalSize))
        {
            host->FSFILE_Close(file);
            continue;
        }

        payloadOffset = plugin->fileOffset + PLUGIN_HEADER_SIZE;

        {
            Result allocResult = SP_AllocPages(
                totalSize,
                rangeLow,
                rangeHigh,
                downward,
                &codeAddress
            );

            if (SP_FAILED(allocResult))
            {
                host->FSFILE_Close(file);
                continue;
            }
        }

        dataAddress = codeAddress + codeSize;

        plugin->allocatedAddr = codeAddress;
        plugin->totalSize = totalSize;
        plugin->codeSize = codeSize;
        plugin->pluginPtrSize = header.pluginPtrSize;
        plugin->mainAddr = codeAddress;

        if (!SP_ReadExact(
                host,
                file,
                payloadOffset + header.pluginPtrSize,
                (void *)codeAddress,
                header.pluginCodeSize
            ) ||
            !SP_ReadExact(
                host,
                file,
                payloadOffset + header.pluginPtrSize + header.pluginCodeSize,
                (void *)dataAddress,
                header.pluginDataSize
            ))
        {
            host->FSFILE_Close(file);
            SP_DiscardPlugin(plugin, 0);
            continue;
        }


        plugin->loaded = 1;
        host->FSFILE_Close(file);
    }

    // Resolve managed refs first, prune failures, then fill author-managed refs from final survivors.
    for (u32 pass = 0; pass < 2; pass++)
    {
        for (u32 pluginIndex = 0; pluginIndex < pluginCount; pluginIndex++)
        {
            SysPlugin *plugin = &plugins[pluginIndex];
            if (!plugin->loaded || !plugin->pluginPtrSize || (pass && !plugin->reserved[0]))
                continue;
            if (pass)
                plugin->reserved[0] = 0;
            if (!SP_RelocatePlugin(host, archive, plugins, pluginCount, pluginIndex, pass != 0))
                SP_DiscardPlugin(plugin, 0);
        }
        SP_PropagateFailures(plugins, pluginCount, 0);
    }

    {
        bool haveExecutablePlugin = false;

        svcFlushEntireDataCache();

        for (u32 pluginIndex = 0; pluginIndex < pluginCount; pluginIndex++)
        {
            SysPlugin *plugin = &plugins[pluginIndex];

            if (!plugin->loaded)
                continue;

            {
                Result protectResult = SP_Protect(
                    selfProcess, plugin->allocatedAddr, plugin->codeSize, MEMPERM_READEXECUTE);

                if (SP_FAILED(protectResult))
                {
                    SP_DiscardPlugin(plugin, selfProcess);
                }
                else
                {
                    plugin->codeProtected = 1;
                    haveExecutablePlugin = true;
                }
            }
        }

        if (haveExecutablePlugin)
            svcInvalidateEntireInstructionCache();
    }

    // protection failures count before any mains run
    SP_PropagateFailures(plugins, pluginCount, selfProcess);

    // lowest number runs first
    {
        u32 failedMainMask = 0;

        for (u32 pluginIndex = 0; pluginIndex < pluginCount; pluginIndex++)
        {
            SysPlugin *plugin = &plugins[pluginIndex];

            if (!plugin->loaded)
            {
                failedMainMask |= 1u << pluginIndex;
                continue;
            }

            if (plugin->depsMask & failedMainMask)
            {
                failedMainMask |= 1u << pluginIndex;
                plugin->loaded = SP_PLUGIN_MARKED;
                continue;
            }

            if (!SP_CallPluginMain(plugin->mainAddr))
            {
                failedMainMask |= 1u << pluginIndex;
                plugin->loaded = SP_PLUGIN_MARKED;
            }
        }

        SP_PropagateFailures(plugins, pluginCount, selfProcess);
    }

    svcCloseHandle(selfProcess);

    host->FSUSER_CloseArchive(archive);
    (void)SP_FreePages(workspaceAddress, workspaceSize);

    return 0;
}

u32 SysPluginLoaderGetEntry(void)
{
    return convertVAToPA(SysPluginLoader_Main, false) | 0x80000000u;
}
