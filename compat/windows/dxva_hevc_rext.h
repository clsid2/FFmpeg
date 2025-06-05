//------------------------------------------------------------------------------
// Copyright (c) 1999 - 2002, Microsoft Corporation.  All rights reserved.
//------------------------------------------------------------------------------

#ifndef __DIRECTX_VA_HEVC_REXT__
#define __DIRECTX_VA_HEVC_REXT__

#pragma pack(push, 1)

/* HEVC Picture Parameter structure for Range Extensions */
typedef struct _DXVA_PicParams_HEVC_RangeExt {
    DXVA_PicParams_HEVC params;

    union {
        struct {
            USHORT transform_skip_rotation_enabled_flag    : 1;
            USHORT transform_skip_context_enabled_flag     : 1;
            USHORT implicit_rdpcm_enabled_flag             : 1;
            USHORT explicit_rdpcm_enabled_flag             : 1;
            USHORT extended_precision_processing_flag      : 1;
            USHORT intra_smoothing_disabled_flag           : 1;
            USHORT persistent_rice_adaptation_enabled_flag : 1;
            USHORT high_precision_offsets_enabled_flag     : 1;
            USHORT cabac_bypass_alignment_enabled_flag     : 1;
            USHORT cross_component_prediction_enabled_flag : 1;
            USHORT chroma_qp_offset_list_enabled_flag      : 1;
            USHORT ReservedBits8                           : 5;
        };
        USHORT dwRangeExtensionFlags;
    };

    UCHAR diff_cu_chroma_qp_offset_depth;
    UCHAR log2_sao_offset_scale_luma;
    UCHAR log2_sao_offset_scale_chroma;
    UCHAR log2_max_transform_skip_block_size_minus2;
    CHAR cb_qp_offset_list[6];
    CHAR cr_qp_offset_list[6];
    UCHAR chroma_qp_offset_list_len_minus1;
    USHORT ReservedBits9;

} DXVA_PicParams_HEVC_RangeExt, *LPDXVA_PicParams_HEVC_RangeExt;

#pragma pack(pop)
#endif
