#include "camera.h"
#include "pipeline.h"
#include "stream.h"
#include "http.h"
#include "isp.h"
#include "autowhitebalance.h"
#include "autoexposure.h"
#include "autofocus.h"
#include "json.hpp"

#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <string>
#include <atomic>
#include <mutex>
#include <thread>
#include <filesystem>

using json = nlohmann::json;

static std::atomic<bool> g_running{true};
static void signal_handler(int) { g_running = false; }

// Find the static/ directory relative to the executable
static std::string find_static_dir(const char* argv0) {
    namespace fs = std::filesystem;
    // Try relative to executable
    fs::path exe = fs::canonical(argv0);
    fs::path dir = exe.parent_path() / "static";
    if (fs::is_directory(dir)) return dir.string();
    // Try ../static (if binary is in build/ subdir)
    dir = exe.parent_path().parent_path() / "static";
    if (fs::is_directory(dir)) return dir.string();
    // Fallback to current directory
    dir = fs::current_path() / "static";
    if (fs::is_directory(dir)) return dir.string();
    return "static";
}

int main(int argc, char* argv[]) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    int port = 8080;
    const char* media_dev = "/dev/media0";
    const char* video_dev = "/dev/video0";
    int default_camera = 0;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-p" && i + 1 < argc) port = std::atoi(argv[++i]);
        else if (arg == "-m" && i + 1 < argc) media_dev = argv[++i];
        else if (arg == "-v" && i + 1 < argc) video_dev = argv[++i];
        else if (arg == "-c" && i + 1 < argc) default_camera = std::atoi(argv[++i]);
        else if (arg == "-h" || arg == "--help") {
            printf("Usage: %s [-p port] [-m media_dev] [-v video_dev] [-c camera_index]\n", argv[0]);
            printf("  -p  HTTP port (default: 8080)\n");
            printf("  -m  Media device (default: /dev/media0)\n");
            printf("  -v  Video device (default: /dev/video0)\n");
            printf("  -c  Default camera: 0=imx519, 1=imx376k, 2=imx371\n");
            return 0;
        }
    }

    // Initialize camera
    Camera camera;
    if (!camera.open(media_dev, video_dev)) {
        fprintf(stderr, "Failed to open camera devices\n");
        return 1;
    }

    printf("Available cameras:\n");
    for (int i = 0; i < (int)camera.configs().size(); i++) {
        auto& c = camera.configs()[i];
        printf("  [%d] %s (%dx%d) %s\n", i, c.name.c_str(),
               c.width, c.height, c.has_af ? "[AF]" : "");
    }

    if (!camera.select(default_camera)) {
        fprintf(stderr, "Failed to select camera %d\n", default_camera);
        return 1;
    }

    // Create components
    MjpegStream mjpeg_stream;
    Pipeline pipeline(camera, mjpeg_stream);

    AutoWhiteBalance awb;
    AutoExposure ae;
    AutoFocus af;

    // Latest focus metric for AF trigger
    std::mutex metric_mutex;
    float latest_focus_metric = 0;

    // Analysis callback: runs every N frames from capture thread
    pipeline.set_analysis_callback([&](const AnalysisFrame& frame) {
        awb.analyze(frame.bayer, frame.width, frame.height, frame.pattern);
        ae.analyze(frame.bayer, frame.width, frame.height,
                   frame.pattern, camera);

        // Compute focus metric
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

        // Update WB gains in pipeline
        PipelineConfig cfg = pipeline.get_config();
        cfg.r_gain = awb.r_gain();
        cfg.b_gain = awb.b_gain();
        pipeline.set_config(cfg);

        // Continuous AF check
        af.update_continuous(camera, fm);
    }, 5);

    // Start pipeline
    {
        PipelineConfig cfg;
        cfg.downsample = 4;
        cfg.jpeg_quality = 80;
        cfg.target_fps = 15;
        pipeline.set_config(cfg);
    }
    if (!pipeline.start()) {
        fprintf(stderr, "Failed to start pipeline\n");
        return 1;
    }

    // Setup HTTP server
    HttpServer http;
    http.set_static_dir(find_static_dir(argv[0]));

    // MJPEG stream endpoint
    http.stream_route("/stream/mjpeg", [&](int fd, const HttpRequest&) {
        mjpeg_stream.serve_client(fd);
    });

    // Snapshot
    http.route("GET", "/snapshot", [&](const HttpRequest&) -> HttpResponse {
        auto frame = mjpeg_stream.get_latest();
        if (frame.data.empty())
            return HttpResponse::error(503, "No frame available");
        HttpResponse resp;
        resp.status = 200;
        resp.content_type = "image/jpeg";
        resp.body.assign((char*)frame.data.data(), frame.data.size());
        return resp;
    });

    // API: list cameras
    http.route("GET", "/api/cameras", [&](const HttpRequest&) -> HttpResponse {
        json arr = json::array();
        for (int i = 0; i < (int)camera.configs().size(); i++) {
            auto& c = camera.configs()[i];
            arr.push_back({
                {"index", i},
                {"name", c.name},
                {"width", c.width},
                {"height", c.height},
                {"has_af", c.has_af},
                {"rotation", c.rotation},
                {"bayer", c.bayer == BayerPattern::RGGB ? "rggb" : "bggr"},
                {"exposure", {{"min", c.exposure.min}, {"max", c.exposure.max}, {"default", c.exposure.def}}},
                {"analogue_gain", {{"min", c.analogue_gain.min}, {"max", c.analogue_gain.max}, {"default", c.analogue_gain.def}}},
                {"digital_gain", {{"min", c.digital_gain.min}, {"max", c.digital_gain.max}, {"default", c.digital_gain.def}}},
            });
        }
        return HttpResponse::json(arr.dump());
    });

    // API: select camera
    http.route("POST", "/api/camera/select", [&](const HttpRequest& req) -> HttpResponse {
        auto j = json::parse(req.body, nullptr, false);
        if (j.is_discarded() || !j.contains("index"))
            return HttpResponse::error(400, "Need {\"index\": N}");

        int idx = j["index"].get<int>();

        pipeline.stop();
        camera.stop_streaming();

        if (!camera.select(idx))
            return HttpResponse::error(500, "Failed to select camera");

        if (!pipeline.start())
            return HttpResponse::error(500, "Failed to start pipeline");

        return HttpResponse::json(json({{"ok", true}, {"camera", camera.active_config()->name}}).dump());
    });

    // API: get status
    http.route("GET", "/api/status", [&](const HttpRequest&) -> HttpResponse {
        auto* cfg = camera.active_config();
        auto pcfg = pipeline.get_config();
        json j = {
            {"camera", cfg ? cfg->name : "none"},
            {"camera_index", camera.active_index()},
            {"streaming", pipeline.is_running()},
            {"fps", pipeline.current_fps()},
            {"jpeg_size", pipeline.last_jpeg_size()},
            {"downsample", pcfg.downsample},
            {"jpeg_quality", pcfg.jpeg_quality},
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

    // API: set controls
    http.route("POST", "/api/control", [&](const HttpRequest& req) -> HttpResponse {
        auto j = json::parse(req.body, nullptr, false);
        if (j.is_discarded())
            return HttpResponse::error(400, "Invalid JSON");

        if (j.contains("exposure"))
            camera.set_exposure(j["exposure"].get<int>());
        if (j.contains("analogue_gain"))
            camera.set_analogue_gain(j["analogue_gain"].get<int>());
        if (j.contains("digital_gain"))
            camera.set_digital_gain(j["digital_gain"].get<int>());

        return HttpResponse::json("{\"ok\":true}");
    });

    // API: focus control
    http.route("POST", "/api/focus", [&](const HttpRequest& req) -> HttpResponse {
        auto* cfg = camera.active_config();
        if (!cfg || !cfg->has_af)
            return HttpResponse::error(400, "Camera has no AF");

        auto j = json::parse(req.body, nullptr, false);
        if (j.is_discarded())
            return HttpResponse::error(400, "Invalid JSON");

        if (j.contains("position")) {
            af.set_mode(AFMode::Manual);
            camera.set_focus(j["position"].get<int>());
        }
        if (j.contains("mode")) {
            std::string mode = j["mode"].get<std::string>();
            if (mode == "manual") {
                af.set_mode(AFMode::Manual);
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

        return HttpResponse::json("{\"ok\":true}");
    });

    // API: AWB control
    http.route("POST", "/api/awb", [&](const HttpRequest& req) -> HttpResponse {
        auto j = json::parse(req.body, nullptr, false);
        if (j.is_discarded())
            return HttpResponse::error(400, "Invalid JSON");

        if (j.contains("auto"))
            awb.set_auto(j["auto"].get<bool>());
        if (j.contains("r_gain") && j.contains("b_gain"))
            awb.set_gains(j["r_gain"].get<float>(), j["b_gain"].get<float>());

        // Update pipeline
        PipelineConfig pcfg = pipeline.get_config();
        pcfg.r_gain = awb.r_gain();
        pcfg.b_gain = awb.b_gain();
        pipeline.set_config(pcfg);

        return HttpResponse::json("{\"ok\":true}");
    });

    // API: AE control
    http.route("POST", "/api/ae", [&](const HttpRequest& req) -> HttpResponse {
        auto j = json::parse(req.body, nullptr, false);
        if (j.is_discarded())
            return HttpResponse::error(400, "Invalid JSON");

        if (j.contains("auto"))
            ae.set_auto(j["auto"].get<bool>());
        if (j.contains("target"))
            ae.set_target_brightness(j["target"].get<float>());

        return HttpResponse::json("{\"ok\":true}");
    });

    // API: stream quality control
    http.route("POST", "/api/stream", [&](const HttpRequest& req) -> HttpResponse {
        auto j = json::parse(req.body, nullptr, false);
        if (j.is_discarded())
            return HttpResponse::error(400, "Invalid JSON");

        PipelineConfig cfg = pipeline.get_config();
        bool need_restart = false;

        if (j.contains("quality"))
            cfg.jpeg_quality = std::clamp(j["quality"].get<int>(), 1, 100);
        if (j.contains("fps"))
            cfg.target_fps = std::clamp(j["fps"].get<int>(), 1, 30);
        if (j.contains("downsample")) {
            int ds = j["downsample"].get<int>();
            if (ds != cfg.downsample && (ds == 1 || ds == 2 || ds == 4)) {
                cfg.downsample = ds;
                need_restart = true;
            }
        }

        if (need_restart) {
            pipeline.stop();
            pipeline.set_config(cfg);
            pipeline.start();
        } else {
            pipeline.set_config(cfg);
        }

        return HttpResponse::json("{\"ok\":true}");
    });

    printf("\nStarting server on port %d...\n", port);
    printf("Open http://localhost:%d in your browser\n\n", port);

    // HTTP server runs in accept loop (blocks)
    // Run in a thread so we can handle signals
    std::thread http_thread([&]() {
        http.start(port);
    });

    // Wait for signal
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    printf("\nShutting down...\n");
    pipeline.stop();
    camera.stop_streaming();
    mjpeg_stream.stop();
    http.stop();

    if (http_thread.joinable())
        http_thread.join();

    camera.close();

    return 0;
}
