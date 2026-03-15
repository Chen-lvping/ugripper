#include "gripper_hmi_driver.h"
#include "gripper_hmi_led_effects.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

static constexpr size_t kBtnUpKeyIndex = 0;
static constexpr size_t kBtnDownKeyIndex = 1;

namespace {
std::atomic<bool> g_stop_requested{false};

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
    bool ledOnly = false;
    bool stateMode = false;
    bool showHelp = false;
    GripperLedColor color{32, 0, 0};
    GripperLedEffect effect{};
};

static void printUsage(const char *program)
{
    std::cout
        << "Usage: " << program << " [--port PATH]... [--baud N] [--duration SEC] [--poll-ms N] [--rgb R G B] [--state NAME] [--led-only]\n"
        << "State examples:\n"
        << "  --state READY\n"
        << "  --state RECORDING\n"
        << "  --state ERROR_1\n"
        << "  --state CALIB_RUN:0.5\n"
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
    if (options.stateMode)
    {
        std::cout << "LED state mode: " << GripperLedEffectRenderer::stateText(options.effect) << std::endl;
    }

    const auto startedAt = std::chrono::steady_clock::now();
    auto lastStatePrint = startedAt;
    std::array<uint8_t, 3> lastColor{255, 255, 255};
    GripperLedEffectRenderer renderer;

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

        if (!options.ledOnly)
        {
            for (auto &device : devices)
            {
                device->requestState();
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

            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastStatePrint).count() >= 1000)
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
