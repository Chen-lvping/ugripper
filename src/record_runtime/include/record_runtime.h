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
#include <set>
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
    std::string systemActionRequestFile = "/tmp/umi_system_action_request";
    std::string systemActionResultFile = "/tmp/umi_system_action_result";
    std::string diskRoot = "/mnt/data_disk";
    std::string envFile = "/etc/environment";
    std::string persistCalibrationFile = "/etc/ugripper/config/calibration/calibration.json";
    std::string exampleCalibrationFile = "./calibration.json";
    std::string fallbackCalibrationFile = "./config/fakeCamCalib.json";
    std::string cameraCodec = "h264";
    std::string stereoControlFile = "/tmp/umi_stereo_camera_control.json";
    std::string stereoStatusFile = "/tmp/umi_stereo_camera_status.json";
    std::string tactileStateDir = "/tmp/umi_tactile_state";
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
        Warning,
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

        bool start(const std::vector<std::string> &arguments,
                   const std::vector<int> &inheritedFileDescriptors = {});
        bool isRunning();
        bool stop(int timeoutMs);
        bool wait(int timeoutMs);
        bool sendSignal(int signalNumber);
        void reset();
        int lastExitCode() const;
        int pid() const;

    private:
        bool pollExit(bool blocking);
        pid_t signalTarget() const;

        std::string name_;
        pid_t pid_ = -1;
        pid_t processGroupId_ = -1;
        int lastExitCode_ = 0;
    };

    class GripperPanelManager
    {
    public:
        struct ConnectionEvent
        {
            std::string side;
            bool connected = false;
        };

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
        bool poll(int timeoutMs, ButtonSnapshot *snapshot, ButtonSnapshot *leftSnapshot = nullptr);
        HealthSnapshot getHealthSnapshot(uint64_t activeTimeoutMs) const;
        bool setBeepState(const GripperBeepState &state);
        bool setBeepEnabled(bool enabled);
        bool silenceBeep();
        bool requestStateForSide(const std::string &side);
        bool readSerialNumberForSide(const std::string &side, std::string *serialNumber, std::string *errorMessage);
        bool readCalibrationForSide(const std::string &side,
                                    gripper_hmi::GripperCalibrationDataV1 *calibrationData,
                                    std::string *errorMessage);
        bool readRuntimeIdentityForSide(const std::string &side,
                                        std::string *serialNumber,
                                        bool *hasSerialNumber,
                                        gripper_hmi::GripperCalibrationDataV1 *calibrationData,
                                        std::string *errorMessage);
        std::vector<ConnectionEvent> consumeConnectionEvents();
        bool isSideReadyForRefresh(const std::string &side, uint64_t activeTimeoutMs) const;
        bool setBeepStateForSide(const std::string &side, const GripperBeepState &state);
        bool setBeepEnabledForSide(const std::string &side, bool enabled);
        bool silenceBeepForSide(const std::string &side);
        void setLedEffect(const GripperLedEffect &effect);
        void setLedColor(uint8_t red, uint8_t green, uint8_t blue);
        void turnOff();

    private:
        static constexpr size_t kBtnUpKeyIndex = 0;
        static constexpr size_t kBtnDownKeyIndex = 1;
        static constexpr uint64_t kReconnectIntervalMs = 1000;

        static std::string sideForPort(const std::string &port);
        int findDriverIndexForSide(const std::string &side) const;
        void recordConnectionEventIfChanged(size_t index, bool connected);
        void maybeReconnectDriver(size_t index);
        std::vector<std::unique_ptr<GripperHmiDriver>> drivers_;
        std::vector<uint64_t> reconnectAttemptMs_;
        std::vector<uint64_t> delayedStateRequestDueMs_;
        std::vector<bool> lastKnownConnectedStates_;
        std::vector<ConnectionEvent> pendingConnectionEvents_;
        size_t inputDriverIndex_ = 0;
        bool hasDedicatedRightInput_ = false;
        bool hasLedEffect_ = false;
        bool hasDirectLedColor_ = false;
        bool hasBeepState_ = false;
        std::vector<GripperBeepState> currentDriverBeepStates_;
        GripperLedEffect currentLedEffect_{};
        GripperLedColor currentLedColor_{};
        GripperBeepState currentBeepState_{};
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
        struct GripperRuntimeState
        {
            std::string side;
            bool connected = false;
            bool hasSerialNumber = false;
            std::string serialNumber;
            bool calibrationValid = false;
            std::string calibrationStatus;
            std::string calibrationSource;
            std::string lastError;
            bool calibrationPayloadCached = false;
            gripper_hmi::GripperCalibrationDataV1 calibrationPayload{};
        };

        struct TactileValidationFinding
        {
            std::string cameraName;
            std::string serialNumber;
            bool damaged = false;
            bool warningActive = false;
            bool warningTriggered = false;
            std::string detail;
            std::string audioCommand;
        };

        EpisodeManager(std::string diskRoot,
                       std::string deviceSn,
                       std::string language,
                       std::string cameraCodec,
                       std::string tactileStateDir,
                       std::string persistCalibrationFile,
                       std::string exampleCalibrationFile,
                       std::string fallbackCalibrationFile,
                       std::string packageVersion,
                       std::string updaterVersion);

        bool initialize();
        std::string createNextEpisodeDir();
        void setGripperRuntimeStates(const std::array<GripperRuntimeState, 2> &states);
        void refreshTactileReferenceCacheForSide(const std::string &side,
                                                std::vector<TactileValidationFinding> *findings = nullptr);
        bool prepareEpisode(const std::string &episodeDir,
                            bool resetRecording,
                            const std::string &resetSourceDir,
                            std::string *errorMessage) const;
        bool validateEpisode(const std::string &episodeDir,
                             std::string *errorMessage,
                             std::vector<TactileValidationFinding> *tactileFindings = nullptr) const;
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
        std::string tactileStateDir_;
        std::string persistCalibrationFile_;
        std::string exampleCalibrationFile_;
        std::string fallbackCalibrationFile_;
        std::string packageVersion_;
        std::string updaterVersion_;
        std::string dataRoot_;
        std::string episodeRoot_;
        std::array<GripperRuntimeState, 2> gripperRuntimeStates_{};
        std::set<std::string> persistentResetSerials_;
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

    struct MotionAlertOutputState
    {
        bool leftAlertActive = false;
        bool rightAlertActive = false;
    };

    static GripperLedEffect makeLedEffect(LedState state, double progress = 0.0);
    static std::string readEnvValue(const std::string &envFile, const std::string &key);
    static std::string queryPackageVersion(const std::string &packageName);
    static std::string toLower(std::string text);
    static std::string jsonEscape(const std::string &value);
    static uint64_t currentSteadyMs();
    static uint64_t currentEpochMs();
    static int64_t currentEpochUs();
    static bool fileExistsAndNotEmpty(const std::string &path);
    static bool runCommandSync(const std::vector<std::string> &arguments);
    static const char *motionAlertSideName(uint8_t side);
    static const char *motionAlertReasonName(uint8_t reason);

    bool startMotionAlertPipe(int *writeFd);
    void stopMotionAlertPipe();
    void clearMotionAlertOutputs();
    void pollMotionAlertPipe();
    void applyMotionAlertState(const std::string &side, bool active);
    void handleGripperConnectionEvents();
    void processPendingGripperRefreshes();
    void refreshGripperRuntimeStateForSide(const std::string &side);
    void clearGripperRuntimeStateForSide(const std::string &side,
                                         bool connected,
                                         const std::string &status,
                                         const std::string &errorMessage);
    bool persistGripperCalibrationCache(std::string *errorMessage);
    bool areSideCriticalDevicesReady(const std::string &side) const;
    bool startRecording(bool resetRecording);
    bool stopRecording(bool dueToError, const std::string &reason);
    void handleButtons(const ButtonSnapshot &buttons, const ButtonSnapshot &leftButtons);
    bool handleShortUpAction();
    bool handleShortDownAction();
    bool handleLongUpAction();
    bool handleLongDownAction();
    void handleDualShutdownAction();
    bool handleLeftDualUmountAction();
    bool recordAudioClip(const std::string &audioType, bool monitorUpButton);
    bool attachPendingPreAudio(const std::string &episodeDir);
    bool checkRecorderProcesses();
    std::optional<HealthFault> evaluateHardwareHealth() const;
    void monitorHardwareHealth();
    bool startAudioPlayer();
    void stopAudioPlayer();
    void maintainAudioPlayer();
    bool startStereoDaemon();
    void stopStereoDaemon();
    void maintainStereoDaemon();
    bool writeStereoControl(bool recording,
                            const std::string &episodeDir,
                            int64_t startSystemTimeUs,
                            int64_t stopSystemTimeUs);
    bool waitForStereoFinalize(const std::string &episodeDir, int timeoutMs, std::string *errorMessage);
    bool mergeEpisodeInfo(const std::string &episodeDir, std::string *errorMessage) const;
    bool syncRuntimeLogToDisk(const char *reason) const;
    bool requestSystemAction(const std::string &action, std::string *errorMessage) const;
    bool waitForSystemActionResult(std::string *result, int timeoutMs, std::string *errorMessage) const;
    void setAudioRecoveryCommand(std::string command);
    void sendAudioCommand(const std::string &command) const;
    void setLedState(LedState state, double progress = 0.0);
    void refreshTactileReferenceCachesForSide(const std::string &side);
    void applyIdleState();

    RecordRuntimeOptions options_;
    std::atomic<bool> stopRequested_{false};
    bool initialized_ = false;
    bool isRecording_ = false;
    ButtonSnapshot lastButtons_{};
    ButtonStateTracker buttonTracker_{};
    ButtonSnapshot lastLeftButtons_{};
    ButtonStateTracker leftButtonTracker_{};
    uint64_t lastButtonActionMs_ = 0;
    std::string deviceSn_;
    std::string language_;
    std::string packageVersion_;
    std::string updaterVersion_;
    std::string currentEpisodeDir_;
    std::string lastEpisodeDir_;
    std::string pendingPreAudioFile_;
    std::string audioRecoveryCommand_;
    bool tactileWarningActive_ = false;
    HealthStatus healthStatus_ = HealthStatus::Unknown;
    std::string lastHealthErrorKey_;
    uint64_t lastHealthCheckMs_ = 0;
    bool audioPlayerStarted_ = false;
    uint64_t lastAudioPlayerStartAttemptMs_ = 0;
    bool stereoDaemonStarted_ = false;
    uint64_t lastStereoDaemonStartAttemptMs_ = 0;
    uint64_t stereoCommandSeq_ = 0;
    std::unique_ptr<EpisodeManager> episodeManager_;
    std::array<EpisodeManager::GripperRuntimeState, 2> gripperRuntimeStates_{};
    std::array<bool, 2> pendingGripperRefresh_{};
    int motionAlertReadFd_ = -1;
    MotionAlertOutputState motionAlertOutputState_{};
    GripperPanelManager panelManager_;
    std::unique_ptr<HmiLedController> ledController_;
    ProcessRunner audioPlayer_{"audio_player"};
    ProcessRunner stereoDaemon_{"stereo_camera_daemon"};
    ProcessRunner cameraRecorder_{"camera_recorder"};
    ProcessRunner sensorRecorder_{"sensor_recorder"};
};

#endif
