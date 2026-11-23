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

struct QueuedMediaItem {
    pk::fs::path path;
    bool animated = false;
    uint32_t frames = 1;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct SkippedMediaItem {
    pk::fs::path path;
    std::string reason;
};

bool hasSupportedImageExtension(const pk::fs::path& p) {
    std::wstring ext = p.extension().wstring();
    for (auto& ch : ext) ch = static_cast<wchar_t>(std::towlower(ch));
    static const std::set<std::wstring> kExts = {
        L".png", L".gif", L".jpg", L".jpeg", L".webp", L".bmp"
    };
    return kExts.contains(ext);
}

std::wstring lowerWide(std::wstring s) {
    for (auto& ch : s) ch = static_cast<wchar_t>(std::towlower(ch));
    return s;
}

std::string shortPathHash(const pk::fs::path& p) {
    std::string utf8 = pk::wideToUtf8(p.wstring());
    return pk::sha256Hex(std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(utf8.data()), utf8.size())).substr(0, 6);
}

struct BatchConvertPlugin {
    pk::HostClient host;

    explicit BatchConvertPlugin(const MemeStudioHostV2* h) : host(h, kPluginId) {}

    // Header-only media scan for either sources or character reference folders/files
    void scanMediaInputs(
        const std::vector<pk::fs::path>& rawInputs,
        std::vector<QueuedMediaItem>& outAccepted,
        std::vector<SkippedMediaItem>& outSkipped,
        bool staticFirstFrameOnly = false) const {
        std::unordered_set<std::wstring> seen;
        std::vector<pk::fs::path> candidates;

        for (const auto& input : rawInputs) {
            std::error_code ec;
            if (pk::fs::is_directory(input, ec)) {
                std::vector<pk::fs::path> dirFiles;
                for (const auto& entry : pk::fs::directory_iterator(input, ec)) {
                    if (entry.is_regular_file(ec) && hasSupportedImageExtension(entry.path())) {
                        dirFiles.push_back(entry.path());
                    }
                }
                std::sort(dirFiles.begin(), dirFiles.end(), [](const pk::fs::path& a, const pk::fs::path& b) {
                    return lowerWide(a.filename().wstring()) < lowerWide(b.filename().wstring());
                });
                candidates.insert(candidates.end(), dirFiles.begin(), dirFiles.end());
            } else {
                candidates.push_back(input);
            }
        }

        for (const auto& candidate : candidates) {
            std::wstring key = pk::canonicalPathKey(candidate);
            if (!seen.insert(key).second) {
                continue;
            }
            std::string pathUtf8 = pk::wideToUtf8(candidate.wstring());
            MemeStudioBytesV2 metaBytes{};
            MemeStudioBytesV2 errBytes{};
            if (host.raw()->probe_media(host.raw()->host_ctx, pathUtf8.c_str(), &metaBytes, &errBytes) != 0) {
                outSkipped.push_back({candidate, host.consumeBytesToString(errBytes)});
                continue;
            }
            auto meta = Json::parse(host.consumeBytesToString(metaBytes));
            outAccepted.push_back(QueuedMediaItem{
                candidate,
                staticFirstFrameOnly ? false : meta.value("animated", false),
                staticFirstFrameOnly ? 1u : meta.value("frames", 1u),
                meta.value("width", 0u),
                meta.value("height", 0u)
            });
        }
    }

    // Supports:
    //  - X sources * 1 character  -> X tasks
    //  - 1 source  * Y characters -> Y tasks
    //  - X sources * Y characters -> X * Y tasks (Cartesian matrix!)
    Json runBatch(const Json& args, MemeStudioCancelTokenV2* cancel) {
        std::vector<pk::fs::path> rawSources;
        for (const auto& s : args.value("sources", Json::array())) {
            rawSources.push_back(pk::utf8ToWideStrict(s.get<std::string>()));
        }
        if (rawSources.empty() && args.contains("source") && args["source"].is_string()) {
            rawSources.push_back(pk::utf8ToWideStrict(args["source"].get<std::string>()));
        }

        std::vector<pk::fs::path> rawCharacters;
        if (args.contains("characters") && args["characters"].is_array()) {
            for (const auto& c : args["characters"]) {
                rawCharacters.push_back(pk::utf8ToWideStrict(c.get<std::string>()));
            }
        }
        if (rawCharacters.empty() && args.contains("character") && args["character"].is_string() &&
            !args["character"].get<std::string>().empty()) {
            rawCharacters.push_back(pk::utf8ToWideStrict(args["character"].get<std::string>()));
        }

        if (rawSources.empty()) {
            throw pk::PluginError("批量队列至少需要 1 张源表情图片或目录");
        }
        if (rawCharacters.empty()) {
            throw pk::PluginError("批量队列至少需要 1 张目标人物参考图或目录");
        }

        pk::fs::path outputDir = pk::utf8ToWideStrict(args.at("output_directory").get<std::string>());
        int copyCount = std::clamp(args.value("count", 1), 1, 10);
        bool continueOnError = args.value("continue_on_error", true);
        bool organizeByCharacter = args.value("organize_by_character", false);
        Json baseJob = args.value("job_template", Json::object());

        std::vector<QueuedMediaItem> sources;
        std::vector<SkippedMediaItem> skippedSources;
        scanMediaInputs(rawSources, sources, skippedSources, false);
        if (sources.empty()) {
            throw pk::PluginError("没有可处理的有效源表情文件");
        }

        std::vector<QueuedMediaItem> characters;
        std::vector<SkippedMediaItem> skippedCharacters;
        scanMediaInputs(rawCharacters, characters, skippedCharacters, true);
        if (characters.empty()) {
            throw pk::PluginError("没有可处理的有效目标人物参考图");
        }

        pk::fs::create_directories(outputDir);

        // Pre-hash ALL input paths (sources + characters + styles) for O(1) anti-overwrite protection!
        std::unordered_set<std::wstring> allInputKeys;
        for (const auto& s : sources) allInputKeys.insert(pk::canonicalPathKey(s.path));
        for (const auto& c : characters) allInputKeys.insert(pk::canonicalPathKey(c.path));
        if (baseJob.contains("styles") && baseJob["styles"].is_array()) {
            for (const auto& st : baseJob["styles"]) {
                allInputKeys.insert(pk::canonicalPathKey(pk::utf8ToWideStrict(st.get<std::string>())));
            }
        }

        // Pre-compute disambiguated stems for sources & characters so X*Y filenames never collide!
        auto makeUniqueStems = [](const std::vector<QueuedMediaItem>& list, const wchar_t* fallbackPrefix) {
            std::vector<std::wstring> stems;
            stems.reserve(list.size());
            std::unordered_set<std::wstring> used;
            for (const auto& it : list) {
                std::wstring st = it.path.stem().wstring();
                if (st.empty()) st = fallbackPrefix;
                if (!used.insert(lowerWide(st)).second) {
                    st += L"_" + pk::utf8ToWideStrict(shortPathHash(it.path));
                    used.insert(lowerWide(st));
                }
                stems.push_back(std::move(st));
            }
            return stems;
        };

        std::vector<std::wstring> srcStems  = makeUniqueStems(sources, L"meme");
        std::vector<std::wstring> charStems = makeUniqueStems(characters, L"char");
        const bool multiCharacter = (characters.size() > 1);

        struct PlannedPair {
            size_t srcIdx = 0;
            size_t charIdx = 0;
            pk::fs::path destPath;
        };
        std::vector<PlannedPair> plan;
        plan.reserve(sources.size() * characters.size());

        std::unordered_set<std::wstring> plannedDestKeys;
        for (size_t si = 0; si < sources.size(); ++si) {
            std::wstring ext = sources[si].animated ? L".gif" : L".png";
            for (size_t ci = 0; ci < characters.size(); ++ci) {
                pk::fs::path targetFolder = organizeByCharacter ? (outputDir / charStems[ci]) : outputDir;
                std::wstring baseName = (!multiCharacter && !organizeByCharacter)
                    ? (srcStems[si] + L"_edited")
                    : (organizeByCharacter ? (srcStems[si] + L"_edited") : (srcStems[si] + L"__" + charStems[ci] + L"_edited"));

                pk::fs::path dest = targetFolder / (baseName + ext);
                std::wstring destKey = pk::canonicalPathKey(dest);
                if (!plannedDestKeys.insert(destKey).second) {
                    dest = targetFolder / (baseName + L"_" + std::to_wstring(si + 1) + L"x" + std::to_wstring(ci + 1) + ext);
                    destKey = pk::canonicalPathKey(dest);
                    plannedDestKeys.insert(destKey);
                }

                // Check collision for single & multi-copy (_001.._NNN)
                if (copyCount == 1) {
                    if (allInputKeys.contains(destKey)) {
                        throw pk::PluginError("批量输出文件与输入图片冲突：" + pk::wideToUtf8(dest.wstring()));
                    }
                } else {
                    for (int c = 1; c <= copyCount; ++c) {
                        std::wostringstream ss;
                        ss << dest.stem().wstring() << L"_" << std::setw(3) << std::setfill(L'0') << c << ext;
                        if (allInputKeys.contains(pk::canonicalPathKey(targetFolder / ss.str()))) {
                            throw pk::PluginError("批量多份输出文件与输入图片冲突");
                        }
                    }
                }
                plan.push_back(PlannedPair{si, ci, std::move(dest)});
            }
        }

        Json results = Json::array();
        size_t succeeded = 0;
        size_t failed = 0;

        for (const auto& pair : plan) {
            host.verifyNotCancelled(cancel);
            const auto& srcItem  = sources[pair.srcIdx];
            const auto& charItem = characters[pair.charIdx];
            if (!pair.destPath.parent_path().empty()) {
                pk::fs::create_directories(pair.destPath.parent_path());
            }

            Json job = baseJob;
            job["source"]    = pk::wideToUtf8(srcItem.path.wstring());
            job["character"] = pk::wideToUtf8(charItem.path.wstring());
            job["output"]    = pk::wideToUtf8(pair.destPath.wstring());
            job["count"]     = copyCount;
            if (args.contains("_simulate_fail_source") &&
                args["_simulate_fail_source"].get<std::string>() == job["source"].get<std::string>()) {
                job["_simulate_fail"] = true;
            }
            if (args.contains("_simulate_fail_character") &&
                args["_simulate_fail_character"].get<std::string>() == job["character"].get<std::string>()) {
                job["_simulate_fail"] = true;
            }

            MemeStudioBytesV2 resBytes{};
            MemeStudioBytesV2 errBytes{};
            std::string jobStr = job.dump();
            int32_t rc = host.raw()->run_core_replace_job(
                host.raw()->host_ctx, jobStr.c_str(), cancel, nullptr, nullptr, &resBytes, &errBytes);
            std::string errMsg = host.consumeBytesToString(errBytes);
            std::string resStr = host.consumeBytesToString(resBytes);

            if (rc == 0) {
                ++succeeded;
                Json rj = resStr.empty() ? Json::object() : Json::parse(resStr);
                results.push_back(Json{
                    {"source", job["source"]},
                    {"character", job["character"]},
                    {"status", "succeeded"},
                    {"outputs", rj.value("outputs", Json::array())}
                });
            } else if (rc == -2) {
                throw pk::PluginCancelled();
            } else {
                ++failed;
                results.push_back(Json{
                    {"source", job["source"]},
                    {"character", job["character"]},
                    {"status", "failed"},
                    {"error", errMsg}
                });
                if (!continueOnError) {
                    break;
                }
            }
        }

        return Json{
            {"source_count", sources.size()},
            {"character_count", characters.size()},
            {"total", plan.size()},
            {"succeeded", succeeded},
            {"failed", failed},
            {"skipped_sources", skippedSources.size()},
            {"skipped_characters", skippedCharacters.size()},
            {"items", results}
        };
    }

    static Json declarativeUiSchema() {
        return Json{
            {"schema_version", 1},
            {"plugin_id", kPluginId},
            {"title", "批量与多人物矩阵转换"},
            {"submit_command", "run-batch"},
            {"sections", Json::array({
                {{"id", "sources"}, {"type", "media_list"}, {"label", "源表情列表 (X 项)"}, {"accept_folders", true}},
                {{"id", "characters"}, {"type", "media_list"}, {"label", "目标人物参考列表 (Y 项)"}, {"accept_folders", true}},
                {{"id", "output_directory"}, {"type", "folder_picker"}, {"label", "批量输出目录"}, {"required", true}},
                {{"id", "count"}, {"type", "int"}, {"label", "每对组合生成份数"}, {"min", 1}, {"max", 10}, {"default", 1}},
                {{"id", "organize_by_character"}, {"type", "bool"}, {"label", "按人物名称分建子目录"}, {"default", false}},
                {{"id", "continue_on_error"}, {"type", "bool"}, {"label", "单项异常自动跳过继续"}, {"default", true}}
            })}
        };
    }
};

enum BatchPanelId : int {
    IdAddSrcFilesBtn  = 3301,
    IdAddSrcDirBtn    = 3302,
    IdRemSrcBtn       = 3303,
    IdClearSrcBtn     = 3304,
    IdSrcQueueLv      = 3305,

    IdAddCharFilesBtn = 3306,
    IdAddCharDirBtn   = 3307,
    IdRemCharBtn      = 3308,
    IdClearCharBtn    = 3309,
    IdCharQueueLv     = 3310,

    IdOutDirEdit      = 3311,
    IdBrowseOutBtn    = 3312,
    IdPromptEdit      = 3313,
    IdCountEdit       = 3314,
    IdSubfolderCheck  = 3315,
    IdContinueCheck   = 3316,
    IdStartBatchBtn   = 3317,
    IdCloseBatchBtn   = 3318
};

constexpr UINT WM_BATCH_DONE = WM_APP + 41;

struct BatchPanelContext {
    BatchConvertPlugin* plugin = nullptr;
    HWND hwnd = nullptr;
    HFONT font = nullptr;

    HWND srcHeaderLabel = nullptr, charHeaderLabel = nullptr;
    HWND srcLv = nullptr, charLv = nullptr;
    HWND outDirEdit = nullptr, promptEdit = nullptr, countEdit = nullptr;
    HWND subfolderCheck = nullptr, continueCheck = nullptr;
    HWND formulaLabel = nullptr, startBtn = nullptr;

    std::vector<QueuedMediaItem> sources;
    std::vector<QueuedMediaItem> characters;

    std::thread worker;
    bool busy = false;
    std::mutex mtx;
    Json lastReport = Json::object();
    std::string lastErr;

    ~BatchPanelContext() {
        if (worker.joinable()) worker.join();
        if (font) DeleteObject(font);
    }

    void updateMatrixFormulaLabel() {
        int copies = 1;
        try { copies = pk::parseIntField(countEdit, "份数", 1, 10); } catch (...) { copies = 1; }
        size_t x = sources.size();
        size_t y = characters.size();
        size_t pairs = x * y;
        size_t totalImages = pairs * static_cast<size_t>(copies);

        pk::setWindowTextUtf8(srcHeaderLabel, "源表情列表 (" + std::to_string(x) + " 项)");
        pk::setWindowTextUtf8(charHeaderLabel, "目标人物参考列表 (" + std::to_string(y) + " 项)");

        std::string text = "矩阵规模：" + std::to_string(x) + " 张源表情 × " +
                           std::to_string(y) + " 张人物参考 × " + std::to_string(copies) +
                           " 份 = 共 " + std::to_string(totalImages) + " 个输出文件";
        pk::setWindowTextUtf8(formulaLabel, text);
    }

    void refreshSourceTable() {
        ListView_DeleteAllItems(srcLv);
        for (size_t i = 0; i < sources.size(); ++i) {
            std::wstring name = std::to_wstring(i + 1) + L". " + sources[i].path.filename().wstring();
            LVITEMW lvi{};
            lvi.mask = LVIF_TEXT;
            lvi.iItem = static_cast<int>(i);
            lvi.pszText = name.data();
            ListView_InsertItem(srcLv, &lvi);

            std::wstring kind = sources[i].animated
                ? (L"GIF (" + std::to_wstring(sources[i].frames) + L"帧)")
                : L"静态图";
            std::wstring full = sources[i].path.wstring();
            ListView_SetItemText(srcLv, static_cast<int>(i), 1, kind.data());
            ListView_SetItemText(srcLv, static_cast<int>(i), 2, full.data());
        }
        updateMatrixFormulaLabel();
    }

    void refreshCharTable() {
        ListView_DeleteAllItems(charLv);
        for (size_t i = 0; i < characters.size(); ++i) {
            std::wstring name = std::to_wstring(i + 1) + L". " + characters[i].path.filename().wstring();
            LVITEMW lvi{};
            lvi.mask = LVIF_TEXT;
            lvi.iItem = static_cast<int>(i);
            lvi.pszText = name.data();
            ListView_InsertItem(charLv, &lvi);

            std::wstring dim = std::to_wstring(characters[i].width) + L"×" + std::to_wstring(characters[i].height);
            std::wstring full = characters[i].path.wstring();
            ListView_SetItemText(charLv, static_cast<int>(i), 1, dim.data());
            ListView_SetItemText(charLv, static_cast<int>(i), 2, full.data());
        }
        updateMatrixFormulaLabel();
    }

    void importIntoQueue(const std::vector<pk::fs::path>& incoming, bool isCharQueue) {
        auto& targetVec = isCharQueue ? characters : sources;
        std::vector<pk::fs::path> merged;
        for (const auto& ex : targetVec) merged.push_back(ex.path);
        merged.insert(merged.end(), incoming.begin(), incoming.end());

        std::vector<QueuedMediaItem> acc;
        std::vector<SkippedMediaItem> sk;
        plugin->scanMediaInputs(merged, acc, sk, isCharQueue);
        targetVec = std::move(acc);

        if (!sources.empty() && pk::trimAscii(pk::getWindowTextUtf8(outDirEdit)).empty()) {
            pk::fs::path suggested = sources.front().path.parent_path() / L"MemeStudio-BatchOutput";
            pk::setWindowTextUtf8(outDirEdit, pk::wideToUtf8(suggested.wstring()));
        }
        if (isCharQueue) refreshCharTable();
        else refreshSourceTable();
    }

    static intptr_t MEMESTUDIO_CALL wndProc(void* ctx, void* rawHwnd, uint32_t msg, uintptr_t wp, intptr_t lp) {
        auto* st = static_cast<BatchPanelContext*>(ctx);
        HWND w = static_cast<HWND>(rawHwnd);
        if (msg == WM_CREATE) {
            st->hwnd = w;
            pk::PanelBuilder ui(w);
            st->font = pk::createDpiUiFont(ui.dpi().dpi, 14);

            std::string initSrc, initChar, initInst;
            MemeStudioBytesV2 wsBytes{};
            if (st->plugin->host.raw()->get_workspace_state(st->plugin->host.raw()->host_ctx, &wsBytes) == 0) {
                try {
                    auto ws = Json::parse(st->plugin->host.consumeBytesToString(wsBytes));
                    initSrc  = ws.value("source_path", std::string());
                    initChar = ws.value("character_path", std::string());
                    initInst = ws.value("instruction", std::string());
                } catch (...) {}
            }

            // Left Half: Source Queue (X items)
            st->srcHeaderLabel = ui.label(L"源表情列表 (0 项)", 16, 14, 180, 22);
            ui.button(L"＋源图片", IdAddSrcFilesBtn, 16, 40, 88, 26);
            ui.button(L"＋源目录", IdAddSrcDirBtn,   108, 40, 88, 26);
            ui.button(L"移除",     IdRemSrcBtn,      200, 40, 60, 26);
            ui.button(L"清空",     IdClearSrcBtn,    264, 40, 60, 26);
            st->srcLv = ui.reportListView(IdSrcQueueLv, 16, 72, 416, 236, {
                {L"序号 / 源文件名", 160},
                {L"类型", 88},
                {L"路径", 150}
            });

            // Right Half: Character Reference Queue (Y items)
            st->charHeaderLabel = ui.label(L"目标人物参考列表 (0 项)", 448, 14, 240, 22);
            ui.button(L"＋人物图",   IdAddCharFilesBtn, 448, 40, 88, 26);
            ui.button(L"＋人物目录", IdAddCharDirBtn,   540, 40, 96, 26);
            ui.button(L"移除",       IdRemCharBtn,      640, 40, 60, 26);
            ui.button(L"清空",       IdClearCharBtn,    704, 40, 60, 26);
            st->charLv = ui.reportListView(IdCharQueueLv, 448, 72, 416, 236, {
                {L"序号 / 人物文件名", 165},
                {L"尺寸", 82},
                {L"路径", 150}
            });

            // Output & Matrix Controls
            ui.label(L"批量输出文件夹：", 16, 322, 130, 22);
            st->outDirEdit = ui.edit(IdOutDirEdit, 146, 318, 634, 26);
            ui.button(L"浏览…", IdBrowseOutBtn, 786, 318, 78, 26);

            ui.label(L"统一附加要求：", 16, 358, 130, 22);
            st->promptEdit = ui.multilineEdit(IdPromptEdit, 146, 356, 718, 74, initInst, false);

            ui.label(L"每对组合份数：", 16, 444, 118, 22);
            st->countEdit = ui.edit(IdCountEdit, 146, 440, 66, 26, "1", ES_NUMBER);
            st->subfolderCheck = ui.checkbox(L"按人物名分建子目录保存", IdSubfolderCheck, 232, 442, 210, 24, false);
            st->continueCheck  = ui.checkbox(L"单项异常自动跳过并继续执行后续组合", IdContinueCheck, 456, 442, 340, 24, true);

            st->formulaLabel = ui.label(L"", 16, 486, 580, 24);
            st->startBtn = ui.button(L"开始矩阵批量转换", IdStartBatchBtn, 606, 480, 158, 34, true);
            ui.button(L"关闭", IdCloseBatchBtn, 772, 480, 92, 34, false);

            ui.applyFont(st->font);

            if (!initSrc.empty())  st->importIntoQueue({pk::utf8ToWideStrict(initSrc)}, false);
            if (!initChar.empty()) st->importIntoQueue({pk::utf8ToWideStrict(initChar)}, true);
            st->updateMatrixFormulaLabel();
            return 0;
        }
        if (msg == WM_COMMAND && !st->busy) {
            int id = LOWORD(wp);
            int code = HIWORD(wp);
            if (id == IdCountEdit && code == EN_CHANGE) {
                st->updateMatrixFormulaLabel();
                return 0;
            }
            if (id == IdAddSrcFilesBtn) {
                auto p = pk::pickMultiplePaths(w, L"选择一张或多张源表情图片 / GIF");
                if (!p.empty()) st->importIntoQueue(p, false);
                return 0;
            }
            if (id == IdAddSrcDirBtn) {
                if (auto d = pk::pickSinglePath(w, false, true, L"选择源表情所在文件夹")) st->importIntoQueue({*d}, false);
                return 0;
            }
            if (id == IdRemSrcBtn) {
                int sel = ListView_GetNextItem(st->srcLv, -1, LVNI_SELECTED);
                if (sel >= 0 && static_cast<size_t>(sel) < st->sources.size()) {
                    st->sources.erase(st->sources.begin() + sel);
                    st->refreshSourceTable();
                }
                return 0;
            }
            if (id == IdClearSrcBtn) {
                st->sources.clear();
                st->refreshSourceTable();
                return 0;
            }
            if (id == IdAddCharFilesBtn) {
                auto p = pk::pickMultiplePaths(w, L"选择一张或多张目标人物参考图");
                if (!p.empty()) st->importIntoQueue(p, true);
                return 0;
            }
            if (id == IdAddCharDirBtn) {
                if (auto d = pk::pickSinglePath(w, false, true, L"选择目标人物参考图所在文件夹")) st->importIntoQueue({*d}, true);
                return 0;
            }
            if (id == IdRemCharBtn) {
                int sel = ListView_GetNextItem(st->charLv, -1, LVNI_SELECTED);
                if (sel >= 0 && static_cast<size_t>(sel) < st->characters.size()) {
                    st->characters.erase(st->characters.begin() + sel);
                    st->refreshCharTable();
                }
                return 0;
            }
            if (id == IdClearCharBtn) {
                st->characters.clear();
                st->refreshCharTable();
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
                    if (st->sources.empty()) throw pk::PluginError("请先添加至少 1 张源表情素材");
                    if (st->characters.empty()) throw pk::PluginError("请先添加至少 1 张目标人物参考图");
                    std::string outDirStr = pk::trimAscii(pk::getWindowTextUtf8(st->outDirEdit));
                    if (outDirStr.empty()) throw pk::PluginError("请指定批量输出文件夹");
                    int count = pk::parseIntField(st->countEdit, "每对组合份数", 1, 10);
                    bool subf = pk::isButtonChecked(st->subfolderCheck);
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
                    for (const auto& it : st->sources) srcArr.push_back(pk::wideToUtf8(it.path.wstring()));
                    Json charArr = Json::array();
                    for (const auto& it : st->characters) charArr.push_back(pk::wideToUtf8(it.path.wstring()));

                    Json runArgs{
                        {"sources", srcArr},
                        {"characters", charArr},
                        {"output_directory", outDirStr},
                        {"count", count},
                        {"organize_by_character", subf},
                        {"continue_on_error", cont},
                        {"job_template", jobTpl}
                    };

                    if (st->worker.joinable()) st->worker.join();
                    st->busy = true;
                    EnableWindow(st->startBtn, FALSE);
                    pk::setWindowTextUtf8(st->formulaLabel,
                        "正在后台生成 " + std::to_string(st->sources.size()) + " × " +
                        std::to_string(st->characters.size()) + " 矩阵组合…");

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
                    pk::showPanelError(w, e.what(), L"批量矩阵转换");
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
                st->updateMatrixFormulaLabel();
                pk::showPanelError(w, err, L"批量转换失败");
            } else {
                std::string summary = "矩阵转换完成：共 " + std::to_string(rep.value("total", 0)) +
                                      " 组（成功 " + std::to_string(rep.value("succeeded", 0)) +
                                      "，失败 " + std::to_string(rep.value("failed", 0)) + "）";
                pk::setWindowTextUtf8(st->formulaLabel, summary);
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

        if (cmd == MEMESTUDIO_CMD_UI_SCHEMA) {
            self->host.writeOutBytes(outResult, BatchConvertPlugin::declarativeUiSchema().dump());
            return 0;
        }

        if (cmd == "open-panel") {
            auto* pctx = new BatchPanelContext{self};
            if (self->host.raw()->open_panel(
                    self->host.raw()->host_ctx, "batch-convert", ownerHwnd,
                    880, 530, "批量与多人物矩阵转换",
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
            std::vector<QueuedMediaItem> accepted;
            std::vector<SkippedMediaItem> skipped;
            self->scanMediaInputs(inputs, accepted, skipped, args.value("static_only", false));
            Json accArr = Json::array();
            for (const auto& a : accepted) {
                accArr.push_back(Json{
                    {"path", pk::wideToUtf8(a.path.wstring())},
                    {"animated", a.animated},
                    {"frames", a.frames},
                    {"width", a.width},
                    {"height", a.height}
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
