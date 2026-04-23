#include "camera_recorder/camera_command_builder.h"
#include "camera_recorder/camera_config.h"
#include "camera_recorder/camera_registry.h"
#include "camera_recorder/camera_types.h"
#include "camera_recorder/stereo_control_json.h"

#include "utils/logger.h"

#include <algorithm>
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <yaml-cpp/yaml.h>

namespace ugripper::camera {

std::string ModeName(CameraRecordMode mode)
{
    switch (mode)
    {
    case CameraRecordMode::DirectCopyH265:
        return "direct-copy-h265";
    case CameraRecordMode::HybridDecodeEncode:
        return "hybrid-decode-encode";
    case CameraRecordMode::StereoHybridDecodeEncode:
        return "stereo-hybrid-decode-encode";
    }
    return "unknown";
}

CameraRecordMode ParseModeText(const std::string& mode_text)
{
    if (mode_text == "direct-copy-h265" || mode_text == "direct_copy_h265")
    {
        return CameraRecordMode::DirectCopyH265;
    }
    if (mode_text == "hybrid-decode-encode" || mode_text == "hybrid_decode_encode")
    {
        return CameraRecordMode::HybridDecodeEncode;
    }
    if (mode_text == "stereo-hybrid-decode-encode" || mode_text == "stereo_hybrid_decode_encode" ||
        mode_text == "stereo_session_encode")
    {
        return CameraRecordMode::StereoHybridDecodeEncode;
    }
    throw std::runtime_error("invalid camera mode: " + mode_text);
}

CameraRole ParseRoleText(const std::string& role_text)
{
    if (role_text.empty())
    {
        return CameraRole::Unknown;
    }
    if (role_text == "main")
    {
        return CameraRole::Main;
    }
    if (role_text == "stereo")
    {
        return CameraRole::Stereo;
    }
    if (role_text == "tactile")
    {
        return CameraRole::Tactile;
    }
    throw std::runtime_error("invalid camera role: " + role_text);
}

CameraSide ParseSideText(const std::string& side_text)
{
    if (side_text.empty())
    {
        return CameraSide::Unknown;
    }
    if (side_text == "mono")
    {
        return CameraSide::Mono;
    }
    if (side_text == "left")
    {
        return CameraSide::Left;
    }
    if (side_text == "right")
    {
        return CameraSide::Right;
    }
    throw std::runtime_error("invalid camera side: " + side_text);
}

CameraKind ParseKindText(const std::string& kind_text)
{
    if (kind_text.empty())
    {
        return CameraKind::Unknown;
    }
    if (kind_text == "direct_h265")
    {
        return CameraKind::DirectH265;
    }
    if (kind_text == "hybrid")
    {
        return CameraKind::Hybrid;
    }
    if (kind_text == "stereo_mjpeg")
    {
        return CameraKind::StereoMjpeg;
    }
    throw std::runtime_error("invalid camera kind: " + kind_text);
}

bool IsStereoCamera(const CameraConfig& config)
{
    return config.mode == CameraRecordMode::StereoHybridDecodeEncode;
}

bool HasGroup(const CameraConfig& config, const std::string& group_name)
{
    for (const auto& group : config.groups)
    {
        if (group == group_name)
        {
            return true;
        }
    }
    return false;
}

bool IsSelectedCamera(const CameraConfig& config, const std::set<std::string>& only_names)
{
    return only_names.empty() || only_names.count(config.name) > 0;
}

bool ShouldManageInStereoDaemon(const CameraConfig& config, const std::set<std::string>& only_names)
{
    return IsSelectedCamera(config, only_names) && IsStereoCamera(config);
}

const std::string& PrimaryOutputFileName(const CameraConfig& config)
{
    if (config.output_files.empty())
    {
        throw std::runtime_error("camera config missing primary output file: " + config.name);
    }
    return config.output_files.front();
}

namespace {

std::string WrapExecEnvCommand(const std::vector<std::string>& env_assignments,
                               const std::string& command)
{
    std::ostringstream oss;
    oss << "exec env";
    for (const auto& assignment : env_assignments)
    {
        oss << ' ' << assignment;
    }
    oss << ' ' << command;
    return oss.str();
}

std::string Trim(const std::string& input)
{
    const std::string whitespace = " \t\r\n";
    const size_t start = input.find_first_not_of(whitespace);
    if (start == std::string::npos)
    {
        return "";
    }
    const size_t end = input.find_last_not_of(whitespace);
    return input.substr(start, end - start + 1);
}

const YAML::Node RequireNode(const YAML::Node& parent, const std::string& key, const std::string& context)
{
    const YAML::Node node = parent[key];
    if (!node)
    {
        throw std::runtime_error("missing field '" + key + "' in " + context);
    }
    return node;
}

std::string RequireString(const YAML::Node& parent, const std::string& key, const std::string& context)
{
    const YAML::Node node = RequireNode(parent, key, context);
    if (!node.IsScalar())
    {
        throw std::runtime_error("field '" + key + "' must be a scalar in " + context);
    }
    const std::string value = Trim(node.as<std::string>());
    if (value.empty())
    {
        throw std::runtime_error("field '" + key + "' must not be empty in " + context);
    }
    return value;
}

int OptionalInt(const YAML::Node& parent, const std::string& key, int default_value, const std::string& context)
{
    const YAML::Node node = parent[key];
    if (!node)
    {
        return default_value;
    }
    if (!node.IsScalar())
    {
        throw std::runtime_error("field '" + key + "' must be a scalar in " + context);
    }
    return node.as<int>();
}

std::string OptionalString(const YAML::Node& parent,
                           const std::string& key,
                           const std::string& default_value,
                           const std::string& context)
{
    const YAML::Node node = parent[key];
    if (!node)
    {
        return default_value;
    }
    if (!node.IsScalar())
    {
        throw std::runtime_error("field '" + key + "' must be a scalar in " + context);
    }
    return Trim(node.as<std::string>());
}

int RequirePositiveInt(const YAML::Node& parent, const std::string& key, const std::string& context)
{
    const int value = RequireNode(parent, key, context).as<int>();
    if (value <= 0)
    {
        throw std::runtime_error("field '" + key + "' must be > 0 in " + context);
    }
    return value;
}

std::vector<std::string> RequireStringSequence(const YAML::Node& parent,
                                               const std::string& key,
                                               const std::string& context,
                                               bool allow_empty)
{
    const YAML::Node node = RequireNode(parent, key, context);
    if (!node.IsSequence() || (!allow_empty && node.size() == 0))
    {
        throw std::runtime_error("field '" + key + "' must be a " +
                                 std::string(allow_empty ? "sequence" : "non-empty sequence") +
                                 " in " + context);
    }

    std::vector<std::string> values;
    values.reserve(node.size());
    for (size_t index = 0; index < node.size(); ++index)
    {
        const YAML::Node item = node[index];
        if (!item.IsScalar())
        {
            throw std::runtime_error(key + "[" + std::to_string(index) +
                                     "] must be a scalar in " + context);
        }
        const std::string value = Trim(item.as<std::string>());
        if (value.empty())
        {
            throw std::runtime_error(key + "[" + std::to_string(index) +
                                     "] must not be empty in " + context);
        }
        values.push_back(value);
    }
    return values;
}

std::vector<std::string> RequireOutputFiles(const YAML::Node& parent, const std::string& context)
{
    return RequireStringSequence(parent, "output_files", context, false);
}

void ValidateNonNegativeFields(const CameraConfig& config, const std::string& context)
{
    if (config.capture_width < 0 || config.capture_height < 0 || config.input_thread_queue_size < 0)
    {
        throw std::runtime_error(
            "fields 'capture_width', 'capture_height', and 'input_thread_queue_size' must be >= 0 in " + context);
    }
    if (config.qp_init < 0 || config.qp_max < 0 || config.qp_min < 0 || config.qp_max_i < 0 ||
        config.qp_min_i < 0)
    {
        throw std::runtime_error("qp fields must be >= 0 in " + context);
    }
}

CameraConfig ParseLegacyCameraConfig(const YAML::Node& camera_node, size_t index)
{
    const std::string context = "cameras[" + std::to_string(index) + "]";
    CameraConfig config;
    config.name = RequireString(camera_node, "name", context);
    config.device = RequireString(camera_node, "device", context);
    config.mode = ParseModeText(RequireString(camera_node, "mode", context));
    if (const YAML::Node roll_node = camera_node["uvc_roll_absolute"])
    {
        if (!roll_node.IsScalar())
        {
            throw std::runtime_error("field 'uvc_roll_absolute' must be a scalar in " + context);
        }
        const int roll_value = roll_node.as<int>();
        if (roll_value <= 0 || roll_value > std::numeric_limits<uint16_t>::max())
        {
            throw std::runtime_error("field 'uvc_roll_absolute' must be in range [1, 65535] in " + context);
        }
        config.uvc_roll_absolute = roll_value;
    }
    config.input_format = OptionalString(camera_node, "input_format", "", context);
    config.capture_width = OptionalInt(camera_node, "capture_width", 0, context);
    config.capture_height = OptionalInt(camera_node, "capture_height", 0, context);
    config.width = RequirePositiveInt(camera_node, "width", context);
    config.height = RequirePositiveInt(camera_node, "height", context);
    config.fps = RequirePositiveInt(camera_node, "fps", context);
    config.output_fps = OptionalInt(camera_node, "output_fps", 0, context);
    config.eye_width = OptionalInt(camera_node, "eye_width", 0, context);
    config.eye_height = OptionalInt(camera_node, "eye_height", 0, context);
    config.video_filter = OptionalString(camera_node, "video_filter", "", context);
    config.output_files = RequireOutputFiles(camera_node, context);
    config.input_thread_queue_size = OptionalInt(camera_node, "input_thread_queue_size", 0, context);
    config.qp_init = OptionalInt(camera_node, "qp_init", config.qp_init, context);
    config.qp_max = OptionalInt(camera_node, "qp_max", config.qp_max, context);
    config.qp_min = OptionalInt(camera_node, "qp_min", config.qp_min, context);
    config.qp_max_i = OptionalInt(camera_node, "qp_max_i", config.qp_max_i, context);
    config.qp_min_i = OptionalInt(camera_node, "qp_min_i", config.qp_min_i, context);
    ValidateNonNegativeFields(config, context);
    return config;
}

CameraConfig ParseSchemaV1CameraConfig(const YAML::Node& camera_node, size_t index)
{
    const std::string context = "CameraRecorder.cameras[" + std::to_string(index) + "]";
    const YAML::Node device_node = RequireNode(camera_node, "device", context);
    const YAML::Node record_node = RequireNode(camera_node, "record", context);
    const YAML::Node output_node = RequireNode(camera_node, "output", context);

    if (!device_node.IsMap() || !record_node.IsMap() || !output_node.IsMap())
    {
        throw std::runtime_error("fields 'device', 'record', and 'output' must be maps in " + context);
    }

    CameraConfig config;
    config.name = RequireString(camera_node, "id", context);
    config.role = ParseRoleText(OptionalString(camera_node, "role", "", context));
    config.side = ParseSideText(OptionalString(camera_node, "side", "", context));
    config.kind = ParseKindText(OptionalString(camera_node, "kind", "", context));
    if (const YAML::Node groups_node = camera_node["groups"])
    {
        if (!groups_node.IsSequence())
        {
            throw std::runtime_error("field 'groups' must be a sequence in " + context);
        }
        for (size_t i = 0; i < groups_node.size(); ++i)
        {
            if (!groups_node[i].IsScalar())
            {
                throw std::runtime_error("groups[" + std::to_string(i) + "] must be a scalar in " + context);
            }
            const std::string group_name = Trim(groups_node[i].as<std::string>());
            if (group_name.empty())
            {
                throw std::runtime_error("groups[" + std::to_string(i) + "] must not be empty in " + context);
            }
            config.groups.push_back(group_name);
        }
    }

    config.device = RequireString(device_node, "path", context + ".device");
    config.input_format = OptionalString(device_node, "input_format", "", context + ".device");
    config.capture_width = OptionalInt(device_node, "capture_width", 0, context + ".device");
    config.capture_height = OptionalInt(device_node, "capture_height", 0, context + ".device");
    config.width = RequirePositiveInt(device_node, "width", context + ".device");
    config.height = RequirePositiveInt(device_node, "height", context + ".device");
    config.fps = RequirePositiveInt(device_node, "fps", context + ".device");

    const std::string pipeline = RequireString(record_node, "pipeline", context + ".record");
    config.mode = ParseModeText(pipeline);
    config.output_fps = OptionalInt(record_node, "output_fps", 0, context + ".record");
    config.input_thread_queue_size =
        OptionalInt(record_node, "input_thread_queue_size", 0, context + ".record");
    config.video_filter = OptionalString(record_node, "video_filter", "", context + ".record");
    if (const YAML::Node qp_node = record_node["qp"])
    {
        if (!qp_node.IsMap())
        {
            throw std::runtime_error("field 'qp' must be a map in " + context + ".record");
        }
        config.qp_init = OptionalInt(qp_node, "init", config.qp_init, context + ".record.qp");
        config.qp_max = OptionalInt(qp_node, "max", config.qp_max, context + ".record.qp");
        config.qp_min = OptionalInt(qp_node, "min", config.qp_min, context + ".record.qp");
        config.qp_max_i = OptionalInt(qp_node, "max_i", config.qp_max_i, context + ".record.qp");
        config.qp_min_i = OptionalInt(qp_node, "min_i", config.qp_min_i, context + ".record.qp");
    }

    config.output_files.push_back(RequireString(output_node, "file_name", context + ".output"));

    if (const YAML::Node uvc_node = device_node["uvc"])
    {
        if (!uvc_node.IsMap())
        {
            throw std::runtime_error("field 'uvc' must be a map in " + context + ".device");
        }
        if (const YAML::Node roll_node = uvc_node["roll_absolute"])
        {
            if (!roll_node.IsScalar())
            {
                throw std::runtime_error("field 'roll_absolute' must be a scalar in " + context + ".device.uvc");
            }
            const int roll_value = roll_node.as<int>();
            if (roll_value <= 0 || roll_value > std::numeric_limits<uint16_t>::max())
            {
                throw std::runtime_error(
                    "field 'roll_absolute' must be in range [1, 65535] in " + context + ".device.uvc");
            }
            config.uvc_roll_absolute = roll_value;
        }
    }

    ValidateNonNegativeFields(config, context);
    return config;
}

std::vector<CameraConfig> ParseLegacyRoot(const YAML::Node& root, const std::filesystem::path& yaml_path)
{
    static std::once_flag warning_once;
    std::call_once(warning_once, [&]() {
        DM_LOG_WARN("[camera_config] legacy flat camera yaml is deprecated but still accepted");
    });

    const YAML::Node cameras = root["cameras"];
    if (!cameras || !cameras.IsSequence() || cameras.size() == 0)
    {
        throw std::runtime_error("camera config yaml must contain non-empty 'cameras' sequence: " +
                                 yaml_path.string());
    }

    std::vector<CameraConfig> configs;
    configs.reserve(cameras.size());
    std::set<std::string> names;
    for (size_t index = 0; index < cameras.size(); ++index)
    {
        const YAML::Node camera_node = cameras[index];
        if (!camera_node.IsMap())
        {
            throw std::runtime_error("cameras[" + std::to_string(index) + "] must be a map in " +
                                     yaml_path.string());
        }
        auto config = ParseLegacyCameraConfig(camera_node, index);
        if (!names.insert(config.name).second)
        {
            throw std::runtime_error("duplicate camera name in yaml: " + config.name);
        }
        configs.push_back(std::move(config));
    }
    return configs;
}

std::vector<CameraConfig> ParseSchemaV1Root(const YAML::Node& root, const std::filesystem::path& yaml_path)
{
    const YAML::Node top = root["CameraRecorder"];
    if (!top || !top.IsMap())
    {
        throw std::runtime_error("camera config yaml must contain top-level 'CameraRecorder' map: " +
                                 yaml_path.string());
    }
    const int schema_version = RequirePositiveInt(top, "schema_version", "CameraRecorder");
    if (schema_version != 1)
    {
        throw std::runtime_error("unsupported CameraRecorder.schema_version: " + std::to_string(schema_version));
    }

    const YAML::Node cameras = RequireNode(top, "cameras", "CameraRecorder");
    if (!cameras.IsSequence() || cameras.size() == 0)
    {
        throw std::runtime_error("CameraRecorder.cameras must be a non-empty sequence: " + yaml_path.string());
    }

    std::vector<CameraConfig> configs;
    configs.reserve(cameras.size());
    std::set<std::string> names;
    for (size_t index = 0; index < cameras.size(); ++index)
    {
        const YAML::Node camera_node = cameras[index];
        if (!camera_node.IsMap())
        {
            throw std::runtime_error("CameraRecorder.cameras[" + std::to_string(index) +
                                     "] must be a map in " + yaml_path.string());
        }
        auto config = ParseSchemaV1CameraConfig(camera_node, index);
        if (!names.insert(config.name).second)
        {
            throw std::runtime_error("duplicate camera id in yaml: " + config.name);
        }
        configs.push_back(std::move(config));
    }
    return configs;
}

std::string ShellQuote(const std::string& value)
{
    std::string out = "'";
    for (char ch : value)
    {
        if (ch == '\'')
        {
            out += "'\\''";
        }
        else
        {
            out += ch;
        }
    }
    out += "'";
    return out;
}

std::string GetRkmppEncoder(const std::string& codec)
{
    return codec == "h264" ? "h264_rkmpp" : "hevc_rkmpp";
}

std::string CommonEncodeArgs(const std::string& codec)
{
    std::ostringstream oss;
    oss << "-c:v " << GetRkmppEncoder(codec) << ' '
        << "-rc_mode CQP "
        << "-qp_init 30 "
        << "-qp_max 38 "
        << "-qp_min 24 "
        << "-qp_max_i 38 "
        << "-qp_min_i 20 ";

    if (codec == "h264")
    {
        oss << "-profile:v main -level 5.1 ";
    }
    else
    {
        oss << "-profile:v main ";
    }
    return oss.str();
}

std::string CommonEncodeArgs(const std::string& codec, const CameraConfig& config)
{
    std::ostringstream oss;
    oss << "-c:v " << GetRkmppEncoder(codec) << ' '
        << "-rc_mode CQP "
        << "-qp_init " << config.qp_init << ' '
        << "-qp_max " << config.qp_max << ' '
        << "-qp_min " << config.qp_min << ' '
        << "-qp_max_i " << config.qp_max_i << ' '
        << "-qp_min_i " << config.qp_min_i << ' ';

    if (codec == "h264")
    {
        oss << "-profile:v main -level 5.1 ";
    }
    else
    {
        oss << "-profile:v main ";
    }
    return oss.str();
}

std::filesystem::path OutputPath(const CameraConfig& config, const CommandBuildOptions& options)
{
    return options.output_dir / PrimaryOutputFileName(config);
}

}  // namespace

std::vector<CameraConfig> LoadCameraConfigList(const std::filesystem::path& yaml_path)
{
    const YAML::Node root = YAML::LoadFile(yaml_path.string());
    if (root["CameraRecorder"])
    {
        return ParseSchemaV1Root(root, yaml_path);
    }
    return ParseLegacyRoot(root, yaml_path);
}

std::string CaptureInputFormat(const CameraConfig& config)
{
    return config.input_format.empty() ? "mjpeg" : config.input_format;
}

int CaptureWidth(const CameraConfig& config)
{
    return config.capture_width > 0 ? config.capture_width : config.width;
}

int CaptureHeight(const CameraConfig& config)
{
    return config.capture_height > 0 ? config.capture_height : config.height;
}

bool UsesFrameDropWithPreservedPts(const CameraConfig& config)
{
    return config.output_fps > 0 && config.output_fps < config.fps && config.fps > 0 &&
           (config.fps % config.output_fps) == 0;
}

int FrameDropModulo(const CameraConfig& config)
{
    return UsesFrameDropWithPreservedPts(config) ? (config.fps / config.output_fps) : 1;
}

int StereoSessionFps(const CameraConfig& config)
{
    return config.output_fps > 0 ? config.output_fps : config.fps;
}

std::optional<std::string> BuildVideoFilter(const CameraConfig& config)
{
    std::vector<std::string> filters;
    if (CaptureWidth(config) != config.width || CaptureHeight(config) != config.height)
    {
        std::ostringstream scale;
        scale << "scale=" << config.width << ':' << config.height << ":flags=neighbor";
        filters.push_back(scale.str());
    }

    if (!config.video_filter.empty())
    {
        filters.push_back(config.video_filter);
    }

    if (config.output_fps > 0 && config.output_fps != config.fps)
    {
        if (UsesFrameDropWithPreservedPts(config))
        {
            std::ostringstream select;
            select << "select=not(mod(n\\," << FrameDropModulo(config) << "))";
            filters.push_back(select.str());
        }
        else
        {
            filters.push_back("fps=" + std::to_string(config.output_fps));
        }
    }

    if (filters.empty())
    {
        return std::nullopt;
    }

    std::ostringstream oss;
    for (size_t index = 0; index < filters.size(); ++index)
    {
        if (index > 0)
        {
            oss << ',';
        }
        oss << filters[index];
    }
    return oss.str();
}

std::optional<std::string> BuildStereoSessionVideoFilter(const CameraConfig& config)
{
    CameraConfig session_config = config;
    session_config.fps = StereoSessionFps(config);
    session_config.output_fps = session_config.fps;
    return BuildVideoFilter(session_config);
}

std::string OutputTimingArgs(const CameraConfig& config)
{
    if (!UsesFrameDropWithPreservedPts(config))
    {
        return "";
    }
    return "-fps_mode passthrough -enc_time_base -1 ";
}

std::string BuildMainCameraCommand(const CameraConfig& config, const CommandBuildOptions& options)
{
    const std::string device = ShellQuote(config.device);
    const std::string output = ShellQuote(OutputPath(config, options).string());

    if (options.codec == "h264")
    {
        std::ostringstream oss;
        oss << options.ffmpeg_bin
            << " -hide_banner -loglevel info -nostats -debug_ts -y "
            << "-f v4l2 -input_format h264 "
            << "-framerate " << config.fps << ' '
            << "-video_size " << config.width << 'x' << config.height << ' '
            << "-copyts "
            << "-i " << device << ' '
            << "-c:v copy "
            << output;
        return oss.str();
    }

    std::ostringstream oss;
    oss << options.gst_bin << " -e "
        << "v4l2src device=" << device << " do-timestamp=true ! "
        << ShellQuote("video/x-h265,width=" + std::to_string(config.width) +
                      ",height=" + std::to_string(config.height) +
                      ",framerate=" + std::to_string(config.fps) + "/1")
        << " ! "
        << "identity silent=false ! "
        << "queue leaky=downstream max-size-buffers=4 ! "
        << "h265parse config-interval=-1 ! "
        << "matroskamux ! "
        << "filesink location=" << output;
    return WrapExecEnvCommand({"GST_DEBUG=identity:7"}, oss.str());
}

std::string BuildHybridCameraCommand(const CameraConfig& config, const CommandBuildOptions& options)
{
    const std::string device = ShellQuote(config.device);
    const std::string output = ShellQuote(OutputPath(config, options).string());
    const auto video_filter = BuildVideoFilter(config);

    std::ostringstream oss;
    oss << options.ffmpeg_bin << " -hide_banner -loglevel warning -nostats -y ";
    if (config.input_thread_queue_size > 0)
    {
        oss << "-thread_queue_size " << config.input_thread_queue_size << ' ';
    }
    oss << "-f v4l2 -input_format " << CaptureInputFormat(config) << ' '
        << "-framerate " << config.fps << ' '
        << "-video_size " << CaptureWidth(config) << 'x' << CaptureHeight(config) << ' '
        << "-i " << device << ' ';
    if (video_filter.has_value())
    {
        oss << "-vf " << ShellQuote(*video_filter) << ' ';
    }
    oss << CommonEncodeArgs(options.codec, config) << OutputTimingArgs(config)
        << "-stats_mux_pre pipe:1 "
        << "-stats_mux_pre_fmt " << ShellQuote("{pts} {tb}") << ' '
        << output;
    return oss.str();
}

std::string BuildStereoHybridCameraCommand(const CameraConfig& config, const CommandBuildOptions& options)
{
    return BuildHybridCameraCommand(config, options);
}

std::string BuildStereoSessionCommand(const CameraConfig& config,
                                      const CommandBuildOptions& options,
                                      const std::filesystem::path& output_path)
{
    const auto video_filter = BuildStereoSessionVideoFilter(config);

    std::ostringstream oss;
    oss << options.ffmpeg_bin
        << " -hide_banner -loglevel warning -nostats -y "
        << "-fflags +genpts "
        << "-f mjpeg "
        << "-framerate " << StereoSessionFps(config) << ' '
        << "-i pipe:0 "
        << "-map 0:v:0 -an ";
    if (video_filter.has_value())
    {
        oss << "-vf " << ShellQuote(*video_filter) << ' ';
    }
    oss << CommonEncodeArgs(options.codec, config)
        << "-stats_mux_pre pipe:1 "
        << "-stats_mux_pre_fmt " << ShellQuote("{pts} {tb}") << ' '
        << ShellQuote(output_path.string());
    return oss.str();
}

std::optional<StereoControlCommand> ParseStereoControlCommand(const nlohmann::json& root)
{
    if (!root.is_object())
    {
        return std::nullopt;
    }

    StereoControlCommand command;
    command.command_seq = root.value("command_seq", static_cast<uint64_t>(0));
    command.recording = root.value("recording", false);
    command.episode_dir = root.value("episode_dir", std::string());
    command.start_system_time_us = root.value("start_system_time_us", static_cast<int64_t>(0));
    command.stop_system_time_us = root.value("stop_system_time_us", static_cast<int64_t>(0));
    return command;
}

nlohmann::json BuildStereoTrackStatusJson(const StereoTrackStatus& status)
{
    nlohmann::json value = nlohmann::json::object();
    value["state"] = status.state;
    value["device"] = status.device;
    value["ready"] = status.ready;
    if (status.first_frame_system_time_us.has_value())
    {
        value["first_frame_system_time_us"] = *status.first_frame_system_time_us;
    }
    if (status.last_frame_system_time_us.has_value())
    {
        value["last_frame_system_time_us"] = *status.last_frame_system_time_us;
    }
    value["session_recording"] = status.session_recording;
    if (!status.last_error.empty())
    {
        value["last_error"] = status.last_error;
    }
    return value;
}

nlohmann::json BuildStereoServiceStatusJson(const StereoServiceStatus& status)
{
    nlohmann::json value = nlohmann::json::object();
    value["recording"] = status.recording;
    value["finalize_pending"] = status.finalize_pending;
    value["active_episode_dir"] = status.active_episode_dir;
    value["last_finalized_episode_dir"] = status.last_finalized_episode_dir;
    value["last_finalize_error"] = status.last_finalize_error;
    value["last_session"] = status.last_session;
    value["ready"] = status.ready;
    value["service_state"] = status.service_state;
    value["cameras"] = nlohmann::json::object();
    for (const auto& [camera_name, camera_status] : status.cameras)
    {
        value["cameras"][camera_name] = BuildStereoTrackStatusJson(camera_status);
    }
    return value;
}

}  // namespace ugripper::camera
