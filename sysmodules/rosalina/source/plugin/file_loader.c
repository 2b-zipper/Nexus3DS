#include <3ds.h>
#include <string.h>
#include <stdio.h>
#include "csvc.h"
#include "plugin.h"
#include "ifile.h"
#include "ifile.h"
#include "utils.h"
#include "draw.h"
#include "menu.h"

// Use a global to avoid stack overflow, those structs are quite heavy
static FS_DirectoryEntry g_entries[10];
static PluginEntry g_foundPlugins[10];

static char g_path[256];
static const char *g_dirPath = "/luma/plugins/%016llX";
static const char *g_defaultPath = "/luma/plugins/default.3gx";

u64 g_titleId;
u32 g_pid;

extern bool PluginConverter_UseCache;

// pluginLoader.s
void gamePatchFunc(void);

static u32 strlen16(const u16 *str)
{
    if (!str)
        return 0;

    const u16 *strEnd = str;

    while (*strEnd)
        ++strEnd;

    return strEnd - str;
}

void GetPlgSelectorSettingsPath(char *path)
{
    sprintf(path, "/luma/plugins/%016llX/plgldr.bin", g_titleId);
}

bool ConfirmOperation(const char *message)
{
    extern u32 g_blockMenuOpen;
    
    g_blockMenuOpen++;
    Draw_Lock();
    Draw_ClearFramebuffer();
    Draw_Unlock();
    
    do
    {
        Draw_Lock();
        Draw_DrawMenuFrame("Confirmation");
        Draw_DrawString(10, 40, COLOR_WHITE, message);
        Draw_DrawString(10, 75, COLOR_WHITE, "Press [A] to confirm, press [B] to cancel.");
        Draw_FlushFramebuffer();
        Draw_Unlock();

        u32 keys;
        do
        {
            keys = waitInputWithTimeout(1000);
        } while (keys == 0 && !menuShouldExit);

        if (keys & KEY_A)
        {
            g_blockMenuOpen--;
            return true;
        }
        else if (keys & KEY_B)
        {
            g_blockMenuOpen--;
            return false;
        }
    } while (!menuShouldExit);

    g_blockMenuOpen--;
    return false;
}

void LoadPlgSelectorSettings(u8 *defaultPlgIndex, PluginEntry *entries, u8 count)
{
    IFile file;
    char path[256];
    u64 size;
    u64 total;

    *defaultPlgIndex = 0xFF;

    GetPlgSelectorSettingsPath(path);

    if (R_SUCCEEDED(IFile_Open(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""), fsMakePath(PATH_ASCII, path), FS_OPEN_READ)))
    {
        if (R_SUCCEEDED(IFile_GetSize(&file, &size)))
        {
            static u8 buffer[2048];
            IFile_Read(&file, &total, buffer, 2048);

            static PluginEntry tmp[10];
            memcpy(tmp, entries, sizeof(PluginEntry) * count);

            *defaultPlgIndex = buffer[0];

            u32 index = 0;
            for (u32 i = sizeof(u8); i < total;)
            {
                const char *name = (char *)(buffer + i);

                for (u32 j = 0; j < count; j++)
                {
                    if (strcmp(name, tmp[j].name) == 0)
                    {
                        entries[index++] = tmp[j];
                        tmp[j].name[0] = 0;
                        break;
                    }
                }

                i += strlen(name) + 1;
            }

            // Invalid index
            if (*defaultPlgIndex < 0xFE && *defaultPlgIndex >= index)
            {
                *defaultPlgIndex = 0xFF;
            }

            // New plugins
            for (u32 i = 0; i < count; i++)
            {
                if (tmp[i].name[0] != 0)
                {
                    // entries[index++] = tmp[i];
                    memcpy(&entries[index++], &tmp[i], sizeof(PluginEntry));
                    tmp[i].name[0] = 0;
                }
            }
        }

        IFile_Close(&file);
    }
}

void SavePluginSelectorSettings(const PluginEntry *entries, u8 count, u8 defaultPlgIndex)
{
    IFile file;
    u64 total;
    char path[256];
    GetPlgSelectorSettingsPath(path);

    if (R_SUCCEEDED(IFile_Open(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""), fsMakePath(PATH_ASCII, path), FS_OPEN_CREATE | FS_OPEN_WRITE)))
    {
        IFile_SetSize(&file, 0);

        // Reserve 1 byte for default plugin index
        u8 max = 0xFF;
        IFile_Write(&file, &total, &max, sizeof(u8), FS_WRITE_FLUSH);

        for (u8 i = 0; i < count; i++)
        {
            const PluginEntry *entry = &entries[i];
            IFile_Write(&file, &total, &entry->name, strlen(entry->name) + 1, FS_WRITE_FLUSH);
        }

        // Write index of default plugin to the first byte
        u32 oldPos = file.pos;
        file.pos = 0;
        IFile_Write(&file, &total, &defaultPlgIndex, sizeof(u8), FS_WRITE_FLUSH);
        file.pos = oldPos;

        IFile_Close(&file);
    }
}

u8 FileOptions(PluginEntry *entries, u8 *count, u8 index, u8 *defaultPlgIndex)
{
    /* Options list structure */
    struct
    {
        const char *name;
        bool enabled;
    } options[] = {
        {"Set as default plugin", true},
        {"Remove from default", false},
        {"Remove this file", true},
        {"Don't load plugin for this app", true}
    };

    const char *name = entries[index].name;
    char message[256];
    const u32 nbOptions = sizeof(options) / sizeof(options[0]);
    u8 selected = 0;

    sprintf(message, "Choose option for '%s'", name);

    ClearScreenQuickly();
    
    extern u32 g_blockMenuOpen;
    g_blockMenuOpen++;
    Draw_Lock();
    hidSetRepeatParameters(200, 100);
    Draw_ClearFramebuffer();
    Draw_Unlock();

    // Init options status
    if (entries[index].isDefault)
    {
        // Set as default option
        options[0].enabled = false;

        // Remove from default option
        options[1].enabled = true;

        // Don't load option
        options[3].enabled = true;
    }
    else if (*defaultPlgIndex == 0xFE)
    {
        // "Don't load" is active
        options[0].enabled = true;
        options[1].enabled = true; // Remove "don't load" setting
        options[3].enabled = false;
    }

    do
    {
        Draw_Lock();
        Draw_DrawMenuFrame("Plugin selector");
        Draw_DrawString(10, 40, COLOR_LIME, message);

        for (u8 i = 0; i < nbOptions; i++)
        {
            bool isSelected = (i == selected);
            u32 color = options[i].enabled ? COLOR_WHITE : COLOR_GRAY;
            
            if (isSelected) {
                Draw_DrawString(15, 40 + (i + 2) * SPACING_Y, COLOR_ORANGE, ">>");
                u32 selectedColor = options[i].enabled ? COLOR_CYAN : COLOR_LIGHT_BLUE;
                Draw_DrawString(35, 40 + (i + 2) * SPACING_Y, selectedColor, options[i].name);
            } else {
                Draw_DrawString(15, 40 + (i + 2) * SPACING_Y, COLOR_GRAY, " *");
                Draw_DrawString(35, 40 + (i + 2) * SPACING_Y, color, options[i].name);
            }
        }
        Draw_FlushFramebuffer();
        Draw_Unlock();

        u32 keys;
        do
        {
            keys = waitInputWithTimeout(1000);
        } while (keys == 0 && !menuShouldExit);

        if ((keys & KEY_A) && options[selected].enabled)
        {
            if (selected == 0) // Set as default
            {
                for (u8 i = 0; i < *count; i++)
                {
                    entries[i].isDefault = false;
                }
                entries[index].isDefault = true;
                *defaultPlgIndex = index;

                // Set as default plugin option
                options[0].enabled = false;

                // Remove from default plugin option
                options[1].enabled = true;
                
                Draw_Lock();
                Draw_ClearFramebuffer();
                Draw_Unlock();
            }
            else if (selected == 1) // Remove from default
            {
                for (u8 i = 0; i < *count; i++)
                {
                    entries[i].isDefault = false;
                }
                *defaultPlgIndex = 0xFF;

                options[0].enabled = true;
                options[1].enabled = false;

                // Don't load option
                options[3].enabled = true;
                
                Draw_Lock();
                Draw_ClearFramebuffer();
                Draw_Unlock();
            }
            else if (selected == 2)
            {
                if (ConfirmOperation("Are you sure you want to remove this file?"))
                {
                    FS_Archive sdmc;

                    if (R_SUCCEEDED(FSUSER_OpenArchive(&sdmc, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""))))
                    {
                        char path[256];

                        if(strcmp(name, "<default.3gx>") == 0){
                            strcpy(path, g_defaultPath);
                        }
                        else
                        {
                            sprintf(path, "/luma/plugins/%016llX/%s", g_titleId, name);
                        }

                        Result delResult = FSUSER_DeleteFile(sdmc, fsMakePath(PATH_ASCII, path));
                        FSUSER_CloseArchive(sdmc);

                        if (R_SUCCEEDED(delResult))
                        {
                            for (u8 i = index; i < *count - 1; i++)
                            {
                                entries[i] = entries[i + 1];
                            }

                            *count -= 1;

                            if(*count <=0) {
                                return 1;
                            }
                            // No operations are available for this file
                            break;
                        }
                    }
                }

                Draw_Lock();
                Draw_ClearFramebuffer();
                Draw_Unlock();
            }
            else if (selected == 3)
            {
                if (ConfirmOperation("Are you sure you want to disable plugin loading?\nYou can re-enable it by selecting\n\"Remove from default\"."))
                {
                    for (u8 i = 0; i < *count; i++)
                    {
                        entries[i].isDefault = false;
                    }
                    // Mark as default plugin with special index
                    *defaultPlgIndex = 0xFE;

                    options[0].enabled = true;
                    options[1].enabled = true;
                    options[3].enabled = false;
                }

                Draw_Lock();
                Draw_ClearFramebuffer();
                Draw_Unlock();
            }
        }
        else if (keys & KEY_B)
        {
            break;
        }
        else if (keys & KEY_DOWN)
        {
            if (++selected >= nbOptions)
                selected = 0;
        }
        else if (keys & KEY_UP)
        {
            if (selected-- <= 0)
                selected = nbOptions - 1;
        }
    } while (!menuShouldExit);
    
    g_blockMenuOpen--;
    return 0;
}

static char *AskForFileName(PluginEntry *entries, u8 count)
{
    char *filename = NULL;
    u8 selected = 0;
    s8 holding = -1;

    u8 defaultPlgIndex = 0xFF;
    LoadPlgSelectorSettings(&defaultPlgIndex, entries, count);

    if(defaultPlgIndex == 0xFE)
    {
        // Don't load any plugin option
        if (!(HID_PAD & KEY_SELECT)) {
            return NULL;
        }
    }
    else if (count == 1)
    {
        if (!(HID_PAD & KEY_SELECT)) {
            return entries[0].name;
        }
    }
    else if (defaultPlgIndex != 0xFF)
    {
        PluginEntry *entry = &entries[defaultPlgIndex];

        if (!(HID_PAD & KEY_SELECT))
        {
            entry->isDefault = true;

            return entry->name;
        }

        entry->isDefault = true;
    }

    extern u32 g_blockMenuOpen;
    g_blockMenuOpen++;
    menuEnter();

    ClearScreenQuickly();
    
    Draw_Lock();
    hidSetRepeatParameters(200, 100);
    Draw_ClearFramebuffer();
    Draw_Unlock();

    do
    {
        Draw_Lock();
        Draw_DrawMenuFrame("Plugin selector");
        Draw_DrawString(10, 40, COLOR_WHITE, "Some 3gx files were found.");
        Draw_DrawString(10, 50, COLOR_WHITE, "[A] Select, [B] Cancel, [X] Options, [Y] Reorder");

        for (u8 i = 0; i < count; i++)
        {
            static char buf[256];
            bool isDefault = entries[i].isDefault;
            bool isSelected = (i == selected && holding == -1);

            char name[128];
            strncpy(name, entries[i].name, sizeof(name) - 1);

            if (i == holding)
            {
                sprintf(buf, "%s%s", (const char *)name, (isDefault ? " [Default]" : ""));
                Draw_DrawString(13, 40 + (i + 3) * SPACING_Y, COLOR_GRAY, "->");
                Draw_DrawString(33, 40 + (i + 3) * SPACING_Y, COLOR_LIGHT_BLUE, buf);
            }
            else
            {
                sprintf(buf, "%s%s", (const char *)name, (isDefault ? " [Default]" : ""));
                
                if (isSelected) {
                    Draw_DrawString(15, 40 + (i + 3) * SPACING_Y, COLOR_ORANGE, ">>");
                    Draw_DrawString(35, 40 + (i + 3) * SPACING_Y, COLOR_CYAN, buf);
                } else {
                    Draw_DrawString(15, 40 + (i + 3) * SPACING_Y, COLOR_GRAY, " *");
                    Draw_DrawString(35, 40 + (i + 3) * SPACING_Y, COLOR_WHITE, buf);
                }
            }
        }

        Draw_FlushFramebuffer();
        Draw_Unlock();

        u32 keys;
        do
        {
            keys = waitInputWithTimeout(1000);
        } while (keys == 0 && !menuShouldExit);

        if (keys & KEY_A)
        {
            if (holding == -1)
            {
                filename = entries[selected].name;
                break;
            }
        }
        else if (keys & KEY_B)
        {
            break;
        }
        else if (keys & KEY_X)
        {
            if (holding == -1)
            {
                if(FileOptions(entries, &count, selected, &defaultPlgIndex))
                    break;

                if (count < selected + 1)
                    selected = count - 1;

                Draw_Lock();
                Draw_ClearFramebuffer();
                Draw_Unlock();
            }
        }
        else if (keys & KEY_Y)
        {
            if (holding == -1)
            {
                holding = selected;
                Draw_Lock();
                Draw_ClearFramebuffer();
                Draw_Unlock();
            }
            else
            {
                selected = holding;
                holding = -1;
                Draw_Lock();
                Draw_ClearFramebuffer();
                Draw_Unlock();
            }
        }
        else if (keys & KEY_DOWN)
        {
            if (holding == -1 && ++selected >= count)
                selected = 0;
            else if (holding != -1 && holding < count - 1)
            {
                PluginEntry tmp = entries[holding];
                entries[holding] = entries[holding + 1];
                entries[holding + 1] = tmp;
                holding++;
                Draw_Lock();
                Draw_ClearFramebuffer();
                Draw_Unlock();
            }
        }
        else if (keys & KEY_UP)
        {
            if (holding == -1 && selected-- <= 0)
                selected = count - 1;
            else if (holding > 0)
            {
                PluginEntry tmp = entries[holding];
                entries[holding] = entries[holding - 1];
                entries[holding - 1] = tmp;
                holding--;
                Draw_Lock();
                Draw_ClearFramebuffer();
                Draw_Unlock();
            }
        }
    } while (!menuShouldExit);

    g_blockMenuOpen--;
    menuLeave();

    SavePluginSelectorSettings(entries, count, defaultPlgIndex);

    return filename;
}

static Result FindPluginFile(u64 tid, u8 defaultFound)
{
    char filename[256];
    u32 entriesNb = 0;
    Handle dir = 0;
    Result res;
    FS_Archive sdmcArchive = 0;
    FS_DirectoryEntry *entries = g_entries;

    u8 foundPluginCount = 0;
    PluginEntry *foundPlugins = g_foundPlugins;

    memset(entries, 0, sizeof(g_entries));
    memset(foundPlugins, 0, sizeof(g_foundPlugins));
    sprintf(g_path, g_dirPath, tid);

    if (R_FAILED((res = FSUSER_OpenArchive(&sdmcArchive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, "")))))
        goto exit;

    if (R_FAILED((res = FSUSER_OpenDirectory(&dir, sdmcArchive, fsMakePath(PATH_ASCII, g_path)))))
    {
        if (defaultFound == 1)
        {
            res = 0;
            goto defaultplg;
        }
        else
            goto exit;
    }

    strcat(g_path, "/");
    while (foundPluginCount < 10 && R_SUCCEEDED(FSDIR_Read(dir, &entriesNb, 10, entries)))
    {
        if (entriesNb == 0)
            break;

        static const u16 *validExtension = u"3gx";

        for (u32 i = 0; i < entriesNb; ++i)
        {
            FS_DirectoryEntry *entry = &entries[i];
            char path[512];

            // If entry is a folder, skip it
            if (entry->attributes & 1)
                continue;

            // Check extension
            u32 size = strlen16(entry->name);
            if (size <= 5)
                continue;

            u16 *fileExt = entry->name + size - 3;

            if (memcmp(fileExt, validExtension, 3 * sizeof(u16)))
                continue;

            // Convert name from utf16 to utf8
            int units = utf16_to_utf8((u8 *)filename, entry->name, 100);
            if (units == -1)
                continue;
            filename[units] = 0;

            if (foundPluginCount >= 10)
                break;

            strcpy(foundPlugins[foundPluginCount].name, filename);

            sprintf(path, "%s%s", g_path, filename);
            foundPluginCount++;
        }
    }

defaultplg:

    if (foundPluginCount < 10 && defaultFound == 1)
    {
        strcpy(foundPlugins[foundPluginCount].name, "<default.3gx>");
        foundPluginCount++;
    }

    if (!foundPluginCount)
        res = MAKERESULT(28, 4, 0, 1018);
    else
    {
        char *name = AskForFileName(foundPlugins, foundPluginCount);

        if (!name)
        {
            res = MAKERESULT(28, 4, 0, 1018);
        }
        else if (strcmp(name, "<default.3gx>") == 0)
        {
            PluginLoaderCtx.pluginPath = g_defaultPath;
            PluginLoaderCtx.header.isDefaultPlugin = 1;
        }
        else
        {
            u32 len = strlen(g_path);
            name[sizeof(((PluginEntry *)NULL)->name) - len] = 0;
            strcat(g_path, name);
            PluginLoaderCtx.pluginPath = g_path;
        }
    }

exit:
    FSDIR_Close(dir);
    FSUSER_CloseArchive(sdmcArchive);

    return res;
}

static Result OpenFile(IFile *file, const char *path)
{
    return IFile_Open(file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""), fsMakePath(PATH_ASCII, path), FS_OPEN_READ);
}

static Result OpenPluginFile(u64 tid, IFile *plugin)
{
    // Note: I didn't find a way to check if the file exists without opening it,
    // so I made this mess instead. If someone knows a better way, please do it.
    u8 defaultFound = 0;

    if (OpenFile(plugin, g_defaultPath) == 0) {
        defaultFound = 1;
        IFile_Close(plugin);
    }

    if (R_FAILED(FindPluginFile(tid, defaultFound)))
    {
        return -1;
    }

    if (PluginLoaderCtx.header.isDefaultPlugin == 0)
    {
        if (OpenFile(plugin, g_path))
            return -1;
    } else {
        if (OpenFile(plugin, g_defaultPath))
            return -1;
    }

    return 0;
}

static Result CheckPluginCompatibility(_3gx_Header *header, u32 processTitle)
{
    static char errorBuf[0x100];

    if (header->targets.count == 0)
        return 0;

    for (u32 i = 0; i < header->targets.count; ++i)
    {
        if (header->targets.titles[i] == processTitle)
            return 0;
    }

    sprintf(errorBuf, "The plugin - %s -\nis not compatible with this game.\n"
                      "Contact \"%s\" for more infos.",
            header->infos.titleMsg, header->infos.authorMsg);

    PluginLoaderCtx.error.message = errorBuf;

    return -1;
}

bool TryToLoadPlugin(Handle process, bool isHomebrew)
{
    u64 tid;
    u64 fileSize;
    IFile plugin;
    Result res;
    _3gx_Header fileHeader;
    _3gx_Header *header = NULL;
    _3gx_Executable *exeHdr = NULL;
    PluginLoaderContext *ctx = &PluginLoaderCtx;
    PluginHeader *pluginHeader = &ctx->header;
    const u32 memRegionSizes[] =
        {
            5 * 1024 * 1024,  // 5 MiB
            2 * 1024 * 1024,  // 2 MiB
            10 * 1024 * 1024, // 10 MiB
            5 * 1024 * 1024,  // 5 MiB (Reserved)
        };

    // Get title id
    svcGetProcessInfo((s64 *)&tid, process, 0x10001);
    g_titleId = tid;

    // Get process id
    svcGetProcessId(&g_pid, process);

    memset(pluginHeader, 0, sizeof(PluginHeader));
    pluginHeader->magic = HeaderMagic;

    // Try to open plugin file
    if (ctx->useUserLoadParameters && (ctx->userLoadParameters.lowTitleId == 0 || (u32)tid == ctx->userLoadParameters.lowTitleId))
    {
        if (!ctx->userLoadParameters.persistent)
            ctx->useUserLoadParameters = false;
        ctx->pluginMemoryStrategy = ctx->userLoadParameters.pluginMemoryStrategy;
        if (OpenFile(&plugin, ctx->userLoadParameters.path))
            return false;

        ctx->pluginPath = ctx->userLoadParameters.path;

        memcpy(pluginHeader->config, ctx->userLoadParameters.config, 32 * sizeof(u32));
    }
    else
    {
        if (R_FAILED(OpenPluginFile(tid, &plugin)))
            return false;
    }

    if (R_FAILED((res = IFile_GetSize(&plugin, &fileSize))))
        ctx->error.message = "Couldn't get file size";

    if (!res && R_FAILED(res = Check_3gx_Magic(&plugin)))
    {
        const char *errors[] =
            {
                "Couldn't read file.",
                "Invalid plugin file\nNot a valid 3GX plugin format!",
                "Outdated plugin file\nCheck for an updated plugin.",
                "Outdated plugin loader\nCheck for Nexus3DS updates."};

        u32 err = (R_MODULE(res) == RM_LDR ? R_DESCRIPTION(res) : 0);

        // Try to convert
        if (err == 2)
        {
            static char convertedPath[256];
            IFile convertedPlugin;

            sprintf(convertedPath, "%s%s", PluginLoaderCtx.pluginPath, ".cvted");

            if (PluginConverter_UseCache && R_SUCCEEDED(IFile_Open(&convertedPlugin, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""), fsMakePath(PATH_ASCII, convertedPath), FS_OPEN_READ)))
            {
                IFile_Close(&plugin);

                PluginLoaderCtx.pluginPath = convertedPath;
                plugin = convertedPlugin;

                if (R_FAILED(res = IFile_GetSize(&plugin, &fileSize)))
                {
                    ctx->error.message = "Couldn't get file size";
                }
            }
            else
            {
                res = IFile_Open(&convertedPlugin, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""), fsMakePath(PATH_ASCII, convertedPath), FS_OPEN_CREATE | FS_OPEN_WRITE);
                if (R_SUCCEEDED(res))
                {
                    if (TryToConvertPlugin(&plugin, &convertedPlugin) == 0)
                    {
                        IFile_Close(&convertedPlugin);
                        IFile_Close(&plugin);

                        res = IFile_Open(&convertedPlugin, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""), fsMakePath(PATH_ASCII, convertedPath), FS_OPEN_READ);
                        if (R_SUCCEEDED(res))
                        {
                            PluginLoaderCtx.pluginPath = convertedPath;
                            plugin = convertedPlugin;

                            if (R_FAILED(res = IFile_GetSize(&plugin, &fileSize)))
                            {
                                ctx->error.message = "Couldn't get file size";
                            }
                        }
                        else
                        {
                            ctx->error.message = errors[err];
                        }
                    }
                    else
                    {
                        ctx->error.message = errors[err];
                    }
                }
            }
        }
        else
            ctx->error.message = errors[err];
    }

    // Read header
    if (!res && R_FAILED((res = Read_3gx_Header(&plugin, &fileHeader))))
        ctx->error.message = "Couldn't read file";

    // Check compatibility
    if (!res && fileHeader.infos.compatibility == PLG_COMPAT_EMULATOR)
    {
        ctx->error.message = "Plugin is only compatible with emulators";
        res = -1;
    }

    // Check if plugin can load on homebrew
    if (!res && (isHomebrew && !fileHeader.infos.allowHomebrewLoad)) {
        // Do not display message as this is a common case
        ctx->error.message = NULL;
        res = -1;
    }

    // Flags
    if (!res)
    {
        ctx->eventsSelfManaged = fileHeader.infos.eventsSelfManaged;
        ctx->isMemPrivate = fileHeader.infos.usePrivateMemory;
        if (ctx->pluginMemoryStrategy == PLG_STRATEGY_SWAP && fileHeader.infos.swapNotNeeded)
            ctx->pluginMemoryStrategy = PLG_STRATEGY_NONE;
    }

    // Set memory region size according to header
    if (!res && R_FAILED((res = MemoryBlock__SetSize(memRegionSizes[fileHeader.infos.memoryRegionSize]))))
    {
        ctx->error.message = "Couldn't set memblock size.";
    }

    // Ensure memory block is mounted
    if (!res && R_FAILED((res = MemoryBlock__IsReady())))
        ctx->error.message = "Failed to allocate memory.";

    // Plugins will not exceed 5MB so this is fine
    if (!res)
    {
        header = (_3gx_Header *)(ctx->memblock.memblock + g_memBlockSize - (u32)fileSize);
        memcpy(header, &fileHeader, sizeof(_3gx_Header));
    }

    // Parse rest of header
    if (!res && R_FAILED((res = Read_3gx_ParseHeader(&plugin, header))))
        ctx->error.message = "Couldn't read file";

    // Read embedded save/load functions
    if (!res && R_FAILED((res = Read_3gx_EmbeddedPayloads(&plugin, header))))
        ctx->error.message = "Invalid save/load payloads.";

    // Save exe checksum
    if (!res)
        ctx->exeLoadChecksum = header->infos.exeLoadChecksum;

    // Check titles compatibility
    if (!res)
        res = CheckPluginCompatibility(header, (u32)tid);

    // Read code
    if (!res && R_FAILED(res = Read_3gx_LoadSegments(&plugin, header, ctx->memblock.memblock + sizeof(PluginHeader))))
    {
        if (res == MAKERESULT(RL_PERMANENT, RS_INVALIDARG, RM_LDR, RD_NO_DATA))
            ctx->error.message = "This plugin requires a loading function.";
        else if (res == MAKERESULT(RL_PERMANENT, RS_INVALIDARG, RM_LDR, RD_INVALID_ADDRESS))
            ctx->error.message = "This plugin file is corrupted.";
        else
            ctx->error.message = "Couldn't read plugin's code";
    }

    if (R_FAILED(res))
    {
        ctx->error.code = res;
        goto exitFail;
    }

    pluginHeader->version = header->version;
    pluginHeader->waitForReplyTimeout = 10000000000ULL;
    // Code size must be page aligned
    exeHdr = &header->executable;
    pluginHeader->exeSize = (sizeof(PluginHeader) + exeHdr->codeSize + exeHdr->rodataSize + exeHdr->dataSize + exeHdr->bssSize + 0x1000) & ~0xFFF;
    // Make heap VA congruent with its PA modulo 1 MiB, so the kernel can map it with sections and large pages.
    pluginHeader->heapVA = 0x06000000 | ((u32)PA_FROM_VA_PTR(ctx->memblock.memblock + pluginHeader->exeSize) & 0xFFFFF);
    pluginHeader->heapSize = g_memBlockSize - pluginHeader->exeSize;
    pluginHeader->plgldrEvent = ctx->plgEventPA;
    pluginHeader->plgldrReply = ctx->plgReplyPA;

    // Clear old event data
    PLG__NotifyEvent(PLG_OK, false);

    // Copy header to memblock
    memcpy(ctx->memblock.memblock, pluginHeader, sizeof(PluginHeader));
    // Clear heap
    memset(ctx->memblock.memblock + pluginHeader->exeSize, 0, pluginHeader->heapSize);

    // Enforce RWX mmu mapping
    svcControlProcess(process, PROCESSOP_SET_MMU_TO_RWX, 0, 0);
    // Ask the kernel to signal when the process is about to be terminated
    svcControlProcess(process, PROCESSOP_SIGNAL_ON_EXIT, 0, 0);

    // Mount the plugin memory in the process
    if (R_SUCCEEDED(MemoryBlock__MountInProcess()) &&
        // ForceRWXPages does not reach the 64 KiB aligned pages, force RWX again.
        R_SUCCEEDED(svcControlProcess(process, PROCESSOP_SET_MMU_TO_RWX, 0, 0)))
    // Install hook
    {
        u32 procStart = 0x00100000;
        u32 *game = (u32 *)procStart;

        extern u32 g_savedGameInstr[2];

        if (R_FAILED((res = svcMapProcessMemoryEx(CUR_PROCESS_HANDLE, procStart, process, procStart, 0x1000, 0))))
        {
            ctx->error.message = "Couldn't map process";
            ctx->error.code = res;
            goto exitFail;
        }

        g_savedGameInstr[0] = game[0];
        g_savedGameInstr[1] = game[1];

        game[0] = 0xE51FF004; // ldr pc, [pc, #-4]
        game[1] = (u32)PA_FROM_VA_PTR(gamePatchFunc);
        svcFlushEntireDataCache();
        svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE, procStart, 0x1000);
    }
    else
        goto exitFail;

    IFile_Close(&plugin);
    return true;

exitFail:
    IFile_Close(&plugin);
    MemoryBlock__Free();

    return false;
}
