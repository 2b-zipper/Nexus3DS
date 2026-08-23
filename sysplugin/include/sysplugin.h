#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "sysplugin_entry.h"

typedef int32_t Result;
typedef uint32_t Handle;
typedef uint64_t SysPlgArchive;

typedef struct
{
    uint32_t type;
    uint32_t size;
    const void *data;
} SysPlgPath;

typedef struct
{
    uint16_t name[0x106];
    char shortName[0x0A];
    char shortExt[0x04];
    uint8_t valid;
    uint8_t reserved;
    uint32_t attributes;
    uint64_t fileSize;
} SysPlgDirectoryEntry;

typedef struct
{
    Result (*FSUSER_OpenArchive)(SysPlgArchive *archive, uint32_t archiveId, SysPlgPath path);
    Result (*FSUSER_CloseArchive)(SysPlgArchive archive);
    Result (*FSUSER_OpenDirectory)(Handle *dir, SysPlgArchive archive, SysPlgPath path);
    Result (*FSDIR_Read)(Handle dir, uint32_t *entriesRead, uint32_t maxEntries, SysPlgDirectoryEntry *entry);
    Result (*FSDIR_Close)(Handle dir);
    Result (*FSUSER_OpenFile)(Handle *file, SysPlgArchive archive, SysPlgPath path, uint32_t openFlags, uint32_t attributes);
    Result (*FSFILE_Read)(Handle file, uint32_t *bytesRead, uint64_t offset, void *buffer, uint32_t size);
    Result (*FSFILE_Write)(Handle file, uint32_t *bytesWritten, uint64_t offset, const void *buffer, uint32_t size, uint32_t flags);
    Result (*FSFILE_Close)(Handle file);
    Result (*FSUSER_DeleteFile)(SysPlgArchive archive, SysPlgPath path);
    Result (*FSUSER_RenameFile)(SysPlgArchive sourceArchive, SysPlgPath sourcePath, SysPlgArchive destinationArchive, SysPlgPath destinationPath);
} SysPluginHost;

#define SYSPLUGIN_MAX_PLUGINS 31u
#define SYSPLUGIN_NAME_SIZE 256u
#define SYSPLUGIN_3NX_HEADER_SIZE 0x30u
#define SYSPLUGIN_3NR_MAGIC       0x21524E33u
#define SYSPLUGIN_HOST_PROVIDER   0u
#define LOADER_PLUGIN_MAGIC       0x25584E33u
#define ROSALINA_PLUGIN_MAGIC     0x24584E33u

#pragma pack(push, 1)
typedef struct
{
    uint32_t magic;
    uint32_t plgid;
    uint32_t codeSize;
    uint32_t dataSize;
    uint32_t bssSize;
    uint32_t fastRelocSize;
    uint32_t repairSize;
    uint32_t ownAbiLo;
    uint32_t ownAbiHi;
    uint32_t expectedEnvLo;
    uint32_t expectedEnvHi;
    uint32_t metadataSize;
} SysPlugin3nxHeader;

typedef struct
{
    uint64_t key;
    uint32_t value;
} SysPluginExportRecord;

typedef struct
{
    uint32_t providerId;
    uint32_t count;
} SysPluginRepairGroup;

typedef struct
{
    uint64_t key;
    uint32_t cacheOffset;
    uint32_t addend;
} SysPluginRepairRecord;

typedef struct
{
    uint32_t magic;
    uint64_t buildPairId;
    uint32_t codeOffset;
    uint32_t codeSize;
    uint32_t dataSize;
    uint32_t loaderCount;
    uint32_t rosalinaCount;
} SysPlugin3nrHeader;
#pragma pack(pop)

bool Main(const SysPluginHost *host, uint32_t pluginMagic, SysPluginEntry *plugins, const char *threeNrPath);
