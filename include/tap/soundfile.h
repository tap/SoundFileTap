/// @file soundfile.h
/// @brief SoundFileTap — hardened sound file reading for the Tap family.
///
/// Single validated API over a vendored, unmodified dr_libs decoder. Consumers
/// include only this header, never dr_wav.h directly. Every file is
/// pre-validated against configurable bounds before the decoder sees a byte,
/// all size arithmetic is overflow-checked, and every allocation (including the
/// decoder's own) is routed through the caller's hooks and counted against a
/// hard cumulative cap.
///
/// Streaming-first: there is deliberately no "decode whole file to one buffer"
/// call — that is where allocation bombs live. Open, read in blocks, close.
///
/// Out-of-bounds files are rejected, never clamped: open fails with
/// tap_sf_err_bounds and no partial state.

#ifndef TAP_SOUNDFILE_H
#define TAP_SOUNDFILE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Opaque reader handle. Not thread-safe; use one per thread or synchronize.
typedef struct tap_sf tap_sf;

typedef enum {
    tap_sf_ok = 0,
    tap_sf_err_io,     ///< file could not be opened / read / seeked
    tap_sf_err_format, ///< not a WAV we understand, or structurally invalid
    tap_sf_err_bounds, ///< valid but exceeds a tap_sf_config limit
    tap_sf_err_alloc,  ///< allocation failed or cumulative cap exceeded
    tap_sf_err_arg     ///< null/invalid argument
} tap_sf_result;

/// Validation limits and allocator wiring. Fill with tap_sf_config_init(),
/// then override fields as needed. Passing NULL to the open calls uses the
/// defaults.
///
/// The allocator hooks receive old sizes on realloc/free — unlike libc-style
/// hooks — because the wrapper enforces max_total_bytes *cumulatively* and must
/// know how much each call releases. Leave the hooks NULL to use libc.
typedef struct {
    uint32_t max_channels;    ///< default 64 (ambisonics ~7th order + headroom)
    uint32_t max_sample_rate; ///< default 384000
    uint64_t max_frames;      ///< 0 = derive from max_total_bytes / (channels * 4)
    uint64_t max_total_bytes; ///< default 2 GiB; hard cap on cumulative live allocation

    void* (*malloc_fn)(size_t size, void* user);
    void* (*realloc_fn)(void* p, size_t old_size, size_t new_size, void* user);
    void (*free_fn)(void* p, size_t old_size, void* user);
    void* alloc_user;
} tap_sf_config;

/// Fill @p out with the default limits and libc allocation (NULL hooks).
void tap_sf_config_init(tap_sf_config* out);

#ifndef TAP_SOUNDFILE_NO_STDIO
/// Open a sound file from @p utf8_path. The path is UTF-8 on every platform,
/// including Windows (converted internally; never passed to the ANSI fopen).
/// @p cfg may be NULL for defaults; it is copied, not retained.
tap_sf_result tap_sf_open_file(tap_sf** out, const char* utf8_path, const tap_sf_config* cfg);
#endif

/// Open a sound file from a memory buffer. @p data must stay valid and
/// unmodified until tap_sf_close(). @p cfg may be NULL for defaults.
tap_sf_result tap_sf_open_memory(tap_sf** out, const void* data, size_t bytes, const tap_sf_config* cfg);

uint32_t tap_sf_channels(const tap_sf* sf);
uint32_t tap_sf_sample_rate(const tap_sf* sf);
uint64_t tap_sf_frame_count(const tap_sf* sf);

/// Read up to @p frames frames of interleaved float32 into @p interleaved
/// (capacity: frames * channels floats). Returns frames actually read; short
/// reads mean end of stream. Integer sources are scaled to [-1, 1).
uint64_t tap_sf_read_f32(tap_sf* sf, float* interleaved, uint64_t frames);

/// Seek so the next read starts at absolute frame index @p frame.
tap_sf_result tap_sf_seek(tap_sf* sf, uint64_t frame);

/// Close and free everything. NULL is a no-op. After close, every byte
/// allocated through the hooks has been returned through free_fn.
void tap_sf_close(tap_sf* sf);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // TAP_SOUNDFILE_H
