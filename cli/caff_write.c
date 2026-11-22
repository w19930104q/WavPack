////////////////////////////////////////////////////////////////////////////
//                           **** WAVPACK ****                            //
//                  Hybrid Lossless Wavefile Compressor                   //
//                Copyright (c) 1998 - 2024 David Bryant.                 //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// caff_write.c

// This module is a helper to the WavPack command-line programs to support CAF files.

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

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

int WriteCaffHeader (FILE *outfile, WavpackContext *wpc, int64_t total_samples, int qmode)
{
    CAFChunkHeader caf_desc_chunk_header, caf_chan_chunk_header, caf_data_chunk_header;
    CAFAudioFormat caf_audio_format;
    CAFFileHeader caf_file_header;
    uint32_t mEditCount, bcount;

    int num_channels = WavpackGetNumChannels (wpc);
    int32_t sample_rate = WavpackGetSampleRate (wpc);
    int bytes_per_sample = WavpackGetBytesPerSample (wpc);
    int bits_per_sample = WavpackGetBitsPerSample (wpc);
    int float_norm_exp = WavpackGetFloatNormExp (wpc);

    if (float_norm_exp && float_norm_exp != 127) {
        error_line ("invalid float data for CAFF, use --normalize-floats and omit MD5 check!");
        goto error_exit;
    }

    // format and write the CAF File Header

    memcpy (caf_file_header.mFileType, "caff", sizeof (caf_file_header.mFileType));
    caf_file_header.mFileVersion = 1;
    caf_file_header.mFileFlags = 0;
    WavpackNativeToBigEndian (&caf_file_header, CAFFileHeaderFormat);

    if (!DoWriteFile (outfile, &caf_file_header, sizeof (caf_file_header), &bcount) ||
        bcount != sizeof (caf_file_header))
            goto error_exit;

    // format and write the Audio Description Chunk

    memcpy (caf_desc_chunk_header.mChunkType, "desc", sizeof (caf_desc_chunk_header.mChunkType));
    caf_desc_chunk_header.mChunkSize = sizeof (caf_audio_format);
    WavpackNativeToBigEndian (&caf_desc_chunk_header, CAFChunkHeaderFormat);

    if (!DoWriteFile (outfile, &caf_desc_chunk_header, sizeof (caf_desc_chunk_header), &bcount) ||
        bcount != sizeof (caf_desc_chunk_header))
            goto error_exit;

    caf_audio_format.mSampleRate = (double) sample_rate;
    memcpy (caf_audio_format.mFormatID, "lpcm", sizeof (caf_audio_format.mFormatID));
    caf_audio_format.mFormatFlags = float_norm_exp ? CAF_FORMAT_FLOAT : 0;

    if (!(qmode & QMODE_BIG_ENDIAN))
        caf_audio_format.mFormatFlags |= CAF_FORMAT_LITTLE_ENDIAN;

    caf_audio_format.mBytesPerPacket = bytes_per_sample * num_channels;
    caf_audio_format.mFramesPerPacket = 1;
    caf_audio_format.mChannelsPerFrame = num_channels;
    caf_audio_format.mBitsPerChannel = bits_per_sample;
    WavpackNativeToBigEndian (&caf_audio_format, CAFAudioFormatFormat);

    if (!DoWriteFile (outfile, &caf_audio_format, sizeof (caf_audio_format), &bcount) ||
        bcount != sizeof (caf_audio_format))
            goto error_exit;

    // we write the Channel Layout Chunk if the shared Apple 'chan' chunk helper
    // determines that one is required (same rules as before: explicit layout tag,
    // any defined channels with > 2 channels, or non-standard mono/stereo)

    {
        uint32_t chan_body_size = AppleChannelLayoutBodySize (wpc, qmode);

        if (chan_body_size) {
            memcpy (caf_chan_chunk_header.mChunkType, "chan", sizeof (caf_chan_chunk_header.mChunkType));
            caf_chan_chunk_header.mChunkSize = chan_body_size;
            WavpackNativeToBigEndian (&caf_chan_chunk_header, CAFChunkHeaderFormat);

            if (!DoWriteFile (outfile, &caf_chan_chunk_header, sizeof (caf_chan_chunk_header), &bcount) ||
                bcount != sizeof (caf_chan_chunk_header))
                    goto error_exit;

            if (!WriteAppleChannelLayoutBody (outfile, wpc, qmode))
                goto error_exit;
        }
    }

    // format and write the Audio Data Chunk

    memcpy (caf_data_chunk_header.mChunkType, "data", sizeof (caf_data_chunk_header.mChunkType));

    if (total_samples == -1)
        caf_data_chunk_header.mChunkSize = -1;
    else
        caf_data_chunk_header.mChunkSize = (total_samples * bytes_per_sample * num_channels) + sizeof (mEditCount);

    WavpackNativeToBigEndian (&caf_data_chunk_header, CAFChunkHeaderFormat);

    if (!DoWriteFile (outfile, &caf_data_chunk_header, sizeof (caf_data_chunk_header), &bcount) ||
        bcount != sizeof (caf_data_chunk_header))
            goto error_exit;

    mEditCount = 0;
    WavpackNativeToBigEndian (&mEditCount, "L");

    if (!DoWriteFile (outfile, &mEditCount, sizeof (mEditCount), &bcount) ||
        bcount != sizeof (mEditCount))
            goto error_exit;

    return TRUE;

error_exit:
    return FALSE;
}
