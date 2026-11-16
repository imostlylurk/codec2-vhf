#ifndef CODEC2_VHF_INTERP_H
#define CODEC2_VHF_INTERP_H

#include "codec2_internal.h"

void interp_Wo(model_t *interp, const model_t *prev, const model_t *next);
int32_t interp_energy(int32_t prev_e, int32_t next_e);  /* log2 Q16 */
void interpolate_lsp(int32_t *interp, const int32_t *prev, const int32_t *next);

/* 1600 bps mode: weighted (non 0.5) interpolation */
void interpolate_lsp_ver2(int32_t *interp, const int32_t *prev,
                          const int32_t *next, int32_t weight /* Q16 */);

/* 1300 bps mode: weighted Wo and energy interpolation */
void interp_Wo2(model_t *interp, const model_t *prev, const model_t *next,
                int32_t weight /* Q16 */);
int32_t interp_energy2(int32_t prev_e, int32_t next_e, int32_t weight);

#endif /* CODEC2_VHF_INTERP_H */
