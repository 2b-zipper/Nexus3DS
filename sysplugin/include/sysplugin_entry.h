#pragma once

#include <stdint.h>

typedef struct
{
    char name[256];
    uint32_t priority;
    uint32_t fileOffset;
    uint32_t plgid;
    union { uint32_t ownAbiLo; uint32_t allocatedAddr; };
    union { uint32_t ownAbiHi; uint32_t totalSize; };
    union { uint32_t expectedEnvLo; uint32_t codeSize; };
    uint32_t pluginPtrSize;
    union { uint32_t expectedEnvHi; uint32_t mainAddr; };
    uint32_t depsMask;
    uint8_t loaded;
    uint8_t codeProtected;
    uint8_t reserved[2];
} SysPluginEntry;
