#include "encoder_driver.h"
#include "sensor_recorder/logging_compat.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>

std::atomic<bool> g_stopFlag(false);

std::string resolveEncoderPort(const std::string &argument) {
    if (argument == "left" || argument == "--left") {
        return "/dev/left_encoder";
    }
    if (argument == "right" || argument == "--right") {
        return "/dev/right_encoder";
    }
    return argument;
}

std::string resolveEncoderLabel(const std::string &argument) {
    if (argument == "left" || argument == "--left" || argument == "/dev/left_encoder") {
        return "left";
    }
    if (argument == "right" || argument == "--right" || argument == "/dev/right_encoder") {
        return "right";
    }
    return argument;
}

void signalHandler(int signum) {
    DM_LOG_INFO_STREAM() << "Interrupt signal (" << signum << ") received. Stopping...";
    g_stopFlag = true;
}

void encoderReadThreadFunc(EncoderDriver *encoder) {
    uint8_t readBuf[256];
    while (!g_stopFlag.load()) {
        if (!encoder->isConnected()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        int bytesRead = encoder->readDataNonBlocking(readBuf, sizeof(readBuf));
        if (bytesRead > 0) {
            const auto framesNum = encoder->parseReceivedData(readBuf, bytesRead);
            if (framesNum) {
                encoder->updateActiveStatus();
            }
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }
}

EncoderData sampleFinalState(EncoderDriver *encoder, std::chrono::milliseconds duration, bool *sampled) {
    EncoderData lastState = encoder->getState();
    const auto deadline = std::chrono::steady_clock::now() + duration;
    bool gotSample = false;

    while (!g_stopFlag.load() && std::chrono::steady_clock::now() < deadline) {
        encoder->requestState(false);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        lastState = encoder->getState();
        gotSample = true;
    }

    if (sampled != nullptr) {
        *sampled = gotSample;
    }

    return lastState;
}

int main(int argc, char *argv[]) {
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    if (argc < 2) {
        std::cerr << "Usage: zeroing <left|right|/dev/encoder_path>" << std::endl;
        return 1;
    }

    const std::string firstArg = argv[1];
    if (firstArg == "-h" || firstArg == "--help") {
        std::cout << "Usage: zeroing <left|right|/dev/encoder_path>" << std::endl;
        return 0;
    }

    std::cout << "--- Encoder Zeroing Tool ---" << std::endl;

    const std::string encoderLabel = resolveEncoderLabel(argv[1]);
    const std::string encoderPort = resolveEncoderPort(argv[1]);
    std::cout << "Encoder side: " << encoderLabel << std::endl;
    std::cout << "Using encoder port: " << encoderPort << std::endl;

    EncoderDriver encoder(1, encoderPort, 1000000, "Joint1_Encoder");

    DM_LOG_INFO("Connecting to encoder...");
    auto status = encoder.connect();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    if (status == ConnectStatus::SERIAL_FAIL) {
        DM_LOG_ERROR("Error: Serial port open failed!");
        return -1;
    } else if (status == ConnectStatus::NO_RESPONSE) {
        DM_LOG_WARN("1Mbps not responding, trying 115200...");

        encoder.disconnect();
        encoder.resetBaudrate(115200);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        status = encoder.connect();
        if (status == ConnectStatus::SUCCESS) {
            DM_LOG_INFO("Encoder found at 115200. Switching encoder to 1Mbps...");
            encoder.setBaudrate(1000000);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            DM_LOG_INFO("Reconnecting host to 1Mbps...");
            encoder.disconnect();
            encoder.resetBaudrate(1000000);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            status = encoder.connect();
            if (status == ConnectStatus::SUCCESS) {
                DM_LOG_INFO("Baudrate negotiation successful (1Mbps).");
            } else {
                DM_LOG_ERROR("Failed to reconnect at 1Mbps after baudrate reset.");
                return -1;
            }
        } else {
            DM_LOG_ERROR("Failed to connect at 115200. Encoder not responding.");
            return -1;
        }
    } else {
        DM_LOG_INFO("Encoder connected ready at 1Mbps.");
    }

    std::thread encoderReadThread(encoderReadThreadFunc, &encoder);

    DM_LOG_INFO("Sending Zero Position Command...");
    const bool zeroSuccess = encoder.setCurrentAsZero();

    if (zeroSuccess) {
        bool sampled = false;
        EncoderData state = sampleFinalState(&encoder, std::chrono::milliseconds(500), &sampled);
        std::cout << "--------------------------------" << std::endl;
        std::cout << " [SUCCESS] Encoder Zeroed." << std::endl;
        std::cout << " Encoder Side: " << encoderLabel << std::endl;
        std::cout << " Sample Window: 0.5s" << std::endl;
        if (sampled) {
            std::cout << " Last Pos (Raw): " << state.currentPosition << std::endl;
            std::cout << " Last Pos (Rad): " << state.currentPositionRad << std::endl;
        } else {
            std::cout << " Last Pos: unavailable" << std::endl;
        }
        std::cout << "--------------------------------" << std::endl;
    } else {
        DM_LOG_ERROR_STREAM() << "[FAILED] Failed to set zero position for encoder side " << encoderLabel
                              << "! (Or verify failed)";
    }

    g_stopFlag = true;
    if (encoderReadThread.joinable()) {
        encoderReadThread.join();
    }

    encoder.disconnect();
    return zeroSuccess ? 0 : 2;
}
