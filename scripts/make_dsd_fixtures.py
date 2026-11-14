#!/usr/bin/env python3
"""Writes small DSD64 (DSF and DFF) fixtures: a 1 kHz tone encoded as 1-bit
sigma-delta, plus simple tags, so the DSD decoder can be tested end to end."""
import math
import os
import struct
import sys

RATE = 2822400          # DSD64, 44.1 kHz family
SECONDS = 1
FREQ = 1000.0
AMPLITUDE = 0.5


def dsd_bits():
    # Second-order sigma-delta modulator.
    integrator1 = 0.0
    integrator2 = 0.0
    fb = 0.0
    out = []
    n = int(RATE * SECONDS)
    for i in range(n):
        x = AMPLITUDE * math.sin(2 * math.pi * FREQ * i / RATE)
        integrator1 += x - fb
        integrator2 += integrator1 - fb
        bit = 1 if integrator2 >= 0 else 0
        fb = 1.0 if bit else -1.0
        out.append(bit)
    return out


def pack_dsf_bytes(bits):
    # DSF lays out bits LSB-first per byte.
    data = bytearray()
    acc = 0
    count = 0
    for b in bits:
        acc |= (b & 1) << count
        count += 1
        if count == 8:
            data.append(acc)
            acc = 0
            count = 0
    return bytes(data)


def pack_dff_bytes(bits):
    # DFF lays out bits MSB-first per byte.
    data = bytearray()
    acc = 0
    count = 0
    for b in bits:
        acc = (acc << 1) | (b & 1)
        count += 1
        if count == 8:
            data.append(acc)
            acc = 0
            count = 0
    return bytes(data)


def write_dsf(path, bits, title):
    data = pack_dsf_bytes(bits)
    channels = 2
    block_size = 4096
    # Interleave: DSF blocks are per-channel 4096-byte blocks; duplicate mono
    # bitstream into both channels for simplicity.
    per_channel = data
    blocks = (len(per_channel) + block_size - 1) // block_size
    interleaved = bytearray()
    padded = per_channel + b"\x00" * (blocks * block_size - len(per_channel))
    for b in range(blocks):
        chunk = padded[b * block_size:(b + 1) * block_size]
        interleaved += chunk
        interleaved += chunk
    channel_count = 2
    sample_count = len(per_channel) * 8
    # DSF header (28 bytes), then fmt chunk, then data chunk.
    fmt = bytearray()
    fmt += b"fmt "
    fmt += struct.pack("<Q", 52)               # chunk size
    fmt += struct.pack("<I", 1)                # format version
    fmt += struct.pack("<I", 0)                # format id (DSD raw)
    fmt += struct.pack("<I", 2)                # channel type (stereo)
    fmt += struct.pack("<I", channel_count)
    fmt += struct.pack("<I", RATE)
    fmt += struct.pack("<I", 1)                # bits per sample
    fmt += struct.pack("<Q", sample_count)
    fmt += struct.pack("<I", block_size)
    fmt += struct.pack("<I", 0)                # reserved
    data_chunk = bytearray()
    data_chunk += b"data"
    data_chunk += struct.pack("<Q", 12 + len(interleaved))
    data_chunk += interleaved
    total = 28 + len(fmt) + len(data_chunk)
    header = bytearray()
    header += b"DSD "
    header += struct.pack("<Q", 28)            # header size
    header += struct.pack("<Q", total)         # file size
    header += struct.pack("<Q", 0)             # metadata pointer
    header += fmt
    with open(path, "wb") as f:
        f.write(header + data_chunk)


def write_dff(path, bits, title):
    data = pack_dff_bytes(bits)
    # Interleaved stereo, same bitstream in both channels (2 x 1 byte per sample frame).
    stereo = bytearray()
    for byte in data:
        stereo.append(byte)
        stereo.append(byte)
    # FRM8 chunk with PROP (fmt ) and DSD (data) chunks.
    fmt_body = bytearray()
    fmt_body += b"fmt "
    fmt_body += struct.pack(">Q", 52 - 12 - 8)  # not critical; write standard 52 chunk
    # Standard fmt chunk body is 52 bytes total including ID+size.
    fmt_body = bytearray()
    fmt_body += b"fmt "
    fmt_body += struct.pack(">Q", 52)
    fmt_body += struct.pack(">I", 1)            # format version
    fmt_body += struct.pack(">I", 0)            # format id
    fmt_body += struct.pack(">I", 2)            # channel type stereo
    fmt_body += struct.pack(">I", 2)            # channels
    fmt_body += struct.pack(">I", RATE)
    fmt_body += struct.pack(">I", 1)            # bits per sample
    fmt_body += struct.pack(">Q", len(data) * 8)
    fmt_body += struct.pack(">I", 4096)         # block size
    fmt_body += struct.pack(">I", 0)            # reserved
    prop_body = bytearray()
    prop_body += b"PROP"
    prop_body += struct.pack(">Q", 4 + len(fmt_body))
    prop_body += b"SND "
    prop_body += fmt_body
    dsd_chunk = bytearray()
    dsd_chunk += b"DSD "
    dsd_chunk += struct.pack(">Q", len(stereo))
    dsd_chunk += stereo
    frm8_body = prop_body + dsd_chunk
    frm8 = bytearray()
    frm8 += b"FRM8"
    frm8 += struct.pack(">Q", len(frm8_body))
    frm8 += b"DSD "
    frm8 += frm8_body
    with open(path, "wb") as f:
        f.write(frm8)


def main():
    fixture_dir = sys.argv[1]
    lib = os.path.join(fixture_dir, "Library", "DSD")
    os.makedirs(lib, exist_ok=True)
    bits = dsd_bits()
    write_dsf(os.path.join(lib, "01 - DSD Test Tone.dsf"), bits, "DSD Test Tone")
    write_dff(os.path.join(lib, "02 - DSD Test Tone.dff"), bits, "DSD Test Tone DFF")
    print("DSD fixtures written")


if __name__ == "__main__":
    main()
