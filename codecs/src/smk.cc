#include <array>
#include <cstdint>
#include <cstring>

#include "smk.hh"

namespace onyx_anim {
    namespace {
        constexpr std::array <std::string_view, 1> kSmkExtensions = {".smk"};

        class smk_decoder_impl final : public anim_decoder {
            public:
                using anim_decoder::open;  // expose 1-arg overload

                [[nodiscard]] std::string_view name() const noexcept override {
                    return smk_decoder::codec_name;
                }

                [[nodiscard]] std::span <const std::string_view> extensions() const noexcept override {
                    return {kSmkExtensions.data(), kSmkExtensions.size()};
                }

                [[nodiscard]] bool sniff(musac::io_stream* s) const override {
                    if (!s) return false;
                    const auto pos = s->tell();
                    std::uint8_t magic[4] = {};
                    const auto n = s->read(magic, sizeof(magic));
                    s->seek(pos, musac::seek_origin::set);
                    if (n < 4) return false;
                    // "SMK2" or "SMK4"
                    return magic[0] == 'S' && magic[1] == 'M' && magic[2] == 'K' &&
                           (magic[3] == '2' || magic[3] == '4');
                }

                [[nodiscard]] result open(musac::io_stream* /*s*/,
                                          const decode_options& /*opts*/) override {
                    return make_unexpected <error_type>("smk_decoder: not yet implemented");
                }

                [[nodiscard]] const anim_info& info() const noexcept override { return info_; }

                [[nodiscard]] frame_result decode_frame(onyx_image::surface& /*out*/) override {
                    return make_unexpected <error_type>("smk_decoder: not yet implemented");
                }

                [[nodiscard]] bool eof() const noexcept override { return true; }

                bool rewind() override { return false; }

            private:
                anim_info info_{};
        };
    } // namespace

    std::unique_ptr <anim_decoder> smk_decoder::create() {
        return std::make_unique <smk_decoder_impl>();
    }
} // namespace onyx_anim
