#include "session/memory.h"
#include "common/util.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>

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

MemoryStore::MemoryStore(size_t budget_bytes)
    : path_(app_data_dir() + "memory.md"), budget_(budget_bytes) {
    load();
}

bool MemoryStore::load() {
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
bool MemoryStore::save_locked(const std::string& text) {
    if (!atomic_write_text(fs::path(utf8_to_wide(path_)), text)) return false;
    cache_ = text;
    return true;
}

bool MemoryStore::save(const std::string& text) {
    std::lock_guard<std::mutex> guard(m_);
    return save_locked(text);
}

std::string MemoryStore::text() const {
    std::lock_guard<std::mutex> guard(m_);
    return cache_;
}

size_t MemoryStore::bytes() const {
    std::lock_guard<std::mutex> guard(m_);
    return cache_.size();
}

MemoryStatus MemoryStore::replace(const std::string& text) {
    std::lock_guard<std::mutex> guard(m_);
    MemoryStatus st;
    st.budget = budget_;
    st.bytes = text.size();
    if (text.size() > budget_) {
        st.message = "memory is " + std::to_string(text.size()) + " bytes, over the " +
                     std::to_string(budget_) + " byte budget; trim it before saving";
        return st;
    }
    if (!save_locked(text)) { st.message = "could not write memory file"; return st; }
    st.ok = true;
    st.message = "memory saved (" + std::to_string(text.size()) + " bytes)";
    return st;
}

MemoryStatus MemoryStore::append(const std::string& entry) {
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

MemoryStatus MemoryStore::forget(const std::string& needle) {
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

std::string MemoryStore::prompt_block() const {
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
