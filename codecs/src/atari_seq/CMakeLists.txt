# ============================================================================
# lib_atari_seq — Atari ST Cyber Paint .SEQ byte-decoder primitives
#
# OBJECT library, internal to onyx_anim_codecs. Mirrors lib_flc shape: pure
# byte-span functions for parsing headers and decoding the compressed delta
# data, free of musac/onyx_image deps. Big-endian throughout (m68k host).
# ============================================================================

add_library(lib_atari_seq OBJECT
        src/header.cc
        src/decoders.cc
)

target_compile_features(lib_atari_seq PUBLIC cxx_std_20)

target_include_directories(lib_atari_seq
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

# PUBLIC because atari_seq/types.hh re-exports onyx_anim::expected and
# bytes::byte_reader_be — consumers of lib_atari_seq's headers need both
# include trees on their compile path.
target_link_libraries(lib_atari_seq PUBLIC onyx_anim_sdk lib_bytes)

set_target_properties(lib_atari_seq PROPERTIES POSITION_INDEPENDENT_CODE ON)

neutrino_target_warnings(lib_atari_seq)
neutrino_target_sanitizers(lib_atari_seq)
