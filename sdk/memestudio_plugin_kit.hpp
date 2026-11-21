#pragma once
#include "memestudio_plugin.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include <shobjidl.h>
#include <wincrypt.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace meme::plugin_kit {

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

struct PluginError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct PluginCancelled : PluginError {
    PluginCancelled() : PluginError("任务已取消") {}
};

// ==================== UTF-8 / UTF-16 & Text Utilities ====================

inline std::string trimAscii(std::string_view text) {
    auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    auto last = text.find_last_not_of(" \t\r\n");
    return std::string(text.substr(first, last - first + 1));
}

inline std::wstring utf8ToWideStrict(std::string_view text) {
    if (text.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (len <= 0) throw PluginError("无效的 UTF-8 字符串");
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), out.data(), len);
    return out;
}

inline std::wstring utf8ToWideLossy(std::string_view text) {
    if (text.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    UINT cp = CP_UTF8;
    if (len <= 0) {
        cp = CP_ACP;
        len = MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        if (len <= 0) return L"[无法解码的日志内容]";
    }
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(cp, 0, text.data(), static_cast<int>(text.size()), out.data(), len);
    return out;
}

inline std::string wideToUtf8(std::wstring_view text) {
    if (text.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) throw PluginError("无效的 UTF-16 字符串");
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), out.data(), len, nullptr, nullptr);
    return out;
}

// ==================== Crypto, Random, Path & Atomic File Helpers ====================

inline std::string randomHex(size_t byteCount = 12) {
    Bytes buf(byteCount);
    if (BCryptGenRandom(nullptr, buf.data(), static_cast<ULONG>(buf.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
        throw PluginError("系统随机数生成失败");
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out(byteCount * 2, '\0');
    for (size_t i = 0; i < byteCount; ++i) {
        out[i * 2]     = kHex[(buf[i] >> 4) & 0x0F];
        out[i * 2 + 1] = kHex[buf[i] & 0x0F];
    }
    return out;
}

inline std::string sha256Hex(std::span<const uint8_t> data) {
    uint8_t digest[32]{};
    if (BCryptHash(
            BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
            const_cast<PUCHAR>(data.empty() ? reinterpret_cast<const uint8_t*>("") : data.data()),
            static_cast<ULONG>(data.size()), digest, sizeof(digest)) < 0) {
        throw PluginError("计算 SHA-256 失败");
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out(64, '\0');
    for (size_t i = 0; i < 32; ++i) {
        out[i * 2]     = kHex[(digest[i] >> 4) & 0x0F];
        out[i * 2 + 1] = kHex[digest[i] & 0x0F];
    }
    return out;
}

inline std::string base64Encode(std::span<const uint8_t> data) {
    if (data.empty()) return {};
    DWORD sz = 0;
    if (!CryptBinaryToStringA(data.data(), static_cast<DWORD>(data.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &sz) || !sz) {
        throw PluginError("Base64 编码失败");
    }
    std::string out(sz, '\0');
    if (!CryptBinaryToStringA(data.data(), static_cast<DWORD>(data.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, out.data(), &sz)) {
        throw PluginError("Base64 编码失败");
    }
    out.resize(sz);
    while (!out.empty() && out.back() == '\0') out.pop_back();
    return out;
}

inline Bytes base64Decode(std::string_view input) {
    if (input.starts_with("data:")) {
        auto comma = input.find(',');
        if (comma == std::string_view::npos) throw PluginError("无效的 Data URL");
        input.remove_prefix(comma + 1);
    }
    while (!input.empty() && (input.front() == ' ' || input.front() == '\r' || input.front() == '\n' || input.front() == '\t')) input.remove_prefix(1);
    while (!input.empty() && (input.back() == ' ' || input.back() == '\r' || input.back() == '\n' || input.back() == '\t')) input.remove_suffix(1);
    if (input.empty()) return {};
    DWORD sz = 0;
    if (!CryptStringToBinaryA(input.data(), static_cast<DWORD>(input.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_STRICT, nullptr, &sz, nullptr, nullptr)) {
        throw PluginError("无效的 Base64 数据");
    }
    Bytes out(sz);
    if (!CryptStringToBinaryA(input.data(), static_cast<DWORD>(input.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_STRICT, out.data(), &sz, nullptr, nullptr)) {
        throw PluginError("Base64 解码失败");
    }
    out.resize(sz);
    return out;
}

inline std::wstring canonicalPathKey(const fs::path& p) {
    std::error_code ec;
    fs::path norm = fs::weakly_canonical(p, ec);
    if (ec) norm = fs::absolute(p, ec).lexically_normal();
    std::wstring s = norm.wstring();
    for (auto& ch : s) ch = static_cast<wchar_t>(std::towlower(ch));
    return s;
}

inline bool isSameFilePath(const fs::path& a, const fs::path& b) {
    if (a.empty() || b.empty()) return false;
    std::error_code ea, eb, eq;
    if (fs::exists(a, ea) && fs::exists(b, eb) && fs::equivalent(a, b, eq) && !eq) return true;
    return canonicalPathKey(a) == canonicalPathKey(b);
}

inline Bytes readFileBytes(const fs::path& p, size_t maxBytes = 128ull * 1024 * 1024) {
    std::ifstream in(p, std::ios::binary | std::ios::ate);
    if (!in) throw PluginError("无法打开文件：" + wideToUtf8(p.wstring()));
    auto len = in.tellg();
    if (len < 0 || static_cast<uint64_t>(len) > maxBytes) throw PluginError("文件大小超出限制");
    Bytes buf(static_cast<size_t>(len));
    in.seekg(0);
    if (!buf.empty() && !in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()))) {
        throw PluginError("读取文件失败");
    }
    return buf;
}

inline std::string readFileTailLossyUtf8(const fs::path& p, size_t maxTailBytes = 24 * 1024) {
    std::error_code ec;
    if (!fs::exists(p, ec)) return {};
    std::ifstream in(p, std::ios::binary | std::ios::ate);
    if (!in) return {};
    auto endPos = in.tellg();
    if (endPos <= 0) return {};
    uint64_t total = static_cast<uint64_t>(endPos);
    uint64_t take = total > maxTailBytes ? maxTailBytes : total;
    in.seekg(static_cast<std::streamoff>(total - take), std::ios::beg);
    std::string raw(static_cast<size_t>(take), '\0');
    in.read(raw.data(), static_cast<std::streamsize>(take));
    raw.resize(static_cast<size_t>(in.gcount()));
    return wideToUtf8(utf8ToWideLossy(raw));
}

inline void writeFileAtomic(const fs::path& p, std::span<const uint8_t> data) {
    if (!p.parent_path().empty()) fs::create_directories(p.parent_path());
    fs::path tmp = p;
    tmp += L"." + utf8ToWideStrict(randomHex(6)) + L".part";
    struct Guard {
        fs::path f; bool ok = false;
        ~Guard() { if (!ok) { std::error_code ec; fs::remove(f, ec); } }
    } g{tmp};
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out || (!data.empty() && !out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size())))) {
            throw PluginError("写入临时文件失败");
        }
        out.flush();
        if (!out) throw PluginError("刷新文件缓冲区失败");
    }
    if (!MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw PluginError("原子写入目标文件失败");
    }
    g.ok = true;
}

inline std::string readEnvVarUtf8(std::string_view name) {
    if (name.empty()) return {};
    std::wstring wname = utf8ToWideStrict(name);
    DWORD req = GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
    if (!req) return {};
    std::wstring wval(req, L'\0');
    DWORD got = GetEnvironmentVariableW(wname.c_str(), wval.data(), req);
    if (!got || got >= req) {
        SecureZeroMemory(wval.data(), wval.size() * sizeof(wchar_t));
        return {};
    }
    wval.resize(got);
    std::string out = wideToUtf8(wval);
    SecureZeroMemory(wval.data(), wval.size() * sizeof(wchar_t));
    return out;
}

// ==================== Geometry & Pixel Helpers ====================

struct NormalizedRect {
    double x = 0.0, y = 0.0, width = 1.0, height = 1.0;
    static constexpr double kTolerance = 1e-6;

    bool isFullImage() const noexcept {
        return std::abs(x) <= kTolerance && std::abs(y) <= kTolerance &&
               std::abs(width - 1.0) <= kTolerance && std::abs(height - 1.0) <= kTolerance;
    }

    void validate(std::string_view label = "选区矩形") const {
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(width) || !std::isfinite(height) ||
            x < -kTolerance || y < -kTolerance || width <= 0.0 || height <= 0.0 ||
            x + width > 1.0 + kTolerance || y + height > 1.0 + kTolerance) {
            throw PluginError(std::string(label) + "超出有效范围 [0, 1]");
        }
    }
};

// ==================== Host ABI v2 RAII Wrapper ====================

class HostClient {
    const MemeStudioHostV2* host_ = nullptr;
    std::string pluginId_;
public:
    HostClient() = default;
    HostClient(const MemeStudioHostV2* host, std::string pluginId)
        : host_(host), pluginId_(std::move(pluginId)) {}

    const MemeStudioHostV2* raw() const noexcept { return host_; }
    const std::string& pluginId() const noexcept { return pluginId_; }

    void writeOutBytes(MemeStudioBytesV2* dst, std::string_view str) const {
        if (!dst || !host_ || !host_->alloc) return;
        dst->data = nullptr;
        dst->size = 0;
        if (str.empty()) return;
        auto* p = static_cast<uint8_t*>(host_->alloc(host_->host_ctx, str.size() + 1));
        if (!p) return;
        std::memcpy(p, str.data(), str.size());
        p[str.size()] = 0;
        dst->data = p;
        dst->size = str.size();
    }

    void writeOutRawBytes(MemeStudioBytesV2* dst, std::span<const uint8_t> bytes) const {
        if (!dst || !host_ || !host_->alloc) return;
        dst->data = nullptr;
        dst->size = 0;
        if (bytes.empty()) return;
        auto* p = static_cast<uint8_t*>(host_->alloc(host_->host_ctx, bytes.size()));
        if (!p) return;
        std::memcpy(p, bytes.data(), bytes.size());
        dst->data = p;
        dst->size = bytes.size();
    }

    std::string consumeBytesToString(MemeStudioBytesV2& b) const {
        std::string s;
        if (b.data && b.size > 0) {
            s.assign(reinterpret_cast<const char*>(b.data), b.size);
        }
        if (host_ && host_->free_bytes) host_->free_bytes(host_->host_ctx, &b);
        return s;
    }

    Bytes consumeBytesToVec(MemeStudioBytesV2& b) const {
        Bytes v;
        if (b.data && b.size > 0) {
            v.assign(b.data, b.data + b.size);
        }
        if (host_ && host_->free_bytes) host_->free_bytes(host_->host_ctx, &b);
        return v;
    }

    void log(int32_t level, const std::string& msg) const {
        if (host_ && host_->log) {
            host_->log(host_->host_ctx, level, pluginId_.c_str(), msg.c_str());
        }
    }

    void verifyNotCancelled(const MemeStudioCancelTokenV2* cancel) const {
        if (cancel && host_ && host_->is_cancelled && host_->is_cancelled(cancel) != 0) {
            throw PluginCancelled();
        }
    }

    fs::path dataDir() const {
        if (!host_ || !host_->get_data_dir) throw PluginError("宿主未提供数据目录服务");
        MemeStudioBytesV2 b{};
        if (host_->get_data_dir(host_->host_ctx, pluginId_.c_str(), &b) != 0) {
            throw PluginError("获取插件专属数据目录失败");
        }
        return utf8ToWideStrict(consumeBytesToString(b));
    }

    std::string protectSecret(const std::string& plain) const {
        if (plain.empty()) return {};
        if (!host_ || !host_->protect_secret) throw PluginError("宿主未提供密钥加密服务");
        MemeStudioBytesV2 b{};
        if (host_->protect_secret(host_->host_ctx, plain.c_str(), &b) != 0) {
            throw PluginError("加密保存密钥失败");
        }
        return consumeBytesToString(b);
    }

    std::string unprotectSecret(const std::string& cipher) const {
        if (cipher.empty()) return {};
        if (!host_ || !host_->unprotect_secret) throw PluginError("宿主未提供密钥解密服务");
        MemeStudioBytesV2 b{};
        if (host_->unprotect_secret(host_->host_ctx, cipher.c_str(), &b) != 0) {
            throw PluginError("解密已保存密钥失败");
        }
        return consumeBytesToString(b);
    }
};

// ==================== Shared DPI & Win32 UI Helpers ====================

struct DpiScale {
    UINT dpi = 96;
    explicit DpiScale(HWND hwnd = nullptr) {
        if (hwnd) {
            UINT d = GetDpiForWindow(hwnd);
            if (d >= 72) dpi = d;
        }
    }
    int px(int dip) const noexcept { return MulDiv(dip, static_cast<int>(dpi), 96); }
};

inline HFONT createDpiUiFont(UINT dpi, int sizeDip = 14, int weight = FW_NORMAL) {
    int heightPx = -MulDiv(sizeDip, static_cast<int>(dpi), 96);
    return CreateFontW(
        heightPx, 0, 0, 0, weight, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
}

inline std::string getWindowTextUtf8(HWND hwnd) {
    if (!hwnd) return {};
    int len = GetWindowTextLengthW(hwnd);
    if (len <= 0) return {};
    std::wstring buf(static_cast<size_t>(len) + 1, L'\0');
    int copied = GetWindowTextW(hwnd, buf.data(), len + 1);
    if (copied <= 0) return {};
    buf.resize(static_cast<size_t>(copied));
    return wideToUtf8(buf);
}

inline void setWindowTextUtf8(HWND hwnd, std::string_view text) {
    if (!hwnd) return;
    std::wstring w = utf8ToWideLossy(text);
    SetWindowTextW(hwnd, w.c_str());
}

inline std::optional<fs::path> pickSinglePath(HWND owner, bool saveDialog, bool pickFolder, const wchar_t* title) {
    using Microsoft::WRL::ComPtr;
    ComPtr<IFileDialog> dialog;
    HRESULT hr = saveDialog
        ? CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))
        : CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(hr) || !dialog) return std::nullopt;
    DWORD opts = 0;
    dialog->GetOptions(&opts);
    opts |= FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR;
    if (pickFolder) opts |= FOS_PICKFOLDERS;
    else if (!saveDialog) opts |= FOS_FILEMUSTEXIST;
    dialog->SetOptions(opts);
    if (title) dialog->SetTitle(title);
    hr = dialog->Show(owner);
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED) || FAILED(hr)) return std::nullopt;
    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item)) || !item) return std::nullopt;
    PWSTR rawPath = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath)) || !rawPath) return std::nullopt;
    fs::path result(rawPath);
    CoTaskMemFree(rawPath);
    return result;
}

} // namespace meme::plugin_kit
