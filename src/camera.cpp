#include "camera.h"

#include <cstdio>
#include <cstring>
#include <cerrno>
#include <climits>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/media.h>
#include <linux/videodev2.h>
#include <linux/v4l2-subdev.h>
#include <linux/v4l2-mediabus.h>

// MIPI packed 10-bit Bayer pixel formats
#ifndef V4L2_PIX_FMT_SRGGB10P
#define V4L2_PIX_FMT_SRGGB10P v4l2_fourcc('p', 'R', 'A', 'A')
#endif
#ifndef V4L2_PIX_FMT_SBGGR10P
#define V4L2_PIX_FMT_SBGGR10P v4l2_fourcc('p', 'B', 'A', 'A')
#endif

static constexpr auto BUF_TYPE = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

static int xioctl(int fd, unsigned long req, void* arg) {
    int r;
    do { r = ioctl(fd, req, arg); } while (r == -1 && errno == EINTR);
    return r;
}

static const CameraConfig kDefaultConfigs[] = {
    {
        "imx519", "imx519 16-001a", "msm_csiphy0",
        4656, 3496, 5824,
        MEDIA_BUS_FMT_SRGGB10_1X10, V4L2_PIX_FMT_SRGGB10P,
        BayerPattern::RGGB, 1,
        {20, 6737, 1000}, {0, 960, 0}, {256, 65535, 256},
        true, "lc898217xc 16-0072", 270
    },
    {
        "imx376k", "imx376 17-0010", "msm_csiphy1",
        2592, 1940, 3248,
        MEDIA_BUS_FMT_SBGGR10_1X10, V4L2_PIX_FMT_SBGGR10P,
        BayerPattern::BGGR, 1,
        {4, 65515, 1600}, {0, 480, 0}, {0, 4096, 1024},
        true, "lc898217xc 17-0074", 270
    },
    {
        "imx371", "imx371 16-0010", "msm_csiphy2",
        4656, 3496, 5824,
        MEDIA_BUS_FMT_SBGGR10_1X10, V4L2_PIX_FMT_SBGGR10P,
        BayerPattern::BGGR, 2,
        {4, 65515, 1600}, {0, 480, 0}, {0, 4096, 1024},
        false, "", 90
    },
};

Camera::Camera() {
    for (auto& c : kDefaultConfigs)
        configs_.push_back(c);
}

Camera::~Camera() { close(); }

bool Camera::open(const char* media_dev, const char* video_dev) {
    media_fd_ = ::open(media_dev, O_RDWR);
    if (media_fd_ < 0) {
        perror("open media device");
        return false;
    }
    video_fd_ = ::open(video_dev, O_RDWR);
    if (video_fd_ < 0) {
        perror("open video device");
        ::close(media_fd_);
        media_fd_ = -1;
        return false;
    }
    if (!enumerate_entities()) {
        fprintf(stderr, "Failed to enumerate media entities\n");
        close();
        return false;
    }
    return true;
}

void Camera::close() {
    stop_streaming();
    if (runtime_.sensor_subdev_fd >= 0) ::close(runtime_.sensor_subdev_fd);
    if (runtime_.af_subdev_fd >= 0) ::close(runtime_.af_subdev_fd);
    runtime_ = {};
    if (video_fd_ >= 0) { ::close(video_fd_); video_fd_ = -1; }
    if (media_fd_ >= 0) { ::close(media_fd_); media_fd_ = -1; }
    entities_.clear();
    active_ = nullptr;
    active_idx_ = -1;
}

bool Camera::enumerate_entities() {
    entities_.clear();
    struct media_entity_desc desc;
    for (uint32_t id = 0; ; id = desc.id) {
        memset(&desc, 0, sizeof(desc));
        desc.id = id | MEDIA_ENT_ID_FLAG_NEXT;
        if (xioctl(media_fd_, MEDIA_IOC_ENUM_ENTITIES, &desc) < 0) {
            if (errno == EINVAL) break;
            perror("MEDIA_IOC_ENUM_ENTITIES");
            return false;
        }
        EntityInfo ei;
        ei.id = desc.id;
        ei.name = desc.name;
        ei.type = desc.type;
        ei.pads = desc.pads;

        if (desc.dev.major != 0) {
            char devpath[64];
            snprintf(devpath, sizeof(devpath), "/dev/char/%d:%d",
                     desc.dev.major, desc.dev.minor);
            char resolved[PATH_MAX];
            if (realpath(devpath, resolved))
                ei.devnode = resolved;
        }
        entities_.push_back(std::move(ei));
    }
    printf("Enumerated %zu media entities\n", entities_.size());
    return true;
}

Camera::EntityInfo* Camera::find_entity(const std::string& name) {
    for (auto& e : entities_)
        if (e.name == name) return &e;
    return nullptr;
}

int Camera::open_entity_subdev(const std::string& entity_name) {
    auto* ei = find_entity(entity_name);
    if (!ei) {
        fprintf(stderr, "Entity not found: %s\n", entity_name.c_str());
        return -1;
    }
    if (ei->devnode.empty()) {
        fprintf(stderr, "Entity %s has no device node\n", entity_name.c_str());
        return -1;
    }
    printf("Opening %s -> %s\n", entity_name.c_str(), ei->devnode.c_str());
    int fd = ::open(ei->devnode.c_str(), O_RDWR);
    if (fd < 0)
        fprintf(stderr, "Cannot open %s (%s): %s\n",
                entity_name.c_str(), ei->devnode.c_str(), strerror(errno));
    return fd;
}

bool Camera::setup_link(const std::string& source, int src_pad,
                        const std::string& sink, int sink_pad, bool enable) {
    auto* src_ent = find_entity(source);
    auto* snk_ent = find_entity(sink);
    if (!src_ent || !snk_ent) {
        fprintf(stderr, "setup_link: entity not found: %s -> %s\n",
                source.c_str(), sink.c_str());
        return false;
    }

    struct media_entity_desc desc;
    memset(&desc, 0, sizeof(desc));
    desc.id = src_ent->id;
    if (xioctl(media_fd_, MEDIA_IOC_ENUM_ENTITIES, &desc) < 0) return false;

    std::vector<media_pad_desc> pads(desc.pads);
    std::vector<media_link_desc> links(desc.links);
    struct media_links_enum lenum;
    memset(&lenum, 0, sizeof(lenum));
    lenum.entity = src_ent->id;
    lenum.pads = pads.data();
    lenum.links = links.data();
    if (xioctl(media_fd_, MEDIA_IOC_ENUM_LINKS, &lenum) < 0) {
        perror("MEDIA_IOC_ENUM_LINKS");
        return false;
    }

    for (auto& link : links) {
        if (link.source.entity == src_ent->id &&
            link.source.index == (uint16_t)src_pad &&
            link.sink.entity == snk_ent->id &&
            link.sink.index == (uint16_t)sink_pad) {

            if (link.flags & MEDIA_LNK_FL_IMMUTABLE)
                return true;

            struct media_link_desc setup = link;
            if (enable)
                setup.flags |= MEDIA_LNK_FL_ENABLED;
            else
                setup.flags &= ~MEDIA_LNK_FL_ENABLED;

            if (xioctl(media_fd_, MEDIA_IOC_SETUP_LINK, &setup) < 0) {
                fprintf(stderr, "setup_link %s:%d -> %s:%d [%s]: %s\n",
                        source.c_str(), src_pad, sink.c_str(), sink_pad,
                        enable ? "on" : "off", strerror(errno));
                return false;
            }
            printf("  Link %s:%d -> %s:%d [%s]\n",
                   source.c_str(), src_pad, sink.c_str(), sink_pad,
                   enable ? "enabled" : "disabled");
            return true;
        }
    }

    fprintf(stderr, "Link not found: %s:%d -> %s:%d\n",
            source.c_str(), src_pad, sink.c_str(), sink_pad);
    return false;
}

bool Camera::set_subdev_format(const std::string& entity, int pad,
                                uint32_t mbus_code, int width, int height) {
    int fd = open_entity_subdev(entity);
    if (fd < 0) return false;

    struct v4l2_subdev_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.which = V4L2_SUBDEV_FORMAT_ACTIVE;
    fmt.pad = pad;
    fmt.format.width = width;
    fmt.format.height = height;
    fmt.format.code = mbus_code;

    int ret = xioctl(fd, VIDIOC_SUBDEV_S_FMT, &fmt);
    ::close(fd);
    if (ret < 0) {
        fprintf(stderr, "set_subdev_format %s pad %d: %s\n",
                entity.c_str(), pad, strerror(errno));
        return false;
    }
    printf("  Format %s pad%d: %dx%d code=0x%04x\n",
           entity.c_str(), pad, fmt.format.width, fmt.format.height, fmt.format.code);
    return true;
}

bool Camera::set_video_format(int width, int height, uint32_t pixfmt) {
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = BUF_TYPE;
    fmt.fmt.pix_mp.width = width;
    fmt.fmt.pix_mp.height = height;
    fmt.fmt.pix_mp.pixelformat = pixfmt;
    fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    fmt.fmt.pix_mp.num_planes = 1;
    fmt.fmt.pix_mp.plane_fmt[0].sizeimage = 0;  // let driver compute
    fmt.fmt.pix_mp.plane_fmt[0].bytesperline = 0;

    if (xioctl(video_fd_, VIDIOC_S_FMT, &fmt) < 0) {
        perror("VIDIOC_S_FMT (MPLANE)");
        return false;
    }
    printf("  Video format: %dx%d planes=%d sizeimage=%d bytesperline=%d\n",
           fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height,
           fmt.fmt.pix_mp.num_planes,
           fmt.fmt.pix_mp.plane_fmt[0].sizeimage,
           fmt.fmt.pix_mp.plane_fmt[0].bytesperline);
    return true;
}

bool Camera::disconnect_all() {
    const char* csiphys[] = {"msm_csiphy0", "msm_csiphy1", "msm_csiphy2", "msm_csiphy3"};
    for (auto phy : csiphys)
        setup_link(phy, 1, "msm_csid0", 0, false);
    setup_link("msm_csid0", 1, "msm_vfe0_rdi0", 0, false);
    return true;
}

bool Camera::activate_pipeline(const CameraConfig& cfg) {
    printf("Activating pipeline for %s\n", cfg.name.c_str());

    // Enable links
    if (!setup_link(cfg.csiphy, 1, "msm_csid0", 0, true)) return false;
    if (!setup_link("msm_csid0", 1, "msm_vfe0_rdi0", 0, true)) return false;

    // Set formats on sensor
    if (!set_subdev_format(cfg.sensor_entity, 0,
                           cfg.mbus_code, cfg.width, cfg.height))
        return false;

    // Set formats on pipeline entities (both pads)
    const char* pipeline_entities[] = {cfg.csiphy.c_str(), "msm_csid0", "msm_vfe0_rdi0"};
    for (auto entity : pipeline_entities) {
        if (!set_subdev_format(entity, 0, cfg.mbus_code, cfg.width, cfg.height))
            return false;
        if (!set_subdev_format(entity, 1, cfg.mbus_code, cfg.width, cfg.height))
            return false;
    }

    // Set V4L2 video format (multiplane)
    if (!set_video_format(cfg.width, cfg.height, cfg.v4l2_pixfmt))
        return false;

    printf("Pipeline activated: %s %dx%d\n", cfg.name.c_str(), cfg.width, cfg.height);
    return true;
}

bool Camera::select(int index) {
    if (index < 0 || index >= (int)configs_.size()) return false;

    stop_streaming();

    if (runtime_.sensor_subdev_fd >= 0) {
        ::close(runtime_.sensor_subdev_fd);
        runtime_.sensor_subdev_fd = -1;
    }
    if (runtime_.af_subdev_fd >= 0) {
        ::close(runtime_.af_subdev_fd);
        runtime_.af_subdev_fd = -1;
    }

    disconnect_all();

    auto& cfg = configs_[index];
    if (!activate_pipeline(cfg)) return false;

    // Open sensor subdev for controls
    runtime_.sensor_subdev_fd = open_entity_subdev(cfg.sensor_entity);
    if (runtime_.sensor_subdev_fd < 0) return false;

    // Open AF subdev if available
    if (cfg.has_af && !cfg.af_entity.empty()) {
        runtime_.af_subdev_fd = open_entity_subdev(cfg.af_entity);
        if (runtime_.af_subdev_fd < 0)
            fprintf(stderr, "Warning: AF motor not available\n");
    }

    // Set default exposure/gain
    set_v4l2_ctrl(runtime_.sensor_subdev_fd, V4L2_CID_EXPOSURE, cfg.exposure.def);
    set_v4l2_ctrl(runtime_.sensor_subdev_fd, V4L2_CID_ANALOGUE_GAIN, cfg.analogue_gain.def);
    set_v4l2_ctrl(runtime_.sensor_subdev_fd, V4L2_CID_DIGITAL_GAIN, cfg.digital_gain.def);

    active_ = &configs_[index];
    active_idx_ = index;
    return true;
}

bool Camera::set_v4l2_ctrl(int fd, uint32_t id, int value) {
    struct v4l2_control ctrl;
    ctrl.id = id;
    ctrl.value = value;
    if (xioctl(fd, VIDIOC_S_CTRL, &ctrl) < 0) {
        fprintf(stderr, "set ctrl 0x%08x = %d: %s\n", id, value, strerror(errno));
        return false;
    }
    return true;
}

int Camera::get_v4l2_ctrl(int fd, uint32_t id) {
    struct v4l2_control ctrl;
    ctrl.id = id;
    if (xioctl(fd, VIDIOC_G_CTRL, &ctrl) < 0) return -1;
    return ctrl.value;
}

bool Camera::set_exposure(int value) {
    if (runtime_.sensor_subdev_fd < 0) return false;
    return set_v4l2_ctrl(runtime_.sensor_subdev_fd, V4L2_CID_EXPOSURE, value);
}

bool Camera::set_analogue_gain(int value) {
    if (runtime_.sensor_subdev_fd < 0) return false;
    return set_v4l2_ctrl(runtime_.sensor_subdev_fd, V4L2_CID_ANALOGUE_GAIN, value);
}

bool Camera::set_digital_gain(int value) {
    if (runtime_.sensor_subdev_fd < 0) return false;
    return set_v4l2_ctrl(runtime_.sensor_subdev_fd, V4L2_CID_DIGITAL_GAIN, value);
}

bool Camera::set_focus(int value) {
    if (runtime_.af_subdev_fd < 0) return false;
    return set_v4l2_ctrl(runtime_.af_subdev_fd, V4L2_CID_FOCUS_ABSOLUTE, value);
}

int Camera::get_exposure() {
    if (runtime_.sensor_subdev_fd < 0) return -1;
    return get_v4l2_ctrl(runtime_.sensor_subdev_fd, V4L2_CID_EXPOSURE);
}

int Camera::get_analogue_gain() {
    if (runtime_.sensor_subdev_fd < 0) return -1;
    return get_v4l2_ctrl(runtime_.sensor_subdev_fd, V4L2_CID_ANALOGUE_GAIN);
}

int Camera::get_digital_gain() {
    if (runtime_.sensor_subdev_fd < 0) return -1;
    return get_v4l2_ctrl(runtime_.sensor_subdev_fd, V4L2_CID_DIGITAL_GAIN);
}

int Camera::get_focus() {
    if (runtime_.af_subdev_fd < 0) return -1;
    return get_v4l2_ctrl(runtime_.af_subdev_fd, V4L2_CID_FOCUS_ABSOLUTE);
}

bool Camera::start_streaming(int buffer_count) {
    if (streaming_ || !active_) return false;

    // Request multiplane buffers
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = buffer_count;
    req.type = BUF_TYPE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(video_fd_, VIDIOC_REQBUFS, &req) < 0) {
        perror("VIDIOC_REQBUFS");
        return false;
    }

    // Map buffers
    buffers_.resize(req.count);
    for (uint32_t i = 0; i < req.count; i++) {
        struct v4l2_buffer buf;
        struct v4l2_plane planes[1];
        memset(&buf, 0, sizeof(buf));
        memset(planes, 0, sizeof(planes));
        buf.type = BUF_TYPE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.length = 1;  // num planes
        buf.m.planes = planes;
        if (xioctl(video_fd_, VIDIOC_QUERYBUF, &buf) < 0) {
            perror("VIDIOC_QUERYBUF");
            return false;
        }
        buffers_[i].length = planes[0].length;
        buffers_[i].start = mmap(nullptr, planes[0].length,
                                 PROT_READ | PROT_WRITE, MAP_SHARED,
                                 video_fd_, planes[0].m.mem_offset);
        if (buffers_[i].start == MAP_FAILED) {
            perror("mmap");
            return false;
        }
    }

    // Queue all buffers
    for (uint32_t i = 0; i < req.count; i++) {
        struct v4l2_buffer buf;
        struct v4l2_plane planes[1];
        memset(&buf, 0, sizeof(buf));
        memset(planes, 0, sizeof(planes));
        buf.type = BUF_TYPE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.length = 1;
        buf.m.planes = planes;
        if (xioctl(video_fd_, VIDIOC_QBUF, &buf) < 0) {
            perror("VIDIOC_QBUF");
            return false;
        }
    }

    // Start streaming
    int type = BUF_TYPE;
    if (xioctl(video_fd_, VIDIOC_STREAMON, &type) < 0) {
        perror("VIDIOC_STREAMON");
        return false;
    }

    streaming_ = true;
    printf("Streaming started (%u buffers)\n", req.count);
    return true;
}

void Camera::stop_streaming() {
    if (!streaming_) return;

    int type = BUF_TYPE;
    xioctl(video_fd_, VIDIOC_STREAMOFF, &type);

    for (auto& b : buffers_) {
        if (b.start && b.start != MAP_FAILED)
            munmap(b.start, b.length);
    }
    buffers_.clear();

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = 0;
    req.type = BUF_TYPE;
    req.memory = V4L2_MEMORY_MMAP;
    xioctl(video_fd_, VIDIOC_REQBUFS, &req);

    streaming_ = false;
    printf("Streaming stopped\n");
}

bool Camera::dequeue_frame(Frame& frame) {
    struct v4l2_buffer buf;
    struct v4l2_plane planes[1];
    memset(&buf, 0, sizeof(buf));
    memset(planes, 0, sizeof(planes));
    buf.type = BUF_TYPE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.length = 1;
    buf.m.planes = planes;
    if (xioctl(video_fd_, VIDIOC_DQBUF, &buf) < 0) {
        if (errno != EAGAIN)
            perror("VIDIOC_DQBUF");
        return false;
    }
    frame.data = (const uint8_t*)buffers_[buf.index].start;
    frame.length = planes[0].bytesused;
    frame.index = buf.index;
    return true;
}

void Camera::release_frame(const Frame& frame) {
    struct v4l2_buffer buf;
    struct v4l2_plane planes[1];
    memset(&buf, 0, sizeof(buf));
    memset(planes, 0, sizeof(planes));
    buf.type = BUF_TYPE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = frame.index;
    buf.length = 1;
    buf.m.planes = planes;
    xioctl(video_fd_, VIDIOC_QBUF, &buf);
}
