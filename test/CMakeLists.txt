add_executable(onyx_anim_tests
        test_main.cc
        test_sniff.cc
        test_registry.cc
        test_expected.cc
        test_bytes.cc
        test_flc_endtoend.cc
        test_amiga_anim_audio.cc
        test_cdxl.cc
        test_yafa.cc
        test_dpan.cc
        test_smk.cc
        test_bink.cc
        flc/test_header.cc
        flc/test_chunks.cc
        flc/test_decoders.cc
)

target_link_libraries(onyx_anim_tests PRIVATE
        neutrino::onyx_anim_sdk
        neutrino::onyx_anim_codecs
        lib_flc
        lib_bytes
        doctest::doctest
)

target_compile_definitions(onyx_anim_tests PRIVATE
        NEUTRINO_ONYX_ANIM_DATA_DIR="${PROJECT_SOURCE_DIR}/data"
)

target_compile_features(onyx_anim_tests PRIVATE cxx_std_20)

neutrino_target_warnings(onyx_anim_tests)
neutrino_target_sanitizers(onyx_anim_tests)

add_test(NAME onyx_anim_tests COMMAND onyx_anim_tests)
