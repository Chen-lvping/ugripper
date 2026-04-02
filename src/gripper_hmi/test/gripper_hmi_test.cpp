#include "gripper_hmi_driver.h"
#include "gripper_hmi_led_effects.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

static constexpr size_t kBtnUpKeyIndex = 0;
static constexpr size_t kBtnDownKeyIndex = 1;

namespace {
std::atomic<bool> g_stop_requested{false};
constexpr int kBeepStateProbeDelayMs = 80;
constexpr int kBeepStateProbeIntervalMs = 100;

template <size_t N>
std::string joinFloatArray(const float (&values)[N])
{
    std::ostringstream stream;
    for (size_t index = 0; index < N; ++index)
    {
        if (index != 0)
        {
            stream << ",";
        }
        stream << values[index];
    }
    return stream.str();
}

void handleSignal(int signal)
{
    if (signal == SIGINT || signal == SIGTERM)
    {
        g_stop_requested.store(true);
    }
}

uint64_t currentSteadyMs()
{
    const auto now = std::chrono::steady_clock::now();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
}

uint64_t currentEpochMs()
{
    const auto now = std::chrono::system_clock::now();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
}
}

struct TestOptions
{
    std::vector<std::string> ports;
    uint32_t baudrate = 115200;
    int durationSec = 10;
    int pollMs = 20;
    bool setLed = false;
    bool enableBeep = false;
    bool customBeep = false;
    bool ledOnly = false;
    bool stateMode = false;
    bool showHelp = false;
    bool readSerialNumber = false;
    bool readCalibration = false;
    bool hasWriteSerialNumber = false;
    int beepDurationMs = 0;
    int beepDuty = static_cast<int>(GripperHmiDriver::kDefaultBeepDuty);
    int beepFreq = static_cast<int>(GripperHmiDriver::kDefaultBeepFrequency);
    std::string serialNumberToWrite;
    std::string writeCalibrationBinPath;
    std::string dumpCalibrationBinPath;
    GripperLedColor color{32, 0, 0};
    GripperLedEffect effect{};
};

static void printUsage(const char *program)
{
    std::cout
        << "Usage: " << program << " [--port PATH]... [--baud N] [--duration SEC] [--poll-ms N] [--rgb R G B] [--beep [--beep-ms N]] [--beep-duty N [--beep-freq N] [--beep-ms N]] [--state NAME] [--led-only]\n"
        << "State examples:\n"
        << "  --state READY\n"
        << "  --state RECORDING\n"
        << "  --state ERROR_1\n"
        << "  --state CALIB_RUN:0.5\n"
        << "Beep example:\n"
        << "  --beep --beep-ms 300\n"
        << "  --beep-duty 50 --beep-freq 1000 --beep-ms 300\n"
        << "  beep uses fixed tested defaults for active buzzer: duty=30 freq=1000\n"
        << "Protocol commands:\n"
        << "  --read-sn\n"
        << "  --write-sn DAG91126320001D6  # encoded into 32-byte field with trailing zeros\n"
        << "  --read-calib [--dump-calib-bin /tmp/umi_calib.bin]\n"
        << "  --write-calib-bin /tmp/umi_calib.bin\n"
        << "Defaults:\n"
        << "  --port /dev/right_gripper\n"
        << "  --port /dev/left_gripper\n"
        << "  --duration 0 means run until Ctrl+C\n";
}

static std::string keyName(int keyIndex)
{
    switch (keyIndex)
    {
    case 0:
        return "BTN_UP";
    case 1:
        return "BTN_DOWN";
    case -1:
        return "BTN_UNKNOWN";
    default:
        return "KEY" + std::to_string(keyIndex);
    }
}

static bool parseInt(const char *text, int *value)
{
    if (text == nullptr || value == nullptr)
    {
        return false;
    }
    char *end = nullptr;
    const long parsed = std::strtol(text, &end, 10);
    if (end == text || *end != '\0')
    {
        return false;
    }
    *value = static_cast<int>(parsed);
    return true;
}

static bool parseOptions(int argc, char **argv, TestOptions *options)
{
    if (options == nullptr)
    {
        return false;
    }

    for (int index = 1; index < argc; ++index)
    {
        const std::string argument = argv[index];
        if (argument == "--help" || argument == "-h")
        {
            options->showHelp = true;
            return true;
        }

        if (argument == "--port")
        {
            if (index + 1 >= argc)
            {
                std::cerr << "Missing value for --port" << std::endl;
                return false;
            }
            options->ports.emplace_back(argv[++index]);
            continue;
        }

        if (argument == "--baud")
        {
            int value = 0;
            if (index + 1 >= argc || !parseInt(argv[++index], &value) || value <= 0)
            {
                std::cerr << "Invalid value for --baud" << std::endl;
                return false;
            }
            options->baudrate = static_cast<uint32_t>(value);
            continue;
        }

        if (argument == "--duration")
        {
            int value = 0;
            if (index + 1 >= argc || !parseInt(argv[++index], &value) || value < 0)
            {
                std::cerr << "Invalid value for --duration" << std::endl;
                return false;
            }
            options->durationSec = value;
            continue;
        }

        if (argument == "--poll-ms")
        {
            int value = 0;
            if (index + 1 >= argc || !parseInt(argv[++index], &value) || value < 0)
            {
                std::cerr << "Invalid value for --poll-ms" << std::endl;
                return false;
            }
            options->pollMs = value;
            continue;
        }

        if (argument == "--rgb")
        {
            int red = 0;
            int green = 0;
            int blue = 0;
            if (index + 3 >= argc ||
                !parseInt(argv[++index], &red) ||
                !parseInt(argv[++index], &green) ||
                !parseInt(argv[++index], &blue) ||
                red < 0 || red > 255 ||
                green < 0 || green > 255 ||
                blue < 0 || blue > 255)
            {
                std::cerr << "Invalid value for --rgb" << std::endl;
                return false;
            }
            options->setLed = true;
            options->color = GripperLedColor{
                static_cast<uint8_t>(red),
                static_cast<uint8_t>(green),
                static_cast<uint8_t>(blue),
            };
            continue;
        }

        if (argument == "--beep")
        {
            options->enableBeep = true;
            continue;
        }

        if (argument == "--beep-duty")
        {
            int value = 0;
            if (index + 1 >= argc || !parseInt(argv[++index], &value) || value < 0 || value > 255)
            {
                std::cerr << "Invalid value for --beep-duty" << std::endl;
                return false;
            }
            options->enableBeep = true;
            options->customBeep = true;
            options->beepDuty = value;
            continue;
        }

        if (argument == "--beep-freq")
        {
            int value = 0;
            if (index + 1 >= argc || !parseInt(argv[++index], &value) || value < 0 || value > 65535)
            {
                std::cerr << "Invalid value for --beep-freq" << std::endl;
                return false;
            }
            options->enableBeep = true;
            options->customBeep = true;
            options->beepFreq = value;
            continue;
        }

        if (argument == "--beep-ms")
        {
            int value = 0;
            if (index + 1 >= argc || !parseInt(argv[++index], &value) || value < 0)
            {
                std::cerr << "Invalid value for --beep-ms" << std::endl;
                return false;
            }
            options->beepDurationMs = value;
            continue;
        }

        if (argument == "--state")
        {
            if (index + 1 >= argc)
            {
                std::cerr << "Missing value for --state" << std::endl;
                return false;
            }
            if (!GripperLedEffectRenderer::parseStateText(argv[++index], &options->effect))
            {
                std::cerr << "Invalid value for --state" << std::endl;
                return false;
            }
            options->stateMode = true;
            continue;
        }

        if (argument == "--led-only")
        {
            options->ledOnly = true;
            continue;
        }

        if (argument == "--read-sn")
        {
            options->readSerialNumber = true;
            continue;
        }

        if (argument == "--write-sn")
        {
            if (index + 1 >= argc)
            {
                std::cerr << "Missing value for --write-sn" << std::endl;
                return false;
            }
            options->serialNumberToWrite = argv[++index];
            options->hasWriteSerialNumber = true;
            continue;
        }

        if (argument == "--read-calib")
        {
            options->readCalibration = true;
            continue;
        }

        if (argument == "--write-calib-bin")
        {
            if (index + 1 >= argc)
            {
                std::cerr << "Missing value for --write-calib-bin" << std::endl;
                return false;
            }
            options->writeCalibrationBinPath = argv[++index];
            continue;
        }

        if (argument == "--dump-calib-bin")
        {
            if (index + 1 >= argc)
            {
                std::cerr << "Missing value for --dump-calib-bin" << std::endl;
                return false;
            }
            options->dumpCalibrationBinPath = argv[++index];
            continue;
        }

        std::cerr << "Unknown argument: " << argument << std::endl;
        return false;
    }

    if (options->ports.empty())
    {
        options->ports.emplace_back("/dev/right_gripper");
        options->ports.emplace_back("/dev/left_gripper");
    }

    return true;
}

static bool readBinaryFile(const std::string &path, std::vector<uint8_t> *buffer)
{
    if (buffer == nullptr)
    {
        return false;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input.is_open())
    {
        return false;
    }

    buffer->assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    return input.good() || input.eof();
}

static bool writeBinaryFile(const std::string &path, const uint8_t *data, size_t size)
{
    if (data == nullptr)
    {
        return false;
    }

    std::ofstream output(path, std::ios::binary);
    if (!output.is_open())
    {
        return false;
    }

    output.write(reinterpret_cast<const char *>(data), static_cast<std::streamsize>(size));
    return output.good();
}

static void printCalibrationSummary(const gripper_hmi::GripperCalibrationDataV1 &data)
{
    const auto &header = data.header;
    std::cout
        << "calibration.magic=" << std::string(header.magic, header.magic + 4) << '\n'
        << "calibration.data_format_version=0x" << std::hex << header.dataFormatVersion << std::dec << '\n'
        << "calibration.payload_size=" << header.payloadSize << '\n'
        << "calibration.header_size=" << header.headerSize << '\n'
        << "calibration.valid_fields=0x" << std::hex << header.validFields << std::dec << '\n'
        << "rgb.camera_model_enum=" << data.rgbCamera.cameraModelEnum << '\n'
        << "rgb.distortion_coefficients=" << joinFloatArray(data.rgbCamera.distortionCoefficients) << '\n'
        << "rgb.intrinsics=" << joinFloatArray(data.rgbCamera.intrinsics) << '\n'
        << "rgb.resolution=" << joinFloatArray(data.rgbCamera.resolution) << '\n'
        << "stereo.cam0.camera_model_enum=" << data.stereoCam0.cameraModelEnum << '\n'
        << "stereo.cam0.focal_length=" << joinFloatArray(data.stereoCam0.focalLength) << '\n'
        << "stereo.cam0.principal_point=" << joinFloatArray(data.stereoCam0.principalPoint) << '\n'
        << "stereo.cam0.distortion_coefficients=" << joinFloatArray(data.stereoCam0.distortionCoefficients) << '\n'
        << "stereo.cam1.camera_model_enum=" << data.stereoCam1.cameraModelEnum << '\n'
        << "stereo.cam1.focal_length=" << joinFloatArray(data.stereoCam1.focalLength) << '\n'
        << "stereo.cam1.principal_point=" << joinFloatArray(data.stereoCam1.principalPoint) << '\n'
        << "stereo.cam1.distortion_coefficients=" << joinFloatArray(data.stereoCam1.distortionCoefficients) << '\n'
        << "extrinsics.t_ic_cam0_to_imu0=" << joinFloatArray(data.extrinsics.tIcCam0ToImu0) << '\n'
        << "extrinsics.timeshift_cam0_to_imu0=" << data.extrinsics.timeshiftCam0ToImu0 << '\n'
        << "extrinsics.t_ic_cam1_to_imu0=" << joinFloatArray(data.extrinsics.tIcCam1ToImu0) << '\n'
        << "extrinsics.timeshift_cam1_to_imu0=" << data.extrinsics.timeshiftCam1ToImu0 << '\n'
        << "extrinsics.baseline_norm=" << data.extrinsics.baselineNorm << '\n'
        << "imu0.update_rate=" << data.imu0.updateRate << '\n'
        << "imu0.accel_noise_density_discrete=" << data.imu0.accelerometerNoiseDensityDiscrete << '\n'
        << "imu0.accel_random_walk=" << data.imu0.accelerometerRandomWalk << '\n'
        << "imu0.gyro_noise_density_discrete=" << data.imu0.gyroscopeNoiseDensityDiscrete << '\n'
        << "imu0.gyro_random_walk=" << data.imu0.gyroscopeRandomWalk << '\n'
        << "residuals.cam0.mean=" << data.residuals.reprojectionErrorCam0Px.mean << '\n'
        << "residuals.cam0.median=" << data.residuals.reprojectionErrorCam0Px.median << '\n'
        << "residuals.cam0.stddev=" << data.residuals.reprojectionErrorCam0Px.stddev << '\n'
        << "residuals.cam1.mean=" << data.residuals.reprojectionErrorCam1Px.mean << '\n'
        << "residuals.cam1.median=" << data.residuals.reprojectionErrorCam1Px.median << '\n'
        << "residuals.cam1.stddev=" << data.residuals.reprojectionErrorCam1Px.stddev << '\n'
        << "residuals.gyro.mean=" << data.residuals.gyroscopeErrorImu0RadS.mean << '\n'
        << "residuals.gyro.median=" << data.residuals.gyroscopeErrorImu0RadS.median << '\n'
        << "residuals.gyro.stddev=" << data.residuals.gyroscopeErrorImu0RadS.stddev << '\n'
        << "residuals.accel.mean=" << data.residuals.accelerometerErrorImu0MS2.mean << '\n'
        << "residuals.accel.median=" << data.residuals.accelerometerErrorImu0MS2.median << '\n'
        << "residuals.accel.stddev=" << data.residuals.accelerometerErrorImu0MS2.stddev << std::endl;
}

int main(int argc, char **argv)
{
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    TestOptions options;
    if (!parseOptions(argc, argv, &options))
    {
        return 1;
    }

    if (options.showHelp)
    {
        printUsage(argv[0]);
        return 0;
    }

    std::vector<std::unique_ptr<GripperHmiDriver>> devices;
    for (size_t index = 0; index < options.ports.size(); ++index)
    {
        auto driver = std::make_unique<GripperHmiDriver>(
            options.ports[index],
            options.baudrate,
            "GripperHmiTest" + std::to_string(index));

        if (!driver->connect())
        {
            std::cerr << "Failed to connect: " << options.ports[index] << std::endl;
            return 1;
        }

        devices.push_back(std::move(driver));
    }

    std::cout << "Connected " << devices.size() << " gripper HMI device(s)" << std::endl;

    if (options.readSerialNumber || options.hasWriteSerialNumber ||
        options.readCalibration || !options.writeCalibrationBinPath.empty())
    {
        bool allOk = true;

        if (options.hasWriteSerialNumber)
        {
            for (auto &device : devices)
            {
                if (!device->writeSerialNumber(options.serialNumberToWrite))
                {
                    std::cerr << "Failed to write SN on: " << device->getPort();
                    if (!device->getLastCommandError().empty())
                    {
                        std::cerr << " reason=" << device->getLastCommandError();
                    }
                    std::cerr << std::endl;
                    allOk = false;
                }
                else
                {
                    std::cout << '[' << device->getPort() << "] write-sn ok: " << options.serialNumberToWrite << std::endl;
                }
            }
        }

        if (options.readSerialNumber)
        {
            for (auto &device : devices)
            {
                std::string serialNumber;
                if (!device->readSerialNumber(&serialNumber))
                {
                    std::cerr << "Failed to read SN on: " << device->getPort();
                    if (!device->getLastCommandError().empty())
                    {
                        std::cerr << " reason=" << device->getLastCommandError();
                    }
                    std::cerr << std::endl;
                    allOk = false;
                }
                else
                {
                    std::cout << '[' << device->getPort() << "] sn=" << serialNumber << std::endl;
                }
            }
        }

        if (!options.writeCalibrationBinPath.empty())
        {
            std::vector<uint8_t> raw;
            if (!readBinaryFile(options.writeCalibrationBinPath, &raw))
            {
                std::cerr << "Failed to read calibration binary: " << options.writeCalibrationBinPath << std::endl;
                return 1;
            }
            if (raw.size() != gripper_hmi::kCalibrationPayloadSize)
            {
                std::cerr << "Invalid calibration binary size: " << raw.size()
                          << " expected=" << gripper_hmi::kCalibrationPayloadSize << std::endl;
                return 1;
            }

            gripper_hmi::GripperCalibrationDataV1 calibrationData{};
            std::memcpy(&calibrationData, raw.data(), sizeof(calibrationData));
            for (auto &device : devices)
            {
                if (!device->writeCalibrationData(calibrationData))
                {
                    std::cerr << "Failed to write calibration on: " << device->getPort();
                    if (!device->getLastCommandError().empty())
                    {
                        std::cerr << " reason=" << device->getLastCommandError();
                    }
                    std::cerr << std::endl;
                    allOk = false;
                }
                else
                {
                    std::cout << '[' << device->getPort() << "] write-calibration ok" << std::endl;
                }
            }
        }

        if (options.readCalibration)
        {
            for (auto &device : devices)
            {
                gripper_hmi::GripperCalibrationDataV1 calibrationData{};
                if (!device->readCalibrationData(&calibrationData))
                {
                    std::cerr << "Failed to read calibration on: " << device->getPort();
                    if (!device->getLastCommandError().empty())
                    {
                        std::cerr << " reason=" << device->getLastCommandError();
                    }
                    std::cerr << std::endl;
                    allOk = false;
                    continue;
                }

                std::cout << '[' << device->getPort() << "] calibration summary" << std::endl;
                printCalibrationSummary(calibrationData);

                if (!options.dumpCalibrationBinPath.empty())
                {
                    const std::string dumpPath =
                        devices.size() == 1
                            ? options.dumpCalibrationBinPath
                            : options.dumpCalibrationBinPath + "." + device->getName() + ".bin";
                    if (!writeBinaryFile(dumpPath,
                                         reinterpret_cast<const uint8_t *>(&calibrationData),
                                         sizeof(calibrationData)))
                    {
                        std::cerr << "Failed to dump calibration binary: " << dumpPath << std::endl;
                        allOk = false;
                    }
                    else
                    {
                        std::cout << '[' << device->getPort() << "] dumped calibration: " << dumpPath << std::endl;
                    }
                }
            }
        }

        return allOk ? 0 : 1;
    }

    if (options.stateMode)
    {
        std::cout << "LED state mode: " << GripperLedEffectRenderer::stateText(options.effect) << std::endl;
    }
    if (options.enableBeep)
    {
        std::cout << "Beep request: enabled=true"
                  << " mode=" << (options.customBeep ? "custom" : "default")
                  << " duty=" << options.beepDuty
                  << " freq=" << options.beepFreq
                  << " duration_ms=" << options.beepDurationMs
                  << std::endl;
    }

    const auto startedAt = std::chrono::steady_clock::now();
    auto lastStatePrint = startedAt;
    std::array<uint8_t, 3> lastColor{255, 255, 255};
    GripperLedEffectRenderer renderer;
    bool beepApplied = false;
    bool beepSilenced = false;
    auto beepStartedAt = startedAt;
    auto lastBeepStateRequestAt = startedAt - std::chrono::milliseconds(kBeepStateProbeIntervalMs);

    while (!g_stop_requested.load())
    {
        const auto now = std::chrono::steady_clock::now();
        if (options.durationSec > 0)
        {
            const auto elapsedSec = std::chrono::duration_cast<std::chrono::seconds>(now - startedAt).count();
            if (elapsedSec >= options.durationSec)
            {
                break;
            }
        }

        if (options.stateMode)
        {
            const auto rendered = renderer.render(options.effect, currentSteadyMs(), currentEpochMs());
            const std::array<uint8_t, 3> color{rendered.red, rendered.green, rendered.blue};
            if (color != lastColor)
            {
                for (auto &device : devices)
                {
                    if (!device->setLedColor(rendered))
                    {
                        std::cerr << "Failed to set LED on: " << device->getPort() << std::endl;
                    }
                }
                lastColor = color;
            }
        }
        else if (options.setLed)
        {
            for (auto &device : devices)
            {
                if (!device->setLedColor(options.color))
                {
                    std::cerr << "Failed to set LED on: " << device->getPort() << std::endl;
                }
            }
            options.setLed = false;
        }

        if (options.enableBeep && !beepApplied)
        {
            const GripperBeepState beepState{
                static_cast<uint16_t>(options.beepDuty),
                static_cast<uint16_t>(options.beepFreq),
            };
            for (auto &device : devices)
            {
                const bool ok = options.customBeep
                                    ? device->setBeepState(beepState)
                                    : device->setBeepEnabled(true);
                if (!ok)
                {
                    std::cerr << "Failed to enable beep on: " << device->getPort() << std::endl;
                }
            }
            beepApplied = true;
            beepStartedAt = now;
        }

        if (options.enableBeep && !beepSilenced && options.beepDurationMs > 0)
        {
            const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - beepStartedAt).count();
            if (elapsedMs >= options.beepDurationMs)
            {
                for (auto &device : devices)
                {
                    if (!device->silenceBeep())
                    {
                        std::cerr << "Failed to silence beep on: " << device->getPort() << std::endl;
                    }
                }
                beepSilenced = true;
            }
        }

        if (!options.ledOnly)
        {
            bool requestedBeepStateThisLoop = false;
            for (auto &device : devices)
            {
                bool shouldRequestState = true;
                if (options.enableBeep)
                {
                    const auto elapsedSinceBeepMs =
                        std::chrono::duration_cast<std::chrono::milliseconds>(now - beepStartedAt).count();
                    const auto elapsedSinceLastProbeMs =
                        std::chrono::duration_cast<std::chrono::milliseconds>(now - lastBeepStateRequestAt).count();
                    shouldRequestState = beepApplied &&
                                         elapsedSinceBeepMs >= kBeepStateProbeDelayMs &&
                                         elapsedSinceLastProbeMs >= kBeepStateProbeIntervalMs;
                }

                if (shouldRequestState)
                {
                    device->requestState();
                    if (options.enableBeep)
                    {
                        requestedBeepStateThisLoop = true;
                    }
                }
                GripperKeyReport report;
                if (device->waitForKeyChange(options.pollMs, &report))
                {
                    std::cout
                        << '[' << device->getPort() << "] key=" << keyName(report.keyIndex) << "(" << report.keyIndex << ")"
                        << " raw=0x" << std::hex << static_cast<int>(report.rawCode) << std::dec
                        << (report.pressed ? " pressed" : " released")
                        << std::endl;
                }
                else
                {
                    device->pollOnce(0);
                }
            }

            if (options.enableBeep && requestedBeepStateThisLoop)
            {
                lastBeepStateRequestAt = now;
            }

            const int statePrintIntervalMs = options.enableBeep ? 100 : 1000;
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastStatePrint).count() >= statePrintIntervalMs)
            {
                lastStatePrint = now;
                for (auto &device : devices)
                {
                    const auto snapshot = device->getSnapshot();
                    std::cout
                        << '[' << device->getPort() << "] active=" << (snapshot.active ? "yes" : "no")
                        << " age_ms=" << snapshot.lastRxAgeMs
                        << " beep_duty=" << snapshot.beepState.duty
                        << " beep_freq=" << snapshot.beepState.frequency
                        << " btn_up=" << (snapshot.keyPressed[kBtnUpKeyIndex] ? 1 : 0)
                        << " btn_down=" << (snapshot.keyPressed[kBtnDownKeyIndex] ? 1 : 0)
                        << " extra=";
                    for (size_t keyIndex = 0; keyIndex < snapshot.keyPressed.size(); ++keyIndex)
                    {
                        if (keyIndex == kBtnUpKeyIndex || keyIndex == kBtnDownKeyIndex)
                        {
                            continue;
                        }
                        std::cout << keyIndex << ':' << (snapshot.keyPressed[keyIndex] ? '1' : '0') << ' ';
                    }
                    std::cout << std::endl;
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(
            options.stateMode ? static_cast<int>(GripperLedEffectRenderer::recommendedRenderIntervalMs())
                              : std::max(1, options.pollMs)));
    }

    return 0;
}
