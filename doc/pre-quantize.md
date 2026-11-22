# WavPack Pre-Quantization Contract

This document is the normative description of the `--pre-quantize` and
`--pre-quantize-round` options of the `wavpack` command-line encoder. The
implementation (`cli/wavpack.c`), this document, and the acceptance test
(`cli/prequantize-tests`) are kept in agreement; the test hardcodes a matrix
of (input bit depth × pre-quantize bits → expected samples and MD5) and
verifies the implementation against it.

## 1. Options

```
--pre-quantize=bits        quantize integer samples by truncation
--pre-quantize-round=bits  quantize integer samples by rounding (half up)
```

`bits` (call it *q*) must be in the range 4–32. The operation is performed by
the command-line program on the samples as they are read from the source,
**before** encoding and **before** the MD5 checksum is calculated. The source
file itself is never modified.

## 2. Definitions

* **Container depth B** — the number of bits each sample actually occupies in
  the source: `bytes-per-sample × 8` (8, 16, 24, or 32 for PCM).
* **Declared depth D** — the bits-per-sample figure in the source header
  (D ≤ B; e.g. 20-bit data in a 3-byte container has D = 20, B = 24).
* **q** — the argument of `--pre-quantize` / `--pre-quantize-round`.

*q is always measured against the container depth B*, never against D. This
is what allows `--pre-quantize` to zero out non-audio padding bits (e.g.
`--pre-quantize=20` on a 20-bit-in-3-byte file, or `--pre-quantize=24` on a
24-bit-in-4-byte file).

## 3. Applicability matrix (input depth × q)

| Source            | B  | Effective q | Operation                          |
|-------------------|----|-------------|------------------------------------|
| 8-bit integer     | 8  | 4–7         | integer rule (§4)                  |
| 16-bit integer    | 16 | 4–15        | integer rule (§4)                  |
| 24-bit integer    | 24 | 4–23        | integer rule (§4)                  |
| 32-bit integer    | 32 | 4–31        | integer rule (§4)                  |
| 32-bit float      | 32 | 4–31        | float rule (§5)                    |
| any, q ≥ B        | —  | —           | no-op, warning issued              |
| DSD (any q)       | —  | —           | ignored, warning issued            |

The same rules apply both when encoding a fresh source and when re-encoding
an existing WavPack file. (Re-encoding DSD with `--pre-quantize` is likewise
a no-op with a warning; it never alters the DSD payload.)

## 4. Integer rule (public rounding rule)

Let `s` be a sample as a signed B-bit integer and `quantum = 2^(B−q)`.

* **`--pre-quantize` (truncate):**
  `s′ = floor(s / quantum) × quantum`
  (equivalently, clearing the low `B−q` bits of the two's-complement value).

* **`--pre-quantize-round` (round half up):**
  `s′ = floor((s + quantum/2) / quantum) × quantum`,
  **except** when `s ≥ 0` and `s + quantum/2 ≥ 2^(B−1)` — i.e. rounding would
  overflow positive full-scale — in which case the sample is truncated
  instead: `s′ = floor(s / quantum) × quantum`.

8-bit WAV data is stored unsigned. The rule is applied to the signed value
`u − 128`; because 128 is a multiple of every legal quantum, this gives the
same result as applying the rule directly to the stored unsigned byte.

## 5. Float rule

For 32-bit IEEE-754 float sources with normalization exponent E (E = 127 for
standard full-scale float data, i.e. peak magnitude 1.0):

```
f′ = floor(f × 2^(q + 126 − E) + 0.5) × 2^(E − 126 − q)
```

computed in double precision and rounded back to binary32. Float data is
**always rounded half up** (`--pre-quantize-round` has no additional effect)
and is **never clipped** — values beyond ±1.0 are quantized like any other.

## 6. Declared bit depth after pre-quantization

Pre-quantization does **not** change the declared bit depth of the stream.
The WavPack file keeps the source's bits-per-sample (D) and container size
(B); only the low-order sample bits are zeroed (or rounded). Consequently:

* `wvunpack -s` reports the *source* bit depth, not q.
* Decoding restores a file at the original declared depth whose samples are
  exactly the quantized values — i.e. the decoded file is bit-identical to
  what the source file would have been, had it been quantized on disk before
  encoding. Decoded values equal the expectations computed independently
  from the public rules in §4–§5.

## 7. MD5 checksum

The MD5 checksum stored in the WavPack stream is calculated over the
**quantized** PCM payload — the same bytes the decoder will output
(post-quantization, in the source's container format and stored channel
order). Consequently:

* `wvunpack -v` (and `-m`) verifies cleanly: the stored MD5 always matches
  the decoded output.
* When quantization is active, the stored MD5 does **not** match the MD5 of
  the original source file's PCM payload. This is intentional: the checksum
  protects the data actually encoded.
* When quantization is a no-op (q ≥ B, or DSD), the MD5 covers the original
  payload as usual.

## 8. Verification

`cli/prequantize-tests` encodes known inputs for every cell of a hardcoded
matrix (input depth × q × rounding → expected samples and expected MD5),
then checks that:

1. the decoded samples equal the hardcoded expected samples;
2. the MD5 stored in each file equals the hardcoded expected MD5, and equals
   the MD5 of the decoded payload (Rule 7);
3. the decoded file still declares the source bit depth (Rule 6);
4. the hardcoded matrix itself agrees with the public rounding rules of
   §4–§5 applied to the test inputs (so document, test and implementation
   cannot silently drift apart);
5. DSD sources are passed through unaltered with a warning, both on fresh
   encode and on re-encode.
