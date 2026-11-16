/* bench: host timing / RAM / stack benchmark for codec2-vhf.
 *
 * usage: bench [hts1a.raw] [passes]
 *
 * Reports, per mode:
 *   - encoder/decoder state sizes (static allocation, no heap)
 *   - init time
 *   - encode / decode time per frame, avg and max, over N passes
 *   - CPU load for full duplex (enc+dec avg / frame duration)
 *   - stack high-water mark for encode/decode (thread paint)
 */

#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "codec2_vhf.h"

#define MAX_SAMPLES (75 * CODEC2_VHF_SAMPLES_PER_FRAME_1600)

static int16_t speech[MAX_SAMPLES];
static int16_t out[MAX_SAMPLES];
static int n_frames;
static int frame_samples;
static int frame_bytes;

static int64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void run_pass(int mode)
{
    static codec2_encoder_t enc;
    static codec2_decoder_t dec;
    uint8_t bits[CODEC2_VHF_BYTES_PER_FRAME];
    codec2_vhf_encoder_init(&enc);
    codec2_vhf_decoder_init(&dec);
    for (int f = 0; f < n_frames; f++)
    {
        if (mode == 0)
        {
            codec2_vhf_encode_3200(&enc, bits, &speech[f * 160]);
            codec2_vhf_decode_3200(&dec, &out[f * 160], bits);
        }
        else if (mode == 1)
        {
            codec2_vhf_encode_1600(&enc, bits, &speech[f * 320]);
            codec2_vhf_decode_1600(&dec, &out[f * 320], bits);
        }
        else
        {
            uint8_t b7[CODEC2_VHF_BYTES_PER_FRAME_1300];
            codec2_vhf_encode_1300(&enc, b7, &speech[f * 320]);
            codec2_vhf_decode_1300(&dec, &out[f * 320], b7);
        }
    }
}

/* Stack usage is NOT measured here (see tools/size_report.sh: it is
 * measured statically from Cortex-M4 -Os -fstack-usage builds, the
 * same methodology as Codec2-mod). */

int main(int argc, char *argv[])
{
    const char *path = argc > 1 ? argv[1] : "../testdata/hts1a.raw";
    int passes = argc > 2 ? atoi(argv[2]) : 20;

    FILE *f = fopen(path, "rb");
    if (!f)
    {
        perror(path);
        return 1;
    }
    size_t n = fread(speech, 2, MAX_SAMPLES, f);
    fclose(f);
    printf("input: %s (%zu samples, %zu ms), %d passes\n",
           path, n, n / 8, passes);

    printf("state sizes: codec2_encoder_t=%zu codec2_decoder_t=%zu "
           "codec2_t=%zu bytes\n\n",
           sizeof(codec2_encoder_t), sizeof(codec2_decoder_t),
           sizeof(codec2_t));

    static const char *names[] = {"3200", "1600", "1300"};
    static const int ms[] = {20, 40, 40};

    for (int mode = 0; mode < 3; mode++)
    {
        frame_samples = mode == 0 ? 160 : 320;
        frame_bytes = mode == 2 ? 7 : 8;
        n_frames = (int)(n / frame_samples);
        if (n_frames > 75)
            n_frames = mode == 0 ? 150 : 75;

        /* init timing */
        int64_t t0 = now_us();
        for (int r = 0; r < 100; r++)
        {
            static codec2_encoder_t e;
            static codec2_decoder_t d;
            codec2_vhf_encoder_init(&e);
            codec2_vhf_decoder_init(&d);
        }
        int64_t init_us = (now_us() - t0) / 100;

        static codec2_encoder_t enc;
        static codec2_decoder_t dec;
        codec2_vhf_encoder_init(&enc);
        codec2_vhf_decoder_init(&dec);

        int64_t enc_total = 0, dec_total = 0, enc_max = 0, dec_max = 0;
        for (int p = 0; p < passes; p++)
        {
            for (int fr = 0; fr < n_frames; fr++)
            {
                uint8_t bits[CODEC2_VHF_BYTES_PER_FRAME];
                t0 = now_us();
                if (mode == 0)
                    codec2_vhf_encode_3200(&enc, bits, &speech[fr * 160]);
                else if (mode == 1)
                    codec2_vhf_encode_1600(&enc, bits, &speech[fr * 320]);
                else
                    codec2_vhf_encode_1300(&enc, bits, &speech[fr * 320]);
                int64_t dt = now_us() - t0;
                enc_total += dt;
                if (dt > enc_max)
                    enc_max = dt;

                t0 = now_us();
                if (mode == 0)
                    codec2_vhf_decode_3200(&dec, &out[fr * 160], bits);
                else if (mode == 1)
                    codec2_vhf_decode_1600(&dec, &out[fr * 320], bits);
                else
                    codec2_vhf_decode_1300(&dec, &out[fr * 320], bits);
                dt = now_us() - t0;
                dec_total += dt;
                if (dt > dec_max)
                    dec_max = dt;
            }
        }
        int count = n_frames * passes;
        double enc_avg = (double)enc_total / count;
        double dec_avg = (double)dec_total / count;
        double load = (enc_avg + dec_avg) / (ms[mode] * 1000.0) * 100.0;

        printf("mode %s (%d x %dms frames, %d B):\n"
               "  init        %8.1f us\n"
               "  encode      %8.1f us avg  %8.1f us max\n"
               "  decode      %8.1f us avg  %8.1f us max\n"
               "  full-duplex CPU load %5.1f %%\n\n",
               names[mode], n_frames, ms[mode], frame_bytes,
               (double)init_us, enc_avg, (double)enc_max, dec_avg,
               (double)dec_max, load);
    }
    (void)frame_bytes;
    return 0;
}
