#pragma once
// Common utilities: application paths, logging, queues, UTF-8 and atomic files.
#include <windows.h>
#include <shlobj.h>
#include <objbase.h>
#include <string>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <fstream>
#include <chrono>
#include <format>
#include <filesystem>
#include <vector>

namespace lar {

inline std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), w.data(), n);
    return w;
}

inline std::string wide_to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

inline std::string exe_dir() {
    std::vector<wchar_t> buf(32768);
    DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
    std::filesystem::path p(std::wstring(buf.data(), n));
    return wide_to_utf8((p.parent_path().wstring() + L"\\"));
}

inline std::string app_data_dir() {
    static const std::string value = [] {
        PWSTR raw = nullptr;
        std::filesystem::path p;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &raw)) && raw) {
            p = raw;
            CoTaskMemFree(raw);
        } else {
            p = utf8_to_wide(exe_dir());
        }
        p /= L"Helm";
        std::error_code ec;
        std::filesystem::create_directories(p, ec);
        return wide_to_utf8(p.wstring() + L"\\");
    }();
    return value;
}

inline std::string new_uuid() {
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) {
        const auto ticks = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        return std::to_string(ticks);
    }
    wchar_t text[40]{};
    StringFromGUID2(guid, text, 40);
    std::wstring w(text);
    if (!w.empty() && w.front() == L'{') w.erase(w.begin());
    if (!w.empty() && w.back() == L'}') w.pop_back();
    return wide_to_utf8(w);
}

inline std::string utf8_prefix(const std::string& s, size_t max_bytes) {
    if (s.size() <= max_bytes) return s;
    size_t n = max_bytes;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    return s.substr(0, n);
}

inline size_t utf8_safe_cut(const std::string& s, size_t desired) {
    if (desired >= s.size()) return s.size();
    while (desired > 0 && (static_cast<unsigned char>(s[desired]) & 0xC0) == 0x80) --desired;
    return desired;
}

inline bool atomic_write_text(const std::filesystem::path& path, const std::string& text) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    const auto tmp = path.wstring() + L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        f.flush();
        if (!f) return false;
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

inline void log(const std::string& line) {
    static std::mutex m;
    std::lock_guard lk(m);
    const std::filesystem::path p = utf8_to_wide(app_data_dir() + "helm.log");
    std::ofstream f(p, std::ios::app);
    auto now = std::chrono::system_clock::now();
    std::string stamped = std::format("[{:%F %T}] ", std::chrono::floor<std::chrono::seconds>(now)) + line + "\n";
    if (f) { f << stamped; f.flush(); }
    OutputDebugStringA(stamped.c_str());
}

class TaskQueue {
public:
    bool push(std::function<void()> fn) {
        {
            std::lock_guard lk(m_);
            if (stop_) return false;
            q_.push(std::move(fn));
        }
        cv_.notify_one();
        return true;
    }
    bool pop(std::function<void()>& out) {
        std::unique_lock lk(m_);
        cv_.wait(lk, [&] { return stop_ || !q_.empty(); });
        if (stop_) return false;
        out = std::move(q_.front());
        q_.pop();
        return true;
    }
    void shutdown() {
        { std::lock_guard lk(m_); stop_ = true; while (!q_.empty()) q_.pop(); }
        cv_.notify_all();
    }
private:
    std::mutex m_;
    std::condition_variable cv_;
    std::queue<std::function<void()>> q_;
    bool stop_ = false;
};

class Utf8Buffer {
public:
    std::string feed(const std::string& bytes) {
        buf_ += bytes;
        size_t valid = valid_prefix_len(buf_);
        std::string out = buf_.substr(0, valid);
        buf_.erase(0, valid);
        return out;
    }
    std::string flush() { std::string out; out.swap(buf_); return out; }
private:
    static size_t valid_prefix_len(const std::string& s) {
        size_t i = 0;
        while (i < s.size()) {
            unsigned char c = static_cast<unsigned char>(s[i]);
            size_t need = (c < 0x80) ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
            if (i + need > s.size()) break;
            bool valid = true;
            for (size_t k = 1; k < need; ++k)
                if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) valid = false;
            if (!valid) { ++i; continue; }
            i += need;
        }
        return i;
    }
    std::string buf_;
};

} // namespace lar
