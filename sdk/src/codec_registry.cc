#include <onyx_anim/sdk/codec_registry.hh>

namespace onyx_anim {
    codec_registry& codec_registry::instance() {
        static codec_registry r;
        return r;
    }

    codec_registry::codec_registry() = default;
    codec_registry::~codec_registry() = default;

    void codec_registry::register_factory(factory_t factory) {
        if (factory) {
            factories_.push_back(std::move(factory));
        }
    }

    std::unique_ptr <anim_decoder>
    codec_registry::create_decoder(musac::io_stream* stream) const {
        if (!stream) return nullptr;
        for (const auto& f : factories_) {
            auto dec = f();
            if (dec && dec->sniff(stream)) {
                return dec;
            }
        }
        return nullptr;
    }

    std::unique_ptr <anim_decoder>
    codec_registry::create_decoder(std::string_view name) const {
        for (const auto& f : factories_) {
            auto dec = f();
            if (dec && dec->name() == name) {
                return dec;
            }
        }
        return nullptr;
    }
} // namespace onyx_anim
