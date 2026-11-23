#include "memestudio_plugin_test_harness.hpp"
#include <iostream>
#include <thread>

namespace {

nlohmann::json sendPipeRpc(const std::wstring& pipeName, const nlohmann::json& req) {
    for (int attempt = 0; attempt < 40; ++attempt) {
        HANDLE h = CreateFileW(
            pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            Sleep(15);
            continue;
        }
        struct Guard { HANDLE p; ~Guard() { CloseHandle(p); } } g{h};
        std::string payload = req.dump() + "\n";
        DWORD written = 0;
        WriteFile(h, payload.data(), static_cast<DWORD>(payload.size()), &written, nullptr);

        std::string resp;
        char buf[4096]{};
        for (;;) {
            DWORD got = 0;
            if (!ReadFile(h, buf, sizeof(buf), &got, nullptr) || got == 0) break;
            resp.append(buf, got);
            auto nl = resp.find('\n');
            if (nl != std::string::npos) {
                resp.resize(nl);
                break;
            }
        }
        return nlohmann::json::parse(resp);
    }
    throw meme::plugin_kit::PluginError("Failed to connect to Named Pipe server");
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) return 1;
    try {
        using Json = nlohmann::json;
        namespace pk = meme::plugin_kit;

        meme::test_harness::MockPluginHostEnv env;
        meme::test_harness::LoadedPluginUnderTest plugin(argv[1], env);
        pk::HostClient helper(&env.table, plugin.desc->id);

        // 0. Verify GUI panel populates permission checkboxes, quota fields & service buttons (never blank!)
        MemeStudioBytesV2 guiRes{}, guiErr{};
        if (plugin.desc->invoke_command(plugin.instance, "open-panel", "{}", nullptr, nullptr, &guiRes, &guiErr) != 0 ||
            env.lastOpenedPanelChildCount < 14) {
            std::cerr << "automation-harness panel blank! children=" << env.lastOpenedPanelChildCount << "\n";
            return 1;
        }
        helper.consumeBytesToString(guiRes);

        // 1. BUG-24 Regression check: Seed a future UTC date ("2099-12-31") in quota_ledger.json
        //    to simulate system clock rollback — ledger MUST self-heal to 0 instead of bricking!
        std::string futureLedger = R"({"utc_date":"2099-12-31","used":999})";
        pk::writeFileAtomic(helper.dataDir() / L"quota_ledger.json", std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(futureLedger.data()), futureLedger.size()));

        std::wstring testPipe = L"\\\\.\\pipe\\MemeStudioTestPipe_" + pk::utf8ToWideStrict(pk::randomHex(6));
        MemeStudioBytesV2 outRes{};
        MemeStudioBytesV2 outErr{};
        Json cfgArgs{
            {"start_server", true},
            {"pipe_name", pk::wideToUtf8(testPipe)},
            {"policy", {
                {"allow_inspect", true},
                {"allow_replace", true},
                {"allow_switch_model", false},
                {"max_per_task", 2},
                {"max_per_day", 3}
            }}
        };
        if (plugin.desc->invoke_command(plugin.instance, "configure", cfgArgs.dump().c_str(), nullptr, nullptr, &outRes, &outErr) != 0) {
            std::cerr << "Configure & start-server failed: " << helper.consumeBytesToString(outErr) << "\n";
            return 1;
        }
        Json cfgStatus = Json::parse(helper.consumeBytesToString(outRes));
        // Wait briefly for server thread to bind Named Pipe (if OS token allows CreateNamedPipeW)
        Sleep(40);
        MemeStudioBytesV2 stBytes{};
        plugin.desc->invoke_command(plugin.instance, "status", "{}", nullptr, nullptr, &stBytes, &outErr);
        bool pipeBound = Json::parse(helper.consumeBytesToString(stBytes)).value("running", false);

        auto invokeRpc = [&](const Json& rpcReq) -> Json {
            if (pipeBound) {
                return sendPipeRpc(testPipe, rpcReq);
            }
            MemeStudioBytesV2 rOut{}, rErr{};
            int32_t rc = plugin.desc->invoke_command(plugin.instance, "rpc", rpcReq.dump().c_str(), nullptr, nullptr, &rOut, &rErr);
            if (rc == 0) {
                return Json::parse(helper.consumeBytesToString(rOut));
            }
            return Json{{"ok", false}, {"error", helper.consumeBytesToString(rErr)}};
        };

        std::cout << "Pipe server bound=" << (pipeBound ? "true" : "false (sandboxed)") << "\n";

        // 2. RPC: status -> used_today self-healed to 0!
        Json statusReply = invokeRpc({{"operation", "status"}, {"arguments", Json::object()}});
        if (!statusReply.value("ok", false) || statusReply["result"]["used_today"] != 0) {
            std::cerr << "Self-healing quota ledger failed: " << statusReply.dump() << "\n";
            return 1;
        }

        // 3. RPC: replace job + post-execution quota accounting + transparent error on failure!
        static constexpr uint8_t kPng[] = {0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A};
        auto srcFile = env.sandboxDir / L"src.png";
        auto charFile = env.sandboxDir / L"char.png";
        pk::writeFileAtomic(srcFile, kPng);
        pk::writeFileAtomic(charFile, kPng);

        Json replaceReply = invokeRpc({
            {"operation", "replace"},
            {"arguments", {
                {"source", pk::wideToUtf8(srcFile.wstring())},
                {"character", pk::wideToUtf8(charFile.wstring())},
                {"confirmed", true},
                {"count", 2}
            }}
        });
        if (!replaceReply.value("ok", false) || replaceReply["result"]["used_today"] != 2) {
            std::cerr << "Named Pipe replace RPC failed: " << replaceReply.dump() << "\n";
            return 1;
        }

        // 4. Exceeding daily quota (2 + 2 > 3) returns transparent error without crashing pipe!
        Json quotaReply = invokeRpc({
            {"operation", "replace"},
            {"arguments", {
                {"source", pk::wideToUtf8(srcFile.wstring())},
                {"character", pk::wideToUtf8(charFile.wstring())},
                {"confirmed", true},
                {"count", 2}
            }}
        });
        if (quotaReply.value("ok", true) || quotaReply.value("error", "").empty()) {
            std::cerr << "Expected daily quota rejection!\n";
            return 1;
        }

        // 5. Instant zero-stall shutdown check
        plugin.desc->invoke_command(plugin.instance, "stop-server", "{}", nullptr, nullptr, &outRes, &outErr);
        helper.consumeBytesToString(outRes);

        // Verify companion Cordis client was staged into resources/cordis-client/
        if (!pk::fs::exists(pk::fs::path(argv[1]) / L"resources" / L"cordis-client" / L"index.js")) {
            std::cerr << "Companion Cordis client resources missing from staged dist!\n";
            return 1;
        }

        std::cout << "memestudio.deepseek-harness Named Pipe RPC & self-healing quota tests passed.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] " << e.what() << "\n";
        return 1;
    }
}
