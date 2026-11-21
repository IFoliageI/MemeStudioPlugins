#include "memestudio_plugin_test_harness.hpp"
#include <iostream>

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::cerr << "Usage: PluginTests <plugin_stage_dir>\n";
        return 1;
    }
    try {
        meme::test_harness::MockPluginHostEnv env;
        meme::test_harness::LoadedPluginUnderTest plugin(argv[1], env);
        meme::plugin_kit::HostClient helper(&env.table, plugin.desc->id);

        // 1. Default prompt transformation appends consistency suffix
        MemeStudioBytesV2 outPrompt{};
        MemeStudioBytesV2 outErr{};
        int32_t rc = plugin.desc->transform_prompt(plugin.instance, "{}", "Base prompt.", &outPrompt, &outErr);
        std::string transformed = helper.consumeBytesToString(outPrompt);
        helper.consumeBytesToString(outErr);
        if (rc != 0 || transformed.find("Base prompt.") == std::string::npos ||
            transformed.find("Maintain consistent character identity") == std::string::npos) {
            std::cerr << "Unexpected default transform output: " << transformed << "\n";
            return 1;
        }

        // 2. Configure custom suffix & persist into isolated AppData plugin dir
        MemeStudioBytesV2 cmdRes{};
        rc = plugin.desc->invoke_command(
            plugin.instance,
            "configure",
            R"({"enabled":true,"suffix":"Custom Studio Tag"})",
            nullptr,
            nullptr,
            &cmdRes,
            &outErr);
        auto resJson = nlohmann::json::parse(helper.consumeBytesToString(cmdRes));
        if (rc != 0 || resJson.at("suffix") != "Custom Studio Tag") {
            std::cerr << "Configure command failed\n";
            return 1;
        }

        // 3. Re-instantiate a SECOND independent plugin instance to verify zero global state collision
        //    and verify persistence round-trip from disk!
        meme::test_harness::LoadedPluginUnderTest secondInstance(argv[1], env);
        rc = secondInstance.desc->transform_prompt(secondInstance.instance, "{}", "Hello.", &outPrompt, &outErr);
        std::string secondOut = helper.consumeBytesToString(outPrompt);
        if (rc != 0 || secondOut != "Hello.\nCustom Studio Tag") {
            std::cerr << "Second instance persistence check failed: " << secondOut << "\n";
            return 1;
        }

        // 4. Verify staged dist directory contains ONLY clean artifacts (.dll, plugin.json, README.md) — zero .obj/.lib/.exp!
        for (const auto& entry : std::filesystem::directory_iterator(argv[1])) {
            auto ext = entry.path().extension().wstring();
            if (ext == L".obj" || ext == L".lib" || ext == L".exp" || ext == L".ilk") {
                std::cerr << "Dirty build artifact leaked into plugin dist folder: " << entry.path().string() << "\n";
                return 1;
            }
        }

        std::cout << "memestudio.example ABI v2 tests passed.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] " << e.what() << "\n";
        return 1;
    }
}
