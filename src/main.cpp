#include "camera.h"
#include "pipeline.h"
#include "stream.h"
#include "http.h"
#include "isp.h"
#include "autowhitebalance.h"
#include "autoexposure.h"
#include "autofocus.h"
#include "logging.h"
#include "args.h"
#include "sha256.h"
#include "json.hpp"

#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <string>
#include <atomic>
#include <mutex>
#include <thread>
#include <filesystem>
#include <termios.h>
#include <unistd.h>

using json = nlohmann::json;

static std::atomic<bool> g_running{true};
static void signal_handler(int) { g_running = false; }

static const char* bayer_name(BayerPattern pattern) {
    switch (pattern) {
    case BayerPattern::RGGB: return "rggb";
    case BayerPattern::BGGR: return "bggr";
    case BayerPattern::GRBG: return "grbg";
    case BayerPattern::GBRG: return "gbrg";
    }
    return "unknown";
}

static std::string find_static_dir(const char* argv0) {
    namespace fs = std::filesystem;
    fs::path exe = fs::canonical(argv0);
    fs::path dir = exe.parent_path() / "static";
    if (fs::is_directory(dir)) return dir.string();
    dir = exe.parent_path().parent_path() / "static";
    if (fs::is_directory(dir)) return dir.string();
    dir = fs::current_path() / "static";
    if (fs::is_directory(dir)) return dir.string();
    return "static";
}

int main(int argc, char* argv[]) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    // Disable terminal echo and input processing
    struct termios orig_termios, new_termios;
    bool termios_modified = false;
    if (isatty(STDIN_FILENO)) {
        if (tcgetattr(STDIN_FILENO, &orig_termios) == 0) {
            new_termios = orig_termios;
            new_termios.c_lflag &= ~(ECHO | ECHONL | ICANON);
            if (tcsetattr(STDIN_FILENO, TCSANOW, &new_termios) == 0) {
                termios_modified = true;
            }
        }
    }

    // RAII guard to restore terminal settings
    struct TermiosGuard {
        bool modified;
        struct termios orig;
        ~TermiosGuard() {
            if (modified) {
                tcsetattr(STDIN_FILENO, TCSANOW, &orig);
            }
        }
    } termios_guard{termios_modified, orig_termios};

    ArgParser args(argc, argv);

    // Help and version
    if (args.has_flag("-h", "--help")) {
        print_help(argv[0]);
        return 0;
    }
    if (args.has_flag("-V", "--version")) {
        print_version(argv[0]);
        return 0;
    }

    // Parse options
    int port = args.get_int("-p", "--port", 8080);
    std::string media_dev = args.get_string("-m", "--media", "/dev/media0");
    std::string video_dev = args.get_string("-v", "--video", "/dev/video0");
    int camera_idx = args.get_int("-c", "--camera", 0);
    int jpeg_quality = args.get_int("-q", "--quality", 80);
    int downsample = args.get_int("-d", "--downsample", 4);
    int target_fps = args.get_int("-f", "--fps", 0);
    std::string log_level_str = args.get_string("-l", "--log-level", "info");
    bool no_awb = args.has_flag("--no-awb", nullptr);
    bool no_ae = args.has_flag("--no-ae", nullptr);
    bool no_af = args.has_flag("--no-af", nullptr);
    bool auth_no_stream = args.has_flag("--auth-no-stream", nullptr);
    bool enable_auth = args.has_flag("--auth", nullptr) || auth_no_stream;
    std::string secret = args.get_string("--secret", nullptr, "");

    // Set log level (case-insensitive)
    std::string level = log_level_str;
    for (char& c : level) c = tolower(c);
    if (level == "debug") g_log_level = LogLevel::DEBUG;
    else if (level == "info") g_log_level = LogLevel::INFO;
    else if (level == "warn" || level == "warning") g_log_level = LogLevel::WARN;
    else if (level == "error" || level == "err") g_log_level = LogLevel::ERROR;

    LOG_INFO("fajita-webcam starting...");
    LOG_DEBUG("Options: port=%d, camera=%d, quality=%d, downsample=%d, fps=%d, auth=%d, auth_stream=%d",
              port, camera_idx, jpeg_quality, downsample, target_fps, enable_auth, enable_auth && !auth_no_stream);

    // Generate secret if auth enabled but no secret provided
    if (enable_auth && secret.empty()) {
        secret = sha256::random_hex(16);
        LOG_INFO("Auth enabled. Generated secret: %s", secret.c_str());
    } else if (enable_auth) {
        LOG_INFO("Auth enabled with provided secret");
    }
    if (enable_auth && auth_no_stream) {
        LOG_INFO("Stream authentication disabled (--auth-no-stream)");
    }

    // Initialize camera
    Camera camera;
    if (!camera.open(media_dev.c_str(), video_dev.c_str())) {
        LOG_ERROR("Failed to open camera devices: media=%s, video=%s",
                  media_dev.c_str(), video_dev.c_str());
        return 1;
    }

    LOG_INFO("Found %zu cameras", camera.configs().size());
    for (size_t i = 0; i < camera.configs().size(); i++) {
        auto& c = camera.configs()[i];
        LOG_INFO("  [%zu] %s (%dx%d) %s", i, c.name.c_str(),
                 c.width, c.height, c.has_af ? "AF" : "");
    }

    if (!camera.select(camera_idx)) {
        LOG_ERROR("Failed to select camera %d", camera_idx);
        return 1;
    }
    LOG_INFO("Selected camera: %s", camera.active_config()->name.c_str());

    // Create pipeline
    MjpegStream mjpeg_stream;
    Pipeline pipeline(camera, mjpeg_stream);

    AutoWhiteBalance awb;
    AutoExposure ae;
    AutoFocus af;

    // Disable auto modes if requested
    if (no_awb) awb.set_auto(false);
    if (no_ae) ae.set_auto(false);

    std::mutex metric_mutex;
    float latest_focus_metric = 0;

    // Analysis callback
    pipeline.set_analysis_callback([&](const AnalysisFrame& frame) {
        if (!no_awb) awb.analyze(frame.bayer, frame.width, frame.height, frame.pattern);
        if (!no_ae) ae.analyze(frame.bayer, frame.width, frame.height, frame.pattern, camera);

        // Focus metric
        int roi_w = std::min(256, frame.width);
        int roi_h = std::min(256, frame.height);
        int roi_x = (frame.width - roi_w) / 2;
        int roi_y = (frame.height - roi_h) / 2;
        float fm = isp_focus_metric(frame.bayer, frame.width, frame.height,
                                    roi_x, roi_y, roi_w, roi_h, frame.pattern);
        {
            std::lock_guard<std::mutex> lock(metric_mutex);
            latest_focus_metric = fm;
        }

        // Update WB gains
        PipelineConfig cfg = pipeline.get_config();
        cfg.r_gain = awb.r_gain();
        cfg.b_gain = awb.b_gain();
        pipeline.set_config(cfg);

        // Continuous AF
        if (!no_af) af.update_continuous(camera, fm);
    }, 5);

    // Configure pipeline
    {
        PipelineConfig cfg;
        cfg.downsample = downsample;
        cfg.jpeg_quality = jpeg_quality;
        cfg.target_fps = target_fps;
        pipeline.set_config(cfg);
    }

    if (!pipeline.start()) {
        LOG_ERROR("Failed to start pipeline");
        return 1;
    }
    LOG_INFO("Pipeline started");

    // HTTP server
    HttpServer http;
    http.set_static_dir(find_static_dir(argv[0]));
    if (enable_auth) {
        http.set_secret(secret);
        http.set_auth_stream(!auth_no_stream);
    }

    // ============ API Routes ============

    // GET /api/cameras - List cameras
    http.route("GET", "/api/cameras", [&](const HttpRequest&) -> HttpResponse {
        json arr = json::array();
        for (size_t i = 0; i < camera.configs().size(); i++) {
            auto& c = camera.configs()[i];
            arr.push_back({
                {"index", i},
                {"name", c.name},
                {"width", c.width},
                {"height", c.height},
                {"has_af", c.has_af},
                {"rotation", c.rotation},
                {"bayer", bayer_name(c.bayer)},
                {"exposure", {{"min", c.exposure.min}, {"max", c.exposure.max}, {"default", c.exposure.def}}},
                {"analogue_gain", {{"min", c.analogue_gain.min}, {"max", c.analogue_gain.max}, {"default", c.analogue_gain.def}}},
                {"digital_gain", {{"min", c.digital_gain.min}, {"max", c.digital_gain.max}, {"default", c.digital_gain.def}}},
            });
        }
        return HttpResponse::json(arr.dump());
    });

    // GET /api/status - Current status
    http.route("GET", "/api/status", [&](const HttpRequest&) -> HttpResponse {
        auto* cfg = camera.active_config();
        auto pcfg = pipeline.get_config();
        json j = {
            {"camera", cfg ? cfg->name : "none"},
            {"camera_index", camera.active_index()},
            {"streaming", pipeline.is_running()},
            {"fps", pipeline.current_fps()},
            {"jpeg_size", pipeline.last_jpeg_size()},
            {"timing", {
                {"isp_ms", pipeline.time_unpack()},
                {"jpeg_ms", pipeline.time_jpeg()},
            }},
            {"downsample", pcfg.downsample},
            {"rotation", pcfg.rotation},
            {"jpeg_quality", pcfg.jpeg_quality},
            {"target_fps", pcfg.target_fps},
            {"exposure", camera.get_exposure()},
            {"analogue_gain", camera.get_analogue_gain()},
            {"digital_gain", camera.get_digital_gain()},
            {"focus", camera.get_focus()},
            {"awb", {
                {"auto", awb.is_auto()},
                {"r_gain", awb.r_gain()},
                {"b_gain", awb.b_gain()},
            }},
            {"ae", {
                {"auto", ae.is_auto()},
                {"target", ae.target_brightness()},
                {"current", ae.current_brightness()},
            }},
            {"af", {
                {"mode", af.mode() == AFMode::Manual ? "manual" :
                         af.mode() == AFMode::Continuous ? "continuous" : "oneshot"},
                {"state", af.state() == AFState::Idle ? "idle" :
                          af.state() == AFState::Scanning ? "scanning" :
                          af.state() == AFState::Locked ? "locked" : "failed"},
                {"position", af.best_position()},
            }},
        };
        return HttpResponse::json(j.dump());
    });

    // POST /api/camera/select - Switch camera
    http.route("POST", "/api/camera/select", [&](const HttpRequest& req) -> HttpResponse {
        auto j = json::parse(req.body, nullptr, false);
        if (j.is_discarded() || !j.contains("index"))
            return HttpResponse::error(400, R"({"error": "Missing 'index' field"})");

        int idx = j["index"].get<int>();
        LOG_INFO("Switching to camera %d", idx);

        af.reset();
        pipeline.stop();
        camera.stop_streaming();

        if (!camera.select(idx)) {
            LOG_ERROR("Failed to select camera %d", idx);
            return HttpResponse::error(500, R"({"error": "Failed to select camera"})");
        }

        {
            std::lock_guard<std::mutex> lock(metric_mutex);
            latest_focus_metric = 0;
        }

        if (!pipeline.start()) {
            LOG_ERROR("Failed to restart pipeline");
            return HttpResponse::error(500, R"({"error": "Failed to start pipeline"})");
        }

        return HttpResponse::json(json{{"ok", true}, {"camera", camera.active_config()->name}}.dump());
    });

    // POST /api/control - Set camera controls
    http.route("POST", "/api/control", [&](const HttpRequest& req) -> HttpResponse {
        auto j = json::parse(req.body, nullptr, false);
        if (j.is_discarded())
            return HttpResponse::error(400, R"({"error": "Invalid JSON"})");

        if (j.contains("exposure")) {
            int val = j["exposure"].get<int>();
            LOG_DEBUG("Setting exposure: %d", val);
            camera.set_exposure(val);
        }
        if (j.contains("analogue_gain")) {
            int val = j["analogue_gain"].get<int>();
            LOG_DEBUG("Setting analogue_gain: %d", val);
            camera.set_analogue_gain(val);
        }
        if (j.contains("digital_gain")) {
            int val = j["digital_gain"].get<int>();
            LOG_DEBUG("Setting digital_gain: %d", val);
            camera.set_digital_gain(val);
        }

        return HttpResponse::json(R"({"ok": true})");
    });

    // POST /api/focus - Control autofocus
    http.route("POST", "/api/focus", [&](const HttpRequest& req) -> HttpResponse {
        auto* cfg = camera.active_config();
        if (!cfg || !cfg->has_af)
            return HttpResponse::error(400, R"({"error": "Camera has no AF"})");

        auto j = json::parse(req.body, nullptr, false);
        if (j.is_discarded())
            return HttpResponse::error(400, R"({"error": "Invalid JSON"})");

        if (j.contains("position")) {
            int pos = j["position"].get<int>();
            LOG_DEBUG("Setting focus position: %d", pos);
            af.set_mode(AFMode::Manual);
            if (!camera.set_focus(pos)) {
                af.mark_failed();
                return HttpResponse::error(500, R"({"error": "Failed to set focus"})");
            }
            af.set_manual_position(pos);
        }
        if (j.contains("mode")) {
            std::string mode = j["mode"].get<std::string>();
            LOG_DEBUG("Setting focus mode: %s", mode.c_str());
            if (mode == "manual") {
                af.set_mode(AFMode::Manual);
                af.set_manual_position(std::max(camera.get_focus(), 0));
            } else if (mode == "oneshot") {
                af.set_mode(AFMode::OneShot);
                af.trigger(camera, [&]() -> float {
                    std::lock_guard<std::mutex> lock(metric_mutex);
                    return latest_focus_metric;
                });
            } else if (mode == "continuous") {
                af.set_mode(AFMode::Continuous);
            }
        }

        return HttpResponse::json(R"({"ok": true})");
    });

    // POST /api/awb - Control white balance
    http.route("POST", "/api/awb", [&](const HttpRequest& req) -> HttpResponse {
        auto j = json::parse(req.body, nullptr, false);
        if (j.is_discarded())
            return HttpResponse::error(400, R"({"error": "Invalid JSON"})");

        if (j.contains("auto"))
            awb.set_auto(j["auto"].get<bool>());
        if (j.contains("r_gain") && j.contains("b_gain"))
            awb.set_gains(j["r_gain"].get<float>(), j["b_gain"].get<float>());

        PipelineConfig pcfg = pipeline.get_config();
        pcfg.r_gain = awb.r_gain();
        pcfg.b_gain = awb.b_gain();
        pipeline.set_config(pcfg);

        return HttpResponse::json(R"({"ok": true})");
    });

    // POST /api/ae - Control auto exposure
    http.route("POST", "/api/ae", [&](const HttpRequest& req) -> HttpResponse {
        auto j = json::parse(req.body, nullptr, false);
        if (j.is_discarded())
            return HttpResponse::error(400, R"({"error": "Invalid JSON"})");

        if (j.contains("auto"))
            ae.set_auto(j["auto"].get<bool>());
        if (j.contains("target"))
            ae.set_target_brightness(j["target"].get<float>());

        return HttpResponse::json(R"({"ok": true})");
    });

    // POST /api/stream - Stream parameters
    http.route("POST", "/api/stream", [&](const HttpRequest& req) -> HttpResponse {
        auto j = json::parse(req.body, nullptr, false);
        if (j.is_discarded())
            return HttpResponse::error(400, R"({"error": "Invalid JSON"})");

        PipelineConfig cfg = pipeline.get_config();
        bool need_restart = false;

        if (j.contains("quality")) {
            cfg.jpeg_quality = std::clamp(j["quality"].get<int>(), 1, 100);
            LOG_DEBUG("Setting JPEG quality: %d", cfg.jpeg_quality);
        }
        if (j.contains("fps")) {
            cfg.target_fps = std::clamp(j["fps"].get<int>(), 0, 30);
            LOG_DEBUG("Setting target FPS: %d", cfg.target_fps);
        }
        if (j.contains("downsample")) {
            int ds = j["downsample"].get<int>();
            if (ds != cfg.downsample && (ds == 1 || ds == 2 || ds == 4)) {
                cfg.downsample = ds;
                need_restart = true;
                LOG_DEBUG("Setting downsample: %d (restart required)", ds);
            }
        }
        if (j.contains("rotation")) {
            int rot = j["rotation"].get<int>();
            if (rot == 0 || rot == 90 || rot == 180 || rot == 270) {
                cfg.rotation = rot;
                LOG_DEBUG("Setting rotation: %d", rot);
            }
        }

        if (need_restart) {
            pipeline.stop();
            pipeline.set_config(cfg);
            pipeline.start();
        } else {
            pipeline.set_config(cfg);
        }

        return HttpResponse::json(R"({"ok": true})");
    });

    // GET /snapshot - Single JPEG
    http.route("GET", "/snapshot", [&](const HttpRequest&) -> HttpResponse {
        auto frame = mjpeg_stream.get_latest();
        if (frame.data.empty())
            return HttpResponse::error(503, R"({"error": "No frame available"})");
        HttpResponse resp;
        resp.status = 200;
        resp.content_type = "image/jpeg";
        resp.body.assign((char*)frame.data.data(), frame.data.size());
        return resp;
    });

    // MJPEG stream
    http.stream_route("/stream/mjpeg", [&](int fd, const HttpRequest&) {
        mjpeg_stream.serve_client(fd);
    });

    // Start server
    LOG_INFO("Open http://localhost:%d in your browser", port);

    std::thread http_thread([&]() {
        http.start(port);
    });

    // Main loop
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    LOG_INFO("Shutting down...");
    pipeline.stop();
    camera.stop_streaming();
    mjpeg_stream.stop();
    http.stop();

    if (http_thread.joinable())
        http_thread.join();

    camera.close();
    LOG_INFO("Server stopped");

    return 0;
}