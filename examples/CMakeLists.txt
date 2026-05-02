add_executable(onyx_anim_info info.cc)
target_link_libraries(onyx_anim_info PRIVATE
        neutrino::onyx_anim_sdk
        neutrino::onyx_anim_codecs
)
target_compile_features(onyx_anim_info PRIVATE cxx_std_20)

neutrino_target_warnings(onyx_anim_info)
neutrino_target_sanitizers(onyx_anim_info)

add_executable(flc_to_ppm flc_to_ppm.cc)
target_link_libraries(flc_to_ppm PRIVATE
        neutrino::onyx_anim_sdk
        neutrino::onyx_anim_codecs
        neutrino::onyx_image
)
target_compile_features(flc_to_ppm PRIVATE cxx_std_20)

neutrino_target_warnings(flc_to_ppm)
neutrino_target_sanitizers(flc_to_ppm)

add_executable(seq_to_ppm seq_to_ppm.cc)
target_link_libraries(seq_to_ppm PRIVATE
        neutrino::onyx_anim_sdk
        neutrino::onyx_anim_codecs
        neutrino::onyx_image
)
target_compile_features(seq_to_ppm PRIVATE cxx_std_20)

neutrino_target_warnings(seq_to_ppm)
neutrino_target_sanitizers(seq_to_ppm)

add_executable(anim_to_ppm anim_to_ppm.cc)
target_link_libraries(anim_to_ppm PRIVATE
        neutrino::onyx_anim_sdk
        neutrino::onyx_anim_codecs
        neutrino::onyx_image
)
target_compile_features(anim_to_ppm PRIVATE cxx_std_20)

neutrino_target_warnings(anim_to_ppm)
neutrino_target_sanitizers(anim_to_ppm)
