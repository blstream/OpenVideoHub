/*
 * PCM codecs
 * Copyright (c) 2001 Fabrice Bellard.
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file pcm.c
 * PCM codecs
 */

#include "avcodec.h"
#include "bitstream.h" // for ff_reverse
#include "bytestream.h"

#define MAX_CHANNELS 64

/* from g711.c by SUN microsystems (unrestricted use) */

#define         SIGN_BIT        (0x80)      /* Sign bit for a A-law byte. */
#define         QUANT_MASK      (0xf)       /* Quantization field mask. */
#define         NSEGS           (8)         /* Number of A-law segments. */
#define         SEG_SHIFT       (4)         /* Left shift for segment number. */
#define         SEG_MASK        (0x70)      /* Segment field mask. */

#define         BIAS            (0x84)      /* Bias for linear code. */

/*
 * alaw2linear() - Convert an A-law value to 16-bit linear PCM
 *
 */
static av_cold int alaw2linear(unsigned char a_val)
{
        int t;
        int seg;

        a_val ^= 0x55;

        t = a_val & QUANT_MASK;
        seg = ((unsigned)a_val & SEG_MASK) >> SEG_SHIFT;
        if(seg) t= (t + t + 1 + 32) << (seg + 2);
        else    t= (t + t + 1     ) << 3;

        return (a_val & SIGN_BIT) ? t : -t;
}

static av_cold int ulaw2linear(unsigned char u_val)
{
        int t;

        /* Complement to obtain normal u-law value. */
        u_val = ~u_val;

        /*
         * Extract and bias the quantization bits. Then
         * shift up by the segment number and subtract out the bias.
         */
        t = ((u_val & QUANT_MASK) << 3) + BIAS;
        t <<= ((unsigned)u_val & SEG_MASK) >> SEG_SHIFT;

        return (u_val & SIGN_BIT) ? (BIAS - t) : (t - BIAS);
}

/* 16384 entries per table */
static uint8_t linear_to_alaw[16384];
static uint8_t linear_to_ulaw[16384];

static av_cold void build_xlaw_table(uint8_t *linear_to_xlaw,
                             int (*xlaw2linear)(unsigned char),
                             int mask)
{
    int i, j, v, v1, v2;

    j = 0;
    for(i=0;i<128;i++) {
        if (i != 127) {
            v1 = xlaw2linear(i ^ mask);
            v2 = xlaw2linear((i + 1) ^ mask);
            v = (v1 + v2 + 4) >> 3;
        } else {
            v = 8192;
        }
        for(;j<v;j++) {
            linear_to_xlaw[8192 + j] = (i ^ mask);
            if (j > 0)
                linear_to_xlaw[8192 - j] = (i ^ (mask ^ 0x80));
        }
    }
    linear_to_xlaw[0] = linear_to_xlaw[1];
}

static av_cold int pcm_encode_init(AVCodecContext *avctx)
{
    avctx->frame_size = 1;
    switch(avctx->codec->id) {
    case CODEC_ID_PCM_ALAW:
        build_xlaw_table(linear_to_alaw, alaw2linear, 0xd5);
        break;
    case CODEC_ID_PCM_MULAW:
        build_xlaw_table(linear_to_ulaw, ulaw2linear, 0xff);
        break;
    default:
        break;
    }

    avctx->block_align = avctx->channels * av_get_bits_per_sample(avctx->codec->id)/8;
    avctx->coded_frame= avcodec_alloc_frame();
    avctx->coded_frame->key_frame= 1;

    return 0;
}

static av_cold int pcm_encode_close(AVCodecContext *avctx)
{
    av_freep(&avctx->coded_frame);

    return 0;
}

/**
 * Write PCM samples macro
 * @param type Datatype of native machine format
 * @param endian bytestream_put_xxx() suffix
 * @param src Source pointer (variable name)
 * @param dst Destination pointer (variable name)
 * @param n Total number of samples (variable name)
 * @param shift Bitshift (bits)
 * @param offset Sample value offset
 */
#define ENCODE(type, endian, src, dst, n, shift, offset) \
    samples_##type = (type*)src; \
    for(;n>0;n--) { \
        register type v = (*samples_##type++ >> shift) + offset; \
        bytestream_put_##endian(&dst, v); \
    }

static int pcm_encode_frame(AVCodecContext *avctx,
                            unsigned char *frame, int buf_size, void *data)
{
    int n, sample_size, v;
    short *samples;
    unsigned char *dst;
    uint8_t *srcu8;
    int16_t *samples_int16_t;
    int32_t *samples_int32_t;
    int64_t *samples_int64_t;
    uint16_t *samples_uint16_t;
    uint32_t *samples_uint32_t;

    sample_size = av_get_bits_per_sample(avctx->codec->id)/8;
    n = buf_size / sample_size;
    samples = data;
    dst = frame;

    if (avctx->sample_fmt!=avctx->codec->sample_fmts[0]) {
        av_log(avctx, AV_LOG_ERROR, "invalid sample_fmt\n");
        return -1;
    }

    switch(avctx->codec->id) {
    case CODEC_ID_PCM_U32LE:
        ENCODE(uint32_t, le32, samples, dst, n, 0, 0x80000000)
        break;
    case CODEC_ID_PCM_U32BE:
        ENCODE(uint32_t, be32, samples, dst, n, 0, 0x80000000)
        break;
    case CODEC_ID_PCM_S24LE:
        ENCODE(int32_t, le24, samples, dst, n, 8, 0)
        break;
    case CODEC_ID_PCM_S24BE:
        ENCODE(int32_t, be24, samples, dst, n, 8, 0)
        break;
    case CODEC_ID_PCM_U24LE:
        ENCODE(uint32_t, le24, samples, dst, n, 8, 0x800000)
        break;
    case CODEC_ID_PCM_U24BE:
        ENCODE(uint32_t, be24, samples, dst, n, 8, 0x800000)
        break;
    case CODEC_ID_PCM_S24DAUD:
        for(;n>0;n--) {
            uint32_t tmp = ff_reverse[(*samples >> 8) & 0xff] +
                           (ff_reverse[*samples & 0xff] << 8);
            tmp <<= 4; // sync flags would go here
            bytestream_put_be24(&dst, tmp);
            samples++;
        }
        break;
    case CODEC_ID_PCM_U16LE:
        ENCODE(uint16_t, le16, samples, dst, n, 0, 0x8000)
        break;
    case CODEC_ID_PCM_U16BE:
        ENCODE(uint16_t, be16, samples, dst, n, 0, 0x8000)
        break;
    case CODEC_ID_PCM_S8:
        srcu8= data;
        for(;n>0;n--) {
            v = *srcu8++;
            *dst++ = v - 128;
        }
        break;
#if WORDS_BIGENDIAN
    case CODEC_ID_PCM_F64LE:
        ENCODE(int64_t, le64, samples, dst, n, 0, 0)
        break;
    case CODEC_ID_PCM_S32LE:
    case CODEC_ID_PCM_F32LE:
        ENCODE(int32_t, le32, samples, dst, n, 0, 0)
        break;
    case CODEC_ID_PCM_S16LE:
        ENCODE(int16_t, le16, samples, dst, n, 0, 0)
        break;
    case CODEC_ID_PCM_F64BE:
    case CODEC_ID_PCM_F32BE:
    case CODEC_ID_PCM_S32BE:
    case CODEC_ID_PCM_S16BE:
#else
    case CODEC_ID_PCM_F64BE:
        ENCODE(int64_t, be64, samples, dst, n, 0, 0)
        break;
    case CODEC_ID_PCM_F32BE:
    case CODEC_ID_PCM_S32BE:
        ENCODE(int32_t, be32, samples, dst, n, 0, 0)
        break;
    case CODEC_ID_PCM_S16BE:
        ENCODE(int16_t, be16, samples, dst, n, 0, 0)
        break;
    case CODEC_ID_PCM_F64LE:
    case CODEC_ID_PCM_F32LE:
    case CODEC_ID_PCM_S32LE:
    case CODEC_ID_PCM_S16LE:
#endif /* WORDS_BIGENDIAN */
    case CODEC_ID_PCM_U8:
        memcpy(dst, samples, n*sample_size);
        dst += n*sample_size;
        break;
    case CODEC_ID_PCM_ZORK:
        for(;n>0;n--) {
            v= *samples++ >> 8;
            if(v<0)   v = -v;
            else      v+= 128;
            *dst++ = v;
        }
        break;
    case CODEC_ID_PCM_ALAW:
        for(;n>0;n--) {
            v = *samples++;
            *dst++ = linear_to_alaw[(v + 32768) >> 2];
        }
        break;
    case CODEC_ID_PCM_MULAW:
        for(;n>0;n--) {
            v = *samples++;
            *dst++ = linear_to_ulaw[(v + 32768) >> 2];
        }
        break;
    default:
        return -1;
    }
    //avctx->frame_size = (dst - frame) / (sample_size * avctx->channels);

    return dst - frame;
}

typedef struct PCMDecode {
    short table[256];
} PCMDecode;

static av_cold int pcm_decode_init(AVCodecContext * avctx)
{
    PCMDecode *s = avctx->priv_data;
    int i;

    switch(avctx->codec->id) {
    case CODEC_ID_PCM_ALAW:
        for(i=0;i<256;i++)
            s->table[i] = alaw2linear(i);
        break;
    case CODEC_ID_PCM_MULAW:
        for(i=0;i<256;i++)
            s->table[i] = ulaw2linear(i);
        break;
    default:
        break;
    }

    avctx->sample_fmt = avctx->codec->sample_fmts[0];
    return 0;
}

/**
 * Read PCM samples macro
 * @param type Datatype of native machine format
 * @param endian bytestream_get_xxx() endian suffix
 * @param src Source pointer (variable name)
 * @param dst Destination pointer (variable name)
 * @param n Total number of samples (variable name)
 * @param shift Bitshift (bits)
 * @param offset Sample value offset
 */
#define DECODE(type, endian, src, dst, n, shift, offset) \
    dst_##type = (type*)dst; \
    for(;n>0;n--) { \
        register type v = bytestream_get_##endian(&src); \
        *dst_##type++ = (v - offset) << shift; \
    } \
    dst = (short*)dst_##type;

static int pcm_decode_frame(AVCodecContext *avctx,
                            void *data, int *data_size,
                            const uint8_t *buf, int buf_size)
{
    PCMDecode *s = avctx->priv_data;
    int sample_size, c, n;
    short *samples;
    const uint8_t *src, *src8, *src2[MAX_CHANNELS];
    uint8_t *dstu8;
    int16_t *dst_int16_t;
    int32_t *dst_int32_t;
    int64_t *dst_int64_t;
    uint16_t *dst_uint16_t;
    uint32_t *dst_uint32_t;

    samples = data;
    src = buf;

    if (avctx->sample_fmt!=avctx->codec->sample_fmts[0]) {
        av_log(avctx, AV_LOG_ERROR, "invalid sample_fmt\n");
        return -1;
    }

    if(avctx->channels <= 0 || avctx->channels > MAX_CHANNELS){
        av_log(avctx, AV_LOG_ERROR, "PCM channels out of bounds\n");
        return -1;
    }

    sample_size = av_get_bits_per_sample(avctx->codec_id)/8;

    /* av_get_bits_per_sample returns 0 for CODEC_ID_PCM_DVD */
    if (CODEC_ID_PCM_DVD == avctx->codec_id)
        /* 2 samples are interleaved per block in PCM_DVD */
        sample_size = avctx->bits_per_sample * 2 / 8;

    n = avctx->channels * sample_size;

    if(n && buf_size % n){
        av_log(avctx, AV_LOG_ERROR, "invalid PCM packet\n");
        return -1;
    }

    buf_size= FFMIN(buf_size, *data_size/2);
    *data_size=0;

    n = buf_size/sample_size;

    switch(avctx->codec->id) {
    case CODEC_ID_PCM_U32LE:
        DECODE(uint32_t, le32, src, samples, n, 0, 0x80000000)
        break;
    case CODEC_ID_PCM_U32BE:
        DECODE(uint32_t, be32, src, samples, n, 0, 0x80000000)
        break;
    case CODEC_ID_PCM_S24LE:
        DECODE(int32_t, le24, src, samples, n, 8, 0)
        break;
    case CODEC_ID_PCM_S24BE:
        DECODE(int32_t, be24, src, samples, n, 8, 0)
        break;
    case CODEC_ID_PCM_U24LE:
        DECODE(uint32_t, le24, src, samples, n, 8, 0x800000)
        break;
    case CODEC_ID_PCM_U24BE:
        DECODE(uint32_t, be24, src, samples, n, 8, 0x800000)
        break;
    case CODEC_ID_PCM_S24DAUD:
        for(;n>0;n--) {
          uint32_t v = bytestream_get_be24(&src);
          v >>= 4; // sync flags are here
          *samples++ = ff_reverse[(v >> 8) & 0xff] +
                       (ff_reverse[v & 0xff] << 8);
        }
        break;
    case CODEC_ID_PCM_S16LE_PLANAR:
        n /= avctx->channels;
        for(c=0;c<avctx->channels;c++)
            src2[c] = &src[c*n*2];
        for(;n>0;n--)
            for(c=0;c<avctx->channels;c++)
                *samples++ = bytestream_get_le16(&src2[c]);
        src = src2[avctx->channels-1];
        break;
    case CODEC_ID_PCM_S16BE:
        n = buf_size >> 1;
        for(;n>0;n--) {
            *samples++ = bytestream_get_be16(&src);
        }
        break;
    case CODEC_ID_PCM_U16LE:
        n = buf_size >> 1;
        for(;n>0;n--) {
            *samples++ = bytestream_get_le16(&src) - 0x8000;
        }
        break;
    case CODEC_ID_PCM_U16BE:
        n = buf_size >> 1;
        for(;n>0;n--) {
            *samples++ = bytestream_get_be16(&src) - 0x8000;
        }
        break;
    case CODEC_ID_PCM_S8:
        n = buf_size;
        for(;n>0;n--) {
            *samples++ = *src++ << 8;
        }
        break;
    case CODEC_ID_PCM_U8:
        n = buf_size;
        for(;n>0;n--) {
            *samples++ = ((int)*src++ - 128) << 8;
        }
        break;
    case CODEC_ID_PCM_ZORK:
        n = buf_size;
        for(;n>0;n--) {
            int x= *src++;
            if(x&128) x-= 128;
            else      x = -x;
            *samples++ = x << 8;
        }
        break;
    case CODEC_ID_PCM_ALAW:
    case CODEC_ID_PCM_MULAW:
        n = buf_size;
        for(;n>0;n--) {
            *samples++ = s->table[*src++];
        }
        break;
    case CODEC_ID_PCM_DVD:
        if(avctx->bits_per_sample != 20 && avctx->bits_per_sample != 24) {
            av_log(avctx, AV_LOG_ERROR, "PCM DVD unsupported sample depth\n");
            return -1;
        } else {
            int jump = avctx->channels * (avctx->bits_per_sample-16) / 4;
            n = buf_size / (avctx->channels * 2 * avctx->bits_per_sample / 8);
            while (n--) {
                for (c=0; c < 2*avctx->channels; c++)
                    *samples++ = bytestream_get_be16(&src);
                src += jump;
            }
        }
        break;
    default:
        return -1;
    }
    *data_size = (uint8_t *)samples - (uint8_t *)data;
    return src - buf;
}

#ifdef CONFIG_ENCODERS
#define PCM_ENCODER(id,name,long_name_)         \
AVCodec name ## _encoder = {                    \
    #name,                                      \
    CODEC_TYPE_AUDIO,                           \
    id,                                         \
    0,                                          \
    pcm_encode_init,                            \
    pcm_encode_frame,                           \
    pcm_encode_close,                           \
    NULL,                                       \
    .long_name = NULL_IF_CONFIG_SMALL(long_name_), \
};
#else
#define PCM_ENCODER(id,name,long_name_)
#endif

#ifdef CONFIG_DECODERS
#define PCM_DECODER(id,name,long_name_)         \
AVCodec name ## _decoder = {                    \
    #name,                                      \
    CODEC_TYPE_AUDIO,                           \
    id,                                         \
    sizeof(PCMDecode),                          \
    pcm_decode_init,                            \
    NULL,                                       \
    NULL,                                       \
    pcm_decode_frame,                           \
    .long_name = NULL_IF_CONFIG_SMALL(long_name_), \
};
#else
#define PCM_DECODER(id,name,long_name_)
#endif

#define PCM_CODEC(id, name, long_name_)         \
    PCM_ENCODER(id,name,long_name_) PCM_DECODER(id,name,long_name_)

PCM_CODEC  (CODEC_ID_PCM_ALAW, pcm_alaw, "A-law PCM");
PCM_CODEC  (CODEC_ID_PCM_DVD, pcm_dvd, "signed 16|20|24-bit big-endian PCM");
PCM_CODEC  (CODEC_ID_PCM_MULAW, pcm_mulaw, "mu-law PCM");
PCM_CODEC  (CODEC_ID_PCM_S8, pcm_s8, "signed 8-bit PCM");
PCM_CODEC  (CODEC_ID_PCM_S16BE, pcm_s16be, "signed 16-bit big-endian PCM");
PCM_CODEC  (CODEC_ID_PCM_S16LE, pcm_s16le, "signed 16-bit little-endian PCM");
PCM_DECODER(CODEC_ID_PCM_S16LE_PLANAR, pcm_s16le_planar, "16-bit little-endian planar PCM");
PCM_CODEC  (CODEC_ID_PCM_S24BE, pcm_s24be, "signed 24-bit big-endian PCM");
PCM_CODEC  (CODEC_ID_PCM_S24DAUD, pcm_s24daud, "D-Cinema audio signed 24-bit PCM");
PCM_CODEC  (CODEC_ID_PCM_S24LE, pcm_s24le, "signed 24-bit little-endian PCM");
PCM_CODEC  (CODEC_ID_PCM_S32BE, pcm_s32be, "signed 32-bit big-endian PCM");
PCM_CODEC  (CODEC_ID_PCM_S32LE, pcm_s32le, "signed 32-bit little-endian PCM");
PCM_CODEC  (CODEC_ID_PCM_U8, pcm_u8, "unsigned 8-bit PCM");
PCM_CODEC  (CODEC_ID_PCM_U16BE, pcm_u16be, "unsigned 16-bit big-endian PCM");
PCM_CODEC  (CODEC_ID_PCM_U16LE, pcm_u16le, "unsigned 16-bit little-endian PCM");
PCM_CODEC  (CODEC_ID_PCM_U24BE, pcm_u24be, "unsigned 24-bit big-endian PCM");
PCM_CODEC  (CODEC_ID_PCM_U24LE, pcm_u24le, "unsigned 24-bit little-endian PCM");
PCM_CODEC  (CODEC_ID_PCM_U32BE, pcm_u32be, "unsigned 32-bit big-endian PCM");
PCM_CODEC  (CODEC_ID_PCM_U32LE, pcm_u32le, "unsigned 32-bit little-endian PCM");
PCM_CODEC  (CODEC_ID_PCM_ZORK, pcm_zork, "Zork PCM");
