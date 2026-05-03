# ============================================================================
# lib_bink — RAD Game Tools Bink Video byte-decoder primitives
#
# OBJECT library, internal to onyx_anim_codecs. Pure byte/bit-span functions
# for parsing the Bink container header (BIK[bfghik]), building per-frame
# Huffman trees, decoding the 8x8/16x16 block stream (MOTION/RESIDUE/INTRA/
# INTER/RUN/PATTERN/FILL/RAW with SCALED 16x16 sub-types), and the 8x8
# integer IDCT. Free of musac/onyx_image deps. Little-endian throughout.
# ============================================================================

add_library(lib_bink OBJECT
        src/header.cc
        src/vlc.cc
        src/tree.cc
        src/bundle.cc
        src/idct.cc
        src/decoders.cc
        src/audio.cc
)

target_compile_features(lib_bink PUBLIC cxx_std_20)

target_include_directories(lib_bink
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

# PUBLIC: bink/types.hh re-exports onyx_anim::expected and bytes::byte_reader,
# so consumers need both include trees on their compile path.
target_link_libraries(lib_bink
        PUBLIC onyx_anim_sdk lib_bytes
        PRIVATE lib_pocketfft
)

set_target_properties(lib_bink PROPERTIES POSITION_INDEPENDENT_CODE ON)

neutrino_target_warnings(lib_bink)
neutrino_target_sanitizers(lib_bink)

# audio.cc instantiates PocketFFT templates that intentionally compare
# `float` to `double` literals (`if (fct != 1.)`). Our default warning
# set treats `-Wdouble-promotion` as an error; relax that just for this
# translation unit so we don't have to patch the upstream header.
set_source_files_properties(src/audio.cc
        PROPERTIES COMPILE_OPTIONS
        "-Wno-double-promotion;-Wno-shadow;-Wno-sign-conversion;-Wno-conversion;-Wno-useless-cast")

