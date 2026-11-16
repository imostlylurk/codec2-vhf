#ifndef CODEC2_VHF_FFT_FIXED_H
#define CODEC2_VHF_FFT_FIXED_H

/*
  Fixed-point radix-2 FFT for codec2-vhf.

  Replaces the float KISS FFT.  Conventions match what the codec's DSP
  was written against (unscaled transforms, like kiss_fftr/kiss_fftri):

    forward:  freq[k] = sum_n time[n] * e^{-j 2 pi k n / N}
    inverse:  time[n] = sum_k freq[k] * e^{+j 2 pi k n / N}
              (freq interpreted with Hermitian extension)

  Data is int32_t in a caller-chosen Q format.  Overflow safety: for
  the unscaled forward transform every intermediate value is bounded by
  sum(|time|), and for the inverse by 4 * sum(|freq|); the callers keep
  those sums below 2^31 (documented per call site in
  codec2_internal.h).

  All twiddles are compile-time Q31 constants; no sin/cos at runtime.
*/

#include <stdint.h>

/* may_alias: callers pass overlapping int32/complex scratch regions
   (e.g. an FFT whose output overwrites its dead input); without it the
   compiler may reorder accesses across the pack/unpack based on the
   assumption that int32_t and the struct never alias. */
typedef struct __attribute__((may_alias))
{
    int32_t r;
    int32_t i;
} cpx_q_t;

_Static_assert(sizeof(cpx_q_t) == 2 * sizeof(int32_t),
               "cpx_q_t must pack exactly two int32 for the FFT packing");

/* complex FFT size (512-point real transforms use 256 complex points) */
#define FFT_FIXED_N 256

/*
  512-point real forward FFT.
  time: 512 int32_t input
  freq: 257 complex output bins 0..256
  scratch: 256 complex elements of working space (need not be aligned)
  freq and scratch must not overlap; time MAY alias freq (the input is
  fully consumed before any output is written).
*/
void fft_real_fwd512(const int32_t *time, cpx_q_t *freq, cpx_q_t *scratch);

/*
  512-point real inverse FFT (unscaled Hermitian inverse).
  freq: 257 complex input bins 0..256 (bins 257..511 taken as the
        Hermitian extension)
  time: 512 int32_t output
  scratch: 256 complex elements of working space
  time MAY alias freq (freq is fully consumed before output is written).
*/
void fft_real_inv512(const cpx_q_t *freq, int32_t *time, cpx_q_t *scratch);

#endif /* CODEC2_VHF_FFT_FIXED_H */
