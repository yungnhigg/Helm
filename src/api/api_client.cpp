#include "api/api_client.h"
#include "common/config.h"
#include "common/util.h"

#include <windows.h>
#include <winhttp.h>

using nlohmann::json;

namespace lar {

namespace {
constexpr wchar_t kHost[] = L"api.anthropic.com";
constexpr wchar_t kPath[] = L"/v1/messages";
// Generous but bounded: an agent step with adaptive thinking can run minutes;
// a dead connection should not hang the engine worker forever.
constexpr int kConnectTimeoutMs = 10000;
constexpr int kReceiveTimeoutMs = 600000;
}

ApiClient::ApiClient(const Config& cfg) : cfg_(cfg) {}

void ApiClient::cancel() {
    cancelled_.store(true);
    std::lock_guard lk(m_);
    if (request_) {
        // Closing the handle fails the blocked read/receive in run() with an
        // error; run() maps that to a cancelled result via the flag.
        WinHttpCloseHandle(static_cast<HINTERNET>(request_));
        request_ = nullptr;
    }
}

ApiTurnResult ApiClient::run(const ApiRequest& request,
                             std::function<void(const std::string&)> on_text,
                             std::function<void(const std::string&)> on_thinking) {
    ApiTurnResult failure;
    if (cfg_.anthropic_api_key.empty()) {
        failure.error = "no Claude API key configured - add one in Settings under Cloud APIs";
        return failure;
    }

    busy_.store(true);
    cancelled_.store(false);
    struct BusyReset { std::atomic<bool>& b; ~BusyReset() { b.store(false); } } busy_reset{busy_};

    json body{
        {"model", request.model},
        {"max_tokens", request.max_tokens},
        {"messages", request.messages},
        {"stream", true},
    };
    if (!request.system.empty()) body["system"] = request.system;
    if (!request.tools.empty()) body["tools"] = request.tools;
    const std::string payload = body.dump();

    HINTERNET session = WinHttpOpen(L"Helm/2.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { failure.error = "WinHttpOpen failed"; return failure; }
    WinHttpSetTimeouts(session, kConnectTimeoutMs, kConnectTimeoutMs, kConnectTimeoutMs, kReceiveTimeoutMs);

    HINTERNET connect = WinHttpConnect(session, kHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        failure.error = "WinHttpConnect failed";
        return failure;
    }

    HINTERNET request_handle = WinHttpOpenRequest(connect, L"POST", kPath, nullptr,
                                                  WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                  WINHTTP_FLAG_SECURE);
    if (!request_handle) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        failure.error = "WinHttpOpenRequest failed";
        return failure;
    }
    {
        std::lock_guard lk(m_);
        request_ = request_handle;
    }

    // The key never appears in a log line; the header string is assembled and
    // handed straight to WinHTTP.
    const std::wstring headers =
        L"Content-Type: application/json\r\n"
        L"Accept: text/event-stream\r\n"
        L"anthropic-version: 2023-06-01\r\n"
        L"x-api-key: " + utf8_to_wide(cfg_.anthropic_api_key) + L"\r\n";

    AnthropicStreamParser parser;
    parser.on_text = std::move(on_text);
    parser.on_thinking = std::move(on_thinking);

    ApiTurnResult result;
    DWORD status = 0;
    std::string error_body;

    const BOOL sent = WinHttpSendRequest(request_handle, headers.c_str(), static_cast<DWORD>(headers.size()),
                                         const_cast<char*>(payload.data()), static_cast<DWORD>(payload.size()),
                                         static_cast<DWORD>(payload.size()), 0) &&
                      WinHttpReceiveResponse(request_handle, nullptr);
    if (sent) {
        DWORD len = sizeof(status);
        WinHttpQueryHeaders(request_handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
        char buffer[16384];
        for (;;) {
            if (cancelled_.load()) break;
            DWORD read = 0;
            if (!WinHttpReadData(request_handle, buffer, sizeof(buffer), &read) || read == 0) break;
            if (status == 200) parser.feed(buffer, read);
            else if (error_body.size() < 65536) error_body.append(buffer, read);
        }
    }

    {
        std::lock_guard lk(m_);
        if (request_) {
            WinHttpCloseHandle(static_cast<HINTERNET>(request_));
            request_ = nullptr;
        }
    }
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);

    if (cancelled_.load()) {
        result.error = "cancelled";
        return result;
    }
    if (!sent) {
        result.error = "connection to api.anthropic.com failed (" + std::to_string(GetLastError()) + ")";
        return result;
    }
    if (status != 200) {
        // The error body is API JSON: {"type":"error","error":{type,message}}.
        std::string detail = error_body;
        try {
            const json parsed = json::parse(error_body);
            detail = parsed["error"].value("type", "error") + ": " +
                     parsed["error"].value("message", "");
        } catch (...) {}
        if (detail.size() > 500) detail.resize(500);
        result.error = "API returned HTTP " + std::to_string(status) + " - " + detail;
        return result;
    }
    return parser.finish();
}

} // namespace lar
