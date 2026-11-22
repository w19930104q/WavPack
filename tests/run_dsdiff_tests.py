#!/usr/bin/env python3
# Acceptance tests for DSDIFF (DFF) multichannel handling in WavPack.
#
# Verifies that a DFF whose DSD chunk size is not an even multiple of the
# declared channel count is deterministically rejected (no silent sample
# truncation), and that well-formed files roundtrip bit-for-bit with the
# exact expected sample count.
#
# Usage: run_dsdiff_tests.py <path-to-build-dir>

import os
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_dff import make_dff

# (channels, data_len, expected_samples_per_channel or None to require rejection)
MATRIX = [
    (2, 4096, 2048),   # even length, divisible
    (2, 4097, None),   # odd length, NOT divisible -> must reject (customer case)
    (2, 4098, 2049),   # even length, divisible
    (2, 4095, None),   # odd length, NOT divisible -> must reject
    (6, 4092, 682),    # even length, divisible
    (6, 4097, None),   # odd length, NOT divisible -> must reject
    (6, 4098, 683),    # even length, divisible
    (6, 4095, None),   # odd length, NOT divisible -> must reject
    (3, 4095, 1365),   # odd length but divisible by odd channel count -> valid
    (1, 4097, 4097),   # odd length mono (legal in DFF) -> valid
]

EXPECTED_ERROR = "is not a valid .DFF file!"

failures = []

def check(ok, msg):
    tag = "PASS" if ok else "FAIL"
    print(f"  [{tag}] {msg}")
    if not ok:
        failures.append(msg)

def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)

def main():
    build_dir = sys.argv[1] if len(sys.argv) > 1 else "build"
    wavpack = os.path.join(build_dir, "wavpack")
    wvunpack = os.path.join(build_dir, "wvunpack")
    for exe in (wavpack, wvunpack):
        if not os.path.isfile(exe):
            sys.exit(f"missing executable: {exe}")

    with tempfile.TemporaryDirectory(prefix="dfftest_") as tmp:
        for channels, data_len, expected in MATRIX:
            name = f"m{channels}ch_{data_len}"
            src = os.path.join(tmp, name + ".dff")
            wv = os.path.join(tmp, name + ".wv")
            with open(src, "wb") as f:
                f.write(make_dff(channels, data_len, seed=channels * 1000 + data_len % 97))

            print(f"case: {channels}ch, DSD data = {data_len} bytes, "
                  f"expect {'reject' if expected is None else str(expected) + ' samples/ch'}")

            r1 = run([wavpack, "-y", "-q", src, "-o", wv])

            if expected is None:
                check(r1.returncode != 0, "encoder rejects non-integral frame count")
                check(EXPECTED_ERROR in r1.stderr,
                      f"error message is {EXPECTED_ERROR!r} (got: {r1.stderr.strip()!r})")
                r2 = run([wavpack, "-y", "-q", src, "-o", wv])
                check(r1.stderr == r2.stderr and r1.returncode == r2.returncode,
                      "rejection is deterministic (identical message on rerun)")
                check(not os.path.exists(wv), "no residual .wv output after rejection")
                leftovers = [f for f in os.listdir(tmp) if f.startswith(name) and f != name + ".dff"]
                check(not leftovers, f"no leftover artifacts ({leftovers})")
            else:
                check(r1.returncode == 0, f"encoder accepts (rc={r1.returncode}, {r1.stderr.strip()!r})")
                if r1.returncode != 0:
                    continue

                # decoded sample count must match source-extractable count
                raw = os.path.join(tmp, name + ".raw")
                rr = run([wvunpack, "-y", "-q", "-r", wv, "-o", raw])
                check(rr.returncode == 0, "decode to raw succeeds")
                got = os.path.getsize(raw) if os.path.exists(raw) else -1
                check(got == expected * channels,
                      f"decoded sample bytes = {got} (expect {expected * channels} "
                      f"= {expected} samples x {channels} ch)")

                # roundtrip must be bit-identical
                restored = os.path.join(tmp, name + ".out.dff")
                rd = run([wvunpack, "-y", "-q", wv, "-o", restored])
                check(rd.returncode == 0, "decode to .dff succeeds")
                same = os.path.exists(restored) and \
                    open(restored, "rb").read() == open(src, "rb").read()
                check(same, "roundtrip is bit-identical")

    print()
    if failures:
        print(f"{len(failures)} FAILURE(S)")
        return 1
    print("ALL TESTS PASSED")
    return 0

if __name__ == "__main__":
    sys.exit(main())
