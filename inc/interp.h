#ifndef CODEC2_VHF_INTERP_H
#define CODEC2_VHF_INTERP_H

#include "codec2_internal.h"

void interp_Wo(model_t *interp, const model_t *prev, const model_t *next);
float interp_energy(float prev_e, float next_e);
void interpolate_lsp(float *interp, const float *prev, const float *next);

/* 1600 bps mode: weighted (non 0.5) interpolation, reference port */
void interpolate_lsp_ver2(float *interp, const float *prev,
                          const float *next, float weight);

/* 1300 bps mode: weighted Wo and energy interpolation, reference port */
void interp_Wo2(model_t *interp, const model_t *prev, const model_t *next,
                float weight);
float interp_energy2(float prev_e, float next_e, float weight);

#endif /* CODEC2_VHF_INTERP_H */
