#include "codec2_internal.h"
#include "analysis.h"
#include "nlp.h"
#include "lpc.h"


/*
  Scratch layout in the encoder's fft_buffer (768 complex slots):
	[0..511]   Sw: speech spectrum, Q16, full Hermitian 0..511;
			   the lower 256 complex slots double as the FFT input
			   (512 int32) which is consumed before output is written
	[512..767] complex FFT scratch
*/

static void make_analysis_window(
	codec2_encoder_t *c2,
	int32_t *restrict w,
	int32_t *restrict W)
{
	static const int mp2 = M_PITCH / 2;
	static const int nw2 = NW / 2;
	static const int fe2 = FFT_ENC / 2;

	cpx_q_t *Sw = &c2->fft_buffer[0];
	cpx_q_t *scratch = &c2->fft_buffer[257];

	/* zero time-domain window */
	memset(w, 0, M_PITCH * sizeof(int32_t));

	/* build Hann window, Q20 */
	int64_t m = 0;

	for (int j = 0; j < NW; j++)
	{
		uint32_t phase = (uint32_t)(((uint64_t)j << 32) / (NW - 1));
		int32_t val = (int32_t)(((2LL << 30) - fp_sin_q31(phase + 0x40000000u)) >> 11); /* Q20 */
		int i = mp2 - nw2 + j;
		w[i] = val;
		m += (int64_t)val * val;
	}

	/* normalize: w_final(Q28) = wu(Q20) << 28 / sqrt(sum(wu^2) * 512),
	   rounded to nearest */
	int64_t denom = fp_isqrt64((uint64_t)m * 512);
	for (int i = 0; i < M_PITCH; i++)
		w[i] = (int32_t)(((int64_t)w[i] << 28) / denom);

	/* circular shift: FFT input is the window itself, Q28 */
	int32_t *x = (int32_t *)Sw; /* 512 int32, aliases Sw[0..255] */
	memset(x, 0, FFT_ENC * sizeof(int32_t));
	for (int i = 0; i < nw2; i++)
		x[i] = w[i + mp2];

	for (int i = FFT_ENC - nw2, j = mp2 - nw2; i < FFT_ENC; i++, j++)
		x[i] = w[j];

	/* real FFT, bins 0..256.  Output written over Sw[0..256]; the input
	   (aliasing Sw[0..255]) is fully consumed before output starts. */
	fft_real_fwd512(x, Sw, scratch);

	/* rearrange frequency response (real part is even-symmetric):
	   W[i] = Re(Sw[fe2 - i]), W[i + fe2] = Re(Sw[i]); FFT of the Q28
	   window is Q28, stored Q16 */
	for (int i = 0; i < fe2; i++)
	{
		W[i] = Sw[fe2 - i].r >> 12; /* == bin i + fe2 */
		W[i + fe2] = Sw[i].r >> 12;
	}
}

static void hs_pitch_refinement(model_t *restrict model, const complex_t *restrict Sw, int32_t pmin, int32_t pmax, int32_t pstep)
{
	int64_t E;  /* energy for current pitch */
	int32_t Wo; /* current "test" fundamental freq, Q24 */
	int32_t Wom;
	int64_t Em; /* maximum energy */

	/* Initialisation: L = pi / Wo */
	model->L = (int)(((int64_t)PI_Q24 << 8) / model->Wo >> 8);
	Wom = model->Wo;
	Em = 0;

	/* Determine harmonic sum for a range of Wo values; pitch in Q8 */
	for (int32_t p = pmin; p <= pmax; p += pstep)
	{
		E = 0;
		Wo = (int32_t)(((int64_t)TWO_PI_Q24 << 8) / p);

		/* harmonic centre bin: Wo * 512 / (2*pi), Q20 */
		int32_t bFloat = (int32_t)(((int64_t)Wo * FFT_BINS_PER_RAD_Q24) >> 28);
		int32_t currentB = bFloat;

		// we test the same number of harmonics per candidate
		for (int mm = 1; mm <= model->L; mm++)
		{
			int b = (currentB + (1 << 19)) >> 20;

			// reading past Nyquist adds some noise that helps
			E += (int64_t)Sw[b].r * Sw[b].r + (int64_t)Sw[b].i * Sw[b].i;
			currentB += bFloat;
		}

		/* Compare to see if this is a maximum */
		if (E > Em)
		{
			Em = E;
			Wom = Wo;
		}
	}

	model->Wo = Wom;
}

static void two_stage_pitch_refinement(model_t *restrict model, const complex_t *restrict Sw)
{
	int32_t pmin, pmax, pstep; /* pitch refinement minimum, maximum and step, Q8 */

	/* Coarse refinement */
	pmax = (int32_t)(((int64_t)TWO_PI_Q24 << 8) / model->Wo) + (5 << 8);
	pmin = (int32_t)(((int64_t)TWO_PI_Q24 << 8) / model->Wo) - (5 << 8);
	pstep = 1 << 8;
	hs_pitch_refinement(model, Sw, pmin, pmax, pstep);

	/* Fine refinement */
	pmax = (int32_t)(((int64_t)TWO_PI_Q24 << 8) / model->Wo) + (1 << 8);
	pmin = (int32_t)(((int64_t)TWO_PI_Q24 << 8) / model->Wo) - (1 << 8);
	pstep = 1 << 6;
	hs_pitch_refinement(model, Sw, pmin, pmax, pstep);

	/* Limit range */
	if (model->Wo < W0_MIN_Q24)
		model->Wo = W0_MIN_Q24;
	if (model->Wo > W0_MAX_Q24)
		model->Wo = W0_MAX_Q24;

	model->L = wo_to_L(model->Wo);

	/* trap occasional round off issues with floor() */
	if ((int64_t)model->Wo * model->L >= WO_GUARD_Q24)
	{
		model->L--;
	}
}

static void estimate_amplitudes(model_t *model, const complex_t *Sw, int32_t *A16)
{
	for (int mm = 1; mm <= model->L; mm++)
	{
		/* Estimate amplitude of harmonic */
		int64_t den = 0; /* denominator of amplitude expression */

		/* bounds of current harmonic */
		int32_t wo_bin_q20 = (int32_t)(((int64_t)model->Wo * FFT_BINS_PER_RAD_Q24) >> 28);
		int am = (int)((((int64_t)mm * wo_bin_q20) - (wo_bin_q20 >> 1)) + (1 << 19)) >> 20;
		int bm = (int)((((int64_t)mm * wo_bin_q20) + (wo_bin_q20 >> 1)) + (1 << 19)) >> 20;

		// clamp
		if (am < 0)
			am = 0;
		if (bm > FFT_ENC / 2)
			bm = FFT_ENC / 2;

		for (int i = am; i < bm; i++)
		{
			den += (int64_t)Sw[i].r * Sw[i].r + (int64_t)Sw[i].i * Sw[i].i;
		}

		/* sqrt of a Q32 sum is Q16 */
		A16[mm] = (int32_t)fp_isqrt64((uint64_t)den);
	}
}

static void est_voicing_mbe(model_t *restrict model, const complex_t *restrict Sw, const int32_t *restrict W, const int32_t *A16)
{
	const int32_t Wo = model->Wo;
	const int32_t wo_bin_q20 = (int32_t)(((int64_t)Wo * FFT_BINS_PER_RAD_Q24) >> 28);

	/*
	   Determine the ratio of low frequency to high frequency energy,
	   voiced speech tends to be dominated by low frequency energy,
	   unvoiced by high frequency. This measure can be used to
	   determine if we have made any gross errors.
	*/
	const int l_1000hz = model->L / 4;
	const int l_2000hz = model->L / 2;
	const int l_4000hz = model->L;

	/* sig/elow/ehigh and error are all kept in "A-units squared at Q4"
	   (A4 = A16 >> 12, matching model->A): products are Q8 and the sums
	   stay far inside int64 while all threshold ratios are preserved */
	int64_t sig = 0, elow = 0, ehigh = 0;
	for (int l = 1; l <= l_4000hz; l++)
	{
		int64_t t = (int64_t)(A16[l] >> 12) * (A16[l] >> 12);

		if (l <= l_2000hz)
		{
			elow += t;
			if (l <= l_1000hz)
				sig += t;
		}
		if (l >= l_2000hz)
			ehigh += t;
	}

	int64_t error = 0; /* accumulated error between original and synthesised */

	/* Just test across the harmonics in the first 1000 Hz */
	for (int l = 1; l <= l_1000hz; l++)
	{
		int64_t Amr = 0, Ami = 0;
		int64_t den = 0; /* denominator of Am expression */

		int32_t lo = (int32_t)((((int64_t)l * wo_bin_q20) - (wo_bin_q20 >> 1) + (1 << 20) - 1) >> 20); /* ceil */
		int32_t hi = (int32_t)((((int64_t)l * wo_bin_q20) + (wo_bin_q20 >> 1) + (1 << 20) - 1) >> 20); /* ceil */

		/* Estimate amplitude of harmonic assuming harmonic is totally voiced */
		int32_t lbin = (int32_t)(((int64_t)l * wo_bin_q20 + (1 << 19)) >> 20);
		int offset = FFT_ENC / 2 - lbin; /* centers Hw[] about current harmonic */

		if (offset < -lo)
			offset = -lo;
		if (offset + hi >= FFT_ENC)
			offset = FFT_ENC - hi - 1;

		const int32_t *restrict Wp = &W[offset];

		if (lo < 0)
			lo = 0;
		if (hi > FFT_ENC)
			hi = FFT_ENC;

		for (int mm = lo; mm < hi; mm++)
		{
			int32_t wv = Wp[mm];

			Amr += (int64_t)Sw[mm].r * wv;
			Ami += (int64_t)Sw[mm].i * wv;
			den += (int64_t)wv * wv;
		}

		if (den == 0)
			continue;

		/* Am = sum(Sw*W)/sum(W^2), both in Q32 -> Am in Q16 */
		Amr = Amr * 256 / den * 256;
		Ami = Ami * 256 / den * 256;

		/* Determine error between estimated harmonic and original,
		   downscaled to the same Q4 domain as sig (with rounding) */
		for (int mm = lo; mm < hi; mm++)
		{
			int64_t dr = ((Sw[mm].r - (Amr * Wp[mm] >> 16)) + 2048) >> 12;
			int64_t di = ((Sw[mm].i - (Ami * Wp[mm] >> 16)) + 2048) >> 12;
			error += dr * dr + di * di;
		}
	}

	if (sig > ((error * V_THRESH_LIN_Q20) >> 20))
		model->voiced = 1;
	/* sig, error <= ~1e12 here, so the Q20 multiplies below stay < 2^63 */
	else
		model->voiced = 0;

	/* post processing, helps clean up some voicing errors ------------------*/
	/* Look for Type 1 errors, strongly V speech that has been
	   accidentally declared UV */
	if (model->voiced == 0)
		if (elow > 10 * ehigh) // >10dB?
			model->voiced = 1;

	/* Look for Type 2 errors, strongly UV speech that has been
	   accidentally declared V */
	if (model->voiced == 1)
	{
		if (elow * UV_LOW_Q20 < ehigh << 20) // <-10dB?
			model->voiced = 0;

		/* A common source of Type 2 errors is the pitch estimator
		   gives a low (50Hz) estimate for UV speech, which gives a
		   good match with noise due to the close harmonic spacing. */
		if ((elow * UV_4DB_Q20 < ehigh << 20) && (model->Wo <= WO_60HZ_Q24)) // <-4dB?
			model->voiced = 0;
	}
}

/* Sn * w with w in Q28, rounded to nearest, result Q16 */
static inline int32_t windowed_sample(int16_t Sn, int32_t w)
{
	int64_t p = (int64_t)Sn * w;
	return (int32_t)((p + (p < 0 ? -2048 : 2048)) >> 12);
}

static void dft_speech(complex_t *Sw, const int16_t *Sn, const int32_t *w)
{
	cpx_q_t *scratch = Sw + 257;

	/* Centre analysis window on time axis; input is Q16 (Sn * w >> 6)
	   and aliases the output region: consumed before output is written */
	int32_t *x = (int32_t *)Sw;
	memset(x, 0, FFT_ENC * sizeof(int32_t));

	for (int i = 0; i < NW / 2; i++)
		x[i] = windowed_sample(Sn[i + M_PITCH / 2], w[i + M_PITCH / 2]);
	for (int i = 0; i < NW / 2; i++)
		x[FFT_ENC - NW / 2 + i] =
			windowed_sample(Sn[i + M_PITCH / 2 - NW / 2],
							w[i + M_PITCH / 2 - NW / 2]);

	fft_real_fwd512(x, Sw, scratch); /* bins 0..256 */

	/* Hermitian mirror - pitch refinement deliberately reads past Nyquist */
	for (int k = 1; k < FFT_ENC / 2; k++)
	{
		Sw[FFT_ENC - k].r = Sw[k].r;
		Sw[FFT_ENC - k].i = -Sw[k].i;
	}
}

void analyse_one_frame(
	codec2_encoder_t *c2,
	model_t *model,
	const int16_t *speech)
{
	complex_t *Sw = c2->fft_buffer; // reuse scratch array
	int32_t A16[MAX_AMP + 1];

	/* Read input speech */
	for (int i = 0; i < M_PITCH - N_SAMP; i++)
		c2->Sn[i] = c2->Sn[i + N_SAMP];
	for (int i = 0; i < N_SAMP; i++)
		c2->Sn[i + M_PITCH - N_SAMP] = speech[i];

	/* Estimate pitch FIRST: nlp()'s FFT scratch shares the tail of the
	   buffer with dft_speech()'s scratch and with the Hermitian mirror
	   of Sw (bins 257..511).  hs_pitch_refinement below deliberately
	   reads a few bins past Nyquist (into that mirror), so the NLP must
	   run before dft_speech writes the mirror, not after. */
	int32_t f0;
	nlp(&c2->nlp, c2->Sn, &f0, &c2->prev_f0_enc, &c2->fft_buffer[257]);
	/* Wo = 2*pi*f0/Fs: Wo_Q24 = (f0_Q16 * TWO_PI_Q24 / 8000) >> 16 */
	model->Wo = (int32_t)((int64_t)f0 * TWO_PI_Q24 / 8000 >> 16);
	model->L = wo_to_L(model->Wo);

	dft_speech(Sw, c2->Sn, c2->w);

	/* estimate model parameters */
	two_stage_pitch_refinement(model, Sw);

	/* estimate phases */
	estimate_amplitudes(model, Sw, A16);
	est_voicing_mbe(model, Sw, c2->W, A16);

	/* store amplitudes in the model at speech Q4 */
	for (int mm = 1; mm <= model->L; mm++)
		model->A[mm] = A16[mm] >> 12;
}

void analysis_init(codec2_encoder_t *c2)
{
	make_analysis_window(c2, c2->w, c2->W);
}
