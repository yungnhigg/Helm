#include "agent/run_guard.h"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <vector>
#include <unordered_set>

namespace lar {
namespace {

std::string trim_copy(std::string s) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

std::string collapse_ws(std::string s, bool lowercase) {
    std::string out;
    out.reserve(s.size());
    bool pending_space = false;
    for (unsigned char c : s) {
        if (std::isspace(c)) {
            pending_space = !out.empty();
            continue;
        }
        if (pending_space) out.push_back(' ');
        pending_space = false;
        out.push_back(lowercase ? static_cast<char>(std::tolower(c)) : static_cast<char>(c));
    }
    return out;
}

std::string normalize_query(const std::string& value) {
    return collapse_ws(trim_copy(value), true);
}

std::string normalize_url(std::string value) {
    value = trim_copy(value);
    const auto fragment = value.find('#');
    if (fragment != std::string::npos) value.erase(fragment);

    // Scheme and host are case-insensitive; path/query may not be. Lower only
    // the authority portion so distinct case-sensitive paths remain distinct.
    const auto scheme = value.find("://");
    if (scheme != std::string::npos) {
        const auto authority_end = value.find_first_of("/?", scheme + 3);
        const auto end = authority_end == std::string::npos ? value.size() : authority_end;
        for (std::size_t i = 0; i < end; ++i)
            value[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(value[i])));
    }
    while (value.size() > 1 && value.back() == '/') value.pop_back();
    return value;
}

std::string normalize_path(std::string value) {
    value = trim_copy(value);
    std::replace(value.begin(), value.end(), '\\', '/');

    std::string prefix;
    std::size_t pos = 0;
    if (value.size() >= 2 && std::isalpha(static_cast<unsigned char>(value[0])) && value[1] == ':') {
        prefix.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(value[0]))));
        prefix += ':';
        pos = 2;
    } else if (value.rfind("//", 0) == 0) {
        prefix = "//";
        pos = 2;
    } else if (!value.empty() && value[0] == '/') {
        prefix = "/";
        pos = 1;
    }

    std::vector<std::string> parts;
    std::string part;
    auto flush = [&] {
        if (part.empty() || part == ".") { part.clear(); return; }
        if (part == "..") {
            if (!parts.empty() && parts.back() != "..") parts.pop_back();
            else if (prefix.empty()) parts.push_back(part);
        } else {
            std::transform(part.begin(), part.end(), part.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            parts.push_back(part);
        }
        part.clear();
    };
    for (; pos <= value.size(); ++pos) {
        if (pos == value.size() || value[pos] == '/') flush();
        else part.push_back(value[pos]);
    }

    std::ostringstream out;
    out << prefix;
    if (!prefix.empty() && prefix != "/" && prefix != "//" && !parts.empty()) out << '/';
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) out << '/';
        out << parts[i];
    }
    return out.str();
}

std::string normalized_string_for_key(const std::string& key, const std::string& value) {
    if (key == "query" || key == "q" || key == "search") return normalize_query(value);
    if (key == "url" || key == "site_url") return normalize_url(value);
    if (key == "path" || key == "cwd" || key == "executable" || key == "output_path")
        return normalize_path(value);
    return trim_copy(value);
}

nlohmann::json canonicalize_json(const nlohmann::json& value, const std::string& key = {}) {
    using nlohmann::json;
    if (value.is_object()) {
        json out = json::object();
        for (auto it = value.begin(); it != value.end(); ++it) {
            // Internal dispatch-only fields must never affect a model-call identity.
            if (it.key().rfind("_helm_", 0) == 0) continue;
            out[it.key()] = canonicalize_json(it.value(), it.key());
        }
        return out;
    }
    if (value.is_array()) {
        json out = json::array();
        for (const auto& item : value) out.push_back(canonicalize_json(item));
        return out;
    }
    if (value.is_string()) {
        const std::string s = value.get<std::string>();
        if (key == "content") {
            return "<content " + std::to_string(s.size()) + " bytes " + stable_text_fingerprint(s) + ">";
        }
        return normalized_string_for_key(key, s);
    }
    return value;
}

bool is_non_progress_result(const std::string& result) {
    const std::string t = normalize_query(result);
    if (t.empty()) return true;
    return t.rfind("error:", 0) == 0 || t.rfind("failed", 0) == 0 ||
           t.rfind("cancelled", 0) == 0 || t.find(" failed:") != std::string::npos ||
           t.find(" cancelled:") != std::string::npos || t == "no results" ||
           t.find("no results found") != std::string::npos;
}

bool is_observational_tool(const std::string& name) {
    static const std::unordered_set<std::string> tools = {
        "search_web", "fetch_web_page", "crawl_site", "github_search",
        "search_archive", "read_text_file", "list_directory", "recall_memory",
        "archive_seen", "desktop_screenshot", "extract_document", "describe_image"
    };
    return tools.contains(name);
}

bool is_repeat_exempt_tool(const std::string& name) {
    // These tools are intentionally nondeterministic or time-based; an exact
    // repeat can be the requested operation rather than a loop.
    return name == "get_time" || name == "roll_dice" || name == "demo_job";
}

} // namespace

std::string stable_text_fingerprint(const std::string& text) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (unsigned char c : text) {
        hash ^= static_cast<std::uint64_t>(c);
        hash *= 1099511628211ULL;
    }
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << hash;
    return out.str();
}

std::string canonical_tool_call_signature(const std::string& name,
                                          const nlohmann::json& arguments) {
    return normalize_query(name) + "|args:" + canonicalize_json(arguments).dump();
}

std::string canonical_tool_resource_key(const std::string& name,
                                        const nlohmann::json& arguments) {
    const std::string tool_name = normalize_query(name);
    if (!is_observational_tool(tool_name)) return {};
    const auto string_arg = [&](const char* key) -> std::string {
        auto it = arguments.find(key);
        return it != arguments.end() && it->is_string() ? it->get<std::string>() : std::string{};
    };

    std::string ident = string_arg("url");
    if (!ident.empty()) return tool_name + "|url:" + normalize_url(ident);
    ident = string_arg("query");
    if (!ident.empty()) return tool_name + "|query:" + normalize_query(ident);
    ident = string_arg("path");
    if (!ident.empty()) return tool_name + "|path:" + normalize_path(ident);
    ident = string_arg("file");
    if (!ident.empty()) return tool_name + "|file:" + normalize_path(ident);
    return tool_name + "|args:" + canonicalize_json(arguments).dump();
}

std::string concise_tool_result_summary(const std::string& text, std::size_t max_chars) {
    std::string out = collapse_ws(trim_copy(text), false);
    if (out.size() > max_chars) out = out.substr(0, max_chars) + "…";
    return out;
}

bool looks_like_stalling_reply(const std::string& content) {
    std::string t = trim_copy(content);
    if (t.empty()) return true;
    std::string head = t.substr(0, 48);
    std::transform(head.begin(), head.end(), head.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    static const char* lead_ins[] = {
        "let me", "i'll", "i will", "i'm going to", "first, i", "first i",
        "next, i", "now i'll", "now let me", "starting with", "to start,"
    };
    bool lead = false;
    for (const char* item : lead_ins) {
        if (head.rfind(item, 0) == 0) { lead = true; break; }
    }
    if (!lead) return false;
    const char last = t.back();
    return last == ':' || last == '-' || (t.size() >= 3 && t.substr(t.size() - 3) == "...");
}

ProgressWatchdog::ProgressWatchdog(std::size_t max_signatures)
    : max_signatures_(max_signatures) {}

void ProgressWatchdog::remember_signature_locked(const std::string& signature) {
    if (max_signatures_ == 0) return;
    while (signature_order_.size() >= max_signatures_) {
        signatures_.erase(signature_order_.front());
        signature_order_.pop_front();
    }
    signatures_.emplace(signature, SignatureState{});
    signature_order_.push_back(signature);
}

GuardDecision ProgressWatchdog::blocked_locked(const std::string& block_key,
                                                const std::string& message,
                                                const std::string& signature,
                                                const std::string& resource_key) {
    if (block_key == last_block_key_) ++repeated_blocks_;
    else { last_block_key_ = block_key; repeated_blocks_ = 1; }
    GuardDecision d;
    d.allow = false;
    d.abort_run = repeated_blocks_ >= 3;
    d.message = message;
    if (d.abort_run) {
        d.message += " The model attempted the same non-progressing action three times after being refused, so Helm stopped this turn instead of spinning indefinitely.";
    }
    d.signature = signature;
    d.resource_key = resource_key;
    return d;
}

GuardDecision ProgressWatchdog::before_call(const std::string& name,
                                            const nlohmann::json& arguments) {
    const std::string signature = canonical_tool_call_signature(name, arguments);
    const std::string resource = canonical_tool_resource_key(name, arguments);
    std::lock_guard lk(m_);

    if (!is_repeat_exempt_tool(normalize_query(name))) {
        if (auto seen = signatures_.find(signature); seen != signatures_.end()) {
            // One exact retry is useful after a concrete failure (temporary
            // network outage, locked file, interrupted process). A successful
            // call can run again only after some other material progress, which
            // supports build/edit/test and click/screenshot cycles without
            // permitting an immediate duplicate spin.
            if (seen->second.has_result && seen->second.last_failed && seen->second.failure_retries < 1) {
                ++seen->second.failure_retries;
                seen->second.has_result = false;
                last_block_key_.clear();
                repeated_blocks_ = 0;
                consecutive_stalling_replies_ = 0;
                return {true, false, {}, signature, resource};
            }
            if (seen->second.has_result && seen->second.completion_epoch < progress_epoch_) {
                seen->second.has_result = false;
                seen->second.last_failed = false;
                seen->second.failure_retries = 0;
                last_block_key_.clear();
                repeated_blocks_ = 0;
                consecutive_stalling_replies_ = 0;
                return {true, false, {}, signature, resource};
            }
            return blocked_locked("duplicate:" + signature,
                "error: this exact tool call already ran and no material progress has occurred since. Use the earlier result, take a different action first, or change the resource or arguments materially.",
                signature, resource);
        }
    }

    if (!resource.empty()) {
        const auto it = resources_.find(resource);
        if (it != resources_.end() && (it->second.unchanged_results >= 2 || it->second.failed_results >= 3)) {
            return blocked_locked("stalled:" + resource,
                "error: the progress watchdog has seen repeated examinations of this resource produce no new information. Choose a different source or action, synthesize the findings already gathered, or materially change how the resource is inspected.",
                signature, resource);
        }
    }

    if (!is_repeat_exempt_tool(normalize_query(name))) remember_signature_locked(signature);
    last_block_key_.clear();
    repeated_blocks_ = 0;
    consecutive_stalling_replies_ = 0;
    return {true, false, {}, signature, resource};
}

ProgressObservation ProgressWatchdog::after_result(const std::string& name,
                                                   const nlohmann::json& arguments,
                                                   const std::string& result) {
    ProgressObservation observation;
    const std::string signature = canonical_tool_call_signature(name, arguments);
    observation.resource_key = canonical_tool_resource_key(name, arguments);
    observation.fingerprint = stable_text_fingerprint(result);
    const bool failed = is_non_progress_result(result);
    observation.meaningful_progress = !failed;

    std::lock_guard lk(m_);
    if (!observation.resource_key.empty() && max_signatures_ > 0) {
        auto [resource_it, inserted] = resources_.try_emplace(observation.resource_key);
        if (inserted) {
            while (resource_order_.size() >= max_signatures_) {
                resources_.erase(resource_order_.front());
                resource_order_.pop_front();
            }
            resource_order_.push_back(observation.resource_key);
        }
        auto& state = resource_it->second;
        if (failed) {
            ++state.failed_results;
            observation.meaningful_progress = false;
        } else {
            state.failed_results = 0;
        }

        if (!state.last_fingerprint.empty() && state.last_fingerprint == observation.fingerprint) {
            ++state.unchanged_results;
            observation.meaningful_progress = false;
        } else {
            state.unchanged_results = 0;
            state.last_fingerprint = observation.fingerprint;
        }
        observation.resource_stalled = state.unchanged_results >= 2 || state.failed_results >= 3;
    }

    if (observation.meaningful_progress) {
        ++progress_epoch_;
        last_block_key_.clear();
        repeated_blocks_ = 0;
    }

    if (auto seen = signatures_.find(signature); seen != signatures_.end()) {
        seen->second.has_result = true;
        seen->second.last_failed = failed;
        if (!failed) seen->second.failure_retries = 0;
        seen->second.completion_epoch = progress_epoch_;
    }
    return observation;
}

void ProgressWatchdog::seed_resource_observation(const std::string& resource_key,
                                                    const std::string& fingerprint,
                                                    bool failed) {
    if (resource_key.empty() || max_signatures_ == 0) return;
    std::lock_guard lk(m_);
    auto [it, inserted] = resources_.try_emplace(resource_key);
    if (inserted) {
        while (resource_order_.size() >= max_signatures_) {
            resources_.erase(resource_order_.front());
            resource_order_.pop_front();
        }
        resource_order_.push_back(resource_key);
    }
    auto& state = it->second;
    state.last_fingerprint = fingerprint;
    state.unchanged_results = fingerprint.empty() ? 0 : 1;
    state.failed_results = failed ? 1 : 0;
}

GuardDecision ProgressWatchdog::on_stalling_reply(const std::string& content) {
    std::lock_guard lk(m_);
    ++consecutive_stalling_replies_;
    GuardDecision d;
    d.allow = false;
    d.abort_run = consecutive_stalling_replies_ >= 4;
    d.message = "That response only announced a plan. Make the concrete tool call now instead of narrating the next step.";
    if (d.abort_run) d.message = "The model repeatedly announced plans without taking an action, so Helm stopped this non-progressing turn.";
    return d;
}

void ProgressWatchdog::on_reply_progress() {
    std::lock_guard lk(m_);
    // A narrated reply may be useful to the user, but it is not an external
    // state change and must not unlock an identical side-effecting tool call.
    // Only a materially successful tool result advances progress_epoch_.
    consecutive_stalling_replies_ = 0;
    last_block_key_.clear();
    repeated_blocks_ = 0;
}

void ProgressWatchdog::reset() {
    std::lock_guard lk(m_);
    signatures_.clear();
    signature_order_.clear();
    resources_.clear();
    resource_order_.clear();
    last_block_key_.clear();
    repeated_blocks_ = 0;
    consecutive_stalling_replies_ = 0;
    progress_epoch_ = 0;
}

} // namespace lar
