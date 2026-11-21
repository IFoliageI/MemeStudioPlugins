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

        static constexpr uint8_t kPng[] = {
            0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A,
            0x00,0x00,0x00,0x0D,'I','H','D','R',0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x08,0x06,0x00,0x00,0x00,0x1F,0x15,0xC4,0x89,
            0x00,0x00,0x00,0x00,'I','E','N','D',0xAE,0x42,0x60,0x82
        };
        std::string b64Png = pk::base64Encode(kPng);

        // Mock compatible gateway that REJECTS `image[]` with HTTP 400 and ONLY accepts `image`!
        // Also use a deliberately short API key `"id"` to test BUG-12 JSON redaction safety!
        env.httpHandler = [&](const Json&, std::string_view body) {
            if (body.find("name=\"image[]\"") != std::string_view::npos) {
                return meme::test_harness::MockHttpExchange{
                    400, false, R"({"error":{"message":"Unsupported field image[] for key id","param":"image[]"}})", {}
                };
            }
            return meme::test_harness::MockHttpExchange{
                200, false, Json{{"data", Json::array({{{"b64_json", b64Png}}})}}.dump(), {}
            };
        };

        // BUG-06 + BUG-12 Regression verification:
        // Starting with default image_field="image[]", item 0 returns 400, item 1 probes "image", succeeds,
        // adapts "image" for all remaining 5 parameter cases, AND short key "id" does NOT corrupt JSON keys!
        MemeStudioBytesV2 outRes{};
        MemeStudioBytesV2 outErr{};
        Json probeArgs{
            {"generation_mode", false},
            {"provider", {
                {"base_url", "https://compat.example.com/v1"},
                {"api_key", "id"},
                {"model", "gpt-image-1"},
                {"image_field", "image[]"}
            }}
        };

        int32_t rc = plugin.desc->invoke_command(
            plugin.instance, "probe", probeArgs.dump().c_str(), nullptr, nullptr, &outRes, &outErr);
        std::string errMsg = helper.consumeBytesToString(outErr);
        if (rc != 0) {
            std::cerr << "Probe command failed: " << errMsg << "\n";
            return 1;
        }

        Json report = Json::parse(helper.consumeBytesToString(outRes));
        if (report.at("recommended_image_field") != "image") {
            std::cerr << "Expected adaptive discovery to recommend image_field='image', got: " << report.dump() << "\n";
            return 1;
        }
        const auto& rows = report.at("results");
        if (rows.size() != 7) {
            std::cerr << "Expected all 7 matrix cases to run after adaptive image_field recovery, got " << rows.size() << "\n";
            return 1;
        }
        // Check short key "id" was redacted inside detail string WITHOUT corrupting row["id"] JSON key!
        if (!rows[0].contains("id") || rows[0]["detail"].get<std::string>().find("[REDACTED]") == std::string::npos) {
            std::cerr << "Short key structured redaction check failed: " << rows[0].dump() << "\n";
            return 1;
        }
        // Check format_webp + stream fallback diagnostics don't clobber each other:
        // Case 5 requested output_format=webp, received PNG -> outcome=="ignored"
        if (rows[5]["outcome"] != "ignored") {
            std::cerr << "Expected format mismatch to report outcome='ignored', got: " << rows[5].dump() << "\n";
            return 1;
        }

        std::cout << "memestudio.api-parameter-test adaptive baseline & redaction tests passed.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] " << e.what() << "\n";
        return 1;
    }
}
