#ifndef CODEC2_VHF_ANALYSIS_H
#define CODEC2_VHF_ANALYSIS_H

#include "codec2_internal.h"

void analyse_one_frame(codec2_encoder_t *c2, model_t *model, const int16_t *speech);
void analysis_init(codec2_encoder_t *c2);

#endif /* CODEC2_VHF_ANALYSIS_H */
