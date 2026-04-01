#pragma once
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>

// Simple command-line argument parser
struct ArgParser {
    int argc;
    char** argv;
    std::string program_name;
    std::string error_msg;

    ArgParser(int argc, char** argv) : argc(argc), argv(argv) {
        if (argc > 0) program_name = argv[0];
    }

    // Get string option: -o value or --option value
    std::string get_string(const char* short_opt, const char* long_opt,
                          const char* default_val = "") {
        for (int i = 1; i < argc; i++) {
            std::string arg = argv[i];
            if ((short_opt && arg == short_opt) || (long_opt && arg == long_opt)) {
                if (i + 1 < argc) return argv[i + 1];
                error_msg = "Missing value for " + arg;
                return default_val;
            }
            // Handle --option=value format
            if (long_opt && arg.rfind(std::string(long_opt) + "=", 0) == 0) {
                return arg.substr(std::string(long_opt).length() + 1);
            }
        }
        return default_val;
    }

    // Get integer option
    int get_int(const char* short_opt, const char* long_opt, int default_val) {
        std::string def_str = std::to_string(default_val);
        std::string val = get_string(short_opt, long_opt, def_str.c_str());
        return std::atoi(val.c_str());
    }

    // Check for boolean flag: -f or --flag
    bool has_flag(const char* short_opt, const char* long_opt) {
        for (int i = 1; i < argc; i++) {
            std::string arg = argv[i];
            if ((short_opt && arg == short_opt) || (long_opt && arg == long_opt)) {
                return true;
            }
        }
        return false;
    }

    // Get positional argument (remaining non-option args)
    std::vector<std::string> positional() {
        std::vector<std::string> result;
        for (int i = 1; i < argc; i++) {
            std::string arg = argv[i];
            if (arg[0] == '-') continue;
            // Also skip option values
            if (i > 1 && argv[i-1][0] == '-') continue;
            result.push_back(arg);
        }
        return result;
    }

    bool has_error() const { return !error_msg.empty(); }
};

// Print version and build info
inline void print_version(const char* name) {
    printf("%s version 1.0.0\n", name);
    printf("Built with NEON SIMD and multi-threaded ISP\n");
}

// Print help message
inline void print_help(const char* name) {
    printf("Usage: %s [OPTIONS]\n\n", name);
    printf("A webcam streaming server with hardware-accelerated image processing.\n\n");

    printf("Options:\n");
    printf("  -p, --port PORT       HTTP server port (default: 8080)\n");
    printf("  -m, --media DEVICE    Media controller device (default: /dev/media0)\n");
    printf("  -v, --video DEVICE    Video capture device (default: /dev/video0)\n");
    printf("  -c, --camera INDEX    Camera index to use (default: 0)\n");
    printf("  -q, --quality N       JPEG quality 1-100 (default: 80)\n");
    printf("  -d, --downsample N    Downsample factor 1/2/4 (default: 4)\n");
    printf("  -f, --fps N           Target frame rate, 0=unlimited (default: 0)\n");
    printf("  -l, --log-level LEVEL Log level: debug/info/warn/error (default: info)\n");
    printf("      --no-awb          Disable auto white balance\n");
    printf("      --no-ae           Disable auto exposure\n");
    printf("      --no-af           Disable auto focus\n");
    printf("      --auth            Enable token authentication\n");
    printf("      --secret STRING   Auth secret (auto-generated if not specified)\n");
    printf("      --auth-no-stream  Enable auth but don't protect MJPEG stream (implies --auth)\n");
    printf("  -h, --help            Show this help message\n");
    printf("  -V, --version         Show version information\n\n");

    printf("Available cameras:\n");
    printf("  0: imx519  (4656x3496, has AF)\n");
    printf("  1: imx376k (2592x1940, has AF)\n");
    printf("  2: imx371  (4656x3496, quad-Bayer)\n\n");

    printf("API Endpoints:\n");
    printf("  GET  /api/cameras      List available cameras\n");
    printf("  GET  /api/status       Get current status\n");
    printf("  POST /api/camera/select  Switch camera\n");
    printf("  POST /api/control      Set exposure/gain\n");
    printf("  POST /api/focus        Control autofocus\n");
    printf("  POST /api/awb          Control white balance\n");
    printf("  POST /api/ae           Control auto exposure\n");
    printf("  POST /api/stream       Set stream parameters\n");
    printf("  GET  /stream/mjpeg     MJPEG video stream\n");
    printf("  GET  /snapshot         Single JPEG snapshot\n\n");

    printf("Examples:\n");
    printf("  %s -p 9000 -c 1        Start on port 9000 with camera 1\n", name);
    printf("  %s -q 90 -d 2          High quality, 2x downsample\n", name);
    printf("  %s -l debug            Enable debug logging\n", name);
}