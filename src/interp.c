#include "codec2_internal.h"
#include "interp.h"

void interp_Wo(
    model_t *interp,              /* interpolated model params */
    const model_t *restrict prev, /* previous frames model params */
    const model_t *restrict next  /* next frames model params */
)
{
    /* trap corner case where voicing est is probably wrong */
    if (interp->voiced && !prev->voiced && !next->voiced)
    {
        interp->voiced = 0;
    }

    /* Wo depends on voicing of this and adjacent frames */
    if (interp->voiced)
    {
        if (prev->voiced && next->voiced)
            interp->Wo = prev->Wo + (next->Wo - prev->Wo) / 2; /* simple average */
        if (!prev->voiced && next->voiced)
            interp->Wo = next->Wo;
        if (prev->voiced && !next->voiced)
            interp->Wo = prev->Wo;
    }
    else
    {
        interp->Wo = W0_MIN_Q24;
    }

    interp->L = wo_to_L(interp->Wo);
}

int32_t interp_energy(
    int32_t prev_e, /* previous energy, log2 Q16 */
    int32_t next_e  /* next energy, log2 Q16 */
)
{
    /* sqrt(prev*next) == (log prev + log next)/2 in the log domain */
    return (int32_t)(((int64_t)prev_e + next_e) / 2);
}

void interpolate_lsp(
    int32_t *interp,            /* interpolated LSPs, radians Q24 */
    const int32_t *restrict prev, /* prev LSPs */
    const int32_t *restrict next  /* next LSPs */
)
{
    for (int i = 0; i < LPC_ORD; i++)
        interp[i] = prev[i] + (next[i] - prev[i]) / 2; /* simple average */
}

/* Weighted interpolation of LSPs (1600 bps mode) */
void interpolate_lsp_ver2(
    int32_t *restrict interp,
    const int32_t *restrict prev,
    const int32_t *restrict next,
    int32_t weight) /* Q16 */
{
    for (int i = 0; i < LPC_ORD; i++)
        interp[i] = (int32_t)(((int64_t)(65536 - weight) * prev[i] +
                               (int64_t)weight * next[i]) >> 16);
}

/* Weighted Wo interpolation (1300 bps mode) */
void interp_Wo2(
    model_t *interp,
    const model_t *restrict prev,
    const model_t *restrict next,
    int32_t weight) /* Q16 */
{
    /* trap corner case where voicing est is probably wrong */
    if (interp->voiced && !prev->voiced && !next->voiced)
    {
        interp->voiced = 0;
    }

    /* Wo depends on voicing of this and adjacent frames */
    if (interp->voiced)
    {
        if (prev->voiced && next->voiced)
            interp->Wo = (int32_t)(((int64_t)(65536 - weight) * prev->Wo +
                                    (int64_t)weight * next->Wo) >> 16);
        if (!prev->voiced && next->voiced)
            interp->Wo = next->Wo;
        if (prev->voiced && !next->voiced)
            interp->Wo = prev->Wo;
    }
    else
    {
        interp->Wo = W0_MIN_Q24;
    }

    interp->L = wo_to_L(interp->Wo);
}

/* Weighted energy interpolation in the log domain (1300 bps mode) */
int32_t interp_energy2(int32_t prev_e, int32_t next_e, int32_t weight)
{
    return (int32_t)(((int64_t)(65536 - weight) * prev_e +
                      (int64_t)weight * next_e) >> 16);
}
