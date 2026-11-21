#include "memestudio_plugin_kit.hpp"
#include "../vendor/nlohmann/json.hpp"
#include <sddl.h>
#include <atomic>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

using Json = nlohmann::json;
namespace pk = meme::plugin_kit;

constexpr const char* kPluginId = "memestudio.antigravity-harness";
constexpr const wchar_t* kDefaultPipeName = L"\\\\.\\pipe\\MemeConvertStudio.DeepSeekHarness.v1";

struct HarnessPolicy {
    bool allowInspect = true;
    bool allowModels = false;
    bool allowReplace = false;
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

// Self-healing UTC quota ledger (fixes BUG-24 where clock rollback or corrupt ledger permanently bricked requests!)
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
            // Self-heal cleanly on new UTC day OR system clock rollback!
            if (recorded != today) {
                return 0;
            }
            int u = j.value("used", 0);
            return std::max(0, u);
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
            {"allow_inspect", p.allowInspect},
            {"allow_models", p.allowModels},
            {"allow_replace", p.allowReplace},
            {"allow_switch_model", p.allowSwitchModel},
            {"allowed_models", p.allowedModels},
            {"max_per_task", p.maxPerTask},
            {"max_per_day", p.maxPerDay}
        };
    }

    static HarnessPolicy policyFromJson(const Json& j) {
        HarnessPolicy p;
        p.allowInspect = j.value("allow_inspect", true);
        p.allowModels = j.value("allow_models", false);
        p.allowReplace = j.value("allow_replace", false);
        p.allowSwitchModel = j.value("allow_switch_model", false);
        p.maxPerTask = std::clamp(j.value("max_per_task", 8), 1, 10000);
        p.maxPerDay = std::clamp(j.value("max_per_day", 32), 1, 10000);
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
            MemeStudioBytesV2 metaBytes{};
            MemeStudioBytesV2 errBytes{};
            if (host.raw()->probe_media(host.raw()->host_ctx, pathUtf8.c_str(), &metaBytes, &errBytes) != 0) {
                throw pk::PluginError(host.consumeBytesToString(errBytes));
            }
            return Json::parse(host.consumeBytesToString(metaBytes));
        }

        if (op == "replace") {
            if (!snap.allowReplace) {
                throw pk::PluginError("当前权限策略未开启图像角色替换授权");
            }
            if (!args.value("confirmed", false)) {
                throw pk::PluginError("生图请求必须显式传入 confirmed=true 确认授权");
            }

            int count = std::clamp(args.value("count", 1), 1, 10);
            ledger.checkCanSpend(count, snap);

            std::string requestedModel = pk::trimAscii(args.value("model", std::string()));
            if (!requestedModel.empty()) {
                if (!snap.allowSwitchModel) {
                    throw pk::PluginError("当前权限策略禁止外部调用覆盖模型");
                }
                if (!snap.allowedModels.empty() &&
                    std::find(snap.allowedModels.begin(), snap.allowedModels.end(), requestedModel) == snap.allowedModels.end()) {
                    throw pk::PluginError("模型不在允许白名单内：" + requestedModel);
                }
            }

            pk::fs::path outParent = args.contains("output_parent") && args["output_parent"].is_string()
                ? pk::fs::path(pk::utf8ToWideStrict(args["output_parent"].get<std::string>()))
                : (host.dataDir() / L"runs");
            pk::fs::path runDir = outParent / (L"meme-run-" + pk::utf8ToWideStrict(pk::randomHex(6)));
            pk::fs::create_directories(runDir);
            pk::fs::path outFile = runDir / L"output.png";

            Json jobPayload{
                {"source", args.at("source").get<std::string>()},
                {"character", args.at("character").get<std::string>()},
                {"output", pk::wideToUtf8(outFile.wstring())},
                {"instruction", args.value("instruction", std::string())},
                {"count", count}
            };
            if (!requestedModel.empty()) jobPayload["model"] = requestedModel;

            std::string jobDump = jobPayload.dump();
            MemeStudioBytesV2 resBytes{};
            MemeStudioBytesV2 errBytes{};
            int32_t rc = host.raw()->run_core_replace_job(
                host.raw()->host_ctx, jobDump.c_str(), nullptr,
                nullptr, nullptr, &resBytes, &errBytes);
            std::string errMsg = host.consumeBytesToString(errBytes);
            std::string resMsg = host.consumeBytesToString(resBytes);
            if (rc != 0) {
                // Transparent error reporting — NEVER swallow downstream cause! (Fixes BUG-24)
                throw pk::PluginError(errMsg.empty() ? "执行替换任务失败" : errMsg);
            }

            // Deduct quota ONLY after successful execution! (Fixes BUG-24 pre-send charge)
            int updatedUsed = ledger.recordSpend(count);
            Json parsedRes = resMsg.empty() ? Json::object() : Json::parse(resMsg);
            parsedRes["used_today"] = updatedUsed;
            return parsedRes;
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
            bool signaledBind = false;
            auto signalBindOnce = [&]() {
                if (!signaledBind && bindDoneEvent) {
                    signaledBind = true;
                    SetEvent(bindDoneEvent);
                }
            };

            PSECURITY_DESCRIPTOR sd = nullptr;
            if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr)) {
                signalBindOnce();
                return;
            }
            struct SdGuard { PSECURITY_DESCRIPTOR p; ~SdGuard() { if (p) LocalFree(p); } } sdGuard{sd};

            HANDLE ioEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!ioEvent) {
                signalBindOnce();
                return;
            }
            struct EventGuard { HANDLE h; ~EventGuard() { CloseHandle(h); } } evGuard{ioEvent};

            bool firstIteration = true;
            while (WaitForSingleObject(stopEvent, 0) != WAIT_OBJECT_0) {
                SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), sd, FALSE};
                DWORD openMode = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
                                 (firstIteration ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0);
                firstIteration = false;
                HANDLE pipe = CreateNamedPipeW(
                    targetPipe.c_str(),
                    openMode,
                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                    1,
                    65536,
                    65536,
                    0,
                    &sa);
                if (pipe == INVALID_HANDLE_VALUE) {
                    signalBindOnce();
                    break;
                }
                serverRunning.store(true, std::memory_order_release);
                signalBindOnce();
                struct PipeGuard { HANDLE h; ~PipeGuard() { CloseHandle(h); } } pg{pipe};

                ResetEvent(ioEvent);
                OVERLAPPED ov{};
                ov.hEvent = ioEvent;
                BOOL connected = ConnectNamedPipe(pipe, &ov);
                if (!connected) {
                    DWORD err = GetLastError();
                    if (err == ERROR_IO_PENDING) {
                        HANDLE waits[2] = {ioEvent, stopEvent};
                        DWORD wr = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
                        if (wr != WAIT_OBJECT_0) {
                            CancelIoEx(pipe, &ov);
                            break;
                        }
                    } else if (err != ERROR_PIPE_CONNECTED) {
                        continue;
                    }
                }

                // Read newline-delimited JSON request with event-driven Overlapped I/O (no 25ms sleep polling!)
                std::string requestLine;
                bool readOk = false;
                while (requestLine.size() < 65536) {
                    char chunk[4096]{};
                    ResetEvent(ioEvent);
                    OVERLAPPED rov{};
                    rov.hEvent = ioEvent;
                    DWORD got = 0;
                    if (!ReadFile(pipe, chunk, sizeof(chunk), &got, &rov)) {
                        if (GetLastError() != ERROR_IO_PENDING) break;
                        HANDLE waits[2] = {ioEvent, stopEvent};
                        if (WaitForMultipleObjects(2, waits, FALSE, 5000) != WAIT_OBJECT_0) {
                            CancelIoEx(pipe, &rov);
                            break;
                        }
                        if (!GetOverlappedResult(pipe, &rov, &got, FALSE) || got == 0) break;
                    }
                    requestLine.append(chunk, got);
                    auto nl = requestLine.find('\n');
                    if (nl != std::string::npos) {
                        requestLine.resize(nl);
                        readOk = true;
                        break;
                    }
                }

                if (readOk) {
                    Json responseEnvelope;
                    try {
                        Json reqJson = Json::parse(requestLine);
                        Json res = handleRpcRequest(reqJson);
                        responseEnvelope = Json{{"ok", true}, {"result", std::move(res)}};
                    } catch (const std::exception& e) {
                        responseEnvelope = Json{{"ok", false}, {"error", e.what()}};
                    }

                    std::string wire = responseEnvelope.dump() + "\n";
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
                // Immediately disconnect so next client never gets EBUSY! (Removes 2000ms trailing read stall!)
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

struct HarnessPanelContext {
    HarnessPlugin* plugin = nullptr;
    static intptr_t MEMESTUDIO_CALL wndProc(void*, void* rawHwnd, uint32_t msg, uintptr_t wp, intptr_t lp) {
        return DefWindowProcW(static_cast<HWND>(rawHwnd), msg, wp, lp);
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
                    self->host.raw()->host_ctx, "automation-harness", ownerHwnd,
                    560, 480, "本地自动化桥接权限与配额",
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
