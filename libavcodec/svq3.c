/*
 * Copyright (c) 2003 The FFmpeg Project
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

/*
 * How to use this decoder:
 * SVQ3 data is transported within Apple Quicktime files. Quicktime files
 * have stsd atoms to describe media trak properties. A stsd atom for a
 * video trak contains 1 or more ImageDescription atoms. These atoms begin
 * with the 4-byte length of the atom followed by the codec fourcc. Some
 * decoders need information in this atom to operate correctly. Such
 * is the case with SVQ3. In order to get the best use out of this decoder,
 * the calling app must make the SVQ3 ImageDescription atom available
 * via the AVCodecContext's extradata[_size] field:
 *
 * AVCodecContext.extradata = pointer to ImageDescription, first characters
 * are expected to be 'S', 'V', 'Q', and '3', NOT the 4-byte atom length
 * AVCodecContext.extradata_size = size of ImageDescription atom memory
 * buffer (which will be the same as the ImageDescription atom size field
 * from the QT file, minus 4 bytes since the length is missing)
 *
 * You will know you have these parameters passed correctly when the decoder
 * correctly decodes this file:
 *  http://samples.mplayerhq.hu/V-codecs/SVQ3/Vertical400kbit.sorenson3.mov
 */

#include <inttypes.h>

#include "libavutil/attributes.h"
#include "internal.h"
#include "avcodec.h"
#include "mpegutils.h"
#include "h264.h"
#include "h264_mvpred.h"
#include "h264data.h"
#include "golomb.h"
#include "hpeldsp.h"
#include "mathops.h"
#include "rectangle.h"
#include "tpeldsp.h"
#include "vdpau_internal.h"

#if CONFIG_ZLIB
#include <zlib.h>
#endif

#include "svq1.h"
#include "svq3.h"

/**
 * @file
 * svq3 decoder.
 */

typedef struct SVQ3Context {
    H264Context h;
    HpelDSPContext hdsp;
    TpelDSPContext tdsp;
    H264Picture *cur_pic;
    H264Picture *next_pic;
    H264Picture *last_pic;
    GetBitContext gb;
    uint8_t *slice_buf;
    int slice_size;
    int halfpel_flag;
    int thirdpel_flag;
    int has_watermark;
    uint32_t watermark_key;
    uint8_t *buf;
    int buf_size;
    int adaptive_quant;
    int next_p_frame_damaged;
    int h_edge_pos;
    int v_edge_pos;
    int last_frame_output;
} SVQ3Context;

#define FULLPEL_MODE  1
#define HALFPEL_MODE  2
#define THIRDPEL_MODE 3
#define PREDICT_MODE  4

/* dual scan (from some older h264 draft)
 * o-->o-->o   o
 *         |  /|
 * o   o   o / o
 * | / |   |/  |
 * o   o   o   o
 *   /
 * o-->o-->o-->o
 */
static const uint8_t svq3_scan[16] = {
    0 + 0 * 4, 1 + 0 * 4, 2 + 0 * 4, 2 + 1 * 4,
    2 + 2 * 4, 3 + 0 * 4, 3 + 1 * 4, 3 + 2 * 4,
    0 + 1 * 4, 0 + 2 * 4, 1 + 1 * 4, 1 + 2 * 4,
    0 + 3 * 4, 1 + 3 * 4, 2 + 3 * 4, 3 + 3 * 4,
};

static const uint8_t luma_dc_zigzag_scan[16] = {
    0 * 16 + 0 * 64, 1 * 16 + 0 * 64, 2 * 16 + 0 * 64, 0 * 16 + 2 * 64,
    3 * 16 + 0 * 64, 0 * 16 + 1 * 64, 1 * 16 + 1 * 64, 2 * 16 + 1 * 64,
    1 * 16 + 2 * 64, 2 * 16 + 2 * 64, 3 * 16 + 2 * 64, 0 * 16 + 3 * 64,
    3 * 16 + 1 * 64, 1 * 16 + 3 * 64, 2 * 16 + 3 * 64, 3 * 16 + 3 * 64,
};

static const uint8_t svq3_pred_0[25][2] = {
    { 0, 0 },
    { 1, 0 }, { 0, 1 },
    { 0, 2 }, { 1, 1 }, { 2, 0 },
    { 3, 0 }, { 2, 1 }, { 1, 2 }, { 0, 3 },
    { 0, 4 }, { 1, 3 }, { 2, 2 }, { 3, 1 }, { 4, 0 },
    { 4, 1 }, { 3, 2 }, { 2, 3 }, { 1, 4 },
    { 2, 4 }, { 3, 3 }, { 4, 2 },
    { 4, 3 }, { 3, 4 },
    { 4, 4 }
};

static const int8_t svq3_pred_1[6][6][5] = {
    { { 2, -1, -1, -1, -1 }, { 2, 1, -1, -1, -1 }, { 1, 2, -1, -1, -1 },
      { 2,  1, -1, -1, -1 }, { 1, 2, -1, -1, -1 }, { 1, 2, -1, -1, -1 } },
    { { 0,  2, -1, -1, -1 }, { 0, 2,  1,  4,  3 }, { 0, 1,  2,  4,  3 },
      { 0,  2,  1,  4,  3 }, { 2, 0,  1,  3,  4 }, { 0, 4,  2,  1,  3 } },
    { { 2,  0, -1, -1, -1 }, { 2, 1,  0,  4,  3 }, { 1, 2,  4,  0,  3 },
      { 2,  1,  0,  4,  3 }, { 2, 1,  4,  3,  0 }, { 1, 2,  4,  0,  3 } },
    { { 2,  0, -1, -1, -1 }, { 2, 0,  1,  4,  3 }, { 1, 2,  0,  4,  3 },
      { 2,  1,  0,  4,  3 }, { 2, 1,  3,  4,  0 }, { 2, 4,  1,  0,  3 } },
    { { 0,  2, -1, -1, -1 }, { 0, 2,  1,  3,  4 }, { 1, 2,  3,  0,  4 },
      { 2,  0,  1,  3,  4 }, { 2, 1,  3,  0,  4 }, { 2, 0,  4,  3,  1 } },
    { { 0,  2, -1, -1, -1 }, { 0, 2,  4,  1,  3 }, { 1, 4,  2,  0,  3 },
      { 4,  2,  0,  1,  3 }, { 2, 0,  1,  4,  3 }, { 4, 2,  1,  0,  3 } },
};

static const struct {
    uint8_t run;
    uint8_t level;
} svq3_dct_tables[2][16] = {
    { { 0, 0 }, { 0, 1 }, { 1, 1 }, { 2, 1 }, { 0, 2 }, { 3, 1 }, { 4, 1 }, { 5, 1 },
      { 0, 3 }, { 1, 2 }, { 2, 2 }, { 6, 1 }, { 7, 1 }, { 8, 1 }, { 9, 1 }, { 0, 4 } },
    { { 0, 0 }, { 0, 1 }, { 1, 1 }, { 0, 2 }, { 2, 1 }, { 0, 3 }, { 0, 4 }, { 0, 5 },
      { 3, 1 }, { 4, 1 }, { 1, 2 }, { 1, 3 }, { 0, 6 }, { 0, 7 }, { 0, 8 }, { 0, 9 } }
};

static const uint32_t svq3_dequant_coeff[32] = {
     3881,  4351,  4890,  5481,   6154,   6914,   7761,   8718,
     9781, 10987, 12339, 13828,  15523,  17435,  19561,  21873,
    24552, 27656, 30847, 34870,  38807,  43747,  49103,  54683,
    61694, 68745, 77615, 89113, 100253, 109366, 126635, 141533
};

static int svq3_decode_end(AVCodecContext *avctx);

void ff_svq3_luma_dc_dequant_idct_c(int16_t *output, int16_t *input, int qp)
{
    const int qmul = svq3_dequant_coeff[qp];
#define stride 16
    int i;
    int temp[16];
    static const uint8_t x_offset[4] = { 0, 1 * stride, 4 * stride, 5 * stride };

    for (i = 0; i < 4; i++) {
        const int z0 = 13 * (input[4 * i + 0] +      input[4 * i + 2]);
        const int z1 = 13 * (input[4 * i + 0] -      input[4 * i + 2]);
        const int z2 =  7 *  input[4 * i + 1] - 17 * input[4 * i + 3];
        const int z3 = 17 *  input[4 * i + 1] +  7 * input[4 * i + 3];

        temp[4 * i + 0] = z0 + z3;
        temp[4 * i + 1] = z1 + z2;
        temp[4 * i + 2] = z1 - z2;
        temp[4 * i + 3] = z0 - z3;
    }

    for (i = 0; i < 4; i++) {
        const int offset = x_offset[i];
        const int z0     = 13 * (temp[4 * 0 + i] +      temp[4 * 2 + i]);
        const int z1     = 13 * (temp[4 * 0 + i] -      temp[4 * 2 + i]);
        const int z2     =  7 *  temp[4 * 1 + i] - 17 * temp[4 * 3 + i];
        const int z3     = 17 *  temp[4 * 1 + i] +  7 * temp[4 * 3 + i];

        output[stride *  0 + offset] = (z0 + z3) * qmul + 0x80000 >> 20;
        output[stride *  2 + offset] = (z1 + z2) * qmul + 0x80000 >> 20;
        output[stride *  8 + offset] = (z1 - z2) * qmul + 0x80000 >> 20;
        output[stride * 10 + offset] = (z0 - z3) * qmul + 0x80000 >> 20;
    }
}
#undef stride

void ff_svq3_add_idct_c(uint8_t *dst, int16_t *block,
                        int stride, int qp, int dc)
{
    const int qmul = svq3_dequant_coeff[qp];
    int i;

    if (dc) {
        dc       = 13 * 13 * (dc == 1 ? 1538 * block[0]
                                      : qmul * (block[0] >> 3) / 2);
        block[0] = 0;
    }

    for (i = 0; i < 4; i++) {
        const int z0 = 13 * (block[0 + 4 * i] +      block[2 + 4 * i]);
        const int z1 = 13 * (block[0 + 4 * i] -      block[2 + 4 * i]);
        const int z2 =  7 *  block[1 + 4 * i] - 17 * block[3 + 4 * i];
        const int z3 = 17 *  block[1 + 4 * i] +  7 * block[3 + 4 * i];

        block[0 + 4 * i] = z0 + z3;
        block[1 + 4 * i] = z1 + z2;
        block[2 + 4 * i] = z1 - z2;
        block[3 + 4 * i] = z0 - z3;
    }

    for (i = 0; i < 4; i++) {
        const int z0 = 13 * (block[i + 4 * 0] +      block[i + 4 * 2]);
        const int z1 = 13 * (block[i + 4 * 0] -      block[i + 4 * 2]);
        const int z2 =  7 *  block[i + 4 * 1] - 17 * block[i + 4 * 3];
        const int z3 = 17 *  block[i + 4 * 1] +  7 * block[i + 4 * 3];
        const int rr = (dc + 0x80000);

        dst[i + stride * 0] = av_clip_uint8(dst[i + stride * 0] + ((z0 + z3) * qmul + rr >> 20));
        dst[i + stride * 1] = av_clip_uint8(dst[i + stride * 1] + ((z1 + z2) * qmul + rr >> 20));
        dst[i + stride * 2] = av_clip_uint8(dst[i + stride * 2] + ((z1 - z2) * qmul + rr >> 20));
        dst[i + stride * 3] = av_clip_uint8(dst[i + stride * 3] + ((z0 - z3) * qmul + rr >> 20));
    }

    memset(block, 0, 16 * sizeof(int16_t));
}

static inline int svq3_decode_block(GetBitContext *gb, int16_t *block,
                                    int index, const int type)
{
    static const uint8_t *const scan_patterns[4] = {
        luma_dc_zigzag_scan, ff_zigzag_scan, svq3_scan, ff_h264_chroma_dc_scan
    };

    int run, level, sign, limit;
    unsigned vlc;
    const int intra           = 3 * type >> 2;
    const uint8_t *const scan = scan_patterns[type];

    for (limit = (16 >> intra); index < 16; index = limit, limit += 8) {
        for (; (vlc = svq3_get_ue_golomb(gb)) != 0; index++) {
            if ((int32_t)vlc < 0)
                return -1;

            sign     = (vlc & 1) ? 0 : -1;
            vlc      = vlc + 1 >> 1;

            if (type == 3) {
                if (vlc < 3) {
                    run   = 0;
                    level = vlc;
                } else if (vlc < 4) {
                    run   = 1;
                    level = 1;
                } else {
                    run   = vlc & 0x3;
                    level = (vlc + 9 >> 2) - run;
                }
            } else {
                if (vlc < 16U) {
                    run   = svq3_dct_tables[intra][vlc].run;
                    level = svq3_dct_tables[intra][vlc].level;
                } else if (intra) {
                    run   = vlc & 0x7;
                    level = (vlc >> 3) + ((run == 0) ? 8 : ((run < 2) ? 2 : ((run < 5) ? 0 : -1)));
                } else {
                    run   = vlc & 0xF;
                    level = (vlc >> 4) + ((run == 0) ? 4 : ((run < 3) ? 2 : ((run < 10) ? 1 : 0)));
                }
            }


            if ((index += run) >= limit)
                return -1;

            block[scan[index]] = (level ^ sign) - sign;
        }

        if (type != 2) {
            break;
        }
    }

    return 0;
}

static inline void svq3_mc_dir_part(SVQ3Context *s,
                                    int x, int y, int width, int height,
                                    int mx, int my, int dxy,
                                    int thirdpel, int dir, int avg)
{
    H264Context *h = &s->h;
    H264SliceContext *sl = &h->slice_ctx[0];
    const H264Picture *pic = (dir == 0) ? s->last_pic : s->next_pic;
    uint8_t *src, *dest;
    int i, emu = 0;
    int blocksize = 2 - (width >> 3); // 16->0, 8->1, 4->2

    mx += x;
    my += y;

    if (mx < 0 || mx >= s->h_edge_pos - width  - 1 ||
        my < 0 || my >= s->v_edge_pos - height - 1) {
        emu = 1;
        mx = av_clip(mx, -16, s->h_edge_pos - width  + 15);
        my = av_clip(my, -16, s->v_edge_pos - height + 15);
    }

    /* form component predictions */
    dest = h->cur_pic.f->data[0] + x + y * sl->linesize;
    src  = pic->f->data[0] + mx + my * sl->linesize;

    if (emu) {
        h->vdsp.emulated_edge_mc(sl->edge_emu_buffer, src,
                                 sl->linesize, sl->linesize,
                                 width + 1, height + 1,
                                 mx, my, s->h_edge_pos, s->v_edge_pos);
        src = sl->edge_emu_buffer;
    }
    if (thirdpel)
        (avg ? s->tdsp.avg_tpel_pixels_tab
             : s->tdsp.put_tpel_pixels_tab)[dxy](dest, src, sl->linesize,
                                                 width, height);
    else
        (avg ? s->hdsp.avg_pixels_tab
             : s->hdsp.put_pixels_tab)[blocksize][dxy](dest, src, sl->linesize,
                                                       height);

    if (!(h->flags & AV_CODEC_FLAG_GRAY)) {
        mx     = mx + (mx < (int) x) >> 1;
        my     = my + (my < (int) y) >> 1;
        width  = width  >> 1;
        height = height >> 1;
        blocksize++;

        for (i = 1; i < 3; i++) {
            dest = h->cur_pic.f->data[i] + (x >> 1) + (y >> 1) * sl->uvlinesize;
            src  = pic->f->data[i] + mx + my * sl->uvlinesize;

            if (emu) {
                h->vdsp.emulated_edge_mc(sl->edge_emu_buffer, src,
                                         sl->uvlinesize, sl->uvlinesize,
                                         width + 1, height + 1,
                                         mx, my, (s->h_edge_pos >> 1),
                                         s->v_edge_pos >> 1);
                src = sl->edge_emu_buffer;
            }
            if (thirdpel)
                (avg ? s->tdsp.avg_tpel_pixels_tab
                     : s->tdsp.put_tpel_pixels_tab)[dxy](dest, src,
                                                         sl->uvlinesize,
                                                         width, height);
            else
                (avg ? s->hdsp.avg_pixels_tab
                     : s->hdsp.put_pixels_tab)[blocksize][dxy](dest, src,
                                                               sl->uvlinesize,
                                                               height);
        }
    }
}

static inline int svq3_mc_dir(SVQ3Context *s, int size, int mode,
                              int dir, int avg)
{
    int i, j, k, mx, my, dx, dy, x, y;
    H264Context *h          = &s->h;
    H264SliceContext *sl    = &h->slice_ctx[0];
    const int part_width    = ((size & 5) == 4) ? 4 : 16 >> (size & 1);
    const int part_height   = 16 >> ((unsigned)(size + 1) / 3);
    const int extra_width   = (mode == PREDICT_MODE) ? -16 * 6 : 0;
    const int h_edge_pos    = 6 * (s->h_edge_pos - part_width)  - extra_width;
    const int v_edge_pos    = 6 * (s->v_edge_pos - part_height) - extra_width;

    for (i = 0; i < 16; i += part_height)
        for (j = 0; j < 16; j += part_width) {
            const int b_xy = (4 * sl->mb_x + (j >> 2)) +
                             (4 * sl->mb_y + (i >> 2)) * h->b_stride;
            int dxy;
            x = 16 * sl->mb_x + j;
            y = 16 * sl->mb_y + i;
            k = (j >> 2 & 1) + (i >> 1 & 2) +
                (j >> 1 & 4) + (i      & 8);

            if (mode != PREDICT_MODE) {
                pred_motion(h, sl, k, part_width >> 2, dir, 1, &mx, &my);
            } else {
                mx = s->next_pic->motion_val[0][b_xy][0] << 1;
                my = s->next_pic->motion_val[0][b_xy][1] << 1;

                if (dir == 0) {
                    mx = mx * h->frame_num_offset /
                         h->prev_frame_num_offset + 1 >> 1;
                    my = my * h->frame_num_offset /
                         h->prev_frame_num_offset + 1 >> 1;
                } else {
                    mx = mx * (h->frame_num_offset - h->prev_frame_num_offset) /
                         h->prev_frame_num_offset + 1 >> 1;
                    my = my * (h->frame_num_offset - h->prev_frame_num_offset) /
                         h->prev_frame_num_offset + 1 >> 1;
                }
            }

            /* clip motion vector prediction to frame border */
            mx = av_clip(mx, extra_width - 6 * x, h_edge_pos - 6 * x);
            my = av_clip(my, extra_width - 6 * y, v_edge_pos - 6 * y);

            /* get (optional) motion vector differential */
            if (mode == PREDICT_MODE) {
                dx = dy = 0;
            } else {
                dy = svq3_get_se_golomb(&h->gb);
                dx = svq3_get_se_golomb(&h->gb);

                if (dx == INVALID_VLC || dy == INVALID_VLC) {
                    av_log(h->avctx, AV_LOG_ERROR, "invalid MV vlc\n");
                    return -1;
                }
            }

            /* compute motion vector */
            if (mode == THIRDPEL_MODE) {
                int fx, fy;
                mx  = (mx + 1 >> 1) + dx;
                my  = (my + 1 >> 1) + dy;
                fx  = (unsigned)(mx + 0x3000) / 3 - 0x1000;
                fy  = (unsigned)(my + 0x3000) / 3 - 0x1000;
                dxy = (mx - 3 * fx) + 4 * (my - 3 * fy);

                svq3_mc_dir_part(s, x, y, part_width, part_height,
                                 fx, fy, dxy, 1, dir, avg);
                mx += mx;
                my += my;
            } else if (mode == HALFPEL_MODE || mode == PREDICT_MODE) {
                mx  = (unsigned)(mx + 1 + 0x3000) / 3 + dx - 0x1000;
                my  = (unsigned)(my + 1 + 0x3000) / 3 + dy - 0x1000;
                dxy = (mx & 1) + 2 * (my & 1);

                svq3_mc_dir_part(s, x, y, part_width, part_height,
                                 mx >> 1, my >> 1, dxy, 0, dir, avg);
                mx *= 3;
                my *= 3;
            } else {
                mx = (unsigned)(mx + 3 + 0x6000) / 6 + dx - 0x1000;
                my = (unsigned)(my + 3 + 0x6000) / 6 + dy - 0x1000;

                svq3_mc_dir_part(s, x, y, part_width, part_height,
                                 mx, my, 0, 0, dir, avg);
                mx *= 6;
                my *= 6;
            }

            /* update mv_cache */
            if (mode != PREDICT_MODE) {
                int32_t mv = pack16to32(mx, my);

                if (part_height == 8 && i < 8) {
                    AV_WN32A(sl->mv_cache[dir][scan8[k] + 1 * 8], mv);

                    if (part_width == 8 && j < 8)
                        AV_WN32A(sl->mv_cache[dir][scan8[k] + 1 + 1 * 8], mv);
                }
                if (part_width == 8 && j < 8)
                    AV_WN32A(sl->mv_cache[dir][scan8[k] + 1], mv);
                if (part_width == 4 || part_height == 4)
                    AV_WN32A(sl->mv_cache[dir][scan8[k]], mv);
            }

            /* write back motion vectors */
            fill_rectangle(h->cur_pic.motion_val[dir][b_xy],
                           part_width >> 2, part_height >> 2, h->b_stride,
                           pack16to32(mx, my), 4);
        }

    return 0;
}

static av_always_inline void hl_decode_mb_idct_luma(const H264Context *h, H264SliceContext *sl,
                                                    int mb_type, const int *block_offset,
                                                    int linesize, uint8_t *dest_y)
{
    int i;
    if (!IS_INTRA4x4(mb_type)) {
        for (i = 0; i < 16; i++)
            if (sl->non_zero_count_cache[scan8[i]] || sl->mb[i * 16]) {
                uint8_t *const ptr = dest_y + block_offset[i];
                ff_svq3_add_idct_c(ptr, sl->mb + i * 16, linesize,
                                   sl->qscale, IS_INTRA(mb_type) ? 1 : 0);
            }
    }
}

static av_always_inline int dctcoef_get(int16_t *mb, int index)
{
    return AV_RN16A(mb + index);
}

static av_always_inline void hl_decode_mb_predict_luma(const H264Context *h,
                                                       H264SliceContext *sl,
                                                       int mb_type,
                                                       const int *block_offset,
                                                       int linesize,
                                                       uint8_t *dest_y)
{
    int i;
    int qscale = sl->qscale;

    if (IS_INTRA4x4(mb_type)) {
        for (i = 0; i < 16; i++) {
            uint8_t *const ptr = dest_y + block_offset[i];
            const int dir      = sl->intra4x4_pred_mode_cache[scan8[i]];

            uint8_t *topright;
            int nnz, tr;
            if (dir == DIAG_DOWN_LEFT_PRED || dir == VERT_LEFT_PRED) {
                const int topright_avail = (sl->topright_samples_available << i) & 0x8000;
                av_assert2(sl->mb_y || linesize <= block_offset[i]);
                if (!topright_avail) {
                    tr       = ptr[3 - linesize] * 0x01010101u;
                    topright = (uint8_t *)&tr;
                } else
                    topright = ptr + 4 - linesize;
            } else
                topright = NULL;

            h->hpc.pred4x4[dir](ptr, topright, linesize);
            nnz = sl->non_zero_count_cache[scan8[i]];
            if (nnz) {
                ff_svq3_add_idct_c(ptr, sl->mb + i * 16, linesize, qscale, 0);
            }
        }
    } else {
        h->hpc.pred16x16[sl->intra16x16_pred_mode](dest_y, linesize);
        ff_svq3_luma_dc_dequant_idct_c(sl->mb,
                                       sl->mb_luma_dc[0], qscale);
    }
}

static void hl_decode_mb(const H264Context *h, H264SliceContext *sl)
{
    const int mb_x    = sl->mb_x;
    const int mb_y    = sl->mb_y;
    const int mb_xy   = sl->mb_xy;
    const int mb_type = h->cur_pic.mb_type[mb_xy];
    uint8_t *dest_y, *dest_cb, *dest_cr;
    int linesize, uvlinesize;
    int i, j;
    const int *block_offset = &h->block_offset[0];
    const int block_h   = 16 >> h->chroma_y_shift;

    dest_y  = h->cur_pic.f->data[0] + (mb_x     + mb_y * sl->linesize)  * 16;
    dest_cb = h->cur_pic.f->data[1] +  mb_x * 8 + mb_y * sl->uvlinesize * block_h;
    dest_cr = h->cur_pic.f->data[2] +  mb_x * 8 + mb_y * sl->uvlinesize * block_h;

    h->vdsp.prefetch(dest_y  + (sl->mb_x & 3) * 4 * sl->linesize   + 64, sl->linesize,      4);
    h->vdsp.prefetch(dest_cb + (sl->mb_x & 7)     * sl->uvlinesize + 64, dest_cr - dest_cb, 2);

    h->list_counts[mb_xy] = sl->list_count;

    linesize   = sl->mb_linesize   = sl->linesize;
    uvlinesize = sl->mb_uvlinesize = sl->uvlinesize;

    if (IS_INTRA(mb_type)) {
        h->hpc.pred8x8[sl->chroma_pred_mode](dest_cb, uvlinesize);
        h->hpc.pred8x8[sl->chroma_pred_mode](dest_cr, uvlinesize);

        hl_decode_mb_predict_luma(h, sl, mb_type, block_offset, linesize, dest_y);
    }

    hl_decode_mb_idct_luma(h, sl, mb_type, block_offset, linesize, dest_y);

    if (sl->cbp & 0x30) {
        uint8_t *dest[2] = { dest_cb, dest_cr };
        h->h264dsp.h264_chroma_dc_dequant_idct(sl->mb + 16 * 16 * 1,
                                               h->dequant4_coeff[IS_INTRA(mb_type) ? 1 : 4][sl->chroma_qp[0]][0]);
        h->h264dsp.h264_chroma_dc_dequant_idct(sl->mb + 16 * 16 * 2,
                                               h->dequant4_coeff[IS_INTRA(mb_type) ? 2 : 5][sl->chroma_qp[1]][0]);
        for (j = 1; j < 3; j++) {
            for (i = j * 16; i < j * 16 + 4; i++)
                if (sl->non_zero_count_cache[scan8[i]] || sl->mb[i * 16]) {
                    uint8_t *const ptr = dest[j - 1] + block_offset[i];
                    ff_svq3_add_idct_c(ptr, sl->mb + i * 16,
                                       uvlinesize,
                                       ff_h264_chroma_qp[0][sl->qscale + 12] - 12, 2);
                }
        }
    }
}

static int svq3_decode_mb(SVQ3Context *s, unsigned int mb_type)
{
    H264Context *h = &s->h;
    H264SliceContext *sl = &h->slice_ctx[0];
    int i, j, k, m, dir, mode;
    int cbp = 0;
    uint32_t vlc;
    int8_t *top, *left;
    const int mb_xy         = sl->mb_xy;
    const int b_xy          = 4 * sl->mb_x + 4 * sl->mb_y * h->b_stride;

    sl->top_samples_available      = (sl->mb_y == 0) ? 0x33FF : 0xFFFF;
    sl->left_samples_available     = (sl->mb_x == 0) ? 0x5F5F : 0xFFFF;
    sl->topright_samples_available = 0xFFFF;

    if (mb_type == 0) {           /* SKIP */
        if (h->pict_type == AV_PICTURE_TYPE_P ||
            s->next_pic->mb_type[mb_xy] == -1) {
            svq3_mc_dir_part(s, 16 * sl->mb_x, 16 * sl->mb_y, 16, 16,
                             0, 0, 0, 0, 0, 0);

            if (h->pict_type == AV_PICTURE_TYPE_B)
                svq3_mc_dir_part(s, 16 * sl->mb_x, 16 * sl->mb_y, 16, 16,
                                 0, 0, 0, 0, 1, 1);

            mb_type = MB_TYPE_SKIP;
        } else {
            mb_type = FFMIN(s->next_pic->mb_type[mb_xy], 6);
            if (svq3_mc_dir(s, mb_type, PREDICT_MODE, 0, 0) < 0)
                return -1;
            if (svq3_mc_dir(s, mb_type, PREDICT_MODE, 1, 1) < 0)
                return -1;

            mb_type = MB_TYPE_16x16;
        }
    } else if (mb_type < 8) {     /* INTER */
        if (s->thirdpel_flag && s->halfpel_flag == !get_bits1(&h->gb))
            mode = THIRDPEL_MODE;
        else if (s->halfpel_flag &&
                 s->thirdpel_flag == !get_bits1(&h->gb))
            mode = HALFPEL_MODE;
        else
            mode = FULLPEL_MODE;

        /* fill caches */
        /* note ref_cache should contain here:
         *  ????????
         *  ???11111
         *  N??11111
         *  N??11111
         *  N??11111
         */

        for (m = 0; m < 2; m++) {
            if (sl->mb_x > 0 && sl->intra4x4_pred_mode[h->mb2br_xy[mb_xy - 1] + 6] != -1) {
                for (i = 0; i < 4; i++)
                    AV_COPY32(sl->mv_cache[m][scan8[0] - 1 + i * 8],
                              h->cur_pic.motion_val[m][b_xy - 1 + i * h->b_stride]);
            } else {
                for (i = 0; i < 4; i++)
                    AV_ZERO32(sl->mv_cache[m][scan8[0] - 1 + i * 8]);
            }
            if (sl->mb_y > 0) {
                memcpy(sl->mv_cache[m][scan8[0] - 1 * 8],
                       h->cur_pic.motion_val[m][b_xy - h->b_stride],
                       4 * 2 * sizeof(int16_t));
                memset(&sl->ref_cache[m][scan8[0] - 1 * 8],
                       (sl->intra4x4_pred_mode[h->mb2br_xy[mb_xy - h->mb_stride]] == -1) ? PART_NOT_AVAILABLE : 1, 4);

                if (sl->mb_x < h->mb_width - 1) {
                    AV_COPY32(sl->mv_cache[m][scan8[0] + 4 - 1 * 8],
                              h->cur_pic.motion_val[m][b_xy - h->b_stride + 4]);
                    sl->ref_cache[m][scan8[0] + 4 - 1 * 8] =
                        (sl->intra4x4_pred_mode[h->mb2br_xy[mb_xy - h->mb_stride + 1] + 6] == -1 ||
                         sl->intra4x4_pred_mode[h->mb2br_xy[mb_xy - h->mb_stride]] == -1) ? PART_NOT_AVAILABLE : 1;
                } else
                    sl->ref_cache[m][scan8[0] + 4 - 1 * 8] = PART_NOT_AVAILABLE;
                if (sl->mb_x > 0) {
                    AV_COPY32(sl->mv_cache[m][scan8[0] - 1 - 1 * 8],
                              h->cur_pic.motion_val[m][b_xy - h->b_stride - 1]);
                    sl->ref_cache[m][scan8[0] - 1 - 1 * 8] =
                        (sl->intra4x4_pred_mode[h->mb2br_xy[mb_xy - h->mb_stride - 1] + 3] == -1) ? PART_NOT_AVAILABLE : 1;
                } else
                    sl->ref_cache[m][scan8[0] - 1 - 1 * 8] = PART_NOT_AVAILABLE;
            } else
                memset(&sl->ref_cache[m][scan8[0] - 1 * 8 - 1],
                       PART_NOT_AVAILABLE, 8);

            if (h->pict_type != AV_PICTURE_TYPE_B)
                break;
        }

        /* decode motion vector(s) and form prediction(s) */
        if (h->pict_type == AV_PICTURE_TYPE_P) {
            if (svq3_mc_dir(s, mb_type - 1, mode, 0, 0) < 0)
                return -1;
        } else {        /* AV_PICTURE_TYPE_B */
            if (mb_type != 2) {
                if (svq3_mc_dir(s, 0, mode, 0, 0) < 0)
                    return -1;
            } else {
                for (i = 0; i < 4; i++)
                    memset(h->cur_pic.motion_val[0][b_xy + i * h->b_stride],
                           0, 4 * 2 * sizeof(int16_t));
            }
            if (mb_type != 1) {
                if (svq3_mc_dir(s, 0, mode, 1, mb_type == 3) < 0)
                    return -1;
            } else {
                for (i = 0; i < 4; i++)
                    memset(h->cur_pic.motion_val[1][b_xy + i * h->b_stride],
                           0, 4 * 2 * sizeof(int16_t));
            }
        }

        mb_type = MB_TYPE_16x16;
    } else if (mb_type == 8 || mb_type == 33) {   /* INTRA4x4 */
        memset(sl->intra4x4_pred_mode_cache, -1, 8 * 5 * sizeof(int8_t));

        if (mb_type == 8) {
            if (sl->mb_x > 0) {
                for (i = 0; i < 4; i++)
                    sl->intra4x4_pred_mode_cache[scan8[0] - 1 + i * 8] = sl->intra4x4_pred_mode[h->mb2br_xy[mb_xy - 1] + 6 - i];
                if (sl->intra4x4_pred_mode_cache[scan8[0] - 1] == -1)
                    sl->left_samples_available = 0x5F5F;
            }
            if (sl->mb_y > 0) {
                sl->intra4x4_pred_mode_cache[4 + 8 * 0] = sl->intra4x4_pred_mode[h->mb2br_xy[mb_xy - h->mb_stride] + 0];
                sl->intra4x4_pred_mode_cache[5 + 8 * 0] = sl->intra4x4_pred_mode[h->mb2br_xy[mb_xy - h->mb_stride] + 1];
                sl->intra4x4_pred_mode_cache[6 + 8 * 0] = sl->intra4x4_pred_mode[h->mb2br_xy[mb_xy - h->mb_stride] + 2];
                sl->intra4x4_pred_mode_cache[7 + 8 * 0] = sl->intra4x4_pred_mode[h->mb2br_xy[mb_xy - h->mb_stride] + 3];

                if (sl->intra4x4_pred_mode_cache[4 + 8 * 0] == -1)
                    sl->top_samples_available = 0x33FF;
            }

            /* decode prediction codes for luma blocks */
            for (i = 0; i < 16; i += 2) {
                vlc = svq3_get_ue_golomb(&h->gb);

                if (vlc >= 25U) {
                    av_log(h->avctx, AV_LOG_ERROR,
                           "luma prediction:%"PRIu32"\n", vlc);
                    return -1;
                }

                left = &sl->intra4x4_pred_mode_cache[scan8[i] - 1];
                top  = &sl->intra4x4_pred_mode_cache[scan8[i] - 8];

                left[1] = svq3_pred_1[top[0] + 1][left[0] + 1][svq3_pred_0[vlc][0]];
                left[2] = svq3_pred_1[top[1] + 1][left[1] + 1][svq3_pred_0[vlc][1]];

                if (left[1] == -1 || left[2] == -1) {
                    av_log(h->avctx, AV_LOG_ERROR, "weird prediction\n");
                    return -1;
                }
            }
        } else {    /* mb_type == 33, DC_128_PRED block type */
            for (i = 0; i < 4; i++)
                memset(&sl->intra4x4_pred_mode_cache[scan8[0] + 8 * i], DC_PRED, 4);
        }

        write_back_intra_pred_mode(h, sl);

        if (mb_type == 8) {
            ff_h264_check_intra4x4_pred_mode(h, sl);

            sl->top_samples_available  = (sl->mb_y == 0) ? 0x33FF : 0xFFFF;
            sl->left_samples_available = (sl->mb_x == 0) ? 0x5F5F : 0xFFFF;
        } else {
            for (i = 0; i < 4; i++)
                memset(&sl->intra4x4_pred_mode_cache[scan8[0] + 8 * i], DC_128_PRED, 4);

            sl->top_samples_available  = 0x33FF;
            sl->left_samples_available = 0x5F5F;
        }

        mb_type = MB_TYPE_INTRA4x4;
    } else {                      /* INTRA16x16 */
        dir = ff_h264_i_mb_type_info[mb_type - 8].pred_mode;
        dir = (dir >> 1) ^ 3 * (dir & 1) ^ 1;

        if ((sl->intra16x16_pred_mode = ff_h264_check_intra_pred_mode(h, sl, dir, 0)) < 0) {
            av_log(h->avctx, AV_LOG_ERROR, "ff_h264_check_intra_pred_mode < 0\n");
            return sl->intra16x16_pred_mode;
        }

        cbp     = ff_h264_i_mb_type_info[mb_type - 8].cbp;
        mb_type = MB_TYPE_INTRA16x16;
    }

    if (!IS_INTER(mb_type) && h->pict_type != AV_PICTURE_TYPE_I) {
        for (i = 0; i < 4; i++)
            memset(h->cur_pic.motion_val[0][b_xy + i * h->b_stride],
                   0, 4 * 2 * sizeof(int16_t));
        if (h->pict_type == AV_PICTURE_TYPE_B) {
            for (i = 0; i < 4; i++)
                memset(h->cur_pic.motion_val[1][b_xy + i * h->b_stride],
                       0, 4 * 2 * sizeof(int16_t));
        }
    }
    if (!IS_INTRA4x4(mb_type)) {
        memset(sl->intra4x4_pred_mode + h->mb2br_xy[mb_xy], DC_PRED, 8);
    }
    if (!IS_SKIP(mb_type) || h->pict_type == AV_PICTURE_TYPE_B) {
        memset(sl->non_zero_count_cache + 8, 0, 14 * 8 * sizeof(uint8_t));
    }

    if (!IS_INTRA16x16(mb_type) &&
        (!IS_SKIP(mb_type) || h->pict_type == AV_PICTURE_TYPE_B)) {
        if ((vlc = svq3_get_ue_golomb(&h->gb)) >= 48U){
            av_log(h->avctx, AV_LOG_ERROR, "cbp_vlc=%"PRIu32"\n", vlc);
            return -1;
        }

        cbp = IS_INTRA(mb_type) ? ff_h264_golomb_to_intra4x4_cbp[vlc]
                                : ff_h264_golomb_to_inter_cbp[vlc];
    }
    if (IS_INTRA16x16(mb_type) ||
        (h->pict_type != AV_PICTURE_TYPE_I && s->adaptive_quant && cbp)) {
        sl->qscale += svq3_get_se_golomb(&h->gb);

        if (sl->qscale > 31u) {
            av_log(h->avctx, AV_LOG_ERROR, "qscale:%d\n", sl->qscale);
            return -1;
        }
    }
    if (IS_INTRA16x16(mb_type)) {
        AV_ZERO128(sl->mb_luma_dc[0] + 0);
        AV_ZERO128(sl->mb_luma_dc[0] + 8);
        if (svq3_decode_block(&h->gb, sl->mb_luma_dc[0], 0, 1)) {
            av_log(h->avctx, AV_LOG_ERROR,
                   "error while decoding intra luma dc\n");
            return -1;
        }
    }

    if (cbp) {
        const int index = IS_INTRA16x16(mb_type) ? 1 : 0;
        const int type  = ((sl->qscale < 24 && IS_INTRA4x4(mb_type)) ? 2 : 1);

        for (i = 0; i < 4; i++)
            if ((cbp & (1 << i))) {
                for (j = 0; j < 4; j++) {
                    k = index ? (1 * (j & 1) + 2 * (i & 1) +
                                 2 * (j & 2) + 4 * (i & 2))
                              : (4 * i + j);
                    sl->non_zero_count_cache[scan8[k]] = 1;

                    if (svq3_decode_block(&h->gb, &sl->mb[16 * k], index, type)) {
                        av_log(h->avctx, AV_LOG_ERROR,
                               "error while decoding block\n");
                        return -1;
                    }
                }
            }

        if ((cbp & 0x30)) {
            for (i = 1; i < 3; ++i)
                if (svq3_decode_block(&h->gb, &sl->mb[16 * 16 * i], 0, 3)) {
                    av_log(h->avctx, AV_LOG_ERROR,
                           "error while decoding chroma dc block\n");
                    return -1;
                }

            if ((cbp & 0x20)) {
                for (i = 1; i < 3; i++) {
                    for (j = 0; j < 4; j++) {
                        k                                 = 16 * i + j;
                        sl->non_zero_count_cache[scan8[k]] = 1;

                        if (svq3_decode_block(&h->gb, &sl->mb[16 * k], 1, 1)) {
                            av_log(h->avctx, AV_LOG_ERROR,
                                   "error while decoding chroma ac block\n");
                            return -1;
                        }
                    }
                }
            }
        }
    }

    sl->cbp                   = cbp;
    h->cur_pic.mb_type[mb_xy] = mb_type;

    if (IS_INTRA(mb_type))
        sl->chroma_pred_mode = ff_h264_check_intra_pred_mode(h, sl, DC_PRED8x8, 1);

    return 0;
}

static int svq3_decode_slice_header(AVCodecContext *avctx)
{
    SVQ3Context *s = avctx->priv_data;
    H264Context *h    = &s->h;
    H264SliceContext *sl = &h->slice_ctx[0];
    const int mb_xy   = sl->mb_xy;
    int i, header;
    unsigned slice_id;

    header = get_bits(&s->gb, 8);

    if (((header & 0x9F) != 1 && (header & 0x9F) != 2) || (header & 0x60) == 0) {
        /* TODO: what? */
        av_log(avctx, AV_LOG_ERROR, "unsupported slice header (%02X)\n", header);
        return -1;
    } else {
        int slice_bits, slice_bytes, slice_length;
        int length = header >> 5 & 3;

        slice_length = show_bits(&s->gb, 8 * length);
        slice_bits   = slice_length * 8;
        slice_bytes  = slice_length + length - 1;

        if (slice_bytes > get_bits_left(&s->gb)) {
            av_log(avctx, AV_LOG_ERROR, "slice after bitstream end\n");
            return -1;
        }

        skip_bits(&s->gb, 8);

        av_fast_malloc(&s->slice_buf, &s->slice_size, slice_bytes + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!s->slice_buf)
            return AVERROR(ENOMEM);

        memcpy(s->slice_buf, s->gb.buffer + s->gb.index / 8, slice_bytes);

        init_get_bits(&h->gb, s->slice_buf, slice_bits);

        if (s->watermark_key) {
            uint32_t header = AV_RL32(&h->gb.buffer[1]);
            AV_WL32(&h->gb.buffer[1], header ^ s->watermark_key);
        }
        if (length > 0) {
            memmove(s->slice_buf, &s->slice_buf[slice_length], length - 1);
        }
        skip_bits_long(&s->gb, slice_bytes * 8);
    }

    if ((slice_id = svq3_get_ue_golomb(&h->gb)) >= 3) {
        av_log(h->avctx, AV_LOG_ERROR, "illegal slice type %u \n", slice_id);
        return -1;
    }

    sl->slice_type = ff_h264_golomb_to_pict_type[slice_id];

    if ((header & 0x9F) == 2) {
        i              = (h->mb_num < 64) ? 6 : (1 + av_log2(h->mb_num - 1));
        sl->mb_skip_run = get_bits(&h->gb, i) -
                         (sl->mb_y * h->mb_width + sl->mb_x);
    } else {
        skip_bits1(&h->gb);
        sl->mb_skip_run = 0;
    }

    sl->slice_num     = get_bits(&h->gb, 8);
    sl->qscale        = get_bits(&h->gb, 5);
    s->adaptive_quant = get_bits1(&h->gb);

    /* unknown fields */
    skip_bits1(&h->gb);

    if (s->has_watermark)
        skip_bits1(&h->gb);

    skip_bits1(&h->gb);
    skip_bits(&h->gb, 2);

    if (skip_1stop_8data_bits(&h->gb) < 0)
        return AVERROR_INVALIDDATA;

    /* reset intra predictors and invalidate motion vector references */
    if (sl->mb_x > 0) {
        memset(sl->intra4x4_pred_mode + h->mb2br_xy[mb_xy - 1] + 3,
               -1, 4 * sizeof(int8_t));
        memset(sl->intra4x4_pred_mode + h->mb2br_xy[mb_xy - sl->mb_x],
               -1, 8 * sizeof(int8_t) * sl->mb_x);
    }
    if (sl->mb_y > 0) {
        memset(sl->intra4x4_pred_mode + h->mb2br_xy[mb_xy - h->mb_stride],
               -1, 8 * sizeof(int8_t) * (h->mb_width - sl->mb_x));

        if (sl->mb_x > 0)
            sl->intra4x4_pred_mode[h->mb2br_xy[mb_xy - h->mb_stride - 1] + 3] = -1;
    }

    return 0;
}

static av_cold int svq3_decode_init(AVCodecContext *avctx)
{
    SVQ3Context *s = avctx->priv_data;
    H264Context *h = &s->h;
    H264SliceContext *sl;
    int m;
    unsigned char *extradata;
    unsigned char *extradata_end;
    unsigned int size;
    int marker_found = 0;
    int ret;

    s->cur_pic  = av_mallocz(sizeof(*s->cur_pic));
    s->last_pic = av_mallocz(sizeof(*s->last_pic));
    s->next_pic = av_mallocz(sizeof(*s->next_pic));
    if (!s->next_pic || !s->last_pic || !s->cur_pic) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    s->cur_pic->f  = av_frame_alloc();
    s->last_pic->f = av_frame_alloc();
    s->next_pic->f = av_frame_alloc();
    if (!s->cur_pic->f || !s->last_pic->f || !s->next_pic->f)
        return AVERROR(ENOMEM);

    if ((ret = ff_h264_decode_init(avctx)) < 0)
        goto fail;

    // we will overwrite it later during decoding
    av_frame_free(&h->cur_pic.f);

    av_frame_free(&h->last_pic_for_ec.f);

    ff_h264dsp_init(&h->h264dsp, 8, 1);
    av_assert0(h->sps.bit_depth_chroma == 0);
    ff_h264_pred_init(&h->hpc, AV_CODEC_ID_SVQ3, 8, 1);
    ff_videodsp_init(&h->vdsp, 8);

    memset(h->pps.scaling_matrix4, 16, 6 * 16 * sizeof(uint8_t));
    memset(h->pps.scaling_matrix8, 16, 2 * 64 * sizeof(uint8_t));

    avctx->bits_per_raw_sample = 8;
    h->sps.bit_depth_luma = 8;
    h->chroma_format_idc = 1;

    ff_hpeldsp_init(&s->hdsp, avctx->flags);
    ff_tpeldsp_init(&s->tdsp);

    sl = h->slice_ctx;

    h->flags           = avctx->flags;
    sl->is_complex     = 1;
    h->sps.chroma_format_idc = 1;
    h->picture_structure = PICT_FRAME;
    avctx->pix_fmt     = AV_PIX_FMT_YUVJ420P;
    avctx->color_range = AVCOL_RANGE_JPEG;

    h->slice_ctx[0].chroma_qp[0] = h->slice_ctx[0].chroma_qp[1] = 4;
    h->chroma_x_shift = h->chroma_y_shift = 1;

    s->halfpel_flag  = 1;
    s->thirdpel_flag = 1;
    s->has_watermark = 0;

    /* prowl for the "SEQH" marker in the extradata */
    extradata     = (unsigned char *)avctx->extradata;
    extradata_end = avctx->extradata + avctx->extradata_size;
    if (extradata) {
        for (m = 0; m + 8 < avctx->extradata_size; m++) {
            if (!memcmp(extradata, "SEQH", 4)) {
                marker_found = 1;
                break;
            }
            extradata++;
        }
    }

    /* if a match was found, parse the extra data */
    if (marker_found) {
        GetBitContext gb;
        int frame_size_code;
        int unk0, unk1, unk2, unk3, unk4;

        size = AV_RB32(&extradata[4]);
        if (size > extradata_end - extradata - 8) {
            ret = AVERROR_INVALIDDATA;
            goto fail;
        }
        init_get_bits(&gb, extradata + 8, size * 8);

        /* 'frame size code' and optional 'width, height' */
        frame_size_code = get_bits(&gb, 3);
        switch (frame_size_code) {
        case 0:
            avctx->width  = 160;
            avctx->height = 120;
            break;
        case 1:
            avctx->width  = 128;
            avctx->height =  96;
            break;
        case 2:
            avctx->width  = 176;
            avctx->height = 144;
            break;
        case 3:
            avctx->width  = 352;
            avctx->height = 288;
            break;
        case 4:
            avctx->width  = 704;
            avctx->height = 576;
            break;
        case 5:
            avctx->width  = 240;
            avctx->height = 180;
            break;
        case 6:
            avctx->width  = 320;
            avctx->height = 240;
            break;
        case 7:
            avctx->width  = get_bits(&gb, 12);
            avctx->height = get_bits(&gb, 12);
            break;
        }

        s->halfpel_flag  = get_bits1(&gb);
        s->thirdpel_flag = get_bits1(&gb);

        /* unknown fields */
        unk0 = get_bits1(&gb);
        unk1 = get_bits1(&gb);
        unk2 = get_bits1(&gb);
        unk3 = get_bits1(&gb);

        h->low_delay = get_bits1(&gb);

        /* unknown field */
        unk4 = get_bits1(&gb);

        av_log(avctx, AV_LOG_DEBUG, "Unknown fields %d %d %d %d %d\n",
               unk0, unk1, unk2, unk3, unk4);

        if (skip_1stop_8data_bits(&gb) < 0) {
            ret = AVERROR_INVALIDDATA;
            goto fail;
        }

        s->has_watermark  = get_bits1(&gb);
        avctx->has_b_frames = !h->low_delay;
        if (s->has_watermark) {
#if CONFIG_ZLIB
            unsigned watermark_width  = svq3_get_ue_golomb(&gb);
            unsigned watermark_height = svq3_get_ue_golomb(&gb);
            int u1                    = svq3_get_ue_golomb(&gb);
            int u2                    = get_bits(&gb, 8);
            int u3                    = get_bits(&gb, 2);
            int u4                    = svq3_get_ue_golomb(&gb);
            unsigned long buf_len     = watermark_width *
                                        watermark_height * 4;
            int offset                = get_bits_count(&gb) + 7 >> 3;
            uint8_t *buf;

            if (watermark_height <= 0 ||
                (uint64_t)watermark_width * 4 > UINT_MAX / watermark_height) {
                ret = -1;
                goto fail;
            }

            buf = av_malloc(buf_len);
            if (!buf) {
                ret = AVERROR(ENOMEM);
                goto fail;
            }
            av_log(avctx, AV_LOG_DEBUG, "watermark size: %ux%u\n",
                   watermark_width, watermark_height);
            av_log(avctx, AV_LOG_DEBUG,
                   "u1: %x u2: %x u3: %x compressed data size: %d offset: %d\n",
                   u1, u2, u3, u4, offset);
            if (uncompress(buf, &buf_len, extradata + 8 + offset,
                           size - offset) != Z_OK) {
                av_log(avctx, AV_LOG_ERROR,
                       "could not uncompress watermark logo\n");
                av_free(buf);
                ret = -1;
                goto fail;
            }
            s->watermark_key = ff_svq1_packet_checksum(buf, buf_len, 0);
            s->watermark_key = s->watermark_key << 16 | s->watermark_key;
            av_log(avctx, AV_LOG_DEBUG,
                   "watermark key %#"PRIx32"\n", s->watermark_key);
            av_free(buf);
#else
            av_log(avctx, AV_LOG_ERROR,
                   "this svq3 file contains watermark which need zlib support compiled in\n");
            ret = -1;
            goto fail;
#endif
        }
    }

    h->width  = avctx->width;
    h->height = avctx->height;
    h->mb_width  = (h->width + 15) / 16;
    h->mb_height = (h->height + 15) / 16;
    h->mb_stride = h->mb_width + 1;
    h->mb_num    = h->mb_width * h->mb_height;
    h->b_stride = 4 * h->mb_width;
    s->h_edge_pos = h->mb_width * 16;
    s->v_edge_pos = h->mb_height * 16;

    if ((ret = ff_h264_alloc_tables(h)) < 0) {
        av_log(avctx, AV_LOG_ERROR, "svq3 memory allocation failed\n");
        goto fail;
    }

    return 0;
fail:
    svq3_decode_end(avctx);
    return ret;
}

static void free_picture(AVCodecContext *avctx, H264Picture *pic)
{
    int i;
    for (i = 0; i < 2; i++) {
        av_buffer_unref(&pic->motion_val_buf[i]);
        av_buffer_unref(&pic->ref_index_buf[i]);
    }
    av_buffer_unref(&pic->mb_type_buf);

    av_frame_unref(pic->f);
}

static int get_buffer(AVCodecContext *avctx, H264Picture *pic)
{
    SVQ3Context *s = avctx->priv_data;
    H264Context *h = &s->h;
    H264SliceContext *sl = &h->slice_ctx[0];
    const int big_mb_num    = h->mb_stride * (h->mb_height + 1) + 1;
    const int mb_array_size = h->mb_stride * h->mb_height;
    const int b4_stride     = h->mb_width * 4 + 1;
    const int b4_array_size = b4_stride * h->mb_height * 4;
    int ret;

    if (!pic->motion_val_buf[0]) {
        int i;

        pic->mb_type_buf = av_buffer_allocz((big_mb_num + h->mb_stride) * sizeof(uint32_t));
        if (!pic->mb_type_buf)
            return AVERROR(ENOMEM);
        pic->mb_type = (uint32_t*)pic->mb_type_buf->data + 2 * h->mb_stride + 1;

        for (i = 0; i < 2; i++) {
            pic->motion_val_buf[i] = av_buffer_allocz(2 * (b4_array_size + 4) * sizeof(int16_t));
            pic->ref_index_buf[i]  = av_buffer_allocz(4 * mb_array_size);
            if (!pic->motion_val_buf[i] || !pic->ref_index_buf[i]) {
                ret = AVERROR(ENOMEM);
                goto fail;
            }

            pic->motion_val[i] = (int16_t (*)[2])pic->motion_val_buf[i]->data + 4;
            pic->ref_index[i]  = pic->ref_index_buf[i]->data;
        }
    }
    pic->reference = !(h->pict_type == AV_PICTURE_TYPE_B);

    ret = ff_get_buffer(avctx, pic->f,
                        pic->reference ? AV_GET_BUFFER_FLAG_REF : 0);
    if (ret < 0)
        goto fail;

    if (!sl->edge_emu_buffer) {
        sl->edge_emu_buffer = av_mallocz_array(pic->f->linesize[0], 17);
        if (!sl->edge_emu_buffer)
            return AVERROR(ENOMEM);
    }

    sl->linesize   = pic->f->linesize[0];
    sl->uvlinesize = pic->f->linesize[1];

    return 0;
fail:
    free_picture(avctx, pic);
    return ret;
}

static int svq3_decode_frame(AVCodecContext *avctx, void *data,
                             int *got_frame, AVPacket *avpkt)
{
    SVQ3Context *s     = avctx->priv_data;
    H264Context *h     = &s->h;
    H264SliceContext *sl = &h->slice_ctx[0];
    int buf_size       = avpkt->size;
    int left;
    uint8_t *buf;
    int ret, m, i;

    /* special case for last picture */
    if (buf_size == 0) {
        if (s->next_pic->f->data[0] && !h->low_delay && !s->last_frame_output) {
            ret = av_frame_ref(data, s->next_pic->f);
            if (ret < 0)
                return ret;
            s->last_frame_output = 1;
            *got_frame          = 1;
        }
        return 0;
    }

    sl->mb_x = sl->mb_y = sl->mb_xy = 0;

    if (s->watermark_key) {
        av_fast_padded_malloc(&s->buf, &s->buf_size, buf_size);
        if (!s->buf)
            return AVERROR(ENOMEM);
        memcpy(s->buf, avpkt->data, buf_size);
        buf = s->buf;
    } else {
        buf = avpkt->data;
    }

    ret = init_get_bits(&s->gb, buf, 8 * buf_size);
    if (ret < 0)
        return ret;

    if (svq3_decode_slice_header(avctx))
        return -1;

    h->pict_type = sl->slice_type;

    if (h->pict_type != AV_PICTURE_TYPE_B)
        FFSWAP(H264Picture*, s->next_pic, s->last_pic);

    av_frame_unref(s->cur_pic->f);

    /* for skipping the frame */
    s->cur_pic->f->pict_type = h->pict_type;
    s->cur_pic->f->key_frame = (h->pict_type == AV_PICTURE_TYPE_I);

    ret = get_buffer(avctx, s->cur_pic);
    if (ret < 0)
        return ret;

    h->cur_pic_ptr = s->cur_pic;
    h->cur_pic     = *s->cur_pic;

    for (i = 0; i < 16; i++) {
        h->block_offset[i]           = (4 * ((scan8[i] - scan8[0]) & 7)) + 4 * sl->linesize * ((scan8[i] - scan8[0]) >> 3);
        h->block_offset[48 + i]      = (4 * ((scan8[i] - scan8[0]) & 7)) + 8 * sl->linesize * ((scan8[i] - scan8[0]) >> 3);
    }
    for (i = 0; i < 16; i++) {
        h->block_offset[16 + i]      =
        h->block_offset[32 + i]      = (4 * ((scan8[i] - scan8[0]) & 7)) + 4 * sl->uvlinesize * ((scan8[i] - scan8[0]) >> 3);
        h->block_offset[48 + 16 + i] =
        h->block_offset[48 + 32 + i] = (4 * ((scan8[i] - scan8[0]) & 7)) + 8 * sl->uvlinesize * ((scan8[i] - scan8[0]) >> 3);
    }

    if (h->pict_type != AV_PICTURE_TYPE_I) {
        if (!s->last_pic->f->data[0]) {
            av_log(avctx, AV_LOG_ERROR, "Missing reference frame.\n");
            av_frame_unref(s->last_pic->f);
            ret = get_buffer(avctx, s->last_pic);
            if (ret < 0)
                return ret;
            memset(s->last_pic->f->data[0], 0, avctx->height * s->last_pic->f->linesize[0]);
            memset(s->last_pic->f->data[1], 0x80, (avctx->height / 2) *
                   s->last_pic->f->linesize[1]);
            memset(s->last_pic->f->data[2], 0x80, (avctx->height / 2) *
                   s->last_pic->f->linesize[2]);
        }

        if (h->pict_type == AV_PICTURE_TYPE_B && !s->next_pic->f->data[0]) {
            av_log(avctx, AV_LOG_ERROR, "Missing reference frame.\n");
            av_frame_unref(s->next_pic->f);
            ret = get_buffer(avctx, s->next_pic);
            if (ret < 0)
                return ret;
            memset(s->next_pic->f->data[0], 0, avctx->height * s->next_pic->f->linesize[0]);
            memset(s->next_pic->f->data[1], 0x80, (avctx->height / 2) *
                   s->next_pic->f->linesize[1]);
            memset(s->next_pic->f->data[2], 0x80, (avctx->height / 2) *
                   s->next_pic->f->linesize[2]);
        }
    }

    if (avctx->debug & FF_DEBUG_PICT_INFO)
        av_log(h->avctx, AV_LOG_DEBUG,
               "%c hpel:%d, tpel:%d aqp:%d qp:%d, slice_num:%02X\n",
               av_get_picture_type_char(h->pict_type),
               s->halfpel_flag, s->thirdpel_flag,
               s->adaptive_quant, h->slice_ctx[0].qscale, sl->slice_num);

    if (avctx->skip_frame >= AVDISCARD_NONREF && h->pict_type == AV_PICTURE_TYPE_B ||
        avctx->skip_frame >= AVDISCARD_NONKEY && h->pict_type != AV_PICTURE_TYPE_I ||
        avctx->skip_frame >= AVDISCARD_ALL)
        return 0;

    if (s->next_p_frame_damaged) {
        if (h->pict_type == AV_PICTURE_TYPE_B)
            return 0;
        else
            s->next_p_frame_damaged = 0;
    }

    if (h->pict_type == AV_PICTURE_TYPE_B) {
        h->frame_num_offset = sl->slice_num - h->prev_frame_num;

        if (h->frame_num_offset < 0)
            h->frame_num_offset += 256;
        if (h->frame_num_offset == 0 ||
            h->frame_num_offset >= h->prev_frame_num_offset) {
            av_log(h->avctx, AV_LOG_ERROR, "error in B-frame picture id\n");
            return -1;
        }
    } else {
        h->prev_frame_num        = h->frame_num;
        h->frame_num             = sl->slice_num;
        h->prev_frame_num_offset = h->frame_num - h->prev_frame_num;

        if (h->prev_frame_num_offset < 0)
            h->prev_frame_num_offset += 256;
    }

    for (m = 0; m < 2; m++) {
        int i;
        for (i = 0; i < 4; i++) {
            int j;
            for (j = -1; j < 4; j++)
                sl->ref_cache[m][scan8[0] + 8 * i + j] = 1;
            if (i < 3)
                sl->ref_cache[m][scan8[0] + 8 * i + j] = PART_NOT_AVAILABLE;
        }
    }

    for (sl->mb_y = 0; sl->mb_y < h->mb_height; sl->mb_y++) {
        for (sl->mb_x = 0; sl->mb_x < h->mb_width; sl->mb_x++) {
            unsigned mb_type;
            sl->mb_xy = sl->mb_x + sl->mb_y * h->mb_stride;

            if ((get_bits_left(&h->gb)) <= 7) {
                if (((get_bits_count(&h->gb) & 7) == 0 ||
                    show_bits(&h->gb, get_bits_left(&h->gb) & 7) == 0)) {

                    if (svq3_decode_slice_header(avctx))
                        return -1;
                }
                /* TODO: support s->mb_skip_run */
            }

            mb_type = svq3_get_ue_golomb(&h->gb);

            if (h->pict_type == AV_PICTURE_TYPE_I)
                mb_type += 8;
            else if (h->pict_type == AV_PICTURE_TYPE_B && mb_type >= 4)
                mb_type += 4;
            if (mb_type > 33 || svq3_decode_mb(s, mb_type)) {
                av_log(h->avctx, AV_LOG_ERROR,
                       "error while decoding MB %d %d\n", sl->mb_x, sl->mb_y);
                return -1;
            }

            if (mb_type != 0 || sl->cbp)
                hl_decode_mb(h, &h->slice_ctx[0]);

            if (h->pict_type != AV_PICTURE_TYPE_B && !h->low_delay)
                h->cur_pic.mb_type[sl->mb_x + sl->mb_y * h->mb_stride] =
                    (h->pict_type == AV_PICTURE_TYPE_P && mb_type < 8) ? (mb_type - 1) : -1;
        }

        ff_draw_horiz_band(avctx, s->cur_pic->f,
                           s->last_pic->f->data[0] ? s->last_pic->f : NULL,
                           16 * sl->mb_y, 16, h->picture_structure, 0,
                           h->low_delay);
    }

    left = buf_size*8 - get_bits_count(&h->gb);

    if (sl->mb_y != h->mb_height || sl->mb_x != h->mb_width) {
        av_log(avctx, AV_LOG_INFO, "frame num %d incomplete pic x %d y %d left %d\n", avctx->frame_number, sl->mb_y, sl->mb_x, left);
        //av_hex_dump(stderr, buf+buf_size-8, 8);
    }

    if (left < 0) {
        av_log(avctx, AV_LOG_ERROR, "frame num %d left %d\n", avctx->frame_number, left);
        return -1;
    }

    if (h->pict_type == AV_PICTURE_TYPE_B || h->low_delay)
        ret = av_frame_ref(data, s->cur_pic->f);
    else if (s->last_pic->f->data[0])
        ret = av_frame_ref(data, s->last_pic->f);
    if (ret < 0)
        return ret;

    /* Do not output the last pic after seeking. */
    if (s->last_pic->f->data[0] || h->low_delay)
        *got_frame = 1;

    if (h->pict_type != AV_PICTURE_TYPE_B) {
        FFSWAP(H264Picture*, s->cur_pic, s->next_pic);
    } else {
        av_frame_unref(s->cur_pic->f);
    }

    return buf_size;
}

static av_cold int svq3_decode_end(AVCodecContext *avctx)
{
    SVQ3Context *s = avctx->priv_data;
    H264Context *h = &s->h;

    free_picture(avctx, s->cur_pic);
    free_picture(avctx, s->next_pic);
    free_picture(avctx, s->last_pic);
    av_frame_free(&s->cur_pic->f);
    av_frame_free(&s->next_pic->f);
    av_frame_free(&s->last_pic->f);
    av_freep(&s->cur_pic);
    av_freep(&s->next_pic);
    av_freep(&s->last_pic);
    av_freep(&s->slice_buf);

    memset(&h->cur_pic, 0, sizeof(h->cur_pic));

    ff_h264_free_context(h);

    av_freep(&s->buf);
    s->buf_size = 0;

    return 0;
}

AVCodec ff_svq3_decoder = {
    .name           = "svq3",
    .long_name      = NULL_IF_CONFIG_SMALL("Sorenson Vector Quantizer 3 / Sorenson Video 3 / SVQ3"),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_SVQ3,
    .priv_data_size = sizeof(SVQ3Context),
    .init           = svq3_decode_init,
    .close          = svq3_decode_end,
    .decode         = svq3_decode_frame,
    .capabilities   = AV_CODEC_CAP_DRAW_HORIZ_BAND |
                      AV_CODEC_CAP_DR1             |
                      AV_CODEC_CAP_DELAY,
    .pix_fmts       = (const enum AVPixelFormat[]) { AV_PIX_FMT_YUVJ420P,
                                                     AV_PIX_FMT_NONE},
};
