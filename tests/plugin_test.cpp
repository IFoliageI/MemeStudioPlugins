#include "memestudio_plugin_test_harness.hpp"
#include <wincodec.h>
#include <iostream>

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        std::cerr << "Usage: PluginTests <plugin_stage_dir> <mock_waifu_exe>\n";
        return 1;
    }
    HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    try {
        using Json = nlohmann::json;
        namespace pk = meme::plugin_kit;

        meme::test_harness::MockPluginHostEnv env;
        // Override encode_media & decode_media with real WIC dimensions so 2x upscale dimensions verify strictly!
        env.table.encode_media = [](
            void*, const char* pathOrNull, const char*, const MemeStudioImageBufferV2* frames, size_t count,
            MemeStudioBytesV2*, MemeStudioBytesV2*) -> int32_t {
            if (!pathOrNull || !frames || !count) return -1;
            Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
            CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
            Microsoft::WRL::ComPtr<IWICBitmap> bmp;
            factory->CreateBitmapFromMemory(
                frames[0].width, frames[0].height, GUID_WICPixelFormat32bppBGRA,
                frames[0].stride, frames[0].stride * frames[0].height, frames[0].bgra_pixels, &bmp);
            Microsoft::WRL::ComPtr<IWICStream> stream;
            factory->CreateStream(&stream);
            stream->InitializeFromFilename(pk::utf8ToWideStrict(pathOrNull).c_str(), GENERIC_WRITE);
            Microsoft::WRL::ComPtr<IWICBitmapEncoder> enc;
            factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
            enc->Initialize(stream.Get(), WICBitmapEncoderNoCache);
            Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> f;
            enc->CreateNewFrame(&f, nullptr);
            f->Initialize(nullptr);
            f->SetSize(frames[0].width, frames[0].height);
            WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
            f->SetPixelFormat(&fmt);
            f->WriteSource(bmp.Get(), nullptr);
            f->Commit();
            enc->Commit();
            return 0;
        };
        env.table.decode_media = [](
            void*, const char* pathUtf8, const uint8_t*, size_t,
            MemeStudioBytesV2*, MemeStudioImageBufferV2** outFrames, size_t* outCount, MemeStudioBytesV2*) -> int32_t {
            Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
            CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
            Microsoft::WRL::ComPtr<IWICBitmapDecoder> dec;
            if (FAILED(factory->CreateDecoderFromFilename(pk::utf8ToWideStrict(pathUtf8).c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) return -1;
            Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> f0;
            dec->GetFrame(0, &f0);
            UINT w = 0, h = 0;
            f0->GetSize(&w, &h);
            Microsoft::WRL::ComPtr<IWICFormatConverter> conv;
            factory->CreateFormatConverter(&conv);
            conv->Initialize(f0.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
            auto* arr = static_cast<MemeStudioImageBufferV2*>(std::calloc(1, sizeof(MemeStudioImageBufferV2)));
            auto* px = static_cast<uint8_t*>(std::malloc(size_t(w) * h * 4));
            conv->CopyPixels(nullptr, w * 4, w * h * 4, px);
            arr[0] = MemeStudioImageBufferV2{w, h, w * 4, 100, px};
            *outFrames = arr;
            *outCount = 1;
            return 0;
        };

        meme::test_harness::LoadedPluginUnderTest plugin(argv[1], env);
        pk::HostClient helper(&env.table, plugin.desc->id);

        pk::fs::path mockExe = argv[2];
        pk::fs::path modelsDir = env.sandboxDir / L"models-cunet";
        pk::fs::create_directories(modelsDir);
        static constexpr uint8_t kDummy[] = {'o', 'k'};
        pk::writeFileAtomic(modelsDir / L"scale2.0x_model.param", kDummy);
        pk::writeFileAtomic(modelsDir / L"scale2.0x_model.bin", kDummy);

        // 1. Regression check: Non-existent exe path MUST be rejected even when input/output toggles are false!
        MemeStudioBytesV2 outRes{};
        MemeStudioBytesV2 outErr{};
        Json badCfg{{"executable", "D:/nonexistent/waifu2x.exe"}, {"input", false}, {"output", false}};
        if (plugin.desc->invoke_command(plugin.instance, "configure", badCfg.dump().c_str(), nullptr, nullptr, &outRes, &outErr) == 0) {
            std::cerr << "Expected non-existent executable path to fail validation even when disabled\n";
            return 1;
        }
        helper.consumeBytesToString(outRes);
        helper.consumeBytesToString(outErr);

        // 2. Configure valid 2x output upscale and run batch of 3 frames — verify SINGLE subprocess invocation!
        Json validCfg{
            {"executable", pk::wideToUtf8(mockExe.wstring())},
            {"models", pk::wideToUtf8(modelsDir.wstring())},
            {"input", false},
            {"output", true},
            {"scale", 2},
            {"noise", 0}
        };
        if (plugin.desc->invoke_command(plugin.instance, "configure", validCfg.dump().c_str(), nullptr, nullptr, &outRes, &outErr) != 0) {
            std::cerr << "Configure failed: " << helper.consumeBytesToString(outErr) << "\n";
            return 1;
        }
        helper.consumeBytesToString(outRes);

        std::vector<MemeStudioImageBufferV2> batch(3);
        for (auto& f : batch) {
            f.width = 16; f.height = 12; f.stride = 64; f.delay_ms = 80;
            f.bgra_pixels = static_cast<uint8_t*>(std::calloc(16 * 12 * 4, 1));
        }

        if (plugin.desc->process_frames_batch(plugin.instance, "output", "{}", batch.data(), batch.size(), nullptr, &outErr) != 0) {
            std::cerr << "Batch upscale failed: " << helper.consumeBytesToString(outErr) << "\n";
            return 1;
        }
        for (auto& f : batch) {
            if (f.width != 32 || f.height != 24) {
                std::cerr << "Unexpected upscaled dimensions: " << f.width << "x" << f.height << "\n";
                return 1;
            }
            std::free(f.bgra_pixels);
        }

        // Verify mock-waifu was spawned ONLY ONCE for all 3 frames!
        pk::fs::path countFile = helper.dataDir() / L"temp" / L"invocations.count";
        auto countBytes = pk::readFileBytes(countFile);
        if (std::stoi(std::string(countBytes.begin(), countBytes.end())) != 1) {
            std::cerr << "Expected single batch invocation for 3 frames!\n";
            return 1;
        }

        // 3. BUG-20 Regression check: Non-zero exit MUST include captured stderr text in error message!
        Json failCfg = validCfg;
        failCfg["noise"] = 3; // Triggers simulated OOM stderr in mock-waifu
        plugin.desc->invoke_command(plugin.instance, "configure", failCfg.dump().c_str(), nullptr, nullptr, &outRes, &outErr);
        helper.consumeBytesToString(outRes);

        MemeStudioImageBufferV2 oneFrame{16, 16, 64, 100, static_cast<uint8_t*>(std::calloc(16 * 16 * 4, 1))};
        int32_t failRc = plugin.desc->process_frames_batch(plugin.instance, "output", "{}", &oneFrame, 1, nullptr, &outErr);
        std::free(oneFrame.bgra_pixels);
        std::string errMsg = helper.consumeBytesToString(outErr);
        if (failRc == 0 || errMsg.find("simulated mock-waifu failure") == std::string::npos) {
            std::cerr << "Expected stderr tail in error message, got: " << errMsg << "\n";
            return 1;
        }

        std::cout << "memestudio.waifu2x batch upscale & stderr capture tests passed.\n";
        if (SUCCEEDED(comHr)) CoUninitialize();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] " << e.what() << "\n";
        if (SUCCEEDED(comHr)) CoUninitialize();
        return 1;
    }
}
