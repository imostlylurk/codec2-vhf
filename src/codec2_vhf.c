#ifdef C2VHF_DUMP_HOOKS
#include <stdio.h>
#include <stdlib.h>
#endif
#include "codec2_vhf.h"
#include "codec2_internal.h"
#include "analysis.h"
#include "synthesis.h"
#include "nlp.h"
#include "lpc.h"
#include "interp.h"
#include "quantise.h"

void codec2_vhf_init(codec2_t *c2)
{
	codec2_vhf_encoder_init(&c2->encoder);
	codec2_vhf_decoder_init(&c2->decoder);
}

void codec2_vhf_encoder_init(codec2_encoder_t *c2)
{
	for (int i = 0; i < M_PITCH; i++)
		c2->Sn[i] = 1.0f;

	size_t mem;

	mem = sizeof(c2->fftr_fwd_mem);
	c2->fftr_fwd_cfg = kiss_fftr_alloc(FFT_ENC, 0, c2->fftr_fwd_mem, &mem);

	c2->nlp.fftr_cfg = c2->fftr_fwd_cfg;

	analysis_init(c2);

	c2->prev_f0_enc = 1.0f / P_MAX_S;

	nlp_init(&c2->nlp);
}

void codec2_vhf_decoder_init(codec2_decoder_t *c2)
{
	c2->next_rn = 1;

	memset(c2->Sn_, 0, sizeof(c2->Sn_));

	size_t mem;

	mem = sizeof(c2->fftr_fwd_mem);
	c2->fftr_fwd_cfg = kiss_fftr_alloc(FFT_ENC, 0, c2->fftr_fwd_mem, &mem);

	mem = sizeof(c2->fftr_inv_mem);
	c2->fftr_inv_cfg = kiss_fftr_alloc(FFT_DEC, 1, c2->fftr_inv_mem, &mem);

	synthesis_init(c2);

	c2->bg_est = 0.0;
	c2->ex_phase = 0.0;

	for (int l = 1; l <= MAX_AMP; l++)
		c2->prev_model_dec.A[l] = 0.0;

	c2->prev_model_dec.Wo = TWO_PI / P_MAX;
	c2->prev_model_dec.L = M_PI / c2->prev_model_dec.Wo;
	c2->prev_model_dec.voiced = 0;

	for (int i = 0; i < LPC_ORD; i++)
		c2->prev_lsps_dec[i] = i * M_PI / (LPC_ORD + 1);

	c2->prev_e_dec = 1;
}

void codec2_vhf_encode_3200(codec2_encoder_t *c2, uint8_t *bits, const int16_t *speech)
{
	model_t model = {0};
	float ak[LPC_ORD + 1];
	float lsps[LPC_ORD];
	float e;
	int Wo_index, e_index;
	int lspd_indexes[LPC_ORD];
	unsigned int nbit = 0;

	memset(bits, 0, BYTES_PER_FRAME); // 64 bits

	/* first 10ms analysis frame - we just want voicing */
	analyse_one_frame(c2, &model, speech);
	pack(bits, &nbit, model.voiced, 1);

	/* second 10ms analysis frame */
	analyse_one_frame(c2, &model, &speech[N_SAMP]);
	pack(bits, &nbit, model.voiced, 1);
	Wo_index = encode_Wo(model.Wo, WO_BITS);
	pack(bits, &nbit, Wo_index, WO_BITS);

	speech_to_uq_lsps(c2, lsps, ak, &e, c2->Sn, c2->w);
	e_index = encode_energy(e, E_BITS);
	pack(bits, &nbit, e_index, E_BITS);

	encode_lspds_scalar(lspd_indexes, lsps);
	for (int i = 0; i < LSPD_SCALAR_INDEXES; i++)
	{
		pack(bits, &nbit, lspd_indexes[i], 5); // codebook indices are 5 bits
	}
}

void codec2_vhf_decode_3200(codec2_decoder_t *c2, int16_t *speech, const uint8_t *bits)
{
	model_t model[2] = {0};
	int lspd_indexes[LPC_ORD];
	float lsps[2][LPC_ORD];
	int Wo_index, e_index;
	float e[2];
	float ak[2][LPC_ORD + 1];
	complex_t Aw[FFT_ENC / 2 + 1]; /* real FFT output: bins 0..FFT_ENC/2 */
	float A2[FFT_ENC / 2];

	/* unpack bits from channel ------------------------------------*/
	unsigned int nbit = 0;
	/* this will partially fill the model params for the 2 x 10ms
	   frames */
	model[0].voiced = unpack(bits, &nbit, 1);
	model[1].voiced = unpack(bits, &nbit, 1);

	Wo_index = unpack(bits, &nbit, WO_BITS);
	model[1].Wo = decode_Wo(Wo_index, WO_BITS);
	model[1].L = M_PI / model[1].Wo;

	e_index = unpack(bits, &nbit, E_BITS);
	e[1] = decode_energy(e_index, E_BITS);

	for (int i = 0; i < LSPD_SCALAR_INDEXES; i++)
	{
		lspd_indexes[i] = unpack(bits, &nbit, 5); // codebook indices are 5 bits
	}
	decode_lspds_scalar(&lsps[1][0], lspd_indexes);

	/* interpolate ------------------------------------------------*/
	/* Wo and energy are sampled every 20ms, so we interpolate just 1
	   10ms frame between 20ms samples */
	interp_Wo(&model[0], &c2->prev_model_dec, &model[1]);
	e[0] = interp_energy(c2->prev_e_dec, e[1]);

	/* LSPs are sampled every 20ms so we interpolate the frame in
	   between, then recover spectral amplitudes */
	interpolate_lsp(&lsps[0][0], c2->prev_lsps_dec, &lsps[1][0]);

	for (int i = 0; i < 2; i++)
	{
		lsp_to_lpc(&lsps[i][0], &ak[i][0]);
		aks_to_mag2(c2, &ak[i][0], &model[i], e[i], Aw, A2);
		apply_lpc_correction(&model[i]);
		synthesise_one_frame(c2, &speech[N_SAMP * i], &model[i], Aw, 1.0f);
	}

	/* update memories for next frame ----------------------------*/
	c2->prev_model_dec = model[1];
	c2->prev_e_dec = e[1];
	for (int i = 0; i < LPC_ORD; i++)
		c2->prev_lsps_dec[i] = lsps[1][i];
}

/*
  Encodes 320 speech samples (40ms of speech) into 64 bits.

  The codec2 algorithm operates internally on 10ms (80 sample)
  frames, so we run the encoding algorithm 4 times:

  frame 0: voicing bit
  frame 1: voicing bit, Wo and E
  frame 2: voicing bit
  frame 3: voicing bit, Wo and E, scalar LSPs

  The bit allocation is:

    Parameter                      frame 2  frame 4   Total
    -------------------------------------------------------
    Harmonic magnitudes (LSPs)      0       36        36
    Pitch (Wo)                      7        7        14
    Energy                          5        5        10
    Voicing (10ms update)           2        2         4
    TOTAL                          14       50        64
*/
void codec2_vhf_encode_1600(codec2_encoder_t *c2, uint8_t *bits, const int16_t *speech)
{
	model_t model = {0};
	float ak[LPC_ORD + 1];
	float lsps[LPC_ORD];
	float e;
	int Wo_index, e_index;
	int lsp_indexes[LSP_SCALAR_INDEXES];
	unsigned int nbit = 0;

	memset(bits, 0, BYTES_PER_FRAME); // 64 bits

	/* frame 1: - voicing -----------------------------------------*/

	analyse_one_frame(c2, &model, speech);
	pack(bits, &nbit, model.voiced, 1);

	/* frame 2: - voicing, scalar Wo & E --------------------------*/

	analyse_one_frame(c2, &model, &speech[N_SAMP]);
	pack(bits, &nbit, model.voiced, 1);

	Wo_index = encode_Wo(model.Wo, WO_BITS);
	pack(bits, &nbit, Wo_index, WO_BITS);

	/* need to run this just to get LPC energy */
	speech_to_uq_lsps(c2, lsps, ak, &e, c2->Sn, c2->w);
	e_index = encode_energy(e, E_BITS);
	pack(bits, &nbit, e_index, E_BITS);

	/* frame 3: - voicing -----------------------------------------*/

	analyse_one_frame(c2, &model, &speech[2 * N_SAMP]);
	pack(bits, &nbit, model.voiced, 1);

	/* frame 4: - voicing, scalar Wo & E, scalar LSPs -------------*/

	analyse_one_frame(c2, &model, &speech[3 * N_SAMP]);
	pack(bits, &nbit, model.voiced, 1);

	Wo_index = encode_Wo(model.Wo, WO_BITS);
	pack(bits, &nbit, Wo_index, WO_BITS);

	speech_to_uq_lsps(c2, lsps, ak, &e, c2->Sn, c2->w);
	e_index = encode_energy(e, E_BITS);
	pack(bits, &nbit, e_index, E_BITS);

	encode_lsps_scalar(lsp_indexes, lsps);
	for (int i = 0; i < LSP_SCALAR_INDEXES; i++)
	{
		pack(bits, &nbit, lsp_indexes[i], lsp_bits(i));
	}
}

/* Decodes frames of 64 bits into 320 samples (40ms) of speech. */
void codec2_vhf_decode_1600(codec2_decoder_t *c2, int16_t *speech, const uint8_t *bits)
{
	model_t model[4] = {0};
	int lsp_indexes[LSP_SCALAR_INDEXES];
	float lsps[4][LPC_ORD];
	int Wo_index, e_index;
	float e[4];
	float ak[4][LPC_ORD + 1];
	complex_t Aw[FFT_ENC / 2 + 1]; /* real FFT output: bins 0..FFT_ENC/2 */
	float A2[FFT_ENC / 2];
	unsigned int nbit = 0;

	/* unpack bits from channel ------------------------------------*/

	/* this will partially fill the model params for the 4 x 10ms
	   frames */

	model[0].voiced = unpack(bits, &nbit, 1);

	model[1].voiced = unpack(bits, &nbit, 1);
	Wo_index = unpack(bits, &nbit, WO_BITS);
	model[1].Wo = decode_Wo(Wo_index, WO_BITS);
	model[1].L = M_PI / model[1].Wo;

	e_index = unpack(bits, &nbit, E_BITS);
	e[1] = decode_energy(e_index, E_BITS);

	model[2].voiced = unpack(bits, &nbit, 1);

	model[3].voiced = unpack(bits, &nbit, 1);
	Wo_index = unpack(bits, &nbit, WO_BITS);
	model[3].Wo = decode_Wo(Wo_index, WO_BITS);
	model[3].L = M_PI / model[3].Wo;

	e_index = unpack(bits, &nbit, E_BITS);
	e[3] = decode_energy(e_index, E_BITS);

	for (int i = 0; i < LSP_SCALAR_INDEXES; i++)
	{
		lsp_indexes[i] = unpack(bits, &nbit, lsp_bits(i));
	}
	decode_lsps_scalar(&lsps[3][0], lsp_indexes);
	check_lsp_order(&lsps[3][0]);
	bw_expand_lsps(&lsps[3][0], 50.0f, 100.0f);

	/* interpolate ------------------------------------------------*/

	/* Wo and energy are sampled every 20ms, so we interpolate just 1
	   10ms frame between 20ms samples */

	interp_Wo(&model[0], &c2->prev_model_dec, &model[1]);
	e[0] = interp_energy(c2->prev_e_dec, e[1]);
	interp_Wo(&model[2], &model[1], &model[3]);
	e[2] = interp_energy(e[1], e[3]);

	/* LSPs are sampled every 40ms so we interpolate the 3 frames in
	   between, then recover spectral amplitudes */

	for (int i = 0; i < 3; i++)
	{
		float weight = 0.25f * (i + 1); /* 0.25, 0.5, 0.75 exactly */
		interpolate_lsp_ver2(&lsps[i][0], c2->prev_lsps_dec, &lsps[3][0], weight);
	}
	#ifdef C2VHF_DUMP_HOOKS
	if (getenv("C2VHF_DUMP")) {
		for (int i = 0; i < 4; i++)
			fprintf(stderr, "SF1600 %d v%d Wo %.9g e %.9g L %d l0 %.9g l1 %.9g l2 %.9g l3 %.9g l4 %.9g l5 %.9g l6 %.9g l7 %.9g l8 %.9g l9 %.9g\n",
				i, model[i].voiced, model[i].Wo, e[i], model[i].L,
				lsps[i][0], lsps[i][1], lsps[i][2], lsps[i][3], lsps[i][4],
				lsps[i][5], lsps[i][6], lsps[i][7], lsps[i][8], lsps[i][9]);
	}
	#endif

	for (int i = 0; i < 4; i++)
	{
		lsp_to_lpc(&lsps[i][0], &ak[i][0]);
		aks_to_mag2(c2, &ak[i][0], &model[i], e[i], Aw, A2);
		apply_lpc_correction(&model[i]);
		synthesise_one_frame(c2, &speech[N_SAMP * i], &model[i], Aw, 1.0f);
	}

	/* update memories for next frame ----------------------------*/

	c2->prev_model_dec = model[3];
	c2->prev_e_dec = e[3];
	for (int i = 0; i < LPC_ORD; i++)
		c2->prev_lsps_dec[i] = lsps[3][i];
}

/*
  Encodes 320 speech samples (40ms of speech) into 52 bits (1300 bps).

  The codec2 algorithm operates internally on 10ms (80 sample)
  frames, so we run the encoding algorithm 4 times:

  frame 0: voicing bit
  frame 1: voicing bit
  frame 2: voicing bit
  frame 3: voicing bit, Wo and E, scalar LSPs

  Wo, energy and LSPs are all sampled every 40ms; the decoder
  interpolates the three in-between 10ms frames.

  The bit allocation is:

    Parameter                      frame 2  frame 4   Total
    -------------------------------------------------------
    Harmonic magnitudes (LSPs)      0       36        36
    Pitch (Wo)                      0        7         7
    Energy                          0        5         5
    Voicing (10ms update)           2        2         4
    TOTAL                          2       50        52
*/
void codec2_vhf_encode_1300(codec2_encoder_t *c2, uint8_t *bits, const int16_t *speech)
{
	model_t model = {0};
	float ak[LPC_ORD + 1];
	float lsps[LPC_ORD];
	float e;
	int Wo_index, e_index;
	int lsp_indexes[LSP_SCALAR_INDEXES];
	unsigned int nbit = 0;

	memset(bits, 0, CODEC2_VHF_BYTES_PER_FRAME_1300); // 52 bits

	/* frame 1: - voicing -----------------------------------------*/

	analyse_one_frame(c2, &model, speech);
	pack(bits, &nbit, model.voiced, 1);

	/* frame 2: - voicing -----------------------------------------*/

	analyse_one_frame(c2, &model, &speech[N_SAMP]);
	pack(bits, &nbit, model.voiced, 1);

	/* frame 3: - voicing -----------------------------------------*/

	analyse_one_frame(c2, &model, &speech[2 * N_SAMP]);
	pack(bits, &nbit, model.voiced, 1);

	/* frame 4: - voicing, scalar Wo & E, scalar LSPs -------------*/

	analyse_one_frame(c2, &model, &speech[3 * N_SAMP]);
	pack(bits, &nbit, model.voiced, 1);

	Wo_index = encode_Wo(model.Wo, WO_BITS);
	pack(bits, &nbit, Wo_index, WO_BITS);

	speech_to_uq_lsps(c2, lsps, ak, &e, c2->Sn, c2->w);
	e_index = encode_energy(e, E_BITS);
	pack(bits, &nbit, e_index, E_BITS);

	encode_lsps_scalar(lsp_indexes, lsps);
	for (int i = 0; i < LSP_SCALAR_INDEXES; i++)
	{
		pack(bits, &nbit, lsp_indexes[i], lsp_bits(i));
	}
}

/* Decodes frames of 52 bits into 320 samples (40ms) of speech. */
void codec2_vhf_decode_1300(codec2_decoder_t *c2, int16_t *speech, const uint8_t *bits)
{
	model_t model[4] = {0};
	int lsp_indexes[LSP_SCALAR_INDEXES];
	float lsps[4][LPC_ORD];
	int Wo_index, e_index;
	float e[4];
	float ak[4][LPC_ORD + 1];
	complex_t Aw[FFT_ENC / 2 + 1]; /* real FFT output: bins 0..FFT_ENC/2 */
	float A2[FFT_ENC / 2];
	unsigned int nbit = 0;

	/* unpack bits from channel ------------------------------------*/

	/* this will partially fill the model params for the 4 x 10ms
	   frames */

	model[0].voiced = unpack(bits, &nbit, 1);
	model[1].voiced = unpack(bits, &nbit, 1);
	model[2].voiced = unpack(bits, &nbit, 1);
	model[3].voiced = unpack(bits, &nbit, 1);

	Wo_index = unpack(bits, &nbit, WO_BITS);
	model[3].Wo = decode_Wo(Wo_index, WO_BITS);
	model[3].L = M_PI / model[3].Wo;

	e_index = unpack(bits, &nbit, E_BITS);
	e[3] = decode_energy(e_index, E_BITS);

	for (int i = 0; i < LSP_SCALAR_INDEXES; i++)
	{
		lsp_indexes[i] = unpack(bits, &nbit, lsp_bits(i));
	}
	decode_lsps_scalar(&lsps[3][0], lsp_indexes);
	check_lsp_order(&lsps[3][0]);
	bw_expand_lsps(&lsps[3][0], 50.0f, 100.0f);

	/* interpolate ------------------------------------------------*/

	/* Wo, energy, and LSPs are sampled every 40ms so we interpolate
	   the 3 frames in between */

	for (int i = 0; i < 3; i++)
	{
		float weight = 0.25f * (i + 1); /* 0.25, 0.5, 0.75 exactly */
		interpolate_lsp_ver2(&lsps[i][0], c2->prev_lsps_dec, &lsps[3][0], weight);
		interp_Wo2(&model[i], &c2->prev_model_dec, &model[3], weight);
		e[i] = interp_energy2(c2->prev_e_dec, e[3], weight);
	}
	#ifdef C2VHF_DUMP_HOOKS
	if (getenv("C2VHF_DUMP")) {
		for (int i = 0; i < 4; i++)
			fprintf(stderr, "SF1300 %d v%d Wo %.9g e %.9g L %d l0 %.9g l1 %.9g l2 %.9g l3 %.9g l4 %.9g l5 %.9g l6 %.9g l7 %.9g l8 %.9g l9 %.9g\n",
				i, model[i].voiced, model[i].Wo, e[i], model[i].L,
				lsps[i][0], lsps[i][1], lsps[i][2], lsps[i][3], lsps[i][4],
				lsps[i][5], lsps[i][6], lsps[i][7], lsps[i][8], lsps[i][9]);
	}
	#endif

	/* then recover spectral amplitudes */

	for (int i = 0; i < 4; i++)
	{
		lsp_to_lpc(&lsps[i][0], &ak[i][0]);
		aks_to_mag2(c2, &ak[i][0], &model[i], e[i], Aw, A2);
		apply_lpc_correction(&model[i]);
		synthesise_one_frame(c2, &speech[N_SAMP * i], &model[i], Aw, 1.0f);
	}

	/* update memories for next frame ----------------------------*/

	c2->prev_model_dec = model[3];
	c2->prev_e_dec = e[3];
	for (int i = 0; i < LPC_ORD; i++)
		c2->prev_lsps_dec[i] = lsps[3][i];
}
