#include "memestudio_plugin_kit.hpp"
#include "../vendor/nlohmann/json.hpp"
#include <mutex>
#include <string>

namespace {

using Json = nlohmann::json;
namespace pk = meme::plugin_kit;

constexpr const char* kPluginId = "memestudio.example";

struct ExamplePluginInstance {
    pk::HostClient host;
    mutable std::mutex mutex;
    std::string customSuffix = "Maintain consistent character identity, accessories, and facial proportions.";
    bool enabled = true;

    explicit ExamplePluginInstance(const MemeStudioHostV2* h)
        : host(h, kPluginId) {
        loadConfig();
    }

    pk::fs::path configFile() const {
        return host.dataDir() / L"config.json";
    }

    void loadConfig() {
        try {
            auto path = configFile();
            if (!pk::fs::exists(path)) return;
            auto raw = pk::readFileBytes(path, 64 * 1024);
            auto j = Json::parse(raw.begin(), raw.end());
            customSuffix = j.value("suffix", customSuffix);
            enabled = j.value("enabled", true);
        } catch (...) {}
    }

    void saveConfig() const {
        Json j{
            {"enabled", enabled},
            {"suffix", customSuffix}
        };
        std::string dumped = j.dump(2);
        pk::writeFileAtomic(configFile(), std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(dumped.data()), dumped.size()));
    }
};

enum ExamplePanelId : int {
    IdEnableCheck = 3001,
    IdSuffixEdit  = 3002,
    IdBtnSave     = 3003,
    IdBtnCancel   = 3004
};

struct ExamplePanelState {
    ExamplePluginInstance* plugin = nullptr;
    HWND hwnd = nullptr;
    HFONT font = nullptr;
    HWND enableCheck = nullptr;
    HWND suffixEdit = nullptr;

    static intptr_t MEMESTUDIO_CALL wndProc(void* ctx, void* rawHwnd, uint32_t msg, uintptr_t wp, intptr_t lp) {
        auto* st = static_cast<ExamplePanelState*>(ctx);
        HWND w = static_cast<HWND>(rawHwnd);
        if (msg == WM_CREATE) {
            st->hwnd = w;
            pk::PanelBuilder ui(w);
            st->font = pk::createDpiUiFont(ui.dpi().dpi, 14);

            bool curEnabled = true;
            std::string curSuffix;
            {
                std::lock_guard lock(st->plugin->mutex);
                curEnabled = st->plugin->enabled;
                curSuffix = st->plugin->customSuffix;
            }

            st->enableCheck = ui.checkbox(L"在角色替换请求前自动追加一致性增强提示词", IdEnableCheck, 16, 16, 408, 24, curEnabled);
            ui.label(L"追加提示词内容（UTF-8）：", 16, 48, 300, 20);
            st->suffixEdit = ui.multilineEdit(IdSuffixEdit, 16, 72, 428, 120, curSuffix, false);
            ui.button(L"保存设置", IdBtnSave, 248, 206, 96, 30, true);
            ui.button(L"关闭", IdBtnCancel, 352, 206, 92, 30, false);

            ui.applyFont(st->font);
            return 0;
        }
        if (msg == WM_COMMAND) {
            int id = LOWORD(wp);
            if (id == IdBtnSave) {
                try {
                    std::string next = pk::trimAscii(pk::getWindowTextUtf8(st->suffixEdit));
                    if (next.size() > 2000) throw pk::PluginError("提示词长度不能超过 2000 字符");
                    {
                        std::lock_guard lock(st->plugin->mutex);
                        st->plugin->enabled = pk::isButtonChecked(st->enableCheck);
                        st->plugin->customSuffix = std::move(next);
                        st->plugin->saveConfig();
                    }
                    DestroyWindow(w);
                } catch (const std::exception& e) {
                    pk::showPanelError(w, e.what());
                }
                return 0;
            }
            if (id == IdBtnCancel || id == IDCANCEL) {
                DestroyWindow(w);
                return 0;
            }
        }
        if (msg == WM_CLOSE) {
            DestroyWindow(w);
            return 0;
        }
        if (msg == WM_NCDESTROY) {
            if (st->font) { DeleteObject(st->font); st->font = nullptr; }
        }
        return DefWindowProcW(w, msg, wp, lp);
    }

    static void MEMESTUDIO_CALL onDestroy(void* ctx) {
        delete static_cast<ExamplePanelState*>(ctx);
    }
};

int32_t MEMESTUDIO_CALL pluginCreate(
    const MemeStudioHostV2* host,
    void** outInstance,
    MemeStudioBytesV2* outErr) {
    if (!host || !outInstance || host->abi_version != MEMESTUDIO_PLUGIN_ABI_V2) {
        return -1;
    }
    try {
        auto* inst = new ExamplePluginInstance(host);
        *outInstance = inst;
        inst->host.log(MEMESTUDIO_LOG_INFO, "示例插件实例已初始化");
        return 0;
    } catch (const std::exception& e) {
        pk::HostClient(host, kPluginId).writeOutBytes(outErr, e.what());
        return -1;
    }
}

void MEMESTUDIO_CALL pluginDestroy(void* instance) {
    delete static_cast<ExamplePluginInstance*>(instance);
}

int32_t MEMESTUDIO_CALL pluginInvokeCommand(
    void* rawInst,
    const char* commandId,
    const char* argsJson,
    void* ownerHwnd,
    MemeStudioCancelTokenV2*,
    MemeStudioBytesV2* outResultJson,
    MemeStudioBytesV2* outErr) {
    auto* self = static_cast<ExamplePluginInstance*>(rawInst);
    if (!self || !commandId) return -1;
    try {
        std::string cmd(commandId);
        Json args = (argsJson && argsJson[0]) ? Json::parse(argsJson) : Json::object();

        // When invoked from GUI without JSON overrides, open the interactive Win32 settings panel!
        if (cmd == "configure" && args.empty()) {
            auto* panel = new ExamplePanelState{self};
            if (self->host.raw()->open_panel(
                    self->host.raw()->host_ctx, "example-config", ownerHwnd,
                    460, 252, "示例插件设置",
                    ExamplePanelState::wndProc, panel, ExamplePanelState::onDestroy) != 0) {
                delete panel;
                throw pk::PluginError("无法打开示例插件配置窗口");
            }
            self->host.writeOutBytes(outResultJson, R"({"opened":true})");
            return 0;
        }

        std::lock_guard lock(self->mutex);
        if (cmd == "configure") {
            if (args.contains("suffix") && args["suffix"].is_string()) {
                std::string next = pk::trimAscii(args["suffix"].get<std::string>());
                if (next.size() > 2000) {
                    throw pk::PluginError("自定义后缀长度不能超过 2000 字符");
                }
                self->customSuffix = std::move(next);
            }
            if (args.contains("enabled") && args["enabled"].is_boolean()) {
                self->enabled = args["enabled"].get<bool>();
            }
            self->saveConfig();
        } else if (cmd != "status") {
            throw pk::PluginError("未知的示例插件命令：" + cmd);
        }

        Json resp{
            {"enabled", self->enabled},
            {"suffix", self->customSuffix}
        };
        self->host.writeOutBytes(outResultJson, resp.dump());
        return 0;
    } catch (const std::exception& e) {
        self->host.writeOutBytes(outErr, e.what());
        return -1;
    }
}

int32_t MEMESTUDIO_CALL pluginTransformPrompt(
    void* rawInst,
    const char*,
    const char* promptUtf8,
    MemeStudioBytesV2* outPromptUtf8,
    MemeStudioBytesV2* outErr) {
    auto* self = static_cast<ExamplePluginInstance*>(rawInst);
    if (!self || !promptUtf8) return -1;
    try {
        std::string suffix;
        bool active = false;
        {
            std::lock_guard lock(self->mutex);
            active = self->enabled;
            suffix = self->customSuffix;
        }
        if (!active || suffix.empty()) {
            return 0;
        }
        std::string combined = std::string(promptUtf8) + "\n" + suffix;
        self->host.writeOutBytes(outPromptUtf8, combined);
        return 0;
    } catch (const std::exception& e) {
        self->host.writeOutBytes(outErr, e.what());
        return -1;
    }
}

const MemeStudioPluginV2 kDescriptor{
    sizeof(MemeStudioPluginV2),
    MEMESTUDIO_PLUGIN_ABI_V2,
    kPluginId,
    pluginCreate,
    pluginDestroy,
    pluginInvokeCommand,
    pluginTransformPrompt,
    nullptr,
    nullptr
};

} // namespace

extern "C" MEMESTUDIO_EXPORT const MemeStudioPluginV2* MEMESTUDIO_CALL MemeStudioGetPluginV2(void) {
    return &kDescriptor;
}
