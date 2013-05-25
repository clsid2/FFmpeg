/*
 * Fraps FPS1 decoder
 * Copyright (c) 2005 Roine Gustafsson
 * Copyright (c) 2006 Konstantin Shishkov
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
 * @file
 * Lossless Fraps 'FPS1' decoder
 * @author Roine Gustafsson (roine at users sf net)
 * @author Konstantin Shishkov
 *
 * Codec algorithm for version 0 is taken from Transcode <www.transcoding.org>
 *
 * Version 2 files support by Konstantin Shishkov
 */

#include "avcodec.h"
#include "get_bits.h"
#include "huffman.h"
#include "bytestream.h"
#include "bswapdsp.h"
#include "internal.h"
#include "thread.h"

#define FPS_TAG MKTAG('F', 'P', 'S', 'x')
#define VLC_BITS 11

/**
 * local variable storage
 */
typedef struct FrapsContext {
    AVCodecContext *avctx;
    BswapDSPContext bdsp;
    int cur_index, prev_index;
    int next_cur_index, next_prev_index;
    ThreadFrame frames[2];
    uint8_t *tmpbuf;
    int tmpbuf_size;
} FrapsContext;

static av_cold int decode_end(AVCodecContext *avctx);

/**
 * initializes decoder
 * @param avctx codec context
 * @return 0 on success or negative if fails
 */
static av_cold int decode_init(AVCodecContext *avctx)
{
    FrapsContext * const s = avctx->priv_data;
    int i;

    avctx->internal->allocate_progress = 1;

    s->prev_index = 0;
    s->cur_index = 1;

    s->avctx  = avctx;
    s->tmpbuf = NULL;

    ff_bswapdsp_init(&s->bdsp);

    for (i = 0; i < FF_ARRAY_ELEMS(s->frames); i++) {
        s->frames[i].f = av_frame_alloc();
        if (!s->frames[i].f) {
            decode_end(avctx);
            return AVERROR(ENOMEM);
        }
    }

    return 0;
}

static int update_thread_context(AVCodecContext *avctx, const AVCodecContext *avctx_from)
{
    FrapsContext *dst = avctx->priv_data, *src = avctx_from->priv_data;
    int i, ret;

    if (avctx == avctx_from) return 0;

    dst->cur_index  = src->next_cur_index;
    dst->prev_index = src->next_prev_index;

    for (i = 0; i < FF_ARRAY_ELEMS(dst->frames); i++) {
        ff_thread_release_buffer(avctx, &dst->frames[i]);
        if (src->frames[i].f->data[0]) {
            ret = ff_thread_ref_frame(&dst->frames[i], &src->frames[i]);
            if (ret < 0)
                return ret;
        }
    }

    return 0;
}

/**
 * Comparator - our nodes should ascend by count
 * but with preserved symbol order
 */
static int huff_cmp(const void *va, const void *vb)
{
    const Node *a = va, *b = vb;
    return (a->count - b->count)*256 + a->sym - b->sym;
}

/**
 * decode Fraps v2 packed plane
 */
static int fraps2_decode_plane(FrapsContext *s, uint8_t *dst, int stride, int w,
                               int h, const uint8_t *src, int size, int Uoff,
                               const int step)
{
    int i, j, ret;
    GetBitContext gb;
    VLC vlc;
    Node nodes[512];

    for (i = 0; i < 256; i++)
        nodes[i].count = bytestream_get_le32(&src);
    size -= 1024;
    if ((ret = ff_huff_build_tree(s->avctx, &vlc, 256, VLC_BITS,
                                  nodes, huff_cmp,
                                  FF_HUFFMAN_FLAG_ZERO_COUNT)) < 0)
        return ret;
    /* we have built Huffman table and are ready to decode plane */

    /* convert bits so they may be used by standard bitreader */
    s->bdsp.bswap_buf((uint32_t *) s->tmpbuf,
                      (const uint32_t *) src, size >> 2);

    if ((ret = init_get_bits8(&gb, s->tmpbuf, size)) < 0)
        return ret;

    for (j = 0; j < h; j++) {
        for (i = 0; i < w*step; i += step) {
            dst[i] = get_vlc2(&gb, vlc.table, VLC_BITS, 3);
            /* lines are stored as deltas between previous lines
             * and we need to add 0x80 to the first lines of chroma planes
             */
            if (j)
                dst[i] += dst[i - stride];
            else if (Uoff)
                dst[i] += 0x80;
            if (get_bits_left(&gb) < 0) {
                ff_free_vlc(&vlc);
                return AVERROR_INVALIDDATA;
            }
        }
        dst += stride;
    }
    ff_free_vlc(&vlc);
    return 0;
}

static void frame_copy(FrapsContext *s,
                       uint8_t *dst_data[3], const int dst_linesizes[3],
                       uint8_t *src_data[3], const int src_linesizes[3],
                       unsigned int version, int width, int height)
{
    int i, k, h, bwidth;
    uint8_t *src, *dst;
    int planes = (version & 1) ? 1 : 3;

    for (i = 0; i < planes; i++) {
        dst = dst_data[i];
        src = src_data[i];
        if (version & 1) {
            /* RGB data */
            h = height;
            bwidth = width * 3;
        } else {
            /* YUV 4:2:0 data */
            h = i ? height >> 1 : height;
            bwidth = i ? width >> 1 : width;
        }

        ff_thread_await_progress(&s->frames[s->prev_index], i, 0);
        for (k = 0; k < h; k++) {
            memcpy(dst, src, bwidth);
            dst += dst_linesizes[i];
            src += src_linesizes[i];
        }
        ff_thread_report_progress(&s->frames[s->cur_index], i, 0);
    }
}

static int decode_frame(AVCodecContext *avctx,
                        void *data, int *got_frame,
                        AVPacket *avpkt)
{
    FrapsContext * const s = avctx->priv_data;
    const uint8_t *buf     = avpkt->data;
    int buf_size           = avpkt->size;
    ThreadFrame *frame, *prev_frame;
    AVFrame *f;
    uint32_t header;
    unsigned int version,header_size;
    unsigned int x, y;
    const uint32_t *buf32;
    uint32_t *luma1,*luma2,*cb,*cr;
    uint32_t offs[4];
    int i, j, ret, is_chroma, is_Pframe;
    const int planes = 3;
    int is_pal;
    uint8_t *out;

    if (buf_size < 4) {
        av_log(avctx, AV_LOG_ERROR, "Packet is too short\n");
        return AVERROR_INVALIDDATA;
    }

    frame = &s->frames[s->cur_index];
    prev_frame = &s->frames[s->prev_index];
    f = frame->f;

    header      = AV_RL32(buf);
    version     = header & 0xff;
    is_pal      = buf[1] == 2 && version == 1;
    header_size = (header & (1<<30))? 8 : 4; /* bit 30 means pad to 8 bytes */

    if (version > 5) {
        avpriv_report_missing_feature(avctx, "Fraps version %u", version);
        return AVERROR_PATCHWELCOME;
    }

    buf += header_size;

    if (is_pal) {
        unsigned needed_size = avctx->width * avctx->height + 1024;
        needed_size += header_size;
        if (buf_size != needed_size) {
            av_log(avctx, AV_LOG_ERROR,
                   "Invalid frame length %d (should be %d)\n",
                   buf_size, needed_size);
            return AVERROR_INVALIDDATA;
        }
    } else if (version < 2) {
        unsigned needed_size = avctx->width * avctx->height * 3;
        if (version == 0) needed_size /= 2;
        needed_size += header_size;
        /* bit 31 means same as previous pic */
        is_Pframe = (header & (1U<<31)) ? 1 : 0;
        if (!is_Pframe && buf_size != needed_size) {
            av_log(avctx, AV_LOG_ERROR,
                   "Invalid frame length %d (should be %d)\n",
                   buf_size, needed_size);
            return AVERROR_INVALIDDATA;
        }
    } else {
        is_Pframe = buf_size == 8 ? 1 : 0;
        if (!is_Pframe) {
        if (AV_RL32(buf) != FPS_TAG || buf_size < planes*1024 + 24) {
            av_log(avctx, AV_LOG_ERROR, "error in data stream\n");
            return AVERROR_INVALIDDATA;
        }
        for (i = 0; i < planes; i++) {
            offs[i] = AV_RL32(buf + 4 + i * 4);
            if (offs[i] >= buf_size - header_size || (i && offs[i] <= offs[i - 1] + 1024)) {
                av_log(avctx, AV_LOG_ERROR, "plane %i offset is out of bounds\n", i);
                return AVERROR_INVALIDDATA;
            }
        }
        offs[planes] = buf_size - header_size;
        for (i = 0; i < planes; i++) {
            av_fast_padded_malloc(&s->tmpbuf, &s->tmpbuf_size, offs[i + 1] - offs[i] - 1024);
            if (!s->tmpbuf)
                return AVERROR(ENOMEM);
        }
        }
    }

    if (is_Pframe && !prev_frame->f->data[0]) {
        av_log(avctx, AV_LOG_ERROR, "decoding must start with keyframe\n");
        return AVERROR_INVALIDDATA;
    }

    ff_thread_release_buffer(avctx, frame);

    f->pict_type = is_Pframe ? AV_PICTURE_TYPE_P : AV_PICTURE_TYPE_I;
    f->key_frame = is_Pframe ? 0 : 1;

    avctx->pix_fmt = version & 1 ? is_pal ? AV_PIX_FMT_PAL8 : AV_PIX_FMT_BGR24 : AV_PIX_FMT_YUVJ420P;
    avctx->color_range = version & 1 ? AVCOL_RANGE_UNSPECIFIED
                                     : AVCOL_RANGE_JPEG;
    avctx->colorspace = version & 1 ? AVCOL_SPC_UNSPECIFIED : AVCOL_SPC_BT709;

    if ((ret = ff_thread_get_buffer(avctx, frame, AV_GET_BUFFER_FLAG_REF)) < 0)
        return ret;

    s->next_prev_index = s->cur_index;
    s->next_cur_index  = (s->cur_index - 1) & 1;

    ff_thread_finish_setup(avctx);

    /* Copy previous frame */
    if (is_Pframe) {
        frame_copy(s,
                   frame->f->data,
                   frame->f->linesize,
                   prev_frame->f->data,
                   prev_frame->f->linesize,
                   version, avctx->width, avctx->height);
        goto end;
    }

    switch (version) {
    case 0:
    default:
        /* Fraps v0 is a reordered YUV420 */
        if (((avctx->width % 8) != 0) || ((avctx->height % 2) != 0)) {
            av_log(avctx, AV_LOG_ERROR, "Invalid frame size %dx%d\n",
                   avctx->width, avctx->height);
            return AVERROR_INVALIDDATA;
        }

        buf32 = (const uint32_t*)buf;
        for (y = 0; y < avctx->height / 2; y++) {
            luma1 = (uint32_t*)&f->data[0][  y * 2      * f->linesize[0] ];
            luma2 = (uint32_t*)&f->data[0][ (y * 2 + 1) * f->linesize[0] ];
            cr    = (uint32_t*)&f->data[1][  y          * f->linesize[1] ];
            cb    = (uint32_t*)&f->data[2][  y          * f->linesize[2] ];
            for (x = 0; x < avctx->width; x += 8) {
                *luma1++ = *buf32++;
                *luma1++ = *buf32++;
                *luma2++ = *buf32++;
                *luma2++ = *buf32++;
                *cr++    = *buf32++;
                *cb++    = *buf32++;
            }
        }
        ff_thread_report_progress(frame, INT_MAX, 0);
        break;

    case 1:
        if (is_pal) {
            uint32_t *pal = (uint32_t *)f->data[1];

            for (y = 0; y < 256; y++) {
                pal[y] = AV_RL32(buf) | 0xFF000000;
                buf += 4;
            }

            for (y = 0; y <avctx->height; y++)
                memcpy(&f->data[0][y * f->linesize[0]],
                       &buf[y * avctx->width],
                       avctx->width);
        } else {
        /* Fraps v1 is an upside-down BGR24 */
            for (y = 0; y<avctx->height; y++)
                memcpy(&f->data[0][(avctx->height - y - 1) * f->linesize[0]],
                       &buf[y * avctx->width * 3],
                       3 * avctx->width);
        }
        ff_thread_report_progress(frame, INT_MAX, 0);
        break;

    case 2:
    case 4:
        /**
         * Fraps v2 is Huffman-coded YUV420 planes
         * Fraps v4 is virtually the same
         */
        for (i = 0; i < planes; i++) {
            is_chroma = !!i;
            if ((ret = fraps2_decode_plane(s, f->data[i], f->linesize[i],
                                           avctx->width  >> is_chroma,
                                           avctx->height >> is_chroma,
                                           buf + offs[i], offs[i + 1] - offs[i],
                                           is_chroma, 1)) < 0) {
                av_log(avctx, AV_LOG_ERROR, "Error decoding plane %i\n", i);
                if (avctx->active_thread_type & FF_THREAD_FRAME) {
                    ff_thread_report_progress(frame, INT_MAX, 0);
                    break;
                } else
                    return ret;
            } else
                ff_thread_report_progress(frame, i, 0);
        }
        break;
    case 3:
    case 5:
        /* Virtually the same as version 4, but is for RGB24 */
        for (i = 0; i < planes; i++) {
            if ((ret = fraps2_decode_plane(s, f->data[0] + i + (f->linesize[0] * (avctx->height - 1)),
                                           -f->linesize[0], avctx->width, avctx->height,
                                           buf + offs[i], offs[i + 1] - offs[i], 0, 3)) < 0) {
                av_log(avctx, AV_LOG_ERROR, "Error decoding plane %i\n", i);
                if (avctx->active_thread_type & FF_THREAD_FRAME)
                    break;
                else
                    return ret;
            }
        }
        out = f->data[0];
        // convert pseudo-YUV into real RGB
        for (j = 0; j < avctx->height; j++) {
            uint8_t *line_end = out + 3*avctx->width;
            while (out < line_end) {
                out[0]  += out[1];
                out[2]  += out[1];
                out += 3;
            }
            out += f->linesize[0] - 3*avctx->width;
        }
        ff_thread_report_progress(frame, INT_MAX, 0);
        break;
    }

end:
    if ((ret = av_frame_ref(data, frame->f)) < 0)
        return ret;
    *got_frame = 1;

    s->prev_index = s->next_prev_index;
    s->cur_index  = s->next_cur_index;

    /* Only release frames that aren't used anymore */
    ff_thread_release_buffer(avctx, &s->frames[s->cur_index]);

    return buf_size;
}

static av_cold int init_thread_copy(AVCodecContext *avctx)
{
    FrapsContext *s = avctx->priv_data;
    int i;

    for (i = 0; i < FF_ARRAY_ELEMS(s->frames); i++) {
        s->frames[i].f = av_frame_alloc();
        if (!s->frames[i].f) {
            decode_end(avctx);
            return AVERROR(ENOMEM);
        }
    }

    return 0;
}

/**
 * closes decoder
 * @param avctx codec context
 * @return 0 on success or negative if fails
 */
static av_cold int decode_end(AVCodecContext *avctx)
{
    FrapsContext *s = (FrapsContext*)avctx->priv_data;
    int i;

    av_freep(&s->tmpbuf);

    for (i = 0; i < FF_ARRAY_ELEMS(s->frames); i++) {
        if (s->frames[i].f)
            ff_thread_release_buffer(avctx, &s->frames[i]);
        av_frame_free(&s->frames[i].f);
    }

    return 0;
}


AVCodec ff_fraps_decoder = {
    .name           = "fraps",
    .long_name      = NULL_IF_CONFIG_SMALL("Fraps"),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_FRAPS,
    .priv_data_size = sizeof(FrapsContext),
    .init           = decode_init,
    .close          = decode_end,
    .decode         = decode_frame,
    .capabilities   = AV_CODEC_CAP_DR1 | AV_CODEC_CAP_FRAME_THREADS,
    .caps_internal  = FF_CODEC_CAP_INIT_THREADSAFE,
    .update_thread_context = ONLY_IF_THREADS_ENABLED(update_thread_context),
    .init_thread_copy      = ONLY_IF_THREADS_ENABLED(init_thread_copy),
};
