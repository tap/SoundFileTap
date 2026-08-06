/// @file soundfile.hpp
/// @brief Thin RAII convenience layer over the C API in tap/soundfile.h.
///
/// No additional functionality — construction wraps the open calls, the
/// destructor closes, and the accessors forward. Everything the C API
/// documents (validation, limits, allocator hooks, streaming reads) applies
/// unchanged.

#ifndef TAP_SOUNDFILE_HPP
#define TAP_SOUNDFILE_HPP

#include <cstddef>
#include <cstdint>
#include <utility>

#include "tap/soundfile.h"

namespace tap::soundfile {

    using result = ::tap_sf_result;
    using config = ::tap_sf_config;

    /// A config pre-filled with the library defaults.
    inline config make_config() {
        config c;
        ::tap_sf_config_init(&c);
        return c;
    }

    class reader {
      public:
        reader() = default;
        ~reader() { close(); }

        reader(const reader&)            = delete;
        reader& operator=(const reader&) = delete;

        reader(reader&& other) noexcept
            : m_sf{std::exchange(other.m_sf, nullptr)} {}

        reader& operator=(reader&& other) noexcept {
            if (this != &other) {
                close();
                m_sf = std::exchange(other.m_sf, nullptr);
            }
            return *this;
        }

#ifndef TAP_SOUNDFILE_NO_STDIO
        result open_file(const char* utf8_path, const config* cfg = nullptr) {
            close();
            return ::tap_sf_open_file(&m_sf, utf8_path, cfg);
        }
#endif

        result open_memory(const void* data, std::size_t bytes, const config* cfg = nullptr) {
            close();
            return ::tap_sf_open_memory(&m_sf, data, bytes, cfg);
        }

        bool is_open() const { return m_sf != nullptr; }

        std::uint32_t channels() const { return ::tap_sf_channels(m_sf); }
        std::uint32_t sample_rate() const { return ::tap_sf_sample_rate(m_sf); }
        std::uint64_t frame_count() const { return ::tap_sf_frame_count(m_sf); }

        std::uint64_t read_f32(float* interleaved, std::uint64_t frames) {
            return ::tap_sf_read_f32(m_sf, interleaved, frames);
        }

        result seek(std::uint64_t frame) { return ::tap_sf_seek(m_sf, frame); }

        void close() {
            ::tap_sf_close(m_sf); // NULL-safe
            m_sf = nullptr;
        }

      private:
        ::tap_sf* m_sf = nullptr;
    };

} // namespace tap::soundfile

#endif // TAP_SOUNDFILE_HPP
