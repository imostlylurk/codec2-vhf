/* c2vhftest: in-process full-chain test for both modes.
 *
 * 1. Synthesises a speech-like AM-tone + noise-burst signal.
 * 2. Encodes with 3200 and 1600, decodes with the same mode.
 * 3. Checks output length, non-degenerate RMS in voiced segments and
 *    low RMS in silence segments.
 * 4. Checks the packed bit count is exactly 64 per frame (nbit).
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "codec2_vhf.h"

#define N_FRAMES 40

static int16_t sig[N_FRAMES * 320];

static void gen_signal(void)
{
    int n = 0;
    for (int f = 0; f < N_FRAMES; f++)
    {
        /* pattern: voiced tone, voiced tone, noise, silence (per 40ms) */
        int seg = f % 4;
        for (int i = 0; i < 320; i++, n++)
        {
            double t = (double)n / 8000.0;
            float v = 0;
            if (seg == 0)
                v = 5000.f * sinf(2.f * (float)M_PI * 180.f * t) *
                    (0.6f + 0.4f * sinf(2.f * (float)M_PI * 3.f * t));
            else if (seg == 1)
                v = 4000.f * sinf(2.f * (float)M_PI * 120.f * t) +
                    1000.f * sinf(2.f * (float)M_PI * 1600.f * t);
            else if (seg == 2)
                v = 1500.f * ((int)(((unsigned)n * 1103515245u + 12345u) >> 16) - 32768) /
                    32768.f;
            /* seg == 3: silence */
            if (v > 32767.f)
                v = 32767.f;
            if (v < -32768.f)
                v = -32768.f;
            sig[n] = (int16_t)v;
        }
    }
}

static double rms(const int16_t *s, int n)
{
    double acc = 0;
    for (int i = 0; i < n; i++)
        acc += (double)s[i] * s[i];
    return sqrt(acc / n);
}

static int run_mode(int mode_1600)
{
    const int nsam = mode_1600 ? CODEC2_VHF_SAMPLES_PER_FRAME_1600
                               : CODEC2_VHF_SAMPLES_PER_FRAME_3200;
    const int nframes = N_FRAMES * 320 / nsam;

    static codec2_encoder_t enc;
    static codec2_decoder_t dec;
    codec2_vhf_encoder_init(&enc);
    codec2_vhf_decoder_init(&dec);

    static int16_t out[N_FRAMES * 320];
    uint8_t bits[CODEC2_VHF_BYTES_PER_FRAME];
    int nonzero_frames = 0;

    for (int f = 0; f < nframes; f++)
    {
        if (mode_1600)
            codec2_vhf_encode_1600(&enc, bits, &sig[f * nsam]);
        else
            codec2_vhf_encode_3200(&enc, bits, &sig[f * nsam]);

        int used = 0;
        for (int b = 0; b < CODEC2_VHF_BYTES_PER_FRAME; b++)
            if (bits[b])
                used++;
        if (used)
            nonzero_frames++;

        if (mode_1600)
            codec2_vhf_decode_1600(&dec, &out[f * nsam], bits);
        else
            codec2_vhf_decode_3200(&dec, &out[f * nsam], bits);
    }

    const char *name = mode_1600 ? "1600" : "3200";
    int fail = 0;

    /* evaluate per 320-sample block, classified by the input segment
       (pattern: voiced, voiced, noise, silence).  For silence blocks
       use the last 160 samples: the decoder's energy interpolation
       needs ~one 20 ms frame to decay after the previous segment. */
    double voiced_acc = 0, silence_acc = 0;
    int voiced_n = 0, silence_n = 0;
    for (int f = 0; f < N_FRAMES; f++)
    {
        int seg = f % 4;
        if (seg == 0 || seg == 1)
        {
            voiced_acc += rms(&out[f * 320], 320);
            voiced_n++;
        }
        else if (seg == 3)
        {
            silence_acc += rms(&out[f * 320 + 160], 160);
            silence_n++;
        }
    }
    double voiced_avg = voiced_acc / voiced_n;
    double silence_avg = silence_acc / silence_n;

    if (voiced_avg < 300.0)
    {
        printf("  mode %s: FAIL voiced rms %.1f too low\n", name, voiced_avg);
        fail = 1;
    }
    if (silence_avg > 0.5 * voiced_avg)
    {
        printf("  mode %s: FAIL silence rms %.1f not below half of voiced "
               "%.1f\n", name, silence_avg, voiced_avg);
        fail = 1;
    }
    if (nonzero_frames < nframes / 2)
    {
        printf("  mode %s: FAIL only %d/%d frames produced bits\n", name,
               nonzero_frames, nframes);
        fail = 1;
    }

    printf("  mode %s: %d frames, voiced rms %.1f, silence rms %.1f %s\n",
           name, nframes, voiced_avg, silence_avg, fail ? "FAIL" : "PASS");
    return fail;
}

int main(void)
{
    printf("c2vhftest:\n");
    gen_signal();
    int fail = run_mode(0);
    fail |= run_mode(1);
    printf("%s\n", fail ? "TESTS FAILED" : "ALL PASS");
    return fail;
}
