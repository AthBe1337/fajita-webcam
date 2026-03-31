#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <functional>

enum class BayerPattern { RGGB, BGGR };

struct ParamRange {
    int min, max, def;
};

struct CameraConfig {
    std::string name;
    std::string sensor_entity;
    std::string csiphy;
    int width, height, stride;
    uint32_t mbus_code;
    uint32_t v4l2_pixfmt;
    BayerPattern bayer;
    ParamRange exposure;
    ParamRange analogue_gain;
    ParamRange digital_gain;
    bool has_af;
    std::string af_entity;
    int rotation;

    int frame_size() const { return stride * height; }
};

// Resolved runtime info for an active camera
struct CameraRuntime {
    int sensor_subdev_fd = -1;
    int af_subdev_fd = -1;
    std::string sensor_subdev_path;
    std::string af_subdev_path;
};

class Camera {
public:
    Camera();
    ~Camera();

    bool open(const char* media_dev = "/dev/media0",
              const char* video_dev = "/dev/video0");
    void close();

    const std::vector<CameraConfig>& configs() const { return configs_; }
    const CameraConfig* active_config() const { return active_; }
    int active_index() const { return active_idx_; }

    // Switch to a camera by index. Stops streaming if active.
    bool select(int index);

    // V4L2 controls
    bool set_exposure(int value);
    bool set_analogue_gain(int value);
    bool set_digital_gain(int value);
    bool set_focus(int value);  // only if has_af

    int get_exposure();
    int get_analogue_gain();
    int get_digital_gain();
    int get_focus();

    // Streaming
    bool start_streaming(int buffer_count = 4);
    void stop_streaming();
    bool is_streaming() const { return streaming_; }

    // Dequeue a frame buffer. Returns pointer to mmap'd data, or nullptr.
    // Caller must call release_buffer() when done.
    struct Frame {
        const uint8_t* data;
        size_t length;
        int index;
    };
    bool dequeue_frame(Frame& frame);
    void release_frame(const Frame& frame);

    int video_fd() const { return video_fd_; }

private:
    // Media device helpers
    struct EntityInfo {
        uint32_t id;
        std::string name;
        std::string devnode;  // e.g. /dev/v4l-subdev19
        uint32_t type;
        uint32_t pads;
    };

    bool enumerate_entities();
    EntityInfo* find_entity(const std::string& name);
    bool setup_link(const std::string& source, int src_pad,
                    const std::string& sink, int sink_pad, bool enable);
    bool set_subdev_format(const std::string& entity, int pad,
                           uint32_t mbus_code, int width, int height);
    bool set_video_format(int width, int height, uint32_t pixfmt);
    bool disconnect_all();
    bool activate_pipeline(const CameraConfig& cfg);

    int open_entity_subdev(const std::string& entity_name);
    bool set_v4l2_ctrl(int fd, uint32_t id, int value);
    int get_v4l2_ctrl(int fd, uint32_t id);

    int media_fd_ = -1;
    int video_fd_ = -1;
    std::vector<EntityInfo> entities_;
    std::vector<CameraConfig> configs_;
    const CameraConfig* active_ = nullptr;
    int active_idx_ = -1;
    CameraRuntime runtime_;
    bool streaming_ = false;

    // mmap buffers
    struct MmapBuffer {
        void* start = nullptr;
        size_t length = 0;
    };
    std::vector<MmapBuffer> buffers_;
};
