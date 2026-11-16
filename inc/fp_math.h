#ifndef CODEC2_VHF_FP_MATH_H
#define CODEC2_VHF_FP_MATH_H

/*
  Fixed-point math helpers for codec2-vhf.

  The library contains no floating point arithmetic at all: every
  quantity is carried in integer Q-format fixed point.  Conventions
  used across the codec:

    angles (radians)      Q24 int32_t
    oscillator phase      uint32_t "turns": 2^32 == 2*pi, wraps freely
    unit phasors          Q31 complex pairs
    spectrum/speech       Q16 (see codec2_internal.h)

  Accuracy: the sine/cosine path is a 1024-entry quarter-wave table
  with linear interpolation (~3e-7 max error, about -130 dB); log2/exp2
  use 64-entry fraction tables (~2e-5 relative error); acos() re-uses
  the same polynomial approximation as the float implementation.
*/

#include <stdint.h>

#define PI_Q24 52707179  /* pi * 2^24 */

/* oscillator phase increment per radian: 2^32 / (2*pi) */
#define PHASE_PER_RAD_Q24 683565276

/* sine/cosine of a 32-bit phase (2^32 == 2*pi) */
int32_t fp_sin_q31(uint32_t phase);
void fp_sincos_q31(uint32_t phase, int32_t *s, int32_t *c);

/* floor(sqrt(v)) for unsigned 64-bit v */
uint32_t fp_isqrt64(uint64_t v);

/* log2(v) in Q16 for integer v >= 1 */
int32_t fp_log2_q16(int64_t v);

/* 2^(x/2^16) in Q31 as int64; x is Q16.
   Valid for -32*65536 <= x <= 30*65536 (result fits in int64) */
int64_t fp_exp2_q31(int32_t x);

/* saturating cast to int32 */
static inline int32_t fp_sat32(int64_t v)
{
    if (v > INT32_MAX)
        return INT32_MAX;
    if (v < INT32_MIN)
        return INT32_MIN;
    return (int32_t)v;
}

/* saturating add/sub for int32 */
static inline int32_t fp_sat_add32(int32_t a, int32_t b)
{
    return fp_sat32((int64_t)a + b);
}

#endif /* CODEC2_VHF_FP_MATH_H */
