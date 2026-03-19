#ifndef RECORD_RUNTIME_H
#define RECORD_RUNTIME_H

#include "gripper_hmi_driver.h"
#include "gripper_hmi_led_effects.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <sys/types.h>

struct RecordRuntimeOptions
{
    std::vector<std::string> gripperPorts;
    std::string cameraRecorderBin = "./build/src/camera_recorder/camera_recorder";
    std::string sensorRecorderBin = "./build/src/sensor_recorder/sensor_recorder";
    std::string audioPlayScript = "./audio/audio_play.py";
    std::string audioRecordScript = "./audio/record_usb_audio.py";
    std::string audioPipe = "/tmp/umi_audio_pipe";
    std::string audioReadyFile = "/tmp/umi_audio_ready";
    std::string audioTempDir = "/tmp/umi_audio";
    std::string noiseProfile = "./audio/noise.prof";
    std::string shutdownRequestFile = "/tmp/umi_shutdown_request";
    std::string diskRoot = "/mnt/data_disk";
    std::string envFile = "/etc/environment";
    std::string persistCalibrationFile = "/etc/ugripper/config/calibration/calibration.json";
    std::string exampleCalibrationFile = "./calibration.json";
    std::string fallbackCalibrationFile = "./config/fakeCamCalib.json";
    std::string cameraCodec = "h264";
    int pollMs = 20;
};

class RecordRuntime
{
public:
    explicit RecordRuntime(RecordRuntimeOptions options);
    ~RecordRuntime();

    bool initialize();
    int run();
    void requestStop();

private:
    struct ButtonSnapshot
    {
        bool upPressed = false;
        bool downPressed = false;
    };

    enum class LedState
    {
        Init,
        Ready,
        Recording,
        Error1,
        Error2,
        Error3,
        Error4,
        Error5,
        CalibPre,
        CalibRun,
        CalibDone,
        Exit,
    };

    class ProcessRunner
    {
    public:
        explicit ProcessRunner(std::string name);
        ~ProcessRunner();

        bool start(const std::vector<std::string> &arguments);
        bool isRunning();
        bool stop(int timeoutMs);
        bool wait(int timeoutMs);
        bool sendSignal(int signalNumber);
        void reset();
        int lastExitCode() const;
        int pid() const;

    private:
        bool pollExit(bool blocking);

        std::string name_;
        pid_t pid_ = -1;
        int lastExitCode_ = 0;
    };

    class GripperPanelManager
    {
    public:
        struct HealthSnapshot
        {
            bool hasConnectedDevice = false;
            bool inputConnected = false;
            bool inputActive = false;
            std::vector<std::string> disconnectedPorts;
            std::vector<std::string> inactivePorts;
        };

        bool connect(const std::vector<std::string> &ports);
        void disconnect();
        bool hasConnectedDevice() const;
        bool poll(int timeoutMs, ButtonSnapshot *snapshot);
        HealthSnapshot getHealthSnapshot(uint64_t activeTimeoutMs) const;
        void setLedEffect(const GripperLedEffect &effect);
        void setLedColor(uint8_t red, uint8_t green, uint8_t blue);
        void turnOff();

    private:
        static constexpr size_t kBtnUpKeyIndex = 0;
        static constexpr size_t kBtnDownKeyIndex = 1;
        static constexpr uint64_t kReconnectIntervalMs = 1000;

        void maybeReconnectDriver(size_t index);
        std::vector<std::unique_ptr<GripperHmiDriver>> drivers_;
        std::vector<uint64_t> reconnectAttemptMs_;
        size_t inputDriverIndex_ = 0;
        bool hasDedicatedRightInput_ = false;
        bool hasLedEffect_ = false;
        bool hasDirectLedColor_ = false;
        GripperLedEffect currentLedEffect_{};
        GripperLedColor currentLedColor_{};
    };

    class HmiLedController
    {
    public:
        explicit HmiLedController(GripperPanelManager *panelManager);
        ~HmiLedController();

        void start();
        void stop();
        void setState(LedState state, double progress = 0.0);

    private:
        GripperPanelManager *panelManager_ = nullptr;
    };

    class EpisodeManager
    {
    public:
        EpisodeManager(std::string diskRoot,
                       std::string deviceSn,
                       std::string language,
                       std::string cameraCodec,
                       std::string persistCalibrationFile,
                       std::string exampleCalibrationFile,
                       std::string fallbackCalibrationFile,
                       std::string packageVersion,
                       std::string updaterVersion);

        bool initialize();
        std::string createNextEpisodeDir();
        bool prepareEpisode(const std::string &episodeDir,
                            bool resetRecording,
                            const std::string &resetSourceDir,
                            std::string *errorMessage) const;
        bool validateEpisode(const std::string &episodeDir, std::string *errorMessage) const;
        const std::string &dataRoot() const;

    private:
        bool writeMetadata(const std::string &episodeDir,
                           bool resetRecording,
                           const std::string &resetSourceDir,
                           std::string *errorMessage) const;
        bool writeFilteredCalibration(const std::string &episodeDir, std::string *errorMessage) const;
        bool prepareEpisodeOutputs(const std::string &episodeDir, std::string *errorMessage) const;

        std::string diskRoot_;
        std::string deviceSn_;
        std::string deviceSnLower_;
        std::string language_;
        std::string cameraCodec_;
        std::string persistCalibrationFile_;
        std::string exampleCalibrationFile_;
        std::string fallbackCalibrationFile_;
        std::string packageVersion_;
        std::string updaterVersion_;
        std::string dataRoot_;
        std::string episodeRoot_;
    };

    struct ButtonStateTracker
    {
        uint64_t upPressedSinceMs = 0;
        uint64_t downPressedSinceMs = 0;
        uint64_t bothPressedSinceMs = 0;
        bool upLongHandled = false;
        bool downLongHandled = false;
        bool dualChordActive = false;
        bool dualLongHandled = false;
        bool shutdownPromptPlayed = false;
    };

    enum class HealthStatus
    {
        Unknown,
        Ok,
        Error,
    };

    struct HealthFault
    {
        LedState ledState = LedState::Error5;
        std::string key;
        std::string detail;
    };

    static GripperLedEffect makeLedEffect(LedState state, double progress = 0.0);
    static std::string readEnvValue(const std::string &envFile, const std::string &key);
    static std::string queryPackageVersion(const std::string &packageName);
    static std::string toLower(std::string text);
    static std::string jsonEscape(const std::string &value);
    static uint64_t currentSteadyMs();
    static uint64_t currentEpochMs();
    static bool fileExistsAndNotEmpty(const std::string &path);
    static bool runCommandSync(const std::vector<std::string> &arguments);

    bool startRecording(bool resetRecording);
    bool stopRecording(bool dueToError, const std::string &reason);
    void handleButtons(const ButtonSnapshot &buttons);
    bool handleShortUpAction();
    bool handleShortDownAction();
    bool handleLongUpAction();
    bool handleLongDownAction();
    void handleDualShutdownAction();
    bool recordAudioClip(const std::string &audioType, bool monitorUpButton);
    bool attachPendingPreAudio(const std::string &episodeDir);
    bool checkRecorderProcesses();
    std::optional<HealthFault> evaluateHardwareHealth() const;
    void monitorHardwareHealth();
    bool startAudioPlayer();
    void stopAudioPlayer();
    void maintainAudioPlayer();
    void setAudioRecoveryCommand(std::string command);
    void sendAudioCommand(const std::string &command) const;
    void setLedState(LedState state, double progress = 0.0);

    RecordRuntimeOptions options_;
    std::atomic<bool> stopRequested_{false};
    bool initialized_ = false;
    bool isRecording_ = false;
    ButtonSnapshot lastButtons_{};
    ButtonStateTracker buttonTracker_{};
    uint64_t lastButtonActionMs_ = 0;
    std::string deviceSn_;
    std::string language_;
    std::string packageVersion_;
    std::string updaterVersion_;
    std::string currentEpisodeDir_;
    std::string lastEpisodeDir_;
    std::string pendingPreAudioFile_;
    std::string audioRecoveryCommand_;
    HealthStatus healthStatus_ = HealthStatus::Unknown;
    std::string lastHealthErrorKey_;
    uint64_t lastHealthCheckMs_ = 0;
    bool audioPlayerStarted_ = false;
    uint64_t lastAudioPlayerStartAttemptMs_ = 0;
    std::unique_ptr<EpisodeManager> episodeManager_;
    GripperPanelManager panelManager_;
    std::unique_ptr<HmiLedController> ledController_;
    ProcessRunner audioPlayer_{"audio_player"};
    ProcessRunner cameraRecorder_{"camera_recorder"};
    ProcessRunner sensorRecorder_{"sensor_recorder"};
};

#endif
