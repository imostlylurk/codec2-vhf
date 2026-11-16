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
		c2->Sn[i] = 1;

	analysis_init(c2);

	c2->prev_f0_enc = 50 << 16; /* 50 Hz, Q16 (1/P_MAX_S) */

	nlp_init(&c2->nlp);
}

void codec2_vhf_decoder_init(codec2_decoder_t *c2)
{
	c2->next_rn = 1;

	memset(c2->Sn_, 0, sizeof(c2->Sn_));

	synthesis_init(c2);

	c2->bg_est = 0;
	c2->ex_phase = 0;

	for (int l = 1; l <= MAX_AMP; l++)
		c2->prev_model_dec.A[l] = 0;

	c2->prev_model_dec.Wo = W0_MIN_Q24;
	c2->prev_model_dec.L = wo_to_L(c2->prev_model_dec.Wo);
	c2->prev_model_dec.voiced = 0;

	for (int i = 0; i < LPC_ORD; i++)
		c2->prev_lsps_dec[i] = (int32_t)((int64_t)i * LSP_INIT_Q24);

	c2->prev_e_dec = 0; /* 1.0 in log2 domain */
}

void codec2_vhf_encode_3200(codec2_encoder_t *c2, uint8_t *bits, const int16_t *speech)
{
	model_t model = {0};
	int32_t ak[LPC_ORD + 1];
	int32_t lsps[LPC_ORD];
	int32_t Elog;
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

	speech_to_uq_lsps(c2, lsps, ak, &Elog, c2->Sn, c2->w);
	e_index = encode_energy(Elog, E_BITS);
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
	int32_t lsps[2][LPC_ORD];
	int Wo_index, e_index;
	int32_t e[2];
	int32_t ak[2][LPC_ORD + 1];

	/* unpack bits from channel ------------------------------------*/
	unsigned int nbit = 0;
	/* this will partially fill the model params for the 2 x 10ms
	   frames */
	model[0].voiced = unpack(bits, &nbit, 1);
	model[1].voiced = unpack(bits, &nbit, 1);

	Wo_index = unpack(bits, &nbit, WO_BITS);
	model[1].Wo = decode_Wo(Wo_index, WO_BITS);
	model[1].L = wo_to_L(model[1].Wo);

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
		aks_to_mag2(c2, &ak[i][0], &model[i], e[i]);
		apply_lpc_correction(&model[i]);
		synthesise_one_frame(c2, &speech[N_SAMP * i], &model[i]);
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
*/
void codec2_vhf_encode_1600(codec2_encoder_t *c2, uint8_t *bits, const int16_t *speech)
{
	model_t model = {0};
	int32_t ak[LPC_ORD + 1];
	int32_t lsps[LPC_ORD];
	int32_t Elog;
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
	speech_to_uq_lsps(c2, lsps, ak, &Elog, c2->Sn, c2->w);
	e_index = encode_energy(Elog, E_BITS);
	pack(bits, &nbit, e_index, E_BITS);

	/* frame 3: - voicing -----------------------------------------*/

	analyse_one_frame(c2, &model, &speech[2 * N_SAMP]);
	pack(bits, &nbit, model.voiced, 1);

	/* frame 4: - voicing, scalar Wo & E, scalar LSPs -------------*/

	analyse_one_frame(c2, &model, &speech[3 * N_SAMP]);
	pack(bits, &nbit, model.voiced, 1);

	Wo_index = encode_Wo(model.Wo, WO_BITS);
	pack(bits, &nbit, Wo_index, WO_BITS);

	speech_to_uq_lsps(c2, lsps, ak, &Elog, c2->Sn, c2->w);
	e_index = encode_energy(Elog, E_BITS);
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
	/* Subframes are decoded one at a time so only the sampled (20ms)
	   frame parameters need to live across the frame; the interpolated
	   parameters reuse a single working model/LSP buffer. */
	model_t model_i = {0};                /* working model, one subframe */
	model_t model_1 = {0}, model_3 = {0}; /* sampled Wo/L/voiced */
	int lsp_indexes[LSP_SCALAR_INDEXES];
	int32_t ak_i[LPC_ORD + 1];
	int32_t lsp_i[LPC_ORD];
	int32_t lsps_3[LPC_ORD]; /* sampled LSPs, 40 ms */
	int Wo_index, e_index;
	int32_t e_1, e_3, e_i;
	int voiced_0, voiced_2; /* interpolated frames' own voicing bits */
	unsigned int nbit = 0;

	/* unpack bits from channel ------------------------------------*/

	/* this will partially fill the model params for the 4 x 10ms
	   frames */

	voiced_0 = unpack(bits, &nbit, 1);

	model_1.voiced = unpack(bits, &nbit, 1);
	Wo_index = unpack(bits, &nbit, WO_BITS);
	model_1.Wo = decode_Wo(Wo_index, WO_BITS);
	model_1.L = wo_to_L(model_1.Wo);

	e_index = unpack(bits, &nbit, E_BITS);
	e_1 = decode_energy(e_index, E_BITS);

	voiced_2 = unpack(bits, &nbit, 1);

	model_3.voiced = unpack(bits, &nbit, 1);
	Wo_index = unpack(bits, &nbit, WO_BITS);
	model_3.Wo = decode_Wo(Wo_index, WO_BITS);
	model_3.L = wo_to_L(model_3.Wo);

	e_index = unpack(bits, &nbit, E_BITS);
	e_3 = decode_energy(e_index, E_BITS);

	for (int i = 0; i < LSP_SCALAR_INDEXES; i++)
	{
		lsp_indexes[i] = unpack(bits, &nbit, lsp_bits(i));
	}
	decode_lsps_scalar(&lsps_3[0], lsp_indexes);
	check_lsp_order(&lsps_3[0]);
	bw_expand_lsps(&lsps_3[0], 50, 100);

	/* interpolate and synthesise ----------------------------------*/

	/* Wo and energy are sampled every 20ms, so we interpolate just 1
	   10ms frame between 20ms samples; LSPs are sampled every 40ms so
	   we interpolate the 3 frames in between */

	for (int i = 0; i < 4; i++)
	{
		if (i == 0)
		{
			model_i.voiced = voiced_0;
			interp_Wo(&model_i, &c2->prev_model_dec, &model_1);
			e_i = interp_energy(c2->prev_e_dec, e_1);
		}
		else if (i == 1)
		{
			model_i = model_1;
			e_i = e_1;
		}
		else if (i == 2)
		{
			model_i.voiced = voiced_2;
			interp_Wo(&model_i, &model_1, &model_3);
			e_i = interp_energy(e_1, e_3);
		}
		else
		{
			model_i = model_3;
			e_i = e_3;
		}

		if (i < 3)
			interpolate_lsp_ver2(&lsp_i[0], c2->prev_lsps_dec, &lsps_3[0],
					     (i + 1) << 14); /* 0.25, 0.5, 0.75 exactly, Q16 */
		else
			for (int j = 0; j < LPC_ORD; j++)
				lsp_i[j] = lsps_3[j];

		lsp_to_lpc(&lsp_i[0], &ak_i[0]);
		aks_to_mag2(c2, &ak_i[0], &model_i, e_i);
		apply_lpc_correction(&model_i);
		synthesise_one_frame(c2, &speech[N_SAMP * i], &model_i);
	}

	/* update memories for next frame ----------------------------*/

	c2->prev_model_dec = model_i;
	c2->prev_e_dec = e_3;
	for (int i = 0; i < LPC_ORD; i++)
		c2->prev_lsps_dec[i] = lsps_3[i];
}

/*
  Encodes 320 speech samples (40ms of speech) into 52 bits (1300 bps).

  Wo, energy and LSPs are all sampled every 40ms; the decoder
  interpolates the three in-between 10ms frames.
*/
void codec2_vhf_encode_1300(codec2_encoder_t *c2, uint8_t *bits, const int16_t *speech)
{
	model_t model = {0};
	int32_t ak[LPC_ORD + 1];
	int32_t lsps[LPC_ORD];
	int32_t Elog;
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

	speech_to_uq_lsps(c2, lsps, ak, &Elog, c2->Sn, c2->w);
	e_index = encode_energy(Elog, E_BITS);
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
	/* Subframes are decoded one at a time so only the sampled (40ms)
	   frame parameters need to live across the frame; the three
	   interpolated subframes reuse a single working model/LSP buffer. */
	model_t model_i = {0};   /* working model, one subframe */
	model_t model_3 = {0};   /* sampled Wo/L/voiced, 40 ms */
	int lsp_indexes[LSP_SCALAR_INDEXES];
	int32_t ak_i[LPC_ORD + 1];
	int32_t lsp_i[LPC_ORD];
	int32_t lsps_3[LPC_ORD]; /* sampled LSPs, 40 ms */
	int Wo_index, e_index;
	int32_t e_3, e_i;
	int voiced_i[3]; /* interpolated frames' own voicing bits */
	unsigned int nbit = 0;

	/* unpack bits from channel ------------------------------------*/

	/* this will partially fill the model params for the 4 x 10ms
	   frames */

	voiced_i[0] = unpack(bits, &nbit, 1);
	voiced_i[1] = unpack(bits, &nbit, 1);
	voiced_i[2] = unpack(bits, &nbit, 1);
	model_3.voiced = unpack(bits, &nbit, 1);

	Wo_index = unpack(bits, &nbit, WO_BITS);
	model_3.Wo = decode_Wo(Wo_index, WO_BITS);
	model_3.L = wo_to_L(model_3.Wo);

	e_index = unpack(bits, &nbit, E_BITS);
	e_3 = decode_energy(e_index, E_BITS);

	for (int i = 0; i < LSP_SCALAR_INDEXES; i++)
	{
		lsp_indexes[i] = unpack(bits, &nbit, lsp_bits(i));
	}
	decode_lsps_scalar(&lsps_3[0], lsp_indexes);
	check_lsp_order(&lsps_3[0]);
	bw_expand_lsps(&lsps_3[0], 50, 100);

	/* interpolate and synthesise ----------------------------------*/

	/* Wo, energy, and LSPs are sampled every 40ms so we interpolate
	   the 3 frames in between */

	for (int i = 0; i < 4; i++)
	{
		if (i < 3)
		{
			int32_t weight = (i + 1) << 14; /* 0.25, 0.5, 0.75 exactly, Q16 */
			model_i.voiced = voiced_i[i];
			interp_Wo2(&model_i, &c2->prev_model_dec, &model_3, weight);
			e_i = interp_energy2(c2->prev_e_dec, e_3, weight);
			interpolate_lsp_ver2(&lsp_i[0], c2->prev_lsps_dec, &lsps_3[0], weight);
		}
		else
		{
			model_i = model_3;
			e_i = e_3;
			for (int j = 0; j < LPC_ORD; j++)
				lsp_i[j] = lsps_3[j];
		}

		lsp_to_lpc(&lsp_i[0], &ak_i[0]);
		aks_to_mag2(c2, &ak_i[0], &model_i, e_i);
		apply_lpc_correction(&model_i);
		synthesise_one_frame(c2, &speech[N_SAMP * i], &model_i);
	}

	/* update memories for next frame ----------------------------*/

	c2->prev_model_dec = model_i;
	c2->prev_e_dec = e_3;
	for (int i = 0; i < LPC_ORD; i++)
		c2->prev_lsps_dec[i] = lsps_3[i];
}
