# DSP-side 3-band equalizer

The 3DS DSP (Teak) mixes all audio and feeds the codec itself, so an equalizer has to run inside the DSP firmware.
This directory holds the routine that does that, plus the tooling to build and test it offline.

## How it works

* The DSP firmware calls `0x3432` once per 4.9 ms audio frame to turn the final mix into 160 stereo s16 samples in
  its BTDMP ring buffer (`call` at program address `0x2FEB`).
* `gen_eq_asm.py` generates a routine that wraps that call: it runs the original routine and then filters the freshly
  written stereo buffer in place with three biquad sections per channel (bass, mids, highs), direct form I,
  Q12 coefficients, 32-bit precision `y` history, saturating output.
  It is placed in a free area of program memory (`0x1000`); the operand of the hooked `call` is redirected to it.
* Parameters live in a block of DSP data memory (`0xD200`, see `gen_eq_asm.py`). The routine does nothing (bit-exact
  bypass) unless the block starts with the magic word `0xE0E1`.
* `eq_design.py` defines the filters: bass = first-order low shelf (midpoint 200 Hz), mids = peaking biquad
  (1 kHz, Q 0.707), highs = first-order high shelf (midpoint 4 kHz); each -24..+24 dB. First-order shelves are used
  because a 200 Hz second-order shelf at 32.7 kHz would need more coefficient precision than 16 bits give.
  The C implementation in `sysmodules/rosalina/source/equalizer.c` must stay in sync with it.

Rosalina (`dsp_eq.c`) applies the patch at runtime (DSP RAM is reachable through the kernel extension's physical mapping at
`0x1FF00000 | 1 << 31`): it writes the routine into DSP program memory, changes the call
operand (only if the surrounding firmware words match `dspEqHookFingerprint`) and fills the parameter block. The
`dsp` module rewrites program memory whenever it (re)loads the firmware, so Rosalina re-checks periodically.

## Rebuilding / testing

Needs [Teakra](https://github.com/wwylele/teakra) (build with cmake; tools `makedsp1`, `dsp1_reader` and `libteakra.a`)
and your own `dspfirm.cdc` (dump it with Rosalina: Miscellaneous options -> Dump DSP firmware).

```
# regenerate the C header used by Rosalina
python3 gen_header.py <teakra-build>/src/makedsp1/makedsp1 ../../sysmodules/rosalina/include/dsp_eq_code.h

# build a patched firmware image and run it in the emulator
python3 gen_eq_asm.py eq.asm && makedsp1 eq.asm eq.dsp1
python3 patch_fw.py dspfirm.cdc eq.dsp1 fw_eq.cdc
g++ -std=c++17 -O2 -I<teakra>/include harness.cpp <teakra-build>/src/libteakra.a -o harness
DSPEQ_WORK=. DSPEQ_HARNESS=./harness python3 regress.py fw_eq.cdc     # 96 points, expects worst error ~0.04 dB
```

`patch_fw.py` output is only for the emulator: the signature of a modified firmware image is not valid.
