#include "memestudio_plugin_test_harness.hpp"
#include <iostream>

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) return 1;
    try {
        meme::test_harness::MockPluginHostEnv env;
        meme::test_harness::LoadedPluginUnderTest plugin(argv[1], env);
        meme::plugin_kit::HostClient helper(&env.table, plugin.desc->id);

        // 1. GUI panel creation check: MUST create real Win32 child controls (never blank!)
        MemeStudioBytesV2 cmdRes{};
        MemeStudioBytesV2 outErr{};
        if (plugin.desc->invoke_command(plugin.instance, "configure", "{}", nullptr, nullptr, &cmdRes, &outErr) != 0) {
            std::cerr << "Open configure panel failed: " << helper.consumeBytesToString(outErr) << "\n";
            return 1;
        }
        helper.consumeBytesToString(cmdRes);
        if (env.lastOpenedPanelChildCount < 5) {
            std::cerr << "Panel opened blank! Child control count = " << env.lastOpenedPanelChildCount << "\n";
            return 1;
        }

        // 2. Default prompt transformation appends consistency suffix
        MemeStudioBytesV2 outPrompt{};
        int32_t rc = plugin.desc->transform_prompt(plugin.instance, "{}", "Base prompt.", &outPrompt, &outErr);
        std::string transformed = helper.consumeBytesToString(outPrompt);
        helper.consumeBytesToString(outErr);
        if (rc != 0 || transformed.find("Base prompt.") == std::string::npos ||
            transformed.find("Maintain consistent character identity") == std::string::npos) {
            std::cerr << "Unexpected default transform output: " << transformed << "\n";
            return 1;
        }

        // 3. Configure custom suffix & verify second instance persistence
        rc = plugin.desc->invoke_command(
            plugin.instance, "configure", R"({"enabled":true,"suffix":"Custom Studio Tag"})",
            nullptr, nullptr, &cmdRes, &outErr);
        auto resJson = nlohmann::json::parse(helper.consumeBytesToString(cmdRes));
        if (rc != 0 || resJson.at("suffix") != "Custom Studio Tag") {
            return 1;
        }

        meme::test_harness::LoadedPluginUnderTest secondInstance(argv[1], env);
        rc = secondInstance.desc->transform_prompt(secondInstance.instance, "{}", "Hello.", &outPrompt, &outErr);
        if (rc != 0 || helper.consumeBytesToString(outPrompt) != "Hello.\nCustom Studio Tag") {
            return 1;
        }

        std::cout << "memestudio.example ABI v2 & GUI panel tests passed (children=" << env.lastOpenedPanelChildCount << ").\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] " << e.what() << "\n";
        return 1;
    }
}
