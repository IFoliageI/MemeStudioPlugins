#pragma once
#include "memestudio_plugin.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>
#include <windowsx.h>
#include <bcrypt.h>
#include <shobjidl.h>
#include <wincrypt.h>
#include <wrl/client.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <optional>
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

// ==================== Shared DPI & Win32 UI Panel Builder ====================

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

inline bool isButtonChecked(HWND hwnd) noexcept {
    return hwnd && SendMessageW(hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

inline void setButtonChecked(HWND hwnd, bool checked) noexcept {
    if (hwnd) SendMessageW(hwnd, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
}

inline int parseIntField(HWND hwnd, std::string_view label, int minVal, int maxVal) {
    std::string s = trimAscii(getWindowTextUtf8(hwnd));
    if (s.empty()) throw PluginError("请填写「" + std::string(label) + "」");
    int v = 0;
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || p != s.data() + s.size() || v < minVal || v > maxVal) {
        throw PluginError("「" + std::string(label) + "」必须是 " + std::to_string(minVal) + ".." + std::to_string(maxVal) + " 之间的整数");
    }
    return v;
}

inline double parseDoubleField(HWND hwnd, std::string_view label, double minVal, double maxVal) {
    std::string s = trimAscii(getWindowTextUtf8(hwnd));
    if (s.empty()) throw PluginError("请填写「" + std::string(label) + "」");
    double v = 0.0;
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || p != s.data() + s.size() || !std::isfinite(v) || v < minVal || v > maxVal) {
        throw PluginError("「" + std::string(label) + "」数值无效或超出允许范围");
    }
    return v;
}

inline void showPanelError(HWND owner, std::string_view msgUtf8, const wchar_t* title = L"操作提示") noexcept {
    std::wstring wm = utf8ToWideLossy(msgUtf8);
    MessageBoxW(owner, wm.c_str(), title, MB_OK | MB_ICONERROR);
}

inline void showPanelInfo(HWND owner, std::string_view msgUtf8, const wchar_t* title = L"提示") noexcept {
    std::wstring wm = utf8ToWideLossy(msgUtf8);
    MessageBoxW(owner, wm.c_str(), title, MB_OK | MB_ICONINFORMATION);
}

class PanelBuilder {
    HWND parent_ = nullptr;
    HINSTANCE inst_ = nullptr;
    DpiScale dpi_;
public:
    explicit PanelBuilder(HWND parent)
        : parent_(parent),
          inst_(reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(parent, GWLP_HINSTANCE))),
          dpi_(parent) {
        INITCOMMONCONTROLSEX icc{sizeof(INITCOMMONCONTROLSEX), ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_PROGRESS_CLASS};
        InitCommonControlsEx(&icc);
    }

    const DpiScale& dpi() const noexcept { return dpi_; }

    HWND label(const wchar_t* text, int x, int y, int w, int h = 20) const {
        return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_ENDELLIPSIS,
            dpi_.px(x), dpi_.px(y), dpi_.px(w), dpi_.px(h), parent_, nullptr, inst_, nullptr);
    }

    HWND button(const wchar_t* text, int id, int x, int y, int w, int h = 28, bool isDefault = false) const {
        return CreateWindowExW(0, L"BUTTON", text,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | (isDefault ? BS_DEFPUSHBUTTON : 0),
            dpi_.px(x), dpi_.px(y), dpi_.px(w), dpi_.px(h),
            parent_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst_, nullptr);
    }

    HWND checkbox(const wchar_t* text, int id, int x, int y, int w, int h = 22, bool checked = false) const {
        HWND b = CreateWindowExW(0, L"BUTTON", text,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            dpi_.px(x), dpi_.px(y), dpi_.px(w), dpi_.px(h),
            parent_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst_, nullptr);
        setButtonChecked(b, checked);
        return b;
    }

    HWND edit(int id, int x, int y, int w, int h = 26, std::string_view initial = "", DWORD extraStyle = ES_AUTOHSCROLL) const {
        HWND e = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | extraStyle,
            dpi_.px(x), dpi_.px(y), dpi_.px(w), dpi_.px(h),
            parent_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst_, nullptr);
        if (!initial.empty()) setWindowTextUtf8(e, initial);
        return e;
    }

    HWND multilineEdit(int id, int x, int y, int w, int h, std::string_view initial = "", bool readOnly = false) const {
        DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL;
        style |= readOnly ? ES_READONLY : ES_WANTRETURN; // Always enable ES_WANTRETURN when editable!
        HWND e = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", style,
            dpi_.px(x), dpi_.px(y), dpi_.px(w), dpi_.px(h),
            parent_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst_, nullptr);
        if (!initial.empty()) setWindowTextUtf8(e, initial);
        return e;
    }

    HWND combo(int id, int x, int y, int w, int dropH, std::initializer_list<const wchar_t*> items, int selIdx = 0, bool editable = false) const {
        HWND c = CreateWindowExW(0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | (editable ? (CBS_DROPDOWN | CBS_AUTOHSCROLL) : CBS_DROPDOWNLIST),
            dpi_.px(x), dpi_.px(y), dpi_.px(w), dpi_.px(dropH),
            parent_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst_, nullptr);
        for (const wchar_t* item : items) {
            SendMessageW(c, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
        }
        if (selIdx >= 0 && items.size() > 0) {
            SendMessageW(c, CB_SETCURSEL, static_cast<WPARAM>(selIdx), 0);
        }
        return c;
    }

    HWND listBox(int id, int x, int y, int w, int h) const {
        return CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | LBS_NOTIFY | WS_VSCROLL,
            dpi_.px(x), dpi_.px(y), dpi_.px(w), dpi_.px(h),
            parent_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst_, nullptr);
    }

    HWND reportListView(int id, int x, int y, int w, int h,
                        std::initializer_list<std::pair<const wchar_t*, int>> colsDip,
                        bool checkboxes = false) const {
        HWND lv = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            dpi_.px(x), dpi_.px(y), dpi_.px(w), dpi_.px(h),
            parent_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst_, nullptr);
        DWORD ex = LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES;
        if (checkboxes) ex |= LVS_EX_CHECKBOXES;
        ListView_SetExtendedListViewStyle(lv, ex);
        int colIdx = 0;
        for (const auto& [title, widthDip] : colsDip) {
            LVCOLUMNW col{};
            col.mask = LVCF_TEXT | LVCF_WIDTH;
            col.pszText = const_cast<LPWSTR>(title);
            col.cx = dpi_.px(widthDip);
            ListView_InsertColumn(lv, colIdx++, &col);
        }
        return lv;
    }

    void applyFont(HFONT font) const noexcept {
        if (!parent_ || !font) return;
        EnumChildWindows(parent_, [](HWND child, LPARAM lp) -> BOOL {
            SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(lp), TRUE);
            return TRUE;
        }, reinterpret_cast<LPARAM>(font));
    }
};

// Cached DIBSection preview & drag-box selection canvas for standalone plugin panels
class PluginPreviewCanvas {
    HWND hwnd_ = nullptr;
    HBITMAP dib_ = nullptr;
    uint32_t imgW_ = 0;
    uint32_t imgH_ = 0;
    bool allowCrop_ = false;
    bool dragging_ = false;
    POINT dragStart_{};
    RECT drawnRc_{};
    NormalizedRect region_{0.0, 0.0, 1.0, 1.0};
    std::wstring hint_ = L"未加载预览图像";
    std::function<void(const NormalizedRect&)> onRegionChanged_;

    void freeDib() noexcept {
        if (dib_) { DeleteObject(dib_); dib_ = nullptr; }
        imgW_ = imgH_ = 0;
    }

    static LRESULT CALLBACK proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<PluginPreviewCanvas*>(GetWindowLongPtrW(w, GWLP_USERDATA));
        if (!self) return DefWindowProcW(w, msg, wp, lp);

        if (msg == WM_ERASEBKGND) return 1;
        if (msg == WM_PAINT) {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(w, &ps);
            RECT rc{};
            GetClientRect(w, &rc);
            int cw = std::max<int>(1, rc.right - rc.left);
            int ch = std::max<int>(1, rc.bottom - rc.top);

            HDC mem = CreateCompatibleDC(dc);
            HBITMAP back = CreateCompatibleBitmap(dc, cw, ch);
            auto oldBmp = SelectObject(mem, back);

            HBRUSH bg = CreateSolidBrush(RGB(246, 248, 250));
            FillRect(mem, &rc, bg);
            DeleteObject(bg);

            if (self->dib_ && self->imgW_ > 0 && self->imgH_ > 0) {
                double sc = std::min(double(cw - 12) / self->imgW_, double(ch - 12) / self->imgH_);
                int dw = std::max(1, int(std::lround(self->imgW_ * sc)));
                int dh = std::max(1, int(std::lround(self->imgH_ * sc)));
                int dx = (cw - dw) / 2;
                int dy = (ch - dh) / 2;
                self->drawnRc_ = {dx, dy, dx + dw, dy + dh};

                HDC srcDc = CreateCompatibleDC(mem);
                auto oldSrc = SelectObject(srcDc, self->dib_);
                SetStretchBltMode(mem, HALFTONE);
                StretchBlt(mem, dx, dy, dw, dh, srcDc, 0, 0, int(self->imgW_), int(self->imgH_), SRCCOPY);
                SelectObject(srcDc, oldSrc);
                DeleteDC(srcDc);

                if (self->allowCrop_) {
                    RECT sel{
                        dx + int(std::lround(self->region_.x * dw)),
                        dy + int(std::lround(self->region_.y * dh)),
                        dx + int(std::lround((self->region_.x + self->region_.width) * dw)),
                        dy + int(std::lround((self->region_.y + self->region_.height) * dh))
                    };
                    HPEN pen = CreatePen(PS_SOLID, 2, RGB(0, 120, 212));
                    auto oldPen = SelectObject(mem, pen);
                    auto oldBr = SelectObject(mem, GetStockObject(NULL_BRUSH));
                    Rectangle(mem, sel.left, sel.top, sel.right, sel.bottom);
                    SelectObject(mem, oldBr);
                    SelectObject(mem, oldPen);
                    DeleteObject(pen);
                }
            } else {
                SetBkMode(mem, TRANSPARENT);
                SetTextColor(mem, RGB(120, 128, 138));
                DrawTextW(mem, self->hint_.c_str(), -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }

            HBRUSH border = CreateSolidBrush(RGB(205, 212, 220));
            FrameRect(mem, &rc, border);
            DeleteObject(border);

            BitBlt(dc, 0, 0, cw, ch, mem, 0, 0, SRCCOPY);
            SelectObject(mem, oldBmp);
            DeleteObject(back);
            DeleteDC(mem);
            EndPaint(w, &ps);
            return 0;
        }
        if (self->allowCrop_ && self->dib_) {
            if (msg == WM_LBUTTONDOWN) {
                self->dragging_ = true;
                self->dragStart_ = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                SetCapture(w);
                return 0;
            }
            if (msg == WM_MOUSEMOVE && self->dragging_) {
                int iw = self->drawnRc_.right - self->drawnRc_.left;
                int ih = self->drawnRc_.bottom - self->drawnRc_.top;
                if (iw > 4 && ih > 4) {
                    auto nx = [&](int p) { return std::clamp(double(p - self->drawnRc_.left) / iw, 0.0, 1.0); };
                    auto ny = [&](int p) { return std::clamp(double(p - self->drawnRc_.top) / ih, 0.0, 1.0); };
                    double x0 = nx(self->dragStart_.x), y0 = ny(self->dragStart_.y);
                    double x1 = nx(GET_X_LPARAM(lp)), y1 = ny(GET_Y_LPARAM(lp));
                    double l = std::min(x0, x1), t = std::min(y0, y1);
                    double rw = std::max(0.02, std::abs(x1 - x0));
                    double rh = std::max(0.02, std::abs(y1 - y0));
                    if (l + rw > 1.0) rw = 1.0 - l;
                    if (t + rh > 1.0) rh = 1.0 - t;
                    self->region_ = {l, t, rw, rh};
                    InvalidateRect(w, nullptr, FALSE);
                    if (self->onRegionChanged_) self->onRegionChanged_(self->region_);
                }
                return 0;
            }
            if (msg == WM_LBUTTONUP && self->dragging_) {
                self->dragging_ = false;
                ReleaseCapture();
                return 0;
            }
        }
        return DefWindowProcW(w, msg, wp, lp);
    }
public:
    ~PluginPreviewCanvas() { freeDib(); }

    HWND create(HWND parent, int xDip, int yDip, int wDip, int hDip, bool allowCrop, const wchar_t* hint = L"未加载预览图像") {
        HINSTANCE inst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(parent, GWLP_HINSTANCE));
        static constexpr const wchar_t* kCls = L"MemePluginPreviewCanvasV2";
        WNDCLASSEXW ex{sizeof(WNDCLASSEXW)};
        if (!GetClassInfoExW(inst, kCls, &ex)) {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(WNDCLASSEXW);
            wc.lpfnWndProc = DefWindowProcW;
            wc.hInstance = inst;
            wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
            wc.lpszClassName = kCls;
            RegisterClassExW(&wc);
        }
        DpiScale dpi(parent);
        allowCrop_ = allowCrop;
        if (hint) hint_ = hint;
        hwnd_ = CreateWindowExW(0, kCls, L"", WS_CHILD | WS_VISIBLE,
            dpi.px(xDip), dpi.px(yDip), dpi.px(wDip), dpi.px(hDip), parent, nullptr, inst, nullptr);
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        SetWindowLongPtrW(hwnd_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&PluginPreviewCanvas::proc));
        return hwnd_;
    }

    void setBgraFrame(const MemeStudioImageBufferV2& frame) {
        freeDib();
        if (!frame.bgra_pixels || frame.width == 0 || frame.height == 0) {
            if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = static_cast<LONG>(frame.width);
        bmi.bmiHeader.biHeight = -static_cast<LONG>(frame.height);
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HDC sdc = GetDC(nullptr);
        dib_ = CreateDIBSection(sdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        ReleaseDC(nullptr, sdc);
        if (dib_ && bits) {
            // Composite onto clean light gray matte once at load time
            auto* dst = static_cast<uint8_t*>(bits);
            for (uint32_t y = 0; y < frame.height; ++y) {
                const uint8_t* srcRow = frame.bgra_pixels + static_cast<size_t>(y) * frame.stride;
                uint8_t* dstRow = dst + static_cast<size_t>(y) * frame.width * 4;
                for (uint32_t x = 0; x < frame.width; ++x) {
                    uint32_t a = srcRow[x * 4 + 3];
                    dstRow[x * 4 + 0] = static_cast<uint8_t>((srcRow[x * 4 + 0] * a + 250 * (255 - a)) / 255);
                    dstRow[x * 4 + 1] = static_cast<uint8_t>((srcRow[x * 4 + 1] * a + 250 * (255 - a)) / 255);
                    dstRow[x * 4 + 2] = static_cast<uint8_t>((srcRow[x * 4 + 2] * a + 250 * (255 - a)) / 255);
                    dstRow[x * 4 + 3] = 255;
                }
            }
            imgW_ = frame.width;
            imgH_ = frame.height;
        }
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    }

    NormalizedRect region() const noexcept { return region_; }
    void setRegion(const NormalizedRect& r) noexcept {
        region_ = r;
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    }
    void setOnRegionChanged(std::function<void(const NormalizedRect&)> cb) { onRegionChanged_ = std::move(cb); }
};

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

inline std::vector<fs::path> pickMultiplePaths(HWND owner, const wchar_t* title) {
    using Microsoft::WRL::ComPtr;
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))) || !dialog) {
        return {};
    }
    DWORD opts = 0;
    dialog->GetOptions(&opts);
    dialog->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_ALLOWMULTISELECT | FOS_NOCHANGEDIR);
    if (title) dialog->SetTitle(title);
    if (FAILED(dialog->Show(owner))) return {};
    ComPtr<IShellItemArray> arr;
    if (FAILED(dialog->GetResults(&arr)) || !arr) return {};
    DWORD count = 0;
    arr->GetCount(&count);
    std::vector<fs::path> out;
    for (DWORD i = 0; i < count; ++i) {
        ComPtr<IShellItem> item;
        if (SUCCEEDED(arr->GetItemAt(i, &item)) && item) {
            PWSTR raw = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) && raw) {
                out.emplace_back(raw);
                CoTaskMemFree(raw);
            }
        }
    }
    return out;
}

} // namespace meme::plugin_kit
