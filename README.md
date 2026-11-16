# codec2-vhf (fixed-point)

Fixed-point Codec2 vocoder modes for VHF digital voice, derived from
[Codec2-mod](https://github.com/M17-Project/Codec2-mod) (M17-Project) and
[Codec2](https://github.com/drowe67/codec2) by David Rowe VK5DGR et al.
LGPL 2.1 - see LICENSE.

**This build contains no floating-point arithmetic.** Every DSP stage
runs in integer Q-format fixed point; the library compiles with zero
floating-point instructions (verified by disassembly scan of the
Cortex-M4 build), making it suitable for MCUs without an FPU, such as
the HR-C7000 (CSKY) targeted by future OpenRTX radios - where the
float codec cannot run at all without soft-float emulation.

## Modes

| Mode | Frame  | Bits | Bitstream format                   |
|------|--------|------|-------------------------------------|
| 3200 | 20 ms  | 64   | identical to reference Codec2      |
| 1600 | 40 ms  | 64   | identical to reference Codec2      |
| 1300 | 40 ms  | 52   | identical to reference Codec2      |

The bit-packing, Gray coding, codebooks and frame layouts are unchanged
from the reference, so `.c2` files interoperate with stock
`c2enc`/`c2dec`/`c2play` in both directions.

## What changed relative to Codec2-mod

- **Full fixed-point conversion in this branch.** All DSP stages
  (FFT, LPC/Levinson, LSP root search, NLP pitch estimator, MBE voicing,
  quantisers, synthesis) were rewritten from `float` to integer Q-format
  arithmetic.  The float KISS FFT was replaced by a purpose-built
  fixed-point radix-2 real FFT (`fft_fixed.c`) with compile-time Q31
  twiddles.  `fp_math.c` provides integer sin/cos (1024-entry
  quarter-wave table + lerp, ~3e-7 error), sqrt, log2, exp2; `util.c`
  keeps the acos polynomial counterpart of the float tree's
  `fast_acosf()`.
- **1600 and 1300 bps modes** carried over from the float tree, fixed
  point too.
- **API unchanged**: `codec2_vhf_*` entry points, static caller-owned
  states, no heap.
- **Fixed-point specifics**:
  - speech/FFT domain Q16, analysis window Q28, model amplitudes Q4,
    Wo/angles Q24, LPCs Q24, phases as uint32 turns (2^32 = 2*pi),
    energies in the log2 Q16 domain, post-filter normalization gain
    Q22 (a Q6 gain quantized quiet-frame harmonics audibly; see the
    Q-format notes below).
  - The energy quantiser, LSP-delta quantiser and interpolation work
    in the log2 domain, so no powf/log10f calls are needed anywhere.
  - The NLP pitch estimator FFT input uses per-frame block floating
    point (all decisions below are ratios, so the scale cancels).
  - The LSP root search walks the Chebyshev interval with
    round-to-nearest-even emulation of the float32 grid, keeping the
    encoder's quantiser decisions aligned with the float reference.
- **Build and tooling**: CMake build, `c2vhfenc`/`c2vhfdec` file tools,
  in-process test suite, host benchmark, Cortex-M4 footprint analysis
  (`tools/size_report.sh` + `tools/stack_calc.py`).

## Compatibility and deviations

The fixed-point encoder produces a *different but interoperable*
bitstream: frame format, codebooks and Gray code are identical, so any
reference decoder decodes fixed-point-encoded audio correctly and vice
versa.  Measured on 28 s of speech (LibriVox) against stock codec2
(`c2enc`, bit-exact with the float codec2-vhf encoder per its README),
on fresh encodes from this tree:

- 33.7% / 46.5% / 35.5% of frames differ (3200/1600/1300); mean
  Hamming distance 2.5-3.1 bits per differing frame, max 8-11 bits.
- Per-parameter deviation (3200): voicing 6-7%, Wo quantiser index
  23% (typically +-1 of 128 levels; the harmonic-sum pitch refinement
  grid is coarser than the Wo quantiser step, so borderline frames
  flip one index), energy 0.1%, LSP indexes 0.1-2.1%.
- Decoder, identical bits, fixed decode vs float decode: SNR
  -0.21 / -0.25 / -0.23 dB, waveform correlation 0.97+, level ratio
  0.99.  The result is level-matched and free of dropouts; the
  100 Hz frame-rate modulation ("buzz") fingerprint measures at parity
  with the float decoder.
- End-to-end log-spectral distance between float-decode(fixed-bits)
  and the all-float chain: 7.1-7.3 dB median - well inside Codec2's
  own distortion (float decode vs clean source measures ~11.5 dB with
  the same method).

The remaining bitstream deviation is bounded by the float reference's
own float32 accumulation noise; eliminating it would require emulating
float32 semantics exactly, which defeats the purpose of a fixed-point
build.  Whole-file waveform correlation across *different* bitstreams
is near zero by construction (independent phase tracks); use the
per-frame metrics above. Expect slightly inferior quality from the
fixed-point encoder relative to float. Other radios will hear you fine;
aliasing may become audible on very loud, near-clipping audio, but
ordinary speech is generally fine.

## API

State is owned by the caller and can be allocated statically.  No
create/destroy, no heap.  The encoder and decoder states are separate
objects; a full-duplex radio (concurrent RX+TX codec threads) holds
both (21,360 B of .bss total).

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
interoperate with `c2enc`, `c2dec`, `c2play`, etc.  Note: `c2vhfenc`
pads a final partial input frame with silence (stock `c2enc` drops
it), so 1300/1600-mode files on non-multiple inputs run 20 ms longer.

```
./build/c2vhfenc 1600 testdata/hts1a.raw out.c2
./build/c2vhfdec out.c2 out.raw
```

## Build and test

```
cmake -B build && cmake --build build
cd build && ctest
```

`c2vhftest` runs an in-process full-chain test of all modes
(synthetic signal, encode, decode, voiced/silence energy checks).
The optional `-DC2VHF_DUMP_HOOKS=ON` build prints per-subframe model
parameters under `C2VHF_DUMP=1` for debugging.

## Benchmarks

Three measurements, most-to-least authoritative.  The float baseline
everywhere is this project's own float `main` branch; stock drowe67
codec2 was used where a bit-exact reference encoder/decoder was needed.

**Exact Cortex-M4 instruction counts** (xPack GCC 15.2.1,
`-Os -mcpu=cortex-m4 -mthumb`, hard-FPU `fpv4-sp-d16` for the float
build, newlib-nano; bare-metal harnesses executing real hts1a frames
under exact instruction-counting emulation - every instruction,
including newlib libm/libgcc, counted):

| Task  | fixed instr/frame | float instr/frame | ratio |
|-------|-------------------|-------------------|-------|
| encode 3200 | 457,269   | 350,701  | 1.30x |
| decode 3200 | 548,772   | 393,695  | 1.39x |
| encode 1600 | 909,974   | 650,480  | 1.40x |
| decode 1600 | 1,131,803 | 795,705  | 1.42x |
| encode 1300 | 829,628   | 620,952  | 1.34x |
| decode 1300 | 1,110,371 | 791,065  | 1.40x |

For real-time 8 kHz audio that is 22-28 Mips encode / 27-28 Mips
decode on the fixed build vs 15-16 / 20 Mips float: both fit a
100 MHz Cortex-M4 with margin, and the fixed build stays within +50%
of the float build in every task.  (The float build spends ~22% of
its decode instructions in double-precision soft routines --
`__divdf3`/`__aeabi_dadd` - from the `double` intermediates the
float tree deliberately retains.)

**Host timing** (x86-64, both `-O3`, same input, interleaved runs):
encode 28.6/56.7/51.6 us and decode 36.6/72.9/~74 us per frame
(3200/1600/1300) vs float 23.2/45.7/41.1 and 29.7/59.3/~59 us
(1.23-1.30x).  Host numbers flatter the fixed build - x86 executes
64-bit multiplies in one cycle - so the M4 table above is the one to
quote.

**Memory, in OpenRTX integration terms** (verified against OpenRTX's
`CODEC2_THREAD_STKSIZE` = 16,384 B codec thread, which by OpenRTX's
own design never carries the codec state - stock codec2 heap-allocates
it, and the 5 KB mic buffer is heap'd with "the stack isn't big
enough"):

- Codec states are caller-owned statics (.bss): 13,856 B encoder,
  7,504 B decoder, 21,360 B full duplex (vs 20,040 / 16,600 / 36,640
  for the float tree).
- The codec call chain inside the 16 KiB codec-thread stack measures
  at most 1,728 B (encode) / 1,632 B (decode) worst-case by
  `-fstack-usage` call-graph analysis of the Cortex-M4 build --
  ~14 KiB of headroom.
- Flash (text, -Os, newlib-nano, --gc-sections): 19.2/19.5/19.3 KiB
  per-mode images, 21.2 KiB all-modes - 35% smaller than the float
  tree (29.8-32.8 KiB).

The 1600/1300 decoders synthesise their four 10 ms subframes on the
fly, holding one working `model_t` instead of four, and the large
per-subframe buffers (the `Aw` spectrum from `aks_to_mag2`, the `H`/`P`
harmonic phase arrays, the IFFT scratch) live at disjoint lifetimes
inside the shared decoder `fft_buffer` instead of on the stack.  The
aks_to_mag2 per-bin loop avoids int64 division entirely by deriving
the inverse-power spectrum from already-computed log2 values.  This
targets the HR-C7000 CSKY MCU where int64 division is a 30-60 cycle
software routine.

## Fixed-point implementation notes

- `inc/fp_math.h` / `src/fp_math.c`: Q-format primitives.  Sine table
  is compile-time; log2/exp2 use 64-entry fraction tables; sqrt is a
  bitwise integer Newton-Raphson seeded via CLZ; `util.c` carries the
  acos polynomial from the float implementation.
- `src/fft_fixed.c`: radix-2 DIT complex FFT (256 pt) plus real
  512-point forward/inverse wrappers matching the unscaled
  kiss_fftr/kiss_fftri conventions the codec math assumes.  The
  butterfly is instantiated per twiddle-sign so the sign folds at
  compile time (a runtime select was worth ~14% of total decode
  instructions on M4).  Overflow safety is documented per call site
  (sum|x| < 2^31 bounds); the amplitude clamp `A_CLAMP_Q4` guards the
  inverse FFT in synthesis.
- Q-format overflow bounds for every FFT call are in
  `inc/codec2_internal.h`.
- Q-format lessons encoded in the code (each was a real defect):
  - a ratio of two values carrying the same Q scale truncates to 0 or
    1: the ear-protection limiter had this bug and *zeroed* every
    subframe peaking above 30000 units (an audible pop/dropout);
  - integer gains need enough fractional quanta: the post-filter
    normalization gain in Q6 resolved a true gain of ~0.03 to 0-2
    quanta and gated quiet-frame harmonics on and off (audible
    roughness); it is Q22 now;
  - unit phasors must scale by exactly 2^31 end-to-end: one extra
    shift saturates every phasor to the int32 corners (sqrt(2)
    magnitude, four-level phase, which sounds like a loud buzz).
- The float reference's LSP search interval walk is emulated with
  round-to-nearest-even on the float32 grid (`f32_round_q30`), keeping
  quantiser decisions aligned; the remaining deviations stem from
  float32 accumulation noise upstream in the reference.
- The encoder analysis scratch is shared by design (speech FFT
  scratch, NLP FFT scratch, and the Hermitian mirror of Sw all alias
  the same tail of `fft_buffer`); the NLP therefore runs *before*
  `dft_speech()` so the mirror survives for the pitch refinement's
  deliberate past-Nyquist reads.

## Important notice: derivative work

**This is a derivative work.**

This code is based heavily and directly on the Codec2 speech codec by
David Rowe, VK5DGR et al.  Large portions of the code, algorithms,
constants, and overall design originate from Codec2 and remain
recognizably derived from it.  All original credit for the Codec2
design, algorithms, and implementation belongs to David Rowe and the
Codec2 contributors; the minimal optimised 3200 extraction this
project builds on comes from the M17-Project's Codec2-mod.  The 1600
and 1300 bps modes are ports of the reference Codec2 framing and
scalar-LSP quantiser onto the Codec2-mod structure.

## License

This project inherits the licensing requirements of Codec2 (LGPL 2.1).
Please refer to the LICENSE file and original Codec2 license for
details and ensure compliance when using or redistributing this code.
