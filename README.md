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

The 1600 bps mode was added in this fork using the same optimised
structure as the 3200 mode: static, fixed-size state (no heap), split
encoder/decoder states, KISS FFT, and the same 10 ms internal analysis
pipeline.  The 1600 encoder packs 4 voicing bits (10 ms update), two
Wo + energy pairs (20 ms update) and one scalar-LSP vector (40 ms
update); the decoder interpolates the three in-between 10 ms frames.

Encoder bit-exactness for both modes has been verified byte-for-byte
against `c2enc` from codec2 1.2 on `hts1a` (150 x 20 ms frames of 3200,
75 x 40 ms frames of 1600).
Decoded audio is not required to be sample-identical to the reference
implementation; waveform correlation with `c2dec` output of the same
bitstream is 0.9998 (3200) and 0.88 waveform / 0.98 short-time envelope
(1600).  The residual differences come from the restructured decoder
DSP and float rounding in the interpolation path - the same class of
difference the upstream Codec2-mod already documents for 3200.

Like upstream, bit-exactness of the encoded bitstream depends on the
accuracy of the C library's `cosf()` during analysis (glibc: identical;
newlib-nano on Cortex-M4: ~1 frame in 150 may differ).

## Goals / roadmap

A single, small, embedded-friendly vocoder suite for VHF digital voice:

- [x] M17 full rate (3200)
- [x] M17 half rate (1600)
- [ ] Codec2 1300 (later)
- [ ] SLATE (KV4P-HT / host-side modem projects)
- [ ] FreeDV 2400A (5 kHz 4FSK) and 2400B (2-tone FSK for FM channels)

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
