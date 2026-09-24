// Host-side stand-in for the parts of libctru that dsp_eq.c / equalizer.c use, so they can be compiled and tested on a PC.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include <stdint.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int8_t s8; typedef int16_t s16; typedef int32_t s32; typedef int64_t s64;
typedef volatile u8 vu8; typedef volatile u16 vu16; typedef volatile u32 vu32;
typedef u32 Result;
typedef u32 Handle;
#define R_FAILED(res) ((res) != 0)
#define R_SUCCEEDED(res) ((res) == 0)
#define CTR_ALIGN(n) __attribute__((aligned(n)))
#define USERBREAK_PANIC 0
#define __dsb() __sync_synchronize()
static inline void svcSleepThread(s64 ns) { (void)ns; }
static inline void svcBreak(int reason) { (void)reason; __builtin_trap(); }
// file system: no SD card in the host test
typedef struct { u32 handle; } FS_Path_dummy;
typedef struct { int type; } FS_Path;
enum { PATH_EMPTY, PATH_ASCII };
#define ARCHIVE_SDMC 0
#define FS_OPEN_READ 1
#define FS_OPEN_WRITE 2
#define FS_OPEN_CREATE 4
static inline FS_Path fsMakePath(int t, const void *d) { (void)d; FS_Path p = {t}; return p; }
#define CORE_SYSTEM 1
#define SYSCLOCK_ARM11 268111856LL
// system tick counter: starts well after boot, advances 0.1 s per call
static inline u64 svcGetSystemTick(void) { static u64 t = 100ULL * 268111856ULL; t += 268111856ULL / 10; return t; }
extern bool g_hostHeadset;
static inline bool osIsHeadsetConnected(void) { return g_hostHeadset; }
