#include "precompiled.h"

// From [http://leptonica.org/source/leptonica-1.79.0.tar.gz <- https://tpgit.github.io/UnOfficialLeptDocs/leptonica/scaling.html <- google:‘"Area averaging"  filter implementation’]

/*====================================================================*
 -  Copyright (C) 2001 Leptonica.  All rights reserved.
 -
 -  Redistribution and use in source and binary forms, with or without
 -  modification, are permitted provided that the following conditions
 -  are met:
 -  1. Redistributions of source code must retain the above copyright
 -     notice, this list of conditions and the following disclaimer.
 -  2. Redistributions in binary form must reproduce the above
 -     copyright notice, this list of conditions and the following
 -     disclaimer in the documentation and/or other materials
 -     provided with the distribution.
 -
 -  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 -  ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 -  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 -  A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL ANY
 -  CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 -  EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 -  PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 -  PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 -  OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 -  NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 -  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *====================================================================*/

typedef int                     l_int32;    /*!< signed 32-bit value */
typedef unsigned int            l_uint32;   /*!< unsigned 32-bit value */
typedef float                   l_float32;  /*!< 32-bit floating point value */

/*! RGBA Color */
enum {
    COLOR_RED = 1,        /*!< red color index in RGBA_QUAD    */
    COLOR_GREEN = 2,      /*!< green color index in RGBA_QUAD  */
    COLOR_BLUE = 3,       /*!< blue color index in RGBA_QUAD   */
    L_ALPHA_CHANNEL = 0   /*!< alpha value index in RGBA_QUAD  */
};

static const l_int32  L_RED_SHIFT =
       8 * (sizeof(l_uint32) - 1 - COLOR_RED);           /* 24 */
static const l_int32  L_GREEN_SHIFT =
       8 * (sizeof(l_uint32) - 1 - COLOR_GREEN);         /* 16 */
static const l_int32  L_BLUE_SHIFT =
       8 * (sizeof(l_uint32) - 1 - COLOR_BLUE);          /*  8 */

/*!
 * \brief   composeRGBPixel()
 *
 * \param[in]    rval, gval, bval
 * \param[out]   ppixel             32-bit pixel
 * \return  0 if OK; 1 on error
 *
 * <pre>
 * Notes:
 *      (1) All channels are 8 bits: the input values must be between
 *          0 and 255.  For speed, this is not enforced by masking
 *          with 0xff before shifting.
 *      (2) A slower implementation uses macros:
 *            SET_DATA_BYTE(ppixel, COLOR_RED, rval);
 *            SET_DATA_BYTE(ppixel, COLOR_GREEN, gval);
 *            SET_DATA_BYTE(ppixel, COLOR_BLUE, bval);
 * </pre>
 */
void
composeRGBPixel(l_int32    rval,
                l_int32    gval,
                l_int32    bval,
                l_uint32  *ppixel)
{
//     PROCNAME("composeRGBPixel");
//
//     if (!ppixel)
//         return ERROR_INT("&pixel not defined", procName, 1);
//
    *ppixel = ((l_uint32)rval << L_RED_SHIFT) |
              ((l_uint32)gval << L_GREEN_SHIFT) |
              ((l_uint32)bval << L_BLUE_SHIFT);
//     return 0;
}

/*------------------------------------------------------------------*
 *                  General area mapped gray scaling                *
 *------------------------------------------------------------------*/
/*!
 * \brief   scaleColorAreaMapLow()
 *
 * <pre>
 * Notes:
 *      (1) This should only be used for downscaling.
 *          We choose to divide each pixel into 16 x 16 sub-pixels.
 *          This is much slower than scaleSmoothLow(), but it gives a
 *          better representation, esp. for downscaling factors between
 *          1.5 and 5.  All src pixels are subdivided into 256 sub-pixels,
 *          and are weighted by the number of sub-pixels covered by
 *          the dest pixel.  This is about 2x slower than scaleSmoothLow(),
 *          but the results are significantly better on small text.
 * </pre>
 */
static void
scaleColorAreaMapLow(l_uint32  *datad,
                    l_int32    wd,
                    l_int32    hd,
                    l_int32    wpld,
                    const l_uint32  *datas,
                    l_int32    ws,
                    l_int32    hs,
                    l_int32    wpls)
{
l_int32    i, j, k, m, wm2, hm2;
l_int32    area00, area10, area01, area11, areal, arear, areat, areab;
l_int32    xu, yu;  /* UL corner in src image, to 1/16 of a pixel */
l_int32    xl, yl;  /* LR corner in src image, to 1/16 of a pixel */
l_int32    xup, yup, xuf, yuf;  /* UL src pixel: integer and fraction */
l_int32    xlp, ylp, xlf, ylf;  /* LR src pixel: integer and fraction */
l_int32    delx, dely, area;
l_int32    v00r, v00g, v00b;  /* contrib. from UL src pixel */
l_int32    v01r, v01g, v01b;  /* contrib. from LL src pixel */
l_int32    v10r, v10g, v10b;  /* contrib from UR src pixel */
l_int32    v11r, v11g, v11b;  /* contrib from LR src pixel */
l_int32    vinr, ving, vinb;  /* contrib from all full interior src pixels */
l_int32    vmidr, vmidg, vmidb;  /* contrib from side parts */
l_int32    rval, gval, bval;
l_uint32   pixel00, pixel10, pixel01, pixel11, pixel;
const l_uint32  *lines;
l_uint32  *lined;
l_float32  scx, scy;

        /* (scx, scy) are scaling factors that are applied to the
         * dest coords to get the corresponding src coords.
         * We need them because we iterate over dest pixels
         * and must find the corresponding set of src pixels. */
    scx = 16.f * (l_float32)ws / (l_float32)wd;
    scy = 16.f * (l_float32)hs / (l_float32)hd;
    wm2 = ws - 2;
    hm2 = hs - 2;

        /* Iterate over the destination pixels */
    for (i = 0; i < hd; i++) {
        yu = (l_int32)(scy * i);
        yl = (l_int32)(scy * (i + 1.0));
        yup = yu >> 4;
        yuf = yu & 0x0f;
        ylp = yl >> 4;
        ylf = yl & 0x0f;
        dely = ylp - yup;
        lined = datad + i * wpld;
        lines = datas + yup * wpls;
        for (j = 0; j < wd; j++) {
            xu = (l_int32)(scx * j);
            xl = (l_int32)(scx * (j + 1.0));
            xup = xu >> 4;
            xuf = xu & 0x0f;
            xlp = xl >> 4;
            xlf = xl & 0x0f;
            delx = xlp - xup;

                /* If near the edge, just use a src pixel value */
            if (xlp > wm2 || ylp > hm2) {
                *(lined + j) = *(lines + xup);
                continue;
            }

                /* Area summed over, in subpixels.  This varies
                 * due to the quantization, so we can't simply take
                 * the area to be a constant: area = scx * scy. */
            area = ((16 - xuf) + 16 * (delx - 1) + xlf) *
                   ((16 - yuf) + 16 * (dely - 1) + ylf);

                /* Do area map summation */
            pixel00 = *(lines + xup);
            pixel10 = *(lines + xlp);
            pixel01 = *(lines + dely * wpls +  xup);
            pixel11 = *(lines + dely * wpls +  xlp);
            area00 = (16 - xuf) * (16 - yuf);
            area10 = xlf * (16 - yuf);
            area01 = (16 - xuf) * ylf;
            area11 = xlf * ylf;
            v00r = area00 * ((pixel00 >> L_RED_SHIFT) & 0xff);
            v00g = area00 * ((pixel00 >> L_GREEN_SHIFT) & 0xff);
            v00b = area00 * ((pixel00 >> L_BLUE_SHIFT) & 0xff);
            v10r = area10 * ((pixel10 >> L_RED_SHIFT) & 0xff);
            v10g = area10 * ((pixel10 >> L_GREEN_SHIFT) & 0xff);
            v10b = area10 * ((pixel10 >> L_BLUE_SHIFT) & 0xff);
            v01r = area01 * ((pixel01 >> L_RED_SHIFT) & 0xff);
            v01g = area01 * ((pixel01 >> L_GREEN_SHIFT) & 0xff);
            v01b = area01 * ((pixel01 >> L_BLUE_SHIFT) & 0xff);
            v11r = area11 * ((pixel11 >> L_RED_SHIFT) & 0xff);
            v11g = area11 * ((pixel11 >> L_GREEN_SHIFT) & 0xff);
            v11b = area11 * ((pixel11 >> L_BLUE_SHIFT) & 0xff);
            vinr = ving = vinb = 0;
            for (k = 1; k < dely; k++) {  /* for full src pixels */
                for (m = 1; m < delx; m++) {
                    pixel = *(lines + k * wpls + xup + m);
                    vinr += 256 * ((pixel >> L_RED_SHIFT) & 0xff);
                    ving += 256 * ((pixel >> L_GREEN_SHIFT) & 0xff);
                    vinb += 256 * ((pixel >> L_BLUE_SHIFT) & 0xff);
                }
            }
            vmidr = vmidg = vmidb = 0;
            areal = (16 - xuf) * 16;
            arear = xlf * 16;
            areat = 16 * (16 - yuf);
            areab = 16 * ylf;
            for (k = 1; k < dely; k++) {  /* for left side */
                pixel = *(lines + k * wpls + xup);
                vmidr += areal * ((pixel >> L_RED_SHIFT) & 0xff);
                vmidg += areal * ((pixel >> L_GREEN_SHIFT) & 0xff);
                vmidb += areal * ((pixel >> L_BLUE_SHIFT) & 0xff);
            }
            for (k = 1; k < dely; k++) {  /* for right side */
                pixel = *(lines + k * wpls + xlp);
                vmidr += arear * ((pixel >> L_RED_SHIFT) & 0xff);
                vmidg += arear * ((pixel >> L_GREEN_SHIFT) & 0xff);
                vmidb += arear * ((pixel >> L_BLUE_SHIFT) & 0xff);
            }
            for (m = 1; m < delx; m++) {  /* for top side */
                pixel = *(lines + xup + m);
                vmidr += areat * ((pixel >> L_RED_SHIFT) & 0xff);
                vmidg += areat * ((pixel >> L_GREEN_SHIFT) & 0xff);
                vmidb += areat * ((pixel >> L_BLUE_SHIFT) & 0xff);
            }
            for (m = 1; m < delx; m++) {  /* for bottom side */
                pixel = *(lines + dely * wpls + xup + m);
                vmidr += areab * ((pixel >> L_RED_SHIFT) & 0xff);
                vmidg += areab * ((pixel >> L_GREEN_SHIFT) & 0xff);
                vmidb += areab * ((pixel >> L_BLUE_SHIFT) & 0xff);
            }

                /* Sum all the contributions */
            rval = (v00r + v01r + v10r + v11r + vinr + vmidr + 128) / area;
            gval = (v00g + v01g + v10g + v11g + ving + vmidg + 128) / area;
            bval = (v00b + v01b + v10b + v11b + vinb + vmidb + 128) / area;
#if  DEBUG_OVERFLOW
            if (rval > 255) lept_stderr("rval ovfl: %d\n", rval);
            if (gval > 255) lept_stderr("gval ovfl: %d\n", gval);
            if (bval > 255) lept_stderr("bval ovfl: %d\n", bval);
#endif  /* DEBUG_OVERFLOW */
            composeRGBPixel(rval, gval, bval, lined + j);
        }
    }
}

void area_averaging_image_scale(uint32_t *dst, int dst_width, int dst_height, const uint32_t *src, int src_width, int src_height)
{
    scaleColorAreaMapLow(dst, dst_width, dst_height, dst_width, src, src_width, src_height, src_width);
}
