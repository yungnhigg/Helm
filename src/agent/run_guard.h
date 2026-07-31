#pragma once
#include <nlohmann/json.hpp>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

namespace lar {

// Stable identities used by both the in-run progress watchdog and the
// persistent per-agent work ledger. Signatures represent one exact call;
// resource keys represent the thing being examined (URL, query, or path).
std::string canonical_tool_call_signature(const std::string& name,
                                          const nlohmann::json& arguments);
std::string canonical_tool_resource_key(const std::string& name,
                                        const nlohmann::json& arguments);
std::string stable_text_fingerprint(const std::string& text);
std::string concise_tool_result_summary(const std::string& text,
                                        std::size_t max_chars = 240);

struct GuardDecision {
    bool allow = true;
    bool abort_run = false;
    std::string message;
    std::string signature;
    std::string resource_key;
};

struct ProgressObservation {
    bool meaningful_progress = true;
    bool resource_stalled = false;
    std::string resource_key;
    std::string fingerprint;
};

// Detects actual non-progress rather than imposing a tool-call budget. Long
// runs remain unlimited while calls produce new information or material state
// changes. The watchdog only blocks exact repeats and repeated examination of
// a resource that has returned the same/empty/error result several times.
class ProgressWatchdog {
public:
    explicit ProgressWatchdog(std::size_t max_signatures = 5000);

    GuardDecision before_call(const std::string& name,
                              const nlohmann::json& arguments);
    ProgressObservation after_result(const std::string& name,
                                     const nlohmann::json& arguments,
                                     const std::string& result);
    // Seed a prior persistent observation without blocking the first refresh.
    // If that refresh returns the same result, the resource is then considered
    // stalled and a further repeat is refused.
    void seed_resource_observation(const std::string& resource_key,
                                   const std::string& fingerprint,
                                   bool failed);

    // Narrow plan-only reply handling. Several consecutive stalls terminate the
    // run as a non-progressing loop; a real reply resets reply-stall state but
    // does not count as an external action for exact-call deduplication.
    GuardDecision on_stalling_reply(const std::string& content);
    void on_reply_progress();
    void reset();

private:
    struct SignatureState {
        bool has_result = false;
        bool last_failed = false;
        int failure_retries = 0;
        // One-shot replay: a duplicate of a call that succeeded hands the
        // cached result back once instead of an opaque refusal.
        bool replayed = false;
        std::size_t completion_epoch = 0;
        std::string last_result;
    };
    struct ResourceState {
        std::string last_fingerprint;
        int unchanged_results = 0;
        int failed_results = 0;
    };

    void remember_signature_locked(const std::string& signature);
    GuardDecision blocked_locked(const std::string& block_key,
                                 const std::string& message,
                                 const std::string& signature,
                                 const std::string& resource_key);

    std::size_t max_signatures_;
    std::unordered_map<std::string, SignatureState> signatures_;
    std::deque<std::string> signature_order_;
    std::unordered_map<std::string, ResourceState> resources_;
    std::deque<std::string> resource_order_;
    std::string last_block_key_;
    int repeated_blocks_ = 0;
    int consecutive_stalling_replies_ = 0;
    std::size_t progress_epoch_ = 0;
    mutable std::mutex m_;
};

bool looks_like_stalling_reply(const std::string& content);

} // namespace lar
