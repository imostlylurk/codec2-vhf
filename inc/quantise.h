#ifndef CODEC2_VHF_QUANTISE_H
#define CODEC2_VHF_QUANTISE_H

#include <stdint.h>

#include "codec2_internal.h"

#define LSP_SCALAR_INDEXES 10

void pack(uint8_t *bitArray, unsigned int *bitIndex,
          unsigned int field, unsigned int fieldWidth);

int unpack(const uint8_t *bitArray, unsigned int *bitIndex,
           unsigned int fieldWidth);

int encode_Wo(int32_t Wo, uint8_t bits);       /* Wo radians Q24 */
int32_t decode_Wo(int index, int bits);        /* radians Q24 */
int encode_energy(int32_t Elog, int bits);     /* Elog log2 Q16 */
int32_t decode_energy(int index, int bits);    /* log2 Q16 */
void encode_lspds_scalar(int *indexes, const int32_t *lsp);
void decode_lspds_scalar(int32_t *lsp_, const int *indexes);

/* 1600 bps mode: scalar LSP (not LSP-difference) quantiser */
int lsp_bits(int i);
void encode_lsps_scalar(int *indexes, const int32_t *lsp);
void decode_lsps_scalar(int32_t *lsp_, const int *indexes);
void check_lsp_order(int32_t *lsp);
void bw_expand_lsps(int32_t *lsp, int min_sep_low, int min_sep_high);

typedef struct
{
    int k;
    int m;
    int log2m;
    const uint16_t *cb;
} lsp_scalar_cb_t;

extern const lsp_scalar_cb_t lsp_scalar_cb[LSP_SCALAR_INDEXES];

#endif /* CODEC2_VHF_QUANTISE_H */
