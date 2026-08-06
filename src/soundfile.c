/// @file soundfile.c
/// @brief SoundFileTap implementation — the ONLY translation unit that includes dr_wav.h.
///
/// Layering (see PLAN.md §5): every open pre-validates the RIFF structure and
/// fmt/data chunks against the config limits BEFORE dr_wav parses anything,
/// with overflow-checked size arithmetic throughout; independently, every
/// allocation dr_wav makes is routed through the accounting allocator below,
/// which enforces max_total_bytes cumulatively as a hard backstop. After
/// dr_wav's own parse, its view of the file is cross-checked against ours and
/// any disagreement rejects the file.

#if !defined(_WIN32)
// 64-bit file offsets for fseeko/ftello on 32-bit POSIX, and the POSIX feature
// level that declares them under strict -std=c11; must precede all includes.
// NOLINTBEGIN(readability-identifier-naming) — feature-test macro names are fixed by POSIX
#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L
// NOLINTEND(readability-identifier-naming)
#endif

#include "tap/soundfile.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#ifndef TAP_SOUNDFILE_NO_STDIO
#include <stdio.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define TAP_SF_FSEEK64 _fseeki64
#define TAP_SF_FTELL64 _ftelli64
#else
#define TAP_SF_FSEEK64 fseeko
#define TAP_SF_FTELL64 ftello
#endif
#endif

// dr_wav is always driven through our IO callbacks, so its own stdio path is
// compiled out unconditionally (not just on embedded builds). Our warning
// flags don't apply to vendored code — it is formatted and warned upstream.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#endif
#define DR_WAV_NO_STDIO
#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

// ---------------------------------------------------------------------------
// Overflow-checked arithmetic. No unchecked size math anywhere in this file.
// ---------------------------------------------------------------------------

static int sf_mul_u64(uint64_t a, uint64_t b, uint64_t* out) {
#if defined(__GNUC__) || defined(__clang__)
    return !__builtin_mul_overflow(a, b, out);
#else
    if (a == 0 || b == 0) {
        *out = 0;
        return 1;
    }
    if (a > UINT64_MAX / b) {
        return 0;
    }
    *out = a * b;
    return 1;
#endif
}

static int sf_add_u64(uint64_t a, uint64_t b, uint64_t* out) {
#if defined(__GNUC__) || defined(__clang__)
    return !__builtin_add_overflow(a, b, out);
#else
    if (a > UINT64_MAX - b) {
        return 0;
    }
    *out = a + b;
    return 1;
#endif
}

// ---------------------------------------------------------------------------
// Accounting allocator. Every allocation — ours and dr_wav's — carries a
// 16-byte header recording its size, so realloc/free can be debited against
// the cumulative live-byte count (dr_wav's callbacks don't pass old sizes).
// The cap applies to requested bytes; the header overhead is not charged.
// ---------------------------------------------------------------------------

typedef struct {
    void* (*malloc_fn)(size_t size, void* user);
    void* (*realloc_fn)(void* p, size_t old_size, size_t new_size, void* user);
    void (*free_fn)(void* p, size_t old_size, void* user);
    void*    user;
    uint64_t live_bytes;
    uint64_t max_total_bytes;
} sf_mem;

typedef struct {
    uint64_t size;
    uint64_t reserved; // pads the header to 16 bytes / max_align_t
} sf_alloc_header;

static void* sf_default_malloc(size_t size, void* user) {
    (void)user;
    return malloc(size);
}

static void* sf_default_realloc(void* p, size_t old_size, size_t new_size, void* user) {
    (void)old_size;
    (void)user;
    return realloc(p, new_size);
}

static void sf_default_free(void* p, size_t old_size, void* user) {
    (void)old_size;
    (void)user;
    free(p);
}

static void* sf_mem_alloc(sf_mem* m, size_t size) {
    uint64_t new_live  = 0;
    uint64_t raw_bytes = 0;
    if (size == 0 || size > SIZE_MAX - sizeof(sf_alloc_header)) {
        return NULL;
    }
    if (!sf_add_u64(m->live_bytes, size, &new_live) || new_live > m->max_total_bytes) {
        return NULL;
    }
    raw_bytes            = (uint64_t)size + sizeof(sf_alloc_header);
    sf_alloc_header* hdr = (sf_alloc_header*)m->malloc_fn((size_t)raw_bytes, m->user);
    if (hdr == NULL) {
        return NULL;
    }
    hdr->size     = size;
    hdr->reserved = 0;
    m->live_bytes = new_live;
    return hdr + 1;
}

static void sf_mem_free(sf_mem* m, void* p) {
    if (p == NULL) {
        return;
    }
    sf_alloc_header* hdr = (sf_alloc_header*)p - 1;
    m->live_bytes -= hdr->size; // cannot underflow: every live block was credited on alloc
    m->free_fn(hdr, (size_t)(hdr->size + sizeof(sf_alloc_header)), m->user);
}

static void* sf_mem_realloc(sf_mem* m, void* p, size_t new_size) {
    if (p == NULL) {
        return sf_mem_alloc(m, new_size);
    }
    if (new_size == 0) {
        sf_mem_free(m, p);
        return NULL;
    }
    sf_alloc_header* hdr      = (sf_alloc_header*)p - 1;
    uint64_t         old_size = hdr->size;
    uint64_t         new_live = 0;
    if (new_size > SIZE_MAX - sizeof(sf_alloc_header)) {
        return NULL;
    }
    if (!sf_add_u64(m->live_bytes - old_size, new_size, &new_live) || new_live > m->max_total_bytes) {
        return NULL;
    }
    sf_alloc_header* new_hdr = (sf_alloc_header*)m->realloc_fn(hdr, (size_t)(old_size + sizeof(sf_alloc_header)),
                                                               new_size + sizeof(sf_alloc_header), m->user);
    if (new_hdr == NULL) {
        return NULL; // original block untouched, accounting unchanged
    }
    new_hdr->size = new_size;
    m->live_bytes = new_live;
    return new_hdr + 1;
}

// dr_wav allocation-callback bridge.

static void* sf_drwav_on_malloc(size_t sz, void* user) {
    return sf_mem_alloc((sf_mem*)user, sz);
}

static void* sf_drwav_on_realloc(void* p, size_t sz, void* user) {
    return sf_mem_realloc((sf_mem*)user, p, sz);
}

static void sf_drwav_on_free(void* p, void* user) {
    sf_mem_free((sf_mem*)user, p);
}

// ---------------------------------------------------------------------------
// IO source: a bounded reader over memory or a FILE*. Pre-validation and
// dr_wav both consume the stream through this, so both see identical bytes
// and identical bounds.
// ---------------------------------------------------------------------------

typedef struct {
    const uint8_t* mem; // NULL in stdio mode
    uint64_t       pos;
    uint64_t       size;
#ifndef TAP_SOUNDFILE_NO_STDIO
    FILE* file; // NULL in memory mode
#endif
} sf_reader;

static size_t sf_reader_read(sf_reader* r, void* dst, size_t n) {
    uint64_t remaining = r->size - r->pos;
    if (n > remaining) {
        n = (size_t)remaining;
    }
    if (n == 0) {
        return 0;
    }
    if (r->mem != NULL) {
        memcpy(dst, r->mem + r->pos, n);
        r->pos += n;
        return n;
    }
#ifndef TAP_SOUNDFILE_NO_STDIO
    {
        size_t got = fread(dst, 1, n, r->file);
        r->pos += got;
        return got;
    }
#else
    return 0;
#endif
}

static int sf_reader_seek_abs(sf_reader* r, uint64_t pos) {
    if (pos > r->size) {
        return 0;
    }
    if (r->mem == NULL) {
#ifndef TAP_SOUNDFILE_NO_STDIO
        if (pos > INT64_MAX || TAP_SF_FSEEK64(r->file, (int64_t)pos, SEEK_SET) != 0) {
            return 0;
        }
#else
        return 0;
#endif
    }
    r->pos = pos;
    return 1;
}

// dr_wav IO-callback bridge.

static size_t sf_drwav_on_read(void* user, void* buffer_out, size_t bytes_to_read) {
    return sf_reader_read((sf_reader*)user, buffer_out, bytes_to_read);
}

static drwav_bool32 sf_drwav_on_seek(void* user, int offset, drwav_seek_origin origin) {
    sf_reader* r      = (sf_reader*)user;
    uint64_t   target = 0;
    uint64_t   base   = 0;
    if (offset < 0) {
        return DRWAV_FALSE; // contract: offset is never negative
    }
    if (origin == DRWAV_SEEK_SET) {
        base = 0;
    }
    else if (origin == DRWAV_SEEK_CUR) {
        base = r->pos;
    }
    else {
        base = r->size;
    }
    if (!sf_add_u64(base, (uint64_t)offset, &target)) {
        return DRWAV_FALSE;
    }
    return sf_reader_seek_abs(r, target) ? DRWAV_TRUE : DRWAV_FALSE;
}

static drwav_bool32 sf_drwav_on_tell(void* user, drwav_int64* cursor) {
    sf_reader* r = (sf_reader*)user;
    if (r->pos > INT64_MAX) {
        return DRWAV_FALSE;
    }
    *cursor = (drwav_int64)r->pos;
    return DRWAV_TRUE;
}

// ---------------------------------------------------------------------------
// WAV pre-validation: our own bounded RIFF walk, before dr_wav sees the file.
// ---------------------------------------------------------------------------

static const uint32_t k_max_chunks = 256; // sanity bound on the chunk walk

typedef struct {
    uint32_t channels;
    uint32_t sample_rate;
    uint32_t bits_per_sample;
    uint16_t format_tag; // effective: 1 = PCM, 3 = IEEE float
    uint32_t block_align;
    uint64_t data_size;
    uint64_t frames;
} sf_wav_info;

static uint16_t sf_load_u16le(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t sf_load_u32le(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// KSDATAFORMAT_SUBTYPE_* GUID tail shared by the PCM and IEEE-float subformats
// (bytes 2..15; bytes 0..1 hold the format tag).
static const uint8_t k_extensible_guid_tail[14] = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80,
                                                   0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};

typedef struct {
    uint16_t format_tag;
    uint16_t channels;
    uint32_t sample_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
} sf_fmt_fields;

// Parse and structurally validate one "fmt " chunk body (already bounded by
// the caller). Returns tap_sf_ok / tap_sf_err_format only.
static tap_sf_result sf_parse_fmt(sf_reader* r, uint32_t chunk_size, sf_fmt_fields* out) {
    uint8_t buf[40];
    if (chunk_size < 16) {
        return tap_sf_err_format;
    }
    size_t want = (chunk_size < sizeof(buf)) ? chunk_size : sizeof(buf);
    if (sf_reader_read(r, buf, want) != want) {
        return tap_sf_err_format;
    }
    out->format_tag      = sf_load_u16le(buf + 0);
    out->channels        = sf_load_u16le(buf + 2);
    out->sample_rate     = sf_load_u32le(buf + 4);
    out->block_align     = sf_load_u16le(buf + 12);
    out->bits_per_sample = sf_load_u16le(buf + 14);

    if (out->format_tag == 0xFFFE) { // WAVE_FORMAT_EXTENSIBLE
        if (chunk_size < 40) {
            return tap_sf_err_format;
        }
        uint16_t cb_size = sf_load_u16le(buf + 16);
        if (cb_size < 22) {
            return tap_sf_err_format;
        }
        uint16_t valid_bits = sf_load_u16le(buf + 18);
        if (valid_bits == 0 || valid_bits > out->bits_per_sample) {
            return tap_sf_err_format;
        }
        if (memcmp(buf + 26, k_extensible_guid_tail, sizeof(k_extensible_guid_tail)) != 0) {
            return tap_sf_err_format;
        }
        out->format_tag = sf_load_u16le(buf + 24); // sub-format tag
    }
    return tap_sf_ok;
}

// Walk the RIFF chunk list, validate fmt/data structure against the file size
// and the config limits, and fill *info. On any failure the reader position is
// unspecified; on success it is also unspecified — callers rewind explicitly.
static tap_sf_result sf_prevalidate_wav(sf_reader* r, const tap_sf_config* cfg, sf_wav_info* info) {
    uint8_t  hdr[12];
    uint64_t walk_end  = 0;
    int      fmt_seen  = 0;
    int      data_seen = 0;

    sf_fmt_fields fmt = {0, 0, 0, 0, 0};
    memset(info, 0, sizeof(*info));

    if (!sf_reader_seek_abs(r, 0) || sf_reader_read(r, hdr, 12) != 12) {
        return tap_sf_err_format;
    }
    if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        return tap_sf_err_format;
    }
    uint32_t riff_size = sf_load_u32le(hdr + 4);
    // The declared container must hold the WAVE id and must not claim bytes
    // beyond the physical file — a size lie is rejected, not clamped.
    if (riff_size < 4 || !sf_add_u64(8, riff_size, &walk_end) || walk_end > r->size) {
        return tap_sf_err_format;
    }

    for (uint32_t chunk_index = 0;; ++chunk_index) {
        if (r->pos == walk_end) {
            break;
        }
        if (chunk_index >= k_max_chunks || walk_end - r->pos < 8) {
            return tap_sf_err_format;
        }
        uint8_t chdr[8];
        if (sf_reader_read(r, chdr, 8) != 8) {
            return tap_sf_err_format;
        }
        uint32_t chunk_size = sf_load_u32le(chdr + 4);
        uint64_t remaining  = walk_end - r->pos;
        if (chunk_size > remaining) {
            return tap_sf_err_format; // chunk claims bytes beyond the container
        }
        uint64_t body_start = r->pos;

        if (memcmp(chdr, "fmt ", 4) == 0) {
            if (fmt_seen) {
                return tap_sf_err_format;
            }
            tap_sf_result res = sf_parse_fmt(r, chunk_size, &fmt);
            if (res != tap_sf_ok) {
                return res;
            }
            fmt_seen = 1;
        }
        else if (memcmp(chdr, "data", 4) == 0) {
            if (data_seen || !fmt_seen) { // data before fmt is a spec violation
                return tap_sf_err_format;
            }
            info->data_size = chunk_size;
            data_seen       = 1;
        }

        // Advance past the chunk body plus its pad byte; a final odd-sized
        // chunk without its pad byte is tolerated as end-of-container.
        uint64_t next = 0;
        if (!sf_add_u64(body_start, (uint64_t)chunk_size + (chunk_size & 1U), &next)) {
            return tap_sf_err_format;
        }
        if (next > walk_end) {
            next = walk_end; // only possible via the missing final pad byte
        }
        if (!sf_reader_seek_abs(r, next)) {
            return tap_sf_err_io;
        }
    }

    if (!fmt_seen || !data_seen) {
        return tap_sf_err_format;
    }

    // --- structural fmt validation (format errors) ---
    if (fmt.channels == 0 || fmt.sample_rate == 0) {
        return tap_sf_err_format;
    }
    if (fmt.format_tag == 1) { // PCM
        if (!(fmt.bits_per_sample == 8 || fmt.bits_per_sample == 16 || fmt.bits_per_sample == 24
              || fmt.bits_per_sample == 32)) {
            return tap_sf_err_format;
        }
    }
    else if (fmt.format_tag == 3) { // IEEE float
        if (!(fmt.bits_per_sample == 32 || fmt.bits_per_sample == 64)) {
            return tap_sf_err_format;
        }
    }
    else {
        return tap_sf_err_format; // compressed / unknown codecs are out of scope
    }
    uint64_t expected_align = 0;
    if (!sf_mul_u64(fmt.channels, fmt.bits_per_sample / 8U, &expected_align) || expected_align > UINT16_MAX
        || fmt.block_align != expected_align) {
        return tap_sf_err_format;
    }
    if (info->data_size % expected_align != 0) {
        return tap_sf_err_format; // partial trailing frame: reject, don't clamp
    }

    // --- limit validation (bounds errors) ---
    if (fmt.channels > cfg->max_channels) {
        return tap_sf_err_bounds;
    }
    if (fmt.sample_rate > cfg->max_sample_rate) {
        return tap_sf_err_bounds;
    }
    uint64_t frames     = info->data_size / expected_align;
    uint64_t max_frames = cfg->max_frames;
    if (max_frames == 0) {
        // Derive: the frame count whose f32 rendering would exceed the byte cap.
        max_frames = cfg->max_total_bytes / (fmt.channels * (uint64_t)sizeof(float));
    }
    if (frames > max_frames) {
        return tap_sf_err_bounds;
    }

    info->channels        = fmt.channels;
    info->sample_rate     = fmt.sample_rate;
    info->bits_per_sample = fmt.bits_per_sample;
    info->format_tag      = fmt.format_tag;
    info->block_align     = (uint32_t)expected_align;
    info->frames          = frames;
    return tap_sf_ok;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

struct tap_sf {
    sf_mem      mem;    // stable address for dr_wav's allocation callbacks
    sf_reader   reader; // stable address for dr_wav's IO callbacks
    sf_wav_info info;
    drwav       wav;
};

void tap_sf_config_init(tap_sf_config* out) {
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->max_channels    = 64;
    out->max_sample_rate = 384000;
    out->max_frames      = 0; // derived from max_total_bytes at open
    out->max_total_bytes = (uint64_t)2 * 1024 * 1024 * 1024;
}

static void sf_reader_close(sf_reader* r) {
#ifndef TAP_SOUNDFILE_NO_STDIO
    if (r->mem == NULL && r->file != NULL) {
        fclose(r->file);
        r->file = NULL;
    }
#else
    (void)r;
#endif
}

// Shared open path: pre-validate through *reader, then hand the rewound stream
// to dr_wav and cross-check its parse against ours. Takes ownership of the
// reader (closes it on failure).
static tap_sf_result sf_open_common(tap_sf** out, sf_reader* reader, const tap_sf_config* cfg) {
    tap_sf_config local_cfg;
    if (cfg == NULL) {
        tap_sf_config_init(&local_cfg);
    }
    else {
        local_cfg = *cfg;
    }

    // Hooks are all-or-nothing: a partial set silently mixing user and libc
    // allocation would be an unrecoverable bug, so reject it at the boundary.
    int hooks_set = (local_cfg.malloc_fn != NULL) + (local_cfg.realloc_fn != NULL) + (local_cfg.free_fn != NULL);
    if (hooks_set != 0 && hooks_set != 3) {
        sf_reader_close(reader);
        return tap_sf_err_arg;
    }
    if (hooks_set == 0) {
        local_cfg.malloc_fn  = sf_default_malloc;
        local_cfg.realloc_fn = sf_default_realloc;
        local_cfg.free_fn    = sf_default_free;
        local_cfg.alloc_user = NULL;
    }

    sf_wav_info   info;
    tap_sf_result res = sf_prevalidate_wav(reader, &local_cfg, &info);
    if (res != tap_sf_ok) {
        sf_reader_close(reader);
        return res;
    }

    sf_mem boot_mem;
    boot_mem.malloc_fn       = local_cfg.malloc_fn;
    boot_mem.realloc_fn      = local_cfg.realloc_fn;
    boot_mem.free_fn         = local_cfg.free_fn;
    boot_mem.user            = local_cfg.alloc_user;
    boot_mem.live_bytes      = 0;
    boot_mem.max_total_bytes = local_cfg.max_total_bytes;

    tap_sf* sf = (tap_sf*)sf_mem_alloc(&boot_mem, sizeof(tap_sf));
    if (sf == NULL) {
        sf_reader_close(reader);
        return tap_sf_err_alloc;
    }
    memset(sf, 0, sizeof(*sf));
    sf->mem    = boot_mem; // live_bytes already includes sf itself
    sf->reader = *reader;
    sf->info   = info;

    drwav_allocation_callbacks alloc_cbs;
    alloc_cbs.pUserData = &sf->mem;
    alloc_cbs.onMalloc  = sf_drwav_on_malloc;
    alloc_cbs.onRealloc = sf_drwav_on_realloc;
    alloc_cbs.onFree    = sf_drwav_on_free;

    if (!sf_reader_seek_abs(&sf->reader, 0)
        || !drwav_init(&sf->wav, sf_drwav_on_read, sf_drwav_on_seek, sf_drwav_on_tell, &sf->reader, &alloc_cbs)) {
        sf_reader_close(&sf->reader);
        sf_mem final_mem = sf->mem;
        sf_mem_free(&final_mem, sf);
        return tap_sf_err_format;
    }

    // Cross-check dr_wav's parse against our pre-validation; any disagreement
    // means one of us misread the file — reject rather than guess who's right.
    if (sf->wav.channels != sf->info.channels || sf->wav.sampleRate != sf->info.sample_rate
        || sf->wav.bitsPerSample != sf->info.bits_per_sample || sf->wav.totalPCMFrameCount != sf->info.frames) {
        tap_sf_close(sf);
        return tap_sf_err_format;
    }

    *out = sf;
    return tap_sf_ok;
}

tap_sf_result tap_sf_open_memory(tap_sf** out, const void* data, size_t bytes, const tap_sf_config* cfg) {
    if (out == NULL) {
        return tap_sf_err_arg;
    }
    *out = NULL;
    if (data == NULL || bytes == 0) {
        return tap_sf_err_arg;
    }
    sf_reader reader;
    memset(&reader, 0, sizeof(reader));
    reader.mem  = (const uint8_t*)data;
    reader.size = bytes;
    return sf_open_common(out, &reader, cfg);
}

#ifndef TAP_SOUNDFILE_NO_STDIO

static FILE* sf_fopen_utf8(const char* utf8_path) {
#if defined(_WIN32)
    // The ANSI fopen would mangle non-ASCII paths; go through UTF-16.
    int wide_len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8_path, -1, NULL, 0);
    if (wide_len <= 0) {
        return NULL;
    }
    wchar_t* wide = (wchar_t*)malloc((size_t)wide_len * sizeof(wchar_t));
    if (wide == NULL) {
        return NULL;
    }
    FILE* f = NULL;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8_path, -1, wide, wide_len) > 0) {
        f = _wfopen(wide, L"rb");
    }
    free(wide);
    return f;
#else
    return fopen(utf8_path, "rb");
#endif
}

tap_sf_result tap_sf_open_file(tap_sf** out, const char* utf8_path, const tap_sf_config* cfg) {
    if (out == NULL) {
        return tap_sf_err_arg;
    }
    *out = NULL;
    if (utf8_path == NULL) {
        return tap_sf_err_arg;
    }
    FILE* f = sf_fopen_utf8(utf8_path);
    if (f == NULL) {
        return tap_sf_err_io;
    }
    if (TAP_SF_FSEEK64(f, 0, SEEK_END) != 0) {
        fclose(f);
        return tap_sf_err_io;
    }
    int64_t size = TAP_SF_FTELL64(f);
    if (size < 0 || TAP_SF_FSEEK64(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return tap_sf_err_io;
    }
    sf_reader reader;
    memset(&reader, 0, sizeof(reader));
    reader.file = f;
    reader.size = (uint64_t)size;
    return sf_open_common(out, &reader, cfg);
}

#endif // TAP_SOUNDFILE_NO_STDIO

uint32_t tap_sf_channels(const tap_sf* sf) {
    return (sf != NULL) ? sf->info.channels : 0;
}

uint32_t tap_sf_sample_rate(const tap_sf* sf) {
    return (sf != NULL) ? sf->info.sample_rate : 0;
}

uint64_t tap_sf_frame_count(const tap_sf* sf) {
    return (sf != NULL) ? sf->info.frames : 0;
}

uint64_t tap_sf_read_f32(tap_sf* sf, float* interleaved, uint64_t frames) {
    if (sf == NULL || interleaved == NULL || frames == 0) {
        return 0;
    }
    // A request whose f32 rendering can't even be sized is caller error.
    uint64_t samples = 0;
    uint64_t bytes   = 0;
    if (!sf_mul_u64(frames, sf->info.channels, &samples) || !sf_mul_u64(samples, sizeof(float), &bytes)) {
        return 0;
    }
    return drwav_read_pcm_frames_f32(&sf->wav, frames, interleaved);
}

tap_sf_result tap_sf_seek(tap_sf* sf, uint64_t frame) {
    if (sf == NULL) {
        return tap_sf_err_arg;
    }
    if (frame > sf->info.frames) {
        return tap_sf_err_arg;
    }
    return drwav_seek_to_pcm_frame(&sf->wav, frame) ? tap_sf_ok : tap_sf_err_io;
}

void tap_sf_close(tap_sf* sf) {
    if (sf == NULL) {
        return;
    }
    drwav_uninit(&sf->wav); // frees dr_wav's allocations through sf->mem
    sf_reader_close(&sf->reader);
    sf_mem final_mem = sf->mem;
    sf_mem_free(&final_mem, sf);
}
