#include "codec2_internal.h"
#include "nlp.h"
#include <string.h>


/* original taps, but re-ordered in a polyphase filter bank, Q20 */
const int32_t nlp_fir_poly_q20[NLP_NPHASE * NLP_TAPS_PP] = {
    // phase 0
    -1134, 2100, 842, -21937, 90498,
    143384, 3377, -12274, 5395, -973,
    // phase 1
    -1154, 3886, -5055, -13431, 120379,
    120379, -13431, -5055, 3886, -1154,
    // phase 2
    -973, 5395, -12274, 3377, 143384,
    90498, -21937, 842, 2100, -1134,
    // phase 3
    -443, 5864, -19083, 27980, 155898,
    58218, -23137, 4513, 577, 0,
    // phase 4
    577, 4513, -23137, 58218, 155898,
    27980, -19083, 5864, -443, 0,
};

/* Scale note: the NLP FFT input is pre-scaled down by 21 bits
   (squared speech * window >> 21), keeping sum|x| < 2^31 for the
   unscaled 512-point FFT (see codec2_internal.h). */

static int32_t post_process_sub_multiples(int64_t *Fw, int64_t gmax, int gmax_bin, int32_t prev_f0)
{
    int min_bin, cmax_bin;
    int mult;
    int64_t thresh;
    int32_t best_f0;
    int b, bmin, bmax, lmax_bin;
    int64_t lmax;
    int prev_f0_bin;

    /* post process estimate by searching submultiples */
    mult = 2;
    min_bin = PE_FFT_SIZE * DEC / P_MAX;
    cmax_bin = gmax_bin;
    prev_f0_bin = (int)(((int64_t)prev_f0 * 20972) >> 32); /* f0 * 2560 / 8000 */

    while (gmax_bin / mult >= min_bin)
    {
        b = gmax_bin / mult; /* determine search interval */
        bmin = 4 * b / 5;
        bmax = 6 * b / 5;
        if (bmin < min_bin)
            bmin = min_bin;

        /* lower threshold to favour previous frames pitch estimate,
            this is a form of pitch tracking */
        if ((prev_f0_bin > bmin) && (prev_f0_bin < bmax))
            thresh = (gmax * CNLP_Q20) >> 21;
        else
            thresh = (gmax * CNLP_Q20) >> 20;

        lmax = 0;
        lmax_bin = bmin;
        for (b = bmin; b <= bmax; b++) /* look for maximum in interval */
        {
            if (Fw[b] > lmax)
            {
                lmax = Fw[b];
                lmax_bin = b;
            }
        }

        if (lmax > thresh)
        {
            if (lmax_bin > 0 && lmax_bin < (PE_FFT_SIZE / 2) &&
                lmax > Fw[lmax_bin - 1] && lmax > Fw[lmax_bin + 1])
            {
                cmax_bin = lmax_bin;
            }
        }

        mult++;
    }

    /* f0 = cmax_bin * 8000 / 2560 Hz, Q16: exact * 204800 */
    best_f0 = cmax_bin * 204800;

    return best_f0;
}

void nlp(
    nlp_t *restrict nlp,
    const int16_t *restrict Sn, /* input speech vector, Q0 */
    int32_t *restrict f0,       /* out: pitch frequency, Hz Q16 */
    int32_t *restrict prev_f0,  /* previous pitch f0, Hz Q16 */
    complex_t *restrict fft_scratch) /* 256+ complex slots of scratch */
{
    int32_t notch;                    /* current notch filter output */
    int64_t *restrict Fw = nlp->Fw;   /* DFT of squared signal */
    int64_t gmax;
    int gmax_bin;
    int32_t best_f0;

    const int m = M_PITCH;
    const int n = N_SAMP;
    const int d = N_SAMP / DEC;
    const int START_POS = M_PITCH - N_SAMP;

    /* Square, notch filter at DC, and LP filter vector */
    /* Square latest input samples */
    for (int i = START_POS; i < m; i++)
    {
        nlp->sq[i] = (int32_t)Sn[i] * Sn[i];
    }

    for (int i = START_POS; i < m; i++)
    { /* notch filter at DC */
        notch = fp_sat32((int64_t)nlp->sq[i] - nlp->mem_x +
                         (((int64_t)nlp->mem_y * COEFF_Q15) >> 15));
        nlp->mem_x = nlp->sq[i];
        nlp->mem_y = notch;
        nlp->sq[i] = notch + 1; /* With 0 input vectors to codec,
                                   kiss_fft() would take a long
                                   time to execute when running in
                                   real time.  Problem was traced
                                   to the FFT call in this
                                   function. Adding this small
                                   constant fixed problem.  Not
                                   exactly sure why. */
    }

    /* decimating polyphase FIR filter */
    /* push left */
    memmove(nlp->sq_fir, nlp->sq_fir + d, (NDEC - d) * sizeof(int32_t));

    for (int i = 0; i < d; i++)
    {
        int idx = START_POS + NLP_NPHASE * i;
        int64_t acc = 0;

        for (int p = 0; p < NLP_NPHASE; p++)
        {
            const int32_t *restrict h = &nlp_fir_poly_q20[p * NLP_TAPS_PP];
            int base = idx - p;

            acc += (int64_t)h[0] * nlp->sq[base - 0 * NLP_NPHASE];
            acc += (int64_t)h[1] * nlp->sq[base - 1 * NLP_NPHASE];
            acc += (int64_t)h[2] * nlp->sq[base - 2 * NLP_NPHASE];
            acc += (int64_t)h[3] * nlp->sq[base - 3 * NLP_NPHASE];
            acc += (int64_t)h[4] * nlp->sq[base - 4 * NLP_NPHASE];
            acc += (int64_t)h[5] * nlp->sq[base - 5 * NLP_NPHASE];
            acc += (int64_t)h[6] * nlp->sq[base - 6 * NLP_NPHASE];
            acc += (int64_t)h[7] * nlp->sq[base - 7 * NLP_NPHASE];
            acc += (int64_t)h[8] * nlp->sq[base - 8 * NLP_NPHASE];
            acc += (int64_t)h[9] * nlp->sq[base - 9 * NLP_NPHASE];
        }

        nlp->sq_fir[NDEC - d + i] = (int32_t)(acc >> 20); /* Q20 taps */
    }

    /* Decimate and DFT.  The unscaled 512-point FFT needs sum|x| < 2^31;
       sum|x| <= max|sq_fir| * sum(w) >> shift, so the shift is chosen
       per frame (block floating point) from the FIR peak.  All NLP
       decisions below are ratios, so the scale factor cancels. */
    memset(nlp->fftr_buff, 0, sizeof(nlp->fftr_buff));

    int32_t fir_max = 0;
    for (int i = 0; i < NDEC; i++)
    {
        int32_t v = nlp->sq_fir[i] < 0 ? -nlp->sq_fir[i] : nlp->sq_fir[i];
        if (v > fir_max)
            fir_max = v;
    }
    /* sum(w) over the NDEC-point hann window is ~1.05e6 in Q15.  The
       shift removes the window's Q15 scale first (>> 15), plus extra
       block-scaling bits so sum|x| < 2^31 for the unscaled FFT. */
    int64_t bound = (int64_t)fir_max * 1050000;
    int shift = 15;
    while ((bound >> (shift - 15)) > (1LL << 30) && shift < 48)
        shift++;

    for (int i = 0; i < NDEC; i++)
    {
        nlp->fftr_buff[i] = (int32_t)(((int64_t)nlp->sq_fir[i] * nlp->w[i]) >> shift);
    }

    /* Fw is int64[257], layout-identical to 257 complex slots, so the
       FFT can write straight into it and be squared in place */
    fft_real_fwd512(nlp->fftr_buff, (cpx_q_t *)Fw, fft_scratch);

    for (int i = 0; i < PE_FFT_SIZE / 2 + 1; i++)
    {
        int32_t fr = ((cpx_q_t *)Fw)[i].r;
        int32_t fi = ((cpx_q_t *)Fw)[i].i;
        Fw[i] = (int64_t)fr * fr + (int64_t)fi * fi;
    }

    /* express everything in f0, as pitch in samples is dep on Fs */
    int pmin = P_MIN;
    int pmax = P_MAX;

    /* find global peak */
    gmax = 0;
    gmax_bin = PE_FFT_SIZE * DEC / pmax;
    int lo = PE_FFT_SIZE * DEC / pmax;
    int hi = PE_FFT_SIZE * DEC / pmin;
    if (hi > PE_FFT_SIZE / 2)
        hi = PE_FFT_SIZE / 2;

    for (int i = lo; i <= hi; i++)
    {
        if (Fw[i] > gmax)
        {
            gmax = Fw[i];
            gmax_bin = i;
        }
    }

    best_f0 = post_process_sub_multiples(Fw, gmax, gmax_bin, *prev_f0);

    /* Shift samples in buffer to make room for new samples */
    for (int i = 0; i < START_POS; i++)
        nlp->sq[i] = nlp->sq[i + n];

    /* return pitch frequency in Hz and update tracking memory */
    *f0 = best_f0;

    *prev_f0 = best_f0;
}

void nlp_init(nlp_t *nlp)
{
    memset(nlp->sq_fir, 0, sizeof(nlp->sq_fir));

    for (int i = 0; i < NDEC; i++)
    {
        uint32_t ph = (uint32_t)(((uint64_t)i << 32) / (NDEC - 1));
        /* (1 - cos)/2 in Q15: cos is Q31, so (2^31 - cos) >> 16 */
        int32_t v = (int32_t)(((1LL << 31) - fp_sin_q31(ph + 0x40000000u)) >> 16);
        nlp->w[i] = (int16_t)(v > 32767 ? 32767 : v);
    }

    memset(nlp->sq, 0, sizeof(nlp->sq));

    nlp->mem_x = 0;
    nlp->mem_y = 0;
}
