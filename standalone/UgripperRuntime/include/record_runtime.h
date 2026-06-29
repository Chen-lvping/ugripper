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
#include "utils/fifo_utils.h"

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
    std::string leftFaysControlFifo = "/dev/shm/ugripper/umi_left_fays_cmd";
    std::string rightFaysControlFifo = "/dev/shm/ugripper/umi_right_fays_cmd";
    std::string audioPlayScript = "./bin/UgripperRuntime/audio/audio_play.py";
    std::string audioRecordScript = "./bin/UgripperRuntime/audio/record_usb_audio.py";
    std::string egoRecordingScript = "./bin/UgripperRuntime/ego/ego_recording_worker.py";
    std::string audioPipe = "/dev/shm/ugripper/umi_audio_pipe";
    std::string audioReadyFile = "/dev/shm/ugripper/umi_audio_ready";
    std::string audioTempDir = "/tmp/umi_audio";
    std::string recordControlPipe = "/dev/shm/ugripper/umi_record_control.pipe";
    std::string recordingLockFile = "/tmp/umi_recording.lock";
    std::string noiseProfile = "./bin/UgripperRuntime/audio/noise.prof";
    std::string systemActionRequestFile = "/run/ugripper/system_action_request";
    std::string systemActionResultFile = "/run/ugripper/system_action_result";
    std::string restoreUsbCommand = "/usr/local/sbin/ugripper_restore_usb";
    std::string diskRoot = "/mnt/data_disk";
    std::string envFile = "/etc/environment";
    std::string persistCalibrationFile = "/etc/ugripper/config/calibration/calibration.json";
    std::string exampleCalibrationFile = "./calibration.json";
    std::string fallbackCalibrationFile = "./bin/UgripperRuntime/config/fakeCamCalib.json";
    std::string tactileStateDir = "/var/lib/ugripper/tactile_state";
    std::string cameraCodec = "h264";
    std::string stereoControlPipe = "/dev/shm/ugripper/umi_stereo_camera_control.pipe";
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
        WaitStorage,
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
                                        std::string *errorMessage);
        std::vector<ConnectionEvent> consumeConnectionEvents();
        bool isSideReadyForRefresh(const std::string &side, uint64_t activeTimeoutMs) const;
        bool setBeepEnabledForSide(const std::string &side, bool enabled);
        bool silenceBeepForSide(const std::string &side);
        void silenceBeep();
        void setLedEffect(const GripperLedEffect &effect);
        void setLedEffectForSide(const std::string &side, const GripperLedEffect &effect);
        void setLedColor(uint8_t red, uint8_t green, uint8_t blue);
        void setLedColorForSide(const std::string &side, uint8_t red, uint8_t green, uint8_t blue);
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
        struct PerSideLedState
        {
            bool hasEffect = false;
            bool hasColor = false;
            GripperLedEffect effect{};
            GripperLedColor color{};
        };
        std::vector<PerSideLedState> perSideLedStates_;
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
            std::string lastError;
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

        struct TactileCameraRuntimeState
        {
            std::string cameraName;
            std::string side;
            std::string devicePath;
            bool present = false;
            std::string serialNumber;
            std::string lastError;
        };

        struct TactileValidationFinding
        {
            std::string cameraName;
            std::string serialNumber;
            std::string side;
            std::string sensorSlot;
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
        void setTactileCameraRuntimeStates(const std::array<TactileCameraRuntimeState, 4> &states);
        void markTactileReferencePendingForSide(const std::string &side,
                                                const std::string &reason);
        bool prepareEpisode(const std::string &episodeDir,
                            bool resetRecording,
                            const std::string &resetSourceDir,
                            std::string *errorMessage) const;
        bool validateEpisode(const std::string &episodeDir,
                             std::string *errorMessage,
                             std::vector<std::string> *errorTypes = nullptr,
                             std::vector<TactileValidationFinding> *tactileFindings = nullptr) const;
        void validateTactileEpisode(const std::string &episodeDir,
                                    std::vector<TactileValidationFinding> *tactileFindings) const;
        bool writeFinalMetadata(const std::string &episodeDir,
                                bool qualityOk,
                                const std::string &qualityErrorMessage,
                                const std::string &qualityErrorType,
                                std::string *errorMessage) const;
        std::string finalizeEpisodeDir(const std::string &episodeDir, std::string *errorMessage) const;
        const std::string &dataRoot() const;

    private:
        bool writeFilteredCalibration(const std::string &episodeDir, std::string *errorMessage) const;
        bool prepareEpisodeOutputs(const std::string &episodeDir, std::string *errorMessage) const;
        const TactileCameraRuntimeState *tactileCameraStateForName(const std::string &cameraName) const;
        std::string cachedTactileSerialForName(const std::string &cameraName) const;

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
        std::array<TactileCameraRuntimeState, 4> tactileCameraRuntimeStates_{};
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
    void refreshGripperRuntimeStateForSide(const std::string &side,
                                           bool markTactileReferencePending);
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
    void initializeTactileCameraRuntimeStates();
    void maintainTactileCameraRuntimeStates();
    void syncTactileCameraRuntimeStatesToEpisodeManager();
    bool startRecording(bool resetRecording);
    bool stopRecording(bool dueToError, const std::string &reason, const std::string &errorType = "");
    void handleButtons(const ButtonSnapshot &buttons);
    bool handleShortUpAction();
    bool handleShortDownAction();
    bool handleLongUpAction();
    bool handleLongDownAction();
    void handleDualShutdownAction();
    bool handleLeftDualUmountAction();
    void handleLeftButtons(const ButtonSnapshot &buttons);
    bool initializeRecordControlPipe();
    void closeRecordControlPipe();
    bool pollRecordControlPipe();
    bool processRecordControlCommand(const std::string &command);
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
    void maintainRestoreUsbRetry();
    void maintainRestoreUsbFailureAlarm();
    void maintainRestoreUsbDevicePresence();
    void suspendStereoDaemonForRestoreUsb(const std::string &reason);
    void resumeStereoDaemonAfterRestoreUsb(const std::string &reason);
    bool isDataDiskMounted() const;
    bool requestDataDiskUmountForRestoreUsb(const std::string &reason);
    bool prepareDataDiskForRestoreUsb(const std::string &reason);
    struct RestoreUsbTriggerContext
    {
        std::string cause;
        std::string evidence;
        std::string side;
    };
    void logRestoreUsbRequested(const RestoreUsbTriggerContext &context);
    bool confirmRestoreUsbTrigger(const RestoreUsbTriggerContext &context);
    bool writeStereoControl(bool recording,
                            const std::string &episodeDir,
                            int64_t startSystemTimeUs,
                            int64_t stopSystemTimeUs);
    bool waitForStereoFinalize(const std::string &episodeDir, int timeoutMs, std::string *errorMessage);
    bool startEgoRecording(const std::string &episodeDir, int64_t startSystemTimeUs, std::string *errorMessage);
    bool stopEgoRecording(const std::string &episodeDir, int64_t stopSystemTimeUs, std::string *errorMessage);
    bool waitForEgoFinalize(const std::string &episodeDir, int timeoutMs, std::string *errorMessage);
    bool cleanupEgoRemote(const std::string &episodeDir, std::string *errorMessage);
    bool mergeEpisodeInfo(const std::string &episodeDir, std::string *errorMessage) const;
    bool syncRuntimeLogToDisk(const char *reason) const;
    bool maintainDataStorage();
    bool ensureEpisodeManagerInitialized();
    void markTactileReferencePendingForSide(const std::string &side,
                                            const std::string &reason);
    void scheduleBackgroundTactileValidation(const std::string &episodeDir);
    void maintainBackgroundTactileValidation();
    void cancelBackgroundTactileValidation();
    void requestStopBackgroundTactileValidation();
    void applyTactileValidationFindings(const std::vector<EpisodeManager::TactileValidationFinding> &findings);
    void applyIdleState();
    void setAudioRecoveryCommand(std::string command);
    void sendAudioCommand(const std::string &command) const;
    void setLedState(LedState state, double progress = 0.0);
    void setTactileWarningLedState();
    void setHardwareFaultLedState(const ugripper::runtime::HealthFault &fault);
    void maybeTriggerRestoreUsbForHardwareFault(const ugripper::runtime::HealthFault &fault);
    std::optional<ugripper::runtime::HealthFault> detectCameraRecorderKernelHang();
    void resetCameraRecorderDStateTracking();
    void handleFailureState(ugripper::runtime::RuntimeLedState state,
                            const std::vector<std::string> &errorTypes,
                            const std::string &detail);
    void logRestoreUsbSkipOnce(const std::string &key, const std::string &message);
    void triggerRestoreUsbOnError(const std::string &reason, bool checkMainCameraRecovery = false);
    bool shouldRestoreUsbForFailure(ugripper::runtime::RuntimeLedState state,
                                    const std::vector<std::string> &errorTypes,
                                    const std::string &detail,
                                    bool *checkMainCameraRecovery) const;
    bool shouldDeferRestoreUsbForSide(ugripper::runtime::HardwareFaultSide side,
                                      const std::string &reason) const;
    bool sideHasAnyRestoreUsbDevice(const std::string &side) const;
    bool restoreUsbCriticalSymlinksPresent(std::string *detail) const;
    bool restoreUsbTargetRecovered() const;
    bool mainCameraRestoreTargetsHealthy() const;
    void triggerRestoreUsbFailureAlarm(const std::string &reason);
    void noteRestoreUsbManualInsert(const std::string &side, const std::string &reason);
    void resetRestoreUsbErrorWindow();
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
    bool perfLogEnabled_ = false;
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
    utils::BufferedFifoLineReader recordControlReader_{4096};
    uint64_t lastRecordControlActionMs_ = 0;
    struct TactileWarningSideState
    {
        bool leftSensor = false;
        bool rightSensor = false;
    };
    std::array<TactileWarningSideState, 2> tactileWarningSides_{};
    bool tactileWarningActive_ = false;
    std::string tactileTriggeredAudioCommand_;
    struct BackgroundTactileValidationResult
    {
        std::string episodeDir;
        std::vector<EpisodeManager::TactileValidationFinding> findings;
    };
    std::mutex backgroundTactileMutex_;
    std::string pendingBackgroundTactileEpisodeDir_;
    std::future<BackgroundTactileValidationResult> backgroundTactileFuture_;
    std::atomic<bool> stopBackgroundTactileValidation_{false};
    std::optional<ugripper::runtime::HealthFault> activeHardwareFault_;
    std::shared_ptr<std::atomic<bool>> restoreUsbInProgress_ = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<uint64_t>> restoreUsbLastFinishMs_ = std::make_shared<std::atomic<uint64_t>>(0);
    int restoreUsbAttemptsInCurrentError_ = 0;
    bool restoreUsbPendingRetryCheck_ = false;
    bool restoreUsbSymlinkCheckPassed_ = false;
    uint64_t restoreUsbSymlinkPassedMs_ = 0;
    uint64_t restoreUsbLastSymlinkProbeMs_ = 0;
    uint64_t restoreUsbLastMissingLogMs_ = 0;
    std::string restoreUsbLastMissingDetail_;
    bool restoreUsbCheckMainCameraRecovery_ = false;
    bool restoreUsbFailureAlarmActive_ = false;
    uint64_t restoreUsbFailureAlarmStartMs_ = 0;
    uint64_t restoreUsbPreflightFailureUntilMs_ = 0;
    bool restoreUsbStereoDaemonStopped_ = false;
    bool episodeManagerInitialized_ = false;
    bool dataStorageWaitLogged_ = false;
    int cameraRecorderDStatePid_ = -1;
    std::string cameraRecorderDStateTaskId_;
    uint64_t cameraRecorderDStateFirstSeenMs_ = 0;
    uint64_t cameraRecorderDStateLastLogMs_ = 0;
    std::array<bool, 2> restoreUsbSidePresent_{};
    std::array<uint64_t, 2> restoreUsbInsertGraceUntilMs_{};
    std::string restoreUsbPendingTriggerKey_;
    std::string restoreUsbPendingTriggerCause_;
    std::string restoreUsbPendingTriggerEvidence_;
    std::string restoreUsbPendingTriggerSide_;
    uint64_t restoreUsbPendingTriggerFirstSeenMs_ = 0;
    uint64_t restoreUsbPendingTriggerLastSeenMs_ = 0;
    bool restoreUsbPendingTriggerLogged_ = false;
    std::string restoreUsbLastSkipLogKey_;
    std::string restoreUsbLastReason_;
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
    struct TactileCameraRuntimeCache
    {
        std::string cameraName;
        std::string side;
        std::string devicePath;
        bool present = false;
        std::string resolvedTarget;
        std::string serialNumber;
        std::string lastError;
        uint64_t nextRefreshAllowedMs = 0;
    };
    std::vector<TactileCameraRuntimeCache> tactileCameraRuntimeStates_;
    GripperPanelManager panelManager_;
    std::unique_ptr<HmiLedController> ledController_;
    mutable ugripper::runtime::ProcessSupervisor processSupervisor_{};
    std::unique_ptr<ugripper::runtime::HmiController> hmiController_;
    std::unique_ptr<ugripper::runtime::HealthMonitor> healthMonitor_;
    std::unique_ptr<ugripper::runtime::AudioCoordinator> audioCoordinator_;
    std::unique_ptr<ugripper::runtime::StereoSessionClient> stereoSessionClient_;
    std::unique_ptr<ugripper::runtime::ShutdownRequestPort> shutdownRequestPort_;
    std::optional<ugripper::runtime::SubprocessHandle> egoRecordingWorker_;
    bool egoRecordingAttempted_ = false;
    std::unique_ptr<ugripper::runtime::RecordingOrchestrator> recordingOrchestrator_;
};

#endif
