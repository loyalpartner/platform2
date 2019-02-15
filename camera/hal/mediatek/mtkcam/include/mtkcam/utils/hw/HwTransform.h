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

#ifndef CAMERA_HAL_MEDIATEK_MTKCAM_INCLUDE_MTKCAM_UTILS_HW_HWTRANSFORM_H_
#define CAMERA_HAL_MEDIATEK_MTKCAM_INCLUDE_MTKCAM_UTILS_HW_HWTRANSFORM_H_
//
#include <algorithm>
#include <mtkcam/def/common.h>

/******************************************************************************
 *
 ******************************************************************************/
namespace NSCamHW {

/******************************************************************************
 * Simplified transform matrix for camera hw module.
 * This simlified transform matrix considers translation and scaling.
 * (rotation is not considered.)
 *  v' = Mv = (TST)*v;
 *  v = [x, y, 1]^T, T: translation, S: scaling
 *
 *  x' = c_00 * x + c_02
 *  y' = c_11 * y + c_12
 ******************************************************************************/
class VISIBILITY_PUBLIC simplifiedMatrix {
 public:
  simplifiedMatrix(float c00 = 1.f,
                   float c02 = 0.f,
                   float c11 = 1.f,
                   float c12 = 0.f)
      : c_00(c00), c_02(c02), c_11(c11), c_12(c12) {}

  MBOOL getInverse(simplifiedMatrix& inv) const {
    if (c_00 == 0.f || c_11 == 0.f) {
      return MFALSE;
    }
    //
    inv = simplifiedMatrix(1.f / c_00, -c_02 / c_00, 1.f / c_11, -c_12 / c_11);
    return MTRUE;
  }

  MVOID transform(NSCam::MPoint const& o, NSCam::MPoint* t) const {
    t->x = c_00 * o.x + c_02;
    t->y = c_11 * o.y + c_12;
  }

  MVOID transform(NSCam::MSize const& o, NSCam::MSize* t) const {
    t->w = c_00 * o.w;
    t->h = c_11 * o.h;
  }

  MVOID transform(NSCam::MRect const& o, NSCam::MRect* t) const {
    transform(o.p, &t->p);
    transform(o.s, &t->s);
  }

  simplifiedMatrix operator*(const simplifiedMatrix& mat) {
    return simplifiedMatrix(
        this->c_00 * mat.c_00, this->c_00 * mat.c_02 + this->c_02,
        this->c_11 * mat.c_11, this->c_11 * mat.c_12 + this->c_12);
  }

  MVOID dump(const char* const str) const;  // debug

 private:
  float c_00;
  float c_02;
  float c_11;
  float c_12;
};
typedef simplifiedMatrix HwMatrix;

class VISIBILITY_PUBLIC HwTransHelper {
 public:
  explicit HwTransHelper(MINT32 const openId);
  ~HwTransHelper();

 public:
  /***************************************************************************
   * get the transform matrix from active array to certain sensor mode
   *
   * @param[in]   sensorMode: the target sensor mode coordinates
   *
   * @param[out]  mat: the transform matrix
   *
   * @return
   *  -MTRUE indicates the matrix is valid
   *  -MFALSE indicates the matrix is not valid
   */
  MBOOL getMatrixFromActive(MUINT32 const sensorMode, HwMatrix* mat);

  /***************************************************************************
   * get the transform matrix from certain sensor mode to active array
   *
   * @param[in]   sensorMode: the current sensor mode cooridinates to be
   *                          transformed
   *
   * @param[out]  mat: the transform matrix
   *
   * @return
   *  -MTRUE indicates the matrix is valid
   *  -MFALSE indicates the matrix is not valid
   */
  MBOOL getMatrixToActive(MUINT32 const sensorMode, HwMatrix* mat);

  /***************************************************************************
   * get the transform matrix from active array to certain sensor mode with
   * Aspect Ratio aligned.
   *
   * @param[in]   sensorMode: the target sensor mode coordinates
   *
   * @param[out]  mat: the transform matrix
   *
   * @return
   *  -MTRUE indicates the matrix is valid
   *  -MFALSE indicates the matrix is not valid
   */
  MBOOL getMatrixFromActiveRatioAlign(MUINT32 const sensorMode, HwMatrix* mat);

  /***************************************************************************
   * get the transform matrix from certain sensor mode to active array with
   * Aspect Ratio aligned.
   *
   * @param[in]   sensorMode: the current sensor mode cooridinates to be
   *                          transformed
   *
   * @param[out]  mat: the transform matrix
   *
   * @return
   *  -MTRUE indicates the matrix is valid
   *  -MFALSE indicates the matrix is not valid
   */
  MBOOL getMatrixToActiveRatioAlign(MUINT32 const sensorMode, HwMatrix* mat);

  /***************************************************************************
   * Calcute the fov difference of target sensor mode compared to the active
   * array. Fov difference is defined as the pixel difference on active
   * array domain in both x, y direction. That is,
   *      fov difference percentage = pixel difference amount / active array
   *
   * @param[in]   sensorMode: the current sensor mode
   *
   * @param[out]  fov_diff_x: fov difference percentage in x direction
   * @param[out]  fov_diff_y: fov difference percentage in y direction
   *
   * @return
   *  -MTRUE indicates the result is valid
   *  -MFALSE indicates the  result is invalid
   */
  MBOOL calculateFovDifference(MUINT32 const sensorMode,
                               float* fov_diff_x,
                               float* fov_diff_y);

  static MINT32 div_round(MINT32 const numerator, MINT32 const denominator) {
    return ((numerator < 0) ^ (denominator < 0))
               ? ((numerator - denominator / 2) / denominator)
               : ((numerator + denominator / 2) / denominator);
  }

  /* Crop source rect to match target aspect ratio.
   *  For example, src(4:3) & target(16:9), it will cut off src left&right side.
   */
  static MBOOL cropAlignRatio(NSCam::MRect const& srcRect,
                              NSCam::MSize const& target,
                              NSCam::MRect* outRect) {
    if (srcRect.s.w * target.w > srcRect.s.h * target.h) {  // pillarbox
      outRect->s.w = div_round(srcRect.s.h * target.w, target.h);
      outRect->s.h = srcRect.s.h;
      outRect->p.x = srcRect.p.x + ((srcRect.s.w - outRect->s.w) >> 1);
      outRect->p.y = srcRect.p.y;
    } else {  // letterbox
      outRect->s.w = srcRect.s.w;
      outRect->s.h = div_round(srcRect.s.w * target.h, target.w);
      outRect->p.x = srcRect.p.x;
      outRect->p.y = srcRect.p.y + ((srcRect.s.h - outRect->s.h) >> 1);
    }
    return MTRUE;
  }

  /* Add region to source rect to match target aspect ratio.
   *  For example, src(4:3) & target(16:9), it will add back src left&right
   * side.
   */
  static MBOOL cropAlignRatioInverse(NSCam::MRect const& srcRect,
                                     NSCam::MSize const& target,
                                     NSCam::MRect* outRect) {
    if (srcRect.s.w * target.w < srcRect.s.h * target.h) {  // pillarbox revert
      outRect->s.w = div_round(srcRect.s.h * target.w, target.h);
      outRect->s.h = srcRect.s.h;
      outRect->p.x = srcRect.p.x - ((outRect->s.w - srcRect.s.w) >> 1);
      outRect->p.y = srcRect.p.y;
    } else {  // letterbox revert
      outRect->s.w = srcRect.s.w;
      outRect->s.h = div_round(srcRect.s.w * target.h, target.w);
      outRect->p.x = srcRect.p.x;
      outRect->p.y = srcRect.p.y - ((outRect->s.h - srcRect.s.h) >> 1);
    }
    return MTRUE;
  }

 protected:
  MINT32 const mOpenId;
};

};  // namespace NSCamHW
/******************************************************************************
 *
 ******************************************************************************/

#endif  // CAMERA_HAL_MEDIATEK_MTKCAM_INCLUDE_MTKCAM_UTILS_HW_HWTRANSFORM_H_
