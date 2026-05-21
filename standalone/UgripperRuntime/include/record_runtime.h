#ifndef RECORD_RUNTIME_H
#define RECORD_RUNTIME_H

#include "gripper_hmi_driver.h"
#include "gripper_hmi_led_effects.h"
#include "record_runtime/audio_coordinator.h"
#include "record_runtime/health_monitor.h"
#include "record_runtime/hmi_controller.h"
#include "record_runtime/process_supervisor.h"
#include "record_runtime/recording_orchestrator.h"
#include "record_runtime/shutdown_request_port.h"
#include "record_runtime/stereo_session_client.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

struct RecordRuntimeOptions
{
    std::vector<std::string> gripperPorts;
    std::string cameraRecorderBin = "./bin/CameraRecorder/CameraRecorder";
    std::string sensorRecorderBin = "./bin/SensorRecorder/SensorRecorder";
    std::string faysStereoDaemonScript = "./bin/FaysStereoRecorder/scripts/run_fays_stereo_daemon.sh";
    std::string leftFaysConfig = "./bin/FaysStereoRecorder/config/fays_vikit_left.yaml";
    std::string rightFaysConfig = "./bin/FaysStereoRecorder/config/fays_vikit_right.yaml";
    std::string leftFaysControlFifo = "/tmp/umi_left_fays_cmd";
    std::string rightFaysControlFifo = "/tmp/umi_right_fays_cmd";
    std::string audioPlayScript = "./bin/UgripperRuntime/audio/audio_play.py";
    std::string audioRecordScript = "./bin/UgripperRuntime/audio/record_usb_audio.py";
    std::string audioPipe = "/tmp/umi_audio_pipe";
    std::string audioReadyFile = "/tmp/umi_audio_ready";
    std::string audioTempDir = "/tmp/umi_audio";
    std::string recordingLockFile = "/tmp/umi_recording.lock";
    std::string noiseProfile = "./bin/UgripperRuntime/audio/noise.prof";
    std::string systemActionRequestFile = "/tmp/umi_system_action_request";
    std::string systemActionResultFile = "/tmp/umi_system_action_result";
    std::string diskRoot = "/mnt/data_disk";
    std::string envFile = "/etc/environment";
    std::string persistCalibrationFile = "/etc/ugripper/config/calibration/calibration.json";
    std::string exampleCalibrationFile = "./calibration.json";
    std::string fallbackCalibrationFile = "./bin/UgripperRuntime/config/fakeCamCalib.json";
    std::string tactileStateDir = "/tmp/umi_tactile_state";
    std::string cameraCodec = "h264";
    std::string stereoControlFile = "/tmp/umi_stereo_camera_control.json";
    std::string stereoStatusFile = "/tmp/umi_stereo_camera_status.json";
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
            uint64_t inputLastRxAgeMs = 0;
            std::vector<std::string> disconnectedPorts;
            std::vector<std::string> inactivePorts;
            std::vector<std::string> inactivePortDetails;
            std::vector<std::string> portActivity;
        };

        bool connect(const std::vector<std::string> &ports);
        void disconnect();
        bool hasConnectedDevice() const;
        bool poll(int timeoutMs, ButtonSnapshot *snapshot);
        bool getButtonsForPortToken(const std::string &token, ButtonSnapshot *snapshot) const;
        HealthSnapshot getHealthSnapshot(uint64_t activeTimeoutMs) const;
        bool requestStateForSide(const std::string &side);
        bool readRuntimeIdentityForSide(const std::string &side,
                                        std::string *serialNumber,
                                        bool *hasSerialNumber,
                                        gripper_hmi::GripperCalibrationDataV1 *calibrationData,
                                        std::string *errorMessage);
        std::vector<ConnectionEvent> consumeConnectionEvents();
        bool isSideReadyForRefresh(const std::string &side, uint64_t activeTimeoutMs) const;
        bool setBeepEnabledForSide(const std::string &side, bool enabled);
        bool silenceBeepForSide(const std::string &side);
        void silenceBeep();
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
        std::vector<GripperBeepState> currentDriverBeepStates_;
        size_t inputDriverIndex_ = 0;
        bool hasDedicatedRightInput_ = false;
        bool hasLedEffect_ = false;
        bool hasDirectLedColor_ = false;
        bool hasBeepState_ = false;
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

        struct MainCameraRuntimeState
        {
            std::string cameraName;
            std::string devicePath;
            bool enabled = true;
            bool present = false;
            std::string serialNumber;
            bool calibrationPayloadCached = false;
            std::array<uint8_t, 1024> calibrationPayload{};
            std::string lastError;
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
                       bool chestCameraEnabled,
                       std::string tactileStateDir,
                       std::string persistCalibrationFile,
                       std::string exampleCalibrationFile,
                       std::string fallbackCalibrationFile,
                       std::string stereoStatusFile,
                       std::string hardwareVersion,
                       std::string packageVersion,
                       std::string updaterVersion);

        bool initialize();
        std::string createNextEpisodeDir();
        void setGripperRuntimeStates(const std::array<GripperRuntimeState, 2> &states);
        void setMainCameraRuntimeStates(const std::array<MainCameraRuntimeState, 3> &states);
        void refreshTactileReferenceCacheForSide(const std::string &side,
                                                std::vector<TactileValidationFinding> *findings = nullptr);
        bool prepareEpisode(const std::string &episodeDir,
                            bool resetRecording,
                            const std::string &resetSourceDir,
                            std::string *errorMessage) const;
        bool validateEpisode(const std::string &episodeDir,
                             std::string *errorMessage,
                             std::vector<TactileValidationFinding> *tactileFindings = nullptr) const;
        bool writeFinalMetadata(const std::string &episodeDir,
                                bool qualityOk,
                                const std::string &qualityErrorMessage,
                                std::string *errorMessage) const;
        const std::string &dataRoot() const;

    private:
        bool writeFilteredCalibration(const std::string &episodeDir, std::string *errorMessage) const;
        bool prepareEpisodeOutputs(const std::string &episodeDir, std::string *errorMessage) const;

        std::string diskRoot_;
        std::string deviceSn_;
        std::string deviceSnLower_;
        std::string language_;
        std::string cameraCodec_;
        bool chestCameraEnabled_ = true;
        std::string tactileStateDir_;
        std::string persistCalibrationFile_;
        std::string exampleCalibrationFile_;
        std::string fallbackCalibrationFile_;
        std::string stereoStatusFile_;
        std::string hardwareVersion_;
        std::string packageVersion_;
        std::string updaterVersion_;
        std::string dataRoot_;
        std::string episodeRoot_;
        std::array<GripperRuntimeState, 2> gripperRuntimeStates_{};
        std::array<MainCameraRuntimeState, 3> mainCameraRuntimeStates_{};
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
    static const char *ledStateName(LedState state);
    static const char *hmiEventName(ugripper::runtime::HmiEventType event_type);

    void handleGripperConnectionEvents();
    void processPendingGripperRefreshes();
    void refreshGripperRuntimeStateForSide(const std::string &side);
    void clearGripperRuntimeStateForSide(const std::string &side,
                                         bool connected,
                                         const std::string &status,
                                         const std::string &errorMessage);
    bool persistGripperCalibrationCache(std::string *errorMessage);
    bool areSideCriticalDevicesReady(const std::string &side) const;
    void initializeMainCameraRuntimeStates();
    void maintainMainCameraRuntimeStates();
    void syncMainCameraRuntimeStatesToEpisodeManager();
    void waitForMainCameraRefreshes();
    bool startRecording(bool resetRecording);
    bool stopRecording(bool dueToError, const std::string &reason);
    void handleButtons(const ButtonSnapshot &buttons);
    bool handleShortUpAction();
    bool handleShortDownAction();
    bool handleLongUpAction();
    bool handleLongDownAction();
    void handleDualShutdownAction();
    bool handleLeftDualUmountAction();
    void handleLeftButtons(const ButtonSnapshot &buttons);
    bool recordAudioClip(const std::string &audioType, bool monitorUpButton);
    bool attachPendingPreAudio(const std::string &episodeDir);
    bool writeRecordingLock(const std::string &episodeDir);
    void removeRecordingLock();
    bool checkRecorderProcesses();
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
    void refreshTactileReferenceCachesForSide(const std::string &side);
    void applyIdleState();
    void setAudioRecoveryCommand(std::string command);
    void sendAudioCommand(const std::string &command) const;
    void setLedState(LedState state, double progress = 0.0);
    bool isRecordingActive() const;
    const std::string &currentEpisodeDir() const;
    const std::string &lastEpisodeDir() const;
    static LedState toLedState(ugripper::runtime::RuntimeLedState state);

    RecordRuntimeOptions options_;
    std::atomic<bool> stopRequested_{false};
    bool initialized_ = false;
    std::string deviceSn_;
    std::string language_;
    bool chestCameraEnabled_ = true;
    std::string hardwareVersion_;
    std::string packageVersion_;
    std::string updaterVersion_;
    std::string pendingPreAudioFile_;
    std::string audioRecoveryCommand_;
    ButtonSnapshot lastLoggedButtons_{};
    ButtonSnapshot lastLoggedLeftButtons_{};
    bool hasLastLoggedButtons_ = false;
    bool hasLastLoggedLeftButtons_ = false;
    ButtonSnapshot lastLeftButtons_{};
    uint64_t leftBothPressedSinceMs_ = 0;
    bool leftDualChordActive_ = false;
    bool leftDualLongHandled_ = false;
    bool tactileWarningActive_ = false;
    std::string tactileTriggeredAudioCommand_;
    LedState lastLoggedLedState_ = LedState::Init;
    double lastLoggedLedProgress_ = 0.0;
    bool hasLastLoggedLedState_ = false;
    ugripper::runtime::HealthState healthState_{};
    std::unique_ptr<EpisodeManager> episodeManager_;
    std::array<EpisodeManager::GripperRuntimeState, 2> gripperRuntimeStates_{};
    std::array<bool, 2> pendingGripperRefresh_{};
    struct MainCameraRefreshResult
    {
        std::string cameraName;
        std::string devicePath;
        std::string resolvedTarget;
        std::string serialNumber;
        bool calibrationPayloadCached = false;
        std::array<uint8_t, 1024> calibrationPayload{};
        std::string errorMessage;
    };
    struct MainCameraRuntimeCache
    {
        std::string cameraName;
        std::string devicePath;
        bool enabled = true;
        bool present = false;
        std::string resolvedTarget;
        std::string serialNumber;
        bool calibrationPayloadCached = false;
        std::array<uint8_t, 1024> calibrationPayload{};
        std::string lastError;
        uint64_t nextRefreshAllowedMs = 0;
        std::future<MainCameraRefreshResult> refreshFuture;
    };
    std::vector<MainCameraRuntimeCache> mainCameraRuntimeStates_;
    GripperPanelManager panelManager_;
    std::unique_ptr<HmiLedController> ledController_;
    mutable ugripper::runtime::ProcessSupervisor processSupervisor_{};
    std::unique_ptr<ugripper::runtime::HmiController> hmiController_;
    std::unique_ptr<ugripper::runtime::HealthMonitor> healthMonitor_;
    std::unique_ptr<ugripper::runtime::AudioCoordinator> audioCoordinator_;
    std::unique_ptr<ugripper::runtime::StereoSessionClient> stereoSessionClient_;
    std::unique_ptr<ugripper::runtime::ShutdownRequestPort> shutdownRequestPort_;
    std::unique_ptr<ugripper::runtime::RecordingOrchestrator> recordingOrchestrator_;
};

#endif
