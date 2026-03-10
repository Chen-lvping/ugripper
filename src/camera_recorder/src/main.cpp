#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

std::atomic<bool> g_stop_requested{false};

void SignalHandler(int) {
    g_stop_requested.store(true, std::memory_order_relaxed);
}

std::string Trim(const std::string& input) {
    const std::string whitespace = " \t\r\n";
    const size_t start = input.find_first_not_of(whitespace);
    if (start == std::string::npos) {
        return "";
    }
    const size_t end = input.find_last_not_of(whitespace);
    return input.substr(start, end - start + 1);
}

std::string ShellQuote(const std::string& value) {
    std::string out = "'";
    for (char c : value) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out += c;
        }
    }
    out += "'";
    return out;
}

std::string ReadEnvFileValue(const std::string& key) {
    std::ifstream file("/etc/environment");
    if (!file.is_open()) {
        return "";
    }

    std::string line;
    while (std::getline(file, line)) {
        line = Trim(line);
        if (line.rfind(key + "=", 0) != 0) {
            continue;
        }
        std::string value = line.substr(key.size() + 1);
        value = Trim(value);
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }
        return value;
    }
    return "";
}

std::string ResolveCodec(const std::string& cli_codec) {
    if (!cli_codec.empty()) {
        return cli_codec;
    }

    const char* env_codec = std::getenv("CAMERA_CODEC");
    if (env_codec && std::string(env_codec).size() > 0) {
        return env_codec;
    }

    const std::string file_codec = ReadEnvFileValue("CAMERA_CODEC");
    if (!file_codec.empty()) {
        return file_codec;
    }

    return "h265";
}

std::string GetRkmppEncoder(const std::string& codec) {
    return (codec == "h264") ? "h264_rkmpp" : "hevc_rkmpp";
}

std::string GetGstMppEncoder(const std::string& codec) {
    return (codec == "h264") ? "mpph264enc" : "mpph265enc";
}

std::string GetGstBitstreamParser(const std::string& codec) {
    return (codec == "h264") ? "h264parse" : "h265parse";
}

std::string CommonEncodeArgs(const std::string& codec) {
    std::ostringstream oss;
    oss << "-c:v " << GetRkmppEncoder(codec) << ' '
        << "-rc_mode CQP "
        << "-qp_init 30 "
        << "-qp_max 38 "
        << "-qp_min 24 "
        << "-qp_max_i 38 "
        << "-qp_min_i 20 ";

    if (codec == "h264") {
        oss << "-profile:v main -level 5.1 ";
    } else {
        oss << "-profile:v main ";
    }
    return oss.str();
}

std::string CommonGstEncodeProps(const std::string& codec) {
    std::ostringstream oss;
    oss << GetGstMppEncoder(codec) << ' '
        << "rc-mode=fixqp "
        << "qp-init=30 "
        << "qp-max=38 "
        << "qp-min=24 "
        << "qp-max-i=38 "
        << "qp-min-i=20";

    if (codec == "h264") {
        oss << ' ' << "profile=main level=51";
    }
    return oss.str();
}

struct Options {
    fs::path output_dir;
    std::string codec;
    int duration_sec = 0;
    bool dry_run = false;
    bool allow_missing = false;
    std::string ffmpeg_bin = "ffmpeg";
    std::set<std::string> only_names;
};

struct ChildProcess {
    std::string name;
    std::string command;
    std::optional<std::string> fallback_command;
    pid_t pid = -1;
    bool running = false;
};

class ProcessManager {
public:
    explicit ProcessManager(std::vector<ChildProcess> processes)
        : processes_(std::move(processes)) {}

    bool StartAll() {
        for (auto& process : processes_) {
            if (!StartWithOptionalFallback(process)) {
                std::cerr << "[camera_recorder] failed to start process: " << process.name << std::endl;
                StopAll();
                return false;
            }
        }
        return true;
    }

    bool MonitorUntilStop(int duration_sec) {
        const auto deadline = (duration_sec > 0)
            ? std::optional<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now() + std::chrono::seconds(duration_sec))
            : std::nullopt;

        while (!g_stop_requested.load(std::memory_order_relaxed)) {
            if (deadline && std::chrono::steady_clock::now() >= *deadline) {
                return true;
            }

            for (auto& process : processes_) {
                if (!process.running || process.pid <= 0) {
                    continue;
                }

                int status = 0;
                const pid_t result = waitpid(process.pid, &status, WNOHANG);
                if (result == 0) {
                    continue;
                }
                if (result == process.pid) {
                    process.running = false;
                    std::cerr << "[camera_recorder] process exited early: " << process.name
                              << " status=" << status << std::endl;
                    return false;
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        return true;
    }

    void StopAll() {
        for (auto& process : processes_) {
            if (process.running && process.pid > 0) {
                kill(-process.pid, SIGINT);
            }
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            bool any_running = false;
            for (auto& process : processes_) {
                if (!process.running || process.pid <= 0) {
                    continue;
                }
                any_running = true;
                int status = 0;
                const pid_t result = waitpid(process.pid, &status, WNOHANG);
                if (result == process.pid) {
                    process.running = false;
                }
            }
            if (!any_running) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        for (auto& process : processes_) {
            if (process.running && process.pid > 0) {
                kill(-process.pid, SIGKILL);
                int status = 0;
                waitpid(process.pid, &status, 0);
                process.running = false;
            }
        }
    }

private:
    bool StartWithOptionalFallback(ChildProcess& process) {
        if (Start(process, process.command)) {
            return true;
        }
        if (!process.fallback_command) {
            return false;
        }
        std::cerr << "[camera_recorder] fallback start for " << process.name << std::endl;
        return Start(process, *process.fallback_command);
    }

    bool Start(ChildProcess& process, const std::string& command) {
        std::cout << "[camera_recorder] starting " << process.name << std::endl;
        std::cout << "[camera_recorder] cmd: " << command << std::endl;

        const pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            return false;
        }

        if (pid == 0) {
            setpgid(0, 0);
            execl("/bin/sh", "sh", "-lc", command.c_str(), static_cast<char*>(nullptr));
            _exit(127);
        }

        process.pid = pid;
        process.running = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));

        int status = 0;
        const pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == 0) {
            return true;
        }

        process.running = false;
        std::cerr << "[camera_recorder] process failed during startup: " << process.name
                  << " status=" << status << std::endl;
        return false;
    }

    std::vector<ChildProcess> processes_;
};

struct SingleCameraConfig {
    std::string name;
    std::string device;
    std::string input_format;
    std::string fallback_input_format;
    int width = 0;
    int height = 0;
    int fps = 0;
    bool allow_direct_copy = false;
    bool prefer_gstreamer_hw_mjpeg = false;
};

struct StereoCameraConfig {
    std::string name;
    std::string device;
    int full_width = 0;
    int full_height = 0;
    int eye_width = 0;
    int eye_height = 0;
    int capture_fps = 0;
    int output_fps = 0;
};

bool Exists(const std::string& path) {
    return fs::exists(path);
}

ChildProcess BuildSingleCameraProcess(const SingleCameraConfig& config,
                                      const Options& options,
                                      const fs::path& output_path) {
    const std::string output = ShellQuote(output_path.string());
    const std::string device = ShellQuote(config.device);
    const std::string encode_args = CommonEncodeArgs(options.codec);

    ChildProcess process;
    process.name = config.name;

    if (config.prefer_gstreamer_hw_mjpeg) {
        std::ostringstream gst;
        gst << "gst-launch-1.0 -e "
            << "v4l2src device=" << device << ' '
            << "! image/jpeg,width=" << config.width
            << ",height=" << config.height
            << ",framerate=" << config.fps << "/1 "
            << "! jpegparse "
            << "! mppjpegdec format=NV12 fast-mode=true ignore-error=true "
            << "! " << CommonGstEncodeProps(options.codec) << ' '
            << "! " << GetGstBitstreamParser(options.codec) << ' '
            << "! matroskamux ! filesink location=" << output;
        process.command = gst.str();

        std::ostringstream fallback;
        fallback << options.ffmpeg_bin
                 << " -hide_banner -loglevel warning -nostats -y "
                 << "-thread_queue_size 512 "
                 << "-f v4l2 -input_format " << config.input_format << ' '
                 << "-framerate " << config.fps << ' '
                 << "-video_size " << config.width << 'x' << config.height << ' '
                 << "-i " << device << ' '
                 << encode_args
                 << output;
        process.fallback_command = fallback.str();
        return process;
    }

    if (config.allow_direct_copy && options.codec == "h264") {
        std::ostringstream oss;
        oss << options.ffmpeg_bin
            << " -hide_banner -loglevel warning -nostats -y "
            << "-thread_queue_size 512 "
            << "-f v4l2 -input_format h264 "
            << "-framerate " << config.fps << ' '
            << "-video_size " << config.width << 'x' << config.height << ' '
            << "-i " << device << ' '
            << "-c copy " << output;
        process.command = oss.str();
        return process;
    }

    if (config.allow_direct_copy && options.codec == "h265") {
        std::ostringstream direct_hevc;
        direct_hevc << options.ffmpeg_bin
                    << " -hide_banner -loglevel warning -nostats -y "
                    << "-thread_queue_size 512 "
                    << "-f v4l2 -input_format hevc "
                    << "-framerate " << config.fps << ' '
                    << "-video_size " << config.width << 'x' << config.height << ' '
                    << "-i " << device << ' '
                    << "-c copy " << output;
        process.command = direct_hevc.str();

        std::ostringstream reencode;
        reencode << options.ffmpeg_bin
                 << " -hide_banner -loglevel warning -nostats -y "
                 << "-thread_queue_size 512 "
                 << "-f v4l2 -input_format h264 "
                 << "-framerate " << config.fps << ' '
                 << "-video_size " << config.width << 'x' << config.height << ' '
                 << "-i " << device << ' '
                 << encode_args
                 << output;
        process.fallback_command = reencode.str();
        return process;
    }

    std::ostringstream oss;
    oss << options.ffmpeg_bin
        << " -hide_banner -loglevel warning -nostats -y "
        << "-thread_queue_size 512 "
        << "-f v4l2 -input_format " << config.input_format << ' '
        << "-framerate " << config.fps << ' '
        << "-video_size " << config.width << 'x' << config.height << ' '
        << "-i " << device << ' '
        << encode_args
        << output;
    process.command = oss.str();

    if (!config.fallback_input_format.empty()) {
        std::ostringstream fallback;
        fallback << options.ffmpeg_bin
                 << " -hide_banner -loglevel warning -nostats -y "
                 << "-thread_queue_size 512 "
                 << "-f v4l2 -input_format " << config.fallback_input_format << ' '
                 << "-framerate " << config.fps << ' '
                 << "-video_size " << config.width << 'x' << config.height << ' '
                 << "-i " << device << ' '
                 << encode_args
                 << output;
        process.fallback_command = fallback.str();
    }

    return process;
}

ChildProcess BuildStereoProcess(const StereoCameraConfig& config,
                                const Options& options,
                                const fs::path& left_output_path,
                                const fs::path& right_output_path) {
    const std::string device = ShellQuote(config.device);
    const std::string left_output = ShellQuote(left_output_path.string());
    const std::string right_output = ShellQuote(right_output_path.string());
    const std::string encoder = GetRkmppEncoder(options.codec);

    std::ostringstream cmd;
    cmd << options.ffmpeg_bin
        << " -hide_banner -loglevel warning -nostats -y "
        << "-thread_queue_size 1024 "
        << "-f v4l2 -input_format mjpeg "
        << "-framerate " << config.capture_fps << ' '
        << "-video_size " << config.full_width << 'x' << config.full_height << ' '
        << "-i " << device << ' '
        << "-filter_complex "
        << ShellQuote(
            "[0:v]split=2[lraw][rraw];"
            "[lraw]crop=" + std::to_string(config.eye_width) + ":" + std::to_string(config.eye_height) + ":0:0,fps=" + std::to_string(config.output_fps) + "[left];"
            "[rraw]crop=" + std::to_string(config.eye_width) + ":" + std::to_string(config.eye_height) + ":" + std::to_string(config.eye_width) + ":0,fps=" + std::to_string(config.output_fps) + "[right]")
        << ' '
        << "-map [left] -map [right] "
        << "-c:v " << encoder << ' '
        << "-rc_mode CQP -qp_init 30 -qp_max 38 -qp_min 20 -qp_max_i 38 -qp_min_i 18 "
        << left_output << ' ' << right_output;

    ChildProcess process;
    process.name = config.name;
    process.command = cmd.str();
    return process;
}

Options ParseArgs(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string arg = argv[index];
        auto require_value = [&](const std::string& flag) -> std::string {
            if (index + 1 >= argc) {
                throw std::runtime_error("missing value for " + flag);
            }
            return argv[++index];
        };

        if (arg == "--output-dir") {
            options.output_dir = require_value(arg);
        } else if (arg == "--codec") {
            options.codec = require_value(arg);
        } else if (arg == "-d" || arg == "--duration") {
            options.duration_sec = std::stoi(require_value(arg));
        } else if (arg == "--ffmpeg-bin") {
            options.ffmpeg_bin = require_value(arg);
        } else if (arg == "--only") {
            std::stringstream ss(require_value(arg));
            std::string item;
            while (std::getline(ss, item, ',')) {
                item = Trim(item);
                if (!item.empty()) {
                    options.only_names.insert(item);
                }
            }
        } else if (arg == "--allow-missing") {
            options.allow_missing = true;
        } else if (arg == "--dry-run") {
            options.dry_run = true;
        } else if (arg == "-h" || arg == "--help") {
            std::cout
                << "Usage: camera_recorder --output-dir DIR [--codec h264|h265] [--duration SEC] [--allow-missing] [--only a,b] [--dry-run]\n"
                << "Records 10 camera streams using ffmpeg/gstreamer + Rockchip hardware codecs.\n";
            std::exit(0);
        } else {
            throw std::runtime_error("unknown argument: " + arg);
        }
    }

    if (options.output_dir.empty()) {
        throw std::runtime_error("--output-dir is required");
    }

    options.codec = ResolveCodec(options.codec);
    if (options.codec != "h264" && options.codec != "h265") {
        throw std::runtime_error("--codec must be h264 or h265");
    }

    return options;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Options options = ParseArgs(argc, argv);
        fs::create_directories(options.output_dir);

        std::signal(SIGINT, SignalHandler);
        std::signal(SIGTERM, SignalHandler);

        const std::vector<SingleCameraConfig> single_cameras = {
            {"left_cam_main", "/dev/left_cam_main", "h264", "mjpeg", 1920, 1080, 30, true},
            {"right_cam_main", "/dev/right_cam_main", "h264", "mjpeg", 1920, 1080, 30, true},
            {"left_tcam_l", "/dev/left_tcam_l", "mjpeg", "", 640, 480, 120, false, true},
            {"left_tcam_r", "/dev/left_tcam_r", "mjpeg", "", 640, 480, 120, false, true},
            {"right_tcam_l", "/dev/right_tcam_l", "mjpeg", "", 640, 480, 120, false, true},
            {"right_tcam_r", "/dev/right_tcam_r", "mjpeg", "", 640, 480, 120, false, true},
        };

        const std::vector<StereoCameraConfig> stereo_cameras = {
            {"left_stereo", "/dev/left_stereo", 2560, 800, 1280, 800, 120, 30},
            {"right_stereo", "/dev/right_stereo", 2560, 800, 1280, 800, 120, 30},
        };

        std::vector<ChildProcess> processes;
        std::vector<std::string> missing_devices;

        auto selected = [&](const std::string& name) {
            return options.only_names.empty() || options.only_names.count(name) > 0;
        };

        for (const auto& camera : single_cameras) {
            if (!selected(camera.name)) {
                continue;
            }
            if (!Exists(camera.device)) {
                missing_devices.push_back(camera.device);
                continue;
            }
            processes.push_back(BuildSingleCameraProcess(camera, options, options.output_dir / (camera.name + ".mkv")));
        }

        for (const auto& camera : stereo_cameras) {
            if (!selected(camera.name)) {
                continue;
            }
            if (!Exists(camera.device)) {
                missing_devices.push_back(camera.device);
                continue;
            }
            processes.push_back(BuildStereoProcess(camera,
                                                   options,
                                                   options.output_dir / (camera.name + "_l.mkv"),
                                                   options.output_dir / (camera.name + "_r.mkv")));
        }

        if (!missing_devices.empty()) {
            std::cerr << "[camera_recorder] missing devices:" << std::endl;
            for (const auto& device : missing_devices) {
                std::cerr << "  - " << device << std::endl;
            }
            if (!options.allow_missing) {
                return 2;
            }
        }

        if (processes.empty()) {
            std::cerr << "[camera_recorder] no camera process to start" << std::endl;
            return 2;
        }

        std::cout << "[camera_recorder] codec=" << options.codec
                  << " output_dir=" << options.output_dir
                  << " duration=" << options.duration_sec
                  << " allow_missing=" << (options.allow_missing ? "true" : "false")
                  << " only_count=" << options.only_names.size()
                  << std::endl;

        if (options.dry_run) {
            for (const auto& process : processes) {
                std::cout << process.name << ":\n" << process.command << std::endl;
                if (process.fallback_command) {
                    std::cout << "fallback:\n" << *process.fallback_command << std::endl;
                }
            }
            return 0;
        }

        ProcessManager manager(std::move(processes));
        if (!manager.StartAll()) {
            return 1;
        }

        const bool monitor_ok = manager.MonitorUntilStop(options.duration_sec);
        manager.StopAll();
        return monitor_ok ? 0 : 1;
    } catch (const std::exception& ex) {
        std::cerr << "[camera_recorder] " << ex.what() << std::endl;
        return 1;
    }
}
