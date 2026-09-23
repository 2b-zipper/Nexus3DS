#pragma once
#include <3ds.h>
typedef struct { u32 handle; } IFile;
static inline Result IFile_Open(IFile *f, int a, FS_Path p, FS_Path q, u32 fl) { (void)f; (void)a; (void)p; (void)q; (void)fl; return 1; }
static inline Result IFile_Close(IFile *f) { (void)f; return 0; }
static inline Result IFile_Read(IFile *f, u64 *t, void *b, u32 l) { (void)f; (void)t; (void)b; (void)l; return 1; }
static inline Result IFile_Write(IFile *f, u64 *t, const void *b, u32 l, u32 fl) { (void)f; (void)t; (void)b; (void)l; (void)fl; return 1; }
static inline Result IFile_SetSize(IFile *f, u64 s) { (void)f; (void)s; return 1; }
