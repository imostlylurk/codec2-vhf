/* Scalar LSP codebooks for the 1600 bps mode.
 * Values in Hz, from drowe67/codec2 src/codebook/lsp{1..10}.txt.
 * This work uses code written by David Rowe VK5DGR et al.
 * LGPL 2.1 - see LICENSE.
 */

#include "codec2_internal.h"
#include "quantise.h"

static const uint16_t lsp_scalar_cb1[16] = {
	225, 250, 275, 300, 325, 350, 375, 400,
	425, 450, 475, 500, 525, 550, 575, 600,
};

static const uint16_t lsp_scalar_cb2[16] = {
	325, 350, 375, 400, 425, 450, 475, 500,
	525, 550, 575, 600, 625, 650, 675, 700,
};

static const uint16_t lsp_scalar_cb3[16] = {
	500, 550, 600, 650, 700, 750, 800, 850,
	900, 950, 1000, 1050, 1100, 1150, 1200, 1250,
};

static const uint16_t lsp_scalar_cb4[16] = {
	700, 800, 900, 1000, 1100, 1200, 1300, 1400,
	1500, 1600, 1700, 1800, 1900, 2000, 2100, 2200,
};

static const uint16_t lsp_scalar_cb5[16] = {
	950, 1050, 1150, 1250, 1350, 1450, 1550, 1650,
	1750, 1850, 1950, 2050, 2150, 2250, 2350, 2450,
};

static const uint16_t lsp_scalar_cb6[16] = {
	1100, 1200, 1300, 1400, 1500, 1600, 1700, 1800,
	1900, 2000, 2100, 2200, 2300, 2400, 2500, 2600,
};

static const uint16_t lsp_scalar_cb7[16] = {
	1500, 1600, 1700, 1800, 1900, 2000, 2100, 2200,
	2300, 2400, 2500, 2600, 2700, 2800, 2900, 3000,
};

static const uint16_t lsp_scalar_cb8[8] = {
	2300, 2400, 2500, 2600, 2700, 2800, 2900, 3000,
};

static const uint16_t lsp_scalar_cb9[8] = {
	2500, 2600, 2700, 2800, 2900, 3000, 3100, 3200,
};

static const uint16_t lsp_scalar_cb10[4] = {
	2900, 3100, 3300, 3500,
};

const lsp_scalar_cb_t lsp_scalar_cb[LSP_SCALAR_INDEXES] =
{
	{ 1, 16, 4, lsp_scalar_cb1 },
	{ 1, 16, 4, lsp_scalar_cb2 },
	{ 1, 16, 4, lsp_scalar_cb3 },
	{ 1, 16, 4, lsp_scalar_cb4 },
	{ 1, 16, 4, lsp_scalar_cb5 },
	{ 1, 16, 4, lsp_scalar_cb6 },
	{ 1, 16, 4, lsp_scalar_cb7 },
	{ 1, 8, 3, lsp_scalar_cb8 },
	{ 1, 8, 3, lsp_scalar_cb9 },
	{ 1, 4, 2, lsp_scalar_cb10 },
};
