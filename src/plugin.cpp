#include "memestudio_plugin_kit.hpp"
#include "../vendor/nlohmann/json.hpp"
#include <commctrl.h>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using Json = nlohmann::json;
namespace pk = meme::plugin_kit;

constexpr const char* kPluginId = "memestudio.api-parameter-test";
constexpr const char* kProbePrompt = "Create a simple blue square on a clean white background.";

// Structured recursive JSON string redactor (fixes BUG-12 short-key JSON self-destruction!)
Json redactJsonTree(const Json& node, const std::string& secret) {
    if (secret.empty()) return node;
    if (node.is_string()) {
        std::string s = node.get<std::string>();
        size_t pos = 0;
        while ((pos = s.find(secret, pos)) != std::string::npos) {
            s.replace(pos, secret.size(), "[REDACTED]");
            pos += 10;
        }
        return s;
    }
    if (node.is_array()) {
        Json arr = Json::array();
        for (const auto& item : node) arr.push_back(redactJsonTree(item, secret));
        return arr;
    }
    if (node.is_object()) {
        Json obj = Json::object();
        for (const auto& [k, v] : node.items()) obj[k] = redactJsonTree(v, secret);
        return obj;
    }
    return node;
}

std::string detectImageContainerFormat(std::span<const uint8_t> bytes) {
    if (bytes.size() >= 8 && bytes[0] == 137 && bytes[1] == 'P' && bytes[2] == 'N' && bytes[3] == 'G') {
        return "png";
    }
    if (bytes.size() >= 3 && bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF) {
        return "jpeg";
    }
    if (bytes.size() >= 12 && std::memcmp(bytes.data(), "RIFF", 4) == 0 && std::memcmp(bytes.data() + 8, "WEBP", 4) == 0) {
        return "webp";
    }
    return {};
}

std::string resolveProbeUrl(std::string base, bool generationMode) {
    base = pk::trimAscii(base);
    if (base.empty()) throw pk::PluginError("请填写接口地址 (Base URL)");
    std::string query;
    auto q = base.find('?');
    if (q != std::string::npos) {
        query = base.substr(q);
        base.resize(q);
    }
    while (!base.empty() && base.back() == '/') base.pop_back();
    if (base.ends_with("/images/edits")) base.resize(base.size() - 13);
    else if (base.ends_with("/images/generations")) base.resize(base.size() - 19);
    else if (base.ends_with("/images")) base.resize(base.size() - 7);
    return base + (generationMode ? "/images/generations" : "/images/edits") + query;
}

struct ProbeCase {
    std::string id;
    std::string label;
    Json overrides = Json::object();
};

std::vector<ProbeCase> defaultProbeMatrix(const Json& baseProvider, bool generationMode) {
    std::string currentField = baseProvider.value("image_field", std::string("image[]"));
    std::string altField = (currentField == "image") ? "image[]" : "image";

    std::vector<ProbeCase> cases;
    // Item 0: Baseline strictly honors user's configured image_field (fixes BUG-06!)
    cases.push_back({"baseline", "基线连接校验 (" + currentField + ")", Json::object()});
    if (!generationMode) {
        cases.push_back({"alt_image_field", "备选上传字段 (" + altField + ")", {{"image_field", altField}}});
    }
    cases.push_back({"quality_high", "生成画质 quality=high", {{"quality", "high"}}});
    cases.push_back({"size_square", "标准尺寸 size=1024x1024", {{"size", "1024x1024"}}});
    cases.push_back({"size_landscape", "横向尺寸 size=1536x1024", {{"size", "1536x1024"}}});
    cases.push_back({"format_webp", "输出格式 webp + 压缩率 85", {{"output_format", "webp"}, {"output_compression", 85}}});
    cases.push_back({"stream_sse", "流式传输 stream=true (partial=2)", {{"stream", true}, {"partial_images", 2}}});
    return cases;
}

pk::Bytes makeMinimalTestPng() {
    static constexpr uint8_t kTinyPng[] = {
        0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A,
        0x00,0x00,0x00,0x0D,'I','H','D','R',0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x08,0x06,0x00,0x00,0x00,0x1F,0x15,0xC4,0x89,
        0x00,0x00,0x00,0x0A,'I','D','A','T',0x78,0x9C,0x63,0x00,0x01,0x00,0x00,0x05,0x00,0x01,0x0D,0x0A,0x2D,0xB4,
        0x00,0x00,0x00,0x00,'I','E','N','D',0xAE,0x42,0x60,0x82
    };
    return pk::Bytes(std::begin(kTinyPng), std::end(kTinyPng));
}

struct ApiProbePlugin {
    pk::HostClient host;

    explicit ApiProbePlugin(const MemeStudioHostV2* h) : host(h, kPluginId) {}

    Json probeSingleCase(
        const Json& effectiveProvider,
        bool generationMode,
        const pk::Bytes& samplePng,
        MemeStudioCancelTokenV2* cancel) {
        std::string apiKey = effectiveProvider.at("api_key").get<std::string>();
        std::string model = pk::trimAscii(effectiveProvider.value("model", std::string("gpt-image-1")));
        std::string url = resolveProbeUrl(effectiveProvider.at("base_url").get<std::string>(), generationMode);
        bool wantStream = effectiveProvider.value("stream", false);
        std::string wantFormat = effectiveProvider.value("output_format", std::string());
        int compression = effectiveProvider.value("output_compression", -1);
        int partialImages = std::clamp(effectiveProvider.value("partial_images", 2), 1, 3);
        int timeoutSec = std::clamp(effectiveProvider.value("timeout_seconds", 120), 5, 3600);

        Json reqFields{
            {"model", model},
            {"prompt", kProbePrompt}
        };
        if (effectiveProvider.contains("quality") && !effectiveProvider["quality"].get<std::string>().empty()) {
            reqFields["quality"] = effectiveProvider["quality"];
        }
        if (effectiveProvider.contains("size") && !effectiveProvider["size"].get<std::string>().empty()) {
            reqFields["size"] = effectiveProvider["size"];
        }
        if (!wantFormat.empty()) reqFields["output_format"] = wantFormat;
        if (compression >= 0) reqFields["output_compression"] = compression;
        if (wantStream) {
            reqFields["stream"] = true;
            reqFields["partial_images"] = partialImages;
        }
        if (effectiveProvider.contains("extra") && effectiveProvider["extra"].is_object()) {
            for (const auto& [ek, ev] : effectiveProvider["extra"].items()) {
                reqFields[ek] = ev;
            }
        }

        std::string contentType;
        pk::Bytes body;
        if (generationMode) {
            contentType = "application/json";
            std::string dumped = reqFields.dump();
            body.assign(dumped.begin(), dumped.end());
        } else {
            std::string boundary = "MemeProbe" + pk::randomHex(12);
            contentType = "multipart/form-data; boundary=" + boundary;
            std::string imageField = effectiveProvider.value("image_field", std::string("image[]"));
            auto appendStr = [&](std::string_view s) { body.insert(body.end(), s.begin(), s.end()); };
            for (const auto& [fk, fv] : reqFields.items()) {
                std::string vs = fv.is_string() ? fv.get<std::string>() : fv.dump();
                appendStr("--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + fk + "\"\r\n\r\n" + vs + "\r\n");
            }
            appendStr("--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + imageField +
                      "\"; filename=\"probe.png\"\r\nContent-Type: image/png\r\n\r\n");
            body.insert(body.end(), samplePng.begin(), samplePng.end());
            appendStr("\r\n--" + boundary + "--\r\n");
        }

        Json headers{
            {"Authorization", "Bearer " + apiKey},
            {"Content-Type", contentType}
        };
        if (wantStream) headers["Accept"] = "text/event-stream";

        Json httpSpec{
            {"url", url},
            {"method", "POST"},
            {"timeout_seconds", timeoutSec},
            {"expect_event_stream", wantStream},
            {"headers", headers}
        };
        std::string httpSpecStr = httpSpec.dump();

        struct SseProbeTrack {
            int partials = 0;
            bool gotCompleted = false;
            std::string sseErr;
        } sseTrack;

        auto onSse = [](void* u, const char* evType, const char* evData) {
            auto* st = static_cast<SseProbeTrack*>(u);
            if (!st || !evData) return;
            auto j = Json::parse(evData, nullptr, false);
            if (!j.is_object()) return;
            std::string t = j.value("type", std::string(evType ? evType : ""));
            if (j.contains("error") || t == "error") {
                st->sseErr = j.dump();
            } else if (t == "image_edit.partial_image" || t == "image_generation.partial_image") {
                ++st->partials;
            } else if (t == "image_edit.completed" || t == "image_generation.completed") {
                if (j.contains("b64_json") && j["b64_json"].is_string()) {
                    st->gotCompleted = true;
                }
            }
        };

        uint32_t status = 0;
        int32_t streamed = 0;
        MemeStudioBytesV2 respBytes{};
        MemeStudioBytesV2 errBytes{};
        int32_t hrc = host.raw()->http_send(
            host.raw()->host_ctx, httpSpecStr.c_str(),
            body.data(), body.size(), cancel,
            onSse, &sseTrack, &status, &streamed, &respBytes, &errBytes);
        std::string hErr = host.consumeBytesToString(errBytes);
        pk::Bytes respVec = host.consumeBytesToVec(respBytes);
        if (hrc == -2) throw pk::PluginCancelled();
        if (hrc != 0) {
            return Json{
                {"status", 0},
                {"outcome", "connection_error"},
                {"detail", hErr}
            };
        }

        if (streamed) {
            if (!sseTrack.sseErr.empty()) {
                return Json{{"status", status}, {"outcome", "rejected"}, {"detail", sseTrack.sseErr}};
            }
            if (!sseTrack.gotCompleted) {
                return Json{{"status", status}, {"outcome", "inconclusive"}, {"detail", "SSE 流结束但未收到完成图像事件"}};
            }
            return Json{
                {"status", status},
                {"outcome", "accepted"},
                {"streamed", true},
                {"partial_images", sseTrack.partials},
                {"detail", "SSE 流式完成图像验证通过（收到局部预览 " + std::to_string(sseTrack.partials) + " 帧）"}
            };
        }

        auto parsed = Json::parse(respVec.begin(), respVec.end(), nullptr, false);
        if (status < 200 || status >= 300 || (parsed.is_object() && parsed.contains("error"))) {
            std::string errParam;
            if (parsed.is_object() && parsed.contains("error") && parsed["error"].is_object() &&
                parsed["error"].contains("param") && parsed["error"]["param"].is_string()) {
                errParam = parsed["error"]["param"].get<std::string>();
            }
            std::string outcome = (status == 400 || status == 422) ? "rejected" :
                                  (status == 401 || status == 403 ? "auth_error" :
                                  (status == 429 ? "rate_limited" : "http_error"));
            std::string detail = parsed.is_object() ? parsed.dump() : ("HTTP " + std::to_string(status));
            return Json{
                {"status", status},
                {"outcome", outcome},
                {"error_parameter", errParam},
                {"detail", detail}
            };
        }

        if (!parsed.is_object() || !parsed.contains("data") || !parsed["data"].is_array() || parsed["data"].empty()) {
            return Json{{"status", status}, {"outcome", "inconclusive"}, {"detail", "响应缺少 data[0] 图像条目"}};
        }

        const auto& first = parsed["data"][0];
        std::string outcome = "accepted";
        std::vector<std::string> notes;

        if (first.contains("b64_json") && first["b64_json"].is_string()) {
            auto imgBytes = pk::base64Decode(first["b64_json"].get<std::string>());
            std::string actualFmt = detectImageContainerFormat(imgBytes);
            if (!wantFormat.empty() && !actualFmt.empty() && actualFmt != wantFormat) {
                outcome = "ignored";
                notes.push_back("请求格式 " + wantFormat + " 被忽略，实际返回 " + actualFmt);
            } else {
                notes.push_back("收到有效内联图像" + (actualFmt.empty() ? "" : " (" + actualFmt + ")"));
            }
        } else if (first.contains("url") && first["url"].is_string()) {
            notes.push_back("返回远程图像 URL");
        } else {
            return Json{{"status", status}, {"outcome", "inconclusive"}, {"detail", "响应缺少 b64_json 或 url"}};
        }

        // Accumulate stream fallback note WITHOUT overwriting output_format mismatch note!
        if (wantStream) {
            outcome = "ignored";
            notes.push_back("请求了 stream=true，但服务端回退返回普通 JSON");
        }

        std::string joined;
        for (size_t i = 0; i < notes.size(); ++i) {
            if (i > 0) joined += "；";
            joined += notes[i];
        }
        return Json{
            {"status", status},
            {"outcome", outcome},
            {"streamed", false},
            {"detail", joined}
        };
    }

    Json runMatrixProbe(const Json& args, MemeStudioCancelTokenV2* cancel) {
        Json baseProvider = args.value("provider", Json::object());
        std::string apiKey = baseProvider.value("api_key", std::string());
        if (apiKey.empty()) {
            apiKey = pk::readEnvVarUtf8(baseProvider.value("api_key_env", std::string("OPENAI_API_KEY")));
            baseProvider["api_key"] = apiKey;
        }
        if (apiKey.empty()) throw pk::PluginError("请先提供有效的 API 密钥");
        if (pk::trimAscii(baseProvider.value("base_url", std::string())).empty()) {
            throw pk::PluginError("请先提供有效的接口地址 (Base URL)");
        }

        bool generationMode = args.value("generation_mode", false);
        auto cases = defaultProbeMatrix(baseProvider, generationMode);
        if (args.contains("custom_cases") && args["custom_cases"].is_array()) {
            int customIdx = 1;
            for (const auto& c : args["custom_cases"]) {
                if (!c.is_object()) continue;
                cases.push_back(ProbeCase{
                    "custom_" + std::to_string(customIdx++),
                    c.value("label", std::string("自定义参数组合")),
                    c.value("overrides", c)
                });
            }
        }

        pk::Bytes samplePng = makeMinimalTestPng();
        Json rows = Json::array();
        bool baselineWorking = false;

        for (size_t i = 0; i < cases.size(); ++i) {
            host.verifyNotCancelled(cancel);
            Json caseProvider = baseProvider;
            for (const auto& [ok, ov] : cases[i].overrides.items()) {
                caseProvider[ok] = ov;
            }

            Json res = probeSingleCase(caseProvider, generationMode, samplePng, cancel);
            res["id"] = cases[i].id;
            res["label"] = cases[i].label;
            res["overrides"] = cases[i].overrides;
            rows.push_back(res);

            std::string outc = res.value("outcome", "");
            if (outc == "auth_error" || outc == "rate_limited" || outc == "connection_error") {
                break; // Hard stop on auth/429/network down
            }

            if (i == 0) {
                if (outc == "accepted" || outc == "ignored") {
                    baselineWorking = true;
                } else if (generationMode || cases.size() < 2 || cases[1].id != "alt_image_field") {
                    break;
                }
                // If baseline failed in multipart mode, DO NOT deadlock yet — try cases[1] (alt_image_field)!
            } else if (i == 1 && !baselineWorking && cases[1].id == "alt_image_field") {
                if (outc == "accepted" || outc == "ignored") {
                    // Adaptive recovery! Adopt the working image_field for all remaining cases!
                    baseProvider["image_field"] = cases[1].overrides["image_field"];
                    baselineWorking = true;
                } else {
                    break; // Neither image[] nor image worked; stop wasting requests
                }
            }
        }

        Json fullReport{
            {"generation_mode", generationMode},
            {"recommended_image_field", baseProvider.value("image_field", "image[]")},
            {"results", rows}
        };
        return redactJsonTree(fullReport, apiKey);
    }
};

enum ProbePanelId : int {
    IdModeCombo     = 3501,
    IdMatrixLv      = 3502,
    IdCustomJsonBox = 3503,
    IdAddCustomBtn  = 3504,
    IdExportJsonBtn = 3505,
    IdStartProbeBtn = 3506,
    IdCloseProbeBtn = 3507
};

constexpr UINT WM_PROBE_DONE = WM_APP + 43;

struct ProbePanelContext {
    ApiProbePlugin* plugin = nullptr;
    HWND hwnd = nullptr;
    HFONT font = nullptr;
    HWND modeCombo = nullptr, matrixLv = nullptr, customBox = nullptr;
    HWND statusLabel = nullptr, exportBtn = nullptr, startBtn = nullptr;

    std::vector<Json> customCases;
    std::thread worker;
    bool busy = false;
    std::mutex mtx;
    Json lastReport = Json::object();
    std::string lastErr;

    ~ProbePanelContext() {
        if (worker.joinable()) worker.join();
        if (font) DeleteObject(font);
    }

    Json pullWorkspaceProvider() {
        MemeStudioBytesV2 wsBytes{};
        if (plugin->host.raw()->get_workspace_state(plugin->host.raw()->host_ctx, &wsBytes) == 0) {
            try {
                auto ws = Json::parse(plugin->host.consumeBytesToString(wsBytes));
                if (ws.contains("provider") && ws["provider"].is_object()) return ws["provider"];
            } catch (...) {}
        }
        return Json::object();
    }

    void populatePlannedMatrix() {
        ListView_DeleteAllItems(matrixLv);
        bool genMode = (SendMessageW(modeCombo, CB_GETCURSEL, 0, 0) == 1);
        auto planned = defaultProbeMatrix(pullWorkspaceProvider(), genMode);
        for (size_t i = 0; i < customCases.size(); ++i) {
            planned.push_back(ProbeCase{
                "custom_" + std::to_string(i + 1),
                customCases[i].value("label", std::string("自定义探测项")),
                customCases[i].value("overrides", customCases[i])
            });
        }

        for (size_t i = 0; i < planned.size(); ++i) {
            std::wstring wlabel = pk::utf8ToWideLossy(planned[i].label);
            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.iItem = static_cast<int>(i);
            item.pszText = wlabel.data();
            ListView_InsertItem(matrixLv, &item);

            std::wstring wover = pk::utf8ToWideLossy(planned[i].overrides.dump());
            ListView_SetItemText(matrixLv, static_cast<int>(i), 1, wover.data());
            ListView_SetItemText(matrixLv, static_cast<int>(i), 2, const_cast<LPWSTR>(L"—"));
            ListView_SetItemText(matrixLv, static_cast<int>(i), 3, const_cast<LPWSTR>(L"待探测"));
            ListView_SetItemText(matrixLv, static_cast<int>(i), 4, const_cast<LPWSTR>(L""));
        }
        pk::setWindowTextUtf8(
            statusLabel,
            "计划发送 " + std::to_string(planned.size()) + " 次真实图像探测请求（产生正常接口调用消耗）");
    }

    static intptr_t MEMESTUDIO_CALL wndProc(void* ctx, void* rawHwnd, uint32_t msg, uintptr_t wp, intptr_t lp) {
        auto* st = static_cast<ProbePanelContext*>(ctx);
        HWND w = static_cast<HWND>(rawHwnd);
        if (msg == WM_CREATE) {
            st->hwnd = w;
            pk::PanelBuilder ui(w);
            st->font = pk::createDpiUiFont(ui.dpi().dpi, 14);

            ui.label(L"探测接口模式：", 16, 16, 110, 22);
            st->modeCombo = ui.combo(IdModeCombo, 126, 12, 340, 160, {
                L"多图编辑接口 (/v1/images/edits · Multipart)",
                L"纯文生图接口 (/v1/images/generations · JSON)"
            }, 0);

            st->matrixLv = ui.reportListView(IdMatrixLv, 16, 48, 788, 320, {
                {L"探测用例", 200},
                {L"覆盖参数", 190},
                {L"HTTP", 65},
                {L"兼容结论", 95},
                {L"诊断说明", 215}
            });

            ui.label(L"追加自定义探测参数（JSON 对象，按 Enter 换行不会误触发送）：", 16, 380, 520, 20);
            st->customBox = ui.multilineEdit(IdCustomJsonBox, 16, 404, 650, 88, "{\r\n  \"quality\": \"medium\"\r\n}", false);
            ui.button(L"添加探测项", IdAddCustomBtn, 676, 404, 128, 30);
            st->exportBtn = ui.button(L"导出报告 JSON…", IdExportJsonBtn, 676, 442, 128, 30);
            EnableWindow(st->exportBtn, FALSE);

            st->statusLabel = ui.label(L"", 16, 506, 520, 24);
            st->startBtn = ui.button(L"开始探测", IdStartProbeBtn, 560, 500, 140, 34, true);
            ui.button(L"关闭", IdCloseProbeBtn, 708, 500, 96, 34, false);

            ui.applyFont(st->font);
            st->populatePlannedMatrix();
            return 0;
        }
        if (msg == WM_COMMAND && !st->busy) {
            int id = LOWORD(wp);
            int code = HIWORD(wp);
            if (id == IdModeCombo && code == CBN_SELCHANGE) {
                st->populatePlannedMatrix();
                return 0;
            }
            if (id == IdAddCustomBtn) {
                try {
                    auto j = Json::parse(pk::getWindowTextUtf8(st->customBox));
                    if (!j.is_object() || j.empty()) throw pk::PluginError("自定义参数必须是非空 JSON 对象");
                    st->customCases.push_back(Json{{"label", "自定义组合 #" + std::to_string(st->customCases.size() + 1)}, {"overrides", j}});
                    st->populatePlannedMatrix();
                } catch (const std::exception& e) {
                    pk::showPanelError(w, e.what(), L"自定义参数校验");
                }
                return 0;
            }
            if (id == IdExportJsonBtn) {
                if (auto p = pk::pickSinglePath(w, true, false, L"导出脱敏探测报告 JSON")) {
                    std::string dumped;
                    {
                        std::lock_guard lk(st->mtx);
                        dumped = st->lastReport.dump(2);
                    }
                    pk::writeFileAtomic(*p, std::span<const uint8_t>(
                        reinterpret_cast<const uint8_t*>(dumped.data()), dumped.size()));
                }
                return 0;
            }
            if (id == IdStartProbeBtn) {
                Json prov = st->pullWorkspaceProvider();
                if (pk::trimAscii(prov.value("base_url", std::string())).empty()) {
                    pk::showPanelError(w, "请先在主窗口右侧配置接口地址与 API 密钥", L"接口探测");
                    return 0;
                }
                if (MessageBoxW(w, L"即将向目标接口发送真实生图请求以验证参数兼容性（会产生正常接口消耗）。\n是否立即开始？",
                                L"确认执行接口探测", MB_OKCANCEL | MB_ICONINFORMATION) != IDOK) {
                    return 0;
                }

                bool genMode = (SendMessageW(st->modeCombo, CB_GETCURSEL, 0, 0) == 1);
                Json probeArgs{
                    {"generation_mode", genMode},
                    {"provider", prov},
                    {"custom_cases", st->customCases}
                };

                if (st->worker.joinable()) st->worker.join();
                st->busy = true;
                EnableWindow(st->startBtn, FALSE);
                pk::setWindowTextUtf8(st->statusLabel, "正在逐项发送探测请求并校验响应…");

                st->worker = std::thread([st, probeArgs = std::move(probeArgs)]() {
                    Json rep;
                    std::string err;
                    try {
                        rep = st->plugin->runMatrixProbe(probeArgs, nullptr);
                    } catch (const std::exception& e) {
                        err = e.what();
                    }
                    {
                        std::lock_guard lk(st->mtx);
                        st->lastReport = std::move(rep);
                        st->lastErr = std::move(err);
                    }
                    if (st->hwnd && IsWindow(st->hwnd)) PostMessageW(st->hwnd, WM_PROBE_DONE, 0, 0);
                });
                return 0;
            }
            if (id == IdCloseProbeBtn || id == IDCANCEL) {
                DestroyWindow(w);
                return 0;
            }
        }
        if (msg == WM_PROBE_DONE) {
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
                pk::setWindowTextUtf8(st->statusLabel, "探测中止");
                pk::showPanelError(w, err, L"接口探测失败");
            } else {
                ListView_DeleteAllItems(st->matrixLv);
                const auto& rows = rep.value("results", Json::array());
                for (size_t i = 0; i < rows.size(); ++i) {
                    const auto& r = rows[i];
                    std::wstring wlab = pk::utf8ToWideLossy(r.value("label", ""));
                    LVITEMW lvi{};
                    lvi.mask = LVIF_TEXT;
                    lvi.iItem = static_cast<int>(i);
                    lvi.pszText = wlab.data();
                    ListView_InsertItem(st->matrixLv, &lvi);

                    std::wstring wov = pk::utf8ToWideLossy(r.value("overrides", Json::object()).dump());
                    std::wstring wst = std::to_wstring(r.value("status", 0));
                    std::wstring wout = pk::utf8ToWideLossy(r.value("outcome", ""));
                    std::wstring wdet = pk::utf8ToWideLossy(r.value("detail", ""));
                    ListView_SetItemText(st->matrixLv, static_cast<int>(i), 1, wov.data());
                    ListView_SetItemText(st->matrixLv, static_cast<int>(i), 2, wst.data());
                    ListView_SetItemText(st->matrixLv, static_cast<int>(i), 3, wout.data());
                    ListView_SetItemText(st->matrixLv, static_cast<int>(i), 4, wdet.data());
                }
                EnableWindow(st->exportBtn, TRUE);
                pk::setWindowTextUtf8(
                    st->statusLabel,
                    "探测完成（建议上传字段名：" + rep.value("recommended_image_field", std::string("image[]")) + "）");
            }
            return 0;
        }
        if (msg == WM_CLOSE) {
            if (!st->busy) DestroyWindow(w);
            return 0;
        }
        return DefWindowProcW(w, msg, wp, lp);
    }

    static void MEMESTUDIO_CALL onDestroy(void* ctx) { delete static_cast<ProbePanelContext*>(ctx); }
};

int32_t MEMESTUDIO_CALL abiCreate(const MemeStudioHostV2* host, void** outInst, MemeStudioBytesV2* outErr) {
    if (!host || !outInst || host->abi_version != MEMESTUDIO_PLUGIN_ABI_V2) return -1;
    try {
        *outInst = new ApiProbePlugin(host);
        return 0;
    } catch (const std::exception& e) {
        pk::HostClient(host, kPluginId).writeOutBytes(outErr, e.what());
        return -1;
    }
}

void MEMESTUDIO_CALL abiDestroy(void* inst) { delete static_cast<ApiProbePlugin*>(inst); }

int32_t MEMESTUDIO_CALL abiInvokeCommand(
    void* rawInst, const char* commandId, const char* argsJson, void* ownerHwnd,
    MemeStudioCancelTokenV2* cancel, MemeStudioBytesV2* outResult, MemeStudioBytesV2* outErr) {
    auto* self = static_cast<ApiProbePlugin*>(rawInst);
    if (!self || !commandId) return -1;
    try {
        std::string cmd(commandId);
        Json args = (argsJson && argsJson[0]) ? Json::parse(argsJson) : Json::object();
        if (cmd == "open-panel") {
            auto* pctx = new ProbePanelContext{self};
            if (self->host.raw()->open_panel(
                    self->host.raw()->host_ctx, "api-parameter-test", ownerHwnd,
                    820, 550, "API 参数兼容性探测",
                    ProbePanelContext::wndProc, pctx, ProbePanelContext::onDestroy) != 0) {
                delete pctx;
                throw pk::PluginError("无法打开参数探测面板");
            }
            self->host.writeOutBytes(outResult, R"({"opened":true})");
            return 0;
        }
        if (cmd == "plan") {
            auto cases = defaultProbeMatrix(args.value("provider", Json::object()), args.value("generation_mode", false));
            Json arr = Json::array();
            for (const auto& c : cases) {
                arr.push_back(Json{{"id", c.id}, {"label", c.label}, {"overrides", c.overrides}});
            }
            self->host.writeOutBytes(outResult, Json{{"cases", arr}}.dump());
            return 0;
        }
        if (cmd == "probe") {
            Json report = self->runMatrixProbe(args, cancel);
            self->host.writeOutBytes(outResult, report.dump());
            return 0;
        }
        throw pk::PluginError("未知的参数探测命令：" + cmd);
    } catch (const std::exception& e) {
        self->host.writeOutBytes(outErr, e.what());
        return -1;
    }
}

const MemeStudioPluginV2 kDescriptor{
    sizeof(MemeStudioPluginV2), MEMESTUDIO_PLUGIN_ABI_V2, kPluginId,
    abiCreate, abiDestroy, abiInvokeCommand, nullptr, nullptr, nullptr
};

} // namespace

extern "C" MEMESTUDIO_EXPORT const MemeStudioPluginV2* MEMESTUDIO_CALL MemeStudioGetPluginV2(void) {
    return &kDescriptor;
}
