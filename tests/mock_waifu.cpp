#include "memestudio_plugin_kit.hpp"
#include <wincodec.h>
#include <iostream>

int wmain(int argc, wchar_t** argv) {
    namespace pk = meme::plugin_kit;
    pk::fs::path inDir;
    pk::fs::path outDir;
    int scale = 2;
    int noise = 0;

    for (int i = 1; i + 1 < argc; ++i) {
        std::wstring flag(argv[i]);
        if (flag == L"-i") inDir = argv[++i];
        else if (flag == L"-o") outDir = argv[++i];
        else if (flag == L"-s") scale = std::stoi(argv[++i]);
        else if (flag == L"-n") noise = std::stoi(argv[++i]);
    }

    if (noise == 3) {
        std::cerr << "Vulkan device out of memory: simulated mock-waifu failure\n";
        return 42;
    }

    if (!pk::fs::is_directory(inDir)) {
        std::cerr << "Input directory missing\n";
        return 2;
    }
    pk::fs::create_directories(outDir);

    pk::fs::path counterFile = inDir.parent_path().parent_path() / L"invocations.count";
    int count = 0;
    if (pk::fs::exists(counterFile)) {
        auto b = pk::readFileBytes(counterFile);
        count = std::stoi(std::string(b.begin(), b.end()));
    }
    std::string nextCount = std::to_string(count + 1);
    pk::writeFileAtomic(counterFile, std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(nextCount.data()), nextCount.size()));

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
            if (SUCCEEDED(hr)) CoUninitialize();
            return 3;
        }

        for (const auto& entry : pk::fs::directory_iterator(inDir)) {
            if (!entry.is_regular_file()) continue;
            Microsoft::WRL::ComPtr<IWICBitmapDecoder> dec;
            if (FAILED(factory->CreateDecoderFromFilename(entry.path().c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) continue;
            Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> f0;
            dec->GetFrame(0, &f0);
            UINT w = 0, h = 0;
            f0->GetSize(&w, &h);

            Microsoft::WRL::ComPtr<IWICBitmapScaler> scaler;
            factory->CreateBitmapScaler(&scaler);
            scaler->Initialize(f0.Get(), w * scale, h * scale, WICBitmapInterpolationModeNearestNeighbor);

            pk::fs::path dst = outDir / entry.path().filename();
            Microsoft::WRL::ComPtr<IWICStream> stream;
            factory->CreateStream(&stream);
            stream->InitializeFromFilename(dst.c_str(), GENERIC_WRITE);
            Microsoft::WRL::ComPtr<IWICBitmapEncoder> enc;
            factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
            enc->Initialize(stream.Get(), WICBitmapEncoderNoCache);
            Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> outFrame;
            enc->CreateNewFrame(&outFrame, nullptr);
            outFrame->Initialize(nullptr);
            outFrame->SetSize(w * scale, h * scale);
            WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
            outFrame->SetPixelFormat(&fmt);
            outFrame->WriteSource(scaler.Get(), nullptr);
            outFrame->Commit();
            enc->Commit();
        }
    }
    if (SUCCEEDED(hr)) CoUninitialize();
    return 0;
}
