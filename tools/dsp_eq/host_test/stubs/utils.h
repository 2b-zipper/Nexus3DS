#pragma once
#include <3ds.h>
#define CLAMP(v, m, M) ((v) <= (m) ? (m) : (v) >= (M) ? (M) : (v))
// physical memory window: DSP RAM and the PDN_DSP_CNT register live in host buffers
extern u8 *g_dspRam;
extern u8 g_pdnDspCnt;
#define PA_PTR(addr) ((void *)((u32)(addr) == 0x10141230u ? (void *)&g_pdnDspCnt : (void *)(g_dspRam + ((u32)(addr) - 0x1FF00000u))))
