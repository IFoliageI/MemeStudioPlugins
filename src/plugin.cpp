#include "memestudio_plugin_kit.hpp"
#include "../vendor/nlohmann/json.hpp"
#include <iomanip>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
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

enum BatchPanelId : int {
    IdAddFilesBtn   = 3301,
    IdAddDirBtn     = 3302,
    IdRemItemBtn    = 3303,
    IdClearQueueBtn = 3304,
    IdQueueList     = 3305,
    IdCharEdit      = 3306,
    IdBrowseCharBtn = 3307,
    IdOutDirEdit    = 3308,
    IdBrowseOutBtn  = 3309,
    IdPromptEdit    = 3310,
    IdCountEdit     = 3311,
    IdContinueCheck = 3312,
    IdStartBatchBtn = 3313,
    IdCloseBatchBtn = 3314
};

constexpr UINT WM_BATCH_DONE = WM_APP + 41;

struct BatchPanelContext {
    BatchConvertPlugin* plugin = nullptr;
    HWND hwnd = nullptr;
    HFONT font = nullptr;
    HWND queueLv = nullptr, charEdit = nullptr, outDirEdit = nullptr;
    HWND promptEdit = nullptr, countEdit = nullptr, continueCheck = nullptr;
    HWND statusLabel = nullptr, startBtn = nullptr;

    std::vector<QueuedSourceItem> items;
    std::thread worker;
    bool busy = false;
    std::mutex mtx;
    Json lastReport = Json::object();
    std::string lastErr;

    ~BatchPanelContext() {
        if (worker.joinable()) worker.join();
        if (font) DeleteObject(font);
    }

    void refreshListView() {
        ListView_DeleteAllItems(queueLv);
        for (size_t i = 0; i < items.size(); ++i) {
            std::wstring idxName = std::to_wstring(i + 1) + L". " + items[i].path.filename().wstring();
            LVITEMW lvi{};
            lvi.mask = LVIF_TEXT;
            lvi.iItem = static_cast<int>(i);
            lvi.pszText = idxName.data();
            ListView_InsertItem(queueLv, &lvi);

            std::wstring kind = items[i].animated
                ? (L"GIF (" + std::to_wstring(items[i].frames) + L" 帧)")
                : L"静态图";
            ListView_SetItemText(queueLv, static_cast<int>(i), 1, kind.data());
            std::wstring full = items[i].path.wstring();
            ListView_SetItemText(queueLv, static_cast<int>(i), 2, full.data());
            ListView_SetItemText(queueLv, static_cast<int>(i), 3, const_cast<LPWSTR>(L"待处理"));
        }
        pk::setWindowTextUtf8(statusLabel, "当前队列共 " + std::to_string(items.size()) + " 个待转换素材");
    }

    void importPaths(const std::vector<pk::fs::path>& incoming) {
        std::vector<pk::fs::path> merged;
        for (const auto& ex : items) merged.push_back(ex.path);
        merged.insert(merged.end(), incoming.begin(), incoming.end());

        std::vector<QueuedSourceItem> acc;
        std::vector<SkippedSourceItem> sk;
        plugin->scanInputs(merged, acc, sk);
        items = std::move(acc);
        if (!items.empty() && pk::trimAscii(pk::getWindowTextUtf8(outDirEdit)).empty()) {
            pk::fs::path suggested = items.front().path.parent_path() / L"MemeStudio-BatchOutput";
            pk::setWindowTextUtf8(outDirEdit, pk::wideToUtf8(suggested.wstring()));
        }
        refreshListView();
    }

    static intptr_t MEMESTUDIO_CALL wndProc(void* ctx, void* rawHwnd, uint32_t msg, uintptr_t wp, intptr_t lp) {
        auto* st = static_cast<BatchPanelContext*>(ctx);
        HWND w = static_cast<HWND>(rawHwnd);
        if (msg == WM_CREATE) {
            st->hwnd = w;
            pk::PanelBuilder ui(w);
            st->font = pk::createDpiUiFont(ui.dpi().dpi, 14);

            std::string initChar, initInst;
            MemeStudioBytesV2 wsBytes{};
            if (st->plugin->host.raw()->get_workspace_state(st->plugin->host.raw()->host_ctx, &wsBytes) == 0) {
                try {
                    auto ws = Json::parse(st->plugin->host.consumeBytesToString(wsBytes));
                    initChar = ws.value("character_path", std::string());
                    initInst = ws.value("instruction", std::string());
                } catch (...) {}
            }

            ui.button(L"添加图片…", IdAddFilesBtn, 16, 14, 96, 28);
            ui.button(L"导入文件夹…", IdAddDirBtn, 118, 14, 108, 28);
            ui.button(L"移除选中", IdRemItemBtn, 232, 14, 86, 28);
            ui.button(L"清空队列", IdClearQueueBtn, 324, 14, 86, 28);

            st->queueLv = ui.reportListView(IdQueueList, 16, 50, 788, 250, {
                {L"序号 / 素材名", 190},
                {L"类型", 110},
                {L"完整路径", 340},
                {L"状态", 120}
            });

            ui.label(L"目标人物参考图：", 16, 314, 130, 22);
            st->charEdit = ui.edit(IdCharEdit, 146, 310, 574, 26, initChar);
            ui.button(L"选择…", IdBrowseCharBtn, 726, 310, 78, 26);

            ui.label(L"批量输出文件夹：", 16, 350, 130, 22);
            st->outDirEdit = ui.edit(IdOutDirEdit, 146, 346, 574, 26);
            ui.button(L"浏览…", IdBrowseOutBtn, 726, 346, 78, 26);

            ui.label(L"统一附加要求：", 16, 386, 130, 22);
            st->promptEdit = ui.multilineEdit(IdPromptEdit, 146, 384, 658, 76, initInst, false);

            ui.label(L"每素材生成份数：", 16, 474, 130, 22);
            st->countEdit = ui.edit(IdCountEdit, 146, 470, 70, 26, "1", ES_NUMBER);
            st->continueCheck = ui.checkbox(L"遇到单文件生成异常时自动记录并继续转换后续队列", IdContinueCheck, 236, 472, 420, 24, true);

            st->statusLabel = ui.label(L"当前队列共 0 个待转换素材", 16, 514, 520, 24);
            st->startBtn = ui.button(L"开始批量转换", IdStartBatchBtn, 560, 508, 144, 34, true);
            ui.button(L"关闭", IdCloseBatchBtn, 712, 508, 92, 34, false);

            ui.applyFont(st->font);
            return 0;
        }
        if (msg == WM_COMMAND && !st->busy) {
            int id = LOWORD(wp);
            if (id == IdAddFilesBtn) {
                auto picked = pk::pickMultiplePaths(w, L"选择待批量转换的表情图片或 GIF");
                if (!picked.empty()) st->importPaths(picked);
                return 0;
            }
            if (id == IdAddDirBtn) {
                if (auto dir = pk::pickSinglePath(w, false, true, L"选择源表情所在文件夹")) {
                    st->importPaths({*dir});
                }
                return 0;
            }
            if (id == IdRemItemBtn) {
                int sel = ListView_GetNextItem(st->queueLv, -1, LVNI_SELECTED);
                if (sel >= 0 && static_cast<size_t>(sel) < st->items.size()) {
                    st->items.erase(st->items.begin() + sel);
                    st->refreshListView();
                }
                return 0;
            }
            if (id == IdClearQueueBtn) {
                st->items.clear();
                st->refreshListView();
                return 0;
            }
            if (id == IdBrowseCharBtn) {
                if (auto p = pk::pickSinglePath(w, false, false, L"选择目标人物参考图")) {
                    pk::setWindowTextUtf8(st->charEdit, pk::wideToUtf8(p->wstring()));
                }
                return 0;
            }
            if (id == IdBrowseOutBtn) {
                if (auto d = pk::pickSinglePath(w, false, true, L"选择批量输出文件夹")) {
                    pk::setWindowTextUtf8(st->outDirEdit, pk::wideToUtf8(d->wstring()));
                }
                return 0;
            }
            if (id == IdStartBatchBtn) {
                try {
                    if (st->items.empty()) throw pk::PluginError("请先添加待转换的源表情素材");
                    std::string charStr = pk::trimAscii(pk::getWindowTextUtf8(st->charEdit));
                    std::string outDirStr = pk::trimAscii(pk::getWindowTextUtf8(st->outDirEdit));
                    if (charStr.empty()) throw pk::PluginError("请选择目标人物参考图");
                    if (outDirStr.empty()) throw pk::PluginError("请指定批量输出文件夹");
                    int count = pk::parseIntField(st->countEdit, "每素材生成份数", 1, 10);
                    bool cont = pk::isButtonChecked(st->continueCheck);
                    std::string inst = pk::getWindowTextUtf8(st->promptEdit);

                    Json jobTpl{{"instruction", inst}};
                    MemeStudioBytesV2 wsBytes{};
                    if (st->plugin->host.raw()->get_workspace_state(st->plugin->host.raw()->host_ctx, &wsBytes) == 0) {
                        try {
                            auto ws = Json::parse(st->plugin->host.consumeBytesToString(wsBytes));
                            if (ws.contains("provider") && ws["provider"].is_object()) {
                                for (const auto& [pkK, pkV] : ws["provider"].items()) jobTpl[pkK] = pkV;
                            }
                        } catch (...) {}
                    }

                    Json srcArr = Json::array();
                    for (const auto& it : st->items) srcArr.push_back(pk::wideToUtf8(it.path.wstring()));
                    Json runArgs{
                        {"sources", srcArr},
                        {"character", charStr},
                        {"output_directory", outDirStr},
                        {"count", count},
                        {"continue_on_error", cont},
                        {"job_template", jobTpl}
                    };

                    if (st->worker.joinable()) st->worker.join();
                    st->busy = true;
                    EnableWindow(st->startBtn, FALSE);
                    pk::setWindowTextUtf8(st->statusLabel, "正在后台执行批量转换队列…");

                    st->worker = std::thread([st, runArgs = std::move(runArgs)]() {
                        Json rep;
                        std::string err;
                        try {
                            rep = st->plugin->runBatch(runArgs, nullptr);
                        } catch (const std::exception& e) {
                            err = e.what();
                        }
                        {
                            std::lock_guard lk(st->mtx);
                            st->lastReport = std::move(rep);
                            st->lastErr = std::move(err);
                        }
                        if (st->hwnd && IsWindow(st->hwnd)) PostMessageW(st->hwnd, WM_BATCH_DONE, 0, 0);
                    });
                } catch (const std::exception& e) {
                    pk::showPanelError(w, e.what(), L"批量转换");
                }
                return 0;
            }
            if (id == IdCloseBatchBtn || id == IDCANCEL) {
                DestroyWindow(w);
                return 0;
            }
        }
        if (msg == WM_BATCH_DONE) {
            if (st->worker.joinable()) st->worker.join();
            st->busy = false;
            EnableWindow(st->startBtn, TRUE);
            Json rep;
            std::string err;
            {
                std::lock_guard lk(st->mtx);
                rep = st->lastReport;
                err = st->lastErr;
            }
            if (!err.empty()) {
                pk::setWindowTextUtf8(st->statusLabel, "批量执行失败");
                pk::showPanelError(w, err, L"批量转换失败");
            } else {
                const auto& resItems = rep.value("items", Json::array());
                for (size_t i = 0; i < resItems.size() && i < st->items.size(); ++i) {
                    std::wstring label = resItems[i].value("status", "") == "succeeded" ? L"已完成" : L"失败";
                    ListView_SetItemText(st->queueLv, static_cast<int>(i), 3, label.data());
                }
                std::string summary = "批量完成：成功 " + std::to_string(rep.value("succeeded", 0)) +
                                      " 项，失败 " + std::to_string(rep.value("failed", 0)) + " 项";
                pk::setWindowTextUtf8(st->statusLabel, summary);
                pk::showPanelInfo(w, summary, L"批量转换完成");
            }
            return 0;
        }
        if (msg == WM_CLOSE) {
            if (!st->busy) DestroyWindow(w);
            return 0;
        }
        return DefWindowProcW(w, msg, wp, lp);
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
