#include "memestudio_plugin_test_harness.hpp"
#include <iostream>

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) return 1;
    try {
        using Json = nlohmann::json;
        namespace pk = meme::plugin_kit;

        meme::test_harness::MockPluginHostEnv env;
        meme::test_harness::LoadedPluginUnderTest plugin(argv[1], env);
        pk::HostClient helper(&env.table, plugin.desc->id);

        // 1. Verify 100% AppData isolation (BUG-15 fix): zero config/log files written into plugin stageDir!
        auto modelDir = env.sandboxDir / L"models";
        pk::fs::create_directories(modelDir);

        // Write 1 valid GGUF v3 header + 1 corrupt/partial .gguf in the same directory (BUG-27 fix!)
        std::vector<uint8_t> validGguf{
            'G','G','U','F', 3,0,0,0,
            0,0,0,0,0,0,0,0, // tensorCount = 0
            0,0,0,0,0,0,0,0  // kvCount = 0
        };
        pk::writeFileAtomic(modelDir / L"diffusion_q4.gguf", validGguf);
        std::vector<uint8_t> corruptGguf{'B','R','O','K','E','N'};
        pk::writeFileAtomic(modelDir / L"partial_download.gguf", corruptGguf);

        MemeStudioBytesV2 outRes{};
        MemeStudioBytesV2 outErr{};
        Json inspectArgs{{"model_directory", pk::wideToUtf8(modelDir.wstring())}};
        if (plugin.desc->invoke_command(plugin.instance, "inspect-models", inspectArgs.dump().c_str(), nullptr, nullptr, &outRes, &outErr) != 0) {
            std::cerr << "inspect-models crashed on corrupt sibling GGUF: " << helper.consumeBytesToString(outErr) << "\n";
            return 1;
        }
        Json inspectRes = Json::parse(helper.consumeBytesToString(outRes));
        if (inspectRes.at("diffusion_candidates").size() != 1 || inspectRes.at("skipped_corrupt_files").size() != 1) {
            std::cerr << "Unexpected GGUF scan report: " << inspectRes.dump() << "\n";
            return 1;
        }

        // 2. BUG-27 unified 32px alignment check: 528x528 (16-multiple but NOT 32-multiple) MUST be rejected cleanly!
        Json cfgArgs{
            {"diffusion_model", pk::wideToUtf8((modelDir / L"diffusion_q4.gguf").wstring())},
            {"port", 18199},
            {"session_takeover", true}
        };
        if (plugin.desc->invoke_command(plugin.instance, "configure", cfgArgs.dump().c_str(), nullptr, nullptr, &outRes, &outErr) != 0) {
            std::cerr << "configure failed: " << helper.consumeBytesToString(outErr) << "\n";
            return 1;
        }
        helper.consumeBytesToString(outRes);

        // Verify non-destructive session endpoint override was notified to host (BUG-01 fix!)
        if (env.lastEndpointOverride.is_null() || env.lastEndpointOverride.at("base_url") != "http://127.0.0.1:18199/v1") {
            std::cerr << "Expected non-destructive session endpoint override\n";
            return 1;
        }

        if (plugin.desc->intercept_image_api(
                plugin.instance, "generate", R"({"size":"528x528"})", R"({"prompt":"test"})",
                nullptr, 0, nullptr, nullptr, nullptr, &outRes, &outErr) == 0) {
            std::cerr << "Expected 528x528 non-32-multiple size to fail validation!\n";
            return 1;
        }
        helper.consumeBytesToString(outErr);

        // 3. Verify direct C++ -> sd-server HTTP call on 1024x1024
        static constexpr uint8_t kPng[] = {0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A};
        env.httpHandler = [&](const Json& spec, std::string_view) -> meme::test_harness::MockHttpExchange {
            if (spec.at("url") != "http://127.0.0.1:18199/v1/images/generations") {
                throw pk::PluginError("Unexpected sd-server URL");
            }
            return {200, false, Json{{"data", Json::array({{{"b64_json", pk::base64Encode(kPng)}}})}}.dump(), {}};
        };
        if (plugin.desc->intercept_image_api(
                plugin.instance, "generate", R"({"size":"1024x1024"})", R"({"prompt":"draw cat"})",
                nullptr, 0, nullptr, nullptr, nullptr, &outRes, &outErr) != 0) {
            std::cerr << "Direct sd-server call failed: " << helper.consumeBytesToString(outErr) << "\n";
            return 1;
        }
        helper.consumeBytesToVec(outRes);

        // 4. BUG-15 Regression check: Log file containing mixed GBK bytes MUST NOT return empty string!
        std::string mixedLog = "Vulkan device initialized\n\xB9\xFA\xBC\xCA\nModel ready\n";
        pk::writeFileAtomic(helper.dataDir() / L"sd-server.log", std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(mixedLog.data()), mixedLog.size()));
        if (plugin.desc->invoke_command(plugin.instance, "read-log", "{}", nullptr, nullptr, &outRes, &outErr) != 0) {
            return 1;
        }
        Json logJson = Json::parse(helper.consumeBytesToString(outRes));
        if (logJson.at("log").get<std::string>().find("Vulkan device initialized") == std::string::npos) {
            std::cerr << "Log reader wiped out mixed-encoding log!\n";
            return 1;
        }

        // Confirm plugin installation stageDir stayed 100% read-only clean!
        if (pk::fs::exists(pk::fs::path(argv[1]) / L"antigravity-config.json") ||
            pk::fs::exists(pk::fs::path(argv[1]) / L".env")) {
            std::cerr << "Plugin polluted its installation directory!\n";
            return 1;
        }

        std::cout << "memestudio.antigravity-image native runner & AppData isolation tests passed.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] " << e.what() << "\n";
        return 1;
    }
}
