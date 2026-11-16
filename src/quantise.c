#include "codec2_internal.h"
#include "quantise.h"

static inline unsigned int binary_to_gray(unsigned int x)
{
	return x ^ (x >> 1);
}

static inline unsigned int gray_to_binary(unsigned int g)
{
	g ^= g >> 16;
	g ^= g >> 8;
	g ^= g >> 4;
	g ^= g >> 2;
	g ^= g >> 1;
	return g;
}

void pack(
	uint8_t *bitArray,		/* The output bit array. */
	unsigned int *bitIndex, /* Index into the string in BITS, not bytes. */
	unsigned int field,		/* The bit field to be packed. */
	unsigned int fieldWidth /* Width of the field in BITS, not bytes. */
)
{
	/* Convert to Gray code if multi-bit */
	if (fieldWidth > 1)
		field = binary_to_gray(field);

	do
	{
		unsigned int bI = *bitIndex;
		unsigned int bitsLeft = 8 - (bI & 0x7);
		unsigned int sliceWidth = bitsLeft < fieldWidth ? bitsLeft : fieldWidth;
		unsigned int wordIndex = bI >> 3;

		bitArray[wordIndex] |=
			(uint8_t)((field >> (fieldWidth - sliceWidth)) << (bitsLeft - sliceWidth));

		*bitIndex = bI + sliceWidth;
		fieldWidth -= sliceWidth;

	} while (fieldWidth != 0);
}

int unpack(
	const uint8_t *bitArray, /* The input bit string. */
	unsigned int *bitIndex,	 /* Index into the string in BITS, not bytes. */
	unsigned int fieldWidth	 /* Width of the field in BITS, not bytes. */
)
{
	static const uint8_t mask8[9] = {
		0x00, 0x01, 0x03, 0x07, 0x0F,
		0x1F, 0x3F, 0x7F, 0xFF};

	unsigned int field = 0;
	unsigned int origWidth = fieldWidth;

	do
	{
		unsigned int bI = *bitIndex;
		unsigned int bitsLeft = 8 - (bI & 0x7);
		unsigned int sliceWidth = bitsLeft < fieldWidth ? bitsLeft : fieldWidth;

		field |= (((bitArray[bI >> 3] >> (bitsLeft - sliceWidth)) &
				   mask8[sliceWidth])
				  << (fieldWidth - sliceWidth));

		*bitIndex = bI + sliceWidth;
		fieldWidth -= sliceWidth;
	} while (fieldWidth != 0);

	return (origWidth > 1) ? gray_to_binary(field) : field;
}

int encode_Wo(int32_t Wo, uint8_t bits)
{
	int Wo_levels = 1 << bits;

	/* norm = (Wo - W0_MIN) / (W0_MAX - W0_MIN), all Q24 */
	int64_t num = (int64_t)Wo - W0_MIN_Q24;
	int64_t index = (num * Wo_levels * 2 + WO_RANGE_Q24) / (2 * WO_RANGE_Q24);

	if (index < 0)
		index = 0;
	else if (index > Wo_levels - 1)
		index = Wo_levels - 1;

	return (int)index;
}

int32_t decode_Wo(int index, int bits)
{
	int Wo_levels = 1 << bits;

	/* Wo = W0_MIN + index * range / levels, Q24 */
	return W0_MIN_Q24 + (int32_t)(((int64_t)WO_RANGE_Q24 * index) / Wo_levels);
}

int encode_energy(int32_t Elog, int bits)
{
	const int e_levels = 1 << bits;

	/* e_db (Q16) = Elog * 10*log10(2); index = round((e_db - E_MIN) * levels / 50dB) */
	int64_t e_db = ((int64_t)Elog * LOG2_TO_DB10_Q16) >> 16;
	int64_t num = (e_db - E_MIN_DB_Q16) * e_levels * 2 + 50 * 65536;
	int64_t index = num / (2 * 50 * 65536);

	if (index < 0)
		index = 0;
	else if (index > e_levels - 1)
		index = e_levels - 1;

	return (int)index;
}

int32_t decode_energy(int index, int bits)
{
	/* e_db = E_MIN + index * 50dB/levels (E_DB_STEP_Q16 is exact in
	   Q16 dB since 50*65536/32 = 102400); Elog = e_db / (10*log10 2) */
	int e_levels = 1 << bits;
	int64_t e_db = E_MIN_DB_Q16 + (int64_t)index * ((50 * 65536) / e_levels);

	return (int32_t)((e_db * DB10_TO_LOG2_Q16) >> 16);
}

void encode_lspds_scalar(int *indexes, const int32_t *lsp)
{
	int last_q_hz = 0;

	for (int i = 0; i < LPC_ORD; i++)
	{
		const uint16_t *cb = delta_lsp_cb[i];
		/* lsp_hz = lsp * 4000/pi: lsp_Q24 * 4000 / PI_Q24, exact */
		int lsp_hz = (int)(((int64_t)lsp[i] * 4000 + PI_Q24 / 2) / PI_Q24);
		int target = (i == 0) ? lsp_hz : (lsp_hz - last_q_hz);

		int best_j;

		/* early saturation detection */
		if (target <= (int)cb[0])
		{
			best_j = 0;
		}
		else if (target >= (int)cb[31])
		{
			best_j = 31;
		}
		else
		{
			/* find first j such that cb[j] > target */
			int lo = 0, hi = 31;
			while (lo + 1 < hi)
			{
				int mid = (lo + hi) >> 1;
				if ((int)cb[mid] <= target)
					lo = mid;
				else
					hi = mid;
			}

			int e_lo = cb[lo] > target ? cb[lo] - target : target - cb[lo];
			int e_hi = cb[hi] > target ? cb[hi] - target : target - cb[hi];

			/* lower index wins ties */
			best_j = (e_lo <= e_hi) ? lo : hi;
		}

		indexes[i] = best_j;

		if (i == 0)
			last_q_hz = cb[best_j];
		else
			last_q_hz += cb[best_j];
	}
}

void decode_lspds_scalar(int32_t *lsp_, const int *indexes)
{
	int lsp_hz = 0;

	for (int i = 0; i < LPC_ORD; i++)
	{
		lsp_hz += delta_lsp_cb[i][indexes[i]];
		/* lsp = lsp_hz * pi/4000, Q24 */
		lsp_[i] = (int32_t)(((int64_t)lsp_hz * PI_Q24 + 2000) / 4000);
	}
}

/* ------------------------------------------------------------------ */
/* 1600 bps mode: scalar LSP quantisation, check/bw helpers.          */
/* ------------------------------------------------------------------ */

int lsp_bits(int i)
{
	return lsp_scalar_cb[i].log2m;
}

/* nearest-neighbour scalar quantiser; codebook entries and lsp_hz are
   integer Hz, so the comparison is exact */
static int quantise_scalar(const uint16_t *cb, int x, int m)
{
	int besti = 0;
	int beste = 1 << 30;

	for (int j = 0; j < m; j++)
	{
		int diff = (int)cb[j] - x;
		int e = diff * diff;

		if (e < beste)
		{
			beste = e;
			besti = j;
		}
	}

	return besti;
}

void encode_lsps_scalar(int *indexes, const int32_t *lsp)
{
	int lsp_hz[LPC_ORD];

	/* convert from radians to Hz so we can use human readable
	   frequencies */
	for (int i = 0; i < LPC_ORD; i++)
		lsp_hz[i] = (int)(((int64_t)lsp[i] * 4000 + PI_Q24 / 2) / PI_Q24);

	/* scalar quantisers */

	for (int i = 0; i < LPC_ORD; i++)
		indexes[i] = quantise_scalar(lsp_scalar_cb[i].cb, lsp_hz[i],
				     lsp_scalar_cb[i].m);
}

void decode_lsps_scalar(int32_t *lsp_, const int *indexes)
{
	int lsp_hz[LPC_ORD];

	for (int i = 0; i < LPC_ORD; i++)
		lsp_hz[i] = (int)lsp_scalar_cb[i].cb[indexes[i]];

	/* convert back to radians */

	for (int i = 0; i < LPC_ORD; i++)
		lsp_[i] = (int32_t)(((int64_t)lsp_hz[i] * PI_Q24 + 2000) / 4000);
}

void check_lsp_order(int32_t *lsp)
{
	for (int i = 1; i < LPC_ORD; i++)
		if (lsp[i] < lsp[i - 1])
		{
			int32_t tmp = lsp[i - 1];

			lsp[i - 1] = lsp[i] - LSP_ORDER_SWAP_Q24;
			lsp[i] = tmp + LSP_ORDER_SWAP_Q24;
			i = 1; /* start check again, as swap may have caused out of order */
		}
}

void bw_expand_lsps(int32_t *lsp, int min_sep_low, int min_sep_high)
{
	/* separations are integer Hz; convert to radians Q24 once */
	int32_t sep_low_q24 = (int32_t)(((int64_t)min_sep_low * PI_Q24 + 2000) / 4000);
	int32_t sep_high_q24 = (int32_t)(((int64_t)min_sep_high * PI_Q24 + 2000) / 4000);

	for (int i = 1; i < 4; i++)
	{
		if ((lsp[i] - lsp[i - 1]) < sep_low_q24)
			lsp[i] = lsp[i - 1] + sep_low_q24;
	}

	/* As quantiser gaps increased, larger BW expansion was required
	   to prevent twinkly noises.  This may need more experiment for
	   different quantisers. */

	for (int i = 4; i < LPC_ORD; i++)
	{
		if (lsp[i] - lsp[i - 1] < sep_high_q24)
			lsp[i] = lsp[i - 1] + sep_high_q24;
	}
}
