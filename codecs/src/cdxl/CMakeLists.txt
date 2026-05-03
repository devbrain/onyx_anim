# ============================================================================
# lib_cdxl — Commodore CDXL byte-decoder primitives
#
# OBJECT library, internal to onyx_anim_codecs. Pure byte-span functions for
# parsing the 32-byte CDXL chunk header. Free of musac/onyx_image deps;
# big-endian throughout (m68k/Amiga heritage).
# ============================================================================

add_library(lib_cdxl OBJECT
        src/header.cc
)

target_compile_features(lib_cdxl PUBLIC cxx_std_20)

target_include_directories(lib_cdxl
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

# PUBLIC because cdxl/types.hh re-exports onyx_anim::expected and
# bytes::byte_reader_be — consumers of lib_cdxl's headers need both
# include trees on their compile path.
target_link_libraries(lib_cdxl PUBLIC onyx_anim_sdk lib_bytes)

set_target_properties(lib_cdxl PROPERTIES POSITION_INDEPENDENT_CODE ON)

neutrino_target_warnings(lib_cdxl)
neutrino_target_sanitizers(lib_cdxl)
