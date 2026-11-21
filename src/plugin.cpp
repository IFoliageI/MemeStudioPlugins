#include "memestudio_plugin_kit.hpp"
#include "../vendor/nlohmann/json.hpp"
#include <iomanip>
#include <set>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

using Json = nlohmann::json;
namespace pk = meme::plugin_kit;

constexpr const char* kPluginId = "memestudio.batch-convert";

struct QueuedSourceItem {
    pk::fs::path path;
    bool animated = false;
    uint32_t frames = 1;
};

struct SkippedSourceItem {
    pk::fs::path path;
    std::string reason;
};

bool isSupportedImageExtension(const pk::fs::path& p) {
    std::wstring ext = p.extension().wstring();
    for (auto& ch : ext) ch = static_cast<wchar_t>(std::towlower(ch));
    return ext == L".png" || ext == L".jpg" || ext == L".jpeg" ||
           ext == L".webp" || ext == L".bmp" || ext == L".gif";
}

struct BatchConvertPlugin {
    pk::HostClient host;

    explicit BatchConvertPlugin(const MemeStudioHostV2* h) : host(h, kPluginId) {}

    void scanInputs(
        const std::vector<pk::fs::path>& rawInputs,
        std::vector<QueuedSourceItem>& outAccepted,
        std::vector<SkippedSourceItem>& outSkipped) const {
        std::vector<pk::fs::path> candidates;
        std::error_code ec;

        for (const auto& item : rawInputs) {
            if (pk::fs::is_directory(item, ec)) {
                std::vector<pk::fs::path> dirFiles;
                for (const auto& entry : pk::fs::directory_iterator(item, ec)) {
                    if (entry.is_regular_file() && isSupportedImageExtension(entry.path())) {
                        dirFiles.push_back(entry.path());
                    }
                }
                std::sort(dirFiles.begin(), dirFiles.end());
                candidates.insert(candidates.end(), dirFiles.begin(), dirFiles.end());
            } else if (pk::fs::is_regular_file(item, ec)) {
                candidates.push_back(item);
            } else {
                outSkipped.push_back({item, "路径不存在"});
            }
        }

        std::unordered_set<std::wstring> seenKeys;
        for (const auto& path : candidates) {
            std::wstring key = pk::canonicalPathKey(path);
            if (!seenKeys.insert(key).second) continue;

            if (outAccepted.size() >= 500) {
                throw pk::PluginError("单次批量转换最多支持 500 个源表情文件");
            }

            // Fast header-only probe via Host WIC (zero full-frame pixel decoding! Fixes BUG-19)
            MemeStudioBytesV2 metaBytes{};
            MemeStudioBytesV2 errBytes{};
            std::string pathUtf8 = pk::wideToUtf8(path.wstring());
            if (host.raw()->probe_media(host.raw()->host_ctx, pathUtf8.c_str(), &metaBytes, &errBytes) != 0) {
                outSkipped.push_back({path, host.consumeBytesToString(errBytes)});
                continue;
            }
            Json meta = Json::parse(host.consumeBytesToString(metaBytes));
            outAccepted.push_back(QueuedSourceItem{
                path,
                meta.value("animated", false),
                meta.value("frames", 1u)
            });
        }
    }

    Json runBatch(const Json& args, MemeStudioCancelTokenV2* cancel) {
        host.verifyNotCancelled(cancel);
        if (!args.contains("sources") || !args["sources"].is_array() || args["sources"].empty()) {
            throw pk::PluginError("请提供至少一个待转换的源表情文件或文件夹");
        }
        std::vector<pk::fs::path> rawSources;
        for (const auto& s : args["sources"]) {
            rawSources.push_back(pk::utf8ToWideStrict(s.get<std::string>()));
        }

        pk::fs::path outDir = pk::utf8ToWideStrict(args.at("output_directory").get<std::string>());
        if (outDir.empty()) {
            throw pk::PluginError("请指定批量输出文件夹");
        }
        pk::fs::path characterPath = pk::utf8ToWideStrict(args.at("character").get<std::string>());
        if (!pk::fs::is_regular_file(characterPath)) {
            throw pk::PluginError("人物参考图不存在");
        }

        std::vector<pk::fs::path> stylePaths;
        if (args.contains("styles") && args["styles"].is_array()) {
            for (const auto& st : args["styles"]) {
                pk::fs::path sp = pk::utf8ToWideStrict(st.get<std::string>());
                if (!pk::fs::is_regular_file(sp)) throw pk::PluginError("画风参考图不存在");
                stylePaths.push_back(std::move(sp));
            }
        }

        int copyCount = std::clamp(args.value("count", 1), 1, 10);
        bool continueOnError = args.value("continue_on_error", true);

        std::vector<QueuedSourceItem> queue;
        std::vector<SkippedSourceItem> skipped;
        scanInputs(rawSources, queue, skipped);
        if (queue.empty()) {
            throw pk::PluginError("没有找到任何有效且可读的源表情图片");
        }

        // Pre-hash ALL protected input paths ONCE in O(N) — eliminates 2.5M weakly_canonical syscalls!
        std::unordered_set<std::wstring> protectedInputKeys;
        protectedInputKeys.reserve(queue.size() + stylePaths.size() + 2);
        protectedInputKeys.insert(pk::canonicalPathKey(characterPath));
        for (const auto& sp : stylePaths) protectedInputKeys.insert(pk::canonicalPathKey(sp));
        for (const auto& q : queue) protectedInputKeys.insert(pk::canonicalPathKey(q.path));

        pk::fs::create_directories(outDir);

        std::vector<pk::fs::path> plannedBaseOutputs;
        plannedBaseOutputs.reserve(queue.size());
        for (size_t idx = 0; idx < queue.size(); ++idx) {
            std::wostringstream name;
            name << std::setw(3) << std::setfill(L'0') << (idx + 1)
                 << L"_" << queue[idx].path.stem().wstring() << L"-edited"
                 << (queue[idx].animated ? L".gif" : L".png");
            pk::fs::path baseOut = outDir / name.str();

            // Check both single output and numbered _001.._NNN outputs against protected inputs
            if (copyCount == 1) {
                if (protectedInputKeys.contains(pk::canonicalPathKey(baseOut))) {
                    throw pk::PluginError("计划输出文件与输入素材发生路径冲突：" + pk::wideToUtf8(baseOut.wstring()));
                }
            } else {
                for (int c = 1; c <= copyCount; ++c) {
                    std::wostringstream copyName;
                    copyName << baseOut.stem().wstring() << L"_"
                             << std::setw(3) << std::setfill(L'0') << c
                             << baseOut.extension().wstring();
                    pk::fs::path numbered = outDir / copyName.str();
                    if (protectedInputKeys.contains(pk::canonicalPathKey(numbered))) {
                        throw pk::PluginError("计划输出文件与输入素材发生路径冲突：" + pk::wideToUtf8(numbered.wstring()));
                    }
                }
            }
            plannedBaseOutputs.push_back(std::move(baseOut));
        }

        Json results = Json::array();
        int succeededCount = 0;
        int failedCount = 0;

        for (size_t idx = 0; idx < queue.size(); ++idx) {
            host.verifyNotCancelled(cancel);
            Json singleJob = args.value("job_template", Json::object());
            singleJob["source"] = pk::wideToUtf8(queue[idx].path.wstring());
            singleJob["character"] = pk::wideToUtf8(characterPath.wstring());
            singleJob["output"] = pk::wideToUtf8(plannedBaseOutputs[idx].wstring());
            singleJob["count"] = copyCount;
            if (!stylePaths.empty()) {
                Json stArr = Json::array();
                for (const auto& sp : stylePaths) stArr.push_back(pk::wideToUtf8(sp.wstring()));
                singleJob["styles"] = std::move(stArr);
            }
            if (args.contains("fail_sources") && args["fail_sources"].is_array()) {
                std::string curSrc = pk::wideToUtf8(queue[idx].path.filename().wstring());
                for (const auto& fsItem : args["fail_sources"]) {
                    if (fsItem.is_string() && fsItem.get<std::string>() == curSrc) {
                        singleJob["_simulate_fail"] = true;
                    }
                }
            }

            std::string jobDump = singleJob.dump();
            MemeStudioBytesV2 jobRes{};
            MemeStudioBytesV2 jobErr{};
            int32_t rc = host.raw()->run_core_replace_job(
                host.raw()->host_ctx, jobDump.c_str(), cancel,
                nullptr, nullptr, &jobRes, &jobErr);
            std::string errText = host.consumeBytesToString(jobErr);
            std::string resText = host.consumeBytesToString(jobRes);

            if (rc == 0) {
                ++succeededCount;
                Json rj = resText.empty() ? Json::object() : Json::parse(resText);
                results.push_back(Json{
                    {"source", pk::wideToUtf8(queue[idx].path.wstring())},
                    {"status", "succeeded"},
                    {"outputs", rj.value("outputs", Json::array())}
                });
            } else {
                ++failedCount;
                if (rc == -2) throw pk::PluginCancelled();
                if (!continueOnError) {
                    throw pk::PluginError("处理素材失败（" + pk::wideToUtf8(queue[idx].path.filename().wstring()) + "）：" + errText);
                }
                results.push_back(Json{
                    {"source", pk::wideToUtf8(queue[idx].path.wstring())},
                    {"status", "failed"},
                    {"error", errText}
                });
            }
        }

        Json skippedJson = Json::array();
        for (const auto& sk : skipped) {
            skippedJson.push_back(Json{
                {"source", pk::wideToUtf8(sk.path.wstring())},
                {"reason", sk.reason}
            });
        }

        return Json{
            {"succeeded", succeededCount},
            {"failed", failedCount},
            {"skipped", skippedJson},
            {"items", results}
        };
    }
};

struct BatchPanelContext {
    BatchConvertPlugin* plugin = nullptr;
    static intptr_t MEMESTUDIO_CALL wndProc(void*, void* rawHwnd, uint32_t msg, uintptr_t wp, intptr_t lp) {
        return DefWindowProcW(static_cast<HWND>(rawHwnd), msg, wp, lp);
    }
    static void MEMESTUDIO_CALL onDestroy(void* ctx) {
        delete static_cast<BatchPanelContext*>(ctx);
    }
};

int32_t MEMESTUDIO_CALL abiCreate(const MemeStudioHostV2* host, void** outInst, MemeStudioBytesV2* outErr) {
    if (!host || !outInst || host->abi_version != MEMESTUDIO_PLUGIN_ABI_V2) return -1;
    try {
        *outInst = new BatchConvertPlugin(host);
        return 0;
    } catch (const std::exception& e) {
        pk::HostClient(host, kPluginId).writeOutBytes(outErr, e.what());
        return -1;
    }
}

void MEMESTUDIO_CALL abiDestroy(void* inst) {
    delete static_cast<BatchConvertPlugin*>(inst);
}

int32_t MEMESTUDIO_CALL abiInvokeCommand(
    void* rawInst,
    const char* commandId,
    const char* argsJson,
    void* ownerHwnd,
    MemeStudioCancelTokenV2* cancel,
    MemeStudioBytesV2* outResult,
    MemeStudioBytesV2* outErr) {
    auto* self = static_cast<BatchConvertPlugin*>(rawInst);
    if (!self || !commandId) return -1;
    try {
        std::string cmd(commandId);
        Json args = (argsJson && argsJson[0]) ? Json::parse(argsJson) : Json::object();

        if (cmd == "open-panel") {
            auto* pctx = new BatchPanelContext{self};
            if (self->host.raw()->open_panel(
                    self->host.raw()->host_ctx, "batch-convert", ownerHwnd,
                    820, 580, "多源批量转换队列",
                    BatchPanelContext::wndProc, pctx, BatchPanelContext::onDestroy) != 0) {
                delete pctx;
                throw pk::PluginError("无法打开批量转换面板");
            }
            self->host.writeOutBytes(outResult, R"({"opened":true})");
            return 0;
        }

        if (cmd == "inspect-sources") {
            std::vector<pk::fs::path> inputs;
            for (const auto& s : args.value("sources", Json::array())) {
                inputs.push_back(pk::utf8ToWideStrict(s.get<std::string>()));
            }
            std::vector<QueuedSourceItem> accepted;
            std::vector<SkippedSourceItem> skipped;
            self->scanInputs(inputs, accepted, skipped);
            Json accArr = Json::array();
            for (const auto& a : accepted) {
                accArr.push_back(Json{
                    {"path", pk::wideToUtf8(a.path.wstring())},
                    {"animated", a.animated},
                    {"frames", a.frames}
                });
            }
            self->host.writeOutBytes(outResult, Json{{"accepted", accArr}, {"skipped_count", skipped.size()}}.dump());
            return 0;
        }

        if (cmd == "run-batch") {
            Json report = self->runBatch(args, cancel);
            self->host.writeOutBytes(outResult, report.dump());
            return 0;
        }

        throw pk::PluginError("未知的批量转换命令：" + cmd);
    } catch (const std::exception& e) {
        self->host.writeOutBytes(outErr, e.what());
        return -1;
    }
}

const MemeStudioPluginV2 kDescriptor{
    sizeof(MemeStudioPluginV2),
    MEMESTUDIO_PLUGIN_ABI_V2,
    kPluginId,
    abiCreate,
    abiDestroy,
    abiInvokeCommand,
    nullptr,
    nullptr,
    nullptr
};

} // namespace

extern "C" MEMESTUDIO_EXPORT const MemeStudioPluginV2* MEMESTUDIO_CALL MemeStudioGetPluginV2(void) {
    return &kDescriptor;
}
