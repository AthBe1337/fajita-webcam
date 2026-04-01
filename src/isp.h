#pragma once
#include <cstdint>
#include "camera.h"

class ThreadPool;

// MIPI 10-bit packed unpack + white balance + optional downsample
// Output: 8-bit bayer (same pattern, possibly smaller)
//
// downsample=1: full resolution output (width x height)
// downsample=2: half resolution (width/2 x height/2), 2x2 same-color average
// downsample=4: quarter resolution (width/4 x height/4), 4x4 same-color average
void isp_unpack_wb(const uint8_t* mipi_in, uint8_t* bayer_out,
                   int width, int height, int stride,
                   int downsample,
                   float r_gain, float b_gain,
                   BayerPattern pattern,
                   ThreadPool* pool = nullptr);

// Bayer demosaic to RGB888 (bilinear interpolation)
// Input: 8-bit bayer (width x height)
// Output: RGB888 (width x height x 3)
void isp_demosaic(const uint8_t* bayer, uint8_t* rgb,
                  int width, int height, BayerPattern pattern,
                  ThreadPool* pool = nullptr);

// Compute channel statistics from raw bayer for AWB/AE
struct ChannelStats {
    float r_mean, gr_mean, gb_mean, b_mean;
    float g_mean;  // average of gr and gb
    float brightness;  // overall green mean as brightness proxy
};

ChannelStats isp_compute_stats(const uint8_t* bayer,
                               int width, int height,
                               int roi_x, int roi_y,
                               int roi_w, int roi_h,
                               BayerPattern pattern);

// Compute focus metric (Laplacian variance on green channel)
float isp_focus_metric(const uint8_t* bayer,
                       int width, int height,
                       int roi_x, int roi_y,
                       int roi_w, int roi_h,
                       BayerPattern pattern);
