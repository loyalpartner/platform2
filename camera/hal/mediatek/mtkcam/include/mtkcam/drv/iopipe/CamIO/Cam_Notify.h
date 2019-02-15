/*
 * Copyright (C) 2019 MediaTek Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef CAMERA_HAL_MEDIATEK_MTKCAM_INCLUDE_MTKCAM_DRV_IOPIPE_CAMIO_CAM_NOTIFY_H_
#define CAMERA_HAL_MEDIATEK_MTKCAM_INCLUDE_MTKCAM_DRV_IOPIPE_CAMIO_CAM_NOTIFY_H_

#include <mtkcam/def/common.h>

/**
    callback path , callback at a user-inidicated timing.
*/
class P1_TUNING_NOTIFY {
 public:
  virtual ~P1_TUNING_NOTIFY() {}
  virtual const char* TuningName(void) = 0;
  virtual void p1TuningNotify(MVOID* pInput, MVOID* pOutput) = 0;

  MVOID* m_pClassObj;
};

typedef struct _LMV_INPUT_INFO {
  NSCam::MSize sRMXOut;
  NSCam::MSize sHBINOut;  // HDS
  NSCam::MSize sTGOut;
  MBOOL bYUVFmt;

  MUINT32 pixMode;

  struct {
    MUINT32 start_x;
    MUINT32 start_y;
    MUINT32 crop_size_w;
    MUINT32 crop_size_h;

    MUINT32 in_size_w;
    MUINT32 in_size_h;
  } RRZ_IN_CROP;
} LMV_INPUT_INFO;

typedef struct _LMV_SGG_CFG {
  MUINT32 PGN;
  MUINT32 GMRC_1;
  MUINT32 GMRC_2;
  MBOOL bSGG2_Bypass;
  MBOOL bSGG2_EN;
} LMV_SGG_CFG;

/* cfg_* register order must same to isp_reg.h*/
typedef struct _LMV_CFG {
  MUINT32 cfg_lmv_prep_me_ctrl1;  // CAM_LMV_PREP_ME_CTRL1
  MUINT32 cfg_lmv_prep_me_ctrl2;  // CAM_LMV_PREP_ME_CTRL2
  MUINT32 cfg_lmv_lmv_th;         // CAM_LMV_LMV_TH
  MUINT32 cfg_lmv_fl_offset;      // CAM_LMV_FL_OFFSET
  MUINT32 cfg_lmv_mb_offset;      // CAM_LMV_MB_OFFSET
  MUINT32 cfg_lmv_mb_interval;    // CAM_LMV_MB_INTERVAL
  MUINT32 cfg_lmv_gmv;            // CAM_LMV_GMV, not use
  MUINT32 cfg_lmv_err_ctrl;       // CAM_LMV_ERR_CTRL, not use
  MUINT32 cfg_lmv_image_ctrl;     // CAM_LMV_IMAGE_CTRL
  MUINT32 enLMV;
  MUINT32 bypassLMV;

 public:
  _LMV_CFG(MUINT32 a_lmvPrepMeCtrl1 = 0,
           MUINT32 a_lmvPrepMeCtrl2 = 0,
           MUINT32 a_lmvLmvTh = 0,
           MUINT32 a_lmvFlOffset = 0,
           MUINT32 a_lmvMbOffset = 0,
           MUINT32 a_lmvMbInterval = 0,
           MUINT32 a_lmvGmv = 0,
           MUINT32 a_lmvErrCtrl = 0,
           MUINT32 a_lmvImageCtrl = 0)
      : cfg_lmv_prep_me_ctrl1(a_lmvPrepMeCtrl1),
        cfg_lmv_prep_me_ctrl2(a_lmvPrepMeCtrl2),
        cfg_lmv_lmv_th(a_lmvLmvTh),
        cfg_lmv_fl_offset(a_lmvFlOffset),
        cfg_lmv_mb_offset(a_lmvMbOffset),
        cfg_lmv_mb_interval(a_lmvMbInterval),
        cfg_lmv_gmv(a_lmvGmv),
        cfg_lmv_err_ctrl(a_lmvErrCtrl),
        cfg_lmv_image_ctrl(a_lmvImageCtrl) {
    enLMV = 0;
    bypassLMV = 0;
  }
} LMV_CFG;

typedef struct _RSS_CROP_SIZE {
  MFLOAT w_start;
  MFLOAT h_start;
  MUINT32 w_size;
  MUINT32 h_size;
} RSS_CROP_SIZE;

typedef struct _RSS_INPUT_INFO {
  MUINT32 tg_out_w;             // tg output width
  MUINT32 tg_out_h;             // tg output height
  MUINT32 rss_in_w;             // rss input width
  MUINT32 rss_in_h;             // rss input hegiht
  MUINT32 rss_scale_up_factor;  // rss scale up factor, default:100
  MUINT32 rrz_out_w;            // rrz output width
  MUINT32 rrz_out_h;            // rrz output height
  MUINT32 bYUVFmt;              // YUV format or not
 public:
  _RSS_INPUT_INFO() {
    tg_out_w = 0;
    tg_out_h = 0;
    rss_in_w = 0;
    rss_in_h = 0;
    rss_scale_up_factor = 100;
    rrz_out_w = 0;
    rrz_out_h = 0;
    bYUVFmt = 0;
  }
} RSS_INPUT_INFO;

typedef struct _RSS_CFG {
  MUINT32 cfg_rss_ctrl_hori_en;
  MUINT32 cfg_rss_ctrl_vert_en;
  MUINT32 cfg_rss_ctrl_output_wait_en;
  MUINT32 cfg_rss_ctrl_vert_first;
  MUINT32 cfg_rss_ctrl_hori_tbl_sel;
  MUINT32 cfg_rss_ctrl_vert_tbl_sel;
  MUINT32 cfg_rss_in_img;
  MUINT32 cfg_rss_out_img;
  MUINT32 cfg_rss_hori_step;
  MUINT32 cfg_rss_vert_step;
  MUINT32 cfg_rss_hori_int_ofst;
  MUINT32 cfg_rss_hori_sub_ofst;
  MUINT32 cfg_rss_vert_int_ofst;
  MUINT32 cfg_rss_vert_sub_ofst;
  MUINT32 enRSS;
  MUINT32 bypassRSS;

 public:
  _RSS_CFG() {
    cfg_rss_ctrl_hori_en = 0;
    cfg_rss_ctrl_vert_en = 0;
    cfg_rss_ctrl_output_wait_en = 0;
    cfg_rss_ctrl_vert_first = 0;
    cfg_rss_ctrl_hori_tbl_sel = 0;
    cfg_rss_ctrl_vert_tbl_sel = 0;
    cfg_rss_in_img = 0;
    cfg_rss_out_img = 0;
    cfg_rss_hori_step = 0;
    cfg_rss_vert_step = 0;
    cfg_rss_hori_int_ofst = 0;
    cfg_rss_hori_sub_ofst = 0;
    cfg_rss_vert_int_ofst = 0;
    cfg_rss_vert_sub_ofst = 0;
    enRSS = 0;
    bypassRSS = 1;
  }
} RSS_CFG;

typedef struct _BIN_INOUT_INFO {
  MUINT32 TgOut_W;
  MUINT32 TgOut_H;
  MUINT32 Bin_MD;  // bypass: RrzCB
  MUINT32 TarBin_EN;
  MUINT32 TarBinOut_W;
  MUINT32 TarBinOut_H;
  MUINT32 CurBinOut_W;  // bypass: RrzCB
  MUINT32 CurBinOut_H;  // bypass: RrzCB
  MUINT32 Magic;
  MUINT32 TarQBNOut_W;  // for AA
  MUINT32 TarRMBOut_W;  // for PS
  MUINT32 CurQBNOut_W;  // for AA
  MUINT32 CurRMBOut_W;  // for PS
} BIN_INPUT_INFO;

typedef struct _RRZ_REG_CFG {
  MBOOL bRRZ_Bypass;
  MUINT32 src_x;
  MUINT32 src_y;
  MUINT32 src_w;
  MUINT32 src_h;
  MUINT32 tar_w;
  MUINT32 tar_h;
} RRZ_REG_CFG;

typedef union {
  struct {
    MUINT32 AF : 1;
    MUINT32 AA : 1;
    MUINT32 FLK : 1;
    MUINT32 LSC : 1;
    MUINT32 DBS : 1;
    MUINT32 ADBS : 1;
    MUINT32 RMG : 1;
    MUINT32 BNR : 1;
    MUINT32 RMM : 1;
    MUINT32 DCPN : 1;
    MUINT32 CPN : 1;
    MUINT32 RPG : 1;
    MUINT32 CPG : 1;
    MUINT32 SL2F : 1;
    MUINT32 PS : 1;
    MUINT32 rsv : 17;
  } Bits;
  MUINT32 Raw;
} CbBypass;

typedef struct _Tuning_Dma {
  MUINTPTR va;
  MINT32 memID;
} Tuning_Dma;

typedef struct _Tuning_CFG {
  MVOID* pIspReg;
  CbBypass Bypass;
  Tuning_Dma dma_bpci;
  Tuning_Dma dma_lsci;
} Tuning_CFG;

#endif  // CAMERA_HAL_MEDIATEK_MTKCAM_INCLUDE_MTKCAM_DRV_IOPIPE_CAMIO_CAM_NOTIFY_H_
