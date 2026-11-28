#include "memestudio_plugin_kit.hpp"
#include "../vendor/nlohmann/json.hpp"
#include <sddl.h>
#include <atomic>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using Json = nlohmann::json;
namespace pk = meme::plugin_kit;

constexpr const char* kPluginId = "memestudio.deepseek-harness";
constexpr const wchar_t* kDefaultPipeName = L"\\\\.\\pipe\\MemeStudio.DeepSeekHarness.v1";

struct HarnessPolicy {
    bool autoStart = true;
    bool allowInspect = true;
    bool allowModels = false;
    bool allowReplace = false;
    bool allowDraw = false;
    bool allowRetouch = false;
    bool allowSwitchModel = false;
    std::vector<std::string> allowedModels;
    int maxPerTask = 8;
    int maxPerDay = 32;
};

std::string currentUtcDateIso() {
    std::time_t now = std::time(nullptr);
    std::tm utcTm{};
    gmtime_s(&utcTm, &now);
    char buf[16]{};
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                  utcTm.tm_year + 1900, utcTm.tm_mon + 1, utcTm.tm_mday);
    return buf;
}

std::wstring buildCurrentUserPipeSddl() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        throw pk::PluginError("无法读取当前进程访问令牌");
    }
    struct TokenGuard { HANDLE h; ~TokenGuard() { CloseHandle(h); } } tg{token};

    DWORD needed = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    if (needed == 0) throw pk::PluginError("查询当前用户 SID 长度失败");
    std::vector<uint8_t> userBuf(needed);
    if (!GetTokenInformation(token, TokenUser, userBuf.data(), needed, &needed)) {
        throw pk::PluginError("读取当前用户 SID 失败");
    }

    auto* tokenUser = reinterpret_cast<TOKEN_USER*>(userBuf.data());
    LPWSTR sidStr = nullptr;
    if (!ConvertSidToStringSidW(tokenUser->User.Sid, &sidStr) || !sidStr) {
        throw pk::PluginError("格式化用户 SID 失败");
    }
    std::wstring sddl = L"D:P(A;;GRGW;;;" + std::wstring(sidStr) + L")";
    LocalFree(sidStr);
    return sddl;
}

std::wstring quoteCliArg(std::wstring_view a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring_view::npos) return std::wstring(a);
    std::wstring q = L"\"";
    for (wchar_t ch : a) {
        if (ch == L'"') q += L"\\\"";
        else q.push_back(ch);
    }
    q.push_back(L'"');
    return q;
}

pk::fs::path siblingCliExecutablePath() {
    std::wstring buf(32768, L'\0');
    DWORD len = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (len == 0 || len >= buf.size()) return {};
    buf.resize(len);
    return pk::fs::path(buf).parent_path() / L"MemeStudio-cli.exe";
}

class SelfHealingQuotaLedger {
    pk::fs::path ledgerPath_;
    mutable std::mutex mutex_;
public:
    explicit SelfHealingQuotaLedger(pk::fs::path path) : ledgerPath_(std::move(path)) {}

    int usedToday() const {
        std::lock_guard lock(mutex_);
        return readCountUnlocked(currentUtcDateIso());
    }

    void checkCanSpend(int count, const HarnessPolicy& policy) const {
        if (count < 1) return;
        if (count > policy.maxPerTask) {
            throw pk::PluginError("单次请求数量 (" + std::to_string(count) +
                                  ") 超出单任务配额上限 (" + std::to_string(policy.maxPerTask) + ")");
        }
        std::lock_guard lock(mutex_);
        int used = readCountUnlocked(currentUtcDateIso());
        if (used + count > policy.maxPerDay) {
            throw pk::PluginError("今日配额不足（已用 " + std::to_string(used) +
                                  " / 上限 " + std::to_string(policy.maxPerDay) + "）");
        }
    }

    int recordSpend(int count) {
        if (count <= 0) return usedToday();
        std::lock_guard lock(mutex_);
        std::string today = currentUtcDateIso();
        int nextUsed = readCountUnlocked(today) + count;
        Json j{{"utc_date", today}, {"used", nextUsed}};
        std::string dumped = j.dump(2);
        pk::writeFileAtomic(ledgerPath_, std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(dumped.data()), dumped.size()));
        return nextUsed;
    }

private:
    int readCountUnlocked(const std::string& today) const noexcept {
        try {
            if (!pk::fs::exists(ledgerPath_)) return 0;
            auto raw = pk::readFileBytes(ledgerPath_, 16 * 1024);
            auto j = Json::parse(raw.begin(), raw.end());
            std::string recorded = j.value("utc_date", std::string());
            if (recorded != today) return 0;
            return std::max(0, j.value("used", 0));
        } catch (...) {
            return 0;
        }
    }
};

struct HarnessPlugin {
    pk::HostClient host;
    mutable std::mutex mutex;
    HarnessPolicy policy;
    std::wstring pipeName = kDefaultPipeName;
    SelfHealingQuotaLedger ledger;

    HANDLE stopEvent = nullptr;
    std::thread serverThread;
    std::atomic_bool serverRunning{false};

    explicit HarnessPlugin(const MemeStudioHostV2* h)
        : host(h, kPluginId),
          ledger(host.dataDir() / L"quota_ledger.json") {
        stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        loadPolicyFromDisk();
        if (policy.autoStart) {
            try {
                startServer();
                if (serverRunning.load(std::memory_order_acquire)) {
                    host.log(MEMESTUDIO_LOG_INFO, "DeepSeek Harness 本机命名管道服务已自动启动");
                }
            } catch (...) {}
        }
    }

    ~HarnessPlugin() {
        stopServer();
        if (stopEvent) {
            CloseHandle(stopEvent);
            stopEvent = nullptr;
        }
    }

    pk::fs::path policyPath() const { return host.dataDir() / L"policy.json"; }

    static Json policyToJson(const HarnessPolicy& p) {
        return Json{
            {"auto_start", p.autoStart},
            {"allow_inspect", p.allowInspect},
            {"allow_models", p.allowModels},
            {"allow_replace", p.allowReplace},
            {"allow_draw", p.allowDraw},
            {"allow_retouch", p.allowRetouch},
            {"allow_switch_model", p.allowSwitchModel},
            {"allowed_models", p.allowedModels},
            {"max_per_task", p.maxPerTask},
            {"max_per_day", p.maxPerDay}
        };
    }

    static HarnessPolicy policyFromJson(const Json& j) {
        HarnessPolicy p;
        p.autoStart        = j.value("auto_start", true);
        p.allowInspect     = j.value("allow_inspect", true);
        p.allowModels      = j.value("allow_models", false);
        p.allowReplace     = j.value("allow_replace", false);
        p.allowDraw        = j.value("allow_draw", false);
        p.allowRetouch     = j.value("allow_retouch", false);
        p.allowSwitchModel = j.value("allow_switch_model", false);
        p.maxPerTask       = std::clamp(j.value("max_per_task", 8), 1, 10000);
        p.maxPerDay        = std::clamp(j.value("max_per_day", 32), 1, 10000);
        if (j.contains("allowed_models") && j["allowed_models"].is_array()) {
            for (const auto& m : j["allowed_models"]) {
                if (m.is_string() && !pk::trimAscii(m.get<std::string>()).empty()) {
                    p.allowedModels.push_back(pk::trimAscii(m.get<std::string>()));
                }
            }
        }
        return p;
    }

    void loadPolicyFromDisk() {
        try {
            if (!pk::fs::exists(policyPath())) return;
            auto raw = pk::readFileBytes(policyPath(), 64 * 1024);
            policy = policyFromJson(Json::parse(raw.begin(), raw.end()));
        } catch (...) {}
    }

    void savePolicyToDisk(const HarnessPolicy& next) {
        std::string dumped = policyToJson(next).dump(2);
        pk::writeFileAtomic(policyPath(), std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(dumped.data()), dumped.size()));
        std::lock_guard lock(mutex);
        policy = next;
    }

    Json readWorkspaceProvider() const {
        MemeStudioBytesV2 wsBytes{};
        if (host.raw()->get_workspace_state &&
            host.raw()->get_workspace_state(host.raw()->host_ctx, &wsBytes) == 0) {
            try {
                auto ws = Json::parse(host.consumeBytesToString(wsBytes));
                if (ws.contains("provider") && ws["provider"].is_object()) {
                    return ws["provider"];
                }
            } catch (...) {}
        }
        return Json::object();
    }

    void checkModelOverrideAllowed(const std::string& requestedModel, const HarnessPolicy& snap) const {
        if (requestedModel.empty()) return;
        if (!snap.allowSwitchModel) {
            throw pk::PluginError("当前权限策略禁止外部调用覆盖模型名称");
        }
        if (!snap.allowedModels.empty() &&
            std::find(snap.allowedModels.begin(), snap.allowedModels.end(), requestedModel) == snap.allowedModels.end()) {
            throw pk::PluginError("模型不在允许白名单内：" + requestedModel);
        }
    }

    pk::fs::path createIsolatedRunFolder(const Json& args) const {
        pk::fs::path outParent = args.contains("output_parent") && args["output_parent"].is_string() &&
                                 !pk::trimAscii(args["output_parent"].get<std::string>()).empty()
            ? pk::fs::path(pk::utf8ToWideStrict(args["output_parent"].get<std::string>()))
            : (host.dataDir() / L"runs");
        pk::fs::path runDir = outParent / (L"meme-run-" + pk::utf8ToWideStrict(pk::randomHex(6)));
        pk::fs::create_directories(runDir);
        return runDir;
    }

    Json runSiblingCliSubcommand(const std::vector<std::wstring>& cliArgs, const pk::fs::path& runDir) const {
        pk::fs::path cliExe = siblingCliExecutablePath();
        if (cliExe.empty() || !pk::fs::is_regular_file(cliExe)) {
            throw pk::PluginError("未找到同目录 MemeStudio-cli.exe 执行器");
        }
        pk::fs::path outLog = runDir / L"cli-stdout.json";
        pk::fs::path errLog = runDir / L"cli-stderr.txt";

        SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE hNul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
        HANDLE hOut = CreateFileW(outLog.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        HANDLE hErr = CreateFileW(errLog.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

        std::wstring cmdLine = quoteCliArg(cliExe.wstring());
        for (const auto& a : cliArgs) {
            cmdLine.push_back(L' ');
            cmdLine += quoteCliArg(a);
        }

        STARTUPINFOW si{sizeof(STARTUPINFOW)};
        si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        si.hStdInput  = hNul;
        si.hStdOutput = hOut;
        si.hStdError  = hErr;

        PROCESS_INFORMATION pi{};
        BOOL ok = CreateProcessW(
            cliExe.c_str(), cmdLine.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW, nullptr, runDir.c_str(), &si, &pi);
        if (hNul != INVALID_HANDLE_VALUE) CloseHandle(hNul);
        if (hOut != INVALID_HANDLE_VALUE) CloseHandle(hOut);
        if (hErr != INVALID_HANDLE_VALUE) CloseHandle(hErr);
        if (!ok) {
            throw pk::PluginError("启动 MemeStudio-cli 失败");
        }

        WaitForSingleObject(pi.hProcess, 600000);
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);

        std::string stdoutText = pk::readFileTailLossyUtf8(outLog, 64 * 1024);
        std::string stderrText = pk::trimAscii(pk::readFileTailLossyUtf8(errLog, 16 * 1024));
        std::error_code ec;
        pk::fs::remove(outLog, ec);
        pk::fs::remove(errLog, ec);

        if (code != 0) {
            throw pk::PluginError(stderrText.empty() ? "子任务执行失败" : stderrText);
        }
        return Json::parse(stdoutText);
    }

    Json handleRpcRequest(const Json& req) {
        std::string op = req.at("operation").get<std::string>();
        Json args = req.value("arguments", Json::object());

        HarnessPolicy snap;
        {
            std::lock_guard lock(mutex);
            snap = policy;
        }

        if (op == "status") {
            return Json{
                {"plugin_id", kPluginId},
                {"running", serverRunning.load(std::memory_order_acquire)},
                {"pipe_name", pk::wideToUtf8(pipeName)},
                {"policy", policyToJson(snap)},
                {"used_today", ledger.usedToday()},
                {"utc_date", currentUtcDateIso()}
            };
        }

        if (op == "inspect") {
            if (!snap.allowInspect) {
                throw pk::PluginError("当前权限策略未允许 inspect 图像检查操作");
            }
            std::string pathUtf8 = args.at("path").get<std::string>();
            MemeStudioBytesV2 metaBytes{}, errBytes{};
            if (host.raw()->probe_media(host.raw()->host_ctx, pathUtf8.c_str(), &metaBytes, &errBytes) != 0) {
                throw pk::PluginError(host.consumeBytesToString(errBytes));
            }
            return Json::parse(host.consumeBytesToString(metaBytes));
        }

        if (op == "models") {
            if (!snap.allowModels) {
                throw pk::PluginError("当前权限策略未开启 models 模型查询授权（请在 Harness 权限面板中勾选）");
            }
            Json prov = readWorkspaceProvider();
            std::string baseUrl = pk::trimAscii(prov.value("base_url", std::string()));
            if (baseUrl.empty()) throw pk::PluginError("MemeStudio 主界面尚未配置接口地址");
            while (!baseUrl.empty() && baseUrl.back() == '/') baseUrl.pop_back();
            std::string modelsUrl = baseUrl.ends_with("/v1") ? (baseUrl + "/models") : (baseUrl + "/v1/models");

            Json spec{
                {"url", modelsUrl},
                {"method", "GET"},
                {"timeout_seconds", 20},
                {"allow_loopback_http", true},
                {"headers", {{"Authorization", "Bearer " + prov.value("api_key", std::string())}}}
            };
            std::string specStr = spec.dump();
            uint32_t status = 0;
            int32_t streamed = 0;
            MemeStudioBytesV2 resp{}, err{};
            if (host.raw()->http_send(host.raw()->host_ctx, specStr.c_str(), nullptr, 0, nullptr, nullptr, nullptr, &status, &streamed, &resp, &err) != 0 || status != 200) {
                std::string e = host.consumeBytesToString(err);
                host.consumeBytesToString(resp);
                throw pk::PluginError(e.empty() ? ("查询模型列表返回 HTTP " + std::to_string(status)) : e);
            }
            auto parsed = Json::parse(host.consumeBytesToString(resp));
            std::vector<std::string> ids;
            for (const auto& it : parsed.value("data", Json::array())) {
                if (it.is_object() && it.contains("id") && it["id"].is_string()) {
                    ids.push_back(it["id"].get<std::string>());
                }
            }
            return Json{{"models", ids}};
        }

        if (op == "replace") {
            if (!snap.allowReplace) {
                throw pk::PluginError("当前权限策略未开启图像角色替换 (replace) 授权");
            }
            if (!args.value("confirmed", false)) {
                throw pk::PluginError("生图请求必须显式传入 confirmed=true 确认授权");
            }

            std::string srcUtf8 = args.at("source").get<std::string>();
            MemeStudioBytesV2 metaB{}, metaE{};
            bool isAnimated = false;
            int framesEst = 1;
            if (host.raw()->probe_media(host.raw()->host_ctx, srcUtf8.c_str(), &metaB, &metaE) == 0) {
                auto mj = Json::parse(host.consumeBytesToString(metaB));
                isAnimated = mj.value("animated", false);
                framesEst = std::max(1, mj.value("frames", 1));
            } else {
                host.consumeBytesToString(metaE);
            }

            int count = std::clamp(args.value("count", 1), 1, 10);
            int maxFrames = std::clamp(args.value("max_frames", 0), 0, 600);
            if (isAnimated && maxFrames > 0) framesEst = std::min(framesEst, maxFrames);
            int totalCostUnits = count * framesEst;
            ledger.checkCanSpend(totalCostUnits, snap);

            std::string requestedModel = pk::trimAscii(args.value("model", std::string()));
            checkModelOverrideAllowed(requestedModel, snap);

            pk::fs::path runDir = createIsolatedRunFolder(args);
            pk::fs::path outFile = runDir / (isAnimated ? L"output.gif" : L"output.png");

            Json prov = readWorkspaceProvider();
            Json jobPayload = prov;
            jobPayload["source"] = srcUtf8;
            jobPayload["character"] = args.at("character").get<std::string>();
            jobPayload["output"] = pk::wideToUtf8(outFile.wstring());
            jobPayload["instruction"] = args.value("instruction", std::string());
            jobPayload["count"] = count;
            if (args.contains("styles")) jobPayload["styles"] = args["styles"];
            if (args.contains("crop")) jobPayload["crop"] = args["crop"];
            if (maxFrames > 0) jobPayload["max_frames"] = maxFrames;
            if (!requestedModel.empty()) jobPayload["model"] = requestedModel;

            std::string jobDump = jobPayload.dump();
            MemeStudioBytesV2 resBytes{}, errBytes{};
            int32_t rc = host.raw()->run_core_replace_job(
                host.raw()->host_ctx, jobDump.c_str(), nullptr, nullptr, nullptr, &resBytes, &errBytes);
            std::string errMsg = host.consumeBytesToString(errBytes);
            std::string resMsg = host.consumeBytesToString(resBytes);
            if (rc != 0) {
                throw pk::PluginError(errMsg.empty() ? "执行替换任务失败" : errMsg);
            }

            int updatedUsed = ledger.recordSpend(totalCostUnits);
            Json parsedRes = resMsg.empty() ? Json::object() : Json::parse(resMsg);
            parsedRes["used_today"] = updatedUsed;
            return parsedRes;
        }

        if (op == "draw" || op == "retouch") {
            if (op == "draw" && !snap.allowDraw) {
                throw pk::PluginError("当前权限策略未开启提示词生图 (draw) 授权");
            }
            if (op == "retouch" && !snap.allowRetouch) {
                throw pk::PluginError("当前权限策略未开启提示词修图与精修 (retouch) 授权");
            }
            if (!args.value("confirmed", false)) {
                throw pk::PluginError("生图请求必须显式传入 confirmed=true 确认授权");
            }

            int count = std::clamp(args.value("count", 1), 1, 10);
            bool isAnimated = false;
            int framesEst = 1;
            if (op == "retouch") {
                std::string srcUtf8 = args.at("source").get<std::string>();
                MemeStudioBytesV2 metaB{}, metaE{};
                if (host.raw()->probe_media(host.raw()->host_ctx, srcUtf8.c_str(), &metaB, &metaE) == 0) {
                    auto mj = Json::parse(host.consumeBytesToString(metaB));
                    isAnimated = mj.value("animated", false);
                    framesEst = std::max(1, mj.value("frames", 1));
                } else {
                    host.consumeBytesToString(metaE);
                }
            }

            int totalUnits = count * framesEst;
            ledger.checkCanSpend(totalUnits, snap);

            std::string requestedModel = pk::trimAscii(args.value("model", std::string()));
            checkModelOverrideAllowed(requestedModel, snap);

            Json prov = readWorkspaceProvider();
            std::string effModel = requestedModel.empty() ? prov.value("model", std::string()) : requestedModel;

            pk::fs::path runDir = createIsolatedRunFolder(args);
            pk::fs::path outFile = runDir / (isAnimated ? L"output.gif" : L"output.png");

            std::vector<std::wstring> cliArgs{
                pk::utf8ToWideStrict(op),
                L"--output", outFile.wstring(),
                L"--prompt", pk::utf8ToWideStrict(args.value("instruction", std::string())),
                L"--count", std::to_wstring(count)
            };
            if (!prov.value("base_url", "").empty()) {
                cliArgs.push_back(L"--base-url");
                cliArgs.push_back(pk::utf8ToWideStrict(prov["base_url"].get<std::string>()));
            }
            if (!prov.value("api_key", "").empty()) {
                cliArgs.push_back(L"--api-key");
                cliArgs.push_back(pk::utf8ToWideStrict(prov["api_key"].get<std::string>()));
            }
            if (!effModel.empty()) {
                cliArgs.push_back(L"--model");
                cliArgs.push_back(pk::utf8ToWideStrict(effModel));
            }
            for (const auto& sp : args.value("styles", Json::array())) {
                cliArgs.push_back(L"--style");
                cliArgs.push_back(pk::utf8ToWideStrict(sp.get<std::string>()));
            }
            if (op == "retouch") {
                cliArgs.push_back(L"--source");
                cliArgs.push_back(pk::utf8ToWideStrict(args.at("source").get<std::string>()));
                std::string mode = args.value("mode", "prompt");
                cliArgs.push_back(L"--mode");
                cliArgs.push_back(pk::utf8ToWideStrict(mode));
                if (args.contains("region") && args["region"].is_object()) {
                    const auto& r = args["region"];
                    std::ostringstream rs;
                    rs << r.value("x", 0.0) << "," << r.value("y", 0.0) << ","
                       << r.value("width", 1.0) << "," << r.value("height", 1.0);
                    cliArgs.push_back(L"--region");
                    cliArgs.push_back(pk::utf8ToWideStrict(rs.str()));
                }
            }

            Json cliRes = runSiblingCliSubcommand(cliArgs, runDir);
            cliRes["used_today"] = ledger.recordSpend(totalUnits);
            return cliRes;
        }

        throw pk::PluginError("不支持的自动化操作：" + op);
    }

    void startServer(std::wstring customPipeName = {}) {
        stopServer();
        if (!customPipeName.empty()) {
            pipeName = std::move(customPipeName);
        }
        ResetEvent(stopEvent);
        serverRunning.store(false, std::memory_order_release);

        std::wstring sddl = buildCurrentUserPipeSddl();
        std::wstring targetPipe = pipeName;
        HANDLE bindDoneEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

        serverThread = std::thread([this, sddl, targetPipe, bindDoneEvent]() {
            SECURITY_ATTRIBUTES sa{};
            sa.nLength = sizeof(SECURITY_ATTRIBUTES);
            sa.bInheritHandle = FALSE;
            if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                    sddl.c_str(), SDDL_REVISION_1, &sa.lpSecurityDescriptor, nullptr)) {
                if (bindDoneEvent) SetEvent(bindDoneEvent);
                return;
            }
            struct SdGuard { PSECURITY_DESCRIPTOR p; ~SdGuard() { LocalFree(p); } } sdg{sa.lpSecurityDescriptor};

            HANDLE ioEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!ioEvent) {
                if (bindDoneEvent) SetEvent(bindDoneEvent);
                return;
            }
            struct EvGuard { HANDLE e; ~EvGuard() { CloseHandle(e); } } evg{ioEvent};

            bool firstIteration = true;
            bool bindSignaled = false;
            auto signalBindOnce = [&]() {
                if (!bindSignaled && bindDoneEvent) {
                    bindSignaled = true;
                    SetEvent(bindDoneEvent);
                }
            };

            while (WaitForSingleObject(stopEvent, 0) != WAIT_OBJECT_0) {
                DWORD openMode = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
                                 (firstIteration ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0);
                firstIteration = false;
                HANDLE pipe = CreateNamedPipeW(
                    targetPipe.c_str(),
                    openMode,
                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                    1, 65536, 65536, 0, &sa);
                if (pipe == INVALID_HANDLE_VALUE) {
                    serverRunning.store(false, std::memory_order_release);
                    signalBindOnce();
                    break;
                }
                serverRunning.store(true, std::memory_order_release);
                signalBindOnce();
                struct PipeGuard { HANDLE h; ~PipeGuard() { CloseHandle(h); } } pg{pipe};

                OVERLAPPED ov{};
                ResetEvent(ioEvent);
                ov.hEvent = ioEvent;
                BOOL connected = ConnectNamedPipe(pipe, &ov);
                if (!connected) {
                    DWORD err = GetLastError();
                    if (err == ERROR_IO_PENDING) {
                        HANDLE waits[2] = {ioEvent, stopEvent};
                        DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
                        if (w != WAIT_OBJECT_0) {
                            CancelIoEx(pipe, &ov);
                            break;
                        }
                    } else if (err != ERROR_PIPE_CONNECTED) {
                        continue;
                    }
                }

                std::string lineBuf;
                bool gotNewline = false;
                while (lineBuf.size() <= 65536 && !gotNewline) {
                    char chunk[4096]{};
                    DWORD got = 0;
                    ResetEvent(ioEvent);
                    OVERLAPPED rov{};
                    rov.hEvent = ioEvent;
                    if (!ReadFile(pipe, chunk, sizeof(chunk), &got, &rov)) {
                        if (GetLastError() != ERROR_IO_PENDING) break;
                        HANDLE waits[2] = {ioEvent, stopEvent};
                        if (WaitForMultipleObjects(2, waits, FALSE, 15000) != WAIT_OBJECT_0) {
                            CancelIoEx(pipe, &rov);
                            break;
                        }
                        if (!GetOverlappedResult(pipe, &rov, &got, FALSE) || got == 0) break;
                    }
                    if (got == 0) break;
                    lineBuf.append(chunk, got);
                    auto nl = lineBuf.find('\n');
                    if (nl != std::string::npos) {
                        lineBuf.resize(nl);
                        gotNewline = true;
                    }
                }

                if (gotNewline && !lineBuf.empty()) {
                    Json reply;
                    try {
                        Json req = Json::parse(lineBuf);
                        reply = Json{{"ok", true}, {"result", handleRpcRequest(req)}};
                    } catch (const std::exception& e) {
                        reply = Json{{"ok", false}, {"error", e.what()}};
                    }

                    std::string wire = reply.dump() + "\n";
                    ResetEvent(ioEvent);
                    OVERLAPPED wov{};
                    wov.hEvent = ioEvent;
                    DWORD written = 0;
                    if (!WriteFile(pipe, wire.data(), static_cast<DWORD>(wire.size()), &written, &wov)) {
                        if (GetLastError() == ERROR_IO_PENDING) {
                            HANDLE waits[2] = {ioEvent, stopEvent};
                            if (WaitForMultipleObjects(2, waits, FALSE, 5000) == WAIT_OBJECT_0) {
                                GetOverlappedResult(pipe, &wov, &written, FALSE);
                            } else {
                                CancelIoEx(pipe, &wov);
                            }
                        }
                    }
                    FlushFileBuffers(pipe);
                }
                DisconnectNamedPipe(pipe);
            }
            serverRunning.store(false, std::memory_order_release);
            signalBindOnce();
        });
        if (bindDoneEvent) {
            WaitForSingleObject(bindDoneEvent, 2000);
            CloseHandle(bindDoneEvent);
        }
    }

    void stopServer() noexcept {
        if (stopEvent) {
            SetEvent(stopEvent);
        }
        if (serverThread.joinable()) {
            serverThread.join();
        }
        serverRunning.store(false, std::memory_order_release);
    }
};

enum HarnessPanelId : int {
    IdSrvToggleBtn   = 3801,
    IdSrvStopBtn     = 3802,
    IdAutoStartCheck = 3803,
    IdInspectCheck   = 3804,
    IdModelsCheck    = 3805,
    IdReplaceCheck   = 3806,
    IdDrawCheck      = 3807,
    IdRetouchCheck   = 3808,
    IdSwitchModCheck = 3809,
    IdAllowModEdit   = 3810,
    IdMaxTaskEdit    = 3811,
    IdMaxDayEdit     = 3812,
    IdSavePolicyBtn  = 3813,
    IdClosePolBtn    = 3814
};

struct HarnessPanelContext {
    HarnessPlugin* plugin = nullptr;
    HWND hwnd = nullptr;
    HFONT font = nullptr;

    HWND srvStatusLabel = nullptr, quotaLabel = nullptr;
    HWND autoStartChk = nullptr, inspectChk = nullptr, modelsChk = nullptr;
    HWND replaceChk = nullptr, drawChk = nullptr, retouchChk = nullptr, switchModChk = nullptr;
    HWND allowModEdit = nullptr, maxTaskEdit = nullptr, maxDayEdit = nullptr;

    ~HarnessPanelContext() {
        if (font) DeleteObject(font);
    }

    static std::string joinCsv(const std::vector<std::string>& v) {
        std::string out;
        for (size_t i = 0; i < v.size(); ++i) {
            if (i > 0) out += ", ";
            out += v[i];
        }
        return out;
    }

    void refreshLiveStatus() {
        bool running = plugin->serverRunning.load(std::memory_order_acquire);
        std::string pipeStr = pk::wideToUtf8(plugin->pipeName);
        pk::setWindowTextUtf8(
            srvStatusLabel,
            running ? ("桥接状态：监听中 (" + pipeStr + ")") : "桥接状态：已停止");

        HarnessPolicy snap;
        {
            std::lock_guard lk(plugin->mutex);
            snap = plugin->policy;
        }
        int used = plugin->ledger.usedToday();
        pk::setWindowTextUtf8(
            quotaLabel,
            "今日累计已调用额度：" + std::to_string(used) + " / " + std::to_string(snap.maxPerDay) + " 次 (UTC)");
    }

    HarnessPolicy gatherPolicy() const {
        HarnessPolicy p;
        p.autoStart        = pk::isButtonChecked(autoStartChk);
        p.allowInspect     = pk::isButtonChecked(inspectChk);
        p.allowModels      = pk::isButtonChecked(modelsChk);
        p.allowReplace     = pk::isButtonChecked(replaceChk);
        p.allowDraw        = pk::isButtonChecked(drawChk);
        p.allowRetouch     = pk::isButtonChecked(retouchChk);
        p.allowSwitchModel = pk::isButtonChecked(switchModChk);
        p.maxPerTask       = pk::parseIntField(maxTaskEdit, "单任务最大张数上限", 1, 10000);
        p.maxPerDay        = pk::parseIntField(maxDayEdit, "每日最大调用上限", 1, 10000);

        std::istringstream ss(pk::getWindowTextUtf8(allowModEdit));
        std::string item;
        while (std::getline(ss, item, ',')) {
            std::string clean = pk::trimAscii(item);
            if (!clean.empty()) p.allowedModels.push_back(std::move(clean));
        }
        return p;
    }

    static intptr_t MEMESTUDIO_CALL wndProc(void* ctx, void* rawHwnd, uint32_t msg, uintptr_t wp, intptr_t lp) {
        auto* st = static_cast<HarnessPanelContext*>(ctx);
        HWND w = static_cast<HWND>(rawHwnd);
        if (msg == WM_CREATE) {
            st->hwnd = w;
            pk::PanelBuilder ui(w);
            st->font = pk::createDpiUiFont(ui.dpi().dpi, 14);

            HarnessPolicy cur;
            {
                std::lock_guard lk(st->plugin->mutex);
                cur = st->plugin->policy;
            }

            st->srvStatusLabel = ui.label(L"", 16, 16, 380, 22);
            ui.button(L"启动监听", IdSrvToggleBtn, 402, 12, 80, 28);
            ui.button(L"停止监听", IdSrvStopBtn,   488, 12, 76, 28);

            st->autoStartChk = ui.checkbox(L"启用本插件时自动启动命名管道监听服务", IdAutoStartCheck, 16, 48, 420, 22, cur.autoStart);

            ui.label(L"授予同账户 DeepSeek Harness 的操作权限（默认最小权限）：", 16, 80, 460, 20);
            st->inspectChk   = ui.checkbox(L"读取本地图像尺寸与动图帧数 (memestudio_inspect)", IdInspectCheck, 24, 104, 520, 24, cur.allowInspect);
            st->modelsChk    = ui.checkbox(L"查询当前可用模型列表 (memestudio_models)", IdModelsCheck, 24, 130, 520, 24, cur.allowModels);
            st->replaceChk   = ui.checkbox(L"提交表情角色替换任务 (memestudio_replace，仍需 confirmed=true)", IdReplaceCheck, 24, 156, 520, 24, cur.allowReplace);
            st->drawChk      = ui.checkbox(L"提交提示词生图任务 (memestudio_draw，仍需 confirmed=true)", IdDrawCheck, 24, 182, 520, 24, cur.allowDraw);
            st->retouchChk   = ui.checkbox(L"提交提示词修图与局部精修 (memestudio_retouch，仍需 confirmed=true)", IdRetouchCheck, 24, 208, 520, 24, cur.allowRetouch);
            st->switchModChk = ui.checkbox(L"允许外部调用指定自定义模型名称", IdSwitchModCheck, 24, 234, 520, 24, cur.allowSwitchModel);

            ui.label(L"允许切换的模型白名单（逗号分隔，留空表示不限制具体模型名）：", 16, 268, 520, 20);
            st->allowModEdit = ui.edit(IdAllowModEdit, 16, 290, 548, 26, joinCsv(cur.allowedModels));

            ui.label(L"单次任务额度上限：", 16, 330, 140, 20);
            st->maxTaskEdit = ui.edit(IdMaxTaskEdit, 156, 326, 96, 26, std::to_string(cur.maxPerTask), ES_NUMBER);

            ui.label(L"每日累计额度上限：", 280, 330, 140, 20);
            st->maxDayEdit = ui.edit(IdMaxDayEdit, 420, 326, 144, 26, std::to_string(cur.maxPerDay), ES_NUMBER);

            st->quotaLabel = ui.label(L"", 16, 366, 548, 22);

            ui.button(L"保存权限设置", IdSavePolicyBtn, 346, 402, 118, 34, true);
            ui.button(L"关闭",         IdClosePolBtn,   472, 402, 92,  34, false);

            ui.applyFont(st->font);
            st->refreshLiveStatus();
            return 0;
        }
        if (msg == WM_COMMAND) {
            int id = LOWORD(wp);
            if (id == IdSrvToggleBtn) {
                try {
                    st->plugin->startServer();
                    st->refreshLiveStatus();
                } catch (const std::exception& e) {
                    pk::showPanelError(w, e.what(), L"启动命名管道服务");
                }
                return 0;
            }
            if (id == IdSrvStopBtn) {
                st->plugin->stopServer();
                st->refreshLiveStatus();
                return 0;
            }
            if (id == IdSavePolicyBtn) {
                try {
                    auto nextPol = st->gatherPolicy();
                    st->plugin->savePolicyToDisk(nextPol);
                    if (nextPol.autoStart && !st->plugin->serverRunning.load(std::memory_order_acquire)) {
                        st->plugin->startServer();
                    }
                    DestroyWindow(w);
                } catch (const std::exception& e) {
                    pk::showPanelError(w, e.what(), L"保存权限策略");
                }
                return 0;
            }
            if (id == IdClosePolBtn || id == IDCANCEL) {
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

    static void MEMESTUDIO_CALL onDestroy(void* ctx) { delete static_cast<HarnessPanelContext*>(ctx); }
};

int32_t MEMESTUDIO_CALL abiCreate(const MemeStudioHostV2* host, void** outInst, MemeStudioBytesV2* outErr) {
    if (!host || !outInst || host->abi_version != MEMESTUDIO_PLUGIN_ABI_V2) return -1;
    try {
        *outInst = new HarnessPlugin(host);
        return 0;
    } catch (const std::exception& e) {
        pk::HostClient(host, kPluginId).writeOutBytes(outErr, e.what());
        return -1;
    }
}

void MEMESTUDIO_CALL abiDestroy(void* inst) { delete static_cast<HarnessPlugin*>(inst); }

int32_t MEMESTUDIO_CALL abiInvokeCommand(
    void* rawInst, const char* commandId, const char* argsJson, void* ownerHwnd,
    MemeStudioCancelTokenV2*, MemeStudioBytesV2* outResult, MemeStudioBytesV2* outErr) {
    auto* self = static_cast<HarnessPlugin*>(rawInst);
    if (!self || !commandId) return -1;
    try {
        std::string cmd(commandId);
        Json args = (argsJson && argsJson[0]) ? Json::parse(argsJson) : Json::object();

        if (cmd == "open-panel") {
            auto* pctx = new HarnessPanelContext{self};
            if (self->host.raw()->open_panel(
                    self->host.raw()->host_ctx, "deepseek-harness", ownerHwnd,
                    580, 452, "DeepSeek Harness 权限与配额",
                    HarnessPanelContext::wndProc, pctx, HarnessPanelContext::onDestroy) != 0) {
                delete pctx;
                throw pk::PluginError("无法打开自动化权限面板");
            }
            self->host.writeOutBytes(outResult, R"({"opened":true})");
            return 0;
        }
        if (cmd == "configure") {
            if (args.contains("policy") && args["policy"].is_object()) {
                self->savePolicyToDisk(HarnessPlugin::policyFromJson(args["policy"]));
            }
            if (args.value("start_server", false)) {
                std::wstring customPipe;
                if (args.contains("pipe_name") && args["pipe_name"].is_string()) {
                    customPipe = pk::utf8ToWideStrict(args["pipe_name"].get<std::string>());
                }
                self->startServer(std::move(customPipe));
            }
        } else if (cmd == "stop-server") {
            self->stopServer();
        } else if (cmd == "rpc") {
            Json rpcRes = self->handleRpcRequest(args);
            self->host.writeOutBytes(outResult, Json{{"ok", true}, {"result", std::move(rpcRes)}}.dump());
            return 0;
        } else if (cmd != "status") {
            throw pk::PluginError("未知的自动化桥接命令：" + cmd);
        }

        HarnessPolicy snap;
        {
            std::lock_guard lock(self->mutex);
            snap = self->policy;
        }
        Json statusResp{
            {"running", self->serverRunning.load(std::memory_order_acquire)},
            {"pipe_name", pk::wideToUtf8(self->pipeName)},
            {"used_today", self->ledger.usedToday()},
            {"policy", HarnessPlugin::policyToJson(snap)}
        };
        self->host.writeOutBytes(outResult, statusResp.dump());
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
    nullptr,
    nullptr
};

} // namespace

extern "C" MEMESTUDIO_EXPORT const MemeStudioPluginV2* MEMESTUDIO_CALL MemeStudioGetPluginV2(void) {
    return &kDescriptor;
}
