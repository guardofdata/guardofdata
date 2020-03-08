#include "precompiled.h"
#include <assert.h>

// ┌─────┬─────┬─────┬─────┬─────┬─────┬─────┐
// │     │     │     │     │     │     │     │
// │     │     │     │     │     │     │     │
// ├─────┴─────┴─┬───┴─────┴───┬─┴─────┴─────┤
// │             │             │             │
// │             │             │             │
// └─────────────┴─────────────┴─────────────┘

void area_averaging_image_scale(uint32_t *dst, int dst_width, int dst_height, const uint32_t *src, int src_width, int src_height)
{
    // 1. Scale horizontally (src -> mid)
    int mid_width  = dst_width,
        mid_height = src_height;
    float src_width_div_by_mid_width = float(src_width) / mid_width;
    float mid_width_div_by_src_width = 1.f / src_width_div_by_mid_width;
    std::vector<uint32_t> mid(mid_width * mid_height);
    for (int y=0; y<mid_height; y++)
        for (int x=0; x<mid_width; x++)
            for (int c=0; c<4; c++) {
                float f = x * src_width_div_by_mid_width;
                int i = int(f);
                float d = ((uint8_t*)&src[i + y*src_width])[c] * (float(i) + 1 - f);
                float end = f + src_width_div_by_mid_width;
                int endi = int(end);
                if (end - float(endi) > 1e-4f) {
                    assert(endi < src_width);
                    d += ((uint8_t*)&src[endi + y*src_width])[c] * (end - float(endi));
                }
                for (i++; i < endi; i++)
                    d += ((uint8_t*)&src[i + y*src_width])[c];
                int r = int(d * mid_width_div_by_src_width + 0.5f);
                assert(r <= 255);
                ((uint8_t*)&mid[x + y*mid_width])[c] = r;
            }

    // 2. Scale vertically (mid -> dst)
    float mid_height_div_by_dst_height = float(mid_height) / dst_height;
    float dst_height_div_by_mid_height = 1.f / mid_height_div_by_dst_height;
    for (int y=0; y<dst_height; y++)
        for (int x=0; x<dst_width; x++)
            for (int c=0; c<4; c++) {
                float f = y * mid_height_div_by_dst_height;
                int i = int(f);
                float d = ((uint8_t*)&mid[x + i*mid_width])[c] * (float(i) + 1 - f);
                float end = f + mid_height_div_by_dst_height;
                int endi = int(end);
                if (end - float(endi) > 1e-4f) {
                    assert(endi < mid_height);
                    d += ((uint8_t*)&mid[x + endi*mid_width])[c] * (end - float(endi));
                }
                for (i++; i < endi; i++)
                    d += ((uint8_t*)&mid[x + i*mid_width])[c];
                int r = int(d * dst_height_div_by_mid_height + 0.5f);
                assert(r <= 255);
                ((uint8_t*)&dst[x + y*dst_width])[c] = r;
            }
}
