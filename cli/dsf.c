////////////////////////////////////////////////////////////////////////////
//                           **** WAVPACK ****                            //
//                  Hybrid Lossless Wavefile Compressor                   //
//                Copyright (c) 1998 - 2024 David Bryant.                 //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// dsf.c

// This module is a helper to the WavPack command-line programs to support DSF files.

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "wavpack.h"
#include "utils.h"

extern int debug_logging_mode;

#pragma pack(push,4)

typedef struct {
    char ckID [4];
    int64_t ckSize;
} DSFChunkHeader;

typedef struct {
    char ckID [4];
    int64_t ckSize;
    int64_t fileSize;
    int64_t metaOffset;
} DSFFileChunk;

typedef struct {
    char ckID [4];
    int64_t ckSize;
    uint32_t formatVersion, formatID;
    uint32_t chanType, numChannels, sampleRate, bitsPerSample;
    int64_t sampleCount;
    uint32_t blockSize, reserved;
} DSFFormatChunk;

#pragma pack(pop)

#define DSFChunkHeaderFormat "4D"
#define DSFFileChunkFormat "4DDD"
#define DSFFormatChunkFormat "4DLLLLLLDL4"

#define DSF_BLOCKSIZE 4096

static const uint16_t channel_masks [] = { 0x04, 0x03, 0x07, 0x33, 0x0f, 0x37, 0x3f };
#define NUM_CHAN_TYPES (sizeof (channel_masks) / sizeof (channel_masks [0]))

static int mask_popcount (uint16_t mask)
{
    int count = 0;

    while (mask) {
        count += mask & 1;
        mask >>= 1;
    }

    return count;
}

int ParseDsfHeaderConfig (FILE *infile, char *infilename, char *fourcc, WavpackContext *wpc, WavpackConfig *config)
{
    int64_t infilesize, total_samples, total_blocks, leftover_samples, data_payload_size, data_bytes_available;
    DSFFileChunk file_chunk;
    DSFFormatChunk format_chunk;
    DSFChunkHeader chunk_header;

    uint32_t bcount;

    infilesize = DoGetFileSize (infile);
    memcpy (&file_chunk, fourcc, 4);

    if ((!DoReadFile (infile, ((char *) &file_chunk) + 4, sizeof (DSFFileChunk) - 4, &bcount) ||
        bcount != sizeof (DSFFileChunk) - 4)) {
            error_line ("%s is not a valid .DSF file!", infilename);
            return WAVPACK_SOFT_ERROR;
    }
    else if (!(config->qmode & QMODE_NO_STORE_WRAPPER) &&
        !WavpackAddWrapper (wpc, &file_chunk, sizeof (DSFFileChunk))) {
            error_line ("%s", WavpackGetErrorMessage (wpc));
            return WAVPACK_SOFT_ERROR;
    }

#if 1   // this might be a little too picky...
    WavpackLittleEndianToNative (&file_chunk, DSFFileChunkFormat);

    if (debug_logging_mode)
        error_line ("file header lengths = %lld, %lld, %lld", file_chunk.ckSize, file_chunk.fileSize, file_chunk.metaOffset);

    if (infilesize && !(config->qmode & QMODE_IGNORE_LENGTH) &&
        file_chunk.fileSize && file_chunk.fileSize + 1 && file_chunk.fileSize != infilesize) {
            error_line ("%s is not a valid .DSF file (by total size)!", infilename);
            return WAVPACK_SOFT_ERROR;
    }
#endif

    if (config->channel_mask || (config->qmode & QMODE_CHANS_UNASSIGNED)) {
        error_line ("this DSF file already has channel order information!");
        return WAVPACK_SOFT_ERROR;
    }

    if (!DoReadFile (infile, ((char *) &format_chunk), sizeof (DSFFormatChunk), &bcount) ||
        bcount != sizeof (DSFFormatChunk) || strncmp (format_chunk.ckID, "fmt ", 4)) {
            error_line ("%s is not a valid .DSF file!", infilename);
            return WAVPACK_SOFT_ERROR;
    }
    else if (!(config->qmode & QMODE_NO_STORE_WRAPPER) &&
        !WavpackAddWrapper (wpc, &format_chunk, sizeof (DSFFormatChunk))) {
            error_line ("%s", WavpackGetErrorMessage (wpc));
            return WAVPACK_SOFT_ERROR;
    }

    WavpackLittleEndianToNative (&format_chunk, DSFFormatChunkFormat);

    if (format_chunk.ckSize != sizeof (DSFFormatChunk) || format_chunk.formatVersion != 1 ||
        format_chunk.formatID != 0 || format_chunk.blockSize != DSF_BLOCKSIZE || format_chunk.reserved ||
        format_chunk.sampleCount <= 0 || format_chunk.sampleCount > MAX_WAVPACK_SAMPLES * 8 ||
        (format_chunk.bitsPerSample != 1 && format_chunk.bitsPerSample != 8) ||
        format_chunk.numChannels < 1 || format_chunk.numChannels > 6 ||
        format_chunk.chanType < 1 || format_chunk.chanType > NUM_CHAN_TYPES) {
            error_line ("%s is not a valid .DSF file!", infilename);
            return WAVPACK_SOFT_ERROR;
    }

    // The channel type declares a speaker layout whose number of channels must
    // match the actual channel count. A mismatch cannot be archived losslessly
    // (the resulting WavPack file is unreadable by our own decoder), so reject
    // the file deterministically instead of silently producing a bad archive.

    if (mask_popcount (channel_masks [format_chunk.chanType - 1]) != (int) format_chunk.numChannels) {
        error_line ("%s is not a valid .DSF file (channel type %u implies %d channel(s), but %u channel(s) are declared)!",
            infilename, format_chunk.chanType,
            mask_popcount (channel_masks [format_chunk.chanType - 1]), format_chunk.numChannels);
        return WAVPACK_SOFT_ERROR;
    }

    if (debug_logging_mode) {
        error_line ("sampling rate = %d Hz", format_chunk.sampleRate);
        error_line ("channel type = %d, channel count = %d", format_chunk.chanType, format_chunk.numChannels);
        error_line ("block size = %d, bits per sample = %d", format_chunk.blockSize, format_chunk.bitsPerSample);
        error_line ("sample count = %lld", format_chunk.sampleCount);
    }

    if (!DoReadFile (infile, ((char *) &chunk_header), sizeof (DSFChunkHeader), &bcount) ||
        bcount != sizeof (DSFChunkHeader) || strncmp (chunk_header.ckID, "data", 4)) {
            error_line ("%s is not a valid .DSF file!", infilename);
            return WAVPACK_SOFT_ERROR;
    }
    else if (!(config->qmode & QMODE_NO_STORE_WRAPPER) &&
        !WavpackAddWrapper (wpc, &chunk_header, sizeof (DSFChunkHeader))) {
            error_line ("%s", WavpackGetErrorMessage (wpc));
            return WAVPACK_SOFT_ERROR;
    }

    WavpackLittleEndianToNative (&chunk_header, DSFChunkHeaderFormat);

    total_samples = format_chunk.sampleCount;
    total_blocks = total_samples / (format_chunk.blockSize * 8);
    leftover_samples = total_samples - (total_blocks * format_chunk.blockSize * 8);

    if (leftover_samples)
        total_blocks++;

    // The data chunk size and the actual file contents must agree with the
    // sample count in the format chunk. A mismatch means any archive would
    // either truncate audio or silently invent/omit bytes, so reject it
    // deterministically (an optional ID3v2 tag after the data chunk is fine).

    data_payload_size = chunk_header.ckSize - 12;

    if (chunk_header.ckSize < 12 ||
        data_payload_size != total_blocks * DSF_BLOCKSIZE * format_chunk.numChannels) {
            error_line ("%s is not a valid .DSF file (data chunk size %lld does not match sample count %lld)!",
                infilename, (long long) data_payload_size, (long long) total_samples);
            return WAVPACK_SOFT_ERROR;
    }

    data_bytes_available = infilesize - DoGetFilePosition (infile);

    if (infilesize && data_bytes_available < data_payload_size) {
        error_line ("%s is not a valid .DSF file (truncated data: needs %lld byte(s), only %lld available)!",
            infilename, (long long) data_payload_size, (long long) data_bytes_available);
        return WAVPACK_SOFT_ERROR;
    }

    if (debug_logging_mode) {
        error_line ("leftover samples = %lld, leftover bits = %d", leftover_samples, (int)(leftover_samples % 8));
        error_line ("data chunk size (specified) = %lld", chunk_header.ckSize - 12);
        error_line ("data chunk size (calculated) = %lld", total_blocks * DSF_BLOCKSIZE * format_chunk.numChannels);
    }

    // If the final block is partial (the sample count does not fill it), the
    // unused trailing bytes must be zero padding: the decoder re-creates that
    // padding, so any non-zero value there could never be restored. The unused
    // high bits of the final byte (when the sample count is not a multiple of
    // 8) are stored verbatim and do round-trip, so they are not checked here.
    // Seekable regular files are validated up front; for non-seekable inputs
    // (e.g. pipes) the packer enforces the same rule while reading blocks.

    if (leftover_samples && infilesize) {
        int64_t data_start_position = DoGetFilePosition (infile);
        uint32_t valid_bytes = (uint32_t) ((total_samples + 7) / 8);
        uint32_t valid_in_last_block = (uint32_t) (valid_bytes - (total_blocks - 1) * DSF_BLOCKSIZE);
        unsigned char *padding_check;
        int64_t block_base;
        int ch;

        if (valid_in_last_block > DSF_BLOCKSIZE || data_start_position < 0 ||
            !(padding_check = malloc ((size_t) DSF_BLOCKSIZE * format_chunk.numChannels))) {
            error_line ("%s is not a valid .DSF file!", infilename);
            return WAVPACK_SOFT_ERROR;
        }

        block_base = (total_blocks - 1) * DSF_BLOCKSIZE * format_chunk.numChannels;

        if (DoSetFilePositionAbsolute (infile, data_start_position + block_base) ||
            !DoReadFile (infile, padding_check, DSF_BLOCKSIZE * format_chunk.numChannels, &bcount) ||
            bcount != DSF_BLOCKSIZE * format_chunk.numChannels) {
                error_line ("%s is not a valid .DSF file (unreadable trailing block)!", infilename);
                free (padding_check);
                return WAVPACK_SOFT_ERROR;
        }

        for (ch = 0; ch < (int) format_chunk.numChannels; ++ch) {
            uint32_t i;

            for (i = valid_in_last_block; i < DSF_BLOCKSIZE; ++i)
                if (padding_check [ch * DSF_BLOCKSIZE + i])
                    break;

            if (i != DSF_BLOCKSIZE) {
                error_line ("%s is not a valid .DSF file (non-zero padding in partial final block; cannot archive losslessly)!",
                    infilename);
                free (padding_check);
                return WAVPACK_SOFT_ERROR;
            }
        }

        free (padding_check);

        if (DoSetFilePositionAbsolute (infile, data_start_position)) {
            error_line ("%s is not a valid .DSF file!", infilename);
            return WAVPACK_SOFT_ERROR;
        }

        if (total_samples & 0x7)
            error_line ("warning: DSF file has partial-byte leftover samples");
    }
    else if (total_samples & 0x7)
        error_line ("warning: DSF file has partial-byte leftover samples");

    if (format_chunk.sampleRate & 0x7)
        error_line ("warning: DSF file has non-integer bytes/second!");

    config->bits_per_sample = 8;
    config->bytes_per_sample = 1;
    config->num_channels = format_chunk.numChannels;
    config->channel_mask = channel_masks [format_chunk.chanType - 1];
    config->sample_rate = (format_chunk.sampleRate + 7) / 8;

    if (format_chunk.bitsPerSample == 1)
        config->qmode |= QMODE_DSD_LSB_FIRST | QMODE_DSD_IN_BLOCKS;
    else
        config->qmode |= QMODE_DSD_MSB_FIRST | QMODE_DSD_IN_BLOCKS;

    if (!WavpackSetConfiguration64 (wpc, config, (total_samples + 7) / 8, NULL)) {
        error_line ("%s: %s", infilename, WavpackGetErrorMessage (wpc));
        return WAVPACK_SOFT_ERROR;
    }

    return WAVPACK_NO_ERROR;
}
