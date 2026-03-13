#include "record_runtime.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cctype>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {
constexpr const char *kCameraStreamsCsv = "left_cam_main,right_cam_main,left_stereo,right_stereo,left_tcam_l,left_tcam_r,right_tcam_l,right_tcam_r";
constexpr uint64_t kActionDebounceMs = 250;
constexpr uint64_t kLongPressThresholdMs = 800;
constexpr uint64_t kDualLongPressThresholdMs = 4000;
constexpr uint64_t kShutdownPromptThresholdMs = 2000;

std::string joinArguments(const std::vector<std::string> &arguments)
{
    std::ostringstream stream;
    for (size_t index = 0; index < arguments.size(); ++index)
    {
        if (index > 0)
        {
            stream << ' ';
        }
        stream << arguments[index];
    }
    return stream.str();
}

std::string makeTimestampString()
{
    const auto now = std::chrono::system_clock::now();
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    return std::to_string(nowMs);
}

bool commandExists(const std::string &command)
{
    if (command.empty())
    {
        return false;
    }
    if (command.find('/') != std::string::npos)
    {
        return access(command.c_str(), X_OK) == 0;
    }

    const char *pathEnv = std::getenv("PATH");
    if (pathEnv == nullptr)
    {
        return false;
    }

    std::stringstream stream(pathEnv);
    std::string directory;
    while (std::getline(stream, directory, ':'))
    {
        if (directory.empty())
        {
            directory = ".";
        }
        const fs::path candidate = fs::path(directory) / command;
        if (access(candidate.c_str(), X_OK) == 0)
        {
            return true;
        }
    }
    return false;
}
}

GripperLedEffect RecordRuntime::makeLedEffect(LedState state, double progress)
{
    switch (state)
    {
    case RecordRuntime::LedState::Init:
        return {GripperLedEffectState::Init, progress};
    case RecordRuntime::LedState::Ready:
        return {GripperLedEffectState::Ready, progress};
    case RecordRuntime::LedState::Recording:
        return {GripperLedEffectState::Recording, progress};
    case RecordRuntime::LedState::Error1:
        return {GripperLedEffectState::Error1, progress};
    case RecordRuntime::LedState::Error2:
        return {GripperLedEffectState::Error2, progress};
    case RecordRuntime::LedState::Error3:
        return {GripperLedEffectState::Error3, progress};
    case RecordRuntime::LedState::Error4:
        return {GripperLedEffectState::Error4, progress};
    case RecordRuntime::LedState::Error5:
        return {GripperLedEffectState::Error5, progress};
    case RecordRuntime::LedState::CalibPre:
        return {GripperLedEffectState::CalibPre, progress};
    case RecordRuntime::LedState::CalibRun:
        return {GripperLedEffectState::CalibRun, progress};
    case RecordRuntime::LedState::CalibDone:
        return {GripperLedEffectState::CalibDone, progress};
    case RecordRuntime::LedState::Exit:
    default:
        return {GripperLedEffectState::Exit, progress};
    }
}

RecordRuntime::RecordRuntime(RecordRuntimeOptions options)
    : options_(std::move(options))
{
}

RecordRuntime::~RecordRuntime()
{
    requestStop();
    stopRecording(false, "shutdown");
    stopAudioPlayer();
    if (ledController_)
    {
        ledController_->stop();
    }
    panelManager_.turnOff();
    panelManager_.disconnect();
}

bool RecordRuntime::initialize()
{
    deviceSn_ = readEnvValue(options_.envFile, "DEVICE_SN");
    if (deviceSn_.empty())
    {
        deviceSn_ = "noname_device";
    }

    options_.cameraCodec = toLower(readEnvValue(options_.envFile, "CAMERA_CODEC"));
    if (options_.cameraCodec.empty())
    {
        options_.cameraCodec = "h264";
    }
    if (options_.cameraCodec != "h264" && options_.cameraCodec != "h265")
    {
        std::cerr << "[WARN] invalid CAMERA_CODEC, fallback to h264: " << options_.cameraCodec << std::endl;
        options_.cameraCodec = "h264";
    }

    if (options_.gripperPorts.empty())
    {
        options_.gripperPorts.emplace_back("/dev/right_gripper");
        options_.gripperPorts.emplace_back("/dev/left_gripper");
    }

    std::error_code error;
    fs::create_directories(options_.audioTempDir, error);

    packageVersion_ = queryPackageVersion("ugripper");
    updaterVersion_ = queryPackageVersion("ugripper-usb-updater");

    episodeManager_ = std::make_unique<EpisodeManager>(
        options_.diskRoot,
        deviceSn_,
        options_.cameraCodec,
        options_.persistCalibrationFile,
        options_.fallbackCalibrationFile,
        packageVersion_,
        updaterVersion_);

    if (!episodeManager_->initialize())
    {
        std::cerr << "[ERROR] failed to initialize episode manager for disk root: "
                  << options_.diskRoot << std::endl;
        return false;
    }

    if (!panelManager_.connect(options_.gripperPorts))
    {
        std::cerr << "[ERROR] failed to connect any gripper HMI port" << std::endl;
        return false;
    }

    ledController_ = std::make_unique<HmiLedController>(&panelManager_);
    ledController_->start();
    setLedState(LedState::Init);

    startAudioPlayer();

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    setLedState(LedState::Ready);
    sendAudioCommand("ready");

    initialized_ = true;
    return true;
}

int RecordRuntime::run()
{
    if (!initialized_)
    {
        return 1;
    }

    std::cout << "[INFO] record_runtime started" << std::endl;
    std::cout << "[INFO] device_sn=" << deviceSn_ << std::endl;
    std::cout << "[INFO] episode_root=" << episodeManager_->dataRoot() << std::endl;

    while (!stopRequested_.load())
    {
        ButtonSnapshot buttons;
        panelManager_.poll(options_.pollMs, &buttons);
        handleButtons(buttons);

        if (isRecording_ && !checkRecorderProcesses())
        {
            stopRecording(true, "camera or sensor recorder exited unexpectedly");
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(options_.pollMs));
    }

    stopRecording(false, "shutdown");
    setLedState(LedState::Exit);
    return 0;
}

void RecordRuntime::requestStop()
{
    stopRequested_.store(true);
}

bool RecordRuntime::startAudioPlayer()
{
    if (!fs::exists(options_.audioPlayScript))
    {
        std::cerr << "[WARN] audio play script not found: " << options_.audioPlayScript << std::endl;
        return false;
    }

    std::error_code error;
    fs::remove(options_.audioReadyFile, error);

    std::vector<std::string> audioArguments;
    if (fs::exists("./.venv/bin/python3"))
    {
        audioArguments = {"./.venv/bin/python3", options_.audioPlayScript};
    }
    else if (commandExists("uv"))
    {
        audioArguments = {"uv", "run", "python3", options_.audioPlayScript};
    }
    else
    {
        audioArguments = {"python3", options_.audioPlayScript};
    }

    if (!audioPlayer_.start(audioArguments))
    {
        std::cerr << "[WARN] failed to launch audio player, sound prompts disabled" << std::endl;
        return false;
    }

    const uint64_t deadlineMs = currentSteadyMs() + 5000;
    while (currentSteadyMs() < deadlineMs)
    {
        if (!audioPlayer_.isRunning())
        {
            std::cerr << "[WARN] audio player exited before ready, last_exit=" << audioPlayer_.lastExitCode() << std::endl;
            audioPlayerStarted_ = false;
            return false;
        }
        if (fs::exists(options_.audioPipe) && fs::exists(options_.audioReadyFile))
        {
            audioPlayerStarted_ = true;
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    std::cerr << "[WARN] audio player ready timeout, pipe or ready marker missing" << std::endl;
    audioPlayer_.stop(1000);
    audioPlayerStarted_ = false;
    return false;
}

void RecordRuntime::stopAudioPlayer()
{
    if (audioPlayerStarted_ && audioPlayer_.isRunning())
    {
        sendAudioCommand("exit");
        audioPlayer_.wait(1000);
    }
    audioPlayer_.stop(1000);
    audioPlayerStarted_ = false;
}

void RecordRuntime::sendAudioCommand(const std::string &command) const
{
    if (command.empty() || !audioPlayerStarted_)
    {
        return;
    }

    const int fd = open(options_.audioPipe.c_str(), O_WRONLY | O_NONBLOCK);
    if (fd < 0)
    {
        if (command != "exit")
        {
            std::cerr << "[WARN] failed to open audio pipe for command '" << command << "': " << std::strerror(errno) << std::endl;
        }
        return;
    }

    const std::string payload = command + "\n";
    const ssize_t written = write(fd, payload.data(), payload.size());
    if (written < 0)
    {
        std::cerr << "[WARN] failed to write audio command '" << command << "': " << std::strerror(errno) << std::endl;
    }
    close(fd);
}

bool RecordRuntime::attachPendingPreAudio(const std::string &episodeDir)
{
    if (pendingPreAudioFile_.empty() || !fs::exists(pendingPreAudioFile_))
    {
        return true;
    }

    std::error_code error;
    const fs::path target = fs::path(episodeDir) / "audio_pre.wav";
    fs::rename(pendingPreAudioFile_, target, error);
    if (error)
    {
        std::cerr << "[WARN] failed to move pre audio into episode: " << error.message() << std::endl;
        return false;
    }

    pendingPreAudioFile_.clear();
    return true;
}

bool RecordRuntime::startRecording(bool resetRecording)
{
    currentEpisodeDir_ = episodeManager_->createNextEpisodeDir();
    if (currentEpisodeDir_.empty())
    {
        std::cerr << "[ERROR] failed to create episode directory" << std::endl;
        setLedState(LedState::Error5);
        sendAudioCommand("error");
        return false;
    }

    std::string errorMessage;
    const std::string resetSource = resetRecording ? lastEpisodeDir_ : std::string();
    if (!episodeManager_->prepareEpisode(currentEpisodeDir_, resetRecording, resetSource, &errorMessage))
    {
        std::cerr << "[ERROR] prepare episode failed: " << errorMessage << std::endl;
        setLedState(LedState::Error5);
        sendAudioCommand("error");
        return false;
    }

    attachPendingPreAudio(currentEpisodeDir_);

    const std::vector<std::string> cameraArgs = {
        options_.cameraRecorderBin,
        "--codec",
        options_.cameraCodec,
        "--output-dir",
        currentEpisodeDir_,
        "--only",
        kCameraStreamsCsv,
    };
    const std::vector<std::string> sensorArgs = {
        options_.sensorRecorderBin,
        currentEpisodeDir_,
    };

    if (!cameraRecorder_.start(cameraArgs))
    {
        std::cerr << "[ERROR] failed to launch camera recorder" << std::endl;
        setLedState(LedState::Error5);
        sendAudioCommand("error");
        return false;
    }

    if (!sensorRecorder_.start(sensorArgs))
    {
        std::cerr << "[ERROR] failed to launch sensor recorder" << std::endl;
        cameraRecorder_.stop(2000);
        setLedState(LedState::Error5);
        sendAudioCommand("error");
        return false;
    }

    isRecording_ = true;
    setLedState(LedState::Recording);
    sendAudioCommand(resetRecording ? "reset_recording_start" : "recording_start");
    std::cout << "[INFO] recording started: " << currentEpisodeDir_ << std::endl;
    return true;
}

bool RecordRuntime::stopRecording(bool dueToError, const std::string &reason)
{
    if (!isRecording_)
    {
        if (dueToError)
        {
            setLedState(LedState::Error5);
            sendAudioCommand("error");
        }
        return true;
    }

    std::cout << "[INFO] stopping recording: " << reason << std::endl;
    sensorRecorder_.stop(5000);
    cameraRecorder_.stop(5000);

    setLedState(LedState::Ready);
    sendAudioCommand("recording_stop");

    isRecording_ = false;
    if (!currentEpisodeDir_.empty())
    {
        lastEpisodeDir_ = currentEpisodeDir_;
    }

    sendAudioCommand("writing");
    setLedState(LedState::Init);
    ::sync();

    std::string errorMessage;
    const bool valid = episodeManager_->validateEpisode(currentEpisodeDir_, &errorMessage);
    if (!valid)
    {
        std::cerr << "[ERROR] episode validation failed: " << errorMessage << std::endl;
    }

    currentEpisodeDir_.clear();

    if (dueToError)
    {
        setLedState(LedState::Error5);
        sendAudioCommand("error");
        return false;
    }

    if (!valid)
    {
        setLedState(LedState::Error1);
        sendAudioCommand("validation_failed");
        return false;
    }

    setLedState(LedState::Ready);
    sendAudioCommand("ready");
    return true;
}

bool RecordRuntime::handleShortUpAction()
{
    std::cout << "[INFO] BTN_UP short press" << std::endl;
    if (!isRecording_)
    {
        return startRecording(false);
    }
    return stopRecording(false, "BTN_UP short stop");
}

bool RecordRuntime::handleShortDownAction()
{
    std::cout << "[INFO] BTN_DOWN short press" << std::endl;
    if (!isRecording_)
    {
        if (lastEpisodeDir_.empty() || !fs::exists(lastEpisodeDir_))
        {
            std::cout << "[INFO] no previous episode, BTN_DOWN reset ignored" << std::endl;
            sendAudioCommand("no_reset_needed");
            setLedState(LedState::Ready);
            return false;
        }
        return startRecording(true);
    }
    return stopRecording(false, "BTN_DOWN short stop");
}

bool RecordRuntime::handleLongUpAction()
{
    std::cout << "[INFO] BTN_UP long press" << std::endl;
    if (isRecording_)
    {
        std::cout << "[WARN] ignore pre-audio while recording" << std::endl;
        return false;
    }
    return recordAudioClip("pre", true);
}

bool RecordRuntime::handleLongDownAction()
{
    std::cout << "[INFO] BTN_DOWN long press" << std::endl;
    if (isRecording_)
    {
        std::cout << "[WARN] ignore post-audio while recording" << std::endl;
        return false;
    }
    return recordAudioClip("post", false);
}

void RecordRuntime::handleDualShutdownAction()
{
    std::cout << "[INFO] dual-button shutdown requested" << std::endl;
    if (isRecording_)
    {
        stopRecording(false, "dual-button shutdown");
    }
    setLedState(LedState::Exit);
    sendAudioCommand("writing");

    std::ofstream requestFile(options_.shutdownRequestFile, std::ios::trunc);
    if (!requestFile.is_open())
    {
        std::cerr << "[ERROR] failed to create shutdown request file: " << options_.shutdownRequestFile << std::endl;
        setLedState(LedState::Error5);
        return;
    }
    requestFile << "shutdown\n";
    requestFile.close();
    stopRequested_.store(true);
}

bool RecordRuntime::recordAudioClip(const std::string &audioType, bool monitorUpButton)
{
    if (!fs::exists(options_.audioRecordScript))
    {
        std::cerr << "[ERROR] audio record script not found: " << options_.audioRecordScript << std::endl;
        sendAudioCommand("error");
        return false;
    }

    const std::string timestamp = makeTimestampString();
    const fs::path tempDir(options_.audioTempDir);
    const fs::path captureFile = tempDir / ("audio_" + audioType + "_" + timestamp + "_capture.wav");
    const fs::path ch1File = tempDir / ("audio_" + audioType + "_" + timestamp + "_ch1.wav");
    const fs::path ch1Denoised = tempDir / ("audio_" + audioType + "_" + timestamp + "_ch1_denoised.wav");
    const fs::path outputFile = tempDir / ("audio_" + audioType + "_" + timestamp + ".wav");

    sendAudioCommand(audioType == "pre" ? "pre_audio_recording" : "post_audio_recording");

    ProcessRunner audioRecorder("audio_recorder");
    if (!audioRecorder.start({"python3", options_.audioRecordScript, "--output", captureFile.string()}))
    {
        std::cerr << "[ERROR] failed to start audio recorder" << std::endl;
        sendAudioCommand("audio_recording_stop");
        return false;
    }

    while (!stopRequested_.load())
    {
        ButtonSnapshot buttons;
        panelManager_.poll(options_.pollMs, &buttons);
        const bool stillPressed = monitorUpButton ? buttons.upPressed : buttons.downPressed;
        if (!stillPressed)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    audioRecorder.sendSignal(SIGINT);
    if (!audioRecorder.wait(5000))
    {
        audioRecorder.stop(1000);
    }

    if (!fileExistsAndNotEmpty(captureFile.string()) || fs::file_size(captureFile) <= 44)
    {
        std::cerr << "[WARN] audio capture is empty" << std::endl;
        fs::remove(captureFile);
        sendAudioCommand("audio_recording_stop");
        return false;
    }

    bool ok = false;
    if (runCommandSync({"sox", captureFile.string(), ch1File.string(), "remix", "1"}))
    {
        if (fs::exists(options_.noiseProfile) &&
            runCommandSync({"sox", ch1File.string(), ch1Denoised.string(), "noisered", options_.noiseProfile, "0.15"}) &&
            runCommandSync({"sox", ch1Denoised.string(), outputFile.string(), "norm"}))
        {
            ok = true;
        }
        else if (runCommandSync({"sox", ch1File.string(), outputFile.string(), "norm"}))
        {
            ok = true;
        }
    }

    fs::remove(captureFile);
    fs::remove(ch1File);
    fs::remove(ch1Denoised);

    if (!ok || !fileExistsAndNotEmpty(outputFile.string()))
    {
        std::cerr << "[ERROR] failed to process audio clip" << std::endl;
        fs::remove(outputFile);
        sendAudioCommand("audio_recording_stop");
        return false;
    }

    if (audioType == "pre")
    {
        pendingPreAudioFile_ = outputFile.string();
        std::cout << "[INFO] pre-audio prepared: " << pendingPreAudioFile_ << std::endl;
    }
    else
    {
        if (!lastEpisodeDir_.empty() && fs::exists(lastEpisodeDir_))
        {
            std::error_code error;
            fs::rename(outputFile, fs::path(lastEpisodeDir_) / "audio_post.wav", error);
            if (error)
            {
                std::cerr << "[ERROR] failed to move post-audio into last episode: " << error.message() << std::endl;
                fs::remove(outputFile);
            }
        }
        else
        {
            std::cerr << "[WARN] no previous episode for post-audio" << std::endl;
            fs::remove(outputFile);
        }
    }

    sendAudioCommand("audio_recording_stop");
    return true;
}

void RecordRuntime::handleButtons(const ButtonSnapshot &buttons)
{
    const uint64_t nowMs = currentSteadyMs();

    if (buttons.upPressed && !lastButtons_.upPressed)
    {
        buttonTracker_.upPressedSinceMs = nowMs;
        buttonTracker_.upLongHandled = false;
    }
    if (buttons.downPressed && !lastButtons_.downPressed)
    {
        buttonTracker_.downPressedSinceMs = nowMs;
        buttonTracker_.downLongHandled = false;
    }

    if (!buttons.upPressed)
    {
        buttonTracker_.upPressedSinceMs = 0;
    }
    if (!buttons.downPressed)
    {
        buttonTracker_.downPressedSinceMs = 0;
    }

    if (buttons.upPressed && buttons.downPressed)
    {
        if (!buttonTracker_.dualChordActive)
        {
            buttonTracker_.dualChordActive = true;
            buttonTracker_.bothPressedSinceMs = nowMs;
            buttonTracker_.dualLongHandled = false;
            buttonTracker_.shutdownPromptPlayed = false;
            std::cout << "[INFO] dual-button chord armed" << std::endl;
        }

        const uint64_t dualHeldMs = nowMs - buttonTracker_.bothPressedSinceMs;
        if (dualHeldMs >= kShutdownPromptThresholdMs && !buttonTracker_.shutdownPromptPlayed)
        {
            buttonTracker_.shutdownPromptPlayed = true;
            sendAudioCommand("shutdown");
        }
        if (dualHeldMs >= kDualLongPressThresholdMs && !buttonTracker_.dualLongHandled)
        {
            buttonTracker_.dualLongHandled = true;
            lastButtonActionMs_ = nowMs;
            handleDualShutdownAction();
        }

        lastButtons_ = buttons;
        return;
    }

    if (buttonTracker_.dualChordActive)
    {
        if (!buttons.upPressed && !buttons.downPressed)
        {
            buttonTracker_.dualChordActive = false;
            buttonTracker_.bothPressedSinceMs = 0;
            buttonTracker_.dualLongHandled = false;
            buttonTracker_.shutdownPromptPlayed = false;
        }
        lastButtons_ = buttons;
        return;
    }

    if (buttons.upPressed && !buttonTracker_.upLongHandled && buttonTracker_.upPressedSinceMs > 0 &&
        (nowMs - buttonTracker_.upPressedSinceMs) >= kLongPressThresholdMs)
    {
        buttonTracker_.upLongHandled = true;
        lastButtonActionMs_ = nowMs;
        handleLongUpAction();
    }

    if (buttons.downPressed && !buttonTracker_.downLongHandled && buttonTracker_.downPressedSinceMs > 0 &&
        (nowMs - buttonTracker_.downPressedSinceMs) >= kLongPressThresholdMs)
    {
        buttonTracker_.downLongHandled = true;
        lastButtonActionMs_ = nowMs;
        handleLongDownAction();
    }

    const bool upReleased = lastButtons_.upPressed && !buttons.upPressed;
    const bool downReleased = lastButtons_.downPressed && !buttons.downPressed;

    if (upReleased && !buttonTracker_.upLongHandled && (nowMs - lastButtonActionMs_) >= kActionDebounceMs)
    {
        lastButtonActionMs_ = nowMs;
        handleShortUpAction();
    }

    if (downReleased && !buttonTracker_.downLongHandled && (nowMs - lastButtonActionMs_) >= kActionDebounceMs)
    {
        lastButtonActionMs_ = nowMs;
        handleShortDownAction();
    }

    if (upReleased)
    {
        buttonTracker_.upLongHandled = false;
    }
    if (downReleased)
    {
        buttonTracker_.downLongHandled = false;
    }

    lastButtons_ = buttons;
}

bool RecordRuntime::checkRecorderProcesses()
{
    const bool cameraOk = cameraRecorder_.isRunning();
    const bool sensorOk = sensorRecorder_.isRunning();
    if (!cameraOk)
    {
        std::cerr << "[ERROR] camera recorder exited, last_exit=" << cameraRecorder_.lastExitCode() << std::endl;
    }
    if (!sensorOk)
    {
        std::cerr << "[ERROR] sensor recorder exited, last_exit=" << sensorRecorder_.lastExitCode() << std::endl;
    }
    return cameraOk && sensorOk;
}

void RecordRuntime::setLedState(LedState state, double progress)
{
    if (ledController_)
    {
        ledController_->setState(state, progress);
    }
}

std::string RecordRuntime::readEnvValue(const std::string &envFile, const std::string &key)
{
    std::ifstream input(envFile);
    std::string line;
    while (std::getline(input, line))
    {
        if (line.rfind(key + "=", 0) != 0)
        {
            continue;
        }
        std::string value = line.substr(key.size() + 1);
        value.erase(std::remove(value.begin(), value.end(), '"'), value.end());
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
        {
            value.pop_back();
        }
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
        {
            value.erase(value.begin());
        }
        return value;
    }
    return {};
}

std::string RecordRuntime::queryPackageVersion(const std::string &packageName)
{
    const std::string command = "dpkg-query -W -f='${Version}' " + packageName + " 2>/dev/null";
    FILE *pipe = popen(command.c_str(), "r");
    if (pipe == nullptr)
    {
        return "unknown";
    }

    char buffer[256];
    std::string output;
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr)
    {
        output += buffer;
    }
    pclose(pipe);

    while (!output.empty() && (output.back() == '\n' || output.back() == '\r'))
    {
        output.pop_back();
    }

    if (output.empty())
    {
        return "unknown";
    }
    return output;
}

std::string RecordRuntime::toLower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return text;
}

std::string RecordRuntime::jsonEscape(const std::string &value)
{
    std::string result;
    result.reserve(value.size());
    for (const char ch : value)
    {
        switch (ch)
        {
        case '\\':
            result += "\\\\";
            break;
        case '"':
            result += "\\\"";
            break;
        case '\n':
            result += "\\n";
            break;
        default:
            result += ch;
            break;
        }
    }
    return result;
}

uint64_t RecordRuntime::currentSteadyMs()
{
    const auto now = std::chrono::steady_clock::now();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
}

uint64_t RecordRuntime::currentEpochMs()
{
    const auto now = std::chrono::system_clock::now();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
}

bool RecordRuntime::fileExistsAndNotEmpty(const std::string &path)
{
    std::error_code error;
    return fs::exists(path, error) && fs::is_regular_file(path, error) && fs::file_size(path, error) > 0;
}

bool RecordRuntime::runCommandSync(const std::vector<std::string> &arguments)
{
    if (arguments.empty())
    {
        return false;
    }

    std::vector<char *> argv;
    argv.reserve(arguments.size() + 1);
    for (const auto &argument : arguments)
    {
        argv.push_back(const_cast<char *>(argument.c_str()));
    }
    argv.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0)
    {
        return false;
    }

    if (pid == 0)
    {
        execvp(argv[0], argv.data());
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
    {
        return false;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

RecordRuntime::ProcessRunner::ProcessRunner(std::string name)
    : name_(std::move(name))
{
}

RecordRuntime::ProcessRunner::~ProcessRunner()
{
    stop(1000);
}

bool RecordRuntime::ProcessRunner::start(const std::vector<std::string> &arguments)
{
    if (arguments.empty())
    {
        return false;
    }

    stop(1000);

    std::vector<char *> argv;
    argv.reserve(arguments.size() + 1);
    for (const auto &argument : arguments)
    {
        argv.push_back(const_cast<char *>(argument.c_str()));
    }
    argv.push_back(nullptr);

    std::cout << "[INFO] launching " << name_ << ": " << joinArguments(arguments) << std::endl;

    pid_ = fork();
    if (pid_ < 0)
    {
        perror("fork");
        pid_ = -1;
        return false;
    }

    if (pid_ == 0)
    {
        execvp(argv[0], argv.data());
        _exit(127);
    }

    lastExitCode_ = 0;
    return true;
}

bool RecordRuntime::ProcessRunner::isRunning()
{
    if (pid_ <= 0)
    {
        return false;
    }
    if (pollExit(false))
    {
        return false;
    }
    return true;
}

bool RecordRuntime::ProcessRunner::wait(int timeoutMs)
{
    if (pid_ <= 0)
    {
        return true;
    }

    const uint64_t startMs = RecordRuntime::currentSteadyMs();
    while ((RecordRuntime::currentSteadyMs() - startMs) < static_cast<uint64_t>(timeoutMs))
    {
        if (pollExit(false))
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

bool RecordRuntime::ProcessRunner::sendSignal(int signalNumber)
{
    if (pid_ <= 0)
    {
        return false;
    }
    return kill(pid_, signalNumber) == 0;
}

bool RecordRuntime::ProcessRunner::stop(int timeoutMs)
{
    if (pid_ <= 0)
    {
        return true;
    }

    kill(pid_, SIGTERM);
    if (wait(timeoutMs))
    {
        return true;
    }

    kill(pid_, SIGKILL);
    pollExit(true);
    return pid_ <= 0;
}

void RecordRuntime::ProcessRunner::reset()
{
    pid_ = -1;
    lastExitCode_ = 0;
}

int RecordRuntime::ProcessRunner::lastExitCode() const
{
    return lastExitCode_;
}

int RecordRuntime::ProcessRunner::pid() const
{
    return pid_;
}

bool RecordRuntime::ProcessRunner::pollExit(bool blocking)
{
    if (pid_ <= 0)
    {
        return true;
    }

    int status = 0;
    const int flags = blocking ? 0 : WNOHANG;
    const pid_t waitResult = waitpid(pid_, &status, flags);
    if (waitResult == 0)
    {
        return false;
    }
    if (waitResult < 0)
    {
        return false;
    }

    if (WIFEXITED(status))
    {
        lastExitCode_ = WEXITSTATUS(status);
    }
    else if (WIFSIGNALED(status))
    {
        lastExitCode_ = 128 + WTERMSIG(status);
    }
    pid_ = -1;
    return true;
}

bool RecordRuntime::GripperPanelManager::connect(const std::vector<std::string> &ports)
{
    drivers_.clear();
    inputDriverIndex_ = 0;
    hasDedicatedRightInput_ = false;

    for (size_t index = 0; index < ports.size(); ++index)
    {
        auto driver = std::make_unique<GripperHmiDriver>(ports[index], 115200, "RecordRuntimeHmi" + std::to_string(index));
        if (!driver->connect())
        {
            std::cerr << "[WARN] failed to connect HMI port: " << ports[index] << std::endl;
            continue;
        }
        if (!hasDedicatedRightInput_ && ports[index].find("right_gripper") != std::string::npos)
        {
            inputDriverIndex_ = drivers_.size();
            hasDedicatedRightInput_ = true;
        }
        drivers_.push_back(std::move(driver));
    }

    return !drivers_.empty();
}

void RecordRuntime::GripperPanelManager::disconnect()
{
    drivers_.clear();
}

bool RecordRuntime::GripperPanelManager::hasConnectedDevice() const
{
    return !drivers_.empty();
}

bool RecordRuntime::GripperPanelManager::poll(int timeoutMs, ButtonSnapshot *snapshot)
{
    ButtonSnapshot current;
    bool received = false;
    for (size_t index = 0; index < drivers_.size(); ++index)
    {
        auto &driver = drivers_[index];
        driver->requestState();
        received = driver->pollOnce(timeoutMs) || received;
        if (index != inputDriverIndex_)
        {
            continue;
        }

        const auto state = driver->getSnapshot();
        if (state.keyPressed.size() > kBtnUpKeyIndex)
        {
            current.upPressed = state.keyPressed[kBtnUpKeyIndex];
        }
        if (state.keyPressed.size() > kBtnDownKeyIndex)
        {
            current.downPressed = state.keyPressed[kBtnDownKeyIndex];
        }
    }

    if (snapshot != nullptr)
    {
        *snapshot = current;
    }
    return received;
}

void RecordRuntime::GripperPanelManager::setLedColor(uint8_t red, uint8_t green, uint8_t blue)
{
    for (auto &driver : drivers_)
    {
        driver->setLedColor(red, green, blue);
    }
}

void RecordRuntime::GripperPanelManager::turnOff()
{
    setLedColor(0, 0, 0);
}

RecordRuntime::HmiLedController::HmiLedController(GripperPanelManager *panelManager)
    : panelManager_(panelManager)
{
}

RecordRuntime::HmiLedController::~HmiLedController()
{
    stop();
}

void RecordRuntime::HmiLedController::start()
{
    if (running_.exchange(true))
    {
        return;
    }
    worker_ = std::thread(&HmiLedController::workerLoop, this);
}

void RecordRuntime::HmiLedController::stop()
{
    if (!running_.exchange(false))
    {
        return;
    }
    if (worker_.joinable())
    {
        worker_.join();
    }
    if (panelManager_ != nullptr)
    {
        panelManager_->turnOff();
    }
}

void RecordRuntime::HmiLedController::setState(LedState state, double progress)
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    effect_ = makeLedEffect(state, progress);
}

void RecordRuntime::HmiLedController::workerLoop()
{
    while (running_.load())
    {
        GripperLedEffect effect;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            effect = effect_;
        }

        const auto rendered = renderer_.render(effect, RecordRuntime::currentSteadyMs(), RecordRuntime::currentEpochMs());
        const std::array<uint8_t, 3> color{rendered.red, rendered.green, rendered.blue};
        if (color != lastColor_ && panelManager_ != nullptr)
        {
            panelManager_->setLedColor(color[0], color[1], color[2]);
            lastColor_ = color;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

RecordRuntime::EpisodeManager::EpisodeManager(std::string diskRoot,
                                              std::string deviceSn,
                                              std::string cameraCodec,
                                              std::string persistCalibrationFile,
                                              std::string fallbackCalibrationFile,
                                              std::string packageVersion,
                                              std::string updaterVersion)
    : diskRoot_(std::move(diskRoot)),
      deviceSn_(std::move(deviceSn)),
      deviceSnLower_(RecordRuntime::toLower(deviceSn_)),
      cameraCodec_(std::move(cameraCodec)),
      persistCalibrationFile_(std::move(persistCalibrationFile)),
      fallbackCalibrationFile_(std::move(fallbackCalibrationFile)),
      packageVersion_(std::move(packageVersion)),
      updaterVersion_(std::move(updaterVersion))
{
    dataRoot_ = diskRoot_ + "/" + deviceSnLower_;
    episodeRoot_ = dataRoot_ + "/data";
}

bool RecordRuntime::EpisodeManager::initialize()
{
    std::error_code error;
    fs::create_directories(episodeRoot_, error);
    if (error)
    {
        std::cerr << "[ERROR] create_directories failed for " << episodeRoot_
                  << ": " << error.message() << std::endl;
        return false;
    }
    return true;
}

std::string RecordRuntime::EpisodeManager::createNextEpisodeDir()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t nowTime = std::chrono::system_clock::to_time_t(now);
    std::tm localTime{};
    localtime_r(&nowTime, &localTime);

    char dateBuffer[16] = {0};
    std::strftime(dateBuffer, sizeof(dateBuffer), "%Y%m%d", &localTime);
    const std::string prefix = std::string("episode_") + dateBuffer + "_";

    int maxId = 0;
    std::error_code error;
    for (const auto &entry : fs::directory_iterator(episodeRoot_, error))
    {
        if (error || !entry.is_directory())
        {
            continue;
        }
        const std::string name = entry.path().filename().string();
        if (name.rfind(prefix, 0) != 0)
        {
            continue;
        }
        const std::string suffix = name.substr(prefix.size());
        if (!suffix.empty() && std::all_of(suffix.begin(), suffix.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; }))
        {
            maxId = std::max(maxId, std::stoi(suffix));
        }
    }

    const int nextId = maxId + 1;
    char idBuffer[16] = {0};
    std::snprintf(idBuffer, sizeof(idBuffer), "%04d", nextId);
    const fs::path episodePath = fs::path(episodeRoot_) / (prefix + idBuffer);
    fs::create_directories(episodePath, error);
    if (error)
    {
        return {};
    }
    return episodePath.string();
}

bool RecordRuntime::EpisodeManager::prepareEpisode(const std::string &episodeDir,
                                                   bool resetRecording,
                                                   const std::string &resetSourceDir,
                                                   std::string *errorMessage) const
{
    if (!writeMetadata(episodeDir, resetRecording, resetSourceDir, errorMessage))
    {
        return false;
    }
    if (!copyCalibration(episodeDir, errorMessage))
    {
        return false;
    }
    if (!prepareEpisodeOutputs(episodeDir, errorMessage))
    {
        return false;
    }
    return true;
}

bool RecordRuntime::EpisodeManager::validateEpisode(const std::string &episodeDir, std::string *errorMessage) const
{
    const std::vector<std::string> requiredFiles = {
        "left_cam_main.mkv",
        "right_cam_main.mkv",
        "left_stereo.mkv",
        "right_stereo.mkv",
        "left_tcam_l.mkv",
        "left_tcam_r.mkv",
        "right_tcam_l.mkv",
        "right_tcam_r.mkv",
        "sensor_data_left.mcap",
        "sensor_data_right.mcap",
        "metadata.json",
        "calibration.json",
    };

    for (const auto &file : requiredFiles)
    {
        const std::string path = episodeDir + "/" + file;
        if (!RecordRuntime::fileExistsAndNotEmpty(path))
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "missing or empty file: " + path;
            }
            return false;
        }
    }

    return true;
}

const std::string &RecordRuntime::EpisodeManager::dataRoot() const
{
    return episodeRoot_;
}

bool RecordRuntime::EpisodeManager::writeMetadata(const std::string &episodeDir,
                                                  bool resetRecording,
                                                  const std::string &resetSourceDir,
                                                  std::string *errorMessage) const
{
    std::ofstream output(episodeDir + "/metadata.json");
    if (!output.is_open())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "cannot open metadata.json for write";
        }
        return false;
    }

    output
        << "{\n"
        << "  \"device_type\": \"UMI\",\n"
        << "  \"device_model\": \"ugripper\",\n"
        << "  \"device_id\": \"" << RecordRuntime::jsonEscape(deviceSn_) << "\",\n"
        << "  \"device_role\": \"local\",\n"
        << "  \"camera_codec\": \"" << RecordRuntime::jsonEscape(cameraCodec_) << "\",\n"
        << "  \"ugripper_version\": \"" << RecordRuntime::jsonEscape(packageVersion_) << "\",\n"
        << "  \"ugripper_usb_updater_version\": \"" << RecordRuntime::jsonEscape(updaterVersion_) << "\",\n"
        << "  \"data_format_version\": \"1\",\n"
        << "  \"record_runtime\": \"cpp\",\n"
        << "  \"reset_recording\": " << (resetRecording ? "true" : "false") << ",\n"
        << "  \"reset_source_episode_dir\": \"" << RecordRuntime::jsonEscape(resetSourceDir) << "\"\n"
        << "}\n";
    return true;
}

bool RecordRuntime::EpisodeManager::copyCalibration(const std::string &episodeDir, std::string *errorMessage) const
{
    const std::string sourceFile = fs::exists(persistCalibrationFile_) ? persistCalibrationFile_ : fallbackCalibrationFile_;
    std::error_code error;
    fs::copy_file(sourceFile, episodeDir + "/calibration.json", fs::copy_options::overwrite_existing, error);
    if (error)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "failed to copy calibration from " + sourceFile + ": " + error.message();
        }
        return false;
    }
    return true;
}

bool RecordRuntime::EpisodeManager::prepareEpisodeOutputs(const std::string &episodeDir, std::string *errorMessage) const
{
    std::ofstream infoFile(episodeDir + "/info.json", std::ios::trunc);
    if (!infoFile.is_open())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "cannot open info.json for write";
        }
        return false;
    }
    infoFile << "{}\n";
    infoFile.close();

    std::error_code error;
    fs::remove(episodeDir + "/cam.mkv", error);
    error.clear();
    fs::remove(episodeDir + "/tact_left.mkv", error);
    error.clear();
    fs::remove(episodeDir + "/tact_right.mkv", error);
    return true;
}
