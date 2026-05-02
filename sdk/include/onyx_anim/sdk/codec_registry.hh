#pragma once

#include <onyx_anim/sdk/onyx_anim_sdk_export.h>
#include <onyx_anim/sdk/decoder.hh>

#include <musac/sdk/io_stream.hh>

#include <functional>
#include <memory>
#include <string_view>
#include <vector>

namespace onyx_anim {
    /**
     * Registry of animation decoders.
     *
     * The SDK ships an empty registry. To populate it with the built-in FLC/SMK
     * decoders, link onyx_anim_codecs and call
     * onyx_anim::register_all_codecs(registry) — see <onyx_anim/codecs/register_codecs.hh>.
     */
    class ONYX_ANIM_SDK_EXPORT codec_registry {
        public:
            codec_registry(const codec_registry&) = delete;
            codec_registry& operator=(const codec_registry&) = delete;

            using factory_t = std::function <std::unique_ptr <anim_decoder>()>;

            [[nodiscard]] static codec_registry& instance();

            /**
             * Register a decoder factory. The factory is called once per
             * create_decoder() invocation to produce a fresh decoder instance.
             */
            void register_factory(factory_t factory);

            /**
             * Find a decoder factory by sniffing. Stream position is preserved.
             * Returns a freshly-constructed decoder, not yet open()ed.
             */
            [[nodiscard]] std::unique_ptr <anim_decoder>
            create_decoder(musac::io_stream* stream) const;

            /**
             * Find a decoder factory by codec name.
             */
            [[nodiscard]] std::unique_ptr <anim_decoder>
            create_decoder(std::string_view name) const;

            [[nodiscard]] std::size_t factory_count() const noexcept {
                return factories_.size();
            }

        private:
            codec_registry();
            ~codec_registry();

            std::vector <factory_t> factories_;
    };
} // namespace onyx_anim
