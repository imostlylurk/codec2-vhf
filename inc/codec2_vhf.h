#ifndef CODEC2_VHF_H
#define CODEC2_VHF_H

/*
  codec2-vhf: optimised Codec2 modes for VHF digital voice.

  Derived from Codec2-mod (M17-Project) and David Rowe's Codec2.
  LGPL 2.1 - see LICENSE.

  Modes:
    3200  20 ms frames, 64 bits - bit-exact with reference Codec2 3200
    1600  40 ms frames, 64 bits - bit-exact with reference Codec2 1600
*/

#include "codec2_internal.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct codec2_t codec2_t;
    typedef struct codec2_encoder_t codec2_encoder_t;
    typedef struct codec2_decoder_t codec2_decoder_t;

/* public frame sizes */
#define CODEC2_VHF_BYTES_PER_FRAME BYTES_PER_FRAME        /* 8: 3200/1600 */
#define CODEC2_VHF_BYTES_PER_FRAME_1300 7                 /* 52 bits */
#define CODEC2_VHF_SAMPLES_PER_FRAME_3200 (2 * N_SAMP)  /* 160, 20 ms */
#define CODEC2_VHF_SAMPLES_PER_FRAME_1600 (4 * N_SAMP)  /* 320, 40 ms */
#define CODEC2_VHF_SAMPLES_PER_FRAME_1300 (4 * N_SAMP)  /* 320, 40 ms */

/* full codec (encoder + decoder in one state) */
    void codec2_vhf_init(codec2_t *c2);

/* encoder-only / decoder-only states (static allocation, no heap) */
    void codec2_vhf_encoder_init(codec2_encoder_t *c2);
    void codec2_vhf_decoder_init(codec2_decoder_t *c2);

/* 3200 bps: 160 samples in/out, 8 byte frames */
    void codec2_vhf_encode_3200(
        codec2_encoder_t *c2,
        uint8_t *bits,
        const int16_t *speech);

    void codec2_vhf_decode_3200(
        codec2_decoder_t *c2,
        int16_t *speech,
        const uint8_t *bits);

/* 1600 bps: 320 samples in/out, 8 byte frames */
    void codec2_vhf_encode_1600(
        codec2_encoder_t *c2,
        uint8_t *bits,
        const int16_t *speech);

    void codec2_vhf_decode_1600(
        codec2_decoder_t *c2,
        int16_t *speech,
        const uint8_t *bits);

/* 1300 bps: 320 samples in/out, 7 byte frames (52 bits) */
    void codec2_vhf_encode_1300(
        codec2_encoder_t *c2,
        uint8_t *bits,
        const int16_t *speech);

    void codec2_vhf_decode_1300(
        codec2_decoder_t *c2,
        int16_t *speech,
        const uint8_t *bits);

#ifdef __cplusplus
}
#endif

#endif /* CODEC2_VHF_H */
