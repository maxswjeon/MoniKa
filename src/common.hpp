// common.hpp - shared types, logging, string/hex/path helpers, constants.
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>
#include <cstdarg>
#include <mutex>
#include <cstdio>
#include <cstring>
#include <cwctype>

namespace kw {
using std::string;
using std::vector;
using std::wstring;

inline std::mutex& log_mutex() {
    static std::mutex m;
    return m;
}
inline wstring log_path() {
    static wstring path = []() -> wstring {
        DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
        if (!n)
            return L"";
        wstring base(n, L'\0');
        DWORD got = GetEnvironmentVariableW(L"LOCALAPPDATA", base.data(), n);
        if (!got || got >= n)
            return L"";
        base.resize(got);
        wstring dir = base + L"\\MoniKa";
        if (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
            return L"";
        return dir + L"\\watcher.log";
    }();
    return path;
}
inline void logf(const char* level, const char* fmt, ...) {
    char buf[1200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lk(log_mutex());
    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[1320];
    snprintf(line, sizeof(line), "[%04d-%02d-%02d %02d:%02d:%02d.%03d %-5s] %s\r\n", st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, level, buf);
    const wstring& path = log_path();
    if (!path.empty()) {
        HANDLE file =
            CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(file, line, (DWORD)strlen(line), &written, nullptr);
            CloseHandle(file);
        }
    }
    // Keep stderr useful when a developer explicitly attaches or redirects it.
    fputs(line, stderr);
    fflush(stderr);
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
}
#define LOGI(...) ::kw::logf("INFO", __VA_ARGS__)
#define LOGW(...) ::kw::logf("WARN", __VA_ARGS__)
#define LOGE(...) ::kw::logf("ERROR", __VA_ARGS__)

inline wstring utf8_to_wide(const string& s) {
    if (s.empty())
        return L"";
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0)
        return L"";
    wstring w(n, 0);
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), w.data(), n) != n)
        return L"";
    return w;
}
inline string wide_to_utf8(const wstring& w) {
    if (w.empty())
        return "";
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0)
        return "";
    string s(n, 0);
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr) != n)
        return "";
    return s;
}
inline wstring to_lower(wstring s) {
    for (auto& c : s)
        c = (wchar_t)towlower(c);
    return s;
}
inline bool iends_with(const wstring& s, const wstring& suf) {
    if (s.size() < suf.size())
        return false;
    return to_lower(s.substr(s.size() - suf.size())) == to_lower(suf);
}

inline string to_hex(const uint8_t* p, size_t n) {
    static const char* H = "0123456789abcdef";
    string s(n * 2, 0);
    for (size_t i = 0; i < n; ++i) {
        s[2 * i] = H[p[i] >> 4];
        s[2 * i + 1] = H[p[i] & 0xf];
    }
    return s;
}
inline bool from_hex(const string& hex, vector<uint8_t>& out) {
    if (hex.size() % 2)
        return false;
    out.resize(hex.size() / 2);
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < out.size(); ++i) {
        int hi = nib(hex[2 * i]), lo = nib(hex[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

inline bool dir_exists(const wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
inline wstring env_var(const wchar_t* name) {
    DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    if (!n)
        return L"";
    wstring value(n, L'\0');
    DWORD got = GetEnvironmentVariableW(name, &value[0], n);
    if (!got || got >= n)
        return L"";
    value.resize(got);
    return value;
}
inline int64_t now_unix() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (int64_t)((u.QuadPart - 116444736000000000ULL) / 10000000ULL);
}
inline bool read_file_head(const wstring& path, size_t n, vector<uint8_t>& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, 0);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    out.resize(n);
    DWORD got = 0;
    BOOL ok = ReadFile(h, out.data(), (DWORD)n, &got, 0);
    CloseHandle(h);
    if (!ok)
        return false;
    out.resize(got);
    return true;
}

static const int PAGE_SIZE = 4096;
static const size_t DEK_LEN = 32;
static const uint8_t ANCHOR_BYTE = 0x88; // codec-record tag right before the DEK
static const uint8_t ANCHOR_TAG_MAX = 8; // byte[-2] must be < this
static const int RESERVED_CANDS[] = {80, 48, 64, 16, 32, 96};
static const int RESERVED_NCAND = 6;
static const wchar_t* KAKAO_IMAGE = L"kakaotalk.exe";
} // namespace kw
