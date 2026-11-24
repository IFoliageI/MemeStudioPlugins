#include "memestudio_plugin_kit.hpp"
#include "../vendor/nlohmann/json.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cwctype>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using Json = nlohmann::json;
namespace pk = meme::plugin_kit;

constexpr const char* kPluginId = "memestudio.qwen-image";

struct QwenLocalConfig {
    pk::fs::path sdServerExe;
    pk::fs::path modelDirectory;
    pk::fs::path diffusionModel;
    pk::fs::path vaePath;
    pk::fs::path llmPath;
    pk::fs::path mmprojPath;
    std::string modelAlias = "qwen-image-local";
    uint16_t port = 18189;
    std::string bearerKey;
    bool turboPreset = false;
    int steps = 20;
    double cfgScale = 3.5;
    double vramBudgetGiB = 0.0;
    bool offloadToCpu = false;
    bool retainLoaded = true;
    bool sessionTakeover = false;
};

struct GgufProbeInfo {
    bool valid = false;
    uint32_t version = 0;
    std::string generalArchitecture;
};

// Complete fault-tolerant binary GGUF v2/v3 header reader (properly skips scalar & array KV entries before general.architecture!)
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
            !f.read(reinterpret_cast<char*>(&kvCount), 8) || kvCount > 100000) {
            return info;
        }

        auto readString = [&](std::string& out) -> bool {
            uint64_t len = 0;
            if (!f.read(reinterpret_cast<char*>(&len), 8) || len > 1024 * 1024) return false;
            out.resize(static_cast<size_t>(len));
            return len == 0 || static_cast<bool>(f.read(out.data(), static_cast<std::streamsize>(len)));
        };

        auto skipValue = [&](auto& self, uint32_t vtype, int depth) -> bool {
            if (depth > 4) return false;
            if (vtype == 8 /* STRING */) {
                uint64_t len = 0;
                if (!f.read(reinterpret_cast<char*>(&len), 8) || len > 16 * 1024 * 1024) return false;
                return static_cast<bool>(f.seekg(static_cast<std::streamoff>(len), std::ios::cur));
            }
            if (vtype == 9 /* ARRAY */) {
                uint32_t subType = 0;
                uint64_t arrCount = 0;
                if (!f.read(reinterpret_cast<char*>(&subType), 4) ||
                    !f.read(reinterpret_cast<char*>(&arrCount), 8) ||
                    subType == 9 || arrCount > 1000000) {
                    return false;
                }
                for (uint64_t k = 0; k < arrCount; ++k) {
                    if (!self(self, subType, depth + 1)) return false;
                }
                return true;
            }
            static constexpr int kScalarSizes[] = {1, 1, 2, 2, 4, 4, 4, 1, -1, -1, 8, 8, 8};
            if (vtype >= std::size(kScalarSizes) || kScalarSizes[vtype] < 0) return false;
            return static_cast<bool>(f.seekg(kScalarSizes[vtype], std::ios::cur));
        };

        for (uint64_t i = 0; i < std::min<uint64_t>(kvCount, 128); ++i) {
            std::string key;
            uint32_t valType = 0;
            if (!readString(key) || !f.read(reinterpret_cast<char*>(&valType), 4)) break;
            if (key == "general.architecture" && valType == 8) {
                readString(info.generalArchitecture);
                break;
            }
            if (!skipValue(skipValue, valType, 0)) break;
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
    if (w < 128 || h < 128 || w > 2048 || h > 2048 || (w % 32) != 0 || (h % 32) != 0 ||
        int64_t(w) * h > 4ll * 1024 * 1024) {
        throw pk::PluginError("Qwen-Image 尺寸每边必须在 128..2048 之间且为 32 的整数倍（最大 400 万像素）");
    }
    return {static_cast<uint32_t>(w), static_cast<uint32_t>(h)};
}

std::wstring quoteWinArg(std::wstring_view a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring_view::npos) return std::wstring(a);
    std::wstring q = L"\"";
    for (wchar_t ch : a) {
        if (ch == L'"') q += L"\\\"";
        else q.push_back(ch);
    }
    q.push_back(L'"');
    return q;
}

// Formats Windows / NTSTATUS process exit codes into clear user-actionable Chinese diagnostics!
std::string describeProcessExitCode(DWORD exitCode) {
    char hexBuf[32]{};
    std::snprintf(hexBuf, sizeof(hexBuf), "0x%08lX", static_cast<unsigned long>(exitCode));
    if (exitCode == 0xC0000135u /* STATUS_DLL_NOT_FOUND */) {
        return std::string("缺少运行库 DLL (") + hexBuf + " STATUS_DLL_NOT_FOUND)。\n"
               "原因说明：所选 sd-server.exe 依赖外部 GPU 运行库（例如 ROCm 版的 libhipblaslt.dll / hipblas.dll，或 CUDA 版的 cudart64_*.dll），但同级目录未包含配套 DLL 或系统版本不匹配。\n"
               "解决方法：请将对应版本的配套运行库压缩包解压到 sd-server.exe 同目录，或改选免外部环境依赖的 Vulkan 版 sd-server.exe。";
    }
    if (exitCode == 0xC000007Bu /* STATUS_INVALID_IMAGE_FORMAT */) {
        return std::string("DLL 位数或架构不匹配 (") + hexBuf + " STATUS_INVALID_IMAGE_FORMAT)。请确认运行库均为 64 位版本。";
    }
    if (exitCode == 0xC0000139u /* STATUS_ENTRYPOINT_NOT_FOUND */) {
        return std::string("运行库版本不兼容，找不到导出函数入口 (") + hexBuf + " STATUS_ENTRYPOINT_NOT_FOUND)。请使用与该 sd-server.exe 同一版本发布的配套运行库，或改用 Vulkan 版本。";
    }
    return "进程异常退出（退出码 " + std::to_string(exitCode) + " / " + hexBuf + "）";
}

// Builds an augmented environment block prepending %HIP_PATH%\bin and %CUDA_PATH%\bin to PATH if installed
std::vector<wchar_t> buildGpuAugmentedEnvironment(const pk::fs::path& exeDir) {
    std::wstring extraPaths = exeDir.wstring();
    for (const wchar_t* envName : {L"HIP_PATH", L"CUDA_PATH"}) {
        DWORD len = GetEnvironmentVariableW(envName, nullptr, 0);
        if (len > 1) {
            std::wstring val(len, L'\0');
            DWORD got = GetEnvironmentVariableW(envName, val.data(), len);
            if (got > 0 && got < len) {
                val.resize(got);
                pk::fs::path binDir = pk::fs::path(val) / L"bin";
                if (pk::fs::is_directory(binDir)) {
                    extraPaths += L";" + binDir.wstring();
                }
            }
        }
    }

    LPWCH rawEnv = GetEnvironmentStringsW();
    if (!rawEnv) return {};
    std::vector<std::wstring> entries;
    bool pathPatched = false;
    for (const wchar_t* p = rawEnv; *p; p += std::wcslen(p) + 1) {
        std::wstring line(p);
        if (line.size() >= 5 && _wcsnicmp(line.c_str(), L"PATH=", 5) == 0) {
            entries.push_back(L"PATH=" + extraPaths + L";" + line.substr(5));
            pathPatched = true;
        } else {
            entries.push_back(std::move(line));
        }
    }
    FreeEnvironmentStringsW(rawEnv);
    if (!pathPatched) entries.push_back(L"PATH=" + extraPaths);

    std::vector<wchar_t> block;
    for (const auto& e : entries) {
        block.insert(block.end(), e.begin(), e.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

struct SdServerCapabilities {
    std::wstring visionFlag = L"--llm_vision"; // Default in master-1106; auto-switched if --llm-vision detected
};

// Runs `sd-server.exe --help` (<200ms) BEFORE starting daemon to catch 0xC0000135 immediately & detect exact CLI flag names!
SdServerCapabilities probeSdServerBinary(const pk::fs::path& exePath, const pk::fs::path& tempHelpLog) {
    if (exePath.empty() || !pk::fs::is_regular_file(exePath)) {
        throw pk::PluginError("请先选择有效的 sd-server.exe 可执行文件");
    }
    if (!tempHelpLog.parent_path().empty()) {
        pk::fs::create_directories(tempHelpLog.parent_path());
    }

    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE nulIn = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    HANDLE outLog = CreateFileW(
        tempHelpLog.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (outLog == INVALID_HANDLE_VALUE) {
        if (nulIn != INVALID_HANDLE_VALUE) CloseHandle(nulIn);
        throw pk::PluginError("无法创建本地引擎预检日志文件");
    }

    std::wstring cmdLine = quoteWinArg(exePath.wstring()) + L" --help";
    pk::fs::path workDir = exePath.parent_path();
    auto envBlock = buildGpuAugmentedEnvironment(workDir);

    STARTUPINFOW si{};
    si.cb = sizeof(STARTUPINFOW);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = nulIn;
    si.hStdOutput = outLog;
    si.hStdError = outLog;

    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(
        exePath.c_str(), cmdLine.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
        envBlock.empty() ? nullptr : envBlock.data(),
        workDir.empty() ? nullptr : workDir.c_str(),
        &si, &pi);

    if (nulIn != INVALID_HANDLE_VALUE) CloseHandle(nulIn);
    CloseHandle(outLog);

    if (!ok) {
        throw pk::PluginError("无法启动 sd-server.exe（Windows 错误码 " + std::to_string(GetLastError()) + "）");
    }

    DWORD waitRc = WaitForSingleObject(pi.hProcess, 15000);
    DWORD exitCode = 0;
    if (waitRc == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        throw pk::PluginError("执行 sd-server.exe --help 预检超时，请检查所选程序是否为命令行版 sd-server.exe");
    }
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    std::string helpText = pk::readFileTailLossyUtf8(tempHelpLog, 64 * 1024);
    std::error_code rmEc;
    pk::fs::remove(tempHelpLog, rmEc);

    if (exitCode != 0) {
        std::string msg = "sd-server.exe 预检未通过：" + describeProcessExitCode(exitCode);
        std::string trimmedHelp = pk::trimAscii(helpText);
        if (!trimmedHelp.empty()) msg += "\n控制台输出：" + trimmedHelp.substr(0, 400);
        throw pk::PluginError(msg);
    }

    if (helpText.find("--listen-port") == std::string::npos) {
        throw pk::PluginError("所选程序不支持 --listen-port 参数，请确认选择的是 sd-server.exe（而非 sd-cli.exe）");
    }

    SdServerCapabilities caps;
    if (helpText.find("--llm-vision") != std::string::npos && helpText.find("--llm_vision") == std::string::npos) {
        caps.visionFlag = L"--llm-vision";
    } else {
        caps.visionFlag = L"--llm_vision";
    }
    return caps;
}

class ManagedDaemonProcess {
    HANDLE job_ = nullptr;
    HANDLE process_ = nullptr;
    DWORD lastExitCode_ = 0;
public:
    ManagedDaemonProcess() = default;
    ~ManagedDaemonProcess() { stop(); }
    ManagedDaemonProcess(const ManagedDaemonProcess&) = delete;
    ManagedDaemonProcess& operator=(const ManagedDaemonProcess&) = delete;

    bool isRunning() noexcept {
        if (!process_) return false;
        if (WaitForSingleObject(process_, 0) == WAIT_TIMEOUT) {
            return true;
        }
        GetExitCodeProcess(process_, &lastExitCode_);
        return false;
    }

    DWORD lastExitCode() noexcept {
        if (process_) GetExitCodeProcess(process_, &lastExitCode_);
        return lastExitCode_;
    }

    uint32_t pid() noexcept {
        return isRunning() ? GetProcessId(process_) : 0;
    }

    void start(const pk::fs::path& exe, const std::vector<std::wstring>& args, const pk::fs::path& logPath) {
        stop();
        lastExitCode_ = 0;
        if (!logPath.parent_path().empty()) {
            pk::fs::create_directories(logPath.parent_path());
        }

        job_ = CreateJobObjectW(nullptr, nullptr);
        if (!job_) throw pk::PluginError("创建推理引擎 Job Object 失败");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits));

        SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE nulIn = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
        HANDLE logFile = CreateFileW(
            logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
            &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (logFile == INVALID_HANDLE_VALUE) {
            if (nulIn != INVALID_HANDLE_VALUE) CloseHandle(nulIn);
            CloseHandle(job_); job_ = nullptr;
            throw pk::PluginError("无法打开位于 AppData 的推理日志文件");
        }

        std::wstring cmdLine = quoteWinArg(exe.wstring());
        for (const auto& a : args) {
            cmdLine.push_back(L' ');
            cmdLine += quoteWinArg(a);
        }

        SIZE_T attrSize = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
        std::vector<uint8_t> attrBuf(attrSize);
        auto* attrList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
        InitializeProcThreadAttributeList(attrList, 1, 0, &attrSize);
        HANDLE whitelist[2] = {logFile, nulIn != INVALID_HANDLE_VALUE ? nulIn : logFile};
        DWORD whitelistCount = (nulIn != INVALID_HANDLE_VALUE) ? 2 : 1;
        UpdateProcThreadAttribute(attrList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                  whitelist, whitelistCount * sizeof(HANDLE), nullptr, nullptr);

        STARTUPINFOEXW si{};
        si.StartupInfo.cb = sizeof(STARTUPINFOEXW);
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.StartupInfo.wShowWindow = SW_HIDE;
        si.StartupInfo.hStdInput = nulIn;
        si.StartupInfo.hStdOutput = logFile;
        si.StartupInfo.hStdError = logFile;
        si.lpAttributeList = attrList;

        PROCESS_INFORMATION pi{};
        pk::fs::path workDir = exe.parent_path();
        auto envBlock = buildGpuAugmentedEnvironment(workDir);

        BOOL ok = CreateProcessW(
            exe.c_str(), cmdLine.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
            envBlock.empty() ? nullptr : envBlock.data(),
            workDir.empty() ? nullptr : workDir.c_str(),
            &si.StartupInfo, &pi);

        DeleteProcThreadAttributeList(attrList);
        if (nulIn != INVALID_HANDLE_VALUE) CloseHandle(nulIn);
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

        // Catch immediate CLI / initialization crashes within first 350ms so we never fake "running"!
        if (WaitForSingleObject(process_, 350) == WAIT_OBJECT_0) {
            GetExitCodeProcess(process_, &lastExitCode_);
            DWORD code = lastExitCode_;
            stop();
            std::string logTail = pk::trimAscii(pk::readFileTailLossyUtf8(logPath, 64 * 1024));
            std::string reason = "sd-server 启动后立即退出：" + describeProcessExitCode(code);
            if (!logTail.empty()) reason += "\n日志输出：\n" + logTail.substr(0, 800);
            throw pk::PluginError(reason);
        }
    }

    void stop() noexcept {
        if (job_) {
            TerminateJobObject(job_, 0);
        }
        if (process_) {
            WaitForSingleObject(process_, 2000);
            GetExitCodeProcess(process_, &lastExitCode_);
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
        config.bearerKey = "qwen-" + pk::randomHex(12);
        loadSettingsFromAppData();
    }

    ~QwenImagePlugin() {
        clearEndpointOverride();
        daemon.stop();
    }

    pk::fs::path settingsFilePath() const { return host.dataDir() / L"qwen-config.json"; }
    pk::fs::path logFilePath() const { return host.dataDir() / L"sd-server.log"; }
    pk::fs::path runtimeRootDir() const { return host.dataDir() / L"runtime"; }

    void clearEndpointOverride() noexcept {
        if (host.raw() && host.raw()->set_session_endpoint_override) {
            host.raw()->set_session_endpoint_override(host.raw()->host_ctx, kPluginId, nullptr);
        }
    }

    void syncEndpointOverrideLocked() {
        if (!host.raw() || !host.raw()->set_session_endpoint_override) return;
        if (config.sessionTakeover) {
            Json ep{
                {"base_url", "http://127.0.0.1:" + std::to_string(config.port) + "/v1"},
                {"api_key", config.bearerKey},
                {"model", config.modelAlias}
            };
            std::string s = ep.dump();
            host.raw()->set_session_endpoint_override(host.raw()->host_ctx, kPluginId, s.c_str());
        } else {
            host.raw()->set_session_endpoint_override(host.raw()->host_ctx, kPluginId, nullptr);
        }
    }

    // Auto-heals zombie sessionTakeover if sd-server exited unexpectedly in the background!
    void reconcileDaemonHealthLocked() {
        if (!config.sdServerExe.empty() && config.sessionTakeover && !daemon.isRunning()) {
            config.sessionTakeover = false;
            syncEndpointOverrideLocked();
        }
    }

    Json inspectModels(const QwenLocalConfig& cfg) const {
        Json report{
            {"diffusion_candidates", Json::array()},
            {"vae_candidates", Json::array()},
            {"llm_candidates", Json::array()},
            {"mmproj_candidates", Json::array()},
            {"skipped_corrupt_files", Json::array()},
            {"recommend_turbo", false}
        };

        auto checkExplicit = [&](const pk::fs::path& p, const char* key) {
            if (p.empty()) return;
            if (!pk::fs::is_regular_file(p)) {
                throw pk::PluginError(std::string("指定的模型文件不存在：") + pk::wideToUtf8(p.wstring()));
            }
            if (p.extension() == L".gguf" || p.extension() == L".GGUF") {
                auto info = inspectGgufHeaderSafe(p);
                if (!info.valid) {
                    throw pk::PluginError(std::string("指定的 GGUF 文件头已损坏：") + pk::wideToUtf8(p.filename().wstring()));
                }
            }
            report[key].push_back(pk::wideToUtf8(p.wstring()));
        };

        checkExplicit(cfg.diffusionModel, "diffusion_candidates");
        checkExplicit(cfg.vaePath, "vae_candidates");
        checkExplicit(cfg.llmPath, "llm_candidates");
        checkExplicit(cfg.mmprojPath, "mmproj_candidates");

        if (cfg.modelDirectory.empty()) return report;
        if (!pk::fs::is_directory(cfg.modelDirectory)) {
            throw pk::PluginError("模型目录不存在：" + pk::wideToUtf8(cfg.modelDirectory.wstring()));
        }

        std::error_code ec;
        for (const auto& entry : pk::fs::directory_iterator(cfg.modelDirectory, ec)) {
            if (!entry.is_regular_file(ec)) continue;
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
                } else if (info.generalArchitecture.starts_with("qwen2vl") ||
                           info.generalArchitecture.starts_with("qwen25vl") ||
                           info.generalArchitecture.starts_with("qwen3vl") ||
                           info.generalArchitecture.starts_with("qwen2") ||
                           info.generalArchitecture.starts_with("qwen3") ||
                           info.generalArchitecture == "llama" ||
                           filenameLower.find("vl") != std::string::npos) {
                    report["llm_candidates"].push_back(fullUtf8);
                } else {
                    if (filenameLower.find("turbo") != std::string::npos) {
                        report["recommend_turbo"] = true;
                    }
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

    Json toPublicJsonLocked() {
        reconcileDaemonHealthLocked();
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
            {"offload_to_cpu", config.offloadToCpu},
            {"retain_loaded", config.retainLoaded},
            {"session_takeover", config.sessionTakeover},
            {"running", daemon.isRunning()},
            {"pid", daemon.pid()},
            {"data_directory", pk::wideToUtf8(host.dataDir().wstring())}
        };
    }

    void updateConfig(const Json& j, bool persist) {
        std::lock_guard lock(mutex);
        auto optPath = [&](const char* k, pk::fs::path& target) {
            if (j.contains(k) && j[k].is_string()) {
                target = pk::utf8ToWideStrict(pk::trimAscii(j[k].get<std::string>()));
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
        if (j.contains("vram_budget_gib")) config.vramBudgetGiB = std::clamp(j["vram_budget_gib"].get<double>(), 0.0, 256.0);
        if (j.contains("offload_to_cpu") && j["offload_to_cpu"].is_boolean()) config.offloadToCpu = j["offload_to_cpu"].get<bool>();
        if (j.contains("retain_loaded") && j["retain_loaded"].is_boolean()) config.retainLoaded = j["retain_loaded"].get<bool>();
        if (j.contains("session_takeover") && j["session_takeover"].is_boolean()) config.sessionTakeover = j["session_takeover"].get<bool>();

        if (persist) {
            Json disk = toPublicJsonLocked();
            disk.erase("running");
            disk.erase("pid");
            disk.erase("data_directory");
            disk["session_takeover"] = false; // Never persist transient session takeover onto disk across restarts!
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
            Json parsed = Json::parse(raw.begin(), raw.end());
            parsed["session_takeover"] = false; // Always start with clean non-hijacked state until user starts server!
            updateConfig(parsed, false);
        } catch (...) {}
    }

    void startLocalServer(
        MemeStudioCancelTokenV2* hostCancel = nullptr,
        const std::atomic<bool>* uiCancel = nullptr) {
        QwenLocalConfig snap;
        {
            std::lock_guard lock(mutex);
            if (daemon.isRunning()) {
                config.sessionTakeover = true;
                syncEndpointOverrideLocked();
                return;
            }
            snap = config;
        }

        if (snap.diffusionModel.empty() || !pk::fs::is_regular_file(snap.diffusionModel)) {
            throw pk::PluginError("启动前请先选择有效的扩散模型权重文件（如 Qwen-Image-2.1-*.gguf）");
        }
        if (snap.vaePath.empty() || !pk::fs::is_regular_file(snap.vaePath)) {
            throw pk::PluginError("启动 Qwen-Image 需要同时指定配套的专用 VAE 权重文件（qwen_image_*_vae*.safetensors）");
        }
        if (snap.llmPath.empty() || !pk::fs::is_regular_file(snap.llmPath)) {
            throw pk::PluginError("启动 Qwen-Image 需要同时指定配套的 GGUF 文本编码器（LLM）文件");
        }

        // 1. Pre-flight check: catches 0xC0000135 missing ROCm/CUDA DLLs & detects --llm_vision vs --llm-vision!
        SdServerCapabilities caps = probeSdServerBinary(snap.sdServerExe, host.dataDir() / L"probe-help.log");

        std::vector<std::wstring> args{
            L"--listen-ip", L"127.0.0.1",
            L"--listen-port", std::to_wstring(snap.port),
            L"--diffusion-model", snap.diffusionModel.wstring(),
            L"--vae", snap.vaePath.wstring(),
            L"--llm", snap.llmPath.wstring()
        };
        if (!snap.mmprojPath.empty()) {
            if (!pk::fs::is_regular_file(snap.mmprojPath)) {
                throw pk::PluginError("指定的视觉投影 mmproj 文件不存在");
            }
            args.push_back(caps.visionFlag);
            args.push_back(snap.mmprojPath.wstring());
        }
        if (snap.offloadToCpu) {
            args.push_back(L"--offload-to-cpu");
        }
        if (snap.vramBudgetGiB > 0.0) {
            args.push_back(L"--max-vram");
            args.push_back(std::to_wstring(snap.vramBudgetGiB));
        }

        {
            std::lock_guard lock(mutex);
            daemon.start(snap.sdServerExe, args, logFilePath());
        }

        // 2. Poll until HTTP port accepts connections & model finishes loading (up to 15 min for 20GB GGUF!)
        try {
            waitUntilServerReady(snap.port, hostCancel, uiCancel);
        } catch (...) {
            std::lock_guard lock(mutex);
            daemon.stop();
            config.sessionTakeover = false;
            syncEndpointOverrideLocked();
            throw;
        }

        std::lock_guard lock(mutex);
        config.sessionTakeover = true;
        syncEndpointOverrideLocked();
    }

    void waitUntilServerReady(
        uint16_t port,
        MemeStudioCancelTokenV2* hostCancel,
        const std::atomic<bool>* uiCancel) {
        std::string statusUrl = "http://127.0.0.1:" + std::to_string(port) + "/sdcpp/v1/models/status";
        std::string openAiModelsUrl = "http://127.0.0.1:" + std::to_string(port) + "/v1/models";

        auto startedAt = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - startedAt < std::chrono::minutes(15)) {
            host.verifyNotCancelled(hostCancel);
            if (uiCancel && uiCancel->load(std::memory_order_acquire)) {
                throw pk::PluginCancelled();
            }
            {
                std::lock_guard lock(mutex);
                if (!daemon.isRunning()) {
                    DWORD code = daemon.lastExitCode();
                    std::string tail = pk::trimAscii(pk::readFileTailLossyUtf8(logFilePath(), 64 * 1024));
                    std::string msg = "本地推理进程在加载模型期间退出：" + describeProcessExitCode(code);
                    if (!tail.empty()) msg += "\n尾部日志：\n" + tail.substr(0, 800);
                    throw pk::PluginError(msg);
                }
            }

            // Probe native /sdcpp/v1/models/status first (always pass genuine hostCancel or nullptr, NEVER a forged pointer!)
            Json spec{{"url", statusUrl}, {"method", "GET"}, {"timeout_seconds", 3}, {"allow_loopback_http", true}};
            std::string specStr = spec.dump();
            uint32_t httpCode = 0;
            int32_t streamed = 0;
            MemeStudioBytesV2 resp{}, err{};
            int32_t hrc = host.raw()->http_send(
                host.raw()->host_ctx, specStr.c_str(), nullptr, 0, hostCancel, nullptr, nullptr, &httpCode, &streamed, &resp, &err);
            host.consumeBytesToString(err);
            std::string body = host.consumeBytesToString(resp);

            if (hrc == 0) {
                if (httpCode == 200) {
                    auto j = Json::parse(body, nullptr, false);
                    std::string state = j.is_object() ? j.value("state", "") : "";
                    if (state == "ready") return;
                    if (state == "error" || state == "empty") {
                        throw pk::PluginError("本地 GGUF 模型加载失败：" + j.value("error", body));
                    }
                } else if (httpCode == 404) {
                    Json mspec{{"url", openAiModelsUrl}, {"method", "GET"}, {"timeout_seconds", 3}, {"allow_loopback_http", true}};
                    std::string mspecStr = mspec.dump();
                    MemeStudioBytesV2 mresp{}, merr{};
                    uint32_t mcode = 0;
                    if (host.raw()->http_send(host.raw()->host_ctx, mspecStr.c_str(), nullptr, 0, hostCancel, nullptr, nullptr, &mcode, &streamed, &mresp, &merr) == 0 && mcode == 200) {
                        host.consumeBytesToString(mresp);
                        host.consumeBytesToString(merr);
                        return;
                    }
                    host.consumeBytesToString(mresp);
                    host.consumeBytesToString(merr);
                }
            }
            Sleep(350);
        }
        throw pk::PluginError("加载本地 GGUF 模型超过 15 分钟仍未就绪");
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
        void (MEMESTUDIO_CALL* onPreview)(void*, const uint8_t*, size_t, int32_t),
        void* previewUser,
        MemeStudioBytesV2* outResp,
        MemeStudioBytesV2* outErr) {
        QwenLocalConfig snap;
        {
            std::lock_guard lock(mutex);
            reconcileDaemonHealthLocked();
            if (!config.sessionTakeover) return 1; // Pass-through to commercial API when takeover is off!
            snap = config;
        }

        if (op == "models") {
            host.writeOutBytes(outResp, Json::array({snap.modelAlias}).dump());
            return 0;
        }

        if (uploadCount > 0 && !snap.sdServerExe.empty() && snap.mmprojPath.empty()) {
            host.writeOutBytes(outErr, "执行角色替换或带图修图必须在本地模型设置中配置配套的视觉投影文件 (mmproj)");
            return -1;
        }

        auto [width, height] = parseAndValidateQwenSize(providerJson.value("size", std::string("auto")));
        int steps = snap.turboPreset ? 8 : snap.steps;
        double cfg = snap.turboPreset ? 1.0 : snap.cfgScale;
        bool wantStream = providerJson.value("stream", false);

        Json refImages = Json::array();
        if (uploadCount > 0 && uploads) {
            for (size_t i = 0; i < uploadCount; ++i) {
                refImages.push_back(pk::base64Encode(std::span<const uint8_t>(uploads[i].data, uploads[i].size)));
            }
        }

        // 1. Try native stable-diffusion.cpp `/sdcpp/v1/img_gen` + `/sdcpp/v1/jobs/<id>` async multi-reference job API!
        Json sampleParams{
            {"sample_method", "euler"},
            {"sample_steps", steps},
            {"guidance", {{"txt_cfg", cfg}}}
        };
        if (snap.turboPreset) {
            sampleParams["custom_sigmas"] = {1.0, 0.978453, 0.95418, 0.926626, 0.89508, 0.845148, 0.704534, 0.414568, 0.0};
        }

        Json jobReq{
            {"prompt", reqJson.value("prompt", std::string())},
            {"negative_prompt", ""},
            {"width", width},
            {"height", height},
            {"batch_count", 1},
            {"seed", -1},
            {"ref_images", refImages},
            {"sample_params", sampleParams},
            {"output_format", "png"},
            {"preview", wantStream ? "proj" : "none"},
            {"preview_interval", 1}
        };
        std::string jobReqStr = jobReq.dump();
        std::string imgGenUrl = "http://127.0.0.1:" + std::to_string(snap.port) + "/sdcpp/v1/img_gen";
        Json submitSpec{
            {"url", imgGenUrl},
            {"method", "POST"},
            {"timeout_seconds", 60},
            {"allow_loopback_http", true},
            {"headers", {{"Content-Type", "application/json"}, {"Authorization", "Bearer " + snap.bearerKey}}}
        };
        std::string submitSpecStr = submitSpec.dump();

        uint32_t status = 0;
        int32_t streamed = 0;
        MemeStudioBytesV2 respBytes{}, errBytes{};
        int32_t hrc = host.raw()->http_send(
            host.raw()->host_ctx, submitSpecStr.c_str(),
            reinterpret_cast<const uint8_t*>(jobReqStr.data()), jobReqStr.size(),
            cancel, nullptr, nullptr, &status, &streamed, &respBytes, &errBytes);
        std::string hErr = host.consumeBytesToString(errBytes);
        std::string submitRespStr = host.consumeBytesToString(respBytes);

        if (hrc == 0 && status >= 200 && status < 300) {
            auto submitJson = Json::parse(submitRespStr);
            std::string jobId = submitJson.value("id", "");
            if (jobId.empty()) {
                host.writeOutBytes(outErr, "本地 sd-server 未返回有效任务 ID");
                return -1;
            }
            std::string pollUrl = "http://127.0.0.1:" + std::to_string(snap.port) + "/sdcpp/v1/jobs/" + jobId;
            Json pollSpec{{"url", pollUrl}, {"method", "GET"}, {"timeout_seconds", 30}, {"allow_loopback_http", true}};
            std::string pollSpecStr = pollSpec.dump();
            int32_t previewCounter = 0;
            std::string lastPreviewB64;

            for (;;) {
                host.verifyNotCancelled(cancel);
                MemeStudioBytesV2 pResp{}, pErr{};
                uint32_t pStatus = 0;
                int32_t prc = host.raw()->http_send(
                    host.raw()->host_ctx, pollSpecStr.c_str(), nullptr, 0,
                    cancel, nullptr, nullptr, &pStatus, &streamed, &pResp, &pErr);
                std::string pe = host.consumeBytesToString(pErr);
                std::string pb = host.consumeBytesToString(pResp);
                if (prc != 0 || pStatus < 200 || pStatus >= 300) {
                    host.writeOutBytes(outErr, "轮询本地推理任务状态失败：" + (pe.empty() ? ("HTTP " + std::to_string(pStatus)) : pe));
                    return -1;
                }

                auto pj = Json::parse(pb);
                if (onPreview && pj.contains("preview") && pj["preview"].is_object()) {
                    std::string b64 = pj["preview"].value("b64_json", "");
                    if (!b64.empty() && b64 != lastPreviewB64) {
                        lastPreviewB64 = b64;
                        try {
                            auto pBytes = pk::base64Decode(b64);
                            onPreview(previewUser, pBytes.data(), pBytes.size(), previewCounter++);
                        } catch (...) {}
                    }
                }

                std::string jobSt = pj.value("status", "");
                if (jobSt == "completed") {
                    std::string finalB64 = pj.at("result").at("images").at(0).at("b64_json").get<std::string>();
                    pk::Bytes pngBytes = pk::base64Decode(finalB64);
                    host.writeOutRawBytes(outResp, pngBytes);
                    return 0;
                }
                if (jobSt == "failed" || jobSt == "cancelled") {
                    host.writeOutBytes(outErr, "本地 GGUF 推理执行失败：" + pj.value("error", jobSt));
                    return -1;
                }
                Sleep(350);
            }
        }

        // 2. Fallback to OpenAI-compatible `/v1/images/...` if server returned 404 on `/sdcpp/v1/img_gen`
        if (hrc == 0 && status == 404) {
            Json oaiBody{
                {"model", snap.modelAlias},
                {"prompt", reqJson.value("prompt", std::string())},
                {"width", width},
                {"height", height},
                {"steps", steps},
                {"cfg_scale", cfg}
            };
            if (!refImages.empty()) oaiBody["reference_images"] = refImages;
            std::string oaiBodyStr = oaiBody.dump();
            std::string oaiUrl = "http://127.0.0.1:" + std::to_string(snap.port) +
                                 (op == "generate" ? "/v1/images/generations" : "/v1/images/edits");
            Json oaiSpec{
                {"url", oaiUrl},
                {"method", "POST"},
                {"timeout_seconds", std::clamp(providerJson.value("timeout_seconds", 900), 5, 3600)},
                {"allow_loopback_http", true},
                {"headers", {{"Authorization", "Bearer " + snap.bearerKey}, {"Content-Type", "application/json"}}}
            };
            std::string oaiSpecStr = oaiSpec.dump();
            MemeStudioBytesV2 oResp{}, oErr{};
            uint32_t oStatus = 0;
            int32_t orc = host.raw()->http_send(
                host.raw()->host_ctx, oaiSpecStr.c_str(),
                reinterpret_cast<const uint8_t*>(oaiBodyStr.data()), oaiBodyStr.size(),
                cancel, nullptr, nullptr, &oStatus, &streamed, &oResp, &oErr);
            std::string oe = host.consumeBytesToString(oErr);
            pk::Bytes oVec = host.consumeBytesToVec(oResp);
            if (orc != 0 || oStatus < 200 || oStatus >= 300) {
                host.writeOutBytes(outErr, "本地推理接口返回错误：" + (oe.empty() ? ("HTTP " + std::to_string(oStatus)) : oe));
                return -1;
            }
            auto parsed = Json::parse(oVec.begin(), oVec.end());
            pk::Bytes pngBytes = pk::base64Decode(parsed.at("data").at(0).at("b64_json").get<std::string>());
            host.writeOutRawBytes(outResp, pngBytes);
            return 0;
        }

        if (hrc != 0) {
            std::lock_guard lock(mutex);
            reconcileDaemonHealthLocked();
            host.writeOutBytes(outErr, "无法连接到本地 sd-server 推理引擎（请先在「本地模型…」面板启动引擎并等待加载就绪）：" + hErr);
            return -1;
        }
        host.writeOutBytes(outErr, "本地 sd-server 返回 HTTP " + std::to_string(status) + "：" + submitRespStr.substr(0, 300));
        return -1;
    }
};

constexpr UINT WM_LOCAL_SD_ASYNC_DONE = WM_APP + 52;
constexpr UINT_PTR kLogRefreshTimerId = 7;

template <typename PluginT>
struct LocalSdEnginePanel {
    PluginT* plugin = nullptr;
    HWND hwnd = nullptr;
    HFONT font = nullptr;

    HWND sdExe = nullptr;
    HWND modelDir = nullptr, diffEdit = nullptr, vaeEdit = nullptr, llmEdit = nullptr, mmprojEdit = nullptr;
    HWND portEdit = nullptr, aliasEdit = nullptr, stepsEdit = nullptr, cfgEdit = nullptr;
    HWND turboCheck = nullptr, offloadCheck = nullptr, takeoverCheck = nullptr;
    HWND logBox = nullptr, statusLabel = nullptr, startBtn = nullptr, stopBtn = nullptr, saveBtn = nullptr;

    std::thread bgWorker;
    std::atomic<bool> busy{false};
    std::atomic<bool> cancelRequested{false};
    std::mutex resMutex;
    std::string asyncError;

    ~LocalSdEnginePanel() {
        cancelRequested.store(true, std::memory_order_release);
        if (bgWorker.joinable()) bgWorker.join();
        if (font) DeleteObject(font);
    }

    enum ControlIds : int {
        IdSdExeEdit      = 3701,
        IdPickSdExeBtn   = 3702,
        IdModelDirEdit   = 3704,
        IdPickModelDir   = 3705,
        IdScanDirBtn     = 3706,
        IdDiffEdit       = 3707,
        IdPickDiffBtn    = 3708,
        IdVaeEdit        = 3709,
        IdPickVaeBtn     = 3710,
        IdLlmEdit        = 3711,
        IdPickLlmBtn     = 3712,
        IdMmprojEdit     = 3713,
        IdPickMmprojBtn  = 3714,
        IdPortEdit       = 3715,
        IdAliasEdit      = 3716,
        IdStepsEdit      = 3717,
        IdCfgEdit        = 3718,
        IdTurboCheck     = 3719,
        IdOffloadCheck   = 3720,
        IdTakeoverCheck  = 3721,
        IdLogViewer      = 3722,
        IdRefreshLogBtn  = 3723,
        IdStartSrvBtn    = 3724,
        IdStopSrvBtn     = 3725,
        IdSaveCfgBtn     = 3726,
        IdClosePanelBtn  = 3727
    };

    Json gatherFormJson(bool forceTakeover = false) const {
        int port = pk::parseIntField(portEdit, "监听端口", 1024, 65535);
        int steps = pk::parseIntField(stepsEdit, "推理步数", 1, 100);
        double cfg = pk::parseDoubleField(cfgEdit, "CFG 系数", 1.0, 30.0);
        std::string alias = pk::trimAscii(pk::getWindowTextUtf8(aliasEdit));
        if (alias.empty()) alias = "qwen-image-local";

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
            {"offload_to_cpu", pk::isButtonChecked(offloadCheck)},
            {"session_takeover", forceTakeover ? true : pk::isButtonChecked(takeoverCheck)}
        };
    }

    void setBusyUi(bool b, std::string_view busyHint = "") {
        busy.store(b, std::memory_order_release);
        EnableWindow(startBtn, b ? FALSE : TRUE);
        EnableWindow(saveBtn, b ? FALSE : TRUE);
        if (b) {
            SetTimer(hwnd, kLogRefreshTimerId, 500, nullptr);
            if (!busyHint.empty()) pk::setWindowTextUtf8(statusLabel, busyHint);
        } else {
            KillTimer(hwnd, kLogRefreshTimerId);
        }
    }

    void refreshStatusAndLog(bool updateStatusText = true) {
        bool running = false;
        uint32_t pid = 0;
        bool takeover = false;
        {
            std::lock_guard lk(plugin->mutex);
            plugin->reconcileDaemonHealthLocked();
            running = plugin->daemon.isRunning();
            pid = plugin->daemon.pid();
            takeover = plugin->config.sessionTakeover;
        }
        pk::setButtonChecked(takeoverCheck, takeover);
        if (updateStatusText && !busy.load(std::memory_order_acquire)) {
            std::string stText = running
                ? ("引擎状态：已就绪并运行中 (PID " + std::to_string(pid) + ") · 关闭本窗口不会中断后台驻留")
                : "引擎状态：未启动 · 配置与运行日志均隔离保存在 %LOCALAPPDATA%";
            pk::setWindowTextUtf8(statusLabel, stText);
        }

        std::string tail = pk::readFileTailLossyUtf8(plugin->logFilePath(), 64 * 1024);
        pk::setWindowTextUtf8(logBox, tail.empty() ? "（暂无 sd-server 运行日志）" : tail);
        SendMessageW(logBox, EM_SETSEL, 0xFFFF, 0xFFFF);
        SendMessageW(logBox, EM_SCROLLCARET, 0, 0);
    }

    void autoCheckTurboIfModelNameMatches(std::string_view diffPathUtf8) {
        std::string lower(diffPathUtf8);
        for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lower.find("turbo") != std::string::npos) {
            pk::setButtonChecked(turboCheck, true);
        }
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
                st->plugin->reconcileDaemonHealthLocked();
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

            ui.label(L"专用 VAE 权重：", 16, 112, 145, 20);
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

            st->turboCheck    = ui.checkbox(L"Turbo 8步极速采样 (Turbo模型必选)", IdTurboCheck, 16, 246, 250, 24, cur.turboPreset);
            st->offloadCheck  = ui.checkbox(L"低显存 CPU 卸载 (--offload-to-cpu)", IdOffloadCheck, 274, 246, 240, 24, cur.offloadToCpu);
            st->takeoverCheck = ui.checkbox(L"接管当前会话连接", IdTakeoverCheck, 524, 246, 200, 24, cur.sessionTakeover);

            ui.label(L"sd-server 运行日志：", 16, 280, 200, 20);
            ui.button(L"刷新日志", IdRefreshLogBtn, 686, 276, 78, 24);
            st->logBox = ui.multilineEdit(IdLogViewer, 16, 304, 748, 176, "", true);

            st->statusLabel = ui.label(L"", 16, 492, 748, 22);
            st->startBtn = ui.button(L"启动引擎并接管", IdStartSrvBtn, 290, 522, 136, 34, true);
            st->stopBtn  = ui.button(L"停止并释放显存", IdStopSrvBtn, 434, 522, 132, 34);
            st->saveBtn  = ui.button(L"仅保存设置", IdSaveCfgBtn, 574, 522, 102, 34);
            ui.button(L"关闭", IdClosePanelBtn, 684, 522, 80, 34);

            ui.applyFont(st->font);
            st->autoCheckTurboIfModelNameMatches(pk::wideToUtf8(cur.diffusionModel.wstring()));
            st->refreshStatusAndLog();
            return 0;
        }
        if (msg == WM_TIMER && wp == kLogRefreshTimerId) {
            st->refreshStatusAndLog(false);
            return 0;
        }
        if (msg == WM_COMMAND) {
            int id = LOWORD(wp);
            if (id == IdStopSrvBtn) {
                st->cancelRequested.store(true, std::memory_order_release);
                st->plugin->stopLocalServer();
                if (!st->busy.load(std::memory_order_acquire)) {
                    st->refreshStatusAndLog();
                }
                return 0;
            }
            if (st->busy.load(std::memory_order_acquire)) {
                return 0;
            }

            auto pickToEdit = [&](HWND targetEdit, bool folder, const wchar_t* promptTitle) {
                if (auto p = pk::pickSinglePath(w, false, folder, promptTitle)) {
                    std::string u8 = pk::wideToUtf8(p->wstring());
                    pk::setWindowTextUtf8(targetEdit, u8);
                    if (targetEdit == st->diffEdit) st->autoCheckTurboIfModelNameMatches(u8);
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
                    QwenLocalConfig scanCfg;
                    scanCfg.modelDirectory = pk::utf8ToWideStrict(pk::trimAscii(pk::getWindowTextUtf8(st->modelDir)));
                    Json rep = st->plugin->inspectModels(scanCfg);
                    if (!rep["diffusion_candidates"].empty()) {
                        std::string d0 = rep["diffusion_candidates"][0].get<std::string>();
                        pk::setWindowTextUtf8(st->diffEdit, d0);
                        st->autoCheckTurboIfModelNameMatches(d0);
                    }
                    if (!rep["vae_candidates"].empty()) pk::setWindowTextUtf8(st->vaeEdit, rep["vae_candidates"][0].get<std::string>());
                    if (!rep["llm_candidates"].empty()) pk::setWindowTextUtf8(st->llmEdit, rep["llm_candidates"][0].get<std::string>());
                    if (!rep["mmproj_candidates"].empty()) pk::setWindowTextUtf8(st->mmprojEdit, rep["mmproj_candidates"][0].get<std::string>());
                    if (rep.value("recommend_turbo", false)) pk::setButtonChecked(st->turboCheck, true);
                    pk::showPanelInfo(w, "目录扫描完成，已自动填入该目录下检测到的组件路径（若扩散模型或 VAE 在其他盘符，请单独点击对应行右侧「选择…」指定）。", L"扫描完成");
                } catch (const std::exception& e) {
                    pk::showPanelError(w, e.what(), L"模型目录扫描");
                }
                return 0;
            }

            if (id == IdRefreshLogBtn) { st->refreshStatusAndLog(); return 0; }

            if (id == IdStartSrvBtn) {
                try {
                    Json cfgPayload = st->gatherFormJson(false);
                    cfgPayload["session_takeover"] = false; // Only activate takeover once server is verified ready!
                    st->plugin->updateConfig(cfgPayload, true);

                    if (st->bgWorker.joinable()) st->bgWorker.join();
                    st->cancelRequested.store(false, std::memory_order_release);
                    st->setBusyUi(true, "正在预检运行库并加载本地 GGUF 模型权重至显存（首次加载约需 5~30 秒）…");

                    st->bgWorker = std::thread([st]() {
                        std::string err;
                        try {
                            st->plugin->startLocalServer(nullptr, &st->cancelRequested);
                        } catch (const std::exception& e) {
                            err = e.what();
                        }
                        {
                            std::lock_guard lk(st->resMutex);
                            st->asyncError = std::move(err);
                        }
                        if (st->hwnd && IsWindow(st->hwnd)) PostMessageW(st->hwnd, WM_LOCAL_SD_ASYNC_DONE, 0, 0);
                    });
                } catch (const std::exception& e) {
                    pk::showPanelError(w, e.what(), L"启动本地推理服务");
                }
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
        if (msg == WM_LOCAL_SD_ASYNC_DONE) {
            if (st->bgWorker.joinable()) st->bgWorker.join();
            st->setBusyUi(false);

            std::string err;
            {
                std::lock_guard lk(st->resMutex);
                err = st->asyncError;
            }
            st->refreshStatusAndLog();
            if (!err.empty()) {
                pk::showPanelError(w, err, L"启动本地推理服务失败");
            }
            return 0;
        }
        if (msg == WM_CLOSE) {
            if (!st->busy.load(std::memory_order_acquire)) DestroyWindow(w);
            return 0;
        }
        return DefWindowProcW(w, msg, wp, lp);
    }

    static void MEMESTUDIO_CALL onDestroy(void* ctx) { delete static_cast<LocalSdEnginePanel*>(ctx); }
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
    MemeStudioCancelTokenV2* cancel, MemeStudioBytesV2* outResult, MemeStudioBytesV2* outErr) {
    auto* self = static_cast<QwenImagePlugin*>(rawInst);
    if (!self || !commandId) return -1;
    try {
        std::string cmd(commandId);
        Json args = (argsJson && argsJson[0]) ? Json::parse(argsJson) : Json::object();

        if (cmd == "open-panel") {
            using PanelT = LocalSdEnginePanel<std::remove_pointer_t<decltype(self)>>;
            auto* pctx = new PanelT{self};
            if (self->host.raw()->open_panel(
                    self->host.raw()->host_ctx, "qwen-image", ownerHwnd,
                    780, 572, "Qwen-Image 本地推理配置",
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
            tmp.diffusionModel.clear();
            tmp.vaePath.clear();
            tmp.llmPath.clear();
            tmp.mmprojPath.clear();
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
            self->startLocalServer(cancel, nullptr);
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
            std::string tail = pk::readFileTailLossyUtf8(self->logFilePath(), 24 * 1024);
            self->host.writeOutBytes(outResult, Json{{"log", tail}}.dump());
            return 0;
        }
        if (cmd == "status") {
            std::lock_guard lock(self->mutex);
            self->host.writeOutBytes(outResult, self->toPublicJsonLocked().dump());
            return 0;
        }
        throw pk::PluginError("未知的本地推理插件命令：" + cmd);
    } catch (const std::exception& e) {
        self->host.writeOutBytes(outErr, e.what());
        return -1;
    }
}

int32_t MEMESTUDIO_CALL abiInterceptImageApi(
    void* rawInst,
    const char* op,
    const char* providerJson,
    const char* requestJson,
    const MemeStudioBytesV2* uploads,
    size_t uploadCount,
    MemeStudioCancelTokenV2* cancel,
    void (MEMESTUDIO_CALL* onPreview)(void*, const uint8_t*, size_t, int32_t),
    void* previewUser,
    MemeStudioBytesV2* outResp,
    MemeStudioBytesV2* outErr) {
    auto* self = static_cast<QwenImagePlugin*>(rawInst);
    if (!self || !op) return -1;
    try {
        Json prov = (providerJson && providerJson[0]) ? Json::parse(providerJson) : Json::object();
        Json req  = (requestJson && requestJson[0]) ? Json::parse(requestJson) : Json::object();
        return self->interceptCall(op, prov, req, uploads, uploadCount, cancel, onPreview, previewUser, outResp, outErr);
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
