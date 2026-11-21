#include "memestudio_plugin_kit.hpp"
#include "../vendor/nlohmann/json.hpp"
#include <commctrl.h>
#include <string>
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

struct ProbePanelContext {
    ApiProbePlugin* plugin = nullptr;
    static intptr_t MEMESTUDIO_CALL wndProc(void*, void* rawHwnd, uint32_t msg, uintptr_t wp, intptr_t lp) {
        return DefWindowProcW(static_cast<HWND>(rawHwnd), msg, wp, lp);
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
                    820, 600, "API 参数兼容性探测",
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
