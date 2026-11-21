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
    void*,
    MemeStudioCancelTokenV2*,
    MemeStudioBytesV2* outResultJson,
    MemeStudioBytesV2* outErr) {
    auto* self = static_cast<ExamplePluginInstance*>(rawInst);
    if (!self || !commandId) return -1;
    try {
        std::string cmd(commandId);
        Json args = (argsJson && argsJson[0]) ? Json::parse(argsJson) : Json::object();

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
