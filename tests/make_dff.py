#!/usr/bin/env python3
# Generate minimal Philips DSDIFF (.dff) files for testing.
# Usage: make_dff.py <outfile> <num_channels> <data_len> [seed]
#
# The DSD data payload is a deterministic byte pattern so roundtrips can be
# verified bit-for-bit. Odd-sized chunks are padded to even per the DFF spec.

import struct
import sys

CHAN_IDS = ["SLFT", "SRGT", "C   ", "LFE ", "LS  ", "RS  "]

def chunk(ckid, payload):
    data = ckid + struct.pack(">Q", len(payload)) + payload
    if len(payload) & 1:
        data += b"\x00"                      # even-alignment pad byte
    return data

def make_dff(num_channels, data_len, seed):
    fver = chunk(b"FVER", struct.pack(">I", 0x01050000))
    fs   = chunk(b"FS  ", struct.pack(">I", 2822400))
    chnl = chunk(b"CHNL", struct.pack(">H", num_channels) +
                 b"".join(CHAN_IDS[i % len(CHAN_IDS)].encode("ascii")
                          for i in range(num_channels)))
    cmpr = chunk(b"CMPR", b"DSD " + b"\x0enot compressed")
    prop = chunk(b"PROP", b"SND " + fs + chnl + cmpr)

    # deterministic, position-dependent payload
    payload = bytes(((i * 37 + seed * 11) & 0xFF) for i in range(data_len))
    dsd = chunk(b"DSD ", payload)

    form = b"DSD " + fver + prop + dsd
    return b"FRM8" + struct.pack(">Q", len(form)) + form

if __name__ == "__main__":
    outfile, num_channels, data_len = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    seed = int(sys.argv[4]) if len(sys.argv) > 4 else 1
    with open(outfile, "wb") as f:
        f.write(make_dff(num_channels, data_len, seed))
