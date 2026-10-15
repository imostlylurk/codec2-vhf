#include "codec2_internal.h"
#include "interp.h"
#include <math.h>

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
            interp->Wo = prev->Wo + 0.5f * (next->Wo - prev->Wo); // simple average, but disguised
        if (!prev->voiced && next->voiced)
            interp->Wo = next->Wo;
        if (prev->voiced && !next->voiced)
            interp->Wo = prev->Wo;
    }
    else
    {
        interp->Wo = W0_MIN;
    }

    interp->L = M_PI / interp->Wo;
}

float interp_energy(
    float prev_e, /* previous energy */
    float next_e  /* next energy */
)
{
    return sqrtf(prev_e * next_e);
}

void interpolate_lsp(
    float *interp,              /* interpolated LSPs */
    const float *restrict prev, /* prev LSPs */
    const float *restrict next  /* next LSPs */
)
{
    for (int i = 0; i < LPC_ORD; i++)
        interp[i] = prev[i] + 0.5f * (next[i] - prev[i]); // simple average, disguised
}

/* Weighted interpolation of LSPs (1600 bps mode).  Reference port:
   note the double-precision intermediate. */
void interpolate_lsp_ver2(
    float *restrict interp,
    const float *restrict prev,
    const float *restrict next,
    float weight)
{
    for (int i = 0; i < LPC_ORD; i++)
        interp[i] = (1.0 - weight) * prev[i] + weight * next[i];
}

/* Weighted Wo interpolation (1300 bps mode).  Reference port:
   same logic as interp_Wo() but with a non-0.5 weight. */
void interp_Wo2(
    model_t *interp,
    const model_t *restrict prev,
    const model_t *restrict next,
    float weight)
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
            interp->Wo = (1.0 - weight) * prev->Wo + weight * next->Wo;
        if (!prev->voiced && next->voiced)
            interp->Wo = next->Wo;
        if (prev->voiced && !next->voiced)
            interp->Wo = prev->Wo;
    }
    else
    {
        interp->Wo = W0_MIN;
    }

    interp->L = M_PI / interp->Wo;
}

/* Weighted energy interpolation in the log domain (1300 bps mode).
   Reference port. */
float interp_energy2(float prev_e, float next_e, float weight)
{
    return POW10F((1.0 - weight) * log10f(prev_e) + weight * log10f(next_e));
}
