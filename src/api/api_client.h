#pragma once
// Blocking streaming client for the Anthropic Messages API over WinHTTP.
// Runs on the engine worker like local generation, so turn serialization and
// busy_ semantics carry over unchanged; cancellation is the one thing that
// needs its own primitive, because a thread blocked in WinHttpReadData cannot
// poll a flag - cancel() closes the request handle from the calling thread,
// which is the documented way to abort a synchronous WinHTTP request.
#include "api/anthropic_stream.h"
#include <atomic>
#include <functional>
#include <mutex>
#include <string>

namespace lar {

struct Config;

struct ApiRequest {
    std::string model;
    int max_tokens = 8192;
    std::string system;
    nlohmann::json messages = nlohmann::json::array();
    nlohmann::json tools = nlohmann::json::array();   // empty array = no tools field sent
};

class ApiClient {
public:
    explicit ApiClient(const Config& cfg);

    // One streaming request. Returns after message_stop, an error, or cancel.
    ApiTurnResult run(const ApiRequest& request,
                      std::function<void(const std::string&)> on_text,
                      std::function<void(const std::string&)> on_thinking);

    // Abort the in-flight request from any thread. Safe when idle.
    void cancel();
    bool busy() const { return busy_.load(); }

private:
    const Config& cfg_;
    std::mutex m_;                    // guards request_
    void* request_ = nullptr;         // HINTERNET of the in-flight request
    std::atomic<bool> busy_{false};
    std::atomic<bool> cancelled_{false};
};

} // namespace lar
