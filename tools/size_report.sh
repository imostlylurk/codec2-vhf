#!/bin/bash
# size_report.sh: static Cortex-M4 footprint analysis, Codec2-mod style.
#
# Builds minimal per-mode images (-Os -mcpu=cortex-m4 -mthumb,
# newlib-nano, --gc-sections) and reports:
#   - flash (text, includes rodata/codebooks)
#   - static RAM (data+bss of the image, excluding caller-owned codec state)
#   - worst-case stack for each entry point, computed by summing
#     -fstack-usage frame sizes along the deepest call path
#
# usage: tools/size_report.sh   (from the repo root or anywhere)

set -e
cd "$(dirname "$0")/.."
OUT=build-size
CC=arm-none-eabi-gcc
OBJDUMP=arm-none-eabi-objdump
CFLAGS="-Os -mcpu=cortex-m4 -mthumb -ffunction-sections -fdata-sections -fstack-usage -specs=nano.specs -Iinc -Ibuild-size"
mkdir -p $OUT

SRCS="src/analysis.c src/codec2_vhf.c src/delta_lsp_cb.c src/interp.c src/kiss_fft.c src/kiss_fftr.c src/lpc.c src/lsp_scalar_cb.c src/nlp.c src/quantise.c src/synthesis.c src/util.c"

compile_lib() {
    mkdir -p $OUT
    for s in $SRCS; do
        o=$OUT/$(basename "$s" .c).o
        if [ ! -f "$o" ] || [ "$s" -nt "$o" ]; then
            $CC $CFLAGS -c "$s" -o "$o"
        fi
    done
}

# minimal entry: use only the referenced mode so --gc-sections can drop
# the rest; volatile sinks prevent dead-code elimination
make_main() {  # $1 = mode number 0/2/4
    cat > $OUT/main_$1.c << EOF
#include <stdint.h>
#include "codec2_vhf.h"
static codec2_encoder_t enc;
static codec2_decoder_t dec;
volatile uint8_t sink;
void _start(void) {
    codec2_vhf_encoder_init(&enc);
    codec2_vhf_decoder_init(&dec);
    static int16_t sp[320];
    static int16_t out[320];
    uint8_t bits[8];
EOF
    if [ "$1" = 0 ]; then
        echo "    codec2_vhf_encode_3200(&enc, bits, sp); codec2_vhf_decode_3200(&dec, out, bits);" >> $OUT/main_$1.c
    elif [ "$1" = 2 ]; then
        echo "    codec2_vhf_encode_1600(&enc, bits, sp); codec2_vhf_decode_1600(&dec, out, bits);" >> $OUT/main_$1.c
    else
        echo "    codec2_vhf_encode_1300(&enc, bits, sp); codec2_vhf_decode_1300(&dec, out, bits);" >> $OUT/main_$1.c
    fi
    echo "    sink = bits[0] + out[0];
    for (;;) {}
}" >> $OUT/main_$1.c
}

link_image() {  # $1 = tag
    $CC $CFLAGS $OUT/main_$1.c $OUT/*.o -Wl,--gc-sections -Wl,-e,_start -nostartfiles --specs=nosys.specs -o $OUT/img_$1.elf -lm
}

echo "== compiling library objects (Cortex-M4, -Os, newlib-nano) =="
compile_lib

for mode in 0 2 4 all; do
    tag=$mode
    [ "$mode" = "all" ] && tag=0
    make_main $tag
    [ "$mode" = "all" ] && cat > $OUT/main_all.c << 'EOF'
/* all-modes image: every entry point referenced */
#include <stdint.h>
#include "codec2_vhf.h"
static codec2_encoder_t enc;
static codec2_decoder_t dec;
volatile uint8_t sink;
void _start(void) {
    codec2_vhf_encoder_init(&enc);
    codec2_vhf_decoder_init(&dec);
    static int16_t sp[320];
    static int16_t out[320];
    uint8_t bits[8];
    codec2_vhf_encode_3200(&enc, bits, sp); codec2_vhf_decode_3200(&dec, out, bits);
    codec2_vhf_encode_1600(&enc, bits, sp); codec2_vhf_decode_1600(&dec, out, bits);
    codec2_vhf_encode_1300(&enc, bits, sp); codec2_vhf_decode_1300(&dec, out, bits);
    sink = bits[0] + out[0];
    for (;;) {}
}
EOF
    if [ "$mode" = "all" ]; then
        # all-modes: link main_all against full objects
        $CC $CFLAGS $OUT/main_all.c $OUT/*.o -Wl,--gc-sections -Wl,-e,_start -nostartfiles --specs=nosys.specs -o $OUT/img_all.elf -lm
    else
        link_image $mode
    fi
done

echo
echo "== flash / RAM per image (arm-none-eabi-size: text data bss) =="
printf "%-22s %10s %8s %8s\n" "image" "text(flash)" "data" "bss"
for t in 0 2 4 all; do
    arm-none-eabi-size $OUT/img_$t.elf | awk -v tag=$t 'NR==2{printf "%-22s %10d %8d %8d\n", "img_"tag, $1, $2, $3}'
done

echo
echo "== worst-case stack per entry (sum of -fstack-usage frames along"
echo "   the deepest call path, from real relocations) =="
$CC $CFLAGS $OUT/main_all.c -c -o $OUT/main_all.o
python3 tools/stack_calc.py $OUT
