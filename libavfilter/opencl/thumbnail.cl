/*
 * Copyright (c) 2020 Wookhyun Han <wookhyunhan@gmail.com>
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

const sampler_t sampler = (CLK_NORMALIZED_COORDS_FALSE |
                           CLK_FILTER_NEAREST);

__kernel void Thumbnail_uchar(__read_only image2d_t src,
                              __global int *histogram,
                              int offset) {
    int2 loc = (int2)(get_global_id(0), get_global_id(1));

    if (loc.x < get_image_width(src) && loc.y < get_image_height(src)) {
        uchar pixel = (uchar)(255 * read_imagef(src, sampler, loc).x);
        atomic_add(&histogram[offset + pixel], 1);
    }
}

__kernel void Thumbnail_uchar2(__read_only image2d_t src,
                               __global int *histogram,
                               int offset) {
    int2 loc = (int2)(get_global_id(0), get_global_id(1));

    if (loc.x < get_image_width(src) && loc.y < get_image_height(src)) {
        uchar2 pixel = convert_uchar2(read_imagef(src, sampler, loc).xy * 255);
        atomic_add(&histogram[offset + pixel.x], 1);
        atomic_add(&histogram[offset + 256 + pixel.y], 1);
    }
}
