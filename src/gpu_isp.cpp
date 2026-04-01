#include "gpu_isp.h"

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <fcntl.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>
#include <gbm.h>

// ---------------------------------------------------------------------------
// Compute shader: MIPI 10-bit unpack + WB + downsample → Bayer (1 uint per pixel)
// ---------------------------------------------------------------------------
static const char* kUnpackShaderSource = R"glsl(#version 310 es
layout(local_size_x = 64) in;

layout(std430, binding = 0) readonly buffer RawBuf   { uint raw_data[]; };
layout(std430, binding = 1)          buffer BayerBuf { uint bayer_out[]; };

uniform int u_raw_w;
uniform int u_raw_h;
uniform int u_stride;
uniform int u_ds;
uniform int u_out_w;
uniform int u_out_h;
uniform float u_r_gain;
uniform float u_b_gain;
uniform int u_bayer;

int bayer_color(int x, int y) {
    int bx = x & 1;
    int by = y & 1;
    if (u_bayer == 0) { if (by == 0 && bx == 0) return 1; if (by == 1 && bx == 1) return 2; }
    else if (u_bayer == 1) { if (by == 0 && bx == 0) return 2; if (by == 1 && bx == 1) return 1; }
    else if (u_bayer == 2) { if (by == 0 && bx == 1) return 1; if (by == 1 && bx == 0) return 2; }
    else { if (by == 0 && bx == 1) return 2; if (by == 1 && bx == 0) return 1; }
    return 0;
}

uint read_raw_byte(int offset) {
    return (raw_data[offset >> 2] >> uint((offset & 3) * 8)) & 0xFFu;
}

void main() {
    int gid = int(gl_GlobalInvocationID.x);
    if (gid >= u_out_w * u_out_h) return;

    int ox = gid % u_out_w;
    int oy = gid / u_out_w;

    float val;

    if (u_ds == 1) {
        int group_start = oy * u_stride + (ox / 4) * 5;
        val = float(read_raw_byte(group_start + (ox & 3)));
    } else {
        int ds = u_ds;
        int samples = (ds * ds) / 4;
        int sum = 0;
        for (int dy = (oy & 1); dy < ds; dy += 2) {
            int iy = oy * ds + dy;
            int line_off = iy * u_stride;
            for (int dx = (ox & 1); dx < ds; dx += 2) {
                int ix = ox * ds + dx;
                int gs = line_off + (ix / 4) * 5;
                sum += int(read_raw_byte(gs + (ix & 3)));
            }
        }
        val = float(sum) / float(samples);
    }

    float gain = 1.0;
    int color = bayer_color(ox, oy);
    if (color == 1) gain = u_r_gain;
    else if (color == 2) gain = u_b_gain;

    bayer_out[gid] = uint(clamp(val * gain + 0.5, 0.0, 255.0));
}
)glsl";

// ---------------------------------------------------------------------------
// Compute shader: Bilinear demosaic Bayer → RGB (packed as 1 uint per pixel)
// ---------------------------------------------------------------------------
static const char* kDemosaicShaderSource = R"glsl(#version 310 es
layout(local_size_x = 64) in;

layout(std430, binding = 1) readonly buffer BayerBuf { uint bayer_in[]; };
layout(std430, binding = 2)          buffer RgbBuf   { uint rgb_out[]; };

uniform int u_width;
uniform int u_height;
uniform int u_bayer;

int bayer_color(int x, int y) {
    int bx = x & 1;
    int by = y & 1;
    if (u_bayer == 0) { if (by == 0 && bx == 0) return 1; if (by == 1 && bx == 1) return 2; }
    else if (u_bayer == 1) { if (by == 0 && bx == 0) return 2; if (by == 1 && bx == 1) return 1; }
    else if (u_bayer == 2) { if (by == 0 && bx == 1) return 1; if (by == 1 && bx == 0) return 2; }
    else { if (by == 0 && bx == 1) return 2; if (by == 1 && bx == 0) return 1; }
    return 0;
}

uint get(int x, int y) {
    x = clamp(x, 0, u_width - 1);
    y = clamp(y, 0, u_height - 1);
    return bayer_in[y * u_width + x];
}

void main() {
    int gid = int(gl_GlobalInvocationID.x);
    if (gid >= u_width * u_height) return;

    int x = gid % u_width;
    int y = gid / u_width;

    uint r, g, b;
    int color = bayer_color(x, y);

    if (color == 0) {
        g = get(x, y);
        int left_color = bayer_color(x - 1, y);
        if (left_color == 1) {
            r = (get(x-1, y) + get(x+1, y) + 1u) >> 1;
            b = (get(x, y-1) + get(x, y+1) + 1u) >> 1;
        } else {
            b = (get(x-1, y) + get(x+1, y) + 1u) >> 1;
            r = (get(x, y-1) + get(x, y+1) + 1u) >> 1;
        }
    } else if (color == 1) {
        r = get(x, y);
        g = (get(x-1,y) + get(x+1,y) + get(x,y-1) + get(x,y+1) + 2u) >> 2;
        b = (get(x-1,y-1) + get(x+1,y-1) + get(x-1,y+1) + get(x+1,y+1) + 2u) >> 2;
    } else {
        b = get(x, y);
        g = (get(x-1,y) + get(x+1,y) + get(x,y-1) + get(x,y+1) + 2u) >> 2;
        r = (get(x-1,y-1) + get(x+1,y-1) + get(x-1,y+1) + get(x+1,y+1) + 2u) >> 2;
    }

    // Pack RGB into 3 consecutive bytes via the uint array.
    // Output layout: sequential R,G,B bytes packed into uint32.
    int byte_off = gid * 3;
    int word = byte_off >> 2;
    int lane = byte_off & 3;

    // Each pixel writes 3 bytes that may span 2 uint32 words.
    // Since gid is unique per pixel, and 3 bytes from different pixels never
    // overlap the same word at the same byte lane, we can use atomicOr safely.
    uint val0 = (r << uint(lane * 8));
    if (lane + 1 < 4) val0 |= (g << uint((lane + 1) * 8));
    if (lane + 2 < 4) val0 |= (b << uint((lane + 2) * 8));
    atomicOr(rgb_out[word], val0);

    if (lane + 1 >= 4)
        atomicOr(rgb_out[word + 1], g << uint((lane + 1 - 4) * 8));
    if (lane + 2 >= 4)
        atomicOr(rgb_out[word + 1], b << uint((lane + 2 - 4) * 8));
}
)glsl";

// ---------------------------------------------------------------------------
// GpuIsp implementation
// ---------------------------------------------------------------------------

GpuIsp::GpuIsp() {}

GpuIsp::~GpuIsp() { shutdown(); }

bool GpuIsp::init() {
    // Temporarily disabled - freedreno has issues with SSBO readback after compute
    // CPU path with NEON + multi-threading is fast enough for current sensor frame rates
    return false;
#if 0
    drm_fd_ = open("/dev/dri/renderD128", O_RDWR);
    if (drm_fd_ < 0) {
        perror("GpuIsp: open renderD128");
        return false;
    }

    auto* gbm = gbm_create_device(drm_fd_);
    if (!gbm) {
        fprintf(stderr, "GpuIsp: gbm_create_device failed\n");
        close(drm_fd_); drm_fd_ = -1;
        return false;
    }
    gbm_device_ = gbm;

    EGLDisplay dpy = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm, nullptr);
    if (dpy == EGL_NO_DISPLAY) {
        fprintf(stderr, "GpuIsp: eglGetPlatformDisplay failed\n");
        shutdown(); return false;
    }
    egl_display_ = dpy;

    EGLint major, minor;
    if (!eglInitialize(dpy, &major, &minor)) {
        fprintf(stderr, "GpuIsp: eglInitialize failed\n");
        shutdown(); return false;
    }

    eglBindAPI(EGL_OPENGL_ES_API);

    EGLint cfg_attr[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE };
    EGLConfig config; EGLint num;
    eglChooseConfig(dpy, cfg_attr, &config, 1, &num);
    if (num == 0) { fprintf(stderr, "GpuIsp: no EGL config\n"); shutdown(); return false; }

    EGLint ctx_attr[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE
    };
    EGLContext ctx = eglCreateContext(dpy, config, EGL_NO_CONTEXT, ctx_attr);
    if (ctx == EGL_NO_CONTEXT) {
        fprintf(stderr, "GpuIsp: GLES 3.1 context failed\n");
        shutdown(); return false;
    }
    egl_context_ = ctx;

    if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
        fprintf(stderr, "GpuIsp: eglMakeCurrent failed\n");
        shutdown(); return false;
    }

    printf("GpuIsp: initialized (%s)\n", glGetString(GL_RENDERER));
    available_ = true;
    return true;
#endif
}

void GpuIsp::shutdown() {
    EGLDisplay dpy = (EGLDisplay)egl_display_;

    if (unpack_program_) { glDeleteProgram(unpack_program_); unpack_program_ = 0; }
    if (demosaic_program_) { glDeleteProgram(demosaic_program_); demosaic_program_ = 0; }
    if (raw_ssbo_) { glDeleteBuffers(1, &raw_ssbo_); raw_ssbo_ = 0; }
    if (bayer_ssbo_) { glDeleteBuffers(1, &bayer_ssbo_); bayer_ssbo_ = 0; }
    if (rgb_ssbo_) { glDeleteBuffers(1, &rgb_ssbo_); rgb_ssbo_ = 0; }
    if (read_buf_) { glDeleteBuffers(1, &read_buf_); read_buf_ = 0; }

    if (egl_context_ && dpy) {
        eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(dpy, (EGLContext)egl_context_);
        egl_context_ = nullptr;
    }
    if (dpy) { eglTerminate(dpy); egl_display_ = nullptr; }
    if (gbm_device_) { gbm_device_destroy((struct gbm_device*)gbm_device_); gbm_device_ = nullptr; }
    if (drm_fd_ >= 0) { close(drm_fd_); drm_fd_ = -1; }

    available_ = false;
    configured_ = false;
}

bool GpuIsp::compile_shader(unsigned int& shader, const char* source) {
    shader = glCreateShader(GL_COMPUTE_SHADER);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint ok;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &len);
        if (len > 0) {
            std::vector<char> log(len + 1, 0);
            glGetShaderInfoLog(shader, len, nullptr, log.data());
            fprintf(stderr, "GpuIsp: shader compile error:\n%s\n", log.data());
        } else {
            fprintf(stderr, "GpuIsp: shader compile failed (no log)\n");
        }
        glDeleteShader(shader);
        shader = 0;
        return false;
    }
    return true;
}

bool GpuIsp::create_program(unsigned int& program, unsigned int shader) {
    program = glCreateProgram();
    glAttachShader(program, shader);
    glLinkProgram(program);

    GLint ok;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &len);
        if (len > 0) {
            std::vector<char> log(len + 1, 0);
            glGetProgramInfoLog(program, len, nullptr, log.data());
            fprintf(stderr, "GpuIsp: program link error:\n%s\n", log.data());
        }
        glDeleteProgram(program);
        program = 0;
        return false;
    }
    glDeleteShader(shader);
    return true;
}

bool GpuIsp::configure(int raw_width, int raw_height, int stride,
                       int downsample, BayerPattern pattern) {
    if (!available_) return false;

    raw_w_ = raw_width;
    raw_h_ = raw_height;
    stride_ = stride;
    downsample_ = downsample;
    pattern_ = pattern;
    out_w_ = raw_width / downsample;
    out_h_ = raw_height / downsample;

    // Compile shaders
    if (unpack_program_) { glDeleteProgram(unpack_program_); unpack_program_ = 0; }
    if (demosaic_program_) { glDeleteProgram(demosaic_program_); demosaic_program_ = 0; }

    GLuint sh;
    if (!compile_shader(sh, kUnpackShaderSource)) return false;
    if (!create_program(unpack_program_, sh)) return false;

    if (!compile_shader(sh, kDemosaicShaderSource)) return false;
    if (!create_program(demosaic_program_, sh)) return false;

    // Allocate SSBOs
    raw_buf_size_ = stride * raw_height;
    bayer_buf_size_ = (size_t)out_w_ * out_h_ * 4;  // 1 uint per pixel
    rgb_buf_size_ = (size_t)((out_w_ * out_h_ * 3 + 3) & ~3);  // packed RGB bytes

    auto alloc_ssbo = [](GLuint& buf, size_t size, GLenum usage) {
        if (buf) glDeleteBuffers(1, &buf);
        glGenBuffers(1, &buf);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, buf);
        glBufferData(GL_SHADER_STORAGE_BUFFER, size, nullptr, usage);
    };

    alloc_ssbo(raw_ssbo_, raw_buf_size_, GL_STREAM_DRAW);     // CPU → GPU
    alloc_ssbo(bayer_ssbo_, bayer_buf_size_, GL_DYNAMIC_COPY);  // GPU ↔ GPU (zeroed from CPU)
    alloc_ssbo(rgb_ssbo_, rgb_buf_size_, GL_STREAM_READ);      // GPU → CPU

    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        fprintf(stderr, "GpuIsp: GL error during configure: 0x%x\n", err);
        return false;
    }

    configured_ = true;
    printf("GpuIsp: configured %dx%d ds=%d -> %dx%d\n",
           raw_w_, raw_h_, downsample_, out_w_, out_h_);
    return true;
}

bool GpuIsp::process(const uint8_t* raw_data, size_t raw_size,
                     float r_gain, float b_gain,
                     uint8_t* rgb_out) {
    if (!available_ || !configured_) return false;

    int bayer_code = 0;
    switch (pattern_) {
    case BayerPattern::RGGB: bayer_code = 0; break;
    case BayerPattern::BGGR: bayer_code = 1; break;
    case BayerPattern::GRBG: bayer_code = 2; break;
    case BayerPattern::GBRG: bayer_code = 3; break;
    }

    // Upload raw data
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, raw_ssbo_);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                    std::min(raw_size, raw_buf_size_), raw_data);

    // Re-upload zeroed bayer and rgb (atomicOr needs zeroed buffers)
    // Use glBufferData to orphan+reallocate, which is faster than glBufferSubData
    // for full-buffer updates and avoids GPU stalls
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, bayer_ssbo_);
    glBufferData(GL_SHADER_STORAGE_BUFFER, bayer_buf_size_, nullptr, GL_DYNAMIC_COPY);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, rgb_ssbo_);
    glBufferData(GL_SHADER_STORAGE_BUFFER, rgb_buf_size_, nullptr, GL_STREAM_READ);

    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        fprintf(stderr, "GpuIsp: upload error: 0x%x\n", err);
        return false;
    }

    int total_pixels = out_w_ * out_h_;
    int groups = (total_pixels + 63) / 64;

    // --- Pass 1: Unpack ---
    glUseProgram(unpack_program_);
    glUniform1i(glGetUniformLocation(unpack_program_, "u_raw_w"), raw_w_);
    glUniform1i(glGetUniformLocation(unpack_program_, "u_raw_h"), raw_h_);
    glUniform1i(glGetUniformLocation(unpack_program_, "u_stride"), stride_);
    glUniform1i(glGetUniformLocation(unpack_program_, "u_ds"), downsample_);
    glUniform1i(glGetUniformLocation(unpack_program_, "u_out_w"), out_w_);
    glUniform1i(glGetUniformLocation(unpack_program_, "u_out_h"), out_h_);
    glUniform1f(glGetUniformLocation(unpack_program_, "u_r_gain"), r_gain);
    glUniform1f(glGetUniformLocation(unpack_program_, "u_b_gain"), b_gain);
    glUniform1i(glGetUniformLocation(unpack_program_, "u_bayer"), bayer_code);

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, raw_ssbo_);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, bayer_ssbo_);

    glDispatchCompute(groups, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    // --- Pass 2: Demosaic ---
    glUseProgram(demosaic_program_);
    glUniform1i(glGetUniformLocation(demosaic_program_, "u_width"), out_w_);
    glUniform1i(glGetUniformLocation(demosaic_program_, "u_height"), out_h_);
    glUniform1i(glGetUniformLocation(demosaic_program_, "u_bayer"), bayer_code);

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, bayer_ssbo_);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, rgb_ssbo_);

    glDispatchCompute(groups, 1, 1);

    // Ensure all writes complete, then unbind indexed SSBO slots before mapping
    glFinish();
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, 0);

    // Read back: use a separate readback buffer to work around freedreno mapping issues
    // after compute shader writes
    if (read_buf_ == 0) {
        glGenBuffers(1, &read_buf_);
    }
    glBindBuffer(GL_COPY_WRITE_BUFFER, read_buf_);
    glBufferData(GL_COPY_WRITE_BUFFER, rgb_buf_size_, nullptr, GL_STREAM_READ);

    // Copy from SSBO to readback buffer
    glBindBuffer(GL_COPY_READ_BUFFER, rgb_ssbo_);
    glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0, rgb_buf_size_);
    glFinish();

    // Map the readback buffer
    glBindBuffer(GL_COPY_READ_BUFFER, read_buf_);
    size_t rgb_bytes = (size_t)out_w_ * out_h_ * 3;
    void* mapped = glMapBufferRange(GL_COPY_READ_BUFFER, 0, rgb_bytes, GL_MAP_READ_BIT);
    if (!mapped) {
        fprintf(stderr, "GpuIsp: map failed after copy\n");
        return false;
    }
    memcpy(rgb_out, mapped, rgb_bytes);
    glUnmapBuffer(GL_COPY_READ_BUFFER);

    return true;
}
