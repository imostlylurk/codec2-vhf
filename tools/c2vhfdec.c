/* c2vhfdec: decode a .c2 file to raw 8 kHz mono s16le speech.
 *
 * usage: c2vhfdec input.c2 output.raw [--mode 3200|1600]
 *
 * Honours the codec2 file header; --mode only needed for headerless
 * files (mode byte values are the codec2 enum: 0 = 3200, 2 = 1600).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "codec2_vhf.h"

static const uint8_t C2_MAGIC[3] = {0xC0, 0xDE, 0xC2};

int main(int argc, char *argv[])
{
    const char *mode_arg = NULL;
    if (argc == 5 && strcmp(argv[3], "--mode") == 0)
        mode_arg = argv[4];
    else if (argc != 3)
    {
        fprintf(stderr, "usage: %s input.c2 output.raw [--mode 3200|1600]\n",
                argv[0]);
        return 1;
    }

    FILE *fin = fopen(argv[1], "rb");
    if (!fin)
    {
        perror(argv[1]);
        return 1;
    }
    FILE *fout = fopen(argv[2], "wb");
    if (!fout)
    {
        perror(argv[2]);
        fclose(fin);
        return 1;
    }

    int mode; /* codec2 enum: 0/2/4 */
    uint8_t hdr[7];
    if (fread(hdr, 1, sizeof(hdr), fin) == sizeof(hdr) &&
        memcmp(hdr, C2_MAGIC, 3) == 0)
    {
        mode = hdr[5];
        fprintf(stderr, "codec2 file header: v%d.%d mode %d\n",
                hdr[3], hdr[4], hdr[5]);
    }
    else
    {
        fseek(fin, 0, SEEK_SET);
        if (!mode_arg)
        {
            fprintf(stderr, "no header and no --mode given; defaulting "
                            "to 3200\n");
            mode = 0;
        }
        else if (strcmp(mode_arg, "1600") == 0)
            mode = 2;
        else if (strcmp(mode_arg, "1300") == 0)
            mode = 4;
        else
            mode = 0;
    }

    const int nsam = mode == 4 ? CODEC2_VHF_SAMPLES_PER_FRAME_1300
                  : mode == 2 ? CODEC2_VHF_SAMPLES_PER_FRAME_1600
                              : CODEC2_VHF_SAMPLES_PER_FRAME_3200;
    const int nbyte = mode == 4 ? CODEC2_VHF_BYTES_PER_FRAME_1300
                    : CODEC2_VHF_BYTES_PER_FRAME;

    static codec2_decoder_t dec;
    codec2_vhf_decoder_init(&dec);

    uint8_t bits[CODEC2_VHF_BYTES_PER_FRAME];
    int16_t speech[CODEC2_VHF_SAMPLES_PER_FRAME_1600];
    long frames = 0;

    while (fread(bits, 1, nbyte, fin) == (size_t)nbyte)
    {
        if (mode == 4)
            codec2_vhf_decode_1300(&dec, speech, bits);
        else if (mode == 2)
            codec2_vhf_decode_1600(&dec, speech, bits);
        else
            codec2_vhf_decode_3200(&dec, speech, bits);
        fwrite(speech, 2, nsam, fout);
        frames++;
    }

    fclose(fin);
    fclose(fout);
    fprintf(stderr, "%s: %ld frames\n", argv[2], frames);
    return 0;
}
