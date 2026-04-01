#include "camera_recorder/camera_recorder.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <cstdio>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <libusb-1.0/libusb.h>
#include <yaml-cpp/yaml.h>
#include <nlohmann/json.hpp>

namespace camera_recorder {

namespace {

using json = nlohmann::json;

std::atomic<bool> g_stop_requested{false};

constexpr auto kRecorderStopSigintTimeout = std::chrono::milliseconds(3000);
constexpr auto kRecorderStopSigtermTimeout = std::chrono::milliseconds(1000);
constexpr auto kRecorderStopReapTimeout = std::chrono::milliseconds(500);
constexpr auto kRecorderStopPollInterval = std::chrono::milliseconds(50);
constexpr auto kUvcControlTimeout = std::chrono::milliseconds(1000);
constexpr auto kDeviceRebindTimeout = std::chrono::seconds(5);
constexpr auto kDeviceRebindPollInterval = std::chrono::milliseconds(100);
constexpr auto kStereoDaemonPollInterval = std::chrono::milliseconds(200);
constexpr auto kStereoRestartInterval = std::chrono::milliseconds(1000);
constexpr auto kStereoFinalizeSettle = std::chrono::milliseconds(700);
constexpr auto kStereoWarmupReadySettle = std::chrono::milliseconds(1200);
constexpr auto kStereoWarmupKeyframeInterval = std::chrono::milliseconds(250);
constexpr int kStereoSessionMaxRestartAttempts = 2;
constexpr int64_t kUsPerSecond = 1'000'000;

constexpr uint8_t kUvcRequestSetCur = 0x01;
constexpr uint8_t kUvcRequestGetCur = 0x81;
constexpr uint8_t kUvcRequestGetLen = 0x85;
constexpr uint8_t kUvcRequestGetInfo = 0x86;
constexpr uint8_t kUvcCsInterfaceDescriptorType = 0x24;
constexpr uint8_t kUvcInputTerminalDescriptorSubtype = 0x02;
constexpr uint16_t kUvcCameraTerminalType = 0x0201;
constexpr uint8_t kUvcCtRollAbsoluteControlSelector = 0x0f;
constexpr uint8_t kUvcControlCapGet = 1u << 0;
constexpr uint8_t kUvcControlCapSet = 1u << 1;

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
    for (char ch : value) {
        if (ch == '\'') {
            out += "'\\''";
        } else {
            out += ch;
        }
    }
    out += "'";
    return out;
}

bool EnsureDirectory(const fs::path& path) {
    std::error_code error;
    fs::create_directories(path, error);
    return !error;
}

std::vector<std::string> ListSortedFiles(const fs::path& dir, const std::string& extension) {
    std::error_code error;
    std::vector<std::string> files;
    if (!fs::exists(dir, error)) {
        return files;
    }
    for (const auto& entry : fs::directory_iterator(dir, error)) {
        if (error || !entry.is_regular_file()) {
            continue;
        }
        if (entry.path().extension() != extension) {
            continue;
        }
        files.push_back(entry.path().filename().string());
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::string JoinArgumentsForShell(const std::vector<std::string>& args) {
    std::ostringstream oss;
    for (size_t index = 0; index < args.size(); ++index) {
        if (index > 0) {
            oss << ' ';
        }
        oss << ShellQuote(args[index]);
    }
    return oss.str();
}

bool RunShellCommandCapture(const std::vector<std::string>& args, std::string* output, int* exit_code) {
    if (output != nullptr) {
        output->clear();
    }
    if (exit_code != nullptr) {
        *exit_code = -1;
    }
    if (args.empty()) {
        return false;
    }

    const std::string command = JoinArgumentsForShell(args) + " 2>&1";
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(command.c_str(), "r"), pclose);
    if (!pipe) {
        return false;
    }

    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), pipe.get()) != nullptr) {
        if (output != nullptr) {
            *output += buffer;
        }
    }

    const int status = pclose(pipe.release());
    if (exit_code != nullptr) {
        if (WIFEXITED(status)) {
            *exit_code = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            *exit_code = 128 + WTERMSIG(status);
        } else {
            *exit_code = status;
        }
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

std::optional<json> ReadJsonFile(const fs::path& path) {
    std::ifstream input(path);
    if (!input.is_open()) {
        return std::nullopt;
    }
    try {
        return json::parse(input);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

bool WriteJsonFile(const fs::path& path, const json& value) {
    const fs::path parent = path.parent_path();
    if (!parent.empty() && !EnsureDirectory(parent)) {
        return false;
    }
    const fs::path temp_path = path.string() + ".tmp";
    {
        std::ofstream output(temp_path, std::ios::trunc);
        if (!output.is_open()) {
            return false;
        }
        output << value.dump(2) << '\n';
        output.flush();
        if (!output.good()) {
            output.close();
            std::error_code cleanup_error;
            fs::remove(temp_path, cleanup_error);
            return false;
        }
    }
    std::error_code rename_error;
    fs::rename(temp_path, path, rename_error);
    if (rename_error) {
        std::error_code cleanup_error;
        fs::remove(temp_path, cleanup_error);
        return false;
    }
    return true;
}

std::optional<int64_t> ParseSecondsTextUs(const std::string& text) {
    try {
        const long double seconds = std::stold(Trim(text));
        return static_cast<int64_t>(std::llround(seconds * static_cast<long double>(kUsPerSecond)));
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

struct VideoWindowUs {
    int64_t start_pts_us = 0;
    int64_t duration_us = 0;
};

std::optional<VideoWindowUs> ProbeVideoWindowUs(const fs::path& path) {
    std::string output;
    int exit_code = -1;
    if (!RunShellCommandCapture({
            "ffprobe",
            "-v", "error",
            "-show_entries", "format=start_time,duration",
            "-of", "default=nokey=1:noprint_wrappers=1",
            path.string(),
        }, &output, &exit_code)) {
        return std::nullopt;
    }

    std::istringstream stream(output);
    std::string start_line;
    std::string duration_line;
    if (!std::getline(stream, start_line) || !std::getline(stream, duration_line)) {
        return std::nullopt;
    }

    const auto start_pts_us = ParseSecondsTextUs(start_line);
    const auto duration_us = ParseSecondsTextUs(duration_line);
    if (!start_pts_us.has_value() || !duration_us.has_value()) {
        return std::nullopt;
    }

    VideoWindowUs window;
    window.start_pts_us = *start_pts_us;
    window.duration_us = *duration_us;
    return window;
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
        std::string value = Trim(line.substr(key.size() + 1));
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
    if (env_codec != nullptr && std::string(env_codec).size() > 0) {
        return env_codec;
    }

    const std::string file_codec = ReadEnvFileValue("CAMERA_CODEC");
    if (!file_codec.empty()) {
        return file_codec;
    }

    return "h265";
}

bool Exists(const std::string& path) {
    std::error_code error;
    return fs::exists(path, error);
}

std::string ReadTrimmedFile(const fs::path& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("failed to open " + path.string());
    }

    std::string value;
    std::getline(file, value);
    return Trim(value);
}

template <typename Integer>
Integer ParseInteger(const std::string& text, int base) {
    size_t consumed = 0;
    const unsigned long long raw = std::stoull(text, &consumed, base);
    if (consumed != text.size()) {
        throw std::runtime_error("invalid integer: " + text);
    }
    if (raw > static_cast<unsigned long long>(std::numeric_limits<Integer>::max())) {
        throw std::runtime_error("integer out of range: " + text);
    }
    return static_cast<Integer>(raw);
}

template <typename Integer>
Integer ReadIntegerFile(const fs::path& path, int base) {
    return ParseInteger<Integer>(ReadTrimmedFile(path), base);
}

struct UsbVideoControlLocation {
    uint8_t bus_number = 0;
    uint8_t device_address = 0;
    uint16_t vendor_id = 0;
    uint16_t product_id = 0;
    uint8_t interface_number = 0;
};

struct LibusbContextHolder {
    libusb_context* ctx = nullptr;

    ~LibusbContextHolder() {
        if (ctx != nullptr) {
            libusb_exit(ctx);
        }
    }
};

struct LibusbDeviceListHolder {
    libusb_device** devices = nullptr;

    ~LibusbDeviceListHolder() {
        if (devices != nullptr) {
            libusb_free_device_list(devices, 1);
        }
    }
};

struct LibusbHandleHolder {
    libusb_device_handle* handle = nullptr;

    ~LibusbHandleHolder() {
        if (handle != nullptr) {
            libusb_close(handle);
        }
    }
};

struct LibusbConfigDescriptorHolder {
    libusb_config_descriptor* config = nullptr;

    ~LibusbConfigDescriptorHolder() {
        if (config != nullptr) {
            libusb_free_config_descriptor(config);
        }
    }
};

class ScopedClaimedInterface {
public:
    ScopedClaimedInterface(libusb_device_handle* handle, int interface_number)
        : handle_(handle), interface_number_(interface_number) {
        const int auto_detach_rc = libusb_set_auto_detach_kernel_driver(handle_, 1);
        if (auto_detach_rc != 0) {
            throw std::runtime_error(
                "libusb_set_auto_detach_kernel_driver failed: " + std::to_string(auto_detach_rc));
        }
        const int claim_rc = libusb_claim_interface(handle_, interface_number_);
        if (claim_rc != 0) {
            throw std::runtime_error("libusb_claim_interface(" + std::to_string(interface_number_) +
                                     ") failed: " + std::to_string(claim_rc));
        }
        claimed_ = true;
    }

    ~ScopedClaimedInterface() {
        if (claimed_) {
            libusb_release_interface(handle_, interface_number_);
        }
    }

private:
    libusb_device_handle* handle_;
    int interface_number_;
    bool claimed_ = false;
};

bool HasSelectorBit(const uint8_t* bm_controls, size_t control_size, uint8_t selector) {
    if (selector == 0) {
        return false;
    }
    const size_t bit_index = static_cast<size_t>(selector - 1);
    const size_t byte_index = bit_index / 8;
    const uint8_t bit_mask = static_cast<uint8_t>(1u << (bit_index % 8));
    return byte_index < control_size && (bm_controls[byte_index] & bit_mask) != 0;
}

UsbVideoControlLocation ResolveUsbVideoControlLocation(const std::string& device) {
    std::error_code error;
    const fs::path device_path = fs::weakly_canonical(fs::path(device), error);
    if (error || device_path.empty()) {
        throw std::runtime_error("failed to resolve device path: " + device);
    }

    const fs::path sysfs_path = fs::path("/sys/class/video4linux") / device_path.filename() / "device";
    fs::path interface_path = fs::weakly_canonical(sysfs_path, error);
    if (error || interface_path.empty()) {
        throw std::runtime_error("failed to resolve sysfs path for " + device_path.string());
    }

    fs::path usb_device_path = interface_path;
    while (!usb_device_path.empty()) {
        if (fs::exists(usb_device_path / "busnum", error) &&
            fs::exists(usb_device_path / "devnum", error) &&
            fs::exists(usb_device_path / "idVendor", error) &&
            fs::exists(usb_device_path / "idProduct", error)) {
            break;
        }
        usb_device_path = usb_device_path.parent_path();
    }
    if (usb_device_path.empty()) {
        throw std::runtime_error("failed to locate USB parent for " + device_path.string());
    }

    UsbVideoControlLocation location;
    location.bus_number = ReadIntegerFile<uint8_t>(usb_device_path / "busnum", 10);
    location.device_address = ReadIntegerFile<uint8_t>(usb_device_path / "devnum", 10);
    location.vendor_id = ReadIntegerFile<uint16_t>(usb_device_path / "idVendor", 16);
    location.product_id = ReadIntegerFile<uint16_t>(usb_device_path / "idProduct", 16);
    location.interface_number = ReadIntegerFile<uint8_t>(interface_path / "bInterfaceNumber", 16);
    return location;
}

libusb_device_handle* OpenUsbDeviceByLocation(libusb_context* ctx, const UsbVideoControlLocation& location) {
    LibusbDeviceListHolder devices_holder;
    const ssize_t device_count = libusb_get_device_list(ctx, &devices_holder.devices);
    if (device_count < 0) {
        throw std::runtime_error("libusb_get_device_list failed: " + std::to_string(device_count));
    }

    for (ssize_t index = 0; index < device_count; ++index) {
        libusb_device* device = devices_holder.devices[index];
        if (libusb_get_bus_number(device) != location.bus_number ||
            libusb_get_device_address(device) != location.device_address) {
            continue;
        }

        libusb_device_descriptor descriptor{};
        const int descriptor_rc = libusb_get_device_descriptor(device, &descriptor);
        if (descriptor_rc != 0) {
            continue;
        }
        if (descriptor.idVendor != location.vendor_id || descriptor.idProduct != location.product_id) {
            continue;
        }

        libusb_device_handle* handle = nullptr;
        const int open_rc = libusb_open(device, &handle);
        if (open_rc != 0 || handle == nullptr) {
            throw std::runtime_error("libusb_open failed: " + std::to_string(open_rc));
        }
        return handle;
    }

    throw std::runtime_error("failed to open USB device for bus=" + std::to_string(location.bus_number) +
                             " dev=" + std::to_string(location.device_address));
}

uint8_t FindRollAbsoluteTerminalId(libusb_device_handle* handle, uint8_t interface_number) {
    libusb_device* device = libusb_get_device(handle);
    if (device == nullptr) {
        throw std::runtime_error("libusb_get_device returned null");
    }

    LibusbConfigDescriptorHolder config_holder;
    const int config_rc = libusb_get_active_config_descriptor(device, &config_holder.config);
    if (config_rc != 0 || config_holder.config == nullptr) {
        throw std::runtime_error("libusb_get_active_config_descriptor failed: " + std::to_string(config_rc));
    }

    for (uint8_t interface_index = 0; interface_index < config_holder.config->bNumInterfaces; ++interface_index) {
        const libusb_interface& interface = config_holder.config->interface[interface_index];
        for (int alt_index = 0; alt_index < interface.num_altsetting; ++alt_index) {
            const libusb_interface_descriptor& alt = interface.altsetting[alt_index];
            if (alt.bInterfaceNumber != interface_number) {
                continue;
            }

            const unsigned char* cursor = alt.extra;
            int remaining = alt.extra_length;
            while (remaining >= 3) {
                const uint8_t length = cursor[0];
                if (length < 3 || length > remaining) {
                    break;
                }
                if (cursor[1] == kUvcCsInterfaceDescriptorType &&
                    cursor[2] == kUvcInputTerminalDescriptorSubtype &&
                    length >= 15) {
                    const uint16_t terminal_type =
                        static_cast<uint16_t>(cursor[4]) |
                        static_cast<uint16_t>(cursor[5] << 8);
                    const uint8_t control_size = cursor[14];
                    if (terminal_type == kUvcCameraTerminalType &&
                        length >= static_cast<uint8_t>(15 + control_size) &&
                        HasSelectorBit(cursor + 15, control_size, kUvcCtRollAbsoluteControlSelector)) {
                        return cursor[3];
                    }
                }
                cursor += length;
                remaining -= length;
            }
        }
    }

    // This camera family exposes the roll control on Camera Terminal ID 1.
    // Some firmware revisions report VC descriptors in a layout that doesn't
    // decode cleanly from libusb's extra blocks here, so fall back to the
    // terminal ID validated on the target hardware instead of failing open.
    return 1;
}

std::vector<uint8_t> UvcControlTransferIn(
    libusb_device_handle* handle,
    uint8_t request,
    uint8_t selector,
    uint8_t terminal_id,
    uint8_t interface_number,
    size_t size) {
    std::vector<uint8_t> buffer(size, 0);
    const int rc = libusb_control_transfer(
        handle,
        0xA1,
        request,
        static_cast<uint16_t>(selector) << 8,
        static_cast<uint16_t>((terminal_id << 8) | interface_number),
        buffer.data(),
        static_cast<uint16_t>(buffer.size()),
        static_cast<unsigned int>(kUvcControlTimeout.count()));
    if (rc < 0) {
        throw std::runtime_error("UVC IN control transfer failed: request=" + std::to_string(request) +
                                 " rc=" + std::to_string(rc));
    }
    if (static_cast<size_t>(rc) != size) {
        throw std::runtime_error("short UVC IN control transfer: expected=" + std::to_string(size) +
                                 " actual=" + std::to_string(rc));
    }
    return buffer;
}

void UvcControlTransferOut(
    libusb_device_handle* handle,
    uint8_t request,
    uint8_t selector,
    uint8_t terminal_id,
    uint8_t interface_number,
    const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> mutable_payload = payload;
    const int rc = libusb_control_transfer(
        handle,
        0x21,
        request,
        static_cast<uint16_t>(selector) << 8,
        static_cast<uint16_t>((terminal_id << 8) | interface_number),
        mutable_payload.data(),
        static_cast<uint16_t>(mutable_payload.size()),
        static_cast<unsigned int>(kUvcControlTimeout.count()));
    if (rc < 0) {
        throw std::runtime_error("UVC OUT control transfer failed: request=" + std::to_string(request) +
                                 " rc=" + std::to_string(rc));
    }
    if (static_cast<size_t>(rc) != payload.size()) {
        throw std::runtime_error("short UVC OUT control transfer: expected=" +
                                 std::to_string(payload.size()) + " actual=" + std::to_string(rc));
    }
}

uint16_t ParseLittleEndianUint16(const std::vector<uint8_t>& payload) {
    if (payload.size() != 2) {
        throw std::runtime_error("unexpected control payload length: " + std::to_string(payload.size()));
    }
    return static_cast<uint16_t>(payload[0]) |
           static_cast<uint16_t>(payload[1] << 8);
}

std::vector<uint8_t> EncodeLittleEndianUint16(uint16_t value, size_t size) {
    std::vector<uint8_t> payload(size, 0);
    if (size > 0) {
        payload[0] = static_cast<uint8_t>(value & 0xff);
    }
    if (size > 1) {
        payload[1] = static_cast<uint8_t>((value >> 8) & 0xff);
    }
    return payload;
}

bool WaitForDeviceNode(const std::string& device, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (Exists(device)) {
            return true;
        }
        std::this_thread::sleep_for(kDeviceRebindPollInterval);
    }
    return Exists(device);
}

bool EnsureUvcRollAbsolute(const CameraConfig& config) {
    if (!config.uvc_roll_absolute.has_value()) {
        return false;
    }
    if (!Exists(config.device)) {
        throw std::runtime_error("device node missing before applying UVC roll: " + config.device);
    }

    const UsbVideoControlLocation location = ResolveUsbVideoControlLocation(config.device);
    LibusbContextHolder ctx_holder;
    const int init_rc = libusb_init(&ctx_holder.ctx);
    if (init_rc != 0 || ctx_holder.ctx == nullptr) {
        throw std::runtime_error("libusb_init failed: " + std::to_string(init_rc));
    }

    LibusbHandleHolder handle_holder;
    handle_holder.handle = OpenUsbDeviceByLocation(ctx_holder.ctx, location);
    const uint8_t terminal_id = FindRollAbsoluteTerminalId(handle_holder.handle, location.interface_number);
    ScopedClaimedInterface claimed_interface(handle_holder.handle, location.interface_number);

    const std::vector<uint8_t> info = UvcControlTransferIn(
        handle_holder.handle,
        kUvcRequestGetInfo,
        kUvcCtRollAbsoluteControlSelector,
        terminal_id,
        location.interface_number,
        1);
    if ((info.at(0) & kUvcControlCapGet) == 0 || (info.at(0) & kUvcControlCapSet) == 0) {
        throw std::runtime_error("UVC roll-absolute control is not readable/writable");
    }

    const std::vector<uint8_t> len = UvcControlTransferIn(
        handle_holder.handle,
        kUvcRequestGetLen,
        kUvcCtRollAbsoluteControlSelector,
        terminal_id,
        location.interface_number,
        2);
    const size_t payload_size = ParseLittleEndianUint16(len);
    if (payload_size == 0) {
        throw std::runtime_error("UVC roll-absolute control returned zero-length payload");
    }

    const std::vector<uint8_t> current_payload = UvcControlTransferIn(
        handle_holder.handle,
        kUvcRequestGetCur,
        kUvcCtRollAbsoluteControlSelector,
        terminal_id,
        location.interface_number,
        payload_size);
    const uint16_t current_value = ParseLittleEndianUint16(current_payload);
    const uint16_t target_value = static_cast<uint16_t>(*config.uvc_roll_absolute);
    if (current_value == target_value) {
        return false;
    }

    UvcControlTransferOut(
        handle_holder.handle,
        kUvcRequestSetCur,
        kUvcCtRollAbsoluteControlSelector,
        terminal_id,
        location.interface_number,
        EncodeLittleEndianUint16(target_value, payload_size));
    const std::vector<uint8_t> verify_payload = UvcControlTransferIn(
        handle_holder.handle,
        kUvcRequestGetCur,
        kUvcCtRollAbsoluteControlSelector,
        terminal_id,
        location.interface_number,
        payload_size);
    const uint16_t verify_value = ParseLittleEndianUint16(verify_payload);
    if (verify_value != target_value) {
        throw std::runtime_error("UVC roll verification mismatch: expected=" +
                                 std::to_string(target_value) + " actual=" + std::to_string(verify_value));
    }

    return true;
}

bool IsMainCamera(const CameraConfig& config) {
    return config.name == "left_cam_main" || config.name == "right_cam_main";
}

int64_t CurrentSystemTimeUs() {
    const auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
}

int64_t CurrentSteadyTimeUs() {
    const auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
}

int64_t BootTimeOffsetUs() {
    return CurrentSystemTimeUs() - CurrentSteadyTimeUs();
}

std::optional<int64_t> ParseFramePtsUsFromStatsLine(const std::string& line) {
    const size_t separator = line.find(' ');
    if (separator == std::string::npos) {
        return std::nullopt;
    }

    const std::string pts_text = Trim(line.substr(0, separator));
    const std::string tb_text = Trim(line.substr(separator + 1));
    if (pts_text.empty() || tb_text.empty()) {
        return std::nullopt;
    }

    const size_t slash = tb_text.find('/');
    if (slash == std::string::npos) {
        return std::nullopt;
    }

    try {
        const int64_t pts = std::stoll(pts_text);
        const int64_t num = std::stoll(tb_text.substr(0, slash));
        const int64_t den = std::stoll(tb_text.substr(slash + 1));
        if (den == 0) {
            return std::nullopt;
        }
        const long double pts_us =
            (static_cast<long double>(pts) * static_cast<long double>(num) * 1'000'000.0L) /
            static_cast<long double>(den);
        return static_cast<int64_t>(std::llround(pts_us));
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<int64_t> ParseFramePtsUsFromDebugTsLine(const std::string& line) {
    const std::string marker = "muxer <- type:video";
    if (line.find(marker) == std::string::npos) {
        return std::nullopt;
    }

    const std::string key = "pkt_pts_time:";
    const size_t key_pos = line.find(key);
    if (key_pos == std::string::npos) {
        return std::nullopt;
    }

    size_t value_begin = key_pos + key.size();
    size_t value_end = line.find(' ', value_begin);
    const std::string value_text = Trim(line.substr(value_begin, value_end - value_begin));
    if (value_text.empty() || value_text == "NOPTS") {
        return std::nullopt;
    }

    try {
        const long double pts_sec = std::stold(value_text);
        return static_cast<int64_t>(std::llround(pts_sec * 1'000'000.0L));
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<int64_t> ParseFramePtsUsFromGstIdentityLine(const std::string& line) {
    const std::string marker = "pts: ";
    const size_t marker_pos = line.find(marker);
    if (marker_pos == std::string::npos || line.find("identity") == std::string::npos) {
        return std::nullopt;
    }

    size_t value_begin = marker_pos + marker.size();
    size_t value_end = line.find(',', value_begin);
    const std::string value_text = Trim(line.substr(value_begin, value_end - value_begin));
    if (value_text.empty() || value_text == "none" || value_text == "N/A") {
        return std::nullopt;
    }

    int hours = 0;
    int minutes = 0;
    long double seconds = 0.0;
    if (std::sscanf(value_text.c_str(), "%d:%d:%Lf", &hours, &minutes, &seconds) != 3) {
        return std::nullopt;
    }

    const long double total_seconds =
        static_cast<long double>(hours) * 3600.0L +
        static_cast<long double>(minutes) * 60.0L +
        seconds;
    return static_cast<int64_t>(std::llround(total_seconds * 1'000'000.0L));
}

std::optional<int64_t> ParseFramePtsUsFromProgressLine(const std::string& line) {
    const std::string key = "out_time_us=";
    if (line.rfind(key, 0) != 0) {
        return std::nullopt;
    }
    try {
        return std::stoll(Trim(line.substr(key.size())));
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::string GetRkmppEncoder(const std::string& codec) {
    return codec == "h264" ? "h264_rkmpp" : "hevc_rkmpp";
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

std::string CommonEncodeArgs(const std::string& codec, const CameraConfig& config) {
    std::ostringstream oss;
    oss << "-c:v " << GetRkmppEncoder(codec) << ' '
        << "-rc_mode CQP "
        << "-qp_init " << config.qp_init << ' '
        << "-qp_max " << config.qp_max << ' '
        << "-qp_min " << config.qp_min << ' '
        << "-qp_max_i " << config.qp_max_i << ' '
        << "-qp_min_i " << config.qp_min_i << ' ';

    if (codec == "h264") {
        oss << "-profile:v main -level 5.1 ";
    } else {
        oss << "-profile:v main ";
    }
    return oss.str();
}

std::string CaptureInputFormat(const CameraConfig& config) {
    return config.input_format.empty() ? "mjpeg" : config.input_format;
}

int CaptureWidth(const CameraConfig& config) {
    return config.capture_width > 0 ? config.capture_width : config.width;
}

int CaptureHeight(const CameraConfig& config) {
    return config.capture_height > 0 ? config.capture_height : config.height;
}

bool UsesFrameDropWithPreservedPts(const CameraConfig& config) {
    return config.output_fps > 0 &&
           config.output_fps < config.fps &&
           config.fps > 0 &&
           (config.fps % config.output_fps) == 0;
}

int FrameDropModulo(const CameraConfig& config) {
    return UsesFrameDropWithPreservedPts(config) ? (config.fps / config.output_fps) : 1;
}

std::optional<std::string> BuildVideoFilter(const CameraConfig& config) {
    std::vector<std::string> filters;
    if (CaptureWidth(config) != config.width || CaptureHeight(config) != config.height) {
        std::ostringstream scale;
        // When we intentionally sample down from a larger source, nearest keeps
        // the "every other pixel/line" semantics stable instead of adding blur.
        scale << "scale=" << config.width << ':' << config.height << ":flags=neighbor";
        filters.push_back(scale.str());
    }

    if (!config.video_filter.empty()) {
        filters.push_back(config.video_filter);
    }

    if (config.output_fps > 0 && config.output_fps != config.fps) {
        if (UsesFrameDropWithPreservedPts(config)) {
            std::ostringstream select;
            // Drop frames by index while keeping the surviving frames on the
            // original timeline. This avoids rebuilding a synthetic CFR PTS axis.
            select << "select=not(mod(n\\," << FrameDropModulo(config) << "))";
            filters.push_back(select.str());
        } else {
            filters.push_back("fps=" + std::to_string(config.output_fps));
        }
    }

    if (filters.empty()) {
        return std::nullopt;
    }

    std::ostringstream oss;
    for (size_t index = 0; index < filters.size(); ++index) {
        if (index > 0) {
            oss << ',';
        }
        oss << filters[index];
    }
    return oss.str();
}

std::string OutputTimingArgs(const CameraConfig& config) {
    if (!UsesFrameDropWithPreservedPts(config)) {
        return "";
    }
    return "-fps_mode passthrough -enc_time_base -1 ";
}

CameraRecordMode ParseMode(const std::string& mode_text) {
    if (mode_text == "direct-copy-h265") {
        return CameraRecordMode::DirectCopyH265;
    }
    if (mode_text == "hybrid-decode-encode") {
        return CameraRecordMode::HybridDecodeEncode;
    }
    if (mode_text == "stereo-hybrid-decode-encode") {
        return CameraRecordMode::StereoHybridDecodeEncode;
    }
    throw std::runtime_error("invalid camera mode: " + mode_text);
}

const YAML::Node RequireNode(const YAML::Node& parent, const std::string& key, const std::string& context) {
    const YAML::Node node = parent[key];
    if (!node) {
        throw std::runtime_error("missing field '" + key + "' in " + context);
    }
    return node;
}

std::string RequireString(const YAML::Node& parent, const std::string& key, const std::string& context) {
    const YAML::Node node = RequireNode(parent, key, context);
    if (!node.IsScalar()) {
        throw std::runtime_error("field '" + key + "' must be a scalar in " + context);
    }
    const std::string value = Trim(node.as<std::string>());
    if (value.empty()) {
        throw std::runtime_error("field '" + key + "' must not be empty in " + context);
    }
    return value;
}

int OptionalInt(const YAML::Node& parent, const std::string& key, int default_value, const std::string& context) {
    const YAML::Node node = parent[key];
    if (!node) {
        return default_value;
    }
    if (!node.IsScalar()) {
        throw std::runtime_error("field '" + key + "' must be a scalar in " + context);
    }
    return node.as<int>();
}

std::string OptionalString(const YAML::Node& parent,
                           const std::string& key,
                           const std::string& default_value,
                           const std::string& context) {
    const YAML::Node node = parent[key];
    if (!node) {
        return default_value;
    }
    if (!node.IsScalar()) {
        throw std::runtime_error("field '" + key + "' must be a scalar in " + context);
    }
    return Trim(node.as<std::string>());
}

int RequirePositiveInt(const YAML::Node& parent, const std::string& key, const std::string& context) {
    const int value = RequireNode(parent, key, context).as<int>();
    if (value <= 0) {
        throw std::runtime_error("field '" + key + "' must be > 0 in " + context);
    }
    return value;
}

std::vector<std::string> RequireOutputFiles(const YAML::Node& parent, const std::string& context) {
    const YAML::Node output_files = RequireNode(parent, "output_files", context);
    if (!output_files.IsSequence() || output_files.size() == 0) {
        throw std::runtime_error("field 'output_files' must be a non-empty sequence in " + context);
    }

    std::vector<std::string> result;
    result.reserve(output_files.size());
    for (size_t index = 0; index < output_files.size(); ++index) {
        const YAML::Node item = output_files[index];
        if (!item.IsScalar()) {
            throw std::runtime_error("output_files[" + std::to_string(index) + "] must be a scalar in " + context);
        }
        const std::string file_name = Trim(item.as<std::string>());
        if (file_name.empty()) {
            throw std::runtime_error("output_files[" + std::to_string(index) + "] must not be empty in " + context);
        }
        result.push_back(file_name);
    }
    return result;
}

CameraConfig ParseCameraConfig(const YAML::Node& camera_node, size_t index) {
    const std::string context = "cameras[" + std::to_string(index) + "]";
    CameraConfig config;
    config.name = RequireString(camera_node, "name", context);
    config.device = RequireString(camera_node, "device", context);
    config.mode = ParseMode(RequireString(camera_node, "mode", context));
    if (const YAML::Node roll_node = camera_node["uvc_roll_absolute"]) {
        if (!roll_node.IsScalar()) {
            throw std::runtime_error("field 'uvc_roll_absolute' must be a scalar in " + context);
        }
        const int roll_value = roll_node.as<int>();
        if (roll_value <= 0 || roll_value > std::numeric_limits<uint16_t>::max()) {
            throw std::runtime_error(
                "field 'uvc_roll_absolute' must be in range [1, 65535] in " + context);
        }
        config.uvc_roll_absolute = roll_value;
    }
    config.input_format = OptionalString(camera_node, "input_format", "", context);
    config.capture_width = OptionalInt(camera_node, "capture_width", 0, context);
    config.capture_height = OptionalInt(camera_node, "capture_height", 0, context);
    config.width = RequirePositiveInt(camera_node, "width", context);
    config.height = RequirePositiveInt(camera_node, "height", context);
    config.fps = RequirePositiveInt(camera_node, "fps", context);
    config.output_fps = OptionalInt(camera_node, "output_fps", 0, context);
    config.eye_width = OptionalInt(camera_node, "eye_width", 0, context);
    config.eye_height = OptionalInt(camera_node, "eye_height", 0, context);
    config.video_filter = OptionalString(camera_node, "video_filter", "", context);
    config.output_files = RequireOutputFiles(camera_node, context);
    config.input_thread_queue_size = OptionalInt(camera_node, "input_thread_queue_size", 0, context);
    if (config.capture_width < 0 || config.capture_height < 0 || config.input_thread_queue_size < 0) {
        throw std::runtime_error(
            "fields 'capture_width', 'capture_height', and 'input_thread_queue_size' must be >= 0 in " + context);
    }
    config.qp_init = OptionalInt(camera_node, "qp_init", config.qp_init, context);
    config.qp_max = OptionalInt(camera_node, "qp_max", config.qp_max, context);
    config.qp_min = OptionalInt(camera_node, "qp_min", config.qp_min, context);
    config.qp_max_i = OptionalInt(camera_node, "qp_max_i", config.qp_max_i, context);
    config.qp_min_i = OptionalInt(camera_node, "qp_min_i", config.qp_min_i, context);
    if (config.qp_init < 0 || config.qp_max < 0 || config.qp_min < 0 ||
        config.qp_max_i < 0 || config.qp_min_i < 0) {
        throw std::runtime_error("qp fields must be >= 0 in " + context);
    }
    return config;
}

class ShellCameraRecorder : public CameraRecorder {
public:
    ShellCameraRecorder(CameraConfig config, Options options)
        : config_(std::move(config)), options_(std::move(options)) {}

    ~ShellCameraRecorder() override {
        JoinOutputReaderThread();
    }

    const CameraConfig& config() const override {
        return config_;
    }

    const std::string& command() const override {
        return command_;
    }

    bool Start() override {
        if (started_) {
            return running_;
        }
        if (!BeforeStart()) {
            return false;
        }

        std::cout << "[camera_recorder] starting " << config_.name
                  << " mode=" << ModeName(config_.mode) << std::endl;
        std::cout << "[camera_recorder] cmd: " << command_ << std::endl;

        int output_pipe[2] = {-1, -1};
        if (pipe(output_pipe) != 0) {
            perror("pipe");
            failure_ = true;
            exit_code_ = -1;
            return false;
        }

        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            close(output_pipe[0]);
            close(output_pipe[1]);
            failure_ = true;
            exit_code_ = -1;
            return false;
        }

        if (pid == 0) {
            setsid();
            close(output_pipe[0]);
            dup2(output_pipe[1], STDOUT_FILENO);
            dup2(output_pipe[1], STDERR_FILENO);
            close(output_pipe[1]);
            execl("/bin/bash", "bash", "-lc", command_.c_str(), static_cast<char*>(nullptr));
            _exit(127);
        }

        close(output_pipe[1]);
        pid_ = pid;
        started_ = true;
        running_ = true;
        StartOutputReaderThread(output_pipe[0]);

        Poll();
        return running_;
    }

    void Poll() override {
        if (!running_ || pid_ <= 0) {
            return;
        }

        int status = 0;
        const pid_t result = waitpid(pid_, &status, WNOHANG);
        if (result == 0 || result == -1) {
            return;
        }

        running_ = false;
        exit_code_ = DecodeExitCode(status);
        const bool unexpected_exit = !stop_requested_;
        if (unexpected_exit) {
            failure_ = true;
            std::cerr << "[camera_recorder] recorder exited early: " << config_.name
                      << " exit_code=" << *exit_code_ << std::endl;
        } else {
            std::cout << "[camera_recorder] recorder stopped: " << config_.name
                      << " exit_code=" << *exit_code_ << std::endl;
        }
    }

    void Stop() override {
        if (!running_ || pid_ <= 0) {
            JoinOutputReaderThread();
            return;
        }

        stop_requested_ = true;
        kill(-pid_, SIGINT);

        const auto soft_deadline = std::chrono::steady_clock::now() + kRecorderStopSigintTimeout;
        while (std::chrono::steady_clock::now() < soft_deadline) {
            Poll();
            if (!running_) {
                JoinOutputReaderThread();
                return;
            }
            std::this_thread::sleep_for(kRecorderStopPollInterval);
        }

        std::cerr << "[camera_recorder] stop timeout after SIGINT, escalating to SIGTERM: "
                  << config_.name << std::endl;
        kill(-pid_, SIGTERM);
        const auto hard_deadline = std::chrono::steady_clock::now() + kRecorderStopSigtermTimeout;
        while (std::chrono::steady_clock::now() < hard_deadline) {
            Poll();
            if (!running_) {
                JoinOutputReaderThread();
                return;
            }
            std::this_thread::sleep_for(kRecorderStopPollInterval);
        }

        std::cerr << "[camera_recorder] stop timeout after SIGTERM, escalating to SIGKILL: "
                  << config_.name << std::endl;
        kill(-pid_, SIGKILL);
        const auto reap_deadline = std::chrono::steady_clock::now() + kRecorderStopReapTimeout;
        while (std::chrono::steady_clock::now() < reap_deadline) {
            Poll();
            if (!running_) {
                break;
            }
            std::this_thread::sleep_for(kRecorderStopPollInterval);
        }
        JoinOutputReaderThread();
    }

    bool IsRunning() const override {
        return running_;
    }

    bool HasStarted() const override {
        return started_;
    }

    bool HasFailure() const override {
        return failure_;
    }

    std::optional<int> ExitCode() const override {
        return exit_code_;
    }

    bool HasWrittenOutput() const override {
        return false;
    }

    std::optional<int64_t> RecordTimeOffsetUs() const override {
        std::lock_guard<std::mutex> lock(first_frame_mutex_);
        return record_time_offset_us_;
    }

    std::optional<int64_t> FirstFramePtsUs() const override {
        std::lock_guard<std::mutex> lock(first_frame_mutex_);
        return first_frame_pts_us_;
    }

    std::optional<int64_t> FirstFrameSystemTimeUs() const override {
        std::lock_guard<std::mutex> lock(first_frame_mutex_);
        return first_frame_system_time_us_;
    }

    std::optional<int64_t> LastFramePtsUs() const override {
        std::lock_guard<std::mutex> lock(first_frame_mutex_);
        return last_frame_pts_us_;
    }

    std::optional<int64_t> LastFrameSystemTimeUs() const override {
        std::lock_guard<std::mutex> lock(first_frame_mutex_);
        return last_frame_system_time_us_;
    }

protected:
    virtual bool BeforeStart() {
        return true;
    }

    virtual std::string BuildCommand() const = 0;

    fs::path OutputPath(const std::string& file_name) const {
        return options_.output_dir / file_name;
    }

    void UpdateFrameTiming(int64_t frame_pts_us) {
        std::lock_guard<std::mutex> lock(first_frame_mutex_);
        const int64_t system_time_us = CurrentSystemTimeUs();
        if (record_time_offset_us_.has_value()) {
            last_frame_pts_us_ = frame_pts_us;
            last_frame_system_time_us_ = system_time_us;
            return;
        }

        first_frame_pts_us_ = frame_pts_us;
        first_frame_system_time_us_ = system_time_us;
        record_time_offset_us_ = system_time_us - frame_pts_us;
        last_frame_pts_us_ = frame_pts_us;
        last_frame_system_time_us_ = system_time_us;
    }

    void HandleOutputLine(const std::string& raw_line) {
        const std::string line = Trim(raw_line);
        if (line.empty()) {
            return;
        }

        if (const auto pts_us = ParseFramePtsUsFromStatsLine(line); pts_us.has_value()) {
            UpdateFrameTiming(*pts_us);
            return;
        }
        if (const auto pts_us = ParseFramePtsUsFromDebugTsLine(line); pts_us.has_value()) {
            UpdateFrameTiming(*pts_us);
            return;
        }
        if (const auto pts_us = ParseFramePtsUsFromGstIdentityLine(line); pts_us.has_value()) {
            UpdateFrameTiming(*pts_us);
            return;
        }
        if (const auto pts_us = ParseFramePtsUsFromProgressLine(line); pts_us.has_value()) {
            UpdateFrameTiming(*pts_us);
            return;
        }

        std::cout << "[" << config_.name << "] " << line << std::endl;
    }

    void StartOutputReaderThread(int read_fd) {
        output_reader_thread_ = std::thread([this, read_fd]() {
            std::unique_ptr<FILE, decltype(&fclose)> stream(fdopen(read_fd, "r"), fclose);
            if (!stream) {
                close(read_fd);
                return;
            }

            char* line = nullptr;
            size_t line_capacity = 0;
            while (true) {
                const ssize_t line_size = getline(&line, &line_capacity, stream.get());
                if (line_size < 0) {
                    break;
                }
                HandleOutputLine(std::string(line, static_cast<size_t>(line_size)));
            }
            free(line);
        });
    }

    void JoinOutputReaderThread() {
        if (output_reader_thread_.joinable()) {
            output_reader_thread_.join();
        }
    }

    static int DecodeExitCode(int status) {
        if (WIFEXITED(status)) {
            return WEXITSTATUS(status);
        }
        if (WIFSIGNALED(status)) {
            return 128 + WTERMSIG(status);
        }
        return status;
    }

    CameraConfig config_;
    Options options_;
    std::string command_;
    pid_t pid_ = -1;
    bool started_ = false;
    bool running_ = false;
    bool stop_requested_ = false;
    bool failure_ = false;
    std::optional<int> exit_code_;
    std::thread output_reader_thread_;
    mutable std::mutex first_frame_mutex_;
    std::optional<int64_t> first_frame_pts_us_;
    std::optional<int64_t> first_frame_system_time_us_;
    std::optional<int64_t> last_frame_pts_us_;
    std::optional<int64_t> last_frame_system_time_us_;
    std::optional<int64_t> record_time_offset_us_;
};

class MainCameraRecorder final : public ShellCameraRecorder {
public:
    MainCameraRecorder(CameraConfig config, Options options)
        : ShellCameraRecorder(std::move(config), std::move(options)) {
        command_ = BuildCommand();
    }

private:
    bool BeforeStart() override {
        if (!config_.uvc_roll_absolute.has_value()) {
            return true;
        }

        try {
            const bool changed = EnsureUvcRollAbsolute(config_);
            if (!changed) {
                return true;
            }
            if (!WaitForDeviceNode(config_.device, kDeviceRebindTimeout)) {
                throw std::runtime_error("device node did not recover after UVC roll update: " + config_.device);
            }
            return true;
        } catch (const std::exception& ex) {
            failure_ = true;
            exit_code_ = -1;
            std::cerr << "[camera_recorder] failed to apply UVC roll for " << config_.name
                      << ": " << ex.what() << std::endl;
            return false;
        }
    }

    std::string BuildCommand() const override {
        const std::string device = ShellQuote(config_.device);
        const std::string output = ShellQuote(OutputPath(config_.output_files.at(0)).string());

        if (options_.codec == "h264") {
            std::ostringstream oss;
            oss << options_.ffmpeg_bin
                << " -hide_banner -loglevel info -nostats -debug_ts -y "
                << "-f v4l2 -input_format h264 "
                << "-framerate " << config_.fps << ' '
                << "-video_size " << config_.width << 'x' << config_.height << ' '
                << "-copyts "
                << "-i " << device << ' '
                << "-c:v copy "
                << output;
            return oss.str();
        }

        std::ostringstream oss;
        oss << "GST_DEBUG=identity:7 " << options_.gst_bin << " -e "
            << "v4l2src device=" << device << " do-timestamp=true ! "
            << ShellQuote(
                   "video/x-h265,width=" + std::to_string(config_.width) +
                   ",height=" + std::to_string(config_.height) +
                   ",framerate=" + std::to_string(config_.fps) + "/1")
            << " ! "
            << "identity silent=false ! "
            << "queue leaky=downstream max-size-buffers=4 ! "
            << "h265parse config-interval=-1 ! "
            << "matroskamux ! "
            << "filesink location="
            << output;
        return oss.str();
    }
};

class HybridCameraRecorder final : public ShellCameraRecorder {
public:
    HybridCameraRecorder(CameraConfig config, Options options)
        : ShellCameraRecorder(std::move(config), std::move(options)) {
        command_ = BuildCommand();
    }

private:
    std::string BuildCommand() const override {
        const std::string device = ShellQuote(config_.device);
        const std::string output = ShellQuote(OutputPath(config_.output_files.at(0)).string());
        const auto video_filter = BuildVideoFilter(config_);

        std::ostringstream oss;
        oss << options_.ffmpeg_bin
            << " -hide_banner -loglevel warning -nostats -y ";
        if (config_.input_thread_queue_size > 0) {
            oss << "-thread_queue_size " << config_.input_thread_queue_size << ' ';
        }
        oss << "-f v4l2 -input_format " << CaptureInputFormat(config_) << ' '
            << "-framerate " << config_.fps << ' '
            << "-video_size " << CaptureWidth(config_) << 'x' << CaptureHeight(config_) << ' '
            << "-i " << device << ' ';
        if (video_filter.has_value()) {
            oss << "-vf " << ShellQuote(*video_filter) << ' ';
        }
        oss << CommonEncodeArgs(options_.codec, config_)
            << OutputTimingArgs(config_)
            << "-stats_mux_pre pipe:1 "
            << "-stats_mux_pre_fmt " << ShellQuote("{pts} {tb}") << ' '
            << output;
        return oss.str();
    }
};

class StereoHybridRecorder final : public ShellCameraRecorder {
public:
    StereoHybridRecorder(CameraConfig config, Options options)
        : ShellCameraRecorder(std::move(config), std::move(options)) {
        command_ = BuildCommand();
    }

private:
    std::string BuildCommand() const override {
        const std::string device = ShellQuote(config_.device);
        const std::string output = ShellQuote(OutputPath(config_.output_files.at(0)).string());
        const auto video_filter = BuildVideoFilter(config_);

        std::ostringstream oss;
        oss << options_.ffmpeg_bin
            << " -hide_banner -loglevel warning -nostats -y ";
        if (config_.input_thread_queue_size > 0) {
            oss << "-thread_queue_size " << config_.input_thread_queue_size << ' ';
        }
        oss << "-f v4l2 -input_format " << CaptureInputFormat(config_) << ' '
            << "-framerate " << config_.fps << ' '
            << "-video_size " << CaptureWidth(config_) << 'x' << CaptureHeight(config_) << ' '
            << "-i " << device << ' ';
        if (video_filter.has_value()) {
            oss << "-vf " << ShellQuote(*video_filter) << ' ';
        }
        oss << CommonEncodeArgs(options_.codec, config_)
            << OutputTimingArgs(config_)
            << "-stats_mux_pre pipe:1 "
            << "-stats_mux_pre_fmt " << ShellQuote("{pts} {tb}") << ' '
            << output;
        return oss.str();
    }
};

std::unique_ptr<CameraRecorder> CreateRecorder(const CameraConfig& config, const Options& options) {
    switch (config.mode) {
    case CameraRecordMode::DirectCopyH265:
        return std::make_unique<MainCameraRecorder>(config, options);
    case CameraRecordMode::HybridDecodeEncode:
        return std::make_unique<HybridCameraRecorder>(config, options);
    case CameraRecordMode::StereoHybridDecodeEncode:
        return std::make_unique<StereoHybridRecorder>(config, options);
    }
    throw std::runtime_error("unknown camera record mode");
}

int StereoWarmupPort(const std::string& camera_name) {
    if (camera_name == "left_stereo") {
        return 19041;
    }
    if (camera_name == "right_stereo") {
        return 19042;
    }
    return 19050;
}

std::string StereoWarmupOutputUrl(int port) {
    return "udp://127.0.0.1:" + std::to_string(port) + "?pkt_size=1316";
}

std::string StereoWarmupInputUrl(int port) {
    return "udp://127.0.0.1:" + std::to_string(port) +
           "?fifo_size=1000000&overrun_nonfatal=1";
}

class StereoWarmupRecorder final : public ShellCameraRecorder {
public:
    StereoWarmupRecorder(CameraConfig config, Options options, int udp_port)
        : ShellCameraRecorder(std::move(config), std::move(options)),
          udp_port_(udp_port) {
        command_ = BuildCommand();
    }

private:
    std::string BuildCommand() const override {
        const std::string device = ShellQuote(config_.device);
        const std::string output_url = ShellQuote(StereoWarmupOutputUrl(udp_port_));
        const auto video_filter = BuildVideoFilter(config_);
        const double keyframe_interval_sec =
            static_cast<double>(kStereoWarmupKeyframeInterval.count()) / 1000.0;
        const int encoded_fps = config_.output_fps > 0 ? config_.output_fps : config_.fps;
        const int gop = std::max(1, static_cast<int>(
            std::llround(static_cast<long double>(encoded_fps) * keyframe_interval_sec)));

        std::ostringstream force_key_frames;
        force_key_frames << "expr:gte(t,n_forced*" << std::fixed << std::setprecision(3)
                         << keyframe_interval_sec << ")";

        std::ostringstream oss;
        oss << options_.ffmpeg_bin
            << " -hide_banner -loglevel warning -nostats -y ";
        if (config_.input_thread_queue_size > 0) {
            oss << "-thread_queue_size " << config_.input_thread_queue_size << ' ';
        }
        oss << "-f v4l2 -input_format " << CaptureInputFormat(config_) << ' '
            << "-framerate " << config_.fps << ' '
            << "-video_size " << CaptureWidth(config_) << 'x' << CaptureHeight(config_) << ' '
            << "-i " << device << ' ';
        if (video_filter.has_value()) {
            oss << "-vf " << ShellQuote(*video_filter) << ' ';
        }
        oss << CommonEncodeArgs(options_.codec, config_)
            << OutputTimingArgs(config_)
            << "-bsf:v dump_extra=freq=keyframe "
            << "-g " << gop << ' '
            << "-keyint_min " << gop << ' '
            << "-force_key_frames " << ShellQuote(force_key_frames.str()) << ' '
            << "-progress pipe:1 "
            << "-stats_period 0.5 "
            << "-stats_mux_pre pipe:1 "
            << "-stats_mux_pre_fmt " << ShellQuote("{pts} {tb}") << ' '
            << "-muxdelay 0 "
            << "-muxpreload 0 "
            << "-mpegts_flags resend_headers+initial_discontinuity+pat_pmt_at_frames "
            << "-mpegts_copyts 1 "
            << "-f mpegts "
            << output_url;
        return oss.str();
    }

    int udp_port_ = 0;
};

class StereoSessionRecorder final : public ShellCameraRecorder {
public:
    StereoSessionRecorder(CameraConfig config,
                          Options options,
                          fs::path output_path,
                          int udp_port)
        : ShellCameraRecorder(std::move(config), std::move(options)),
          output_path_(std::move(output_path)),
          udp_port_(udp_port) {
        command_ = BuildCommand();
    }

    bool HasWrittenOutput() const override {
        std::error_code error;
        return fs::exists(output_path_, error) && fs::file_size(output_path_, error) > 0;
    }

private:
    bool BeforeStart() override {
        const fs::path parent = output_path_.parent_path();
        return parent.empty() || EnsureDirectory(parent);
    }

    std::string BuildCommand() const override {
        const std::string input_url = ShellQuote(StereoWarmupInputUrl(udp_port_));
        const std::string output = ShellQuote(output_path_.string());

        std::ostringstream oss;
        oss << options_.ffmpeg_bin
            << " -hide_banner -loglevel warning -nostats -y "
            << "-thread_queue_size 512 "
            << "-fflags +genpts+discardcorrupt "
            << "-err_detect ignore_err "
            << "-analyzeduration 1500000 "
            << "-probesize 1500000 "
            << "-f mpegts "
            << "-i " << input_url << ' '
            << "-map 0:v:0 -an "
            << "-progress pipe:1 "
            << "-stats_period 0.5 "
            << "-c copy "
            << "-copyinkf "
            << "-muxdelay 0 "
            << "-muxpreload 0 "
            << output;
        return oss.str();
    }

    fs::path output_path_;
    int udp_port_ = 0;
};

struct StereoCommandState {
    uint64_t command_seq = 0;
    bool recording = false;
    std::string episode_dir;
    int64_t start_system_time_us = 0;
    int64_t stop_system_time_us = 0;
};

class StereoCameraTrack {
public:
    StereoCameraTrack(CameraConfig config, Options options)
        : config_(std::move(config)),
          options_(std::move(options)),
          udp_port_(StereoWarmupPort(config_.name)) {}

    void Stop() {
        if (session_recorder_) {
            session_recorder_->Stop();
            session_recorder_.reset();
        }
        if (warmup_recorder_) {
            warmup_recorder_->Stop();
            warmup_recorder_.reset();
        }
        session_episode_dir_.clear();
        session_failed_ = false;
        session_restart_attempts_ = 0;
    }

    void Poll(bool session_active) {
        if (warmup_recorder_) {
            warmup_recorder_->Poll();
            if (!warmup_recorder_->IsRunning()) {
                if (session_recorder_ && session_recorder_->IsRunning()) {
                    session_failed_ = true;
                    last_error_ = "stereo warmup exited during active session for " + config_.name;
                    session_recorder_->Stop();
                    session_recorder_.reset();
                }
                warmup_recorder_.reset();
            }
        }

        if (session_recorder_) {
            session_recorder_->Poll();
            const bool session_has_progress =
                session_recorder_->FirstFrameSystemTimeUs().has_value() || session_recorder_->HasWrittenOutput();
            if (!session_recorder_->IsRunning() && !stop_requested_by_control_ && !session_failed_) {
                if (!session_has_progress &&
                    session_restart_attempts_ <= kStereoSessionMaxRestartAttempts &&
                    ready()) {
                    std::cout << "[camera_recorder] stereo session recorder exited before first frame, restarting: "
                              << config_.name
                              << " attempt=" << (session_restart_attempts_ + 1)
                              << std::endl;
                    session_recorder_.reset();
                    std::string restart_error;
                    if (!StartSessionRecorder(session_episode_dir_, &restart_error)) {
                        session_failed_ = true;
                        last_error_ = restart_error;
                    }
                } else {
                    session_failed_ = true;
                    last_error_ = "stereo session recorder exited early for " + config_.name;
                }
            }
        }

        if ((!warmup_recorder_ || !warmup_recorder_->IsRunning()) &&
            (!session_active || !session_recorder_)) {
            TryStartWarmupRecorder();
        }
    }

    bool ready() const {
        if (!warmup_recorder_ || !warmup_recorder_->IsRunning()) {
            return false;
        }
        const auto first_frame_system_time_us = warmup_recorder_->FirstFrameSystemTimeUs();
        const auto last_frame_system_time_us = warmup_recorder_->LastFrameSystemTimeUs();
        if (first_frame_system_time_us.has_value() && last_frame_system_time_us.has_value()) {
            return (CurrentSystemTimeUs() - *last_frame_system_time_us) <= (2 * kUsPerSecond);
        }
        return (CurrentSystemTimeUs() - last_start_attempt_system_time_us_) >=
               (kStereoWarmupReadySettle.count() * 1000);
    }

    std::string state() const {
        if (session_recorder_ && session_recorder_->IsRunning()) {
            return ready() ? "recording" : "recording_recovering";
        }
        if (ready()) {
            return "ready";
        }
        if (warmup_recorder_ && warmup_recorder_->IsRunning()) {
            return "warming";
        }
        if (!last_error_.empty()) {
            return "recovering";
        }
        return "not_ready";
    }

    const std::string& name() const {
        return config_.name;
    }

    json BuildStatusJson() const {
        json value = json::object();
        value["state"] = state();
        value["device"] = config_.device;
        value["udp_port"] = udp_port_;
        value["ready"] = ready();
        if (warmup_recorder_) {
            if (const auto offset = warmup_recorder_->RecordTimeOffsetUs(); offset.has_value()) {
                value["record_time_offset_us"] = *offset;
            }
            if (const auto first = warmup_recorder_->FirstFrameSystemTimeUs(); first.has_value()) {
                value["first_frame_system_time_us"] = *first;
            }
            if (const auto last = warmup_recorder_->LastFrameSystemTimeUs(); last.has_value()) {
                value["last_frame_system_time_us"] = *last;
            }
        }
        value["session_recording"] = session_recorder_ && session_recorder_->IsRunning();
        if (!last_error_.empty()) {
            value["last_error"] = last_error_;
        }
        return value;
    }

    bool StartSession(const std::string& episode_dir,
                      int64_t start_system_time_us,
                      std::string* error_message) {
        if (!ready()) {
            if (error_message != nullptr) {
                *error_message = "stereo warmup not ready for " + config_.name;
            }
            return false;
        }
        if (session_recorder_ && session_recorder_->IsRunning()) {
            if (error_message != nullptr) {
                *error_message = "stereo session already active for " + config_.name;
            }
            return false;
        }

        if (!StartSessionRecorder(episode_dir, error_message)) {
            return false;
        }

        session_episode_dir_ = episode_dir;
        session_start_system_time_us_ = start_system_time_us;
        stop_requested_by_control_ = false;
        session_failed_ = false;
        last_error_.clear();
        return true;
    }

    void AbortSessionStart() {
        stop_requested_by_control_ = true;
        session_failed_ = false;
        session_start_system_time_us_ = 0;
        session_episode_dir_.clear();
        session_restart_attempts_ = 0;
        if (session_recorder_) {
            session_recorder_->Stop();
            session_recorder_.reset();
        }
    }

    void CancelSession() {
        AbortSessionStart();
    }

    bool FinalizeSession(const std::string& episode_dir,
                         int64_t start_system_time_us,
                         int64_t stop_system_time_us,
                         json* info_json,
                         std::string* error_message) {
        stop_requested_by_control_ = true;

        if (session_episode_dir_ != episode_dir) {
            if (error_message != nullptr) {
                *error_message = "stereo session episode mismatch for " + config_.name;
            }
            return false;
        }

        auto recorder = std::move(session_recorder_);
        session_episode_dir_.clear();
        if (!recorder) {
            if (error_message != nullptr) {
                *error_message = "stereo session recorder missing for " + config_.name;
            }
            return false;
        }

        recorder->Stop();
        recorder->Poll();

        if (session_failed_) {
            if (error_message != nullptr) {
                *error_message = last_error_.empty() ? ("stereo session failed for " + config_.name) : last_error_;
            }
            session_failed_ = false;
            return false;
        }

        const fs::path final_output = fs::path(episode_dir) / config_.output_files.at(0);
        std::error_code fs_error;
        if (!fs::exists(final_output, fs_error) || fs::file_size(final_output, fs_error) == 0) {
            if (error_message != nullptr) {
                *error_message = "stereo output missing or empty for " + config_.name + ": " + final_output.string();
            }
            return false;
        }

        auto record_time_offset_us = recorder->RecordTimeOffsetUs();
        auto first_frame_pts_us = recorder->FirstFramePtsUs();
        auto first_frame_system_time_us = recorder->FirstFrameSystemTimeUs();
        auto last_frame_pts_us = recorder->LastFramePtsUs();
        auto last_frame_system_time_us = recorder->LastFrameSystemTimeUs();
        if (!record_time_offset_us.has_value() ||
            !first_frame_pts_us.has_value() ||
            !first_frame_system_time_us.has_value() ||
            !last_frame_pts_us.has_value() ||
            !last_frame_system_time_us.has_value()) {
            const auto warmup_offset_us = warmup_recorder_ ? warmup_recorder_->RecordTimeOffsetUs()
                                                           : std::optional<int64_t>{};
            const auto probed_window = ProbeVideoWindowUs(final_output);
            if (warmup_offset_us.has_value() && probed_window.has_value()) {
                record_time_offset_us = warmup_offset_us;
                first_frame_pts_us = probed_window->start_pts_us;
                first_frame_system_time_us = *record_time_offset_us + *first_frame_pts_us;
                last_frame_pts_us = probed_window->start_pts_us + probed_window->duration_us;
                last_frame_system_time_us = *record_time_offset_us + *last_frame_pts_us;
            } else {
                if (error_message != nullptr) {
                    *error_message = "stereo timing metadata incomplete for " + config_.name;
                }
                return false;
            }
        }

        json camera_info = json::object();
        camera_info["record_time_offset_us"] = *record_time_offset_us;
        camera_info["first_written_frame_pts_us"] = *first_frame_pts_us;
        camera_info["first_written_frame_system_time_us"] = *first_frame_system_time_us;
        camera_info["last_written_frame_pts_us"] = *last_frame_pts_us;
        camera_info["last_written_frame_system_time_us"] = *last_frame_system_time_us;

        if (info_json != nullptr) {
            (*info_json)[config_.name] = camera_info;
        }
        session_failed_ = false;
        session_start_system_time_us_ = 0;
        session_restart_attempts_ = 0;
        return true;
    }

private:
    bool StartSessionRecorder(const std::string& episode_dir, std::string* error_message) {
        auto recorder = std::make_unique<StereoSessionRecorder>(
            config_,
            options_,
            fs::path(episode_dir) / config_.output_files.at(0),
            udp_port_);
        if (!recorder->Start()) {
            if (error_message != nullptr) {
                *error_message = "failed to start stereo session recorder for " + config_.name;
            }
            return false;
        }

        session_recorder_ = std::move(recorder);
        ++session_restart_attempts_;
        return true;
    }

    void TryStartWarmupRecorder() {
        const int64_t now_us = CurrentSystemTimeUs();
        if ((now_us - last_start_attempt_system_time_us_) < (kStereoRestartInterval.count() * 1000)) {
            return;
        }
        last_start_attempt_system_time_us_ = now_us;

        if (!Exists(config_.device)) {
            last_error_ = "device node missing: " + config_.device;
            return;
        }

        auto recorder = std::make_unique<StereoWarmupRecorder>(config_, options_, udp_port_);
        if (!recorder->Start()) {
            last_error_ = "failed to start warmup recorder";
            return;
        }

        warmup_recorder_ = std::move(recorder);
        last_error_.clear();
    }

    CameraConfig config_;
    Options options_;
    int udp_port_ = 0;
    std::unique_ptr<StereoWarmupRecorder> warmup_recorder_;
    std::unique_ptr<StereoSessionRecorder> session_recorder_;
    std::string session_episode_dir_;
    int64_t session_start_system_time_us_ = 0;
    bool stop_requested_by_control_ = false;
    bool session_failed_ = false;
    int session_restart_attempts_ = 0;
    int64_t last_start_attempt_system_time_us_ = 0;
    std::string last_error_;
};

class StereoDaemonRunner {
public:
    StereoDaemonRunner(Options options, std::vector<CameraConfig> configs)
        : options_(std::move(options)) {
        for (const auto& config : configs) {
            const bool selected =
                options_.only_names.empty() || options_.only_names.count(config.name) > 0;
            if (!selected || config.mode != CameraRecordMode::StereoHybridDecodeEncode) {
                continue;
            }
            tracks_.emplace_back(config, options_);
        }
    }

    bool Run() {
        if (tracks_.empty()) {
            std::cerr << "[camera_recorder] stereo daemon found no stereo cameras to manage" << std::endl;
            return false;
        }

        while (!g_stop_requested.load(std::memory_order_relaxed)) {
            ApplyControlCommand();
            const bool session_active = command_state_.recording || finalize_pending_;
            for (auto& track : tracks_) {
                track.Poll(session_active);
            }
            if (finalize_pending_) {
                MaybeFinalizeStoppedSession();
            }
            WriteStatus();
            std::this_thread::sleep_for(kStereoDaemonPollInterval);
        }
        ShutdownTracks();
        WriteStatus();
        return true;
    }

private:
    void ShutdownTracks() {
        for (auto& track : tracks_) {
            track.Stop();
        }
    }

    void ApplyControlCommand() {
        const auto parsed = ReadJsonFile(options_.control_file);
        if (!parsed.has_value() || !parsed->is_object()) {
            return;
        }
        const json& root = *parsed;
        const uint64_t command_seq = root.value("command_seq", static_cast<uint64_t>(0));
        if (command_seq == command_state_.command_seq) {
            return;
        }

        command_state_.command_seq = command_seq;
        command_state_.recording = root.value("recording", false);
        command_state_.episode_dir = root.value("episode_dir", std::string());
        command_state_.start_system_time_us = root.value("start_system_time_us", static_cast<int64_t>(0));
        command_state_.stop_system_time_us = root.value("stop_system_time_us", static_cast<int64_t>(0));

        if (command_state_.recording) {
            finalize_pending_ = false;
            last_finalize_error_.clear();
            last_session_result_ = json::object();
            active_episode_dir_ = command_state_.episode_dir;
            active_start_system_time_us_ = command_state_.start_system_time_us;
            active_stop_system_time_us_ = 0;
            for (auto& track : tracks_) {
                std::string error_message;
                if (!track.StartSession(active_episode_dir_, active_start_system_time_us_, &error_message)) {
                    last_finalize_error_ = error_message;
                    for (auto& rollback_track : tracks_) {
                        rollback_track.AbortSessionStart();
                    }
                    active_episode_dir_.clear();
                    active_start_system_time_us_ = 0;
                    active_stop_system_time_us_ = 0;
                    command_state_.recording = false;
                    return;
                }
            }
            return;
        }

        if (!active_episode_dir_.empty() &&
            command_state_.episode_dir == active_episode_dir_ &&
            command_state_.stop_system_time_us > 0) {
            finalize_pending_ = true;
            active_stop_system_time_us_ = command_state_.stop_system_time_us;
            finalize_requested_steady_us_ = CurrentSteadyTimeUs();
        }
    }

    void MaybeFinalizeStoppedSession() {
        const int64_t elapsed_us = CurrentSteadyTimeUs() - finalize_requested_steady_us_;
        if (elapsed_us < (kStereoFinalizeSettle.count() * 1000)) {
            return;
        }

        json stereo_info = json::object();
        stereo_info["cameras"] = json::object();
        stereo_info["episode_dir"] = active_episode_dir_;
        stereo_info["start_system_time_us"] = active_start_system_time_us_;
        stereo_info["stop_system_time_us"] = active_stop_system_time_us_;

        for (auto& track : tracks_) {
            std::string error_message;
            if (!track.FinalizeSession(active_episode_dir_,
                                       active_start_system_time_us_,
                                       active_stop_system_time_us_,
                                       &stereo_info["cameras"],
                                       &error_message)) {
                for (auto& rollback_track : tracks_) {
                    rollback_track.CancelSession();
                }
                last_finalize_error_ = error_message;
                finalize_pending_ = false;
                active_episode_dir_.clear();
                active_start_system_time_us_ = 0;
                active_stop_system_time_us_ = 0;
                return;
            }
        }

        last_session_result_ = stereo_info;
        last_finalize_error_.clear();

        finalize_pending_ = false;
        last_finalized_episode_dir_ = active_episode_dir_;
        active_episode_dir_.clear();
        active_start_system_time_us_ = 0;
        active_stop_system_time_us_ = 0;
    }

    void WriteStatus() const {
        json status = json::object();
        status["recording"] = command_state_.recording;
        status["finalize_pending"] = finalize_pending_;
        status["active_episode_dir"] = active_episode_dir_;
        status["last_finalized_episode_dir"] = last_finalized_episode_dir_;
        status["last_finalize_error"] = last_finalize_error_;
        status["last_session"] = last_session_result_;
        status["ready"] = true;
        status["service_state"] = finalize_pending_ ? "finalizing" : "warming";
        status["cameras"] = json::object();

        bool all_ready = true;
        for (const auto& track : tracks_) {
            const json camera_status = track.BuildStatusJson();
            const bool camera_ready = camera_status.value("ready", false);
            all_ready = all_ready && camera_ready;
            status["cameras"][track.name()] = camera_status;
        }
        status["ready"] = all_ready;
        if (finalize_pending_) {
            status["service_state"] = "finalizing";
        } else if (command_state_.recording) {
            status["service_state"] = all_ready ? "recording" : "recording_recovering";
        } else {
            status["service_state"] = all_ready ? "ready" : "warming";
        }
        WriteJsonFile(options_.status_file, status);
    }

    Options options_;
    std::vector<StereoCameraTrack> tracks_;
    StereoCommandState command_state_;
    bool finalize_pending_ = false;
    std::string active_episode_dir_;
    std::string last_finalized_episode_dir_;
    std::string last_finalize_error_;
    json last_session_result_ = json::object();
    int64_t active_start_system_time_us_ = 0;
    int64_t active_stop_system_time_us_ = 0;
    int64_t finalize_requested_steady_us_ = 0;
};

}  // namespace

CameraRecorderManager::CameraRecorderManager(Options options)
    : options_(std::move(options)) {}

bool CameraRecorderManager::Prepare(const std::vector<CameraConfig>& configs, std::vector<std::string>* missing_devices) {
    recorders_.clear();
    had_failure_ = false;

    auto selected = [&](const std::string& name) {
        return options_.only_names.empty() || options_.only_names.count(name) > 0;
    };

    for (const auto& config : configs) {
        if (!selected(config.name)) {
            continue;
        }
        if (!Exists(config.device)) {
            if (missing_devices != nullptr) {
                missing_devices->push_back(config.device);
            }
            continue;
        }
        recorders_.push_back(CreateRecorder(config, options_));
    }
    return true;
}

bool CameraRecorderManager::StartAll() {
    std::vector<int> started(recorders_.size(), 0);
    std::vector<std::thread> start_threads;
    start_threads.reserve(recorders_.size());

    for (size_t index = 0; index < recorders_.size(); ++index) {
        start_threads.emplace_back([this, &started, index]() {
            started[index] = recorders_[index]->Start() ? 1 : 0;
        });
    }

    for (auto& thread : start_threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }

    size_t started_count = 0;
    for (size_t index = 0; index < recorders_.size(); ++index) {
        if (started[index]) {
            started_count++;
        } else {
            had_failure_ = true;
        }
    }

    return started_count > 0;
}

bool CameraRecorderManager::MonitorUntilStop() {
    const auto deadline = (options_.duration_sec > 0)
        ? std::optional<std::chrono::steady_clock::time_point>(
            std::chrono::steady_clock::now() + std::chrono::seconds(options_.duration_sec))
        : std::nullopt;

    while (!g_stop_requested.load(std::memory_order_relaxed)) {
        if (deadline && std::chrono::steady_clock::now() >= *deadline) {
            break;
        }

        bool any_running = false;
        for (auto& recorder : recorders_) {
            recorder->Poll();
            any_running = any_running || recorder->IsRunning();
            if (recorder->HasFailure()) {
                had_failure_ = true;
            }
        }

        if (!any_running) {
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    return !had_failure_;
}

void CameraRecorderManager::StopAll() {
    std::vector<std::thread> stop_threads;
    stop_threads.reserve(recorders_.size());
    for (size_t index = 0; index < recorders_.size(); ++index) {
        stop_threads.emplace_back([this, index]() {
            recorders_[index]->Stop();
        });
    }

    for (auto& thread : stop_threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }

    for (auto& recorder : recorders_) {
        recorder->Poll();
        if (recorder->HasFailure()) {
            had_failure_ = true;
        }
    }
}

bool CameraRecorderManager::empty() const {
    return recorders_.empty();
}

bool CameraRecorderManager::had_failure() const {
    return had_failure_;
}

const std::vector<std::unique_ptr<CameraRecorder>>& CameraRecorderManager::recorders() const {
    return recorders_;
}

bool CameraRecorderManager::WriteInfoJson() const {
    const int64_t boot_time_offset_us = BootTimeOffsetUs();
    const fs::path info_json_path = options_.output_dir / "info.json";
    std::ofstream output(info_json_path, std::ios::trunc);
    if (!output.is_open()) {
        std::cerr << "[camera_recorder] failed to open info.json for write: "
                  << info_json_path << std::endl;
        return false;
    }

    output << "{\n";
    output << "  \"boot_time_offset\": " << std::fixed << std::setprecision(6)
           << (static_cast<double>(boot_time_offset_us) / 1'000'000.0) << ",\n";
    output << "  \"boot_time_offset_us\": " << boot_time_offset_us;

    bool missing_offset = false;
    for (const auto& recorder : recorders_) {
        auto offset_us = recorder->RecordTimeOffsetUs();
        if (!offset_us.has_value() && options_.codec == "h265" && IsMainCamera(recorder->config())) {
            offset_us = boot_time_offset_us;
            std::cerr << "[camera_recorder] fallback to boot_time_offset_us for main camera "
                      << recorder->config().name << " in h265 direct-stream mode" << std::endl;
        }
        if (!offset_us.has_value()) {
            std::cerr << "[camera_recorder] missing record time offset for "
                      << recorder->config().name << std::endl;
            missing_offset = true;
            continue;
        }
        output << ",\n"
               << "  \"" << recorder->config().name << "_record_time_offset_us\": "
               << *offset_us;
    }
    output << "\n}\n";
    return !missing_offset;
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
        } else if (arg == "--gst-bin") {
            options.gst_bin = require_value(arg);
        } else if (arg == "--config-yaml") {
            options.config_yaml = require_value(arg);
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
        } else if (arg == "--stereo-daemon") {
            options.stereo_daemon = true;
        } else if (arg == "--control-file") {
            options.control_file = require_value(arg);
        } else if (arg == "--status-file") {
            options.status_file = require_value(arg);
        } else if (arg == "--dry-run") {
            options.dry_run = true;
        } else if (arg == "-h" || arg == "--help") {
            std::cout
                << "Usage: camera_recorder --output-dir DIR [--codec h264|h265] [--duration SEC] [--config-yaml PATH] [--allow-missing] [--only a,b] [--dry-run]\n"
                << "   or: camera_recorder --stereo-daemon [--config-yaml PATH] [--control-file PATH] [--status-file PATH]\n"
                << "Records main/tactile/stereo streams with YAML-driven recorder classes.\n";
            std::exit(0);
        } else {
            throw std::runtime_error("unknown argument: " + arg);
        }
    }

    if (!options.stereo_daemon && options.output_dir.empty()) {
        throw std::runtime_error("--output-dir is required");
    }

    options.codec = ResolveCodec(options.codec);
    if (options.codec != "h264" && options.codec != "h265") {
        throw std::runtime_error("--codec must be h264 or h265");
    }

    return options;
}

std::vector<CameraConfig> LoadCameraConfigList(const fs::path& yaml_path) {
    std::error_code error;
    if (!fs::exists(yaml_path, error)) {
        throw std::runtime_error("camera config yaml not found: " + yaml_path.string());
    }

    const YAML::Node root = YAML::LoadFile(yaml_path.string());
    const YAML::Node cameras = root["cameras"];
    if (!cameras || !cameras.IsSequence() || cameras.size() == 0) {
        throw std::runtime_error("camera config yaml must contain non-empty 'cameras' sequence: " + yaml_path.string());
    }

    std::vector<CameraConfig> configs;
    configs.reserve(cameras.size());
    std::set<std::string> names;
    for (size_t index = 0; index < cameras.size(); ++index) {
        const YAML::Node camera_node = cameras[index];
        if (!camera_node.IsMap()) {
            throw std::runtime_error("cameras[" + std::to_string(index) + "] must be a map in " + yaml_path.string());
        }

        CameraConfig config = ParseCameraConfig(camera_node, index);
        if (!names.insert(config.name).second) {
            throw std::runtime_error("duplicate camera name in yaml: " + config.name);
        }
        configs.push_back(std::move(config));
    }

    return configs;
}

std::string ModeName(CameraRecordMode mode) {
    switch (mode) {
    case CameraRecordMode::DirectCopyH265:
        return "direct-copy-h265";
    case CameraRecordMode::HybridDecodeEncode:
        return "hybrid-decode-encode";
    case CameraRecordMode::StereoHybridDecodeEncode:
        return "stereo-hybrid-decode-encode";
    }
    return "unknown";
}

bool RunStereoDaemon(const Options& options, const std::vector<CameraConfig>& configs) {
    StereoDaemonRunner runner(options, configs);
    return runner.Run();
}

void InstallSignalHandlers() {
    g_stop_requested.store(false, std::memory_order_relaxed);
    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);
}

}  // namespace camera_recorder
