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

        auto inDir = env.sandboxDir / L"inputs";
        auto outDir = env.sandboxDir / L"outputs";
        auto charFile = env.sandboxDir / L"character.png";
        pk::fs::create_directories(inDir);

        static constexpr uint8_t kPng[] = {0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A};
        static constexpr uint8_t kGif[] = {'G','I','F','8','9','a',0x01,0x00};
        static constexpr uint8_t kCorrupt[] = {'B','A','D','!'};

        pk::writeFileAtomic(charFile, kPng);
        pk::writeFileAtomic(inDir / L"01_first.png", kPng);
        pk::writeFileAtomic(inDir / L"02_second.gif", kGif);
        pk::writeFileAtomic(inDir / L"03_corrupt.png", kCorrupt);
        pk::writeFileAtomic(inDir / L"04_third.png", kPng);

        // 1. Run batch with 1 corrupt file (auto-skipped) and 1 simulated runtime failure (isolated, queue continues!)
        MemeStudioBytesV2 outRes{};
        MemeStudioBytesV2 outErr{};
        Json batchArgs{
            {"sources", Json::array({pk::wideToUtf8(inDir.wstring()), pk::wideToUtf8((inDir / L"01_first.png").wstring())})}, // Includes duplicate to test dedup!
            {"character", pk::wideToUtf8(charFile.wstring())},
            {"output_directory", pk::wideToUtf8(outDir.wstring())},
            {"count", 2},
            {"continue_on_error", true},
            {"fail_sources", Json::array({"02_second.gif"})}
        };

        int32_t rc = plugin.desc->invoke_command(
            plugin.instance, "run-batch", batchArgs.dump().c_str(), nullptr, nullptr, &outRes, &outErr);
        std::string errMsg = helper.consumeBytesToString(outErr);
        if (rc != 0) {
            std::cerr << "Batch execution failed: " << errMsg << "\n";
            return 1;
        }

        Json report = Json::parse(helper.consumeBytesToString(outRes));
        if (report.at("succeeded") != 2 || report.at("failed") != 1 || report.at("skipped").size() != 1) {
            std::cerr << "Unexpected batch summary: " << report.dump() << "\n";
            return 1;
        }

        // 2. Collision protection: reject if a source file already has the planned output filename inside outDir!
        pk::writeFileAtomic(outDir / L"001_sample-edited.png", kPng);
        auto collisionSrc = env.sandboxDir / L"sample.png";
        pk::writeFileAtomic(collisionSrc, kPng);
        Json collisionArgs{
            {"sources", Json::array({pk::wideToUtf8(collisionSrc.wstring()), pk::wideToUtf8((outDir / L"001_sample-edited.png").wstring())})},
            {"character", pk::wideToUtf8(charFile.wstring())},
            {"output_directory", pk::wideToUtf8(outDir.wstring())},
            {"count", 1}
        };
        if (plugin.desc->invoke_command(plugin.instance, "run-batch", collisionArgs.dump().c_str(), nullptr, nullptr, &outRes, &outErr) == 0) {
            std::cerr << "Expected input/output collision to be rejected!\n";
            return 1;
        }
        helper.consumeBytesToString(outRes);
        helper.consumeBytesToString(outErr);

        std::cout << "memestudio.batch-convert standalone queue tests passed.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] " << e.what() << "\n";
        return 1;
    }
}
