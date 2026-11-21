#pragma once
#include "memestudio_plugin_kit.hpp"
#include "../vendor/nlohmann/json.hpp"
#include <cstdlib>
#include <functional>
#include <iostream>
#include <vector>

namespace meme::test_harness {

namespace fs = std::filesystem;
using Json = nlohmann::json;

struct MockHttpExchange {
    uint32_t status = 200;
    bool streamed = false;
    std::string responseBody;
    std::vector<std::pair<std::string, std::string>> sseEvents;
};

struct TestPanelCreateBinding {
    intptr_t(MEMESTUDIO_CALL* proc)(void*, void*, uint32_t, uintptr_t, intptr_t) = nullptr;
    void* ctx = nullptr;
    void(MEMESTUDIO_CALL* onDestroy)(void*) = nullptr;
};

inline LRESULT CALLBACK testPanelHostWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    auto* b = reinterpret_cast<TestPanelCreateBinding*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!b) return DefWindowProcW(hwnd, msg, wp, lp);
    intptr_t r = b->proc ? b->proc(b->ctx, hwnd, msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
    if (msg == WM_NCDESTROY) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        if (b->onDestroy) b->onDestroy(b->ctx);
    }
    return r;
}

class MockPluginHostEnv {
public:
    fs::path sandboxDir;
    bool cancelled = false;
    Json workspaceState = Json::object();
    Json lastEndpointOverride = nullptr;
    std::string lastPresentedFile;
    int lastOpenedPanelChildCount = 0;
    std::string lastOpenedPanelTitle;
    std::vector<std::string> logs;
    std::vector<Json> submittedReplaceJobs;
    std::function<MockHttpExchange(const Json& reqSpec, std::string_view body)> httpHandler;

    MemeStudioHostV2 table{};

    MockPluginHostEnv() {
        auto suffix = L"mcs-plugin-test-" + plugin_kit::utf8ToWideStrict(plugin_kit::randomHex(8));
        std::error_code ec;
        sandboxDir = fs::temp_directory_path(ec) / suffix;
        if (ec || !fs::create_directories(sandboxDir, ec)) {
            sandboxDir = fs::current_path() / suffix;
            fs::create_directories(sandboxDir);
        }

        table.struct_size = sizeof(MemeStudioHostV2);
        table.abi_version = MEMESTUDIO_PLUGIN_ABI_V2;
        table.host_ctx = this;

        table.alloc = [](void*, size_t sz) -> void* { return sz ? std::malloc(sz) : nullptr; };
        table.free = [](void*, void* p) { if (p) std::free(p); };
        table.free_bytes = [](void*, MemeStudioBytesV2* b) {
            if (b && b->data) {
                std::free(b->data);
                b->data = nullptr;
                b->size = 0;
            }
        };
        table.log = [](void* ctx, int32_t, const char*, const char* msg) {
            auto* self = static_cast<MockPluginHostEnv*>(ctx);
            if (self && msg) self->logs.emplace_back(msg);
        };
        table.is_cancelled = [](const MemeStudioCancelTokenV2* tok) -> int32_t {
            if (!tok) return 0;
            const auto* flag = reinterpret_cast<const bool*>(tok);
            return *flag ? 1 : 0;
        };
        table.get_data_dir = [](void* ctx, const char* pid, MemeStudioBytesV2* outPath) -> int32_t {
            auto* self = static_cast<MockPluginHostEnv*>(ctx);
            fs::path dir = self->sandboxDir / L"data" / plugin_kit::utf8ToWideStrict(pid ? pid : "default");
            fs::create_directories(dir);
            writeBytes(outPath, plugin_kit::wideToUtf8(dir.wstring()));
            return 0;
        };
        table.protect_secret = [](void*, const char* plain, MemeStudioBytesV2* outCipher) -> int32_t {
            std::string p(plain ? plain : "");
            DATA_BLOB in{static_cast<DWORD>(p.size()), reinterpret_cast<BYTE*>(p.data())}, out{};
            if (!CryptProtectData(&in, L"TestSecret", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return -1;
            std::string b64 = plugin_kit::base64Encode(std::span<const uint8_t>(out.pbData, out.cbData));
            LocalFree(out.pbData);
            writeBytes(outCipher, b64);
            return 0;
        };
        table.unprotect_secret = [](void*, const char* cipher, MemeStudioBytesV2* outPlain) -> int32_t {
            auto raw = plugin_kit::base64Decode(cipher ? cipher : "");
            DATA_BLOB in{static_cast<DWORD>(raw.size()), raw.data()}, out{};
            if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return -1;
            std::string s(reinterpret_cast<char*>(out.pbData), out.cbData);
            SecureZeroMemory(out.pbData, out.cbData);
            LocalFree(out.pbData);
            writeBytes(outPlain, s);
            return 0;
        };
        table.http_send = [](
            void* ctx, const char* reqJson, const uint8_t* body, size_t bodyLen,
            MemeStudioCancelTokenV2* cancelTok,
            void (MEMESTUDIO_CALL* onSse)(void*, const char*, const char*), void* user,
            uint32_t* outStatus, int32_t* outStreamed, MemeStudioBytesV2* outResp, MemeStudioBytesV2* outErr) -> int32_t {
            auto* self = static_cast<MockPluginHostEnv*>(ctx);
            if (cancelTok && *reinterpret_cast<const bool*>(cancelTok)) {
                writeBytes(outErr, "任务已取消");
                return -2;
            }
            if (!self->httpHandler) {
                writeBytes(outErr, "未配置模拟 HTTP 处理器");
                return -3;
            }
            try {
                Json spec = Json::parse(reqJson);
                std::string_view bv(reinterpret_cast<const char*>(body ? body : reinterpret_cast<const uint8_t*>("")), body ? bodyLen : 0);
                MockHttpExchange ex = self->httpHandler(spec, bv);
                if (outStatus) *outStatus = ex.status;
                if (outStreamed) *outStreamed = ex.streamed ? 1 : 0;
                if (ex.streamed && onSse) {
                    for (const auto& [et, ed] : ex.sseEvents) {
                        onSse(user, et.c_str(), ed.c_str());
                    }
                }
                writeBytes(outResp, ex.responseBody);
                return 0;
            } catch (const std::exception& e) {
                writeBytes(outErr, e.what());
                return -3;
            }
        };
        table.probe_media = [](void*, const char* pathUtf8, MemeStudioBytesV2* outMeta, MemeStudioBytesV2* outErr) -> int32_t {
            try {
                fs::path p = plugin_kit::utf8ToWideStrict(pathUtf8);
                if (!fs::is_regular_file(p)) throw plugin_kit::PluginError("文件不存在");
                auto bytes = plugin_kit::readFileBytes(p, 16 * 1024 * 1024);
                bool isGif = bytes.size() >= 6 && std::memcmp(bytes.data(), "GIF8", 4) == 0;
                bool isPng = bytes.size() >= 8 && bytes[0] == 137 && bytes[1] == 'P' && bytes[2] == 'N' && bytes[3] == 'G';
                if (!isGif && !isPng) throw plugin_kit::PluginError("无法识别的图像文件头");
                Json meta{{"width", 16}, {"height", 16}, {"frames", isGif ? 2 : 1}, {"animated", isGif}, {"loop", isGif ? Json(0) : Json(nullptr)}};
                writeBytes(outMeta, meta.dump());
                return 0;
            } catch (const std::exception& e) {
                writeBytes(outErr, e.what());
                return -1;
            }
        };
        table.decode_media = [](
            void*, const char*, const uint8_t*, size_t,
            MemeStudioBytesV2* outMeta, MemeStudioImageBufferV2** outFrames, size_t* outCount, MemeStudioBytesV2*) -> int32_t {
            auto* arr = static_cast<MemeStudioImageBufferV2*>(std::calloc(1, sizeof(MemeStudioImageBufferV2)));
            auto* px = static_cast<uint8_t*>(std::calloc(16 * 16 * 4, 1));
            for (size_t i = 0; i < 16 * 16; ++i) {
                px[i * 4 + 0] = 40; px[i * 4 + 1] = 120; px[i * 4 + 2] = 220; px[i * 4 + 3] = 255;
            }
            arr[0] = MemeStudioImageBufferV2{16, 16, 64, 100, px};
            *outFrames = arr;
            *outCount = 1;
            if (outMeta) writeBytes(outMeta, R"({"animated":false,"frames":1,"loop":null})");
            return 0;
        };
        table.free_images = [](void*, MemeStudioImageBufferV2* frames, size_t count) {
            if (!frames) return;
            for (size_t i = 0; i < count; ++i) std::free(frames[i].bgra_pixels);
            std::free(frames);
        };
        table.encode_media = [](
            void*, const char* pathOrNull, const char*, const MemeStudioImageBufferV2* frames, size_t count,
            MemeStudioBytesV2* outEncoded, MemeStudioBytesV2* outErr) -> int32_t {
            if (!frames || count == 0) {
                writeBytes(outErr, "无图像帧");
                return -1;
            }
            static constexpr uint8_t kTinyPng[] = {
                0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A,
                0x00,0x00,0x00,0x0D,'I','H','D','R',0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x08,0x06,0x00,0x00,0x00,0x1F,0x15,0xC4,0x89,
                0x00,0x00,0x00,0x0A,'I','D','A','T',0x78,0x9C,0x63,0x00,0x01,0x00,0x00,0x05,0x00,0x01,0x0D,0x0A,0x2D,0xB4,
                0x00,0x00,0x00,0x00,'I','E','N','D',0xAE,0x42,0x60,0x82
            };
            if (pathOrNull && pathOrNull[0]) {
                plugin_kit::writeFileAtomic(plugin_kit::utf8ToWideStrict(pathOrNull), kTinyPng);
            }
            if (outEncoded) {
                auto* b = static_cast<uint8_t*>(std::malloc(sizeof(kTinyPng)));
                std::memcpy(b, kTinyPng, sizeof(kTinyPng));
                outEncoded->data = b;
                outEncoded->size = sizeof(kTinyPng);
            }
            return 0;
        };
        table.get_workspace_state = [](void* ctx, MemeStudioBytesV2* outJson) -> int32_t {
            auto* self = static_cast<MockPluginHostEnv*>(ctx);
            writeBytes(outJson, self->workspaceState.dump());
            return 0;
        };
        table.set_session_endpoint_override = [](void* ctx, const char* pid, const char* epJson) -> int32_t {
            auto* self = static_cast<MockPluginHostEnv*>(ctx);
            if (!epJson || !epJson[0]) self->lastEndpointOverride = nullptr;
            else {
                self->lastEndpointOverride = Json::parse(epJson);
                self->lastEndpointOverride["_plugin"] = pid ? pid : "";
            }
            return 0;
        };
        table.present_output_file = [](void* ctx, const char* pathUtf8) -> int32_t {
            auto* self = static_cast<MockPluginHostEnv*>(ctx);
            self->lastPresentedFile = pathUtf8 ? pathUtf8 : "";
            return 0;
        };
        table.run_core_replace_job = [](
            void* ctx, const char* jobJson, MemeStudioCancelTokenV2* cancelTok,
            void (MEMESTUDIO_CALL* onProg)(void*, const char*), void* user,
            MemeStudioBytesV2* outRes, MemeStudioBytesV2* outErr) -> int32_t {
            auto* self = static_cast<MockPluginHostEnv*>(ctx);
            if (cancelTok && *reinterpret_cast<const bool*>(cancelTok)) {
                writeBytes(outErr, "任务已取消");
                return -2;
            }
            Json job = Json::parse(jobJson);
            if (job.value("_simulate_fail", false)) {
                writeBytes(outErr, "模拟单文件生成失败");
                return -1;
            }
            self->submittedReplaceJobs.push_back(job);
            std::string outPath = job.value("output", "");
            if (!outPath.empty()) {
                static constexpr uint8_t kPng[] = {0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A};
                plugin_kit::writeFileAtomic(plugin_kit::utf8ToWideStrict(outPath), kPng);
            }
            if (onProg) {
                onProg(user, R"({"completed_frames":1,"total_frames":1,"message":"完成"})");
            }
            writeBytes(outRes, Json{{"outputs", Json::array({outPath})}}.dump());
            return 0;
        };
        // Creates a REAL Win32 hidden panel window, triggers WM_CREATE inside the plugin,
        // counts all child HWNDs via EnumChildWindows, and destroys cleanly!
        table.open_panel = [](
            void* ctx, const char*, void*, int32_t wDip, int32_t hDip, const char* titleUtf8,
            intptr_t(MEMESTUDIO_CALL* wndProc)(void*, void*, uint32_t, uintptr_t, intptr_t),
            void* panelCtx, void(MEMESTUDIO_CALL* onDestroy)(void*)) -> int32_t {
            auto* self = static_cast<MockPluginHostEnv*>(ctx);
            HINSTANCE inst = GetModuleHandleW(nullptr);
            static constexpr const wchar_t* kTestPanelCls = L"MemeTestPanelHostV2";
            WNDCLASSEXW ex{sizeof(WNDCLASSEXW)};
            if (!GetClassInfoExW(inst, kTestPanelCls, &ex)) {
                WNDCLASSEXW wc{};
                wc.cbSize = sizeof(WNDCLASSEXW);
                wc.lpfnWndProc = testPanelHostWndProc;
                wc.hInstance = inst;
                wc.lpszClassName = kTestPanelCls;
                RegisterClassExW(&wc);
            }
            TestPanelCreateBinding binding{wndProc, panelCtx, onDestroy};
            HWND win = CreateWindowExW(
                0, kTestPanelCls, L"TestPanel",
                WS_POPUP | WS_CAPTION,
                0, 0, std::max(300, int(wDip)), std::max(200, int(hDip)),
                nullptr, nullptr, inst, &binding);
            if (!win) {
                if (onDestroy && panelCtx) onDestroy(panelCtx);
                return -1;
            }
            int childCount = 0;
            EnumChildWindows(win, [](HWND, LPARAM lp) -> BOOL {
                ++(*reinterpret_cast<int*>(lp));
                return TRUE;
            }, reinterpret_cast<LPARAM>(&childCount));

            self->lastOpenedPanelChildCount = childCount;
            self->lastOpenedPanelTitle = titleUtf8 ? titleUtf8 : "";
            DestroyWindow(win);
            return 0;
        };
    }

    ~MockPluginHostEnv() {
        std::error_code ec;
        fs::remove_all(sandboxDir, ec);
    }

    static void writeBytes(MemeStudioBytesV2* out, std::string_view s) {
        if (!out) return;
        out->data = nullptr;
        out->size = 0;
        if (s.empty()) return;
        auto* p = static_cast<uint8_t*>(std::malloc(s.size() + 1));
        std::memcpy(p, s.data(), s.size());
        p[s.size()] = 0;
        out->data = p;
        out->size = s.size();
    }
};

struct LoadedPluginUnderTest {
    HMODULE dll = nullptr;
    const MemeStudioPluginV2* desc = nullptr;
    void* instance = nullptr;
    Json manifest;

    LoadedPluginUnderTest(const fs::path& stageDir, MockPluginHostEnv& env) {
        auto manifestBytes = plugin_kit::readFileBytes(stageDir / L"plugin.json", 256 * 1024);
        manifest = Json::parse(manifestBytes.begin(), manifestBytes.end());
        std::string libName = manifest.at("library").get<std::string>();
        fs::path dllPath = stageDir / plugin_kit::utf8ToWideStrict(libName);
        dll = LoadLibraryExW(dllPath.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!dll) throw plugin_kit::PluginError("加载插件 DLL 失败：" + libName);
        auto entry = reinterpret_cast<MemeStudioPluginEntryV2>(GetProcAddress(dll, MEMESTUDIO_PLUGIN_ENTRY_V2));
        if (!entry) throw plugin_kit::PluginError("缺少 MemeStudioGetPluginV2 导出符号");
        desc = entry();
        if (!desc || desc->abi_version != MEMESTUDIO_PLUGIN_ABI_V2 || manifest.at("id").get<std::string>() != desc->id) {
            throw plugin_kit::PluginError("插件 ABI 版本或 ID 与 plugin.json 不一致");
        }
        MemeStudioBytesV2 err{};
        if (desc->create && desc->create(&env.table, &instance, &err) != 0) {
            std::string msg = err.data ? std::string(reinterpret_cast<char*>(err.data), err.size) : "create failed";
            env.table.free_bytes(env.table.host_ctx, &err);
            throw plugin_kit::PluginError("插件实例初始化失败：" + msg);
        }
    }

    ~LoadedPluginUnderTest() {
        if (desc && desc->destroy) desc->destroy(instance);
        if (dll) FreeLibrary(dll);
    }
};

} // namespace meme::test_harness
