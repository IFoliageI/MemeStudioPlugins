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

        // 0. Verify dual-queue Win32 GUI panel & declarative UI schema (WinUI 3 readiness!)
        MemeStudioBytesV2 guiRes{}, guiErr{};
        if (plugin.desc->invoke_command(plugin.instance, "open-panel", "{}", nullptr, nullptr, &guiRes, &guiErr) != 0 ||
            env.lastOpenedPanelChildCount < 20) {
            std::cerr << "batch-convert dual-queue panel blank! children=" << env.lastOpenedPanelChildCount << "\n";
            return 1;
        }
        helper.consumeBytesToString(guiRes);

        if (plugin.desc->invoke_command(plugin.instance, MEMESTUDIO_CMD_UI_SCHEMA, "{}", nullptr, nullptr, &guiRes, &guiErr) != 0) {
            return 1;
        }
        Json uiSchema = Json::parse(helper.consumeBytesToString(guiRes));
        if (uiSchema.at("submit_command") != "run-batch") return 1;

        auto inDir   = env.sandboxDir / L"sources";
        auto charDir = env.sandboxDir / L"characters";
        auto outDir  = env.sandboxDir / L"outputs";
        pk::fs::create_directories(inDir);
        pk::fs::create_directories(charDir);

        static constexpr uint8_t kPng[] = {0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A};
        static constexpr uint8_t kGif[] = {'G','I','F','8','9','a',0x01,0x00,0x01,0x00};
        static constexpr uint8_t kBad[] = {'N','O','T','_','I','M','G'};

        pk::writeFileAtomic(inDir / L"meme_a.png", kPng);
        pk::writeFileAtomic(inDir / L"meme_b.gif", kGif);
        pk::writeFileAtomic(inDir / L"corrupt.png", kBad);

        pk::writeFileAtomic(charDir / L"alice.png", kPng);
        pk::writeFileAtomic(charDir / L"bob.png", kPng);
        pk::writeFileAtomic(charDir / L"carol.png", kPng);

        MemeStudioBytesV2 outRes{};
        MemeStudioBytesV2 outErr{};

        // 1. Mode A: 1 Source * 3 Characters -> 3 distinct outputs!
        env.submittedReplaceJobs.clear();
        Json oneByThreeArgs{
            {"sources", Json::array({pk::wideToUtf8((inDir / L"meme_a.png").wstring())})},
            {"characters", Json::array({pk::wideToUtf8(charDir.wstring())})},
            {"output_directory", pk::wideToUtf8((outDir / L"mode_1x3").wstring())}
        };
        if (plugin.desc->invoke_command(plugin.instance, "run-batch", oneByThreeArgs.dump().c_str(), nullptr, nullptr, &outRes, &outErr) != 0) {
            std::cerr << "1*3 run-batch failed: " << helper.consumeBytesToString(outErr) << "\n";
            return 1;
        }
        Json rep1x3 = Json::parse(helper.consumeBytesToString(outRes));
        if (rep1x3.at("source_count") != 1 || rep1x3.at("character_count") != 3 ||
            rep1x3.at("total") != 3 || rep1x3.at("succeeded") != 3 || env.submittedReplaceJobs.size() != 3) {
            std::cerr << "Unexpected 1*3 report: " << rep1x3.dump() << "\n";
            return 1;
        }

        // 2. Mode B: 2 Valid Sources (1 PNG + 1 GIF, skipping 1 corrupt) * 3 Characters -> 2 * 3 = 6 Cartesian matrix outputs!
        //    Also simulate 1 character ("bob.png") failing to verify fault isolation across X*Y pairs!
        env.submittedReplaceJobs.clear();
        Json twoByThreeArgs{
            {"sources", Json::array({pk::wideToUtf8(inDir.wstring())})},
            {"characters", Json::array({pk::wideToUtf8(charDir.wstring())})},
            {"output_directory", pk::wideToUtf8((outDir / L"mode_2x3").wstring())},
            {"organize_by_character", true},
            {"continue_on_error", true},
            {"_simulate_fail_character", pk::wideToUtf8((charDir / L"bob.png").wstring())}
        };
        if (plugin.desc->invoke_command(plugin.instance, "run-batch", twoByThreeArgs.dump().c_str(), nullptr, nullptr, &outRes, &outErr) != 0) {
            std::cerr << "2*3 run-batch failed: " << helper.consumeBytesToString(outErr) << "\n";
            return 1;
        }
        Json rep2x3 = Json::parse(helper.consumeBytesToString(outRes));
        // 2 sources * 3 characters = 6 pairs; bob fails on 2 pairs -> 4 succeeded, 2 failed!
        if (rep2x3.at("source_count") != 2 || rep2x3.at("character_count") != 3 ||
            rep2x3.at("total") != 6 || rep2x3.at("succeeded") != 4 || rep2x3.at("failed") != 2) {
            std::cerr << "Unexpected 2*3 matrix report: " << rep2x3.dump() << "\n";
            return 1;
        }

        std::cout << "memestudio.batch-convert 1*X and X*Y Cartesian matrix tests passed.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] " << e.what() << "\n";
        return 1;
    }
}
