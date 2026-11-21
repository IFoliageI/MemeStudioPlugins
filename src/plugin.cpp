#include "memestudio_plugin_kit.hpp"
#include "../vendor/nlohmann/json.hpp"
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace {

using Json = nlohmann::json;
namespace pk = meme::plugin_kit;

constexpr const char* kPluginId = "memestudio.waifu2x";

struct UpscaleSettings {
    pk::fs::path executable;
    pk::fs::path modelsDir;
    bool upscaleInput = false;
    bool upscaleOutput = false;
    bool tta = false;
    int scale = 2;
    int noise = 0;
    int tile = 0;
    int gpu = -2;
    int timeoutSeconds = 600;
};

bool hasModelPair(const pk::fs::path& dir) {
    std::error_code ec;
    if (!pk::fs::is_directory(dir, ec)) return false;
    bool foundParam = false;
    bool foundBin = false;
    for (const auto& entry : pk::fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().wstring();
        for (auto& ch : ext) ch = static_cast<wchar_t>(std::towlower(ch));
        if (ext == L".param") foundParam = true;
        if (ext == L".bin") foundBin = true;
        if (foundParam && foundBin) return true;
    }
    return false;
}

void validateSettings(const UpscaleSettings& s) {
    if (s.scale < 1 || s.scale > 32 || (s.scale & (s.scale - 1)) != 0) {
        throw pk::PluginError("超分倍率必须为 1、2、4、8、16 或 32");
    }
    if (s.noise < -1 || s.noise > 3) {
        throw pk::PluginError("降噪等级必须在 -1 到 3 之间");
    }
    if (s.tile != 0 && (s.tile < 32 || s.tile > 4096)) {
        throw pk::PluginError("分块大小必须为 0（自动）或 32..4096");
    }
    if (s.gpu < -2 || s.gpu > 15) {
        throw pk::PluginError("GPU 设备编号必须在 -2 到 15 之间");
    }
    if (s.timeoutSeconds < 5 || s.timeoutSeconds > 3600) {
        throw pk::PluginError("超分超时时间必须在 5 到 3600 秒之间");
    }
    // Always validate explicitly supplied paths even when toggle is off (fixes legacy bypass bug!)
    bool mustHavePaths = (s.upscaleInput || s.upscaleOutput);
    if (mustHavePaths || !s.executable.empty()) {
        if (s.executable.empty() || !pk::fs::is_regular_file(s.executable)) {
            throw pk::PluginError("指定的 waifu2x 可执行文件不存在");
        }
    }
    if (!s.modelsDir.empty()) {
        if (!hasModelPair(s.modelsDir)) {
            throw pk::PluginError("指定的模型目录无效（未找到配套的 .param 与 .bin 文件）");
        }
    } else if (mustHavePaths && !s.executable.empty()) {
        // Default models directory next to waifu2x-ncnn-vulkan.exe if present
        pk::fs::path sibling = s.executable.parent_path() / L"models-cunet";
        if (!hasModelPair(sibling) && !hasModelPair(s.executable.parent_path() / L"models-upconv_7_anime_style_art_rgb")) {
            throw pk::PluginError("未找到默认模型目录，请显式指定包含 .param/.bin 的模型文件夹");
        }
    }
}

std::wstring quoteWinArg(std::wstring_view arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
        return std::wstring(arg);
    }
    std::wstring out = L"\"";
    for (size_t i = 0;; ++i) {
        size_t bs = 0;
        while (i < arg.size() && arg[i] == L'\\') { ++i; ++bs; }
        if (i == arg.size()) { out.append(bs * 2, L'\\'); break; }
        if (arg[i] == L'"') { out.append(bs * 2 + 1, L'\\'); out.push_back(L'"'); }
        else { out.append(bs, L'\\'); out.push_back(arg[i]); }
    }
    out.push_back(L'"');
    return out;
}

void runWaifu2xBatchSubprocess(
    const UpscaleSettings& cfg,
    const pk::fs::path& inDir,
    const pk::fs::path& outDir,
    const pk::fs::path& logFile,
    const pk::HostClient& host,
    const MemeStudioCancelTokenV2* cancel) {
    host.verifyNotCancelled(cancel);

    struct HandleGuard {
        HANDLE h = nullptr;
        ~HandleGuard() { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    };

    HandleGuard job{CreateJobObjectW(nullptr, nullptr)};
    if (!job.h) throw pk::PluginError("创建 Waifu2x Job Object 失败");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job.h, JobObjectExtendedLimitInformation, &limits, sizeof(limits));

    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HandleGuard logHandle{CreateFileW(
        logFile.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr)};
    if (logHandle.h == INVALID_HANDLE_VALUE) {
        throw pk::PluginError("无法创建 Waifu2x 运行日志文件");
    }

    std::vector<std::wstring> args{
        L"-i", inDir.wstring(),
        L"-o", outDir.wstring(),
        L"-s", std::to_wstring(cfg.scale),
        L"-n", std::to_wstring(cfg.noise),
        L"-f", L"png"
    };
    pk::fs::path effectiveModels = cfg.modelsDir;
    if (effectiveModels.empty()) {
        pk::fs::path cunet = cfg.executable.parent_path() / L"models-cunet";
        effectiveModels = hasModelPair(cunet)
            ? cunet
            : (cfg.executable.parent_path() / L"models-upconv_7_anime_style_art_rgb");
    }
    args.push_back(L"-m");
    args.push_back(effectiveModels.wstring());
    if (cfg.tile > 0) {
        args.push_back(L"-t");
        args.push_back(std::to_wstring(cfg.tile));
    }
    if (cfg.gpu >= -1) {
        args.push_back(L"-g");
        args.push_back(std::to_wstring(cfg.gpu));
    }
    if (cfg.tta) {
        args.push_back(L"-x");
    }

    std::wstring cmdLine = quoteWinArg(cfg.executable.wstring());
    for (const auto& a : args) {
        cmdLine.push_back(L' ');
        cmdLine += quoteWinArg(a);
    }

    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<uint8_t> attrBuf(attrSize);
    auto* attrList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
    InitializeProcThreadAttributeList(attrList, 1, 0, &attrSize);
    struct AttrCleanup {
        LPPROC_THREAD_ATTRIBUTE_LIST l;
        ~AttrCleanup() { DeleteProcThreadAttributeList(l); }
    } attrCleanup{attrList};

    HANDLE whitelist[1] = {logHandle.h};
    UpdateProcThreadAttribute(attrList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, whitelist, sizeof(whitelist), nullptr, nullptr);

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(STARTUPINFOEXW);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.StartupInfo.wShowWindow = SW_HIDE;
    si.StartupInfo.hStdInput = INVALID_HANDLE_VALUE;
    si.StartupInfo.hStdOutput = logHandle.h;
    si.StartupInfo.hStdError = logHandle.h;
    si.lpAttributeList = attrList;

    PROCESS_INFORMATION pi{};
    pk::fs::path workDir = cfg.executable.parent_path();
    if (!CreateProcessW(
            cfg.executable.c_str(), cmdLine.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
            nullptr, workDir.empty() ? nullptr : workDir.c_str(), &si.StartupInfo, &pi)) {
        throw pk::PluginError("启动 Waifu2x 进程失败（Windows 错误码 " + std::to_string(GetLastError()) + "）");
    }

    HandleGuard proc{pi.hProcess};
    HandleGuard th{pi.hThread};
    CloseHandle(logHandle.h);
    logHandle.h = nullptr;

    if (!AssignProcessToJobObject(job.h, proc.h)) {
        TerminateProcess(proc.h, 1);
        throw pk::PluginError("绑定 Waifu2x 至 Job Object 失败");
    }
    ResumeThread(th.h);

    ULONGLONG startTick = GetTickCount64();
    ULONGLONG limitMs = static_cast<ULONGLONG>(cfg.timeoutSeconds) * 1000ull;
    for (;;) {
        DWORD wr = WaitForSingleObject(proc.h, 40);
        if (wr == WAIT_OBJECT_0) break;
        if (cancel && host.raw()->is_cancelled && host.raw()->is_cancelled(cancel) != 0) {
            TerminateJobObject(job.h, 130);
            WaitForSingleObject(proc.h, 2000);
            throw pk::PluginCancelled();
        }
        if (GetTickCount64() - startTick > limitMs) {
            TerminateJobObject(job.h, 124);
            WaitForSingleObject(proc.h, 2000);
            std::string tail = pk::trimAscii(pk::readFileTailLossyUtf8(logFile, 2048));
            throw pk::PluginError("Waifu2x 执行超时（超过 " + std::to_string(cfg.timeoutSeconds) + " 秒）" +
                                  (tail.empty() ? "" : "：\n" + tail));
        }
    }

    DWORD exitCode = 1;
    GetExitCodeProcess(proc.h, &exitCode);
    if (exitCode != 0) {
        // Critical BUG-20 fix: Read and surface the captured stderr/stdout log before temp cleanup!
        std::string tail = pk::trimAscii(pk::readFileTailLossyUtf8(logFile, 2048));
        throw pk::PluginError("Waifu2x 处理失败（退出码 " + std::to_string(exitCode) + "）" +
                              (tail.empty() ? "" : "：\n" + tail));
    }
}

struct Waifu2xPlugin {
    pk::HostClient host;
    mutable std::mutex mutex;
    UpscaleSettings settings;

    explicit Waifu2xPlugin(const MemeStudioHostV2* h) : host(h, kPluginId) {
        loadFromDisk();
    }

    pk::fs::path settingsFile() const {
        return host.dataDir() / L"waifu2x.json";
    }

    static Json toJson(const UpscaleSettings& s) {
        return Json{
            {"executable", pk::wideToUtf8(s.executable.wstring())},
            {"models", pk::wideToUtf8(s.modelsDir.wstring())},
            {"input", s.upscaleInput},
            {"output", s.upscaleOutput},
            {"tta", s.tta},
            {"scale", s.scale},
            {"noise", s.noise},
            {"tile", s.tile},
            {"gpu", s.gpu},
            {"timeout", s.timeoutSeconds}
        };
    }

    static UpscaleSettings fromJson(const Json& j) {
        UpscaleSettings s;
        if (j.contains("executable") && j["executable"].is_string()) {
            s.executable = pk::utf8ToWideStrict(j["executable"].get<std::string>());
        }
        if (j.contains("models") && j["models"].is_string()) {
            s.modelsDir = pk::utf8ToWideStrict(j["models"].get<std::string>());
        }
        s.upscaleInput = j.value("input", false);
        s.upscaleOutput = j.value("output", false);
        s.tta = j.value("tta", false);
        s.scale = j.value("scale", 2);
        s.noise = j.value("noise", 0);
        s.tile = j.value("tile", 0);
        s.gpu = j.value("gpu", -2);
        s.timeoutSeconds = j.value("timeout", 600);
        return s;
    }

    void loadFromDisk() {
        try {
            auto p = settingsFile();
            if (!pk::fs::exists(p)) return;
            auto raw = pk::readFileBytes(p, 128 * 1024);
            settings = fromJson(Json::parse(raw.begin(), raw.end()));
        } catch (...) {}
    }

    void saveToDisk(const UpscaleSettings& next) {
        validateSettings(next);
        std::string dumped = toJson(next).dump(2);
        pk::writeFileAtomic(settingsFile(), std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(dumped.data()), dumped.size()));
        std::lock_guard lock(mutex);
        settings = next;
    }

    void processBatch(
        std::string_view stage,
        MemeStudioImageBufferV2* frames,
        size_t count,
        MemeStudioCancelTokenV2* cancel) {
        if (!frames || count == 0) return;
        UpscaleSettings snap;
        {
            std::lock_guard lock(mutex);
            snap = settings;
        }
        bool shouldRun = (stage == "input" && snap.upscaleInput) ||
                         (stage == "output" && snap.upscaleOutput);
        if (!shouldRun || (snap.scale == 1 && snap.noise == -1)) {
            return;
        }
        validateSettings(snap);

        // Pre-validate memory bounds before running GPU process!
        uint64_t targetW = static_cast<uint64_t>(frames[0].width) * snap.scale;
        uint64_t targetH = static_cast<uint64_t>(frames[0].height) * snap.scale;
        if (targetW > 16384 || targetH > 16384 || targetW * targetH > 32ull * 1024 * 1024 ||
            targetW * targetH * 4 * count > 384ull * 1024 * 1024) {
            throw pk::PluginError("超分后的图像或动画总像素超出 384MB 安全上限，请调低超分倍率");
        }

        pk::fs::path batchRoot = host.dataDir() / L"temp" / pk::utf8ToWideStrict(pk::randomHex(8));
        pk::fs::path inDir = batchRoot / L"in";
        pk::fs::path outDir = batchRoot / L"out";
        pk::fs::path logFile = batchRoot / L"waifu2x.log";
        pk::fs::create_directories(inDir);
        pk::fs::create_directories(outDir);

        struct BatchDirCleanup {
            pk::fs::path dir;
            ~BatchDirCleanup() {
                std::error_code ec;
                pk::fs::remove_all(dir, ec);
            }
        } cleanup{batchRoot};

        // 1. Export all frames in batch
        MemeStudioBytesV2 errBytes{};
        for (size_t i = 0; i < count; ++i) {
            host.verifyNotCancelled(cancel);
            std::wostringstream fn;
            fn << L"frame_" << std::setw(5) << std::setfill(L'0') << i << L".png";
            std::string framePathUtf8 = pk::wideToUtf8((inDir / fn.str()).wstring());
            if (host.raw()->encode_media(
                    host.raw()->host_ctx, framePathUtf8.c_str(), R"({"format":"png"})",
                    &frames[i], 1, nullptr, &errBytes) != 0) {
                throw pk::PluginError("写入超分输入帧失败：" + host.consumeBytesToString(errBytes));
            }
        }

        // 2. Run Waifu2x ONCE for the entire directory!
        runWaifu2xBatchSubprocess(snap, inDir, outDir, logFile, host, cancel);

        // 3. Decode all upscaled frames back into host-allocated buffers
        for (size_t i = 0; i < count; ++i) {
            host.verifyNotCancelled(cancel);
            std::wostringstream fn;
            fn << L"frame_" << std::setw(5) << std::setfill(L'0') << i << L".png";
            pk::fs::path outFile = outDir / fn.str();
            if (!pk::fs::is_regular_file(outFile)) {
                throw pk::PluginError("Waifu2x 未生成预期输出文件：" + pk::wideToUtf8(fn.str()));
            }
            std::string outPathUtf8 = pk::wideToUtf8(outFile.wstring());
            MemeStudioImageBufferV2* upFrames = nullptr;
            size_t upCount = 0;
            if (host.raw()->decode_media(
                    host.raw()->host_ctx, outPathUtf8.c_str(), nullptr, 0,
                    nullptr, &upFrames, &upCount, &errBytes) != 0 || upCount == 0) {
                throw pk::PluginError("解码超分输出帧失败：" + host.consumeBytesToString(errBytes));
            }

            struct UpGuard {
                const MemeStudioHostV2* h;
                MemeStudioImageBufferV2* f;
                size_t n;
                ~UpGuard() { if (h && h->free_images && f) h->free_images(h->host_ctx, f, n); }
            } upGuard{host.raw(), upFrames, upCount};

            uint32_t expW = frames[i].width * static_cast<uint32_t>(snap.scale);
            uint32_t expH = frames[i].height * static_cast<uint32_t>(snap.scale);
            if (upFrames[0].width != expW || upFrames[0].height != expH) {
                throw pk::PluginError("Waifu2x 输出尺寸与预期倍率不符");
            }

            size_t newBytes = static_cast<size_t>(upFrames[0].width) * upFrames[0].height * 4;
            auto* newBuf = static_cast<uint8_t*>(host.raw()->alloc(host.raw()->host_ctx, newBytes));
            if (!newBuf) throw pk::PluginError("分配超分帧像素内存失败");

            for (uint32_t y = 0; y < upFrames[0].height; ++y) {
                std::memcpy(
                    newBuf + static_cast<size_t>(y) * upFrames[0].width * 4,
                    upFrames[0].bgra_pixels + static_cast<size_t>(y) * upFrames[0].stride,
                    static_cast<size_t>(upFrames[0].width) * 4);
            }
            host.raw()->free(host.raw()->host_ctx, frames[i].bgra_pixels);
            frames[i].width = upFrames[0].width;
            frames[i].height = upFrames[0].height;
            frames[i].stride = upFrames[0].width * 4;
            frames[i].bgra_pixels = newBuf;
        }
    }
};

int32_t MEMESTUDIO_CALL abiCreate(const MemeStudioHostV2* host, void** outInst, MemeStudioBytesV2* outErr) {
    if (!host || !outInst || host->abi_version != MEMESTUDIO_PLUGIN_ABI_V2) return -1;
    try {
        *outInst = new Waifu2xPlugin(host);
        return 0;
    } catch (const std::exception& e) {
        pk::HostClient(host, kPluginId).writeOutBytes(outErr, e.what());
        return -1;
    }
}

void MEMESTUDIO_CALL abiDestroy(void* inst) {
    delete static_cast<Waifu2xPlugin*>(inst);
}

int32_t MEMESTUDIO_CALL abiInvokeCommand(
    void* rawInst,
    const char* commandId,
    const char* argsJson,
    void*,
    MemeStudioCancelTokenV2*,
    MemeStudioBytesV2* outResult,
    MemeStudioBytesV2* outErr) {
    auto* self = static_cast<Waifu2xPlugin*>(rawInst);
    if (!self || !commandId) return -1;
    try {
        std::string cmd(commandId);
        Json args = (argsJson && argsJson[0]) ? Json::parse(argsJson) : Json::object();

        if (cmd == "configure" && !args.empty()) {
            UpscaleSettings current;
            {
                std::lock_guard lock(self->mutex);
                current = self->settings;
            }
            Json merged = Waifu2xPlugin::toJson(current);
            for (const auto& [k, v] : args.items()) merged[k] = v;
            self->saveToDisk(Waifu2xPlugin::fromJson(merged));
        } else if (cmd != "configure" && cmd != "status") {
            throw pk::PluginError("未知的 Waifu2x 插件命令：" + cmd);
        }

        UpscaleSettings snap;
        {
            std::lock_guard lock(self->mutex);
            snap = self->settings;
        }
        self->host.writeOutBytes(outResult, Waifu2xPlugin::toJson(snap).dump());
        return 0;
    } catch (const std::exception& e) {
        self->host.writeOutBytes(outErr, e.what());
        return -1;
    }
}

int32_t MEMESTUDIO_CALL abiProcessFramesBatch(
    void* rawInst,
    const char* stage,
    const char*,
    MemeStudioImageBufferV2* frames,
    size_t frameCount,
    MemeStudioCancelTokenV2* cancel,
    MemeStudioBytesV2* outErr) {
    auto* self = static_cast<Waifu2xPlugin*>(rawInst);
    if (!self || !stage) return -1;
    try {
        self->processBatch(stage, frames, frameCount, cancel);
        return 0;
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
    abiProcessFramesBatch,
    nullptr
};

} // namespace

extern "C" MEMESTUDIO_EXPORT const MemeStudioPluginV2* MEMESTUDIO_CALL MemeStudioGetPluginV2(void) {
    return &kDescriptor;
}
