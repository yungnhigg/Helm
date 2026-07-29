#include "agent/registry.h"
#include "agent/jobs.h"
#include "common/util.h"
#include <windows.h>
#include <winhttp.h>
#include <shlwapi.h>
#include <filesystem>
#include <fstream>
#include <regex>
#include <queue>
#include <unordered_set>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <stdexcept>

#pragma comment(lib, "winhttp.lib")

namespace fs = std::filesystem;
namespace lar {

struct ParsedUrl {
    std::wstring scheme;
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = 0;
    bool secure = false;
};

static bool parse_url(const std::wstring& url, ParsedUrl& out) {
    URL_COMPONENTSW uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t scheme[16]{}, host[512]{}, path[4096]{}, extra[4096]{};
    uc.lpszScheme = scheme; uc.dwSchemeLength = _countof(scheme);
    uc.lpszHostName = host; uc.dwHostNameLength = _countof(host);
    uc.lpszUrlPath = path; uc.dwUrlPathLength = _countof(path);
    uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = _countof(extra);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return false;
    out.scheme.assign(scheme, uc.dwSchemeLength);
    out.host.assign(host, uc.dwHostNameLength);
    out.path.assign(path, uc.dwUrlPathLength);
    out.path.append(extra, uc.dwExtraInfoLength);
    if (out.path.empty()) out.path = L"/";
    out.port = uc.nPort;
    out.secure = uc.nScheme == INTERNET_SCHEME_HTTPS;
    return !out.host.empty() && (uc.nScheme == INTERNET_SCHEME_HTTP || uc.nScheme == INTERNET_SCHEME_HTTPS);
}

static bool same_origin(const ParsedUrl& a, const ParsedUrl& b) {
    return _wcsicmp(a.scheme.c_str(), b.scheme.c_str()) == 0 &&
           _wcsicmp(a.host.c_str(), b.host.c_str()) == 0 &&
           a.port == b.port;
}

static std::string fetch_url(HINTERNET session, const std::wstring& url, std::string& content_type) {
    ParsedUrl u;
    if (!parse_url(url, u)) throw std::runtime_error("invalid URL");
    HINTERNET connect = WinHttpConnect(session, u.host.c_str(), u.port, 0);
    if (!connect) throw std::runtime_error("WinHttpConnect failed");
    DWORD flags = u.secure ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(connect, L"GET", u.path.c_str(), nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) { WinHttpCloseHandle(connect); throw std::runtime_error("WinHttpOpenRequest failed"); }
    WinHttpAddRequestHeaders(request, L"Accept: text/html,text/plain,application/xhtml+xml\r\n", -1, WINHTTP_ADDREQ_FLAG_ADD);
    BOOL ok = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(request, nullptr);
    if (!ok) {
        WinHttpCloseHandle(request); WinHttpCloseHandle(connect);
        throw std::runtime_error("HTTP request failed: " + std::to_string(GetLastError()));
    }
    DWORD status = 0, len = sizeof(status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
    if (status < 200 || status >= 400) {
        WinHttpCloseHandle(request); WinHttpCloseHandle(connect);
        throw std::runtime_error("HTTP status " + std::to_string(status));
    }
    wchar_t type[256]{}; len = sizeof(type);
    if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX,
                            type, &len, WINHTTP_NO_HEADER_INDEX)) content_type = wide_to_utf8(type);

    std::string body;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available) || available == 0) break;
        if (body.size() + available > 5 * 1024 * 1024) available = static_cast<DWORD>(5 * 1024 * 1024 - body.size());
        if (available == 0) break;
        const size_t old = body.size();
        body.resize(old + available);
        DWORD read = 0;
        if (!WinHttpReadData(request, body.data() + old, available, &read)) break;
        body.resize(old + read);
    }
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    return body;
}

static std::string html_to_text(std::string html) {
    html = std::regex_replace(html, std::regex("<script[\\s\\S]*?</script>", std::regex::icase), " ");
    html = std::regex_replace(html, std::regex("<style[\\s\\S]*?</style>", std::regex::icase), " ");
    html = std::regex_replace(html, std::regex("<[^>]+>"), " ");
    html = std::regex_replace(html, std::regex("&nbsp;", std::regex::icase), " ");
    html = std::regex_replace(html, std::regex("&amp;", std::regex::icase), "&");
    html = std::regex_replace(html, std::regex("&lt;", std::regex::icase), "<");
    html = std::regex_replace(html, std::regex("&gt;", std::regex::icase), ">");
    html = std::regex_replace(html, std::regex("\\s+"), " ");
    return html;
}

static std::vector<std::wstring> links_from(const std::string& html, const std::wstring& base) {
    std::vector<std::wstring> out;
    std::regex re(R"(<a[^>]+href\s*=\s*["']([^"'#]+)["'])", std::regex::icase);
    for (std::sregex_iterator it(html.begin(), html.end(), re), end; it != end; ++it) {
        const std::wstring href = utf8_to_wide((*it)[1].str());
        wchar_t combined[8192]{};
        DWORD size = _countof(combined);
        if (SUCCEEDED(UrlCombineW(base.c_str(), href.c_str(), combined, &size, URL_ESCAPE_UNSAFE)))
            out.emplace_back(combined);
    }
    return out;
}

static std::string file_slug(size_t index, const std::wstring& url) {
    std::string s = wide_to_utf8(url);
    for (char& c : s) if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
    if (s.size() > 80) s.resize(80);
    return std::to_string(index) + "-" + s + ".txt";
}

void register_tool_web_crawl(Registry& r) {
    r.add({
        "crawl_site",
        "Crawl a website locally, following same-host links, and save extracted page text under the Helm data directory.",
        {{"url", ParamType::String, "Starting http or https URL"},
         {"max_pages", ParamType::Integer, "Maximum same-host pages to fetch, from 1 to 200"}},
        ToolClass::Job,
        {},
        [](const nlohmann::json& a, JobHandle& job) {
            const std::wstring start = utf8_to_wide(a.at("url").get<std::string>());
            const int max_pages = std::clamp(a.at("max_pages").get<int>(), 1, 200);
            ParsedUrl root;
            if (!parse_url(start, root)) throw std::runtime_error("invalid start URL");

            HINTERNET session = WinHttpOpen(L"Helm/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
            if (!session) throw std::runtime_error("WinHttpOpen failed");
            WinHttpSetTimeouts(session, 10000, 10000, 15000, 30000);

            const std::string crawl_id = new_uuid();
            fs::path out_dir = utf8_to_wide(app_data_dir() + "crawls\\" + crawl_id + "\\");
            std::error_code ec;
            fs::create_directories(out_dir, ec);

            std::queue<std::wstring> pending;
            std::unordered_set<std::wstring> seen;
            pending.push(start);
            size_t saved = 0;
            std::ostringstream manifest;

            while (!pending.empty() && saved < static_cast<size_t>(max_pages) && !job.cancelled()) {
                std::wstring url = std::move(pending.front()); pending.pop();
                if (!seen.insert(url).second) continue;
                ParsedUrl current;
                if (!parse_url(url, current) || !same_origin(current, root)) continue;
                try {
                    std::string type;
                    std::string body = fetch_url(session, url, type);
                    std::string text = type.find("text/html") != std::string::npos || body.find("<html") != std::string::npos
                        ? html_to_text(body) : body;
                    const std::string filename = file_slug(saved, url);
                    atomic_write_text(out_dir / utf8_to_wide(filename), "URL: " + wide_to_utf8(url) + "\n\n" + text);
                    manifest << wide_to_utf8(url) << "\t" << filename << "\n";
                    ++saved;
                    job.report(static_cast<int>(saved * 100 / max_pages), "saved " + std::to_string(saved) + " page(s)");
                    if (type.find("text/html") != std::string::npos || body.find("<html") != std::string::npos) {
                        for (auto& link : links_from(body, url)) {
                            ParsedUrl next;
                            if (parse_url(link, next) && same_origin(next, root) && !seen.contains(link))
                                pending.push(std::move(link));
                        }
                    }
                } catch (const std::exception& e) {
                    manifest << wide_to_utf8(url) << "\tERROR: " << e.what() << "\n";
                }
            }
            WinHttpCloseHandle(session);
            atomic_write_text(out_dir / L"manifest.tsv", manifest.str());
            if (job.cancelled()) return std::string("crawl cancelled after ") + std::to_string(saved) + " page(s); partial data: " + wide_to_utf8(out_dir.wstring());
            return "saved " + std::to_string(saved) + " page(s) to " + wide_to_utf8(out_dir.wstring());
        }
    });
}

} // namespace lar
