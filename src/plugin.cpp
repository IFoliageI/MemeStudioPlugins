#include "memestudio_plugin_kit.hpp"
#include "../vendor/nlohmann/json.hpp"
#include <shellapi.h>
#include <chrono>
#include <limits>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using Json = nlohmann::json;
namespace pk = meme::plugin_kit;
using SteadyClock = std::chrono::steady_clock;

constexpr const char* kPluginId = "memestudio.api-load-balancing";

struct ChannelSpec {
    std::string name = "Channel-1";
    std::string baseUrl = "https://api.openai.com/v1";
    std::string apiKey;
    std::string apiKeyEnv;
    std::vector<std::string> models{"gpt-image-1"};
    int priority = 0;         // 0..99 (lower = higher priority tier)
    int weight = 100;         // 1..10000
    double unitCost = 1.0;    // > 0
    bool enabled = true;
};

struct ChannelHealth {
    int64_t smoothWeight = 0;
    double cumulativeCost = 0.0;
    uint64_t totalRequests = 0;
    uint64_t successes = 0;
    uint64_t failovers = 0;
    bool authDisabled = false;
    SteadyClock::time_point cooldownUntil{};
    std::string lastError;
};

struct RoutingConfig {
    bool takeoverEnabled = false;
    std::string strategy = "weighted"; // "weighted" | "cost"
    bool serverFailover = true;        // Failover on 500/502/503/504/52x & timeouts
    int cooldownSeconds = 60;
    std::vector<ChannelSpec> channels;
};

std::string resolveTargetUrl(std::string base, std::string_view op) {
    base = pk::trimAscii(base);
    if (base.empty()) throw pk::PluginError("渠道 Base URL 不能为空");
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
    else if (base.ends_with("/models")) base.resize(base.size() - 7);

    if (op == "models") return base + "/models" + query;
    if (op == "generate") return base + "/images/generations" + query;
    return base + "/images/edits" + query;
}

bool channelSupportsModel(const ChannelSpec& ch, std::string_view model) {
    if (ch.models.empty() || model.empty()) return true;
    return std::find(ch.models.begin(), ch.models.end(), model) != ch.models.end();
}

std::string effectiveChannelKey(const ChannelSpec& ch) {
    if (!ch.apiKey.empty()) return ch.apiKey;
    if (!ch.apiKeyEnv.empty()) return pk::readEnvVarUtf8(ch.apiKeyEnv);
    return {};
}

bool isServerFailoverStatus(uint32_t status) noexcept {
    return status == 500 || status == 502 || status == 503 || status == 504 ||
           status == 520 || status == 522 || status == 524;
}

struct LoadBalancingPlugin {
    pk::HostClient host;
    mutable std::mutex mutex;
    RoutingConfig config;
    mutable std::vector<ChannelHealth> health;

    explicit LoadBalancingPlugin(const MemeStudioHostV2* h) : host(h, kPluginId) {
        loadConfigFromDisk();
    }

    pk::fs::path configPath() const {
        return host.dataDir() / L"routing.json";
    }

    void syncOverrideBannerLocked() {
        if (!host.raw()->set_session_endpoint_override) return;
        if (config.takeoverEnabled && !config.channels.empty()) {
            Json banner{
                {"base_url", "https://routed-pool.local/v1"},
                {"api_key", ""},
                {"model", config.channels.front().models.empty() ? "gpt-image-1" : config.channels.front().models.front()}
            };
            std::string dumped = banner.dump();
            host.raw()->set_session_endpoint_override(host.raw()->host_ctx, kPluginId, dumped.c_str());
        } else {
            host.raw()->set_session_endpoint_override(host.raw()->host_ctx, kPluginId, nullptr);
        }
    }

    void applyNewConfig(const Json& j, bool persistToDisk) {
        RoutingConfig next;
        next.takeoverEnabled = j.value("enabled", j.value("takeover_enabled", false));
        next.strategy = j.value("strategy", std::string("weighted"));
        if (next.strategy != "weighted" && next.strategy != "cost") {
            throw pk::PluginError("未知的负载均衡策略：" + next.strategy);
        }
        next.serverFailover = j.value("server_failover", true);
        next.cooldownSeconds = std::clamp(j.value("cooldown_seconds", 60), 1, 3600);

        if (j.contains("channels") && j["channels"].is_array()) {
            if (j["channels"].size() > 64) {
                throw pk::PluginError("渠道池最多支持 64 个渠道");
            }
            for (const auto& cj : j["channels"]) {
                ChannelSpec ch;
                ch.name = pk::trimAscii(cj.value("name", std::string("Channel")));
                if (ch.name.empty() || ch.name.size() > 128) throw pk::PluginError("渠道名称无效");
                ch.baseUrl = pk::trimAscii(cj.at("base_url").get<std::string>());
                (void)resolveTargetUrl(ch.baseUrl, "models");
                ch.apiKey = cj.value("api_key", std::string());
                if (ch.apiKey.empty() && cj.contains("encrypted_key") && cj["encrypted_key"].is_string()) {
                    try { ch.apiKey = host.unprotectSecret(cj["encrypted_key"].get<std::string>()); } catch (...) {}
                }
                ch.apiKeyEnv = cj.value("api_key_env", std::string());
                if (cj.contains("models") && cj["models"].is_array()) {
                    ch.models.clear();
                    for (const auto& m : cj["models"]) {
                        std::string ms = pk::trimAscii(m.get<std::string>());
                        if (!ms.empty()) ch.models.push_back(std::move(ms));
                    }
                }
                ch.priority = std::clamp(cj.value("priority", 0), 0, 99);
                ch.weight = std::clamp(cj.value("weight", 100), 1, 10000);
                ch.unitCost = cj.value("unit_cost", 1.0);
                if (!std::isfinite(ch.unitCost) || ch.unitCost <= 0.0) {
                    throw pk::PluginError("渠道单位成本必须为正数");
                }
                ch.enabled = cj.value("enabled", true);
                next.channels.push_back(std::move(ch));
            }
        }

        if (next.takeoverEnabled && next.channels.empty()) {
            throw pk::PluginError("启用渠道池接管前请至少配置一个有效渠道");
        }

        if (persistToDisk) {
            Json diskJson{
                {"enabled", next.takeoverEnabled},
                {"strategy", next.strategy},
                {"server_failover", next.serverFailover},
                {"cooldown_seconds", next.cooldownSeconds},
                {"channels", Json::array()}
            };
            for (const auto& ch : next.channels) {
                diskJson["channels"].push_back(Json{
                    {"name", ch.name},
                    {"base_url", ch.baseUrl},
                    {"encrypted_key", ch.apiKey.empty() ? "" : host.protectSecret(ch.apiKey)},
                    {"api_key_env", ch.apiKeyEnv},
                    {"models", ch.models},
                    {"priority", ch.priority},
                    {"weight", ch.weight},
                    {"unit_cost", ch.unitCost},
                    {"enabled", ch.enabled}
                });
            }
            std::string dumped = diskJson.dump(2);
            pk::writeFileAtomic(configPath(), std::span<const uint8_t>(
                reinterpret_cast<const uint8_t*>(dumped.data()), dumped.size()));
        }

        std::lock_guard lock(mutex);
        config = std::move(next);
        health.assign(config.channels.size(), ChannelHealth{});
        syncOverrideBannerLocked();
    }

    void loadConfigFromDisk() {
        try {
            auto p = configPath();
            if (!pk::fs::exists(p)) return;
            auto raw = pk::readFileBytes(p, 512 * 1024);
            applyNewConfig(Json::parse(raw.begin(), raw.end()), false);
        } catch (...) {}
    }

    // Selects next candidate channel index not in `triedIndices` using Priority Tier -> Smooth Weighted RR / Min Cost
    int selectNextChannelLocked(std::string_view requestedModel, const std::set<size_t>& triedIndices) const {
        auto now = SteadyClock::now();
        int bestPriority = std::numeric_limits<int>::max();
        std::vector<size_t> tierCandidates;

        for (size_t i = 0; i < config.channels.size(); ++i) {
            if (triedIndices.contains(i)) continue;
            const auto& ch = config.channels[i];
            const auto& st = health[i];
            if (!ch.enabled || st.authDisabled) continue;
            if (st.cooldownUntil > now) continue;
            if (!channelSupportsModel(ch, requestedModel)) continue;
            if (effectiveChannelKey(ch).empty()) continue;

            if (ch.priority < bestPriority) {
                bestPriority = ch.priority;
                tierCandidates.clear();
                tierCandidates.push_back(i);
            } else if (ch.priority == bestPriority) {
                tierCandidates.push_back(i);
            }
        }

        if (tierCandidates.empty()) return -1;
        if (tierCandidates.size() == 1) return static_cast<int>(tierCandidates[0]);

        if (config.strategy == "cost") {
            size_t chosen = tierCandidates[0];
            double bestProj = health[chosen].cumulativeCost + config.channels[chosen].unitCost;
            for (size_t k = 1; k < tierCandidates.size(); ++k) {
                size_t idx = tierCandidates[k];
                double proj = health[idx].cumulativeCost + config.channels[idx].unitCost;
                if (proj < bestProj) {
                    bestProj = proj;
                    chosen = idx;
                }
            }
            return static_cast<int>(chosen);
        }

        // Nginx-style Smooth Weighted Round-Robin (only mutated on primary selection, or cleanly balanced)
        int64_t totalWeight = 0;
        size_t bestIdx = tierCandidates[0];
        for (size_t idx : tierCandidates) {
            health[idx].smoothWeight += config.channels[idx].weight;
            totalWeight += config.channels[idx].weight;
            if (health[idx].smoothWeight > health[bestIdx].smoothWeight) {
                bestIdx = idx;
            }
        }
        health[bestIdx].smoothWeight -= totalWeight;
        return static_cast<int>(bestIdx);
    }

    Json buildStatisticsJson() const {
        std::lock_guard lock(mutex);
        auto now = SteadyClock::now();
        Json arr = Json::array();
        for (size_t i = 0; i < config.channels.size(); ++i) {
            const auto& ch = config.channels[i];
            const auto& st = health[i];
            int64_t cdRem = st.cooldownUntil > now
                ? std::chrono::duration_cast<std::chrono::seconds>(st.cooldownUntil - now).count()
                : 0;
            arr.push_back(Json{
                {"name", ch.name},
                {"enabled", ch.enabled},
                {"priority", ch.priority},
                {"weight", ch.weight},
                {"requests", st.totalRequests},
                {"successes", st.successes},
                {"failovers", st.failovers},
                {"cumulative_cost", st.cumulativeCost},
                {"auth_disabled", st.authDisabled},
                {"cooldown_remaining_seconds", cdRem},
                {"last_error", st.lastError}
            });
        }
        return Json{
            {"enabled", config.takeoverEnabled},
            {"strategy", config.strategy},
            {"channels", arr}
        };
    }

    pk::Bytes executeSingleChannelImageCall(
        const ChannelSpec& ch,
        std::string_view op,
        const Json& baseProvider,
        const std::string& prompt,
        const MemeStudioBytesV2* uploads,
        size_t uploadCount,
        MemeStudioCancelTokenV2* cancel,
        void (MEMESTUDIO_CALL* onPreview)(void*, const uint8_t*, size_t, int32_t),
        void* previewUser,
        uint32_t& outHttpStatus,
        int32_t& outTransportRc,
        std::string& outErrDetail) const {
        std::string key = effectiveChannelKey(ch);
        std::string model = baseProvider.value("model", ch.models.empty() ? std::string("gpt-image-1") : ch.models.front());
        bool stream = baseProvider.value("stream", false);
        int timeoutSec = std::clamp(baseProvider.value("timeout_seconds", 600), 5, 3600);
        std::string url = resolveTargetUrl(ch.baseUrl, op);

        std::string contentType;
        pk::Bytes body;
        if (op == "generate" && uploadCount == 0) {
            contentType = "application/json";
            Json jb{{"model", model}, {"prompt", prompt}};
            if (stream) {
                jb["stream"] = true;
                jb["partial_images"] = std::clamp(baseProvider.value("partial_images", 2), 1, 3);
            }
            std::string dumped = jb.dump();
            body.assign(dumped.begin(), dumped.end());
        } else {
            std::string boundary = "MemeRoute" + pk::randomHex(12);
            contentType = "multipart/form-data; boundary=" + boundary;
            std::string imageField = baseProvider.value("image_field", std::string("image[]"));
            auto appendStr = [&](std::string_view s) { body.insert(body.end(), s.begin(), s.end()); };
            appendStr("--" + boundary + "\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n" + model + "\r\n");
            appendStr("--" + boundary + "\r\nContent-Disposition: form-data; name=\"prompt\"\r\n\r\n" + prompt + "\r\n");
            for (size_t u = 0; u < uploadCount; ++u) {
                appendStr("--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + imageField +
                          "\"; filename=\"image_" + std::to_string(u) + ".png\"\r\nContent-Type: image/png\r\n\r\n");
                body.insert(body.end(), uploads[u].data, uploads[u].data + uploads[u].size);
                appendStr("\r\n");
            }
            appendStr("--" + boundary + "--\r\n");
        }

        Json headers{
            {"Authorization", "Bearer " + key},
            {"Content-Type", contentType}
        };
        if (stream) headers["Accept"] = "text/event-stream";

        Json httpSpec{
            {"url", url},
            {"method", "POST"},
            {"timeout_seconds", timeoutSec},
            {"expect_event_stream", stream},
            {"headers", headers}
        };
        std::string httpSpecStr = httpSpec.dump();

        struct SseRouteState {
            void (MEMESTUDIO_CALL* cb)(void*, const uint8_t*, size_t, int32_t) = nullptr;
            void* user = nullptr;
            pk::Bytes finalBytes;
            std::string sseErr;
        } sseSt{onPreview, previewUser};

        auto sseThunk = [](void* u, const char* evType, const char* evData) {
            auto* st = static_cast<SseRouteState*>(u);
            if (!st || !evData) return;
            auto j = Json::parse(evData, nullptr, false);
            if (!j.is_object()) return;
            std::string t = j.value("type", std::string(evType ? evType : ""));
            if (j.contains("error") || t == "error") {
                st->sseErr = j.dump();
            } else if ((t == "image_edit.partial_image" || t == "image_generation.partial_image") &&
                       st->cb && j.contains("b64_json") && j["b64_json"].is_string()) {
                auto raw = pk::base64Decode(j["b64_json"].get<std::string>());
                st->cb(st->user, raw.data(), raw.size(), j.value("partial_image_index", 0));
            } else if ((t == "image_edit.completed" || t == "image_generation.completed") &&
                       j.contains("b64_json") && j["b64_json"].is_string()) {
                st->finalBytes = pk::base64Decode(j["b64_json"].get<std::string>());
            }
        };

        outHttpStatus = 0;
        int32_t wasStreamed = 0;
        MemeStudioBytesV2 respBytes{};
        MemeStudioBytesV2 errBytes{};
        outTransportRc = host.raw()->http_send(
            host.raw()->host_ctx, httpSpecStr.c_str(),
            body.data(), body.size(), cancel,
            sseThunk, &sseSt, &outHttpStatus, &wasStreamed, &respBytes, &errBytes);
        outErrDetail = host.consumeBytesToString(errBytes);
        pk::Bytes respVec = host.consumeBytesToVec(respBody_fix_helper(respBytes));
        if (outTransportRc != 0) {
            return {};
        }

        if (wasStreamed) {
            if (!sseSt.sseErr.empty()) {
                outHttpStatus = 502;
                outErrDetail = sseSt.sseErr;
                return {};
            }
            if (sseSt.finalBytes.empty()) {
                outTransportRc = -3;
                outErrDetail = "流式响应未包含最终图像";
                return {};
            }
            return std::move(sseSt.finalBytes);
        }

        auto parsed = Json::parse(respVec.begin(), respVec.end(), nullptr, false);
        if (!parsed.is_object()) {
            if (outHttpStatus == 200) outHttpStatus = 502;
            outErrDetail = "非 JSON 响应";
            return {};
        }
        // Catch HTTP 200 wrapping {"error": ...} so failover triggers properly! (BUG-22 fix)
        if (outHttpStatus < 200 || outHttpStatus >= 300 || parsed.contains("error")) {
            if (outHttpStatus >= 200 && outHttpStatus < 300) outHttpStatus = 502;
            outErrDetail = parsed.contains("error") ? parsed["error"].dump() : parsed.dump();
            return {};
        }
        if (!parsed.contains("data") || !parsed["data"].is_array() || parsed["data"].empty() ||
            !parsed["data"][0].contains("b64_json")) {
            outHttpStatus = 502;
            outErrDetail = "响应缺少 data[0].b64_json";
            return {};
        }
        return pk::base64Decode(parsed["data"][0]["b64_json"].get<std::string>());
    }

    static MemeStudioBytesV2& respBody_fix_helper(MemeStudioBytesV2& b) noexcept { return b; }

    int32_t interceptCall(
        std::string_view op,
        const Json& providerJson,
        const Json& reqJson,
        const MemeStudioBytesV2* uploads,
        size_t uploadCount,
        MemeStudioCancelTokenV2* cancel,
        void (MEMESTUDIO_CALL* onPreview)(void*, const uint8_t*, size_t, int32_t),
        void* previewUser,
        MemeStudioBytesV2* outResp,
        MemeStudioBytesV2* outErr) {
        {
            std::lock_guard lock(mutex);
            if (!config.takeoverEnabled) {
                return 1; // Pass-through to Host single-endpoint client
            }
        }

        if (op == "models") {
            std::lock_guard lock(mutex);
            std::set<std::string> unionModels;
            for (const auto& ch : config.channels) {
                if (!ch.enabled) continue;
                for (const auto& m : ch.models) unionModels.insert(m);
            }
            Json arr = Json::array();
            for (const auto& m : unionModels) arr.push_back(m);
            host.writeOutBytes(outResp, arr.dump());
            return 0;
        }

        std::string requestedModel = pk::trimAscii(providerJson.value("model", std::string()));
        std::string prompt = reqJson.value("prompt", std::string());
        std::set<size_t> tried;
        std::string lastFailureMsg = "渠道池中没有匹配模型且处于健康状态的可用渠道";

        for (;;) {
            host.verifyNotCancelled(cancel);
            ChannelSpec selectedSpec;
            size_t selectedIdx = 0;
            bool allowServerFailover = true;
            int cooldownSec = 60;

            {
                std::lock_guard lock(mutex);
                int idx = selectNextChannelLocked(requestedModel, tried);
                if (idx < 0) break;
                selectedIdx = static_cast<size_t>(idx);
                tried.insert(selectedIdx);
                selectedSpec = config.channels[selectedIdx];
                allowServerFailover = config.serverFailover;
                cooldownSec = config.cooldownSeconds;
                ++health[selectedIdx].totalRequests;
            }

            uint32_t httpStatus = 0;
            int32_t transportRc = 0;
            std::string errDetail;
            pk::Bytes imgBytes = executeSingleChannelImageCall(
                selectedSpec, op, providerJson, prompt, uploads, uploadCount,
                cancel, onPreview, previewUser, httpStatus, transportRc, errDetail);

            if (transportRc == -2) throw pk::PluginCancelled();

            if (!imgBytes.empty()) {
                std::lock_guard lock(mutex);
                ++health[selectedIdx].successes;
                health[selectedIdx].cumulativeCost += config.channels[selectedIdx].unitCost;
                health[selectedIdx].lastError.clear();
                host.writeOutRawBytes(outResp, imgBytes);
                return 0;
            }

            // Classify failure & update persistent channel health state (BUG-22 fix!)
            bool canFailover = false;
            {
                std::lock_guard lock(mutex);
                auto& st = health[selectedIdx];
                st.lastError = errDetail;

                if (transportRc == -3) {
                    // Network / DNS / TLS / Timeout -> enter cooldown & failover!
                    st.cooldownUntil = SteadyClock::now() + std::chrono::seconds(cooldownSec);
                    ++st.failovers;
                    canFailover = true;
                } else if (httpStatus == 401 || httpStatus == 403) {
                    st.authDisabled = true;
                    ++st.failovers;
                    canFailover = true;
                } else if (httpStatus == 429) {
                    st.cooldownUntil = SteadyClock::now() + std::chrono::seconds(cooldownSec);
                    ++st.failovers;
                    canFailover = true;
                } else if (isServerFailoverStatus(httpStatus) && allowServerFailover) {
                    st.cooldownUntil = SteadyClock::now() + std::chrono::seconds(cooldownSec);
                    ++st.failovers;
                    canFailover = true;
                }
            }

            lastFailureMsg = "渠道 [" + selectedSpec.name + "] 请求失败：" +
                             (httpStatus ? ("HTTP " + std::to_string(httpStatus) + " ") : "") + errDetail;
            if (!canFailover) {
                break; // Non-retryable client error (e.g. HTTP 400 invalid prompt/parameter)
            }
        }

        host.writeOutBytes(outErr, lastFailureMsg);
        return -1;
    }

    // Fault-isolated multi-channel comparison (fixes BUG-23 collateral abort & one-shot window!)
    Json runComparison(const Json& args, MemeStudioCancelTokenV2* cancel) {
        std::string prompt = pk::trimAscii(args.value("prompt", std::string()));
        if (prompt.empty()) throw pk::PluginError("请填写对比测试提示词");

        pk::fs::path outDir = args.contains("output_directory") && args["output_directory"].is_string()
            ? pk::fs::path(pk::utf8ToWideStrict(args["output_directory"].get<std::string>()))
            : (host.dataDir() / L"comparisons" / pk::utf8ToWideStrict(pk::randomHex(6)));
        pk::fs::create_directories(outDir);

        std::vector<ChannelSpec> snapshot;
        {
            std::lock_guard lock(mutex);
            snapshot = config.channels;
        }

        std::set<std::string> dedupEndpointModel;
        Json reportItems = Json::array();
        int idxCounter = 0;

        for (const auto& ch : snapshot) {
            host.verifyNotCancelled(cancel);
            if (!ch.enabled || effectiveChannelKey(ch).empty()) continue;
            std::string model = ch.models.empty() ? "gpt-image-1" : ch.models.front();
            std::string dedupKey = ch.baseUrl + "|" + model;
            if (!dedupEndpointModel.insert(dedupKey).second) continue;

            ++idxCounter;
            uint32_t status = 0;
            int32_t trc = 0;
            std::string errDetail;
            Json prov{{"model", model}, {"timeout_seconds", args.value("timeout_seconds", 120)}};

            // Every channel executes in complete fault isolation — timeout/500/invalid JSON on Channel 1
            // NEVER aborts Channel 2..N! (Critical BUG-23 fix)
            pk::Bytes img = executeSingleChannelImageCall(
                ch, "generate", prov, prompt, nullptr, 0, cancel, nullptr, nullptr, status, trc, errDetail);
            if (trc == -2) throw pk::PluginCancelled();

            if (!img.empty()) {
                pk::fs::path fileOut = outDir / (L"channel_" + std::to_wstring(idxCounter) + L".png");
                pk::writeFileAtomic(fileOut, img);
                reportItems.push_back(Json{
                    {"channel", ch.name},
                    {"model", model},
                    {"status", "succeeded"},
                    {"output", pk::wideToUtf8(fileOut.wstring())}
                });
            } else {
                reportItems.push_back(Json{
                    {"channel", ch.name},
                    {"model", model},
                    {"status", "failed"},
                    {"http_status", status},
                    {"error", errDetail}
                });
            }
        }

        Json fullReport{
            {"output_directory", pk::wideToUtf8(outDir.wstring())},
            {"results", reportItems}
        };
        std::string reportStr = fullReport.dump(2);
        pk::writeFileAtomic(outDir / L"report.json", std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(reportStr.data()), reportStr.size()));
        return fullReport;
    }
};

enum RoutingPanelId : int {
    IdTakeoverCheck   = 3601,
    IdStrategyCombo   = 3602,
    IdServerFailCheck = 3603,
    IdCooldownEdit    = 3604,
    IdChannelsLv      = 3605,
    IdAddChannelBtn   = 3606,
    IdDelChannelBtn   = 3607,
    IdResetHealthBtn  = 3608,

    IdChEnabledCheck  = 3610,
    IdChNameEdit      = 3611,
    IdChUrlEdit       = 3612,
    IdChKeyEdit       = 3613,
    IdChModelsEdit    = 3614,
    IdChPriorityEdit  = 3615,
    IdChWeightEdit    = 3616,
    IdChCostEdit      = 3617,

    IdComparePrompt   = 3620,
    IdRunCompareBtn   = 3621,
    IdSaveRoutingBtn  = 3622,
    IdCloseRoutingBtn = 3623
};

constexpr UINT WM_COMPARE_DONE = WM_APP + 44;

struct RoutingPanelContext {
    LoadBalancingPlugin* plugin = nullptr;
    HWND hwnd = nullptr;
    HFONT font = nullptr;

    HWND takeoverCheck = nullptr, strategyCombo = nullptr, serverFailCheck = nullptr, cooldownEdit = nullptr;
    HWND channelsLv = nullptr;
    HWND chEnabled = nullptr, chName = nullptr, chUrl = nullptr, chKey = nullptr;
    HWND chModels = nullptr, chPriority = nullptr, chWeight = nullptr, chCost = nullptr;
    HWND cmpPrompt = nullptr, cmpBtn = nullptr, saveBtn = nullptr, statusLabel = nullptr;

    RoutingConfig draft;
    int currentIdx = -1;
    bool populatingDetail = false;

    std::thread worker;
    bool busy = false;
    std::mutex mtx;
    Json cmpReport = Json::object();
    std::string cmpErr;

    ~RoutingPanelContext() {
        if (worker.joinable()) worker.join();
        if (font) DeleteObject(font);
    }

    static std::string joinModels(const std::vector<std::string>& models) {
        std::string s;
        for (size_t i = 0; i < models.size(); ++i) {
            if (i > 0) s += ", ";
            s += models[i];
        }
        return s;
    }

    static std::vector<std::string> splitModels(const std::string& raw) {
        std::vector<std::string> out;
        std::istringstream ss(raw);
        std::string item;
        while (std::getline(ss, item, ',')) {
            std::string clean = pk::trimAscii(item);
            if (!clean.empty()) out.push_back(std::move(clean));
        }
        if (out.empty()) out.push_back("gpt-image-1");
        return out;
    }

    // Automatically syncs edited detail fields back into draft.channels[currentIdx] (fixes BUG-07!)
    void flushCurrentChannelEdits(bool strictValidation) {
        if (populatingDetail || currentIdx < 0 || static_cast<size_t>(currentIdx) >= draft.channels.size()) {
            return;
        }
        auto& ch = draft.channels[static_cast<size_t>(currentIdx)];
        ch.enabled = pk::isButtonChecked(chEnabled);
        std::string nm = pk::trimAscii(pk::getWindowTextUtf8(chName));
        if (!nm.empty()) ch.name = nm;
        else if (strictValidation) throw pk::PluginError("渠道名称不能为空");

        std::string u = pk::trimAscii(pk::getWindowTextUtf8(chUrl));
        if (!u.empty()) ch.baseUrl = u;
        else if (strictValidation) throw pk::PluginError("渠道接口地址不能为空");

        ch.apiKey = pk::trimAscii(pk::getWindowTextUtf8(chKey));
        ch.models = splitModels(pk::getWindowTextUtf8(chModels));

        try { ch.priority = pk::parseIntField(chPriority, "优先级", 0, 99); }
        catch (...) { if (strictValidation) throw; }

        try { ch.weight = pk::parseIntField(chWeight, "权重", 1, 10000); }
        catch (...) { if (strictValidation) throw; }

        try { ch.unitCost = pk::parseDoubleField(chCost, "单位成本", 0.000001, 1000000.0); }
        catch (...) { if (strictValidation) throw; }
    }

    void loadChannelIntoDetail(int idx) {
        populatingDetail = true;
        currentIdx = idx;
        bool valid = (idx >= 0 && static_cast<size_t>(idx) < draft.channels.size());
        for (HWND h : {chEnabled, chName, chUrl, chKey, chModels, chPriority, chWeight, chCost}) {
            EnableWindow(h, valid ? TRUE : FALSE);
        }
        if (valid) {
            const auto& ch = draft.channels[static_cast<size_t>(idx)];
            pk::setButtonChecked(chEnabled, ch.enabled);
            pk::setWindowTextUtf8(chName, ch.name);
            pk::setWindowTextUtf8(chUrl, ch.baseUrl);
            pk::setWindowTextUtf8(chKey, ch.apiKey);
            pk::setWindowTextUtf8(chModels, joinModels(ch.models));
            pk::setWindowTextUtf8(chPriority, std::to_string(ch.priority));
            pk::setWindowTextUtf8(chWeight, std::to_string(ch.weight));
            char costBuf[32]{};
            std::snprintf(costBuf, sizeof(costBuf), "%.4g", ch.unitCost);
            pk::setWindowTextUtf8(chCost, costBuf);
        } else {
            for (HWND h : {chName, chUrl, chKey, chModels, chPriority, chWeight, chCost}) {
                pk::setWindowTextUtf8(h, "");
            }
        }
        populatingDetail = false;
    }

    void refreshChannelsTable() {
        ListView_DeleteAllItems(channelsLv);
        Json stats = plugin->buildStatisticsJson();
        const auto& statRows = stats.value("channels", Json::array());

        for (size_t i = 0; i < draft.channels.size(); ++i) {
            const auto& ch = draft.channels[i];
            std::wstring wname = pk::utf8ToWideLossy((ch.enabled ? "✓ " : "○ ") + ch.name);
            LVITEMW lvi{};
            lvi.mask = LVIF_TEXT;
            lvi.iItem = static_cast<int>(i);
            lvi.pszText = wname.data();
            ListView_InsertItem(channelsLv, &lvi);

            std::wstring wurl = pk::utf8ToWideLossy(ch.baseUrl);
            std::wstring wpw = L"P" + std::to_wstring(ch.priority) + L" / W" + std::to_wstring(ch.weight);
            std::wstring wmod = pk::utf8ToWideLossy(joinModels(ch.models));

            std::wstring wcnt = L"0 / 0 / 0";
            std::wstring whealth = ch.enabled ? L"正常" : L"已停用";
            if (i < statRows.size()) {
                const auto& sr = statRows[i];
                wcnt = std::to_wstring(sr.value("requests", 0)) + L" / " +
                       std::to_wstring(sr.value("successes", 0)) + L" / " +
                       std::to_wstring(sr.value("failovers", 0));
                if (sr.value("auth_disabled", false)) whealth = L"鉴权熔断";
                else if (sr.value("cooldown_remaining_seconds", 0) > 0) {
                    whealth = L"冷却 (" + std::to_wstring(sr.value("cooldown_remaining_seconds", 0)) + L"s)";
                }
            }

            ListView_SetItemText(channelsLv, static_cast<int>(i), 1, wurl.data());
            ListView_SetItemText(channelsLv, static_cast<int>(i), 2, wpw.data());
            ListView_SetItemText(channelsLv, static_cast<int>(i), 3, wmod.data());
            ListView_SetItemText(channelsLv, static_cast<int>(i), 4, wcnt.data());
            ListView_SetItemText(channelsLv, static_cast<int>(i), 5, whealth.data());
        }
    }

    Json buildConfigPayloadFromForm() {
        flushCurrentChannelEdits(true);
        int cd = pk::parseIntField(cooldownEdit, "故障冷却秒数", 1, 3600);
        bool takeover = pk::isButtonChecked(takeoverCheck);
        std::string strat = (SendMessageW(strategyCombo, CB_GETCURSEL, 0, 0) == 1) ? "cost" : "weighted";
        bool srvFail = pk::isButtonChecked(serverFailCheck);

        Json chArr = Json::array();
        for (const auto& c : draft.channels) {
            chArr.push_back(Json{
                {"name", c.name},
                {"base_url", c.baseUrl},
                {"api_key", c.apiKey},
                {"api_key_env", c.apiKeyEnv},
                {"models", c.models},
                {"priority", c.priority},
                {"weight", c.weight},
                {"unit_cost", c.unitCost},
                {"enabled", c.enabled}
            });
        }
        return Json{
            {"enabled", takeover},
            {"strategy", strat},
            {"server_failover", srvFail},
            {"cooldown_seconds", cd},
            {"channels", chArr}
        };
    }

    static intptr_t MEMESTUDIO_CALL wndProc(void* ctx, void* rawHwnd, uint32_t msg, uintptr_t wp, intptr_t lp) {
        auto* st = static_cast<RoutingPanelContext*>(ctx);
        HWND w = static_cast<HWND>(rawHwnd);
        if (msg == WM_CREATE) {
            st->hwnd = w;
            pk::PanelBuilder ui(w);
            st->font = pk::createDpiUiFont(ui.dpi().dpi, 14);

            {
                std::lock_guard lk(st->plugin->mutex);
                st->draft = st->plugin->config;
            }

            st->takeoverCheck = ui.checkbox(L"启用渠道池透明接管主窗口请求", IdTakeoverCheck, 16, 14, 240, 24, st->draft.takeoverEnabled);
            ui.label(L"调度策略：", 268, 16, 78, 20);
            st->strategyCombo = ui.combo(IdStrategyCombo, 346, 12, 160, 140, {L"平滑加权轮询 (Weighted)", L"最低累计成本 (Cost)"},
                                         st->draft.strategy == "cost" ? 1 : 0);
            st->serverFailCheck = ui.checkbox(L"5xx / 超时自动故障切换", IdServerFailCheck, 520, 14, 186, 24, st->draft.serverFailover);
            ui.label(L"冷却(s)：", 714, 16, 62, 20);
            st->cooldownEdit = ui.edit(IdCooldownEdit, 776, 12, 68, 26, std::to_string(st->draft.cooldownSeconds), ES_NUMBER);

            st->channelsLv = ui.reportListView(IdChannelsLv, 16, 48, 828, 210, {
                {L"渠道名称", 135},
                {L"接口地址 (Base URL)", 215},
                {L"优先级/权重", 95},
                {L"支持模型", 145},
                {L"请求/成功/切换", 115},
                {L"运行状态", 95}
            });

            ui.button(L"＋ 新增渠道", IdAddChannelBtn, 16, 266, 106, 28);
            ui.button(L"－ 删除选中", IdDelChannelBtn, 128, 266, 106, 28);
            ui.button(L"重置冷却与统计", IdResetHealthBtn, 240, 266, 126, 28);

            // Real-time bound detail editor
            st->chEnabled = ui.checkbox(L"启用当前选中渠道", IdChEnabledCheck, 16, 306, 160, 22, true);
            ui.label(L"渠道名称：", 190, 308, 80, 20);
            st->chName = ui.edit(IdChNameEdit, 270, 304, 200, 26);
            ui.label(L"API 密钥：", 486, 308, 80, 20);
            st->chKey = ui.edit(IdChKeyEdit, 566, 304, 278, 26, "", ES_PASSWORD | ES_AUTOHSCROLL);

            ui.label(L"接口地址：", 16, 344, 80, 20);
            st->chUrl = ui.edit(IdChUrlEdit, 96, 340, 374, 26);
            ui.label(L"模型列表：", 486, 344, 80, 20);
            st->chModels = ui.edit(IdChModelsEdit, 566, 340, 278, 26);

            ui.label(L"优先级(0最高)：", 16, 380, 108, 20);
            st->chPriority = ui.edit(IdChPriorityEdit, 124, 376, 80, 26, "0", ES_NUMBER);
            ui.label(L"权重(1..10000)：", 220, 380, 118, 20);
            st->chWeight = ui.edit(IdChWeightEdit, 338, 376, 90, 26, "100", ES_NUMBER);
            ui.label(L"单位成本：", 448, 380, 80, 20);
            st->chCost = ui.edit(IdChCostEdit, 528, 376, 96, 26, "1.0");

            // Integrated Multi-Channel Comparison Bar (ES_WANTRETURN + repeatable!)
            ui.label(L"多渠道出图横向对比提示词（各唯一渠道独立运行，单渠道异常不连坐）：", 16, 418, 600, 20);
            st->cmpPrompt = ui.multilineEdit(IdComparePrompt, 16, 442, 686, 68, "A cheerful cartoon mascot holding a star sticker, clean white background.", false);
            st->cmpBtn = ui.button(L"运行多渠道对比", IdRunCompareBtn, 712, 442, 132, 34);

            st->statusLabel = ui.label(L"提示：编辑渠道属性后点击「保存并应用」将自动同步当前编辑项", 16, 526, 590, 24);
            st->saveBtn = ui.button(L"保存并应用", IdSaveRoutingBtn, 624, 520, 118, 34, true);
            ui.button(L"关闭", IdCloseRoutingBtn, 750, 520, 94, 34, false);

            ui.applyFont(st->font);
            st->refreshChannelsTable();
            if (!st->draft.channels.empty()) {
                ListView_SetItemState(st->channelsLv, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                st->loadChannelIntoDetail(0);
            } else {
                st->loadChannelIntoDetail(-1);
            }
            return 0;
        }
        if (msg == WM_NOTIFY && !st->busy) {
            auto* hdr = reinterpret_cast<NMHDR*>(lp);
            if (hdr && hdr->idFrom == IdChannelsLv && hdr->code == LVN_ITEMCHANGED) {
                auto* nmlv = reinterpret_cast<NMLISTVIEW*>(lp);
                if ((nmlv->uChanged & LVIF_STATE) && (nmlv->uNewState & LVIS_SELECTED)) {
                    st->flushCurrentChannelEdits(false);
                    st->loadChannelIntoDetail(nmlv->iItem);
                }
            }
        }
        if (msg == WM_COMMAND && !st->busy) {
            int id = LOWORD(wp);
            if (id == IdAddChannelBtn) {
                st->flushCurrentChannelEdits(false);
                ChannelSpec ch;
                ch.name = "Channel-" + std::to_string(st->draft.channels.size() + 1);
                st->draft.channels.push_back(std::move(ch));
                st->refreshChannelsTable();
                int newIdx = static_cast<int>(st->draft.channels.size()) - 1;
                ListView_SetItemState(st->channelsLv, newIdx, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                st->loadChannelIntoDetail(newIdx);
                return 0;
            }
            if (id == IdDelChannelBtn) {
                if (st->currentIdx >= 0 && static_cast<size_t>(st->currentIdx) < st->draft.channels.size()) {
                    st->populatingDetail = true;
                    st->draft.channels.erase(st->draft.channels.begin() + st->currentIdx);
                    st->populatingDetail = false;
                    st->currentIdx = -1;
                    st->refreshChannelsTable();
                    int nextSel = st->draft.channels.empty() ? -1 : 0;
                    if (nextSel >= 0) {
                        ListView_SetItemState(st->channelsLv, nextSel, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                    }
                    st->loadChannelIntoDetail(nextSel);
                }
                return 0;
            }
            if (id == IdResetHealthBtn) {
                {
                    std::lock_guard lk(st->plugin->mutex);
                    st->plugin->health.assign(st->plugin->config.channels.size(), ChannelHealth{});
                }
                st->refreshChannelsTable();
                pk::setWindowTextUtf8(st->statusLabel, "已清空所有渠道的冷却计时与运行统计");
                return 0;
            }
            if (id == IdRunCompareBtn) {
                try {
                    Json payload = st->buildConfigPayloadFromForm();
                    st->plugin->applyNewConfig(payload, true);
                    std::string prompt = pk::trimAscii(pk::getWindowTextUtf8(st->cmpPrompt));
                    if (prompt.empty()) throw pk::PluginError("请填写对比测试提示词");

                    if (st->worker.joinable()) st->worker.join();
                    st->busy = true;
                    EnableWindow(st->cmpBtn, FALSE);
                    EnableWindow(st->saveBtn, FALSE);
                    pk::setWindowTextUtf8(st->statusLabel, "正在各启用渠道间独立执行横向出图对比…");

                    st->worker = std::thread([st, prompt = std::move(prompt)]() {
                        Json rep;
                        std::string err;
                        try {
                            rep = st->plugin->runComparison(Json{{"prompt", prompt}}, nullptr);
                        } catch (const std::exception& e) {
                            err = e.what();
                        }
                        {
                            std::lock_guard lk(st->mtx);
                            st->cmpReport = std::move(rep);
                            st->cmpErr = std::move(err);
                        }
                        if (st->hwnd && IsWindow(st->hwnd)) PostMessageW(st->hwnd, WM_COMPARE_DONE, 0, 0);
                    });
                } catch (const std::exception& e) {
                    pk::showPanelError(w, e.what(), L"多渠道对比");
                }
                return 0;
            }
            if (id == IdSaveRoutingBtn) {
                try {
                    Json payload = st->buildConfigPayloadFromForm();
                    st->plugin->applyNewConfig(payload, true);
                    DestroyWindow(w);
                } catch (const std::exception& e) {
                    pk::showPanelError(w, e.what(), L"保存渠道池配置");
                }
                return 0;
            }
            if (id == IdCloseRoutingBtn || id == IDCANCEL) {
                DestroyWindow(w);
                return 0;
            }
        }
        if (msg == WM_COMPARE_DONE) {
            if (st->worker.joinable()) st->worker.join();
            st->busy = false;
            EnableWindow(st->cmpBtn, TRUE); // Repeatable — never permanently disabled!
            EnableWindow(st->saveBtn, TRUE);

            Json rep;
            std::string err;
            {
                std::lock_guard lk(st->mtx);
                rep = st->cmpReport;
                err = st->cmpErr;
            }
            if (!err.empty()) {
                pk::setWindowTextUtf8(st->statusLabel, "多渠道对比执行失败");
                pk::showPanelError(w, err, L"多渠道对比失败");
            } else {
                std::string outDir = rep.value("output_directory", std::string());
                pk::setWindowTextUtf8(st->statusLabel, "多渠道对比完成，结果与 report.json 已保存至：" + outDir);
                if (!outDir.empty()) {
                    ShellExecuteW(w, L"open", pk::utf8ToWideLossy(outDir).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
            }
            return 0;
        }
        if (msg == WM_CLOSE) {
            if (!st->busy) DestroyWindow(w);
            return 0;
        }
        return DefWindowProcW(w, msg, wp, lp);
    }

    static void MEMESTUDIO_CALL onDestroy(void* ctx) { delete static_cast<RoutingPanelContext*>(ctx); }
};

int32_t MEMESTUDIO_CALL abiCreate(const MemeStudioHostV2* host, void** outInst, MemeStudioBytesV2* outErr) {
    if (!host || !outInst || host->abi_version != MEMESTUDIO_PLUGIN_ABI_V2) return -1;
    try {
        *outInst = new LoadBalancingPlugin(host);
        return 0;
    } catch (const std::exception& e) {
        pk::HostClient(host, kPluginId).writeOutBytes(outErr, e.what());
        return -1;
    }
}

void MEMESTUDIO_CALL abiDestroy(void* inst) { delete static_cast<LoadBalancingPlugin*>(inst); }

int32_t MEMESTUDIO_CALL abiInvokeCommand(
    void* rawInst, const char* commandId, const char* argsJson, void* ownerHwnd,
    MemeStudioCancelTokenV2* cancel, MemeStudioBytesV2* outResult, MemeStudioBytesV2* outErr) {
    auto* self = static_cast<LoadBalancingPlugin*>(rawInst);
    if (!self || !commandId) return -1;
    try {
        std::string cmd(commandId);
        Json args = (argsJson && argsJson[0]) ? Json::parse(argsJson) : Json::object();

        if (cmd == "open-panel") {
            auto* pctx = new RoutingPanelContext{self};
            if (self->host.raw()->open_panel(
                    self->host.raw()->host_ctx, "api-load-balancing", ownerHwnd,
                    860, 570, "多渠道均衡与故障转移",
                    RoutingPanelContext::wndProc, pctx, RoutingPanelContext::onDestroy) != 0) {
                delete pctx;
                throw pk::PluginError("无法打开渠道池配置面板");
            }
            self->host.writeOutBytes(outResult, R"({"opened":true})");
            return 0;
        }
        if (cmd == "configure") {
            self->applyNewConfig(args, true);
            self->host.writeOutBytes(outResult, self->buildStatisticsJson().dump());
            return 0;
        }
        if (cmd == "statistics") {
            self->host.writeOutBytes(outResult, self->buildStatisticsJson().dump());
            return 0;
        }
        if (cmd == "reset-health") {
            {
                std::lock_guard lock(self->mutex);
                self->health.assign(self->config.channels.size(), ChannelHealth{});
            }
            self->host.writeOutBytes(outResult, self->buildStatisticsJson().dump());
            return 0;
        }
        if (cmd == "compare") {
            Json cmp = self->runComparison(args, cancel);
            self->host.writeOutBytes(outResult, cmp.dump());
            return 0;
        }
        throw pk::PluginError("未知的渠道均衡命令：" + cmd);
    } catch (const std::exception& e) {
        self->host.writeOutBytes(outErr, e.what());
        return -1;
    }
}

int32_t MEMESTUDIO_CALL abiInterceptImageApi(
    void* rawInst,
    const char* op,
    const char* providerJson,
    const char* requestJson,
    const MemeStudioBytesV2* uploads,
    size_t uploadCount,
    MemeStudioCancelTokenV2* cancel,
    void (MEMESTUDIO_CALL* onPreview)(void*, const uint8_t*, size_t, int32_t),
    void* user,
    MemeStudioBytesV2* outResp,
    MemeStudioBytesV2* outErr) {
    auto* self = static_cast<LoadBalancingPlugin*>(rawInst);
    if (!self || !op) return -1;
    try {
        Json prov = (providerJson && providerJson[0]) ? Json::parse(providerJson) : Json::object();
        Json req = (requestJson && requestJson[0]) ? Json::parse(requestJson) : Json::object();
        return self->interceptCall(op, prov, req, uploads, uploadCount, cancel, onPreview, user, outResp, outErr);
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
    abiInterceptImageApi
};

} // namespace

extern "C" MEMESTUDIO_EXPORT const MemeStudioPluginV2* MEMESTUDIO_CALL MemeStudioGetPluginV2(void) {
    return &kDescriptor;
}
