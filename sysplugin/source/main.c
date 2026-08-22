#include "sysplugin.h"

#define ARCHIVE_SDMC 0x00000009u
#define FS_OPEN_READ 1u
#define FS_OPEN_WRITE 2u
#define FS_WRITE_FLUSH 1u
#define SYSPLG_PATH_EMPTY 1u
#define SYSPLG_PATH_ASCII 3u
#define SYSPLUGIN_EXPECTED_ENV_OFFSET 0x24u

static const char PLUGINS_DIR[] = "/luma/plugins";
static const char THREE_NR_PATH[] = "/boot.3nr";
static const char THREE_NR_KEYED_PATH[] = "/boot.00000000.3nr";

typedef struct
{
    uint32_t pluginMagic;
    uint32_t environmentLo;
    uint32_t environmentHi;
    SysPluginEntry *plugins;
    uint32_t pluginCount;
    uint32_t activeMask;
} FixerModule;

typedef struct
{
    const SysPluginHost *host;
    SysPlgArchive archive;
    Handle threeNrFile;
    FixerModule modules[2];
} FixerContext;

static SysPluginEntry g_otherPlugins[SYSPLUGIN_MAX_PLUGINS];

static bool Failed(Result result)
{
    return result < 0;
}

static uint32_t StringLength(const char *string)
{
    uint32_t length = 0;
    while (string[length])
        length++;
    return length;
}

static int32_t StringCompare(const char *a, const char *b)
{
    while (*a && *a == *b)
    {
        a++;
        b++;
    }
    return (int32_t)(uint8_t)*a - (int32_t)(uint8_t)*b;
}

static void CopyString(char *dst, const char *src)
{
    uint32_t i = 0;
    while (i + 1 < SYSPLUGIN_NAME_SIZE && src[i])
    {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static void MakeKeyedPath(char path[19], uint32_t key)
{
    CopyString(path, THREE_NR_KEYED_PATH);

    for (uint32_t i = 0; i < 8; i++)
    {
        uint32_t digit = key & 0xFu;
        path[13 - i] = digit < 10 ? (char)('0' + digit) : (char)('A' + digit - 10);
        key >>= 4;
    }
}

static SysPlgPath MakeAsciiPath(const char *string)
{
    SysPlgPath path;
    path.type = SYSPLG_PATH_ASCII;
    path.size = StringLength(string) + 1;
    path.data = string;
    return path;
}

static bool RenameThreeNr(FixerContext *context, const char *sourcePath, uint32_t key)
{
    char targetPath[19];
    Handle existing = 0;

    MakeKeyedPath(targetPath, key);

    if (!Failed(context->host->FSUSER_OpenFile(
            &existing,
            context->archive,
            MakeAsciiPath(targetPath),
            FS_OPEN_READ,
            0
        )))
    {
        if (Failed(context->host->FSFILE_Close(existing)) ||
            Failed(context->host->FSUSER_DeleteFile(context->archive, MakeAsciiPath(targetPath))))
        {
            return false;
        }
    }

    return !Failed(context->host->FSUSER_RenameFile(
        context->archive,
        MakeAsciiPath(sourcePath),
        context->archive,
        MakeAsciiPath(targetPath)
    ));
}

static void MakePluginPath(const char *name, char path[272])
{
    uint32_t i = 0;
    uint32_t j = 0;
    while (PLUGINS_DIR[i])
        path[j++] = PLUGINS_DIR[i++];
    path[j++] = '/';
    i = 0;
    while (name[i])
        path[j++] = name[i++];
    path[j] = 0;
}

static bool ReadExact(const SysPluginHost *host, Handle file, uint64_t offset, void *buffer, uint32_t size)
{
    uint32_t read = 0;
    return !size || (!Failed(host->FSFILE_Read(file, &read, offset, buffer, size)) && read == size);
}

static bool WriteExact(const SysPluginHost *host, Handle file, uint64_t offset, const void *buffer, uint32_t size, uint32_t flags)
{
    uint32_t written = 0;
    return !size || (!Failed(host->FSFILE_Write(file, &written, offset, buffer, size, flags)) && written == size);
}

static bool Add32(uint32_t a, uint32_t b, uint32_t *out)
{
    uint32_t value = a + b;
    if (value < a)
        return false;
    *out = value;
    return true;
}

static bool Mul32(uint32_t a, uint32_t b, uint32_t *out)
{
    if (a && b > 0xFFFFFFFFu / a)
        return false;
    *out = a * b;
    return true;
}

static bool Read3nxHeader(const SysPluginHost *host, Handle file, uint32_t fileOffset, SysPlugin3nxHeader *header, uint32_t *nextOffset)
{
    uint32_t end;

    if (!ReadExact(host, file, fileOffset, header, sizeof(*header)) ||
        !Add32(fileOffset, SYSPLUGIN_3NX_HEADER_SIZE, &end) ||
        !Add32(end, header->fastRelocSize, &end) ||
        !Add32(end, header->codeSize, &end) ||
        !Add32(end, header->dataSize, &end) ||
        !Add32(end, header->repairSize, &end) ||
        end > 0xFFFFFFF0u)
        return false;

    *nextOffset = (end + 0xFu) & ~0xFu;
    return *nextOffset > fileOffset;
}

static bool Earlier(const SysPluginEntry *current, const char *name, uint32_t priority, uint32_t fileOffset)
{
    int32_t comparison;
    if (priority != current->priority)
        return priority < current->priority;
    comparison = StringCompare(name, current->name);
    if (comparison != 0)
        return comparison < 0;
    return fileOffset < current->fileOffset;
}

static void CopySelected(SysPluginEntry *dst, const SysPluginEntry *src)
{
    CopyString(dst->name, src->name);
    dst->priority = src->priority;
    dst->fileOffset = src->fileOffset;
    dst->plgid = src->plgid;
    dst->ownAbiLo = src->ownAbiLo;
    dst->ownAbiHi = src->ownAbiHi;
    dst->expectedEnvLo = src->expectedEnvLo;
    dst->expectedEnvHi = src->expectedEnvHi;
}

static void AddPlugin(SysPluginEntry *plugins, uint32_t *count, const char *name, uint32_t priority, uint32_t fileOffset, const SysPlugin3nxHeader *header)
{
    uint32_t pos;

    for (pos = 0; pos < *count; pos++)
    {
        if (plugins[pos].plgid != header->plgid)
            continue;
        if (!Earlier(&plugins[pos], name, priority, fileOffset))
            return;
        break;
    }

    if (pos == *count)
    {
        if (*count < SYSPLUGIN_MAX_PLUGINS)
            pos = (*count)++;
        else
        {
            pos = SYSPLUGIN_MAX_PLUGINS - 1;
            if (!Earlier(&plugins[pos], name, priority, fileOffset))
                return;
        }
    }

    while (pos > 0 && Earlier(&plugins[pos - 1], name, priority, fileOffset))
    {
        CopySelected(&plugins[pos], &plugins[pos - 1]);
        pos--;
    }

    CopyString(plugins[pos].name, name);
    plugins[pos].priority = priority;
    plugins[pos].fileOffset = fileOffset;
    plugins[pos].plgid = header->plgid;
    plugins[pos].ownAbiLo = header->ownAbiLo;
    plugins[pos].ownAbiHi = header->ownAbiHi;
    plugins[pos].expectedEnvLo = header->expectedEnvLo;
    plugins[pos].expectedEnvHi = header->expectedEnvHi;
}

static bool ScanPlugins(FixerContext *context, FixerModule *module)
{
    Handle directory = 0;
    uint32_t count = 0;

    if (Failed(context->host->FSUSER_OpenDirectory(&directory, context->archive, MakeAsciiPath(PLUGINS_DIR))))
        return false;

    for (;;)
    {
        SysPlgDirectoryEntry entry;
        uint32_t entriesRead = 0;
        char name[SYSPLUGIN_NAME_SIZE];
        uint32_t length = 0;

        if (Failed(context->host->FSDIR_Read(directory, &entriesRead, 1, &entry)))
        {
            context->host->FSDIR_Close(directory);
            return false;
        }
        if (!entriesRead)
            break;

        while (length + 1 < sizeof(name) && entry.name[length])
        {
            name[length] = (char)entry.name[length];
            length++;
        }
        name[length] = 0;

        if (length < 7 || name[length - 4] != '.' || name[length - 3] != '3' || name[length - 2] != 'n' || name[length - 1] != 'x')
            continue;

        {
            char *extension = &name[length - 4];
            char *priorityDot = extension - 1;
            uint32_t priority = 0;
            bool valid = true;
            char path[272];
            Handle file = 0;

            while (priorityDot > name && *priorityDot != '.')
                priorityDot--;
            if (*priorityDot != '.' || priorityDot + 1 == extension)
                continue;

            for (char *character = priorityDot + 1; character < extension; character++)
            {
                uint32_t digit;
                if (*character < '0' || *character > '9')
                {
                    valid = false;
                    break;
                }
                digit = (uint32_t)(*character - '0');
                if (priority > (0xFFFFFFFFu - digit) / 10u)
                {
                    valid = false;
                    break;
                }
                priority = priority * 10u + digit;
            }
            if (!valid)
                continue;

            MakePluginPath(name, path);
            if (Failed(context->host->FSUSER_OpenFile(&file, context->archive, MakeAsciiPath(path), FS_OPEN_READ, 0)))
                continue;

            for (uint32_t fileOffset = 0; fileOffset < entry.fileSize;)
            {
                SysPlugin3nxHeader header;
                uint32_t nextOffset;
                if (!Read3nxHeader(context->host, file, fileOffset, &header, &nextOffset) ||
                    (header.magic != ROSALINA_PLUGIN_MAGIC && header.magic != LOADER_PLUGIN_MAGIC) ||
                    nextOffset > entry.fileSize)
                    break;
                if (header.magic == module->pluginMagic)
                    AddPlugin(module->plugins, &count, name, priority, fileOffset, &header);
                fileOffset = nextOffset;
            }

            context->host->FSFILE_Close(file);
        }
    }

    context->host->FSDIR_Close(directory);
    module->pluginCount = count;
    module->activeMask = count ? ((1u << count) - 1u) : 0;
    return true;
}

static bool IsActive(const FixerModule *module, uint32_t index)
{
    return (module->activeMask & (1u << index)) != 0;
}

static const SysPluginEntry *FindPlugin(const FixerModule *module, uint32_t id)
{
    for (uint32_t i = 0; i < module->pluginCount; i++)
    {
        if (IsActive(module, i) && module->plugins[i].plgid == id)
            return &module->plugins[i];
    }
    return 0;
}

static bool SameStack(const SysPluginEntry *entry, const char *name)
{
    return StringCompare(entry->name, name) == 0;
}

static FixerModule *FindModuleByMagic(FixerContext *context, uint32_t magic)
{
    for (uint32_t m = 0; m < 2; m++)
    {
        if (context->modules[m].pluginMagic == magic)
            return &context->modules[m];
    }
    return 0;
}

static void DropSelectedStackSuffix(FixerContext *context, const char *name, uint32_t fileOffset)
{
    for (uint32_t m = 0; m < 2; m++)
    {
        FixerModule *module = &context->modules[m];
        for (uint32_t i = 0; i < module->pluginCount; i++)
        {
            if (SameStack(&module->plugins[i], name) && module->plugins[i].fileOffset >= fileOffset)
                module->activeMask &= ~(1u << i);
        }
    }
}

static bool MarkPhysicalStackSuffixFailed(FixerContext *context, const SysPluginEntry *failedEntry)
{
    char path[272];
    Handle file = 0;
    uint32_t fileOffset = failedEntry->fileOffset;
    bool markedAny = false;

    MakePluginPath(failedEntry->name, path);
    if (Failed(context->host->FSUSER_OpenFile(&file, context->archive, MakeAsciiPath(path), FS_OPEN_READ | FS_OPEN_WRITE, 0)))
        return false;

    for (;;)
    {
        SysPlugin3nxHeader header;
        uint32_t nextOffset;
        FixerModule *module;
        uint32_t environment[2];

        if (!Read3nxHeader(context->host, file, fileOffset, &header, &nextOffset))
            break;

        module = FindModuleByMagic(context, header.magic);
        if (!module)
            break;

        environment[0] = module->environmentLo | 1u;
        environment[1] = module->environmentHi;
        if (!WriteExact(context->host, file, fileOffset + SYSPLUGIN_EXPECTED_ENV_OFFSET, environment, 8, FS_WRITE_FLUSH))
        {
            context->host->FSFILE_Close(file);
            return false;
        }

        markedAny = true;
        fileOffset = nextOffset;
    }

    context->host->FSFILE_Close(file);
    DropSelectedStackSuffix(context, failedEntry->name, failedEntry->fileOffset);
    return markedAny;
}

static bool GetRepairBounds(uint32_t entryOffset, const SysPlugin3nxHeader *header, uint32_t *repairStart, uint32_t *repairEnd)
{
    uint32_t value;
    if (!Add32(entryOffset, SYSPLUGIN_3NX_HEADER_SIZE, &value) ||
        !Add32(value, header->fastRelocSize, &value) ||
        !Add32(value, header->codeSize, &value) ||
        !Add32(value, header->dataSize, &value))
        return false;
    *repairStart = value;
    return Add32(value, header->repairSize, repairEnd);
}

static bool FindRecord(const SysPluginHost *host, Handle file, uint32_t tableOffset, uint32_t count, uint64_t key, SysPluginExportRecord *record)
{
    uint32_t low = 0;
    uint32_t high = count;

    while (low < high)
    {
        uint32_t mid = low + ((high - low) >> 1);
        if (!ReadExact(host, file, tableOffset + mid * sizeof(*record), record, sizeof(*record)))
            return false;
        if (record->key < key)
            low = mid + 1;
        else
            high = mid;
    }

    if (low >= count || !ReadExact(host, file, tableOffset + low * sizeof(*record), record, sizeof(*record)))
        return false;
    return record->key == key;
}

static bool FindHostRecord(const FixerContext *context, const FixerModule *module, uint64_t key, SysPluginExportRecord *record)
{
    SysPlugin3nrHeader header;
    uint32_t tableOffset;
    uint32_t loaderBytes;
    uint32_t count;

    if (!ReadExact(context->host, context->threeNrFile, 0, &header, sizeof(header)) ||
        header.magic != SYSPLUGIN_3NR_MAGIC ||
        !Mul32(header.loaderCount, sizeof(SysPluginExportRecord), &loaderBytes))
        return false;

    tableOffset = header.codeOffset + header.codeSize;
    if (module->pluginMagic == LOADER_PLUGIN_MAGIC)
        count = header.loaderCount;
    else if (module->pluginMagic == ROSALINA_PLUGIN_MAGIC)
    {
        if (!Add32(tableOffset, loaderBytes, &tableOffset))
            return false;
        count = header.rosalinaCount;
    }
    else
        return false;

    return FindRecord(context->host, context->threeNrFile, tableOffset, count, key, record);
}

static bool FindPluginRecord(const FixerContext *context, const FixerModule *module, const SysPluginEntry *provider, uint64_t key, SysPluginExportRecord *record)
{
    char path[272];
    Handle file = 0;
    SysPlugin3nxHeader header;
    uint32_t repairStart;
    uint32_t repairEnd;
    uint32_t exportCount;
    uint32_t nextOffset;
    bool result = false;

    MakePluginPath(provider->name, path);
    if (Failed(context->host->FSUSER_OpenFile(&file, context->archive, MakeAsciiPath(path), FS_OPEN_READ, 0)))
        return false;

    if (Read3nxHeader(context->host, file, provider->fileOffset, &header, &nextOffset) &&
        header.magic == module->pluginMagic && header.plgid == provider->plgid &&
        GetRepairBounds(provider->fileOffset, &header, &repairStart, &repairEnd) &&
        header.repairSize >= 4 && ReadExact(context->host, file, repairStart, &exportCount, 4))
    {
        uint32_t bytes;
        if (Mul32(exportCount, sizeof(*record), &bytes) && bytes <= repairEnd - repairStart - 4)
            result = FindRecord(context->host, file, repairStart + 4, exportCount, key, record);
    }

    context->host->FSFILE_Close(file);
    return result;
}

static bool RepairEntry(const FixerContext *context, const FixerModule *module, const SysPluginEntry *entry)
{
    char path[272];
    Handle file = 0;
    SysPlugin3nxHeader header;
    uint32_t repairStart;
    uint32_t repairEnd;
    uint32_t exportCount;
    uint32_t cursor;
    uint32_t nextOffset;
    bool ok = false;

    MakePluginPath(entry->name, path);
    if (Failed(context->host->FSUSER_OpenFile(&file, context->archive, MakeAsciiPath(path), FS_OPEN_READ | FS_OPEN_WRITE, 0)))
        return false;

    if (!Read3nxHeader(context->host, file, entry->fileOffset, &header, &nextOffset) ||
        header.magic != module->pluginMagic || header.plgid != entry->plgid ||
        !GetRepairBounds(entry->fileOffset, &header, &repairStart, &repairEnd) ||
        header.repairSize < 4 || !ReadExact(context->host, file, repairStart, &exportCount, 4))
        goto done;

    {
        uint32_t exportBytes;
        if (!Mul32(exportCount, sizeof(SysPluginExportRecord), &exportBytes) ||
            !Add32(repairStart + 4, exportBytes, &cursor) || cursor > repairEnd)
            goto done;
    }

    while (cursor < repairEnd)
    {
        SysPluginRepairGroup group;
        uint32_t groupBytes;
        const SysPluginEntry *provider = 0;

        if (repairEnd - cursor < sizeof(group) || !ReadExact(context->host, file, cursor, &group, sizeof(group)))
            goto done;
        cursor += sizeof(group);
        if (!Mul32(group.count, sizeof(SysPluginRepairRecord), &groupBytes) || groupBytes > repairEnd - cursor)
            goto done;

        if (group.providerId != SYSPLUGIN_HOST_PROVIDER)
        {
            provider = FindPlugin(module, group.providerId);
            if (!provider)
            {
                cursor += groupBytes;
                continue;
            }
            if (provider == entry)
                goto done;
        }

        for (uint32_t i = 0; i < group.count; i++)
        {
            SysPluginRepairRecord repair;
            SysPluginExportRecord record;
            uint32_t value;
            uint32_t cacheFileOffset;
            uint32_t runtimeEnd;

            if (!ReadExact(context->host, file, cursor, &repair, sizeof(repair)))
                goto done;
            cursor += sizeof(repair);

            if (!Add32(SYSPLUGIN_3NX_HEADER_SIZE, header.fastRelocSize, &runtimeEnd) ||
                !Add32(runtimeEnd, header.codeSize, &runtimeEnd) ||
                !Add32(runtimeEnd, header.dataSize, &runtimeEnd) ||
                (repair.cacheOffset & 3u) || repair.cacheOffset < SYSPLUGIN_3NX_HEADER_SIZE || repair.cacheOffset > runtimeEnd - 4)
                goto done;

            if (group.providerId == SYSPLUGIN_HOST_PROVIDER)
            {
                if (!FindHostRecord(context, module, repair.key, &record))
                    goto done;
            }
            else if (!FindPluginRecord(context, module, provider, repair.key, &record))
                goto done;

            if (!Add32(record.value, repair.addend, &value) ||
                !Add32(entry->fileOffset, repair.cacheOffset, &cacheFileOffset) ||
                !WriteExact(context->host, file, cacheFileOffset, &value, 4, 0))
                goto done;
        }
    }

    ok = true;
done:
    context->host->FSFILE_Close(file);
    return ok;
}

static bool WriteExpected(FixerContext *context, SysPluginEntry *entry, uint32_t lo, uint32_t hi)
{
    char path[272];
    Handle file = 0;
    uint32_t environment[2] = { lo, hi };
    bool ok;

    MakePluginPath(entry->name, path);
    if (Failed(context->host->FSUSER_OpenFile(&file, context->archive, MakeAsciiPath(path), FS_OPEN_READ | FS_OPEN_WRITE, 0)))
        return false;
    ok = WriteExact(context->host, file, entry->fileOffset + SYSPLUGIN_EXPECTED_ENV_OFFSET, environment, 8, FS_WRITE_FLUSH);
    context->host->FSFILE_Close(file);
    if (ok)
    {
        entry->expectedEnvLo = lo;
        entry->expectedEnvHi = hi;
    }
    return ok;
}

static void ComputeEnvironment(FixerModule *module, uint64_t buildPairId)
{
    module->environmentLo = (uint32_t)buildPairId;
    module->environmentHi = (uint32_t)(buildPairId >> 32);
    for (uint32_t i = 0; i < module->pluginCount; i++)
    {
        module->environmentLo ^= module->plugins[i].ownAbiLo;
        module->environmentHi ^= module->plugins[i].ownAbiHi;
    }
}

static bool InvalidateAll(FixerContext *context)
{
    for (uint32_t m = 0; m < 2; m++)
    {
        FixerModule *module = &context->modules[m];
        for (uint32_t i = 0; i < module->pluginCount; i++)
        {
            if (!WriteExpected(context, &module->plugins[i], 0, 0))
                return false;
        }
    }
    return true;
}

static bool RepairModule(FixerContext *context, FixerModule *module)
{
    for (uint32_t i = 0; i < module->pluginCount; i++)
    {
        if (IsActive(module, i) && !RepairEntry(context, module, &module->plugins[i]) &&
            !MarkPhysicalStackSuffixFailed(context, &module->plugins[i]))
            return false;
    }
    return true;
}

static bool CommitModule(FixerContext *context, FixerModule *module)
{
    for (uint32_t i = 0; i < module->pluginCount; i++)
    {
        uint32_t lo = module->environmentLo | (IsActive(module, i) ? 0u : 1u);
        if (!WriteExpected(context, &module->plugins[i], lo, module->environmentHi))
            return false;
    }
    return true;
}

__attribute__((section(".text.Main"), used, noinline))
bool Main(const SysPluginHost *host, uint32_t pluginMagic, SysPluginEntry *plugins, const char *threeNrPath)
{
    FixerContext context;
    SysPlgPath emptyPath;
    char empty = 0;
    SysPlugin3nrHeader header;
    uint32_t currentCount = 0;
    uint32_t otherMagic;

    if (!host || !plugins || !threeNrPath ||
        (pluginMagic != LOADER_PLUGIN_MAGIC && pluginMagic != ROSALINA_PLUGIN_MAGIC))
        return false;

    while (currentCount < SYSPLUGIN_MAX_PLUGINS && plugins[currentCount].plgid)
        currentCount++;

    otherMagic = pluginMagic == LOADER_PLUGIN_MAGIC ? ROSALINA_PLUGIN_MAGIC : LOADER_PLUGIN_MAGIC;
    emptyPath.type = SYSPLG_PATH_EMPTY;
    emptyPath.size = 1;
    emptyPath.data = &empty;

    context.host = host;
    context.archive = 0;
    context.threeNrFile = 0;
    context.modules[0].pluginMagic = pluginMagic;
    context.modules[0].plugins = plugins;
    context.modules[0].pluginCount = currentCount;
    context.modules[0].activeMask = currentCount ? ((1u << currentCount) - 1u) : 0;
    context.modules[1].pluginMagic = otherMagic;
    context.modules[1].plugins = g_otherPlugins;
    context.modules[1].pluginCount = 0;
    context.modules[1].activeMask = 0;

    if (Failed(host->FSUSER_OpenArchive(&context.archive, ARCHIVE_SDMC, emptyPath)) ||
        Failed(host->FSUSER_OpenFile(&context.threeNrFile, context.archive, MakeAsciiPath(threeNrPath), FS_OPEN_READ, 0)) ||
        !ReadExact(host, context.threeNrFile, 0, &header, sizeof(header)) || header.magic != SYSPLUGIN_3NR_MAGIC ||
        !ScanPlugins(&context, &context.modules[1]))
        goto fail;

    ComputeEnvironment(&context.modules[0], header.buildPairId);
    ComputeEnvironment(&context.modules[1], header.buildPairId);

    if (!InvalidateAll(&context))
        goto fail;

    if (!RepairModule(&context, &context.modules[0]) || !RepairModule(&context, &context.modules[1]) ||
        !CommitModule(&context, &context.modules[0]) || !CommitModule(&context, &context.modules[1]))
        goto fail;

    if (Failed(host->FSFILE_Close(context.threeNrFile)))
        goto fail;
    context.threeNrFile = 0;

    if (StringCompare(threeNrPath, THREE_NR_PATH) == 0 &&
        !RenameThreeNr(&context, threeNrPath, (uint32_t)header.buildPairId))
    {
        goto fail;
    }

    host->FSUSER_CloseArchive(context.archive);
    return true;

fail:
    if (context.threeNrFile)
        host->FSFILE_Close(context.threeNrFile);
    if (context.archive)
        host->FSUSER_CloseArchive(context.archive);
    return false;
}