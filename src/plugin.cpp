#include "memestudio_plugin_kit.hpp"
#include "../vendor/nlohmann/json.hpp"
#include <array>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace {

using Json = nlohmann::json;
namespace pk = meme::plugin_kit;

constexpr const char* kPluginId = "memestudio.antigravity-image";

struct QwenLocalConfig {
    pk::fs::path sdServerExe;
    pk::fs::path modelDirectory;
    pk::fs::path diffusionModel;
    pk::fs::path vaePath;
    pk::fs::path llmPath;
    pk::fs::path mmprojPath;
    std::string modelAlias = "antigravity-image-local";
    uint16_t port = 18189;
    std::string bearerKey;
    bool turboPreset = false;     // Safe default: only enable 8-step/cfg=1 when Turbo model or user opts in!
    int steps = 20;
    double cfgScale = 3.5;
    double vramBudgetGiB = 0.0;
    bool retainLoaded = true;
    bool sessionTakeover = false;
};

// Safe binary GGUF v2/v3 header magic & architecture reader (never throws uncaught on partial/corrupt files!)
struct GgufProbeInfo {
    bool valid = false;
    uint32_t version = 0;
    std::string generalArchitecture;
};

GgufProbeInfo inspectGgufHeaderSafe(const pk::fs::path& filePath) noexcept {
    GgufProbeInfo info;
    try {
        std::ifstream f(filePath, std::ios::binary);
        if (!f) return info;
        char magic[4]{};
        if (!f.read(magic, 4) || std::memcmp(magic, "GGUF", 4) != 0) return info;
        uint32_t ver = 0;
        if (!f.read(reinterpret_cast<char*>(&ver), 4) || (ver != 2 && ver != 3)) return info;
        info.valid = true;
        info.version = ver;

        uint64_t tensorCount = 0;
        uint64_t kvCount = 0;
        if (!f.read(reinterpret_cast<char*>(&tensorCount), 8) ||
            !f.read(reinterpret_cast<char*>(&kvCount), 8)) {
            return info;
        }

        // Scan up to first 32 small metadata KV pairs to locate "general.architecture" if present
        auto readString = [&](std::string& out) -> bool {
            uint64_t len = 0;
            if (!f.read(reinterpret_cast<char*>(&len), 8) || len > 4096) return false;
            out.resize(static_cast<size_t>(len));
            return len == 0 || static_cast<bool>(f.read(out.data(), static_cast<std::streamsize>(len)));
        };

        for (uint64_t i = 0; i < std::min<uint64_t>(kvCount, 16); ++i) {
            std::string key;
            uint32_t valType = 0;
            if (!readString(key) || !f.read(reinterpret_cast<char*>(&valType), 4)) break;
            if (valType == 8 /* GGUF_TYPE_STRING */) {
                std::string sval;
                if (!readString(sval)) break;
                if (key == "general.architecture") {
                    info.generalArchitecture = std::move(sval);
                    break;
                }
            } else {
                break; // Stop lightweight scan at non-string metadata without risking complex array seeks
            }
        }
    } catch (...) {
        return GgufProbeInfo{};
    }
    return info;
}

std::pair<uint32_t, uint32_t> parseAndValidateQwenSize(std::string_view sizeSpec) {
    std::string s = pk::trimAscii(sizeSpec);
    if (s.empty() || s == "auto") return {1024, 1024};
    auto xPos = s.find_first_of("xX");
    if (xPos == std::string::npos) {
        throw pk::PluginError("尺寸格式无效（示例：1024x1024 或 auto）");
    }
    int w = 0, h = 0;
    try {
        w = std::stoi(s.substr(0, xPos));
        h = std::stoi(s.substr(xPos + 1));
    } catch (...) {
        throw pk::PluginError("尺寸宽高必须是整数");
    }
    // Unified 32px alignment validation across front-end and backend (fixes BUG-27 16 vs 32 conflict!)
    if (w < 128 || h < 128 || w > 2048 || h > 2048 || (w % 32) != 0 || (h % 32) != 0 ||
        int64_t(w) * h > 4ll * 1024 * 1024) {
        throw pk::PluginError("Qwen-Image 尺寸每边必须在 128..2048 之间且为 32 的整数倍（最大 400 万像素）");
    }
    return {static_cast<uint32_t>(w), static_cast<uint32_t>(h)};
}

// Kernel-bound process wrapper using JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE so zero orphan sd-server.exe
// processes ever survive plugin unload or host crash!
class ManagedDaemonProcess {
    HANDLE job_ = nullptr;
    HANDLE process_ = nullptr;
public:
    ManagedDaemonProcess() = default;
    ~ManagedDaemonProcess() { stop(); }
    ManagedDaemonProcess(const ManagedDaemonProcess&) = delete;
    ManagedDaemonProcess& operator=(const ManagedDaemonProcess&) = delete;

    bool isRunning() const noexcept {
        if (!process_) return false;
        return WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
    }

    uint32_t pid() const noexcept {
        return process_ ? GetProcessId(process_) : 0;
    }

    void start(const pk::fs::path& exe, const std::vector<std::wstring>& args, const pk::fs::path& logPath) {
        stop();
        if (exe.empty() || !pk::fs::is_regular_file(exe)) {
            throw pk::PluginError("本地推理引擎可执行文件不存在：" + pk::wideToUtf8(exe.wstring()));
        }
        if (!logPath.parent_path().empty()) {
            pk::fs::create_directories(logPath.parent_path());
        }

        job_ = CreateJobObjectW(nullptr, nullptr);
        if (!job_) throw pk::PluginError("创建推理引擎 Job Object 失败");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits));

        SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE logFile = CreateFileW(
            logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
            &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (logFile == INVALID_HANDLE_VALUE) {
            CloseHandle(job_); job_ = nullptr;
            throw pk::PluginError("无法打开位于 AppData 的推理日志文件");
        }

        auto quoteArg = [](std::wstring_view a) -> std::wstring {
            if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring_view::npos) return std::wstring(a);
            std::wstring q = L"\"";
            for (wchar_t ch : a) {
                if (ch == L'"') q += L"\\\"";
                else q.push_back(ch);
            }
            q.push_back(L'"');
            return q;
        };

        std::wstring cmdLine = quoteArg(exe.wstring());
        for (const auto& a : args) {
            cmdLine.push_back(L' ');
            cmdLine += quoteArg(a);
        }

        SIZE_T attrSize = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
        std::vector<uint8_t> attrBuf(attrSize);
        auto* attrList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
        InitializeProcThreadAttributeList(attrList, 1, 0, &attrSize);
        HANDLE whitelist[1] = {logFile};
        UpdateProcThreadAttribute(attrList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, whitelist, sizeof(whitelist), nullptr, nullptr);

        STARTUPINFOEXW si{};
        si.StartupInfo.cb = sizeof(STARTUPINFOEXW);
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.StartupInfo.wShowWindow = SW_HIDE;
        si.StartupInfo.hStdInput = INVALID_HANDLE_VALUE;
        si.StartupInfo.hStdOutput = logFile;
        si.StartupInfo.hStdError = logFile;
        si.lpAttributeList = attrList;

        PROCESS_INFORMATION pi{};
        pk::fs::path workDir = exe.parent_path();
        BOOL ok = CreateProcessW(
            exe.c_str(), cmdLine.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
            nullptr, workDir.empty() ? nullptr : workDir.c_str(), &si.StartupInfo, &pi);

        DeleteProcThreadAttributeList(attrList);
        CloseHandle(logFile);

        if (!ok) {
            CloseHandle(job_); job_ = nullptr;
            throw pk::PluginError("启动 sd-server 进程失败（Windows 错误码 " + std::to_string(GetLastError()) + "）");
        }

        process_ = pi.hProcess;
        if (!AssignProcessToJobObject(job_, process_)) {
            TerminateProcess(process_, 1);
            CloseHandle(pi.hThread);
            stop();
            throw pk::PluginError("无法将 sd-server 绑定至 Job Object");
        }
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);
    }

    void stop() noexcept {
        if (job_) {
            TerminateJobObject(job_, 0);
        }
        if (process_) {
            WaitForSingleObject(process_, 2000);
            CloseHandle(process_);
            process_ = nullptr;
        }
        if (job_) {
            CloseHandle(job_);
            job_ = nullptr;
        }
    }
};

struct QwenImagePlugin {
    pk::HostClient host;
    mutable std::mutex mutex;
    QwenLocalConfig config;
    ManagedDaemonProcess daemon;

    explicit QwenImagePlugin(const MemeStudioHostV2* h) : host(h, kPluginId) {
        config.bearerKey = "antigravity-" + pk::randomHex(12);
        loadSettingsFromAppData();
    }

    ~QwenImagePlugin() {
        clearEndpointOverride();
        daemon.stop();
    }

    // 100% isolated inside %LOCALAPPDATA%\MemeStudio\plugins\memestudio.antigravity-image\ (fixes BUG-15!)
    pk::fs::path settingsFilePath() const { return host.dataDir() / L"antigravity-config.json"; }
    pk::fs::path logFilePath() const { return host.dataDir() / L"sd-server.log"; }

    void clearEndpointOverride() noexcept {
        if (host.raw() && host.raw()->set_session_endpoint_override) {
            host.raw()->set_session_endpoint_override(host.raw()->host_ctx, kPluginId, nullptr);
        }
    }

    void syncEndpointOverrideLocked() {
        if (!host.raw()->set_session_endpoint_override) return;
        if (config.sessionTakeover) {
            Json ep{
                {"base_url", "http://127.0.0.1:" + std::to_string(config.port) + "/v1"},
                {"api_key", config.bearerKey},
                {"model", config.modelAlias}
            };
            std::string dumped = ep.dump();
            host.raw()->set_session_endpoint_override(host.raw()->host_ctx, kPluginId, dumped.c_str());
        } else {
            host.raw()->set_session_endpoint_override(host.raw()->host_ctx, kPluginId, nullptr);
        }
    }

    Json inspectModels(const QwenLocalConfig& cfg) const {
        Json report{
            {"explicit_paths", false},
            {"diffusion_candidates", Json::array()},
            {"llm_candidates", Json::array()},
            {"mmproj_candidates", Json::array()},
            {"vae_candidates", Json::array()},
            {"skipped_corrupt_files", Json::array()}
        };

        // BUG-27 fix: When explicit component paths are already configured, validate ONLY those files
        // instead of blindly scanning every file in modelDirectory and crashing on unrelated/partial GGUFs!
        if (!cfg.diffusionModel.empty()) {
            report["explicit_paths"] = true;
            if (!pk::fs::is_regular_file(cfg.diffusionModel)) {
                throw pk::PluginError("指定的扩散模型文件不存在：" + pk::wideToUtf8(cfg.diffusionModel.wstring()));
            }
            auto info = inspectGgufHeaderSafe(cfg.diffusionModel);
            if (!info.valid) {
                throw pk::PluginError("指定的扩散模型不是有效的 GGUF v2/v3 文件");
            }
            report["diffusion_candidates"].push_back(pk::wideToUtf8(cfg.diffusionModel.wstring()));
            if (!cfg.vaePath.empty()) {
                if (!pk::fs::is_regular_file(cfg.vaePath)) throw pk::PluginError("指定的 VAE 文件不存在");
                report["vae_candidates"].push_back(pk::wideToUtf8(cfg.vaePath.wstring()));
            }
            if (!cfg.llmPath.empty()) {
                if (!pk::fs::is_regular_file(cfg.llmPath)) throw pk::PluginError("指定的文本编码器文件不存在");
                report["llm_candidates"].push_back(pk::wideToUtf8(cfg.llmPath.wstring()));
            }
            return report;
        }

        std::error_code ec;
        if (cfg.modelDirectory.empty() || !pk::fs::is_directory(cfg.modelDirectory, ec)) {
            throw pk::PluginError("模型目录不存在");
        }

        for (const auto& entry : pk::fs::directory_iterator(cfg.modelDirectory, ec)) {
            if (!entry.is_regular_file()) continue;
            std::wstring ext = entry.path().extension().wstring();
            for (auto& ch : ext) ch = static_cast<wchar_t>(std::towlower(ch));
            std::string filenameLower = pk::wideToUtf8(entry.path().filename().wstring());
            for (char& c : filenameLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

            if (ext == L".gguf") {
                auto info = inspectGgufHeaderSafe(entry.path());
                if (!info.valid) {
                    report["skipped_corrupt_files"].push_back(pk::wideToUtf8(entry.path().filename().wstring()));
                    continue;
                }
                std::string fullUtf8 = pk::wideToUtf8(entry.path().wstring());
                if (filenameLower.find("mmproj") != std::string::npos) {
                    report["mmproj_candidates"].push_back(fullUtf8);
                } else if (info.generalArchitecture == "antigravity2vl" || info.generalArchitecture == "antigravity3vl" ||
                           info.generalArchitecture == "llama" || filenameLower.find("vl") != std::string::npos) {
                    report["llm_candidates"].push_back(fullUtf8);
                } else {
                    report["diffusion_candidates"].push_back(fullUtf8);
                }
            } else if (ext == L".safetensors") {
                std::string fullUtf8 = pk::wideToUtf8(entry.path().wstring());
                if (filenameLower.find("vae") != std::string::npos) {
                    report["vae_candidates"].push_back(fullUtf8);
                } else {
                    report["diffusion_candidates"].push_back(fullUtf8);
                }
            }
        }
        return report;
    }

    Json toPublicJsonLocked() const {
        return Json{
            {"sd_executable", pk::wideToUtf8(config.sdServerExe.wstring())},
            {"model_directory", pk::wideToUtf8(config.modelDirectory.wstring())},
            {"diffusion_model", pk::wideToUtf8(config.diffusionModel.wstring())},
            {"vae", pk::wideToUtf8(config.vaePath.wstring())},
            {"llm", pk::wideToUtf8(config.llmPath.wstring())},
            {"mmproj", pk::wideToUtf8(config.mmprojPath.wstring())},
            {"model_alias", config.modelAlias},
            {"port", config.port},
            {"turbo", config.turboPreset},
            {"steps", config.steps},
            {"cfg_scale", config.cfgScale},
            {"vram_budget_gib", config.vramBudgetGiB},
            {"retain_loaded", config.retainLoaded},
            {"session_takeover", config.sessionTakeover},
            {"running", daemon.isRunning()},
            {"pid", daemon.pid()},
            {"data_directory", pk::wideToUtf8(host.dataDir().wstring())}
        };
    }

    void updateConfig(const Json& j, bool persist) {
        std::lock_guard lock(mutex);
        auto optPath = [&](const char* k, pk::fs::path& dst) {
            if (j.contains(k) && j[k].is_string()) {
                dst = pk::utf8ToWideStrict(j[k].get<std::string>());
            }
        };
        optPath("sd_executable", config.sdServerExe);
        optPath("model_directory", config.modelDirectory);
        optPath("diffusion_model", config.diffusionModel);
        optPath("vae", config.vaePath);
        optPath("llm", config.llmPath);
        optPath("mmproj", config.mmprojPath);

        if (j.contains("model_alias") && j["model_alias"].is_string()) {
            std::string alias = pk::trimAscii(j["model_alias"].get<std::string>());
            if (alias.empty()) throw pk::PluginError("模型别名不能为空");
            config.modelAlias = std::move(alias);
        }
        if (j.contains("port")) {
            int p = j["port"].get<int>();
            if (p < 1024 || p > 65535) throw pk::PluginError("本地监听端口必须在 1024..65535 之间");
            config.port = static_cast<uint16_t>(p);
        }
        if (j.contains("api_key") && j["api_key"].is_string() && !j["api_key"].get<std::string>().empty()) {
            config.bearerKey = j["api_key"].get<std::string>();
        } else if (j.contains("encrypted_key") && j["encrypted_key"].is_string() && !j["encrypted_key"].get<std::string>().empty()) {
            try { config.bearerKey = host.unprotectSecret(j["encrypted_key"].get<std::string>()); } catch (...) {}
        }
        if (j.contains("turbo") && j["turbo"].is_boolean()) config.turboPreset = j["turbo"].get<bool>();
        if (j.contains("steps")) config.steps = std::clamp(j["steps"].get<int>(), 1, 100);
        if (j.contains("cfg_scale")) config.cfgScale = std::clamp(j["cfg_scale"].get<double>(), 1.0, 30.0);
        if (j.contains("vram_budget_gib")) config.vramBudgetGiB = std::max(0.0, j["vram_budget_gib"].get<double>());
        if (j.contains("retain_loaded") && j["retain_loaded"].is_boolean()) config.retainLoaded = j["retain_loaded"].get<bool>();
        if (j.contains("session_takeover") && j["session_takeover"].is_boolean()) config.sessionTakeover = j["session_takeover"].get<bool>();

        if (persist) {
            Json disk = toPublicJsonLocked();
            disk.erase("running");
            disk.erase("pid");
            disk.erase("data_directory");
            disk["encrypted_key"] = host.protectSecret(config.bearerKey);
            std::string dumped = disk.dump(2);
            pk::writeFileAtomic(settingsFilePath(), std::span<const uint8_t>(
                reinterpret_cast<const uint8_t*>(dumped.data()), dumped.size()));
        }
        syncEndpointOverrideLocked();
    }

    void loadSettingsFromAppData() {
        try {
            auto p = settingsFilePath();
            if (!pk::fs::exists(p)) return;
            auto raw = pk::readFileBytes(p, 256 * 1024);
            updateConfig(Json::parse(raw.begin(), raw.end()), false);
        } catch (...) {}
    }

    void startLocalServer() {
        std::lock_guard lock(mutex);
        if (daemon.isRunning()) return;
        if (config.diffusionModel.empty()) {
            throw pk::PluginError("启动前请先指定扩散模型文件");
        }
        std::vector<std::wstring> args{
            L"--listen-ip", L"127.0.0.1",
            L"--listen-port", std::to_wstring(config.port),
            L"--diffusion-model", config.diffusionModel.wstring()
        };
        if (!config.vaePath.empty()) {
            args.push_back(L"--vae");
            args.push_back(config.vaePath.wstring());
        }
        if (!config.llmPath.empty()) {
            args.push_back(L"--llm");
            args.push_back(config.llmPath.wstring());
        }
        if (!config.mmprojPath.empty()) {
            args.push_back(L"--llm-vision");
            args.push_back(config.mmprojPath.wstring());
        }
        daemon.start(config.sdServerExe, args, logFilePath());
        syncEndpointOverrideLocked();
    }

    void stopLocalServer() {
        std::lock_guard lock(mutex);
        daemon.stop();
        config.sessionTakeover = false;
        syncEndpointOverrideLocked();
    }

    int32_t interceptCall(
        std::string_view op,
        const Json& providerJson,
        const Json& reqJson,
        const MemeStudioBytesV2* uploads,
        size_t uploadCount,
        MemeStudioCancelTokenV2* cancel,
        MemeStudioBytesV2* outResp,
        MemeStudioBytesV2* outErr) {
        QwenLocalConfig snap;
        {
            std::lock_guard lock(mutex);
            if (!config.sessionTakeover) return 1; // Pass-through when local takeover is not active
            snap = config;
        }

        if (op == "models") {
            host.writeOutBytes(outResp, Json::array({snap.modelAlias}).dump());
            return 0;
        }

        auto [width, height] = parseAndValidateQwenSize(providerJson.value("size", std::string("auto")));
        int steps = snap.turboPreset ? 8 : snap.steps;
        double cfg = snap.turboPreset ? 1.0 : snap.cfgScale;

        Json sdBody{
            {"model", snap.modelAlias},
            {"prompt", reqJson.value("prompt", std::string())},
            {"width", width},
            {"height", height},
            {"steps", steps},
            {"cfg_scale", cfg}
        };
        if (snap.turboPreset) {
            sdBody["sigmas"] = {1.0, 0.978453, 0.95418, 0.926626, 0.89508, 0.845148, 0.704534, 0.414568, 0.0};
        }
        if (uploadCount > 0 && uploads) {
            Json refImages = Json::array();
            for (size_t i = 0; i < uploadCount; ++i) {
                refImages.push_back(pk::base64Encode(std::span<const uint8_t>(uploads[i].data, uploads[i].size)));
            }
            sdBody["reference_images"] = std::move(refImages);
        }

        std::string bodyStr = sdBody.dump();
        std::string endpoint = "http://127.0.0.1:" + std::to_string(snap.port) +
                               (op == "generate" ? "/v1/images/generations" : "/v1/images/edits");
        Json httpSpec{
            {"url", endpoint},
            {"method", "POST"},
            {"timeout_seconds", std::clamp(providerJson.value("timeout_seconds", 900), 5, 3600)},
            {"allow_loopback_http", true},
            {"headers", {
                {"Authorization", "Bearer " + snap.bearerKey},
                {"Content-Type", "application/json"}
            }}
        };
        std::string specStr = httpSpec.dump();

        uint32_t status = 0;
        int32_t streamed = 0;
        MemeStudioBytesV2 respBytes{};
        MemeStudioBytesV2 errBytes{};
        int32_t hrc = host.raw()->http_send(
            host.raw()->host_ctx, specStr.c_str(),
            reinterpret_cast<const uint8_t*>(bodyStr.data()), bodyStr.size(),
            cancel, nullptr, nullptr, &status, &streamed, &respBytes, &errBytes);
        std::string hErr = host.consumeBytesToString(errBytes);
        pk::Bytes respVec = host.consumeBytesToVec(respBytes);
        if (hrc != 0) {
            host.writeOutBytes(outErr, "连接本地 sd-server 推理服务失败：" + hErr);
            return -1;
        }
        if (status < 200 || status >= 300) {
            host.writeOutBytes(outErr, "本地 sd-server 返回 HTTP " + std::to_string(status));
            return -1;
        }

        auto parsed = Json::parse(respVec.begin(), respVec.end());
        if (!parsed.contains("data") || !parsed["data"].is_array() || parsed["data"].empty() ||
            !parsed["data"][0].contains("b64_json")) {
            host.writeOutBytes(outErr, "本地推理响应缺少 data[0].b64_json");
            return -1;
        }
        pk::Bytes pngBytes = pk::base64Decode(parsed["data"][0]["b64_json"].get<std::string>());
        host.writeOutRawBytes(outResp, pngBytes);
        return 0;
    }
};

template <typename PluginT>
struct LocalSdEnginePanel {
    PluginT* plugin = nullptr;
    HWND hwnd = nullptr;
    HFONT font = nullptr;

    HWND sdExe = nullptr, modelDir = nullptr, diffEdit = nullptr, vaeEdit = nullptr, llmEdit = nullptr, mmprojEdit = nullptr;
    HWND portEdit = nullptr, aliasEdit = nullptr, stepsEdit = nullptr, cfgEdit = nullptr;
    HWND turboCheck = nullptr, takeoverCheck = nullptr, logBox = nullptr, statusLabel = nullptr;

    ~LocalSdEnginePanel() {
        if (font) DeleteObject(font);
    }

    enum ControlIds : int {
        IdSdExeEdit      = 3701,
        IdPickSdExeBtn   = 3702,
        IdModelDirEdit   = 3703,
        IdPickModelDir   = 3704,
        IdScanDirBtn     = 3705,
        IdDiffEdit       = 3706,
        IdPickDiffBtn    = 3707,
        IdVaeEdit        = 3708,
        IdPickVaeBtn     = 3709,
        IdLlmEdit        = 3710,
        IdPickLlmBtn     = 3711,
        IdMmprojEdit     = 3712,
        IdPickMmprojBtn  = 3713,
        IdPortEdit       = 3714,
        IdAliasEdit      = 3715,
        IdStepsEdit      = 3716,
        IdCfgEdit        = 3717,
        IdTurboCheck     = 3718,
        IdTakeoverCheck  = 3719,
        IdLogViewer      = 3720,
        IdRefreshLogBtn  = 3721,
        IdStartSrvBtn    = 3722,
        IdStopSrvBtn     = 3723,
        IdSaveCfgBtn     = 3724,
        IdClosePanelBtn  = 3725
    };

    Json gatherFormJson(bool forceTakeoverOnStart = false) const {
        int port = pk::parseIntField(portEdit, "监听端口", 1024, 65535);
        int steps = pk::parseIntField(stepsEdit, "推理步数", 1, 100);
        double cfg = pk::parseDoubleField(cfgEdit, "CFG 系数", 1.0, 30.0);
        std::string alias = pk::trimAscii(pk::getWindowTextUtf8(aliasEdit));
        if (alias.empty()) alias = "local-sdcpp-model";

        return Json{
            {"sd_executable", pk::trimAscii(pk::getWindowTextUtf8(sdExe))},
            {"model_directory", pk::trimAscii(pk::getWindowTextUtf8(modelDir))},
            {"diffusion_model", pk::trimAscii(pk::getWindowTextUtf8(diffEdit))},
            {"vae", pk::trimAscii(pk::getWindowTextUtf8(vaeEdit))},
            {"llm", pk::trimAscii(pk::getWindowTextUtf8(llmEdit))},
            {"mmproj", pk::trimAscii(pk::getWindowTextUtf8(mmprojEdit))},
            {"port", port},
            {"model_alias", alias},
            {"steps", steps},
            {"cfg_scale", cfg},
            {"turbo", pk::isButtonChecked(turboCheck)},
            {"session_takeover", forceTakeoverOnStart ? true : pk::isButtonChecked(takeoverCheck)}
        };
    }

    void refreshStatusAndLog() {
        bool running = false;
        uint32_t pid = 0;
        {
            std::lock_guard lk(plugin->mutex);
            running = plugin->daemon.isRunning();
            pid = plugin->daemon.pid();
        }
        std::string stText = running
            ? ("引擎状态：运行中 (PID " + std::to_string(pid) + ") · 数据目录隔离于 %LOCALAPPDATA%")
            : "引擎状态：未启动 · 所有日志与配置均安全隔离在 %LOCALAPPDATA%";
        pk::setWindowTextUtf8(statusLabel, stText);

        std::string tail = pk::readFileTailLossyUtf8(plugin->logFilePath(), 16 * 1024);
        pk::setWindowTextUtf8(logBox, tail.empty() ? "（暂无 sd-server 运行日志）" : tail);
    }

    static intptr_t MEMESTUDIO_CALL wndProc(void* ctx, void* rawHwnd, uint32_t msg, uintptr_t wp, intptr_t lp) {
        auto* st = static_cast<LocalSdEnginePanel*>(ctx);
        HWND w = static_cast<HWND>(rawHwnd);
        if (msg == WM_CREATE) {
            st->hwnd = w;
            pk::PanelBuilder ui(w);
            st->font = pk::createDpiUiFont(ui.dpi().dpi, 14);

            auto cur = [&]() {
                std::lock_guard lk(st->plugin->mutex);
                return st->plugin->config;
            }();

            ui.label(L"sd-server.exe 路径：", 16, 16, 145, 20);
            st->sdExe = ui.edit(IdSdExeEdit, 162, 12, 518, 26, pk::wideToUtf8(cur.sdServerExe.wstring()));
            ui.button(L"浏览…", IdPickSdExeBtn, 686, 12, 78, 26);

            ui.label(L"GGUF 模型目录：", 16, 48, 145, 20);
            st->modelDir = ui.edit(IdModelDirEdit, 162, 44, 434, 26, pk::wideToUtf8(cur.modelDirectory.wstring()));
            ui.button(L"选择…", IdPickModelDir, 602, 44, 78, 26);
            ui.button(L"自动扫描", IdScanDirBtn, 686, 44, 78, 26);

            ui.label(L"扩散模型权重：", 16, 80, 145, 20);
            st->diffEdit = ui.edit(IdDiffEdit, 162, 76, 518, 26, pk::wideToUtf8(cur.diffusionModel.wstring()));
            ui.button(L"选择…", IdPickDiffBtn, 686, 76, 78, 26);

            ui.label(L"VAE 权重 (可选)：", 16, 112, 145, 20);
            st->vaeEdit = ui.edit(IdVaeEdit, 162, 108, 518, 26, pk::wideToUtf8(cur.vaePath.wstring()));
            ui.button(L"选择…", IdPickVaeBtn, 686, 108, 78, 26);

            ui.label(L"文本编码器 LLM：", 16, 144, 145, 20);
            st->llmEdit = ui.edit(IdLlmEdit, 162, 140, 518, 26, pk::wideToUtf8(cur.llmPath.wstring()));
            ui.button(L"选择…", IdPickLlmBtn, 686, 140, 78, 26);

            ui.label(L"视觉投影 mmproj：", 16, 176, 145, 20);
            st->mmprojEdit = ui.edit(IdMmprojEdit, 162, 172, 518, 26, pk::wideToUtf8(cur.mmprojPath.wstring()));
            ui.button(L"选择…", IdPickMmprojBtn, 686, 172, 78, 26);

            ui.label(L"本地端口：", 16, 214, 80, 20);
            st->portEdit = ui.edit(IdPortEdit, 96, 210, 76, 26, std::to_string(cur.port), ES_NUMBER);
            ui.label(L"模型别名：", 188, 214, 80, 20);
            st->aliasEdit = ui.edit(IdAliasEdit, 268, 210, 170, 26, cur.modelAlias);
            ui.label(L"步数：", 454, 214, 48, 20);
            st->stepsEdit = ui.edit(IdStepsEdit, 502, 210, 62, 26, std::to_string(cur.steps), ES_NUMBER);
            ui.label(L"CFG：", 580, 214, 46, 20);
            char cfgBuf[32]{};
            std::snprintf(cfgBuf, sizeof(cfgBuf), "%.2g", cur.cfgScale);
            st->cfgEdit = ui.edit(IdCfgEdit, 626, 210, 62, 26, cfgBuf);

            st->turboCheck = ui.checkbox(L"启用 Turbo 8步极速采样时间表", IdTurboCheck, 16, 246, 250, 24, cur.turboPreset);
            st->takeoverCheck = ui.checkbox(L"接管当前主界面会话连接（不覆盖已保存商业密钥）", IdTakeoverCheck, 286, 246, 420, 24, cur.sessionTakeover);

            ui.label(L"sd-server 尾部日志：", 16, 280, 200, 20);
            ui.button(L"刷新日志", IdRefreshLogBtn, 686, 276, 78, 24);
            st->logBox = ui.multilineEdit(IdLogViewer, 16, 304, 748, 176, "", true);

            st->statusLabel = ui.label(L"", 16, 492, 748, 22);
            ui.button(L"启动引擎并接管", IdStartSrvBtn, 290, 522, 136, 34, true);
            ui.button(L"停止并释放显存", IdStopSrvBtn, 434, 522, 132, 34);
            ui.button(L"仅保存设置", IdSaveCfgBtn, 574, 522, 102, 34);
            ui.button(L"关闭", IdClosePanelBtn, 684, 522, 80, 34);

            ui.applyFont(st->font);
            st->refreshStatusAndLog();
            return 0;
        }
        if (msg == WM_COMMAND) {
            int id = LOWORD(wp);
            auto pickToEdit = [&](HWND targetEdit, bool folder, const wchar_t* promptTitle) {
                if (auto p = pk::pickSinglePath(w, false, folder, promptTitle)) {
                    pk::setWindowTextUtf8(targetEdit, pk::wideToUtf8(p->wstring()));
                }
            };
            if (id == IdPickSdExeBtn)  { pickToEdit(st->sdExe, false, L"选择 sd-server.exe"); return 0; }
            if (id == IdPickModelDir)  { pickToEdit(st->modelDir, true, L"选择 GGUF 模型文件夹"); return 0; }
            if (id == IdPickDiffBtn)   { pickToEdit(st->diffEdit, false, L"选择扩散模型文件"); return 0; }
            if (id == IdPickVaeBtn)    { pickToEdit(st->vaeEdit, false, L"选择 VAE 权重文件"); return 0; }
            if (id == IdPickLlmBtn)    { pickToEdit(st->llmEdit, false, L"选择 LLM 文本编码器"); return 0; }
            if (id == IdPickMmprojBtn) { pickToEdit(st->mmprojEdit, false, L"选择视觉投影 mmproj"); return 0; }

            if (id == IdScanDirBtn) {
                try {
                    auto scanCfg = [&]() {
                        std::lock_guard lk(st->plugin->mutex);
                        return st->plugin->config;
                    }();
                    scanCfg.diffusionModel.clear();
                    scanCfg.modelDirectory = pk::utf8ToWideStrict(pk::trimAscii(pk::getWindowTextUtf8(st->modelDir)));
                    Json rep = st->plugin->inspectModels(scanCfg);
                    if (!rep["diffusion_candidates"].empty()) pk::setWindowTextUtf8(st->diffEdit, rep["diffusion_candidates"][0].get<std::string>());
                    if (!rep["vae_candidates"].empty()) pk::setWindowTextUtf8(st->vaeEdit, rep["vae_candidates"][0].get<std::string>());
                    if (!rep["llm_candidates"].empty()) pk::setWindowTextUtf8(st->llmEdit, rep["llm_candidates"][0].get<std::string>());
                    if (!rep["mmproj_candidates"].empty()) pk::setWindowTextUtf8(st->mmprojEdit, rep["mmproj_candidates"][0].get<std::string>());
                    pk::showPanelInfo(w, "模型目录扫描完成，已填入检测到的候选组件。", L"扫描完成");
                } catch (const std::exception& e) {
                    pk::showPanelError(w, e.what(), L"模型目录扫描");
                }
                return 0;
            }
            if (id == IdRefreshLogBtn) { st->refreshStatusAndLog(); return 0; }
            if (id == IdStartSrvBtn) {
                try {
                    pk::setButtonChecked(st->takeoverCheck, true);
                    st->plugin->updateConfig(st->gatherFormJson(true), true);
                    st->plugin->startLocalServer();
                    st->refreshStatusAndLog();
                } catch (const std::exception& e) {
                    pk::showPanelError(w, e.what(), L"启动本地推理服务");
                }
                return 0;
            }
            if (id == IdStopSrvBtn) {
                st->plugin->stopLocalServer();
                pk::setButtonChecked(st->takeoverCheck, false);
                st->refreshStatusAndLog();
                return 0;
            }
            if (id == IdSaveCfgBtn) {
                try {
                    st->plugin->updateConfig(st->gatherFormJson(false), true);
                    DestroyWindow(w);
                } catch (const std::exception& e) {
                    pk::showPanelError(w, e.what(), L"保存设置");
                }
                return 0;
            }
            if (id == IdClosePanelBtn || id == IDCANCEL) {
                DestroyWindow(w);
                return 0;
            }
        }
        if (msg == WM_CLOSE) {
            DestroyWindow(w);
            return 0;
        }
        return DefWindowProcW(w, msg, wp, lp);
    }

    static void MEMESTUDIO_CALL onDestroy(void* ctx) { delete static_cast<LocalSdEnginePanel*>(ctx); }
};

struct QwenPanelContext {
    QwenImagePlugin* plugin = nullptr;
    static intptr_t MEMESTUDIO_CALL wndProc(void*, void* rawHwnd, uint32_t msg, uintptr_t wp, intptr_t lp) {
        return DefWindowProcW(static_cast<HWND>(rawHwnd), msg, wp, lp);
    }
    static void MEMESTUDIO_CALL onDestroy(void* ctx) { delete static_cast<QwenPanelContext*>(ctx); }
};

int32_t MEMESTUDIO_CALL abiCreate(const MemeStudioHostV2* host, void** outInst, MemeStudioBytesV2* outErr) {
    if (!host || !outInst || host->abi_version != MEMESTUDIO_PLUGIN_ABI_V2) return -1;
    try {
        *outInst = new QwenImagePlugin(host);
        return 0;
    } catch (const std::exception& e) {
        pk::HostClient(host, kPluginId).writeOutBytes(outErr, e.what());
        return -1;
    }
}

void MEMESTUDIO_CALL abiDestroy(void* inst) { delete static_cast<QwenImagePlugin*>(inst); }

int32_t MEMESTUDIO_CALL abiInvokeCommand(
    void* rawInst, const char* commandId, const char* argsJson, void* ownerHwnd,
    MemeStudioCancelTokenV2*, MemeStudioBytesV2* outResult, MemeStudioBytesV2* outErr) {
    auto* self = static_cast<QwenImagePlugin*>(rawInst);
    if (!self || !commandId) return -1;
    try {
        std::string cmd(commandId);
        Json args = (argsJson && argsJson[0]) ? Json::parse(argsJson) : Json::object();

        
        if (cmd == "open-panel") {
            using PanelT = LocalSdEnginePanel<std::remove_pointer_t<decltype(self)>>;
            auto* pctx = new PanelT{self};
            if (self->host.raw()->open_panel(
                    self->host.raw()->host_ctx, "local-sd-engine", ownerHwnd,
                    780, 572, "本地 GGUF 推理引擎配置",
                    PanelT::wndProc, pctx, PanelT::onDestroy) != 0) {
                delete pctx;
                throw pk::PluginError("无法打开本地推理面板");
            }
            self->host.writeOutBytes(outResult, R"({"opened":true})");
            return 0;
        }
        if (cmd == "configure") {
            self->updateConfig(args, true);
            std::lock_guard lock(self->mutex);
            self->host.writeOutBytes(outResult, self->toPublicJsonLocked().dump());
            return 0;
        }
        if (cmd == "inspect-models") {
            QwenLocalConfig tmp;
            {
                std::lock_guard lock(self->mutex);
                tmp = self->config;
            }
            if (args.contains("model_directory") && args["model_directory"].is_string()) {
                tmp.modelDirectory = pk::utf8ToWideStrict(args["model_directory"].get<std::string>());
            }
            if (args.contains("diffusion_model") && args["diffusion_model"].is_string()) {
                tmp.diffusionModel = pk::utf8ToWideStrict(args["diffusion_model"].get<std::string>());
            }
            self->host.writeOutBytes(outResult, self->inspectModels(tmp).dump());
            return 0;
        }
        if (cmd == "start") {
            if (!args.empty()) self->updateConfig(args, true);
            self->startLocalServer();
            std::lock_guard lock(self->mutex);
            self->host.writeOutBytes(outResult, self->toPublicJsonLocked().dump());
            return 0;
        }
        if (cmd == "stop") {
            self->stopLocalServer();
            std::lock_guard lock(self->mutex);
            self->host.writeOutBytes(outResult, self->toPublicJsonLocked().dump());
            return 0;
        }
        if (cmd == "read-log") {
            // Tail-only lossy UTF-8 read (fixes BUG-15 GBK wipeout & full-file UI stall!)
            std::string tail = pk::readFileTailLossyUtf8(self->logFilePath(), 24 * 1024);
            self->host.writeOutBytes(outResult, Json{{"log", tail}}.dump());
            return 0;
        }
        if (cmd == "status") {
            std::lock_guard lock(self->mutex);
            self->host.writeOutBytes(outResult, self->toPublicJsonLocked().dump());
            return 0;
        }
        throw pk::PluginError("未知的本地推理命令：" + cmd);
    } catch (const std::exception& e) {
        self->host.writeOutBytes(outErr, e.what());
        return -1;
    }
}

int32_t MEMESTUDIO_CALL abiInterceptImageApi(
    void* rawInst, const char* op, const char* providerJson, const char* requestJson,
    const MemeStudioBytesV2* uploads, size_t uploadCount, MemeStudioCancelTokenV2* cancel,
    void (MEMESTUDIO_CALL*)(void*, const uint8_t*, size_t, int32_t), void*,
    MemeStudioBytesV2* outResp, MemeStudioBytesV2* outErr) {
    auto* self = static_cast<QwenImagePlugin*>(rawInst);
    if (!self || !op) return -1;
    try {
        Json prov = (providerJson && providerJson[0]) ? Json::parse(providerJson) : Json::object();
        Json req = (requestJson && requestJson[0]) ? Json::parse(requestJson) : Json::object();
        return self->interceptCall(op, prov, req, uploads, uploadCount, cancel, outResp, outErr);
    } catch (const std::exception& e) {
        self->host.writeOutBytes(outErr, e.what());
        return -1;
    }
}

const MemeStudioPluginV2 kDescriptor{
    sizeof(MemeStudioPluginV2),
    MEMESTUDIO_PLUGIN_ABI_V2,
    kPluginId,
    abiCreate,
    abiDestroy,
    abiInvokeCommand,
    nullptr,
    nullptr,
    abiInterceptImageApi
};

} // namespace

extern "C" MEMESTUDIO_EXPORT const MemeStudioPluginV2* MEMESTUDIO_CALL MemeStudioGetPluginV2(void) {
    return &kDescriptor;
}
