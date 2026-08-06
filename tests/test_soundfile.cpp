// Unit tests for the SoundFileTap public API. WAV files are built in memory by
// the helpers below — byte-exact control over every header field is the point,
// since most of these tests exist to lie to the parser. The checked-in corpus
// under fuzz/ is for fuzzing seeds; nothing here reads it.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "tap/soundfile.h"
#include "tap/soundfile.hpp"

namespace {

    // --- little-endian byte writers -------------------------------------------

    void put_u16(std::vector<uint8_t>& v, uint16_t x) {
        v.push_back(static_cast<uint8_t>(x & 0xFF));
        v.push_back(static_cast<uint8_t>(x >> 8));
    }

    void put_u32(std::vector<uint8_t>& v, uint32_t x) {
        put_u16(v, static_cast<uint16_t>(x & 0xFFFF));
        put_u16(v, static_cast<uint16_t>(x >> 16));
    }

    void put_tag(std::vector<uint8_t>& v, const char* tag) {
        v.insert(v.end(), tag, tag + 4);
    }

    // --- WAV builder ----------------------------------------------------------

    struct wav_spec {
        uint16_t format_tag      = 1; // 1 = PCM, 3 = IEEE float
        uint16_t channels        = 1;
        uint32_t sample_rate     = 48000;
        uint16_t bits_per_sample = 16;
        bool     extensible      = false;

        // Lies for negative tests; 0 / UINT32_MAX = "compute honestly".
        uint32_t forced_riff_size   = UINT32_MAX;
        uint32_t forced_data_size   = UINT32_MAX;
        uint16_t forced_block_align = 0;
        bool     data_before_fmt    = false;
    };

    const uint8_t k_guid_tail[14] = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80,
                                     0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};

    std::vector<uint8_t> build_fmt_chunk(const wav_spec& s) {
        std::vector<uint8_t> c;
        uint16_t             block_align = s.forced_block_align != 0 ? s.forced_block_align
                                                                     : static_cast<uint16_t>(s.channels * (s.bits_per_sample / 8));
        put_tag(c, "fmt ");
        put_u32(c, s.extensible ? 40 : 16);
        put_u16(c, s.extensible ? 0xFFFE : s.format_tag);
        put_u16(c, s.channels);
        put_u32(c, s.sample_rate);
        put_u32(c, s.sample_rate * block_align);
        put_u16(c, block_align);
        put_u16(c, s.bits_per_sample);
        if (s.extensible) {
            put_u16(c, 22);                // cbSize
            put_u16(c, s.bits_per_sample); // valid bits
            put_u32(c, 0);                 // channel mask
            put_u16(c, s.format_tag);      // sub-format tag
            c.insert(c.end(), k_guid_tail, k_guid_tail + 14);
        }
        return c;
    }

    std::vector<uint8_t> build_wav(const wav_spec& s, const std::vector<uint8_t>& payload) {
        std::vector<uint8_t> fmt = build_fmt_chunk(s);

        std::vector<uint8_t> data;
        put_tag(data, "data");
        put_u32(data, s.forced_data_size != UINT32_MAX ? s.forced_data_size : static_cast<uint32_t>(payload.size()));
        data.insert(data.end(), payload.begin(), payload.end());
        if (payload.size() % 2 != 0) {
            data.push_back(0); // pad byte
        }

        std::vector<uint8_t> body;
        if (s.data_before_fmt) {
            body = data;
            body.insert(body.end(), fmt.begin(), fmt.end());
        }
        else {
            body = fmt;
            body.insert(body.end(), data.begin(), data.end());
        }

        std::vector<uint8_t> out;
        put_tag(out, "RIFF");
        put_u32(out, s.forced_riff_size != UINT32_MAX ? s.forced_riff_size : static_cast<uint32_t>(4 + body.size()));
        put_tag(out, "WAVE");
        out.insert(out.end(), body.begin(), body.end());
        return out;
    }

    std::vector<uint8_t> encode_s16(const std::vector<int16_t>& samples) {
        std::vector<uint8_t> bytes;
        for (int16_t s : samples) {
            put_u16(bytes, static_cast<uint16_t>(s));
        }
        return bytes;
    }

    std::vector<uint8_t> encode_s24(const std::vector<int32_t>& samples) {
        std::vector<uint8_t> bytes;
        for (int32_t s : samples) {
            bytes.push_back(static_cast<uint8_t>(s & 0xFF));
            bytes.push_back(static_cast<uint8_t>((s >> 8) & 0xFF));
            bytes.push_back(static_cast<uint8_t>((s >> 16) & 0xFF));
        }
        return bytes;
    }

    std::vector<uint8_t> encode_f32(const std::vector<float>& samples) {
        std::vector<uint8_t> bytes;
        for (float s : samples) {
            uint32_t u = 0;
            std::memcpy(&u, &s, 4);
            put_u32(bytes, u);
        }
        return bytes;
    }

    tap_sf_result open_bytes(tap_sf** sf, const std::vector<uint8_t>& bytes, const tap_sf_config* cfg = nullptr) {
        return tap_sf_open_memory(sf, bytes.data(), bytes.size(), cfg);
    }

    // --- defaults & argument validation ---------------------------------------

    TEST(config, init_fills_documented_defaults) {
        tap_sf_config cfg;
        tap_sf_config_init(&cfg);
        EXPECT_EQ(cfg.max_channels, 64u);
        EXPECT_EQ(cfg.max_sample_rate, 384000u);
        EXPECT_EQ(cfg.max_frames, 0u);
        EXPECT_EQ(cfg.max_total_bytes, uint64_t{2} * 1024 * 1024 * 1024);
        EXPECT_EQ(cfg.malloc_fn, nullptr);
        EXPECT_EQ(cfg.realloc_fn, nullptr);
        EXPECT_EQ(cfg.free_fn, nullptr);
    }

    TEST(args, null_and_empty_are_rejected) {
        tap_sf* sf   = reinterpret_cast<tap_sf*>(0x1);
        uint8_t byte = 0;
        EXPECT_EQ(tap_sf_open_memory(nullptr, &byte, 1, nullptr), tap_sf_err_arg);
        EXPECT_EQ(tap_sf_open_memory(&sf, nullptr, 1, nullptr), tap_sf_err_arg);
        EXPECT_EQ(sf, nullptr); // out is nulled even on failure
        EXPECT_EQ(tap_sf_open_memory(&sf, &byte, 0, nullptr), tap_sf_err_arg);
        EXPECT_EQ(tap_sf_open_file(nullptr, "x.wav", nullptr), tap_sf_err_arg);
        EXPECT_EQ(tap_sf_open_file(&sf, nullptr, nullptr), tap_sf_err_arg);
        EXPECT_EQ(tap_sf_channels(nullptr), 0u);
        EXPECT_EQ(tap_sf_read_f32(nullptr, nullptr, 8), 0u);
        EXPECT_EQ(tap_sf_seek(nullptr, 0), tap_sf_err_arg);
        tap_sf_close(nullptr); // must be a no-op
    }

    TEST(args, partial_allocator_hooks_are_rejected) {
        wav_spec      spec;
        auto          bytes = build_wav(spec, encode_s16({0, 0}));
        tap_sf_config cfg;
        tap_sf_config_init(&cfg);
        cfg.malloc_fn = [](size_t size, void*) -> void* { return std::malloc(size); };
        tap_sf* sf    = nullptr;
        EXPECT_EQ(open_bytes(&sf, bytes, &cfg), tap_sf_err_arg);
    }

    // --- round trips -----------------------------------------------------------

    TEST(round_trip, mono_16bit) {
        std::vector<int16_t> samples = {0, 8192, -8192, 16384, -16384, 32767, -32768, 0};
        wav_spec             spec;
        auto                 bytes = build_wav(spec, encode_s16(samples));

        tap_sf* sf = nullptr;
        ASSERT_EQ(open_bytes(&sf, bytes), tap_sf_ok);
        EXPECT_EQ(tap_sf_channels(sf), 1u);
        EXPECT_EQ(tap_sf_sample_rate(sf), 48000u);
        EXPECT_EQ(tap_sf_frame_count(sf), samples.size());

        std::vector<float> out(samples.size());
        EXPECT_EQ(tap_sf_read_f32(sf, out.data(), samples.size()), samples.size());
        for (size_t i = 0; i < samples.size(); ++i) {
            EXPECT_NEAR(out[i], static_cast<float>(samples[i]) / 32768.0f, 1e-6f) << "sample " << i;
        }
        EXPECT_EQ(tap_sf_read_f32(sf, out.data(), 1), 0u); // end of stream
        tap_sf_close(sf);
    }

    TEST(round_trip, stereo_24bit) {
        std::vector<int32_t> samples = {0, 1 << 22, -(1 << 22), (1 << 23) - 1, -(1 << 23), 42};
        wav_spec             spec;
        spec.channels        = 2;
        spec.bits_per_sample = 24;
        spec.sample_rate     = 44100;
        auto bytes           = build_wav(spec, encode_s24(samples));

        tap_sf* sf = nullptr;
        ASSERT_EQ(open_bytes(&sf, bytes), tap_sf_ok);
        EXPECT_EQ(tap_sf_channels(sf), 2u);
        EXPECT_EQ(tap_sf_frame_count(sf), samples.size() / 2);

        std::vector<float> out(samples.size());
        EXPECT_EQ(tap_sf_read_f32(sf, out.data(), samples.size() / 2), samples.size() / 2);
        for (size_t i = 0; i < samples.size(); ++i) {
            EXPECT_NEAR(out[i], static_cast<float>(samples[i]) / 8388608.0f, 1e-6f) << "sample " << i;
        }
        tap_sf_close(sf);
    }

    TEST(round_trip, eight_channel_float32_96k) {
        std::vector<float> samples;
        for (int frame = 0; frame < 4; ++frame) {
            for (int ch = 0; ch < 8; ++ch) {
                samples.push_back(static_cast<float>(frame) * 0.25f - static_cast<float>(ch) * 0.01f);
            }
        }
        wav_spec spec;
        spec.format_tag      = 3;
        spec.channels        = 8;
        spec.bits_per_sample = 32;
        spec.sample_rate     = 96000;
        auto bytes           = build_wav(spec, encode_f32(samples));

        tap_sf* sf = nullptr;
        ASSERT_EQ(open_bytes(&sf, bytes), tap_sf_ok);
        EXPECT_EQ(tap_sf_channels(sf), 8u);
        EXPECT_EQ(tap_sf_sample_rate(sf), 96000u);
        std::vector<float> out(samples.size());
        EXPECT_EQ(tap_sf_read_f32(sf, out.data(), 4), 4u);
        for (size_t i = 0; i < samples.size(); ++i) {
            EXPECT_EQ(out[i], samples[i]) << "sample " << i; // float payload is bit-exact
        }
        tap_sf_close(sf);
    }

    TEST(round_trip, extensible_16_channel_ambix_style) {
        wav_spec spec;
        spec.extensible      = true;
        spec.format_tag      = 3; // sub-format: IEEE float
        spec.channels        = 16;
        spec.bits_per_sample = 32;
        std::vector<float> samples(16 * 3, 0.5f);
        auto               bytes = build_wav(spec, encode_f32(samples));

        tap_sf* sf = nullptr;
        ASSERT_EQ(open_bytes(&sf, bytes), tap_sf_ok);
        EXPECT_EQ(tap_sf_channels(sf), 16u);
        EXPECT_EQ(tap_sf_frame_count(sf), 3u);
        tap_sf_close(sf);
    }

    TEST(round_trip, block_reads_and_seek) {
        std::vector<int16_t> samples;
        for (int i = 0; i < 16; ++i) {
            samples.push_back(static_cast<int16_t>(i * 1000));
        }
        wav_spec spec;
        auto     bytes = build_wav(spec, encode_s16(samples));

        tap_sf* sf = nullptr;
        ASSERT_EQ(open_bytes(&sf, bytes), tap_sf_ok);

        float block[8];
        EXPECT_EQ(tap_sf_read_f32(sf, block, 5), 5u);
        EXPECT_EQ(tap_sf_read_f32(sf, block, 5), 5u);
        EXPECT_NEAR(block[0], 5000.0f / 32768.0f, 1e-6f);
        EXPECT_EQ(tap_sf_read_f32(sf, block, 8), 6u); // short read at end

        ASSERT_EQ(tap_sf_seek(sf, 12), tap_sf_ok);
        EXPECT_EQ(tap_sf_read_f32(sf, block, 8), 4u);
        EXPECT_NEAR(block[0], 12000.0f / 32768.0f, 1e-6f);

        ASSERT_EQ(tap_sf_seek(sf, 0), tap_sf_ok);
        EXPECT_EQ(tap_sf_read_f32(sf, block, 1), 1u);
        EXPECT_NEAR(block[0], 0.0f, 1e-6f);

        EXPECT_EQ(tap_sf_seek(sf, 17), tap_sf_err_arg); // beyond frame count
        tap_sf_close(sf);
    }

    TEST(round_trip, tolerates_extra_chunk_with_pad_byte) {
        wav_spec spec;
        auto     wav = build_wav(spec, encode_s16({100, 200}));
        // Splice an odd-sized junk chunk (with its pad byte) between WAVE and fmt.
        std::vector<uint8_t> junk;
        put_tag(junk, "JUNK");
        put_u32(junk, 3);
        junk.insert(junk.end(), {1, 2, 3, 0}); // 3 bytes + pad
        wav.insert(wav.begin() + 12, junk.begin(), junk.end());
        // Fix the RIFF size.
        uint32_t riff_size = static_cast<uint32_t>(wav.size() - 8);
        wav[4]             = static_cast<uint8_t>(riff_size & 0xFF);
        wav[5]             = static_cast<uint8_t>((riff_size >> 8) & 0xFF);
        wav[6]             = static_cast<uint8_t>((riff_size >> 16) & 0xFF);
        wav[7]             = static_cast<uint8_t>((riff_size >> 24) & 0xFF);

        tap_sf* sf = nullptr;
        ASSERT_EQ(open_bytes(&sf, wav), tap_sf_ok);
        EXPECT_EQ(tap_sf_frame_count(sf), 2u);
        tap_sf_close(sf);
    }

#ifndef TAP_SOUNDFILE_NO_STDIO
    TEST(round_trip, open_file_matches_open_memory) {
        std::vector<int16_t> samples = {1000, -1000, 2000, -2000};
        wav_spec             spec;
        auto                 bytes = build_wav(spec, encode_s16(samples));

        std::string path = testing::TempDir() + "tap_sf_test.wav";
        FILE*       f    = std::fopen(path.c_str(), "wb");
        ASSERT_NE(f, nullptr);
        ASSERT_EQ(std::fwrite(bytes.data(), 1, bytes.size(), f), bytes.size());
        std::fclose(f);

        tap_sf* sf = nullptr;
        ASSERT_EQ(tap_sf_open_file(&sf, path.c_str(), nullptr), tap_sf_ok);
        EXPECT_EQ(tap_sf_frame_count(sf), samples.size());
        std::vector<float> out(samples.size());
        EXPECT_EQ(tap_sf_read_f32(sf, out.data(), samples.size()), samples.size());
        EXPECT_NEAR(out[2], 2000.0f / 32768.0f, 1e-6f);
        tap_sf_close(sf);
        std::remove(path.c_str());
    }

    TEST(round_trip, open_file_missing_is_io_error) {
        tap_sf* sf = nullptr;
        EXPECT_EQ(tap_sf_open_file(&sf, "/nonexistent/tap_sf_nope.wav", nullptr), tap_sf_err_io);
    }
#endif

    // --- malformed files: reject with tap_sf_err_format ------------------------

    TEST(malformed, not_riff) {
        std::vector<uint8_t> bytes(64, 0x41);
        tap_sf*              sf = nullptr;
        EXPECT_EQ(open_bytes(&sf, bytes), tap_sf_err_format);
    }

    TEST(malformed, truncated_header) {
        wav_spec spec;
        auto     bytes = build_wav(spec, encode_s16({1, 2, 3, 4}));
        bytes.resize(10);
        tap_sf* sf = nullptr;
        EXPECT_EQ(open_bytes(&sf, bytes), tap_sf_err_format);
    }

    TEST(malformed, riff_size_beyond_file) {
        wav_spec spec;
        spec.forced_riff_size = 100000;
        auto    bytes         = build_wav(spec, encode_s16({1, 2}));
        tap_sf* sf            = nullptr;
        EXPECT_EQ(open_bytes(&sf, bytes), tap_sf_err_format);
    }

    TEST(malformed, data_chunk_size_beyond_file) {
        wav_spec spec;
        spec.forced_data_size = 100000; // claims far more than the file holds
        auto    bytes         = build_wav(spec, encode_s16({1, 2}));
        tap_sf* sf            = nullptr;
        EXPECT_EQ(open_bytes(&sf, bytes), tap_sf_err_format);
    }

    TEST(malformed, zero_channels) {
        wav_spec spec;
        spec.channels           = 0;
        spec.forced_block_align = 2; // avoid a zero block-align masking the check
        auto    bytes           = build_wav(spec, encode_s16({1, 2}));
        tap_sf* sf              = nullptr;
        EXPECT_EQ(open_bytes(&sf, bytes), tap_sf_err_format);
    }

    TEST(malformed, unsupported_bit_depth) {
        wav_spec spec;
        spec.bits_per_sample    = 12;
        spec.forced_block_align = 2;
        auto    bytes           = build_wav(spec, encode_s16({1, 2}));
        tap_sf* sf              = nullptr;
        EXPECT_EQ(open_bytes(&sf, bytes), tap_sf_err_format);
    }

    TEST(malformed, float64_pcm_tag_mismatch) {
        wav_spec spec;
        spec.format_tag         = 1; // PCM claiming 64-bit
        spec.bits_per_sample    = 64;
        spec.forced_block_align = 8;
        auto    bytes           = build_wav(spec, std::vector<uint8_t>(16, 0));
        tap_sf* sf              = nullptr;
        EXPECT_EQ(open_bytes(&sf, bytes), tap_sf_err_format);
    }

    TEST(malformed, inconsistent_block_align) {
        wav_spec spec;
        spec.forced_block_align = 5; // mono 16-bit should be 2
        auto    bytes           = build_wav(spec, encode_s16({1, 2}));
        tap_sf* sf              = nullptr;
        EXPECT_EQ(open_bytes(&sf, bytes), tap_sf_err_format);
    }

    TEST(malformed, data_before_fmt) {
        wav_spec spec;
        spec.data_before_fmt = true;
        auto    bytes        = build_wav(spec, encode_s16({1, 2}));
        tap_sf* sf           = nullptr;
        EXPECT_EQ(open_bytes(&sf, bytes), tap_sf_err_format);
    }

    TEST(malformed, partial_trailing_frame) {
        wav_spec spec;
        spec.channels = 2; // block align 4; 6-byte payload = 1.5 frames
        auto    bytes = build_wav(spec, std::vector<uint8_t>{1, 2, 3, 4, 5, 6});
        tap_sf* sf    = nullptr;
        EXPECT_EQ(open_bytes(&sf, bytes), tap_sf_err_format);
    }

    TEST(malformed, compressed_codec_rejected) {
        wav_spec spec;
        spec.format_tag = 0x0055; // MP3-in-WAV
        auto    bytes   = build_wav(spec, encode_s16({1, 2}));
        tap_sf* sf      = nullptr;
        EXPECT_EQ(open_bytes(&sf, bytes), tap_sf_err_format);
    }

    TEST(malformed, missing_data_chunk) {
        wav_spec             spec;
        std::vector<uint8_t> fmt = build_fmt_chunk(spec);
        std::vector<uint8_t> wav;
        put_tag(wav, "RIFF");
        put_u32(wav, static_cast<uint32_t>(4 + fmt.size()));
        put_tag(wav, "WAVE");
        wav.insert(wav.end(), fmt.begin(), fmt.end());
        tap_sf* sf = nullptr;
        EXPECT_EQ(open_bytes(&sf, wav), tap_sf_err_format);
    }

    // --- limits: reject with tap_sf_err_bounds ---------------------------------

    TEST(limits, channels_over_limit) {
        wav_spec spec;
        spec.channels = 8;
        std::vector<int16_t> samples(8 * 2, 0);
        auto                 bytes = build_wav(spec, encode_s16(samples));
        tap_sf_config        cfg;
        tap_sf_config_init(&cfg);
        cfg.max_channels = 4;
        tap_sf* sf       = nullptr;
        EXPECT_EQ(open_bytes(&sf, bytes, &cfg), tap_sf_err_bounds);
    }

    TEST(limits, sample_rate_over_limit) {
        wav_spec spec;
        spec.sample_rate    = 192000;
        auto          bytes = build_wav(spec, encode_s16({1, 2}));
        tap_sf_config cfg;
        tap_sf_config_init(&cfg);
        cfg.max_sample_rate = 96000;
        tap_sf* sf          = nullptr;
        EXPECT_EQ(open_bytes(&sf, bytes, &cfg), tap_sf_err_bounds);
    }

    TEST(limits, explicit_max_frames) {
        wav_spec             spec;
        std::vector<int16_t> samples(8, 0);
        auto                 bytes = build_wav(spec, encode_s16(samples));
        tap_sf_config        cfg;
        tap_sf_config_init(&cfg);
        cfg.max_frames = 4;
        tap_sf* sf     = nullptr;
        EXPECT_EQ(open_bytes(&sf, bytes, &cfg), tap_sf_err_bounds);
        cfg.max_frames = 8;
        EXPECT_EQ(open_bytes(&sf, bytes, &cfg), tap_sf_ok);
        tap_sf_close(sf);
    }

    TEST(limits, max_frames_derived_from_total_bytes) {
        wav_spec             spec;
        std::vector<int16_t> samples(100, 0);
        auto                 bytes = build_wav(spec, encode_s16(samples));
        tap_sf_config        cfg;
        tap_sf_config_init(&cfg);
        cfg.max_total_bytes = 64; // mono f32 -> derived max 16 frames
        tap_sf* sf          = nullptr;
        EXPECT_EQ(open_bytes(&sf, bytes, &cfg), tap_sf_err_bounds);
    }

    // --- allocator hooks & the cumulative backstop -----------------------------

    struct alloc_log {
        std::map<void*, size_t> live;
        uint64_t                total_allocated      = 0;
        int                     mismatched_old_sizes = 0;
        bool                    user_ok              = true;
        void*                   expected_user        = nullptr;

        void note_user(void* user) {
            if (user != expected_user) {
                user_ok = false;
            }
        }
    };

    void* logged_malloc(size_t size, void* user) {
        auto* log = static_cast<alloc_log*>(user);
        void* p   = std::malloc(size);
        if (p != nullptr) {
            log->live[p] = size;
            log->total_allocated += size;
        }
        return p;
    }

    void* logged_realloc(void* p, size_t old_size, size_t new_size, void* user) {
        auto* log = static_cast<alloc_log*>(user);
        if (p != nullptr) {
            auto it = log->live.find(p);
            if (it == log->live.end() || it->second != old_size) {
                ++log->mismatched_old_sizes;
            }
            log->live.erase(p);
        }
        void* q = std::realloc(p, new_size);
        if (q != nullptr) {
            log->live[q] = new_size;
            log->total_allocated += new_size;
        }
        return q;
    }

    void logged_free(void* p, size_t old_size, void* user) {
        auto* log = static_cast<alloc_log*>(user);
        if (p != nullptr) {
            auto it = log->live.find(p);
            if (it == log->live.end() || it->second != old_size) {
                ++log->mismatched_old_sizes;
            }
            log->live.erase(p);
        }
        std::free(p);
    }

    TEST(allocator, hooks_see_every_byte_and_balance_at_close) {
        wav_spec             spec;
        std::vector<int16_t> samples(64, 123);
        auto                 bytes = build_wav(spec, encode_s16(samples));

        alloc_log     log;
        tap_sf_config cfg;
        tap_sf_config_init(&cfg);
        cfg.malloc_fn  = logged_malloc;
        cfg.realloc_fn = logged_realloc;
        cfg.free_fn    = logged_free;
        cfg.alloc_user = &log;

        tap_sf* sf = nullptr;
        ASSERT_EQ(open_bytes(&sf, bytes, &cfg), tap_sf_ok);
        EXPECT_GT(log.total_allocated, 0u); // dr_wav + the handle went through us
        std::vector<float> out(64);
        EXPECT_EQ(tap_sf_read_f32(sf, out.data(), 64), 64u);
        tap_sf_close(sf);

        EXPECT_TRUE(log.live.empty()) << log.live.size() << " allocation(s) leaked past close";
        EXPECT_EQ(log.mismatched_old_sizes, 0);
    }

    TEST(allocator, cumulative_cap_is_a_backstop_independent_of_prevalidation) {
        wav_spec      spec;
        auto          bytes = build_wav(spec, encode_s16({1, 2}));
        tap_sf_config cfg;
        tap_sf_config_init(&cfg);
        // Neutralize the derived-frame pre-validation path (explicit max_frames),
        // then set the byte cap below what the open itself must allocate: only
        // the allocator backstop can reject this.
        cfg.max_frames      = 1000000;
        cfg.max_total_bytes = 16;
        tap_sf* sf          = nullptr;
        EXPECT_EQ(open_bytes(&sf, bytes, &cfg), tap_sf_err_alloc);
    }

    // --- C++ convenience layer -------------------------------------------------

    TEST(cpp_layer, raii_reader_round_trip_and_move) {
        std::vector<int16_t> samples = {100, 200, 300, 400};
        wav_spec             spec;
        auto                 bytes = build_wav(spec, encode_s16(samples));

        tap::soundfile::reader r;
        EXPECT_FALSE(r.is_open());
        ASSERT_EQ(r.open_memory(bytes.data(), bytes.size()), tap_sf_ok);
        EXPECT_TRUE(r.is_open());
        EXPECT_EQ(r.channels(), 1u);
        EXPECT_EQ(r.frame_count(), 4u);

        tap::soundfile::reader moved = std::move(r);
        EXPECT_FALSE(r.is_open()); // NOLINT(bugprone-use-after-move) — testing the moved-from state
        EXPECT_TRUE(moved.is_open());

        float out[4] = {};
        EXPECT_EQ(moved.read_f32(out, 4), 4u);
        EXPECT_NEAR(out[3], 400.0f / 32768.0f, 1e-6f);
        moved.close();
        EXPECT_FALSE(moved.is_open());
    }

} // namespace
