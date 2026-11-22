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

        // 0. Verify GUI panel populates full channel pool + comparison controls (never blank!)
        MemeStudioBytesV2 guiRes{}, guiErr{};
        if (plugin.desc->invoke_command(plugin.instance, "open-panel", "{}", nullptr, nullptr, &guiRes, &guiErr) != 0 ||
            env.lastOpenedPanelChildCount < 22) {
            std::cerr << "api-load-balancing panel blank! children=" << env.lastOpenedPanelChildCount << "\n";
            return 1;
        }
        helper.consumeBytesToString(guiRes);

        static constexpr uint8_t kPng[] = {0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A};
        std::string b64Png = pk::base64Encode(kPng);

        // 1. When disabled, intercept_image_api returns 1 (pass-through)
        MemeStudioBytesV2 outResp{};
        MemeStudioBytesV2 outErr{};
        if (plugin.desc->intercept_image_api(plugin.instance, "generate", "{}", "{}", nullptr, 0, nullptr, nullptr, nullptr, &outResp, &outErr) != 1) {
            std::cerr << "Expected pass-through (1) when takeover is disabled\n";
            return 1;
        }

        // 2. Configure 2 channels: Primary (fails with HTTP 500!) + Backup (succeeds!)
        //    Tests BUG-22 fixes: HTTP 500 failover AND cross-request persistent cooldown memory!
        int primaryHits = 0;
        int backupHits = 0;
        env.httpHandler = [&](const Json& spec, std::string_view) -> meme::test_harness::MockHttpExchange {
            std::string url = spec.at("url").get<std::string>();
            if (url.find("primary.example.com") != std::string::npos) {
                ++primaryHits;
                return {500, false, R"({"error":{"message":"Internal upstream crash"}})", {}};
            }
            ++backupHits;
            return {200, false, Json{{"data", Json::array({{{"b64_json", b64Png}}})}}.dump(), {}};
        };

        Json cfg{
            {"enabled", true},
            {"strategy", "weighted"},
            {"server_failover", true},
            {"cooldown_seconds", 120},
            {"channels", Json::array({
                {{"name", "Primary-500"}, {"base_url", "https://primary.example.com/v1"}, {"api_key", "sk-p"}, {"models", {"gpt-image-1"}}, {"priority", 0}},
                {{"name", "Backup-OK"}, {"base_url", "https://backup.example.com/v1"}, {"api_key", "sk-b"}, {"models", {"gpt-image-1"}}, {"priority", 1}}
            })}
        };
        if (plugin.desc->invoke_command(plugin.instance, "configure", cfg.dump().c_str(), nullptr, nullptr, &outResp, &outErr) != 0) {
            std::cerr << "Configure failed: " << helper.consumeBytesToString(outErr) << "\n";
            return 1;
        }
        helper.consumeBytesToString(outResp);

        // First request: hits Primary (gets 500 -> enters 120s cooldown) and fails over to Backup!
        int32_t rc1 = plugin.desc->intercept_image_api(
            plugin.instance, "generate", R"({"model":"gpt-image-1"})", R"({"prompt":"hello"})",
            nullptr, 0, nullptr, nullptr, nullptr, &outResp, &outErr);
        helper.consumeBytesToVec(outResp);
        if (rc1 != 0 || primaryHits != 1 || backupHits != 1) {
            std::cerr << "Expected HTTP 500 failover to backup channel\n";
            return 1;
        }

        // Second request (separate task!): Primary MUST remain in cooldown (primaryHits stays 1, backupHits becomes 2)!
        int32_t rc2 = plugin.desc->intercept_image_api(
            plugin.instance, "generate", R"({"model":"gpt-image-1"})", R"({"prompt":"second job"})",
            nullptr, 0, nullptr, nullptr, nullptr, &outResp, &outErr);
        helper.consumeBytesToVec(outResp);
        if (rc2 != 0 || primaryHits != 1 || backupHits != 2) {
            std::cerr << "Expected persistent cross-job cooldown memory! primaryHits=" << primaryHits << "\n";
            return 1;
        }

        // 3. BUG-23 Regression check: Multi-channel comparison isolates Primary failure without aborting Backup,
        //    and can be invoked repeatedly!
        Json cmpArgs{{"prompt", "Compare channels"}};
        if (plugin.desc->invoke_command(plugin.instance, "compare", cmpArgs.dump().c_str(), nullptr, nullptr, &outResp, &outErr) != 0) {
            std::cerr << "Compare command failed: " << helper.consumeBytesToString(outErr) << "\n";
            return 1;
        }
        Json cmpReport = Json::parse(helper.consumeBytesToString(outResp));
        if (cmpReport.at("results").size() != 2 ||
            cmpReport["results"][0]["status"] != "failed" ||
            cmpReport["results"][1]["status"] != "succeeded") {
            std::cerr << "Expected fault-isolated comparison results: " << cmpReport.dump() << "\n";
            return 1;
        }

        std::cout << "memestudio.api-load-balancing persistent router & isolated comparison tests passed.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] " << e.what() << "\n";
        return 1;
    }
}
