////////////////////////////////////////////////////////////////////////////
//                           **** WAVPACK ****                            //
//                  Hybrid Lossless Wavefile Compressor                   //
//                Copyright (c) 1998 - 2024 David Bryant.                 //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// caff.c

// This module is a helper to the WavPack command-line programs to support CAF files.

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#include "wavpack.h"
#include "utils.h"

extern int debug_logging_mode;

typedef struct
{
    char mFileType [4];
    uint16_t mFileVersion;
    uint16_t mFileFlags;
} CAFFileHeader;

#define CAFFileHeaderFormat "4SS"

#pragma pack(push,4)
typedef struct
{
    char mChunkType [4];
    int64_t mChunkSize;
} CAFChunkHeader;
#pragma pack(pop)

#define CAFChunkHeaderFormat "4D"

typedef struct
{
    double mSampleRate;
    char mFormatID [4];
    uint32_t mFormatFlags;
    uint32_t mBytesPerPacket;
    uint32_t mFramesPerPacket;
    uint32_t mChannelsPerFrame;
    uint32_t mBitsPerChannel;
} CAFAudioFormat;

#define CAFAudioFormatFormat "D4LLLLL"
#define CAF_FORMAT_FLOAT            0x1
#define CAF_FORMAT_LITTLE_ENDIAN    0x2

int ParseCaffHeaderConfig (FILE *infile, char *infilename, char *fourcc, WavpackContext *wpc, WavpackConfig *config)
{
    uint32_t chan_chunk = 0, desc_chunk = 0, channel_layout = 0, bcount;
    unsigned char *channel_identities = NULL;
    unsigned char *channel_reorder = NULL;
    int64_t total_samples = 0, infilesize;
    CAFFileHeader caf_file_header;
    CAFChunkHeader caf_chunk_header;
    CAFAudioFormat caf_audio_format;

    infilesize = DoGetFileSize (infile);
    memcpy (&caf_file_header, fourcc, 4);

    if ((!DoReadFile (infile, ((char *) &caf_file_header) + 4, sizeof (CAFFileHeader) - 4, &bcount) ||
        bcount != sizeof (CAFFileHeader) - 4)) {
            error_line ("%s is not a valid .CAF file!", infilename);
            return WAVPACK_SOFT_ERROR;
    }
    else if (!(config->qmode & QMODE_NO_STORE_WRAPPER) &&
        !WavpackAddWrapper (wpc, &caf_file_header, sizeof (CAFFileHeader))) {
            error_line ("%s", WavpackGetErrorMessage (wpc));
            return WAVPACK_SOFT_ERROR;
    }

    WavpackBigEndianToNative (&caf_file_header, CAFFileHeaderFormat);

    if (caf_file_header.mFileVersion != 1) {
        error_line ("%s: can't handle version %d .CAF files!", infilename, caf_file_header.mFileVersion);
        return WAVPACK_SOFT_ERROR;
    }

    // loop through all elements of the RIFF wav header
    // (until the data chuck) and copy them to the output file

    while (1) {
        if (!DoReadFile (infile, &caf_chunk_header, sizeof (CAFChunkHeader), &bcount) ||
            bcount != sizeof (CAFChunkHeader)) {
                error_line ("%s is not a valid .CAF file!", infilename);
                return WAVPACK_SOFT_ERROR;
        }
        else if (!(config->qmode & QMODE_NO_STORE_WRAPPER) &&
            !WavpackAddWrapper (wpc, &caf_chunk_header, sizeof (CAFChunkHeader))) {
                error_line ("%s", WavpackGetErrorMessage (wpc));
                return WAVPACK_SOFT_ERROR;
        }

        WavpackBigEndianToNative (&caf_chunk_header, CAFChunkHeaderFormat);

        // if it's the format chunk, we want to get some info out of there and
        // make sure it's a .caf file we can handle

        if (!strncmp (caf_chunk_header.mChunkType, "desc", 4)) {
            int supported = TRUE;

            if (caf_chunk_header.mChunkSize != sizeof (CAFAudioFormat) ||
                !DoReadFile (infile, &caf_audio_format, (uint32_t) caf_chunk_header.mChunkSize, &bcount) ||
                bcount != caf_chunk_header.mChunkSize) {
                    error_line ("%s is not a valid .CAF file!", infilename);
                    return WAVPACK_SOFT_ERROR;
            }
            else if (!(config->qmode & QMODE_NO_STORE_WRAPPER) &&
                !WavpackAddWrapper (wpc, &caf_audio_format, (uint32_t) caf_chunk_header.mChunkSize)) {
                    error_line ("%s", WavpackGetErrorMessage (wpc));
                    return WAVPACK_SOFT_ERROR;
            }

            WavpackBigEndianToNative (&caf_audio_format, CAFAudioFormatFormat);
            desc_chunk = 1;

            if (debug_logging_mode) {
                char formatstr [5];

                memcpy (formatstr, caf_audio_format.mFormatID, 4);
                formatstr [4] = 0;
                error_line ("format = %s, flags = %x, sampling rate = %g",
                    formatstr, caf_audio_format.mFormatFlags, caf_audio_format.mSampleRate);
                error_line ("packet = %d bytes and %d frames",
                    caf_audio_format.mBytesPerPacket, caf_audio_format.mFramesPerPacket);
                error_line ("channels per frame = %d, bits per channel = %d",
                    caf_audio_format.mChannelsPerFrame, caf_audio_format.mBitsPerChannel);
            }

            if (strncmp (caf_audio_format.mFormatID, "lpcm", 4) || (caf_audio_format.mFormatFlags & ~3))
                supported = FALSE;
            else if (!(caf_audio_format.mSampleRate > 0.0 && caf_audio_format.mSampleRate <= 16777215.0))
                supported = FALSE;
            else if (!caf_audio_format.mChannelsPerFrame || caf_audio_format.mChannelsPerFrame > WAVPACK_MAX_CLI_CHANS)
                supported = FALSE;
            else if (caf_audio_format.mBitsPerChannel < 1 || caf_audio_format.mBitsPerChannel > 32 ||
                ((caf_audio_format.mFormatFlags & CAF_FORMAT_FLOAT) && caf_audio_format.mBitsPerChannel != 32))
                    supported = FALSE;
            else if (caf_audio_format.mFramesPerPacket != 1 ||
                caf_audio_format.mBytesPerPacket / caf_audio_format.mChannelsPerFrame < (caf_audio_format.mBitsPerChannel + 7) / 8 ||
                caf_audio_format.mBytesPerPacket / caf_audio_format.mChannelsPerFrame > 4 ||
                caf_audio_format.mBytesPerPacket % caf_audio_format.mChannelsPerFrame)
                    supported = FALSE;

            if (!supported) {
                error_line ("%s is an unsupported .CAF format!", infilename);
                return WAVPACK_SOFT_ERROR;
            }

            config->bytes_per_sample = caf_audio_format.mBytesPerPacket / caf_audio_format.mChannelsPerFrame;
            config->float_norm_exp = (caf_audio_format.mFormatFlags & CAF_FORMAT_FLOAT) ? 127 : 0;
            config->bits_per_sample = caf_audio_format.mBitsPerChannel;
            config->num_channels = caf_audio_format.mChannelsPerFrame;

            if ((config->qmode & QMODE_EVEN_BYTE_DEPTH) && (config->bits_per_sample % 8))
                config->bits_per_sample += 8 - (config->bits_per_sample % 8);

            if (caf_audio_format.mSampleRate != floor (caf_audio_format.mSampleRate))
                error_line ("warning: the nonintegral sample rate of %s will be rounded", infilename);

            if (caf_audio_format.mSampleRate < 1.0)
                config->sample_rate = 1;
            else
                config->sample_rate = (int) floor (caf_audio_format.mSampleRate + 0.5);

            if (!(caf_audio_format.mFormatFlags & CAF_FORMAT_LITTLE_ENDIAN) && config->bytes_per_sample > 1)
                config->qmode |= QMODE_BIG_ENDIAN;

            if (config->bytes_per_sample == 1)
                config->qmode |= QMODE_SIGNED_BYTES;

            if (debug_logging_mode) {
                if (config->float_norm_exp == 127)
                    error_line ("data format: 32-bit %s-endian floating point", (config->qmode & QMODE_BIG_ENDIAN) ? "big" : "little");
                else
                    error_line ("data format: %d-bit %s-endian integers stored in %d byte(s)",
                        config->bits_per_sample, (config->qmode & QMODE_BIG_ENDIAN) ? "big" : "little", config->bytes_per_sample);
            }
        }
        else if (!strncmp (caf_chunk_header.mChunkType, "chan", 4)) {
            AppleChannelLayout *apple_channel_layout;

            if (caf_chunk_header.mChunkSize < 0 || caf_chunk_header.mChunkSize > 1024 ||
                caf_chunk_header.mChunkSize < sizeof (AppleChannelLayout)) {
                    error_line ("this .CAF file has an invalid 'chan' chunk!");
                    return WAVPACK_SOFT_ERROR;
            }

            if (debug_logging_mode)
                error_line ("'chan' chunk is %d bytes", (int) caf_chunk_header.mChunkSize);

            apple_channel_layout = malloc ((size_t) caf_chunk_header.mChunkSize);

            if (!DoReadFile (infile, apple_channel_layout, (uint32_t) caf_chunk_header.mChunkSize, &bcount) ||
                bcount != caf_chunk_header.mChunkSize) {
                    error_line ("%s is not a valid .CAF file!", infilename);
                    free (apple_channel_layout);
                    return WAVPACK_SOFT_ERROR;
            }
            else if (!(config->qmode & QMODE_NO_STORE_WRAPPER) &&
                !WavpackAddWrapper (wpc, apple_channel_layout, (uint32_t) caf_chunk_header.mChunkSize)) {
                    error_line ("%s", WavpackGetErrorMessage (wpc));
                    free (apple_channel_layout);
                    return WAVPACK_SOFT_ERROR;
            }

            chan_chunk = 1;

            if (ParseAppleChannelLayout (apple_channel_layout, (uint32_t) caf_chunk_header.mChunkSize,
                config, &channel_layout, &channel_reorder, &channel_identities) == WAVPACK_SOFT_ERROR) {
                    free (apple_channel_layout);
                    return WAVPACK_SOFT_ERROR;
            }

            free (apple_channel_layout);
        }
        else if (!strncmp (caf_chunk_header.mChunkType, "data", 4)) {     // on the data chunk, get size and exit loop
            uint32_t mEditCount;

            if (!desc_chunk || !DoReadFile (infile, &mEditCount, sizeof (mEditCount), &bcount) ||
                bcount != sizeof (mEditCount)) {
                    error_line ("%s is not a valid .CAF file!", infilename);
                    return WAVPACK_SOFT_ERROR;
            }
            else if (!(config->qmode & QMODE_NO_STORE_WRAPPER) &&
                !WavpackAddWrapper (wpc, &mEditCount, sizeof (mEditCount))) {
                    error_line ("%s", WavpackGetErrorMessage (wpc));
                    return WAVPACK_SOFT_ERROR;
            }

            if ((config->qmode & QMODE_IGNORE_LENGTH) || caf_chunk_header.mChunkSize == -1) {
                config->qmode |= QMODE_IGNORE_LENGTH;

                if (infilesize && DoGetFilePosition (infile) != -1) {
                    total_samples = (infilesize - DoGetFilePosition (infile)) / caf_audio_format.mBytesPerPacket;

                    if ((infilesize - DoGetFilePosition (infile)) % caf_audio_format.mBytesPerPacket)
                        error_line ("warning: audio length does not divide evenly, %d bytes will be discarded!",
                            (int)((infilesize - DoGetFilePosition (infile)) % caf_audio_format.mBytesPerPacket));
                }
                else
                    total_samples = -1;
            }
            else {
                if (infilesize && infilesize - caf_chunk_header.mChunkSize > 16777216) {
                    error_line (".CAF file %s has over 16 MB of extra CAFF data, probably is corrupt!", infilename);
                    return WAVPACK_SOFT_ERROR;
                }

                if ((caf_chunk_header.mChunkSize - 4) % caf_audio_format.mBytesPerPacket) {
                    error_line (".CAF file %s has an invalid data chunk size, probably is corrupt!", infilename);
                    return WAVPACK_SOFT_ERROR;
                }

                total_samples = (caf_chunk_header.mChunkSize - 4) / caf_audio_format.mBytesPerPacket;

                if (!total_samples) {
                    error_line ("this .CAF file has no audio samples, probably is corrupt!");
                    return WAVPACK_SOFT_ERROR;
                }

                if (total_samples > MAX_WAVPACK_SAMPLES) {
                    error_line ("%s has too many samples for WavPack!", infilename);
                    return WAVPACK_SOFT_ERROR;
                }
            }

            break;
        }
        else {          // just copy unknown chunks to output file

            uint32_t bytes_to_copy = (uint32_t) caf_chunk_header.mChunkSize;
            char *buff;

            if (caf_chunk_header.mChunkSize < 0 || caf_chunk_header.mChunkSize > 1048576) {
                error_line ("%s is not a valid .CAF file!", infilename);
                return WAVPACK_SOFT_ERROR;
            }

            buff = malloc (bytes_to_copy);

            if (debug_logging_mode)
                error_line ("extra unknown chunk \"%c%c%c%c\" of %d bytes",
                    caf_chunk_header.mChunkType [0], caf_chunk_header.mChunkType [1], caf_chunk_header.mChunkType [2],
                    caf_chunk_header.mChunkType [3], caf_chunk_header.mChunkSize);

            if (!DoReadFile (infile, buff, bytes_to_copy, &bcount) ||
                bcount != bytes_to_copy ||
                (!(config->qmode & QMODE_NO_STORE_WRAPPER) &&
                !WavpackAddWrapper (wpc, buff, bytes_to_copy))) {
                    error_line ("%s", WavpackGetErrorMessage (wpc));
                    free (buff);
                    return WAVPACK_SOFT_ERROR;
            }

            free (buff);
        }
    }

    if (!chan_chunk && !config->channel_mask && config->num_channels <= 2 && !(config->qmode & QMODE_CHANS_UNASSIGNED))
        config->channel_mask = 0x5 - config->num_channels;

    if (!WavpackSetConfiguration64 (wpc, config, total_samples, channel_identities)) {
        error_line ("%s", WavpackGetErrorMessage (wpc));
        return WAVPACK_SOFT_ERROR;
    }

    if (channel_identities)
        free (channel_identities);

    if (channel_layout || channel_reorder) {
        if (!WavpackSetChannelLayout (wpc, channel_layout, channel_reorder)) {
            error_line ("problem with setting channel layout (should not happen)");
            return WAVPACK_SOFT_ERROR;
        }

        if (channel_reorder)
            free (channel_reorder);
    }

    return WAVPACK_NO_ERROR;
}
