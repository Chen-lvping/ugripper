# Graph Report - .  (2026-05-14)

## Corpus Check
- 229 files · ~198,976 words
- Verdict: corpus is large enough that graph structure adds value.

## Summary
- 1308 nodes · 2851 edges · 78 communities (69 shown, 9 thin omitted)
- Extraction: 97% EXTRACTED · 3% INFERRED · 1% AMBIGUOUS · INFERRED: 79 edges (avg confidence: 0.78)
- Token cost: 1,436 input · 3,453 output

## Graph Freshness
- Built from commit: `c0d1771d`
- Run `git rev-parse HEAD` and compare to check if the graph is stale.
- Run `graphify update .` after code changes (no API cost).

## Community Hubs (Navigation)
- [[_COMMUNITY_Community 0|Community 0]]
- [[_COMMUNITY_Community 1|Community 1]]
- [[_COMMUNITY_Community 2|Community 2]]
- [[_COMMUNITY_Community 3|Community 3]]
- [[_COMMUNITY_Community 4|Community 4]]
- [[_COMMUNITY_Community 5|Community 5]]
- [[_COMMUNITY_Community 6|Community 6]]
- [[_COMMUNITY_Community 7|Community 7]]
- [[_COMMUNITY_Community 8|Community 8]]
- [[_COMMUNITY_Community 9|Community 9]]
- [[_COMMUNITY_Community 10|Community 10]]
- [[_COMMUNITY_Community 11|Community 11]]
- [[_COMMUNITY_Community 12|Community 12]]
- [[_COMMUNITY_Community 13|Community 13]]
- [[_COMMUNITY_Community 14|Community 14]]
- [[_COMMUNITY_Community 15|Community 15]]
- [[_COMMUNITY_Community 16|Community 16]]
- [[_COMMUNITY_Community 17|Community 17]]
- [[_COMMUNITY_Community 18|Community 18]]
- [[_COMMUNITY_Community 19|Community 19]]
- [[_COMMUNITY_Community 20|Community 20]]
- [[_COMMUNITY_Community 21|Community 21]]
- [[_COMMUNITY_Community 22|Community 22]]
- [[_COMMUNITY_Community 23|Community 23]]
- [[_COMMUNITY_Community 24|Community 24]]
- [[_COMMUNITY_Community 25|Community 25]]
- [[_COMMUNITY_Community 26|Community 26]]
- [[_COMMUNITY_Community 27|Community 27]]
- [[_COMMUNITY_Community 28|Community 28]]
- [[_COMMUNITY_Community 29|Community 29]]
- [[_COMMUNITY_Community 30|Community 30]]
- [[_COMMUNITY_Community 31|Community 31]]
- [[_COMMUNITY_Community 32|Community 32]]
- [[_COMMUNITY_Community 33|Community 33]]
- [[_COMMUNITY_Community 34|Community 34]]
- [[_COMMUNITY_Community 35|Community 35]]
- [[_COMMUNITY_Community 37|Community 37]]
- [[_COMMUNITY_Community 38|Community 38]]
- [[_COMMUNITY_Community 39|Community 39]]
- [[_COMMUNITY_Community 40|Community 40]]
- [[_COMMUNITY_Community 41|Community 41]]
- [[_COMMUNITY_Community 42|Community 42]]
- [[_COMMUNITY_Community 43|Community 43]]
- [[_COMMUNITY_Community 45|Community 45]]
- [[_COMMUNITY_Community 46|Community 46]]
- [[_COMMUNITY_Community 47|Community 47]]
- [[_COMMUNITY_Community 49|Community 49]]
- [[_COMMUNITY_Community 50|Community 50]]
- [[_COMMUNITY_Community 51|Community 51]]
- [[_COMMUNITY_Community 52|Community 52]]
- [[_COMMUNITY_Community 53|Community 53]]
- [[_COMMUNITY_Community 54|Community 54]]
- [[_COMMUNITY_Community 55|Community 55]]
- [[_COMMUNITY_Community 72|Community 72]]
- [[_COMMUNITY_Community 73|Community 73]]
- [[_COMMUNITY_Community 74|Community 74]]
- [[_COMMUNITY_Community 75|Community 75]]
- [[_COMMUNITY_Community 76|Community 76]]
- [[_COMMUNITY_Community 77|Community 77]]

## God Nodes (most connected - your core abstractions)
1. `main()` - 193 edges
2. `record_runtime` - 164 edges
3. `CameraRecorder` - 72 edges
4. `camera_domain` - 50 edges
5. `Ugripper V2` - 35 edges
6. `test` - 33 edges
7. `GripperHmiClient` - 31 edges
8. `GripperHmiTool` - 30 edges
9. `EpisodeManager::writeFilteredCalibration()` - 27 edges
10. `Initialize()` - 27 edges

## Surprising Connections (you probably didn't know these)
- `Ugripper V2` --calls--> `TryGetEncoderResponseFrameLength()`  [INFERRED]
  README.md → standalone/SensorRecorder/sensor_protocol.cc
- `HealthMonitor()` --references--> `runtime_config`  [AMBIGUOUS]
  standalone/UgripperRuntime/runtime_domain.cc → docs/archive/2026-refactor-history/repo-architecture.md
- `RecordingOrchestrator()` --references--> `runtime_config`  [AMBIGUOUS]
  standalone/UgripperRuntime/runtime_domain.cc → docs/archive/2026-refactor-history/repo-architecture.md
- `Ugripper V2` --calls--> `CreateFileShutdownRequestPort()`  [INFERRED]
  README.md → standalone/UgripperRuntime/runtime_process.cc
- `TaskScheduler` --conceptually_related_to--> `HmiController()`  [AMBIGUOUS]
  docs/archive/2026-refactor-history/ugripper-refactor-plan.md → standalone/UgripperRuntime/include/record_runtime/hmi_controller.h

## Communities (78 total, 9 thin omitted)

### Community 0 - "Community 0"
Cohesion: 0.05
Nodes (68): GripperHmiTool, abortCalibrationWriteStateLocked(), allocateCommandIdLocked(), formatHexByte(), getSnapshot(), handleIoFailureLocked(), handleParsedFrame(), hasAllCalibrationChunks() (+60 more)

### Community 1 - "Community 1"
Cohesion: 0.07
Nodes (58): analyze_video(), _build_default_run_dir(), _build_issue_tags(), build_report_paths(), _collect_frame_stats(), _copy_file(), _estimate_effective_fps(), _format_float() (+50 more)

### Community 2 - "Community 2"
Cohesion: 0.06
Nodes (25): AudioPlayer, _find_event_device(), _handle_volume_key(), setup_audio_device(), VolumeKeyListener, _build_log_callback(), classify_topic(), fail() (+17 more)

### Community 3 - "Community 3"
Cohesion: 0.06
Nodes (34): record_runtime, applySideCalibrationPayload(), currentDateString(), EpisodeManager::writeFilteredCalibration(), eraseSideCalibrationEntries(), findJsonPath(), findSourceCalibrationEntry(), HmiLedController::setState() (+26 more)

### Community 4 - "Community 4"
Cohesion: 0.06
Nodes (48): Changelog, Top-level CMakeLists, ros2-humble-arm Container Environment Request, Naming And YAML Contract, Agent Overview, Package Contents Baseline, pp_main, ugripper camera domain model (+40 more)

### Community 5 - "Community 5"
Cohesion: 0.06
Nodes (40): chest_cam_main, left_cam_main, left_tcam_l, right_cam_main, right_tcam_l, ModeName(), BootTimeOffsetUs(), CreateRecorder() (+32 more)

### Community 6 - "Community 6"
Cohesion: 0.06
Nodes (16): Python3, test_check_video_windows_rules, test/scripts/*, Python3 Interpreter, test_check_hmi_event_log_health_faults_fail, test_check_hmi_event_log_pass, load_report(), build_suite_summary() (+8 more)

### Community 7 - "Community 7"
Cohesion: 0.09
Nodes (27): SerialEncoder, calculate_crc16(), find_devices(), 对所有编码器进行归零操作          Args:         baudrate: 波特率，默认115200, 对单个编码器进行归零          Args:         device_path: 设备路径，如 '/dev/ttyCH9344USB0', 计算Modbus CRC16校验     返回: (crc_low, crc_high), 向编码器发送归零命令          Args:         ser: 串口对象         device_name: 设备名称（用于日志）, 查找所有ttyCH9344USB设备（0-30） (+19 more)

### Community 8 - "Community 8"
Cohesion: 0.11
Nodes (31): joinArguments(), maintainAudioPlayer(), startAudioPlayer(), stopAudioPlayer(), AudioCoordinator(), CreateFileAudioCommandPort(), CreateFileStereoSessionPort(), FileAudioCommandPort (+23 more)

### Community 9 - "Community 9"
Cohesion: 0.11
Nodes (37): ClientOptions, detect_existing_ports(), is_wch_serial_port(), build_calibration_bin(), build_delivery_readme(), colorize(), column_letters_to_index(), create_delivery_bundle() (+29 more)

### Community 10 - "Community 10"
Cohesion: 0.11
Nodes (32): _run_pactl(), _bootstrap_pulse_env(), _card_from_properties(), configure_pulse_audio_env(), disable_pulse_idle_suspend(), get_forced_usb_audio_target(), is_supported_usb_audio_input_device(), _list_loaded_modules() (+24 more)

### Community 11 - "Community 11"
Cohesion: 0.1
Nodes (28): Exception, build_im648_command(), build_port_info(), chain_text(), choose_encoder_port(), crc16_modbus(), decode_encoder_position(), encode_encoder_read() (+20 more)

### Community 12 - "Community 12"
Cohesion: 0.13
Nodes (31): resolve_root_dir(), _add_data_root(), add_label(), build_report_root(), compute_damage_metrics(), detect_latest_root(), detect_layout(), device_label_from_metadata() (+23 more)

### Community 13 - "Community 13"
Cohesion: 0.12
Nodes (30): build_payload_template(), camera_model_enum_from_strings(), detect_raw_pair_from_dir(), fill_float_array(), fill_statistics(), finalize_payload(), GripperCalibrationDataV1, GripperCalibrationHeader (+22 more)

### Community 14 - "Community 14"
Cohesion: 0.16
Nodes (26): PkgConfig::LZ4, PkgConfig::LIBSERIALPORT, SensorRecorder, sensorrecorder_local_libserialport, standalone, Threads::Threads, UgripperRuntime, zeroing (+18 more)

### Community 15 - "Community 15"
Cohesion: 0.13
Nodes (28): PrintCalibrationInfo(), PrintDeviceInfo(), printTransform(), CalibrationLooksValid(), CameraModelName(), CamTsQueue, DistortionModelName(), DumpCalibrationJson() (+20 more)

### Community 16 - "Community 16"
Cohesion: 0.12
Nodes (16): StopAll(), EncoderData, SerialManager, load_camera_params(), 从 Kalibr camchain.yaml 文件中读取内参和畸变参数, record_capture(), render_output(), require_sox() (+8 more)

### Community 17 - "Community 17"
Cohesion: 0.13
Nodes (25): gripper_hmi_led_effects, gripper_hmi_protocol, clampProgress(), clampToU8(), errorLevel(), parseStateText(), render(), buildAbortCalibrationWriteCommand() (+17 more)

### Community 18 - "Community 18"
Cohesion: 0.18
Nodes (24): BuildStereoServiceStatusJson(), BuildStereoTrackStatusJson(), IsSelectedCamera(), IsStereoCamera(), LoadCameraConfigList(), OptionalInt(), OptionalString(), ParseKindText() (+16 more)

### Community 19 - "Community 19"
Cohesion: 0.08
Nodes (7): Header(), extract_pts_from_mkv(), load_boot_time(), Use boot_time_offset from info.json (seconds), Extract PTS timestamps from MKV file using ffprobe     Returns: list of (frame_i, Write camera frames using accurate PTS from ffprobe, write_camera_with_pts()

### Community 20 - "Community 20"
Cohesion: 0.13
Nodes (24): analyze_device_root(), analyze_episode_stream(), analyze_packet_sequence(), build_alert_episode_rows(), build_alert_episode_stream_rows(), build_device_summary_rows(), build_episode_stream_rows(), build_episode_summary_rows() (+16 more)

### Community 21 - "Community 21"
Cohesion: 0.26
Nodes (22): GTest::gtest_main, nlohmann_json, PP::utils, runtime_control_plane, runtime_process_boundary, ugripperruntime_support_utils, test_audio_coordinator, test_button_event_timing (+14 more)

### Community 22 - "Community 22"
Cohesion: 0.13
Nodes (21): yaml-cpp, Ugripper Current Status Overview, docs/REFACTOR_LOG.md, pp_main 合并后 ugripper 正式测试方案, scripts/build_arm_deb_in_pp_arm_dev.sh, build_deb.sh, scripts/build_runtime_venv.sh, config/camera_recorder.yaml (+13 more)

### Community 23 - "Community 23"
Cohesion: 0.13
Nodes (22): activeEpisodeVideoArtifacts(), captureTactileGrayFrame(), commandExists(), computeTactileFrameMetrics(), EpisodeManager::createNextEpisodeDir(), EpisodeManager::validateEpisode(), extractJsonIntegerField(), extractJsonNumberField() (+14 more)

### Community 24 - "Community 24"
Cohesion: 0.22
Nodes (21): applyIdleState(), boolText(), handleDualShutdownAction(), handleLeftButtons(), handleLeftDualUmountAction(), handleLongDownAction(), handleLongUpAction(), handleShortDownAction() (+13 more)

### Community 25 - "Community 25"
Cohesion: 0.11
Nodes (16): ReadEnvValue(), TrimWhitespace(), Initialize(), SetLevel(), activeCriticalDevicePaths(), flushDirectoryToDisk(), isFalseLikeValue(), joinCsv() (+8 more)

### Community 26 - "Community 26"
Cohesion: 0.13
Nodes (20): src/utils, Control Channel Ledger Baseline, Audio Command FIFO /tmp/umi_audio_pipe, Audio Ready File /tmp/umi_audio_ready, Shutdown Request File /tmp/umi_shutdown_request, Equivalence Checklist, Naming YAML Contract, pp_main naming baseline (+12 more)

### Community 27 - "Community 27"
Cohesion: 0.14
Nodes (15): CameraXuDevice, deserialize(), fnv1a32(), main_camera_xu_tool, joinFloatArray(), keyName(), parseInt(), parseOptions() (+7 more)

### Community 28 - "Community 28"
Cohesion: 0.14
Nodes (17): writeBinaryFile(), CurrentEpochMs(), EpisodeManager::initialize(), EpisodeManager::refreshTactileReferenceCacheForSide(), makeLocalDateTimeString(), readBinaryFileExact(), sanitizeFileComponent(), storePersistentResetSerials() (+9 more)

### Community 29 - "Community 29"
Cohesion: 0.2
Nodes (16): build_markdown_report(), build_validate_command(), choose_python_runner(), discover_device_roots(), EpisodeResult, find_first_issue(), format_timestamp(), has_episode_dirs() (+8 more)

### Community 30 - "Community 30"
Cohesion: 0.22
Nodes (17): MakeHybridCamera(), CameraSampleDir(), add_host_only_camera_gtest, GTest, gtest_discover_tests, test, test_camera_command_builder, test_camera_config (+9 more)

### Community 31 - "Community 31"
Cohesion: 0.28
Nodes (10): Status(), MagicToHex(), ParseByteArray(), ParseKeyValueMap(), ParseString(), ParseStringView(), ParseUint32(), StrCat() (+2 more)

### Community 32 - "Community 32"
Cohesion: 0.13
Nodes (5): isPrintableSerialChar(), makeDefaultCalibrationDataV1(), MainCameraCalibrationDataV1, MainCameraCalibrationHeader, MainCameraIntrinsicsBlock

### Community 33 - "Community 33"
Cohesion: 0.2
Nodes (14): BuildHybridCameraCommand(), BuildMainCameraCommand(), BuildStereoHybridCameraCommand(), BuildVideoFilter(), CaptureHeight(), CaptureInputFormat(), CaptureWidth(), FrameDropModulo() (+6 more)

### Community 34 - "Community 34"
Cohesion: 0.25
Nodes (13): assert_output_has_mcaps(), build_topic_stats(), collect_gap_events(), create_output_dir(), ensure_mcap_dependency(), iter_mcap_messages(), ns_to_text(), percentile() (+5 more)

### Community 35 - "Community 35"
Cohesion: 0.21
Nodes (12): sensor_domain, sensor_protocol, test_bsp_crc, test_encoder_protocol, test_imu_batch_timestamp, MakePayload(), ClampMonotonicTimestamp(), CreateBatchTimestampSmoothingState() (+4 more)

### Community 37 - "Community 37"
Cohesion: 0.25
Nodes (11): BuildStereoSessionCommand(), BuildStereoSessionVideoFilter(), CommonEncodeArgs(), GetRkmppEncoder(), CAMERA_CODEC, DEVICE_SN, Config Entrypoints Baseline, /etc/environment (+3 more)

### Community 39 - "Community 39"
Cohesion: 0.22
Nodes (7): printUsage(), build_parser(), parse_args(), check_sensor_mcap, nlohmann_json::nlohmann_json, CheckFile(), ToJson()

### Community 40 - "Community 40"
Cohesion: 0.2
Nodes (10): test/src/camera_recorder, test/src/gripper_hmi, test/src/record_runtime, test/src/sensor_recorder, test/src/utils, audio/*.py, GoogleTest, Test Layout (+2 more)

### Community 41 - "Community 41"
Cohesion: 0.22
Nodes (9): gripper_hmi, utils, utils_smoke_test, test_env_utils, test_file_utils, test_time_utils, EnvUtilsTest, TEST_F() (+1 more)

### Community 42 - "Community 42"
Cohesion: 0.25
Nodes (3): 启动进程并创建新进程组，以便 kill 时能杀掉子进程, setup_gpio_high(), start_process()

### Community 43 - "Community 43"
Cohesion: 0.32
Nodes (5): Interval, IntervalTree, visit_contained(), visit_near(), visit_overlapping()

### Community 45 - "Community 45"
Cohesion: 0.32
Nodes (8): areSideCriticalDevicesReady(), clearGripperRuntimeStateForSide(), EpisodeManager::writeMetadata(), gripperStateIndexForSide(), handleGripperConnectionEvents(), processPendingGripperRefreshes(), refreshGripperRuntimeStateForSide(), refreshTactileReferenceCachesForSide()

### Community 46 - "Community 46"
Cohesion: 0.29
Nodes (7): attachPendingPreAudio(), flushFileToDisk(), makeTimestampString(), moveFileWithCrossDeviceFallback(), recordAudioClip(), runCommandSync(), FileExistsAndNotEmpty()

### Community 47 - "Community 47"
Cohesion: 0.46
Nodes (7): AppendHmiPortActivity(), CallLog(), CheckRecorderProcesses(), EvaluateHealth(), JoinStrings(), Poll(), StartRecording()

### Community 49 - "Community 49"
Cohesion: 0.29
Nodes (5): fays_mcap, FaysStereoRecorder, mcap, src/third_party/mcap_builder, MCAP CMake Builder

### Community 51 - "Community 51"
Cohesion: 0.7
Nodes (4): extract_class_name(), extract_function_name(), format(), get_level_color_start()

### Community 52 - "Community 52"
Cohesion: 0.67
Nodes (3): audio_log(), read_env_file_value(), ResolveCodec()

### Community 54 - "Community 54"
Cohesion: 0.67
Nodes (3): graphify-out/GRAPH_REPORT.md, graphify, graphify-out/wiki/index.md

### Community 55 - "Community 55"
Cohesion: 0.67
Nodes (3): ASR README, sherpa-onnx, ASR/test.py

## Ambiguous Edges - Review These
- `HealthMonitor()` → `HmiController()`  [AMBIGUOUS]
  docs/archive/2026-refactor-history/ppmain-ugripper-process-thread-refactor-reference.md · relation: references
- `HealthMonitor()` → `runtime_config`  [AMBIGUOUS]
  docs/archive/2026-refactor-history/ppmain-ugripper-process-thread-refactor-reference.md · relation: references
- `RecordingOrchestrator()` → `runtime_config`  [AMBIGUOUS]
  docs/archive/2026-refactor-history/ppmain-ugripper-process-thread-refactor-reference.md · relation: references
- `HmiController()` → `TaskScheduler`  [AMBIGUOUS]
  docs/archive/2026-refactor-history/ppmain-ugripper-process-thread-refactor-reference.md · relation: conceptually_related_to
- `Ugripper V2` → `AudioCommandPort`  [AMBIGUOUS]
  docs/archive/2026-refactor-history/baseline/config-entrypoints.md · relation: conceptually_related_to
- `graphify` → `graphify-out/wiki/index.md`  [AMBIGUOUS]
  AGENTS.md · relation: references
- `UgripperRuntime` → `PkgConfig::LZ4`  [AMBIGUOUS]
  standalone/UgripperRuntime/CMakeLists.txt · relation: references
- `gripper_hmi` → `record_runtime`  [AMBIGUOUS]
  docs/archive/2026-refactor-history/standalone-merge-plan.md · relation: conceptually_related_to
- `SensorRecorder` → `PkgConfig::LIBSERIALPORT`  [AMBIGUOUS]
  standalone/SensorRecorder/CMakeLists.txt · relation: references
- `SensorRecorder` → `sensorrecorder_local_libserialport`  [AMBIGUOUS]
  standalone/SensorRecorder/CMakeLists.txt · relation: references
- `SensorRecorder` → `PkgConfig::LZ4`  [AMBIGUOUS]
  standalone/SensorRecorder/CMakeLists.txt · relation: references
- `zeroing` → `PkgConfig::LIBSERIALPORT`  [AMBIGUOUS]
  standalone/SensorRecorder/CMakeLists.txt · relation: references
- `zeroing` → `sensorrecorder_local_libserialport`  [AMBIGUOUS]
  standalone/SensorRecorder/CMakeLists.txt · relation: references
- `TODO List` → `Agent Overview`  [AMBIGUOUS]
  docs/todo_list.md · relation: conceptually_related_to
- `Ugripper Current Status Overview` → `pp_main 合并后 ugripper 正式测试方案`  [AMBIGUOUS]
  docs/agent/current-status.md · relation: conceptually_related_to
- `StereoSessionPort` → `CAMERA_CODEC`  [AMBIGUOUS]
  docs/archive/2026-refactor-history/baseline/config-entrypoints.md · relation: conceptually_related_to

## Knowledge Gaps
- **105 isolated node(s):** `StreamSpec`, `Use boot_time_offset from info.json (seconds)`, `Extract PTS timestamps from MKV file using ffprobe     Returns: list of (frame_i`, `Write camera frames using accurate PTS from ffprobe`, `从 Kalibr camchain.yaml 文件中读取内参和畸变参数` (+100 more)
  These have ≤1 connection - possible missing edges or undocumented components.
- **9 thin communities (<3 nodes) omitted from report** — run `graphify query` to explore isolated nodes.

## Suggested Questions
_Questions this graph is uniquely positioned to answer:_

- **What is the exact relationship between `HealthMonitor()` and `HmiController()`?**
  _Edge tagged AMBIGUOUS (relation: references) - confidence is low._
- **What is the exact relationship between `HealthMonitor()` and `runtime_config`?**
  _Edge tagged AMBIGUOUS (relation: references) - confidence is low._
- **What is the exact relationship between `RecordingOrchestrator()` and `runtime_config`?**
  _Edge tagged AMBIGUOUS (relation: references) - confidence is low._
- **What is the exact relationship between `HmiController()` and `TaskScheduler`?**
  _Edge tagged AMBIGUOUS (relation: conceptually_related_to) - confidence is low._
- **What is the exact relationship between `Ugripper V2` and `AudioCommandPort`?**
  _Edge tagged AMBIGUOUS (relation: conceptually_related_to) - confidence is low._
- **What is the exact relationship between `graphify` and `graphify-out/wiki/index.md`?**
  _Edge tagged AMBIGUOUS (relation: references) - confidence is low._
- **What is the exact relationship between `UgripperRuntime` and `PkgConfig::LZ4`?**
  _Edge tagged AMBIGUOUS (relation: references) - confidence is low._