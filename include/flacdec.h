////////////////////////////////////////////////////////////////////////////
//                           **** WAVPACK ****                            //
//                  Hybrid Lossless Wavefile Compressor                   //
//                Copyright (c) 1998 - 2026 David Bryant.                 //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// flacdec.h

// This module provides a self-contained FLAC (Free Lossless Audio Codec)
// decoder that allows the WavPack command-line programs (and library
// clients) to read FLAC files without any third-party dependencies
// (i.e., libFLAC is NOT required).
//
// Only the commonly used subset is supported: 16-bit and 24-bit integer
// PCM with 1 to 8 channels at any valid FLAC sample rate. Files outside
// this subset (e.g., 8-bit or 32-bit) are rejected with a descriptive
// error rather than being silently misconverted. No assumptions are made
// about compression level, block partitioning, or frame structure.
//
// The decoder is strictly sequential (no seeking), so it works with
// pipes as well as regular files.

#ifndef FLACDEC_H
#define FLACDEC_H

#include <stdio.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FlacDecoder_s FlacDecoder;

// Open a FLAC stream for decoding. The FILE must be positioned immediately
// after the 4-byte "fLaC" marker (i.e., the marker has already been consumed
// and verified by the caller). All metadata blocks are parsed during this
// call; on success the stream is positioned at the first audio frame.
// On failure NULL is returned and, if provided, "error" is filled with a
// descriptive message.

FlacDecoder *FlacDecOpen (FILE *infile, char *error, int error_size);

// Stream parameters (valid after a successful FlacDecOpen).

uint32_t FlacDecGetSampleRate (const FlacDecoder *dec);
int FlacDecGetNumChannels (const FlacDecoder *dec);
int FlacDecGetBitsPerSample (const FlacDecoder *dec);

// Total samples per channel from STREAMINFO, or -1 if the stream does
// not declare a length.

int64_t FlacDecGetTotalSamples (const FlacDecoder *dec);

// The 16-byte audio MD5 from STREAMINFO. Returns 1 if the stream declares
// a non-zero MD5, otherwise 0 (and "md5" is untouched).

int FlacDecGetMD5 (const FlacDecoder *dec, unsigned char md5 [16]);

// Number of samples (per channel) successfully decoded so far.

uint64_t FlacDecGetSamplesDecoded (const FlacDecoder *dec);

// Vorbis comment access (for tag mapping). Each item has a name and a
// value (both NUL-terminated UTF-8).

int FlacDecGetNumTagItems (const FlacDecoder *dec);
const char *FlacDecGetTagItemName (const FlacDecoder *dec, int index);
const char *FlacDecGetTagItemValue (const FlacDecoder *dec, int index);

// Embedded picture access (for cover art mapping).

int FlacDecGetNumPictures (const FlacDecoder *dec);
int FlacDecGetPicture (const FlacDecoder *dec, int index, uint32_t *type,
    const char **mime, const unsigned char **data, uint32_t *data_size);

// Decode audio. Up to "max_samples" samples (per channel) are decoded and
// written to "buffer" as interleaved little-endian signed PCM in the
// stream's native container width (2 bytes for 16-bit, 3 bytes for
// 24-bit); the caller must provide max_samples * channels * container
// bytes of space. This byte layout is exactly what the FLAC MD5 covers.
//
// Returns the number of samples (per channel) written, 0 at end of
// stream, or -1 on a decode error (use FlacDecGetError() for details).
// If an error occurs after a partial delivery, the partial count is
// returned first and -1 is returned on the next call.

int64_t FlacDecRead (FlacDecoder *dec, unsigned char *buffer, int64_t max_samples);

// Retrieve the error message for the most recent failure.

const char *FlacDecGetError (const FlacDecoder *dec);

// Free all resources. The FILE passed to FlacDecOpen is NOT closed.

void FlacDecClose (FlacDecoder *dec);

#ifdef __cplusplus
}
#endif

#endif
