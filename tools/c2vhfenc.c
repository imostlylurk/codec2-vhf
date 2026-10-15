/* c2vhfenc: encode raw 8 kHz mono s16le speech to a .c2 file.
 *
 * usage: c2vhfenc 3200|1600 input.raw output.c2
 *
 * Writes the stock codec2 7-byte file header (magic C0DEC2, version,
 * mode, flags) so output interops with c2dec/c2play and friends.
 * Pads the input with silence to a whole frame.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "codec2_vhf.h"

static const uint8_t C2_MAGIC[3] = {0xC0, 0xDE, 0xC2};

int main(int argc, char *argv[])
{
    if (argc != 4)
    {
        fprintf(stderr, "usage: %s 3200|1600 input.raw output.c2\n", argv[0]);
        return 1;
    }

    int mode; /* codec2 enum: 0 = 3200, 2 = 1600, 4 = 1300 */
    if (strcmp(argv[1], "3200") == 0)
        mode = 0;
    else if (strcmp(argv[1], "1600") == 0)
        mode = 2;
    else if (strcmp(argv[1], "1300") == 0)
        mode = 4;
    else
    {
        fprintf(stderr, "mode must be 3200, 1600 or 1300\n");
        return 1;
    }

    FILE *fin = fopen(argv[2], "rb");
    if (!fin)
    {
        perror(argv[2]);
        return 1;
    }
    FILE *fout = fopen(argv[3], "wb");
    if (!fout)
    {
        perror(argv[3]);
        fclose(fin);
        return 1;
    }

    const int nsam = mode == 4 ? CODEC2_VHF_SAMPLES_PER_FRAME_1300
                  : mode == 2 ? CODEC2_VHF_SAMPLES_PER_FRAME_1600
                              : CODEC2_VHF_SAMPLES_PER_FRAME_3200;
    const int nbyte = mode == 4 ? CODEC2_VHF_BYTES_PER_FRAME_1300
                    : CODEC2_VHF_BYTES_PER_FRAME;

    /* codec2 file header: mode 0 = CODEC2_MODE_3200, 2 = CODEC2_MODE_1600,
       4 = CODEC2_MODE_1300 */
    uint8_t hdr[7] = {C2_MAGIC[0], C2_MAGIC[1], C2_MAGIC[2], 1, 2,
                      (uint8_t)mode, 0};
    fwrite(hdr, 1, sizeof(hdr), fout);

    static codec2_encoder_t enc;
    codec2_vhf_encoder_init(&enc);

    int16_t speech[CODEC2_VHF_SAMPLES_PER_FRAME_1600];
    uint8_t bits[CODEC2_VHF_BYTES_PER_FRAME];
    long frames = 0;
    size_t n;

    while ((n = fread(speech, 2, nsam, fin)) > 0)
    {
        for (size_t i = n; i < (size_t)nsam; i++)
            speech[i] = 0; /* pad final partial frame with silence */
        if (mode == 4)
            codec2_vhf_encode_1300(&enc, bits, speech);
        else if (mode == 2)
            codec2_vhf_encode_1600(&enc, bits, speech);
        else
            codec2_vhf_encode_3200(&enc, bits, speech);
        fwrite(bits, 1, nbyte, fout);
        frames++;
    }

    fclose(fin);
    fclose(fout);
    fprintf(stderr, "%s: %ld frames\n", argv[3], frames);
    return 0;
}
