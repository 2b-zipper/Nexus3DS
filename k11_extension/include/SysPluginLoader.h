#pragma once

#include "types.h"

#define SYSPLUGIN_MAX_PLUGINS 31u

#define ROSALINA_PLUGIN_MAGIC 0x24584E33u
#define LOADER_PLUGIN_MAGIC   0x25584E33u

#define SYSPLG_PATH_EMPTY 1u
#define SYSPLG_PATH_ASCII 3u

typedef struct
{
    u32 type;
    u32 size;
    const void *data;
} SysPlgPath;

typedef u64 SysPlgArchive;

typedef struct
{
    u16 name[0x106];
    char shortName[0x0A];
    char shortExt[0x04];
    u8 valid;
    u8 reserved;
    u32 attributes;
    u64 fileSize;
} SysPlgDirectoryEntry;

typedef struct
{
    Result (*FSUSER_OpenArchive)(SysPlgArchive *archive, u32 archiveId, SysPlgPath path);
    Result (*FSUSER_CloseArchive)(SysPlgArchive archive);

    Result (*FSUSER_OpenDirectory)(Handle *dir, SysPlgArchive archive, SysPlgPath path);
    Result (*FSDIR_Read)(Handle dir, u32 *entriesRead, u32 maxEntries, SysPlgDirectoryEntry *entry);
    Result (*FSDIR_Close)(Handle dir);

    Result (*FSUSER_OpenFile)(Handle *file, SysPlgArchive archive, SysPlgPath path, u32 openFlags, u32 attributes);
    Result (*FSFILE_Read)(Handle file, u32 *bytesRead, u64 offset, void *buffer, u32 size);
    Result (*FSFILE_Write)(Handle file, u32 *bytesWritten, u64 offset, const void *buffer, u32 size, u32 flags);
    Result (*FSFILE_Close)(Handle file);
    Result (*FSUSER_DeleteFile)(SysPlgArchive archive, SysPlgPath path);
    Result (*FSUSER_RenameFile)(SysPlgArchive sourceArchive, SysPlgPath sourcePath, SysPlgArchive destinationArchive, SysPlgPath destinationPath);

} SysPluginHost;

typedef Result (*SysPluginLoaderEntryFn)(
    const SysPluginHost *host,
    u32 pluginMagic,
    u32 rangeLow,
    u32 rangeHigh,
    bool unlock
);

Result SysPluginLoader_Main(
    const SysPluginHost *host,
    u32 pluginMagic,
    u32 rangeLow,
    u32 rangeHigh,
    bool unlock
);
