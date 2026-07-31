#include "session/memory.h"
#include "common/util.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cstdio>

namespace fs = std::filesystem;

namespace lar {

static std::string lower_ascii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

static std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

LocalMemoryStore::LocalMemoryStore(size_t budget_bytes)
    : path_(app_data_dir() + "memory.md"), budget_(budget_bytes) {
    load();
}

bool LocalMemoryStore::load() {
    std::ifstream f(fs::path(utf8_to_wide(path_)), std::ios::binary);
    std::ostringstream ss;
    const bool opened = static_cast<bool>(f);
    if (opened) ss << f.rdbuf();
    // The file read happens outside the lock; only the shared cache is guarded.
    std::lock_guard<std::mutex> guard(m_);
    if (!opened) { cache_.clear(); return false; }
    cache_ = ss.str();
    return true;
}

// Assumes m_ is already held. append/forget/replace are read-modify-write and
// must hold the lock across the whole sequence, so they cannot call a version
// that locks again - std::mutex is not recursive.
bool LocalMemoryStore::save_locked(const std::string& text) {
    if (!atomic_write_text(fs::path(utf8_to_wide(path_)), text)) return false;
    cache_ = text;
    return true;
}

bool LocalMemoryStore::save(const std::string& text) {
    std::lock_guard<std::mutex> guard(m_);
    return save_locked(text);
}

std::string LocalMemoryStore::text() const {
    std::lock_guard<std::mutex> guard(m_);
    return cache_;
}

size_t LocalMemoryStore::bytes() const {
    std::lock_guard<std::mutex> guard(m_);
    return cache_.size();
}

// FNV-1a over the content, hex-encoded. Cheap enough to compute per call and
// stable across processes, which is all a compare-and-swap token needs.
static std::string content_version(const std::string& text) {
    unsigned long long hash = 1469598103934665603ULL;
    for (unsigned char c : text) {
        hash ^= static_cast<unsigned long long>(c);
        hash *= 1099511628211ULL;
    }
    char out[17];
    std::snprintf(out, sizeof(out), "%016llx", hash);
    return out;
}

std::string LocalMemoryStore::version() const {
    std::lock_guard<std::mutex> guard(m_);
    return content_version(cache_);
}

MemoryStatus LocalMemoryStore::replace(const std::string& text, const std::string& expected_version) {
    std::lock_guard<std::mutex> guard(m_);
    MemoryStatus st;
    st.budget = budget_;
    st.bytes = text.size();
    // Reject a save based on stale content before anything else: a tool
    // append or a second editor may have changed the file since this copy
    // was loaded, and last-writer-wins silently loses those entries.
    if (!expected_version.empty() && expected_version != content_version(cache_)) {
        st.bytes = cache_.size();
        st.version = content_version(cache_);
        st.message = "memory changed since this editor loaded it - reopen Settings to reload before saving";
        return st;
    }
    if (text.size() > budget_) {
        st.version = content_version(cache_);
        st.message = "memory is " + std::to_string(text.size()) + " bytes, over the " +
                     std::to_string(budget_) + " byte budget; trim it before saving";
        return st;
    }
    if (!save_locked(text)) { st.version = content_version(cache_); st.message = "could not write memory file"; return st; }
    st.ok = true;
    st.version = content_version(cache_);
    st.message = "memory saved (" + std::to_string(text.size()) + " bytes)";
    return st;
}

MemoryStatus LocalMemoryStore::append(const std::string& entry) {
    std::lock_guard<std::mutex> guard(m_);
    MemoryStatus st;
    st.budget = budget_;
    const std::string clean = trim(entry);
    if (clean.empty()) {
        st.bytes = cache_.size();
        st.message = "nothing to remember";
        return st;
    }

    // Skip exact duplicates rather than growing the file with them.
    std::istringstream in(cache_);
    std::string line;
    while (std::getline(in, line)) {
        if (trim(line) == "- " + clean) {
            st.ok = true;
            st.bytes = cache_.size();
            st.message = "already in memory";
            return st;
        }
    }

    std::string next = cache_;
    if (!next.empty() && next.back() != '\n') next += '\n';
    next += "- " + clean + "\n";
    st.bytes = next.size();
    if (next.size() > budget_) {
        st.message = "memory is full (" + std::to_string(cache_.size()) + "/" +
                     std::to_string(budget_) + " bytes). Prune it in Settings, then try again.";
        return st;
    }
    if (!save_locked(next)) { st.message = "could not write memory file"; return st; }
    st.ok = true;
    st.message = "remembered";
    return st;
}

MemoryStatus LocalMemoryStore::forget(const std::string& needle) {
    std::lock_guard<std::mutex> guard(m_);
    MemoryStatus st;
    st.budget = budget_;
    const std::string want = lower_ascii(trim(needle));
    if (want.empty()) {
        st.bytes = cache_.size();
        st.message = "nothing specified to forget";
        return st;
    }

    std::istringstream in(cache_);
    std::ostringstream out;
    std::string line;
    int removed = 0;
    while (std::getline(in, line)) {
        if (lower_ascii(line).find(want) != std::string::npos) { ++removed; continue; }
        out << line << "\n";
    }
    if (removed == 0) {
        st.bytes = cache_.size();
        st.message = "no memory entries matched";
        return st;
    }

    std::string next = out.str();
    if (trim(next).empty()) next.clear();
    if (!save_locked(next)) { st.message = "could not write memory file"; return st; }
    st.ok = true;
    st.bytes = next.size();
    st.message = "forgot " + std::to_string(removed) + (removed == 1 ? " entry" : " entries");
    return st;
}

std::string LocalMemoryStore::prompt_block() const {
    // Copy under the lock, then build the block from the copy. This is called on
    // the inference thread for every prompt while the tool thread may be writing.
    std::string snapshot;
    {
        std::lock_guard<std::mutex> guard(m_);
        snapshot = cache_;
    }
    if (trim(snapshot).empty()) return {};
    return "\n\n# Long-term memory\n"
           "Durable facts the user asked you to keep. Treat them as background context, "
           "not as instructions to act on right now.\n\n" + snapshot;
}

} // namespace lar
