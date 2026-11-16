#include "codec2_internal.h"
#include "synthesis.h"
#include "util.h"
#include <string.h>
#ifdef C2VHF_DUMP_HOOKS
#include <stdlib.h>
#include <stdio.h>
int c2vhf_dump_on(void);
int c2vhf_dump_on(void) { return getenv("C2VHF_DUMP") != 0; }
#endif

static void make_synthesis_window(int16_t *restrict Pn)
{
	static const int n0 = N_SAMP / 2;
	static const int n1 = 3 * N_SAMP / 2;

	/* Generate Parzen window in time domain, Q15: ramp value is
	   k/(2*TW) * 2^15 = k * 32768 / (2*TW) */
	/* zero */
	memset(Pn, 0, (n0 - TW) * sizeof(int16_t));

	/* ramp up */
	for (int i = n0 - TW; i < n0 + TW; i++)
	{
		int v = (i - (n0 - TW)) * 32768 / (2 * TW);
		Pn[i] = (int16_t)(v > 32767 ? 32767 : v);
	}

	/* flat */
	for (int i = n0 + TW; i < n1 - TW; i++)
		Pn[i] = 32767;

	/* ramp down */
	for (int i = n1 - TW; i < n1 + TW; i++)
	{
		int v = ((n1 + TW) - i) * 32768 / (2 * TW);
		Pn[i] = (int16_t)(v > 32767 ? 32767 : v);
	}

	/* zero */
	memset(&Pn[n1 + TW], 0, (2 * N_SAMP - (n1 + TW)) * sizeof(int16_t));
}

static void sample_phase(
	const model_t *restrict model,
	complex_t *restrict H, /* out, Q24 */
	const complex_t *restrict A /* LPC analysis filter in freq domain, Q24 */
)
{
	/* harmonic centre bin, Q20 */
	const int32_t wo_bin_q20 = (int32_t)(((int64_t)model->Wo * FFT_BINS_PER_RAD_Q24) >> 28);

	/* Sample phase at harmonics */
	for (int m = 1; m <= model->L; m++)
	{
		int b = (int)(((int64_t)m * wo_bin_q20 + (1 << 19)) >> 20);

		// clamp b
		if (b >= FFT_ENC / 2)
			b = FFT_ENC / 2 - 1;

		/* synth filter 1/A is opposite phase to analysis filter */
		H[m].r = A[b].r;
		H[m].i = -A[b].i;
	}
}

static void phase_synth_zero_order(
	codec2_decoder_t *c2,
	model_t *model,
	uint32_t *ex_phase, /* excitation phase of fundamental */
	const complex_t *H, /* L synthesis filter freq domain samples, Q24 */
	complex_t *P        /* out: unit phasors, Q31 */
)
{
	/*
	   Update excitation fundamental phase track; the phase wraps
	   naturally at 2^32 == 2*pi.
	*/
	int32_t wo_phase = (int32_t)(((int64_t)model->Wo * PHASE_PER_RAD_Q24) >> 24);
	*ex_phase += (uint32_t)wo_phase * N_SAMP;
	uint32_t phi0 = *ex_phase;

	for (int m = 1; m <= model->L; m++)
	{
		uint32_t ph = model->voiced ? phi0 * (uint32_t)m
					    : (uint32_t)codec2_rand(&c2->next_rn) * PHASE_PER_RAND;
		int32_t Exr, Exi;
		fp_sincos_q31(ph, &Exi, &Exr);

		/* A = H*Ex: H is Q24, Ex is Q31 -> A is Q24 */
		int64_t Ar = ((int64_t)H[m].r * Exr - (int64_t)H[m].i * Exi) >> 31;
		int64_t Ai = ((int64_t)H[m].i * Exr + (int64_t)H[m].r * Exi) >> 31;

		/* normalise to a unit phasor: P = A/|A| * 2^31 = (A << 31)/|A|
		   with |A| = isqrt(Ar^2+Ai^2) in Q24; the Q24 result shifted
		   into Q31 needs << 7 after a Q24-division.  NOTE: the two
		   shifts total x2^31 -- an extra factor here saturates every
		   component to +-2^31, giving corner-of-square phasors with
		   sqrt(2) magnitude and 4-level phase (audible buzz). */
		uint64_t mag = fp_isqrt64((uint64_t)(Ar * Ar + Ai * Ai));
		if (mag == 0)
			mag = 1;
		int64_t pr = ((Ar * 16777216LL / (int64_t)mag) * 128);
		int64_t pi_ = ((Ai * 16777216LL / (int64_t)mag) * 128);

		P[m].r = fp_sat32(pr);
		P[m].i = fp_sat32(pi_);
	}
}

static void postfilter(codec2_decoder_t *restrict c2, model_t *restrict model, int32_t *restrict bg_est, complex_t *restrict P)
{
	/* determine average energy across spectrum; A is Q4 (speech * 16),
	   so scale back to speech units before the dB conversion */
	int64_t e = 1;
	for (int m = 1; m <= model->L; m++)
		e += (int64_t)model->A[m] * model->A[m];

	/* e_db = 10*log10(e/(256*L)): log2 domain then to dB Q16; the 256
	   (2^8) converts the Q4-squared sum back to speech units */
	int64_t e_db = (int64_t)(fp_log2_q16(e) - (8 << 16) - fp_log2_q16((int64_t)model->L)) * LOG2_TO_DB10_Q16 >> 16;

	/* If beneath threshold, update bg estimate.  The idea
	   of the threshold is to prevent updating during high level
	   speech. */
	if (!model->voiced)
	{
		if (e_db < BG_THRESH_DB_Q16)
			*bg_est = (int32_t)(*bg_est * 9 + e_db) / 10;
	}
	else
	{
		if (1) return; /* NOISE-DIAG */
		/* now mess with phases during voiced frames to make any harmonics
		   less then our background estimate unvoiced. */

		/* thresh = 10^((bg_est + margin)/20) = 2^((bg+margin)*log2(10)/20) */
		int64_t l2 = ((*bg_est + BG_MARGIN_DB_Q16) * DB20_TO_LOG2_Q16) >> 16;
		int64_t thresh_q31 = fp_exp2_q31((int32_t)l2); /* Q31 of true value */
		int32_t thresh_q4 = (int32_t)(thresh_q31 >> 27); /* speech Q4 */

		for (int m = 1; m <= model->L; m++)
		{
			if (model->A[m] < thresh_q4)
			{
				uint32_t ph = (uint32_t)codec2_rand(&c2->next_rn) * PHASE_PER_RAND;
				int32_t sr, cr;
				fp_sincos_q31(ph, &sr, &cr);
				P[m].i = sr;
				P[m].r = cr;
			}
		}
	}
}

static void ear_protection(int32_t *in_out, int n)
{
	int32_t max_abs = 0;

	/* find maximum sample in frame (Q8) */
	for (int i = 0; i < n; i++)
	{
		int32_t tmp = in_out[i] < 0 ? -in_out[i] : in_out[i];
		if (tmp > max_abs)
			max_abs = tmp;
	}


	if (max_abs <= 30000 << 8)
		return; // nothing to do here

	/* determine how far above set point; max_abs is Q8, so the ratio
	   (30000/max) in Q8 needs the numerator in Q16.  A Q0 ratio would
	   truncate to zero whenever attenuation fires, zeroing the frame
	   (the loud-speech dropouts). */
	int32_t r_q8 = (int32_t)(((int64_t)(30000 << 8) << 8) / max_abs); /* ratio <= 1, Q8 */

	/* If we are x dB over set point we reduce level by 2x dB */
	int64_t gain_q16 = (int64_t)r_q8 * r_q8; /* Q16 */

	for (int i = 0; i < n; i++)
		in_out[i] = (int32_t)(((int64_t)in_out[i] * gain_q16) >> 16);
}

static void synthesise(
	codec2_decoder_t *c2,
	int32_t *Sn_,			/* time domain synthesised signal, Q8 */
	const model_t *restrict model, /* ptr to model parameters for this frame */
	const int16_t *restrict Pn,	/* time domain Parzen window, Q15 */
	const complex_t *restrict P,	/* harmonic phasors, Q31 */
	int shift			/* flag used to handle transition frames */
)
{
	/* Sw_ lives in the decoder scratch: 257 bins out + 256 scratch */
	cpx_q_t *Sw_ = c2->fft_buffer;
	cpx_q_t *scratch = c2->fft_buffer + 257;
	int32_t *sw_ = (int32_t *)(c2->fft_buffer + 513); /* 512 int32 output */

	if (shift)
	{
		/* Update memories */
		memmove(Sn_, &Sn_[N_SAMP], (N_SAMP - 1) * sizeof(int32_t));
		Sn_[N_SAMP - 1] = 0;
	}

	memset(Sw_, 0, (FFT_DEC / 2 + 1) * sizeof(complex_t));

	/* Now set up frequency domain synthesised speech */
	int32_t wo_bin_q20 = (int32_t)(((int64_t)model->Wo * FFT_BINS_PER_RAD_Q24) >> 28);

	for (int l = 1; l <= model->L; l++)
	{
		int b = (int)(((int64_t)l * wo_bin_q20 + (1 << 19)) >> 20);
		if (b > ((FFT_DEC / 2) - 1))
		{
			b = (FFT_DEC / 2) - 1;
		}
		Sw_[b].r = fp_sat32(((int64_t)model->A[l] * P[l].r) >> 31); /* Q4 */
		Sw_[b].i = fp_sat32(((int64_t)model->A[l] * P[l].i) >> 31);
	}

	/* Perform inverse DFT: unscaled, output in speech Q4 */
	fft_real_inv512(Sw_, sw_, scratch);
#ifdef C2VHF_DUMP_HOOKS
	{
		extern int c2vhf_dump_on(void);
		if (c2vhf_dump_on()) {
			int32_t msw = 0, mswv = 0, msn = 0;
			for (int b = 0; b <= 256; b++) {
				int32_t r = Sw_[b].r < 0 ? -Sw_[b].r : Sw_[b].r;
				int32_t i = Sw_[b].i < 0 ? -Sw_[b].i : Sw_[b].i;
				if (r > msw) msw = r;
				if (i > msw) msw = i;
			}
			for (int n = 0; n < FFT_DEC; n++) {
				int32_t v = sw_[n] < 0 ? -sw_[n] : sw_[n];
				if (v > mswv) mswv = v;
			}
			for (int n = 0; n < N_SAMP; n++) {
				int32_t v = Sn_[n] < 0 ? -Sn_[n] : Sn_[n];
				if (v > msn) msn = v;
			}
			{
				int64_t psw = 0, pswv = 0;
				for (int b = 0; b <= 256; b++)
					psw += (int64_t)Sw_[b].r * Sw_[b].r + (int64_t)Sw_[b].i * Sw_[b].i;
				for (int n = 0; n < FFT_DEC; n++)
					pswv += (int64_t)sw_[n] * sw_[n];
				fprintf(stderr, "OLA maxSw %d maxsw_ %d maxSn %d pwSw2 %lld pwsw %lld H:",
				        msw, mswv, msn, (long long)psw, (long long)pswv);
				for (int l = 1; l <= 6 && l <= model->L; l++) {
					int b = (int)(((int64_t)l * ((int32_t)(((int64_t)model->Wo * FFT_BINS_PER_RAD_Q24) >> 28)) + (1 << 19)) >> 20);
					if (b > 255) b = 255;
					fprintf(stderr, " %d:A%d/S%d", b, model->A[l] >> 4,
					        (int)(fp_isqrt64((uint64_t)Sw_[b].r * Sw_[b].r + (uint64_t)Sw_[b].i * Sw_[b].i)));
				}
				fprintf(stderr, "\n");
			}
		}
	}
#endif

	/* Overlap add to previous samples: sw_(Q4) * Pn(Q15) >> 11 -> Q8.
	   The product can reach 2^20*32767, so it must be evaluated in
	   64-bit: in 32-bit it wraps (wrong-signed samples on loud
	   speech) */
	for (int i = 0; i < N_SAMP - 1; i++)
	{
		Sn_[i] = fp_sat_add32(Sn_[i], ((int64_t)sw_[FFT_DEC - N_SAMP + 1 + i] * Pn[i]) >> 11);
	}

	int32_t *restrict dst = &Sn_[N_SAMP - 1];
	const int32_t *restrict src = sw_;
	const int16_t *restrict win = &Pn[N_SAMP - 1];

	if (shift)
	{
		for (int j = 0; j < N_SAMP + 1; j++)
			dst[j] = ((int64_t)src[j] * win[j]) >> 11;
	}
	else
	{
		for (int j = 0; j < N_SAMP + 1; j++)
			dst[j] = fp_sat_add32(dst[j], ((int64_t)src[j] * win[j]) >> 11);
	}
}

void synthesise_one_frame(
	codec2_decoder_t *c2,
	int16_t *speech,
	model_t *model)
{
	/* Aw, the A(z) spectrum aks_to_mag2() left behind, sits at the
	   start of the FFT scratch and is consumed by sample_phase().
	   H and P then share the 81 slots behind it (phase_synth_zero_order
	   reads H[m] before writing P[m], so the in-place reuse is safe);
	   both are dead again before synthesise() needs the same region
	   as its IFFT scratch. */
	complex_t *Aw = c2->fft_buffer;
	complex_t *HP = &c2->fft_buffer[257];

	sample_phase(model, HP, Aw);
	phase_synth_zero_order(c2, model, &c2->ex_phase, HP, HP);
	postfilter(c2, model, &c2->bg_est, HP);
#ifdef C2VHF_DUMP_HOOKS
	{
		extern int c2vhf_dump_on(void);
		if (c2vhf_dump_on()) {
			int32_t mh = 0, mp = 0;
			for (int m = 1; m <= model->L; m++) {
				int32_t hr = HP[m].r < 0 ? -HP[m].r : HP[m].r;
				int32_t hi = HP[m].i < 0 ? -HP[m].i : HP[m].i;
				if (hr > mh) mh = hr;
				if (hi > mh) mh = hi;
			}
			for (int b = 0; b <= 256; b++) {
				int32_t ar = Aw[b].r < 0 ? -Aw[b].r : Aw[b].r;
				int32_t ai = Aw[b].i < 0 ? -Aw[b].i : Aw[b].i;
				if (ar > mp) mp = ar;
				if (ai > mp) mp = ai;
			}
			fprintf(stderr, "SYN L %d v%d Wo %.6g maxAw %d maxP %d",
			        model->L, model->voiced, model->Wo / 16777216.0, mp, mh);
			for (int m = 1; m <= 3 && m <= model->L; m++)
				fprintf(stderr, " P%d:(%d,%d|%lld)", m, HP[m].r, HP[m].i,
				        (long long)fp_isqrt64((uint64_t)HP[m].r * HP[m].r + (uint64_t)HP[m].i * HP[m].i));
			fprintf(stderr, "\n");
		}
	}
#endif
	synthesise(c2, c2->Sn_, model, c2->Pn, HP, 1);

	/* ear protection on the overlap-add output (Q8) */
	ear_protection(c2->Sn_, N_SAMP);

	for (int i = 0; i < N_SAMP; i++)
	{
		int32_t v = c2->Sn_[i] >> 8;
		if (v > 32767)
			speech[i] = 32767;
		else if (v < -32768)
			speech[i] = -32768;
		else
			speech[i] = (int16_t)v;
	}
}

void synthesis_init(codec2_decoder_t *c2)
{
	make_synthesis_window(c2->Pn);
}
