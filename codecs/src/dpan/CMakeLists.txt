# ============================================================================
# lib_dpan — Deluxe Paint Animation byte-decoder primitives
#
# OBJECT library, internal to onyx_anim_codecs. Pure byte-span functions for
# parsing the LPF file header / page table and for applying RunSkipDump
# (compression type 1) records onto a chunky 8-bit buffer. Free of
# musac/onyx_image deps. Little-endian throughout (DOS heritage).
# ============================================================================

add_library(lib_dpan OBJECT
        src/header.cc
        src/decoders.cc
)

target_compile_features(lib_dpan PUBLIC cxx_std_20)

target_include_directories(lib_dpan
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

# PUBLIC because dpan/types.hh re-exports onyx_anim::expected and
# bytes::byte_reader — consumers of lib_dpan's headers need both
# include trees on their compile path.
target_link_libraries(lib_dpan PUBLIC onyx_anim_sdk lib_bytes)

set_target_properties(lib_dpan PROPERTIES POSITION_INDEPENDENT_CODE ON)

neutrino_target_warnings(lib_dpan)
neutrino_target_sanitizers(lib_dpan)
