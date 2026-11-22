////////////////////////////////////////////////////////////////////////////
//                           **** WAVPACK ****                            //
//                  Hybrid Lossless Wavefile Compressor                   //
//                Copyright (c) 1998 - 2026 David Bryant.                 //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// flac.c

// This module is a helper to the WavPack command-line compressor to
// support FLAC (Free Lossless Audio Codec) input files. The actual
// decoding is performed by the self-contained decoder in the library
// (src/flacdec.c); this file handles the WavPack configuration, the
// mandatory verification of the FLAC audio MD5 and sample count, and
// the mapping of Vorbis comments and embedded pictures to APEv2 tags.

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "wavpack.h"
#include "flacdec.h"
#include "utils.h"

#if defined (_WIN32)
#define stricmp(x,y) _stricmp(x,y)
#else
#include <strings.h>
#define stricmp strcasecmp
#endif

extern int debug_logging_mode;

void FlacCliClose (void);

// The active decoder for the file currently being packed (the CLI is
// strictly single-file-at-a-time, so a static is safe here).

static FlacDecoder *flac_dec;
static int flac_read_error;
static char flac_read_error_msg [160];

// FLAC channel order is defined such that it always matches the Microsoft
// (and WavPack) speaker order for the standard layouts, so no reordering
// is ever required; we simply supply the correct channel mask.

static const int flac_channel_masks [9] = {
    0,              // (unused)
    0x4,            // 1: FC
    0x3,            // 2: FL FR
    0x7,            // 3: FL FR FC
    0x33,           // 4: FL FR BL BR
    0x37,           // 5: FL FR FC BL BR
    0x3f,           // 6: FL FR FC LFE BL BR
    0x70f,          // 7: FL FR FC LFE BC SL SR
    0x63f           // 8: FL FR FC LFE BL BR SL SR
};

// mapping of common Vorbis comment field names to conventional APEv2 item
// names; anything not listed here is passed through unchanged

static const struct {
    char *vorbis, *ape;
} tag_xlate [] = {
    { "TITLE",          "Title" },
    { "ARTIST",         "Artist" },
    { "ALBUM",          "Album" },
    { "ALBUMARTIST",    "AlbumArtist" },
    { "COMPOSER",       "Composer" },
    { "CONDUCTOR",      "Conductor" },
    { "DATE",           "Year" },
    { "TRACKNUMBER",    "Track" },
    { "DISCNUMBER",     "Disc" },
    { "GENRE",          "Genre" },
    { "COMMENT",        "Comment" },
    { "PERFORMER",      "Performer" },
    { "ORGANIZATION",   "Publisher" },
    { "PUBLISHER",      "Publisher" },
    { "COPYRIGHT",      "Copyright" },
    { "ENCODER",        "Encoder" },
    { "ISRC",           "ISRC" },
    { "LYRICS",         "Lyrics" },
    { "BPM",            "BPM" },
    { "MOOD",           "Mood" }
};

#define NUM_TAG_XLATE (sizeof (tag_xlate) / sizeof (tag_xlate [0]))

// picture type names, identical to the ID3 APIC types used in import_id3.c

static const char *picture_types [] = {
    "Other", "Png Icon", "Icon", "Front", "Back", "Leaflet", "Media",
    "Lead Artist", "Artist", "Conductor", "Band", "Composer", "Lyricist",
    "Recording Location", "During Recording", "During Performance", "Video Capture",
    "Phish", "Illustration", "Band Logotype", "Publisher Logotype" };

#define NUM_PICTURE_TYPES (sizeof (picture_types) / sizeof (picture_types [0]))

int ParseFlacHeaderConfig (FILE *infile, char *infilename, char *fourcc, WavpackContext *wpc, WavpackConfig *config)
{
    char error [160] = "";
    int64_t total_samples;

    FlacCliClose ();            // just in case a previous file errored out
    flac_read_error = 0;

    flac_dec = FlacDecOpen (infile, error, sizeof (error));

    if (!flac_dec) {
        error_line ("%s: %s", infilename, error [0] ? error : "not a valid FLAC file");
        return WAVPACK_SOFT_ERROR;
    }

    config->num_channels = FlacDecGetNumChannels (flac_dec);
    config->bits_per_sample = FlacDecGetBitsPerSample (flac_dec);
    config->bytes_per_sample = (config->bits_per_sample + 7) / 8;
    config->sample_rate = (int32_t) FlacDecGetSampleRate (flac_dec);

    if (!config->channel_mask && !(config->qmode & QMODE_CHANS_UNASSIGNED))
        config->channel_mask = flac_channel_masks [config->num_channels];

    // we always verify the FLAC audio MD5, so make sure one is computed
    // (and stored) even if the user did not specify -m

    config->flags |= CONFIG_MD5_CHECKSUM;

    // a FLAC transcode never stores a "wrapper" (we cannot reconstruct the
    // original FLAC file on decode, and trailing junk must not be stored)

    config->qmode |= QMODE_NO_STORE_WRAPPER;

    total_samples = FlacDecGetTotalSamples (flac_dec);

    if (total_samples < 0)
        config->qmode |= QMODE_IGNORE_LENGTH;

    if (debug_logging_mode)
        error_line ("FLAC: %d channel(s), %d bits, %d Hz, %lld samples",
            config->num_channels, config->bits_per_sample, config->sample_rate,
            (long long) total_samples);

    if (!WavpackSetConfiguration64 (wpc, config, total_samples, NULL)) {
        error_line ("%s: %s", infilename, WavpackGetErrorMessage (wpc));
        return WAVPACK_SOFT_ERROR;
    }

    return WAVPACK_NO_ERROR;
}

// Drop-in replacement for DoReadFile() used by pack_audio() when the source
// is a FLAC file. Decodes audio into the caller's buffer as little-endian
// PCM. Errors are latched so that FlacCliVerify() can report them after the
// pack loop (which does not check read results) terminates.

int FlacCliRead (FILE *infile, void *buffer, uint32_t bytes, uint32_t *bytes_read)
{
    int frame_bytes = FlacDecGetNumChannels (flac_dec) * ((FlacDecGetBitsPerSample (flac_dec) + 7) / 8);
    int64_t samples;

    (void) infile;      // the decoder has its own handle on the stream

    if (flac_read_error) {
        *bytes_read = 0;
        return 1;
    }

    samples = FlacDecRead (flac_dec, buffer, bytes / frame_bytes);

    if (samples < 0) {
        flac_read_error = 1;
        strncpy (flac_read_error_msg, FlacDecGetError (flac_dec), sizeof (flac_read_error_msg) - 1);
        flac_read_error_msg [sizeof (flac_read_error_msg) - 1] = 0;
        samples = 0;
    }

    *bytes_read = (uint32_t) samples * frame_bytes;
    return 1;
}

// Called after the pack loop completes. The md5_digest parameter is the MD5
// that pack_audio() computed over the decoded PCM (which is exactly the byte
// stream the FLAC MD5 covers). Any discrepancy -- a decode error, a short
// sample count, or an MD5 mismatch -- is a deterministic hard error.

int FlacCliVerify (const unsigned char *md5_digest)
{
    unsigned char flac_md5 [16];
    int64_t total_samples;

    if (!flac_dec)
        return WAVPACK_NO_ERROR;

    if (flac_read_error) {
        error_line ("FLAC decode error: %s", flac_read_error_msg);
        return WAVPACK_HARD_ERROR;
    }

    total_samples = FlacDecGetTotalSamples (flac_dec);

    if (total_samples >= 0 && FlacDecGetSamplesDecoded (flac_dec) != (uint64_t) total_samples) {
        error_line ("FLAC stream contained %llu samples, but STREAMINFO declares %llu!",
            (unsigned long long) FlacDecGetSamplesDecoded (flac_dec),
            (unsigned long long) total_samples);
        return WAVPACK_HARD_ERROR;
    }

    if (FlacDecGetMD5 (flac_dec, flac_md5) && memcmp (md5_digest, flac_md5, 16)) {
        error_line ("MD5 of decoded audio does not match FLAC STREAMINFO MD5!");
        return WAVPACK_HARD_ERROR;
    }

    return WAVPACK_NO_ERROR;
}

// Map the FLAC Vorbis comments and embedded pictures into APEv2 tag items
// on the (nearly complete) WavPack file. Returns the number of items
// written, or -1 on error.

int FlacCliImportTags (WavpackContext *wpc)
{
    int i, items = 0;

    if (!flac_dec)
        return 0;

    for (i = 0; i < FlacDecGetNumTagItems (flac_dec); ++i) {
        const char *name = FlacDecGetTagItemName (flac_dec, i);
        const char *value = FlacDecGetTagItemValue (flac_dec, i);
        const char *ape_name = name;
        size_t j;

        if (!name || !value || !strlen (value))
            continue;

        for (j = 0; j < NUM_TAG_XLATE; ++j)
            if (!stricmp (name, tag_xlate [j].vorbis)) {
                ape_name = tag_xlate [j].ape;
                break;
            }

        if (!WavpackAppendTagItem (wpc, ape_name, value, (int) strlen (value)))
            return -1;

        items++;
    }

    for (i = 0; i < FlacDecGetNumPictures (flac_dec); ++i) {
        const unsigned char *data;
        uint32_t type, data_size;
        const char *mime, *ext = "";
        char item [80];

        if (!FlacDecGetPicture (flac_dec, i, &type, &mime, &data, &data_size) || !data_size)
            continue;

        if (type < NUM_PICTURE_TYPES)
            snprintf (item, sizeof (item), "Cover Art (%s)", picture_types [type]);
        else
            strcpy (item, "Cover Art (Other)");

        if (mime && (strstr (mime, "jpeg") || strstr (mime, "JPEG")))
            ext = ".jpg";
        else if (mime && (strstr (mime, "png") || strstr (mime, "PNG")))
            ext = ".png";
        else if (data_size >= 2 && data [0] == 0xff && data [1] == 0xd8)
            ext = ".jpg";
        else if (data_size >= 2 && data [0] == 0x89 && data [1] == 0x50)
            ext = ".png";

        {
            int binary_size = (int) strlen (item) + (int) strlen (ext) + 1 + (int) data_size;
            char *binary_value = malloc (binary_size);
            int res;

            if (!binary_value)
                return -1;

            strcpy (binary_value, item);
            strcat (binary_value, ext);
            memcpy (binary_value + binary_size - data_size, data, data_size);

            res = WavpackAppendBinaryTagItem (wpc, item, binary_value, binary_size);
            free (binary_value);

            if (!res)
                return -1;

            items++;
        }
    }

    return items;
}

// Free the decoder for the current file (if any). Safe to call anytime.

void FlacCliClose (void)
{
    if (flac_dec) {
        FlacDecClose (flac_dec);
        flac_dec = NULL;
    }

    flac_read_error = 0;
}
