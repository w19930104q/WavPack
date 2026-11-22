#!/usr/bin/env python3
###########################################################################
#                           **** WAVPACK ****                             #
#                  Hybrid Lossless Wavefile Compressor                    #
#                Copyright (c) 1998 - 2026 David Bryant.                  #
#                          All Rights Reserved.                           #
#      Distributed under the BSD Software License (see license.txt)       #
###########################################################################

# flac_tests.py
#
# Deterministic FLAC sample-set generator and transcoding acceptance tests
# for the WavPack FLAC input support (cli/flac.c + src/flacdec.c).
#
# The generator is a small, self-contained FLAC *encoder* (no third-party
# code) that produces a fixed set of FLAC files exercising:
#
#   - 16-bit and 24-bit depths, 1/2/4/6/8 channels, several sample rates
#     (including one that requires the 16-bit rate field in frame headers)
#   - constant, verbatim, fixed (orders 0-4) and LPC subframes
#   - independent / left-side / right-side / mid-side stereo decorrelation
#   - wasted bits, escaped (uncompressed) residual partitions, varied
#     partition orders, varied block sizes (192 .. 4096 + odd tail block)
#   - Vorbis comments and embedded pictures (front/back cover)
#   - a stream with unknown length / no MD5
#   - deliberately corrupted files (bad STREAMINFO MD5, corrupted frame
#     data, truncated stream) and unsupported subsets (8-bit, 32-bit)
#
# The runner transcodes each good sample with the wavpack CLI, decodes the
# result back with wvunpack, and asserts:
#
#   - encode succeeds and the FLAC audio MD5 is verified during encode
#   - "wvunpack -v" verifies the stored MD5 (which equals the FLAC MD5)
#   - the decoded PCM is bit-identical to the generator's source PCM and
#     its MD5 equals the FLAC STREAMINFO MD5
#   - the decoded sample count matches the STREAMINFO declaration
#   - every Vorbis comment / picture appears in the APEv2 tag of the .wv
#
# Corrupted and unsupported samples must fail with a non-zero exit code, a
# matching error message, and no output file left behind.
#
# usage: flac_tests.py generate <outdir>
#        flac_tests.py run <workdir> --wavpack <exe> --wvunpack <exe> [--wvtag <exe>]

import hashlib
import json
import math
import os
import random
import re
import struct
import subprocess
import sys

# --------------------------------------------------------------------------
# CRC-8 (poly 0x07) and CRC-16 (poly 0x8005), MSB-first, init 0 (FLAC spec)
# --------------------------------------------------------------------------

def _crc8_table():
    tab = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = ((c << 1) ^ 0x07) & 0xff if c & 0x80 else (c << 1) & 0xff
        tab.append(c)
    return tab

def _crc16_table():
    tab = []
    for i in range(256):
        c = i << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x8005) & 0xffff if c & 0x8000 else (c << 1) & 0xffff
        tab.append(c)
    return tab

CRC8_TAB = _crc8_table()
CRC16_TAB = _crc16_table()

def crc8(data):
    c = 0
    for b in data:
        c = CRC8_TAB[c ^ b]
    return c

def crc16(data):
    c = 0
    for b in data:
        c = ((c << 8) ^ CRC16_TAB[((c >> 8) ^ b) & 0xff]) & 0xffff
    return c

# --------------------------------------------------------------------------
# bit writer (MSB first)
# --------------------------------------------------------------------------

class BitWriter:
    def __init__(self):
        self.out = bytearray()
        self.acc = 0
        self.n = 0

    def write(self, val, nbits):
        assert val >= 0 and val < (1 << nbits) if nbits else val == 0
        self.acc = (self.acc << nbits) | val
        self.n += nbits
        while self.n >= 8:
            self.n -= 8
            self.out.append((self.acc >> self.n) & 0xff)
        self.acc &= (1 << self.n) - 1

    def write_signed(self, val, nbits):
        assert -(1 << (nbits - 1)) <= val < (1 << (nbits - 1))
        self.write(val & ((1 << nbits) - 1), nbits)

    def write_unary(self, q):
        if q:
            self.write(0, q)
        self.write(1, 1)

    def align(self):
        if self.n:
            self.write(0, 8 - self.n)

    def bytes(self):
        assert self.n == 0
        return bytes(self.out)

# --------------------------------------------------------------------------
# FLAC encoder pieces
# --------------------------------------------------------------------------

FIXED_COEFS = {0: [], 1: [1], 2: [2, -1], 3: [3, -3, 1], 4: [4, -6, 4, -1]}
BLOCK_CODES = {192: 1, 576: 2, 1152: 3, 2304: 4, 4608: 5}
RATE_CODES = {88200: 1, 176400: 2, 192000: 3, 8000: 4, 16000: 5, 22050: 6,
              24000: 7, 32000: 8, 44100: 9, 48000: 10, 96000: 11}
BPS_CODES = {8: 1, 12: 2, 16: 4, 20: 5, 24: 6, 32: 7}

def fold(r):
    return (r << 1) if r >= 0 else ((-r) << 1) - 1

def rice_param(uvals):
    if not uvals:
        return 0
    mean = sum(uvals) // len(uvals)
    p = max(0, int(mean).bit_length() - 1)
    return min(p, 14)

def encode_residual(bw, residuals, block_size, order, porder, escape_partitions=()):
    assert block_size % (1 << porder) == 0
    bw.write(0, 2)                      # rice method, 4-bit parameter
    bw.write(porder, 4)
    idx = 0
    for p in range(1 << porder):
        count = (block_size >> porder) - (order if p == 0 else 0)
        part = residuals[idx:idx + count]
        idx += count
        if p in escape_partitions:
            bw.write(0xF, 4)            # escape code: raw residuals
            rawbits = max((abs(v).bit_length() + 1 for v in part), default=0)
            bw.write(rawbits, 5)
            for v in part:
                if rawbits:
                    bw.write_signed(v, rawbits)
        else:
            uvals = [fold(v) for v in part]
            param = rice_param(uvals)
            bw.write(param, 4)
            for u in uvals:
                bw.write_unary(u >> param)
                if param:
                    bw.write(u & ((1 << param) - 1), param)
    assert idx == len(residuals)

def encode_subframe(bw, samples, bps, spec):
    wasted = 0
    if spec[0] == 'wasted':
        wasted = spec[1]
        spec = spec[2]
        assert all(s % (1 << wasted) == 0 for s in samples)
        samples = [s >> wasted for s in samples]
        bps -= wasted

    kind = spec[0]
    if kind == 'constant':
        typecode = 0
    elif kind == 'verbatim':
        typecode = 1
    elif kind == 'fixed':
        typecode = 8 + spec[1]
    elif kind == 'lpc':
        typecode = 32 + len(spec[1]) - 1
    else:
        raise ValueError('bad subframe spec %r' % (spec,))

    bw.write(0, 1)                      # padding bit
    bw.write(typecode, 6)
    if wasted:
        bw.write(1, 1)
        bw.write_unary(wasted - 1)
    else:
        bw.write(0, 1)

    if kind == 'constant':
        assert all(s == samples[0] for s in samples)
        bw.write_signed(samples[0], bps)
    elif kind == 'verbatim':
        for s in samples:
            bw.write_signed(s, bps)
    else:
        if kind == 'fixed':
            order = spec[1]
            coefs = FIXED_COEFS[order]
            shift = 0
            porder = spec[2]
            esc = spec[3] if len(spec) > 3 else ()
        else:
            coefs = spec[1]
            order = len(coefs)
            shift = spec[2]
            porder = spec[3]
            esc = spec[4] if len(spec) > 4 else ()

        for i in range(order):
            bw.write_signed(samples[i], bps)

        if kind == 'lpc':
            prec = max(abs(c).bit_length() + 1 for c in coefs)
            assert prec <= 15
            bw.write(prec - 1, 4)
            bw.write_signed(shift, 5)
            for c in coefs:
                bw.write_signed(c, prec)

        residuals = []
        for i in range(order, len(samples)):
            pred = sum(coefs[j] * samples[i - 1 - j] for j in range(order)) >> shift
            residuals.append(samples[i] - pred)

        encode_residual(bw, residuals, len(samples), order, porder, esc)

def coded_number(n):
    if n < 0x80:
        return bytes([n])
    for nbytes, lead, leadbits in ((2, 0xC0, 5), (3, 0xE0, 4), (4, 0xF0, 3),
                                   (5, 0xF8, 2), (6, 0xFC, 1), (7, 0xFE, 0)):
        if n < (1 << (leadbits + 6 * (nbytes - 1))):
            out = [lead | (n >> (6 * (nbytes - 1)))]
            for k in range(nbytes - 2, -1, -1):
                out.append(0x80 | ((n >> (6 * k)) & 0x3F))
            return bytes(out)
    raise ValueError('coded number too large')

def encode_frame(frame_channels, bps, rate, assignment, specs, frame_index):
    nch = len(frame_channels)
    bs = len(frame_channels[0])
    assert all(len(c) == bs for c in frame_channels)

    bw = BitWriter()
    bw.write(0x3FFE, 14)                # sync
    bw.write(0, 1)                      # reserved
    bw.write(0, 1)                      # blocking strategy: fixed

    if bs in BLOCK_CODES:
        bcode, extra_b = BLOCK_CODES[bs], b''
    elif bs >= 256 and bs <= 32768 and bs % 256 == 0 and (bs // 256) & ((bs // 256) - 1) == 0:
        bcode, extra_b = 8 + (bs // 256).bit_length() - 1, b''
    elif bs <= 256:
        bcode, extra_b = 6, bytes([bs - 1])
    else:
        bcode, extra_b = 7, (bs - 1).to_bytes(2, 'big')

    if rate in RATE_CODES:
        rcode, extra_r = RATE_CODES[rate], b''
    elif rate % 1000 == 0 and rate // 1000 <= 255:
        rcode, extra_r = 12, bytes([rate // 1000])
    elif rate <= 65535:
        rcode, extra_r = 13, rate.to_bytes(2, 'big')
    elif rate % 10 == 0 and rate // 10 <= 65535:
        rcode, extra_r = 14, (rate // 10).to_bytes(2, 'big')
    else:
        raise ValueError('cannot code sample rate %d' % rate)

    bw.write(bcode, 4)
    bw.write(rcode, 4)

    if assignment == 'independent':
        acode = nch - 1
        chans = frame_channels
    else:
        acode = {'left_side': 8, 'right_side': 9, 'mid_side': 10}[assignment]
        left, right = frame_channels
        side = [l - r for l, r in zip(left, right)]
        if assignment == 'left_side':
            chans = [left, side]
        elif assignment == 'right_side':
            chans = [side, right]
        else:
            mid = [(l + r) >> 1 for l, r in zip(left, right)]
            chans = [mid, side]

    bw.write(acode, 4)
    bw.write(BPS_CODES[bps], 3)
    bw.write(0, 1)                      # reserved

    hdr = bw.bytes() + coded_number(frame_index) + extra_b + extra_r
    hdr += bytes([crc8(hdr)])

    bw = BitWriter()
    for c in range(nch):
        side_chan = (assignment in ('left_side', 'mid_side') and c == 1) or \
                    (assignment == 'right_side' and c == 0)
        encode_subframe(bw, chans[c], bps + (1 if side_chan else 0), specs[c])
    bw.align()

    frame = hdr + bw.bytes()
    frame += crc16(frame).to_bytes(2, 'big')
    return frame

# --------------------------------------------------------------------------
# FLAC metadata
# --------------------------------------------------------------------------

def streaminfo_block(min_bs, max_bs, min_fs, max_fs, rate, nch, bps, total, md5):
    out = bytearray()
    out += min_bs.to_bytes(2, 'big') + max_bs.to_bytes(2, 'big')
    out += min_fs.to_bytes(3, 'big') + max_fs.to_bytes(3, 'big')
    packed = (rate << 44) | ((nch - 1) << 41) | ((bps - 1) << 36) | total
    out += packed.to_bytes(8, 'big')
    out += md5
    return bytes(out)

def vorbis_comment_block(vendor, comments):
    out = bytearray()
    out += len(vendor).to_bytes(4, 'little') + vendor
    out += len(comments).to_bytes(4, 'little')
    for c in comments:
        out += len(c).to_bytes(4, 'little') + c
    return bytes(out)

def picture_block(ptype, mime, desc, data, w=0, h=0, depth=0, colors=0):
    out = bytearray()
    out += ptype.to_bytes(4, 'big')
    out += len(mime).to_bytes(4, 'big') + mime
    out += len(desc).to_bytes(4, 'big') + desc
    out += w.to_bytes(4, 'big') + h.to_bytes(4, 'big')
    out += depth.to_bytes(4, 'big') + colors.to_bytes(4, 'big')
    out += len(data).to_bytes(4, 'big') + data
    return bytes(out)

def flac_file(si, meta_blocks, frames):
    blocks = [(0, si)] + meta_blocks
    out = bytearray(b'fLaC')
    for i, (btype, payload) in enumerate(blocks):
        last = 0x80 if i == len(blocks) - 1 else 0
        out += bytes([last | btype]) + len(payload).to_bytes(3, 'big') + payload
    for f in frames:
        out += f
    return bytes(out)

# --------------------------------------------------------------------------
# PCM generation (deterministic)
# --------------------------------------------------------------------------

def clamp(v, bps):
    lo, hi = -(1 << (bps - 1)), (1 << (bps - 1)) - 1
    return max(lo, min(hi, v))

def gen_signals(seed, total, nch, bps, regions=()):
    """Generate per-channel PCM. regions: list of (start, end, style) where
    style is 'silence', 'noise' or 'wasted2'; anything not covered is a
    sine+noise mix."""
    rng = random.Random(seed)
    maxamp = (1 << (bps - 1)) - 1
    chans = []
    for c in range(nch):
        f1 = 0.005 + 0.0023 * c
        f2 = 0.017 + 0.0031 * c
        ph = rng.random() * 6.283
        sig = []
        for i in range(total):
            v = 0.55 * maxamp * math.sin(2 * math.pi * f1 * i + ph) \
              + 0.20 * maxamp * math.sin(2 * math.pi * f2 * i) \
              + rng.uniform(-0.02, 0.02) * maxamp
            sig.append(clamp(int(v), bps))
        chans.append(sig)

    for (start, end, style) in regions:
        for c in range(nch):
            if style == 'silence':
                for i in range(start, end):
                    chans[c][i] = 0
            elif style == 'noise':
                for i in range(start, end):
                    chans[c][i] = clamp(rng.randint(-maxamp, maxamp), bps)
            elif style == 'wasted2':
                for i in range(start, end):
                    chans[c][i] = chans[c][i] & ~3
    return chans

def pcm_to_bytes(chans, bps):
    container = (bps + 7) // 8
    out = bytearray()
    for i in range(len(chans[0])):
        for c in range(len(chans)):
            out += chans[c][i].to_bytes(container, 'little', signed=True)
    return bytes(out)

# --------------------------------------------------------------------------
# sample set construction
# --------------------------------------------------------------------------

# tiny valid 1x1 PNG (transparent) used as front cover
PNG_FRONT = bytes([
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
    0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4, 0x89, 0x00, 0x00, 0x00,
    0x0A, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0x00, 0x01, 0x00, 0x00,
    0x05, 0x00, 0x01, 0x0D, 0x0A, 0x2D, 0xB4, 0x00, 0x00, 0x00, 0x00, 0x49,
    0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82])

# a second, distinct "PNG" payload used as back cover
PNG_BACK = bytes([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A]) + \
    b'back-cover-payload-for-flac-tests' + bytes(64)

# this table mirrors tag_xlate[] in cli/flac.c; the test verifies that the
# C mapping produces exactly these APEv2 item names
TAG_XLATE = {
    'TITLE': 'Title', 'ARTIST': 'Artist', 'ALBUM': 'Album',
    'ALBUMARTIST': 'AlbumArtist', 'COMPOSER': 'Composer',
    'CONDUCTOR': 'Conductor', 'DATE': 'Year', 'TRACKNUMBER': 'Track',
    'DISCNUMBER': 'Disc', 'GENRE': 'Genre', 'COMMENT': 'Comment',
    'PERFORMER': 'Performer', 'ORGANIZATION': 'Publisher',
    'PUBLISHER': 'Publisher', 'COPYRIGHT': 'Copyright', 'ENCODER': 'Encoder',
    'ISRC': 'ISRC', 'LYRICS': 'Lyrics', 'BPM': 'BPM', 'MOOD': 'Mood',
}

PICTURE_TYPES = [
    'Other', 'Png Icon', 'Icon', 'Front', 'Back', 'Leaflet', 'Media',
    'Lead Artist', 'Artist', 'Conductor', 'Band', 'Composer', 'Lyricist',
    'Recording Location', 'During Recording', 'During Performance',
    'Video Capture', 'Phish', 'Illustration', 'Band Logotype',
    'Publisher Logotype']

def expected_ape_name(vorbis_name):
    return TAG_XLATE.get(vorbis_name.upper(), vorbis_name)

def build_flac(rate, nch, bps, frame_specs, seed=1234, comments=None,
               pictures=None, total_override=None, md5_override=None):
    """frame_specs: list of (block_size, assignment, [subframe spec per ch],
    pcm_style or None). Returns (flac_bytes, pcm_bytes, frame_offsets)."""
    total = sum(fs[0] for fs in frame_specs)
    regions = []
    pos = 0
    for (bs, _a, _s, style) in frame_specs:
        if style:
            regions.append((pos, pos + bs, style))
        pos += bs

    chans = gen_signals(seed, total, nch, bps, regions)
    pcm = pcm_to_bytes(chans, bps)
    md5 = hashlib.md5(pcm).digest() if md5_override is None else md5_override
    declared_total = total if total_override is None else total_override

    frames = []
    offsets = []
    pos = 0
    for idx, (bs, assignment, specs, _style) in enumerate(frame_specs):
        fchans = [c[pos:pos + bs] for c in chans]
        offsets.append(None)    # patched after assembly
        frames.append((fchans, assignment, specs, idx))
        pos += bs

    meta = []
    if comments is not None:
        meta.append((4, vorbis_comment_block(b'flac-tests 1.0',
                                             [c.encode('utf-8') for c in comments])))
    if pictures:
        for (ptype, mime, data) in pictures:
            meta.append((6, picture_block(ptype, mime, b'', data)))

    # first pass to compute frame sizes for STREAMINFO
    encoded = [encode_frame(fc, bps, rate, a, s, i) for (fc, a, s, i) in frames]
    sizes = [len(f) for f in encoded]
    blocksizes = [fs[0] for fs in frame_specs]

    si = streaminfo_block(min(blocksizes), max(blocksizes),
                          min(sizes), max(sizes), rate, nch, bps,
                          declared_total, md5)
    data = flac_file(si, meta, encoded)

    # record the offset of each frame inside the file
    offs = []
    p = len(data) - sum(sizes)
    for s in sizes:
        offs.append(p)
        p += s
    return data, pcm, offs

# --------------------------------------------------------------------------
# the fixed sample set
# --------------------------------------------------------------------------

def make_sample_set():
    """Returns a list of sample dicts."""
    samples = []

    stereo_frames = [
        (4096, 'independent', [('fixed', 2, 0), ('fixed', 2, 0)], None),
        (4096, 'independent', [('lpc', [1], 0, 1), ('lpc', [15, -7], 3, 2)], None),
        (4096, 'mid_side',    [('fixed', 1, 0), ('fixed', 1, 0)], None),
        (4096, 'left_side',   [('fixed', 2, 1), ('fixed', 2, 1)], None),
        (4096, 'right_side',  [('lpc', [2, -1], 1, 0), ('fixed', 1, 0)], None),
        (4096, 'independent', [('verbatim',), ('verbatim',)], 'noise'),
        (4096, 'independent', [('constant',), ('constant',)], 'silence'),
        (4096, 'independent', [('wasted', 2, ('fixed', 0, 0)),
                               ('wasted', 2, ('fixed', 0, 0))], 'wasted2'),
        (4096, 'independent', [('fixed', 1, 1, (1,)), ('fixed', 1, 1)], None),
        (4096, 'independent', [('fixed', 4, 3), ('fixed', 4, 3)], None),
        (192,  'independent', [('fixed', 1, 0), ('fixed', 1, 0)], None),
        (576,  'independent', [('fixed', 3, 2), ('fixed', 3, 2)], None),
        (1000, 'independent', [('fixed', 2, 0), ('fixed', 2, 0)], None),
    ]

    comments = [
        'TITLE=Flac Tests Are Fun',
        'ARTIST=WavPack Developers',
        'ALBUM=Transcoding Pipeline',
        'DATE=2026',
        'TRACKNUMBER=7',
        'GENRE=Test Tone',
        'COMMENT=16-bit stereo, mixed subframes',
        'MYFIELD=custom passthrough value',
    ]
    pictures = [(3, b'image/png', PNG_FRONT), (4, b'image/png', PNG_BACK)]

    data, pcm, _ = build_flac(44100, 2, 16, stereo_frames, seed=101,
                              comments=comments, pictures=pictures)
    samples.append(dict(
        name='stereo16_44k', data=data, pcm=pcm, expect='ok',
        rate=44100, nch=2, bps=16, comments=comments, pictures=pictures))

    # 16-bit mono
    mono_frames = [(3000, 'independent', [('fixed', 1, 2)], None),
                   (3000, 'independent', [('lpc', [2, -1], 1, 1)], None),
                   (3000, 'independent', [('fixed', 0, 0)], None)]
    data, pcm, _ = build_flac(22050, 1, 16, mono_frames, seed=102,
                              comments=['TITLE=Mono Test', 'ARTIST=Nobody'])
    samples.append(dict(
        name='mono16_22k', data=data, pcm=pcm, expect='ok',
        rate=22050, nch=1, bps=16,
        comments=['TITLE=Mono Test', 'ARTIST=Nobody'], pictures=None))

    # 24-bit 6-channel
    sur_frames = [(2048, 'independent', [('lpc', [2, -1], 1, 1)] * 6, None),
                  (2048, 'independent', [('fixed', 2, 0)] * 6, None)]
    data, pcm, _ = build_flac(48000, 6, 24, sur_frames, seed=103,
                              comments=['TITLE=Surround 24-bit'])
    samples.append(dict(
        name='surround24_48k', data=data, pcm=pcm, expect='ok',
        rate=48000, nch=6, bps=24, comments=['TITLE=Surround 24-bit'],
        pictures=None))

    # 16-bit 8-channel
    octa_frames = [(1024, 'independent', [('fixed', 3, 1)] * 8, None),
                   (1024, 'independent', [('fixed', 2, 2)] * 8, None)]
    data, pcm, _ = build_flac(32000, 8, 16, octa_frames, seed=104)
    samples.append(dict(
        name='octa16_32k', data=data, pcm=pcm, expect='ok',
        rate=32000, nch=8, bps=16, comments=None, pictures=None))

    # 24-bit 4-channel with a sample rate that needs the 16-bit header field
    quad_frames = [(192, 'independent', [('fixed', 1, 0)] * 4, None),
                   (1000, 'independent', [('fixed', 2, 0)] * 4, None)]
    data, pcm, _ = build_flac(44101, 4, 24, quad_frames, seed=105)
    samples.append(dict(
        name='quad24_44101', data=data, pcm=pcm, expect='ok',
        rate=44101, nch=4, bps=24, comments=None, pictures=None))

    # unknown length and no MD5 in STREAMINFO
    nl_frames = [(2048, 'independent', [('fixed', 1, 0)] * 2, None),
                 (2048, 'independent', [('fixed', 1, 0)] * 2, None)]
    data, pcm, _ = build_flac(44100, 2, 16, nl_frames, seed=106,
                              total_override=0, md5_override=bytes(16))
    samples.append(dict(
        name='nolength16_44k', data=data, pcm=pcm, expect='ok',
        rate=44100, nch=2, bps=16, comments=None, pictures=None))

    # --- deliberately corrupted / unsupported samples ---

    base_frames = [(2048, 'independent', [('fixed', 1, 0)] * 2, None),
                   (2048, 'independent', [('fixed', 2, 1)] * 2, None)]

    # STREAMINFO MD5 deliberately wrong -> must fail the MD5 verification
    bad_md5 = bytearray(hashlib.md5(b'not the audio').digest())
    data, pcm, _ = build_flac(44100, 2, 16, base_frames, seed=107,
                              md5_override=bytes(bad_md5))
    samples.append(dict(
        name='corrupt_md5', data=data, pcm=None, expect='fail',
        fail_pattern=r'MD5'))

    # one byte flipped inside frame audio data -> CRC-16 must catch it
    data, pcm, offs = build_flac(44100, 2, 16, base_frames, seed=108)
    data = bytearray(data)
    flip_at = offs[1] + 20        # well inside the second frame's data
    data[flip_at] ^= 0xFF
    samples.append(dict(
        name='corrupt_frame', data=bytes(data), pcm=None, expect='fail',
        fail_pattern=r'(?i)(crc|error|mismatch|unexpected)'))

    # truncated stream -> must fail
    data, pcm, offs = build_flac(44100, 2, 16, base_frames, seed=109)
    samples.append(dict(
        name='truncated', data=data[:int(len(data) * 0.6)], pcm=None,
        expect='fail', fail_pattern=r'(?i)(unexpected|error|samples)'))

    # unsupported bit depths -> clean, deterministic rejection
    d32_frames = [(256, 'independent', [('verbatim',)] * 2, 'noise')]
    data, pcm, _ = build_flac(44100, 2, 32, d32_frames, seed=110)
    samples.append(dict(
        name='depth32', data=data, pcm=None, expect='fail',
        fail_pattern=r'(?i)unsupported'))

    d8_frames = [(256, 'independent', [('fixed', 1, 0)], None)]
    data, pcm, _ = build_flac(44100, 1, 8, d8_frames, seed=111)
    samples.append(dict(
        name='depth8', data=data, pcm=None, expect='fail',
        fail_pattern=r'(?i)unsupported'))

    return samples

# --------------------------------------------------------------------------
# APEv2 parsing (for tag read-back verification)
# --------------------------------------------------------------------------

def parse_apev2(path):
    with open(path, 'rb') as f:
        data = f.read()
    assert len(data) >= 32, 'file too small for APEv2'
    footer = data[-32:]
    assert footer[:8] == b'APETAGEX', 'no APEv2 footer found'
    version, size, count, flags = struct.unpack('<IIII', footer[8:24])
    items = {}
    pos = len(data) - size
    end = len(data) - 32
    n = 0
    while pos < end:
        vsize, iflags = struct.unpack('<II', data[pos:pos + 8])
        pos += 8
        nul = data.index(b'\0', pos)
        name = data[pos:nul].decode('utf-8')
        pos = nul + 1
        value = data[pos:pos + vsize]
        pos += vsize
        items[name] = (iflags, value)
        n += 1
    assert n == count, 'APEv2 item count mismatch'
    return items

# --------------------------------------------------------------------------
# generate / run
# --------------------------------------------------------------------------

def write_sample_set(outdir):
    os.makedirs(outdir, exist_ok=True)
    manifest = []
    for s in make_sample_set():
        flac_path = os.path.join(outdir, s['name'] + '.flac')
        with open(flac_path, 'wb') as f:
            f.write(s['data'])
        entry = {'name': s['name'], 'file': flac_path, 'expect': s['expect']}
        if s['expect'] == 'ok':
            pcm_path = os.path.join(outdir, s['name'] + '.pcm')
            with open(pcm_path, 'wb') as f:
                f.write(s['pcm'])
            entry.update(
                pcm=pcm_path,
                pcm_md5=hashlib.md5(s['pcm']).hexdigest(),
                total_samples=len(s['pcm']) // (((s['bps'] + 7) // 8) * s['nch']),
                rate=s['rate'], nch=s['nch'], bps=s['bps'])
            if s.get('comments'):
                entry['tags'] = {expected_ape_name(c.split('=', 1)[0]):
                                 c.split('=', 1)[1] for c in s['comments']}
            if s.get('pictures'):
                entry['pictures'] = [
                    {'item': 'Cover Art (%s)' % PICTURE_TYPES[p[0]],
                     'md5': hashlib.md5(p[2]).hexdigest(),
                     'size': len(p[2])} for p in s['pictures']]
        else:
            entry['fail_pattern'] = s['fail_pattern']
        manifest.append(entry)

    with open(os.path.join(outdir, 'manifest.json'), 'w') as f:
        json.dump(manifest, f, indent=2)
    return manifest

def run_cmd(args):
    return subprocess.run(args, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, timeout=300)

class Failure(Exception):
    pass

def check(cond, msg):
    if not cond:
        raise Failure(msg)

def run_tests(workdir, wavpack, wvunpack, wvtag):
    sample_dir = os.path.join(workdir, 'samples')
    out_dir = os.path.join(workdir, 'out')
    os.makedirs(out_dir, exist_ok=True)
    manifest = write_sample_set(sample_dir)

    passed = 0
    for entry in manifest:
        name = entry['name']
        wv_path = os.path.join(out_dir, name + '.wv')
        raw_path = os.path.join(out_dir, name + '.raw')
        for p in (wv_path, raw_path):
            if os.path.exists(p):
                os.unlink(p)

        enc = run_cmd([wavpack, '-y', '-q', '-o', wv_path, entry['file']])
        stderr = enc.stderr.decode('utf-8', 'replace')

        if entry['expect'] == 'fail':
            check(enc.returncode != 0,
                  '%s: encode unexpectedly succeeded' % name)
            check(re.search(entry['fail_pattern'], stderr),
                  '%s: error message %r does not match %r' %
                  (name, stderr.strip(), entry['fail_pattern']))
            check(not os.path.exists(wv_path),
                  '%s: output file not deleted after failure' % name)
            print('ok   %-16s (rejected as expected: %s)' %
                  (name, stderr.strip().splitlines()[-1][:70]))
            passed += 1
            continue

        check(enc.returncode == 0,
              '%s: encode failed: %s' % (name, stderr.strip()))
        check(os.path.exists(wv_path), '%s: no output written' % name)

        # the stored MD5 (identical to the FLAC STREAMINFO MD5) must verify
        ver = run_cmd([wvunpack, '-v', wv_path])
        check(ver.returncode == 0,
              '%s: wvunpack -v failed: %s' %
              (name, ver.stderr.decode('utf-8', 'replace').strip()))

        # decode back to raw PCM and compare against the generator's source
        dec = run_cmd([wvunpack, '-y', '-q', '-r', '-o', raw_path, wv_path])
        check(dec.returncode == 0,
              '%s: wvunpack failed: %s' %
              (name, dec.stderr.decode('utf-8', 'replace').strip()))
        with open(raw_path, 'rb') as f:
            raw = f.read()

        container = (entry['bps'] + 7) // 8
        check(len(raw) == entry['total_samples'] * entry['nch'] * container,
              '%s: decoded sample count %d != STREAMINFO declares %d' %
              (name, len(raw) // (entry['nch'] * container),
               entry['total_samples']))
        check(hashlib.md5(raw).hexdigest() == entry['pcm_md5'],
              '%s: decoded PCM MD5 does not match FLAC STREAMINFO MD5' % name)
        with open(entry['pcm'], 'rb') as f:
            check(raw == f.read(), '%s: decoded PCM is not bit-identical' % name)

        # tags: every Vorbis comment and picture must be in the APEv2 tag
        if entry.get('tags') or entry.get('pictures'):
            items = parse_apev2(wv_path)
            for ape_name, value in (entry.get('tags') or {}).items():
                check(ape_name in items,
                      '%s: APEv2 item %r missing' % (name, ape_name))
                got = items[ape_name][1].decode('utf-8')
                check(got == value,
                      '%s: APEv2 item %r is %r, expected %r' %
                      (name, ape_name, got, value))
            for pic in (entry.get('pictures') or []):
                check(pic['item'] in items,
                      '%s: APEv2 item %r missing' % (name, pic['item']))
                value = items[pic['item']][1]
                nul = value.index(b'\0')
                payload = value[nul + 1:]
                check(len(payload) == pic['size'] and
                      hashlib.md5(payload).hexdigest() == pic['md5'],
                      '%s: cover art payload mismatch for %r' %
                      (name, pic['item']))

        if wvtag:
            tag = run_cmd([wvtag, '-l', wv_path])
            check(tag.returncode == 0, '%s: wvtag failed' % name)
            listing = tag.stdout.decode('utf-8', 'replace')
            for ape_name in (entry.get('tags') or {}):
                check(ape_name in listing,
                      '%s: wvtag listing missing %r' % (name, ape_name))

        print('ok   %-16s (%d ch, %d-bit, %d Hz, %d samples)' %
              (name, entry['nch'], entry['bps'], entry['rate'],
               entry['total_samples']))
        passed += 1

    print('\n%d/%d tests passed' % (passed, len(manifest)))
    return 0 if passed == len(manifest) else 1

def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2

    cmd = argv[1]
    if cmd == 'generate':
        write_sample_set(argv[2])
        print('sample set written to', argv[2])
        return 0
    elif cmd == 'run':
        wavpack = wvunpack = wvtag = None
        args = argv[3:]
        i = 0
        while i < len(args):
            if args[i] == '--wavpack':
                wavpack = args[i + 1]; i += 2
            elif args[i] == '--wvunpack':
                wvunpack = args[i + 1]; i += 2
            elif args[i] == '--wvtag':
                wvtag = args[i + 1]; i += 2
            else:
                i += 1
        if not wavpack or not wvunpack:
            print('error: --wavpack and --wvunpack are required')
            return 2
        try:
            return run_tests(argv[2], wavpack, wvunpack, wvtag)
        except Failure as e:
            print('FAIL:', e)
            return 1
    else:
        print(__doc__)
        return 2

if __name__ == '__main__':
    sys.exit(main(sys.argv))
