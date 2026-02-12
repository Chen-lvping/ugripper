#include "encoder_driver.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

std::atomic<bool> g_stopFlag(false);

void signalHandler(int signum) {
    std::cout << "\nInterrupt signal (" << signum << ") received. Stopping..." << std::endl;
    g_stopFlag = true;
}

void encoderReadThreadFunc(EncoderDriver* encoder) {
    uint8_t readBuf[256];
    while (!g_stopFlag.load()) {
        if (!encoder->isConnected()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        int bytesRead = encoder->readDataNonBlocking(readBuf, sizeof(readBuf));
        if (bytesRead > 0) {
            auto frames_num = encoder->parseReceivedData(readBuf, bytesRead);
            if (frames_num) {
                encoder->updateActiveStatus();
            }
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }
}

int main() {
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    std::cout << "--- Encoder Zeroing Tool ---" << std::endl;

    EncoderDriver encoder(1, "/dev/ttyS7", 1000000, "Joint1_Encoder");

    std::cout << "Connecting to encoder..." << std::endl;
    auto status = encoder.connect();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    if (status == ConnectStatus::SERIAL_FAIL) {
        std::cerr << "Error: Serial port open failed!" << std::endl;
        return -1;
    } else if (status == ConnectStatus::NO_RESPONSE) {
        std::cout << "1Mbps not responding, trying 115200..." << std::endl;

        encoder.disconnect();
        encoder.resetBaudrate(115200);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        status = encoder.connect();
        if (status == ConnectStatus::SUCCESS) {
            std::cout << "Encoder found at 115200. Switching encoder to 1Mbps..." << std::endl;
            encoder.setBaudrate(1000000);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            std::cout << "Reconnecting host to 1Mbps..." << std::endl;
            encoder.disconnect();
            encoder.resetBaudrate(1000000);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            status = encoder.connect();
            if (status == ConnectStatus::SUCCESS) {
                std::cout << "Baudrate negotiation successful (1Mbps)." << std::endl;
            } else {
                std::cerr << "Failed to reconnect at 1Mbps after baudrate reset." << std::endl;
                return -1;
            }
        } else {
            std::cerr << "Failed to connect at 115200. Encoder not responding." << std::endl;
            return -1;
        }
    } else {
        std::cout << "Encoder connected ready at 1Mbps." << std::endl;
    }

    std::thread encoderReadThread(encoderReadThreadFunc, &encoder);

    std::cout << "Sending Zero Position Command..." << std::endl;
    bool zeroSuccess = encoder.setCurrentAsZero();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    if (zeroSuccess) {
        EncoderData state = encoder.getState();
        std::cout << "--------------------------------" << std::endl;
        std::cout << " [SUCCESS] Encoder Zeroed." << std::endl;
        std::cout << " Current Pos (Raw): " << state.currentPosition << std::endl;
        std::cout << " Current Pos (Rad): " << state.currentPositionRad << std::endl;
        std::cout << "--------------------------------" << std::endl;
    } else {
        std::cerr << " [FAILED] Failed to set zero position! (Or verify failed)" << std::endl;
    }

    g_stopFlag = true;
    if (encoderReadThread.joinable()) {
        encoderReadThread.join();
    }

    encoder.disconnect();
    return 0;
}
