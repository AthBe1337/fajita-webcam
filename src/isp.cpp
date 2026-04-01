#include "isp.h"
#include "threadpool.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#ifdef __aarch64__
#include <arm_neon.h>
#define HAS_NEON 1
#else
#define HAS_NEON 0
#endif

enum class BayerSite { R, Gr, Gb, B };

static inline BayerSite bayer_site(int x, int y, BayerPattern pat) {
    bool odd_x = (x & 1) != 0;
    bool odd_y = (y & 1) != 0;

    switch (pat) {
    case BayerPattern::RGGB:
        if (!odd_y) return odd_x ? BayerSite::Gr : BayerSite::R;
        return odd_x ? BayerSite::B : BayerSite::Gb;
    case BayerPattern::BGGR:
        if (!odd_y) return odd_x ? BayerSite::Gb : BayerSite::B;
        return odd_x ? BayerSite::R : BayerSite::Gr;
    case BayerPattern::GRBG:
        if (!odd_y) return odd_x ? BayerSite::R : BayerSite::Gr;
        return odd_x ? BayerSite::Gb : BayerSite::B;
    case BayerPattern::GBRG:
        if (!odd_y) return odd_x ? BayerSite::B : BayerSite::Gb;
        return odd_x ? BayerSite::Gr : BayerSite::R;
    }
    return BayerSite::R;
}

static inline bool is_red(int x, int y, BayerPattern pat) {
    return bayer_site(x, y, pat) == BayerSite::R;
}

static inline bool is_blue(int x, int y, BayerPattern pat) {
    return bayer_site(x, y, pat) == BayerSite::B;
}

static inline uint8_t clamp8(int v) {
    return (uint8_t)std::clamp(v, 0, 255);
}

// ---------------------------------------------------------------------------
// MIPI 10-bit unpack: extract 4 pixels (high 8 bits) from 5 packed bytes
// ---------------------------------------------------------------------------

static inline void unpack_mipi10_line(const uint8_t* __restrict line,
                                      uint8_t* __restrict out, int width) {
    int x = 0;
#if HAS_NEON
    // Process 16 pixels (4 groups of 5 bytes = 20 bytes) per iteration.
    // Each group: [P0_hi8, P1_hi8, P2_hi8, P3_hi8, low_bits] → take first 4.
    for (; x + 15 < width; x += 16) {
        int off = (x >> 2) * 5;  // x/4 * 5
        // Load 4 groups of 5 bytes, extract the 4 high-byte pixels from each
        uint8_t tmp[16];
        const uint8_t* p = line + off;
        tmp[0]  = p[0];  tmp[1]  = p[1];  tmp[2]  = p[2];  tmp[3]  = p[3];
        tmp[4]  = p[5];  tmp[5]  = p[6];  tmp[6]  = p[7];  tmp[7]  = p[8];
        tmp[8]  = p[10]; tmp[9]  = p[11]; tmp[10] = p[12]; tmp[11] = p[13];
        tmp[12] = p[15]; tmp[13] = p[16]; tmp[14] = p[17]; tmp[15] = p[18];
        vst1q_u8(out + x, vld1q_u8(tmp));
    }
#endif
    // Scalar fallback
    for (; x + 3 < width; x += 4) {
        int off = (x >> 2) * 5;
        out[x + 0] = line[off + 0];
        out[x + 1] = line[off + 1];
        out[x + 2] = line[off + 2];
        out[x + 3] = line[off + 3];
    }
}

// ---------------------------------------------------------------------------
// Apply WB gains to a row of unpacked 8-bit bayer.
// gain_even/gain_odd are the WB multipliers for even/odd pixel columns on
// this particular row (pre-computed from the Bayer pattern).
// Uses fixed-point: gain * 128, then >> 7.
// ---------------------------------------------------------------------------

static inline void apply_wb_row(uint8_t* __restrict row, int width,
                                int gain_even_fp, int gain_odd_fp) {
    int x = 0;
#if HAS_NEON
    // Process 16 pixels at a time using fixed-point multiply
    // Even pixels at indices 0,2,4,... get gain_even
    // Odd pixels at indices 1,3,5,... get gain_odd
    int16x8_t vge = vdupq_n_s16((int16_t)gain_even_fp);
    int16x8_t vgo = vdupq_n_s16((int16_t)gain_odd_fp);

    for (; x + 15 < width; x += 16) {
        uint8x16_t pixels = vld1q_u8(row + x);

        // Deinterleave even/odd
        uint8x8x2_t pairs = vuzp_u8(vget_low_u8(pixels), vget_high_u8(pixels));
        // pairs.val[0] = even indices (0,2,4,...), pairs.val[1] = odd indices

        // Widen to 16-bit and multiply
        uint16x8_t even16 = vmovl_u8(pairs.val[0]);
        uint16x8_t odd16  = vmovl_u8(pairs.val[1]);

        // Fixed-point multiply: (pixel * gain_fp + 64) >> 7
        int16x8_t even_result = vqshrn_high_n_s32(
            vqshrn_n_s32(vmull_s16(vget_low_s16(vreinterpretq_s16_u16(even16)), vget_low_s16(vge)), 7),
            vmull_s16(vget_high_s16(vreinterpretq_s16_u16(even16)), vget_high_s16(vge)), 7);
        int16x8_t odd_result = vqshrn_high_n_s32(
            vqshrn_n_s32(vmull_s16(vget_low_s16(vreinterpretq_s16_u16(odd16)), vget_low_s16(vgo)), 7),
            vmull_s16(vget_high_s16(vreinterpretq_s16_u16(odd16)), vget_high_s16(vgo)), 7);

        // Narrow to u8 with saturation
        uint8x8_t even8 = vqmovun_s16(even_result);
        uint8x8_t odd8  = vqmovun_s16(odd_result);

        // Re-interleave
        uint8x8x2_t zipped = vzip_u8(even8, odd8);
        vst1q_u8(row + x, vcombine_u8(zipped.val[0], zipped.val[1]));
    }
#endif
    for (; x + 1 < width; x += 2) {
        row[x + 0] = clamp8((row[x + 0] * gain_even_fp + 64) >> 7);
        row[x + 1] = clamp8((row[x + 1] * gain_odd_fp + 64) >> 7);
    }
}

// Compute fixed-point (x128) WB gains for even and odd columns on a given row
static inline void wb_gains_for_row(int y, BayerPattern pattern,
                                    float r_gain, float b_gain,
                                    int& gain_even_fp, int& gain_odd_fp) {
    BayerSite site_even = bayer_site(0, y, pattern);
    BayerSite site_odd  = bayer_site(1, y, pattern);

    auto to_fp = [](BayerSite s, float rg, float bg) -> int {
        float g = 1.0f;
        if (s == BayerSite::R) g = rg;
        else if (s == BayerSite::B) g = bg;
        return (int)(g * 128.0f + 0.5f);
    };
    gain_even_fp = to_fp(site_even, r_gain, b_gain);
    gain_odd_fp  = to_fp(site_odd, r_gain, b_gain);
}

// ---------------------------------------------------------------------------
// isp_unpack_wb: MIPI 10-bit → 8-bit Bayer with WB and optional downsample
// ---------------------------------------------------------------------------

// Process a range of output rows [oy_start, oy_end) for the downsample path
static void unpack_wb_ds_rows(const uint8_t* mipi_in, uint8_t* bayer_out,
                              int width, int stride, int ds,
                              float r_gain, float b_gain,
                              BayerPattern pattern,
                              int out_w,
                              int oy_start, int oy_end) {
    int samples_per_pixel = (ds * ds) / 4;
    float inv_samples = 1.0f / samples_per_pixel;

    // Per-thread local buffers
    std::vector<uint8_t> line_buf(width);
    std::vector<int> accum(out_w);

    for (int oy = oy_start; oy < oy_end; oy++) {
        std::fill(accum.begin(), accum.end(), 0);

        for (int dy = (oy & 1); dy < ds; dy += 2) {
            int iy = oy * ds + dy;
            const uint8_t* line = mipi_in + iy * stride;

            // Unpack MIPI 10-bit to 8-bit
            unpack_mipi10_line(line, line_buf.data(), width);

            // Accumulate matching Bayer phase samples
#if HAS_NEON
            if (ds == 4) {
                // Optimised path for ds=4: each output pixel averages 2 input
                // pixels from this line, spaced 2 apart within a 4-pixel block.
                // Even output pixels sample line_buf[block+0] and line_buf[block+2]
                // Odd output pixels sample line_buf[block+1] and line_buf[block+3]
                int ox = 0;
                for (; ox + 7 < out_w; ox += 8) {
                    // Load 32 input pixels = 8 blocks of 4
                    int bx = ox * 4;
                    uint8x16_t a = vld1q_u8(line_buf.data() + bx);
                    uint8x16_t b = vld1q_u8(line_buf.data() + bx + 16);
                    // Deinterleave: pairs.val[0] has even-idx, val[1] has odd-idx
                    uint8x16x2_t pairs = vuzpq_u8(a, b);
                    // Now pairs.val[0][i] = pixel at even positions, val[1][i] = odd
                    // For block j: even_sum = pairs.val[0][2j] + pairs.val[0][2j+1]
                    //              odd_sum  = pairs.val[1][2j] + pairs.val[1][2j+1]
                    uint16x8_t even_wide = vpaddlq_u8(pairs.val[0]);
                    uint16x8_t odd_wide  = vpaddlq_u8(pairs.val[1]);
                    // Interleave back: accum[ox+0] += even, accum[ox+1] += odd, ...
                    int32x4_t acc0 = vld1q_s32(accum.data() + ox);
                    int32x4_t acc1 = vld1q_s32(accum.data() + ox + 4);
                    // Widen and interleave even/odd sums
                    uint16x8x2_t interleaved = vzipq_u16(even_wide, odd_wide);
                    acc0 = vaddq_s32(acc0, vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(interleaved.val[0]))));
                    acc1 = vaddq_s32(acc1, vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(interleaved.val[1]))));
                    vst1q_s32(accum.data() + ox, acc0);
                    vst1q_s32(accum.data() + ox + 4, acc1);
                }
                for (; ox < out_w; ox++) {
                    int block_x = ox * 4;
                    for (int dx = (ox & 1); dx < 4; dx += 2)
                        accum[ox] += line_buf[block_x + dx];
                }
            } else
#endif
            {
                for (int ox = 0; ox < out_w; ox++) {
                    int block_x = ox * ds;
                    for (int dx = (ox & 1); dx < ds; dx += 2)
                        accum[ox] += line_buf[block_x + dx];
                }
            }
        }

        // Apply WB gain and write output
        int gain_even_fp, gain_odd_fp;
        wb_gains_for_row(oy, pattern, r_gain, b_gain, gain_even_fp, gain_odd_fp);

        uint8_t* out = bayer_out + oy * out_w;
        int ox = 0;
#if HAS_NEON
        int16x8_t vge = vdupq_n_s16((int16_t)gain_even_fp);
        int16x8_t vgo = vdupq_n_s16((int16_t)gain_odd_fp);
        float32x4_t vinv = vdupq_n_f32(inv_samples);

        for (; ox + 7 < out_w; ox += 8) {
            // Load 8 accum values, convert to float, multiply by inv_samples
            int32x4_t a0 = vld1q_s32(accum.data() + ox);
            int32x4_t a1 = vld1q_s32(accum.data() + ox + 4);
            float32x4_t f0 = vmulq_f32(vcvtq_f32_s32(a0), vinv);
            float32x4_t f1 = vmulq_f32(vcvtq_f32_s32(a1), vinv);
            // Convert back to int16
            int16x4_t i0 = vmovn_s32(vcvtq_s32_f32(f0));
            int16x4_t i1 = vmovn_s32(vcvtq_s32_f32(f1));
            int16x8_t vals = vcombine_s16(i0, i1);

            // Apply WB: interleave even/odd gains
            // Even indices get gain_even, odd get gain_odd
            // Deinterleave vals into even/odd
            int16x4x2_t deint = vuzp_s16(vget_low_s16(vals), vget_high_s16(vals));
            int32x4_t re = vmull_s16(deint.val[0], vget_low_s16(vge));
            int32x4_t ro = vmull_s16(deint.val[1], vget_low_s16(vgo));
            int16x4_t se = vqshrn_n_s32(re, 7);
            int16x4_t so = vqshrn_n_s32(ro, 7);
            // Re-interleave
            int16x4x2_t zipped = vzip_s16(se, so);
            int16x8_t result = vcombine_s16(zipped.val[0], zipped.val[1]);
            uint8x8_t out8 = vqmovun_s16(result);
            vst1_u8(out + ox, out8);
        }
#endif
        for (; ox < out_w; ox++) {
            int val = (int)(accum[ox] * inv_samples + 0.5f);
            int gain = (ox & 1) ? gain_odd_fp : gain_even_fp;
            out[ox] = clamp8((val * gain + 64) >> 7);
        }
    }
}

// Process a range of rows [y_start, y_end) for full-resolution unpack
static void unpack_wb_fullres_rows(const uint8_t* mipi_in, uint8_t* bayer_out,
                                   int width, int stride,
                                   float r_gain, float b_gain,
                                   BayerPattern pattern,
                                   int y_start, int y_end) {
    for (int y = y_start; y < y_end; y++) {
        const uint8_t* line = mipi_in + y * stride;
        uint8_t* out = bayer_out + y * width;

        unpack_mipi10_line(line, out, width);

        int gain_even_fp, gain_odd_fp;
        wb_gains_for_row(y, pattern, r_gain, b_gain, gain_even_fp, gain_odd_fp);
        apply_wb_row(out, width, gain_even_fp, gain_odd_fp);
    }
}

void isp_unpack_wb(const uint8_t* mipi_in, uint8_t* bayer_out,
                   int width, int height, int stride,
                   int downsample,
                   float r_gain, float b_gain,
                   BayerPattern pattern,
                   ThreadPool* pool) {
    if (downsample == 1) {
        if (pool) {
            pool->parallel_for(height, [&](int y_start, int y_end) {
                unpack_wb_fullres_rows(mipi_in, bayer_out, width, stride,
                                       r_gain, b_gain, pattern, y_start, y_end);
            });
        } else {
            unpack_wb_fullres_rows(mipi_in, bayer_out, width, stride,
                                   r_gain, b_gain, pattern, 0, height);
        }
    } else {
        int out_w = width / downsample;
        int out_h = height / downsample;

        if (pool) {
            // Split output rows across threads. Ensure even alignment for Bayer.
            int n = pool->num_threads();
            int chunk = ((out_h + n - 1) / n + 1) & ~1;  // round up to even
            pool->run([&](int tid) {
                int start = tid * chunk;
                int end = std::min(start + chunk, out_h);
                if (start < out_h)
                    unpack_wb_ds_rows(mipi_in, bayer_out, width, stride,
                                      downsample, r_gain, b_gain, pattern,
                                      out_w, start, end);
            });
        } else {
            unpack_wb_ds_rows(mipi_in, bayer_out, width, stride,
                              downsample, r_gain, b_gain, pattern,
                              out_w, 0, out_h);
        }
    }
}

// ---------------------------------------------------------------------------
// Demosaic
// ---------------------------------------------------------------------------

// Process interior 2x2 tiles for rows [y_start, y_end), y must be even and >= 2
static void demosaic_interior_rows(const uint8_t* bayer, uint8_t* rgb,
                                   int width, int /*height*/,
                                   int r_dx, int r_dy, int b_dx, int b_dy,
                                   int y_start, int y_end) {
    for (int y = y_start; y < y_end; y += 2) {
        for (int x = 2; x < width - 2; x += 2) {
            // Red site
            {
                int rx = x + r_dx, ry = y + r_dy;
                const uint8_t* rm = bayer + (ry - 1) * width;
                const uint8_t* r0 = bayer + ry * width;
                const uint8_t* rp = bayer + (ry + 1) * width;
                uint8_t* o = rgb + (ry * width + rx) * 3;
                o[0] = r0[rx];
                o[1] = (uint8_t)(((int)r0[rx-1] + r0[rx+1] + rm[rx] + rp[rx] + 2) >> 2);
                o[2] = (uint8_t)(((int)rm[rx-1] + rm[rx+1] + rp[rx-1] + rp[rx+1] + 2) >> 2);
            }
            // Blue site
            {
                int bx = x + b_dx, by = y + b_dy;
                const uint8_t* bm = bayer + (by - 1) * width;
                const uint8_t* b0 = bayer + by * width;
                const uint8_t* bp = bayer + (by + 1) * width;
                uint8_t* o = rgb + (by * width + bx) * 3;
                o[2] = b0[bx];
                o[1] = (uint8_t)(((int)b0[bx-1] + b0[bx+1] + bm[bx] + bp[bx] + 2) >> 2);
                o[0] = (uint8_t)(((int)bm[bx-1] + bm[bx+1] + bp[bx-1] + bp[bx+1] + 2) >> 2);
            }
            // Gr site (green on red row)
            {
                int gx = x + (1 - r_dx), gy = y + r_dy;
                const uint8_t* gm = bayer + (gy - 1) * width;
                const uint8_t* g0 = bayer + gy * width;
                const uint8_t* gp = bayer + (gy + 1) * width;
                uint8_t* o = rgb + (gy * width + gx) * 3;
                o[1] = g0[gx];
                o[0] = (uint8_t)(((int)g0[gx-1] + g0[gx+1] + 1) >> 1);
                o[2] = (uint8_t)(((int)gm[gx] + gp[gx] + 1) >> 1);
            }
            // Gb site (green on blue row)
            {
                int gx = x + (1 - b_dx), gy = y + b_dy;
                const uint8_t* gm = bayer + (gy - 1) * width;
                const uint8_t* g0 = bayer + gy * width;
                const uint8_t* gp = bayer + (gy + 1) * width;
                uint8_t* o = rgb + (gy * width + gx) * 3;
                o[1] = g0[gx];
                o[2] = (uint8_t)(((int)g0[gx-1] + g0[gx+1] + 1) >> 1);
                o[0] = (uint8_t)(((int)gm[gx] + gp[gx] + 1) >> 1);
            }
        }
    }
}

void isp_demosaic(const uint8_t* bayer, uint8_t* rgb,
                  int width, int height, BayerPattern pattern,
                  ThreadPool* pool) {
    BayerSite s00 = bayer_site(0, 0, pattern);
    BayerSite s10 = bayer_site(1, 0, pattern);
    BayerSite s01 = bayer_site(0, 1, pattern);

    int r_dx, r_dy, b_dx, b_dy;
    if      (s00 == BayerSite::R) { r_dx = 0; r_dy = 0; }
    else if (s10 == BayerSite::R) { r_dx = 1; r_dy = 0; }
    else if (s01 == BayerSite::R) { r_dx = 0; r_dy = 1; }
    else                          { r_dx = 1; r_dy = 1; }

    if      (s00 == BayerSite::B) { b_dx = 0; b_dy = 0; }
    else if (s10 == BayerSite::B) { b_dx = 1; b_dy = 0; }
    else if (s01 == BayerSite::B) { b_dx = 0; b_dy = 1; }
    else                          { b_dx = 1; b_dy = 1; }

    // Interior: rows [2, height-2) in steps of 2
    int interior_start = 2;
    int interior_end = height - 2;

    if (pool && interior_end > interior_start + 4) {
        int n = pool->num_threads();
        int total_rows = (interior_end - interior_start) / 2;
        int chunk = ((total_rows + n - 1) / n);

        pool->run([&](int tid) {
            int row_start = interior_start + tid * chunk * 2;
            int row_end = std::min(row_start + chunk * 2, interior_end);
            if (row_start < interior_end)
                demosaic_interior_rows(bayer, rgb, width, height,
                                       r_dx, r_dy, b_dx, b_dy,
                                       row_start, row_end);
        });
    } else {
        demosaic_interior_rows(bayer, rgb, width, height,
                               r_dx, r_dy, b_dx, b_dy,
                               interior_start, interior_end);
    }

    // Border pixels: use clamped access
    auto get = [&](int x, int y) -> uint8_t {
        x = std::clamp(x, 0, width - 1);
        y = std::clamp(y, 0, height - 1);
        return bayer[y * width + x];
    };

    auto do_pixel = [&](int x, int y) {
        uint8_t* out = rgb + (y * width + x) * 3;
        int r, g, b;
        switch (bayer_site(x, y, pattern)) {
        case BayerSite::R:
            r = get(x, y);
            g = ((int)get(x-1,y) + get(x+1,y) + get(x,y-1) + get(x,y+1) + 2) >> 2;
            b = ((int)get(x-1,y-1) + get(x+1,y-1) + get(x-1,y+1) + get(x+1,y+1) + 2) >> 2;
            break;
        case BayerSite::B:
            b = get(x, y);
            g = ((int)get(x-1,y) + get(x+1,y) + get(x,y-1) + get(x,y+1) + 2) >> 2;
            r = ((int)get(x-1,y-1) + get(x+1,y-1) + get(x-1,y+1) + get(x+1,y+1) + 2) >> 2;
            break;
        case BayerSite::Gr:
            g = get(x, y);
            r = ((int)get(x-1,y) + get(x+1,y) + 1) >> 1;
            b = ((int)get(x,y-1) + get(x,y+1) + 1) >> 1;
            break;
        case BayerSite::Gb:
            g = get(x, y);
            b = ((int)get(x-1,y) + get(x+1,y) + 1) >> 1;
            r = ((int)get(x,y-1) + get(x,y+1) + 1) >> 1;
            break;
        }
        out[0] = (uint8_t)r;
        out[1] = (uint8_t)g;
        out[2] = (uint8_t)b;
    };

    for (int y = 0; y < 2 && y < height; y++)
        for (int x = 0; x < width; x++) do_pixel(x, y);
    for (int y = std::max(2, height - 2); y < height; y++)
        for (int x = 0; x < width; x++) do_pixel(x, y);
    for (int y = 2; y < height - 2; y++) {
        for (int x = 0; x < 2; x++) do_pixel(x, y);
        for (int x = std::max(2, width - 2); x < width; x++) do_pixel(x, y);
    }
}

// ---------------------------------------------------------------------------
// Stats and focus metric (unchanged)
// ---------------------------------------------------------------------------

ChannelStats isp_compute_stats(const uint8_t* bayer,
                               int width, int height,
                               int roi_x, int roi_y,
                               int roi_w, int roi_h,
                               BayerPattern pattern) {
    roi_x = std::clamp(roi_x & ~1, 0, width - 2);
    roi_y = std::clamp(roi_y & ~1, 0, height - 2);
    roi_w = std::min(roi_w & ~1, width - roi_x);
    roi_h = std::min(roi_h & ~1, height - roi_y);

    double r_sum = 0, gr_sum = 0, gb_sum = 0, b_sum = 0;
    int count = 0;

    for (int y = roi_y; y < roi_y + roi_h; y += 2) {
        for (int x = roi_x; x < roi_x + roi_w; x += 2) {
            uint8_t p00 = bayer[y * width + x];
            uint8_t p10 = bayer[y * width + x + 1];
            uint8_t p01 = bayer[(y+1) * width + x];
            uint8_t p11 = bayer[(y+1) * width + x + 1];

            auto accumulate = [&](uint8_t value, int px, int py) {
                switch (bayer_site(px, py, pattern)) {
                case BayerSite::R: r_sum += value; break;
                case BayerSite::Gr: gr_sum += value; break;
                case BayerSite::Gb: gb_sum += value; break;
                case BayerSite::B: b_sum += value; break;
                }
            };

            accumulate(p00, x, y);
            accumulate(p10, x + 1, y);
            accumulate(p01, x, y + 1);
            accumulate(p11, x + 1, y + 1);
            count++;
        }
    }

    ChannelStats stats{};
    if (count > 0) {
        stats.r_mean = (float)(r_sum / count);
        stats.gr_mean = (float)(gr_sum / count);
        stats.gb_mean = (float)(gb_sum / count);
        stats.b_mean = (float)(b_sum / count);
        stats.g_mean = (stats.gr_mean + stats.gb_mean) * 0.5f;
        stats.brightness = stats.g_mean;
    }
    return stats;
}

float isp_focus_metric(const uint8_t* bayer,
                       int width, int height,
                       int roi_x, int roi_y,
                       int roi_w, int roi_h,
                       BayerPattern pattern) {
    roi_x = std::clamp(roi_x & ~1, 2, width - roi_w - 2);
    roi_y = std::clamp(roi_y & ~1, 2, height - roi_h - 2);
    roi_w = std::min(roi_w & ~1, width - roi_x - 2);
    roi_h = std::min(roi_h & ~1, height - roi_y - 2);

    double sum = 0, sum_sq = 0;
    int count = 0;

    for (int y = roi_y + 2; y < roi_y + roi_h - 2; y++) {
        int green_offset;
        if (pattern == BayerPattern::RGGB)
            green_offset = (y % 2 == 0) ? 1 : 0;
        else
            green_offset = (y % 2 == 0) ? 1 : 0;

        for (int x = roi_x + 2 + green_offset; x < roi_x + roi_w - 2; x += 2) {
            float center = bayer[y * width + x];
            float lap = 4.0f * center
                - bayer[(y-2) * width + x]
                - bayer[(y+2) * width + x]
                - bayer[y * width + x - 2]
                - bayer[y * width + x + 2];
            sum += lap;
            sum_sq += lap * lap;
            count++;
        }
    }

    if (count < 2) return 0;
    double mean = sum / count;
    return (float)(sum_sq / count - mean * mean);
}
