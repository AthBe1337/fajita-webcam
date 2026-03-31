#include "isp.h"
#include <algorithm>
#include <cmath>
#include <cstring>

#ifdef __aarch64__
#include <arm_neon.h>
#define HAS_NEON 1
#else
#define HAS_NEON 0
#endif

// Determine which Bayer channel a pixel belongs to
// Returns 0=R, 1=Gr, 2=Gb, 3=B for RGGB
// For BGGR: 0=B, 1=Gb, 2=Gr, 3=R (we remap so R/B gains apply correctly)
static inline bool is_red(int x, int y, BayerPattern pat) {
    switch (pat) {
    case BayerPattern::RGGB: return (x % 2 == 0) && (y % 2 == 0);
    case BayerPattern::BGGR: return (x % 2 == 1) && (y % 2 == 1);
    }
    return false;
}

static inline bool is_blue(int x, int y, BayerPattern pat) {
    switch (pat) {
    case BayerPattern::RGGB: return (x % 2 == 1) && (y % 2 == 1);
    case BayerPattern::BGGR: return (x % 2 == 0) && (y % 2 == 0);
    }
    return false;
}

static inline uint8_t clamp8(float v) {
    return (uint8_t)std::clamp((int)(v + 0.5f), 0, 255);
}

void isp_unpack_wb(const uint8_t* mipi_in, uint8_t* bayer_out,
                   int width, int height, int stride,
                   int downsample,
                   float r_gain, float b_gain,
                   BayerPattern pattern) {
    if (downsample == 1) {
        // Full resolution: unpack each line, apply WB
        for (int y = 0; y < height; y++) {
            const uint8_t* line = mipi_in + y * stride;
            uint8_t* out = bayer_out + y * width;

            int x = 0;
#if HAS_NEON
            // Process 16 pixels (20 bytes) at a time with NEON
            for (; x + 15 < width; x += 16) {
                int byte_off = x * 5 / 4;
                // Load 20 bytes = 16 pixels of 10-bit packed
                // Layout: [P0_hi, P1_hi, P2_hi, P3_hi, P0123_lo, P4_hi, ...]
                // Each group of 5 bytes has 4 pixels

                // Process 4 groups of 4 pixels
                uint8_t pixels[16];
                for (int g = 0; g < 4; g++) {
                    int off = byte_off + g * 5;
                    pixels[g*4 + 0] = line[off + 0];
                    pixels[g*4 + 1] = line[off + 1];
                    pixels[g*4 + 2] = line[off + 2];
                    pixels[g*4 + 3] = line[off + 3];
                    // Byte 4 has the low 2 bits - we discard them (8-bit output)
                }

                // Apply WB gains based on bayer position
                for (int i = 0; i < 16; i++) {
                    int px = x + i, py = y;
                    float gain = 1.0f;
                    if (is_red(px, py, pattern)) gain = r_gain;
                    else if (is_blue(px, py, pattern)) gain = b_gain;
                    out[x + i] = clamp8(pixels[i] * gain);
                }
            }
#endif
            // Scalar fallback
            for (; x + 3 < width; x += 4) {
                int off = x * 5 / 4;
                uint8_t p0 = line[off + 0];
                uint8_t p1 = line[off + 1];
                uint8_t p2 = line[off + 2];
                uint8_t p3 = line[off + 3];

                // Apply WB
                for (int i = 0; i < 4; i++) {
                    uint8_t p = (i == 0) ? p0 : (i == 1) ? p1 : (i == 2) ? p2 : p3;
                    float gain = 1.0f;
                    if (is_red(x+i, y, pattern)) gain = r_gain;
                    else if (is_blue(x+i, y, pattern)) gain = b_gain;
                    out[x + i] = clamp8(p * gain);
                }
            }
        }
    } else {
        // Downsample in raw Bayer space while preserving the CFA phase.
        // Averaging every pixel in a ds x ds block destroys the Bayer mosaic and
        // produces a near-greyscale image after demosaic. Instead, each output
        // pixel averages only samples from the matching Bayer phase.
        int ds = downsample;
        int out_w = width / ds;
        int out_h = height / ds;
        int samples_per_pixel = (ds * ds) / 4;

        std::vector<uint8_t> line_buf(width);
        std::vector<int> accum(out_w);

        for (int oy = 0; oy < out_h; oy++) {
            std::fill(accum.begin(), accum.end(), 0);

            // Because ds is even, every output block starts at an even/even input
            // coordinate. The output Bayer phase is therefore defined by ox/oy parity.
            for (int dy = (oy & 1); dy < ds; dy += 2) {
                int iy = oy * ds + dy;
                const uint8_t* line = mipi_in + iy * stride;

                for (int x = 0; x + 3 < width; x += 4) {
                    int off = x * 5 / 4;
                    line_buf[x + 0] = line[off + 0];
                    line_buf[x + 1] = line[off + 1];
                    line_buf[x + 2] = line[off + 2];
                    line_buf[x + 3] = line[off + 3];
                }

                for (int ox = 0; ox < out_w; ox++) {
                    int block_x = ox * ds;
                    for (int dx = (ox & 1); dx < ds; dx += 2)
                        accum[ox] += line_buf[block_x + dx];
                }
            }

            float scale = 1.0f / samples_per_pixel;
            uint8_t* out = bayer_out + oy * out_w;
            for (int ox = 0; ox < out_w; ox++) {
                float gain = 1.0f;
                if (is_red(ox, oy, pattern)) gain = r_gain;
                else if (is_blue(ox, oy, pattern)) gain = b_gain;
                out[ox] = clamp8(accum[ox] * scale * gain);
            }
        }
    }
}

void isp_demosaic(const uint8_t* bayer, uint8_t* rgb,
                  int width, int height, BayerPattern pattern) {
    // Simple bilinear interpolation demosaicing
    // For each pixel, interpolate missing color channels from neighbors

    // Offsets for RGGB: R at (0,0), Gr at (1,0), Gb at (0,1), B at (1,1)
    // For BGGR: B at (0,0), Gb at (1,0), Gr at (0,1), R at (1,1)
    int r_dx, r_dy, b_dx, b_dy;
    if (pattern == BayerPattern::RGGB) {
        r_dx = 0; r_dy = 0; b_dx = 1; b_dy = 1;
    } else { // BGGR
        b_dx = 0; b_dy = 0; r_dx = 1; r_dy = 1;
    }

    auto get = [&](int x, int y) -> uint8_t {
        x = std::clamp(x, 0, width - 1);
        y = std::clamp(y, 0, height - 1);
        return bayer[y * width + x];
    };

    for (int y = 0; y < height; y++) {
        uint8_t* out = rgb + y * width * 3;
        for (int x = 0; x < width; x++) {
            int bx = x % 2, by = y % 2;
            float r, g, b;

            if (bx == r_dx && by == r_dy) {
                // Red pixel
                r = get(x, y);
                g = (get(x-1, y) + get(x+1, y) + get(x, y-1) + get(x, y+1)) * 0.25f;
                b = (get(x-1, y-1) + get(x+1, y-1) + get(x-1, y+1) + get(x+1, y+1)) * 0.25f;
            } else if (bx == b_dx && by == b_dy) {
                // Blue pixel
                b = get(x, y);
                g = (get(x-1, y) + get(x+1, y) + get(x, y-1) + get(x, y+1)) * 0.25f;
                r = (get(x-1, y-1) + get(x+1, y-1) + get(x-1, y+1) + get(x+1, y+1)) * 0.25f;
            } else if (by == r_dy) {
                // Green pixel on red row (Gr)
                g = get(x, y);
                r = (get(x-1, y) + get(x+1, y)) * 0.5f;
                b = (get(x, y-1) + get(x, y+1)) * 0.5f;
            } else {
                // Green pixel on blue row (Gb)
                g = get(x, y);
                b = (get(x-1, y) + get(x+1, y)) * 0.5f;
                r = (get(x, y-1) + get(x, y+1)) * 0.5f;
            }

            out[x * 3 + 0] = clamp8(r);
            out[x * 3 + 1] = clamp8(g);
            out[x * 3 + 2] = clamp8(b);
        }
    }
}

ChannelStats isp_compute_stats(const uint8_t* bayer,
                               int width, int height,
                               int roi_x, int roi_y,
                               int roi_w, int roi_h,
                               BayerPattern pattern) {
    // Ensure ROI is within bounds and even-aligned
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

            if (pattern == BayerPattern::RGGB) {
                r_sum += p00; gr_sum += p10; gb_sum += p01; b_sum += p11;
            } else {
                b_sum += p00; gb_sum += p10; gr_sum += p01; r_sum += p11;
            }
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
    // Compute Laplacian variance on green channel
    // Extract green pixels (every other pixel in a checkerboard)
    roi_x = std::clamp(roi_x & ~1, 2, width - roi_w - 2);
    roi_y = std::clamp(roi_y & ~1, 2, height - roi_h - 2);
    roi_w = std::min(roi_w & ~1, width - roi_x - 2);
    roi_h = std::min(roi_h & ~1, height - roi_y - 2);

    // Green pixels for RGGB: (1,0), (0,1) positions in each 2x2 block
    // Green pixels for BGGR: (0,0) and (1,1) are B and R, so green at (1,0) and (0,1)
    // Actually for both patterns, green is at odd-parity positions

    double sum = 0, sum_sq = 0;
    int count = 0;

    // Use green channel: sample every 2nd row, green column
    for (int y = roi_y + 2; y < roi_y + roi_h - 2; y++) {
        // Determine which x positions are green on this row
        int green_offset;
        if (pattern == BayerPattern::RGGB)
            green_offset = (y % 2 == 0) ? 1 : 0;  // Gr at x=odd on even rows
        else
            green_offset = (y % 2 == 0) ? 1 : 0;  // Gb at x=odd on even rows

        for (int x = roi_x + 2 + green_offset; x < roi_x + roi_w - 2; x += 2) {
            // Laplacian: use green neighbors (skip 2 pixels to stay on green)
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
