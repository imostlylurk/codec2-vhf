#ifndef CODEC2_VHF_INTERNAL_H
#define CODEC2_VHF_INTERNAL_H

/*
  This work uses code written by David Rowe VK5DGR et al.
  https://github.com/drowe67/codec2

  Fixed-point internal state.  The library performs no floating point
  arithmetic; every signal and parameter lives in an integer Q format:

    speech samples        int16_t, Q0
    analysis window w[]   Q28
    windowed speech / FFT Q16 (Snw = Sn * w, max |Snw| ~ 142 for full
                          scale speech; the unscaled 512-point FFT is
                          safe because sum|Snw| <= 0.6007*32767*2^16
                          < 2^31 for any input, worst case DC)
    analysis window w[]   Q22
    model amplitudes A[]  Q4 (speech units * 16), clamped to 2^20
    Wo / angles           Q24 radians (see fp_math.h for phases)
    LPC coefficients      Q24
    energies              log2 domain, Q16 (Elog)
    decoded energy        Q6 linear, derived from Elog once per frame
    synthesised overlap   Q8
    phases                uint32_t, 2^32 == 2*pi
*/

#include <stdint.h>
#include <string.h>

#include "fp_math.h"
#include "fft_fixed.h"

#define BYTES_PER_FRAME 8
#define SAMPLES_PER_FRAME (2 * N_SAMP)

#define MAX_AMP 80

#define FFT_ENC 512
#define FFT_DEC 512
#define LPC_ORD 10


#define SAMP_RATE 8000
#define N_SAMP 80
#define M_PITCH 320
#define P_MIN 20
#define P_MAX 160
#define NW 279
#define TW 40

#define PMAX_M 320
#define PE_FFT_SIZE 512
#define DEC 5
#define NDEC (M_PITCH / DEC)
#define NLP_NTAP 48
#define NLP_NPHASE DEC
#define NLP_TAPS_PP ((NLP_NTAP + DEC - 1) / DEC)

#define WO_BITS 7
#define WO_LEVELS (1 << WO_BITS)
#define E_BITS 5
#define E_LEVELS (1 << E_BITS)
#define LSPD_SCALAR_INDEXES 10

#define CODEC2_RAND_MAX 32767

/* asserts: this is the exact Codec2 3200 bps configuration */
_Static_assert(BYTES_PER_FRAME == 8, "Codec2 3200 only");
_Static_assert(LPC_ORD == 10, "Codec2 3200 assumes LPC_ORD == 10");
_Static_assert(MAX_AMP >= 80, "MAX_AMP must be >= 80 for Codec2 3200");
_Static_assert(FFT_ENC == 512, "Codec2 3200 assumes FFT_ENC == 512");
_Static_assert(FFT_DEC == FFT_ENC, "Encoder/decoder FFT sizes must match");
_Static_assert(PE_FFT_SIZE == 512, "Pitch estimator FFT size must be 512");
_Static_assert((M_PITCH % DEC) == 0, "M_PITCH must be divisible by DEC");
_Static_assert(NDEC == (M_PITCH / DEC), "NDEC must equal M_PITCH / DEC");
_Static_assert(NLP_NTAP == 48, "NLP FIR unrolling assumes NLP_NTAP == 48");
_Static_assert(WO_BITS <= 8, "encode_Wo() assumes <= 8-bit fields");
_Static_assert(E_BITS <= 8, "encode_energy() assumes <= 8-bit fields");
_Static_assert(TW <= N_SAMP / 2, "Parzen window ramp too long");

/* ------------------------------------------------------------------ */
/* Fixed-point constants                                              */
/* ------------------------------------------------------------------ */

/* angles, Q24 radians */
#define TWO_PI_Q24 105414357             /* 2*pi */
#define W0_MIN_Q24 658840                /* 2*pi/160 */
#define W0_MAX_Q24 5270718               /* 2*pi/20 */
#define WO_RANGE_Q24 (W0_MAX_Q24 - W0_MIN_Q24)
#define WO_60HZ_Q24 790608               /* 60 * 2*pi / 8000 */
#define LPC_150HZ_Q24 1976519            /* pi * 150 / 4000 */
#define WO_GUARD_Q24 50071820            /* 0.95 * pi */
#define LSP_INIT_Q24 4791562             /* pi / (LPC_ORD + 1) */
#define LSP_ORDER_SWAP_Q24 1677722       /* 0.1 rad */

/* FFT bin mapping: Wo (Q24) -> harmonic centre bin (Q20) */
#define FFT_BINS_PER_RAD_Q24 1367130551  /* 512 / (2*pi) * 2^24 */

/* voicing decision thresholds */
#define V_THRESH_LIN_Q20 4174456         /* 10^0.6 (V_THRESH = 6 dB) */
#define UV_LOW_Q20 104858                /* 0.1 */
#define UV_4DB_Q20 417446                /* 10^-0.4 */

/* NLP */
#define COEFF_Q15 31130                  /* notch filter coefficient 0.95 */
#define CNLP_Q20 314573                  /* 0.3 */

/* energy: log2 domain, Q16 */
#define LOG2_TO_DB10_Q16 197283          /* 10*log10(2) */
#define DB10_TO_LOG2_Q16 21771           /* 1 / (10*log10(2)) */
#define E_MIN_DB_Q16 (-655360)           /* -10 dB */
#define E_DB_STEP_Q16 102400             /* 50 dB / 32 levels = 1.5625 dB */
#define E_ZERO_FLOOR_LOG2_Q16 (-2612471) /* log2(1e-12) */
#define BG_THRESH_DB_Q16 2621440         /* 40 dB */
#define BG_MARGIN_DB_Q16 393216          /* 6 dB */
#define DB20_TO_LOG2_Q16 10885           /* log2(10)/20, for 10^(x/20) */

/* LPC post filter */
#define LPCPF_GAMMA_Q30 536870912        /* 0.5 */
#define LPCPF_TWO_BETA_Q16 26214         /* 0.4 */
#define PWR_A2G_Q16 13107                /* 0.2 (log2 weighting of A2g) */
#define PWR_A2_Q16 78643                 /* 1.2 (log2 weighting of A2) */
#define A2_FLOOR_Q48 281474977           /* 1e-6, Q48 (A2 is |FFT(ak)|^2) */
#define BOOST_1000_Q8 502                /* 1.96 low-frequency boost */
#define A_CLAMP_Q4 (1 << 20)             /* max model amplitude (unused in
                                            practice; guards the inverse FFT) */

/* decoder */
#define PHASE_PER_RAND 131076            /* 2^32 / CODEC2_RAND_MAX */

/* LSP root finding */
#define LSP_DELTA1_Q30 10737418          /* 0.01 */

/* consts */
extern const int32_t bw_gamma_q30[LPC_ORD + 1];
extern const int32_t nlp_fir_poly_q20[NLP_NPHASE * NLP_TAPS_PP];
extern const uint16_t delta_lsp_cb[LPC_ORD][32];

typedef cpx_q_t complex_t;

/* L = floor(pi / Wo); wo is radians Q24 */
static inline int wo_to_L(int32_t wo)
{
    return (int)(((int64_t)PI_Q24 << 24) / wo >> 24);
}

typedef struct model_t
{
    int32_t Wo;               /* radians Q24 */
    int L;
    int32_t A[MAX_AMP + 1];   /* harmonic amplitudes, speech units Q4 */
    int voiced;
} model_t;

typedef struct nlp_t
{
    int16_t w[NDEC];          /* decimation window, Q15 */
    int32_t sq[PMAX_M];       /* squared speech, Q0 */
    int32_t sq_fir[NDEC];     /* decimated FIR output, Q0 */
    int32_t mem_x, mem_y;     /* notch filter memory, Q0 */
    int32_t fftr_buff[PE_FFT_SIZE]; /* FFT input, NLP pre-scaled */
    int64_t Fw[PE_FFT_SIZE / 2 + 1]; /* power spectrum */
} nlp_t;

typedef struct codec2_encoder_t
{
    int16_t Sn[M_PITCH];      /* input speech history, Q0 */
    int32_t w[M_PITCH];       /* analysis window, Q28 */
    int32_t W[FFT_ENC];       /* window spectrum, Q16 */

    nlp_t nlp;

    int32_t prev_f0_enc;      /* previous pitch estimate, Hz Q16 */

    complex_t fft_buffer[FFT_ENC/2 + 1 + 256]; /* 513: 257 freq out + 256 scratch */
} codec2_encoder_t;

typedef struct codec2_decoder_t
{
    uint32_t next_rn;

    int32_t Sn_[2 * N_SAMP];  /* synthesis overlap-add buffer, Q8 */
    int16_t Pn[2 * N_SAMP];   /* Parzen window, Q15 */

    uint32_t ex_phase;        /* excitation fundamental phase, 2^32 = 2*pi */
    int32_t bg_est;           /* background noise estimate, dB Q16 */

    model_t prev_model_dec;
    int32_t prev_lsps_dec[LPC_ORD]; /* radians Q24 */
    int32_t prev_e_dec;       /* energy, log2 Q16 */

    complex_t fft_buffer[2 * FFT_DEC / 2 + 1 + 256]; /* 769: 256 a + 256 scratch + 257 Awg */
} codec2_decoder_t;

typedef struct codec2_t
{
    codec2_encoder_t encoder;
    codec2_decoder_t decoder;
} codec2_t;

#endif /* CODEC2_VHF_INTERNAL_H */
