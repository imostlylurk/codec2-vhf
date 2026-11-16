#ifndef CODEC2_VHF_LPC_H
#define CODEC2_VHF_LPC_H

#include "codec2_internal.h"

void aks_to_mag2(
    codec2_decoder_t *c2,
    const int32_t *ak,       /* LPCs, Q24 */
    model_t *model,
    int32_t Elog);           /* frame energy, log2 Q16 */
/* Leaves the A(z) spectrum (257 bins, Q24) in c2->fft_buffer[0..256]
   for synthesise_one_frame(). */

void speech_to_uq_lsps(
    codec2_encoder_t *c2,
    int32_t *lsp,            /* out: unquantised LSPs, radians Q24 */
    int32_t *ak,             /* out: LPCs, Q24 */
    int32_t *energy,         /* out: Elog, log2 Q16 */
    const int16_t *Sn,       /* windowed-input speech history, Q0 */
    const int32_t *w);       /* analysis window, Q22 */

void lsp_to_lpc(
    const int32_t *lsp,      /* LSP frequencies, radians Q24 */
    int32_t *ak);            /* out: LPCs, Q24 */

void apply_lpc_correction(model_t *model);

#endif /* CODEC2_VHF_LPC_H */
