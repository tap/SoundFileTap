#!/usr/bin/env python3
"""Generate the fuzz seed corpus (fuzz/corpus/).

Tiny (<10 KB) WAV files covering the supported format shapes, plus a few
near-valid mutants that park fuzzer coverage next to the rejection paths.
Deterministic: running twice produces byte-identical files. Unit tests do NOT
read these — they build WAVs in memory for byte-exact control; this corpus
exists to bootstrap libFuzzer/ClusterFuzzLite coverage.
"""

from __future__ import annotations

import math
import struct
from pathlib import Path

GUID_TAIL = bytes(
    [0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71]
)


def fmt_chunk(tag: int, channels: int, rate: int, bits: int, extensible: bool = False) -> bytes:
    block_align = channels * bits // 8
    base = struct.pack(
        "<HHIIHH", 0xFFFE if extensible else tag, channels, rate, rate * block_align, block_align, bits
    )
    if extensible:
        base += struct.pack("<HHI", 22, bits, 0) + struct.pack("<H", tag) + GUID_TAIL
    return b"fmt " + struct.pack("<I", len(base)) + base


def data_chunk(payload: bytes) -> bytes:
    chunk = b"data" + struct.pack("<I", len(payload)) + payload
    if len(payload) % 2:
        chunk += b"\x00"
    return chunk


def wav(*chunks: bytes) -> bytes:
    body = b"".join(chunks)
    return b"RIFF" + struct.pack("<I", 4 + len(body)) + b"WAVE" + body


def sine_s16(frames: int, channels: int) -> bytes:
    out = bytearray()
    for i in range(frames):
        for ch in range(channels):
            v = int(20000 * math.sin(2 * math.pi * (i + ch) / 16))
            out += struct.pack("<h", v)
    return bytes(out)


def sine_s24(frames: int, channels: int) -> bytes:
    out = bytearray()
    for i in range(frames):
        for ch in range(channels):
            v = int(4_000_000 * math.sin(2 * math.pi * (i + ch) / 16))
            out += struct.pack("<i", v)[:3]
    return bytes(out)


def sine_f32(frames: int, channels: int) -> bytes:
    out = bytearray()
    for i in range(frames):
        for ch in range(channels):
            out += struct.pack("<f", math.sin(2 * math.pi * (i + ch) / 16))
    return bytes(out)


def main() -> None:
    corpus = Path(__file__).resolve().parent.parent / "fuzz" / "corpus"
    corpus.mkdir(parents=True, exist_ok=True)

    seeds: dict[str, bytes] = {
        "mono_16bit_48k.wav": wav(fmt_chunk(1, 1, 48000, 16), data_chunk(sine_s16(32, 1))),
        "stereo_24bit_44k.wav": wav(fmt_chunk(1, 2, 44100, 24), data_chunk(sine_s24(16, 2))),
        "mono_8bit_8k.wav": wav(
            fmt_chunk(1, 1, 8000, 8),
            data_chunk(bytes((128 + int(100 * math.sin(i / 3))) & 0xFF for i in range(33))),  # odd size -> pad
        ),
        "eight_ch_f32_96k.wav": wav(fmt_chunk(3, 8, 96000, 32), data_chunk(sine_f32(8, 8))),
        "extensible_16ch_f32.wav": wav(
            fmt_chunk(3, 16, 48000, 32, extensible=True), data_chunk(sine_f32(4, 16))
        ),
        "extensible_4ch_pcm16.wav": wav(
            fmt_chunk(1, 4, 48000, 16, extensible=True), data_chunk(sine_s16(8, 4))
        ),
        "junk_chunk_then_audio.wav": wav(
            b"JUNK" + struct.pack("<I", 3) + b"abc\x00",
            fmt_chunk(1, 1, 48000, 16),
            data_chunk(sine_s16(8, 1)),
        ),
        # Near-valid mutants: keep coverage adjacent to the rejection paths.
        "mutant_data_size_lie.wav": wav(
            fmt_chunk(1, 1, 48000, 16), b"data" + struct.pack("<I", 0xFFFF) + sine_s16(4, 1)
        ),
        "mutant_zero_channels.wav": wav(fmt_chunk(1, 1, 48000, 16)[:10] + b"\x00\x00" + fmt_chunk(1, 1, 48000, 16)[12:], data_chunk(sine_s16(4, 1))),
        "mutant_truncated.wav": wav(fmt_chunk(1, 1, 48000, 16), data_chunk(sine_s16(32, 1)))[:40],
    }

    for name, blob in seeds.items():
        assert len(blob) < 10_000, f"{name} too large for a seed ({len(blob)} bytes)"
        (corpus / name).write_bytes(blob)
        print(f"wrote {corpus / name} ({len(blob)} bytes)")


if __name__ == "__main__":
    main()
