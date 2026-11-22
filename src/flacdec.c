////////////////////////////////////////////////////////////////////////////
//                           **** WAVPACK ****                            //
//                  Hybrid Lossless Wavefile Compressor                   //
//                Copyright (c) 1998 - 2026 David Bryant.                 //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// flacdec.c

// Self-contained FLAC (Free Lossless Audio Codec) decoder. See flacdec.h
// for the interface description and the supported subset. This
// implementation makes no assumptions about encoder settings: any valid
// block size, partition order, subframe type (constant, verbatim, fixed,
// LPC), wasted-bits count, and stereo decorrelation mode is handled, and
// every frame is verified against its CRC-8 and CRC-16 checksums.

#include <stdlib.h>
#include <string.h>

#include "flacdec.h"

#define FLAC_MAX_CHANNELS   8
#define FLAC_MAX_BLOCKSIZE  65536
#define IO_BUFFER_SIZE      65536

typedef struct {
    char *name, *value;
} flac_tag;

typedef struct {
    uint32_t type;
    char *mime;
    unsigned char *data;
    uint32_t data_size;
} flac_picture;

struct FlacDecoder_s {
    FILE *infile;

    // STREAMINFO contents
    uint32_t sample_rate;
    int num_channels, bits_per_sample;
    uint64_t total_samples;         // valid only if has_total is set
    int has_total;
    unsigned char md5 [16];
    int has_md5;

    // metadata for tag mapping
    flac_tag *tags;
    int num_tags, alloc_tags;
    flac_picture *pictures;
    int num_pictures, alloc_pictures;

    // buffered input (sequential only, never seeks)
    unsigned char io_buf [IO_BUFFER_SIZE];
    size_t buf_pos, buf_fill;
    int at_eof;

    // bit reader (MSB first); bytes enter through read_byte() so that
    // the running CRC-8 / CRC-16 always cover exactly the frame bytes
    uint64_t bit_acc;
    int bit_cnt;
    uint8_t crc8;
    uint16_t crc16;

    // decoded frame data (one frame of history is kept for streaming reads)
    int32_t *chan_data [FLAC_MAX_CHANNELS];
    uint32_t frame_len, frame_pos;  // samples in current frame and delivery cursor

    uint64_t samples_decoded;
    int error;
    char error_msg [160];
};

////////////////////////////////////////////////////////////////////////////
// CRC-8 (poly 0x07) and CRC-16 (poly 0x8005), both MSB-first, init 0.   //
////////////////////////////////////////////////////////////////////////////

static uint8_t crc8_table [256];
static uint16_t crc16_table [256];
static int crc_tables_ready;

static void init_crc_tables (void)
{
    int i, j;

    if (crc_tables_ready)
        return;

    for (i = 0; i < 256; ++i) {
        uint32_t c8 = i, c16 = i << 8;

        for (j = 0; j < 8; ++j) {
            c8 = (c8 & 0x80) ? (c8 << 1) ^ 0x07 : c8 << 1;
            c16 = (c16 & 0x8000) ? (c16 << 1) ^ 0x8005 : c16 << 1;
        }

        crc8_table [i] = (uint8_t) c8;
        crc16_table [i] = (uint16_t) c16;
    }

    crc_tables_ready = 1;
}

////////////////////////////////////////////////////////////////////////////
// Error handling                                                        //
////////////////////////////////////////////////////////////////////////////

static int set_error (FlacDecoder *dec, const char *msg)
{
    if (!dec->error) {
        dec->error = 1;
        strncpy (dec->error_msg, msg, sizeof (dec->error_msg) - 1);
        dec->error_msg [sizeof (dec->error_msg) - 1] = 0;
    }

    return -1;
}

const char *FlacDecGetError (const FlacDecoder *dec)
{
    return dec ? dec->error_msg : "invalid decoder";
}

////////////////////////////////////////////////////////////////////////////
// Buffered byte input. Two flavors: with and without CRC accumulation.  //
////////////////////////////////////////////////////////////////////////////

static int read_byte_crc (FlacDecoder *dec)
{
    int b;

    if (dec->buf_pos >= dec->buf_fill) {
        size_t got = fread (dec->io_buf, 1, sizeof (dec->io_buf), dec->infile);

        if (!got) {
            dec->at_eof = 1;
            return -1;
        }

        dec->buf_pos = 0;
        dec->buf_fill = got;
    }

    b = dec->io_buf [dec->buf_pos++];
    dec->crc8 = crc8_table [dec->crc8 ^ b];
    dec->crc16 = (uint16_t)((dec->crc16 << 8) ^ crc16_table [((dec->crc16 >> 8) ^ b) & 0xff]);
    return b;
}

static int read_byte_raw (FlacDecoder *dec)
{
    if (dec->buf_pos >= dec->buf_fill) {
        size_t got = fread (dec->io_buf, 1, sizeof (dec->io_buf), dec->infile);

        if (!got) {
            dec->at_eof = 1;
            return -1;
        }

        dec->buf_pos = 0;
        dec->buf_fill = got;
    }

    return dec->io_buf [dec->buf_pos++];
}

// Read "n" bytes into "buffer" (metadata parsing; no CRC involved).
// Returns 0 on success, -1 on EOF.

static int read_bytes (FlacDecoder *dec, unsigned char *buffer, size_t n)
{
    while (n) {
        size_t avail = dec->buf_fill - dec->buf_pos;

        if (!avail) {
            size_t got = fread (dec->io_buf, 1, sizeof (dec->io_buf), dec->infile);

            if (!got) {
                dec->at_eof = 1;
                return -1;
            }

            dec->buf_pos = 0;
            dec->buf_fill = got;
            avail = got;
        }

        if (avail > n)
            avail = n;

        memcpy (buffer, dec->io_buf + dec->buf_pos, avail);
        dec->buf_pos += avail;
        buffer += avail;
        n -= avail;
    }

    return 0;
}

////////////////////////////////////////////////////////////////////////////
// Bit reader (MSB-first). Bits are pulled from read_byte_crc() so the   //
// frame checksums stay in sync.                                         //
////////////////////////////////////////////////////////////////////////////

static int fill_bits (FlacDecoder *dec, int n)
{
    while (dec->bit_cnt < n) {
        int b = read_byte_crc (dec);

        if (b < 0)
            return set_error (dec, "unexpected end of FLAC stream");

        dec->bit_acc = (dec->bit_acc << 8) | (unsigned char) b;
        dec->bit_cnt += 8;
    }

    return 0;
}

// Read n bits (0..32) as an unsigned value.

static uint32_t get_bits (FlacDecoder *dec, int n)
{
    uint32_t result;

    if (dec->error)
        return 0;

    if (!n)
        return 0;

    if (fill_bits (dec, n) < 0)
        return 0;

    dec->bit_cnt -= n;
    result = (uint32_t)(dec->bit_acc >> dec->bit_cnt);

    if (n < 32)
        result &= (1u << n) - 1;

    return result;
}

// Read n bits (1..32) as a sign-extended two's-complement value.

static int32_t get_signed_bits (FlacDecoder *dec, int n)
{
    uint32_t u = get_bits (dec, n);
    uint64_t sign = 1ULL << (n - 1);

    return (int32_t)((int64_t)(u ^ sign) - (int64_t) sign);
}

static int count_leading_zeros64 (uint64_t v)
{
#if defined(__GNUC__) || defined(__clang__)
    return v ? __builtin_clzll (v) : 64;
#else
    int n = 0;

    if (!v)
        return 64;

    if (!(v >> 32)) { n += 32; v <<= 32; }
    if (!(v >> 48)) { n += 16; v <<= 16; }
    if (!(v >> 56)) { n += 8;  v <<= 8;  }
    if (!(v >> 60)) { n += 4;  v <<= 4;  }
    if (!(v >> 62)) { n += 2;  v <<= 2;  }
    if (!(v >> 63)) { n += 1; }
    return n;
#endif
}

// Read a unary-coded value (number of 0 bits before the next 1 bit).

static uint32_t get_unary (FlacDecoder *dec)
{
    uint32_t count = 0;

    while (!dec->error) {
        uint64_t aligned;
        int lz;

        if (dec->bit_cnt == 0 && fill_bits (dec, 8) < 0)
            return 0;

        aligned = dec->bit_acc << (64 - dec->bit_cnt);

        if (!aligned) {
            count += dec->bit_cnt;
            dec->bit_cnt = 0;
            continue;
        }

        lz = count_leading_zeros64 (aligned);
        count += lz;
        dec->bit_cnt -= lz + 1;
        return count;
    }

    return 0;
}

////////////////////////////////////////////////////////////////////////////
// Metadata parsing                                                      //
////////////////////////////////////////////////////////////////////////////

static int add_tag (FlacDecoder *dec, const char *name, int name_len, const char *value, int value_len)
{
    flac_tag *new_tags;
    char *n, *v;

    if (dec->num_tags == dec->alloc_tags) {
        int new_alloc = dec->alloc_tags ? dec->alloc_tags * 2 : 16;

        if (!(new_tags = realloc (dec->tags, new_alloc * sizeof (flac_tag))))
            return set_error (dec, "out of memory parsing Vorbis comments");

        dec->tags = new_tags;
        dec->alloc_tags = new_alloc;
    }

    n = malloc (name_len + 1);
    v = malloc (value_len + 1);

    if (!n || !v) {
        free (n);
        free (v);
        return set_error (dec, "out of memory parsing Vorbis comments");
    }

    memcpy (n, name, name_len);
    n [name_len] = 0;
    memcpy (v, value, value_len);
    v [value_len] = 0;

    dec->tags [dec->num_tags].name = n;
    dec->tags [dec->num_tags].value = v;
    dec->num_tags++;
    return 0;
}

static int parse_vorbis_comment (FlacDecoder *dec, const unsigned char *buf, uint32_t len)
{
    uint32_t pos = 0, vendor_len, count, i;

    if (len < 8)
        return set_error (dec, "malformed Vorbis comment block");

    vendor_len = (uint32_t) buf [0] | (buf [1] << 8) | ((uint32_t) buf [2] << 16) | ((uint32_t) buf [3] << 24);

    if (vendor_len > len - 8)
        return set_error (dec, "malformed Vorbis comment block (vendor length)");

    pos = 4 + vendor_len;
    count = (uint32_t) buf [pos] | (buf [pos + 1] << 8) | ((uint32_t) buf [pos + 2] << 16) | ((uint32_t) buf [pos + 3] << 24);
    pos += 4;

    for (i = 0; i < count; ++i) {
        uint32_t clen, j;
        const char *eq;

        if (len - pos < 4)
            return set_error (dec, "malformed Vorbis comment block (truncated entry)");

        clen = (uint32_t) buf [pos] | (buf [pos + 1] << 8) | ((uint32_t) buf [pos + 2] << 16) | ((uint32_t) buf [pos + 3] << 24);
        pos += 4;

        if (clen > len - pos)
            return set_error (dec, "malformed Vorbis comment block (truncated entry)");

        // entries must be "NAME=VALUE"; silently skip malformed ones
        // rather than failing the whole conversion

        eq = NULL;

        for (j = 0; j < clen; ++j)
            if (buf [pos + j] == '=') {
                eq = (const char *) buf + pos + j;
                break;
            }

        if (eq && eq != (const char *) buf + pos) {
            const unsigned char *name = buf + pos;
            int name_len = (int)(eq - (const char *) name);
            const char *value = eq + 1;
            int value_len = (int)(clen - name_len - 1), legal = 1;

            // APEv2 item names must be printable ASCII; skip anything else

            for (j = 0; j < (uint32_t) name_len; ++j)
                if (name [j] < 0x20 || name [j] > 0x7e) {
                    legal = 0;
                    break;
                }

            if (legal && add_tag (dec, (const char *) name, name_len, value, value_len) < 0)
                return -1;
        }

        pos += clen;
    }

    return 0;
}

static int parse_picture (FlacDecoder *dec, const unsigned char *buf, uint32_t len)
{
    uint32_t pos = 0, mime_len, desc_len, data_len, type;
    flac_picture *pic, *new_pics;

    #define PIC_U32(p) (((uint32_t) buf [(p)] << 24) | ((uint32_t) buf [(p)+1] << 16) | \
        (buf [(p)+2] << 8) | buf [(p)+3])

    if (len < 32)
        return set_error (dec, "malformed picture block");

    type = PIC_U32 (0);
    pos = 4;

    mime_len = PIC_U32 (pos);
    pos += 4;

    if (mime_len > len - pos || len - pos - mime_len < 24)
        return set_error (dec, "malformed picture block (mime)");

    desc_len = PIC_U32 (pos + mime_len);

    if (desc_len > len - pos - mime_len - 4 || len - pos - mime_len - 4 - desc_len < 20)
        return set_error (dec, "malformed picture block (description)");

    data_len = PIC_U32 (pos + mime_len + 4 + desc_len + 16);

    if (data_len > len - pos - mime_len - 4 - desc_len - 20)
        return set_error (dec, "malformed picture block (data)");

    if (dec->num_pictures == dec->alloc_pictures) {
        int new_alloc = dec->alloc_pictures ? dec->alloc_pictures * 2 : 4;

        if (!(new_pics = realloc (dec->pictures, new_alloc * sizeof (flac_picture))))
            return set_error (dec, "out of memory parsing picture block");

        dec->pictures = new_pics;
        dec->alloc_pictures = new_alloc;
    }

    pic = dec->pictures + dec->num_pictures;
    pic->type = type;
    pic->mime = malloc (mime_len + 1);
    pic->data = malloc (data_len ? data_len : 1);

    if (!pic->mime || !pic->data) {
        free (pic->mime);
        free (pic->data);
        return set_error (dec, "out of memory parsing picture block");
    }

    memcpy (pic->mime, buf + pos, mime_len);
    pic->mime [mime_len] = 0;
    memcpy (pic->data, buf + pos + mime_len + 4 + desc_len + 20, data_len);
    pic->data_size = data_len;
    dec->num_pictures++;

    #undef PIC_U32
    return 0;
}

////////////////////////////////////////////////////////////////////////////
// FlacDecOpen: parse all metadata blocks (STREAMINFO must be first).    //
////////////////////////////////////////////////////////////////////////////

FlacDecoder *FlacDecOpen (FILE *infile, char *error, int error_size)
{
    FlacDecoder *dec;
    int last = 0, first = 1;

    if (error && error_size)
        error [0] = 0;

    init_crc_tables ();

    if (!(dec = calloc (1, sizeof (FlacDecoder)))) {
        if (error && error_size)
            strncpy (error, "out of memory", error_size - 1);

        return NULL;
    }

    dec->infile = infile;

    while (!last) {
        unsigned char hdr [4];
        uint32_t block_len;
        int block_type;

        if (read_bytes (dec, hdr, 4) < 0) {
            set_error (dec, "unexpected end of FLAC stream in metadata");
            goto fail;
        }

        last = hdr [0] & 0x80;
        block_type = hdr [0] & 0x7f;
        block_len = ((uint32_t) hdr [1] << 16) | (hdr [2] << 8) | hdr [3];

        if (block_type == 127) {
            set_error (dec, "invalid FLAC metadata block type");
            goto fail;
        }

        if (first && block_type != 0) {
            set_error (dec, "FLAC stream does not start with STREAMINFO");
            goto fail;
        }

        if (block_type == 0) {                  // STREAMINFO
            unsigned char si [34];
            uint32_t min_block, max_block;

            if (block_len < 34 || read_bytes (dec, si, 34) < 0) {
                set_error (dec, "malformed STREAMINFO block");
                goto fail;
            }

            min_block = (si [0] << 8) | si [1];
            max_block = (si [2] << 8) | si [3];
            dec->sample_rate = ((uint32_t) si [10] << 12) | (si [11] << 4) | (si [12] >> 4);
            dec->num_channels = ((si [12] >> 1) & 0x7) + 1;
            dec->bits_per_sample = (((si [12] & 0x1) << 4) | (si [13] >> 4)) + 1;
            dec->total_samples = ((uint64_t)(si [13] & 0xf) << 32) |
                ((uint32_t) si [14] << 24) | (si [15] << 16) | (si [16] << 8) | si [17];
            dec->has_total = dec->total_samples != 0;
            memcpy (dec->md5, si + 18, 16);

            {
                int i;

                for (i = 0; i < 16 && !dec->md5 [i]; ++i);

                dec->has_md5 = (i < 16);
            }

            if (!dec->sample_rate) {
                set_error (dec, "FLAC STREAMINFO has invalid (zero) sample rate");
                goto fail;
            }

            if (dec->bits_per_sample != 16 && dec->bits_per_sample != 24) {
                snprintf (dec->error_msg, sizeof (dec->error_msg),
                    "unsupported FLAC bit depth %d (only 16-bit and 24-bit are supported)",
                    dec->bits_per_sample);
                dec->error = 1;
                goto fail;
            }

            if (min_block < 16 || max_block > FLAC_MAX_BLOCKSIZE || min_block > max_block) {
                set_error (dec, "FLAC STREAMINFO has invalid block size limits");
                goto fail;
            }

            block_len -= 34;
        }

        if (block_len) {
            unsigned char *block_data = NULL;

            // only Vorbis comments and pictures are actually parsed; everything
            // else is skipped (but still must be read since we cannot seek)

            if (block_type == 4 || block_type == 6) {
                if (!(block_data = malloc (block_len))) {
                    set_error (dec, "out of memory parsing FLAC metadata");
                    goto fail;
                }

                if (read_bytes (dec, block_data, block_len) < 0) {
                    free (block_data);
                    set_error (dec, "unexpected end of FLAC stream in metadata");
                    goto fail;
                }

                if ((block_type == 4 && parse_vorbis_comment (dec, block_data, block_len) < 0) ||
                    (block_type == 6 && parse_picture (dec, block_data, block_len) < 0)) {
                        free (block_data);
                        goto fail;
                }

                free (block_data);
            }
            else {
                uint32_t remaining = block_len;
                unsigned char dummy [4096];

                while (remaining) {
                    uint32_t chunk = remaining > sizeof (dummy) ? sizeof (dummy) : remaining;

                    if (read_bytes (dec, dummy, chunk) < 0) {
                        set_error (dec, "unexpected end of FLAC stream in metadata");
                        goto fail;
                    }

                    remaining -= chunk;
                }
            }
        }

        first = 0;
    }

    // allocate the channel decode buffers (full maximum block size so that
    // any frame structure is accepted without reallocation)

    {
        int ch;

        for (ch = 0; ch < dec->num_channels; ++ch)
            if (!(dec->chan_data [ch] = malloc (FLAC_MAX_BLOCKSIZE * sizeof (int32_t)))) {
                set_error (dec, "out of memory");
                goto fail;
            }
    }

    return dec;

fail:
    if (error && error_size) {
        strncpy (error, dec->error_msg, error_size - 1);
        error [error_size - 1] = 0;
    }

    FlacDecClose (dec);
    return NULL;
}

////////////////////////////////////////////////////////////////////////////
// Accessors                                                             //
////////////////////////////////////////////////////////////////////////////

uint32_t FlacDecGetSampleRate (const FlacDecoder *dec) { return dec->sample_rate; }
int FlacDecGetNumChannels (const FlacDecoder *dec) { return dec->num_channels; }
int FlacDecGetBitsPerSample (const FlacDecoder *dec) { return dec->bits_per_sample; }
int64_t FlacDecGetTotalSamples (const FlacDecoder *dec) { return dec->has_total ? (int64_t) dec->total_samples : -1; }
uint64_t FlacDecGetSamplesDecoded (const FlacDecoder *dec) { return dec->samples_decoded; }
int FlacDecGetNumTagItems (const FlacDecoder *dec) { return dec->num_tags; }
int FlacDecGetNumPictures (const FlacDecoder *dec) { return dec->num_pictures; }

int FlacDecGetMD5 (const FlacDecoder *dec, unsigned char md5 [16])
{
    if (!dec->has_md5)
        return 0;

    memcpy (md5, dec->md5, 16);
    return 1;
}

const char *FlacDecGetTagItemName (const FlacDecoder *dec, int index)
{
    return (index >= 0 && index < dec->num_tags) ? dec->tags [index].name : NULL;
}

const char *FlacDecGetTagItemValue (const FlacDecoder *dec, int index)
{
    return (index >= 0 && index < dec->num_tags) ? dec->tags [index].value : NULL;
}

int FlacDecGetPicture (const FlacDecoder *dec, int index, uint32_t *type,
    const char **mime, const unsigned char **data, uint32_t *data_size)
{
    if (index < 0 || index >= dec->num_pictures)
        return 0;

    if (type) *type = dec->pictures [index].type;
    if (mime) *mime = dec->pictures [index].mime;
    if (data) *data = dec->pictures [index].data;
    if (data_size) *data_size = dec->pictures [index].data_size;
    return 1;
}

void FlacDecClose (FlacDecoder *dec)
{
    int i;

    if (!dec)
        return;

    for (i = 0; i < FLAC_MAX_CHANNELS; ++i)
        free (dec->chan_data [i]);

    for (i = 0; i < dec->num_tags; ++i) {
        free (dec->tags [i].name);
        free (dec->tags [i].value);
    }

    for (i = 0; i < dec->num_pictures; ++i) {
        free (dec->pictures [i].mime);
        free (dec->pictures [i].data);
    }

    free (dec->tags);
    free (dec->pictures);
    free (dec);
}

////////////////////////////////////////////////////////////////////////////
// Frame decoding                                                        //
////////////////////////////////////////////////////////////////////////////

// Decode the residual of a subframe into out [0..count-1].

static int decode_residual (FlacDecoder *dec, int32_t *out, uint32_t block_size, int pred_order)
{
    int method = get_bits (dec, 2);
    int param_bits, escape_code;
    int partition_order, partitions, p;

    if (dec->error)
        return -1;

    if (method > 1)
        return set_error (dec, "invalid residual coding method");

    param_bits = method ? 5 : 4;
    escape_code = method ? 0x1f : 0xf;

    partition_order = get_bits (dec, 4);
    partitions = 1 << partition_order;

    if (block_size % partitions)
        return set_error (dec, "invalid residual partition order for block size");

    if ((int)(block_size >> partition_order) < pred_order)
        return set_error (dec, "invalid residual partition order for predictor order");

    for (p = 0; p < partitions; ++p) {
        uint32_t count = (block_size >> partition_order) - (p ? 0 : pred_order);
        int param = get_bits (dec, param_bits);

        if (param == escape_code) {
            int raw_bits = get_bits (dec, 5);

            while (count--)
                *out++ = raw_bits ? get_signed_bits (dec, raw_bits) : 0;
        }
        else {
            while (count--) {
                uint32_t uval = (get_unary (dec) << param) | get_bits (dec, param);

                *out++ = (int32_t)(uval >> 1) ^ -(int32_t)(uval & 1);
            }
        }

        if (dec->error)
            return -1;
    }

    return 0;
}

// Decode one subframe into dec->chan_data [channel], which must have room
// for block_size samples. Returns 0 on success.

static int decode_subframe (FlacDecoder *dec, int32_t *data, uint32_t block_size, int bps)
{
    int type, wasted = 0;
    uint32_t i;

    if (get_bits (dec, 1))
        return set_error (dec, "invalid subframe header (padding bit set)");

    type = get_bits (dec, 6);

    if (get_bits (dec, 1)) {                    // wasted-bits flag
        wasted = (int) get_unary (dec) + 1;
        bps -= wasted;

        if (bps <= 0)
            return set_error (dec, "invalid wasted-bits count");
    }

    if (type == 0) {                            // constant
        int32_t value = get_signed_bits (dec, bps);

        for (i = 0; i < block_size; ++i)
            data [i] = value;
    }
    else if (type == 1) {                       // verbatim
        for (i = 0; i < block_size; ++i)
            data [i] = get_signed_bits (dec, bps);
    }
    else if (type >= 8 && type <= 12) {         // fixed predictor, order 0-4
        int order = type & 0x7;

        for (i = 0; i < (uint32_t) order; ++i)
            data [i] = get_signed_bits (dec, bps);

        if (decode_residual (dec, data + order, block_size, order) < 0)
            return -1;

        {
            uint32_t *ud = (uint32_t *) data;

            switch (order) {
                case 0:
                    break;
                case 1:
                    for (i = 1; i < block_size; ++i)
                        ud [i] += ud [i-1];
                    break;
                case 2:
                    for (i = 2; i < block_size; ++i)
                        ud [i] += 2 * ud [i-1] - ud [i-2];
                    break;
                case 3:
                    for (i = 3; i < block_size; ++i)
                        ud [i] += 3 * ud [i-1] - 3 * ud [i-2] + ud [i-3];
                    break;
                case 4:
                    for (i = 4; i < block_size; ++i)
                        ud [i] += 4 * ud [i-1] - 6 * ud [i-2] + 4 * ud [i-3] - ud [i-4];
                    break;
            }
        }
    }
    else if (type >= 32) {                      // LPC, order 1-32
        int order = (type & 0x1f) + 1;
        int precision, shift, j;
        int32_t coefs [32];

        for (i = 0; i < (uint32_t) order; ++i)
            data [i] = get_signed_bits (dec, bps);

        precision = (int) get_bits (dec, 4) + 1;

        if (precision == 16)
            return set_error (dec, "invalid LPC precision");

        shift = (int) get_signed_bits (dec, 5);

        for (j = 0; j < order; ++j)
            coefs [j] = get_signed_bits (dec, precision);

        if (decode_residual (dec, data + order, block_size, order) < 0)
            return -1;

        {
            uint32_t *ud = (uint32_t *) data;

            for (i = order; i < block_size; ++i) {
                int64_t sum = 0;
                int32_t predicted;

                for (j = 0; j < order; ++j)
                    sum += (int64_t) coefs [j] * (int32_t) ud [i - 1 - j];

                if (shift >= 0)
                    predicted = (int32_t)(sum >> shift);
                else
                    predicted = (int32_t)(sum << -shift);

                ud [i] += (uint32_t) predicted;
            }
        }
    }
    else
        return set_error (dec, "reserved subframe type");

    if (dec->error)
        return -1;

    if (wasted) {
        uint32_t *ud = (uint32_t *) data;

        for (i = 0; i < block_size; ++i)
            ud [i] <<= wasted;
    }

    return 0;
}

// Parse a "coded number" (UTF-8 style, up to 7 bytes / 36 bits). The value
// itself (frame or sample number) is not needed for sequential decoding,
// but it must be consumed and validated.

static int parse_coded_number (FlacDecoder *dec)
{
    int b = (int) get_bits (dec, 8);
    int extra, i;

    if (dec->error)
        return -1;

    if (!(b & 0x80))
        extra = 0;
    else if ((b & 0xe0) == 0xc0)
        extra = 1;
    else if ((b & 0xf0) == 0xe0)
        extra = 2;
    else if ((b & 0xf8) == 0xf0)
        extra = 3;
    else if ((b & 0xfc) == 0xf8)
        extra = 4;
    else if ((b & 0xfe) == 0xfc)
        extra = 5;
    else if (b == 0xfe)
        extra = 6;
    else
        return set_error (dec, "invalid coded number in frame header");

    for (i = 0; i < extra; ++i)
        if ((get_bits (dec, 8) & 0xc0) != 0x80)
            return set_error (dec, "invalid coded number in frame header");

    return dec->error ? -1 : 0;
}

// Decode the next frame into the channel buffers. Returns 1 if a frame was
// decoded, 0 on clean end-of-stream, -1 on error.

static int decode_frame (FlacDecoder *dec)
{
    static const uint32_t rate_table [16] = {
        0, 88200, 176400, 192000, 8000, 16000, 22050, 24000,
        32000, 44100, 48000, 96000, 0, 0, 0, 0
    };
    static const int bps_table [8] = { 0, 8, 12, -1, 16, 20, 24, 32 };

    uint32_t block_size, frame_rate;
    int block_code, rate_code, assignment, size_code, frame_bps;
    int num_channels, ch, stored_crc8, stored_crc16;

    // check for clean end-of-stream at a frame boundary

    if (dec->buf_pos >= dec->buf_fill) {
        size_t got = fread (dec->io_buf, 1, sizeof (dec->io_buf), dec->infile);

        if (!got) {
            dec->at_eof = 1;
            return 0;
        }

        dec->buf_pos = 0;
        dec->buf_fill = got;
    }

    dec->crc8 = 0;
    dec->crc16 = 0;
    dec->bit_acc = 0;
    dec->bit_cnt = 0;

    if (get_bits (dec, 14) != 0x3ffe)
        return set_error (dec, "lost sync in FLAC stream (invalid frame header)");

    if (get_bits (dec, 1))
        return set_error (dec, "invalid frame header (reserved bit set)");

    get_bits (dec, 1);                          // blocking strategy (not needed)

    block_code = (int) get_bits (dec, 4);
    rate_code = (int) get_bits (dec, 4);
    assignment = (int) get_bits (dec, 4);
    size_code = (int) get_bits (dec, 3);

    if (get_bits (dec, 1))
        return set_error (dec, "invalid frame header (reserved bit set)");

    if (!block_code || assignment > 10 || size_code == 3 || rate_code == 15)
        return set_error (dec, "invalid frame header");

    if (parse_coded_number (dec) < 0)
        return -1;

    switch (block_code) {
        case 1:  block_size = 192; break;
        case 2:  block_size = 576; break;
        case 3:  block_size = 1152; break;
        case 4:  block_size = 2304; break;
        case 5:  block_size = 4608; break;
        case 6:  block_size = (uint32_t) get_bits (dec, 8) + 1; break;
        case 7:  block_size = (uint32_t) get_bits (dec, 16) + 1; break;
        default: block_size = 256u << (block_code - 8); break;
    }

    if (rate_code < 12)
        frame_rate = rate_table [rate_code];
    else if (rate_code == 12)
        frame_rate = get_bits (dec, 8) * 1000;
    else if (rate_code == 13)
        frame_rate = get_bits (dec, 16);
    else
        frame_rate = get_bits (dec, 16) * 10;

    frame_bps = size_code ? bps_table [size_code] : dec->bits_per_sample;

    // the CRC-8 byte itself is covered by the frame CRC-16, so it must be
    // read through the CRC-accumulating path (snapshot the CRC-8 first)

    {
        uint8_t computed_crc8 = dec->crc8;

        stored_crc8 = read_byte_crc (dec);

        if (stored_crc8 < 0)
            return set_error (dec, "unexpected end of FLAC stream in frame header");

        if (stored_crc8 != computed_crc8)
            return set_error (dec, "FLAC frame header CRC-8 mismatch");
    }

    if (dec->bit_cnt)
        return set_error (dec, "internal error: frame header not byte aligned");

    // the frame must be consistent with STREAMINFO

    if (frame_rate != dec->sample_rate)
        return set_error (dec, "frame sample rate does not match STREAMINFO");

    if (frame_bps != dec->bits_per_sample)
        return set_error (dec, "frame bit depth does not match STREAMINFO");

    num_channels = (assignment >= 8) ? 2 : assignment + 1;

    if (num_channels != dec->num_channels)
        return set_error (dec, "frame channel count does not match STREAMINFO");

    if (block_size > FLAC_MAX_BLOCKSIZE)
        return set_error (dec, "frame block size exceeds maximum");

    // decode the subframes; the "side" channel of a decorrelated stereo
    // pair carries one extra bit

    for (ch = 0; ch < num_channels; ++ch) {
        int sub_bps = dec->bits_per_sample;

        if ((assignment == 8 && ch == 1) ||      // left/side: side is ch 1
            (assignment == 9 && ch == 0) ||      // side/right: side is ch 0
            (assignment == 10 && ch == 1))       // mid/side: side is ch 1
            sub_bps++;

        if (decode_subframe (dec, dec->chan_data [ch], block_size, sub_bps) < 0)
            return -1;
    }

    // byte-align, then verify the frame CRC-16

    dec->bit_cnt -= (dec->bit_cnt & 7);

    if (dec->bit_cnt)
        return set_error (dec, "internal error: frame not byte aligned");

    stored_crc16 = read_byte_raw (dec);

    if (stored_crc16 >= 0)
        stored_crc16 = (stored_crc16 << 8) | read_byte_raw (dec);

    if (stored_crc16 < 0)
        return set_error (dec, "unexpected end of FLAC stream in frame footer");

    if (stored_crc16 != dec->crc16)
        return set_error (dec, "FLAC frame CRC-16 mismatch (corrupt audio data)");

    // undo any stereo decorrelation

    if (assignment >= 8) {
        uint32_t *ch0 = (uint32_t *) dec->chan_data [0];
        uint32_t *ch1 = (uint32_t *) dec->chan_data [1];
        uint32_t i;

        if (assignment == 8)                    // left/side
            for (i = 0; i < block_size; ++i)
                ch1 [i] = ch0 [i] - ch1 [i];
        else if (assignment == 9)               // side/right
            for (i = 0; i < block_size; ++i)
                ch0 [i] = ch0 [i] + ch1 [i];
        else                                    // mid/side
            for (i = 0; i < block_size; ++i) {
                int64_t mid = (int32_t) ch0 [i];
                int64_t side = (int32_t) ch1 [i];

                mid = (mid << 1) | (side & 1);
                ch0 [i] = (uint32_t)(int32_t)((mid + side) >> 1);
                ch1 [i] = (uint32_t)(int32_t)((mid - side) >> 1);
            }
    }

    dec->frame_len = block_size;
    dec->frame_pos = 0;
    return 1;
}

////////////////////////////////////////////////////////////////////////////
// FlacDecRead: deliver interleaved little-endian PCM.                   //
////////////////////////////////////////////////////////////////////////////

int64_t FlacDecRead (FlacDecoder *dec, unsigned char *buffer, int64_t max_samples)
{
    int container_bytes = (dec->bits_per_sample + 7) / 8;
    int64_t delivered = 0;

    if (dec->error)
        return -1;

    while (delivered < max_samples) {
        if (dec->frame_pos >= dec->frame_len) {
            int res = decode_frame (dec);

            if (res <= 0) {
                if (delivered > 0)
                    break;                      // deliver partial results; EOF/error reported on next call

                return res;
            }
        }

        {
            uint32_t count = dec->frame_len - dec->frame_pos;
            int ch = dec->num_channels;
            uint32_t i;
            int c;

            if ((int64_t) count > max_samples - delivered)
                count = (uint32_t)(max_samples - delivered);

            if (container_bytes == 2) {
                for (i = 0; i < count; ++i)
                    for (c = 0; c < ch; ++c) {
                        uint32_t v = (uint32_t) dec->chan_data [c][dec->frame_pos + i];

                        *buffer++ = (unsigned char) v;
                        *buffer++ = (unsigned char)(v >> 8);
                    }
            }
            else {
                for (i = 0; i < count; ++i)
                    for (c = 0; c < ch; ++c) {
                        uint32_t v = (uint32_t) dec->chan_data [c][dec->frame_pos + i];

                        *buffer++ = (unsigned char) v;
                        *buffer++ = (unsigned char)(v >> 8);
                        *buffer++ = (unsigned char)(v >> 16);
                    }
            }

            dec->frame_pos += count;
            delivered += count;
        }
    }

    dec->samples_decoded += delivered;
    return delivered;
}
