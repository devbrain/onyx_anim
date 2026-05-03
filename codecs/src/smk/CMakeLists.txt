# ============================================================================
# lib_smk — Smacker (RAD Game Tools) byte-decoder primitives
#
# OBJECT library, internal to onyx_anim_codecs. Pure byte-span functions for
# parsing the 104-byte file header, building / walking the per-stream Huffman
# trees (small-tree 8-bit values; big-tree 16-bit values + 3-entry escape
# recency cache), decoding the per-frame palette delta + 4×4 video block
# stream (MONO/FULL/SKIP/FILL), and decompressing audio chunks (raw or
# Smacker DPCM, 8/16-bit, mono/stereo). Free of musac/onyx_image deps.
# Little-endian throughout.
# ============================================================================

add_library(lib_smk OBJECT
        src/header.cc
        src/huffman.cc
        src/decoders.cc
)

target_compile_features(lib_smk PUBLIC cxx_std_20)

target_include_directories(lib_smk
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

# PUBLIC because smk/types.hh re-exports onyx_anim::expected and
# bytes::byte_reader — consumers of lib_smk's headers need both
# include trees on their compile path.
target_link_libraries(lib_smk PUBLIC onyx_anim_sdk lib_bytes)

set_target_properties(lib_smk PROPERTIES POSITION_INDEPENDENT_CODE ON)

neutrino_target_warnings(lib_smk)
neutrino_target_sanitizers(lib_smk)
