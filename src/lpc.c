#include "codec2_internal.h"
#include "lpc.h"
#include "util.h"
#ifdef C2VHF_DUMP_HOOKS
#include <stdlib.h>
#include <stdio.h>
#endif

/* non-zero span of the analysis window w[] */
#define W_START (M_PITCH / 2 - NW / 2)
#define W_END (W_START + NW)

/* ~15Hz bandwidth expansion, Q30 */
const int32_t bw_gamma_q30[LPC_ORD + 1] = {
    1073741824,
    1067299373,
    1060895577,
    1054530203,
    1048203022,
    1041913804,
    1035662321,
    1029448347,
    1023271657,
    1017132027,
    1011029235,
};

/* sum of Wn[i]*Wn[i+j] over the non-zero window span.
   Wn is Q16, so R[] is Q32.  Bounded by (0.6007*32767*2^16)^2 < 2^62. */
static void autocorrelate(
    const int32_t *restrict Wn, /* frame of windowed speech, Q16 */
    int64_t *restrict Rn        /* array of P+1 autocorrelation coeffs, Q32 */
)
{
    for (int j = 0; j < LPC_ORD + 1; j++)
    {
        int64_t acc = 0;
        for (int i = W_START; i < W_END - j; i++)
            acc += (int64_t)Wn[i] * Wn[i + j];
        Rn[j] = acc;
    }
}

/*
  Levinson-Durbin on the normalised autocorrelation.  Rn is first
  down-scaled so Rn[0] fits comfortably in int32 (<= 2^30); k comes out
  in Q31 and the coefficients in Q24.
*/
static void levinson_durbin(
    const int64_t *restrict R, /* order+1 autocorrelation coeffs, Q32 */
    int32_t *restrict lpcs     /* order+1 LPCs, Q24 */
)
{
    int32_t a[LPC_ORD + 1];
    int32_t a_prev[LPC_ORD + 1];
    int32_t Rn[LPC_ORD + 1];
    int32_t e, k;
    int64_t sum;

    int shift = 0;
    int64_t r0 = R[0];
    while (r0 > (1LL << 30))
    {
        r0 >>= 1;
        shift++;
    }

    for (int j = 0; j < LPC_ORD + 1; j++)
        Rn[j] = (int32_t)(R[j] >> shift);

    /* a[0] = 1 by definition */
    a[0] = 1 << 24;
    e = Rn[0];

    for (int i = 1; i <= LPC_ORD; i++)
    {
        if (e == 0)
        {
            /* perfectly correlated input collapsed the prediction error;
               the float reference would produce inf/nan here, which the
               downstream quantisers treat as benign LSPs anyway */
            for (int j = 1; j <= LPC_ORD; j++)
                a[j] = 0;
            break;
        }
        sum = 0;
        for (int j = 1; j <= i - 1; j++)
            sum += ((int64_t)a_prev[j] * Rn[i - j]) >> 24; /* a is Q24 */

        /* k = -(R[i] + sum) / e, Q31; |k| <= 1 for a valid predictor.
           Clamp num first so the Q31 shift cannot overflow; any |k| > 1
           is clamped to 0 below anyway (legitimate k needs |num| < e). */
        int64_t num = (int64_t)Rn[i] + sum;
        if (num > (1LL << 31))
            num = 1LL << 31;
        else if (num < -(1LL << 31))
            num = -(1LL << 31);
        int64_t kn = -(num * (1LL << 31)) / e;
        if (kn > (1LL << 31))
        {
            kn = 0;
        }
        else if (kn < -(1LL << 31))
        {
            kn = 0;
        }
        k = (int32_t)kn;

        a[i] = k >> 7; /* Q31 -> Q24 */

        for (int j = 1; j <= i - 1; j++)
            a[j] = fp_sat32((int64_t)a_prev[j] + (((int64_t)k * a_prev[i - j]) >> 31));

        /* e *= (1 - k^2), all Q31 */
        int64_t k2 = ((int64_t)k * k) >> 31;
        e = (int32_t)(((int64_t)e * ((1LL << 31) - k2)) >> 31);
        if (e == 0 && i < LPC_ORD)
        {
            /* error collapsed (perfectly periodic input); zero remaining
               coefficients so the output stays benign */
            for (int j = i + 1; j <= LPC_ORD; j++)
                a[j] = 0;
            break;
        }

            /* copy for next iteration */
        for (int j = 1; j <= i; j++)
            a_prev[j] = a[j];
    }

    /* output LPCs */
    lpcs[0] = 1 << 24;
    for (int i = 1; i <= LPC_ORD; i++)
        lpcs[i] = a[i];
}

/* NOTE: this function uses fully unrolled loop for LPC_ORD=10
   coef is Q24, x is Q30 (in [-1,1]), result is Q30 */
static inline int64_t cheb_poly_eva(
    const int32_t *restrict coef, /* coefficients of the polynomial */
    int32_t x                     /* the point where polynomial is evaluated */
)
{
    // N = 5 (LPC_ORD/2)
    int64_t T0 = 1 << 30;
    int64_t T1 = x;

    int64_t sum = ((int64_t)coef[5] * T0 >> 24) + ((int64_t)coef[4] * T1 >> 24);

    // i = 2
    int64_t T2 = 2 * (((int64_t)x * T1) >> 30) - T0;
    sum += (int64_t)coef[3] * T2 >> 24;

    // i = 3
    int64_t T3 = 2 * (((int64_t)x * T2) >> 30) - T1;
    sum += (int64_t)coef[2] * T3 >> 24;

    // i = 4
    int64_t T4 = 2 * (((int64_t)x * T3) >> 30) - T2;
    sum += (int64_t)coef[1] * T4 >> 24;

    // i = 5
    int64_t T5 = 2 * (((int64_t)x * T4) >> 30) - T3;
    sum += (int64_t)coef[0] * T5 >> 24;

    return sum;
}

/*
  Round an exact Q30 value to the float32 grid (round-to-nearest-even).
  The reference implementation walks the LSP search interval in float32
  arithmetic; replicating its rounding exactly in integer ops keeps the
  fixed-point encoder's quantiser decisions aligned with the float
  bitstream.  Valid for |d| representing |x| < 4.
*/
static int32_t f32_round_q30(int64_t d)
{
    if (d == 0)
        return 0;

    uint64_t a = d < 0 ? (uint64_t)-(d + 1) + 1 : (uint64_t)d;
    int e = 30;
    while ((a >> e) == 0)
        e--;
    /* |x| = a/2^30 with MSB at e, so exp(x) = e-30 and the float32
       ulp of 2^(exp-23) is 2^(e-23) in Q30 units */
    int s = e - 23;
    if (s <= 0)
        return (int32_t)d; /* exactly representable */

    uint64_t ulp = 1ULL << s;
    uint64_t q = a >> s;
    uint64_t rem = a & (ulp - 1);
    if (rem > (ulp >> 1) || (rem == (ulp >> 1) && (q & 1)))
        q++;
    int64_t r = (int64_t)(q << s);
    return d < 0 ? (int32_t)-r : (int32_t)r;
}

/* NOTE: This function uses 6 bisections */
static int lpc_to_lsp(
    const int32_t *a, /* LPC coefficients, Q24 */
    int32_t *freq     /* LSP frequencies, radians Q24 */
)
{
    int64_t psuml, psumr, psumm;
    int32_t xl, xr, xm;
    const int32_t *pt; /* whether P' or Q' */
    int roots = 0;     /* number of roots found */
    int32_t Q[LPC_ORD + 1];
    int32_t P[LPC_ORD + 1];

    int m = LPC_ORD / 2; /* order of P'(z) & Q'(z) polynomials */

    /* a in Q24 -> P/Q built in Q24 (values stay small) */
    P[0] = Q[0] = 1 << 24;
    for (int i = 1; i <= m; i++)
    {
        P[i] = a[i] + a[LPC_ORD + 1 - i] - P[i - 1];
        Q[i] = a[i] - a[LPC_ORD + 1 - i] + Q[i - 1];
    }

    for (int i = 0; i < m; i++)
    {
        P[i] *= 2;
        Q[i] *= 2;
    }

    /* Search for a zero in P'(z) polynomial first and then alternate to Q'(z).
    Keep alternating between the two polynomials as each zero is found */
    xl = 1 << 30; /* start at point xl = 1 (Q30) */
    const int32_t delta = LSP_DELTA1_Q30;

    for (int j = 0; j < LPC_ORD; j++)
    {
        pt = (j & 1) ? Q : P; /* determines whether P' or Q' is eval. */

        xr = xl;
        psuml = cheb_poly_eva(pt, xl); /* evals poly. at xl */
    
        while (xr >= -(1 << 30))
        {
            /* float reference: xr = xl - 0.01f, rounded on the float32
               grid (round-to-nearest-even) */
            xr = f32_round_q30((int64_t)xl - delta); /* interval spacing */
            psumr = cheb_poly_eva(pt, xr); /* poly(xl-delta_x) */

            /* if no sign change increment xr and re-evaluate
               poly(xr). Repeat til sign change.  */
            if ((psumr <= 0 && psuml >= 0) || (psumr >= 0 && psuml <= 0))
            {
                roots++;

                /* manually unrolled for nb=5 (thus 6x) */
                xm = f32_round_q30((int64_t)xl + xr) >> 1;
                psumm = cheb_poly_eva(pt, xm);
                if ((psumm > 0) == (psuml > 0) && psumm != 0)
                {
                    xl = xm;
                    psuml = psumm;
                }
                else
                {
                    xr = xm;
                }

                xm = f32_round_q30((int64_t)xl + xr) >> 1;
                psumm = cheb_poly_eva(pt, xm);
                if ((psumm > 0) == (psuml > 0) && psumm != 0)
                {
                    xl = xm;
                    psuml = psumm;
                }
                else
                {
                    xr = xm;
                }

                xm = f32_round_q30((int64_t)xl + xr) >> 1;
                psumm = cheb_poly_eva(pt, xm);
                if ((psumm > 0) == (psuml > 0) && psumm != 0)
                {
                    xl = xm;
                    psuml = psumm;
                }
                else
                {
                    xr = xm;
                }

                xm = f32_round_q30((int64_t)xl + xr) >> 1;
                psumm = cheb_poly_eva(pt, xm);
                if ((psumm > 0) == (psuml > 0) && psumm != 0)
                {
                    xl = xm;
                    psuml = psumm;
                }
                else
                {
                    xr = xm;
                }

                xm = f32_round_q30((int64_t)xl + xr) >> 1;
                psumm = cheb_poly_eva(pt, xm);
                if ((psumm > 0) == (psuml > 0) && psumm != 0)
                {
                    xl = xm;
                    psuml = psumm;
                }
                else
                {
                    xr = xm;
                }

                xm = f32_round_q30((int64_t)xl + xr) >> 1;
                psumm = cheb_poly_eva(pt, xm);
                if ((psumm > 0) == (psuml > 0) && psumm != 0)
                {
                    xl = xm;
                    psuml = psumm;
                }
                else
                {
                    xr = xm;
                }

                /* once zero is found, reset initial interval to xr */
                freq[j] = fp_acos_q24(xm * 2); /* Q30 -> Q31 -> radians Q24 */
                xl = xm;
                break;
            }
            else
            {
                psuml = psumr;
                xl = xr;
            }
        }
        if (roots <= j)
        {
            /* no root found in this interval: leave a benign value */
            freq[j] = (int32_t)(((int64_t)PI_Q24 * j) / LPC_ORD);
        }
    }

    return (roots);
}

void lsp_to_lpc(
    const int32_t *restrict lsp, /* array of LSP frequencies, radians Q24 */
    int32_t *restrict ak         /* array of LPC coefficients, Q24 */
)
{
    int64_t xout1, xout2, xin1, xin2;
    int64_t *n1, *n2, *n3, *n4 = 0;
    int32_t freq[LPC_ORD]; /* cos(lsp), Q31 */
    int64_t Wp[2 * LPC_ORD + 2];

    /* convert from radians to the x=cos(w) domain */
    for (int i = 0; i < LPC_ORD; i++)
    {
        uint32_t ph = (uint32_t)(((int64_t)lsp[i] * PHASE_PER_RAD_Q24) >> 24);
        int32_t s, c;
        fp_sincos_q31(ph, &s, &c);
        freq[i] = c;
    }

    /* initialise contents of array */
    memset(Wp, 0, sizeof(Wp));

    xin1 = 1 << 24;
    xin2 = 1 << 24;

    /* reconstruct P(z) and Q(z) by cascading second order polynomials
      in form 1 - 2xz(-1) +z(-2), where x is the LSP coefficient.
      Lattice state is Q24 in 64-bit containers. */
    for (int j = 0; j <= LPC_ORD; j++)
    {
        for (int i = 0; i < (LPC_ORD / 2); i++)
        {
            n1 = Wp + (i * 4);
            n2 = n1 + 1;
            n3 = n2 + 1;
            n4 = n3 + 1;
            xout1 = xin1 - (((int64_t)freq[2 * i] * *n1) >> 30) + *n2;
            xout2 = xin2 - (((int64_t)freq[2 * i + 1] * *n3) >> 30) + *n4;
            *n2 = *n1;
            *n4 = *n3;
            *n1 = xin1;
            *n3 = xin2;
            xin1 = xout1;
            xin2 = xout2;
        }

        xout1 = xin1 + *(n4 + 1);
        xout2 = xin2 - *(n4 + 2);

        ak[j] = fp_sat32((xout1 + xout2) >> 1);

        *(n4 + 1) = xin1;
        *(n4 + 2) = xin2;

        xin1 = 0;
        xin2 = 0;
    }

}

void aks_to_mag2(codec2_decoder_t *c2,
                 const int32_t *ak,  /* LPCs, Q24 */
                 model_t *model,
                 int32_t Elog)       /* energy, log2 Q16 */
{
    /* Scratch layout inside c2->fft_buffer (769 complex slots).  The
       A(z) spectrum the caller needs for synthesis is left behind in
       [0..256], so the call site needs no Aw buffer.  The A_gamma(z)
       transform runs first: once both transforms are done its 512
       int32 input region is dead and is overwritten by the Aw output:

       [0..255]     a / ag: FFT input, 512 int32; becomes Aw (bins
                    0..256) after the second transform
       [256..511]   complex FFT scratch (both transforms); Pw (256
                    int32) overlays it once the transforms are done
       [512..768]   Awg: FFT of ag, Q24 (bins 0..256); overwritten
                    in-place with log2(A2g) int32[256] after it */
    int32_t *a = (int32_t *)&c2->fft_buffer[0];
    cpx_q_t *scratch = &c2->fft_buffer[256];
    cpx_q_t *Aw = c2->fft_buffer;
    cpx_q_t *Awg = &c2->fft_buffer[512];
    int32_t *Pw = (int32_t *)&c2->fft_buffer[256]; /* overlays scratch */

    int am, bm; /* limits of current band */

    /* FFT of A_gamma(z) */
    int64_t g = LPCPF_GAMMA_Q30;
    memset(a, 0, FFT_ENC * sizeof(int32_t));
    a[0] = ak[0];
    for (int i = 1; i <= LPC_ORD; i++)
    {
        a[i] = (int32_t)(((int64_t)ak[i] * g) >> 30);
        g >>= 1;
    }

    fft_real_fwd512(a, Awg, scratch);

    /* convert to log2(A2g) in-place: the int32 log2 values compact
       into the first 128 cpx slots, overwriting the cpx data at lower
       byte offsets (safe: the write at byte 4*i trails the read at
       byte 8*i for i > 0) */
    int32_t *L2A2g_arr = (int32_t *)Awg;
    for (int i = 0; i < FFT_ENC / 2; i++)
    {
        int64_t A2gi = (int64_t)Awg[i].r * Awg[i].r + (int64_t)Awg[i].i * Awg[i].i + A2_FLOOR_Q48;
        L2A2g_arr[i] = (int32_t)(fp_log2_q16(A2gi) - (48 << 16));
    }

    /* FFT of A(z) into [0..256]: overlaps the consumed ag input and
       scratch[0]; fft_real_fwd512 reads its whole input before
       writing any output, so the overlap is safe */
    memset(a, 0, FFT_ENC * sizeof(int32_t));
    for (int i = 0; i <= LPC_ORD; i++)
        a[i] = ak[i];

    fft_real_fwd512(a, Aw, scratch);

    /* per-bin post-filtered power spectrum, log domain:
       log2 Pw = 0.2*log2(A2g) - 1.2*log2(A2)  (A2 read back from Aw)
       invA2 = exp2(-log2 A2) via fp_exp2_q31, replacing the int64
       divide (1<<56)/A2i which costs 30-60+ cycles on CSKY */
    int64_t e_before = 0; /* sum 1/A2, Q8 */
    int64_t e_after = 0;  /* sum Pw, Q8 */

    for (int i = 0; i < FFT_ENC / 2; i++)
    {
        int64_t A2i = (int64_t)Aw[i].r * Aw[i].r + (int64_t)Aw[i].i * Aw[i].i + A2_FLOOR_Q48;
        int64_t raw_L2 = fp_log2_q16(A2i);
        int64_t L2A2 = raw_L2 - (48 << 16);
        /* 64-bit products: log2 terms span ±20 Q16, and 13107/78643
           times that overflows int32 (wraps the post-filter at bins
           where |A_gamma|^2 < 0.18, e.g. strong formants) */
        int64_t logPw = ((int64_t)PWR_A2G_Q16 * L2A2g_arr[i] - (int64_t)PWR_A2_Q16 * L2A2) >> 16;

        if (logPw > (23 << 16))
            logPw = 23 << 16; /* keep Pw in int32 Q8 range */
        Pw[i] = (int32_t)(fp_exp2_q31((int32_t)logPw) >> 23); /* Q8 */

        e_after += Pw[i];
        /* invA2_Q8 = exp2(-L2A2) >> 23: fp_exp2_q31(-L2A2) gives
           (1/A2_true) × 2^31, >> 23 gives (1/A2_true) × 2^8 */
        e_before += fp_exp2_q31((int32_t)(-L2A2)) >> 23;
    }

    /* decoded linear energy, Q16 (Q6 truncated quiet-frame energies by
       up to 6%, which the gain below then multiplied through) */
    int64_t e_q16 = fp_exp2_q31(Elog) >> 15;

    /* normalization gain: gain = E * e_before / e_after.
       Q22 (not Q6!): at typical speech energies the true gain is ~0.03,
       which resolves to just 0-2 quanta in Q6 integer division and
       crushes quiet frames' harmonics by up to sqrt(2) with harmonic
       gating (audible roughness).  Q22 keeps ~5 decimal digits. */
    int64_t gain_q22 = e_q16 * ((e_before << 6) / e_after);

    /* boost low frequencies a bit (< 1000 Hz), after e_after is taken */
    for (int i = 0; i < FFT_ENC / 2 * 1000 / (SAMP_RATE / 2); i++)
        Pw[i] = fp_sat32((int64_t)Pw[i] * BOOST_1000_Q8 >> 8);

    /* Determine magnitudes */
    for (int m = 1; m <= model->L; m++)
    {
        int64_t wo_bin_q20 = ((int64_t)model->Wo * FFT_BINS_PER_RAD_Q24) >> 28;
        am = (int)((((int64_t)m * wo_bin_q20) - (wo_bin_q20 >> 1)) + (1 << 19)) >> 20;
        bm = (int)((((int64_t)m * wo_bin_q20) + (wo_bin_q20 >> 1)) + (1 << 19)) >> 20;
        if (bm > FFT_ENC / 2)
            bm = FFT_ENC / 2;
        if (am < 0)
            am = 0;

        int64_t Em = 0;
        for (int i = am; i < bm; i++)
            Em += Pw[i];

        /* A = sqrt(Em * gain): Em is Q8, gain is Q22, so the product is
           Q30 -> sqrt -> Q15 -> >>11 -> Q4 */
        int64_t mag = Em * gain_q22;
        int32_t Am = (int32_t)(fp_isqrt64((uint64_t)mag) >> 11); /* Q4 */
        if (Am > A_CLAMP_Q4)
            Am = A_CLAMP_Q4;
        model->A[m] = Am;
    }
#ifdef C2VHF_DUMP_HOOKS
    if (getenv("C2VHF_DUMP")) {
        long s2 = 0; int mxa = 0;
        for (int m = 1; m <= model->L; m++) { s2 += (long)((int64_t)model->A[m]*model->A[m]>>8); if (model->A[m] > mxa) mxa = model->A[m]; }
        fprintf(stderr, "AKS L %d v%d Wo %.6g E %.6g sumA2 %.6g maxA %.6g "
                "eb %.6g ea %.6g eq6 %.6g gain %.6g\n",
                model->L, model->voiced, model->Wo/16777216.0, fp_exp2_q31(Elog)/2147483648.0,
                (double)s2, (double)mxa/16.0,
                e_before/256.0, e_after/256.0, e_q16/65536.0, gain_q22/4194304.0);
        fprintf(stderr, "PW:");
        { int32_t *PWv = (int32_t *)&c2->fft_buffer[256];
          for (int q = 0; q < 16; q++) fprintf(stderr, " %.6g", PWv[q]/256.0); }
        fprintf(stderr, "\n");
        fprintf(stderr, "A[]:");
        for (int m = 1; m <= model->L; m++) fprintf(stderr, " %.6g", model->A[m]/16.0);
        fprintf(stderr, "\n");
    }
#endif
}

/* Apply first harmonic LPC correction at decoder.
   This helps improve low pitch males after LPC modelling. */
void apply_lpc_correction(model_t *model)
{
    if (model->Wo < LPC_150HZ_Q24)
    {
        model->A[1] = model->A[1] * 32 / 1000;
    }
}

void speech_to_uq_lsps(
    codec2_encoder_t *c2,
    int32_t *lsp,        /* out: LSPs, radians Q24 */
    int32_t *ak,         /* out: LPCs, Q24 */
    int32_t *energy,     /* out: Elog, log2 Q16 */
    const int16_t *Sn,   /* speech history, Q0 */
    const int32_t *w)    /* window, Q22 */
{
    int roots;
    int64_t R[LPC_ORD + 1];
    int64_t e;
    int32_t Elog;

    /* windowed speech, Q16: Wn = Sn * w >> 12 (w is Q28), rounded */
    int32_t *Wn = (int32_t *)c2->fft_buffer; /* 512 int32, plenty */

    memset(Wn, 0, W_START * sizeof(int32_t));
    memset(&Wn[W_END], 0, (M_PITCH - W_END) * sizeof(int32_t));
    e = 0;
    for (int i = W_START; i < W_END; i++)
    {
        int64_t p = (int64_t)Sn[i] * w[i];
        Wn[i] = (int32_t)((p + (p < 0 ? -2048 : 2048)) >> 12);
        e += (int64_t)Wn[i] * Wn[i];
    }

    /* trap 0 energy case as LPC analysis will fail; float floor was 1e-6,
       which is 1e-6 * 2^32 = 4295 in Q32 */
    if (e < 4295)
    {
        for (int i = 0; i < LPC_ORD; i++)
            lsp[i] = (int32_t)(((int64_t)PI_Q24 * i) / LPC_ORD);

        memset(ak, 0, (LPC_ORD + 1) * sizeof(int32_t));
        ak[0] = 1 << 24;
        *energy = E_ZERO_FLOOR_LOG2_Q16;
        return;
    }

    autocorrelate(Wn, R);
    levinson_durbin(R, ak);

    /* E = sum ak[i]*R[i], using unexpanded ak.  With Rn = R >> shift:
       E_true = 2^(shift-56) * sum(ak_q24 * Rn), so
       Elog = log2(sum) + (shift - 56) << 16. */
    int shift = 0;
    int64_t r0 = R[0];
    while (r0 > (1LL << 30))
    {
        r0 >>= 1;
        shift++;
    }
    int32_t Rn[LPC_ORD + 1];
    for (int j = 0; j < LPC_ORD + 1; j++)
        Rn[j] = (int32_t)(R[j] >> shift);

    int64_t E = 0;
    for (int i = 0; i <= LPC_ORD; i++)
        E += (int64_t)ak[i] * Rn[i];

    if (E > 0)
        Elog = fp_log2_q16(E) + (shift - 56) * 65536;
    else
        Elog = E_ZERO_FLOOR_LOG2_Q16;

    /* 15 Hz BW expansion.  Important to do this after energy calculation
       to avoid -ve energy values. */
    for (int i = 0; i <= LPC_ORD; i++)
    {
        ak[i] = (int32_t)(((int64_t)ak[i] * bw_gamma_q30[i]) >> 30);
    }

    roots = lpc_to_lsp(ak, lsp); // hardcoded to 6 bisections
    if (roots != LPC_ORD)
    {
        /* if root finding fails use some benign LSP values instead */
        for (int i = 0; i < LPC_ORD; i++)
            lsp[i] = (int32_t)(((int64_t)PI_Q24 * i) / LPC_ORD);
    }

    *energy = Elog;
}
