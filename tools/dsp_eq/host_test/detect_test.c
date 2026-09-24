// Runs the Rosalina patch logic on a DSP RAM dump (no emulation) and prints what it changed.
// usage: detect_test <dsp ram dump 0x80000 bytes>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void hosttest_set_gains(int, int, int);
void hosttest_tick(unsigned char *);
void hosttest_status(unsigned char *, unsigned *, unsigned *);
int main(int argc, char **argv)
{
    unsigned char *ram = malloc(0x80000);
    FILE *f = fopen(argv[1], "rb");
    if (!f || fread(ram, 1, 0x80000, f) != 0x80000) { printf("cannot read dump\n"); return 1; }
    fclose(f);
    unsigned short *w = (unsigned short *)ram;
    unsigned short before[0x40] ; memcpy(before, w + 0x1000, sizeof(before));
    hosttest_set_gains(6, 0, 0);
    for (int i = 0; i < 40; i++)
        hosttest_tick(ram);
    unsigned hook, magic;
    hosttest_status(ram, &hook, &magic);
    printf("hook A operand now %04x, magic %04x\n", hook, magic);
    for (unsigned a = 0x2F00; a < 0x3100; a++)
        if (w[a] == 0x1000 || w[a] == 0x100A) printf("  program word 0x%04X = %04X  (patched)\n", a, w[a]);
    printf("  code area: word0 %04x (call), operand A %04x, entry B %04x operand B %04x\n", w[0x1000], w[0x1001], w[0x100A], w[0x100B]);
    printf("  parameter block at data 0x8100: magic %04x, first coefficient word %04x\n", ((unsigned short *)(ram + 0x40000))[0x8100], ((unsigned short *)(ram + 0x40000))[0x8110]);
    return 0;
}
