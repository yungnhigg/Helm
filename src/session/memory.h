#pragma once
// Global long-term memory: one markdown file, shared across every session and
// agent, injected into the system prompt on every turn.
//
// Deliberately plain markdown rather than JSON, because it is edited by hand
// from the settings pane and a stray character should never brick it. One
// bullet per entry; anything the user types in the editor is preserved as-is.
//
// Writes are explicit. There is no implicit capture: the model does not decide
// what is worth keeping, because a model that does records "ok" and its own
// unaccepted suggestions and the file rots within a week. Entries arrive either
// from a /remember operator command (deterministic, works in chat mode where no
// tools exist) or from the remember() tool in agent mode.
//
// The whole file goes into every system prompt, so it is budgeted. On overflow
// writes are refused rather than silently truncated — losing a memory without
// saying so is worse than refusing to add one.
#include <string>
#include <mutex>
#include <vector>

namespace lar {

struct MemoryStatus {
    bool ok = false;
    std::string message;
    size_t bytes = 0;
    size_t budget = 0;
};

class MemoryStore {
public:
    explicit MemoryStore(size_t budget_bytes);

    // Raw file contents, exactly as stored.
    std::string text() const;
    size_t bytes() const;
    size_t budget() const { return budget_; }

    // Replace the whole file (settings editor). Refuses if over budget.
    MemoryStatus replace(const std::string& text);

    // Append one entry as a bullet. Refuses if it would exceed budget.
    MemoryStatus append(const std::string& entry);

    // Remove entries containing `needle` (case-insensitive). Reports the count.
    MemoryStatus forget(const std::string& needle);

    // Markdown block for the system prompt, or empty when there is nothing.
    std::string prompt_block() const;

    std::string path() const { return path_; }

private:
    bool load();
    bool save(const std::string& text);
    // Assumes m_ is held. The read-modify-write paths need the lock across the
    // whole operation and std::mutex is not recursive.
    bool save_locked(const std::string& text);


    // cache_ is read by the inference thread while it builds a prompt and
    // written by the tool thread when remember/forget fires, plus read and
    // written by the UI thread from the memory editor. Concurrent access to a
    // std::string is undefined behaviour, and this had no lock at all.
    mutable std::mutex m_;
    std::string path_;
    std::string cache_;
    size_t budget_;
};

} // namespace lar
