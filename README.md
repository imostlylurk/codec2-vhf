# codec2-vhf

Optimised Codec2 vocoder modes for VHF digital voice, derived from
[Codec2-mod](https://github.com/M17-Project/Codec2-mod) (M17-Project) and
[Codec2](https://github.com/drowe67/codec2) by David Rowe VK5DGR et al.
LGPL 2.1 - see LICENSE.

## Modes

| Mode | Frame  | Bits | Bitstream                         |
|------|--------|------|-----------------------------------|
| 3200 | 20 ms  | 64   | bit-exact with reference Codec2   |
| 1600 | 40 ms  | 64   | bit-exact with reference Codec2   |
| 1300 | 40 ms  | 52   | bit-exact with reference Codec2   |

The 1600 and 1300 bps modes were added in this fork using the same
optimised structure as the 3200 mode: static, fixed-size state (no
heap), split encoder/decoder states, KISS FFT, and the same 10 ms
internal analysis pipeline.  The 1600 encoder packs 4 voicing bits
(10 ms update), two Wo + energy pairs (20 ms update) and one
scalar-LSP vector (40 ms update).  The 1300 encoder samples Wo,
energy and scalar LSPs once per 40 ms frame; the decoder interpolates
the three in-between 10 ms frames in both modes.

Encoder bit-exactness for all three modes has been verified
byte-for-byte against `c2enc` from codec2 1.2 on `hts1a` (150 x 20 ms
frames of 3200, 75 x 40 ms frames of 1600 and 1300).
Decoded audio is not sample-identical to the reference implementation,
but after the 1600/1300 interpolation-weight fix (see git history) the
decoder parameter streams (Wo, energy, LSPs, voicing per 10 ms
sub-frame) match the reference decoder exactly on real speech, and
waveform correlation with `c2dec` output of the same bitstream is
0.996+ with short-time envelope correlation 1.0000 for all three modes
(measured on a 28 s speech sample).  The residual difference is float
rounding in the restructured decoder DSP.

Like upstream, bit-exactness of the encoded bitstream depends on the
accuracy of the C library's `cosf()` during analysis (glibc: identical;
newlib-nano on Cortex-M4: ~1 frame in 150 may differ).

## Goals / roadmap

A single, small, embedded-friendly vocoder suite for VHF digital voice:

- [x] M17, Full rate voice (3200)
- [x] M17 HR (Legacy), SLATE (1600)
- [x] Codec2 1300 (FreeDV2400A/B)

## API

State is owned by the caller and can be allocated statically.  No
create/destroy, no heap.  The encoder and decoder states are separate
objects and can run concurrently full duplex.

```c
#include "codec2_vhf.h"

static codec2_encoder_t enc;
static codec2_decoder_t dec;

codec2_vhf_encoder_init(&enc);
codec2_vhf_decoder_init(&dec);

int16_t speech_3200[CODEC2_VHF_SAMPLES_PER_FRAME_3200]; /* 160 */
uint8_t bits[CODEC2_VHF_BYTES_PER_FRAME];               /* 8   */
codec2_vhf_encode_3200(&enc, bits, speech_3200);
codec2_vhf_decode_3200(&dec, speech_3200, bits);

int16_t speech_1600[CODEC2_VHF_SAMPLES_PER_FRAME_1600]; /* 320 */
codec2_vhf_encode_1600(&enc, bits, speech_1600);
codec2_vhf_decode_1600(&dec, speech_1600, bits);

int16_t speech_1300[CODEC2_VHF_SAMPLES_PER_FRAME_1300]; /* 320 */
uint8_t bits1300[CODEC2_VHF_BYTES_PER_FRAME_1300];      /* 7   */
codec2_vhf_encode_1300(&enc, bits1300, speech_1300);
codec2_vhf_decode_1300(&dec, speech_1300, bits1300);
```

## Tools

`c2vhfenc` / `c2vhfdec` convert between raw 8 kHz mono s16le speech
and `.c2` files carrying the standard codec2 file header, so files
interoperate with `c2enc`, `c2dec`, `c2play`, etc.

```
./build/c2vhfenc 1600 testdata/hts1a.raw out.c2
./build/c2vhfdec out.c2 out.raw
```

## Build and test

```
cmake -B build && cmake --build build
cd build && ctest
```

`c2vhftest` runs an in-process full-chain test of both modes
(synthesis, encode, decode, voiced/silence energy checks).  A
bit-exactness check against stock `c2enc` (if installed):

```
./build/c2vhfenc 3200 testdata/hts1a.raw /tmp/a.c2
c2enc 3200 testdata/hts1a.raw /tmp/b.c2
cmp <(tail -c +8 /tmp/a.c2) <(tail -c +8 /tmp/b.c2) && echo bit-exact
```

## Performance

This codebase keeps all of Codec2-mod's optimisations: no runtime
`malloc`/`free`, fully deterministic fixed-size state, single-mode
linking, much smaller flash footprint and encoder stack than reference
Codec2.  Upstream Codec2-mod STM32F411 measurements for the 3200 mode
(git history of this file): 2.7x faster encode / 2.1x decode at 23.8
KiB flash vs 167.2 KiB reference; 19.6 KiB static encoder state,
16.2 KiB decoder state, ~1 KiB encode stack.  The 1600 mode adds two
extra 10 ms analyses and one scalar-LSP search per 40 ms frame and
stays comfortably real time on the same class of hardware.

## Benchmarks

Tooling: `tests/bench.c` (host timing, `cmake --build build --target bench`),
`tools/size_report.sh` + `tools/stack_calc.py` (static Cortex-M4 footprint,
same methodology as Codec2-mod: `-Os`, newlib-nano, `--gc-sections`,
`-fstack-usage` call-graph analysis).  Reference figures are a from-source
codec2 1.2 build with identical flags (`-O3 -ffp-contract=off`), heap size
measured with a malloc counter.

Host timing (x86-64, hts1a, 30 passes, avg per frame):

| Task  | ref 3200 | vhf 3200 | ref 1600 | vhf 1600 | ref 1300 | vhf 1300 |
|-------|----------|----------|----------|----------|----------|----------|
| encode| 34.6 us  | **16.9** | 74.4 us  | **33.0** | 61.9 us  | **29.7** |
| decode| 20.9 us  | 21.5     | 45.8 us  | **42.4** | 42.2 us  | 42.5     |

The encoder runs ~2x faster than reference in every mode (matching the
2.7x Codec2-mod measured on STM32); on the host the decoder advantage
of the mod mostly disappears.  Notably, per second of audio every mode
costs about the same to encode (~0.85 us/ms): the 10 ms analysis
pipeline dominates, and the longer frames of 1600/1300 exactly
compensate for their extra analyses.

RAM (static, caller-owned; identical for all three modes since the
analysis/synthesis state is shared):

| State               | codec2-vhf        | reference (heap, measured) |
|---------------------|-------------------|----------------------------|
| encoder-only        | 20,040 B          | 30,748 B (enc+dec in one)  |
| decoder-only        | 16,600 B          | (same instance)            |
| full duplex         | 36,640 B          | 30,748 B                   |

Reference is smaller for full duplex, but an encoder-only or
decoder-only build (modem separate from codec) saves 35-46 % RAM.
Extra modes cost no additional state.

Flash (Cortex-M4 -Os, newlib-nano, `--gc-sections`, GCC 16.2 - Codec2-mod
measured 23.8 KiB for 3200 on an older toolchain, so compare deltas,
not absolutes):

| Image      | text (flash) |
|------------|--------------|
| 3200 only  | 30,596 B     |
| 1600 only  | 30,980 B     |
| 1300 only  | 31,036 B     |
| all modes  | 33,588 B     |

A single additional mode costs about 0.4 KiB (it reuses the entire
analysis/synthesis pipeline); shipping all three costs about 2.9 KiB
over 3200-only (encode/decode frame logic plus the scalar-LSP
quantiser and codebooks used by 1600/1300).

Worst-case stack (deepest call path from `-fstack-usage` frames):

| Entry point          | stack |
|----------------------|-------|
| encode, any mode     | ~1.0 KiB |
| decode 3200          | 4.97 KiB |
| decode 1600          | 5.81 KiB |
| decode 1300          | 5.82 KiB |


The 1600/1300 decoders use ~0.85 KiB more stack than 3200 (4x10 ms
model/LSP/Aw interpolation arrays instead of 2x).

Cost of the interpolation-weight precision fix (float instead of the
truncating int): +24..56 B flash, 0 B stack, and +1.5 % decoder
instructions (~10k per 40 ms frame, ~83 us at 120 MHz, i.e. +0.2 %
CPU load) - measured by valgrind instruction counts on identical
workloads before/after.

STM32F4 projection at 100 MHz, on Codec2-mod's measurement basis
(their measured STM32F411 @ 100 MHz, -Os: 3200 enc 5.32/5.96 ms and
dec 7.14/9.26 ms avg/max per 20 ms frame; our per-mode costs scaled by
measured host-timing and instruction-count ratios vs 3200, decoder
figures including the +1.5 % weight-fix penalty):

| Mode | Frame  | enc avg/max | dec avg/max | full-duplex | worst frame |
|------|--------|-------------|-------------|-------------|-------------|
| 3200 | 20 ms  | 5.3 / 6.0 ms | 7.1 / 9.3 ms | 62 % | 76 % |
| 1600 | 40 ms  | 10.4 / 11.7 ms | 14.5 / 18.8 ms | 62 % | 76 % |
| 1300 | 40 ms  | 9.6 / 10.7 ms | 14.5 / 18.8 ms | 60 % | 74 % |

Per-second cost is flat across modes (the 10 ms analysis pipeline
dominates; instruction counts show ~330k Ir per 20 ms equivalent for
all three), so full-duplex load is mode-independent; 1300 comes out
slightly ahead because its encoder runs one fewer LPC pass.  At
120 MHz scale by 100/120 (e.g. ~52 % / ~50 %).  Worst-case frames
keep ~1.3x real-time margin at 100 MHz.  These are projections from
measurement ratios, not hardware runs.

## Important notice: derivative work

**This is a derivative work.**

This code is based heavily and directly on the Codec2 speech codec by
David Rowe, VK5DGR et al.  Large portions of the code, algorithms,
constants, and overall design originate from Codec2 and remain
recognizably derived from it.  All original credit for the Codec2
design, algorithms, and implementation belongs to David Rowe and the
Codec2 contributors; the minimal optimised 3200 extraction this
project builds on comes from the M17-Project's Codec2-mod.  The 1600
bps mode is a bit-exact port of the reference Codec2 scalar LSP
quantiser and 40 ms framing onto the Codec2-mod structure.

## License

This project inherits the licensing requirements of Codec2 (LGPL 2.1).
Please refer to the LICENSE file and original Codec2 license for
details and ensure compliance when using or redistributing this code.
