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

        // 0. Verify GUI panel populates all GGUF paths, runtime controls & log viewer (never blank!)
        MemeStudioBytesV2 guiRes{}, guiErr{};
        if (plugin.desc->invoke_command(plugin.instance, "open-panel", "{}", nullptr, nullptr, &guiRes, &guiErr) != 0 ||
            env.lastOpenedPanelChildCount < 25) {
            std::cerr << "local sd-server panel blank! children=" << env.lastOpenedPanelChildCount << "\n";
            return 1;
        }
        helper.consumeBytesToString(guiRes);

        // 1. Verify 100% AppData isolation (BUG-15 fix): zero config/log files written into plugin stageDir!
        auto modelDir = env.sandboxDir / L"models";
        pk::fs::create_directories(modelDir);

        // Write 1 diffusion GGUF + 1 real general.architecture="qwen2vl" encoder GGUF (without "vl" in filename!) + 1 corrupt .gguf
        std::vector<uint8_t> validGguf{
            'G','G','U','F', 3,0,0,0,
            0,0,0,0,0,0,0,0, // tensorCount = 0
            0,0,0,0,0,0,0,0  // kvCount = 0
        };
        pk::writeFileAtomic(modelDir / L"diffusion_q4.gguf", validGguf);

        // GGUF v3 with 1 KV entry: key="general.architecture" (len 20), type=8 (STRING), val="qwen2vl" (len 7)
        std::vector<uint8_t> qwenVlGguf{
            'G','G','U','F', 3,0,0,0,
            0,0,0,0,0,0,0,0, // tensorCount = 0
            1,0,0,0,0,0,0,0, // kvCount = 1
            20,0,0,0,0,0,0,0,
            'g','e','n','e','r','a','l','.','a','r','c','h','i','t','e','c','t','u','r','e',
            8,0,0,0, // GGUF_TYPE_STRING = 8
            7,0,0,0,0,0,0,0,
            'q','w','e','n','2','v','l'
        };
        pk::writeFileAtomic(modelDir / L"encoder_weight.gguf", qwenVlGguf);

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
        if (inspectRes.at("diffusion_candidates").size() != 1 ||
            inspectRes.at("llm_candidates").size() != 1 ||
            inspectRes.at("skipped_corrupt_files").size() != 1) {
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

        // 3. Verify native stable-diffusion.cpp `/sdcpp/v1/img_gen` + `/sdcpp/v1/jobs/<id>` job API with Turbo custom_sigmas & preview!
        Json turboCfg{{"turbo", true}, {"session_takeover", true}};
        plugin.desc->invoke_command(plugin.instance, "configure", turboCfg.dump().c_str(), nullptr, nullptr, &outRes, &outErr);
        helper.consumeBytesToString(outRes);

        static constexpr uint8_t kPng[] = {0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A};
        int pollCalls = 0;
        env.httpHandler = [&](const Json& spec, std::string_view reqBody) -> meme::test_harness::MockHttpExchange {
            std::string url = spec.at("url").get<std::string>();
            if (url == "http://127.0.0.1:18199/sdcpp/v1/img_gen") {
                auto bj = Json::parse(reqBody);
                if (bj.at("sample_params").at("sample_steps") != 8 ||
                    !bj.at("sample_params").contains("custom_sigmas") ||
                    bj.at("ref_images").size() != 1) {
                    throw pk::PluginError("Unexpected /sdcpp/v1/img_gen payload: " + std::string(reqBody));
                }
                return {200, false, R"({"id":"job_101"})", {}};
            }
            if (url == "http://127.0.0.1:18199/sdcpp/v1/jobs/job_101") {
                ++pollCalls;
                std::string b64 = pk::base64Encode(kPng);
                if (pollCalls == 1) {
                    return {200, false, Json{{"status", "running"}, {"preview", {{"b64_json", b64}}}}.dump(), {}};
                }
                return {200, false, Json{
                    {"status", "completed"},
                    {"result", {{"images", Json::array({{{"b64_json", b64}}})}}}
                }.dump(), {}};
            }
            throw pk::PluginError("Unexpected sd-server URL: " + url);
        };

        int previewHits = 0;
        MemeStudioBytesV2 refUpload{const_cast<uint8_t*>(kPng), sizeof(kPng)};
        if (plugin.desc->intercept_image_api(
                plugin.instance, "edit", R"({"size":"1024x1024","stream":true})", R"({"prompt":"replace char"})",
                &refUpload, 1, nullptr,
                [](void* u, const uint8_t*, size_t, int32_t) { ++(*static_cast<int*>(u)); },
                &previewHits, &outRes, &outErr) != 0) {
            std::cerr << "Native /sdcpp/v1/img_gen call failed: " << helper.consumeBytesToString(outErr) << "\n";
            return 1;
        }
        helper.consumeBytesToVec(outRes);
        if (pollCalls != 2 || previewHits != 1) {
            std::cerr << "Expected 2 job polls and 1 preview callback, got polls=" << pollCalls << " previews=" << previewHits << "\n";
            return 1;
        }

        // Verify cold-restart safety: reloading plugin from disk NEVER leaves zombie session_takeover=true when daemon isn't running!
        meme::test_harness::LoadedPluginUnderTest restarted(argv[1], env);
        restarted.desc->invoke_command(restarted.instance, "status", "{}", nullptr, nullptr, &outRes, &outErr);
        Json restartSt = Json::parse(helper.consumeBytesToString(outRes));
        if (restartSt.at("session_takeover") != false) {
            std::cerr << "Expected cold-restart session_takeover to reset to false!\n";
            return 1;
        }

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
        if (pk::fs::exists(pk::fs::path(argv[1]) / L"qwen-config.json") ||
            pk::fs::exists(pk::fs::path(argv[1]) / L".env")) {
            std::cerr << "Plugin polluted its installation directory!\n";
            return 1;
        }

        std::cout << "memestudio.qwen-image native runner, GGUF qwen2vl header & AppData isolation tests passed.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] " << e.what() << "\n";
        return 1;
    }
}
