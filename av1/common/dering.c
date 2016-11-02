/*
 * Copyright (c) 2016, Alliance for Open Media. All rights reserved
 *
 * This source code is subject to the terms of the BSD 2 Clause License and
 * the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
 * was not distributed with this source code in the LICENSE file, you can
 * obtain it at www.aomedia.org/license/software. If the Alliance for Open
 * Media Patent License 1.0 was not distributed with this source code in the
 * PATENTS file, you can obtain it at www.aomedia.org/license/patent.
 */

// clang-format off

#include <string.h>
#include <math.h>

#include "./aom_scale_rtcd.h"
#include "aom/aom_integer.h"
#include "av1/common/dering.h"
#include "av1/common/onyxc_int.h"
#include "av1/common/reconinter.h"
#include "av1/common/od_dering.h"

int compute_level_from_index(int global_level, int gi) {
  static const int dering_gains[DERING_REFINEMENT_LEVELS] = { 0, 11, 16, 22 };
  int level;
  if (global_level == 0) return 0;
  level = (global_level * dering_gains[gi] + 8) >> 4;
  return clamp(level, gi, MAX_DERING_LEVEL - 1);
}

int sb_all_skip(const AV1_COMMON *const cm, int mi_row, int mi_col) {
  int r, c;
  int maxc, maxr;
  int skip = 1;
  maxc = cm->mi_cols - mi_col;
  maxr = cm->mi_rows - mi_row;
  if (maxr > MAX_MIB_SIZE) maxr = MAX_MIB_SIZE;
  if (maxc > MAX_MIB_SIZE) maxc = MAX_MIB_SIZE;
  for (r = 0; r < maxr; r++) {
    for (c = 0; c < maxc; c++) {
      skip = skip &&
             cm->mi_grid_visible[(mi_row + r) * cm->mi_stride + mi_col + c]
                 ->mbmi.skip;
    }
  }
  return skip;
}

int sb_all_skip_out(const AV1_COMMON *const cm, int mi_row, int mi_col,
    unsigned char (*bskip)[2], int *count_ptr) {
  int r, c;
  int maxc, maxr;
  int skip = 1;
  MODE_INFO **grid;
  int count=0;
  grid = cm->mi_grid_visible;
  maxc = cm->mi_cols - mi_col;
  maxr = cm->mi_rows - mi_row;
  if (maxr > MAX_MIB_SIZE) maxr = MAX_MIB_SIZE;
  if (maxc > MAX_MIB_SIZE) maxc = MAX_MIB_SIZE;
  for (r = 0; r < maxr; r++) {
    MODE_INFO **grid_row;
    grid_row = &grid[(mi_row + r) * cm->mi_stride + mi_col];
    for (c = 0; c < maxc; c++) {
      if (!grid_row[c]->mbmi.skip) {
        skip = 0;
        bskip[count][0] = r;
        bskip[count][1] = c;
        count++;
      }
    }
  }
  *count_ptr = count;
  return skip;
}

static INLINE void copy_8x8_16_8bit(uint8_t *dst, int dstride, int16_t *src, int sstride) {
  int i, j;
  for (i = 0; i < 8; i++)
    for (j = 0; j < 8; j++)
      dst[i * dstride + j] = src[i * sstride + j];
}

static INLINE void copy_4x4_16_8bit(uint8_t *dst, int dstride, int16_t *src, int sstride) {
  int i, j;
  for (i = 0; i < 4; i++)
    for (j = 0; j < 4; j++)
      dst[i * dstride + j] = src[i * sstride + j];
}

/* TODO: Optimize this function for SSE. */
void copy_blocks_16_8bit(uint8_t *dst, int dstride, int16_t *src, int sstride,
    unsigned char (*bskip)[2], int dering_count, int bsize)
{
  int bi, bx, by;
  if (bsize == 3) {
    for (bi = 0; bi < dering_count; bi++) {
      by = bskip[bi][0];
      bx = bskip[bi][1];
      copy_8x8_16_8bit(&dst[(by << 3) * dstride + (bx << 3)],
                     dstride,
                     &src[(by << 3) * sstride + (bx << 3)], sstride);
    }
  } else {
    for (bi = 0; bi < dering_count; bi++) {
      by = bskip[bi][0];
      bx = bskip[bi][1];
      copy_4x4_16_8bit(&dst[(by << 2) * dstride + (bx << 2)],
                     dstride,
                     &src[(by << 2) * sstride + (bx << 2)], sstride);
    }
  }
}

void av1_dering_frame(YV12_BUFFER_CONFIG *frame, AV1_COMMON *cm,
                      MACROBLOCKD *xd, int global_level) {
  int r, c;
  int sbr, sbc;
  int nhsb, nvsb;
  od_dering_in *src[3];
  unsigned char bskip[MAX_MIB_SIZE*MAX_MIB_SIZE][2];
  int dering_count;
  int dir[OD_DERING_NBLOCKS][OD_DERING_NBLOCKS] = { { 0 } };
  int stride;
  int bsize[3];
  int dec[3];
  int pli;
  int coeff_shift = AOMMAX(cm->bit_depth - 8, 0);
  int nplanes;
  if (xd->plane[1].subsampling_x == xd->plane[1].subsampling_y &&
      xd->plane[2].subsampling_x == xd->plane[2].subsampling_y)
    nplanes = 3;
  else
    nplanes = 1;
  nvsb = (cm->mi_rows + MAX_MIB_SIZE - 1) / MAX_MIB_SIZE;
  nhsb = (cm->mi_cols + MAX_MIB_SIZE - 1) / MAX_MIB_SIZE;
  av1_setup_dst_planes(xd->plane, frame, 0, 0);
  for (pli = 0; pli < 3; pli++) {
    dec[pli] = xd->plane[pli].subsampling_x;
    bsize[pli] = 8 >> dec[pli];
  }
  stride = bsize[0] * cm->mi_cols;
  for (pli = 0; pli < 3; pli++) {
    src[pli] = aom_malloc(sizeof(*src) * cm->mi_rows * cm->mi_cols * 64);
    for (r = 0; r < bsize[pli] * cm->mi_rows; ++r) {
      for (c = 0; c < bsize[pli] * cm->mi_cols; ++c) {
#if CONFIG_AOM_HIGHBITDEPTH
        if (cm->use_highbitdepth) {
          src[pli][r * stride + c] = CONVERT_TO_SHORTPTR(
              xd->plane[pli].dst.buf)[r * xd->plane[pli].dst.stride + c];
        } else {
#endif
          src[pli][r * stride + c] =
              xd->plane[pli].dst.buf[r * xd->plane[pli].dst.stride + c];
#if CONFIG_AOM_HIGHBITDEPTH
        }
#endif
      }
    }
  }
  for (sbr = 0; sbr < nvsb; sbr++) {
    for (sbc = 0; sbc < nhsb; sbc++) {
      int level;
      int nhb, nvb;
      nhb = AOMMIN(MAX_MIB_SIZE, cm->mi_cols - MAX_MIB_SIZE * sbc);
      nvb = AOMMIN(MAX_MIB_SIZE, cm->mi_rows - MAX_MIB_SIZE * sbr);
      level = compute_level_from_index(
          global_level, cm->mi_grid_visible[MAX_MIB_SIZE * sbr * cm->mi_stride +
                                            MAX_MIB_SIZE * sbc]
                            ->mbmi.dering_gain);
      if (level == 0 ||
          sb_all_skip_out(cm, sbr * MAX_MIB_SIZE, sbc * MAX_MIB_SIZE, bskip, &dering_count))
        continue;
      for (pli = 0; pli < nplanes; pli++) {
        int16_t dst[MAX_MIB_SIZE * MAX_MIB_SIZE * 8 * 8];
        int threshold;
        /* FIXME: This is a temporary hack that uses more conservative
           deringing for chroma. */
        if (pli)
          threshold = (level * 5 + 4) >> 3 << coeff_shift;
        else
          threshold = level << coeff_shift;
        if (threshold == 0) continue;
        od_dering(dst, MAX_MIB_SIZE * bsize[pli],
                  &src[pli][sbr * stride * bsize[pli] * MAX_MIB_SIZE +
                            sbc * bsize[pli] * MAX_MIB_SIZE],
                  stride, nhb, nvb, sbc, sbr, nhsb, nvsb, dec[pli], dir, pli,
                  bskip, dering_count, threshold, coeff_shift);
#if CONFIG_AOM_HIGHBITDEPTH
        if (cm->use_highbitdepth) {
          copy_blocks_16bit(
              (int16_t*)&CONVERT_TO_SHORTPTR(
                  xd->plane[pli].dst.buf)[xd->plane[pli].dst.stride *
                  (bsize[pli] * MAX_MIB_SIZE * sbr) +
                  sbc * bsize[pli] * MAX_MIB_SIZE],
              xd->plane[pli].dst.stride, dst, MAX_MIB_SIZE * bsize[pli], bskip,
              dering_count, 3 - dec[pli]);
        } else {
#endif
          copy_blocks_16_8bit(
              &xd->plane[pli].dst.buf[xd->plane[pli].dst.stride *
                                    (bsize[pli] * MAX_MIB_SIZE * sbr) +
                                    sbc * bsize[pli] * MAX_MIB_SIZE],
              xd->plane[pli].dst.stride, dst, MAX_MIB_SIZE * bsize[pli], bskip,
              dering_count, 3 - dec[pli]);
#if CONFIG_AOM_HIGHBITDEPTH
        }
#endif
      }
    }
  }
  for (pli = 0; pli < nplanes; pli++) {
    aom_free(src[pli]);
  }
}
