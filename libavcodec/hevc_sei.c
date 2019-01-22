/*
 * HEVC Supplementary Enhancement Information messages
 *
 * Copyright (C) 2012 - 2013 Guillaume Martres
 * Copyright (C) 2012 - 2013 Gildas Cocherel
 * Copyright (C) 2013 Vittorio Giovara
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

#include "golomb.h"
#include "hevc_ps.h"
#include "hevc_sei.h"
#include "libavutil/hdr_dynamic_metadata.h"

static int decode_nal_sei_decoded_picture_hash(HEVCSEIPictureHash *s, GetBitContext *gb)
{
    int cIdx, i;
    uint8_t hash_type;
    //uint16_t picture_crc;
    //uint32_t picture_checksum;
    hash_type = get_bits(gb, 8);

    for (cIdx = 0; cIdx < 3/*((s->sps->chroma_format_idc == 0) ? 1 : 3)*/; cIdx++) {
        if (hash_type == 0) {
            s->is_md5 = 1;
            for (i = 0; i < 16; i++)
                s->md5[cIdx][i] = get_bits(gb, 8);
        } else if (hash_type == 1) {
            // picture_crc = get_bits(gb, 16);
            skip_bits(gb, 16);
        } else if (hash_type == 2) {
            // picture_checksum = get_bits_long(gb, 32);
            skip_bits(gb, 32);
        }
    }
    return 0;
}

static int decode_nal_sei_mastering_display_info(HEVCSEIMasteringDisplay *s, GetBitContext *gb)
{
    int i;
    // Mastering primaries
    for (i = 0; i < 3; i++) {
        s->display_primaries[i][0] = get_bits(gb, 16);
        s->display_primaries[i][1] = get_bits(gb, 16);
    }
    // White point (x, y)
    s->white_point[0] = get_bits(gb, 16);
    s->white_point[1] = get_bits(gb, 16);

    // Max and min luminance of mastering display
    s->max_luminance = get_bits_long(gb, 32);
    s->min_luminance = get_bits_long(gb, 32);

    // As this SEI message comes before the first frame that references it,
    // initialize the flag to 2 and decrement on IRAP access unit so it
    // persists for the coded video sequence (e.g., between two IRAPs)
    s->present = 2;
    return 0;
}

static int decode_nal_sei_content_light_info(HEVCSEIContentLight *s, GetBitContext *gb)
{
    // Max and average light levels
    s->max_content_light_level     = get_bits_long(gb, 16);
    s->max_pic_average_light_level = get_bits_long(gb, 16);
    // As this SEI message comes before the first frame that references it,
    // initialize the flag to 2 and decrement on IRAP access unit so it
    // persists for the coded video sequence (e.g., between two IRAPs)
    s->present = 2;
    return  0;
}

static int decode_nal_sei_frame_packing_arrangement(HEVCSEIFramePacking *s, GetBitContext *gb)
{
    get_ue_golomb_long(gb);             // frame_packing_arrangement_id
    s->present = !get_bits1(gb);

    if (s->present) {
        s->arrangement_type               = get_bits(gb, 7);
        s->quincunx_subsampling           = get_bits1(gb);
        s->content_interpretation_type    = get_bits(gb, 6);

        // spatial_flipping_flag, frame0_flipped_flag, field_views_flag
        skip_bits(gb, 3);
        s->current_frame_is_frame0_flag = get_bits1(gb);
        // frame0_self_contained_flag, frame1_self_contained_flag
        skip_bits(gb, 2);

        if (!s->quincunx_subsampling && s->arrangement_type != 5)
            skip_bits(gb, 16);  // frame[01]_grid_position_[xy]
        skip_bits(gb, 8);       // frame_packing_arrangement_reserved_byte
        skip_bits1(gb);         // frame_packing_arrangement_persistence_flag
    }
    skip_bits1(gb);             // upsampled_aspect_ratio_flag
    return 0;
}

static int decode_nal_sei_display_orientation(HEVCSEIDisplayOrientation *s, GetBitContext *gb)
{
    s->present = !get_bits1(gb);

    if (s->present) {
        s->hflip = get_bits1(gb);     // hor_flip
        s->vflip = get_bits1(gb);     // ver_flip

        s->anticlockwise_rotation = get_bits(gb, 16);
        skip_bits1(gb);     // display_orientation_persistence_flag
    }

    return 0;
}

static int decode_nal_sei_pic_timing(HEVCSEI *s, GetBitContext *gb, const HEVCParamSets *ps,
                                     void *logctx, int size)
{
    HEVCSEIPictureTiming *h = &s->picture_timing;
    HEVCSPS *sps;

    if (!ps->sps_list[s->active_seq_parameter_set_id])
        return(AVERROR(ENOMEM));
    sps = (HEVCSPS*)ps->sps_list[s->active_seq_parameter_set_id]->data;

    if (sps->vui.frame_field_info_present_flag) {
        int pic_struct = get_bits(gb, 4);
        h->picture_struct = AV_PICTURE_STRUCTURE_UNKNOWN;
        if (pic_struct == 2 || pic_struct == 10 || pic_struct == 12) {
            av_log(logctx, AV_LOG_DEBUG, "BOTTOM Field\n");
            h->picture_struct = AV_PICTURE_STRUCTURE_BOTTOM_FIELD;
        } else if (pic_struct == 1 || pic_struct == 9 || pic_struct == 11) {
            av_log(logctx, AV_LOG_DEBUG, "TOP Field\n");
            h->picture_struct = AV_PICTURE_STRUCTURE_TOP_FIELD;
        }
        get_bits(gb, 2);                   // source_scan_type
        get_bits(gb, 1);                   // duplicate_flag
        skip_bits1(gb);
        size--;
    }
    skip_bits_long(gb, 8 * size);

    return 0;
}

static int decode_registered_user_data_closed_caption(HEVCSEIA53Caption *s, GetBitContext *gb,
                                                      int size)
{
    int flag;
    int user_data_type_code;
    int cc_count;

    if (size < 3)
       return AVERROR(EINVAL);

    user_data_type_code = get_bits(gb, 8);
    if (user_data_type_code == 0x3) {
        skip_bits(gb, 1); // reserved

        flag = get_bits(gb, 1); // process_cc_data_flag
        if (flag) {
            skip_bits(gb, 1);
            cc_count = get_bits(gb, 5);
            skip_bits(gb, 8); // reserved
            size -= 2;

            if (cc_count && size >= cc_count * 3) {
                const uint64_t new_size = (s->a53_caption_size + cc_count
                                           * UINT64_C(3));
                int i, ret;

                if (new_size > INT_MAX)
                    return AVERROR(EINVAL);

                /* Allow merging of the cc data from two fields. */
                ret = av_reallocp(&s->a53_caption, new_size);
                if (ret < 0)
                    return ret;

                for (i = 0; i < cc_count; i++) {
                    s->a53_caption[s->a53_caption_size++] = get_bits(gb, 8);
                    s->a53_caption[s->a53_caption_size++] = get_bits(gb, 8);
                    s->a53_caption[s->a53_caption_size++] = get_bits(gb, 8);
                }
                skip_bits(gb, 8); // marker_bits
            }
        }
    } else {
        int i;
        for (i = 0; i < size - 1; i++)
            skip_bits(gb, 8);
    }

    return 0;
}

static int decode_registered_user_data_dynamic_hdr_plus(AVDynamicHDRPlus *s, GetBitContext *gb,
                                                        void *logctx, int size)
{
    const int luminance_den = 10000;
    const int peak_luminance_den = 15;
    const int rgb_den = 100000;
    const int fraction_pixel_den = 1000;
    const int knee_point_den = 4095;
    const int bezier_anchor_den = 1023;
    const int saturation_weight_den = 8;

    int w, i, j;

    if (get_bits_left(gb) < 2)
        return AVERROR_INVALIDDATA;
    s->num_windows = get_bits(gb, 2);
    if (s->num_windows < 1 || s->num_windows > 3) {
        av_log(logctx, AV_LOG_ERROR, "num_windows=%d, must be in [1, 3]\n",
               s->num_windows);
        return AVERROR_INVALIDDATA;
    }

    if (get_bits_left(gb) < ((19 * 8 + 1) * (s->num_windows - 1)))
        return AVERROR_INVALIDDATA;

    for (w = 1; w < s->num_windows; w++) {
        s->params[w].window_upper_left_corner_x.num = get_bits(gb, 16);
        s->params[w].window_upper_left_corner_y.num = get_bits(gb, 16);
        s->params[w].window_lower_right_corner_x.num = get_bits(gb, 16);
        s->params[w].window_lower_right_corner_y.num = get_bits(gb, 16);
        // The corners are set to absolute coordinates here. They should be
        // converted to the relative coordinates (in [0, 1]) in the decoder.
        s->params[w].window_upper_left_corner_x.den = 1;
        s->params[w].window_upper_left_corner_y.den = 1;
        s->params[w].window_lower_right_corner_x.den = 1;
        s->params[w].window_lower_right_corner_y.den = 1;

        s->params[w].center_of_ellipse_x = get_bits(gb, 16);
        s->params[w].center_of_ellipse_y = get_bits(gb, 16);
        s->params[w].rotation_angle = get_bits(gb, 8);
        s->params[w].semimajor_axis_internal_ellipse = get_bits(gb, 16);
        s->params[w].semimajor_axis_external_ellipse = get_bits(gb, 16);
        s->params[w].semiminor_axis_external_ellipse = get_bits(gb, 16);
        s->params[w].overlap_process_option = get_bits(gb, 1);
    }

    if (get_bits_left(gb) < 28)
        return AVERROR(EINVAL);

    s->targeted_system_display_maximum_luminance.num = get_bits(gb, 27);
    s->targeted_system_display_maximum_luminance.den = luminance_den;
    s->targeted_system_display_actual_peak_luminance_flag = get_bits(gb, 1);

    if (s->targeted_system_display_actual_peak_luminance_flag) {
        int rows, cols;
        if (get_bits_left(gb) < 10)
            return AVERROR(EINVAL);
        rows = get_bits(gb, 5);
        cols = get_bits(gb, 5);
        if (((rows < 2) || (rows > 25)) || ((cols < 2) || (cols > 25))) {
            av_log(logctx, AV_LOG_ERROR, "num_rows=%d, num_cols=%d, they must [2, 25] for targeted_system_display_actual_peak_luminance\n", rows, cols);
            return AVERROR_INVALIDDATA;
        }
        s->num_rows_targeted_system_display_actual_peak_luminance = rows;
        s->num_cols_targeted_system_display_actual_peak_luminance = cols;

        if (get_bits_left(gb) < (rows * cols * 4))
            return AVERROR(EINVAL);

        for (i = 0; i < rows; i++) {
            for (j = 0; j < cols; j++) {
                s->targeted_system_display_actual_peak_luminance[i][j].num = get_bits(gb, 4);
                s->targeted_system_display_actual_peak_luminance[i][j].den = peak_luminance_den;
            }
        }
    }
    for (w = 0; w < s->num_windows; w++) {
        if (get_bits_left(gb) < (3 * 17 + 17 + 4))
            return AVERROR(EINVAL);
        for (i = 0; i < 3; i++) {
            s->params[w].maxscl[i].num = get_bits(gb, 17);
            s->params[w].maxscl[i].den = rgb_den;
        }
        s->params[w].average_maxrgb.num = get_bits(gb, 17);
        s->params[w].average_maxrgb.den = rgb_den;
        s->params[w].num_distribution_maxrgb_percentiles = get_bits(gb, 4);

        if (get_bits_left(gb) <
            (s->params[w].num_distribution_maxrgb_percentiles * 24))
            return AVERROR(EINVAL);
        for (i = 0; i < s->params[w].num_distribution_maxrgb_percentiles; i++) {
            s->params[w].distribution_maxrgb[i].percentage = get_bits(gb, 7);
            s->params[w].distribution_maxrgb[i].percentile.num = get_bits(gb, 17);
            s->params[w].distribution_maxrgb[i].percentile.den = rgb_den;
        }

        if (get_bits_left(gb) < 10)
            return AVERROR(EINVAL);
        s->params[w].fraction_bright_pixels.num = get_bits(gb, 10);
        s->params[w].fraction_bright_pixels.den = fraction_pixel_den;
    }
    if (get_bits_left(gb) < 1)
        return AVERROR(EINVAL);
    s->mastering_display_actual_peak_luminance_flag = get_bits(gb, 1);
    if (s->mastering_display_actual_peak_luminance_flag) {
        int rows, cols;
        if (get_bits_left(gb) < 10)
            return AVERROR(EINVAL);
        rows = get_bits(gb, 5);
        cols = get_bits(gb, 5);
        if (((rows < 2) || (rows > 25)) || ((cols < 2) || (cols > 25))) {
            av_log(logctx, AV_LOG_ERROR, "num_rows=%d, num_cols=%d, they must be in [2, 25] for mastering_display_actual_peak_luminance\n", rows, cols);
            return AVERROR_INVALIDDATA;
        }
        s->num_rows_mastering_display_actual_peak_luminance = rows;
        s->num_cols_mastering_display_actual_peak_luminance = cols;

        if (get_bits_left(gb) < (rows * cols * 4))
            return AVERROR(EINVAL);

        for (i = 0; i < rows; i++) {
            for (j = 0; j < cols; j++) {
                s->mastering_display_actual_peak_luminance[i][j].num = get_bits(gb, 4);
                s->mastering_display_actual_peak_luminance[i][j].den = peak_luminance_den;
            }
        }
    }

    for (w = 0; w < s->num_windows; w++) {
        if (get_bits_left(gb) < 1)
            return AVERROR(EINVAL);

        s->params[w].tone_mapping_flag = get_bits(gb, 1);
        if (s->params[w].tone_mapping_flag) {
            if (get_bits_left(gb) < 28)
                return AVERROR(EINVAL);
            s->params[w].knee_point_x.num = get_bits(gb, 12);
            s->params[w].knee_point_x.den = knee_point_den;
            s->params[w].knee_point_y.num = get_bits(gb, 12);
            s->params[w].knee_point_y.den = knee_point_den;
            s->params[w].num_bezier_curve_anchors = get_bits(gb, 4);

            if (get_bits_left(gb) < (s->params[w].num_bezier_curve_anchors * 10))
                return AVERROR(EINVAL);

            for (i = 0; i < s->params[w].num_bezier_curve_anchors; i++) {
                s->params[w].bezier_curve_anchors[i].num = get_bits(gb, 10);
                s->params[w].bezier_curve_anchors[i].den = bezier_anchor_den;
            }
        }

        if (get_bits_left(gb) < 1)
            return AVERROR(EINVAL);

        s->params[w].color_saturation_mapping_flag = get_bits(gb, 1);
        if (s->params[w].color_saturation_mapping_flag) {
            if (get_bits_left(gb) < 6)
                return AVERROR(EINVAL);
            s->params[w].color_saturation_weight.num = get_bits(gb, 6);
            s->params[w].color_saturation_weight.den = saturation_weight_den;
        }
    }

    skip_bits(gb, get_bits_left(gb));

    return 0;
}

static int decode_nal_sei_user_data_registered_itu_t_t35(HEVCSEI *s,
                                                         GetBitContext *gb,
                                                         void *logctx,
                                                         int size)
{
    uint8_t country_code;
    uint16_t provider_code;
    uint32_t user_identifier;

    if (size < 7)
        return AVERROR(EINVAL);
    size -= 7;

    country_code = get_bits(gb, 8);
    if (country_code == 0xFF) {
        skip_bits(gb, 8);
        size--;
    }

    provider_code = get_bits(gb, 16);
    user_identifier = get_bits_long(gb, 32);

    // Check for dynamic metadata - HDR10+(SMPTE 2094-40).
    if ((provider_code == 0x003C) &&
        ((user_identifier & 0xFFFFFF00) == 0x00010400)) {
        int err;
        size_t size;
        AVDynamicHDRPlus *hdr_plus = av_dynamic_hdr_plus_alloc(&size);
        if (!hdr_plus)
            return AVERROR(ENOMEM);

        if (s->dynamic_hdr_plus.info)
            av_buffer_unref(&s->dynamic_hdr_plus.info);

        s->dynamic_hdr_plus.info =
            av_buffer_create((uint8_t*)hdr_plus, size,
                             av_buffer_default_free, NULL, 0);
        if (!s->dynamic_hdr_plus.info) {
            av_freep(&hdr_plus);
            return AVERROR(ENOMEM);
        }

        hdr_plus->itu_t_t35_country_code = country_code;
        hdr_plus->application_version = (uint8_t)((user_identifier & 0x000000FF));

        err = decode_registered_user_data_dynamic_hdr_plus(hdr_plus, gb, logctx, size);
        if (err < 0)
            av_buffer_unref(&s->dynamic_hdr_plus.info);

        return err;
    }

    switch (user_identifier) {
        case MKBETAG('G', 'A', '9', '4'):
            return decode_registered_user_data_closed_caption(&s->a53_caption, gb, size);
        default:
            skip_bits_long(gb, size * 8);
            break;
    }
    return 0;
}

static int decode_nal_sei_active_parameter_sets(HEVCSEI *s, GetBitContext *gb, void *logctx)
{
    int num_sps_ids_minus1;
    int i;
    unsigned active_seq_parameter_set_id;

    get_bits(gb, 4); // active_video_parameter_set_id
    get_bits(gb, 1); // self_contained_cvs_flag
    get_bits(gb, 1); // num_sps_ids_minus1
    num_sps_ids_minus1 = get_ue_golomb_long(gb); // num_sps_ids_minus1

    if (num_sps_ids_minus1 < 0 || num_sps_ids_minus1 > 15) {
        av_log(logctx, AV_LOG_ERROR, "num_sps_ids_minus1 %d invalid\n", num_sps_ids_minus1);
        return AVERROR_INVALIDDATA;
    }

    active_seq_parameter_set_id = get_ue_golomb_long(gb);
    if (active_seq_parameter_set_id >= HEVC_MAX_SPS_COUNT) {
        av_log(logctx, AV_LOG_ERROR, "active_parameter_set_id %d invalid\n", active_seq_parameter_set_id);
        return AVERROR_INVALIDDATA;
    }
    s->active_seq_parameter_set_id = active_seq_parameter_set_id;

    for (i = 1; i <= num_sps_ids_minus1; i++)
        get_ue_golomb_long(gb); // active_seq_parameter_set_id[i]

    return 0;
}

static int decode_nal_sei_alternative_transfer(HEVCSEIAlternativeTransfer *s, GetBitContext *gb)
{
    s->present = 1;
    s->preferred_transfer_characteristics = get_bits(gb, 8);
    return 0;
}

static int decode_nal_sei_prefix(GetBitContext *gb, void *logctx, HEVCSEI *s,
                                 const HEVCParamSets *ps, int type, int size)
{
    switch (type) {
    case 256:  // Mismatched value from HM 8.1
        return decode_nal_sei_decoded_picture_hash(&s->picture_hash, gb);
    case HEVC_SEI_TYPE_FRAME_PACKING:
        return decode_nal_sei_frame_packing_arrangement(&s->frame_packing, gb);
    case HEVC_SEI_TYPE_DISPLAY_ORIENTATION:
        return decode_nal_sei_display_orientation(&s->display_orientation, gb);
    case HEVC_SEI_TYPE_PICTURE_TIMING:
        return decode_nal_sei_pic_timing(s, gb, ps, logctx, size);
    case HEVC_SEI_TYPE_MASTERING_DISPLAY_INFO:
        return decode_nal_sei_mastering_display_info(&s->mastering_display, gb);
    case HEVC_SEI_TYPE_CONTENT_LIGHT_LEVEL_INFO:
        return decode_nal_sei_content_light_info(&s->content_light, gb);
    case HEVC_SEI_TYPE_ACTIVE_PARAMETER_SETS:
        return decode_nal_sei_active_parameter_sets(s, gb, logctx);
    case HEVC_SEI_TYPE_USER_DATA_REGISTERED_ITU_T_T35:
        return decode_nal_sei_user_data_registered_itu_t_t35(s, gb, logctx, size);
    case HEVC_SEI_TYPE_ALTERNATIVE_TRANSFER_CHARACTERISTICS:
        return decode_nal_sei_alternative_transfer(&s->alternative_transfer, gb);
    default:
        av_log(logctx, AV_LOG_DEBUG, "Skipped PREFIX SEI %d\n", type);
        skip_bits_long(gb, 8 * size);
        return 0;
    }
}

static int decode_nal_sei_suffix(GetBitContext *gb, void *logctx, HEVCSEI *s,
                                 int type, int size)
{
    switch (type) {
    case HEVC_SEI_TYPE_DECODED_PICTURE_HASH:
        return decode_nal_sei_decoded_picture_hash(&s->picture_hash, gb);
    default:
        av_log(logctx, AV_LOG_DEBUG, "Skipped SUFFIX SEI %d\n", type);
        skip_bits_long(gb, 8 * size);
        return 0;
    }
}

static int decode_nal_sei_message(GetBitContext *gb, void *logctx, HEVCSEI *s,
                                  const HEVCParamSets *ps, int nal_unit_type)
{
    int payload_type = 0;
    int payload_size = 0;
    int byte = 0xFF;
    av_log(logctx, AV_LOG_DEBUG, "Decoding SEI\n");

    while (byte == 0xFF) {
        if (get_bits_left(gb) < 16 || payload_type > INT_MAX - 255)
            return AVERROR_INVALIDDATA;
        byte          = get_bits(gb, 8);
        payload_type += byte;
    }
    byte = 0xFF;
    while (byte == 0xFF) {
        if (get_bits_left(gb) < 8 + 8LL*payload_size)
            return AVERROR_INVALIDDATA;
        byte          = get_bits(gb, 8);
        payload_size += byte;
    }
    if (nal_unit_type == HEVC_NAL_SEI_PREFIX) {
        return decode_nal_sei_prefix(gb, logctx, s, ps, payload_type, payload_size);
    } else { /* nal_unit_type == NAL_SEI_SUFFIX */
        return decode_nal_sei_suffix(gb, logctx, s, payload_type, payload_size);
    }
}

static int more_rbsp_data(GetBitContext *gb)
{
    return get_bits_left(gb) > 0 && show_bits(gb, 8) != 0x80;
}

int ff_hevc_decode_nal_sei(GetBitContext *gb, void *logctx, HEVCSEI *s,
                           const HEVCParamSets *ps, int type)
{
    int ret;

    do {
        ret = decode_nal_sei_message(gb, logctx, s, ps, type);
        if (ret < 0)
            return ret;
    } while (more_rbsp_data(gb));
    return 1;
}

void ff_hevc_reset_sei(HEVCSEI *s)
{
    s->a53_caption.a53_caption_size = 0;
    av_freep(&s->a53_caption.a53_caption);
    av_buffer_unref(&s->dynamic_hdr_plus.info);
}
