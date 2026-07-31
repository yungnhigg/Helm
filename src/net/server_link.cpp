#include "net/server_link.h"
#include "agent/loop.h"
#include "common/config.h"
#include "common/util.h"
#include "engine/engine.h"
#include "ui/bridge.h"

#include <nlohmann/json.hpp>

#include <windows.h>
#include <winhttp.h>

#include <chrono>
#include <fstream>

using nlohmann::json;

namespace lar {
namespace {

constexpr int kHeartbeatSeconds = 5;
constexpr int kHttpTimeoutMs = 5000;

// A machine identity that survives restarts, model changes, and renames, so
// the server's registry rows and queued commands stay attached to the same
// box. Generated once and kept next to the rest of the app state.
std::string load_or_create_instance_id() {
    const std::string path = app_data_dir() + "instance-id.txt";
    {
        std::ifstream in(utf8_to_wide(path), std::ios::binary);
        if (in) {
            std::string id((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            while (!id.empty() && (id.back() == '\n' || id.back() == '\r' || id.back() == ' ')) id.pop_back();
            if (!id.empty()) return id;
        }
    }
    const std::string id = new_uuid();
    atomic_write_text(utf8_to_wide(path), id);
    return id;
}

std::string computer_name() {
    wchar_t buffer[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD len = MAX_COMPUTERNAME_LENGTH + 1;
    if (GetComputerNameW(buffer, &len)) return wide_to_utf8(buffer);
    return "helm-instance";
}

struct ParsedUrl {
    std::wstring host;
    INTERNET_PORT port = 0;
    bool secure = false;
    std::wstring base_path;   // path prefix from server_url, usually "/"
};

bool parse_server_url(const std::string& url, ParsedUrl& out) {
    const std::wstring wide = utf8_to_wide(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256]{}, path[1024]{};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 1023;
    if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &uc)) return false;
    out.host.assign(host, uc.dwHostNameLength);
    out.base_path.assign(path, uc.dwUrlPathLength);
    while (!out.base_path.empty() && out.base_path.back() == L'/') out.base_path.pop_back();
    out.port = uc.nPort;
    out.secure = uc.nScheme == INTERNET_SCHEME_HTTPS;
    return !out.host.empty() &&
           (uc.nScheme == INTERNET_SCHEME_HTTP || uc.nScheme == INTERNET_SCHEME_HTTPS);
}

} // namespace

ServerLink::ServerLink(const Config& cfg, Engine& eng, AgentLoop& loop, Bridge& bridge)
    : cfg_(cfg), eng_(eng), loop_(loop), bridge_(bridge) {
    if (cfg_.server_url.empty()) return;
    if (cfg_.server_token.empty()) {
        log("server_url is configured but server_token is empty - server link disabled. "
            "Copy the token from the server's token.txt into app.json.");
        return;
    }
    ParsedUrl probe;
    if (!parse_server_url(cfg_.server_url, probe)) {
        log("server_url is not a valid http(s) URL - server link disabled: " + cfg_.server_url);
        return;
    }
    instance_id_ = load_or_create_instance_id();
    instance_name_ = cfg_.instance_name.empty() ? computer_name() : cfg_.instance_name;
    worker_ = std::thread([this] { run(); });
}

ServerLink::~ServerLink() {
    stop_.store(true);
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void ServerLink::run() {
    log("server link active: " + cfg_.server_url + " as \"" + instance_name_ + "\" (" + instance_id_ + ")");
    bool registered = false;
    bool was_reachable = true;   // start optimistic so the first failure logs
    for (;;) {
        {
            std::unique_lock lk(m_);
            cv_.wait_for(lk, std::chrono::seconds(kHeartbeatSeconds), [this] { return stop_.load(); });
        }
        if (stop_.load()) return;

        const bool ok = heartbeat_once(registered);
        // Log transitions, not every beat: an unreachable server at 5s
        // intervals would otherwise write 17k identical lines a day.
        if (ok && !was_reachable) log("server link restored");
        if (!ok && was_reachable) log("server link unreachable (will keep retrying quietly)");
        was_reachable = ok;
    }
}

bool ServerLink::heartbeat_once(bool& registered) {
    if (!registered) {
        std::string response;
        const json body{{"instance_id", instance_id_}, {"name", instance_name_}};
        if (post_json(L"/v1/instances/register", body.dump(), response) != 200) return false;
        registered = true;
    }

    // State is sampled from lock-free atomics; the model id string follows
    // the same read-between-turns convention every tool thread already uses.
    const bool loaded = eng_.loaded();
    const json body{
        {"instance_id", instance_id_},
        {"name", instance_name_},
        {"model_id", loaded ? cfg_.active_model_id : std::string()},
        {"state", !loaded ? "unloaded" : (loop_.has_in_flight_work() ? "busy" : "idle")},
        // VRAM reporting lands with the API-mode work; a per-beat nvidia-smi
        // spawn is not worth two process launches a second on this box.
        {"vram_used", 0},
        {"vram_total", 0},
    };

    std::string response;
    if (post_json(L"/v1/instances/heartbeat", body.dump(), response) != 200) return false;

    try {
        const json parsed = json::parse(response);
        for (const auto& command : parsed.value("commands", json::array())) {
            const std::string name = command.is_string() ? command.get<std::string>() : std::string();
            // The command vocabulary is closed on purpose. Anything else in
            // the response is logged and dropped - the server is trusted, but
            // a control channel should still refuse what it does not know.
            if (name == "halt") bridge_.halt_local_model("server");
            else if (name == "evict") bridge_.evict_local_model("server");
            else log("server sent an unknown command (ignored): " + name);
        }
    } catch (const std::exception& e) {
        log(std::string("server heartbeat response was not valid JSON: ") + e.what());
        return false;
    }
    return true;
}

int ServerLink::post_json(const std::wstring& path, const std::string& body, std::string& response_body) {
    response_body.clear();
    ParsedUrl u;
    if (!parse_server_url(cfg_.server_url, u)) return 0;

    HINTERNET session = WinHttpOpen(L"Helm ServerLink/2.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return 0;
    WinHttpSetTimeouts(session, kHttpTimeoutMs, kHttpTimeoutMs, kHttpTimeoutMs, kHttpTimeoutMs);

    int status_out = 0;
    HINTERNET connect = WinHttpConnect(session, u.host.c_str(), u.port, 0);
    if (connect) {
        const std::wstring full_path = u.base_path + path;
        HINTERNET request = WinHttpOpenRequest(connect, L"POST", full_path.c_str(), nullptr,
                                               WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                               u.secure ? WINHTTP_FLAG_SECURE : 0);
        if (request) {
            const std::wstring headers =
                L"Content-Type: application/json\r\n"
                L"Authorization: Bearer " + utf8_to_wide(cfg_.server_token) + L"\r\n";
            const BOOL ok = WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(headers.size()),
                                               const_cast<char*>(body.data()), static_cast<DWORD>(body.size()),
                                               static_cast<DWORD>(body.size()), 0) &&
                            WinHttpReceiveResponse(request, nullptr);
            if (ok) {
                DWORD status = 0, len = sizeof(status);
                WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
                status_out = static_cast<int>(status);
                for (;;) {
                    DWORD available = 0;
                    if (!WinHttpQueryDataAvailable(request, &available) || available == 0) break;
                    // Heartbeat responses are a few hundred bytes; anything
                    // approaching a megabyte is not our server.
                    if (response_body.size() + available > 1024 * 1024) break;
                    const size_t old = response_body.size();
                    response_body.resize(old + available);
                    DWORD read = 0;
                    if (!WinHttpReadData(request, response_body.data() + old, available, &read)) break;
                    response_body.resize(old + read);
                }
            }
            WinHttpCloseHandle(request);
        }
        WinHttpCloseHandle(connect);
    }
    WinHttpCloseHandle(session);
    return status_out;
}

} // namespace lar
