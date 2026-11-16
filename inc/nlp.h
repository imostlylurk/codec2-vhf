#ifndef CODEC2_VHF_NLP_H
#define CODEC2_VHF_NLP_H

#include "codec2_internal.h"

void nlp(
    nlp_t *nlp,
    const int16_t *Sn,       /* input speech vector, Q0 */
    int32_t *f0,             /* out: pitch frequency, Hz Q16 */
    int32_t *prev_f0,        /* previous pitch f0, Hz Q16 (memory) */
    complex_t *fft_scratch); /* caller-owned scratch, 256 complex slots */

void nlp_init(nlp_t *nlp);

#endif /* CODEC2_VHF_NLP_H */
