#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Acceptance test for --channel-order round-trip fidelity.

Verifies that:
  1. Encoding with any legal --channel-order permutation and decoding restores
     every sample of every channel bit-for-bit (4/6/8-channel permutation
     matrices hardcoded below).
  2. Files encoded WITHOUT --channel-order (and files whose --channel-order is
     already the Microsoft order) produce .wv files byte-identical to the
     pre-fix encoder (golden SHA-256 hashes captured from commit 5d7837c).
  3. The channel mask recorded in the file headers matches the actual channel
     arrangement, validated with parsers written independently from the RIFF
     and WavPack block specifications (no WavPack code is used for parsing).

Usage:
    python3 tests/test_channel_order.py [wavpack-binary] [wvunpack-binary]

Defaults: "wavpack" and "wvunpack" from PATH. A temporary working directory is
created automatically and removed on success.
"""

import hashlib
import os
import shutil
import struct
import subprocess
import sys
import tempfile

# ---------------------------------------------------------------------------
# Hardcoded test matrices
# ---------------------------------------------------------------------------

# Microsoft speaker bit positions, mirroring the RIFF dwChannelMask definition
# (and the "speakers" table in the WavPack CLI).
SPEAKERS = ["FL", "FR", "FC", "LFE", "BL", "BR", "FLC", "FRC", "BC",
            "SL", "SR", "TC", "TFL", "TFC", "TFR", "TBL", "TBC", "TBR"]

# Permutation matrices: for each channel count, a subset of all permutations
# of a fixed speaker set (identity, adjacent swaps, rotations, reversal, and
# the exact 6-channel case from the bug report). Every permutation of a set
# shares the same expected channel mask.
PERM_MATRICES = {
    4: [
        ("FL", "FR", "BL", "BR"),          # identity (Microsoft quad order)
        ("FR", "FL", "BL", "BR"),          # front pair swapped
        ("FL", "FR", "BR", "BL"),          # back pair swapped
        ("FR", "FL", "BR", "BL"),          # both pairs swapped
        ("BR", "BL", "FR", "FL"),          # full reversal
        ("BL", "BR", "FL", "FR"),          # half rotation
        ("FR", "BL", "BR", "FL"),          # rotation by one
        ("BL", "FR", "BR", "FL"),          # mixed
    ],
    6: [
        ("FL", "FR", "FC", "LFE", "BL", "BR"),   # identity (Microsoft 5.1)
        ("FR", "FL", "FC", "LFE", "BR", "BL"),   # the reported bug case
        ("FL", "FR", "FC", "LFE", "BR", "BL"),   # surround pair swapped
        ("FR", "FL", "LFE", "FC", "BL", "BR"),   # front/center shuffled
        ("BR", "BL", "LFE", "FC", "FR", "FL"),   # full reversal
        ("FR", "FC", "LFE", "BL", "BR", "FL"),   # rotation by one
        ("BL", "BR", "LFE", "FC", "FR", "FL"),   # surround pair moved to front
        ("FC", "LFE", "FL", "FR", "BR", "BL"),   # mixed
    ],
    8: [
        ("FL", "FR", "FC", "LFE", "BL", "BR", "SL", "SR"),   # identity (7.1)
        ("FR", "FL", "FC", "LFE", "BR", "BL", "SR", "SL"),   # all pairs swapped
        ("FL", "FR", "FC", "LFE", "BR", "BL", "SL", "SR"),   # surround swapped
        ("FL", "FR", "FC", "LFE", "BL", "BR", "SR", "SL"),   # side pair swapped
        ("SR", "SL", "BR", "BL", "LFE", "FC", "FR", "FL"),   # full reversal
        ("FR", "FC", "LFE", "BL", "BR", "SL", "SR", "FL"),   # rotation by one
        ("SL", "SR", "FL", "FR", "FC", "LFE", "BL", "BR"),   # sides to front
        ("BR", "BL", "FC", "LFE", "FL", "FR", "SR", "SL"),   # mixed
    ],
}

# Golden SHA-256 hashes of .wv files produced by the PRE-FIX encoder
# (commit 5d7837c) for the deterministic sources generated below. The default
# path (no --channel-order) and the identity-order path must remain
# byte-identical to these forever.
GOLDEN_WV_SHA256 = {
    # (nch, order or None for default)
    (1, None): "f92e39a7c519d9e06ca4ea2e6ffa2aa1518db838e24e424c3de5a22c833a847d",
    (2, None): "f67afde9fe686e01e6e0d6e3067d1639e385e82b79cbf6df54adb603bd16bf71",
    (4, None): "f119d290ef408e755fab32f0ad9eb349784fe29b5d4bd92fc12d98e39aaeb966",
    (6, None): "b2bdfacb78c66a03d0d6f79f2e2f40bceeeef3cace75220a5e2627b5d2faeb08",
    (8, None): "21c3945b13f4498309fd9f6e12c7df11e15c95afd99850e2a40cebf6d4b562e6",
    (4, ("FL", "FR", "BL", "BR")):
        "68833cfe85c1bdcb45080b44f37318039118fccd83a2a283a510477dfafe7f28",
    (6, ("FL", "FR", "FC", "LFE", "BL", "BR")):
        "b2bdfacb78c66a03d0d6f79f2e2f40bceeeef3cace75220a5e2627b5d2faeb08",
    (8, ("FL", "FR", "FC", "LFE", "BL", "BR", "SL", "SR")):
        "ca771e3b90669e871661e4a4a4829bab727366ed623a960edff6d0e6d267c4ad",
}

SAMPLE_RATE = 44100
NUM_FRAMES = 5000

# ---------------------------------------------------------------------------
# Source generation (deterministic)
# ---------------------------------------------------------------------------

def sample_value(ch, frame):
    """Distinct, easily identified pattern per channel."""
    return ((ch + 1) * 2000 + (frame % 1000)) % 30000 - 15000


def generate_samples(nch):
    """Return samples as frames[n][ch] of 16-bit ints."""
    return [[sample_value(ch, f) for ch in range(nch)] for f in range(NUM_FRAMES)]


def write_wav_pcm16(path, nch, frames):
    """Write a plain PCM (non-extensible) 16-bit WAV, like a DAW without
    channel layout metadata would."""
    block = nch * 2
    data = bytearray()
    for frame in frames:
        for v in frame:
            data += struct.pack("<h", v)
    fmt = struct.pack("<HHIIHH", 1, nch, SAMPLE_RATE, SAMPLE_RATE * block, block, 16)
    riff = (b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt +
            b"data" + struct.pack("<I", len(data)) + bytes(data))
    with open(path, "wb") as fo:
        fo.write(b"RIFF" + struct.pack("<I", len(riff)) + riff)


def write_raw_pcm16(path, nch, frames):
    with open(path, "wb") as fo:
        for frame in frames:
            for v in frame:
                fo.write(struct.pack("<h", v))

# ---------------------------------------------------------------------------
# Independent parsers (written from the RIFF / WavPack block specs only)
# ---------------------------------------------------------------------------

def riff_parse(path):
    """Minimal RIFF/WAVE parser per the Microsoft multimedia spec.

    Returns dict with num_channels, format_tag, channel_mask (None if the fmt
    chunk is not WAVEFORMATEXTENSIBLE), and the raw data-chunk bytes."""
    with open(path, "rb") as fi:
        blob = fi.read()
    assert blob[0:4] == b"RIFF", "not RIFF"
    assert blob[8:12] == b"WAVE", "not WAVE"
    assert struct.unpack("<I", blob[4:8])[0] == len(blob) - 8, "bad RIFF size"
    result = {"channel_mask": None, "data": None}
    pos = 12
    while pos + 8 <= len(blob):
        cid = blob[pos:pos + 4]
        size = struct.unpack("<I", blob[pos + 4:pos + 8])[0]
        body = blob[pos + 8:pos + 8 + size]
        assert len(body) == size, "truncated chunk"
        if cid == b"fmt ":
            (result["format_tag"], result["num_channels"], result["sample_rate"],
             _br, result["block_align"], result["bits"]) = struct.unpack("<HHIIHH", body[:16])
            if result["format_tag"] == 0xFFFE:
                cb_size = struct.unpack("<H", body[16:18])[0]
                assert cb_size >= 22, "extensible fmt too small"
                result["channel_mask"] = struct.unpack("<I", body[20:24])[0]
        elif cid == b"data":
            result["data"] = body
        pos += 8 + size + (size & 1)     # chunks are word-aligned
    assert result["data"] is not None, "no data chunk"
    return result


def wav_data_to_frames(data, nch):
    frames = []
    block = nch * 2
    assert len(data) % block == 0
    for off in range(0, len(data), block):
        frames.append([struct.unpack("<h", data[off + 2 * c:off + 2 * c + 2])[0]
                       for c in range(nch)])
    return frames


def wavpack_parse(path):
    """Minimal WavPack block/metadata parser per the published file format.

    Returns (num_channels, channel_mask, qmode_byte, reorder_string_or_None)
    from the first block's ID_CHANNEL_INFO (0x0d) and ID_NEW_CONFIG_BLOCK
    (0x2a) metadata."""
    with open(path, "rb") as fi:
        blob = fi.read()
    assert blob[0:4] == b"wvpk", "not a WavPack file"
    block_end = 8 + struct.unpack("<I", blob[4:8])[0]
    pos = 32  # sizeof(WavpackHeader)
    info = {"num_channels": None, "channel_mask": None,
            "qmode": 0, "reorder": None}
    while pos + 2 <= block_end:
        meta_id = blob[pos]
        large = meta_id & 0x80
        odd = meta_id & 0x40
        meta_id &= 0x3f
        if large:
            words = blob[pos + 1] | (blob[pos + 2] << 8) | (blob[pos + 3] << 16)
            hdr = 4
        else:
            words = blob[pos + 1]
            hdr = 2
        nbytes = words * 2 - (1 if odd else 0)
        body = blob[pos + hdr:pos + hdr + nbytes]
        assert len(body) == nbytes, "truncated metadata"
        if meta_id == 0x0d:                       # ID_CHANNEL_INFO
            info["num_channels"] = body[0]
            info["channel_mask"] = int.from_bytes(body[1:], "little")
        elif meta_id == 0x2a:                     # ID_NEW_CONFIG_BLOCK
            if len(body) >= 2:
                info["qmode"] = body[1]
            if len(body) >= 4:
                nchans = body[3]
                reorder = list(body[4:])
                reorder += list(range(len(reorder), nchans))  # implicit identity tail
                info["reorder"] = reorder
        pos += hdr + words * 2
    return info

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def speaker_mask(order):
    mask = 0
    for name in order:
        mask |= 1 << SPEAKERS.index(name)
    return mask


def dense_permutation(order):
    """Replicates the CLI's remapping of speaker indices to a dense 0-based
    permutation (the reorder string expected in the file)."""
    dense = [SPEAKERS.index(name) for name in order]
    i = 0
    while i < len(dense):
        if i in dense:
            i += 1
        else:
            dense = [d - 1 if d > i else d for d in dense]
    return dense


def is_microsoft_order(order):
    return dense_permutation(order) == list(range(len(order)))


def sha256_file(path):
    with open(path, "rb") as fi:
        return hashlib.sha256(fi.read()).hexdigest()


def run(cmd):
    res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if res.returncode != 0:
        raise AssertionError("command failed (%d): %s\n%s" %
                             (res.returncode, " ".join(cmd),
                              res.stderr.decode("utf-8", "replace")))

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

def test_default_path_unchanged(wavpack, wvunpack, workdir):
    """Encoding without --channel-order must be byte-identical to the pre-fix
    encoder, and decoding must reproduce the source bit-for-bit."""
    for nch in (1, 2, 4, 6, 8):
        frames = generate_samples(nch)
        src = os.path.join(workdir, "def%d.wav" % nch)
        wv = os.path.join(workdir, "def%d.wv" % nch)
        back = os.path.join(workdir, "def%d_back.wav" % nch)
        write_wav_pcm16(src, nch, frames)
        run([wavpack, "-y", "-q", src, "-o", wv])
        digest = sha256_file(wv)
        assert digest == GOLDEN_WV_SHA256[(nch, None)], \
            "default-path %dch .wv differs from pre-fix golden" % nch
        run([wvunpack, "-y", "-q", wv, "-o", back])
        parsed = riff_parse(back)
        assert wav_data_to_frames(parsed["data"], nch) == frames, \
            "default-path %dch round-trip samples differ" % nch
    print("ok: default path byte-identical to pre-fix encoder (1/2/4/6/8 ch)")


def test_identity_order_unchanged(wavpack, workdir):
    """--channel-order naming the Microsoft order must not change the output
    at all (no reordering metadata is stored)."""
    for nch in (4, 6, 8):
        order = PERM_MATRICES[nch][0]
        assert is_microsoft_order(order)
        frames = generate_samples(nch)
        src = os.path.join(workdir, "id%d.wav" % nch)
        wv = os.path.join(workdir, "id%d.wv" % nch)
        write_wav_pcm16(src, nch, frames)
        run([wavpack, "-y", "-q", "--channel-order=" + ",".join(order), src, "-o", wv])
        digest = sha256_file(wv)
        assert digest == GOLDEN_WV_SHA256[(nch, order)], \
            "identity-order %dch .wv differs from pre-fix golden" % nch
        info = wavpack_parse(wv)
        assert info["reorder"] is None and not (info["qmode"] & 0x8), \
            "identity-order %dch unexpectedly stores reordering info" % nch
    print("ok: Microsoft-order --channel-order output unchanged (4/6/8 ch)")


def test_permutation_roundtrips(wavpack, wvunpack, workdir):
    """Every permutation in the matrices must round-trip bit-for-bit, and the
    .wv must record a channel mask matching the stored (Microsoft-order)
    arrangement plus the exact reorder string."""
    for nch, matrix in sorted(PERM_MATRICES.items()):
        frames = generate_samples(nch)
        src = os.path.join(workdir, "p%d.wav" % nch)
        write_wav_pcm16(src, nch, frames)
        expected_mask = speaker_mask(PERM_MATRICES[nch][0])
        for order in matrix:
            assert sorted(order) == sorted(PERM_MATRICES[nch][0]), "bad matrix"
            tag = "x".join(str(SPEAKERS.index(s)) for s in order)
            wv = os.path.join(workdir, "p%d_%s.wv" % (nch, tag))
            back = os.path.join(workdir, "p%d_%s.wav" % (nch, tag))
            run([wavpack, "-y", "-q", "--channel-order=" + ",".join(order), src, "-o", wv])
            run([wvunpack, "-y", "-q", wv, "-o", back])

            # 1. per-sample assertion on every frame of every channel
            parsed = riff_parse(back)
            assert parsed["num_channels"] == nch
            got = wav_data_to_frames(parsed["data"], nch)
            assert len(got) == len(frames)
            for f in range(len(frames)):
                for c in range(nch):
                    assert got[f][c] == frames[f][c], \
                        "%s: frame %d channel %d (%s) mismatch" % (order, f, c, order[c])

            # 2. the mask recorded in the .wv must be exactly the bits of the
            #    specified speakers (the stored samples are in Microsoft
            #    order, which this mask describes)
            info = wavpack_parse(wv)
            assert info["num_channels"] == nch
            assert info["channel_mask"] == expected_mask, \
                "%s: .wv mask %#x != expected %#x" % (order, info["channel_mask"], expected_mask)

            # 3. non-Microsoft orders must store the exact reorder string
            if is_microsoft_order(order):
                assert info["reorder"] is None and not (info["qmode"] & 0x8)
            else:
                assert info["qmode"] & 0x8, "%s: QMODE_REORDERED_CHANS not stored" % (order,)
                assert info["reorder"] == dense_permutation(order), \
                    "%s: reorder string %s != expected %s" % \
                    (order, info["reorder"], dense_permutation(order))
    print("ok: all %d permutations round-trip bit-exactly with correct metadata" %
          sum(len(m) for m in PERM_MATRICES.values()))


def test_riff_mask_independent(wavpack, wvunpack, workdir):
    """Raw-PCM sources have no wrapper to restore, so the decoder generates a
    fresh WAVEFORMATEXTENSIBLE header: validate its dwChannelMask with an
    independent RIFF parser and check the samples bit-for-bit."""
    for nch, matrix in sorted(PERM_MATRICES.items()):
        frames = generate_samples(nch)
        raw = os.path.join(workdir, "raw%d.pcm" % nch)
        write_raw_pcm16(raw, nch, frames)
        expected_mask = speaker_mask(matrix[0])
        for order in matrix:
            tag = "x".join(str(SPEAKERS.index(s)) for s in order)
            wv = os.path.join(workdir, "r%d_%s.wv" % (nch, tag))
            back = os.path.join(workdir, "r%d_%s.wav" % (nch, tag))
            run([wavpack, "-y", "-q", "--raw-pcm=%d,16,%d" % (SAMPLE_RATE, nch),
                 "--channel-order=" + ",".join(order), raw, "-o", wv])
            run([wvunpack, "-y", "-q", wv, "-o", back])
            parsed = riff_parse(back)
            assert parsed["format_tag"] == 0xFFFE, \
                "%s: expected WAVEFORMATEXTENSIBLE header" % (order,)
            assert parsed["num_channels"] == nch
            # the mask must name exactly the speakers of the arrangement
            assert parsed["channel_mask"] == expected_mask, \
                "%s: RIFF mask %#x != expected %#x" % (order, parsed["channel_mask"], expected_mask)
            assert bin(parsed["channel_mask"]).count("1") == nch
            got = wav_data_to_frames(parsed["data"], nch)
            for f in range(len(frames)):
                for c in range(nch):
                    assert got[f][c] == frames[f][c], \
                        "%s: frame %d channel %d mismatch" % (order, f, c)
    print("ok: generated RIFF headers carry the correct channel mask (independent parse)")


def main():
    wavpack = sys.argv[1] if len(sys.argv) > 1 else "wavpack"
    wvunpack = sys.argv[2] if len(sys.argv) > 2 else "wvunpack"
    workdir = tempfile.mkdtemp(prefix="wp_channel_order_")
    try:
        test_default_path_unchanged(wavpack, wvunpack, workdir)
        test_identity_order_unchanged(wavpack, workdir)
        test_permutation_roundtrips(wavpack, wvunpack, workdir)
        test_riff_mask_independent(wavpack, wvunpack, workdir)
    finally:
        shutil.rmtree(workdir, ignore_errors=True)
    print("PASS: all channel-order acceptance tests passed")


if __name__ == "__main__":
    main()
