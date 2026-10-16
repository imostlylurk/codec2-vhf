cmake_minimum_required(VERSION 3.13)
project(codec2-vhf C)

set(CMAKE_C_STANDARD 11)

# debug hooks: per-sub-frame parameter dumps (C2VHF_DUMP=1 at runtime);
# costs ~2 KiB flash via printf machinery, so off by default
option(C2VHF_DUMP_HOOKS "build decoder parameter-dump hooks" OFF)

add_library(c2vhf STATIC
    src/analysis.c
    src/codec2_vhf.c
    src/delta_lsp_cb.c
    src/interp.c
    src/kiss_fft.c
    src/kiss_fftr.c
    src/lpc.c
    src/lsp_scalar_cb.c
    src/nlp.c
    src/quantise.c
    src/synthesis.c
    src/util.c
)
target_include_directories(c2vhf PUBLIC inc)
if(C2VHF_DUMP_HOOKS)
    target_compile_definitions(c2vhf PRIVATE C2VHF_DUMP_HOOKS)
endif()
# keep float math reproducible for bit-exactness on FMA-capable targets.
# NOTE: do not add -Wfloat-conversion: the codebase deliberately keeps
# double-precision intermediates from the reference implementation and
# "fixing" those warnings would break bit-exactness.
target_compile_options(c2vhf PRIVATE -O3 -ffp-contract=off -Wall -Wextra)

# c2vhfenc / c2vhfdec: raw 8 kHz mono s16 <-> .c2 file (with the stock
# codec2 7-byte header, so files interop with c2enc/c2dec/c2play)
add_executable(c2vhfenc tools/c2vhfenc.c)
target_link_libraries(c2vhfenc PRIVATE c2vhf m)

add_executable(c2vhfdec tools/c2vhfdec.c)
target_link_libraries(c2vhfdec PRIVATE c2vhf m)

add_executable(c2vhftest tests/c2vhftest.c)
target_link_libraries(c2vhftest PRIVATE c2vhf m)
target_compile_options(c2vhftest PRIVATE -O3 -ffp-contract=off)

add_executable(bench tests/bench.c)
target_link_libraries(bench PRIVATE c2vhf m pthread)
target_compile_options(bench PRIVATE -O3 -ffp-contract=off)

enable_testing()
add_test(NAME c2vhftest COMMAND c2vhftest)
