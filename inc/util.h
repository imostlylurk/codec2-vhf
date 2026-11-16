#ifndef CODEC2_VHF_UTIL_H
#define CODEC2_VHF_UTIL_H

#include <stdint.h>

/* acos(x) in radians Q24; x is Q31 in [-1, 1].  Same polynomial shape
   as the float implementation's fast_acosf(), which lived in this file
   in the float tree. */
int32_t fp_acos_q24(int32_t x);

/* the codec's deterministic PRNG (bit-identical to the float version) */
static inline int codec2_rand(uint32_t *prng_state)
{
	*prng_state = *prng_state * 1103515245U + 12345U;
	return ((unsigned)(*prng_state >> 16) & 0x7FFF);
}

#endif /* CODEC2_VHF_UTIL_H */
