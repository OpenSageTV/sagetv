/*
 * H.26L/H.264/AVC/JVT/14496-10/... encoder/decoder
 * Copyright (c) 2003 Michael Niedermayer <michaelni@gmx.at>
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
 *
 */

/**
 * @file h264.c
 * H.264 / AVC / MPEG4 part10 codec.
 * @author Michael Niedermayer <michaelni@gmx.at>
 */

#include "common.h"
#include "dsputil.h"
#include "avcodec.h"
#include "mpegvideo.h"
#include "h264data.h"
#include "golomb.h"

#include "cabac.h"

//#undef NDEBUG
#include <assert.h>

#define interlaced_dct interlaced_dct_is_a_bad_name
#define mb_intra mb_intra_isnt_initalized_see_mb_type

#define LUMA_DC_BLOCK_INDEX   25
#define CHROMA_DC_BLOCK_INDEX 26

#define CHROMA_DC_COEFF_TOKEN_VLC_BITS 8
#define COEFF_TOKEN_VLC_BITS           8
#define TOTAL_ZEROS_VLC_BITS           9
#define CHROMA_DC_TOTAL_ZEROS_VLC_BITS 3
#define RUN_VLC_BITS                   3
#define RUN7_VLC_BITS                  6

#define MAX_SPS_COUNT 32
#define MAX_PPS_COUNT 256

#define MAX_MMCO_COUNT 66

/* Compiling in interlaced support reduces the speed
 * of progressive decoding by about 2%. */
#define ALLOW_INTERLACE

#ifdef ALLOW_INTERLACE
#define MB_MBAFF h->mb_mbaff
#define MB_FIELD h->mb_field_decoding_flag
#define FRAME_MBAFF h->mb_aff_frame
#else
#define MB_MBAFF 0
#define MB_FIELD 0
#define FRAME_MBAFF 0
#undef  IS_INTERLACED
#define IS_INTERLACED(mb_type) 0
#endif

/**
 * Sequence parameter set
 */
typedef struct SPS{

    int profile_idc;
    int level_idc;
    int transform_bypass;              ///< qpprime_y_zero_transform_bypass_flag
    int log2_max_frame_num;            ///< log2_max_frame_num_minus4 + 4
    int poc_type;                      ///< pic_order_cnt_type
    int log2_max_poc_lsb;              ///< log2_max_pic_order_cnt_lsb_minus4
    int delta_pic_order_always_zero_flag;
    int offset_for_non_ref_pic;
    int offset_for_top_to_bottom_field;
    int poc_cycle_length;              ///< num_ref_frames_in_pic_order_cnt_cycle
    int ref_frame_count;               ///< num_ref_frames
    int gaps_in_frame_num_allowed_flag;
    int mb_width;                      ///< frame_width_in_mbs_minus1 + 1
    int mb_height;                     ///< frame_height_in_mbs_minus1 + 1
    int frame_mbs_only_flag;
    int mb_aff;                        ///<mb_adaptive_frame_field_flag
    int direct_8x8_inference_flag;
    int crop;                   ///< frame_cropping_flag
    int crop_left;              ///< frame_cropping_rect_left_offset
    int crop_right;             ///< frame_cropping_rect_right_offset
    int crop_top;               ///< frame_cropping_rect_top_offset
    int crop_bottom;            ///< frame_cropping_rect_bottom_offset
    int vui_parameters_present_flag;
    AVRational sar;
    int timing_info_present_flag;
    uint32_t num_units_in_tick;
    uint32_t time_scale;
    int fixed_frame_rate_flag;
    short offset_for_ref_frame[256]; //FIXME dyn aloc?
    int bitstream_restriction_flag;
    int num_reorder_frames;
    int scaling_matrix_present;
    uint8_t scaling_matrix4[6][16];
    uint8_t scaling_matrix8[2][64];
}SPS;

/**
 * Picture parameter set
 */
typedef struct PPS{
    unsigned int sps_id;
    int cabac;                  ///< entropy_coding_mode_flag
    int pic_order_present;      ///< pic_order_present_flag
    int slice_group_count;      ///< num_slice_groups_minus1 + 1
    int mb_slice_group_map_type;
    unsigned int ref_count[2];  ///< num_ref_idx_l0/1_active_minus1 + 1
    int weighted_pred;          ///< weighted_pred_flag
    int weighted_bipred_idc;
    int init_qp;                ///< pic_init_qp_minus26 + 26
    int init_qs;                ///< pic_init_qs_minus26 + 26
    int chroma_qp_index_offset;
    int deblocking_filter_parameters_present; ///< deblocking_filter_parameters_present_flag
    int constrained_intra_pred; ///< constrained_intra_pred_flag
    int redundant_pic_cnt_present; ///< redundant_pic_cnt_present_flag
    int transform_8x8_mode;     ///< transform_8x8_mode_flag
    uint8_t scaling_matrix4[6][16];
    uint8_t scaling_matrix8[2][64];
}PPS;

/**
 * Memory management control operation opcode.
 */
typedef enum MMCOOpcode{
    MMCO_END=0,
    MMCO_SHORT2UNUSED,
    MMCO_LONG2UNUSED,
    MMCO_SHORT2LONG,
    MMCO_SET_MAX_LONG,
    MMCO_RESET,
    MMCO_LONG,
} MMCOOpcode;

/**
 * Memory management control operation.
 */
typedef struct MMCO{
    MMCOOpcode opcode;
    int short_frame_num;
    int long_index;
} MMCO;

/**
 * H264Context
 */
typedef struct H264Context{
    MpegEncContext s;
    int nal_ref_idc;
    int nal_unit_type;
    uint8_t *rbsp_buffer;
    unsigned int rbsp_buffer_size;

    /**
      * Used to parse AVC variant of h264
      */
    int is_avc; ///< this flag is != 0 if codec is avc1
    int got_avcC; ///< flag used to parse avcC data only once
    int nal_length_size; ///< Number of bytes used for nal length (1, 2 or 4)

    int chroma_qp; //QPc

    int prev_mb_skipped;
    int next_mb_skipped;

    //prediction stuff
    int chroma_pred_mode;
    int intra16x16_pred_mode;

    int top_mb_xy;
    int left_mb_xy[2];

    int8_t intra4x4_pred_mode_cache[5*8];
    int8_t (*intra4x4_pred_mode)[8];
    void (*pred4x4  [9+3])(uint8_t *src, uint8_t *topright, int stride);//FIXME move to dsp?
    void (*pred8x8l [9+3])(uint8_t *src, int topleft, int topright, int stride);
    void (*pred8x8  [4+3])(uint8_t *src, int stride);
    void (*pred16x16[4+3])(uint8_t *src, int stride);
    unsigned int topleft_samples_available;
    unsigned int top_samples_available;
    unsigned int topright_samples_available;
    unsigned int left_samples_available;
    uint8_t (*top_borders[2])[16+2*8];
    uint8_t left_border[2*(17+2*9)];

    /**
     * non zero coeff count cache.
     * is 64 if not available.
     */
    DECLARE_ALIGNED_8(uint8_t, non_zero_count_cache[6*8]);
    uint8_t (*non_zero_count)[16];

    /**
     * Motion vector cache.
     */
    DECLARE_ALIGNED_8(int16_t, mv_cache[2][5*8][2]);
    DECLARE_ALIGNED_8(int8_t, ref_cache[2][5*8]);
#define LIST_NOT_USED -1 //FIXME rename?
#define PART_NOT_AVAILABLE -2

    /**
     * is 1 if the specific list MV&references are set to 0,0,-2.
     */
    int mv_cache_clean[2];

    /**
     * number of neighbors (top and/or left) that used 8x8 dct
     */
    int neighbor_transform_size;

    /**
     * block_offset[ 0..23] for frame macroblocks
     * block_offset[24..47] for field macroblocks
     */
    int block_offset[2*(16+8)];

    uint32_t *mb2b_xy; //FIXME are these 4 a good idea?
    uint32_t *mb2b8_xy;
    int b_stride; //FIXME use s->b4_stride
    int b8_stride;

    int mb_linesize;   ///< may be equal to s->linesize or s->linesize*2, for mbaff
    int mb_uvlinesize;

    int emu_edge_width;
    int emu_edge_height;

    int halfpel_flag;
    int thirdpel_flag;

    int unknown_svq3_flag;
    int next_slice_index;

    SPS sps_buffer[MAX_SPS_COUNT];
    SPS sps; ///< current sps

    PPS pps_buffer[MAX_PPS_COUNT];
    /**
     * current pps
     */
    PPS pps; //FIXME move to Picture perhaps? (->no) do we need that?

    uint32_t dequant4_buffer[6][52][16];
    uint32_t dequant8_buffer[2][52][64];
    uint32_t (*dequant4_coeff[6])[16];
    uint32_t (*dequant8_coeff[2])[64];
    int dequant_coeff_pps;     ///< reinit tables when pps changes

    int slice_num;
    uint8_t *slice_table_base;
    uint8_t *slice_table;      ///< slice_table_base + 2*mb_stride + 1
    int slice_type;
    int slice_type_fixed;

    //interlacing specific flags
    int mb_aff_frame;
    int mb_field_decoding_flag;
    int mb_mbaff;              ///< mb_aff_frame && mb_field_decoding_flag

    unsigned int sub_mb_type[4];

    //POC stuff
    int poc_lsb;
    int poc_msb;
    int delta_poc_bottom;
    int delta_poc[2];
    int frame_num;
    int prev_poc_msb;             ///< poc_msb of the last reference pic for POC type 0
    int prev_poc_lsb;             ///< poc_lsb of the last reference pic for POC type 0
    int frame_num_offset;         ///< for POC type 2
    int prev_frame_num_offset;    ///< for POC type 2
    int prev_frame_num;           ///< frame_num of the last pic for POC type 1/2

    /**
     * frame_num for frames or 2*frame_num for field pics.
     */
    int curr_pic_num;

    /**
     * max_frame_num or 2*max_frame_num for field pics.
     */
    int max_pic_num;

    //Weighted pred stuff
    int use_weight;
    int use_weight_chroma;
    int luma_log2_weight_denom;
    int chroma_log2_weight_denom;
    int luma_weight[2][48];
    int luma_offset[2][48];
    int chroma_weight[2][48][2];
    int chroma_offset[2][48][2];
    int implicit_weight[48][48];

    //deblock
    int deblocking_filter;         ///< disable_deblocking_filter_idc with 1<->0
    int slice_alpha_c0_offset;
    int slice_beta_offset;

    int redundant_pic_count;

    int direct_spatial_mv_pred;
    int dist_scale_factor[16];
    int dist_scale_factor_field[32];
    int map_col_to_list0[2][16];
    int map_col_to_list0_field[2][32];

    /**
     * num_ref_idx_l0/1_active_minus1 + 1
     */
    unsigned int ref_count[2];   ///< counts frames or fields, depending on current mb mode
    unsigned int list_count;
    Picture *short_ref[32];
    Picture *long_ref[32];
    Picture default_ref_list[2][32];
    Picture ref_list[2][48];     ///< 0..15: frame refs, 16..47: mbaff field refs
    Picture *delayed_pic[18]; //FIXME size?
    Picture *delayed_output_pic;

    /**
     * memory management control operations buffer.
     */
    MMCO mmco[MAX_MMCO_COUNT];
    int mmco_index;

    int long_ref_count;  ///< number of actual long term references
    int short_ref_count; ///< number of actual short term references

    //data partitioning
    GetBitContext intra_gb;
    GetBitContext inter_gb;
    GetBitContext *intra_gb_ptr;
    GetBitContext *inter_gb_ptr;

    DECLARE_ALIGNED_8(DCTELEM, mb[16*24]);
    DCTELEM mb_padding[256];        ///< as mb is addressed by scantable[i] and scantable is uint8_t we can either check that i is not to large or ensure that there is some unused stuff after mb

    /**
     * Cabac
     */
    CABACContext cabac;
    uint8_t      cabac_state[460];
    int          cabac_init_idc;

    /* 0x100 -> non null luma_dc, 0x80/0x40 -> non null chroma_dc (cb/cr), 0x?0 -> chroma_cbp(0,1,2), 0x0? luma_cbp */
    uint16_t     *cbp_table;
    int cbp;
    int top_cbp;
    int left_cbp;
    /* chroma_pred_mode for i4x4 or i16x16, else 0 */
    uint8_t     *chroma_pred_mode_table;
    int         last_qscale_diff;
    int16_t     (*mvd_table[2])[2];
    DECLARE_ALIGNED_8(int16_t, mvd_cache[2][5*8][2]);
    uint8_t     *direct_table;
    uint8_t     direct_cache[5*8];

    uint8_t zigzag_scan[16];
    uint8_t zigzag_scan8x8[64];
    uint8_t zigzag_scan8x8_cavlc[64];
    uint8_t field_scan[16];
    uint8_t field_scan8x8[64];
    uint8_t field_scan8x8_cavlc[64];
    const uint8_t *zigzag_scan_q0;
    const uint8_t *zigzag_scan8x8_q0;
    const uint8_t *zigzag_scan8x8_cavlc_q0;
    const uint8_t *field_scan_q0;
    const uint8_t *field_scan8x8_q0;
    const uint8_t *field_scan8x8_cavlc_q0;

    int x264_build;
}H264Context;

static VLC coeff_token_vlc[4];
static VLC chroma_dc_coeff_token_vlc;

static VLC total_zeros_vlc[15];
static VLC chroma_dc_total_zeros_vlc[3];

static VLC run_vlc[6];
static VLC run7_vlc;

static void svq3_luma_dc_dequant_idct_c(DCTELEM *block, int qp);
static void svq3_add_idct_c(uint8_t *dst, DCTELEM *block, int stride, int qp, int dc);
static void filter_mb( H264Context *h, int mb_x, int mb_y, uint8_t *img_y, uint8_t *img_cb, uint8_t *img_cr, unsigned int linesize, unsigned int uvlinesize);
static void filter_mb_fast( H264Context *h, int mb_x, int mb_y, uint8_t *img_y, uint8_t *img_cb, uint8_t *img_cr, unsigned int linesize, unsigned int uvlinesize);

static av_always_inline uint32_t pack16to32(int a, int b){
#ifdef WORDS_BIGENDIAN
   return (b&0xFFFF) + (a<<16);
#else
   return (a&0xFFFF) + (b<<16);
#endif
}

const uint8_t ff_rem6[52]={
0, 1, 2, 3, 4, 5, 0, 1, 2, 3, 4, 5, 0, 1, 2, 3, 4, 5, 0, 1, 2, 3, 4, 5, 0, 1, 2, 3, 4, 5, 0, 1, 2, 3, 4, 5, 0, 1, 2, 3, 4, 5, 0, 1, 2, 3, 4, 5, 0, 1, 2, 3,
};

const uint8_t ff_div6[52]={
0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5, 5, 5, 6, 6, 6, 6, 6, 6, 7, 7, 7, 7, 7, 7, 8, 8, 8, 8,
};


/**
 * fill a rectangle.
 * @param h height of the rectangle, should be a constant
 * @param w width of the rectangle, should be a constant
 * @param size the size of val (1 or 4), should be a constant
 */
static av_always_inline void fill_rectangle(void *vp, int w, int h, int stride, uint32_t val, int size){
    uint8_t *p= (uint8_t*)vp;
    assert(size==1 || size==4);
    assert(w<=4);

    w      *= size;
    stride *= size;

    assert((((long)vp)&(FFMIN(w, STRIDE_ALIGN)-1)) == 0);
    assert((stride&(w-1))==0);
    if(w==2){
        const uint16_t v= size==4 ? val : val*0x0101;
        *(uint16_t*)(p + 0*stride)= v;
        if(h==1) return;
        *(uint16_t*)(p + 1*stride)= v;
        if(h==2) return;
        *(uint16_t*)(p + 2*stride)=
        *(uint16_t*)(p + 3*stride)= v;
    }else if(w==4){
        const uint32_t v= size==4 ? val : val*0x01010101;
        *(uint32_t*)(p + 0*stride)= v;
        if(h==1) return;
        *(uint32_t*)(p + 1*stride)= v;
        if(h==2) return;
        *(uint32_t*)(p + 2*stride)=
        *(uint32_t*)(p + 3*stride)= v;
    }else if(w==8){
    //gcc can't optimize 64bit math on x86_32
#if defined(ARCH_X86_64) || (defined(MP_WORDSIZE) && MP_WORDSIZE >= 64)
        const uint64_t v= val*0x0100000001ULL;
        *(uint64_t*)(p + 0*stride)= v;
        if(h==1) return;
        *(uint64_t*)(p + 1*stride)= v;
        if(h==2) return;
        *(uint64_t*)(p + 2*stride)=
        *(uint64_t*)(p + 3*stride)= v;
    }else if(w==16){
        const uint64_t v= val*0x0100000001ULL;
        *(uint64_t*)(p + 0+0*stride)=
        *(uint64_t*)(p + 8+0*stride)=
        *(uint64_t*)(p + 0+1*stride)=
        *(uint64_t*)(p + 8+1*stride)= v;
        if(h==2) return;
        *(uint64_t*)(p + 0+2*stride)=
        *(uint64_t*)(p + 8+2*stride)=
        *(uint64_t*)(p + 0+3*stride)=
        *(uint64_t*)(p + 8+3*stride)= v;
#else
        *(uint32_t*)(p + 0+0*stride)=
        *(uint32_t*)(p + 4+0*stride)= val;
        if(h==1) return;
        *(uint32_t*)(p + 0+1*stride)=
        *(uint32_t*)(p + 4+1*stride)= val;
        if(h==2) return;
        *(uint32_t*)(p + 0+2*stride)=
        *(uint32_t*)(p + 4+2*stride)=
        *(uint32_t*)(p + 0+3*stride)=
        *(uint32_t*)(p + 4+3*stride)= val;
    }else if(w==16){
        *(uint32_t*)(p + 0+0*stride)=
        *(uint32_t*)(p + 4+0*stride)=
        *(uint32_t*)(p + 8+0*stride)=
        *(uint32_t*)(p +12+0*stride)=
        *(uint32_t*)(p + 0+1*stride)=
        *(uint32_t*)(p + 4+1*stride)=
        *(uint32_t*)(p + 8+1*stride)=
        *(uint32_t*)(p +12+1*stride)= val;
        if(h==2) return;
        *(uint32_t*)(p + 0+2*stride)=
        *(uint32_t*)(p + 4+2*stride)=
        *(uint32_t*)(p + 8+2*stride)=
        *(uint32_t*)(p +12+2*stride)=
        *(uint32_t*)(p + 0+3*stride)=
        *(uint32_t*)(p + 4+3*stride)=
        *(uint32_t*)(p + 8+3*stride)=
        *(uint32_t*)(p +12+3*stride)= val;
#endif
    }else
        assert(0);
    assert(h==4);
}

static void fill_caches(H264Context *h, int mb_type, int for_deblock){
    MpegEncContext * const s = &h->s;
    const int mb_xy= s->mb_x + s->mb_y*s->mb_stride;
    int topleft_xy, top_xy, topright_xy, left_xy[2];
    int topleft_type, top_type, topright_type, left_type[2];
    int left_block[8];
    int i;

    //FIXME deblocking could skip the intra and nnz parts.
    if(for_deblock && (h->slice_num == 1 || h->slice_table[mb_xy] == h->slice_table[mb_xy-s->mb_stride]) && !FRAME_MBAFF)
        return;

    //wow what a mess, why didn't they simplify the interlacing&intra stuff, i can't imagine that these complex rules are worth it

    top_xy     = mb_xy  - s->mb_stride;
    topleft_xy = top_xy - 1;
    topright_xy= top_xy + 1;
    left_xy[1] = left_xy[0] = mb_xy-1;
    left_block[0]= 0;
    left_block[1]= 1;
    left_block[2]= 2;
    left_block[3]= 3;
    left_block[4]= 7;
    left_block[5]= 10;
    left_block[6]= 8;
    left_block[7]= 11;
    if(FRAME_MBAFF){
        const int pair_xy          = s->mb_x     + (s->mb_y & ~1)*s->mb_stride;
        const int top_pair_xy      = pair_xy     - s->mb_stride;
        const int topleft_pair_xy  = top_pair_xy - 1;
        const int topright_pair_xy = top_pair_xy + 1;
        const int topleft_mb_frame_flag  = !IS_INTERLACED(s->current_picture.mb_type[topleft_pair_xy]);
        const int top_mb_frame_flag      = !IS_INTERLACED(s->current_picture.mb_type[top_pair_xy]);
        const int topright_mb_frame_flag = !IS_INTERLACED(s->current_picture.mb_type[topright_pair_xy]);
        const int left_mb_frame_flag = !IS_INTERLACED(s->current_picture.mb_type[pair_xy-1]);
        const int curr_mb_frame_flag = !IS_INTERLACED(mb_type);
        const int bottom = (s->mb_y & 1);
        tprintf(s->avctx, "fill_caches: curr_mb_frame_flag:%d, left_mb_frame_flag:%d, topleft_mb_frame_flag:%d, top_mb_frame_flag:%d, topright_mb_frame_flag:%d\n", curr_mb_frame_flag, left_mb_frame_flag, topleft_mb_frame_flag, top_mb_frame_flag, topright_mb_frame_flag);
        if (bottom
                ? !curr_mb_frame_flag // bottom macroblock
                : (!curr_mb_frame_flag && !top_mb_frame_flag) // top macroblock
                ) {
            top_xy -= s->mb_stride;
        }
        if (bottom
                ? !curr_mb_frame_flag // bottom macroblock
                : (!curr_mb_frame_flag && !topleft_mb_frame_flag) // top macroblock
                ) {
            topleft_xy -= s->mb_stride;
        }
        if (bottom
                ? !curr_mb_frame_flag // bottom macroblock
                : (!curr_mb_frame_flag && !topright_mb_frame_flag) // top macroblock
                ) {
            topright_xy -= s->mb_stride;
        }
        if (left_mb_frame_flag != curr_mb_frame_flag) {
            left_xy[1] = left_xy[0] = pair_xy - 1;
            if (curr_mb_frame_flag) {
                if (bottom) {
                    left_block[0]= 2;
                    left_block[1]= 2;
                    left_block[2]= 3;
                    left_block[3]= 3;
                    left_block[4]= 8;
                    left_block[5]= 11;
                    left_block[6]= 8;
                    left_block[7]= 11;
                } else {
                    left_block[0]= 0;
                    left_block[1]= 0;
                    left_block[2]= 1;
                    left_block[3]= 1;
                    left_block[4]= 7;
                    left_block[5]= 10;
                    left_block[6]= 7;
                    left_block[7]= 10;
                }
            } else {
                left_xy[1] += s->mb_stride;
                //left_block[0]= 0;
                left_block[1]= 2;
                left_block[2]= 0;
                left_block[3]= 2;
                //left_block[4]= 7;
                left_block[5]= 10;
                left_block[6]= 7;
                left_block[7]= 10;
            }
        }
    }

    h->top_mb_xy = top_xy;
    h->left_mb_xy[0] = left_xy[0];
    h->left_mb_xy[1] = left_xy[1];
    if(for_deblock){
        topleft_type = 0;
        topright_type = 0;
        top_type     = h->slice_table[top_xy     ] < 255 ? s->current_picture.mb_type[top_xy]     : 0;
        left_type[0] = h->slice_table[left_xy[0] ] < 255 ? s->current_picture.mb_type[left_xy[0]] : 0;
        left_type[1] = h->slice_table[left_xy[1] ] < 255 ? s->current_picture.mb_type[left_xy[1]] : 0;

        if(FRAME_MBAFF && !IS_INTRA(mb_type)){
            int list;
            int v = *(uint16_t*)&h->non_zero_count[mb_xy][14];
            for(i=0; i<16; i++)
                h->non_zero_count_cache[scan8[i]] = (v>>i)&1;
            for(list=0; list<h->list_count; list++){
                if(USES_LIST(mb_type,list)){
                    uint32_t *src = (uint32_t*)s->current_picture.motion_val[list][h->mb2b_xy[mb_xy]];
                    uint32_t *dst = (uint32_t*)h->mv_cache[list][scan8[0]];
                    int8_t *ref = &s->current_picture.ref_index[list][h->mb2b8_xy[mb_xy]];
                    for(i=0; i<4; i++, dst+=8, src+=h->b_stride){
                        dst[0] = src[0];
                        dst[1] = src[1];
                        dst[2] = src[2];
                        dst[3] = src[3];
                    }
                    *(uint32_t*)&h->ref_cache[list][scan8[ 0]] =
                    *(uint32_t*)&h->ref_cache[list][scan8[ 2]] = pack16to32(ref[0],ref[1])*0x0101;
                    ref += h->b8_stride;
                    *(uint32_t*)&h->ref_cache[list][scan8[ 8]] =
                    *(uint32_t*)&h->ref_cache[list][scan8[10]] = pack16to32(ref[0],ref[1])*0x0101;
                }else{
                    fill_rectangle(&h-> mv_cache[list][scan8[ 0]], 4, 4, 8, 0, 4);
                    fill_rectangle(&h->ref_cache[list][scan8[ 0]], 4, 4, 8, (uint8_t)LIST_NOT_USED, 1);
                }
            }
        }
    }else{
        topleft_type = h->slice_table[topleft_xy ] == h->slice_num ? s->current_picture.mb_type[topleft_xy] : 0;
        top_type     = h->slice_table[top_xy     ] == h->slice_num ? s->current_picture.mb_type[top_xy]     : 0;
        topright_type= h->slice_table[topright_xy] == h->slice_num ? s->current_picture.mb_type[topright_xy]: 0;
        left_type[0] = h->slice_table[left_xy[0] ] == h->slice_num ? s->current_picture.mb_type[left_xy[0]] : 0;
        left_type[1] = h->slice_table[left_xy[1] ] == h->slice_num ? s->current_picture.mb_type[left_xy[1]] : 0;
    }

    if(IS_INTRA(mb_type)){
        h->topleft_samples_available=
        h->top_samples_available=
        h->left_samples_available= 0xFFFF;
        h->topright_samples_available= 0xEEEA;

        if(!IS_INTRA(top_type) && (top_type==0 || h->pps.constrained_intra_pred)){
            h->topleft_samples_available= 0xB3FF;
            h->top_samples_available= 0x33FF;
            h->topright_samples_available= 0x26EA;
        }
        for(i=0; i<2; i++){
            if(!IS_INTRA(left_type[i]) && (left_type[i]==0 || h->pps.constrained_intra_pred)){
                h->topleft_samples_available&= 0xDF5F;
                h->left_samples_available&= 0x5F5F;
            }
        }

        if(!IS_INTRA(topleft_type) && (topleft_type==0 || h->pps.constrained_intra_pred))
            h->topleft_samples_available&= 0x7FFF;

        if(!IS_INTRA(topright_type) && (topright_type==0 || h->pps.constrained_intra_pred))
            h->topright_samples_available&= 0xFBFF;

        if(IS_INTRA4x4(mb_type)){
            if(IS_INTRA4x4(top_type)){
                h->intra4x4_pred_mode_cache[4+8*0]= h->intra4x4_pred_mode[top_xy][4];
                h->intra4x4_pred_mode_cache[5+8*0]= h->intra4x4_pred_mode[top_xy][5];
                h->intra4x4_pred_mode_cache[6+8*0]= h->intra4x4_pred_mode[top_xy][6];
                h->intra4x4_pred_mode_cache[7+8*0]= h->intra4x4_pred_mode[top_xy][3];
            }else{
                int pred;
                if(!top_type || (IS_INTER(top_type) && h->pps.constrained_intra_pred))
                    pred= -1;
                else{
                    pred= 2;
                }
                h->intra4x4_pred_mode_cache[4+8*0]=
                h->intra4x4_pred_mode_cache[5+8*0]=
                h->intra4x4_pred_mode_cache[6+8*0]=
                h->intra4x4_pred_mode_cache[7+8*0]= pred;
            }
            for(i=0; i<2; i++){
                if(IS_INTRA4x4(left_type[i])){
                    h->intra4x4_pred_mode_cache[3+8*1 + 2*8*i]= h->intra4x4_pred_mode[left_xy[i]][left_block[0+2*i]];
                    h->intra4x4_pred_mode_cache[3+8*2 + 2*8*i]= h->intra4x4_pred_mode[left_xy[i]][left_block[1+2*i]];
                }else{
                    int pred;
                    if(!left_type[i] || (IS_INTER(left_type[i]) && h->pps.constrained_intra_pred))
                        pred= -1;
                    else{
                        pred= 2;
                    }
                    h->intra4x4_pred_mode_cache[3+8*1 + 2*8*i]=
                    h->intra4x4_pred_mode_cache[3+8*2 + 2*8*i]= pred;
                }
            }
        }
    }


/*
0 . T T. T T T T
1 L . .L . . . .
2 L . .L . . . .
3 . T TL . . . .
4 L . .L . . . .
5 L . .. . . . .
*/
//FIXME constraint_intra_pred & partitioning & nnz (lets hope this is just a typo in the spec)
    if(top_type){
        h->non_zero_count_cache[4+8*0]= h->non_zero_count[top_xy][4];
        h->non_zero_count_cache[5+8*0]= h->non_zero_count[top_xy][5];
        h->non_zero_count_cache[6+8*0]= h->non_zero_count[top_xy][6];
        h->non_zero_count_cache[7+8*0]= h->non_zero_count[top_xy][3];

        h->non_zero_count_cache[1+8*0]= h->non_zero_count[top_xy][9];
        h->non_zero_count_cache[2+8*0]= h->non_zero_count[top_xy][8];

        h->non_zero_count_cache[1+8*3]= h->non_zero_count[top_xy][12];
        h->non_zero_count_cache[2+8*3]= h->non_zero_count[top_xy][11];

    }else{
        h->non_zero_count_cache[4+8*0]=
        h->non_zero_count_cache[5+8*0]=
        h->non_zero_count_cache[6+8*0]=
        h->non_zero_count_cache[7+8*0]=

        h->non_zero_count_cache[1+8*0]=
        h->non_zero_count_cache[2+8*0]=

        h->non_zero_count_cache[1+8*3]=
        h->non_zero_count_cache[2+8*3]= h->pps.cabac && !IS_INTRA(mb_type) ? 0 : 64;

    }

    for (i=0; i<2; i++) {
        if(left_type[i]){
            h->non_zero_count_cache[3+8*1 + 2*8*i]= h->non_zero_count[left_xy[i]][left_block[0+2*i]];
            h->non_zero_count_cache[3+8*2 + 2*8*i]= h->non_zero_count[left_xy[i]][left_block[1+2*i]];
            h->non_zero_count_cache[0+8*1 +   8*i]= h->non_zero_count[left_xy[i]][left_block[4+2*i]];
            h->non_zero_count_cache[0+8*4 +   8*i]= h->non_zero_count[left_xy[i]][left_block[5+2*i]];
        }else{
            h->non_zero_count_cache[3+8*1 + 2*8*i]=
            h->non_zero_count_cache[3+8*2 + 2*8*i]=
            h->non_zero_count_cache[0+8*1 +   8*i]=
            h->non_zero_count_cache[0+8*4 +   8*i]= h->pps.cabac && !IS_INTRA(mb_type) ? 0 : 64;
        }
    }

    if( h->pps.cabac ) {
        // top_cbp
        if(top_type) {
            h->top_cbp = h->cbp_table[top_xy];
        } else if(IS_INTRA(mb_type)) {
            h->top_cbp = 0x1C0;
        } else {
            h->top_cbp = 0;
        }
        // left_cbp
        if (left_type[0]) {
            h->left_cbp = h->cbp_table[left_xy[0]] & 0x1f0;
        } else if(IS_INTRA(mb_type)) {
            h->left_cbp = 0x1C0;
        } else {
            h->left_cbp = 0;
        }
        if (left_type[0]) {
            h->left_cbp |= ((h->cbp_table[left_xy[0]]>>((left_block[0]&(~1))+1))&0x1) << 1;
        }
        if (left_type[1]) {
            h->left_cbp |= ((h->cbp_table[left_xy[1]]>>((left_block[2]&(~1))+1))&0x1) << 3;
        }
    }

#if 1
    if(IS_INTER(mb_type) || IS_DIRECT(mb_type)){
        int list;
        for(list=0; list<h->list_count; list++){
            if(!USES_LIST(mb_type, list) && !IS_DIRECT(mb_type) && !h->deblocking_filter){
                /*if(!h->mv_cache_clean[list]){
                    memset(h->mv_cache [list],  0, 8*5*2*sizeof(int16_t)); //FIXME clean only input? clean at all?
                    memset(h->ref_cache[list], PART_NOT_AVAILABLE, 8*5*sizeof(int8_t));
                    h->mv_cache_clean[list]= 1;
                }*/
                continue;
            }
            h->mv_cache_clean[list]= 0;

            if(USES_LIST(top_type, list)){
                const int b_xy= h->mb2b_xy[top_xy] + 3*h->b_stride;
                const int b8_xy= h->mb2b8_xy[top_xy] + h->b8_stride;
                *(uint32_t*)h->mv_cache[list][scan8[0] + 0 - 1*8]= *(uint32_t*)s->current_picture.motion_val[list][b_xy + 0];
                *(uint32_t*)h->mv_cache[list][scan8[0] + 1 - 1*8]= *(uint32_t*)s->current_picture.motion_val[list][b_xy + 1];
                *(uint32_t*)h->mv_cache[list][scan8[0] + 2 - 1*8]= *(uint32_t*)s->current_picture.motion_val[list][b_xy + 2];
                *(uint32_t*)h->mv_cache[list][scan8[0] + 3 - 1*8]= *(uint32_t*)s->current_picture.motion_val[list][b_xy + 3];
                h->ref_cache[list][scan8[0] + 0 - 1*8]=
                h->ref_cache[list][scan8[0] + 1 - 1*8]= s->current_picture.ref_index[list][b8_xy + 0];
                h->ref_cache[list][scan8[0] + 2 - 1*8]=
                h->ref_cache[list][scan8[0] + 3 - 1*8]= s->current_picture.ref_index[list][b8_xy + 1];
            }else{
                *(uint32_t*)h->mv_cache [list][scan8[0] + 0 - 1*8]=
                *(uint32_t*)h->mv_cache [list][scan8[0] + 1 - 1*8]=
                *(uint32_t*)h->mv_cache [list][scan8[0] + 2 - 1*8]=
                *(uint32_t*)h->mv_cache [list][scan8[0] + 3 - 1*8]= 0;
                *(uint32_t*)&h->ref_cache[list][scan8[0] + 0 - 1*8]= ((top_type ? LIST_NOT_USED : PART_NOT_AVAILABLE)&0xFF)*0x01010101;
            }

            for(i=0; i<2; i++){
                int cache_idx = scan8[0] - 1 + i*2*8;
                if(USES_LIST(left_type[i], list)){
                    const int b_xy= h->mb2b_xy[left_xy[i]] + 3;
                    const int b8_xy= h->mb2b8_xy[left_xy[i]] + 1;
                    *(uint32_t*)h->mv_cache[list][cache_idx  ]= *(uint32_t*)s->current_picture.motion_val[list][b_xy + h->b_stride*left_block[0+i*2]];
                    *(uint32_t*)h->mv_cache[list][cache_idx+8]= *(uint32_t*)s->current_picture.motion_val[list][b_xy + h->b_stride*left_block[1+i*2]];
                    h->ref_cache[list][cache_idx  ]= s->current_picture.ref_index[list][b8_xy + h->b8_stride*(left_block[0+i*2]>>1)];
                    h->ref_cache[list][cache_idx+8]= s->current_picture.ref_index[list][b8_xy + h->b8_stride*(left_block[1+i*2]>>1)];
                }else{
                    *(uint32_t*)h->mv_cache [list][cache_idx  ]=
                    *(uint32_t*)h->mv_cache [list][cache_idx+8]= 0;
                    h->ref_cache[list][cache_idx  ]=
                    h->ref_cache[list][cache_idx+8]= left_type[i] ? LIST_NOT_USED : PART_NOT_AVAILABLE;
                }
            }

            if((for_deblock || (IS_DIRECT(mb_type) && !h->direct_spatial_mv_pred)) && !FRAME_MBAFF)
                continue;

            if(USES_LIST(topleft_type, list)){
                const int b_xy = h->mb2b_xy[topleft_xy] + 3 + 3*h->b_stride;
                const int b8_xy= h->mb2b8_xy[topleft_xy] + 1 + h->b8_stride;
                *(uint32_t*)h->mv_cache[list][scan8[0] - 1 - 1*8]= *(uint32_t*)s->current_picture.motion_val[list][b_xy];
                h->ref_cache[list][scan8[0] - 1 - 1*8]= s->current_picture.ref_index[list][b8_xy];
            }else{
                *(uint32_t*)h->mv_cache[list][scan8[0] - 1 - 1*8]= 0;
                h->ref_cache[list][scan8[0] - 1 - 1*8]= topleft_type ? LIST_NOT_USED : PART_NOT_AVAILABLE;
            }

            if(USES_LIST(topright_type, list)){
                const int b_xy= h->mb2b_xy[topright_xy] + 3*h->b_stride;
                const int b8_xy= h->mb2b8_xy[topright_xy] + h->b8_stride;
                *(uint32_t*)h->mv_cache[list][scan8[0] + 4 - 1*8]= *(uint32_t*)s->current_picture.motion_val[list][b_xy];
                h->ref_cache[list][scan8[0] + 4 - 1*8]= s->current_picture.ref_index[list][b8_xy];
            }else{
                *(uint32_t*)h->mv_cache [list][scan8[0] + 4 - 1*8]= 0;
                h->ref_cache[list][scan8[0] + 4 - 1*8]= topright_type ? LIST_NOT_USED : PART_NOT_AVAILABLE;
            }

            if((IS_SKIP(mb_type) || IS_DIRECT(mb_type)) && !FRAME_MBAFF)
                continue;

            h->ref_cache[list][scan8[5 ]+1] =
            h->ref_cache[list][scan8[7 ]+1] =
            h->ref_cache[list][scan8[13]+1] =  //FIXME remove past 3 (init somewhere else)
            h->ref_cache[list][scan8[4 ]] =
            h->ref_cache[list][scan8[12]] = PART_NOT_AVAILABLE;
            *(uint32_t*)h->mv_cache [list][scan8[5 ]+1]=
            *(uint32_t*)h->mv_cache [list][scan8[7 ]+1]=
            *(uint32_t*)h->mv_cache [list][scan8[13]+1]= //FIXME remove past 3 (init somewhere else)
            *(uint32_t*)h->mv_cache [list][scan8[4 ]]=
            *(uint32_t*)h->mv_cache [list][scan8[12]]= 0;

            if( h->pps.cabac ) {
                /* XXX beurk, Load mvd */
                if(USES_LIST(top_type, list)){
                    const int b_xy= h->mb2b_xy[top_xy] + 3*h->b_stride;
                    *(uint32_t*)h->mvd_cache[list][scan8[0] + 0 - 1*8]= *(uint32_t*)h->mvd_table[list][b_xy + 0];
                    *(uint32_t*)h->mvd_cache[list][scan8[0] + 1 - 1*8]= *(uint32_t*)h->mvd_table[list][b_xy + 1];
                    *(uint32_t*)h->mvd_cache[list][scan8[0] + 2 - 1*8]= *(uint32_t*)h->mvd_table[list][b_xy + 2];
                    *(uint32_t*)h->mvd_cache[list][scan8[0] + 3 - 1*8]= *(uint32_t*)h->mvd_table[list][b_xy + 3];
                }else{
                    *(uint32_t*)h->mvd_cache [list][scan8[0] + 0 - 1*8]=
                    *(uint32_t*)h->mvd_cache [list][scan8[0] + 1 - 1*8]=
                    *(uint32_t*)h->mvd_cache [list][scan8[0] + 2 - 1*8]=
                    *(uint32_t*)h->mvd_cache [list][scan8[0] + 3 - 1*8]= 0;
                }
                if(USES_LIST(left_type[0], list)){
                    const int b_xy= h->mb2b_xy[left_xy[0]] + 3;
                    *(uint32_t*)h->mvd_cache[list][scan8[0] - 1 + 0*8]= *(uint32_t*)h->mvd_table[list][b_xy + h->b_stride*left_block[0]];
                    *(uint32_t*)h->mvd_cache[list][scan8[0] - 1 + 1*8]= *(uint32_t*)h->mvd_table[list][b_xy + h->b_stride*left_block[1]];
                }else{
                    *(uint32_t*)h->mvd_cache [list][scan8[0] - 1 + 0*8]=
                    *(uint32_t*)h->mvd_cache [list][scan8[0] - 1 + 1*8]= 0;
                }
                if(USES_LIST(left_type[1], list)){
                    const int b_xy= h->mb2b_xy[left_xy[1]] + 3;
                    *(uint32_t*)h->mvd_cache[list][scan8[0] - 1 + 2*8]= *(uint32_t*)h->mvd_table[list][b_xy + h->b_stride*left_block[2]];
                    *(uint32_t*)h->mvd_cache[list][scan8[0] - 1 + 3*8]= *(uint32_t*)h->mvd_table[list][b_xy + h->b_stride*left_block[3]];
                }else{
                    *(uint32_t*)h->mvd_cache [list][scan8[0] - 1 + 2*8]=
                    *(uint32_t*)h->mvd_cache [list][scan8[0] - 1 + 3*8]= 0;
                }
                *(uint32_t*)h->mvd_cache [list][scan8[5 ]+1]=
                *(uint32_t*)h->mvd_cache [list][scan8[7 ]+1]=
                *(uint32_t*)h->mvd_cache [list][scan8[13]+1]= //FIXME remove past 3 (init somewhere else)
                *(uint32_t*)h->mvd_cache [list][scan8[4 ]]=
                *(uint32_t*)h->mvd_cache [list][scan8[12]]= 0;

                if(h->slice_type == B_TYPE){
                    fill_rectangle(&h->direct_cache[scan8[0]], 4, 4, 8, 0, 1);

                    if(IS_DIRECT(top_type)){
                        *(uint32_t*)&h->direct_cache[scan8[0] - 1*8]= 0x01010101;
                    }else if(IS_8X8(top_type)){
                        int b8_xy = h->mb2b8_xy[top_xy] + h->b8_stride;
                        h->direct_cache[scan8[0] + 0 - 1*8]= h->direct_table[b8_xy];
                        h->direct_cache[scan8[0] + 2 - 1*8]= h->direct_table[b8_xy + 1];
                    }else{
                        *(uint32_t*)&h->direct_cache[scan8[0] - 1*8]= 0;
                    }

                    if(IS_DIRECT(left_type[0]))
                        h->direct_cache[scan8[0] - 1 + 0*8]= 1;
                    else if(IS_8X8(left_type[0]))
                        h->direct_cache[scan8[0] - 1 + 0*8]= h->direct_table[h->mb2b8_xy[left_xy[0]] + 1 + h->b8_stride*(left_block[0]>>1)];
                    else
                        h->direct_cache[scan8[0] - 1 + 0*8]= 0;

                    if(IS_DIRECT(left_type[1]))
                        h->direct_cache[scan8[0] - 1 + 2*8]= 1;
                    else if(IS_8X8(left_type[1]))
                        h->direct_cache[scan8[0] - 1 + 2*8]= h->direct_table[h->mb2b8_xy[left_xy[1]] + 1 + h->b8_stride*(left_block[2]>>1)];
                    else
                        h->direct_cache[scan8[0] - 1 + 2*8]= 0;
                }
            }

            if(FRAME_MBAFF){
#define MAP_MVS\
                    MAP_F2F(scan8[0] - 1 - 1*8, topleft_type)\
                    MAP_F2F(scan8[0] + 0 - 1*8, top_type)\
                    MAP_F2F(scan8[0] + 1 - 1*8, top_type)\
                    MAP_F2F(scan8[0] + 2 - 1*8, top_type)\
                    MAP_F2F(scan8[0] + 3 - 1*8, top_type)\
                    MAP_F2F(scan8[0] + 4 - 1*8, topright_type)\
                    MAP_F2F(scan8[0] - 1 + 0*8, left_type[0])\
                    MAP_F2F(scan8[0] - 1 + 1*8, left_type[0])\
                    MAP_F2F(scan8[0] - 1 + 2*8, left_type[1])\
                    MAP_F2F(scan8[0] - 1 + 3*8, left_type[1])
                if(MB_FIELD){
#define MAP_F2F(idx, mb_type)\
                    if(!IS_INTERLACED(mb_type) && h->ref_cache[list][idx] >= 0){\
                        h->ref_cache[list][idx] <<= 1;\
                        h->mv_cache[list][idx][1] /= 2;\
                        h->mvd_cache[list][idx][1] /= 2;\
                    }
                    MAP_MVS
#undef MAP_F2F
                }else{
#define MAP_F2F(idx, mb_type)\
                    if(IS_INTERLACED(mb_type) && h->ref_cache[list][idx] >= 0){\
                        h->ref_cache[list][idx] >>= 1;\
                        h->mv_cache[list][idx][1] <<= 1;\
                        h->mvd_cache[list][idx][1] <<= 1;\
                    }
                    MAP_MVS
#undef MAP_F2F
                }
            }
        }
    }
#endif

    h->neighbor_transform_size= !!IS_8x8DCT(top_type) + !!IS_8x8DCT(left_type[0]);
}

static inline void write_back_intra_pred_mode(H264Context *h){
    MpegEncContext * const s = &h->s;
    const int mb_xy= s->mb_x + s->mb_y*s->mb_stride;

    h->intra4x4_pred_mode[mb_xy][0]= h->intra4x4_pred_mode_cache[7+8*1];
    h->intra4x4_pred_mode[mb_xy][1]= h->intra4x4_pred_mode_cache[7+8*2];
    h->intra4x4_pred_mode[mb_xy][2]= h->intra4x4_pred_mode_cache[7+8*3];
    h->intra4x4_pred_mode[mb_xy][3]= h->intra4x4_pred_mode_cache[7+8*4];
    h->intra4x4_pred_mode[mb_xy][4]= h->intra4x4_pred_mode_cache[4+8*4];
    h->intra4x4_pred_mode[mb_xy][5]= h->intra4x4_pred_mode_cache[5+8*4];
    h->intra4x4_pred_mode[mb_xy][6]= h->intra4x4_pred_mode_cache[6+8*4];
}

/**
 * checks if the top & left blocks are available if needed & changes the dc mode so it only uses the available blocks.
 */
static inline int check_intra4x4_pred_mode(H264Context *h){
    MpegEncContext * const s = &h->s;
    static const int8_t top [12]= {-1, 0,LEFT_DC_PRED,-1,-1,-1,-1,-1, 0};
    static const int8_t left[12]= { 0,-1, TOP_DC_PRED, 0,-1,-1,-1, 0,-1,DC_128_PRED};
    int i;

    if(!(h->top_samples_available&0x8000)){
        for(i=0; i<4; i++){
            int status= top[ h->intra4x4_pred_mode_cache[scan8[0] + i] ];
            if(status<0){
                av_log(h->s.avctx, AV_LOG_ERROR, "top block unavailable for requested intra4x4 mode %d at %d %d\n", status, s->mb_x, s->mb_y);
                return -1;
            } else if(status){
                h->intra4x4_pred_mode_cache[scan8[0] + i]= status;
            }
        }
    }

    if(!(h->left_samples_available&0x8000)){
        for(i=0; i<4; i++){
            int status= left[ h->intra4x4_pred_mode_cache[scan8[0] + 8*i] ];
            if(status<0){
                av_log(h->s.avctx, AV_LOG_ERROR, "left block unavailable for requested intra4x4 mode %d at %d %d\n", status, s->mb_x, s->mb_y);
                return -1;
            } else if(status){
                h->intra4x4_pred_mode_cache[scan8[0] + 8*i]= status;
            }
        }
    }

    return 0;
} //FIXME cleanup like next

/**
 * checks if the top & left blocks are available if needed & changes the dc mode so it only uses the available blocks.
 */
static inline int check_intra_pred_mode(H264Context *h, int mode){
    MpegEncContext * const s = &h->s;
    static const int8_t top [7]= {LEFT_DC_PRED8x8, 1,-1,-1};
    static const int8_t left[7]= { TOP_DC_PRED8x8,-1, 2,-1,DC_128_PRED8x8};

    if(mode > 6U) {
        av_log(h->s.avctx, AV_LOG_ERROR, "out of range intra chroma pred mode at %d %d\n", s->mb_x, s->mb_y);
        return -1;
    }

    if(!(h->top_samples_available&0x8000)){
        mode= top[ mode ];
        if(mode<0){
            av_log(h->s.avctx, AV_LOG_ERROR, "top block unavailable for requested intra mode at %d %d\n", s->mb_x, s->mb_y);
            return -1;
        }
    }

    if(!(h->left_samples_available&0x8000)){
        mode= left[ mode ];
        if(mode<0){
            av_log(h->s.avctx, AV_LOG_ERROR, "left block unavailable for requested intra mode at %d %d\n", s->mb_x, s->mb_y);
            return -1;
        }
    }

    return mode;
}

/**
 * gets the predicted intra4x4 prediction mode.
 */
static inline int pred_intra_mode(H264Context *h, int n){
    const int index8= scan8[n];
    const int left= h->intra4x4_pred_mode_cache[index8 - 1];
    const int top = h->intra4x4_pred_mode_cache[index8 - 8];
    const int min= FFMIN(left, top);

    tprintf(h->s.avctx, "mode:%d %d min:%d\n", left ,top, min);

    if(min<0) return DC_PRED;
    else      return min;
}

static inline void write_back_non_zero_count(H264Context *h){
    MpegEncContext * const s = &h->s;
    const int mb_xy= s->mb_x + s->mb_y*s->mb_stride;

    h->non_zero_count[mb_xy][0]= h->non_zero_count_cache[7+8*1];
    h->non_zero_count[mb_xy][1]= h->non_zero_count_cache[7+8*2];
    h->non_zero_count[mb_xy][2]= h->non_zero_count_cache[7+8*3];
    h->non_zero_count[mb_xy][3]= h->non_zero_count_cache[7+8*4];
    h->non_zero_count[mb_xy][4]= h->non_zero_count_cache[4+8*4];
    h->non_zero_count[mb_xy][5]= h->non_zero_count_cache[5+8*4];
    h->non_zero_count[mb_xy][6]= h->non_zero_count_cache[6+8*4];

    h->non_zero_count[mb_xy][9]= h->non_zero_count_cache[1+8*2];
    h->non_zero_count[mb_xy][8]= h->non_zero_count_cache[2+8*2];
    h->non_zero_count[mb_xy][7]= h->non_zero_count_cache[2+8*1];

    h->non_zero_count[mb_xy][12]=h->non_zero_count_cache[1+8*5];
    h->non_zero_count[mb_xy][11]=h->non_zero_count_cache[2+8*5];
    h->non_zero_count[mb_xy][10]=h->non_zero_count_cache[2+8*4];

    if(FRAME_MBAFF){
        // store all luma nnzs, for deblocking
        int v = 0, i;
        for(i=0; i<16; i++)
            v += (!!h->non_zero_count_cache[scan8[i]]) << i;
        *(uint16_t*)&h->non_zero_count[mb_xy][14] = v;
    }
}

/**
 * gets the predicted number of non zero coefficients.
 * @param n block index
 */
static inline int pred_non_zero_count(H264Context *h, int n){
    const int index8= scan8[n];
    const int left= h->non_zero_count_cache[index8 - 1];
    const int top = h->non_zero_count_cache[index8 - 8];
    int i= left + top;

    if(i<64) i= (i+1)>>1;

    tprintf(h->s.avctx, "pred_nnz L%X T%X n%d s%d P%X\n", left, top, n, scan8[n], i&31);

    return i&31;
}

static inline int fetch_diagonal_mv(H264Context *h, const int16_t **C, int i, int list, int part_width){
    const int topright_ref= h->ref_cache[list][ i - 8 + part_width ];
    MpegEncContext *s = &h->s;

    /* there is no consistent mapping of mvs to neighboring locations that will
     * make mbaff happy, so we can't move all this logic to fill_caches */
    if(FRAME_MBAFF){
        const uint32_t *mb_types = s->current_picture_ptr->mb_type;
        const int16_t *mv;
        *(uint32_t*)h->mv_cache[list][scan8[0]-2] = 0;
        *C = h->mv_cache[list][scan8[0]-2];

        if(!MB_FIELD
           && (s->mb_y&1) && i < scan8[0]+8 && topright_ref != PART_NOT_AVAILABLE){
            int topright_xy = s->mb_x + (s->mb_y-1)*s->mb_stride + (i == scan8[0]+3);
            if(IS_INTERLACED(mb_types[topright_xy])){
#define SET_DIAG_MV(MV_OP, REF_OP, X4, Y4)\
                const int x4 = X4, y4 = Y4;\
                const int mb_type = mb_types[(x4>>2)+(y4>>2)*s->mb_stride];\
                if(!USES_LIST(mb_type,list) && !IS_8X8(mb_type))\
                    return LIST_NOT_USED;\
                mv = s->current_picture_ptr->motion_val[list][x4 + y4*h->b_stride];\
                h->mv_cache[list][scan8[0]-2][0] = mv[0];\
                h->mv_cache[list][scan8[0]-2][1] = mv[1] MV_OP;\
                return s->current_picture_ptr->ref_index[list][(x4>>1) + (y4>>1)*h->b8_stride] REF_OP;

                SET_DIAG_MV(*2, >>1, s->mb_x*4+(i&7)-4+part_width, s->mb_y*4-1);
            }
        }
        if(topright_ref == PART_NOT_AVAILABLE
           && ((s->mb_y&1) || i >= scan8[0]+8) && (i&7)==4
           && h->ref_cache[list][scan8[0]-1] != PART_NOT_AVAILABLE){
            if(!MB_FIELD
               && IS_INTERLACED(mb_types[h->left_mb_xy[0]])){
                SET_DIAG_MV(*2, >>1, s->mb_x*4-1, (s->mb_y|1)*4+(s->mb_y&1)*2+(i>>4)-1);
            }
            if(MB_FIELD
               && !IS_INTERLACED(mb_types[h->left_mb_xy[0]])
               && i >= scan8[0]+8){
                // leftshift will turn LIST_NOT_USED into PART_NOT_AVAILABLE, but that's ok.
                SET_DIAG_MV(>>1, <<1, s->mb_x*4-1, (s->mb_y&~1)*4 - 1 + ((i-scan8[0])>>3)*2);
            }
        }
#undef SET_DIAG_MV
    }

    if(topright_ref != PART_NOT_AVAILABLE){
        *C= h->mv_cache[list][ i - 8 + part_width ];
        return topright_ref;
    }else{
        tprintf(s->avctx, "topright MV not available\n");

        *C= h->mv_cache[list][ i - 8 - 1 ];
        return h->ref_cache[list][ i - 8 - 1 ];
    }
}

/**
 * gets the predicted MV.
 * @param n the block index
 * @param part_width the width of the partition (4, 8,16) -> (1, 2, 4)
 * @param mx the x component of the predicted motion vector
 * @param my the y component of the predicted motion vector
 */
static inline void pred_motion(H264Context * const h, int n, int part_width, int list, int ref, int * const mx, int * const my){
    const int index8= scan8[n];
    const int top_ref=      h->ref_cache[list][ index8 - 8 ];
    const int left_ref=     h->ref_cache[list][ index8 - 1 ];
    const int16_t * const A= h->mv_cache[list][ index8 - 1 ];
    const int16_t * const B= h->mv_cache[list][ index8 - 8 ];
    const int16_t * C;
    int diagonal_ref, match_count;

    assert(part_width==1 || part_width==2 || part_width==4);

/* mv_cache
  B . . A T T T T
  U . . L . . , .
  U . . L . . . .
  U . . L . . , .
  . . . L . . . .
*/

    diagonal_ref= fetch_diagonal_mv(h, &C, index8, list, part_width);
    match_count= (diagonal_ref==ref) + (top_ref==ref) + (left_ref==ref);
    tprintf(h->s.avctx, "pred_motion match_count=%d\n", match_count);
    if(match_count > 1){ //most common
        *mx= mid_pred(A[0], B[0], C[0]);
        *my= mid_pred(A[1], B[1], C[1]);
    }else if(match_count==1){
        if(left_ref==ref){
            *mx= A[0];
            *my= A[1];
        }else if(top_ref==ref){
            *mx= B[0];
            *my= B[1];
        }else{
            *mx= C[0];
            *my= C[1];
        }
    }else{
        if(top_ref == PART_NOT_AVAILABLE && diagonal_ref == PART_NOT_AVAILABLE && left_ref != PART_NOT_AVAILABLE){
            *mx= A[0];
            *my= A[1];
        }else{
            *mx= mid_pred(A[0], B[0], C[0]);
            *my= mid_pred(A[1], B[1], C[1]);
        }
    }

    tprintf(h->s.avctx, "pred_motion (%2d %2d %2d) (%2d %2d %2d) (%2d %2d %2d) -> (%2d %2d %2d) at %2d %2d %d list %d\n", top_ref, B[0], B[1],                    diagonal_ref, C[0], C[1], left_ref, A[0], A[1], ref, *mx, *my, h->s.mb_x, h->s.mb_y, n, list);
}

/**
 * gets the directionally predicted 16x8 MV.
 * @param n the block index
 * @param mx the x component of the predicted motion vector
 * @param my the y component of the predicted motion vector
 */
static inline void pred_16x8_motion(H264Context * const h, int n, int list, int ref, int * const mx, int * const my){
    if(n==0){
        const int top_ref=      h->ref_cache[list][ scan8[0] - 8 ];
        const int16_t * const B= h->mv_cache[list][ scan8[0] - 8 ];

        tprintf(h->s.avctx, "pred_16x8: (%2d %2d %2d) at %2d %2d %d list %d\n", top_ref, B[0], B[1], h->s.mb_x, h->s.mb_y, n, list);

        if(top_ref == ref){
            *mx= B[0];
            *my= B[1];
            return;
        }
    }else{
        const int left_ref=     h->ref_cache[list][ scan8[8] - 1 ];
        const int16_t * const A= h->mv_cache[list][ scan8[8] - 1 ];

        tprintf(h->s.avctx, "pred_16x8: (%2d %2d %2d) at %2d %2d %d list %d\n", left_ref, A[0], A[1], h->s.mb_x, h->s.mb_y, n, list);

        if(left_ref == ref){
            *mx= A[0];
            *my= A[1];
            return;
        }
    }

    //RARE
    pred_motion(h, n, 4, list, ref, mx, my);
}

/**
 * gets the directionally predicted 8x16 MV.
 * @param n the block index
 * @param mx the x component of the predicted motion vector
 * @param my the y component of the predicted motion vector
 */
static inline void pred_8x16_motion(H264Context * const h, int n, int list, int ref, int * const mx, int * const my){
    if(n==0){
        const int left_ref=      h->ref_cache[list][ scan8[0] - 1 ];
        const int16_t * const A=  h->mv_cache[list][ scan8[0] - 1 ];

        tprintf(h->s.avctx, "pred_8x16: (%2d %2d %2d) at %2d %2d %d list %d\n", left_ref, A[0], A[1], h->s.mb_x, h->s.mb_y, n, list);

        if(left_ref == ref){
            *mx= A[0];
            *my= A[1];
            return;
        }
    }else{
        const int16_t * C;
        int diagonal_ref;

        diagonal_ref= fetch_diagonal_mv(h, &C, scan8[4], list, 2);

        tprintf(h->s.avctx, "pred_8x16: (%2d %2d %2d) at %2d %2d %d list %d\n", diagonal_ref, C[0], C[1], h->s.mb_x, h->s.mb_y, n, list);

        if(diagonal_ref == ref){
            *mx= C[0];
            *my= C[1];
            return;
        }
    }

    //RARE
    pred_motion(h, n, 2, list, ref, mx, my);
}

static inline void pred_pskip_motion(H264Context * const h, int * const mx, int * const my){
    const int top_ref = h->ref_cache[0][ scan8[0] - 8 ];
    const int left_ref= h->ref_cache[0][ scan8[0] - 1 ];

    tprintf(h->s.avctx, "pred_pskip: (%d) (%d) at %2d %2d\n", top_ref, left_ref, h->s.mb_x, h->s.mb_y);

    if(top_ref == PART_NOT_AVAILABLE || left_ref == PART_NOT_AVAILABLE
       || (top_ref == 0  && *(uint32_t*)h->mv_cache[0][ scan8[0] - 8 ] == 0)
       || (left_ref == 0 && *(uint32_t*)h->mv_cache[0][ scan8[0] - 1 ] == 0)){

        *mx = *my = 0;
        return;
    }

    pred_motion(h, 0, 4, 0, 0, mx, my);

    return;
}

static inline void direct_dist_scale_factor(H264Context * const h){
    const int poc = h->s.current_picture_ptr->poc;
    const int poc1 = h->ref_list[1][0].poc;
    int i;
    for(i=0; i<h->ref_count[0]; i++){
        int poc0 = h->ref_list[0][i].poc;
        int td = av_clip(poc1 - poc0, -128, 127);
        if(td == 0 /* FIXME || pic0 is a long-term ref */){
            h->dist_scale_factor[i] = 256;
        }else{
            int tb = av_clip(poc - poc0, -128, 127);
            int tx = (16384 + (FFABS(td) >> 1)) / td;
            h->dist_scale_factor[i] = av_clip((tb*tx + 32) >> 6, -1024, 1023);
        }
    }
    if(FRAME_MBAFF){
        for(i=0; i<h->ref_count[0]; i++){
            h->dist_scale_factor_field[2*i] =
            h->dist_scale_factor_field[2*i+1] = h->dist_scale_factor[i];
        }
    }
}
static inline void direct_ref_list_init(H264Context * const h){
    MpegEncContext * const s = &h->s;
    Picture * const ref1 = &h->ref_list[1][0];
    Picture * const cur = s->current_picture_ptr;
    int list, i, j;
    if(cur->pict_type == I_TYPE)
        cur->ref_count[0] = 0;
    if(cur->pict_type != B_TYPE)
        cur->ref_count[1] = 0;
    for(list=0; list<2; list++){
        cur->ref_count[list] = h->ref_count[list];
        for(j=0; j<h->ref_count[list]; j++)
            cur->ref_poc[list][j] = h->ref_list[list][j].poc;
    }
    if(cur->pict_type != B_TYPE || h->direct_spatial_mv_pred)
        return;
    for(list=0; list<2; list++){
        for(i=0; i<ref1->ref_count[list]; i++){
            const int poc = ref1->ref_poc[list][i];
            h->map_col_to_list0[list][i] = 0; /* bogus; fills in for missing frames */
            for(j=0; j<h->ref_count[list]; j++)
                if(h->ref_list[list][j].poc == poc){
                    h->map_col_to_list0[list][i] = j;
                    break;
                }
        }
    }
    if(FRAME_MBAFF){
        for(list=0; list<2; list++){
            for(i=0; i<ref1->ref_count[list]; i++){
                j = h->map_col_to_list0[list][i];
                h->map_col_to_list0_field[list][2*i] = 2*j;
                h->map_col_to_list0_field[list][2*i+1] = 2*j+1;
            }
        }
    }
}

static inline void pred_direct_motion(H264Context * const h, int *mb_type){
    MpegEncContext * const s = &h->s;
    const int mb_xy =   s->mb_x +   s->mb_y*s->mb_stride;
    const int b8_xy = 2*s->mb_x + 2*s->mb_y*h->b8_stride;
    const int b4_xy = 4*s->mb_x + 4*s->mb_y*h->b_stride;
    const int mb_type_col = h->ref_list[1][0].mb_type[mb_xy];
    const int16_t (*l1mv0)[2] = (const int16_t (*)[2]) &h->ref_list[1][0].motion_val[0][b4_xy];
    const int16_t (*l1mv1)[2] = (const int16_t (*)[2]) &h->ref_list[1][0].motion_val[1][b4_xy];
    const int8_t *l1ref0 = &h->ref_list[1][0].ref_index[0][b8_xy];
    const int8_t *l1ref1 = &h->ref_list[1][0].ref_index[1][b8_xy];
    const int is_b8x8 = IS_8X8(*mb_type);
    unsigned int sub_mb_type;
    int i8, i4;

#define MB_TYPE_16x16_OR_INTRA (MB_TYPE_16x16|MB_TYPE_INTRA4x4|MB_TYPE_INTRA16x16|MB_TYPE_INTRA_PCM)
    if(IS_8X8(mb_type_col) && !h->sps.direct_8x8_inference_flag){
        /* FIXME save sub mb types from previous frames (or derive from MVs)
         * so we know exactly what block size to use */
        sub_mb_type = MB_TYPE_8x8|MB_TYPE_P0L0|MB_TYPE_P0L1|MB_TYPE_DIRECT2; /* B_SUB_4x4 */
        *mb_type =    MB_TYPE_8x8|MB_TYPE_L0L1;
    }else if(!is_b8x8 && (mb_type_col & MB_TYPE_16x16_OR_INTRA)){
        sub_mb_type = MB_TYPE_16x16|MB_TYPE_P0L0|MB_TYPE_P0L1|MB_TYPE_DIRECT2; /* B_SUB_8x8 */
        *mb_type =    MB_TYPE_16x16|MB_TYPE_P0L0|MB_TYPE_P0L1|MB_TYPE_DIRECT2; /* B_16x16 */
    }else{
        sub_mb_type = MB_TYPE_16x16|MB_TYPE_P0L0|MB_TYPE_P0L1|MB_TYPE_DIRECT2; /* B_SUB_8x8 */
        *mb_type =    MB_TYPE_8x8|MB_TYPE_L0L1;
    }
    if(!is_b8x8)
        *mb_type |= MB_TYPE_DIRECT2;
    if(MB_FIELD)
        *mb_type |= MB_TYPE_INTERLACED;

    tprintf(s->avctx, "mb_type = %08x, sub_mb_type = %08x, is_b8x8 = %d, mb_type_col = %08x\n", *mb_type, sub_mb_type, is_b8x8, mb_type_col);

    if(h->direct_spatial_mv_pred){
        int ref[2];
        int mv[2][2];
        int list;

        /* FIXME interlacing + spatial direct uses wrong colocated block positions */

        /* ref = min(neighbors) */
        for(list=0; list<2; list++){
            int refa = h->ref_cache[list][scan8[0] - 1];
            int refb = h->ref_cache[list][scan8[0] - 8];
            int refc = h->ref_cache[list][scan8[0] - 8 + 4];
            if(refc == -2)
                refc = h->ref_cache[list][scan8[0] - 8 - 1];
            ref[list] = refa;
            if(ref[list] < 0 || (refb < ref[list] && refb >= 0))
                ref[list] = refb;
            if(ref[list] < 0 || (refc < ref[list] && refc >= 0))
                ref[list] = refc;
            if(ref[list] < 0)
                ref[list] = -1;
        }

        if(ref[0] < 0 && ref[1] < 0){
            ref[0] = ref[1] = 0;
            mv[0][0] = mv[0][1] =
            mv[1][0] = mv[1][1] = 0;
        }else{
            for(list=0; list<2; list++){
                if(ref[list] >= 0)
                    pred_motion(h, 0, 4, list, ref[list], &mv[list][0], &mv[list][1]);
                else
                    mv[list][0] = mv[list][1] = 0;
            }
        }

        if(ref[1] < 0){
            *mb_type &= ~MB_TYPE_P0L1;
            sub_mb_type &= ~MB_TYPE_P0L1;
        }else if(ref[0] < 0){
            *mb_type &= ~MB_TYPE_P0L0;
            sub_mb_type &= ~MB_TYPE_P0L0;
        }

        if(IS_16X16(*mb_type)){
            int a=0, b=0;

            fill_rectangle(&h->ref_cache[0][scan8[0]], 4, 4, 8, (uint8_t)ref[0], 1);
            fill_rectangle(&h->ref_cache[1][scan8[0]], 4, 4, 8, (uint8_t)ref[1], 1);
            if(!IS_INTRA(mb_type_col)
               && (   (l1ref0[0] == 0 && FFABS(l1mv0[0][0]) <= 1 && FFABS(l1mv0[0][1]) <= 1)
                   || (l1ref0[0]  < 0 && l1ref1[0] == 0 && FFABS(l1mv1[0][0]) <= 1 && FFABS(l1mv1[0][1]) <= 1
                       && (h->x264_build>33 || !h->x264_build)))){
                if(ref[0] > 0)
                    a= pack16to32(mv[0][0],mv[0][1]);
                if(ref[1] > 0)
                    b= pack16to32(mv[1][0],mv[1][1]);
            }else{
                a= pack16to32(mv[0][0],mv[0][1]);
                b= pack16to32(mv[1][0],mv[1][1]);
            }
            fill_rectangle(&h->mv_cache[0][scan8[0]], 4, 4, 8, a, 4);
            fill_rectangle(&h->mv_cache[1][scan8[0]], 4, 4, 8, b, 4);
        }else{
            for(i8=0; i8<4; i8++){
                const int x8 = i8&1;
                const int y8 = i8>>1;

                if(is_b8x8 && !IS_DIRECT(h->sub_mb_type[i8]))
                    continue;
                h->sub_mb_type[i8] = sub_mb_type;

                fill_rectangle(&h->mv_cache[0][scan8[i8*4]], 2, 2, 8, pack16to32(mv[0][0],mv[0][1]), 4);
                fill_rectangle(&h->mv_cache[1][scan8[i8*4]], 2, 2, 8, pack16to32(mv[1][0],mv[1][1]), 4);
                fill_rectangle(&h->ref_cache[0][scan8[i8*4]], 2, 2, 8, (uint8_t)ref[0], 1);
                fill_rectangle(&h->ref_cache[1][scan8[i8*4]], 2, 2, 8, (uint8_t)ref[1], 1);

                /* col_zero_flag */
                if(!IS_INTRA(mb_type_col) && (   l1ref0[x8 + y8*h->b8_stride] == 0
                                              || (l1ref0[x8 + y8*h->b8_stride] < 0 && l1ref1[x8 + y8*h->b8_stride] == 0
                                                  && (h->x264_build>33 || !h->x264_build)))){
                    const int16_t (*l1mv)[2]= l1ref0[x8 + y8*h->b8_stride] == 0 ? l1mv0 : l1mv1;
                    if(IS_SUB_8X8(sub_mb_type)){
                        const int16_t *mv_col = l1mv[x8*3 + y8*3*h->b_stride];
                        if(FFABS(mv_col[0]) <= 1 && FFABS(mv_col[1]) <= 1){
                            if(ref[0] == 0)
                                fill_rectangle(&h->mv_cache[0][scan8[i8*4]], 2, 2, 8, 0, 4);
                            if(ref[1] == 0)
                                fill_rectangle(&h->mv_cache[1][scan8[i8*4]], 2, 2, 8, 0, 4);
                        }
                    }else
                    for(i4=0; i4<4; i4++){
                        const int16_t *mv_col = l1mv[x8*2 + (i4&1) + (y8*2 + (i4>>1))*h->b_stride];
                        if(FFABS(mv_col[0]) <= 1 && FFABS(mv_col[1]) <= 1){
                            if(ref[0] == 0)
                                *(uint32_t*)h->mv_cache[0][scan8[i8*4+i4]] = 0;
                            if(ref[1] == 0)
                                *(uint32_t*)h->mv_cache[1][scan8[i8*4+i4]] = 0;
                        }
                    }
                }
            }
        }
    }else{ /* direct temporal mv pred */
        const int *map_col_to_list0[2] = {h->map_col_to_list0[0], h->map_col_to_list0[1]};
        const int *dist_scale_factor = h->dist_scale_factor;

        if(FRAME_MBAFF){
            if(IS_INTERLACED(*mb_type)){
                map_col_to_list0[0] = h->map_col_to_list0_field[0];
                map_col_to_list0[1] = h->map_col_to_list0_field[1];
                dist_scale_factor = h->dist_scale_factor_field;
            }
            if(IS_INTERLACED(*mb_type) != IS_INTERLACED(mb_type_col)){
                /* FIXME assumes direct_8x8_inference == 1 */
                const int pair_xy = s->mb_x + (s->mb_y&~1)*s->mb_stride;
                int mb_types_col[2];
                int y_shift;

                *mb_type = MB_TYPE_8x8|MB_TYPE_L0L1
                         | (is_b8x8 ? 0 : MB_TYPE_DIRECT2)
                         | (*mb_type & MB_TYPE_INTERLACED);
                sub_mb_type = MB_TYPE_P0L0|MB_TYPE_P0L1|MB_TYPE_DIRECT2|MB_TYPE_16x16;

                if(IS_INTERLACED(*mb_type)){
                    /* frame to field scaling */
                    mb_types_col[0] = h->ref_list[1][0].mb_type[pair_xy];
                    mb_types_col[1] = h->ref_list[1][0].mb_type[pair_xy+s->mb_stride];
                    if(s->mb_y&1){
                        l1ref0 -= 2*h->b8_stride;
                        l1ref1 -= 2*h->b8_stride;
                        l1mv0 -= 4*h->b_stride;
                        l1mv1 -= 4*h->b_stride;
                    }
                    y_shift = 0;

                    if(   (mb_types_col[0] & MB_TYPE_16x16_OR_INTRA)
                       && (mb_types_col[1] & MB_TYPE_16x16_OR_INTRA)
                       && !is_b8x8)
                        *mb_type |= MB_TYPE_16x8;
                    else
                        *mb_type |= MB_TYPE_8x8;
                }else{
                    /* field to frame scaling */
                    /* col_mb_y = (mb_y&~1) + (topAbsDiffPOC < bottomAbsDiffPOC ? 0 : 1)
                     * but in MBAFF, top and bottom POC are equal */
                    int dy = (s->mb_y&1) ? 1 : 2;
                    mb_types_col[0] =
                    mb_types_col[1] = h->ref_list[1][0].mb_type[pair_xy+s->mb_stride];
                    l1ref0 += dy*h->b8_stride;
                    l1ref1 += dy*h->b8_stride;
                    l1mv0 += 2*dy*h->b_stride;
                    l1mv1 += 2*dy*h->b_stride;
                    y_shift = 2;

                    if((mb_types_col[0] & (MB_TYPE_16x16_OR_INTRA|MB_TYPE_16x8))
                       && !is_b8x8)
                        *mb_type |= MB_TYPE_16x16;
                    else
                        *mb_type |= MB_TYPE_8x8;
                }

                for(i8=0; i8<4; i8++){
                    const int x8 = i8&1;
                    const int y8 = i8>>1;
                    int ref0, scale;
                    const int16_t (*l1mv)[2]= l1mv0;

                    if(is_b8x8 && !IS_DIRECT(h->sub_mb_type[i8]))
                        continue;
                    h->sub_mb_type[i8] = sub_mb_type;

                    fill_rectangle(&h->ref_cache[1][scan8[i8*4]], 2, 2, 8, 0, 1);
                    if(IS_INTRA(mb_types_col[y8])){
                        fill_rectangle(&h->ref_cache[0][scan8[i8*4]], 2, 2, 8, 0, 1);
                        fill_rectangle(&h-> mv_cache[0][scan8[i8*4]], 2, 2, 8, 0, 4);
                        fill_rectangle(&h-> mv_cache[1][scan8[i8*4]], 2, 2, 8, 0, 4);
                        continue;
                    }

                    ref0 = l1ref0[x8 + (y8*2>>y_shift)*h->b8_stride];
                    if(ref0 >= 0)
                        ref0 = map_col_to_list0[0][ref0*2>>y_shift];
                    else{
                        ref0 = map_col_to_list0[1][l1ref1[x8 + (y8*2>>y_shift)*h->b8_stride]*2>>y_shift];
                        l1mv= l1mv1;
                    }
                    scale = dist_scale_factor[ref0];
                    fill_rectangle(&h->ref_cache[0][scan8[i8*4]], 2, 2, 8, ref0, 1);

                    {
                        const int16_t *mv_col = l1mv[x8*3 + (y8*6>>y_shift)*h->b_stride];
                        int my_col = (mv_col[1]<<y_shift)/2;
                        int mx = (scale * mv_col[0] + 128) >> 8;
                        int my = (scale * my_col + 128) >> 8;
                        fill_rectangle(&h->mv_cache[0][scan8[i8*4]], 2, 2, 8, pack16to32(mx,my), 4);
                        fill_rectangle(&h->mv_cache[1][scan8[i8*4]], 2, 2, 8, pack16to32(mx-mv_col[0],my-my_col), 4);
                    }
                }
                return;
            }
        }

        /* one-to-one mv scaling */

        if(IS_16X16(*mb_type)){
            int ref, mv0, mv1;

            fill_rectangle(&h->ref_cache[1][scan8[0]], 4, 4, 8, 0, 1);
            if(IS_INTRA(mb_type_col)){
                ref=mv0=mv1=0;
            }else{
                const int ref0 = l1ref0[0] >= 0 ? map_col_to_list0[0][l1ref0[0]]
                                                : map_col_to_list0[1][l1ref1[0]];
                const int scale = dist_scale_factor[ref0];
                const int16_t *mv_col = l1ref0[0] >= 0 ? l1mv0[0] : l1mv1[0];
                int mv_l0[2];
                mv_l0[0] = (scale * mv_col[0] + 128) >> 8;
                mv_l0[1] = (scale * mv_col[1] + 128) >> 8;
                ref= ref0;
                mv0= pack16to32(mv_l0[0],mv_l0[1]);
                mv1= pack16to32(mv_l0[0]-mv_col[0],mv_l0[1]-mv_col[1]);
            }
            fill_rectangle(&h->ref_cache[0][scan8[0]], 4, 4, 8, ref, 1);
            fill_rectangle(&h-> mv_cache[0][scan8[0]], 4, 4, 8, mv0, 4);
            fill_rectangle(&h-> mv_cache[1][scan8[0]], 4, 4, 8, mv1, 4);
        }else{
            for(i8=0; i8<4; i8++){
                const int x8 = i8&1;
                const int y8 = i8>>1;
                int ref0, scale;
                const int16_t (*l1mv)[2]= l1mv0;

                if(is_b8x8 && !IS_DIRECT(h->sub_mb_type[i8]))
                    continue;
                h->sub_mb_type[i8] = sub_mb_type;
                fill_rectangle(&h->ref_cache[1][scan8[i8*4]], 2, 2, 8, 0, 1);
                if(IS_INTRA(mb_type_col)){
                    fill_rectangle(&h->ref_cache[0][scan8[i8*4]], 2, 2, 8, 0, 1);
                    fill_rectangle(&h-> mv_cache[0][scan8[i8*4]], 2, 2, 8, 0, 4);
                    fill_rectangle(&h-> mv_cache[1][scan8[i8*4]], 2, 2, 8, 0, 4);
                    continue;
                }

                ref0 = l1ref0[x8 + y8*h->b8_stride];
                if(ref0 >= 0)
                    ref0 = map_col_to_list0[0][ref0];
                else{
                    ref0 = map_col_to_list0[1][l1ref1[x8 + y8*h->b8_stride]];
                    l1mv= l1mv1;
                }
                scale = dist_scale_factor[ref0];

                fill_rectangle(&h->ref_cache[0][scan8[i8*4]], 2, 2, 8, ref0, 1);
                if(IS_SUB_8X8(sub_mb_type)){
                    const int16_t *mv_col = l1mv[x8*3 + y8*3*h->b_stride];
                    int mx = (scale * mv_col[0] + 128) >> 8;
                    int my = (scale * mv_col[1] + 128) >> 8;
                    fill_rectangle(&h->mv_cache[0][scan8[i8*4]], 2, 2, 8, pack16to32(mx,my), 4);
                    fill_rectangle(&h->mv_cache[1][scan8[i8*4]], 2, 2, 8, pack16to32(mx-mv_col[0],my-mv_col[1]), 4);
                }else
                for(i4=0; i4<4; i4++){
                    const int16_t *mv_col = l1mv[x8*2 + (i4&1) + (y8*2 + (i4>>1))*h->b_stride];
                    int16_t *mv_l0 = h->mv_cache[0][scan8[i8*4+i4]];
                    mv_l0[0] = (scale * mv_col[0] + 128) >> 8;
                    mv_l0[1] = (scale * mv_col[1] + 128) >> 8;
                    *(uint32_t*)h->mv_cache[1][scan8[i8*4+i4]] =
                        pack16to32(mv_l0[0]-mv_col[0],mv_l0[1]-mv_col[1]);
                }
            }
        }
    }
}

static inline void write_back_motion(H264Context *h, int mb_type){
    MpegEncContext * const s = &h->s;
    const int b_xy = 4*s->mb_x + 4*s->mb_y*h->b_stride;
    const int b8_xy= 2*s->mb_x + 2*s->mb_y*h->b8_stride;
    int list;

    if(!USES_LIST(mb_type, 0))
        fill_rectangle(&s->current_picture.ref_index[0][b8_xy], 2, 2, h->b8_stride, (uint8_t)LIST_NOT_USED, 1);

    for(list=0; list<h->list_count; list++){
        int y;
        if(!USES_LIST(mb_type, list))
            continue;

        for(y=0; y<4; y++){
            *(uint64_t*)s->current_picture.motion_val[list][b_xy + 0 + y*h->b_stride]= *(uint64_t*)h->mv_cache[list][scan8[0]+0 + 8*y];
            *(uint64_t*)s->current_picture.motion_val[list][b_xy + 2 + y*h->b_stride]= *(uint64_t*)h->mv_cache[list][scan8[0]+2 + 8*y];
        }
        if( h->pps.cabac ) {
            if(IS_SKIP(mb_type))
                fill_rectangle(h->mvd_table[list][b_xy], 4, 4, h->b_stride, 0, 4);
            else
            for(y=0; y<4; y++){
                *(uint64_t*)h->mvd_table[list][b_xy + 0 + y*h->b_stride]= *(uint64_t*)h->mvd_cache[list][scan8[0]+0 + 8*y];
                *(uint64_t*)h->mvd_table[list][b_xy + 2 + y*h->b_stride]= *(uint64_t*)h->mvd_cache[list][scan8[0]+2 + 8*y];
            }
        }

        {
            int8_t *ref_index = &s->current_picture.ref_index[list][b8_xy];
            ref_index[0+0*h->b8_stride]= h->ref_cache[list][scan8[0]];
            ref_index[1+0*h->b8_stride]= h->ref_cache[list][scan8[4]];
            ref_index[0+1*h->b8_stride]= h->ref_cache[list][scan8[8]];
            ref_index[1+1*h->b8_stride]= h->ref_cache[list][scan8[12]];
        }
    }

    if(h->slice_type == B_TYPE && h->pps.cabac){
        if(IS_8X8(mb_type)){
            uint8_t *direct_table = &h->direct_table[b8_xy];
            direct_table[1+0*h->b8_stride] = IS_DIRECT(h->sub_mb_type[1]) ? 1 : 0;
            direct_table[0+1*h->b8_stride] = IS_DIRECT(h->sub_mb_type[2]) ? 1 : 0;
            direct_table[1+1*h->b8_stride] = IS_DIRECT(h->sub_mb_type[3]) ? 1 : 0;
        }
    }
}

/**
 * Decodes a network abstraction layer unit.
 * @param consumed is the number of bytes used as input
 * @param length is the length of the array
 * @param dst_length is the number of decoded bytes FIXME here or a decode rbsp tailing?
 * @returns decoded bytes, might be src+1 if no escapes
 */
static uint8_t *decode_nal(H264Context *h, uint8_t *src, int *dst_length, int *consumed, int length){
    int i, si, di;
    uint8_t *dst;

//    src[0]&0x80;                //forbidden bit
    h->nal_ref_idc= src[0]>>5;
    h->nal_unit_type= src[0]&0x1F;

    src++; length--;
#if 0
    for(i=0; i<length; i++)
        printf("%2X ", src[i]);
#endif
    for(i=0; i+1<length; i+=2){
        if(src[i]) continue;
        if(i>0 && src[i-1]==0) i--;
        if(i+2<length && src[i+1]==0 && src[i+2]<=3){
            if(src[i+2]!=3){
                /* startcode, so we must be past the end */
                length=i;
            }
            break;
        }
    }

    if(i>=length-1){ //no escaped 0
        *dst_length= length;
        *consumed= length+1; //+1 for the header
        return src;
    }

    h->rbsp_buffer= av_fast_realloc(h->rbsp_buffer, &h->rbsp_buffer_size, length);
    dst= h->rbsp_buffer;

    if (dst == NULL){
        return NULL;
    }

//printf("decoding esc\n");
    si=di=0;
    while(si<length){
        //remove escapes (very rare 1:2^22)
        if(si+2<length && src[si]==0 && src[si+1]==0 && src[si+2]<=3){
            if(src[si+2]==3){ //escape
                dst[di++]= 0;
                dst[di++]= 0;
                si+=3;
                continue;
            }else //next start code
                break;
        }

        dst[di++]= src[si++];
    }

    *dst_length= di;
    *consumed= si + 1;//+1 for the header
//FIXME store exact number of bits in the getbitcontext (its needed for decoding)
    return dst;
}

/**
 * identifies the exact end of the bitstream
 * @return the length of the trailing, or 0 if damaged
 */
static int decode_rbsp_trailing(H264Context *h, uint8_t *src){
    int v= *src;
    int r;

    tprintf(h->s.avctx, "rbsp trailing %X\n", v);

    for(r=1; r<9; r++){
        if(v&1) return r;
        v>>=1;
    }
    return 0;
}

/**
 * idct tranforms the 16 dc values and dequantize them.
 * @param qp quantization parameter
 */
static void h264_luma_dc_dequant_idct_c(DCTELEM *block, int qp, int qmul){
#define stride 16
    int i;
    int temp[16]; //FIXME check if this is a good idea
    static const int x_offset[4]={0, 1*stride, 4* stride,  5*stride};
    static const int y_offset[4]={0, 2*stride, 8* stride, 10*stride};

//memset(block, 64, 2*256);
//return;
    for(i=0; i<4; i++){
        const int offset= y_offset[i];
        const int z0= block[offset+stride*0] + block[offset+stride*4];
        const int z1= block[offset+stride*0] - block[offset+stride*4];
        const int z2= block[offset+stride*1] - block[offset+stride*5];
        const int z3= block[offset+stride*1] + block[offset+stride*5];

        temp[4*i+0]= z0+z3;
        temp[4*i+1]= z1+z2;
        temp[4*i+2]= z1-z2;
        temp[4*i+3]= z0-z3;
    }

    for(i=0; i<4; i++){
        const int offset= x_offset[i];
        const int z0= temp[4*0+i] + temp[4*2+i];
        const int z1= temp[4*0+i] - temp[4*2+i];
        const int z2= temp[4*1+i] - temp[4*3+i];
        const int z3= temp[4*1+i] + temp[4*3+i];

        block[stride*0 +offset]= ((((z0 + z3)*qmul + 128 ) >> 8)); //FIXME think about merging this into decode_resdual
        block[stride*2 +offset]= ((((z1 + z2)*qmul + 128 ) >> 8));
        block[stride*8 +offset]= ((((z1 - z2)*qmul + 128 ) >> 8));
        block[stride*10+offset]= ((((z0 - z3)*qmul + 128 ) >> 8));
    }
}

#if 0
/**
 * dct tranforms the 16 dc values.
 * @param qp quantization parameter ??? FIXME
 */
static void h264_luma_dc_dct_c(DCTELEM *block/*, int qp*/){
//    const int qmul= dequant_coeff[qp][0];
    int i;
    int temp[16]; //FIXME check if this is a good idea
    static const int x_offset[4]={0, 1*stride, 4* stride,  5*stride};
    static const int y_offset[4]={0, 2*stride, 8* stride, 10*stride};

    for(i=0; i<4; i++){
        const int offset= y_offset[i];
        const int z0= block[offset+stride*0] + block[offset+stride*4];
        const int z1= block[offset+stride*0] - block[offset+stride*4];
        const int z2= block[offset+stride*1] - block[offset+stride*5];
        const int z3= block[offset+stride*1] + block[offset+stride*5];

        temp[4*i+0]= z0+z3;
        temp[4*i+1]= z1+z2;
        temp[4*i+2]= z1-z2;
        temp[4*i+3]= z0-z3;
    }

    for(i=0; i<4; i++){
        const int offset= x_offset[i];
        const int z0= temp[4*0+i] + temp[4*2+i];
        const int z1= temp[4*0+i] - temp[4*2+i];
        const int z2= temp[4*1+i] - temp[4*3+i];
        const int z3= temp[4*1+i] + temp[4*3+i];

        block[stride*0 +offset]= (z0 + z3)>>1;
        block[stride*2 +offset]= (z1 + z2)>>1;
        block[stride*8 +offset]= (z1 - z2)>>1;
        block[stride*10+offset]= (z0 - z3)>>1;
    }
}
#endif

#undef xStride
#undef stride

static void chroma_dc_dequant_idct_c(DCTELEM *block, int qp, int qmul){
    const int stride= 16*2;
    const int xStride= 16;
    int a,b,c,d,e;

    a= block[stride*0 + xStride*0];
    b= block[stride*0 + xStride*1];
    c= block[stride*1 + xStride*0];
    d= block[stride*1 + xStride*1];

    e= a-b;
    a= a+b;
    b= c-d;
    c= c+d;

    block[stride*0 + xStride*0]= ((a+c)*qmul) >> 7;
    block[stride*0 + xStride*1]= ((e+b)*qmul) >> 7;
    block[stride*1 + xStride*0]= ((a-c)*qmul) >> 7;
    block[stride*1 + xStride*1]= ((e-b)*qmul) >> 7;
}

#if 0
static void chroma_dc_dct_c(DCTELEM *block){
    const int stride= 16*2;
    const int xStride= 16;
    int a,b,c,d,e;

    a= block[stride*0 + xStride*0];
    b= block[stride*0 + xStride*1];
    c= block[stride*1 + xStride*0];
    d= block[stride*1 + xStride*1];

    e= a-b;
    a= a+b;
    b= c-d;
    c= c+d;

    block[stride*0 + xStride*0]= (a+c);
    block[stride*0 + xStride*1]= (e+b);
    block[stride*1 + xStride*0]= (a-c);
    block[stride*1 + xStride*1]= (e-b);
}
#endif

/**
 * gets the chroma qp.
 */
static inline int get_chroma_qp(int chroma_qp_index_offset, int qscale){

    return chroma_qp[av_clip(qscale + chroma_qp_index_offset, 0, 51)];
}

//FIXME need to check that this doesnt overflow signed 32 bit for low qp, i am not sure, it's very close
//FIXME check that gcc inlines this (and optimizes intra & separate_dc stuff away)
static inline int quantize_c(DCTELEM *block, uint8_t *scantable, int qscale, int intra, int separate_dc){
    int i;
    const int * const quant_table= quant_coeff[qscale];
    const int bias= intra ? (1<<QUANT_SHIFT)/3 : (1<<QUANT_SHIFT)/6;
    const unsigned int threshold1= (1<<QUANT_SHIFT) - bias - 1;
    const unsigned int threshold2= (threshold1<<1);
    int last_non_zero;

    if(separate_dc){
        if(qscale<=18){
            //avoid overflows
            const int dc_bias= intra ? (1<<(QUANT_SHIFT-2))/3 : (1<<(QUANT_SHIFT-2))/6;
            const unsigned int dc_threshold1= (1<<(QUANT_SHIFT-2)) - dc_bias - 1;
            const unsigned int dc_threshold2= (dc_threshold1<<1);

            int level= block[0]*quant_coeff[qscale+18][0];
            if(((unsigned)(level+dc_threshold1))>dc_threshold2){
                if(level>0){
                    level= (dc_bias + level)>>(QUANT_SHIFT-2);
                    block[0]= level;
                }else{
                    level= (dc_bias - level)>>(QUANT_SHIFT-2);
                    block[0]= -level;
                }
//                last_non_zero = i;
            }else{
                block[0]=0;
            }
        }else{
            const int dc_bias= intra ? (1<<(QUANT_SHIFT+1))/3 : (1<<(QUANT_SHIFT+1))/6;
            const unsigned int dc_threshold1= (1<<(QUANT_SHIFT+1)) - dc_bias - 1;
            const unsigned int dc_threshold2= (dc_threshold1<<1);

            int level= block[0]*quant_table[0];
            if(((unsigned)(level+dc_threshold1))>dc_threshold2){
                if(level>0){
                    level= (dc_bias + level)>>(QUANT_SHIFT+1);
                    block[0]= level;
                }else{
                    level= (dc_bias - level)>>(QUANT_SHIFT+1);
                    block[0]= -level;
                }
//                last_non_zero = i;
            }else{
                block[0]=0;
            }
        }
        last_non_zero= 0;
        i=1;
    }else{
        last_non_zero= -1;
        i=0;
    }

    for(; i<16; i++){
        const int j= scantable[i];
        int level= block[j]*quant_table[j];

//        if(   bias+level >= (1<<(QMAT_SHIFT - 3))
//           || bias-level >= (1<<(QMAT_SHIFT - 3))){
        if(((unsigned)(level+threshold1))>threshold2){
            if(level>0){
                level= (bias + level)>>QUANT_SHIFT;
                block[j]= level;
            }else{
                level= (bias - level)>>QUANT_SHIFT;
                block[j]= -level;
            }
            last_non_zero = i;
        }else{
            block[j]=0;
        }
    }

    return last_non_zero;
}

static void pred4x4_vertical_c(uint8_t *src, uint8_t *topright, int stride){
    const uint32_t a= ((uint32_t*)(src-stride))[0];
    ((uint32_t*)(src+0*stride))[0]= a;
    ((uint32_t*)(src+1*stride))[0]= a;
    ((uint32_t*)(src+2*stride))[0]= a;
    ((uint32_t*)(src+3*stride))[0]= a;
}

static void pred4x4_horizontal_c(uint8_t *src, uint8_t *topright, int stride){
    ((uint32_t*)(src+0*stride))[0]= src[-1+0*stride]*0x01010101;
    ((uint32_t*)(src+1*stride))[0]= src[-1+1*stride]*0x01010101;
    ((uint32_t*)(src+2*stride))[0]= src[-1+2*stride]*0x01010101;
    ((uint32_t*)(src+3*stride))[0]= src[-1+3*stride]*0x01010101;
}

static void pred4x4_dc_c(uint8_t *src, uint8_t *topright, int stride){
    const int dc= (  src[-stride] + src[1-stride] + src[2-stride] + src[3-stride]
                   + src[-1+0*stride] + src[-1+1*stride] + src[-1+2*stride] + src[-1+3*stride] + 4) >>3;

    ((uint32_t*)(src+0*stride))[0]=
    ((uint32_t*)(src+1*stride))[0]=
    ((uint32_t*)(src+2*stride))[0]=
    ((uint32_t*)(src+3*stride))[0]= dc* 0x01010101;
}

static void pred4x4_left_dc_c(uint8_t *src, uint8_t *topright, int stride){
    const int dc= (  src[-1+0*stride] + src[-1+1*stride] + src[-1+2*stride] + src[-1+3*stride] + 2) >>2;

    ((uint32_t*)(src+0*stride))[0]=
    ((uint32_t*)(src+1*stride))[0]=
    ((uint32_t*)(src+2*stride))[0]=
    ((uint32_t*)(src+3*stride))[0]= dc* 0x01010101;
}

static void pred4x4_top_dc_c(uint8_t *src, uint8_t *topright, int stride){
    const int dc= (  src[-stride] + src[1-stride] + src[2-stride] + src[3-stride] + 2) >>2;

    ((uint32_t*)(src+0*stride))[0]=
    ((uint32_t*)(src+1*stride))[0]=
    ((uint32_t*)(src+2*stride))[0]=
    ((uint32_t*)(src+3*stride))[0]= dc* 0x01010101;
}

static void pred4x4_128_dc_c(uint8_t *src, uint8_t *topright, int stride){
    ((uint32_t*)(src+0*stride))[0]=
    ((uint32_t*)(src+1*stride))[0]=
    ((uint32_t*)(src+2*stride))[0]=
    ((uint32_t*)(src+3*stride))[0]= 128U*0x01010101U;
}


#define LOAD_TOP_RIGHT_EDGE\
    const int t4= topright[0];\
    const int t5= topright[1];\
    const int t6= topright[2];\
    const int t7= topright[3];\

#define LOAD_LEFT_EDGE\
    const int l0= src[-1+0*stride];\
    const int l1= src[-1+1*stride];\
    const int l2= src[-1+2*stride];\
    const int l3= src[-1+3*stride];\

#define LOAD_TOP_EDGE\
    const int t0= src[ 0-1*stride];\
    const int t1= src[ 1-1*stride];\
    const int t2= src[ 2-1*stride];\
    const int t3= src[ 3-1*stride];\

static void pred4x4_down_right_c(uint8_t *src, uint8_t *topright, int stride){
    const int lt= src[-1-1*stride];
    LOAD_TOP_EDGE
    LOAD_LEFT_EDGE

    src[0+3*stride]=(l3 + 2*l2 + l1 + 2)>>2;
    src[0+2*stride]=
    src[1+3*stride]=(l2 + 2*l1 + l0 + 2)>>2;
    src[0+1*stride]=
    src[1+2*stride]=
    src[2+3*stride]=(l1 + 2*l0 + lt + 2)>>2;
    src[0+0*stride]=
    src[1+1*stride]=
    src[2+2*stride]=
    src[3+3*stride]=(l0 + 2*lt + t0 + 2)>>2;
    src[1+0*stride]=
    src[2+1*stride]=
    src[3+2*stride]=(lt + 2*t0 + t1 + 2)>>2;
    src[2+0*stride]=
    src[3+1*stride]=(t0 + 2*t1 + t2 + 2)>>2;
    src[3+0*stride]=(t1 + 2*t2 + t3 + 2)>>2;
}

static void pred4x4_down_left_c(uint8_t *src, uint8_t *topright, int stride){
    LOAD_TOP_EDGE
    LOAD_TOP_RIGHT_EDGE
//    LOAD_LEFT_EDGE

    src[0+0*stride]=(t0 + t2 + 2*t1 + 2)>>2;
    src[1+0*stride]=
    src[0+1*stride]=(t1 + t3 + 2*t2 + 2)>>2;
    src[2+0*stride]=
    src[1+1*stride]=
    src[0+2*stride]=(t2 + t4 + 2*t3 + 2)>>2;
    src[3+0*stride]=
    src[2+1*stride]=
    src[1+2*stride]=
    src[0+3*stride]=(t3 + t5 + 2*t4 + 2)>>2;
    src[3+1*stride]=
    src[2+2*stride]=
    src[1+3*stride]=(t4 + t6 + 2*t5 + 2)>>2;
    src[3+2*stride]=
    src[2+3*stride]=(t5 + t7 + 2*t6 + 2)>>2;
    src[3+3*stride]=(t6 + 3*t7 + 2)>>2;
}

static void pred4x4_vertical_right_c(uint8_t *src, uint8_t *topright, int stride){
    const int lt= src[-1-1*stride];
    LOAD_TOP_EDGE
    LOAD_LEFT_EDGE
    const __attribute__((unused)) int unu= l3;

    src[0+0*stride]=
    src[1+2*stride]=(lt + t0 + 1)>>1;
    src[1+0*stride]=
    src[2+2*stride]=(t0 + t1 + 1)>>1;
    src[2+0*stride]=
    src[3+2*stride]=(t1 + t2 + 1)>>1;
    src[3+0*stride]=(t2 + t3 + 1)>>1;
    src[0+1*stride]=
    src[1+3*stride]=(l0 + 2*lt + t0 + 2)>>2;
    src[1+1*stride]=
    src[2+3*stride]=(lt + 2*t0 + t1 + 2)>>2;
    src[2+1*stride]=
    src[3+3*stride]=(t0 + 2*t1 + t2 + 2)>>2;
    src[3+1*stride]=(t1 + 2*t2 + t3 + 2)>>2;
    src[0+2*stride]=(lt + 2*l0 + l1 + 2)>>2;
    src[0+3*stride]=(l0 + 2*l1 + l2 + 2)>>2;
}

static void pred4x4_vertical_left_c(uint8_t *src, uint8_t *topright, int stride){
    LOAD_TOP_EDGE
    LOAD_TOP_RIGHT_EDGE
    const __attribute__((unused)) int unu= t7;

    src[0+0*stride]=(t0 + t1 + 1)>>1;
    src[1+0*stride]=
    src[0+2*stride]=(t1 + t2 + 1)>>1;
    src[2+0*stride]=
    src[1+2*stride]=(t2 + t3 + 1)>>1;
    src[3+0*stride]=
    src[2+2*stride]=(t3 + t4+ 1)>>1;
    src[3+2*stride]=(t4 + t5+ 1)>>1;
    src[0+1*stride]=(t0 + 2*t1 + t2 + 2)>>2;
    src[1+1*stride]=
    src[0+3*stride]=(t1 + 2*t2 + t3 + 2)>>2;
    src[2+1*stride]=
    src[1+3*stride]=(t2 + 2*t3 + t4 + 2)>>2;
    src[3+1*stride]=
    src[2+3*stride]=(t3 + 2*t4 + t5 + 2)>>2;
    src[3+3*stride]=(t4 + 2*t5 + t6 + 2)>>2;
}

static void pred4x4_horizontal_up_c(uint8_t *src, uint8_t *topright, int stride){
    LOAD_LEFT_EDGE

    src[0+0*stride]=(l0 + l1 + 1)>>1;
    src[1+0*stride]=(l0 + 2*l1 + l2 + 2)>>2;
    src[2+0*stride]=
    src[0+1*stride]=(l1 + l2 + 1)>>1;
    src[3+0*stride]=
    src[1+1*stride]=(l1 + 2*l2 + l3 + 2)>>2;
    src[2+1*stride]=
    src[0+2*stride]=(l2 + l3 + 1)>>1;
    src[3+1*stride]=
    src[1+2*stride]=(l2 + 2*l3 + l3 + 2)>>2;
    src[3+2*stride]=
    src[1+3*stride]=
    src[0+3*stride]=
    src[2+2*stride]=
    src[2+3*stride]=
    src[3+3*stride]=l3;
}

static void pred4x4_horizontal_down_c(uint8_t *src, uint8_t *topright, int stride){
    const int lt= src[-1-1*stride];
    LOAD_TOP_EDGE
    LOAD_LEFT_EDGE
    const __attribute__((unused)) int unu= t3;

    src[0+0*stride]=
    src[2+1*stride]=(lt + l0 + 1)>>1;
    src[1+0*stride]=
    src[3+1*stride]=(l0 + 2*lt + t0 + 2)>>2;
    src[2+0*stride]=(lt + 2*t0 + t1 + 2)>>2;
    src[3+0*stride]=(t0 + 2*t1 + t2 + 2)>>2;
    src[0+1*stride]=
    src[2+2*stride]=(l0 + l1 + 1)>>1;
    src[1+1*stride]=
    src[3+2*stride]=(lt + 2*l0 + l1 + 2)>>2;
    src[0+2*stride]=
    src[2+3*stride]=(l1 + l2+ 1)>>1;
    src[1+2*stride]=
    src[3+3*stride]=(l0 + 2*l1 + l2 + 2)>>2;
    src[0+3*stride]=(l2 + l3 + 1)>>1;
    src[1+3*stride]=(l1 + 2*l2 + l3 + 2)>>2;
}

void ff_pred16x16_vertical_c(uint8_t *src, int stride){
    int i;
    const uint32_t a= ((uint32_t*)(src-stride))[0];
    const uint32_t b= ((uint32_t*)(src-stride))[1];
    const uint32_t c= ((uint32_t*)(src-stride))[2];
    const uint32_t d= ((uint32_t*)(src-stride))[3];

    for(i=0; i<16; i++){
        ((uint32_t*)(src+i*stride))[0]= a;
        ((uint32_t*)(src+i*stride))[1]= b;
        ((uint32_t*)(src+i*stride))[2]= c;
        ((uint32_t*)(src+i*stride))[3]= d;
    }
}

void ff_pred16x16_horizontal_c(uint8_t *src, int stride){
    int i;

    for(i=0; i<16; i++){
        ((uint32_t*)(src+i*stride))[0]=
        ((uint32_t*)(src+i*stride))[1]=
        ((uint32_t*)(src+i*stride))[2]=
        ((uint32_t*)(src+i*stride))[3]= src[-1+i*stride]*0x01010101;
    }
}

void ff_pred16x16_dc_c(uint8_t *src, int stride){
    int i, dc=0;

    for(i=0;i<16; i++){
        dc+= src[-1+i*stride];
    }

    for(i=0;i<16; i++){
        dc+= src[i-stride];
    }

    dc= 0x01010101*((dc + 16)>>5);

    for(i=0; i<16; i++){
        ((uint32_t*)(src+i*stride))[0]=
        ((uint32_t*)(src+i*stride))[1]=
        ((uint32_t*)(src+i*stride))[2]=
        ((uint32_t*)(src+i*stride))[3]= dc;
    }
}

static void pred16x16_left_dc_c(uint8_t *src, int stride){
    int i, dc=0;

    for(i=0;i<16; i++){
        dc+= src[-1+i*stride];
    }

    dc= 0x01010101*((dc + 8)>>4);

    for(i=0; i<16; i++){
        ((uint32_t*)(src+i*stride))[0]=
        ((uint32_t*)(src+i*stride))[1]=
        ((uint32_t*)(src+i*stride))[2]=
        ((uint32_t*)(src+i*stride))[3]= dc;
    }
}

static void pred16x16_top_dc_c(uint8_t *src, int stride){
    int i, dc=0;

    for(i=0;i<16; i++){
        dc+= src[i-stride];
    }
    dc= 0x01010101*((dc + 8)>>4);

    for(i=0; i<16; i++){
        ((uint32_t*)(src+i*stride))[0]=
        ((uint32_t*)(src+i*stride))[1]=
        ((uint32_t*)(src+i*stride))[2]=
        ((uint32_t*)(src+i*stride))[3]= dc;
    }
}

void ff_pred16x16_128_dc_c(uint8_t *src, int stride){
    int i;

    for(i=0; i<16; i++){
        ((uint32_t*)(src+i*stride))[0]=
        ((uint32_t*)(src+i*stride))[1]=
        ((uint32_t*)(src+i*stride))[2]=
        ((uint32_t*)(src+i*stride))[3]= 0x01010101U*128U;
    }
}

static inline void pred16x16_plane_compat_c(uint8_t *src, int stride, const int svq3){
  int i, j, k;
  int a;
  uint8_t *cm = ff_cropTbl + MAX_NEG_CROP;
  const uint8_t * const src0 = src+7-stride;
  const uint8_t *src1 = src+8*stride-1;
  const uint8_t *src2 = src1-2*stride;      // == src+6*stride-1;
  int H = src0[1] - src0[-1];
  int V = src1[0] - src2[ 0];
  for(k=2; k<=8; ++k) {
    src1 += stride; src2 -= stride;
    H += k*(src0[k] - src0[-k]);
    V += k*(src1[0] - src2[ 0]);
  }
  if(svq3){
    H = ( 5*(H/4) ) / 16;
    V = ( 5*(V/4) ) / 16;

    /* required for 100% accuracy */
    i = H; H = V; V = i;
  }else{
    H = ( 5*H+32 ) >> 6;
    V = ( 5*V+32 ) >> 6;
  }

  a = 16*(src1[0] + src2[16] + 1) - 7*(V+H);
  for(j=16; j>0; --j) {
    int b = a;
    a += V;
    for(i=-16; i<0; i+=4) {
      src[16+i] = cm[ (b    ) >> 5 ];
      src[17+i] = cm[ (b+  H) >> 5 ];
      src[18+i] = cm[ (b+2*H) >> 5 ];
      src[19+i] = cm[ (b+3*H) >> 5 ];
      b += 4*H;
    }
    src += stride;
  }
}

void ff_pred16x16_plane_c(uint8_t *src, int stride){
    pred16x16_plane_compat_c(src, stride, 0);
}

void ff_pred8x8_vertical_c(uint8_t *src, int stride){
    int i;
    const uint32_t a= ((uint32_t*)(src-stride))[0];
    const uint32_t b= ((uint32_t*)(src-stride))[1];

    for(i=0; i<8; i++){
        ((uint32_t*)(src+i*stride))[0]= a;
        ((uint32_t*)(src+i*stride))[1]= b;
    }
}

void ff_pred8x8_horizontal_c(uint8_t *src, int stride){
    int i;

    for(i=0; i<8; i++){
        ((uint32_t*)(src+i*stride))[0]=
        ((uint32_t*)(src+i*stride))[1]= src[-1+i*stride]*0x01010101;
    }
}

void ff_pred8x8_128_dc_c(uint8_t *src, int stride){
    int i;

    for(i=0; i<8; i++){
        ((uint32_t*)(src+i*stride))[0]=
        ((uint32_t*)(src+i*stride))[1]= 0x01010101U*128U;
    }
}

static void pred8x8_left_dc_c(uint8_t *src, int stride){
    int i;
    int dc0, dc2;

    dc0=dc2=0;
    for(i=0;i<4; i++){
        dc0+= src[-1+i*stride];
        dc2+= src[-1+(i+4)*stride];
    }
    dc0= 0x01010101*((dc0 + 2)>>2);
    dc2= 0x01010101*((dc2 + 2)>>2);

    for(i=0; i<4; i++){
        ((uint32_t*)(src+i*stride))[0]=
        ((uint32_t*)(src+i*stride))[1]= dc0;
    }
    for(i=4; i<8; i++){
        ((uint32_t*)(src+i*stride))[0]=
        ((uint32_t*)(src+i*stride))[1]= dc2;
    }
}

static void pred8x8_top_dc_c(uint8_t *src, int stride){
    int i;
    int dc0, dc1;

    dc0=dc1=0;
    for(i=0;i<4; i++){
        dc0+= src[i-stride];
        dc1+= src[4+i-stride];
    }
    dc0= 0x01010101*((dc0 + 2)>>2);
    dc1= 0x01010101*((dc1 + 2)>>2);

    for(i=0; i<4; i++){
        ((uint32_t*)(src+i*stride))[0]= dc0;
        ((uint32_t*)(src+i*stride))[1]= dc1;
    }
    for(i=4; i<8; i++){
        ((uint32_t*)(src+i*stride))[0]= dc0;
        ((uint32_t*)(src+i*stride))[1]= dc1;
    }
}


void ff_pred8x8_dc_c(uint8_t *src, int stride){
    int i;
    int dc0, dc1, dc2, dc3;

    dc0=dc1=dc2=0;
    for(i=0;i<4; i++){
        dc0+= src[-1+i*stride] + src[i-stride];
        dc1+= src[4+i-stride];
        dc2+= src[-1+(i+4)*stride];
    }
    dc3= 0x01010101*((dc1 + dc2 + 4)>>3);
    dc0= 0x01010101*((dc0 + 4)>>3);
    dc1= 0x01010101*((dc1 + 2)>>2);
    dc2= 0x01010101*((dc2 + 2)>>2);

    for(i=0; i<4; i++){
        ((uint32_t*)(src+i*stride))[0]= dc0;
        ((uint32_t*)(src+i*stride))[1]= dc1;
    }
    for(i=4; i<8; i++){
        ((uint32_t*)(src+i*stride))[0]= dc2;
        ((uint32_t*)(src+i*stride))[1]= dc3;
    }
}

void ff_pred8x8_plane_c(uint8_t *src, int stride){
  int j, k;
  int a;
  uint8_t *cm = ff_cropTbl + MAX_NEG_CROP;
  const uint8_t * const src0 = src+3-stride;
  const uint8_t *src1 = src+4*stride-1;
  const uint8_t *src2 = src1-2*stride;      // == src+2*stride-1;
  int H = src0[1] - src0[-1];
  int V = src1[0] - src2[ 0];
  for(k=2; k<=4; ++k) {
    src1 += stride; src2 -= stride;
    H += k*(src0[k] - src0[-k]);
    V += k*(src1[0] - src2[ 0]);
  }
  H = ( 17*H+16 ) >> 5;
  V = ( 17*V+16 ) >> 5;

  a = 16*(src1[0] + src2[8]+1) - 3*(V+H);
  for(j=8; j>0; --j) {
    int b = a;
    a += V;
    src[0] = cm[ (b    ) >> 5 ];
    src[1] = cm[ (b+  H) >> 5 ];
    src[2] = cm[ (b+2*H) >> 5 ];
    src[3] = cm[ (b+3*H) >> 5 ];
    src[4] = cm[ (b+4*H) >> 5 ];
    src[5] = cm[ (b+5*H) >> 5 ];
    src[6] = cm[ (b+6*H) >> 5 ];
    src[7] = cm[ (b+7*H) >> 5 ];
    src += stride;
  }
}

#define SRC(x,y) src[(x)+(y)*stride]
#define PL(y) \
    const int l##y = (SRC(-1,y-1) + 2*SRC(-1,y) + SRC(-1,y+1) + 2) >> 2;
#define PREDICT_8x8_LOAD_LEFT \
    const int l0 = ((has_topleft ? SRC(-1,-1) : SRC(-1,0)) \
                     + 2*SRC(-1,0) + SRC(-1,1) + 2) >> 2; \
    PL(1) PL(2) PL(3) PL(4) PL(5) PL(6) \
    const int l7 attribute_unused = (SRC(-1,6) + 3*SRC(-1,7) + 2) >> 2

#define PT(x) \
    const int t##x = (SRC(x-1,-1) + 2*SRC(x,-1) + SRC(x+1,-1) + 2) >> 2;
#define PREDICT_8x8_LOAD_TOP \
    const int t0 = ((has_topleft ? SRC(-1,-1) : SRC(0,-1)) \
                     + 2*SRC(0,-1) + SRC(1,-1) + 2) >> 2; \
    PT(1) PT(2) PT(3) PT(4) PT(5) PT(6) \
    const int t7 attribute_unused = ((has_topright ? SRC(8,-1) : SRC(7,-1)) \
                     + 2*SRC(7,-1) + SRC(6,-1) + 2) >> 2

#define PTR(x) \
    t##x = (SRC(x-1,-1) + 2*SRC(x,-1) + SRC(x+1,-1) + 2) >> 2;
#define PREDICT_8x8_LOAD_TOPRIGHT \
    int t8, t9, t10, t11, t12, t13, t14, t15; \
    if(has_topright) { \
        PTR(8) PTR(9) PTR(10) PTR(11) PTR(12) PTR(13) PTR(14) \
        t15 = (SRC(14,-1) + 3*SRC(15,-1) + 2) >> 2; \
    } else t8=t9=t10=t11=t12=t13=t14=t15= SRC(7,-1);

#define PREDICT_8x8_LOAD_TOPLEFT \
    const int lt = (SRC(-1,0) + 2*SRC(-1,-1) + SRC(0,-1) + 2) >> 2

#define PREDICT_8x8_DC(v) \
    int y; \
    for( y = 0; y < 8; y++ ) { \
        ((uint32_t*)src)[0] = \
        ((uint32_t*)src)[1] = v; \
        src += stride; \
    }

static void pred8x8l_128_dc_c(uint8_t *src, int has_topleft, int has_topright, int stride)
{
    PREDICT_8x8_DC(0x80808080);
}
static void pred8x8l_left_dc_c(uint8_t *src, int has_topleft, int has_topright, int stride)
{
    PREDICT_8x8_LOAD_LEFT;
    const uint32_t dc = ((l0+l1+l2+l3+l4+l5+l6+l7+4) >> 3) * 0x01010101;
    PREDICT_8x8_DC(dc);
}
static void pred8x8l_top_dc_c(uint8_t *src, int has_topleft, int has_topright, int stride)
{
    PREDICT_8x8_LOAD_TOP;
    const uint32_t dc = ((t0+t1+t2+t3+t4+t5+t6+t7+4) >> 3) * 0x01010101;
    PREDICT_8x8_DC(dc);
}
static void pred8x8l_dc_c(uint8_t *src, int has_topleft, int has_topright, int stride)
{
    PREDICT_8x8_LOAD_LEFT;
    PREDICT_8x8_LOAD_TOP;
    const uint32_t dc = ((l0+l1+l2+l3+l4+l5+l6+l7
                         +t0+t1+t2+t3+t4+t5+t6+t7+8) >> 4) * 0x01010101;
    PREDICT_8x8_DC(dc);
}
static void pred8x8l_horizontal_c(uint8_t *src, int has_topleft, int has_topright, int stride)
{
    PREDICT_8x8_LOAD_LEFT;
#define ROW(y) ((uint32_t*)(src+y*stride))[0] =\
               ((uint32_t*)(src+y*stride))[1] = 0x01010101 * l##y
    ROW(0); ROW(1); ROW(2); ROW(3); ROW(4); ROW(5); ROW(6); ROW(7);
#undef ROW
}
static void pred8x8l_vertical_c(uint8_t *src, int has_topleft, int has_topright, int stride)
{
    int y;
    PREDICT_8x8_LOAD_TOP;
    src[0] = t0;
    src[1] = t1;
    src[2] = t2;
    src[3] = t3;
    src[4] = t4;
    src[5] = t5;
    src[6] = t6;
    src[7] = t7;
    for( y = 1; y < 8; y++ )
        *(uint64_t*)(src+y*stride) = *(uint64_t*)src;
}
static void pred8x8l_down_left_c(uint8_t *src, int has_topleft, int has_topright, int stride)
{
    PREDICT_8x8_LOAD_TOP;
    PREDICT_8x8_LOAD_TOPRIGHT;
    SRC(0,0)= (t0 + 2*t1 + t2 + 2) >> 2;
    SRC(0,1)=SRC(1,0)= (t1 + 2*t2 + t3 + 2) >> 2;
    SRC(0,2)=SRC(1,1)=SRC(2,0)= (t2 + 2*t3 + t4 + 2) >> 2;
    SRC(0,3)=SRC(1,2)=SRC(2,1)=SRC(3,0)= (t3 + 2*t4 + t5 + 2) >> 2;
    SRC(0,4)=SRC(1,3)=SRC(2,2)=SRC(3,1)=SRC(4,0)= (t4 + 2*t5 + t6 + 2) >> 2;
    SRC(0,5)=SRC(1,4)=SRC(2,3)=SRC(3,2)=SRC(4,1)=SRC(5,0)= (t5 + 2*t6 + t7 + 2) >> 2;
    SRC(0,6)=SRC(1,5)=SRC(2,4)=SRC(3,3)=SRC(4,2)=SRC(5,1)=SRC(6,0)= (t6 + 2*t7 + t8 + 2) >> 2;
    SRC(0,7)=SRC(1,6)=SRC(2,5)=SRC(3,4)=SRC(4,3)=SRC(5,2)=SRC(6,1)=SRC(7,0)= (t7 + 2*t8 + t9 + 2) >> 2;
    SRC(1,7)=SRC(2,6)=SRC(3,5)=SRC(4,4)=SRC(5,3)=SRC(6,2)=SRC(7,1)= (t8 + 2*t9 + t10 + 2) >> 2;
    SRC(2,7)=SRC(3,6)=SRC(4,5)=SRC(5,4)=SRC(6,3)=SRC(7,2)= (t9 + 2*t10 + t11 + 2) >> 2;
    SRC(3,7)=SRC(4,6)=SRC(5,5)=SRC(6,4)=SRC(7,3)= (t10 + 2*t11 + t12 + 2) >> 2;
    SRC(4,7)=SRC(5,6)=SRC(6,5)=SRC(7,4)= (t11 + 2*t12 + t13 + 2) >> 2;
    SRC(5,7)=SRC(6,6)=SRC(7,5)= (t12 + 2*t13 + t14 + 2) >> 2;
    SRC(6,7)=SRC(7,6)= (t13 + 2*t14 + t15 + 2) >> 2;
    SRC(7,7)= (t14 + 3*t15 + 2) >> 2;
}
static void pred8x8l_down_right_c(uint8_t *src, int has_topleft, int has_topright, int stride)
{
    PREDICT_8x8_LOAD_TOP;
    PREDICT_8x8_LOAD_LEFT;
    PREDICT_8x8_LOAD_TOPLEFT;
    SRC(0,7)= (l7 + 2*l6 + l5 + 2) >> 2;
    SRC(0,6)=SRC(1,7)= (l6 + 2*l5 + l4 + 2) >> 2;
    SRC(0,5)=SRC(1,6)=SRC(2,7)= (l5 + 2*l4 + l3 + 2) >> 2;
    SRC(0,4)=SRC(1,5)=SRC(2,6)=SRC(3,7)= (l4 + 2*l3 + l2 + 2) >> 2;
    SRC(0,3)=SRC(1,4)=SRC(2,5)=SRC(3,6)=SRC(4,7)= (l3 + 2*l2 + l1 + 2) >> 2;
    SRC(0,2)=SRC(1,3)=SRC(2,4)=SRC(3,5)=SRC(4,6)=SRC(5,7)= (l2 + 2*l1 + l0 + 2) >> 2;
    SRC(0,1)=SRC(1,2)=SRC(2,3)=SRC(3,4)=SRC(4,5)=SRC(5,6)=SRC(6,7)= (l1 + 2*l0 + lt + 2) >> 2;
    SRC(0,0)=SRC(1,1)=SRC(2,2)=SRC(3,3)=SRC(4,4)=SRC(5,5)=SRC(6,6)=SRC(7,7)= (l0 + 2*lt + t0 + 2) >> 2;
    SRC(1,0)=SRC(2,1)=SRC(3,2)=SRC(4,3)=SRC(5,4)=SRC(6,5)=SRC(7,6)= (lt + 2*t0 + t1 + 2) >> 2;
    SRC(2,0)=SRC(3,1)=SRC(4,2)=SRC(5,3)=SRC(6,4)=SRC(7,5)= (t0 + 2*t1 + t2 + 2) >> 2;
    SRC(3,0)=SRC(4,1)=SRC(5,2)=SRC(6,3)=SRC(7,4)= (t1 + 2*t2 + t3 + 2) >> 2;
    SRC(4,0)=SRC(5,1)=SRC(6,2)=SRC(7,3)= (t2 + 2*t3 + t4 + 2) >> 2;
    SRC(5,0)=SRC(6,1)=SRC(7,2)= (t3 + 2*t4 + t5 + 2) >> 2;
    SRC(6,0)=SRC(7,1)= (t4 + 2*t5 + t6 + 2) >> 2;
    SRC(7,0)= (t5 + 2*t6 + t7 + 2) >> 2;

}
static void pred8x8l_vertical_right_c(uint8_t *src, int has_topleft, int has_topright, int stride)
{
    PREDICT_8x8_LOAD_TOP;
    PREDICT_8x8_LOAD_LEFT;
    PREDICT_8x8_LOAD_TOPLEFT;
    SRC(0,6)= (l5 + 2*l4 + l3 + 2) >> 2;
    SRC(0,7)= (l6 + 2*l5 + l4 + 2) >> 2;
    SRC(0,4)=SRC(1,6)= (l3 + 2*l2 + l1 + 2) >> 2;
    SRC(0,5)=SRC(1,7)= (l4 + 2*l3 + l2 + 2) >> 2;
    SRC(0,2)=SRC(1,4)=SRC(2,6)= (l1 + 2*l0 + lt + 2) >> 2;
    SRC(0,3)=SRC(1,5)=SRC(2,7)= (l2 + 2*l1 + l0 + 2) >> 2;
    SRC(0,1)=SRC(1,3)=SRC(2,5)=SRC(3,7)= (l0 + 2*lt + t0 + 2) >> 2;
    SRC(0,0)=SRC(1,2)=SRC(2,4)=SRC(3,6)= (lt + t0 + 1) >> 1;
    SRC(1,1)=SRC(2,3)=SRC(3,5)=SRC(4,7)= (lt + 2*t0 + t1 + 2) >> 2;
    SRC(1,0)=SRC(2,2)=SRC(3,4)=SRC(4,6)= (t0 + t1 + 1) >> 1;
    SRC(2,1)=SRC(3,3)=SRC(4,5)=SRC(5,7)= (t0 + 2*t1 + t2 + 2) >> 2;
    SRC(2,0)=SRC(3,2)=SRC(4,4)=SRC(5,6)= (t1 + t2 + 1) >> 1;
    SRC(3,1)=SRC(4,3)=SRC(5,5)=SRC(6,7)= (t1 + 2*t2 + t3 + 2) >> 2;
    SRC(3,0)=SRC(4,2)=SRC(5,4)=SRC(6,6)= (t2 + t3 + 1) >> 1;
    SRC(4,1)=SRC(5,3)=SRC(6,5)=SRC(7,7)= (t2 + 2*t3 + t4 + 2) >> 2;
    SRC(4,0)=SRC(5,2)=SRC(6,4)=SRC(7,6)= (t3 + t4 + 1) >> 1;
    SRC(5,1)=SRC(6,3)=SRC(7,5)= (t3 + 2*t4 + t5 + 2) >> 2;
    SRC(5,0)=SRC(6,2)=SRC(7,4)= (t4 + t5 + 1) >> 1;
    SRC(6,1)=SRC(7,3)= (t4 + 2*t5 + t6 + 2) >> 2;
    SRC(6,0)=SRC(7,2)= (t5 + t6 + 1) >> 1;
    SRC(7,1)= (t5 + 2*t6 + t7 + 2) >> 2;
    SRC(7,0)= (t6 + t7 + 1) >> 1;
}
static void pred8x8l_horizontal_down_c(uint8_t *src, int has_topleft, int has_topright, int stride)
{
    PREDICT_8x8_LOAD_TOP;
    PREDICT_8x8_LOAD_LEFT;
    PREDICT_8x8_LOAD_TOPLEFT;
    SRC(0,7)= (l6 + l7 + 1) >> 1;
    SRC(1,7)= (l5 + 2*l6 + l7 + 2) >> 2;
    SRC(0,6)=SRC(2,7)= (l5 + l6 + 1) >> 1;
    SRC(1,6)=SRC(3,7)= (l4 + 2*l5 + l6 + 2) >> 2;
    SRC(0,5)=SRC(2,6)=SRC(4,7)= (l4 + l5 + 1) >> 1;
    SRC(1,5)=SRC(3,6)=SRC(5,7)= (l3 + 2*l4 + l5 + 2) >> 2;
    SRC(0,4)=SRC(2,5)=SRC(4,6)=SRC(6,7)= (l3 + l4 + 1) >> 1;
    SRC(1,4)=SRC(3,5)=SRC(5,6)=SRC(7,7)= (l2 + 2*l3 + l4 + 2) >> 2;
    SRC(0,3)=SRC(2,4)=SRC(4,5)=SRC(6,6)= (l2 + l3 + 1) >> 1;
    SRC(1,3)=SRC(3,4)=SRC(5,5)=SRC(7,6)= (l1 + 2*l2 + l3 + 2) >> 2;
    SRC(0,2)=SRC(2,3)=SRC(4,4)=SRC(6,5)= (l1 + l2 + 1) >> 1;
    SRC(1,2)=SRC(3,3)=SRC(5,4)=SRC(7,5)= (l0 + 2*l1 + l2 + 2) >> 2;
    SRC(0,1)=SRC(2,2)=SRC(4,3)=SRC(6,4)= (l0 + l1 + 1) >> 1;
    SRC(1,1)=SRC(3,2)=SRC(5,3)=SRC(7,4)= (lt + 2*l0 + l1 + 2) >> 2;
    SRC(0,0)=SRC(2,1)=SRC(4,2)=SRC(6,3)= (lt + l0 + 1) >> 1;
    SRC(1,0)=SRC(3,1)=SRC(5,2)=SRC(7,3)= (l0 + 2*lt + t0 + 2) >> 2;
    SRC(2,0)=SRC(4,1)=SRC(6,2)= (t1 + 2*t0 + lt + 2) >> 2;
    SRC(3,0)=SRC(5,1)=SRC(7,2)= (t2 + 2*t1 + t0 + 2) >> 2;
    SRC(4,0)=SRC(6,1)= (t3 + 2*t2 + t1 + 2) >> 2;
    SRC(5,0)=SRC(7,1)= (t4 + 2*t3 + t2 + 2) >> 2;
    SRC(6,0)= (t5 + 2*t4 + t3 + 2) >> 2;
    SRC(7,0)= (t6 + 2*t5 + t4 + 2) >> 2;
}
static void pred8x8l_vertical_left_c(uint8_t *src, int has_topleft, int has_topright, int stride)
{
    PREDICT_8x8_LOAD_TOP;
    PREDICT_8x8_LOAD_TOPRIGHT;
    SRC(0,0)= (t0 + t1 + 1) >> 1;
    SRC(0,1)= (t0 + 2*t1 + t2 + 2) >> 2;
    SRC(0,2)=SRC(1,0)= (t1 + t2 + 1) >> 1;
    SRC(0,3)=SRC(1,1)= (t1 + 2*t2 + t3 + 2) >> 2;
    SRC(0,4)=SRC(1,2)=SRC(2,0)= (t2 + t3 + 1) >> 1;
    SRC(0,5)=SRC(1,3)=SRC(2,1)= (t2 + 2*t3 + t4 + 2) >> 2;
    SRC(0,6)=SRC(1,4)=SRC(2,2)=SRC(3,0)= (t3 + t4 + 1) >> 1;
    SRC(0,7)=SRC(1,5)=SRC(2,3)=SRC(3,1)= (t3 + 2*t4 + t5 + 2) >> 2;
    SRC(1,6)=SRC(2,4)=SRC(3,2)=SRC(4,0)= (t4 + t5 + 1) >> 1;
    SRC(1,7)=SRC(2,5)=SRC(3,3)=SRC(4,1)= (t4 + 2*t5 + t6 + 2) >> 2;
    SRC(2,6)=SRC(3,4)=SRC(4,2)=SRC(5,0)= (t5 + t6 + 1) >> 1;
    SRC(2,7)=SRC(3,5)=SRC(4,3)=SRC(5,1)= (t5 + 2*t6 + t7 + 2) >> 2;
    SRC(3,6)=SRC(4,4)=SRC(5,2)=SRC(6,0)= (t6 + t7 + 1) >> 1;
    SRC(3,7)=SRC(4,5)=SRC(5,3)=SRC(6,1)= (t6 + 2*t7 + t8 + 2) >> 2;
    SRC(4,6)=SRC(5,4)=SRC(6,2)=SRC(7,0)= (t7 + t8 + 1) >> 1;
    SRC(4,7)=SRC(5,5)=SRC(6,3)=SRC(7,1)= (t7 + 2*t8 + t9 + 2) >> 2;
    SRC(5,6)=SRC(6,4)=SRC(7,2)= (t8 + t9 + 1) >> 1;
    SRC(5,7)=SRC(6,5)=SRC(7,3)= (t8 + 2*t9 + t10 + 2) >> 2;
    SRC(6,6)=SRC(7,4)= (t9 + t10 + 1) >> 1;
    SRC(6,7)=SRC(7,5)= (t9 + 2*t10 + t11 + 2) >> 2;
    SRC(7,6)= (t10 + t11 + 1) >> 1;
    SRC(7,7)= (t10 + 2*t11 + t12 + 2) >> 2;
}
static void pred8x8l_horizontal_up_c(uint8_t *src, int has_topleft, int has_topright, int stride)
{
    PREDICT_8x8_LOAD_LEFT;
    SRC(0,0)= (l0 + l1 + 1) >> 1;
    SRC(1,0)= (l0 + 2*l1 + l2 + 2) >> 2;
    SRC(0,1)=SRC(2,0)= (l1 + l2 + 1) >> 1;
    SRC(1,1)=SRC(3,0)= (l1 + 2*l2 + l3 + 2) >> 2;
    SRC(0,2)=SRC(2,1)=SRC(4,0)= (l2 + l3 + 1) >> 1;
    SRC(1,2)=SRC(3,1)=SRC(5,0)= (l2 + 2*l3 + l4 + 2) >> 2;
    SRC(0,3)=SRC(2,2)=SRC(4,1)=SRC(6,0)= (l3 + l4 + 1) >> 1;
    SRC(1,3)=SRC(3,2)=SRC(5,1)=SRC(7,0)= (l3 + 2*l4 + l5 + 2) >> 2;
    SRC(0,4)=SRC(2,3)=SRC(4,2)=SRC(6,1)= (l4 + l5 + 1) >> 1;
    SRC(1,4)=SRC(3,3)=SRC(5,2)=SRC(7,1)= (l4 + 2*l5 + l6 + 2) >> 2;
    SRC(0,5)=SRC(2,4)=SRC(4,3)=SRC(6,2)= (l5 + l6 + 1) >> 1;
    SRC(1,5)=SRC(3,4)=SRC(5,3)=SRC(7,2)= (l5 + 2*l6 + l7 + 2) >> 2;
    SRC(0,6)=SRC(2,5)=SRC(4,4)=SRC(6,3)= (l6 + l7 + 1) >> 1;
    SRC(1,6)=SRC(3,5)=SRC(5,4)=SRC(7,3)= (l6 + 3*l7 + 2) >> 2;
    SRC(0,7)=SRC(1,7)=SRC(2,6)=SRC(2,7)=SRC(3,6)=
    SRC(3,7)=SRC(4,5)=SRC(4,6)=SRC(4,7)=SRC(5,5)=
    SRC(5,6)=SRC(5,7)=SRC(6,4)=SRC(6,5)=SRC(6,6)=
    SRC(6,7)=SRC(7,4)=SRC(7,5)=SRC(7,6)=SRC(7,7)= l7;
}
#undef PREDICT_8x8_LOAD_LEFT
#undef PREDICT_8x8_LOAD_TOP
#undef PREDICT_8x8_LOAD_TOPLEFT
#undef PREDICT_8x8_LOAD_TOPRIGHT
#undef PREDICT_8x8_DC
#undef PTR
#undef PT
#undef PL
#undef SRC

static inline void mc_dir_part(H264Context *h, Picture *pic, int n, int square, int chroma_height, int delta, int list,
                           uint8_t *dest_y, uint8_t *dest_cb, uint8_t *dest_cr,
                           int src_x_offset, int src_y_offset,
                           qpel_mc_func *qpix_op, h264_chroma_mc_func chroma_op){
    MpegEncContext * const s = &h->s;
    const int mx= h->mv_cache[list][ scan8[n] ][0] + src_x_offset*8;
    int my=       h->mv_cache[list][ scan8[n] ][1] + src_y_offset*8;
    const int luma_xy= (mx&3) + ((my&3)<<2);
    uint8_t * src_y = pic->data[0] + (mx>>2) + (my>>2)*h->mb_linesize;
    uint8_t * src_cb, * src_cr;
    int extra_width= h->emu_edge_width;
    int extra_height= h->emu_edge_height;
    int emu=0;
    const int full_mx= mx>>2;
    const int full_my= my>>2;
    const int pic_width  = 16*s->mb_width;
    const int pic_height = 16*s->mb_height >> MB_MBAFF;

    if(!pic->data[0]) //FIXME this is unacceptable, some senseable error concealment must be done for missing reference frames
        return;

    if(mx&7) extra_width -= 3;
    if(my&7) extra_height -= 3;

    if(   full_mx < 0-extra_width
       || full_my < 0-extra_height
       || full_mx + 16/*FIXME*/ > pic_width + extra_width
       || full_my + 16/*FIXME*/ > pic_height + extra_height){
        ff_emulated_edge_mc(s->edge_emu_buffer, src_y - 2 - 2*h->mb_linesize, h->mb_linesize, 16+5, 16+5/*FIXME*/, full_mx-2, full_my-2, pic_width, pic_height);
            src_y= s->edge_emu_buffer + 2 + 2*h->mb_linesize;
        emu=1;
    }

    qpix_op[luma_xy](dest_y, src_y, h->mb_linesize); //FIXME try variable height perhaps?
    if(!square){
        qpix_op[luma_xy](dest_y + delta, src_y + delta, h->mb_linesize);
    }

    if(s->flags&CODEC_FLAG_GRAY) return;

    if(MB_MBAFF){
        // chroma offset when predicting from a field of opposite parity
        my += 2 * ((s->mb_y & 1) - (h->ref_cache[list][scan8[n]] & 1));
        emu |= (my>>3) < 0 || (my>>3) + 8 >= (pic_height>>1);
    }
    src_cb= pic->data[1] + (mx>>3) + (my>>3)*h->mb_uvlinesize;
    src_cr= pic->data[2] + (mx>>3) + (my>>3)*h->mb_uvlinesize;

    if(emu){
        ff_emulated_edge_mc(s->edge_emu_buffer, src_cb, h->mb_uvlinesize, 9, 9/*FIXME*/, (mx>>3), (my>>3), pic_width>>1, pic_height>>1);
            src_cb= s->edge_emu_buffer;
    }
    chroma_op(dest_cb, src_cb, h->mb_uvlinesize, chroma_height, mx&7, my&7);

    if(emu){
        ff_emulated_edge_mc(s->edge_emu_buffer, src_cr, h->mb_uvlinesize, 9, 9/*FIXME*/, (mx>>3), (my>>3), pic_width>>1, pic_height>>1);
            src_cr= s->edge_emu_buffer;
    }
    chroma_op(dest_cr, src_cr, h->mb_uvlinesize, chroma_height, mx&7, my&7);
}

static inline void mc_part_std(H264Context *h, int n, int square, int chroma_height, int delta,
                           uint8_t *dest_y, uint8_t *dest_cb, uint8_t *dest_cr,
                           int x_offset, int y_offset,
                           qpel_mc_func *qpix_put, h264_chroma_mc_func chroma_put,
                           qpel_mc_func *qpix_avg, h264_chroma_mc_func chroma_avg,
                           int list0, int list1){
    MpegEncContext * const s = &h->s;
    qpel_mc_func *qpix_op=  qpix_put;
    h264_chroma_mc_func chroma_op= chroma_put;

    dest_y  += 2*x_offset + 2*y_offset*h->  mb_linesize;
    dest_cb +=   x_offset +   y_offset*h->mb_uvlinesize;
    dest_cr +=   x_offset +   y_offset*h->mb_uvlinesize;
    x_offset += 8*s->mb_x;
    y_offset += 8*(s->mb_y >> MB_MBAFF);

    if(list0){
        Picture *ref= &h->ref_list[0][ h->ref_cache[0][ scan8[n] ] ];
        mc_dir_part(h, ref, n, square, chroma_height, delta, 0,
                           dest_y, dest_cb, dest_cr, x_offset, y_offset,
                           qpix_op, chroma_op);

        qpix_op=  qpix_avg;
        chroma_op= chroma_avg;
    }

    if(list1){
        Picture *ref= &h->ref_list[1][ h->ref_cache[1][ scan8[n] ] ];
        mc_dir_part(h, ref, n, square, chroma_height, delta, 1,
                           dest_y, dest_cb, dest_cr, x_offset, y_offset,
                           qpix_op, chroma_op);
    }
}

static inline void mc_part_weighted(H264Context *h, int n, int square, int chroma_height, int delta,
                           uint8_t *dest_y, uint8_t *dest_cb, uint8_t *dest_cr,
                           int x_offset, int y_offset,
                           qpel_mc_func *qpix_put, h264_chroma_mc_func chroma_put,
                           h264_weight_func luma_weight_op, h264_weight_func chroma_weight_op,
                           h264_biweight_func luma_weight_avg, h264_biweight_func chroma_weight_avg,
                           int list0, int list1){
    MpegEncContext * const s = &h->s;

    dest_y  += 2*x_offset + 2*y_offset*h->  mb_linesize;
    dest_cb +=   x_offset +   y_offset*h->mb_uvlinesize;
    dest_cr +=   x_offset +   y_offset*h->mb_uvlinesize;
    x_offset += 8*s->mb_x;
    y_offset += 8*(s->mb_y >> MB_MBAFF);

    if(list0 && list1){
        /* don't optimize for luma-only case, since B-frames usually
         * use implicit weights => chroma too. */
        uint8_t *tmp_cb = s->obmc_scratchpad;
        uint8_t *tmp_cr = s->obmc_scratchpad + 8;
        uint8_t *tmp_y  = s->obmc_scratchpad + 8*h->mb_uvlinesize;
        int refn0 = h->ref_cache[0][ scan8[n] ];
        int refn1 = h->ref_cache[1][ scan8[n] ];

        mc_dir_part(h, &h->ref_list[0][refn0], n, square, chroma_height, delta, 0,
                    dest_y, dest_cb, dest_cr,
                    x_offset, y_offset, qpix_put, chroma_put);
        mc_dir_part(h, &h->ref_list[1][refn1], n, square, chroma_height, delta, 1,
                    tmp_y, tmp_cb, tmp_cr,
                    x_offset, y_offset, qpix_put, chroma_put);

        if(h->use_weight == 2){
            int weight0 = h->implicit_weight[refn0][refn1];
            int weight1 = 64 - weight0;
            luma_weight_avg(  dest_y,  tmp_y,  h->  mb_linesize, 5, weight0, weight1, 0);
            chroma_weight_avg(dest_cb, tmp_cb, h->mb_uvlinesize, 5, weight0, weight1, 0);
            chroma_weight_avg(dest_cr, tmp_cr, h->mb_uvlinesize, 5, weight0, weight1, 0);
        }else{
            luma_weight_avg(dest_y, tmp_y, h->mb_linesize, h->luma_log2_weight_denom,
                            h->luma_weight[0][refn0], h->luma_weight[1][refn1],
                            h->luma_offset[0][refn0] + h->luma_offset[1][refn1]);
            chroma_weight_avg(dest_cb, tmp_cb, h->mb_uvlinesize, h->chroma_log2_weight_denom,
                            h->chroma_weight[0][refn0][0], h->chroma_weight[1][refn1][0],
                            h->chroma_offset[0][refn0][0] + h->chroma_offset[1][refn1][0]);
            chroma_weight_avg(dest_cr, tmp_cr, h->mb_uvlinesize, h->chroma_log2_weight_denom,
                            h->chroma_weight[0][refn0][1], h->chroma_weight[1][refn1][1],
                            h->chroma_offset[0][refn0][1] + h->chroma_offset[1][refn1][1]);
        }
    }else{
        int list = list1 ? 1 : 0;
        int refn = h->ref_cache[list][ scan8[n] ];
        Picture *ref= &h->ref_list[list][refn];
        mc_dir_part(h, ref, n, square, chroma_height, delta, list,
                    dest_y, dest_cb, dest_cr, x_offset, y_offset,
                    qpix_put, chroma_put);

        luma_weight_op(dest_y, h->mb_linesize, h->luma_log2_weight_denom,
                       h->luma_weight[list][refn], h->luma_offset[list][refn]);
        if(h->use_weight_chroma){
            chroma_weight_op(dest_cb, h->mb_uvlinesize, h->chroma_log2_weight_denom,
                             h->chroma_weight[list][refn][0], h->chroma_offset[list][refn][0]);
            chroma_weight_op(dest_cr, h->mb_uvlinesize, h->chroma_log2_weight_denom,
                             h->chroma_weight[list][refn][1], h->chroma_offset[list][refn][1]);
        }
    }
}

static inline void mc_part(H264Context *h, int n, int square, int chroma_height, int delta,
                           uint8_t *dest_y, uint8_t *dest_cb, uint8_t *dest_cr,
                           int x_offset, int y_offset,
                           qpel_mc_func *qpix_put, h264_chroma_mc_func chroma_put,
                           qpel_mc_func *qpix_avg, h264_chroma_mc_func chroma_avg,
                           h264_weight_func *weight_op, h264_biweight_func *weight_avg,
                           int list0, int list1){
    if((h->use_weight==2 && list0 && list1
        && (h->implicit_weight[ h->ref_cache[0][scan8[n]] ][ h->ref_cache[1][scan8[n]] ] != 32))
       || h->use_weight==1)
        mc_part_weighted(h, n, square, chroma_height, delta, dest_y, dest_cb, dest_cr,
                         x_offset, y_offset, qpix_put, chroma_put,
                         weight_op[0], weight_op[3], weight_avg[0], weight_avg[3], list0, list1);
    else
        mc_part_std(h, n, square, chroma_height, delta, dest_y, dest_cb, dest_cr,
                    x_offset, y_offset, qpix_put, chroma_put, qpix_avg, chroma_avg, list0, list1);
}

static inline void prefetch_motion(H264Context *h, int list){
    /* fetch pixels for estimated mv 4 macroblocks ahead
     * optimized for 64byte cache lines */
    MpegEncContext * const s = &h->s;
    const int refn = h->ref_cache[list][scan8[0]];
    if(refn >= 0){
        const int mx= (h->mv_cache[list][scan8[0]][0]>>2) + 16*s->mb_x + 8;
        const int my= (h->mv_cache[list][scan8[0]][1]>>2) + 16*s->mb_y;
        uint8_t **src= h->ref_list[list][refn].data;
        int off= mx + (my + (s->mb_x&3)*4)*h->mb_linesize + 64;
        s->dsp.prefetch(src[0]+off, s->linesize, 4);
        off= (mx>>1) + ((my>>1) + (s->mb_x&7))*s->uvlinesize + 64;
        s->dsp.prefetch(src[1]+off, src[2]-src[1], 2);
    }
}

static void hl_motion(H264Context *h, uint8_t *dest_y, uint8_t *dest_cb, uint8_t *dest_cr,
                      qpel_mc_func (*qpix_put)[16], h264_chroma_mc_func (*chroma_put),
                      qpel_mc_func (*qpix_avg)[16], h264_chroma_mc_func (*chroma_avg),
                      h264_weight_func *weight_op, h264_biweight_func *weight_avg){
    MpegEncContext * const s = &h->s;
    const int mb_xy= s->mb_x + s->mb_y*s->mb_stride;
    const int mb_type= s->current_picture.mb_type[mb_xy];

    assert(IS_INTER(mb_type));

    prefetch_motion(h, 0);

    if(IS_16X16(mb_type)){
        mc_part(h, 0, 1, 8, 0, dest_y, dest_cb, dest_cr, 0, 0,
                qpix_put[0], chroma_put[0], qpix_avg[0], chroma_avg[0],
                &weight_op[0], &weight_avg[0],
                IS_DIR(mb_type, 0, 0), IS_DIR(mb_type, 0, 1));
    }else if(IS_16X8(mb_type)){
        mc_part(h, 0, 0, 4, 8, dest_y, dest_cb, dest_cr, 0, 0,
                qpix_put[1], chroma_put[0], qpix_avg[1], chroma_avg[0],
                &weight_op[1], &weight_avg[1],
                IS_DIR(mb_type, 0, 0), IS_DIR(mb_type, 0, 1));
        mc_part(h, 8, 0, 4, 8, dest_y, dest_cb, dest_cr, 0, 4,
                qpix_put[1], chroma_put[0], qpix_avg[1], chroma_avg[0],
                &weight_op[1], &weight_avg[1],
                IS_DIR(mb_type, 1, 0), IS_DIR(mb_type, 1, 1));
    }else if(IS_8X16(mb_type)){
        mc_part(h, 0, 0, 8, 8*h->mb_linesize, dest_y, dest_cb, dest_cr, 0, 0,
                qpix_put[1], chroma_put[1], qpix_avg[1], chroma_avg[1],
                &weight_op[2], &weight_avg[2],
                IS_DIR(mb_type, 0, 0), IS_DIR(mb_type, 0, 1));
        mc_part(h, 4, 0, 8, 8*h->mb_linesize, dest_y, dest_cb, dest_cr, 4, 0,
                qpix_put[1], chroma_put[1], qpix_avg[1], chroma_avg[1],
                &weight_op[2], &weight_avg[2],
                IS_DIR(mb_type, 1, 0), IS_DIR(mb_type, 1, 1));
    }else{
        int i;

        assert(IS_8X8(mb_type));

        for(i=0; i<4; i++){
            const int sub_mb_type= h->sub_mb_type[i];
            const int n= 4*i;
            int x_offset= (i&1)<<2;
            int y_offset= (i&2)<<1;

            if(IS_SUB_8X8(sub_mb_type)){
                mc_part(h, n, 1, 4, 0, dest_y, dest_cb, dest_cr, x_offset, y_offset,
                    qpix_put[1], chroma_put[1], qpix_avg[1], chroma_avg[1],
                    &weight_op[3], &weight_avg[3],
                    IS_DIR(sub_mb_type, 0, 0), IS_DIR(sub_mb_type, 0, 1));
            }else if(IS_SUB_8X4(sub_mb_type)){
                mc_part(h, n  , 0, 2, 4, dest_y, dest_cb, dest_cr, x_offset, y_offset,
                    qpix_put[2], chroma_put[1], qpix_avg[2], chroma_avg[1],
                    &weight_op[4], &weight_avg[4],
                    IS_DIR(sub_mb_type, 0, 0), IS_DIR(sub_mb_type, 0, 1));
                mc_part(h, n+2, 0, 2, 4, dest_y, dest_cb, dest_cr, x_offset, y_offset+2,
                    qpix_put[2], chroma_put[1], qpix_avg[2], chroma_avg[1],
                    &weight_op[4], &weight_avg[4],
                    IS_DIR(sub_mb_type, 0, 0), IS_DIR(sub_mb_type, 0, 1));
            }else if(IS_SUB_4X8(sub_mb_type)){
                mc_part(h, n  , 0, 4, 4*h->mb_linesize, dest_y, dest_cb, dest_cr, x_offset, y_offset,
                    qpix_put[2], chroma_put[2], qpix_avg[2], chroma_avg[2],
                    &weight_op[5], &weight_avg[5],
                    IS_DIR(sub_mb_type, 0, 0), IS_DIR(sub_mb_type, 0, 1));
                mc_part(h, n+1, 0, 4, 4*h->mb_linesize, dest_y, dest_cb, dest_cr, x_offset+2, y_offset,
                    qpix_put[2], chroma_put[2], qpix_avg[2], chroma_avg[2],
                    &weight_op[5], &weight_avg[5],
                    IS_DIR(sub_mb_type, 0, 0), IS_DIR(sub_mb_type, 0, 1));
            }else{
                int j;
                assert(IS_SUB_4X4(sub_mb_type));
                for(j=0; j<4; j++){
                    int sub_x_offset= x_offset + 2*(j&1);
                    int sub_y_offset= y_offset +   (j&2);
                    mc_part(h, n+j, 1, 2, 0, dest_y, dest_cb, dest_cr, sub_x_offset, sub_y_offset,
                        qpix_put[2], chroma_put[2], qpix_avg[2], chroma_avg[2],
                        &weight_op[6], &weight_avg[6],
                        IS_DIR(sub_mb_type, 0, 0), IS_DIR(sub_mb_type, 0, 1));
                }
            }
        }
    }

    prefetch_motion(h, 1);
}

static void decode_init_vlc(void){
    static int done = 0;

    if (!done) {
        int i;
        done = 1;

        init_vlc(&chroma_dc_coeff_token_vlc, CHROMA_DC_COEFF_TOKEN_VLC_BITS, 4*5,
                 &chroma_dc_coeff_token_len [0], 1, 1,
                 &chroma_dc_coeff_token_bits[0], 1, 1, 1);

        for(i=0; i<4; i++){
            init_vlc(&coeff_token_vlc[i], COEFF_TOKEN_VLC_BITS, 4*17,
                     &coeff_token_len [i][0], 1, 1,
                     &coeff_token_bits[i][0], 1, 1, 1);
        }

        for(i=0; i<3; i++){
            init_vlc(&chroma_dc_total_zeros_vlc[i], CHROMA_DC_TOTAL_ZEROS_VLC_BITS, 4,
                     &chroma_dc_total_zeros_len [i][0], 1, 1,
                     &chroma_dc_total_zeros_bits[i][0], 1, 1, 1);
        }
        for(i=0; i<15; i++){
            init_vlc(&total_zeros_vlc[i], TOTAL_ZEROS_VLC_BITS, 16,
                     &total_zeros_len [i][0], 1, 1,
                     &total_zeros_bits[i][0], 1, 1, 1);
        }

        for(i=0; i<6; i++){
            init_vlc(&run_vlc[i], RUN_VLC_BITS, 7,
                     &run_len [i][0], 1, 1,
                     &run_bits[i][0], 1, 1, 1);
        }
        init_vlc(&run7_vlc, RUN7_VLC_BITS, 16,
                 &run_len [6][0], 1, 1,
                 &run_bits[6][0], 1, 1, 1);
    }
}

/**
 * Sets the intra prediction function pointers.
 */
static void init_pred_ptrs(H264Context *h){
//    MpegEncContext * const s = &h->s;

    h->pred4x4[VERT_PRED           ]= pred4x4_vertical_c;
    h->pred4x4[HOR_PRED            ]= pred4x4_horizontal_c;
    h->pred4x4[DC_PRED             ]= pred4x4_dc_c;
    h->pred4x4[DIAG_DOWN_LEFT_PRED ]= pred4x4_down_left_c;
    h->pred4x4[DIAG_DOWN_RIGHT_PRED]= pred4x4_down_right_c;
    h->pred4x4[VERT_RIGHT_PRED     ]= pred4x4_vertical_right_c;
    h->pred4x4[HOR_DOWN_PRED       ]= pred4x4_horizontal_down_c;
    h->pred4x4[VERT_LEFT_PRED      ]= pred4x4_vertical_left_c;
    h->pred4x4[HOR_UP_PRED         ]= pred4x4_horizontal_up_c;
    h->pred4x4[LEFT_DC_PRED        ]= pred4x4_left_dc_c;
    h->pred4x4[TOP_DC_PRED         ]= pred4x4_top_dc_c;
    h->pred4x4[DC_128_PRED         ]= pred4x4_128_dc_c;

    h->pred8x8l[VERT_PRED           ]= pred8x8l_vertical_c;
    h->pred8x8l[HOR_PRED            ]= pred8x8l_horizontal_c;
    h->pred8x8l[DC_PRED             ]= pred8x8l_dc_c;
    h->pred8x8l[DIAG_DOWN_LEFT_PRED ]= pred8x8l_down_left_c;
    h->pred8x8l[DIAG_DOWN_RIGHT_PRED]= pred8x8l_down_right_c;
    h->pred8x8l[VERT_RIGHT_PRED     ]= pred8x8l_vertical_right_c;
    h->pred8x8l[HOR_DOWN_PRED       ]= pred8x8l_horizontal_down_c;
    h->pred8x8l[VERT_LEFT_PRED      ]= pred8x8l_vertical_left_c;
    h->pred8x8l[HOR_UP_PRED         ]= pred8x8l_horizontal_up_c;
    h->pred8x8l[LEFT_DC_PRED        ]= pred8x8l_left_dc_c;
    h->pred8x8l[TOP_DC_PRED         ]= pred8x8l_top_dc_c;
    h->pred8x8l[DC_128_PRED         ]= pred8x8l_128_dc_c;

    h->pred8x8[DC_PRED8x8     ]= ff_pred8x8_dc_c;
    h->pred8x8[VERT_PRED8x8   ]= ff_pred8x8_vertical_c;
    h->pred8x8[HOR_PRED8x8    ]= ff_pred8x8_horizontal_c;
    h->pred8x8[PLANE_PRED8x8  ]= ff_pred8x8_plane_c;
    h->pred8x8[LEFT_DC_PRED8x8]= pred8x8_left_dc_c;
    h->pred8x8[TOP_DC_PRED8x8 ]= pred8x8_top_dc_c;
    h->pred8x8[DC_128_PRED8x8 ]= ff_pred8x8_128_dc_c;

    h->pred16x16[DC_PRED8x8     ]= ff_pred16x16_dc_c;
    h->pred16x16[VERT_PRED8x8   ]= ff_pred16x16_vertical_c;
    h->pred16x16[HOR_PRED8x8    ]= ff_pred16x16_horizontal_c;
    h->pred16x16[PLANE_PRED8x8  ]= ff_pred16x16_plane_c;
    h->pred16x16[LEFT_DC_PRED8x8]= pred16x16_left_dc_c;
    h->pred16x16[TOP_DC_PRED8x8 ]= pred16x16_top_dc_c;
    h->pred16x16[DC_128_PRED8x8 ]= ff_pred16x16_128_dc_c;
}

static void free_tables(H264Context *h){
    av_freep(&h->intra4x4_pred_mode);
    av_freep(&h->chroma_pred_mode_table);
    av_freep(&h->cbp_table);
    av_freep(&h->mvd_table[0]);
    av_freep(&h->mvd_table[1]);
    av_freep(&h->direct_table);
    av_freep(&h->non_zero_count);
    av_freep(&h->slice_table_base);
    av_freep(&h->top_borders[1]);
    av_freep(&h->top_borders[0]);
    h->slice_table= NULL;

    av_freep(&h->mb2b_xy);
    av_freep(&h->mb2b8_xy);

    av_freep(&h->s.obmc_scratchpad);
}

static void init_dequant8_coeff_table(H264Context *h){
    int i,q,x;
    const int transpose = (h->s.dsp.h264_idct8_add != ff_h264_idct8_add_c); //FIXME ugly
    h->dequant8_coeff[0] = h->dequant8_buffer[0];
    h->dequant8_coeff[1] = h->dequant8_buffer[1];

    for(i=0; i<2; i++ ){
        if(i && !memcmp(h->pps.scaling_matrix8[0], h->pps.scaling_matrix8[1], 64*sizeof(uint8_t))){
            h->dequant8_coeff[1] = h->dequant8_buffer[0];
            break;
        }

        for(q=0; q<52; q++){
            int shift = ff_div6[q];
            int idx = ff_rem6[q];
            for(x=0; x<64; x++)
                h->dequant8_coeff[i][q][transpose ? (x>>3)|((x&7)<<3) : x] =
                    ((uint32_t)dequant8_coeff_init[idx][ dequant8_coeff_init_scan[((x>>1)&12) | (x&3)] ] *
                    h->pps.scaling_matrix8[i][x]) << shift;
        }
    }
}

static void init_dequant4_coeff_table(H264Context *h){
    int i,j,q,x;
    const int transpose = (h->s.dsp.h264_idct_add != ff_h264_idct_add_c); //FIXME ugly
    for(i=0; i<6; i++ ){
        h->dequant4_coeff[i] = h->dequant4_buffer[i];
        for(j=0; j<i; j++){
            if(!memcmp(h->pps.scaling_matrix4[j], h->pps.scaling_matrix4[i], 16*sizeof(uint8_t))){
                h->dequant4_coeff[i] = h->dequant4_buffer[j];
                break;
            }
        }
        if(j<i)
            continue;

        for(q=0; q<52; q++){
            int shift = ff_div6[q] + 2;
            int idx = ff_rem6[q];
            for(x=0; x<16; x++)
                h->dequant4_coeff[i][q][transpose ? (x>>2)|((x<<2)&0xF) : x] =
                    ((uint32_t)dequant4_coeff_init[idx][(x&1) + ((x>>2)&1)] *
                    h->pps.scaling_matrix4[i][x]) << shift;
        }
    }
}

static void init_dequant_tables(H264Context *h){
    int i,x;
    init_dequant4_coeff_table(h);
    if(h->pps.transform_8x8_mode)
        init_dequant8_coeff_table(h);
    if(h->sps.transform_bypass){
        for(i=0; i<6; i++)
            for(x=0; x<16; x++)
                h->dequant4_coeff[i][0][x] = 1<<6;
        if(h->pps.transform_8x8_mode)
            for(i=0; i<2; i++)
                for(x=0; x<64; x++)
                    h->dequant8_coeff[i][0][x] = 1<<6;
    }
}


/**
 * allocates tables.
 * needs width/height
 */
static int alloc_tables(H264Context *h){
    MpegEncContext * const s = &h->s;
    const int big_mb_num= s->mb_stride * (s->mb_height+1);
    int x,y;

    CHECKED_ALLOCZ(h->intra4x4_pred_mode, big_mb_num * 8  * sizeof(uint8_t))

    CHECKED_ALLOCZ(h->non_zero_count    , big_mb_num * 16 * sizeof(uint8_t))
    CHECKED_ALLOCZ(h->slice_table_base  , (big_mb_num+s->mb_stride) * sizeof(uint8_t))
    CHECKED_ALLOCZ(h->top_borders[0]    , s->mb_width * (16+8+8) * sizeof(uint8_t))
    CHECKED_ALLOCZ(h->top_borders[1]    , s->mb_width * (16+8+8) * sizeof(uint8_t))
    CHECKED_ALLOCZ(h->cbp_table, big_mb_num * sizeof(uint16_t))

    if( h->pps.cabac ) {
        CHECKED_ALLOCZ(h->chroma_pred_mode_table, big_mb_num * sizeof(uint8_t))
        CHECKED_ALLOCZ(h->mvd_table[0], 32*big_mb_num * sizeof(uint16_t));
        CHECKED_ALLOCZ(h->mvd_table[1], 32*big_mb_num * sizeof(uint16_t));
        CHECKED_ALLOCZ(h->direct_table, 32*big_mb_num * sizeof(uint8_t));
    }

    memset(h->slice_table_base, -1, (big_mb_num+s->mb_stride)  * sizeof(uint8_t));
    h->slice_table= h->slice_table_base + s->mb_stride*2 + 1;

    CHECKED_ALLOCZ(h->mb2b_xy  , big_mb_num * sizeof(uint32_t));
    CHECKED_ALLOCZ(h->mb2b8_xy , big_mb_num * sizeof(uint32_t));
    for(y=0; y<s->mb_height; y++){
        for(x=0; x<s->mb_width; x++){
            const int mb_xy= x + y*s->mb_stride;
            const int b_xy = 4*x + 4*y*h->b_stride;
            const int b8_xy= 2*x + 2*y*h->b8_stride;

            h->mb2b_xy [mb_xy]= b_xy;
            h->mb2b8_xy[mb_xy]= b8_xy;
        }
    }

    s->obmc_scratchpad = NULL;

    if(!h->dequant4_coeff[0])
        init_dequant_tables(h);

    return 0;
fail:
    free_tables(h);
    return -1;
}

static void common_init(H264Context *h){
    MpegEncContext * const s = &h->s;

    s->width = s->avctx->width;
    s->height = s->avctx->height;
    s->codec_id= s->avctx->codec->id;

    init_pred_ptrs(h);

    h->dequant_coeff_pps= -1;
    s->unrestricted_mv=1;
    s->decode=1; //FIXME

    memset(h->pps.scaling_matrix4, 16, 6*16*sizeof(uint8_t));
    memset(h->pps.scaling_matrix8, 16, 2*64*sizeof(uint8_t));
}

static int decode_init(AVCodecContext *avctx){
    H264Context *h= avctx->priv_data;
    MpegEncContext * const s = &h->s;

    MPV_decode_defaults(s);

    s->avctx = avctx;
    common_init(h);

    s->out_format = FMT_H264;
    s->workaround_bugs= avctx->workaround_bugs;

    // set defaults
//    s->decode_mb= ff_h263_decode_mb;
    s->low_delay= 1;
    avctx->pix_fmt= PIX_FMT_YUV420P;

    decode_init_vlc();

    if(avctx->extradata_size > 0 && avctx->extradata &&
       *(char *)avctx->extradata == 1){
        h->is_avc = 1;
        h->got_avcC = 0;
    } else {
        h->is_avc = 0;
    }

    return 0;
}

static int frame_start(H264Context *h){
    MpegEncContext * const s = &h->s;
    int i;

    if(MPV_frame_start(s, s->avctx) < 0)
        return -1;
    ff_er_frame_start(s);

    assert(s->linesize && s->uvlinesize);

    for(i=0; i<16; i++){
        h->block_offset[i]= 4*((scan8[i] - scan8[0])&7) + 4*s->linesize*((scan8[i] - scan8[0])>>3);
        h->block_offset[24+i]= 4*((scan8[i] - scan8[0])&7) + 8*s->linesize*((scan8[i] - scan8[0])>>3);
    }
    for(i=0; i<4; i++){
        h->block_offset[16+i]=
        h->block_offset[20+i]= 4*((scan8[i] - scan8[0])&7) + 4*s->uvlinesize*((scan8[i] - scan8[0])>>3);
        h->block_offset[24+16+i]=
        h->block_offset[24+20+i]= 4*((scan8[i] - scan8[0])&7) + 8*s->uvlinesize*((scan8[i] - scan8[0])>>3);
    }

    /* can't be in alloc_tables because linesize isn't known there.
     * FIXME: redo bipred weight to not require extra buffer? */
    if(!s->obmc_scratchpad)
        s->obmc_scratchpad = av_malloc(16*2*s->linesize + 8*2*s->uvlinesize);

    /* some macroblocks will be accessed before they're available */
    if(FRAME_MBAFF)
        memset(h->slice_table, -1, (s->mb_height*s->mb_stride-1) * sizeof(uint8_t));

//    s->decode= (s->flags&CODEC_FLAG_PSNR) || !s->encoding || s->current_picture.reference /*|| h->contains_intra*/ || 1;
    return 0;
}

static inline void backup_mb_border(H264Context *h, uint8_t *src_y, uint8_t *src_cb, uint8_t *src_cr, int linesize, int uvlinesize){
    MpegEncContext * const s = &h->s;
    int i;

    src_y  -=   linesize;
    src_cb -= uvlinesize;
    src_cr -= uvlinesize;

    // There are two lines saved, the line above the the top macroblock of a pair,
    // and the line above the bottom macroblock
    h->left_border[0]= h->top_borders[0][s->mb_x][15];
    for(i=1; i<17; i++){
        h->left_border[i]= src_y[15+i*  linesize];
    }

    *(uint64_t*)(h->top_borders[0][s->mb_x]+0)= *(uint64_t*)(src_y +  16*linesize);
    *(uint64_t*)(h->top_borders[0][s->mb_x]+8)= *(uint64_t*)(src_y +8+16*linesize);

    if(!(s->flags&CODEC_FLAG_GRAY)){
        h->left_border[17  ]= h->top_borders[0][s->mb_x][16+7];
        h->left_border[17+9]= h->top_borders[0][s->mb_x][24+7];
        for(i=1; i<9; i++){
            h->left_border[i+17  ]= src_cb[7+i*uvlinesize];
            h->left_border[i+17+9]= src_cr[7+i*uvlinesize];
        }
        *(uint64_t*)(h->top_borders[0][s->mb_x]+16)= *(uint64_t*)(src_cb+8*uvlinesize);
        *(uint64_t*)(h->top_borders[0][s->mb_x]+24)= *(uint64_t*)(src_cr+8*uvlinesize);
    }
}

static inline void xchg_mb_border(H264Context *h, uint8_t *src_y, uint8_t *src_cb, uint8_t *src_cr, int linesize, int uvlinesize, int xchg){
    MpegEncContext * const s = &h->s;
    int temp8, i;
    uint64_t temp64;
    int deblock_left = (s->mb_x > 0);
    int deblock_top  = (s->mb_y > 0);

    src_y  -=   linesize + 1;
    src_cb -= uvlinesize + 1;
    src_cr -= uvlinesize + 1;

#define XCHG(a,b,t,xchg)\
t= a;\
if(xchg)\
    a= b;\
b= t;

    if(deblock_left){
        for(i = !deblock_top; i<17; i++){
            XCHG(h->left_border[i     ], src_y [i*  linesize], temp8, xchg);
        }
    }

    if(deblock_top){
        XCHG(*(uint64_t*)(h->top_borders[0][s->mb_x]+0), *(uint64_t*)(src_y +1), temp64, xchg);
        XCHG(*(uint64_t*)(h->top_borders[0][s->mb_x]+8), *(uint64_t*)(src_y +9), temp64, 1);
        if(s->mb_x+1 < s->mb_width){
            XCHG(*(uint64_t*)(h->top_borders[0][s->mb_x+1]), *(uint64_t*)(src_y +17), temp64, 1);
        }
    }

    if(!(s->flags&CODEC_FLAG_GRAY)){
        if(deblock_left){
            for(i = !deblock_top; i<9; i++){
                XCHG(h->left_border[i+17  ], src_cb[i*uvlinesize], temp8, xchg);
                XCHG(h->left_border[i+17+9], src_cr[i*uvlinesize], temp8, xchg);
            }
        }
        if(deblock_top){
            XCHG(*(uint64_t*)(h->top_borders[0][s->mb_x]+16), *(uint64_t*)(src_cb+1), temp64, 1);
            XCHG(*(uint64_t*)(h->top_borders[0][s->mb_x]+24), *(uint64_t*)(src_cr+1), temp64, 1);
        }
    }
}

static inline void backup_pair_border(H264Context *h, uint8_t *src_y, uint8_t *src_cb, uint8_t *src_cr, int linesize, int uvlinesize){
    MpegEncContext * const s = &h->s;
    int i;

    src_y  -= 2 *   linesize;
    src_cb -= 2 * uvlinesize;
    src_cr -= 2 * uvlinesize;

    // There are two lines saved, the line above the the top macroblock of a pair,
    // and the line above the bottom macroblock
    h->left_border[0]= h->top_borders[0][s->mb_x][15];
    h->left_border[1]= h->top_borders[1][s->mb_x][15];
    for(i=2; i<34; i++){
        h->left_border[i]= src_y[15+i*  linesize];
    }

    *(uint64_t*)(h->top_borders[0][s->mb_x]+0)= *(uint64_t*)(src_y +  32*linesize);
    *(uint64_t*)(h->top_borders[0][s->mb_x]+8)= *(uint64_t*)(src_y +8+32*linesize);
    *(uint64_t*)(h->top_borders[1][s->mb_x]+0)= *(uint64_t*)(src_y +  33*linesize);
    *(uint64_t*)(h->top_borders[1][s->mb_x]+8)= *(uint64_t*)(src_y +8+33*linesize);

    if(!(s->flags&CODEC_FLAG_GRAY)){
        h->left_border[34     ]= h->top_borders[0][s->mb_x][16+7];
        h->left_border[34+   1]= h->top_borders[1][s->mb_x][16+7];
        h->left_border[34+18  ]= h->top_borders[0][s->mb_x][24+7];
        h->left_border[34+18+1]= h->top_borders[1][s->mb_x][24+7];
        for(i=2; i<18; i++){
            h->left_border[i+34   ]= src_cb[7+i*uvlinesize];
            h->left_border[i+34+18]= src_cr[7+i*uvlinesize];
        }
        *(uint64_t*)(h->top_borders[0][s->mb_x]+16)= *(uint64_t*)(src_cb+16*uvlinesize);
        *(uint64_t*)(h->top_borders[0][s->mb_x]+24)= *(uint64_t*)(src_cr+16*uvlinesize);
        *(uint64_t*)(h->top_borders[1][s->mb_x]+16)= *(uint64_t*)(src_cb+17*uvlinesize);
        *(uint64_t*)(h->top_borders[1][s->mb_x]+24)= *(uint64_t*)(src_cr+17*uvlinesize);
    }
}

static inline void xchg_pair_border(H264Context *h, uint8_t *src_y, uint8_t *src_cb, uint8_t *src_cr, int linesize, int uvlinesize, int xchg){
    MpegEncContext * const s = &h->s;
    int temp8, i;
    uint64_t temp64;
    int deblock_left = (s->mb_x > 0);
    int deblock_top  = (s->mb_y > 1);

    tprintf(s->avctx, "xchg_pair_border: src_y:%p src_cb:%p src_cr:%p ls:%d uvls:%d\n", src_y, src_cb, src_cr, linesize, uvlinesize);

    src_y  -= 2 *   linesize + 1;
    src_cb -= 2 * uvlinesize + 1;
    src_cr -= 2 * uvlinesize + 1;

#define XCHG(a,b,t,xchg)\
t= a;\
if(xchg)\
    a= b;\
b= t;

    if(deblock_left){
        for(i = (!deblock_top)<<1; i<34; i++){
            XCHG(h->left_border[i     ], src_y [i*  linesize], temp8, xchg);
        }
    }

    if(deblock_top){
        XCHG(*(uint64_t*)(h->top_borders[0][s->mb_x]+0), *(uint64_t*)(src_y +1), temp64, xchg);
        XCHG(*(uint64_t*)(h->top_borders[0][s->mb_x]+8), *(uint64_t*)(src_y +9), temp64, 1);
        XCHG(*(uint64_t*)(h->top_borders[1][s->mb_x]+0), *(uint64_t*)(src_y +1 +linesize), temp64, xchg);
        XCHG(*(uint64_t*)(h->top_borders[1][s->mb_x]+8), *(uint64_t*)(src_y +9 +linesize), temp64, 1);
        if(s->mb_x+1 < s->mb_width){
            XCHG(*(uint64_t*)(h->top_borders[0][s->mb_x+1]), *(uint64_t*)(src_y +17), temp64, 1);
            XCHG(*(uint64_t*)(h->top_borders[1][s->mb_x+1]), *(uint64_t*)(src_y +17 +linesize), temp64, 1);
        }
    }

    if(!(s->flags&CODEC_FLAG_GRAY)){
        if(deblock_left){
            for(i = (!deblock_top) << 1; i<18; i++){
                XCHG(h->left_border[i+34   ], src_cb[i*uvlinesize], temp8, xchg);
                XCHG(h->left_border[i+34+18], src_cr[i*uvlinesize], temp8, xchg);
            }
        }
        if(deblock_top){
            XCHG(*(uint64_t*)(h->top_borders[0][s->mb_x]+16), *(uint64_t*)(src_cb+1), temp64, 1);
            XCHG(*(uint64_t*)(h->top_borders[0][s->mb_x]+24), *(uint64_t*)(src_cr+1), temp64, 1);
            XCHG(*(uint64_t*)(h->top_borders[1][s->mb_x]+16), *(uint64_t*)(src_cb+1 +uvlinesize), temp64, 1);
            XCHG(*(uint64_t*)(h->top_borders[1][s->mb_x]+24), *(uint64_t*)(src_cr+1 +uvlinesize), temp64, 1);
        }
    }
}

static void av_always_inline hl_decode_mb_internal(H264Context *h, int simple){
    MpegEncContext * const s = &h->s;
    const int mb_x= s->mb_x;
    const int mb_y= s->mb_y;
    const int mb_xy= mb_x + mb_y*s->mb_stride;
    const int mb_type= s->current_picture.mb_type[mb_xy];
    uint8_t  *dest_y, *dest_cb, *dest_cr;
    int linesize, uvlinesize /*dct_offset*/;
    int i;
    int *block_offset = &h->block_offset[0];
    const unsigned int bottom = mb_y & 1;
    const int transform_bypass = (s->qscale == 0 && h->sps.transform_bypass), is_h264 = (simple || s->codec_id == CODEC_ID_H264);
    void (*idct_add)(uint8_t *dst, DCTELEM *block, int stride);
    void (*idct_dc_add)(uint8_t *dst, DCTELEM *block, int stride);

    dest_y  = s->current_picture.data[0] + (mb_y * 16* s->linesize  ) + mb_x * 16;
    dest_cb = s->current_picture.data[1] + (mb_y * 8 * s->uvlinesize) + mb_x * 8;
    dest_cr = s->current_picture.data[2] + (mb_y * 8 * s->uvlinesize) + mb_x * 8;

    s->dsp.prefetch(dest_y + (s->mb_x&3)*4*s->linesize + 64, s->linesize, 4);
    s->dsp.prefetch(dest_cb + (s->mb_x&7)*s->uvlinesize + 64, dest_cr - dest_cb, 2);

    if (!simple && MB_FIELD) {
        linesize   = h->mb_linesize   = s->linesize * 2;
        uvlinesize = h->mb_uvlinesize = s->uvlinesize * 2;
        block_offset = &h->block_offset[24];
        if(mb_y&1){ //FIXME move out of this func?
            dest_y -= s->linesize*15;
            dest_cb-= s->uvlinesize*7;
            dest_cr-= s->uvlinesize*7;
        }
        if(FRAME_MBAFF) {
            int list;
            for(list=0; list<h->list_count; list++){
                if(!USES_LIST(mb_type, list))
                    continue;
                if(IS_16X16(mb_type)){
                    int8_t *ref = &h->ref_cache[list][scan8[0]];
                    fill_rectangle(ref, 4, 4, 8, 16+*ref^(s->mb_y&1), 1);
                }else{
                    for(i=0; i<16; i+=4){
                        //FIXME can refs be smaller than 8x8 when !direct_8x8_inference ?
                        int ref = h->ref_cache[list][scan8[i]];
                        if(ref >= 0)
                            fill_rectangle(&h->ref_cache[list][scan8[i]], 2, 2, 8, 16+ref^(s->mb_y&1), 1);
                    }
                }
            }
        }
    } else {
        linesize   = h->mb_linesize   = s->linesize;
        uvlinesize = h->mb_uvlinesize = s->uvlinesize;
//        dct_offset = s->linesize * 16;
    }

    if(transform_bypass){
        idct_dc_add =
        idct_add = IS_8x8DCT(mb_type) ? s->dsp.add_pixels8 : s->dsp.add_pixels4;
    }else if(IS_8x8DCT(mb_type)){
        idct_dc_add = s->dsp.h264_idct8_dc_add;
        idct_add = s->dsp.h264_idct8_add;
    }else{
        idct_dc_add = s->dsp.h264_idct_dc_add;
        idct_add = s->dsp.h264_idct_add;
    }

    if(!simple && FRAME_MBAFF && h->deblocking_filter && IS_INTRA(mb_type)
       && (!bottom || !IS_INTRA(s->current_picture.mb_type[mb_xy-s->mb_stride]))){
        int mbt_y = mb_y&~1;
        uint8_t *top_y  = s->current_picture.data[0] + (mbt_y * 16* s->linesize  ) + mb_x * 16;
        uint8_t *top_cb = s->current_picture.data[1] + (mbt_y * 8 * s->uvlinesize) + mb_x * 8;
        uint8_t *top_cr = s->current_picture.data[2] + (mbt_y * 8 * s->uvlinesize) + mb_x * 8;
        xchg_pair_border(h, top_y, top_cb, top_cr, s->linesize, s->uvlinesize, 1);
    }

    if (!simple && IS_INTRA_PCM(mb_type)) {
        unsigned int x, y;

        // The pixels are stored in h->mb array in the same order as levels,
        // copy them in output in the correct order.
        for(i=0; i<16; i++) {
            for (y=0; y<4; y++) {
                for (x=0; x<4; x++) {
                    *(dest_y + block_offset[i] + y*linesize + x) = h->mb[i*16+y*4+x];
                }
            }
        }
        for(i=16; i<16+4; i++) {
            for (y=0; y<4; y++) {
                for (x=0; x<4; x++) {
                    *(dest_cb + block_offset[i] + y*uvlinesize + x) = h->mb[i*16+y*4+x];
                }
            }
        }
        for(i=20; i<20+4; i++) {
            for (y=0; y<4; y++) {
                for (x=0; x<4; x++) {
                    *(dest_cr + block_offset[i] + y*uvlinesize + x) = h->mb[i*16+y*4+x];
                }
            }
        }
    } else {
        if(IS_INTRA(mb_type)){
            if(h->deblocking_filter && (simple || !FRAME_MBAFF))
                xchg_mb_border(h, dest_y, dest_cb, dest_cr, linesize, uvlinesize, 1);

            if(simple || !(s->flags&CODEC_FLAG_GRAY)){
                h->pred8x8[ h->chroma_pred_mode ](dest_cb, uvlinesize);
                h->pred8x8[ h->chroma_pred_mode ](dest_cr, uvlinesize);
            }

            if(IS_INTRA4x4(mb_type)){
                if(simple || !s->encoding){
                    if(IS_8x8DCT(mb_type)){
                        for(i=0; i<16; i+=4){
                            uint8_t * const ptr= dest_y + block_offset[i];
                            const int dir= h->intra4x4_pred_mode_cache[ scan8[i] ];
                            const int nnz = h->non_zero_count_cache[ scan8[i] ];
                            h->pred8x8l[ dir ](ptr, (h->topleft_samples_available<<i)&0x8000,
                                                   (h->topright_samples_available<<i)&0x4000, linesize);
                            if(nnz){
                                if(nnz == 1 && h->mb[i*16])
                                    idct_dc_add(ptr, h->mb + i*16, linesize);
                                else
                                    idct_add(ptr, h->mb + i*16, linesize);
                            }
                        }
                    }else
                    for(i=0; i<16; i++){
                        uint8_t * const ptr= dest_y + block_offset[i];
                        uint8_t *topright;
                        const int dir= h->intra4x4_pred_mode_cache[ scan8[i] ];
                        int nnz, tr;

                        if(dir == DIAG_DOWN_LEFT_PRED || dir == VERT_LEFT_PRED){
                            const int topright_avail= (h->topright_samples_available<<i)&0x8000;
                            assert(mb_y || linesize <= block_offset[i]);
                            if(!topright_avail){
                                tr= ptr[3 - linesize]*0x01010101;
                                topright= (uint8_t*) &tr;
                            }else
                                topright= ptr + 4 - linesize;
                        }else
                            topright= NULL;

                        h->pred4x4[ dir ](ptr, topright, linesize);
                        nnz = h->non_zero_count_cache[ scan8[i] ];
                        if(nnz){
                            if(is_h264){
                                if(nnz == 1 && h->mb[i*16])
                                    idct_dc_add(ptr, h->mb + i*16, linesize);
                                else
                                    idct_add(ptr, h->mb + i*16, linesize);
                            }else
                                svq3_add_idct_c(ptr, h->mb + i*16, linesize, s->qscale, 0);
                        }
                    }
                }
            }else{
                h->pred16x16[ h->intra16x16_pred_mode ](dest_y , linesize);
                if(is_h264){
                    if(!transform_bypass)
                        h264_luma_dc_dequant_idct_c(h->mb, s->qscale, h->dequant4_coeff[IS_INTRA(mb_type) ? 0:3][s->qscale][0]);
                }else
                    svq3_luma_dc_dequant_idct_c(h->mb, s->qscale);
            }
            if(h->deblocking_filter && (simple || !FRAME_MBAFF))
                xchg_mb_border(h, dest_y, dest_cb, dest_cr, linesize, uvlinesize, 0);
        }else if(is_h264){
            hl_motion(h, dest_y, dest_cb, dest_cr,
                      s->me.qpel_put, s->dsp.put_h264_chroma_pixels_tab,
                      s->me.qpel_avg, s->dsp.avg_h264_chroma_pixels_tab,
                      s->dsp.weight_h264_pixels_tab, s->dsp.biweight_h264_pixels_tab);
        }


        if(!IS_INTRA4x4(mb_type)){
            if(is_h264){
                if(IS_INTRA16x16(mb_type)){
                    for(i=0; i<16; i++){
                        if(h->non_zero_count_cache[ scan8[i] ])
                            idct_add(dest_y + block_offset[i], h->mb + i*16, linesize);
                        else if(h->mb[i*16])
                            idct_dc_add(dest_y + block_offset[i], h->mb + i*16, linesize);
                    }
                }else{
                    const int di = IS_8x8DCT(mb_type) ? 4 : 1;
                    for(i=0; i<16; i+=di){
                        int nnz = h->non_zero_count_cache[ scan8[i] ];
                        if(nnz){
                            if(nnz==1 && h->mb[i*16])
                                idct_dc_add(dest_y + block_offset[i], h->mb + i*16, linesize);
                            else
                                idct_add(dest_y + block_offset[i], h->mb + i*16, linesize);
                        }
                    }
                }
            }else{
                for(i=0; i<16; i++){
                    if(h->non_zero_count_cache[ scan8[i] ] || h->mb[i*16]){ //FIXME benchmark weird rule, & below
                        uint8_t * const ptr= dest_y + block_offset[i];
                        svq3_add_idct_c(ptr, h->mb + i*16, linesize, s->qscale, IS_INTRA(mb_type) ? 1 : 0);
                    }
                }
            }
        }

        if(simple || !(s->flags&CODEC_FLAG_GRAY)){
            uint8_t *dest[2] = {dest_cb, dest_cr};
            if(transform_bypass){
                idct_add = idct_dc_add = s->dsp.add_pixels4;
            }else{
                idct_add = s->dsp.h264_idct_add;
                idct_dc_add = s->dsp.h264_idct_dc_add;
                chroma_dc_dequant_idct_c(h->mb + 16*16, h->chroma_qp, h->dequant4_coeff[IS_INTRA(mb_type) ? 1:4][h->chroma_qp][0]);
                chroma_dc_dequant_idct_c(h->mb + 16*16+4*16, h->chroma_qp, h->dequant4_coeff[IS_INTRA(mb_type) ? 2:5][h->chroma_qp][0]);
            }
            if(is_h264){
                for(i=16; i<16+8; i++){
                    if(h->non_zero_count_cache[ scan8[i] ])
                        idct_add(dest[(i&4)>>2] + block_offset[i], h->mb + i*16, uvlinesize);
                    else if(h->mb[i*16])
                        idct_dc_add(dest[(i&4)>>2] + block_offset[i], h->mb + i*16, uvlinesize);
                }
            }else{
                for(i=16; i<16+8; i++){
                    if(h->non_zero_count_cache[ scan8[i] ] || h->mb[i*16]){
                        uint8_t * const ptr= dest[(i&4)>>2] + block_offset[i];
                        svq3_add_idct_c(ptr, h->mb + i*16, uvlinesize, chroma_qp[s->qscale + 12] - 12, 2);
                    }
                }
            }
        }
    }
    if(h->deblocking_filter) {
        if (!simple && FRAME_MBAFF) {
            //FIXME try deblocking one mb at a time?
            // the reduction in load/storing mvs and such might outweigh the extra backup/xchg_border
            const int mb_y = s->mb_y - 1;
            uint8_t  *pair_dest_y, *pair_dest_cb, *pair_dest_cr;
            const int mb_xy= mb_x + mb_y*s->mb_stride;
            const int mb_type_top   = s->current_picture.mb_type[mb_xy];
            const int mb_type_bottom= s->current_picture.mb_type[mb_xy+s->mb_stride];
            if (!bottom) return;
            pair_dest_y  = s->current_picture.data[0] + (mb_y * 16* s->linesize  ) + mb_x * 16;
            pair_dest_cb = s->current_picture.data[1] + (mb_y * 8 * s->uvlinesize) + mb_x * 8;
            pair_dest_cr = s->current_picture.data[2] + (mb_y * 8 * s->uvlinesize) + mb_x * 8;

            if(IS_INTRA(mb_type_top | mb_type_bottom))
                xchg_pair_border(h, pair_dest_y, pair_dest_cb, pair_dest_cr, s->linesize, s->uvlinesize, 0);

            backup_pair_border(h, pair_dest_y, pair_dest_cb, pair_dest_cr, s->linesize, s->uvlinesize);
            // deblock a pair
            // top
            s->mb_y--;
            tprintf(h->s.avctx, "call mbaff filter_mb mb_x:%d mb_y:%d pair_dest_y = %p, dest_y = %p\n", mb_x, mb_y, pair_dest_y, dest_y);
            fill_caches(h, mb_type_top, 1); //FIXME don't fill stuff which isn't used by filter_mb
            h->chroma_qp = get_chroma_qp(h->pps.chroma_qp_index_offset, s->current_picture.qscale_table[mb_xy]);
            filter_mb(h, mb_x, mb_y, pair_dest_y, pair_dest_cb, pair_dest_cr, linesize, uvlinesize);
            // bottom
            s->mb_y++;
            tprintf(h->s.avctx, "call mbaff filter_mb\n");
            fill_caches(h, mb_type_bottom, 1); //FIXME don't fill stuff which isn't used by filter_mb
            h->chroma_qp = get_chroma_qp(h->pps.chroma_qp_index_offset, s->current_picture.qscale_table[mb_xy+s->mb_stride]);
            filter_mb(h, mb_x, mb_y+1, dest_y, dest_cb, dest_cr, linesize, uvlinesize);
        } else {
            tprintf(h->s.avctx, "call filter_mb\n");
            backup_mb_border(h, dest_y, dest_cb, dest_cr, linesize, uvlinesize);
            fill_caches(h, mb_type, 1); //FIXME don't fill stuff which isn't used by filter_mb
            filter_mb_fast(h, mb_x, mb_y, dest_y, dest_cb, dest_cr, linesize, uvlinesize);
        }
    }
}

/**
 * Process a macroblock; this case avoids checks for expensive uncommon cases.
 */
static void hl_decode_mb_simple(H264Context *h){
    hl_decode_mb_internal(h, 1);
}

/**
 * Process a macroblock; this handles edge cases, such as interlacing.
 */
static void av_noinline hl_decode_mb_complex(H264Context *h){
    hl_decode_mb_internal(h, 0);
}

static void hl_decode_mb(H264Context *h){
    MpegEncContext * const s = &h->s;
    const int mb_x= s->mb_x;
    const int mb_y= s->mb_y;
    const int mb_xy= mb_x + mb_y*s->mb_stride;
    const int mb_type= s->current_picture.mb_type[mb_xy];
    int is_complex = FRAME_MBAFF || MB_FIELD || IS_INTRA_PCM(mb_type) || s->codec_id != CODEC_ID_H264 || (s->flags&CODEC_FLAG_GRAY) || s->encoding;

    if(!s->decode)
        return;

    if (is_complex)
        hl_decode_mb_complex(h);
    else hl_decode_mb_simple(h);
}

/**
 * fills the default_ref_list.
 */
static int fill_default_ref_list(H264Context *h){
    MpegEncContext * const s = &h->s;
    int i;
    int smallest_poc_greater_than_current = -1;
    Picture sorted_short_ref[32];

    if(h->slice_type==B_TYPE){
        int out_i;
        int limit= INT_MIN;

        /* sort frame according to poc in B slice */
        for(out_i=0; out_i<h->short_ref_count; out_i++){
            int best_i=INT_MIN;
            int best_poc=INT_MAX;

            for(i=0; i<h->short_ref_count; i++){
                const int poc= h->short_ref[i]->poc;
                if(poc > limit && poc < best_poc){
                    best_poc= poc;
                    best_i= i;
                }
            }

            assert(best_i != INT_MIN);

            limit= best_poc;
            sorted_short_ref[out_i]= *h->short_ref[best_i];
            tprintf(h->s.avctx, "sorted poc: %d->%d poc:%d fn:%d\n", best_i, out_i, sorted_short_ref[out_i].poc, sorted_short_ref[out_i].frame_num);
            if (-1 == smallest_poc_greater_than_current) {
                if (h->short_ref[best_i]->poc >= s->current_picture_ptr->poc) {
                    smallest_poc_greater_than_current = out_i;
                }
            }
        }
    }

    if(s->picture_structure == PICT_FRAME){
        if(h->slice_type==B_TYPE){
            int list;
            tprintf(h->s.avctx, "current poc: %d, smallest_poc_greater_than_current: %d\n", s->current_picture_ptr->poc, smallest_poc_greater_than_current);

            // find the largest poc
            for(list=0; list<2; list++){
                int index = 0;
                int j= -99;
                int step= list ? -1 : 1;

                for(i=0; i<h->short_ref_count && index < h->ref_count[list]; i++, j+=step) {
                    while(j<0 || j>= h->short_ref_count){
                        if(j != -99 && step == (list ? -1 : 1))
                            return -1;
                        step = -step;
                        j= smallest_poc_greater_than_current + (step>>1);
                    }
                    if(sorted_short_ref[j].reference != 3) continue;
                    h->default_ref_list[list][index  ]= sorted_short_ref[j];
                    h->default_ref_list[list][index++].pic_id= sorted_short_ref[j].frame_num;
                }

                for(i = 0; i < 16 && index < h->ref_count[ list ]; i++){
                    if(h->long_ref[i] == NULL) continue;
                    if(h->long_ref[i]->reference != 3) continue;

                    h->default_ref_list[ list ][index  ]= *h->long_ref[i];
                    h->default_ref_list[ list ][index++].pic_id= i;;
                }

                if(list && (smallest_poc_greater_than_current<=0 || smallest_poc_greater_than_current>=h->short_ref_count) && (1 < index)){
                    // swap the two first elements of L1 when
                    // L0 and L1 are identical
                    Picture temp= h->default_ref_list[1][0];
                    h->default_ref_list[1][0] = h->default_ref_list[1][1];
                    h->default_ref_list[1][1] = temp;
                }

                if(index < h->ref_count[ list ])
                    memset(&h->default_ref_list[list][index], 0, sizeof(Picture)*(h->ref_count[ list ] - index));
            }
        }else{
            int index=0;
            for(i=0; i<h->short_ref_count; i++){
                if(h->short_ref[i]->reference != 3) continue; //FIXME refernce field shit
                h->default_ref_list[0][index  ]= *h->short_ref[i];
                h->default_ref_list[0][index++].pic_id= h->short_ref[i]->frame_num;
            }
            for(i = 0; i < 16; i++){
                if(h->long_ref[i] == NULL) continue;
                if(h->long_ref[i]->reference != 3) continue;
                h->default_ref_list[0][index  ]= *h->long_ref[i];
                h->default_ref_list[0][index++].pic_id= i;;
            }
            if(index < h->ref_count[0])
                memset(&h->default_ref_list[0][index], 0, sizeof(Picture)*(h->ref_count[0] - index));
        }
    }else{ //FIELD
        if(h->slice_type==B_TYPE){
        }else{
            //FIXME second field balh
        }
    }
#ifdef TRACE
    for (i=0; i<h->ref_count[0]; i++) {
        tprintf(h->s.avctx, "List0: %s fn:%d 0x%p\n", (h->default_ref_list[0][i].long_ref ? "LT" : "ST"), h->default_ref_list[0][i].pic_id, h->default_ref_list[0][i].data[0]);
    }
    if(h->slice_type==B_TYPE){
        for (i=0; i<h->ref_count[1]; i++) {
            tprintf(h->s.avctx, "List1: %s fn:%d 0x%p\n", (h->default_ref_list[1][i].long_ref ? "LT" : "ST"), h->default_ref_list[1][i].pic_id, h->default_ref_list[0][i].data[0]);
        }
    }
#endif
    return 0;
}

static void print_short_term(H264Context *h);
static void print_long_term(H264Context *h);

static int decode_ref_pic_list_reordering(H264Context *h){
    MpegEncContext * const s = &h->s;
    int list, index;

    print_short_term(h);
    print_long_term(h);
    if(h->slice_type==I_TYPE || h->slice_type==SI_TYPE) return 0; //FIXME move before func

    for(list=0; list<h->list_count; list++){
        memcpy(h->ref_list[list], h->default_ref_list[list], sizeof(Picture)*h->ref_count[list]);

        if(get_bits1(&s->gb)){
            int pred= h->curr_pic_num;

            for(index=0; ; index++){
                unsigned int reordering_of_pic_nums_idc= get_ue_golomb(&s->gb);
                unsigned int pic_id;
                int i;
                Picture *ref = NULL;

                if(reordering_of_pic_nums_idc==3)
                    break;

                if(index >= h->ref_count[list]){
                    av_log(h->s.avctx, AV_LOG_ERROR, "reference count overflow\n");
                    return -1;
                }

                if(reordering_of_pic_nums_idc<3){
                    if(reordering_of_pic_nums_idc<2){
                        const unsigned int abs_diff_pic_num= get_ue_golomb(&s->gb) + 1;

                        if(abs_diff_pic_num >= h->max_pic_num){
                            av_log(h->s.avctx, AV_LOG_ERROR, "abs_diff_pic_num overflow\n");
                            return -1;
                        }

                        if(reordering_of_pic_nums_idc == 0) pred-= abs_diff_pic_num;
                        else                                pred+= abs_diff_pic_num;
                        pred &= h->max_pic_num - 1;

                        for(i= h->short_ref_count-1; i>=0; i--){
                            ref = h->short_ref[i];
                            assert(ref->reference == 3);
                            assert(!ref->long_ref);
                            if(ref->data[0] != NULL && ref->frame_num == pred && ref->long_ref == 0) // ignore non existing pictures by testing data[0] pointer
                                break;
                        }
                        if(i>=0)
                            ref->pic_id= ref->frame_num;
                    }else{
                        pic_id= get_ue_golomb(&s->gb); //long_term_pic_idx
                        if(pic_id>31){
                            av_log(h->s.avctx, AV_LOG_ERROR, "long_term_pic_idx overflow\n");
                            return -1;
                        }
                        ref = h->long_ref[pic_id];
                        if(ref){
                            ref->pic_id= pic_id;
                            assert(ref->reference == 3);
                            assert(ref->long_ref);
                            i=0;
                        }else{
                            i=-1;
                        }
                    }

                    if (i < 0) {
                        av_log(h->s.avctx, AV_LOG_ERROR, "reference picture missing during reorder\n");
                        memset(&h->ref_list[list][index], 0, sizeof(Picture)); //FIXME
                    } else {
                        for(i=index; i+1<h->ref_count[list]; i++){
                            if(ref->long_ref == h->ref_list[list][i].long_ref && ref->pic_id == h->ref_list[list][i].pic_id)
                                break;
                        }
                        for(; i > index; i--){
                            h->ref_list[list][i]= h->ref_list[list][i-1];
                        }
                        h->ref_list[list][index]= *ref;
                    }
                }else{
                    av_log(h->s.avctx, AV_LOG_ERROR, "illegal reordering_of_pic_nums_idc\n");
                    return -1;
                }
            }
        }
    }
    for(list=0; list<h->list_count; list++){
        for(index= 0; index < h->ref_count[list]; index++){
            if(!h->ref_list[list][index].data[0])
                h->ref_list[list][index]= s->current_picture;
        }
    }

    if(h->slice_type==B_TYPE && !h->direct_spatial_mv_pred)
        direct_dist_scale_factor(h);
    direct_ref_list_init(h);
    return 0;
}

static void fill_mbaff_ref_list(H264Context *h){
    int list, i, j;
    for(list=0; list<2; list++){ //FIXME try list_count
        for(i=0; i<h->ref_count[list]; i++){
            Picture *frame = &h->ref_list[list][i];
            Picture *field = &h->ref_list[list][16+2*i];
            field[0] = *frame;
            for(j=0; j<3; j++)
                field[0].linesize[j] <<= 1;
            field[1] = field[0];
            for(j=0; j<3; j++)
                field[1].data[j] += frame->linesize[j];

            h->luma_weight[list][16+2*i] = h->luma_weight[list][16+2*i+1] = h->luma_weight[list][i];
            h->luma_offset[list][16+2*i] = h->luma_offset[list][16+2*i+1] = h->luma_offset[list][i];
            for(j=0; j<2; j++){
                h->chroma_weight[list][16+2*i][j] = h->chroma_weight[list][16+2*i+1][j] = h->chroma_weight[list][i][j];
                h->chroma_offset[list][16+2*i][j] = h->chroma_offset[list][16+2*i+1][j] = h->chroma_offset[list][i][j];
            }
        }
    }
    for(j=0; j<h->ref_count[1]; j++){
        for(i=0; i<h->ref_count[0]; i++)
            h->implicit_weight[j][16+2*i] = h->implicit_weight[j][16+2*i+1] = h->implicit_weight[j][i];
        memcpy(h->implicit_weight[16+2*j],   h->implicit_weight[j], sizeof(*h->implicit_weight));
        memcpy(h->implicit_weight[16+2*j+1], h->implicit_weight[j], sizeof(*h->implicit_weight));
    }
}

static int pred_weight_table(H264Context *h){
    MpegEncContext * const s = &h->s;
    int list, i;
    int luma_def, chroma_def;

    h->use_weight= 0;
    h->use_weight_chroma= 0;
    h->luma_log2_weight_denom= get_ue_golomb(&s->gb);
    h->chroma_log2_weight_denom= get_ue_golomb(&s->gb);
    luma_def = 1<<h->luma_log2_weight_denom;
    chroma_def = 1<<h->chroma_log2_weight_denom;

    for(list=0; list<2; list++){
        for(i=0; i<h->ref_count[list]; i++){
            int luma_weight_flag, chroma_weight_flag;

            luma_weight_flag= get_bits1(&s->gb);
            if(luma_weight_flag){
                h->luma_weight[list][i]= get_se_golomb(&s->gb);
                h->luma_offset[list][i]= get_se_golomb(&s->gb);
                if(   h->luma_weight[list][i] != luma_def
                   || h->luma_offset[list][i] != 0)
                    h->use_weight= 1;
            }else{
                h->luma_weight[list][i]= luma_def;
                h->luma_offset[list][i]= 0;
            }

            chroma_weight_flag= get_bits1(&s->gb);
            if(chroma_weight_flag){
                int j;
                for(j=0; j<2; j++){
                    h->chroma_weight[list][i][j]= get_se_golomb(&s->gb);
                    h->chroma_offset[list][i][j]= get_se_golomb(&s->gb);
                    if(   h->chroma_weight[list][i][j] != chroma_def
                       || h->chroma_offset[list][i][j] != 0)
                        h->use_weight_chroma= 1;
                }
            }else{
                int j;
                for(j=0; j<2; j++){
                    h->chroma_weight[list][i][j]= chroma_def;
                    h->chroma_offset[list][i][j]= 0;
                }
            }
        }
        if(h->slice_type != B_TYPE) break;
    }
    h->use_weight= h->use_weight || h->use_weight_chroma;
    return 0;
}

static void implicit_weight_table(H264Context *h){
    MpegEncContext * const s = &h->s;
    int ref0, ref1;
    int cur_poc = s->current_picture_ptr->poc;

    if(   h->ref_count[0] == 1 && h->ref_count[1] == 1
       && h->ref_list[0][0].poc + h->ref_list[1][0].poc == 2*cur_poc){
        h->use_weight= 0;
        h->use_weight_chroma= 0;
        return;
    }

    h->use_weight= 2;
    h->use_weight_chroma= 2;
    h->luma_log2_weight_denom= 5;
    h->chroma_log2_weight_denom= 5;

    for(ref0=0; ref0 < h->ref_count[0]; ref0++){
        int poc0 = h->ref_list[0][ref0].poc;
        for(ref1=0; ref1 < h->ref_count[1]; ref1++){
            int poc1 = h->ref_list[1][ref1].poc;
            int td = av_clip(poc1 - poc0, -128, 127);
            if(td){
                int tb = av_clip(cur_poc - poc0, -128, 127);
                int tx = (16384 + (FFABS(td) >> 1)) / td;
                int dist_scale_factor = av_clip((tb*tx + 32) >> 6, -1024, 1023) >> 2;
                if(dist_scale_factor < -64 || dist_scale_factor > 128)
                    h->implicit_weight[ref0][ref1] = 32;
                else
                    h->implicit_weight[ref0][ref1] = 64 - dist_scale_factor;
            }else
                h->implicit_weight[ref0][ref1] = 32;
        }
    }
}

static inline void unreference_pic(H264Context *h, Picture *pic){
    int i;
    pic->reference=0;
    if(pic == h->delayed_output_pic)
        pic->reference=1;
    else{
        for(i = 0; h->delayed_pic[i]; i++)
            if(pic == h->delayed_pic[i]){
                pic->reference=1;
                break;
            }
    }
}

/**
 * instantaneous decoder refresh.
 */
static void idr(H264Context *h){
    int i;

    for(i=0; i<16; i++){
        if (h->long_ref[i] != NULL) {
            unreference_pic(h, h->long_ref[i]);
            h->long_ref[i]= NULL;
        }
    }
    h->long_ref_count=0;

    for(i=0; i<h->short_ref_count; i++){
        unreference_pic(h, h->short_ref[i]);
        h->short_ref[i]= NULL;
    }
    h->short_ref_count=0;
}

/* forget old pics after a seek */
static void flush_dpb(AVCodecContext *avctx){
    H264Context *h= avctx->priv_data;
    int i;
    for(i=0; i<16; i++) {
        if(h->delayed_pic[i])
            h->delayed_pic[i]->reference= 0;
        h->delayed_pic[i]= NULL;
    }
    if(h->delayed_output_pic)
        h->delayed_output_pic->reference= 0;
    h->delayed_output_pic= NULL;
    idr(h);
    if(h->s.current_picture_ptr)
        h->s.current_picture_ptr->reference= 0;
}

/**
 *
 * @return the removed picture or NULL if an error occurs
 */
static Picture * remove_short(H264Context *h, int frame_num){
    MpegEncContext * const s = &h->s;
    int i;

    if(s->avctx->debug&FF_DEBUG_MMCO)
        av_log(h->s.avctx, AV_LOG_DEBUG, "remove short %d count %d\n", frame_num, h->short_ref_count);

    for(i=0; i<h->short_ref_count; i++){
        Picture *pic= h->short_ref[i];
        if(s->avctx->debug&FF_DEBUG_MMCO)
            av_log(h->s.avctx, AV_LOG_DEBUG, "%d %d %p\n", i, pic->frame_num, pic);
        if(pic->frame_num == frame_num){
            h->short_ref[i]= NULL;
            memmove(&h->short_ref[i], &h->short_ref[i+1], (h->short_ref_count - i - 1)*sizeof(Picture*));
            h->short_ref_count--;
            return pic;
        }
    }
    return NULL;
}

/**
 *
 * @return the removed picture or NULL if an error occurs
 */
static Picture * remove_long(H264Context *h, int i){
    Picture *pic;

    pic= h->long_ref[i];
    h->long_ref[i]= NULL;
    if(pic) h->long_ref_count--;

    return pic;
}

/**
 * print short term list
 */
static void print_short_term(H264Context *h) {
    uint32_t i;
    if(h->s.avctx->debug&FF_DEBUG_MMCO) {
        av_log(h->s.avctx, AV_LOG_DEBUG, "short term list:\n");
        for(i=0; i<h->short_ref_count; i++){
            Picture *pic= h->short_ref[i];
            av_log(h->s.avctx, AV_LOG_DEBUG, "%d fn:%d poc:%d %p\n", i, pic->frame_num, pic->poc, pic->data[0]);
        }
    }
}

/**
 * print long term list
 */
static void print_long_term(H264Context *h) {
    uint32_t i;
    if(h->s.avctx->debug&FF_DEBUG_MMCO) {
        av_log(h->s.avctx, AV_LOG_DEBUG, "long term list:\n");
        for(i = 0; i < 16; i++){
            Picture *pic= h->long_ref[i];
            if (pic) {
                av_log(h->s.avctx, AV_LOG_DEBUG, "%d fn:%d poc:%d %p\n", i, pic->frame_num, pic->poc, pic->data[0]);
            }
        }
    }
}

/**
 * Executes the reference picture marking (memory management control operations).
 */
static int execute_ref_pic_marking(H264Context *h, MMCO *mmco, int mmco_count){
    MpegEncContext * const s = &h->s;
    int i, j;
    int current_is_long=0;
    Picture *pic;

    if((s->avctx->debug&FF_DEBUG_MMCO) && mmco_count==0)
        av_log(h->s.avctx, AV_LOG_DEBUG, "no mmco here\n");

    for(i=0; i<mmco_count; i++){
        if(s->avctx->debug&FF_DEBUG_MMCO)
            av_log(h->s.avctx, AV_LOG_DEBUG, "mmco:%d %d %d\n", h->mmco[i].opcode, h->mmco[i].short_frame_num, h->mmco[i].long_index);

        switch(mmco[i].opcode){
        case MMCO_SHORT2UNUSED:
            pic= remove_short(h, mmco[i].short_frame_num);
            if(pic)
                unreference_pic(h, pic);
            else if(s->avctx->debug&FF_DEBUG_MMCO)
                av_log(h->s.avctx, AV_LOG_DEBUG, "mmco: remove_short() failure\n");
            break;
        case MMCO_SHORT2LONG:
            pic= remove_long(h, mmco[i].long_index);
            if(pic) unreference_pic(h, pic);

            h->long_ref[ mmco[i].long_index ]= remove_short(h, mmco[i].short_frame_num);
            if (h->long_ref[ mmco[i].long_index ]){
                h->long_ref[ mmco[i].long_index ]->long_ref=1;
                h->long_ref_count++;
            }
            break;
        case MMCO_LONG2UNUSED:
            pic= remove_long(h, mmco[i].long_index);
            if(pic)
                unreference_pic(h, pic);
            else if(s->avctx->debug&FF_DEBUG_MMCO)
                av_log(h->s.avctx, AV_LOG_DEBUG, "mmco: remove_long() failure\n");
            break;
        case MMCO_LONG:
            pic= remove_long(h, mmco[i].long_index);
            if(pic) unreference_pic(h, pic);

            h->long_ref[ mmco[i].long_index ]= s->current_picture_ptr;
            h->long_ref[ mmco[i].long_index ]->long_ref=1;
            h->long_ref_count++;

            current_is_long=1;
            break;
        case MMCO_SET_MAX_LONG:
            assert(mmco[i].long_index <= 16);
            // just remove the long term which index is greater than new max
            for(j = mmco[i].long_index; j<16; j++){
                pic = remove_long(h, j);
                if (pic) unreference_pic(h, pic);
            }
            break;
        case MMCO_RESET:
            while(h->short_ref_count){
                pic= remove_short(h, h->short_ref[0]->frame_num);
                if(pic) unreference_pic(h, pic);
            }
            for(j = 0; j < 16; j++) {
                pic= remove_long(h, j);
                if(pic) unreference_pic(h, pic);
            }
            break;
        default: assert(0);
        }
    }

    if(!current_is_long){
        pic= remove_short(h, s->current_picture_ptr->frame_num);
        if(pic){
            unreference_pic(h, pic);
            av_log(h->s.avctx, AV_LOG_ERROR, "illegal short term buffer state detected\n");
        }

        if(h->short_ref_count)
            memmove(&h->short_ref[1], &h->short_ref[0], h->short_ref_count*sizeof(Picture*));

        h->short_ref[0]= s->current_picture_ptr;
        h->short_ref[0]->long_ref=0;
        h->short_ref_count++;
    }

    print_short_term(h);
    print_long_term(h);
    return 0;
}

static int decode_ref_pic_marking(H264Context *h){
    MpegEncContext * const s = &h->s;
    int i;

    if(h->nal_unit_type == NAL_IDR_SLICE){ //FIXME fields
        s->broken_link= get_bits1(&s->gb) -1;
        h->mmco[0].long_index= get_bits1(&s->gb) - 1; // current_long_term_idx
        if(h->mmco[0].long_index == -1)
            h->mmco_index= 0;
        else{
            h->mmco[0].opcode= MMCO_LONG;
            h->mmco_index= 1;
        }
    }else{
        if(get_bits1(&s->gb)){ // adaptive_ref_pic_marking_mode_flag
            for(i= 0; i<MAX_MMCO_COUNT; i++) {
                MMCOOpcode opcode= get_ue_golomb(&s->gb);;

                h->mmco[i].opcode= opcode;
                if(opcode==MMCO_SHORT2UNUSED || opcode==MMCO_SHORT2LONG){
                    h->mmco[i].short_frame_num= (h->frame_num - get_ue_golomb(&s->gb) - 1) & ((1<<h->sps.log2_max_frame_num)-1); //FIXME fields
/*                    if(h->mmco[i].short_frame_num >= h->short_ref_count || h->short_ref[ h->mmco[i].short_frame_num ] == NULL){
                        av_log(s->avctx, AV_LOG_ERROR, "illegal short ref in memory management control operation %d\n", mmco);
                        return -1;
                    }*/
                }
                if(opcode==MMCO_SHORT2LONG || opcode==MMCO_LONG2UNUSED || opcode==MMCO_LONG || opcode==MMCO_SET_MAX_LONG){
                    unsigned int long_index= get_ue_golomb(&s->gb);
                    if(/*h->mmco[i].long_index >= h->long_ref_count || h->long_ref[ h->mmco[i].long_index ] == NULL*/ long_index >= 16){
                        av_log(h->s.avctx, AV_LOG_ERROR, "illegal long ref in memory management control operation %d\n", opcode);
                        return -1;
                    }
                    h->mmco[i].long_index= long_index;
                }

                if(opcode > (unsigned)MMCO_LONG){
                    av_log(h->s.avctx, AV_LOG_ERROR, "illegal memory management control operation %d\n", opcode);
                    return -1;
                }
                if(opcode == MMCO_END)
                    break;
            }
            h->mmco_index= i;
        }else{
            assert(h->long_ref_count + h->short_ref_count <= h->sps.ref_frame_count);

            if(h->long_ref_count + h->short_ref_count == h->sps.ref_frame_count){ //FIXME fields
                h->mmco[0].opcode= MMCO_SHORT2UNUSED;
                h->mmco[0].short_frame_num= h->short_ref[ h->short_ref_count - 1 ]->frame_num;
                h->mmco_index= 1;
            }else
                h->mmco_index= 0;
        }
    }

    return 0;
}

static int init_poc(H264Context *h){
    MpegEncContext * const s = &h->s;
    const int max_frame_num= 1<<h->sps.log2_max_frame_num;
    int field_poc[2];

    if(h->nal_unit_type == NAL_IDR_SLICE){
        h->frame_num_offset= 0;
    }else{
        if(h->frame_num < h->prev_frame_num)
            h->frame_num_offset= h->prev_frame_num_offset + max_frame_num;
        else
            h->frame_num_offset= h->prev_frame_num_offset;
    }

    if(h->sps.poc_type==0){
        const int max_poc_lsb= 1<<h->sps.log2_max_poc_lsb;

        if(h->nal_unit_type == NAL_IDR_SLICE){
             h->prev_poc_msb=
             h->prev_poc_lsb= 0;
        }

        if     (h->poc_lsb < h->prev_poc_lsb && h->prev_poc_lsb - h->poc_lsb >= max_poc_lsb/2)
            h->poc_msb = h->prev_poc_msb + max_poc_lsb;
        else if(h->poc_lsb > h->prev_poc_lsb && h->prev_poc_lsb - h->poc_lsb < -max_poc_lsb/2)
            h->poc_msb = h->prev_poc_msb - max_poc_lsb;
        else
            h->poc_msb = h->prev_poc_msb;
//printf("poc: %d %d\n", h->poc_msb, h->poc_lsb);
        field_poc[0] =
        field_poc[1] = h->poc_msb + h->poc_lsb;
        if(s->picture_structure == PICT_FRAME)
            field_poc[1] += h->delta_poc_bottom;
    }else if(h->sps.poc_type==1){
        int abs_frame_num, expected_delta_per_poc_cycle, expectedpoc;
        int i;

        if(h->sps.poc_cycle_length != 0)
            abs_frame_num = h->frame_num_offset + h->frame_num;
        else
            abs_frame_num = 0;

        if(h->nal_ref_idc==0 && abs_frame_num > 0)
            abs_frame_num--;

        expected_delta_per_poc_cycle = 0;
        for(i=0; i < h->sps.poc_cycle_length; i++)
            expected_delta_per_poc_cycle += h->sps.offset_for_ref_frame[ i ]; //FIXME integrate during sps parse

        if(abs_frame_num > 0){
            int poc_cycle_cnt          = (abs_frame_num - 1) / h->sps.poc_cycle_length;
            int frame_num_in_poc_cycle = (abs_frame_num - 1) % h->sps.poc_cycle_length;

            expectedpoc = poc_cycle_cnt * expected_delta_per_poc_cycle;
            for(i = 0; i <= frame_num_in_poc_cycle; i++)
                expectedpoc = expectedpoc + h->sps.offset_for_ref_frame[ i ];
        } else
            expectedpoc = 0;

        if(h->nal_ref_idc == 0)
            expectedpoc = expectedpoc + h->sps.offset_for_non_ref_pic;

        field_poc[0] = expectedpoc + h->delta_poc[0];
        field_poc[1] = field_poc[0] + h->sps.offset_for_top_to_bottom_field;

        if(s->picture_structure == PICT_FRAME)
            field_poc[1] += h->delta_poc[1];
    }else{
        int poc;
        if(h->nal_unit_type == NAL_IDR_SLICE){
            poc= 0;
        }else{
            if(h->nal_ref_idc) poc= 2*(h->frame_num_offset + h->frame_num);
            else               poc= 2*(h->frame_num_offset + h->frame_num) - 1;
        }
        field_poc[0]= poc;
        field_poc[1]= poc;
    }

    if(s->picture_structure != PICT_BOTTOM_FIELD)
        s->current_picture_ptr->field_poc[0]= field_poc[0];
    if(s->picture_structure != PICT_TOP_FIELD)
        s->current_picture_ptr->field_poc[1]= field_poc[1];
    if(s->picture_structure == PICT_FRAME) // FIXME field pix?
        s->current_picture_ptr->poc= FFMIN(field_poc[0], field_poc[1]);

    return 0;
}

/**
 * decodes a slice header.
 * this will allso call MPV_common_init() and frame_start() as needed
 */
static int decode_slice_header(H264Context *h){
    MpegEncContext * const s = &h->s;
    unsigned int first_mb_in_slice;
    unsigned int pps_id;
    int num_ref_idx_active_override_flag;
    static const uint8_t slice_type_map[5]= {P_TYPE, B_TYPE, I_TYPE, SP_TYPE, SI_TYPE};
    unsigned int slice_type, tmp;
    int default_ref_list_done = 0;

    s->current_picture.reference= h->nal_ref_idc != 0;
    s->dropable= h->nal_ref_idc == 0;

    first_mb_in_slice= get_ue_golomb(&s->gb);

    if((s->flags2 & CODEC_FLAG2_CHUNKS) && first_mb_in_slice == 0){
        h->slice_num = 0;
        s->current_picture_ptr= NULL;
    }

    slice_type= get_ue_golomb(&s->gb);
    if(slice_type > 9){
        av_log(h->s.avctx, AV_LOG_ERROR, "slice type too large (%d) at %d %d\n", h->slice_type, s->mb_x, s->mb_y);
        return -1;
    }
    if(slice_type > 4){
        slice_type -= 5;
        h->slice_type_fixed=1;
    }else
        h->slice_type_fixed=0;

    slice_type= slice_type_map[ slice_type ];
    if (slice_type == I_TYPE
        || (h->slice_num != 0 && slice_type == h->slice_type) ) {
        default_ref_list_done = 1;
    }
    h->slice_type= slice_type;

    s->pict_type= h->slice_type; // to make a few old func happy, it's wrong though

    pps_id= get_ue_golomb(&s->gb);
    if(pps_id>=MAX_PPS_COUNT){
        av_log(h->s.avctx, AV_LOG_ERROR, "pps_id out of range\n");
        return -1;
    }
    h->pps= h->pps_buffer[pps_id];
    if(h->pps.slice_group_count == 0){
        av_log(h->s.avctx, AV_LOG_ERROR, "non existing PPS referenced\n");
        return -1;
    }

    h->sps= h->sps_buffer[ h->pps.sps_id ];
    if(h->sps.log2_max_frame_num == 0){
        av_log(h->s.avctx, AV_LOG_ERROR, "non existing SPS referenced\n");
        return -1;
    }

    if(h->dequant_coeff_pps != pps_id){
        h->dequant_coeff_pps = pps_id;
        init_dequant_tables(h);
    }

    s->mb_width= h->sps.mb_width;
    s->mb_height= h->sps.mb_height * (2 - h->sps.frame_mbs_only_flag);

    h->b_stride=  s->mb_width*4;
    h->b8_stride= s->mb_width*2;

    s->width = 16*s->mb_width - 2*(h->sps.crop_left + h->sps.crop_right );
    if(h->sps.frame_mbs_only_flag)
        s->height= 16*s->mb_height - 2*(h->sps.crop_top  + h->sps.crop_bottom);
    else
        s->height= 16*s->mb_height - 4*(h->sps.crop_top  + h->sps.crop_bottom); //FIXME recheck

    if (s->context_initialized
        && (   s->width != s->avctx->width || s->height != s->avctx->height)) {
        free_tables(h);
        MPV_common_end(s);
    }
    if (!s->context_initialized) {
        if (MPV_common_init(s) < 0)
            return -1;

        if(s->dsp.h264_idct_add == ff_h264_idct_add_c){ //FIXME little ugly
            memcpy(h->zigzag_scan, zigzag_scan, 16*sizeof(uint8_t));
            memcpy(h-> field_scan,  field_scan, 16*sizeof(uint8_t));
        }else{
            int i;
            for(i=0; i<16; i++){
#define T(x) (x>>2) | ((x<<2) & 0xF)
                h->zigzag_scan[i] = T(zigzag_scan[i]);
                h-> field_scan[i] = T( field_scan[i]);
#undef T
            }
        }
        if(s->dsp.h264_idct8_add == ff_h264_idct8_add_c){
            memcpy(h->zigzag_scan8x8,       zigzag_scan8x8,       64*sizeof(uint8_t));
            memcpy(h->zigzag_scan8x8_cavlc, zigzag_scan8x8_cavlc, 64*sizeof(uint8_t));
            memcpy(h->field_scan8x8,        field_scan8x8,        64*sizeof(uint8_t));
            memcpy(h->field_scan8x8_cavlc,  field_scan8x8_cavlc,  64*sizeof(uint8_t));
        }else{
            int i;
            for(i=0; i<64; i++){
#define T(x) (x>>3) | ((x&7)<<3)
                h->zigzag_scan8x8[i]       = T(zigzag_scan8x8[i]);
                h->zigzag_scan8x8_cavlc[i] = T(zigzag_scan8x8_cavlc[i]);
                h->field_scan8x8[i]        = T(field_scan8x8[i]);
                h->field_scan8x8_cavlc[i]  = T(field_scan8x8_cavlc[i]);
#undef T
            }
        }
        if(h->sps.transform_bypass){ //FIXME same ugly
            h->zigzag_scan_q0          = zigzag_scan;
            h->zigzag_scan8x8_q0       = zigzag_scan8x8;
            h->zigzag_scan8x8_cavlc_q0 = zigzag_scan8x8_cavlc;
            h->field_scan_q0           = field_scan;
            h->field_scan8x8_q0        = field_scan8x8;
            h->field_scan8x8_cavlc_q0  = field_scan8x8_cavlc;
        }else{
            h->zigzag_scan_q0          = h->zigzag_scan;
            h->zigzag_scan8x8_q0       = h->zigzag_scan8x8;
            h->zigzag_scan8x8_cavlc_q0 = h->zigzag_scan8x8_cavlc;
            h->field_scan_q0           = h->field_scan;
            h->field_scan8x8_q0        = h->field_scan8x8;
            h->field_scan8x8_cavlc_q0  = h->field_scan8x8_cavlc;
        }

        alloc_tables(h);

        s->avctx->width = s->width;
        s->avctx->height = s->height;
        s->avctx->sample_aspect_ratio= h->sps.sar;
        if(!s->avctx->sample_aspect_ratio.den)
            s->avctx->sample_aspect_ratio.den = 1;

        if(h->sps.timing_info_present_flag){
            s->avctx->time_base= (AVRational){h->sps.num_units_in_tick * 2, h->sps.time_scale};
            if(h->x264_build > 0 && h->x264_build < 44)
                s->avctx->time_base.den *= 2;
            av_reduce(&s->avctx->time_base.num, &s->avctx->time_base.den,
                      s->avctx->time_base.num, s->avctx->time_base.den, 1<<30);
        }
    }

    if(h->slice_num == 0){
        if(frame_start(h) < 0)
            return -1;
    }

    s->current_picture_ptr->frame_num= //FIXME frame_num cleanup
    h->frame_num= get_bits(&s->gb, h->sps.log2_max_frame_num);

    h->mb_mbaff = 0;
    h->mb_aff_frame = 0;
    if(h->sps.frame_mbs_only_flag){
        s->picture_structure= PICT_FRAME;
    }else{
        if(get_bits1(&s->gb)) { //field_pic_flag
            s->picture_structure= PICT_TOP_FIELD + get_bits1(&s->gb); //bottom_field_flag
            av_log(h->s.avctx, AV_LOG_ERROR, "PAFF interlacing is not implemented\n");
        } else {
            s->picture_structure= PICT_FRAME;
            h->mb_aff_frame = h->sps.mb_aff;
        }
    }
    assert(s->mb_num == s->mb_width * s->mb_height);
    if(first_mb_in_slice << h->mb_aff_frame >= s->mb_num ||
       first_mb_in_slice                    >= s->mb_num){
        av_log(h->s.avctx, AV_LOG_ERROR, "first_mb_in_slice overflow\n");
        return -1;
    }
    s->resync_mb_x = s->mb_x = first_mb_in_slice % s->mb_width;
    s->resync_mb_y = s->mb_y = (first_mb_in_slice / s->mb_width) << h->mb_aff_frame;
    assert(s->mb_y < s->mb_height);

    if(s->picture_structure==PICT_FRAME){
        h->curr_pic_num=   h->frame_num;
        h->max_pic_num= 1<< h->sps.log2_max_frame_num;
    }else{
        h->curr_pic_num= 2*h->frame_num;
        h->max_pic_num= 1<<(h->sps.log2_max_frame_num + 1);
    }

    if(h->nal_unit_type == NAL_IDR_SLICE){
        get_ue_golomb(&s->gb); /* idr_pic_id */
    }

    if(h->sps.poc_type==0){
        h->poc_lsb= get_bits(&s->gb, h->sps.log2_max_poc_lsb);

        if(h->pps.pic_order_present==1 && s->picture_structure==PICT_FRAME){
            h->delta_poc_bottom= get_se_golomb(&s->gb);
        }
    }

    if(h->sps.poc_type==1 && !h->sps.delta_pic_order_always_zero_flag){
        h->delta_poc[0]= get_se_golomb(&s->gb);

        if(h->pps.pic_order_present==1 && s->picture_structure==PICT_FRAME)
            h->delta_poc[1]= get_se_golomb(&s->gb);
    }

    init_poc(h);

    if(h->pps.redundant_pic_cnt_present){
        h->redundant_pic_count= get_ue_golomb(&s->gb);
    }

    //set defaults, might be overriden a few line later
    h->ref_count[0]= h->pps.ref_count[0];
    h->ref_count[1]= h->pps.ref_count[1];

    if(h->slice_type == P_TYPE || h->slice_type == SP_TYPE || h->slice_type == B_TYPE){
        if(h->slice_type == B_TYPE){
            h->direct_spatial_mv_pred= get_bits1(&s->gb);
            if(h->sps.mb_aff && h->direct_spatial_mv_pred)
                av_log(h->s.avctx, AV_LOG_ERROR, "MBAFF + spatial direct mode is not implemented\n");
        }
        num_ref_idx_active_override_flag= get_bits1(&s->gb);

        if(num_ref_idx_active_override_flag){
            h->ref_count[0]= get_ue_golomb(&s->gb) + 1;
            if(h->slice_type==B_TYPE)
                h->ref_count[1]= get_ue_golomb(&s->gb) + 1;

            if(h->ref_count[0]-1 > 32-1 || h->ref_count[1]-1 > 32-1){
                av_log(h->s.avctx, AV_LOG_ERROR, "reference overflow\n");
                h->ref_count[0]= h->ref_count[1]= 1;
                return -1;
            }
        }
        if(h->slice_type == B_TYPE)
            h->list_count= 2;
        else
            h->list_count= 1;
    }else
        h->list_count= 0;

    if(!default_ref_list_done){
        fill_default_ref_list(h);
    }

    if(decode_ref_pic_list_reordering(h) < 0)
        return -1;

    if(   (h->pps.weighted_pred          && (h->slice_type == P_TYPE || h->slice_type == SP_TYPE ))
       || (h->pps.weighted_bipred_idc==1 && h->slice_type==B_TYPE ) )
        pred_weight_table(h);
    else if(h->pps.weighted_bipred_idc==2 && h->slice_type==B_TYPE)
        implicit_weight_table(h);
    else
        h->use_weight = 0;

    if(s->current_picture.reference)
        decode_ref_pic_marking(h);

    if(FRAME_MBAFF)
        fill_mbaff_ref_list(h);

    if( h->slice_type != I_TYPE && h->slice_type != SI_TYPE && h->pps.cabac ){
        tmp = get_ue_golomb(&s->gb);
        if(tmp > 2){
            av_log(s->avctx, AV_LOG_ERROR, "cabac_init_idc overflow\n");
            return -1;
        }
        h->cabac_init_idc= tmp;
    }

    h->last_qscale_diff = 0;
    tmp = h->pps.init_qp + get_se_golomb(&s->gb);
    if(tmp>51){
        av_log(s->avctx, AV_LOG_ERROR, "QP %u out of range\n", tmp);
        return -1;
    }
    s->qscale= tmp;
    h->chroma_qp = get_chroma_qp(h->pps.chroma_qp_index_offset, s->qscale);
    //FIXME qscale / qp ... stuff
    if(h->slice_type == SP_TYPE){
        get_bits1(&s->gb); /* sp_for_switch_flag */
    }
    if(h->slice_type==SP_TYPE || h->slice_type == SI_TYPE){
        get_se_golomb(&s->gb); /* slice_qs_delta */
    }

    h->deblocking_filter = 1;
    h->slice_alpha_c0_offset = 0;
    h->slice_beta_offset = 0;
    if( h->pps.deblocking_filter_parameters_present ) {
        tmp= get_ue_golomb(&s->gb);
        if(tmp > 2){
            av_log(s->avctx, AV_LOG_ERROR, "deblocking_filter_idc %u out of range\n", tmp);
            return -1;
        }
        h->deblocking_filter= tmp;
        if(h->deblocking_filter < 2)
            h->deblocking_filter^= 1; // 1<->0

        if( h->deblocking_filter ) {
            h->slice_alpha_c0_offset = get_se_golomb(&s->gb) << 1;
            h->slice_beta_offset = get_se_golomb(&s->gb) << 1;
        }
    }
    if(   s->avctx->skip_loop_filter >= AVDISCARD_ALL
       ||(s->avctx->skip_loop_filter >= AVDISCARD_NONKEY && h->slice_type != I_TYPE)
       ||(s->avctx->skip_loop_filter >= AVDISCARD_BIDIR  && h->slice_type == B_TYPE)
       ||(s->avctx->skip_loop_filter >= AVDISCARD_NONREF && h->nal_ref_idc == 0))
        h->deblocking_filter= 0;

#if 0 //FMO
    if( h->pps.num_slice_groups > 1  && h->pps.mb_slice_group_map_type >= 3 && h->pps.mb_slice_group_map_type <= 5)
        slice_group_change_cycle= get_bits(&s->gb, ?);
#endif

    h->slice_num++;

    h->emu_edge_width= (s->flags&CODEC_FLAG_EMU_EDGE) ? 0 : 16;
    h->emu_edge_height= FRAME_MBAFF ? 0 : h->emu_edge_width;

    if(s->avctx->debug&FF_DEBUG_PICT_INFO){
        av_log(h->s.avctx, AV_LOG_DEBUG, "slice:%d %s mb:%d %c pps:%u frame:%d poc:%d/%d ref:%d/%d qp:%d loop:%d:%d:%d weight:%d%s\n",
               h->slice_num,
               (s->picture_structure==PICT_FRAME ? "F" : s->picture_structure==PICT_TOP_FIELD ? "T" : "B"),
               first_mb_in_slice,
               av_get_pict_type_char(h->slice_type),
               pps_id, h->frame_num,
               s->current_picture_ptr->field_poc[0], s->current_picture_ptr->field_poc[1],
               h->ref_count[0], h->ref_count[1],
               s->qscale,
               h->deblocking_filter, h->slice_alpha_c0_offset/2, h->slice_beta_offset/2,
               h->use_weight,
               h->use_weight==1 && h->use_weight_chroma ? "c" : ""
               );
    }

    if((s->avctx->flags2 & CODEC_FLAG2_FAST) && !s->current_picture.reference){
        s->me.qpel_put= s->dsp.put_2tap_qpel_pixels_tab;
        s->me.qpel_avg= s->dsp.avg_2tap_qpel_pixels_tab;
    }else{
        s->me.qpel_put= s->dsp.put_h264_qpel_pixels_tab;
        s->me.qpel_avg= s->dsp.avg_h264_qpel_pixels_tab;
    }

    return 0;
}

/**
 *
 */
static inline int get_level_prefix(GetBitContext *gb){
    unsigned int buf;
    int log;

    OPEN_READER(re, gb);
    UPDATE_CACHE(re, gb);
    buf=GET_CACHE(re, gb);

    log= 32 - av_log2(buf);
#ifdef TRACE
    print_bin(buf>>(32-log), log);
    av_log(NULL, AV_LOG_DEBUG, "%5d %2d %3d lpr @%5d in %s get_level_prefix\n", buf>>(32-log), log, log-1, get_bits_count(gb), __FILE__);
#endif

    LAST_SKIP_BITS(re, gb, log);
    CLOSE_READER(re, gb);

    return log-1;
}

static inline int get_dct8x8_allowed(H264Context *h){
    int i;
    for(i=0; i<4; i++){
        if(!IS_SUB_8X8(h->sub_mb_type[i])
           || (!h->sps.direct_8x8_inference_flag && IS_DIRECT(h->sub_mb_type[i])))
            return 0;
    }
    return 1;
}

/**
 * decodes a residual block.
 * @param n block index
 * @param scantable scantable
 * @param max_coeff number of coefficients in the block
 * @return <0 if an error occured
 */
static int decode_residual(H264Context *h, GetBitContext *gb, DCTELEM *block, int n, const uint8_t *scantable, const uint32_t *qmul, int max_coeff){
    MpegEncContext * const s = &h->s;
    static const int coeff_token_table_index[17]= {0, 0, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3, 3};
    int level[16];
    int zeros_left, coeff_num, coeff_token, total_coeff, i, j, trailing_ones, run_before;

    //FIXME put trailing_onex into the context

    if(n == CHROMA_DC_BLOCK_INDEX){
        coeff_token= get_vlc2(gb, chroma_dc_coeff_token_vlc.table, CHROMA_DC_COEFF_TOKEN_VLC_BITS, 1);
        total_coeff= coeff_token>>2;
    }else{
        if(n == LUMA_DC_BLOCK_INDEX){
            total_coeff= pred_non_zero_count(h, 0);
            coeff_token= get_vlc2(gb, coeff_token_vlc[ coeff_token_table_index[total_coeff] ].table, COEFF_TOKEN_VLC_BITS, 2);
            total_coeff= coeff_token>>2;
        }else{
            total_coeff= pred_non_zero_count(h, n);
            coeff_token= get_vlc2(gb, coeff_token_vlc[ coeff_token_table_index[total_coeff] ].table, COEFF_TOKEN_VLC_BITS, 2);
            total_coeff= coeff_token>>2;
            h->non_zero_count_cache[ scan8[n] ]= total_coeff;
        }
    }

    //FIXME set last_non_zero?

    if(total_coeff==0)
        return 0;
    if(total_coeff > (unsigned)max_coeff) {
        av_log(h->s.avctx, AV_LOG_ERROR, "corrupted macroblock %d %d (total_coeff=%d)\n", s->mb_x, s->mb_y, total_coeff);
        return -1;
    }

    trailing_ones= coeff_token&3;
    tprintf(h->s.avctx, "trailing:%d, total:%d\n", trailing_ones, total_coeff);
    assert(total_coeff<=16);

    for(i=0; i<trailing_ones; i++){
        level[i]= 1 - 2*get_bits1(gb);
    }

    if(i<total_coeff) {
        int level_code, mask;
        int suffix_length = total_coeff > 10 && trailing_ones < 3;
        int prefix= get_level_prefix(gb);

        //first coefficient has suffix_length equal to 0 or 1
        if(prefix<14){ //FIXME try to build a large unified VLC table for all this
            if(suffix_length)
                level_code= (prefix<<suffix_length) + get_bits(gb, suffix_length); //part
            else
                level_code= (prefix<<suffix_length); //part
        }else if(prefix==14){
            if(suffix_length)
                level_code= (prefix<<suffix_length) + get_bits(gb, suffix_length); //part
            else
                level_code= prefix + get_bits(gb, 4); //part
        }else if(prefix==15){
            level_code= (prefix<<suffix_length) + get_bits(gb, 12); //part
            if(suffix_length==0) level_code+=15; //FIXME doesn't make (much)sense
        }else{
            av_log(h->s.avctx, AV_LOG_ERROR, "prefix too large at %d %d\n", s->mb_x, s->mb_y);
            return -1;
        }

        if(trailing_ones < 3) level_code += 2;

        suffix_length = 1;
        if(level_code > 5)
            suffix_length++;
        mask= -(level_code&1);
        level[i]= (((2+level_code)>>1) ^ mask) - mask;
        i++;

        //remaining coefficients have suffix_length > 0
        for(;i<total_coeff;i++) {
            static const int suffix_limit[7] = {0,5,11,23,47,95,INT_MAX };
            prefix = get_level_prefix(gb);
            if(prefix<15){
                level_code = (prefix<<suffix_length) + get_bits(gb, suffix_length);
            }else if(prefix==15){
                level_code =  (prefix<<suffix_length) + get_bits(gb, 12);
            }else{
                av_log(h->s.avctx, AV_LOG_ERROR, "prefix too large at %d %d\n", s->mb_x, s->mb_y);
                return -1;
            }
            mask= -(level_code&1);
            level[i]= (((2+level_code)>>1) ^ mask) - mask;
            if(level_code > suffix_limit[suffix_length])
                suffix_length++;
        }
    }

    if(total_coeff == max_coeff)
        zeros_left=0;
    else{
        if(n == CHROMA_DC_BLOCK_INDEX)
            zeros_left= get_vlc2(gb, chroma_dc_total_zeros_vlc[ total_coeff-1 ].table, CHROMA_DC_TOTAL_ZEROS_VLC_BITS, 1);
        else
            zeros_left= get_vlc2(gb, total_zeros_vlc[ total_coeff-1 ].table, TOTAL_ZEROS_VLC_BITS, 1);
    }

    coeff_num = zeros_left + total_coeff - 1;
    j = scantable[coeff_num];
    if(n > 24){
        block[j] = level[0];
        for(i=1;i<total_coeff;i++) {
            if(zeros_left <= 0)
                run_before = 0;
            else if(zeros_left < 7){
                run_before= get_vlc2(gb, run_vlc[zeros_left-1].table, RUN_VLC_BITS, 1);
            }else{
                run_before= get_vlc2(gb, run7_vlc.table, RUN7_VLC_BITS, 2);
            }
            zeros_left -= run_before;
            coeff_num -= 1 + run_before;
            j= scantable[ coeff_num ];

            block[j]= level[i];
        }
    }else{
        block[j] = (level[0] * qmul[j] + 32)>>6;
        for(i=1;i<total_coeff;i++) {
            if(zeros_left <= 0)
                run_before = 0;
            else if(zeros_left < 7){
                run_before= get_vlc2(gb, run_vlc[zeros_left-1].table, RUN_VLC_BITS, 1);
            }else{
                run_before= get_vlc2(gb, run7_vlc.table, RUN7_VLC_BITS, 2);
            }
            zeros_left -= run_before;
            coeff_num -= 1 + run_before;
            j= scantable[ coeff_num ];

            block[j]= (level[i] * qmul[j] + 32)>>6;
        }
    }

    if(zeros_left<0){
        av_log(h->s.avctx, AV_LOG_ERROR, "negative number of zero coeffs at %d %d\n", s->mb_x, s->mb_y);
        return -1;
    }

    return 0;
}

static void predict_field_decoding_flag(H264Context *h){
    MpegEncContext * const s = &h->s;
    const int mb_xy= s->mb_x + s->mb_y*s->mb_stride;
    int mb_type = (h->slice_table[mb_xy-1] == h->slice_num)
                ? s->current_picture.mb_type[mb_xy-1]
                : (h->slice_table[mb_xy-s->mb_stride] == h->slice_num)
                ? s->current_picture.mb_type[mb_xy-s->mb_stride]
                : 0;
    h->mb_mbaff = h->mb_field_decoding_flag = IS_INTERLACED(mb_type) ? 1 : 0;
}

/**
 * decodes a P_SKIP or B_SKIP macroblock
 */
static void decode_mb_skip(H264Context *h){
    MpegEncContext * const s = &h->s;
    const int mb_xy= s->mb_x + s->mb_y*s->mb_stride;
    int mb_type=0;

    memset(h->non_zero_count[mb_xy], 0, 16);
    memset(h->non_zero_count_cache + 8, 0, 8*5); //FIXME ugly, remove pfui

    if(MB_FIELD)
        mb_type|= MB_TYPE_INTERLACED;

    if( h->slice_type == B_TYPE )
    {
        // just for fill_caches. pred_direct_motion will set the real mb_type
        mb_type|= MB_TYPE_16x16|MB_TYPE_P0L0|MB_TYPE_P0L1|MB_TYPE_DIRECT2|MB_TYPE_SKIP;

        fill_caches(h, mb_type, 0); //FIXME check what is needed and what not ...
        pred_direct_motion(h, &mb_type);
        mb_type|= MB_TYPE_SKIP;
    }
    else
    {
        int mx, my;
        mb_type|= MB_TYPE_16x16|MB_TYPE_P0L0|MB_TYPE_P1L0|MB_TYPE_SKIP;

        fill_caches(h, mb_type, 0); //FIXME check what is needed and what not ...
        pred_pskip_motion(h, &mx, &my);
        fill_rectangle(&h->ref_cache[0][scan8[0]], 4, 4, 8, 0, 1);
        fill_rectangle(  h->mv_cache[0][scan8[0]], 4, 4, 8, pack16to32(mx,my), 4);
    }

    write_back_motion(h, mb_type);
    s->current_picture.mb_type[mb_xy]= mb_type;
    s->current_picture.qscale_table[mb_xy]= s->qscale;
    h->slice_table[ mb_xy ]= h->slice_num;
    h->prev_mb_skipped= 1;
}

/**
 * decodes a macroblock
 * @returns 0 if ok, AC_ERROR / DC_ERROR / MV_ERROR if an error is noticed
 */
static int decode_mb_cavlc(H264Context *h){
    MpegEncContext * const s = &h->s;
    const int mb_xy= s->mb_x + s->mb_y*s->mb_stride;
    int partition_count;
    unsigned int mb_type, cbp;
    int dct8x8_allowed= h->pps.transform_8x8_mode;

    s->dsp.clear_blocks(h->mb); //FIXME avoid if already clear (move after skip handlong?

    tprintf(s->avctx, "pic:%d mb:%d/%d\n", h->frame_num, s->mb_x, s->mb_y);
    cbp = 0; /* avoid warning. FIXME: find a solution without slowing
                down the code */
    if(h->slice_type != I_TYPE && h->slice_type != SI_TYPE){
        if(s->mb_skip_run==-1)
            s->mb_skip_run= get_ue_golomb(&s->gb);

        if (s->mb_skip_run--) {
            if(FRAME_MBAFF && (s->mb_y&1) == 0){
                if(s->mb_skip_run==0)
                    h->mb_mbaff = h->mb_field_decoding_flag = get_bits1(&s->gb);
                else
                    predict_field_decoding_flag(h);
            }
            decode_mb_skip(h);
            return 0;
        }
    }
    if(FRAME_MBAFF){
        if( (s->mb_y&1) == 0 )
            h->mb_mbaff = h->mb_field_decoding_flag = get_bits1(&s->gb);
    }else
        h->mb_field_decoding_flag= (s->picture_structure!=PICT_FRAME);

    h->prev_mb_skipped= 0;

    mb_type= get_ue_golomb(&s->gb);
    if(h->slice_type == B_TYPE){
        if(mb_type < 23){
            partition_count= b_mb_type_info[mb_type].partition_count;
            mb_type=         b_mb_type_info[mb_type].type;
        }else{
            mb_type -= 23;
            goto decode_intra_mb;
        }
    }else if(h->slice_type == P_TYPE /*|| h->slice_type == SP_TYPE */){
        if(mb_type < 5){
            partition_count= p_mb_type_info[mb_type].partition_count;
            mb_type=         p_mb_type_info[mb_type].type;
        }else{
            mb_type -= 5;
            goto decode_intra_mb;
        }
    }else{
       assert(h->slice_type == I_TYPE);
decode_intra_mb:
        if(mb_type > 25){
            av_log(h->s.avctx, AV_LOG_ERROR, "mb_type %d in %c slice too large at %d %d\n", mb_type, av_get_pict_type_char(h->slice_type), s->mb_x, s->mb_y);
            return -1;
        }
        partition_count=0;
        cbp= i_mb_type_info[mb_type].cbp;
        h->intra16x16_pred_mode= i_mb_type_info[mb_type].pred_mode;
        mb_type= i_mb_type_info[mb_type].type;
    }

    if(MB_FIELD)
        mb_type |= MB_TYPE_INTERLACED;

    h->slice_table[ mb_xy ]= h->slice_num;

    if(IS_INTRA_PCM(mb_type)){
        unsigned int x, y;

        // we assume these blocks are very rare so we dont optimize it
        align_get_bits(&s->gb);

        // The pixels are stored in the same order as levels in h->mb array.
        for(y=0; y<16; y++){
            const int index= 4*(y&3) + 32*((y>>2)&1) + 128*(y>>3);
            for(x=0; x<16; x++){
                tprintf(s->avctx, "LUMA ICPM LEVEL (%3d)\n", show_bits(&s->gb, 8));
                h->mb[index + (x&3) + 16*((x>>2)&1) + 64*(x>>3)]= get_bits(&s->gb, 8);
            }
        }
        for(y=0; y<8; y++){
            const int index= 256 + 4*(y&3) + 32*(y>>2);
            for(x=0; x<8; x++){
                tprintf(s->avctx, "CHROMA U ICPM LEVEL (%3d)\n", show_bits(&s->gb, 8));
                h->mb[index + (x&3) + 16*(x>>2)]= get_bits(&s->gb, 8);
            }
        }
        for(y=0; y<8; y++){
            const int index= 256 + 64 + 4*(y&3) + 32*(y>>2);
            for(x=0; x<8; x++){
                tprintf(s->avctx, "CHROMA V ICPM LEVEL (%3d)\n", show_bits(&s->gb, 8));
                h->mb[index + (x&3) + 16*(x>>2)]= get_bits(&s->gb, 8);
            }
        }

        // In deblocking, the quantizer is 0
        s->current_picture.qscale_table[mb_xy]= 0;
        h->chroma_qp = get_chroma_qp(h->pps.chroma_qp_index_offset, 0);
        // All coeffs are present
        memset(h->non_zero_count[mb_xy], 16, 16);

        s->current_picture.mb_type[mb_xy]= mb_type;
        return 0;
    }

    if(MB_MBAFF){
        h->ref_count[0] <<= 1;
        h->ref_count[1] <<= 1;
    }

    fill_caches(h, mb_type, 0);

    //mb_pred
    if(IS_INTRA(mb_type)){
            int pred_mode;
//            init_top_left_availability(h);
            if(IS_INTRA4x4(mb_type)){
                int i;
                int di = 1;
                if(dct8x8_allowed && get_bits1(&s->gb)){
                    mb_type |= MB_TYPE_8x8DCT;
                    di = 4;
                }

//                fill_intra4x4_pred_table(h);
                for(i=0; i<16; i+=di){
                    int mode= pred_intra_mode(h, i);

                    if(!get_bits1(&s->gb)){
                        const int rem_mode= get_bits(&s->gb, 3);
                        mode = rem_mode + (rem_mode >= mode);
                    }

                    if(di==4)
                        fill_rectangle( &h->intra4x4_pred_mode_cache[ scan8[i] ], 2, 2, 8, mode, 1 );
                    else
                        h->intra4x4_pred_mode_cache[ scan8[i] ] = mode;
                }
                write_back_intra_pred_mode(h);
                if( check_intra4x4_pred_mode(h) < 0)
                    return -1;
            }else{
                h->intra16x16_pred_mode= check_intra_pred_mode(h, h->intra16x16_pred_mode);
                if(h->intra16x16_pred_mode < 0)
                    return -1;
            }

            pred_mode= check_intra_pred_mode(h, get_ue_golomb(&s->gb));
            if(pred_mode < 0)
                return -1;
            h->chroma_pred_mode= pred_mode;
    }else if(partition_count==4){
        int i, j, sub_partition_count[4], list, ref[2][4];

        if(h->slice_type == B_TYPE){
            for(i=0; i<4; i++){
                h->sub_mb_type[i]= get_ue_golomb(&s->gb);
                if(h->sub_mb_type[i] >=13){
                    av_log(h->s.avctx, AV_LOG_ERROR, "B sub_mb_type %u out of range at %d %d\n", h->sub_mb_type[i], s->mb_x, s->mb_y);
                    return -1;
                }
                sub_partition_count[i]= b_sub_mb_type_info[ h->sub_mb_type[i] ].partition_count;
                h->sub_mb_type[i]=      b_sub_mb_type_info[ h->sub_mb_type[i] ].type;
            }
            if(   IS_DIRECT(h->sub_mb_type[0]) || IS_DIRECT(h->sub_mb_type[1])
               || IS_DIRECT(h->sub_mb_type[2]) || IS_DIRECT(h->sub_mb_type[3])) {
                pred_direct_motion(h, &mb_type);
                h->ref_cache[0][scan8[4]] =
                h->ref_cache[1][scan8[4]] =
                h->ref_cache[0][scan8[12]] =
                h->ref_cache[1][scan8[12]] = PART_NOT_AVAILABLE;
            }
        }else{
            assert(h->slice_type == P_TYPE || h->slice_type == SP_TYPE); //FIXME SP correct ?
            for(i=0; i<4; i++){
                h->sub_mb_type[i]= get_ue_golomb(&s->gb);
                if(h->sub_mb_type[i] >=4){
                    av_log(h->s.avctx, AV_LOG_ERROR, "P sub_mb_type %u out of range at %d %d\n", h->sub_mb_type[i], s->mb_x, s->mb_y);
                    return -1;
                }
                sub_partition_count[i]= p_sub_mb_type_info[ h->sub_mb_type[i] ].partition_count;
                h->sub_mb_type[i]=      p_sub_mb_type_info[ h->sub_mb_type[i] ].type;
            }
        }

        for(list=0; list<h->list_count; list++){
            int ref_count= IS_REF0(mb_type) ? 1 : h->ref_count[list];
            for(i=0; i<4; i++){
                if(IS_DIRECT(h->sub_mb_type[i])) continue;
                if(IS_DIR(h->sub_mb_type[i], 0, list)){
                    unsigned int tmp = get_te0_golomb(&s->gb, ref_count); //FIXME init to 0 before and skip?
                    if(tmp>=ref_count){
                        av_log(h->s.avctx, AV_LOG_ERROR, "ref %u overflow\n", tmp);
                        return -1;
                    }
                    ref[list][i]= tmp;
                }else{
                 //FIXME
                    ref[list][i] = -1;
                }
            }
        }

        if(dct8x8_allowed)
            dct8x8_allowed = get_dct8x8_allowed(h);

        for(list=0; list<h->list_count; list++){
            const int ref_count= IS_REF0(mb_type) ? 1 : h->ref_count[list];

            for(i=0; i<4; i++){
                if(IS_DIRECT(h->sub_mb_type[i])) {
                    h->ref_cache[list][ scan8[4*i] ] = h->ref_cache[list][ scan8[4*i]+1 ];
                    continue;
                }
                h->ref_cache[list][ scan8[4*i]   ]=h->ref_cache[list][ scan8[4*i]+1 ]=
                h->ref_cache[list][ scan8[4*i]+8 ]=h->ref_cache[list][ scan8[4*i]+9 ]= ref[list][i];

                if(IS_DIR(h->sub_mb_type[i], 0, list)){
                    const int sub_mb_type= h->sub_mb_type[i];
                    const int block_width= (sub_mb_type & (MB_TYPE_16x16|MB_TYPE_16x8)) ? 2 : 1;
                    for(j=0; j<sub_partition_count[i]; j++){
                        int mx, my;
                        const int index= 4*i + block_width*j;
                        int16_t (* mv_cache)[2]= &h->mv_cache[list][ scan8[index] ];
                        pred_motion(h, index, block_width, list, h->ref_cache[list][ scan8[index] ], &mx, &my);
                        mx += get_se_golomb(&s->gb);
                        my += get_se_golomb(&s->gb);
                        tprintf(s->avctx, "final mv:%d %d\n", mx, my);

                        if(IS_SUB_8X8(sub_mb_type)){
                            mv_cache[ 1 ][0]=
                            mv_cache[ 8 ][0]= mv_cache[ 9 ][0]= mx;
                            mv_cache[ 1 ][1]=
                            mv_cache[ 8 ][1]= mv_cache[ 9 ][1]= my;
                        }else if(IS_SUB_8X4(sub_mb_type)){
                            mv_cache[ 1 ][0]= mx;
                            mv_cache[ 1 ][1]= my;
                        }else if(IS_SUB_4X8(sub_mb_type)){
                            mv_cache[ 8 ][0]= mx;
                            mv_cache[ 8 ][1]= my;
                        }
                        mv_cache[ 0 ][0]= mx;
                        mv_cache[ 0 ][1]= my;
                    }
                }else{
                    uint32_t *p= (uint32_t *)&h->mv_cache[list][ scan8[4*i] ][0];
                    p[0] = p[1]=
                    p[8] = p[9]= 0;
                }
            }
        }
    }else if(IS_DIRECT(mb_type)){
        pred_direct_motion(h, &mb_type);
        dct8x8_allowed &= h->sps.direct_8x8_inference_flag;
    }else{
        int list, mx, my, i;
         //FIXME we should set ref_idx_l? to 0 if we use that later ...
        if(IS_16X16(mb_type)){
            for(list=0; list<h->list_count; list++){
                    unsigned int val;
                    if(IS_DIR(mb_type, 0, list)){
                        val= get_te0_golomb(&s->gb, h->ref_count[list]);
                        if(val >= h->ref_count[list]){
                            av_log(h->s.avctx, AV_LOG_ERROR, "ref %u overflow\n", val);
                            return -1;
                        }
                    }else
                        val= LIST_NOT_USED&0xFF;
                    fill_rectangle(&h->ref_cache[list][ scan8[0] ], 4, 4, 8, val, 1);
            }
            for(list=0; list<h->list_count; list++){
                unsigned int val;
                if(IS_DIR(mb_type, 0, list)){
                    pred_motion(h, 0, 4, list, h->ref_cache[list][ scan8[0] ], &mx, &my);
                    mx += get_se_golomb(&s->gb);
                    my += get_se_golomb(&s->gb);
                    tprintf(s->avctx, "final mv:%d %d\n", mx, my);

                    val= pack16to32(mx,my);
                }else
                    val=0;
                fill_rectangle(h->mv_cache[list][ scan8[0] ], 4, 4, 8, val, 4);
            }
        }
        else if(IS_16X8(mb_type)){
            for(list=0; list<h->list_count; list++){
                    for(i=0; i<2; i++){
                        unsigned int val;
                        if(IS_DIR(mb_type, i, list)){
                            val= get_te0_golomb(&s->gb, h->ref_count[list]);
                            if(val >= h->ref_count[list]){
                                av_log(h->s.avctx, AV_LOG_ERROR, "ref %u overflow\n", val);
                                return -1;
                            }
                        }else
                            val= LIST_NOT_USED&0xFF;
                        fill_rectangle(&h->ref_cache[list][ scan8[0] + 16*i ], 4, 2, 8, val, 1);
                    }
            }
            for(list=0; list<h->list_count; list++){
                for(i=0; i<2; i++){
                    unsigned int val;
                    if(IS_DIR(mb_type, i, list)){
                        pred_16x8_motion(h, 8*i, list, h->ref_cache[list][scan8[0] + 16*i], &mx, &my);
                        mx += get_se_golomb(&s->gb);
                        my += get_se_golomb(&s->gb);
                        tprintf(s->avctx, "final mv:%d %d\n", mx, my);

                        val= pack16to32(mx,my);
                    }else
                        val=0;
                    fill_rectangle(h->mv_cache[list][ scan8[0] + 16*i ], 4, 2, 8, val, 4);
                }
            }
        }else{
            assert(IS_8X16(mb_type));
            for(list=0; list<h->list_count; list++){
                    for(i=0; i<2; i++){
                        unsigned int val;
                        if(IS_DIR(mb_type, i, list)){ //FIXME optimize
                            val= get_te0_golomb(&s->gb, h->ref_count[list]);
                            if(val >= h->ref_count[list]){
                                av_log(h->s.avctx, AV_LOG_ERROR, "ref %u overflow\n", val);
                                return -1;
                            }
                        }else
                            val= LIST_NOT_USED&0xFF;
                        fill_rectangle(&h->ref_cache[list][ scan8[0] + 2*i ], 2, 4, 8, val, 1);
                    }
            }
            for(list=0; list<h->list_count; list++){
                for(i=0; i<2; i++){
                    unsigned int val;
                    if(IS_DIR(mb_type, i, list)){
                        pred_8x16_motion(h, i*4, list, h->ref_cache[list][ scan8[0] + 2*i ], &mx, &my);
                        mx += get_se_golomb(&s->gb);
                        my += get_se_golomb(&s->gb);
                        tprintf(s->avctx, "final mv:%d %d\n", mx, my);

                        val= pack16to32(mx,my);
                    }else
                        val=0;
                    fill_rectangle(h->mv_cache[list][ scan8[0] + 2*i ], 2, 4, 8, val, 4);
                }
            }
        }
    }

    if(IS_INTER(mb_type))
        write_back_motion(h, mb_type);

    if(!IS_INTRA16x16(mb_type)){
        cbp= get_ue_golomb(&s->gb);
        if(cbp > 47){
            av_log(h->s.avctx, AV_LOG_ERROR, "cbp too large (%u) at %d %d\n", cbp, s->mb_x, s->mb_y);
            return -1;
        }

        if(IS_INTRA4x4(mb_type))
            cbp= golomb_to_intra4x4_cbp[cbp];
        else
            cbp= golomb_to_inter_cbp[cbp];
    }
    h->cbp = cbp;

    if(dct8x8_allowed && (cbp&15) && !IS_INTRA(mb_type)){
        if(get_bits1(&s->gb))
            mb_type |= MB_TYPE_8x8DCT;
    }
    s->current_picture.mb_type[mb_xy]= mb_type;

    if(cbp || IS_INTRA16x16(mb_type)){
        int i8x8, i4x4, chroma_idx;
        int chroma_qp, dquant;
        GetBitContext *gb= IS_INTRA(mb_type) ? h->intra_gb_ptr : h->inter_gb_ptr;
        const uint8_t *scan, *scan8x8, *dc_scan;

//        fill_non_zero_count_cache(h);

        if(IS_INTERLACED(mb_type)){
            scan8x8= s->qscale ? h->field_scan8x8_cavlc : h->field_scan8x8_cavlc_q0;
            scan= s->qscale ? h->field_scan : h->field_scan_q0;
            dc_scan= luma_dc_field_scan;
        }else{
            scan8x8= s->qscale ? h->zigzag_scan8x8_cavlc : h->zigzag_scan8x8_cavlc_q0;
            scan= s->qscale ? h->zigzag_scan : h->zigzag_scan_q0;
            dc_scan= luma_dc_zigzag_scan;
        }

        dquant= get_se_golomb(&s->gb);

        if( dquant > 25 || dquant < -26 ){
            av_log(h->s.avctx, AV_LOG_ERROR, "dquant out of range (%d) at %d %d\n", dquant, s->mb_x, s->mb_y);
            return -1;
        }

        s->qscale += dquant;
        if(((unsigned)s->qscale) > 51){
            if(s->qscale<0) s->qscale+= 52;
            else            s->qscale-= 52;
        }

        h->chroma_qp= chroma_qp= get_chroma_qp(h->pps.chroma_qp_index_offset, s->qscale);
        if(IS_INTRA16x16(mb_type)){
            if( decode_residual(h, h->intra_gb_ptr, h->mb, LUMA_DC_BLOCK_INDEX, dc_scan, h->dequant4_coeff[0][s->qscale], 16) < 0){
                return -1; //FIXME continue if partitioned and other return -1 too
            }

            assert((cbp&15) == 0 || (cbp&15) == 15);

            if(cbp&15){
                for(i8x8=0; i8x8<4; i8x8++){
                    for(i4x4=0; i4x4<4; i4x4++){
                        const int index= i4x4 + 4*i8x8;
                        if( decode_residual(h, h->intra_gb_ptr, h->mb + 16*index, index, scan + 1, h->dequant4_coeff[0][s->qscale], 15) < 0 ){
                            return -1;
                        }
                    }
                }
            }else{
                fill_rectangle(&h->non_zero_count_cache[scan8[0]], 4, 4, 8, 0, 1);
            }
        }else{
            for(i8x8=0; i8x8<4; i8x8++){
                if(cbp & (1<<i8x8)){
                    if(IS_8x8DCT(mb_type)){
                        DCTELEM *buf = &h->mb[64*i8x8];
                        uint8_t *nnz;
                        for(i4x4=0; i4x4<4; i4x4++){
                            if( decode_residual(h, gb, buf, i4x4+4*i8x8, scan8x8+16*i4x4,
                                                h->dequant8_coeff[IS_INTRA( mb_type ) ? 0:1][s->qscale], 16) <0 )
                                return -1;
                        }
                        nnz= &h->non_zero_count_cache[ scan8[4*i8x8] ];
                        nnz[0] += nnz[1] + nnz[8] + nnz[9];
                    }else{
                        for(i4x4=0; i4x4<4; i4x4++){
                            const int index= i4x4 + 4*i8x8;

                            if( decode_residual(h, gb, h->mb + 16*index, index, scan, h->dequant4_coeff[IS_INTRA( mb_type ) ? 0:3][s->qscale], 16) <0 ){
                                return -1;
                            }
                        }
                    }
                }else{
                    uint8_t * const nnz= &h->non_zero_count_cache[ scan8[4*i8x8] ];
                    nnz[0] = nnz[1] = nnz[8] = nnz[9] = 0;
                }
            }
        }

        if(cbp&0x30){
            for(chroma_idx=0; chroma_idx<2; chroma_idx++)
                if( decode_residual(h, gb, h->mb + 256 + 16*4*chroma_idx, CHROMA_DC_BLOCK_INDEX, chroma_dc_scan, NULL, 4) < 0){
                    return -1;
                }
        }

        if(cbp&0x20){
            for(chroma_idx=0; chroma_idx<2; chroma_idx++){
                for(i4x4=0; i4x4<4; i4x4++){
                    const int index= 16 + 4*chroma_idx + i4x4;
                    if( decode_residual(h, gb, h->mb + 16*index, index, scan + 1, h->dequant4_coeff[chroma_idx+1+(IS_INTRA( mb_type ) ? 0:3)][chroma_qp], 15) < 0){
                        return -1;
                    }
                }
            }
        }else{
            uint8_t * const nnz= &h->non_zero_count_cache[0];
            nnz[ scan8[16]+0 ] = nnz[ scan8[16]+1 ] =nnz[ scan8[16]+8 ] =nnz[ scan8[16]+9 ] =
            nnz[ scan8[20]+0 ] = nnz[ scan8[20]+1 ] =nnz[ scan8[20]+8 ] =nnz[ scan8[20]+9 ] = 0;
        }
    }else{
        uint8_t * const nnz= &h->non_zero_count_cache[0];
        fill_rectangle(&nnz[scan8[0]], 4, 4, 8, 0, 1);
        nnz[ scan8[16]+0 ] = nnz[ scan8[16]+1 ] =nnz[ scan8[16]+8 ] =nnz[ scan8[16]+9 ] =
        nnz[ scan8[20]+0 ] = nnz[ scan8[20]+1 ] =nnz[ scan8[20]+8 ] =nnz[ scan8[20]+9 ] = 0;
    }
    s->current_picture.qscale_table[mb_xy]= s->qscale;
    write_back_non_zero_count(h);

    if(MB_MBAFF){
        h->ref_count[0] >>= 1;
        h->ref_count[1] >>= 1;
    }

    return 0;
}

static int decode_cabac_field_decoding_flag(H264Context *h) {
    MpegEncContext * const s = &h->s;
    const int mb_x = s->mb_x;
    const int mb_y = s->mb_y & ~1;
    const int mba_xy = mb_x - 1 +  mb_y   *s->mb_stride;
    const int mbb_xy = mb_x     + (mb_y-2)*s->mb_stride;

    unsigned int ctx = 0;

    if( h->slice_table[mba_xy] == h->slice_num && IS_INTERLACED( s->current_picture.mb_type[mba_xy] ) ) {
        ctx += 1;
    }
    if( h->slice_table[mbb_xy] == h->slice_num && IS_INTERLACED( s->current_picture.mb_type[mbb_xy] ) ) {
        ctx += 1;
    }

    return get_cabac_noinline( &h->cabac, &h->cabac_state[70 + ctx] );
}

static int decode_cabac_intra_mb_type(H264Context *h, int ctx_base, int intra_slice) {
    uint8_t *state= &h->cabac_state[ctx_base];
    int mb_type;

    if(intra_slice){
        MpegEncContext * const s = &h->s;
        const int mba_xy = h->left_mb_xy[0];
        const int mbb_xy = h->top_mb_xy;
        int ctx=0;
        if( h->slice_table[mba_xy] == h->slice_num && !IS_INTRA4x4( s->current_picture.mb_type[mba_xy] ) )
            ctx++;
        if( h->slice_table[mbb_xy] == h->slice_num && !IS_INTRA4x4( s->current_picture.mb_type[mbb_xy] ) )
            ctx++;
        if( get_cabac_noinline( &h->cabac, &state[ctx] ) == 0 )
            return 0;   /* I4x4 */
        state += 2;
    }else{
        if( get_cabac_noinline( &h->cabac, &state[0] ) == 0 )
            return 0;   /* I4x4 */
    }

    if( get_cabac_terminate( &h->cabac ) )
        return 25;  /* PCM */

    mb_type = 1; /* I16x16 */
    mb_type += 12 * get_cabac_noinline( &h->cabac, &state[1] ); /* cbp_luma != 0 */
    if( get_cabac_noinline( &h->cabac, &state[2] ) ) /* cbp_chroma */
        mb_type += 4 + 4 * get_cabac_noinline( &h->cabac, &state[2+intra_slice] );
    mb_type += 2 * get_cabac_noinline( &h->cabac, &state[3+intra_slice] );
    mb_type += 1 * get_cabac_noinline( &h->cabac, &state[3+2*intra_slice] );
    return mb_type;
}

static int decode_cabac_mb_type( H264Context *h ) {
    MpegEncContext * const s = &h->s;

    if( h->slice_type == I_TYPE ) {
        return decode_cabac_intra_mb_type(h, 3, 1);
    } else if( h->slice_type == P_TYPE ) {
        if( get_cabac_noinline( &h->cabac, &h->cabac_state[14] ) == 0 ) {
            /* P-type */
            if( get_cabac_noinline( &h->cabac, &h->cabac_state[15] ) == 0 ) {
                /* P_L0_D16x16, P_8x8 */
                return 3 * get_cabac_noinline( &h->cabac, &h->cabac_state[16] );
            } else {
                /* P_L0_D8x16, P_L0_D16x8 */
                return 2 - get_cabac_noinline( &h->cabac, &h->cabac_state[17] );
            }
        } else {
            return decode_cabac_intra_mb_type(h, 17, 0) + 5;
        }
    } else if( h->slice_type == B_TYPE ) {
        const int mba_xy = h->left_mb_xy[0];
        const int mbb_xy = h->top_mb_xy;
        int ctx = 0;
        int bits;

        if( h->slice_table[mba_xy] == h->slice_num && !IS_DIRECT( s->current_picture.mb_type[mba_xy] ) )
            ctx++;
        if( h->slice_table[mbb_xy] == h->slice_num && !IS_DIRECT( s->current_picture.mb_type[mbb_xy] ) )
            ctx++;

        if( !get_cabac_noinline( &h->cabac, &h->cabac_state[27+ctx] ) )
            return 0; /* B_Direct_16x16 */

        if( !get_cabac_noinline( &h->cabac, &h->cabac_state[27+3] ) ) {
            return 1 + get_cabac_noinline( &h->cabac, &h->cabac_state[27+5] ); /* B_L[01]_16x16 */
        }

        bits = get_cabac_noinline( &h->cabac, &h->cabac_state[27+4] ) << 3;
        bits|= get_cabac_noinline( &h->cabac, &h->cabac_state[27+5] ) << 2;
        bits|= get_cabac_noinline( &h->cabac, &h->cabac_state[27+5] ) << 1;
        bits|= get_cabac_noinline( &h->cabac, &h->cabac_state[27+5] );
        if( bits < 8 )
            return bits + 3; /* B_Bi_16x16 through B_L1_L0_16x8 */
        else if( bits == 13 ) {
            return decode_cabac_intra_mb_type(h, 32, 0) + 23;
        } else if( bits == 14 )
            return 11; /* B_L1_L0_8x16 */
        else if( bits == 15 )
            return 22; /* B_8x8 */

        bits= ( bits<<1 ) | get_cabac_noinline( &h->cabac, &h->cabac_state[27+5] );
        return bits - 4; /* B_L0_Bi_* through B_Bi_Bi_* */
    } else {
        /* TODO SI/SP frames? */
        return -1;
    }
}

static int decode_cabac_mb_skip( H264Context *h, int mb_x, int mb_y ) {
    MpegEncContext * const s = &h->s;
    int mba_xy, mbb_xy;
    int ctx = 0;

    if(FRAME_MBAFF){ //FIXME merge with the stuff in fill_caches?
        int mb_xy = mb_x + (mb_y&~1)*s->mb_stride;
        mba_xy = mb_xy - 1;
        if( (mb_y&1)
            && h->slice_table[mba_xy] == h->slice_num
            && MB_FIELD == !!IS_INTERLACED( s->current_picture.mb_type[mba_xy] ) )
            mba_xy += s->mb_stride;
        if( MB_FIELD ){
            mbb_xy = mb_xy - s->mb_stride;
            if( !(mb_y&1)
                && h->slice_table[mbb_xy] == h->slice_num
                && IS_INTERLACED( s->current_picture.mb_type[mbb_xy] ) )
                mbb_xy -= s->mb_stride;
        }else
            mbb_xy = mb_x + (mb_y-1)*s->mb_stride;
    }else{
        int mb_xy = mb_x + mb_y*s->mb_stride;
        mba_xy = mb_xy - 1;
        mbb_xy = mb_xy - s->mb_stride;
    }

    if( h->slice_table[mba_xy] == h->slice_num && !IS_SKIP( s->current_picture.mb_type[mba_xy] ))
        ctx++;
    if( h->slice_table[mbb_xy] == h->slice_num && !IS_SKIP( s->current_picture.mb_type[mbb_xy] ))
        ctx++;

    if( h->slice_type == B_TYPE )
        ctx += 13;
    return get_cabac_noinline( &h->cabac, &h->cabac_state[11+ctx] );
}

static int decode_cabac_mb_intra4x4_pred_mode( H264Context *h, int pred_mode ) {
    int mode = 0;

    if( get_cabac( &h->cabac, &h->cabac_state[68] ) )
        return pred_mode;

    mode += 1 * get_cabac( &h->cabac, &h->cabac_state[69] );
    mode += 2 * get_cabac( &h->cabac, &h->cabac_state[69] );
    mode += 4 * get_cabac( &h->cabac, &h->cabac_state[69] );

    if( mode >= pred_mode )
        return mode + 1;
    else
        return mode;
}

static int decode_cabac_mb_chroma_pre_mode( H264Context *h) {
    const int mba_xy = h->left_mb_xy[0];
    const int mbb_xy = h->top_mb_xy;

    int ctx = 0;

    /* No need to test for IS_INTRA4x4 and IS_INTRA16x16, as we set chroma_pred_mode_table to 0 */
    if( h->slice_table[mba_xy] == h->slice_num && h->chroma_pred_mode_table[mba_xy] != 0 )
        ctx++;

    if( h->slice_table[mbb_xy] == h->slice_num && h->chroma_pred_mode_table[mbb_xy] != 0 )
        ctx++;

    if( get_cabac_noinline( &h->cabac, &h->cabac_state[64+ctx] ) == 0 )
        return 0;

    if( get_cabac_noinline( &h->cabac, &h->cabac_state[64+3] ) == 0 )
        return 1;
    if( get_cabac_noinline( &h->cabac, &h->cabac_state[64+3] ) == 0 )
        return 2;
    else
        return 3;
}

static const uint8_t block_idx_x[16] = {
    0, 1, 0, 1, 2, 3, 2, 3, 0, 1, 0, 1, 2, 3, 2, 3
};
static const uint8_t block_idx_y[16] = {
    0, 0, 1, 1, 0, 0, 1, 1, 2, 2, 3, 3, 2, 2, 3, 3
};
static const uint8_t block_idx_xy[4][4] = {
    { 0, 2, 8,  10},
    { 1, 3, 9,  11},
    { 4, 6, 12, 14},
    { 5, 7, 13, 15}
};

static int decode_cabac_mb_cbp_luma( H264Context *h) {
    int cbp = 0;
    int cbp_b = -1;
    int i8x8;

    if( h->slice_table[h->top_mb_xy] == h->slice_num ) {
        cbp_b = h->top_cbp;
        tprintf(h->s.avctx, "cbp_b = top_cbp = %x\n", cbp_b);
    }

    for( i8x8 = 0; i8x8 < 4; i8x8++ ) {
        int cbp_a = -1;
        int x, y;
        int ctx = 0;

        x = block_idx_x[4*i8x8];
        y = block_idx_y[4*i8x8];

        if( x > 0 )
            cbp_a = cbp;
        else if( h->slice_table[h->left_mb_xy[0]] == h->slice_num ) {
            cbp_a = h->left_cbp;
            tprintf(h->s.avctx, "cbp_a = left_cbp = %x\n", cbp_a);
        }

        if( y > 0 )
            cbp_b = cbp;

        /* No need to test for skip as we put 0 for skip block */
        /* No need to test for IPCM as we put 1 for IPCM block */
        if( cbp_a >= 0 ) {
            int i8x8a = block_idx_xy[(x-1)&0x03][y]/4;
            if( ((cbp_a >> i8x8a)&0x01) == 0 )
                ctx++;
        }

        if( cbp_b >= 0 ) {
            int i8x8b = block_idx_xy[x][(y-1)&0x03]/4;
            if( ((cbp_b >> i8x8b)&0x01) == 0 )
                ctx += 2;
        }

        if( get_cabac( &h->cabac, &h->cabac_state[73 + ctx] ) ) {
            cbp |= 1 << i8x8;
        }
    }
    return cbp;
}
static int decode_cabac_mb_cbp_chroma( H264Context *h) {
    int ctx;
    int cbp_a, cbp_b;

    cbp_a = (h->left_cbp>>4)&0x03;
    cbp_b = (h-> top_cbp>>4)&0x03;

    ctx = 0;
    if( cbp_a > 0 ) ctx++;
    if( cbp_b > 0 ) ctx += 2;
    if( get_cabac_noinline( &h->cabac, &h->cabac_state[77 + ctx] ) == 0 )
        return 0;

    ctx = 4;
    if( cbp_a == 2 ) ctx++;
    if( cbp_b == 2 ) ctx += 2;
    return 1 + get_cabac_noinline( &h->cabac, &h->cabac_state[77 + ctx] );
}
static int decode_cabac_mb_dqp( H264Context *h) {
    MpegEncContext * const s = &h->s;
    int mbn_xy;
    int   ctx = 0;
    int   val = 0;

    if( s->mb_x > 0 )
        mbn_xy = s->mb_x + s->mb_y*s->mb_stride - 1;
    else
        mbn_xy = s->mb_width - 1 + (s->mb_y-1)*s->mb_stride;

    if( h->last_qscale_diff != 0 )
        ctx++;

    while( get_cabac_noinline( &h->cabac, &h->cabac_state[60 + ctx] ) ) {
        if( ctx < 2 )
            ctx = 2;
        else
            ctx = 3;
        val++;
        if(val > 102) //prevent infinite loop
            return INT_MIN;
    }

    if( val&0x01 )
        return (val + 1)/2;
    else
        return -(val + 1)/2;
}
static int decode_cabac_p_mb_sub_type( H264Context *h ) {
    if( get_cabac( &h->cabac, &h->cabac_state[21] ) )
        return 0;   /* 8x8 */
    if( !get_cabac( &h->cabac, &h->cabac_state[22] ) )
        return 1;   /* 8x4 */
    if( get_cabac( &h->cabac, &h->cabac_state[23] ) )
        return 2;   /* 4x8 */
    return 3;       /* 4x4 */
}
static int decode_cabac_b_mb_sub_type( H264Context *h ) {
    int type;
    if( !get_cabac( &h->cabac, &h->cabac_state[36] ) )
        return 0;   /* B_Direct_8x8 */
    if( !get_cabac( &h->cabac, &h->cabac_state[37] ) )
        return 1 + get_cabac( &h->cabac, &h->cabac_state[39] ); /* B_L0_8x8, B_L1_8x8 */
    type = 3;
    if( get_cabac( &h->cabac, &h->cabac_state[38] ) ) {
        if( get_cabac( &h->cabac, &h->cabac_state[39] ) )
            return 11 + get_cabac( &h->cabac, &h->cabac_state[39] ); /* B_L1_4x4, B_Bi_4x4 */
        type += 4;
    }
    type += 2*get_cabac( &h->cabac, &h->cabac_state[39] );
    type +=   get_cabac( &h->cabac, &h->cabac_state[39] );
    return type;
}

static inline int decode_cabac_mb_transform_size( H264Context *h ) {
    return get_cabac_noinline( &h->cabac, &h->cabac_state[399 + h->neighbor_transform_size] );
}

static int decode_cabac_mb_ref( H264Context *h, int list, int n ) {
    int refa = h->ref_cache[list][scan8[n] - 1];
    int refb = h->ref_cache[list][scan8[n] - 8];
    int ref  = 0;
    int ctx  = 0;

    if( h->slice_type == B_TYPE) {
        if( refa > 0 && !h->direct_cache[scan8[n] - 1] )
            ctx++;
        if( refb > 0 && !h->direct_cache[scan8[n] - 8] )
            ctx += 2;
    } else {
        if( refa > 0 )
            ctx++;
        if( refb > 0 )
            ctx += 2;
    }

    while( get_cabac( &h->cabac, &h->cabac_state[54+ctx] ) ) {
        ref++;
        if( ctx < 4 )
            ctx = 4;
        else
            ctx = 5;
        if(ref >= 32 /*h->ref_list[list]*/){
            av_log(h->s.avctx, AV_LOG_ERROR, "overflow in decode_cabac_mb_ref\n");
            return 0; //FIXME we should return -1 and check the return everywhere
        }
    }
    return ref;
}

static int decode_cabac_mb_mvd( H264Context *h, int list, int n, int l ) {
    int amvd = abs( h->mvd_cache[list][scan8[n] - 1][l] ) +
               abs( h->mvd_cache[list][scan8[n] - 8][l] );
    int ctxbase = (l == 0) ? 40 : 47;
    int ctx, mvd;

    if( amvd < 3 )
        ctx = 0;
    else if( amvd > 32 )
        ctx = 2;
    else
        ctx = 1;

    if(!get_cabac(&h->cabac, &h->cabac_state[ctxbase+ctx]))
        return 0;

    mvd= 1;
    ctx= 3;
    while( mvd < 9 && get_cabac( &h->cabac, &h->cabac_state[ctxbase+ctx] ) ) {
        mvd++;
        if( ctx < 6 )
            ctx++;
    }

    if( mvd >= 9 ) {
        int k = 3;
        while( get_cabac_bypass( &h->cabac ) ) {
            mvd += 1 << k;
            k++;
            if(k>24){
                av_log(h->s.avctx, AV_LOG_ERROR, "overflow in decode_cabac_mb_mvd\n");
                return INT_MIN;
            }
        }
        while( k-- ) {
            if( get_cabac_bypass( &h->cabac ) )
                mvd += 1 << k;
        }
    }
    return get_cabac_bypass_sign( &h->cabac, -mvd );
}

static int inline get_cabac_cbf_ctx( H264Context *h, int cat, int idx ) {
    int nza, nzb;
    int ctx = 0;

    if( cat == 0 ) {
        nza = h->left_cbp&0x100;
        nzb = h-> top_cbp&0x100;
    } else if( cat == 1 || cat == 2 ) {
        nza = h->non_zero_count_cache[scan8[idx] - 1];
        nzb = h->non_zero_count_cache[scan8[idx] - 8];
    } else if( cat == 3 ) {
        nza = (h->left_cbp>>(6+idx))&0x01;
        nzb = (h-> top_cbp>>(6+idx))&0x01;
    } else {
        assert(cat == 4);
        nza = h->non_zero_count_cache[scan8[16+idx] - 1];
        nzb = h->non_zero_count_cache[scan8[16+idx] - 8];
    }

    if( nza > 0 )
        ctx++;

    if( nzb > 0 )
        ctx += 2;

    return ctx + 4 * cat;
}

static const __attribute((used)) uint8_t last_coeff_flag_offset_8x8[63] = {
    0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    3, 3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4,
    5, 5, 5, 5, 6, 6, 6, 6, 7, 7, 7, 7, 8, 8, 8
};

static int decode_cabac_residual( H264Context *h, DCTELEM *block, int cat, int n, const uint8_t *scantable, const uint32_t *qmul, int max_coeff) {
    const int mb_xy  = h->s.mb_x + h->s.mb_y*h->s.mb_stride;
    static const int significant_coeff_flag_offset[2][6] = {
      { 105+0, 105+15, 105+29, 105+44, 105+47, 402 },
      { 277+0, 277+15, 277+29, 277+44, 277+47, 436 }
    };
    static const int last_coeff_flag_offset[2][6] = {
      { 166+0, 166+15, 166+29, 166+44, 166+47, 417 },
      { 338+0, 338+15, 338+29, 338+44, 338+47, 451 }
    };
    static const int coeff_abs_level_m1_offset[6] = {
        227+0, 227+10, 227+20, 227+30, 227+39, 426
    };
    static const uint8_t significant_coeff_flag_offset_8x8[2][63] = {
      { 0, 1, 2, 3, 4, 5, 5, 4, 4, 3, 3, 4, 4, 4, 5, 5,
        4, 4, 4, 4, 3, 3, 6, 7, 7, 7, 8, 9,10, 9, 8, 7,
        7, 6,11,12,13,11, 6, 7, 8, 9,14,10, 9, 8, 6,11,
       12,13,11, 6, 9,14,10, 9,11,12,13,11,14,10,12 },
      { 0, 1, 1, 2, 2, 3, 3, 4, 5, 6, 7, 7, 7, 8, 4, 5,
        6, 9,10,10, 8,11,12,11, 9, 9,10,10, 8,11,12,11,
        9, 9,10,10, 8,11,12,11, 9, 9,10,10, 8,13,13, 9,
        9,10,10, 8,13,13, 9, 9,10,10,14,14,14,14,14 }
    };

    int index[64];

    int last;
    int coeff_count = 0;

    int abslevel1 = 1;
    int abslevelgt1 = 0;

    uint8_t *significant_coeff_ctx_base;
    uint8_t *last_coeff_ctx_base;
    uint8_t *abs_level_m1_ctx_base;

#ifndef ARCH_X86
#define CABAC_ON_STACK
#endif
#ifdef CABAC_ON_STACK
#define CC &cc
    CABACContext cc;
    cc.range     = h->cabac.range;
    cc.low       = h->cabac.low;
    cc.bytestream= h->cabac.bytestream;
#else
#define CC &h->cabac
#endif


    /* cat: 0-> DC 16x16  n = 0
     *      1-> AC 16x16  n = luma4x4idx
     *      2-> Luma4x4   n = luma4x4idx
     *      3-> DC Chroma n = iCbCr
     *      4-> AC Chroma n = 4 * iCbCr + chroma4x4idx
     *      5-> Luma8x8   n = 4 * luma8x8idx
     */

    /* read coded block flag */
    if( cat != 5 ) {
        if( get_cabac( CC, &h->cabac_state[85 + get_cabac_cbf_ctx( h, cat, n ) ] ) == 0 ) {
            if( cat == 1 || cat == 2 )
                h->non_zero_count_cache[scan8[n]] = 0;
            else if( cat == 4 )
                h->non_zero_count_cache[scan8[16+n]] = 0;
#ifdef CABAC_ON_STACK
            h->cabac.range     = cc.range     ;
            h->cabac.low       = cc.low       ;
            h->cabac.bytestream= cc.bytestream;
#endif
            return 0;
        }
    }

    significant_coeff_ctx_base = h->cabac_state
        + significant_coeff_flag_offset[MB_FIELD][cat];
    last_coeff_ctx_base = h->cabac_state
        + last_coeff_flag_offset[MB_FIELD][cat];
    abs_level_m1_ctx_base = h->cabac_state
        + coeff_abs_level_m1_offset[cat];

    if( cat == 5 ) {
#define DECODE_SIGNIFICANCE( coefs, sig_off, last_off ) \
        for(last= 0; last < coefs; last++) { \
            uint8_t *sig_ctx = significant_coeff_ctx_base + sig_off; \
            if( get_cabac( CC, sig_ctx )) { \
                uint8_t *last_ctx = last_coeff_ctx_base + last_off; \
                index[coeff_count++] = last; \
                if( get_cabac( CC, last_ctx ) ) { \
                    last= max_coeff; \
                    break; \
                } \
            } \
        }\
        if( last == max_coeff -1 ) {\
            index[coeff_count++] = last;\
        }
        const uint8_t *sig_off = significant_coeff_flag_offset_8x8[MB_FIELD];
#if defined(ARCH_X86_32) && defined(CONFIG_7REGS) && defined(CONFIG_EBX_AVAILABLE)
        coeff_count= decode_significance_8x8_x86(CC, significant_coeff_ctx_base, index, sig_off);
    } else {
        coeff_count= decode_significance_x86(CC, max_coeff, significant_coeff_ctx_base, index);
#else
        DECODE_SIGNIFICANCE( 63, sig_off[last], last_coeff_flag_offset_8x8[last] );
    } else {
        DECODE_SIGNIFICANCE( max_coeff - 1, last, last );
#endif
    }
    assert(coeff_count > 0);

    if( cat == 0 )
        h->cbp_table[mb_xy] |= 0x100;
    else if( cat == 1 || cat == 2 )
        h->non_zero_count_cache[scan8[n]] = coeff_count;
    else if( cat == 3 )
        h->cbp_table[mb_xy] |= 0x40 << n;
    else if( cat == 4 )
        h->non_zero_count_cache[scan8[16+n]] = coeff_count;
    else {
        assert( cat == 5 );
        fill_rectangle(&h->non_zero_count_cache[scan8[n]], 2, 2, 8, coeff_count, 1);
    }

    for( coeff_count--; coeff_count >= 0; coeff_count-- ) {
        uint8_t *ctx = (abslevelgt1 != 0 ? 0 : FFMIN( 4, abslevel1 )) + abs_level_m1_ctx_base;
        int j= scantable[index[coeff_count]];

        if( get_cabac( CC, ctx ) == 0 ) {
            if( !qmul ) {
                block[j] = get_cabac_bypass_sign( CC, -1);
            }else{
                block[j] = (get_cabac_bypass_sign( CC, -qmul[j]) + 32) >> 6;;
            }

            abslevel1++;
        } else {
            int coeff_abs = 2;
            ctx = 5 + FFMIN( 4, abslevelgt1 ) + abs_level_m1_ctx_base;
            while( coeff_abs < 15 && get_cabac( CC, ctx ) ) {
                coeff_abs++;
            }

            if( coeff_abs >= 15 ) {
                int j = 0;
                while( get_cabac_bypass( CC ) ) {
                    j++;
                }

                coeff_abs=1;
                while( j-- ) {
                    coeff_abs += coeff_abs + get_cabac_bypass( CC );
                }
                coeff_abs+= 14;
            }

            if( !qmul ) {
                if( get_cabac_bypass( CC ) ) block[j] = -coeff_abs;
                else                                block[j] =  coeff_abs;
            }else{
                if( get_cabac_bypass( CC ) ) block[j] = (-coeff_abs * qmul[j] + 32) >> 6;
                else                                block[j] = ( coeff_abs * qmul[j] + 32) >> 6;
            }

            abslevelgt1++;
        }
    }
#ifdef CABAC_ON_STACK
            h->cabac.range     = cc.range     ;
            h->cabac.low       = cc.low       ;
            h->cabac.bytestream= cc.bytestream;
#endif
    return 0;
}

static void inline compute_mb_neighbors(H264Context *h)
{
    MpegEncContext * const s = &h->s;
    const int mb_xy  = s->mb_x + s->mb_y*s->mb_stride;
    h->top_mb_xy     = mb_xy - s->mb_stride;
    h->left_mb_xy[0] = mb_xy - 1;
    if(FRAME_MBAFF){
        const int pair_xy          = s->mb_x     + (s->mb_y & ~1)*s->mb_stride;
        const int top_pair_xy      = pair_xy     - s->mb_stride;
        const int top_mb_frame_flag      = !IS_INTERLACED(s->current_picture.mb_type[top_pair_xy]);
        const int left_mb_frame_flag = !IS_INTERLACED(s->current_picture.mb_type[pair_xy-1]);
        const int curr_mb_frame_flag = !MB_FIELD;
        const int bottom = (s->mb_y & 1);
        if (bottom
                ? !curr_mb_frame_flag // bottom macroblock
                : (!curr_mb_frame_flag && !top_mb_frame_flag) // top macroblock
                ) {
            h->top_mb_xy -= s->mb_stride;
        }
        if (left_mb_frame_flag != curr_mb_frame_flag) {
            h->left_mb_xy[0] = pair_xy - 1;
        }
    }
    return;
}

/**
 * decodes a macroblock
 * @returns 0 if ok, AC_ERROR / DC_ERROR / MV_ERROR if an error is noticed
 */
static int decode_mb_cabac(H264Context *h) {
    MpegEncContext * const s = &h->s;
    const int mb_xy= s->mb_x + s->mb_y*s->mb_stride;
    int mb_type, partition_count, cbp = 0;
    int dct8x8_allowed= h->pps.transform_8x8_mode;

    s->dsp.clear_blocks(h->mb); //FIXME avoid if already clear (move after skip handlong?)

    tprintf(s->avctx, "pic:%d mb:%d/%d\n", h->frame_num, s->mb_x, s->mb_y);
    if( h->slice_type != I_TYPE && h->slice_type != SI_TYPE ) {
        int skip;
        /* a skipped mb needs the aff flag from the following mb */
        if( FRAME_MBAFF && s->mb_x==0 && (s->mb_y&1)==0 )
            predict_field_decoding_flag(h);
        if( FRAME_MBAFF && (s->mb_y&1)==1 && h->prev_mb_skipped )
            skip = h->next_mb_skipped;
        else
            skip = decode_cabac_mb_skip( h, s->mb_x, s->mb_y );
        /* read skip flags */
        if( skip ) {
            if( FRAME_MBAFF && (s->mb_y&1)==0 ){
                s->current_picture.mb_type[mb_xy] = MB_TYPE_SKIP;
                h->next_mb_skipped = decode_cabac_mb_skip( h, s->mb_x, s->mb_y+1 );
                if(h->next_mb_skipped)
                    predict_field_decoding_flag(h);
                else
                    h->mb_mbaff = h->mb_field_decoding_flag = decode_cabac_field_decoding_flag(h);
            }

            decode_mb_skip(h);

            h->cbp_table[mb_xy] = 0;
            h->chroma_pred_mode_table[mb_xy] = 0;
            h->last_qscale_diff = 0;

            return 0;

        }
    }
    if(FRAME_MBAFF){
        if( (s->mb_y&1) == 0 )
            h->mb_mbaff =
            h->mb_field_decoding_flag = decode_cabac_field_decoding_flag(h);
    }else
        h->mb_field_decoding_flag= (s->picture_structure!=PICT_FRAME);

    h->prev_mb_skipped = 0;

    compute_mb_neighbors(h);
    if( ( mb_type = decode_cabac_mb_type( h ) ) < 0 ) {
        av_log( h->s.avctx, AV_LOG_ERROR, "decode_cabac_mb_type failed\n" );
        return -1;
    }

    if( h->slice_type == B_TYPE ) {
        if( mb_type < 23 ){
            partition_count= b_mb_type_info[mb_type].partition_count;
            mb_type=         b_mb_type_info[mb_type].type;
        }else{
            mb_type -= 23;
            goto decode_intra_mb;
        }
    } else if( h->slice_type == P_TYPE ) {
        if( mb_type < 5) {
            partition_count= p_mb_type_info[mb_type].partition_count;
            mb_type=         p_mb_type_info[mb_type].type;
        } else {
            mb_type -= 5;
            goto decode_intra_mb;
        }
    } else {
       assert(h->slice_type == I_TYPE);
decode_intra_mb:
        partition_count = 0;
        cbp= i_mb_type_info[mb_type].cbp;
        h->intra16x16_pred_mode= i_mb_type_info[mb_type].pred_mode;
        mb_type= i_mb_type_info[mb_type].type;
    }
    if(MB_FIELD)
        mb_type |= MB_TYPE_INTERLACED;

    h->slice_table[ mb_xy ]= h->slice_num;

    if(IS_INTRA_PCM(mb_type)) {
        const uint8_t *ptr;
        unsigned int x, y;

        // We assume these blocks are very rare so we dont optimize it.
        // FIXME The two following lines get the bitstream position in the cabac
        // decode, I think it should be done by a function in cabac.h (or cabac.c).
        ptr= h->cabac.bytestream;
        if(h->cabac.low&0x1) ptr--;
        if(CABAC_BITS==16){
            if(h->cabac.low&0x1FF) ptr--;
        }

        // The pixels are stored in the same order as levels in h->mb array.
        for(y=0; y<16; y++){
            const int index= 4*(y&3) + 32*((y>>2)&1) + 128*(y>>3);
            for(x=0; x<16; x++){
                tprintf(s->avctx, "LUMA ICPM LEVEL (%3d)\n", *ptr);
                h->mb[index + (x&3) + 16*((x>>2)&1) + 64*(x>>3)]= *ptr++;
            }
        }
        for(y=0; y<8; y++){
            const int index= 256 + 4*(y&3) + 32*(y>>2);
            for(x=0; x<8; x++){
                tprintf(s->avctx, "CHROMA U ICPM LEVEL (%3d)\n", *ptr);
                h->mb[index + (x&3) + 16*(x>>2)]= *ptr++;
            }
        }
        for(y=0; y<8; y++){
            const int index= 256 + 64 + 4*(y&3) + 32*(y>>2);
            for(x=0; x<8; x++){
                tprintf(s->avctx, "CHROMA V ICPM LEVEL (%3d)\n", *ptr);
                h->mb[index + (x&3) + 16*(x>>2)]= *ptr++;
            }
        }

        ff_init_cabac_decoder(&h->cabac, ptr, h->cabac.bytestream_end - ptr);

        // All blocks are present
        h->cbp_table[mb_xy] = 0x1ef;
        h->chroma_pred_mode_table[mb_xy] = 0;
        // In deblocking, the quantizer is 0
        s->current_picture.qscale_table[mb_xy]= 0;
        h->chroma_qp = get_chroma_qp(h->pps.chroma_qp_index_offset, 0);
        // All coeffs are present
        memset(h->non_zero_count[mb_xy], 16, 16);
        s->current_picture.mb_type[mb_xy]= mb_type;
        return 0;
    }

    if(MB_MBAFF){
        h->ref_count[0] <<= 1;
        h->ref_count[1] <<= 1;
    }

    fill_caches(h, mb_type, 0);

    if( IS_INTRA( mb_type ) ) {
        int i, pred_mode;
        if( IS_INTRA4x4( mb_type ) ) {
            if( dct8x8_allowed && decode_cabac_mb_transform_size( h ) ) {
                mb_type |= MB_TYPE_8x8DCT;
                for( i = 0; i < 16; i+=4 ) {
                    int pred = pred_intra_mode( h, i );
                    int mode = decode_cabac_mb_intra4x4_pred_mode( h, pred );
                    fill_rectangle( &h->intra4x4_pred_mode_cache[ scan8[i] ], 2, 2, 8, mode, 1 );
                }
            } else {
                for( i = 0; i < 16; i++ ) {
                    int pred = pred_intra_mode( h, i );
                    h->intra4x4_pred_mode_cache[ scan8[i] ] = decode_cabac_mb_intra4x4_pred_mode( h, pred );

                //av_log( s->avctx, AV_LOG_ERROR, "i4x4 pred=%d mode=%d\n", pred, h->intra4x4_pred_mode_cache[ scan8[i] ] );
                }
            }
            write_back_intra_pred_mode(h);
            if( check_intra4x4_pred_mode(h) < 0 ) return -1;
        } else {
            h->intra16x16_pred_mode= check_intra_pred_mode( h, h->intra16x16_pred_mode );
            if( h->intra16x16_pred_mode < 0 ) return -1;
        }
        h->chroma_pred_mode_table[mb_xy] =
        pred_mode                        = decode_cabac_mb_chroma_pre_mode( h );

        pred_mode= check_intra_pred_mode( h, pred_mode );
        if( pred_mode < 0 ) return -1;
        h->chroma_pred_mode= pred_mode;
    } else if( partition_count == 4 ) {
        int i, j, sub_partition_count[4], list, ref[2][4];

        if( h->slice_type == B_TYPE ) {
            for( i = 0; i < 4; i++ ) {
                h->sub_mb_type[i] = decode_cabac_b_mb_sub_type( h );
                sub_partition_count[i]= b_sub_mb_type_info[ h->sub_mb_type[i] ].partition_count;
                h->sub_mb_type[i]=      b_sub_mb_type_info[ h->sub_mb_type[i] ].type;
            }
            if( IS_DIRECT(h->sub_mb_type[0] | h->sub_mb_type[1] |
                          h->sub_mb_type[2] | h->sub_mb_type[3]) ) {
                pred_direct_motion(h, &mb_type);
                if( h->ref_count[0] > 1 || h->ref_count[1] > 1 ) {
                    for( i = 0; i < 4; i++ )
                        if( IS_DIRECT(h->sub_mb_type[i]) )
                            fill_rectangle( &h->direct_cache[scan8[4*i]], 2, 2, 8, 1, 1 );
                }
            }
        } else {
            for( i = 0; i < 4; i++ ) {
                h->sub_mb_type[i] = decode_cabac_p_mb_sub_type( h );
                sub_partition_count[i]= p_sub_mb_type_info[ h->sub_mb_type[i] ].partition_count;
                h->sub_mb_type[i]=      p_sub_mb_type_info[ h->sub_mb_type[i] ].type;
            }
        }

        for( list = 0; list < h->list_count; list++ ) {
                for( i = 0; i < 4; i++ ) {
                    if(IS_DIRECT(h->sub_mb_type[i])) continue;
                    if(IS_DIR(h->sub_mb_type[i], 0, list)){
                        if( h->ref_count[list] > 1 )
                            ref[list][i] = decode_cabac_mb_ref( h, list, 4*i );
                        else
                            ref[list][i] = 0;
                    } else {
                        ref[list][i] = -1;
                    }
                                                       h->ref_cache[list][ scan8[4*i]+1 ]=
                    h->ref_cache[list][ scan8[4*i]+8 ]=h->ref_cache[list][ scan8[4*i]+9 ]= ref[list][i];
                }
        }

        if(dct8x8_allowed)
            dct8x8_allowed = get_dct8x8_allowed(h);

        for(list=0; list<h->list_count; list++){
            for(i=0; i<4; i++){
                if(IS_DIRECT(h->sub_mb_type[i])){
                    fill_rectangle(h->mvd_cache[list][scan8[4*i]], 2, 2, 8, 0, 4);
                    continue;
                }
                h->ref_cache[list][ scan8[4*i]   ]=h->ref_cache[list][ scan8[4*i]+1 ];

                if(IS_DIR(h->sub_mb_type[i], 0, list) && !IS_DIRECT(h->sub_mb_type[i])){
                    const int sub_mb_type= h->sub_mb_type[i];
                    const int block_width= (sub_mb_type & (MB_TYPE_16x16|MB_TYPE_16x8)) ? 2 : 1;
                    for(j=0; j<sub_partition_count[i]; j++){
                        int mpx, mpy;
                        int mx, my;
                        const int index= 4*i + block_width*j;
                        int16_t (* mv_cache)[2]= &h->mv_cache[list][ scan8[index] ];
                        int16_t (* mvd_cache)[2]= &h->mvd_cache[list][ scan8[index] ];
                        pred_motion(h, index, block_width, list, h->ref_cache[list][ scan8[index] ], &mpx, &mpy);

                        mx = mpx + decode_cabac_mb_mvd( h, list, index, 0 );
                        my = mpy + decode_cabac_mb_mvd( h, list, index, 1 );
                        tprintf(s->avctx, "final mv:%d %d\n", mx, my);

                        if(IS_SUB_8X8(sub_mb_type)){
                            mv_cache[ 1 ][0]=
                            mv_cache[ 8 ][0]= mv_cache[ 9 ][0]= mx;
                            mv_cache[ 1 ][1]=
                            mv_cache[ 8 ][1]= mv_cache[ 9 ][1]= my;

                            mvd_cache[ 1 ][0]=
                            mvd_cache[ 8 ][0]= mvd_cache[ 9 ][0]= mx - mpx;
                            mvd_cache[ 1 ][1]=
                            mvd_cache[ 8 ][1]= mvd_cache[ 9 ][1]= my - mpy;
                        }else if(IS_SUB_8X4(sub_mb_type)){
                            mv_cache[ 1 ][0]= mx;
                            mv_cache[ 1 ][1]= my;

                            mvd_cache[ 1 ][0]= mx - mpx;
                            mvd_cache[ 1 ][1]= my - mpy;
                        }else if(IS_SUB_4X8(sub_mb_type)){
                            mv_cache[ 8 ][0]= mx;
                            mv_cache[ 8 ][1]= my;

                            mvd_cache[ 8 ][0]= mx - mpx;
                            mvd_cache[ 8 ][1]= my - mpy;
                        }
                        mv_cache[ 0 ][0]= mx;
                        mv_cache[ 0 ][1]= my;

                        mvd_cache[ 0 ][0]= mx - mpx;
                        mvd_cache[ 0 ][1]= my - mpy;
                    }
                }else{
                    uint32_t *p= (uint32_t *)&h->mv_cache[list][ scan8[4*i] ][0];
                    uint32_t *pd= (uint32_t *)&h->mvd_cache[list][ scan8[4*i] ][0];
                    p[0] = p[1] = p[8] = p[9] = 0;
                    pd[0]= pd[1]= pd[8]= pd[9]= 0;
                }
            }
        }
    } else if( IS_DIRECT(mb_type) ) {
        pred_direct_motion(h, &mb_type);
        fill_rectangle(h->mvd_cache[0][scan8[0]], 4, 4, 8, 0, 4);
        fill_rectangle(h->mvd_cache[1][scan8[0]], 4, 4, 8, 0, 4);
        dct8x8_allowed &= h->sps.direct_8x8_inference_flag;
    } else {
        int list, mx, my, i, mpx, mpy;
        if(IS_16X16(mb_type)){
            for(list=0; list<h->list_count; list++){
                if(IS_DIR(mb_type, 0, list)){
                        const int ref = h->ref_count[list] > 1 ? decode_cabac_mb_ref( h, list, 0 ) : 0;
                        fill_rectangle(&h->ref_cache[list][ scan8[0] ], 4, 4, 8, ref, 1);
                }else
                    fill_rectangle(&h->ref_cache[list][ scan8[0] ], 4, 4, 8, (uint8_t)LIST_NOT_USED, 1); //FIXME factorize and the other fill_rect below too
            }
            for(list=0; list<h->list_count; list++){
                if(IS_DIR(mb_type, 0, list)){
                    pred_motion(h, 0, 4, list, h->ref_cache[list][ scan8[0] ], &mpx, &mpy);

                    mx = mpx + decode_cabac_mb_mvd( h, list, 0, 0 );
                    my = mpy + decode_cabac_mb_mvd( h, list, 0, 1 );
                    tprintf(s->avctx, "final mv:%d %d\n", mx, my);

                    fill_rectangle(h->mvd_cache[list][ scan8[0] ], 4, 4, 8, pack16to32(mx-mpx,my-mpy), 4);
                    fill_rectangle(h->mv_cache[list][ scan8[0] ], 4, 4, 8, pack16to32(mx,my), 4);
                }else
                    fill_rectangle(h->mv_cache[list][ scan8[0] ], 4, 4, 8, 0, 4);
            }
        }
        else if(IS_16X8(mb_type)){
            for(list=0; list<h->list_count; list++){
                    for(i=0; i<2; i++){
                        if(IS_DIR(mb_type, i, list)){
                            const int ref= h->ref_count[list] > 1 ? decode_cabac_mb_ref( h, list, 8*i ) : 0;
                            fill_rectangle(&h->ref_cache[list][ scan8[0] + 16*i ], 4, 2, 8, ref, 1);
                        }else
                            fill_rectangle(&h->ref_cache[list][ scan8[0] + 16*i ], 4, 2, 8, (LIST_NOT_USED&0xFF), 1);
                    }
            }
            for(list=0; list<h->list_count; list++){
                for(i=0; i<2; i++){
                    if(IS_DIR(mb_type, i, list)){
                        pred_16x8_motion(h, 8*i, list, h->ref_cache[list][scan8[0] + 16*i], &mpx, &mpy);
                        mx = mpx + decode_cabac_mb_mvd( h, list, 8*i, 0 );
                        my = mpy + decode_cabac_mb_mvd( h, list, 8*i, 1 );
                        tprintf(s->avctx, "final mv:%d %d\n", mx, my);

                        fill_rectangle(h->mvd_cache[list][ scan8[0] + 16*i ], 4, 2, 8, pack16to32(mx-mpx,my-mpy), 4);
                        fill_rectangle(h->mv_cache[list][ scan8[0] + 16*i ], 4, 2, 8, pack16to32(mx,my), 4);
                    }else{
                        fill_rectangle(h->mvd_cache[list][ scan8[0] + 16*i ], 4, 2, 8, 0, 4);
                        fill_rectangle(h-> mv_cache[list][ scan8[0] + 16*i ], 4, 2, 8, 0, 4);
                    }
                }
            }
        }else{
            assert(IS_8X16(mb_type));
            for(list=0; list<h->list_count; list++){
                    for(i=0; i<2; i++){
                        if(IS_DIR(mb_type, i, list)){ //FIXME optimize
                            const int ref= h->ref_count[list] > 1 ? decode_cabac_mb_ref( h, list, 4*i ) : 0;
                            fill_rectangle(&h->ref_cache[list][ scan8[0] + 2*i ], 2, 4, 8, ref, 1);
                        }else
                            fill_rectangle(&h->ref_cache[list][ scan8[0] + 2*i ], 2, 4, 8, (LIST_NOT_USED&0xFF), 1);
                    }
            }
            for(list=0; list<h->list_count; list++){
                for(i=0; i<2; i++){
                    if(IS_DIR(mb_type, i, list)){
                        pred_8x16_motion(h, i*4, list, h->ref_cache[list][ scan8[0] + 2*i ], &mpx, &mpy);
                        mx = mpx + decode_cabac_mb_mvd( h, list, 4*i, 0 );
                        my = mpy + decode_cabac_mb_mvd( h, list, 4*i, 1 );

                        tprintf(s->avctx, "final mv:%d %d\n", mx, my);
                        fill_rectangle(h->mvd_cache[list][ scan8[0] + 2*i ], 2, 4, 8, pack16to32(mx-mpx,my-mpy), 4);
                        fill_rectangle(h->mv_cache[list][ scan8[0] + 2*i ], 2, 4, 8, pack16to32(mx,my), 4);
                    }else{
                        fill_rectangle(h->mvd_cache[list][ scan8[0] + 2*i ], 2, 4, 8, 0, 4);
                        fill_rectangle(h-> mv_cache[list][ scan8[0] + 2*i ], 2, 4, 8, 0, 4);
                    }
                }
            }
        }
    }

   if( IS_INTER( mb_type ) ) {
        h->chroma_pred_mode_table[mb_xy] = 0;
        write_back_motion( h, mb_type );
   }

    if( !IS_INTRA16x16( mb_type ) ) {
        cbp  = decode_cabac_mb_cbp_luma( h );
        cbp |= decode_cabac_mb_cbp_chroma( h ) << 4;
    }

    h->cbp_table[mb_xy] = h->cbp = cbp;

    if( dct8x8_allowed && (cbp&15) && !IS_INTRA( mb_type ) ) {
        if( decode_cabac_mb_transform_size( h ) )
            mb_type |= MB_TYPE_8x8DCT;
    }
    s->current_picture.mb_type[mb_xy]= mb_type;

    if( cbp || IS_INTRA16x16( mb_type ) ) {
        const uint8_t *scan, *scan8x8, *dc_scan;
        int dqp;

        if(IS_INTERLACED(mb_type)){
            scan8x8= s->qscale ? h->field_scan8x8 : h->field_scan8x8_q0;
            scan= s->qscale ? h->field_scan : h->field_scan_q0;
            dc_scan= luma_dc_field_scan;
        }else{
            scan8x8= s->qscale ? h->zigzag_scan8x8 : h->zigzag_scan8x8_q0;
            scan= s->qscale ? h->zigzag_scan : h->zigzag_scan_q0;
            dc_scan= luma_dc_zigzag_scan;
        }

        h->last_qscale_diff = dqp = decode_cabac_mb_dqp( h );
        if( dqp == INT_MIN ){
            av_log(h->s.avctx, AV_LOG_ERROR, "cabac decode of qscale diff failed at %d %d\n", s->mb_x, s->mb_y);
            return -1;
        }
        s->qscale += dqp;
        if(((unsigned)s->qscale) > 51){
            if(s->qscale<0) s->qscale+= 52;
            else            s->qscale-= 52;
        }
        h->chroma_qp = get_chroma_qp(h->pps.chroma_qp_index_offset, s->qscale);

        if( IS_INTRA16x16( mb_type ) ) {
            int i;
            //av_log( s->avctx, AV_LOG_ERROR, "INTRA16x16 DC\n" );
            if( decode_cabac_residual( h, h->mb, 0, 0, dc_scan, NULL, 16) < 0)
                return -1;
            if( cbp&15 ) {
                for( i = 0; i < 16; i++ ) {
                    //av_log( s->avctx, AV_LOG_ERROR, "INTRA16x16 AC:%d\n", i );
                    if( decode_cabac_residual(h, h->mb + 16*i, 1, i, scan + 1, h->dequant4_coeff[0][s->qscale], 15) < 0 )
                        return -1;
                }
            } else {
                fill_rectangle(&h->non_zero_count_cache[scan8[0]], 4, 4, 8, 0, 1);
            }
        } else {
            int i8x8, i4x4;
            for( i8x8 = 0; i8x8 < 4; i8x8++ ) {
                if( cbp & (1<<i8x8) ) {
                    if( IS_8x8DCT(mb_type) ) {
                        if( decode_cabac_residual(h, h->mb + 64*i8x8, 5, 4*i8x8,
                            scan8x8, h->dequant8_coeff[IS_INTRA( mb_type ) ? 0:1][s->qscale], 64) < 0 )
                            return -1;
                    } else
                    for( i4x4 = 0; i4x4 < 4; i4x4++ ) {
                        const int index = 4*i8x8 + i4x4;
                        //av_log( s->avctx, AV_LOG_ERROR, "Luma4x4: %d\n", index );
//START_TIMER
                        if( decode_cabac_residual(h, h->mb + 16*index, 2, index, scan, h->dequant4_coeff[IS_INTRA( mb_type ) ? 0:3][s->qscale], 16) < 0 )
                            return -1;
//STOP_TIMER("decode_residual")
                    }
                } else {
                    uint8_t * const nnz= &h->non_zero_count_cache[ scan8[4*i8x8] ];
                    nnz[0] = nnz[1] = nnz[8] = nnz[9] = 0;
                }
            }
        }

        if( cbp&0x30 ){
            int c;
            for( c = 0; c < 2; c++ ) {
                //av_log( s->avctx, AV_LOG_ERROR, "INTRA C%d-DC\n",c );
                if( decode_cabac_residual(h, h->mb + 256 + 16*4*c, 3, c, chroma_dc_scan, NULL, 4) < 0)
                    return -1;
            }
        }

        if( cbp&0x20 ) {
            int c, i;
            for( c = 0; c < 2; c++ ) {
                for( i = 0; i < 4; i++ ) {
                    const int index = 16 + 4 * c + i;
                    //av_log( s->avctx, AV_LOG_ERROR, "INTRA C%d-AC %d\n",c, index - 16 );
                    if( decode_cabac_residual(h, h->mb + 16*index, 4, index - 16, scan + 1, h->dequant4_coeff[c+1+(IS_INTRA( mb_type ) ? 0:3)][h->chroma_qp], 15) < 0)
                        return -1;
                }
            }
        } else {
            uint8_t * const nnz= &h->non_zero_count_cache[0];
            nnz[ scan8[16]+0 ] = nnz[ scan8[16]+1 ] =nnz[ scan8[16]+8 ] =nnz[ scan8[16]+9 ] =
            nnz[ scan8[20]+0 ] = nnz[ scan8[20]+1 ] =nnz[ scan8[20]+8 ] =nnz[ scan8[20]+9 ] = 0;
        }
    } else {
        uint8_t * const nnz= &h->non_zero_count_cache[0];
        fill_rectangle(&nnz[scan8[0]], 4, 4, 8, 0, 1);
        nnz[ scan8[16]+0 ] = nnz[ scan8[16]+1 ] =nnz[ scan8[16]+8 ] =nnz[ scan8[16]+9 ] =
        nnz[ scan8[20]+0 ] = nnz[ scan8[20]+1 ] =nnz[ scan8[20]+8 ] =nnz[ scan8[20]+9 ] = 0;
        h->last_qscale_diff = 0;
    }

    s->current_picture.qscale_table[mb_xy]= s->qscale;
    write_back_non_zero_count(h);

    if(MB_MBAFF){
        h->ref_count[0] >>= 1;
        h->ref_count[1] >>= 1;
    }

    return 0;
}


static void filter_mb_edgev( H264Context *h, uint8_t *pix, int stride, int16_t bS[4], int qp ) {
    int i, d;
    const int index_a = qp + h->slice_alpha_c0_offset;
    const int alpha = (alpha_table+52)[index_a];
    const int beta  = (beta_table+52)[qp + h->slice_beta_offset];

    if( bS[0] < 4 ) {
        int8_t tc[4];
        for(i=0; i<4; i++)
            tc[i] = bS[i] ? (tc0_table+52)[index_a][bS[i] - 1] : -1;
        h->s.dsp.h264_h_loop_filter_luma(pix, stride, alpha, beta, tc);
    } else {
        /* 16px edge length, because bS=4 is triggered by being at
         * the edge of an intra MB, so all 4 bS are the same */
            for( d = 0; d < 16; d++ ) {
                const int p0 = pix[-1];
                const int p1 = pix[-2];
                const int p2 = pix[-3];

                const int q0 = pix[0];
                const int q1 = pix[1];
                const int q2 = pix[2];

                if( FFABS( p0 - q0 ) < alpha &&
                    FFABS( p1 - p0 ) < beta &&
                    FFABS( q1 - q0 ) < beta ) {

                    if(FFABS( p0 - q0 ) < (( alpha >> 2 ) + 2 )){
                        if( FFABS( p2 - p0 ) < beta)
                        {
                            const int p3 = pix[-4];
                            /* p0', p1', p2' */
                            pix[-1] = ( p2 + 2*p1 + 2*p0 + 2*q0 + q1 + 4 ) >> 3;
                            pix[-2] = ( p2 + p1 + p0 + q0 + 2 ) >> 2;
                            pix[-3] = ( 2*p3 + 3*p2 + p1 + p0 + q0 + 4 ) >> 3;
                        } else {
                            /* p0' */
                            pix[-1] = ( 2*p1 + p0 + q1 + 2 ) >> 2;
                        }
                        if( FFABS( q2 - q0 ) < beta)
                        {
                            const int q3 = pix[3];
                            /* q0', q1', q2' */
                            pix[0] = ( p1 + 2*p0 + 2*q0 + 2*q1 + q2 + 4 ) >> 3;
                            pix[1] = ( p0 + q0 + q1 + q2 + 2 ) >> 2;
                            pix[2] = ( 2*q3 + 3*q2 + q1 + q0 + p0 + 4 ) >> 3;
                        } else {
                            /* q0' */
                            pix[0] = ( 2*q1 + q0 + p1 + 2 ) >> 2;
                        }
                    }else{
                        /* p0', q0' */
                        pix[-1] = ( 2*p1 + p0 + q1 + 2 ) >> 2;
                        pix[ 0] = ( 2*q1 + q0 + p1 + 2 ) >> 2;
                    }
                    tprintf(h->s.avctx, "filter_mb_edgev i:%d d:%d\n# bS:4 -> [%02x, %02x, %02x, %02x, %02x, %02x] =>[%02x, %02x, %02x, %02x]\n", i, d, p2, p1, p0, q0, q1, q2, pix[-2], pix[-1], pix[0], pix[1]);
                }
                pix += stride;
            }
    }
}
static void filter_mb_edgecv( H264Context *h, uint8_t *pix, int stride, int16_t bS[4], int qp ) {
    int i;
    const int index_a = qp + h->slice_alpha_c0_offset;
    const int alpha = (alpha_table+52)[index_a];
    const int beta  = (beta_table+52)[qp + h->slice_beta_offset];

    if( bS[0] < 4 ) {
        int8_t tc[4];
        for(i=0; i<4; i++)
            tc[i] = bS[i] ? (tc0_table+52)[index_a][bS[i] - 1] + 1 : 0;
        h->s.dsp.h264_h_loop_filter_chroma(pix, stride, alpha, beta, tc);
    } else {
        h->s.dsp.h264_h_loop_filter_chroma_intra(pix, stride, alpha, beta);
    }
}

static void filter_mb_mbaff_edgev( H264Context *h, uint8_t *pix, int stride, int16_t bS[8], int qp[2] ) {
    int i;
    for( i = 0; i < 16; i++, pix += stride) {
        int index_a;
        int alpha;
        int beta;

        int qp_index;
        int bS_index = (i >> 1);
        if (!MB_FIELD) {
            bS_index &= ~1;
            bS_index |= (i & 1);
        }

        if( bS[bS_index] == 0 ) {
            continue;
        }

        qp_index = MB_FIELD ? (i >> 3) : (i & 1);
        index_a = qp[qp_index] + h->slice_alpha_c0_offset;
        alpha = (alpha_table+52)[index_a];
        beta  = (beta_table+52)[qp[qp_index] + h->slice_beta_offset];

        if( bS[bS_index] < 4 ) {
            const int tc0 = (tc0_table+52)[index_a][bS[bS_index] - 1];
            const int p0 = pix[-1];
            const int p1 = pix[-2];
            const int p2 = pix[-3];
            const int q0 = pix[0];
            const int q1 = pix[1];
            const int q2 = pix[2];

            if( FFABS( p0 - q0 ) < alpha &&
                FFABS( p1 - p0 ) < beta &&
                FFABS( q1 - q0 ) < beta ) {
                int tc = tc0;
                int i_delta;

                if( FFABS( p2 - p0 ) < beta ) {
                    pix[-2] = p1 + av_clip( ( p2 + ( ( p0 + q0 + 1 ) >> 1 ) - ( p1 << 1 ) ) >> 1, -tc0, tc0 );
                    tc++;
                }
                if( FFABS( q2 - q0 ) < beta ) {
                    pix[1] = q1 + av_clip( ( q2 + ( ( p0 + q0 + 1 ) >> 1 ) - ( q1 << 1 ) ) >> 1, -tc0, tc0 );
                    tc++;
                }

                i_delta = av_clip( (((q0 - p0 ) << 2) + (p1 - q1) + 4) >> 3, -tc, tc );
                pix[-1] = av_clip_uint8( p0 + i_delta );    /* p0' */
                pix[0]  = av_clip_uint8( q0 - i_delta );    /* q0' */
                tprintf(h->s.avctx, "filter_mb_mbaff_edgev i:%d, qp:%d, indexA:%d, alpha:%d, beta:%d, tc:%d\n# bS:%d -> [%02x, %02x, %02x, %02x, %02x, %02x] =>[%02x, %02x, %02x, %02x]\n", i, qp[qp_index], index_a, alpha, beta, tc, bS[bS_index], pix[-3], p1, p0, q0, q1, pix[2], p1, pix[-1], pix[0], q1);
            }
        }else{
            const int p0 = pix[-1];
            const int p1 = pix[-2];
            const int p2 = pix[-3];

            const int q0 = pix[0];
            const int q1 = pix[1];
            const int q2 = pix[2];

            if( FFABS( p0 - q0 ) < alpha &&
                FFABS( p1 - p0 ) < beta &&
                FFABS( q1 - q0 ) < beta ) {

                if(FFABS( p0 - q0 ) < (( alpha >> 2 ) + 2 )){
                    if( FFABS( p2 - p0 ) < beta)
                    {
                        const int p3 = pix[-4];
                        /* p0', p1', p2' */
                        pix[-1] = ( p2 + 2*p1 + 2*p0 + 2*q0 + q1 + 4 ) >> 3;
                        pix[-2] = ( p2 + p1 + p0 + q0 + 2 ) >> 2;
                        pix[-3] = ( 2*p3 + 3*p2 + p1 + p0 + q0 + 4 ) >> 3;
                    } else {
                        /* p0' */
                        pix[-1] = ( 2*p1 + p0 + q1 + 2 ) >> 2;
                    }
                    if( FFABS( q2 - q0 ) < beta)
                    {
                        const int q3 = pix[3];
                        /* q0', q1', q2' */
                        pix[0] = ( p1 + 2*p0 + 2*q0 + 2*q1 + q2 + 4 ) >> 3;
                        pix[1] = ( p0 + q0 + q1 + q2 + 2 ) >> 2;
                        pix[2] = ( 2*q3 + 3*q2 + q1 + q0 + p0 + 4 ) >> 3;
                    } else {
                        /* q0' */
                        pix[0] = ( 2*q1 + q0 + p1 + 2 ) >> 2;
                    }
                }else{
                    /* p0', q0' */
                    pix[-1] = ( 2*p1 + p0 + q1 + 2 ) >> 2;
                    pix[ 0] = ( 2*q1 + q0 + p1 + 2 ) >> 2;
                }
                tprintf(h->s.avctx, "filter_mb_mbaff_edgev i:%d, qp:%d, indexA:%d, alpha:%d, beta:%d\n# bS:4 -> [%02x, %02x, %02x, %02x, %02x, %02x] =>[%02x, %02x, %02x, %02x, %02x, %02x]\n", i, qp[qp_index], index_a, alpha, beta, p2, p1, p0, q0, q1, q2, pix[-3], pix[-2], pix[-1], pix[0], pix[1], pix[2]);
            }
        }
    }
}
static void filter_mb_mbaff_edgecv( H264Context *h, uint8_t *pix, int stride, int16_t bS[8], int qp[2] ) {
    int i;
    for( i = 0; i < 8; i++, pix += stride) {
        int index_a;
        int alpha;
        int beta;

        int qp_index;
        int bS_index = i;

        if( bS[bS_index] == 0 ) {
            continue;
        }

        qp_index = MB_FIELD ? (i >> 2) : (i & 1);
        index_a = qp[qp_index] + h->slice_alpha_c0_offset;
        alpha = (alpha_table+52)[index_a];
        beta  = (beta_table+52)[qp[qp_index] + h->slice_beta_offset];

        if( bS[bS_index] < 4 ) {
            const int tc = (tc0_table+52)[index_a][bS[bS_index] - 1] + 1;
            const int p0 = pix[-1];
            const int p1 = pix[-2];
            const int q0 = pix[0];
            const int q1 = pix[1];

            if( FFABS( p0 - q0 ) < alpha &&
                FFABS( p1 - p0 ) < beta &&
                FFABS( q1 - q0 ) < beta ) {
                const int i_delta = av_clip( (((q0 - p0 ) << 2) + (p1 - q1) + 4) >> 3, -tc, tc );

                pix[-1] = av_clip_uint8( p0 + i_delta );    /* p0' */
                pix[0]  = av_clip_uint8( q0 - i_delta );    /* q0' */
                tprintf(h->s.avctx, "filter_mb_mbaff_edgecv i:%d, qp:%d, indexA:%d, alpha:%d, beta:%d, tc:%d\n# bS:%d -> [%02x, %02x, %02x, %02x, %02x, %02x] =>[%02x, %02x, %02x, %02x]\n", i, qp[qp_index], index_a, alpha, beta, tc, bS[bS_index], pix[-3], p1, p0, q0, q1, pix[2], p1, pix[-1], pix[0], q1);
            }
        }else{
            const int p0 = pix[-1];
            const int p1 = pix[-2];
            const int q0 = pix[0];
            const int q1 = pix[1];

            if( FFABS( p0 - q0 ) < alpha &&
                FFABS( p1 - p0 ) < beta &&
                FFABS( q1 - q0 ) < beta ) {

                pix[-1] = ( 2*p1 + p0 + q1 + 2 ) >> 2;   /* p0' */
                pix[0]  = ( 2*q1 + q0 + p1 + 2 ) >> 2;   /* q0' */
                tprintf(h->s.avctx, "filter_mb_mbaff_edgecv i:%d\n# bS:4 -> [%02x, %02x, %02x, %02x, %02x, %02x] =>[%02x, %02x, %02x, %02x, %02x, %02x]\n", i, pix[-3], p1, p0, q0, q1, pix[2], pix[-3], pix[-2], pix[-1], pix[0], pix[1], pix[2]);
            }
        }
    }
}

static void filter_mb_edgeh( H264Context *h, uint8_t *pix, int stride, int16_t bS[4], int qp ) {
    int i, d;
    const int index_a = qp + h->slice_alpha_c0_offset;
    const int alpha = (alpha_table+52)[index_a];
    const int beta  = (beta_table+52)[qp + h->slice_beta_offset];
    const int pix_next  = stride;

    if( bS[0] < 4 ) {
        int8_t tc[4];
        for(i=0; i<4; i++)
            tc[i] = bS[i] ? (tc0_table+52)[index_a][bS[i] - 1] : -1;
        h->s.dsp.h264_v_loop_filter_luma(pix, stride, alpha, beta, tc);
    } else {
        /* 16px edge length, see filter_mb_edgev */
            for( d = 0; d < 16; d++ ) {
                const int p0 = pix[-1*pix_next];
                const int p1 = pix[-2*pix_next];
                const int p2 = pix[-3*pix_next];
                const int q0 = pix[0];
                const int q1 = pix[1*pix_next];
                const int q2 = pix[2*pix_next];

                if( FFABS( p0 - q0 ) < alpha &&
                    FFABS( p1 - p0 ) < beta &&
                    FFABS( q1 - q0 ) < beta ) {

                    const int p3 = pix[-4*pix_next];
                    const int q3 = pix[ 3*pix_next];

                    if(FFABS( p0 - q0 ) < (( alpha >> 2 ) + 2 )){
                        if( FFABS( p2 - p0 ) < beta) {
                            /* p0', p1', p2' */
                            pix[-1*pix_next] = ( p2 + 2*p1 + 2*p0 + 2*q0 + q1 + 4 ) >> 3;
                            pix[-2*pix_next] = ( p2 + p1 + p0 + q0 + 2 ) >> 2;
                            pix[-3*pix_next] = ( 2*p3 + 3*p2 + p1 + p0 + q0 + 4 ) >> 3;
                        } else {
                            /* p0' */
                            pix[-1*pix_next] = ( 2*p1 + p0 + q1 + 2 ) >> 2;
                        }
                        if( FFABS( q2 - q0 ) < beta) {
                            /* q0', q1', q2' */
                            pix[0*pix_next] = ( p1 + 2*p0 + 2*q0 + 2*q1 + q2 + 4 ) >> 3;
                            pix[1*pix_next] = ( p0 + q0 + q1 + q2 + 2 ) >> 2;
                            pix[2*pix_next] = ( 2*q3 + 3*q2 + q1 + q0 + p0 + 4 ) >> 3;
                        } else {
                            /* q0' */
                            pix[0*pix_next] = ( 2*q1 + q0 + p1 + 2 ) >> 2;
                        }
                    }else{
                        /* p0', q0' */
                        pix[-1*pix_next] = ( 2*p1 + p0 + q1 + 2 ) >> 2;
                        pix[ 0*pix_next] = ( 2*q1 + q0 + p1 + 2 ) >> 2;
                    }
                    tprintf(h->s.avctx, "filter_mb_edgeh i:%d d:%d, qp:%d, indexA:%d, alpha:%d, beta:%d\n# bS:%d -> [%02x, %02x, %02x, %02x, %02x, %02x] =>[%02x, %02x, %02x, %02x]\n", i, d, qp, index_a, alpha, beta, bS[i], p2, p1, p0, q0, q1, q2, pix[-2*pix_next], pix[-pix_next], pix[0], pix[pix_next]);
                }
                pix++;
            }
    }
}

static void filter_mb_edgech( H264Context *h, uint8_t *pix, int stride, int16_t bS[4], int qp ) {
    int i;
    const int index_a = qp + h->slice_alpha_c0_offset;
    const int alpha = (alpha_table+52)[index_a];
    const int beta  = (beta_table+52)[qp + h->slice_beta_offset];

    if( bS[0] < 4 ) {
        int8_t tc[4];
        for(i=0; i<4; i++)
            tc[i] = bS[i] ? (tc0_table+52)[index_a][bS[i] - 1] + 1 : 0;
        h->s.dsp.h264_v_loop_filter_chroma(pix, stride, alpha, beta, tc);
    } else {
        h->s.dsp.h264_v_loop_filter_chroma_intra(pix, stride, alpha, beta);
    }
}

static void filter_mb_fast( H264Context *h, int mb_x, int mb_y, uint8_t *img_y, uint8_t *img_cb, uint8_t *img_cr, unsigned int linesize, unsigned int uvlinesize) {
    MpegEncContext * const s = &h->s;
    int mb_xy, mb_type;
    int qp, qp0, qp1, qpc, qpc0, qpc1, qp_thresh;

    if(mb_x==0 || mb_y==0 || !s->dsp.h264_loop_filter_strength) {
        filter_mb(h, mb_x, mb_y, img_y, img_cb, img_cr, linesize, uvlinesize);
        return;
    }
    assert(!FRAME_MBAFF);

    mb_xy = mb_x + mb_y*s->mb_stride;
    mb_type = s->current_picture.mb_type[mb_xy];
    qp = s->current_picture.qscale_table[mb_xy];
    qp0 = s->current_picture.qscale_table[mb_xy-1];
    qp1 = s->current_picture.qscale_table[h->top_mb_xy];
    qpc = get_chroma_qp( h->pps.chroma_qp_index_offset, qp );
    qpc0 = get_chroma_qp( h->pps.chroma_qp_index_offset, qp0 );
    qpc1 = get_chroma_qp( h->pps.chroma_qp_index_offset, qp1 );
    qp0 = (qp + qp0 + 1) >> 1;
    qp1 = (qp + qp1 + 1) >> 1;
    qpc0 = (qpc + qpc0 + 1) >> 1;
    qpc1 = (qpc + qpc1 + 1) >> 1;
    qp_thresh = 15 - h->slice_alpha_c0_offset;
    if(qp <= qp_thresh && qp0 <= qp_thresh && qp1 <= qp_thresh &&
       qpc <= qp_thresh && qpc0 <= qp_thresh && qpc1 <= qp_thresh)
        return;

    if( IS_INTRA(mb_type) ) {
        int16_t bS4[4] = {4,4,4,4};
        int16_t bS3[4] = {3,3,3,3};
        if( IS_8x8DCT(mb_type) ) {
            filter_mb_edgev( h, &img_y[4*0], linesize, bS4, qp0 );
            filter_mb_edgev( h, &img_y[4*2], linesize, bS3, qp );
            filter_mb_edgeh( h, &img_y[4*0*linesize], linesize, bS4, qp1 );
            filter_mb_edgeh( h, &img_y[4*2*linesize], linesize, bS3, qp );
        } else {
            filter_mb_edgev( h, &img_y[4*0], linesize, bS4, qp0 );
            filter_mb_edgev( h, &img_y[4*1], linesize, bS3, qp );
            filter_mb_edgev( h, &img_y[4*2], linesize, bS3, qp );
            filter_mb_edgev( h, &img_y[4*3], linesize, bS3, qp );
            filter_mb_edgeh( h, &img_y[4*0*linesize], linesize, bS4, qp1 );
            filter_mb_edgeh( h, &img_y[4*1*linesize], linesize, bS3, qp );
            filter_mb_edgeh( h, &img_y[4*2*linesize], linesize, bS3, qp );
            filter_mb_edgeh( h, &img_y[4*3*linesize], linesize, bS3, qp );
        }
        filter_mb_edgecv( h, &img_cb[2*0], uvlinesize, bS4, qpc0 );
        filter_mb_edgecv( h, &img_cb[2*2], uvlinesize, bS3, qpc );
        filter_mb_edgecv( h, &img_cr[2*0], uvlinesize, bS4, qpc0 );
        filter_mb_edgecv( h, &img_cr[2*2], uvlinesize, bS3, qpc );
        filter_mb_edgech( h, &img_cb[2*0*uvlinesize], uvlinesize, bS4, qpc1 );
        filter_mb_edgech( h, &img_cb[2*2*uvlinesize], uvlinesize, bS3, qpc );
        filter_mb_edgech( h, &img_cr[2*0*uvlinesize], uvlinesize, bS4, qpc1 );
        filter_mb_edgech( h, &img_cr[2*2*uvlinesize], uvlinesize, bS3, qpc );
        return;
    } else {
        DECLARE_ALIGNED_8(int16_t, bS[2][4][4]);
        uint64_t (*bSv)[4] = (uint64_t(*)[4])bS;
        int edges;
        if( IS_8x8DCT(mb_type) && (h->cbp&7) == 7 ) {
            edges = 4;
            bSv[0][0] = bSv[0][2] = bSv[1][0] = bSv[1][2] = 0x0002000200020002ULL;
        } else {
            int mask_edge1 = (mb_type & (MB_TYPE_16x16 | MB_TYPE_8x16)) ? 3 :
                             (mb_type & MB_TYPE_16x8) ? 1 : 0;
            int mask_edge0 = (mb_type & (MB_TYPE_16x16 | MB_TYPE_8x16))
                             && (s->current_picture.mb_type[mb_xy-1] & (MB_TYPE_16x16 | MB_TYPE_8x16))
                             ? 3 : 0;
            int step = IS_8x8DCT(mb_type) ? 2 : 1;
            edges = (mb_type & MB_TYPE_16x16) && !(h->cbp & 15) ? 1 : 4;
            s->dsp.h264_loop_filter_strength( bS, h->non_zero_count_cache, h->ref_cache, h->mv_cache,
                                              (h->slice_type == B_TYPE), edges, step, mask_edge0, mask_edge1 );
        }
        if( IS_INTRA(s->current_picture.mb_type[mb_xy-1]) )
            bSv[0][0] = 0x0004000400040004ULL;
        if( IS_INTRA(s->current_picture.mb_type[h->top_mb_xy]) )
            bSv[1][0] = 0x0004000400040004ULL;

#define FILTER(hv,dir,edge)\
        if(bSv[dir][edge]) {\
            filter_mb_edge##hv( h, &img_y[4*edge*(dir?linesize:1)], linesize, bS[dir][edge], edge ? qp : qp##dir );\
            if(!(edge&1)) {\
                filter_mb_edgec##hv( h, &img_cb[2*edge*(dir?uvlinesize:1)], uvlinesize, bS[dir][edge], edge ? qpc : qpc##dir );\
                filter_mb_edgec##hv( h, &img_cr[2*edge*(dir?uvlinesize:1)], uvlinesize, bS[dir][edge], edge ? qpc : qpc##dir );\
            }\
        }
        if( edges == 1 ) {
            FILTER(v,0,0);
            FILTER(h,1,0);
        } else if( IS_8x8DCT(mb_type) ) {
            FILTER(v,0,0);
            FILTER(v,0,2);
            FILTER(h,1,0);
            FILTER(h,1,2);
        } else {
            FILTER(v,0,0);
            FILTER(v,0,1);
            FILTER(v,0,2);
            FILTER(v,0,3);
            FILTER(h,1,0);
            FILTER(h,1,1);
            FILTER(h,1,2);
            FILTER(h,1,3);
        }
#undef FILTER
    }
}

static void filter_mb( H264Context *h, int mb_x, int mb_y, uint8_t *img_y, uint8_t *img_cb, uint8_t *img_cr, unsigned int linesize, unsigned int uvlinesize) {
    MpegEncContext * const s = &h->s;
    const int mb_xy= mb_x + mb_y*s->mb_stride;
    const int mb_type = s->current_picture.mb_type[mb_xy];
    const int mvy_limit = IS_INTERLACED(mb_type) ? 2 : 4;
    int first_vertical_edge_done = 0;
    int dir;
    /* FIXME: A given frame may occupy more than one position in
     * the reference list. So ref2frm should be populated with
     * frame numbers, not indices. */
    static const int ref2frm[34] = {-1,-1,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,
                                    16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31};

    //for sufficiently low qp, filtering wouldn't do anything
    //this is a conservative estimate: could also check beta_offset and more accurate chroma_qp
    if(!FRAME_MBAFF){
        int qp_thresh = 15 - h->slice_alpha_c0_offset - FFMAX(0, h->pps.chroma_qp_index_offset);
        int qp = s->current_picture.qscale_table[mb_xy];
        if(qp <= qp_thresh
           && (mb_x == 0 || ((qp + s->current_picture.qscale_table[mb_xy-1] + 1)>>1) <= qp_thresh)
           && (mb_y == 0 || ((qp + s->current_picture.qscale_table[h->top_mb_xy] + 1)>>1) <= qp_thresh)){
            return;
        }
    }

    if (FRAME_MBAFF
            // left mb is in picture
            && h->slice_table[mb_xy-1] != 255
            // and current and left pair do not have the same interlaced type
            && (IS_INTERLACED(mb_type) != IS_INTERLACED(s->current_picture.mb_type[mb_xy-1]))
            // and left mb is in the same slice if deblocking_filter == 2
            && (h->deblocking_filter!=2 || h->slice_table[mb_xy-1] == h->slice_table[mb_xy])) {
        /* First vertical edge is different in MBAFF frames
         * There are 8 different bS to compute and 2 different Qp
         */
        const int pair_xy = mb_x + (mb_y&~1)*s->mb_stride;
        const int left_mb_xy[2] = { pair_xy-1, pair_xy-1+s->mb_stride };
        int16_t bS[8];
        int qp[2];
        int chroma_qp[2];
        int mb_qp, mbn0_qp, mbn1_qp;
        int i;
        first_vertical_edge_done = 1;

        if( IS_INTRA(mb_type) )
            bS[0] = bS[1] = bS[2] = bS[3] = bS[4] = bS[5] = bS[6] = bS[7] = 4;
        else {
            for( i = 0; i < 8; i++ ) {
                int mbn_xy = MB_FIELD ? left_mb_xy[i>>2] : left_mb_xy[i&1];

                if( IS_INTRA( s->current_picture.mb_type[mbn_xy] ) )
                    bS[i] = 4;
                else if( h->non_zero_count_cache[12+8*(i>>1)] != 0 ||
                         /* FIXME: with 8x8dct + cavlc, should check cbp instead of nnz */
                         h->non_zero_count[mbn_xy][MB_FIELD ? i&3 : (i>>2)+(mb_y&1)*2] )
                    bS[i] = 2;
                else
                    bS[i] = 1;
            }
        }

        mb_qp = s->current_picture.qscale_table[mb_xy];
        mbn0_qp = s->current_picture.qscale_table[left_mb_xy[0]];
        mbn1_qp = s->current_picture.qscale_table[left_mb_xy[1]];
        qp[0] = ( mb_qp + mbn0_qp + 1 ) >> 1;
        chroma_qp[0] = ( get_chroma_qp( h->pps.chroma_qp_index_offset, mb_qp ) +
                         get_chroma_qp( h->pps.chroma_qp_index_offset, mbn0_qp ) + 1 ) >> 1;
        qp[1] = ( mb_qp + mbn1_qp + 1 ) >> 1;
        chroma_qp[1] = ( get_chroma_qp( h->pps.chroma_qp_index_offset, mb_qp ) +
                         get_chroma_qp( h->pps.chroma_qp_index_offset, mbn1_qp ) + 1 ) >> 1;

        /* Filter edge */
        tprintf(s->avctx, "filter mb:%d/%d MBAFF, QPy:%d/%d, QPc:%d/%d ls:%d uvls:%d", mb_x, mb_y, qp[0], qp[1], chroma_qp[0], chroma_qp[1], linesize, uvlinesize);
        { int i; for (i = 0; i < 8; i++) tprintf(s->avctx, " bS[%d]:%d", i, bS[i]); tprintf(s->avctx, "\n"); }
        filter_mb_mbaff_edgev ( h, &img_y [0], linesize,   bS, qp );
        filter_mb_mbaff_edgecv( h, &img_cb[0], uvlinesize, bS, chroma_qp );
        filter_mb_mbaff_edgecv( h, &img_cr[0], uvlinesize, bS, chroma_qp );
    }
    /* dir : 0 -> vertical edge, 1 -> horizontal edge */
    for( dir = 0; dir < 2; dir++ )
    {
        int edge;
        const int mbm_xy = dir == 0 ? mb_xy -1 : h->top_mb_xy;
        const int mbm_type = s->current_picture.mb_type[mbm_xy];
        int start = h->slice_table[mbm_xy] == 255 ? 1 : 0;

        const int edges = (mb_type & (MB_TYPE_16x16|MB_TYPE_SKIP))
                                  == (MB_TYPE_16x16|MB_TYPE_SKIP) ? 1 : 4;
        // how often to recheck mv-based bS when iterating between edges
        const int mask_edge = (mb_type & (MB_TYPE_16x16 | (MB_TYPE_16x8 << dir))) ? 3 :
                              (mb_type & (MB_TYPE_8x16 >> dir)) ? 1 : 0;
        // how often to recheck mv-based bS when iterating along each edge
        const int mask_par0 = mb_type & (MB_TYPE_16x16 | (MB_TYPE_8x16 >> dir));

        if (first_vertical_edge_done) {
            start = 1;
            first_vertical_edge_done = 0;
        }

        if (h->deblocking_filter==2 && h->slice_table[mbm_xy] != h->slice_table[mb_xy])
            start = 1;

        if (FRAME_MBAFF && (dir == 1) && ((mb_y&1) == 0) && start == 0
            && !IS_INTERLACED(mb_type)
            && IS_INTERLACED(mbm_type)
            ) {
            // This is a special case in the norm where the filtering must
            // be done twice (one each of the field) even if we are in a
            // frame macroblock.
            //
            static const int nnz_idx[4] = {4,5,6,3};
            unsigned int tmp_linesize   = 2 *   linesize;
            unsigned int tmp_uvlinesize = 2 * uvlinesize;
            int mbn_xy = mb_xy - 2 * s->mb_stride;
            int qp, chroma_qp;
            int i, j;
            int16_t bS[4];

            for(j=0; j<2; j++, mbn_xy += s->mb_stride){
                if( IS_INTRA(mb_type) ||
                    IS_INTRA(s->current_picture.mb_type[mbn_xy]) ) {
                    bS[0] = bS[1] = bS[2] = bS[3] = 3;
                } else {
                    const uint8_t *mbn_nnz = h->non_zero_count[mbn_xy];
                    for( i = 0; i < 4; i++ ) {
                        if( h->non_zero_count_cache[scan8[0]+i] != 0 ||
                            mbn_nnz[nnz_idx[i]] != 0 )
                            bS[i] = 2;
                        else
                            bS[i] = 1;
                    }
                }
                // Do not use s->qscale as luma quantizer because it has not the same
                // value in IPCM macroblocks.
                qp = ( s->current_picture.qscale_table[mb_xy] + s->current_picture.qscale_table[mbn_xy] + 1 ) >> 1;
                tprintf(s->avctx, "filter mb:%d/%d dir:%d edge:%d, QPy:%d ls:%d uvls:%d", mb_x, mb_y, dir, edge, qp, tmp_linesize, tmp_uvlinesize);
                { int i; for (i = 0; i < 4; i++) tprintf(s->avctx, " bS[%d]:%d", i, bS[i]); tprintf(s->avctx, "\n"); }
                filter_mb_edgeh( h, &img_y[j*linesize], tmp_linesize, bS, qp );
                chroma_qp = ( h->chroma_qp +
                              get_chroma_qp( h->pps.chroma_qp_index_offset, s->current_picture.qscale_table[mbn_xy] ) + 1 ) >> 1;
                filter_mb_edgech( h, &img_cb[j*uvlinesize], tmp_uvlinesize, bS, chroma_qp );
                filter_mb_edgech( h, &img_cr[j*uvlinesize], tmp_uvlinesize, bS, chroma_qp );
            }

            start = 1;
        }

        /* Calculate bS */
        for( edge = start; edge < edges; edge++ ) {
            /* mbn_xy: neighbor macroblock */
            const int mbn_xy = edge > 0 ? mb_xy : mbm_xy;
            const int mbn_type = s->current_picture.mb_type[mbn_xy];
            int16_t bS[4];
            int qp;

            if( (edge&1) && IS_8x8DCT(mb_type) )
                continue;

            if( IS_INTRA(mb_type) ||
                IS_INTRA(mbn_type) ) {
                int value;
                if (edge == 0) {
                    if (   (!IS_INTERLACED(mb_type) && !IS_INTERLACED(mbm_type))
                        || ((FRAME_MBAFF || (s->picture_structure != PICT_FRAME)) && (dir == 0))
                    ) {
                        value = 4;
                    } else {
                        value = 3;
                    }
                } else {
                    value = 3;
                }
                bS[0] = bS[1] = bS[2] = bS[3] = value;
            } else {
                int i, l;
                int mv_done;

                if( edge & mask_edge ) {
                    bS[0] = bS[1] = bS[2] = bS[3] = 0;
                    mv_done = 1;
                }
                else if( FRAME_MBAFF && IS_INTERLACED(mb_type ^ mbn_type)) {
                    bS[0] = bS[1] = bS[2] = bS[3] = 1;
                    mv_done = 1;
                }
                else if( mask_par0 && (edge || (mbn_type & (MB_TYPE_16x16 | (MB_TYPE_8x16 >> dir)))) ) {
                    int b_idx= 8 + 4 + edge * (dir ? 8:1);
                    int bn_idx= b_idx - (dir ? 8:1);
                    int v = 0;
                    for( l = 0; !v && l < 1 + (h->slice_type == B_TYPE); l++ ) {
                        v |= ref2frm[h->ref_cache[l][b_idx]+2] != ref2frm[h->ref_cache[l][bn_idx]+2] ||
                             FFABS( h->mv_cache[l][b_idx][0] - h->mv_cache[l][bn_idx][0] ) >= 4 ||
                             FFABS( h->mv_cache[l][b_idx][1] - h->mv_cache[l][bn_idx][1] ) >= mvy_limit;
                    }
                    bS[0] = bS[1] = bS[2] = bS[3] = v;
                    mv_done = 1;
                }
                else
                    mv_done = 0;

                for( i = 0; i < 4; i++ ) {
                    int x = dir == 0 ? edge : i;
                    int y = dir == 0 ? i    : edge;
                    int b_idx= 8 + 4 + x + 8*y;
                    int bn_idx= b_idx - (dir ? 8:1);

                    if( h->non_zero_count_cache[b_idx] != 0 ||
                        h->non_zero_count_cache[bn_idx] != 0 ) {
                        bS[i] = 2;
                    }
                    else if(!mv_done)
                    {
                        bS[i] = 0;
                        for( l = 0; l < 1 + (h->slice_type == B_TYPE); l++ ) {
                            if( ref2frm[h->ref_cache[l][b_idx]+2] != ref2frm[h->ref_cache[l][bn_idx]+2] ||
                                FFABS( h->mv_cache[l][b_idx][0] - h->mv_cache[l][bn_idx][0] ) >= 4 ||
                                FFABS( h->mv_cache[l][b_idx][1] - h->mv_cache[l][bn_idx][1] ) >= mvy_limit ) {
                                bS[i] = 1;
                                break;
                            }
                        }
                    }
                }

                if(bS[0]+bS[1]+bS[2]+bS[3] == 0)
                    continue;
            }

            /* Filter edge */
            // Do not use s->qscale as luma quantizer because it has not the same
            // value in IPCM macroblocks.
            qp = ( s->current_picture.qscale_table[mb_xy] + s->current_picture.qscale_table[mbn_xy] + 1 ) >> 1;
            //tprintf(s->avctx, "filter mb:%d/%d dir:%d edge:%d, QPy:%d, QPc:%d, QPcn:%d\n", mb_x, mb_y, dir, edge, qp, h->chroma_qp, s->current_picture.qscale_table[mbn_xy]);
            tprintf(s->avctx, "filter mb:%d/%d dir:%d edge:%d, QPy:%d ls:%d uvls:%d", mb_x, mb_y, dir, edge, qp, linesize, uvlinesize);
            { int i; for (i = 0; i < 4; i++) tprintf(s->avctx, " bS[%d]:%d", i, bS[i]); tprintf(s->avctx, "\n"); }
            if( dir == 0 ) {
                filter_mb_edgev( h, &img_y[4*edge], linesize, bS, qp );
                if( (edge&1) == 0 ) {
                    int chroma_qp = ( h->chroma_qp +
                                      get_chroma_qp( h->pps.chroma_qp_index_offset, s->current_picture.qscale_table[mbn_xy] ) + 1 ) >> 1;
                    filter_mb_edgecv( h, &img_cb[2*edge], uvlinesize, bS, chroma_qp );
                    filter_mb_edgecv( h, &img_cr[2*edge], uvlinesize, bS, chroma_qp );
                }
            } else {
                filter_mb_edgeh( h, &img_y[4*edge*linesize], linesize, bS, qp );
                if( (edge&1) == 0 ) {
                    int chroma_qp = ( h->chroma_qp +
                                      get_chroma_qp( h->pps.chroma_qp_index_offset, s->current_picture.qscale_table[mbn_xy] ) + 1 ) >> 1;
                    filter_mb_edgech( h, &img_cb[2*edge*uvlinesize], uvlinesize, bS, chroma_qp );
                    filter_mb_edgech( h, &img_cr[2*edge*uvlinesize], uvlinesize, bS, chroma_qp );
                }
            }
        }
    }
}

static int decode_slice(H264Context *h){
    MpegEncContext * const s = &h->s;
    const int part_mask= s->partitioned_frame ? (AC_END|AC_ERROR) : 0x7F;

    s->mb_skip_run= -1;

    if( h->pps.cabac ) {
        int i;

        /* realign */
        align_get_bits( &s->gb );

        /* init cabac */
        ff_init_cabac_states( &h->cabac);
        ff_init_cabac_decoder( &h->cabac,
                               s->gb.buffer + get_bits_count(&s->gb)/8,
                               ( s->gb.size_in_bits - get_bits_count(&s->gb) + 7)/8);
        /* calculate pre-state */
        for( i= 0; i < 460; i++ ) {
            int pre;
            if( h->slice_type == I_TYPE )
                pre = av_clip( ((cabac_context_init_I[i][0] * s->qscale) >>4 ) + cabac_context_init_I[i][1], 1, 126 );
            else
                pre = av_clip( ((cabac_context_init_PB[h->cabac_init_idc][i][0] * s->qscale) >>4 ) + cabac_context_init_PB[h->cabac_init_idc][i][1], 1, 126 );

            if( pre <= 63 )
                h->cabac_state[i] = 2 * ( 63 - pre ) + 0;
            else
                h->cabac_state[i] = 2 * ( pre - 64 ) + 1;
        }

        for(;;){
//START_TIMER
            int ret = decode_mb_cabac(h);
            int eos;
//STOP_TIMER("decode_mb_cabac")

            if(ret>=0) hl_decode_mb(h);

            if( ret >= 0 && FRAME_MBAFF ) { //FIXME optimal? or let mb_decode decode 16x32 ?
                s->mb_y++;

                if(ret>=0) ret = decode_mb_cabac(h);

                if(ret>=0) hl_decode_mb(h);
                s->mb_y--;
            }
            eos = get_cabac_terminate( &h->cabac );

            if( ret < 0 || h->cabac.bytestream > h->cabac.bytestream_end + 2) {
                av_log(h->s.avctx, AV_LOG_ERROR, "error while decoding MB %d %d, bytestream (%d)\n", s->mb_x, s->mb_y, h->cabac.bytestream_end - h->cabac.bytestream);
                ff_er_add_slice(s, s->resync_mb_x, s->resync_mb_y, s->mb_x, s->mb_y, (AC_ERROR|DC_ERROR|MV_ERROR)&part_mask);
                return -1;
            }

            if( ++s->mb_x >= s->mb_width ) {
                s->mb_x = 0;
                ff_draw_horiz_band(s, 16*s->mb_y, 16);
                ++s->mb_y;
                if(FRAME_MBAFF) {
                    ++s->mb_y;
                }
            }

            if( eos || s->mb_y >= s->mb_height ) {
                tprintf(s->avctx, "slice end %d %d\n", get_bits_count(&s->gb), s->gb.size_in_bits);
                ff_er_add_slice(s, s->resync_mb_x, s->resync_mb_y, s->mb_x-1, s->mb_y, (AC_END|DC_END|MV_END)&part_mask);
                return 0;
            }
        }

    } else {
        for(;;){
            int ret = decode_mb_cavlc(h);

            if(ret>=0) hl_decode_mb(h);

            if(ret>=0 && FRAME_MBAFF){ //FIXME optimal? or let mb_decode decode 16x32 ?
                s->mb_y++;
                ret = decode_mb_cavlc(h);

                if(ret>=0) hl_decode_mb(h);
                s->mb_y--;
            }

            if(ret<0){
                av_log(h->s.avctx, AV_LOG_ERROR, "error while decoding MB %d %d\n", s->mb_x, s->mb_y);
                ff_er_add_slice(s, s->resync_mb_x, s->resync_mb_y, s->mb_x, s->mb_y, (AC_ERROR|DC_ERROR|MV_ERROR)&part_mask);

                return -1;
            }

            if(++s->mb_x >= s->mb_width){
                s->mb_x=0;
                ff_draw_horiz_band(s, 16*s->mb_y, 16);
                ++s->mb_y;
                if(FRAME_MBAFF) {
                    ++s->mb_y;
                }
                if(s->mb_y >= s->mb_height){
                    tprintf(s->avctx, "slice end %d %d\n", get_bits_count(&s->gb), s->gb.size_in_bits);

                    if(get_bits_count(&s->gb) == s->gb.size_in_bits ) {
                        ff_er_add_slice(s, s->resync_mb_x, s->resync_mb_y, s->mb_x-1, s->mb_y, (AC_END|DC_END|MV_END)&part_mask);

                        return 0;
                    }else{
                        ff_er_add_slice(s, s->resync_mb_x, s->resync_mb_y, s->mb_x, s->mb_y, (AC_END|DC_END|MV_END)&part_mask);

                        return -1;
                    }
                }
            }

            if(get_bits_count(&s->gb) >= s->gb.size_in_bits && s->mb_skip_run<=0){
                tprintf(s->avctx, "slice end %d %d\n", get_bits_count(&s->gb), s->gb.size_in_bits);
                if(get_bits_count(&s->gb) == s->gb.size_in_bits ){
                    ff_er_add_slice(s, s->resync_mb_x, s->resync_mb_y, s->mb_x-1, s->mb_y, (AC_END|DC_END|MV_END)&part_mask);

                    return 0;
                }else{
                    ff_er_add_slice(s, s->resync_mb_x, s->resync_mb_y, s->mb_x, s->mb_y, (AC_ERROR|DC_ERROR|MV_ERROR)&part_mask);

                    return -1;
                }
            }
        }
    }

#if 0
    for(;s->mb_y < s->mb_height; s->mb_y++){
        for(;s->mb_x < s->mb_width; s->mb_x++){
            int ret= decode_mb(h);

            hl_decode_mb(h);

            if(ret<0){
                av_log(s->avctx, AV_LOG_ERROR, "error while decoding MB %d %d\n", s->mb_x, s->mb_y);
                ff_er_add_slice(s, s->resync_mb_x, s->resync_mb_y, s->mb_x, s->mb_y, (AC_ERROR|DC_ERROR|MV_ERROR)&part_mask);

                return -1;
            }

            if(++s->mb_x >= s->mb_width){
                s->mb_x=0;
                if(++s->mb_y >= s->mb_height){
                    if(get_bits_count(s->gb) == s->gb.size_in_bits){
                        ff_er_add_slice(s, s->resync_mb_x, s->resync_mb_y, s->mb_x-1, s->mb_y, (AC_END|DC_END|MV_END)&part_mask);

                        return 0;
                    }else{
                        ff_er_add_slice(s, s->resync_mb_x, s->resync_mb_y, s->mb_x, s->mb_y, (AC_END|DC_END|MV_END)&part_mask);

                        return -1;
                    }
                }
            }

            if(get_bits_count(s->?gb) >= s->gb?.size_in_bits){
                if(get_bits_count(s->gb) == s->gb.size_in_bits){
                    ff_er_add_slice(s, s->resync_mb_x, s->resync_mb_y, s->mb_x-1, s->mb_y, (AC_END|DC_END|MV_END)&part_mask);

                    return 0;
                }else{
                    ff_er_add_slice(s, s->resync_mb_x, s->resync_mb_y, s->mb_x, s->mb_y, (AC_ERROR|DC_ERROR|MV_ERROR)&part_mask);

                    return -1;
                }
            }
        }
        s->mb_x=0;
        ff_draw_horiz_band(s, 16*s->mb_y, 16);
    }
#endif
    return -1; //not reached
}

static int decode_unregistered_user_data(H264Context *h, int size){
    MpegEncContext * const s = &h->s;
    uint8_t user_data[16+256];
    int e, build, i;

    if(size<16)
        return -1;

    for(i=0; i<sizeof(user_data)-1 && i<size; i++){
        user_data[i]= get_bits(&s->gb, 8);
    }

    user_data[i]= 0;
    e= sscanf(user_data+16, "x264 - core %d"/*%s - H.264/MPEG-4 AVC codec - Copyleft 2005 - http://www.videolan.org/x264.html*/, &build);
    if(e==1 && build>=0)
        h->x264_build= build;

    if(s->avctx->debug & FF_DEBUG_BUGS)
        av_log(s->avctx, AV_LOG_DEBUG, "user data:\"%s\"\n", user_data+16);

    for(; i<size; i++)
        skip_bits(&s->gb, 8);

    return 0;
}

static int decode_sei(H264Context *h){
    MpegEncContext * const s = &h->s;

    while(get_bits_count(&s->gb) + 16 < s->gb.size_in_bits){
        int size, type;

        type=0;
        do{
            type+= show_bits(&s->gb, 8);
        }while(get_bits(&s->gb, 8) == 255);

        size=0;
        do{
            size+= show_bits(&s->gb, 8);
        }while(get_bits(&s->gb, 8) == 255);

        switch(type){
        case 5:
            if(decode_unregistered_user_data(h, size) < 0)
                return -1;
            break;
        default:
            skip_bits(&s->gb, 8*size);
        }

        //FIXME check bits here
        align_get_bits(&s->gb);
    }

    return 0;
}

static inline void decode_hrd_parameters(H264Context *h, SPS *sps){
    MpegEncContext * const s = &h->s;
    int cpb_count, i;
    cpb_count = get_ue_golomb(&s->gb) + 1;
    get_bits(&s->gb, 4); /* bit_rate_scale */
    get_bits(&s->gb, 4); /* cpb_size_scale */
    for(i=0; i<cpb_count; i++){
        get_ue_golomb(&s->gb); /* bit_rate_value_minus1 */
        get_ue_golomb(&s->gb); /* cpb_size_value_minus1 */
        get_bits1(&s->gb);     /* cbr_flag */
    }
    get_bits(&s->gb, 5); /* initial_cpb_removal_delay_length_minus1 */
    get_bits(&s->gb, 5); /* cpb_removal_delay_length_minus1 */
    get_bits(&s->gb, 5); /* dpb_output_delay_length_minus1 */
    get_bits(&s->gb, 5); /* time_offset_length */
}

static inline int decode_vui_parameters(H264Context *h, SPS *sps){
    MpegEncContext * const s = &h->s;
    int aspect_ratio_info_present_flag;
    unsigned int aspect_ratio_idc;
    int nal_hrd_parameters_present_flag, vcl_hrd_parameters_present_flag;

    aspect_ratio_info_present_flag= get_bits1(&s->gb);

    if( aspect_ratio_info_present_flag ) {
        aspect_ratio_idc= get_bits(&s->gb, 8);
        if( aspect_ratio_idc == EXTENDED_SAR ) {
            sps->sar.num= get_bits(&s->gb, 16);
            sps->sar.den= get_bits(&s->gb, 16);
        }else if(aspect_ratio_idc < 14){
            sps->sar=  pixel_aspect[aspect_ratio_idc];
        }else{
            av_log(h->s.avctx, AV_LOG_ERROR, "illegal aspect ratio\n");
            return -1;
        }
    }else{
        sps->sar.num=
        sps->sar.den= 0;
    }
//            s->avctx->aspect_ratio= sar_width*s->width / (float)(s->height*sar_height);

    if(get_bits1(&s->gb)){      /* overscan_info_present_flag */
        get_bits1(&s->gb);      /* overscan_appropriate_flag */
    }

    if(get_bits1(&s->gb)){      /* video_signal_type_present_flag */
        get_bits(&s->gb, 3);    /* video_format */
        get_bits1(&s->gb);      /* video_full_range_flag */
        if(get_bits1(&s->gb)){  /* colour_description_present_flag */
            get_bits(&s->gb, 8); /* colour_primaries */
            get_bits(&s->gb, 8); /* transfer_characteristics */
            get_bits(&s->gb, 8); /* matrix_coefficients */
        }
    }

    if(get_bits1(&s->gb)){      /* chroma_location_info_present_flag */
        get_ue_golomb(&s->gb);  /* chroma_sample_location_type_top_field */
        get_ue_golomb(&s->gb);  /* chroma_sample_location_type_bottom_field */
    }

    sps->timing_info_present_flag = get_bits1(&s->gb);
    if(sps->timing_info_present_flag){
        sps->num_units_in_tick = get_bits_long(&s->gb, 32);
        sps->time_scale = get_bits_long(&s->gb, 32);
        sps->fixed_frame_rate_flag = get_bits1(&s->gb);
    }

    nal_hrd_parameters_present_flag = get_bits1(&s->gb);
    if(nal_hrd_parameters_present_flag)
        decode_hrd_parameters(h, sps);
    vcl_hrd_parameters_present_flag = get_bits1(&s->gb);
    if(vcl_hrd_parameters_present_flag)
        decode_hrd_parameters(h, sps);
    if(nal_hrd_parameters_present_flag || vcl_hrd_parameters_present_flag)
        get_bits1(&s->gb);     /* low_delay_hrd_flag */
    get_bits1(&s->gb);         /* pic_struct_present_flag */

    sps->bitstream_restriction_flag = get_bits1(&s->gb);
    if(sps->bitstream_restriction_flag){
        unsigned int num_reorder_frames;
        get_bits1(&s->gb);     /* motion_vectors_over_pic_boundaries_flag */
        get_ue_golomb(&s->gb); /* max_bytes_per_pic_denom */
        get_ue_golomb(&s->gb); /* max_bits_per_mb_denom */
        get_ue_golomb(&s->gb); /* log2_max_mv_length_horizontal */
        get_ue_golomb(&s->gb); /* log2_max_mv_length_vertical */
        num_reorder_frames= get_ue_golomb(&s->gb);
        get_ue_golomb(&s->gb); /*max_dec_frame_buffering*/

        if(num_reorder_frames > 16 /*max_dec_frame_buffering || max_dec_frame_buffering > 16*/){
            av_log(h->s.avctx, AV_LOG_ERROR, "illegal num_reorder_frames %d\n", num_reorder_frames);
            return -1;
        }

        sps->num_reorder_frames= num_reorder_frames;
    }

    return 0;
}

static void decode_scaling_list(H264Context *h, uint8_t *factors, int size,
                                const uint8_t *jvt_list, const uint8_t *fallback_list){
    MpegEncContext * const s = &h->s;
    int i, last = 8, next = 8;
    const uint8_t *scan = size == 16 ? zigzag_scan : zigzag_scan8x8;
    if(!get_bits1(&s->gb)) /* matrix not written, we use the predicted one */
        memcpy(factors, fallback_list, size*sizeof(uint8_t));
    else
    for(i=0;i<size;i++){
        if(next)
            next = (last + get_se_golomb(&s->gb)) & 0xff;
        if(!i && !next){ /* matrix not written, we use the preset one */
            memcpy(factors, jvt_list, size*sizeof(uint8_t));
            break;
        }
        last = factors[scan[i]] = next ? next : last;
    }
}

static void decode_scaling_matrices(H264Context *h, SPS *sps, PPS *pps, int is_sps,
                                   uint8_t (*scaling_matrix4)[16], uint8_t (*scaling_matrix8)[64]){
    MpegEncContext * const s = &h->s;
    int fallback_sps = !is_sps && sps->scaling_matrix_present;
    const uint8_t *fallback[4] = {
        fallback_sps ? sps->scaling_matrix4[0] : default_scaling4[0],
        fallback_sps ? sps->scaling_matrix4[3] : default_scaling4[1],
        fallback_sps ? sps->scaling_matrix8[0] : default_scaling8[0],
        fallback_sps ? sps->scaling_matrix8[1] : default_scaling8[1]
    };
    if(get_bits1(&s->gb)){
        sps->scaling_matrix_present |= is_sps;
        decode_scaling_list(h,scaling_matrix4[0],16,default_scaling4[0],fallback[0]); // Intra, Y
        decode_scaling_list(h,scaling_matrix4[1],16,default_scaling4[0],scaling_matrix4[0]); // Intra, Cr
        decode_scaling_list(h,scaling_matrix4[2],16,default_scaling4[0],scaling_matrix4[1]); // Intra, Cb
        decode_scaling_list(h,scaling_matrix4[3],16,default_scaling4[1],fallback[1]); // Inter, Y
        decode_scaling_list(h,scaling_matrix4[4],16,default_scaling4[1],scaling_matrix4[3]); // Inter, Cr
        decode_scaling_list(h,scaling_matrix4[5],16,default_scaling4[1],scaling_matrix4[4]); // Inter, Cb
        if(is_sps || pps->transform_8x8_mode){
            decode_scaling_list(h,scaling_matrix8[0],64,default_scaling8[0],fallback[2]);  // Intra, Y
            decode_scaling_list(h,scaling_matrix8[1],64,default_scaling8[1],fallback[3]);  // Inter, Y
        }
    } else if(fallback_sps) {
        memcpy(scaling_matrix4, sps->scaling_matrix4, 6*16*sizeof(uint8_t));
        memcpy(scaling_matrix8, sps->scaling_matrix8, 2*64*sizeof(uint8_t));
    }
}

static inline int decode_seq_parameter_set(H264Context *h){
    MpegEncContext * const s = &h->s;
    int profile_idc, level_idc;
    unsigned int sps_id, tmp, mb_width, mb_height;
    int i;
    SPS *sps;

    profile_idc= get_bits(&s->gb, 8);
    get_bits1(&s->gb);   //constraint_set0_flag
    get_bits1(&s->gb);   //constraint_set1_flag
    get_bits1(&s->gb);   //constraint_set2_flag
    get_bits1(&s->gb);   //constraint_set3_flag
    get_bits(&s->gb, 4); // reserved
    level_idc= get_bits(&s->gb, 8);
    sps_id= get_ue_golomb(&s->gb);

    if (sps_id >= MAX_SPS_COUNT){
        // ok it has gone out of hand, someone is sending us bad stuff.
        av_log(h->s.avctx, AV_LOG_ERROR, "illegal sps_id (%d)\n", sps_id);
        return -1;
    }

    sps= &h->sps_buffer[ sps_id ];
    sps->profile_idc= profile_idc;
    sps->level_idc= level_idc;

    if(sps->profile_idc >= 100){ //high profile
        if(get_ue_golomb(&s->gb) == 3) //chroma_format_idc
            get_bits1(&s->gb);  //residual_color_transform_flag
        get_ue_golomb(&s->gb);  //bit_depth_luma_minus8
        get_ue_golomb(&s->gb);  //bit_depth_chroma_minus8
        sps->transform_bypass = get_bits1(&s->gb);
        decode_scaling_matrices(h, sps, NULL, 1, sps->scaling_matrix4, sps->scaling_matrix8);
    }else
        sps->scaling_matrix_present = 0;

    sps->log2_max_frame_num= get_ue_golomb(&s->gb) + 4;
    sps->poc_type= get_ue_golomb(&s->gb);

    if(sps->poc_type == 0){ //FIXME #define
        sps->log2_max_poc_lsb= get_ue_golomb(&s->gb) + 4;
    } else if(sps->poc_type == 1){//FIXME #define
        sps->delta_pic_order_always_zero_flag= get_bits1(&s->gb);
        sps->offset_for_non_ref_pic= get_se_golomb(&s->gb);
        sps->offset_for_top_to_bottom_field= get_se_golomb(&s->gb);
        tmp= get_ue_golomb(&s->gb);

        if(tmp >= sizeof(sps->offset_for_ref_frame) / sizeof(sps->offset_for_ref_frame[0])){
            av_log(h->s.avctx, AV_LOG_ERROR, "poc_cycle_length overflow %u\n", tmp);
            return -1;
        }
        sps->poc_cycle_length= tmp;

        for(i=0; i<sps->poc_cycle_length; i++)
            sps->offset_for_ref_frame[i]= get_se_golomb(&s->gb);
    }else if(sps->poc_type != 2){
        av_log(h->s.avctx, AV_LOG_ERROR, "illegal POC type %d\n", sps->poc_type);
        return -1;
    }

    tmp= get_ue_golomb(&s->gb);
    if(tmp > MAX_PICTURE_COUNT-2){
        av_log(h->s.avctx, AV_LOG_ERROR, "too many reference frames\n");
    }
    sps->ref_frame_count= tmp;
    sps->gaps_in_frame_num_allowed_flag= get_bits1(&s->gb);
    mb_width= get_ue_golomb(&s->gb) + 1;
    mb_height= get_ue_golomb(&s->gb) + 1;
    if(mb_width >= INT_MAX/16 || mb_height >= INT_MAX/16 ||
       avcodec_check_dimensions(NULL, 16*mb_width, 16*mb_height)){
        av_log(h->s.avctx, AV_LOG_ERROR, "mb_width/height overflow\n");
        return -1;
    }
    sps->mb_width = mb_width;
    sps->mb_height= mb_height;

    sps->frame_mbs_only_flag= get_bits1(&s->gb);
    if(!sps->frame_mbs_only_flag)
        sps->mb_aff= get_bits1(&s->gb);
    else
        sps->mb_aff= 0;

    sps->direct_8x8_inference_flag= get_bits1(&s->gb);

#ifndef ALLOW_INTERLACE
    if(sps->mb_aff)
        av_log(h->s.avctx, AV_LOG_ERROR, "MBAFF support not included; enable it at compile-time.\n");
#endif
    if(!sps->direct_8x8_inference_flag && sps->mb_aff)
        av_log(h->s.avctx, AV_LOG_ERROR, "MBAFF + !direct_8x8_inference is not implemented\n");

    sps->crop= get_bits1(&s->gb);
    if(sps->crop){
        sps->crop_left  = get_ue_golomb(&s->gb);
        sps->crop_right = get_ue_golomb(&s->gb);
        sps->crop_top   = get_ue_golomb(&s->gb);
        sps->crop_bottom= get_ue_golomb(&s->gb);
        if(sps->crop_left || sps->crop_top){
            av_log(h->s.avctx, AV_LOG_ERROR, "insane cropping not completely supported, this could look slightly wrong ...\n");
        }
    }else{
        sps->crop_left  =
        sps->crop_right =
        sps->crop_top   =
        sps->crop_bottom= 0;
    }

    sps->vui_parameters_present_flag= get_bits1(&s->gb);
    if( sps->vui_parameters_present_flag )
        decode_vui_parameters(h, sps);

    if(s->avctx->debug&FF_DEBUG_PICT_INFO){
        av_log(h->s.avctx, AV_LOG_DEBUG, "sps:%u profile:%d/%d poc:%d ref:%d %dx%d %s %s crop:%d/%d/%d/%d %s\n",
               sps_id, sps->profile_idc, sps->level_idc,
               sps->poc_type,
               sps->ref_frame_count,
               sps->mb_width, sps->mb_height,
               sps->frame_mbs_only_flag ? "FRM" : (sps->mb_aff ? "MB-AFF" : "PIC-AFF"),
               sps->direct_8x8_inference_flag ? "8B8" : "",
               sps->crop_left, sps->crop_right,
               sps->crop_top, sps->crop_bottom,
               sps->vui_parameters_present_flag ? "VUI" : ""
               );
    }
    return 0;
}

static inline int decode_picture_parameter_set(H264Context *h, int bit_length){
    MpegEncContext * const s = &h->s;
    unsigned int tmp, pps_id= get_ue_golomb(&s->gb);
    PPS *pps;

    if(pps_id>=MAX_PPS_COUNT){
        av_log(h->s.avctx, AV_LOG_ERROR, "pps_id out of range\n");
        return -1;
    }
    pps = &h->pps_buffer[pps_id];

    tmp= get_ue_golomb(&s->gb);
    if(tmp>=MAX_SPS_COUNT){
        av_log(h->s.avctx, AV_LOG_ERROR, "sps_id out of range\n");
        return -1;
    }
    pps->sps_id= tmp;

    pps->cabac= get_bits1(&s->gb);
    pps->pic_order_present= get_bits1(&s->gb);
    pps->slice_group_count= get_ue_golomb(&s->gb) + 1;
    if(pps->slice_group_count > 1 ){
        pps->mb_slice_group_map_type= get_ue_golomb(&s->gb);
        av_log(h->s.avctx, AV_LOG_ERROR, "FMO not supported\n");
        switch(pps->mb_slice_group_map_type){
        case 0:
#if 0
|   for( i = 0; i <= num_slice_groups_minus1; i++ ) |   |        |
|    run_length[ i ]                                |1  |ue(v)   |
#endif
            break;
        case 2:
#if 0
|   for( i = 0; i < num_slice_groups_minus1; i++ )  |   |        |
|{                                                  |   |        |
|    top_left_mb[ i ]                               |1  |ue(v)   |
|    bottom_right_mb[ i ]                           |1  |ue(v)   |
|   }                                               |   |        |
#endif
            break;
        case 3:
        case 4:
        case 5:
#if 0
|   slice_group_change_direction_flag               |1  |u(1)    |
|   slice_group_change_rate_minus1                  |1  |ue(v)   |
#endif
            break;
        case 6:
#if 0
|   slice_group_id_cnt_minus1                       |1  |ue(v)   |
|   for( i = 0; i <= slice_group_id_cnt_minus1; i++ |   |        |
|)                                                  |   |        |
|    slice_group_id[ i ]                            |1  |u(v)    |
#endif
            break;
        }
    }
    pps->ref_count[0]= get_ue_golomb(&s->gb) + 1;
    pps->ref_count[1]= get_ue_golomb(&s->gb) + 1;
    if(pps->ref_count[0]-1 > 32-1 || pps->ref_count[1]-1 > 32-1){
        av_log(h->s.avctx, AV_LOG_ERROR, "reference overflow (pps)\n");
        pps->ref_count[0]= pps->ref_count[1]= 1;
        return -1;
    }

    pps->weighted_pred= get_bits1(&s->gb);
    pps->weighted_bipred_idc= get_bits(&s->gb, 2);
    pps->init_qp= get_se_golomb(&s->gb) + 26;
    pps->init_qs= get_se_golomb(&s->gb) + 26;
    pps->chroma_qp_index_offset= get_se_golomb(&s->gb);
    pps->deblocking_filter_parameters_present= get_bits1(&s->gb);
    pps->constrained_intra_pred= get_bits1(&s->gb);
    pps->redundant_pic_cnt_present = get_bits1(&s->gb);

    pps->transform_8x8_mode= 0;
    h->dequant_coeff_pps= -1; //contents of sps/pps can change even if id doesn't, so reinit
    memset(pps->scaling_matrix4, 16, 6*16*sizeof(uint8_t));
    memset(pps->scaling_matrix8, 16, 2*64*sizeof(uint8_t));

    if(get_bits_count(&s->gb) < bit_length){
        pps->transform_8x8_mode= get_bits1(&s->gb);
        decode_scaling_matrices(h, &h->sps_buffer[pps->sps_id], pps, 0, pps->scaling_matrix4, pps->scaling_matrix8);
        get_se_golomb(&s->gb);  //second_chroma_qp_index_offset
    }

    if(s->avctx->debug&FF_DEBUG_PICT_INFO){
        av_log(h->s.avctx, AV_LOG_DEBUG, "pps:%u sps:%u %s slice_groups:%d ref:%d/%d %s qp:%d/%d/%d %s %s %s %s\n",
               pps_id, pps->sps_id,
               pps->cabac ? "CABAC" : "CAVLC",
               pps->slice_group_count,
               pps->ref_count[0], pps->ref_count[1],
               pps->weighted_pred ? "weighted" : "",
               pps->init_qp, pps->init_qs, pps->chroma_qp_index_offset,
               pps->deblocking_filter_parameters_present ? "LPAR" : "",
               pps->constrained_intra_pred ? "CONSTR" : "",
               pps->redundant_pic_cnt_present ? "REDU" : "",
               pps->transform_8x8_mode ? "8x8DCT" : ""
               );
    }

    return 0;
}

/**
 * finds the end of the current frame in the bitstream.
 * @return the position of the first byte of the next frame, or -1
 */
static int find_frame_end(H264Context *h, const uint8_t *buf, int buf_size){
    int i;
    uint32_t state;
    ParseContext *pc = &(h->s.parse_context);
//printf("first %02X%02X%02X%02X\n", buf[0], buf[1],buf[2],buf[3]);
//    mb_addr= pc->mb_addr - 1;
    state= pc->state;
    if(state>13)
        state= 7;

    for(i=0; i<buf_size; i++){
        if(state==7){
            for(; i<buf_size; i++){
                if(!buf[i]){
                    state=2;
                    break;
                }
            }
        }else if(state<=2){
            if(buf[i]==1)   state^= 5; //2->7, 1->4, 0->5
            else if(buf[i]) state = 7;
            else            state>>=1; //2->1, 1->0, 0->0
        }else if(state<=5){
            int v= buf[i] & 0x1F;
            if(v==7 || v==8 || v==9){
                if(pc->frame_start_found){
                    i++;
found:
                    pc->state=7;
                    pc->frame_start_found= 0;
                    return i-(state&5);
                }
            }else if(v==1 || v==2 || v==5){
                if(pc->frame_start_found){
                    state+=8;
                    continue;
                }else
                    pc->frame_start_found = 1;
            }
            state= 7;
        }else{
            if(buf[i] & 0x80)
                goto found;
            state= 7;
        }
    }
    pc->state= state;
    return END_NOT_FOUND;
}

#ifdef CONFIG_H264_PARSER
static int h264_parse(AVCodecParserContext *s,
                      AVCodecContext *avctx,
                      uint8_t **poutbuf, int *poutbuf_size,
                      const uint8_t *buf, int buf_size)
{
    H264Context *h = s->priv_data;
    ParseContext *pc = &h->s.parse_context;
    int next;

    if(s->flags & PARSER_FLAG_COMPLETE_FRAMES){
        next= buf_size;
    }else{
        next= find_frame_end(h, buf, buf_size);

        if (ff_combine_frame(pc, next, (uint8_t **)&buf, &buf_size) < 0) {
            *poutbuf = NULL;
            *poutbuf_size = 0;
            return buf_size;
        }

        if(next<0){
            find_frame_end(h, &pc->buffer[pc->last_index + next], -next); //update state
        }
    }

    *poutbuf = (uint8_t *)buf;
    *poutbuf_size = buf_size;
    return next;
}

static int h264_split(AVCodecContext *avctx,
                      const uint8_t *buf, int buf_size)
{
    int i;
    uint32_t state = -1;
    int has_sps= 0;

    for(i=0; i<=buf_size; i++){
        if((state&0xFFFFFF1F) == 0x107)
            has_sps=1;
/*        if((state&0xFFFFFF1F) == 0x101 || (state&0xFFFFFF1F) == 0x102 || (state&0xFFFFFF1F) == 0x105){
        }*/
        if((state&0xFFFFFF00) == 0x100 && (state&0xFFFFFF1F) != 0x107 && (state&0xFFFFFF1F) != 0x108 && (state&0xFFFFFF1F) != 0x109){
            if(has_sps){
                while(i>4 && buf[i-5]==0) i--;
                return i-4;
            }
        }
        if (i<buf_size)
            state= (state<<8) | buf[i];
    }
    return 0;
}
#endif /* CONFIG_H264_PARSER */

static int decode_nal_units(H264Context *h, uint8_t *buf, int buf_size){
    MpegEncContext * const s = &h->s;
    AVCodecContext * const avctx= s->avctx;
    int buf_index=0;
#if 0
    int i;
    for(i=0; i<50; i++){
        av_log(NULL, AV_LOG_ERROR,"%02X ", buf[i]);
    }
#endif
    if(!(s->flags2 & CODEC_FLAG2_CHUNKS)){
        h->slice_num = 0;
        s->current_picture_ptr= NULL;
    }

    for(;;){
        int consumed;
        int dst_length;
        int bit_length;
        uint8_t *ptr;
        int i, nalsize = 0;

      if(h->is_avc) {
        if(buf_index >= buf_size) break;
        nalsize = 0;
        for(i = 0; i < h->nal_length_size; i++)
            nalsize = (nalsize << 8) | buf[buf_index++];
        if(nalsize <= 1 || nalsize > buf_size){
            if(nalsize == 1){
                buf_index++;
                continue;
            }else{
                av_log(h->s.avctx, AV_LOG_ERROR, "AVC: nal size %d\n", nalsize);
                break;
            }
        }
      } else {
        // start code prefix search
        for(; buf_index + 3 < buf_size; buf_index++){
            // this should allways succeed in the first iteration
            if(buf[buf_index] == 0 && buf[buf_index+1] == 0 && buf[buf_index+2] == 1)
                break;
        }

        if(buf_index+3 >= buf_size) break;

        buf_index+=3;
      }

        ptr= decode_nal(h, buf + buf_index, &dst_length, &consumed, h->is_avc ? nalsize : buf_size - buf_index);
        if (ptr==NULL || dst_length <= 0){
            return -1;
        }
        while(ptr[dst_length - 1] == 0 && dst_length > 1)
            dst_length--;
        bit_length= 8*dst_length - decode_rbsp_trailing(h, ptr + dst_length - 1);

        if(s->avctx->debug&FF_DEBUG_STARTCODE){
            av_log(h->s.avctx, AV_LOG_DEBUG, "NAL %d at %d/%d length %d\n", h->nal_unit_type, buf_index, buf_size, dst_length);
        }

        if (h->is_avc && (nalsize != consumed))
            av_log(h->s.avctx, AV_LOG_ERROR, "AVC: Consumed only %d bytes instead of %d\n", consumed, nalsize);

        buf_index += consumed;

        if(  (s->hurry_up == 1 && h->nal_ref_idc  == 0) //FIXME dont discard SEI id
           ||(avctx->skip_frame >= AVDISCARD_NONREF && h->nal_ref_idc  == 0))
            continue;

        switch(h->nal_unit_type){
        case NAL_IDR_SLICE:
            idr(h); //FIXME ensure we don't loose some frames if there is reordering
        case NAL_SLICE:
            init_get_bits(&s->gb, ptr, bit_length);
            h->intra_gb_ptr=
            h->inter_gb_ptr= &s->gb;
            s->data_partitioning = 0;

            if(decode_slice_header(h) < 0){
                av_log(h->s.avctx, AV_LOG_ERROR, "decode_slice_header error\n");
                break;
            }
            s->current_picture_ptr->key_frame= (h->nal_unit_type == NAL_IDR_SLICE);
            if(h->redundant_pic_count==0 && s->hurry_up < 5
               && (avctx->skip_frame < AVDISCARD_NONREF || h->nal_ref_idc)
               && (avctx->skip_frame < AVDISCARD_BIDIR  || h->slice_type!=B_TYPE)
               && (avctx->skip_frame < AVDISCARD_NONKEY || h->slice_type==I_TYPE)
               && avctx->skip_frame < AVDISCARD_ALL)
                decode_slice(h);
            break;
        case NAL_DPA:
            init_get_bits(&s->gb, ptr, bit_length);
            h->intra_gb_ptr=
            h->inter_gb_ptr= NULL;
            s->data_partitioning = 1;

            if(decode_slice_header(h) < 0){
                av_log(h->s.avctx, AV_LOG_ERROR, "decode_slice_header error\n");
            }
            break;
        case NAL_DPB:
            init_get_bits(&h->intra_gb, ptr, bit_length);
            h->intra_gb_ptr= &h->intra_gb;
            break;
        case NAL_DPC:
            init_get_bits(&h->inter_gb, ptr, bit_length);
            h->inter_gb_ptr= &h->inter_gb;

            if(h->redundant_pic_count==0 && h->intra_gb_ptr && s->data_partitioning
               && s->context_initialized
               && s->hurry_up < 5
               && (avctx->skip_frame < AVDISCARD_NONREF || h->nal_ref_idc)
               && (avctx->skip_frame < AVDISCARD_BIDIR  || h->slice_type!=B_TYPE)
               && (avctx->skip_frame < AVDISCARD_NONKEY || h->slice_type==I_TYPE)
               && avctx->skip_frame < AVDISCARD_ALL)
                decode_slice(h);
            break;
        case NAL_SEI:
            init_get_bits(&s->gb, ptr, bit_length);
            decode_sei(h);
            break;
        case NAL_SPS:
            init_get_bits(&s->gb, ptr, bit_length);
            decode_seq_parameter_set(h);

            if(s->flags& CODEC_FLAG_LOW_DELAY)
                s->low_delay=1;

            if(avctx->has_b_frames < 2)
                avctx->has_b_frames= !s->low_delay;
            break;
        case NAL_PPS:
            init_get_bits(&s->gb, ptr, bit_length);

            decode_picture_parameter_set(h, bit_length);

            break;
        case NAL_AUD:
        case NAL_END_SEQUENCE:
        case NAL_END_STREAM:
        case NAL_FILLER_DATA:
        case NAL_SPS_EXT:
        case NAL_AUXILIARY_SLICE:
            break;
        default:
            av_log(avctx, AV_LOG_ERROR, "Unknown NAL code: %d\n", h->nal_unit_type);
        }
    }

    return buf_index;
}

/**
 * returns the number of bytes consumed for building the current frame
 */
static int get_consumed_bytes(MpegEncContext *s, int pos, int buf_size){
    if(s->flags&CODEC_FLAG_TRUNCATED){
        pos -= s->parse_context.last_index;
        if(pos<0) pos=0; // FIXME remove (unneeded?)

        return pos;
    }else{
        if(pos==0) pos=1; //avoid infinite loops (i doubt thats needed but ...)
        if(pos+10>buf_size) pos=buf_size; // oops ;)

        return pos;
    }
}

static int decode_frame(AVCodecContext *avctx,
                             void *data, int *data_size,
                             uint8_t *buf, int buf_size)
{
    H264Context *h = avctx->priv_data;
    MpegEncContext *s = &h->s;
    AVFrame *pict = data;
    int buf_index;

    s->flags= avctx->flags;
    s->flags2= avctx->flags2;

   /* no supplementary picture */
    if (buf_size == 0) {
        Picture *out;
        int i, out_idx;

//FIXME factorize this with the output code below
        out = h->delayed_pic[0];
        out_idx = 0;
        for(i=1; h->delayed_pic[i] && !h->delayed_pic[i]->key_frame; i++)
            if(h->delayed_pic[i]->poc < out->poc){
                out = h->delayed_pic[i];
                out_idx = i;
            }

        for(i=out_idx; h->delayed_pic[i]; i++)
            h->delayed_pic[i] = h->delayed_pic[i+1];

        if(out){
            *data_size = sizeof(AVFrame);
            *pict= *(AVFrame*)out;
        }

        return 0;
    }

    if(s->flags&CODEC_FLAG_TRUNCATED){
        int next= find_frame_end(h, buf, buf_size);

        if( ff_combine_frame(&s->parse_context, next, &buf, &buf_size) < 0 )
            return buf_size;
//printf("next:%d buf_size:%d last_index:%d\n", next, buf_size, s->parse_context.last_index);
    }

    if(h->is_avc && !h->got_avcC) {
        int i, cnt, nalsize;
        unsigned char *p = avctx->extradata;
        if(avctx->extradata_size < 7) {
            av_log(avctx, AV_LOG_ERROR, "avcC too short\n");
            return -1;
        }
        if(*p != 1) {
            av_log(avctx, AV_LOG_ERROR, "Unknown avcC version %d\n", *p);
            return -1;
        }
        /* sps and pps in the avcC always have length coded with 2 bytes,
           so put a fake nal_length_size = 2 while parsing them */
        h->nal_length_size = 2;
        // Decode sps from avcC
        cnt = *(p+5) & 0x1f; // Number of sps
        p += 6;
        for (i = 0; i < cnt; i++) {
            nalsize = AV_RB16(p) + 2;
            if(decode_nal_units(h, p, nalsize) < 0) {
                av_log(avctx, AV_LOG_ERROR, "Decoding sps %d from avcC failed\n", i);
                return -1;
            }
            p += nalsize;
        }
        // Decode pps from avcC
        cnt = *(p++); // Number of pps
        for (i = 0; i < cnt; i++) {
            nalsize = AV_RB16(p) + 2;
            if(decode_nal_units(h, p, nalsize)  != nalsize) {
                av_log(avctx, AV_LOG_ERROR, "Decoding pps %d from avcC failed\n", i);
                return -1;
            }
            p += nalsize;
        }
        // Now store right nal length size, that will be use to parse all other nals
        h->nal_length_size = ((*(((char*)(avctx->extradata))+4))&0x03)+1;
        // Do not reparse avcC
        h->got_avcC = 1;
    }

    if(avctx->frame_number==0 && !h->is_avc && s->avctx->extradata_size){
        if(decode_nal_units(h, s->avctx->extradata, s->avctx->extradata_size) < 0)
            return -1;
    }

    buf_index=decode_nal_units(h, buf, buf_size);
    if(buf_index < 0)
        return -1;

    if(!(s->flags2 & CODEC_FLAG2_CHUNKS) && !s->current_picture_ptr){
        av_log(avctx, AV_LOG_ERROR, "no frame!\n");
        return -1;
    }

    if(!(s->flags2 & CODEC_FLAG2_CHUNKS) || (s->mb_y >= s->mb_height && s->mb_height)){
        Picture *out = s->current_picture_ptr;
        Picture *cur = s->current_picture_ptr;
        Picture *prev = h->delayed_output_pic;
        int i, pics, cross_idr, out_of_order, out_idx;

        s->mb_y= 0;

        s->current_picture_ptr->qscale_type= FF_QSCALE_TYPE_H264;
        s->current_picture_ptr->pict_type= s->pict_type;

        h->prev_frame_num_offset= h->frame_num_offset;
        h->prev_frame_num= h->frame_num;
        if(s->current_picture_ptr->reference){
            h->prev_poc_msb= h->poc_msb;
            h->prev_poc_lsb= h->poc_lsb;
        }
        if(s->current_picture_ptr->reference)
            execute_ref_pic_marking(h, h->mmco, h->mmco_index);

        ff_er_frame_end(s);

        MPV_frame_end(s);

    //FIXME do something with unavailable reference frames

#if 0 //decode order
        *data_size = sizeof(AVFrame);
#else
        /* Sort B-frames into display order */

        if(h->sps.bitstream_restriction_flag
           && s->avctx->has_b_frames < h->sps.num_reorder_frames){
            s->avctx->has_b_frames = h->sps.num_reorder_frames;
            s->low_delay = 0;
        }

        pics = 0;
        while(h->delayed_pic[pics]) pics++;

        assert(pics+1 < sizeof(h->delayed_pic) / sizeof(h->delayed_pic[0]));

        h->delayed_pic[pics++] = cur;
        if(cur->reference == 0)
            cur->reference = 1;

        cross_idr = 0;
        for(i=0; h->delayed_pic[i]; i++)
            if(h->delayed_pic[i]->key_frame || h->delayed_pic[i]->poc==0)
                cross_idr = 1;

        out = h->delayed_pic[0];
        out_idx = 0;
        for(i=1; h->delayed_pic[i] && !h->delayed_pic[i]->key_frame; i++)
            if(h->delayed_pic[i]->poc < out->poc){
                out = h->delayed_pic[i];
                out_idx = i;
            }

        out_of_order = !cross_idr && prev && out->poc < prev->poc;
        if(h->sps.bitstream_restriction_flag && s->avctx->has_b_frames >= h->sps.num_reorder_frames)
            { }
        else if(prev && pics <= s->avctx->has_b_frames)
            out = prev;
        else if((out_of_order && pics-1 == s->avctx->has_b_frames && pics < 15)
           || (s->low_delay &&
            ((!cross_idr && prev && out->poc > prev->poc + 2)
             || cur->pict_type == B_TYPE)))
        {
            s->low_delay = 0;
            s->avctx->has_b_frames++;
            out = prev;
        }
        else if(out_of_order)
            out = prev;

        if(out_of_order || pics > s->avctx->has_b_frames){
            for(i=out_idx; h->delayed_pic[i]; i++)
                h->delayed_pic[i] = h->delayed_pic[i+1];
        }

        if(prev == out)
            *data_size = 0;
        else
            *data_size = sizeof(AVFrame);
        if(prev && prev != out && prev->reference == 1)
            prev->reference = 0;
        h->delayed_output_pic = out;
#endif

        if(out)
            *pict= *(AVFrame*)out;
        else
            av_log(avctx, AV_LOG_DEBUG, "no picture\n");
    }

    assert(pict->data[0] || !*data_size);
    ff_print_debug_info(s, pict);
//printf("out %d\n", (int)pict->data[0]);
#if 0 //?

    /* Return the Picture timestamp as the frame number */
    /* we substract 1 because it is added on utils.c    */
    avctx->frame_number = s->picture_number - 1;
#endif
    return get_consumed_bytes(s, buf_index, buf_size);
}
#if 0
static inline void fill_mb_avail(H264Context *h){
    MpegEncContext * const s = &h->s;
    const int mb_xy= s->mb_x + s->mb_y*s->mb_stride;

    if(s->mb_y){
        h->mb_avail[0]= s->mb_x                 && h->slice_table[mb_xy - s->mb_stride - 1] == h->slice_num;
        h->mb_avail[1]=                            h->slice_table[mb_xy - s->mb_stride    ] == h->slice_num;
        h->mb_avail[2]= s->mb_x+1 < s->mb_width && h->slice_table[mb_xy - s->mb_stride + 1] == h->slice_num;
    }else{
        h->mb_avail[0]=
        h->mb_avail[1]=
        h->mb_avail[2]= 0;
    }
    h->mb_avail[3]= s->mb_x && h->slice_table[mb_xy - 1] == h->slice_num;
    h->mb_avail[4]= 1; //FIXME move out
    h->mb_avail[5]= 0; //FIXME move out
}
#endif

#if 0 //selftest
#define COUNT 8000
#define SIZE (COUNT*40)
int main(){
    int i;
    uint8_t temp[SIZE];
    PutBitContext pb;
    GetBitContext gb;
//    int int_temp[10000];
    DSPContext dsp;
    AVCodecContext avctx;

    dsputil_init(&dsp, &avctx);

    init_put_bits(&pb, temp, SIZE);
    printf("testing unsigned exp golomb\n");
    for(i=0; i<COUNT; i++){
        START_TIMER
        set_ue_golomb(&pb, i);
        STOP_TIMER("set_ue_golomb");
    }
    flush_put_bits(&pb);

    init_get_bits(&gb, temp, 8*SIZE);
    for(i=0; i<COUNT; i++){
        int j, s;

        s= show_bits(&gb, 24);

        START_TIMER
        j= get_ue_golomb(&gb);
        if(j != i){
            printf("missmatch! at %d (%d should be %d) bits:%6X\n", i, j, i, s);
//            return -1;
        }
        STOP_TIMER("get_ue_golomb");
    }


    init_put_bits(&pb, temp, SIZE);
    printf("testing signed exp golomb\n");
    for(i=0; i<COUNT; i++){
        START_TIMER
        set_se_golomb(&pb, i - COUNT/2);
        STOP_TIMER("set_se_golomb");
    }
    flush_put_bits(&pb);

    init_get_bits(&gb, temp, 8*SIZE);
    for(i=0; i<COUNT; i++){
        int j, s;

        s= show_bits(&gb, 24);

        START_TIMER
        j= get_se_golomb(&gb);
        if(j != i - COUNT/2){
            printf("missmatch! at %d (%d should be %d) bits:%6X\n", i, j, i, s);
//            return -1;
        }
        STOP_TIMER("get_se_golomb");
    }

    printf("testing 4x4 (I)DCT\n");

    DCTELEM block[16];
    uint8_t src[16], ref[16];
    uint64_t error= 0, max_error=0;

    for(i=0; i<COUNT; i++){
        int j;
//        printf("%d %d %d\n", r1, r2, (r2-r1)*16);
        for(j=0; j<16; j++){
            ref[j]= random()%255;
            src[j]= random()%255;
        }

        h264_diff_dct_c(block, src, ref, 4);

        //normalize
        for(j=0; j<16; j++){
//            printf("%d ", block[j]);
            block[j]= block[j]*4;
            if(j&1) block[j]= (block[j]*4 + 2)/5;
            if(j&4) block[j]= (block[j]*4 + 2)/5;
        }
//        printf("\n");

        s->dsp.h264_idct_add(ref, block, 4);
/*        for(j=0; j<16; j++){
            printf("%d ", ref[j]);
        }
        printf("\n");*/

        for(j=0; j<16; j++){
            int diff= FFABS(src[j] - ref[j]);

            error+= diff*diff;
            max_error= FFMAX(max_error, diff);
        }
    }
    printf("error=%f max_error=%d\n", ((float)error)/COUNT/16, (int)max_error );
#if 0
    printf("testing quantizer\n");
    for(qp=0; qp<52; qp++){
        for(i=0; i<16; i++)
            src1_block[i]= src2_block[i]= random()%255;

    }
#endif
    printf("Testing NAL layer\n");

    uint8_t bitstream[COUNT];
    uint8_t nal[COUNT*2];
    H264Context h;
    memset(&h, 0, sizeof(H264Context));

    for(i=0; i<COUNT; i++){
        int zeros= i;
        int nal_length;
        int consumed;
        int out_length;
        uint8_t *out;
        int j;

        for(j=0; j<COUNT; j++){
            bitstream[j]= (random() % 255) + 1;
        }

        for(j=0; j<zeros; j++){
            int pos= random() % COUNT;
            while(bitstream[pos] == 0){
                pos++;
                pos %= COUNT;
            }
            bitstream[pos]=0;
        }

        START_TIMER

        nal_length= encode_nal(&h, nal, bitstream, COUNT, COUNT*2);
        if(nal_length<0){
            printf("encoding failed\n");
            return -1;
        }

        out= decode_nal(&h, nal, &out_length, &consumed, nal_length);

        STOP_TIMER("NAL")

        if(out_length != COUNT){
            printf("incorrect length %d %d\n", out_length, COUNT);
            return -1;
        }

        if(consumed != nal_length){
            printf("incorrect consumed length %d %d\n", nal_length, consumed);
            return -1;
        }

        if(memcmp(bitstream, out, COUNT)){
            printf("missmatch\n");
            return -1;
        }
    }

    printf("Testing RBSP\n");


    return 0;
}
#endif


static int decode_end(AVCodecContext *avctx)
{
    H264Context *h = avctx->priv_data;
    MpegEncContext *s = &h->s;

    av_freep(&h->rbsp_buffer);
    free_tables(h); //FIXME cleanup init stuff perhaps
    MPV_common_end(s);

//    memset(h, 0, sizeof(H264Context));

    return 0;
}


AVCodec h264_decoder = {
    "h264",
    CODEC_TYPE_VIDEO,
    CODEC_ID_H264,
    sizeof(H264Context),
    decode_init,
    NULL,
    decode_end,
    decode_frame,
    /*CODEC_CAP_DRAW_HORIZ_BAND |*/ CODEC_CAP_DR1 | CODEC_CAP_TRUNCATED | CODEC_CAP_DELAY,
    .flush= flush_dpb,
};

#ifdef CONFIG_H264_PARSER
AVCodecParser h264_parser = {
    { CODEC_ID_H264 },
    sizeof(H264Context),
    NULL,
    h264_parse,
    ff_parse_close,
    h264_split,
};
#endif

#include "svq3.c"
