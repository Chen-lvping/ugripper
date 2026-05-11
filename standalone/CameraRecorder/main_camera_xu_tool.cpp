#include "main_camera_calibration_data.h"

#include <fcntl.h>
#include <linux/usb/video.h>
#include <linux/uvcvideo.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace
{

constexpr uint8_t kXuUnitId = 0x03;
constexpr uint8_t kXuSelector = 0x17;
constexpr uint16_t kXuPacketSize = 9;
constexpr size_t kPayloadChunkSize = 8;
constexpr size_t kPayloadChunkCount = main_camera::kCalibrationPayloadSize / kPayloadChunkSize;

enum class BandwidthMode : uint8_t
{
    kHighBandwidth = 0x00,
    kLowBandwidth = 0x01,
};

struct Options
{
    std::string devicePath = "/dev/right_cam_main";
    bool showHelp = false;
    bool readCalibration = false;
    bool setBandwidth = false;
    bool dumpRaw = false;
    bool writeRaw = false;
    std::string dumpPath;
    std::string writeRawPath;
    BandwidthMode bandwidthMode = BandwidthMode::kHighBandwidth;
};

class CameraXuDevice
{
public:
    explicit CameraXuDevice(const std::string &devicePath)
        : devicePath_(devicePath)
    {
        fd_ = open(devicePath.c_str(), O_RDWR | O_NONBLOCK);
        if (fd_ < 0)
        {
            throw std::runtime_error("failed to open device: " + devicePath);
        }
    }

    ~CameraXuDevice()
    {
        if (fd_ >= 0)
        {
            close(fd_);
        }
    }

    CameraXuDevice(const CameraXuDevice &) = delete;
    CameraXuDevice &operator=(const CameraXuDevice &) = delete;

    void setBandwidth(BandwidthMode mode)
    {
        std::array<uint8_t, kXuPacketSize> command = {0x97, 0x97, 0x97, 0x97, 0x97, 0x97, 0x97, 0x02, 0x01};
        command[8] = static_cast<uint8_t>(mode);
        xuSet(command);
    }

    std::array<uint8_t, main_camera::kCalibrationPayloadSize> readCalibrationRaw()
    {
        std::array<uint8_t, main_camera::kCalibrationPayloadSize> payload = {};
        std::array<uint8_t, kXuPacketSize> readIndexCommand = {0x97, 0x97, 0x97, 0x97, 0x97, 0x97, 0x97, 0x01, 0x00};
        std::array<uint8_t, kXuPacketSize> response = {};

        for (size_t chunkIndex = 0; chunkIndex < kPayloadChunkCount; ++chunkIndex)
        {
            readIndexCommand[8] = static_cast<uint8_t>(chunkIndex);
            xuSet(readIndexCommand);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            response.fill(0);
            xuGet(&response);
            std::memcpy(payload.data() + chunkIndex * kPayloadChunkSize, response.data() + 1, kPayloadChunkSize);
        }

        return payload;
    }

    void writeCalibrationRaw(const std::array<uint8_t, main_camera::kCalibrationPayloadSize> &payload)
    {
        std::array<uint8_t, kXuPacketSize> packet = {};
        for (size_t chunkIndex = 0; chunkIndex < kPayloadChunkCount; ++chunkIndex)
        {
            packet[0] = static_cast<uint8_t>(chunkIndex);
            std::memcpy(packet.data() + 1, payload.data() + chunkIndex * kPayloadChunkSize, kPayloadChunkSize);
            xuSet(packet);
        }
    }

    void commitCalibrationToFlash()
    {
        std::array<uint8_t, kXuPacketSize> command = {0x96, 0x96, 0x96, 0x96, 0x96, 0x96, 0x96, 0x96, 0x96};
        xuSet(command);
        std::this_thread::sleep_for(std::chrono::seconds(3));
    }

private:
    void xuSet(std::array<uint8_t, kXuPacketSize> &buffer)
    {
        struct uvc_xu_control_query query = {};
        query.unit = kXuUnitId;
        query.selector = kXuSelector;
        query.query = UVC_SET_CUR;
        query.size = kXuPacketSize;
        query.data = buffer.data();
        if (ioctl(fd_, UVCIOC_CTRL_QUERY, &query) < 0)
        {
            throw std::runtime_error("UVC SET_CUR failed on " + devicePath_);
        }
    }

    void xuGet(std::array<uint8_t, kXuPacketSize> *buffer)
    {
        struct uvc_xu_control_query query = {};
        query.unit = kXuUnitId;
        query.selector = kXuSelector;
        query.query = UVC_GET_CUR;
        query.size = kXuPacketSize;
        query.data = buffer->data();
        if (ioctl(fd_, UVCIOC_CTRL_QUERY, &query) < 0)
        {
            throw std::runtime_error("UVC GET_CUR failed on " + devicePath_);
        }
    }

    std::string devicePath_;
    int fd_ = -1;
};

uint32_t fnv1a32(const uint8_t *data, size_t size)
{
    uint32_t hash = 2166136261u;
    for (size_t index = 0; index < size; ++index)
    {
        hash ^= static_cast<uint32_t>(data[index]);
        hash *= 16777619u;
    }
    return hash;
}

std::array<uint8_t, main_camera::kCalibrationPayloadSize>
serialize(const main_camera::MainCameraCalibrationDataV1 &data)
{
    std::array<uint8_t, main_camera::kCalibrationPayloadSize> raw = {};
    std::memcpy(raw.data(), &data, sizeof(data));
    return raw;
}

main_camera::MainCameraCalibrationDataV1
deserialize(const std::array<uint8_t, main_camera::kCalibrationPayloadSize> &raw)
{
    main_camera::MainCameraCalibrationDataV1 data{};
    std::memcpy(&data, raw.data(), sizeof(data));
    return data;
}

void writeBinaryFile(const std::string &path,
                     const std::array<uint8_t, main_camera::kCalibrationPayloadSize> &payload)
{
    std::ofstream output(path, std::ios::binary);
    if (!output)
    {
        throw std::runtime_error("failed to open dump path: " + path);
    }
    output.write(reinterpret_cast<const char *>(payload.data()), static_cast<std::streamsize>(payload.size()));
    if (!output)
    {
        throw std::runtime_error("failed to write dump path: " + path);
    }
}

std::array<uint8_t, main_camera::kCalibrationPayloadSize> readBinaryFile(const std::string &path)
{
    std::array<uint8_t, main_camera::kCalibrationPayloadSize> payload = {};
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        throw std::runtime_error("failed to open input path: " + path);
    }
    input.read(reinterpret_cast<char *>(payload.data()), static_cast<std::streamsize>(payload.size()));
    if (input.gcount() != static_cast<std::streamsize>(payload.size()))
    {
        throw std::runtime_error("input payload must be exactly 1024 bytes: " + path);
    }
    return payload;
}

void printCalibrationSummary(const main_camera::MainCameraCalibrationDataV1 &data)
{
    const uint32_t checksum = fnv1a32(reinterpret_cast<const uint8_t *>(&data.mainCamera), sizeof(data.mainCamera));
    uint32_t storedChecksum = 0;
    std::memcpy(&storedChecksum, data.header.reserved, sizeof(storedChecksum));

    std::cout << "magic=" << std::string(data.header.magic, data.header.magic + 4) << '\n'
              << "data_format_version=0x" << std::hex << data.header.dataFormatVersion << std::dec << '\n'
              << "payload_size=" << data.header.payloadSize << '\n'
              << "header_size=" << data.header.headerSize << '\n'
              << "valid_fields=0x" << std::hex << data.header.validFields << std::dec << '\n'
              << "camera_model_enum=" << data.mainCamera.cameraModelEnum << '\n'
              << "distortion_coeffs="
              << data.mainCamera.distortionCoefficients[0] << ','
              << data.mainCamera.distortionCoefficients[1] << ','
              << data.mainCamera.distortionCoefficients[2] << ','
              << data.mainCamera.distortionCoefficients[3] << '\n'
              << "intrinsics="
              << data.mainCamera.intrinsics[0] << ','
              << data.mainCamera.intrinsics[1] << ','
              << data.mainCamera.intrinsics[2] << ','
              << data.mainCamera.intrinsics[3] << '\n'
              << "resolution="
              << data.mainCamera.resolution[0] << 'x'
              << data.mainCamera.resolution[1] << '\n'
              << "stored_checksum=0x" << std::hex << storedChecksum << std::dec << '\n'
              << "computed_checksum=0x" << std::hex << checksum << std::dec << '\n';
}

void printUsage(const char *program)
{
    std::cout
        << "Usage: " << program << " [options]\n"
        << "  --device /dev/right_cam_main\n"
        << "  --read-calib\n"
        << "  --dump-raw /tmp/right_main_camera_calib.bin\n"
        << "  --write-raw /tmp/right_main_camera_calib.bin\n"
        << "  --set-bandwidth high|low\n";
}

bool parseOptions(int argc, char **argv, Options *options)
{
    for (int index = 1; index < argc; ++index)
    {
        const std::string arg = argv[index];
        if (arg == "--help" || arg == "-h")
        {
            options->showHelp = true;
            return true;
        }
        if (arg == "--device" && index + 1 < argc)
        {
            options->devicePath = argv[++index];
            continue;
        }
        if (arg == "--read-calib")
        {
            options->readCalibration = true;
            continue;
        }
        if (arg == "--dump-raw" && index + 1 < argc)
        {
            options->dumpRaw = true;
            options->dumpPath = argv[++index];
            continue;
        }
        if (arg == "--write-raw" && index + 1 < argc)
        {
            options->writeRaw = true;
            options->writeRawPath = argv[++index];
            continue;
        }
        if (arg == "--set-bandwidth" && index + 1 < argc)
        {
            options->setBandwidth = true;
            const std::string mode = argv[++index];
            if (mode == "high")
            {
                options->bandwidthMode = BandwidthMode::kHighBandwidth;
            }
            else if (mode == "low")
            {
                options->bandwidthMode = BandwidthMode::kLowBandwidth;
            }
            else
            {
                std::cerr << "invalid bandwidth mode: " << mode << std::endl;
                return false;
            }
            continue;
        }

        std::cerr << "unknown argument: " << arg << std::endl;
        return false;
    }

    return true;
}
}  // namespace

int main(int argc, char **argv)
{
    try
    {
        Options options;
        if (!parseOptions(argc, argv, &options))
        {
            printUsage(argv[0]);
            return 1;
        }
        if (options.showHelp)
        {
            printUsage(argv[0]);
            return 0;
        }

        CameraXuDevice device(options.devicePath);

        if (options.setBandwidth)
        {
            device.setBandwidth(options.bandwidthMode);
            std::cout << "set_bandwidth=ok mode="
                      << (options.bandwidthMode == BandwidthMode::kHighBandwidth ? "high_bandwidth" : "low_bandwidth")
                      << std::endl;
        }

        if (options.writeRaw)
        {
            const auto payload = readBinaryFile(options.writeRawPath);
            device.writeCalibrationRaw(payload);
            device.commitCalibrationToFlash();
            std::cout << "write_raw=ok path=" << options.writeRawPath << std::endl;
        }

        if (options.readCalibration || options.dumpRaw)
        {
            const auto payload = device.readCalibrationRaw();
            if (options.dumpRaw)
            {
                writeBinaryFile(options.dumpPath, payload);
                std::cout << "dump_raw=ok path=" << options.dumpPath << std::endl;
            }
            if (options.readCalibration)
            {
                printCalibrationSummary(deserialize(payload));
            }
        }

        return 0;
    }
    catch (const std::exception &ex)
    {
        std::cerr << ex.what() << std::endl;
        return 1;
    }
}
