#ifndef CODEC2_VHF_QUANTISE_H
#define CODEC2_VHF_QUANTISE_H

#include <stdint.h>

#include "codec2_internal.h"

#define LSP_SCALAR_INDEXES 10

void pack(uint8_t *bitArray, unsigned int *bitIndex,
          unsigned int field, unsigned int fieldWidth);

int unpack(const uint8_t *bitArray, unsigned int *bitIndex,
           unsigned int fieldWidth);

int encode_Wo(float Wo, uint8_t bits);
float decode_Wo(int index, int bits);
int encode_energy(float e, int bits);
float decode_energy(int index, int bits);
void encode_lspds_scalar(int *indexes, const float *lsp);
void decode_lspds_scalar(float *lsp_, const int *indexes);

/* 1600 bps mode: scalar LSP (not LSP-difference) quantiser, from the
 * reference Codec2 (bit-exact port) */
int lsp_bits(int i);
void encode_lsps_scalar(int *indexes, const float *lsp);
void decode_lsps_scalar(float *lsp_, const int *indexes);
void check_lsp_order(float *lsp);
void bw_expand_lsps(float *lsp, float min_sep_low, float min_sep_high);

typedef struct
{
    int k;
    int m;
    int log2m;
    const uint16_t *cb;
} lsp_scalar_cb_t;

extern const lsp_scalar_cb_t lsp_scalar_cb[LSP_SCALAR_INDEXES];

#endif /* CODEC2_VHF_QUANTISE_H */
