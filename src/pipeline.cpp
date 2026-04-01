#include "pipeline.h"
#include "isp.h"

#include <cstdio>
#include <cstring>
#include <chrono>
#include <algorithm>
#include <vector>
#include <jpeglib.h>

static uint8_t clamp_u8(int value) {
    return static_cast<uint8_t>(std::clamp(value, 0, 255));
}

static void downsample_bayer_same_phase(const uint8_t* in, uint8_t* out,
                                        int width, int height, int downsample) {
    int out_w = width / downsample;
    int out_h = height / downsample;
    int samples_per_pixel = (downsample * downsample) / 4;

    for (int oy = 0; oy < out_h; oy++) {
        int base_y = oy * downsample;
        for (int ox = 0; ox < out_w; ox++) {
            int base_x = ox * downsample;
            int sum = 0;

            for (int dy = (oy & 1); dy < downsample; dy += 2) {
                const uint8_t* row = in + (base_y + dy) * width;
                for (int dx = (ox & 1); dx < downsample; dx += 2)
                    sum += row[base_x + dx];
            }

            out[oy * out_w + ox] = clamp_u8((sum + samples_per_pixel / 2) / samples_per_pixel);
        }
    }
}

static void upsample_rgb_2x(const uint8_t* in, uint8_t* out, int width, int height) {
    int out_w = width * 2;
    for (int y = 0; y < height; y++) {
        const uint8_t* src = in + y * width * 3;
        uint8_t* dst0 = out + (y * 2) * out_w * 3;
        uint8_t* dst1 = dst0 + out_w * 3;

        for (int x = 0; x < width; x++) {
            const uint8_t* px = src + x * 3;
            for (int rx = 0; rx < 2; rx++) {
                uint8_t* p0 = dst0 + (x * 2 + rx) * 3;
                uint8_t* p1 = dst1 + (x * 2 + rx) * 3;
                p0[0] = p1[0] = px[0];
                p0[1] = p1[1] = px[1];
                p0[2] = p1[2] = px[2];
            }
        }
    }
}

Pipeline::Pipeline(Camera& camera, MjpegStream& stream)
    : camera_(camera), stream_(stream) {}

Pipeline::~Pipeline() { stop(); }

void Pipeline::set_config(const PipelineConfig& cfg) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    config_ = cfg;
}

PipelineConfig Pipeline::get_config() const {
    std::lock_guard<std::mutex> lock(config_mutex_);
    return config_;
}

void Pipeline::set_analysis_callback(AnalysisCallback cb, int interval) {
    analysis_cb_ = std::move(cb);
    analysis_interval_ = interval;
}

bool Pipeline::start() {
    if (running_) return false;
    if (!camera_.is_streaming()) {
        if (!camera_.start_streaming())
            return false;
    }
#if HAS_GPU_ISP
    if (!gpu_isp_.is_available()) {
        if (gpu_isp_.init())
            printf("Pipeline: GPU ISP available\n");
        else
            printf("Pipeline: GPU ISP not available, using CPU\n");
    }
    gpu_configured_ = false;
#endif
    running_ = true;
    thread_ = std::thread(&Pipeline::capture_loop, this);
    if (analysis_cb_)
        analysis_thread_ = std::thread(&Pipeline::analysis_loop, this);
    return true;
}

void Pipeline::stop() {
    running_ = false;
    analysis_cv_.notify_all();
    if (thread_.joinable())
        thread_.join();
    if (analysis_thread_.joinable())
        analysis_thread_.join();
}

void Pipeline::analysis_loop() {
    std::vector<uint8_t> local_bayer;
    int w, h;
    BayerPattern pat;

    while (running_) {
        {
            std::unique_lock<std::mutex> lock(analysis_mutex_);
            analysis_cv_.wait(lock, [&]{ return analysis_ready_ || !running_; });
            if (!running_) break;
            local_bayer.resize(analysis_bayer_buf_.size());
            std::memcpy(local_bayer.data(), analysis_bayer_buf_.data(), analysis_bayer_buf_.size());
            w = analysis_width_;
            h = analysis_height_;
            pat = analysis_pattern_;
            analysis_ready_ = false;
        }

        AnalysisFrame af;
        af.bayer = local_bayer.data();
        af.width = w;
        af.height = h;
        af.pattern = pat;
        analysis_cb_(af);
    }
}

void Pipeline::encode_jpeg(const uint8_t* rgb, int width, int height,
                            int quality, std::vector<uint8_t>& out) {
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);

    unsigned char* buf = nullptr;
    unsigned long buf_size = 0;
    jpeg_mem_dest(&cinfo, &buf, &buf_size);

    cinfo.image_width = width;
    cinfo.image_height = height;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);

    jpeg_start_compress(&cinfo, TRUE);

    // Batch scanlines for better throughput
    constexpr int BATCH = 8;
    JSAMPROW rows[BATCH];
    while (cinfo.next_scanline < cinfo.image_height) {
        int remaining = cinfo.image_height - cinfo.next_scanline;
        int n = std::min(remaining, BATCH);
        for (int i = 0; i < n; i++)
            rows[i] = const_cast<uint8_t*>(rgb + (cinfo.next_scanline + i) * width * 3);
        jpeg_write_scanlines(&cinfo, rows, n);
    }
    jpeg_finish_compress(&cinfo);

    out.assign(buf, buf + buf_size);
    free(buf);
    jpeg_destroy_compress(&cinfo);
}

void Pipeline::capture_loop() {
    printf("Pipeline capture loop started\n");

    int frame_count = 0;
    auto fps_start = std::chrono::steady_clock::now();
    int fps_frames = 0;

    // Per-stage timing accumulators
    using Clock = std::chrono::steady_clock;
    float acc_unpack = 0, acc_demosaic = 0, acc_jpeg = 0;
    int timing_frames = 0;

    // Pre-allocated buffers — reused across frames to avoid per-frame heap allocation
    std::vector<uint8_t> bayer;
    std::vector<uint8_t> rgb;
    std::vector<uint8_t> jpeg;
    std::vector<uint8_t> collapsed_bayer;
    std::vector<uint8_t> collapsed_rgb;

    while (running_) {
        auto cfg = get_config();
        auto* cam_cfg = camera_.active_config();
        if (!cam_cfg) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // Dequeue frame from V4L2
        Camera::Frame raw_frame;
        if (!camera_.dequeue_frame(raw_frame)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        frame_count++;
        // Skip first frame (known to be empty)
        if (frame_count == 1) {
            camera_.release_frame(raw_frame);
            continue;
        }

        int ds = cfg.downsample;
        int out_w = cam_cfg->width / ds;
        int out_h = cam_cfg->height / ds;

        auto t0 = Clock::now();

        bool used_gpu = false;
#if HAS_GPU_ISP
        // GPU fast path: handles standard (non-quad-Bayer) sensors
        if (gpu_isp_.is_available() && cam_cfg->cfa_block != 2) {
            if (!gpu_configured_ ||
                gpu_isp_.output_width() != cam_cfg->width / ds ||
                gpu_isp_.output_height() != cam_cfg->height / ds) {
                gpu_configured_ = gpu_isp_.configure(
                    cam_cfg->width, cam_cfg->height, cam_cfg->stride,
                    ds, cam_cfg->bayer);
            }

            if (gpu_configured_) {
                int out_sz = (cam_cfg->width / ds) * (cam_cfg->height / ds);
                rgb.resize(out_sz * 3);

                used_gpu = gpu_isp_.process(
                    raw_frame.data, raw_frame.length,
                    cfg.r_gain, cfg.b_gain,
                    rgb.data());

                if (used_gpu) {
                    // Analysis: need bayer data for AWB/AE/AF (CPU unpack a subset)
                    if (analysis_cb_ && (frame_count % analysis_interval_ == 0)) {
                        bayer.resize(out_w * out_h);
                        isp_unpack_wb(raw_frame.data, bayer.data(),
                                      cam_cfg->width, cam_cfg->height, cam_cfg->stride,
                                      ds, cfg.r_gain, cfg.b_gain, cam_cfg->bayer);
                        std::lock_guard<std::mutex> lock(analysis_mutex_);
                        size_t sz = out_w * out_h;
                        analysis_bayer_buf_.resize(sz);
                        std::memcpy(analysis_bayer_buf_.data(), bayer.data(), sz);
                        analysis_width_ = out_w;
                        analysis_height_ = out_h;
                        analysis_pattern_ = cam_cfg->bayer;
                        analysis_ready_ = true;
                        analysis_cv_.notify_one();
                    }
                    camera_.release_frame(raw_frame);
                }
            }
        }
#endif

        if (!used_gpu && cam_cfg->cfa_block == 2) {
            // IMX371 behaves like a 2x2 same-color mosaic. First collapse it to a
            // conventional Bayer grid at half linear resolution, then apply any
            // additional downsampling on that regular Bayer image.
            int collapsed_w = cam_cfg->width / 2;
            int collapsed_h = cam_cfg->height / 2;
            collapsed_bayer.resize(collapsed_w * collapsed_h);

            isp_unpack_wb(raw_frame.data, collapsed_bayer.data(),
                          cam_cfg->width, cam_cfg->height, cam_cfg->stride,
                          2, cfg.r_gain, cfg.b_gain, cam_cfg->bayer, &isp_pool_);

            const uint8_t* analysis_bayer = collapsed_bayer.data();
            int analysis_w = collapsed_w;
            int analysis_h = collapsed_h;

            if (ds == 1) {
                collapsed_rgb.resize(collapsed_w * collapsed_h * 3);
                rgb.resize(out_w * out_h * 3);

                if (analysis_cb_ && (frame_count % analysis_interval_ == 0)) {
                    std::lock_guard<std::mutex> lock(analysis_mutex_);
                    size_t sz = analysis_w * analysis_h;
                    analysis_bayer_buf_.resize(sz);
                    std::memcpy(analysis_bayer_buf_.data(), analysis_bayer, sz);
                    analysis_width_ = analysis_w;
                    analysis_height_ = analysis_h;
                    analysis_pattern_ = cam_cfg->bayer;
                    analysis_ready_ = true;
                    analysis_cv_.notify_one();
                }

                camera_.release_frame(raw_frame);

                isp_demosaic(collapsed_bayer.data(), collapsed_rgb.data(),
                             collapsed_w, collapsed_h, cam_cfg->bayer, &isp_pool_);
                upsample_rgb_2x(collapsed_rgb.data(), rgb.data(), collapsed_w, collapsed_h);
            } else {
                if (ds == 2) {
                    bayer.resize(collapsed_bayer.size());
                    std::memcpy(bayer.data(), collapsed_bayer.data(), collapsed_bayer.size());
                    analysis_bayer = bayer.data();
                } else {
                    bayer.resize(out_w * out_h);
                    downsample_bayer_same_phase(collapsed_bayer.data(), bayer.data(),
                                                collapsed_w, collapsed_h, 2);
                    analysis_bayer = bayer.data();
                    analysis_w = out_w;
                    analysis_h = out_h;
                }

                rgb.resize(out_w * out_h * 3);

                if (analysis_cb_ && (frame_count % analysis_interval_ == 0)) {
                    std::lock_guard<std::mutex> lock(analysis_mutex_);
                    size_t sz = analysis_w * analysis_h;
                    analysis_bayer_buf_.resize(sz);
                    std::memcpy(analysis_bayer_buf_.data(), analysis_bayer, sz);
                    analysis_width_ = analysis_w;
                    analysis_height_ = analysis_h;
                    analysis_pattern_ = cam_cfg->bayer;
                    analysis_ready_ = true;
                    analysis_cv_.notify_one();
                }

                camera_.release_frame(raw_frame);

                isp_demosaic(bayer.data(), rgb.data(), out_w, out_h, cam_cfg->bayer, &isp_pool_);
            }
        } else if (!used_gpu) {
            bayer.resize(out_w * out_h);
            rgb.resize(out_w * out_h * 3);

            // Unpack MIPI 10-bit + apply WB gains + downsample
            isp_unpack_wb(raw_frame.data, bayer.data(),
                          cam_cfg->width, cam_cfg->height, cam_cfg->stride,
                          ds, cfg.r_gain, cfg.b_gain, cam_cfg->bayer, &isp_pool_);

            // Analysis callback (AWB/AE/AF) — async handoff
            if (analysis_cb_ && (frame_count % analysis_interval_ == 0)) {
                std::lock_guard<std::mutex> lock(analysis_mutex_);
                size_t sz = out_w * out_h;
                analysis_bayer_buf_.resize(sz);
                std::memcpy(analysis_bayer_buf_.data(), bayer.data(), sz);
                analysis_width_ = out_w;
                analysis_height_ = out_h;
                analysis_pattern_ = cam_cfg->bayer;
                analysis_ready_ = true;
                analysis_cv_.notify_one();
            }

            camera_.release_frame(raw_frame);

            // Demosaic
            isp_demosaic(bayer.data(), rgb.data(), out_w, out_h, cam_cfg->bayer, &isp_pool_);
        }

        auto t1 = Clock::now(); // end of ISP (unpack+demosaic)

        // JPEG encode
        encode_jpeg(rgb.data(), out_w, out_h, cfg.jpeg_quality, jpeg);
        last_jpeg_size_ = jpeg.size();

        auto t2 = Clock::now(); // end of JPEG

        // Push to MJPEG stream
        stream_.push_frame(jpeg.data(), jpeg.size());

        // Per-stage timing accumulation
        auto ms = [](auto a, auto b) {
            return std::chrono::duration<float, std::milli>(b - a).count();
        };
        acc_unpack += ms(t0, t1);    // ISP total (unpack + demosaic)
        acc_jpeg += ms(t1, t2);      // JPEG encode
        timing_frames++;

        // FPS calculation
        fps_frames++;
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration<float>(now - fps_start).count();
        if (elapsed >= 1.0f) {
            fps_ = fps_frames / elapsed;
            if (timing_frames > 0) {
                time_unpack_ms_ = acc_unpack / timing_frames;
                time_jpeg_ms_ = acc_jpeg / timing_frames;
                // Total frame time = elapsed / frames
                float total_ms = elapsed * 1000.0f / timing_frames;
                float other_ms = total_ms - time_unpack_ms_ - time_jpeg_ms_;
                printf("  ISP: %.1fms  JPEG: %.1fms  Wait: %.1fms  (%.1f fps)\n",
                       time_unpack_ms_.load(), time_jpeg_ms_.load(), other_ms, fps_.load());
            }
            fps_frames = 0;
            fps_start = now;
            acc_unpack = acc_demosaic = acc_jpeg = 0;
            timing_frames = 0;
        }

        // Frame rate limiting
        if (cfg.target_fps > 0) {
            float frame_time = 1.0f / cfg.target_fps;
            auto frame_end = std::chrono::steady_clock::now();
            auto frame_elapsed = std::chrono::duration<float>(frame_end - now).count();
            if (frame_elapsed < frame_time) {
                int sleep_ms = (int)((frame_time - frame_elapsed) * 1000);
                if (sleep_ms > 0)
                    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
            }
        }
    }

    printf("Pipeline capture loop ended\n");
}
