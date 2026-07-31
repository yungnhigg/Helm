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
    // Content version after the call (set by replace; the editor round-trips
    // it so a stale save is rejected instead of clobbering newer entries).
    std::string version;
};

// The memory seam for Phase E's server backend, mirroring ISessionStore.
class IMemoryStore {
public:
    virtual ~IMemoryStore() = default;

    // Raw file contents, exactly as stored.
    virtual std::string text() const = 0;
    virtual size_t bytes() const = 0;
    virtual size_t budget() const = 0;

    // Replace the whole file (settings editor). Refuses if over budget, or if
    // expected_version is non-empty and no longer matches the stored content -
    // the compare-and-swap that stops two writers from silently losing edits.
    virtual MemoryStatus replace(const std::string& text, const std::string& expected_version) = 0;

    // Version token of the current content, for later replace() calls.
    virtual std::string version() const = 0;

    // Append one entry as a bullet. Refuses if it would exceed budget.
    virtual MemoryStatus append(const std::string& entry) = 0;

    // Remove entries containing `needle` (case-insensitive). Reports the count.
    virtual MemoryStatus forget(const std::string& needle) = 0;

    // Markdown block for the system prompt, or empty when there is nothing.
    virtual std::string prompt_block() const = 0;

    virtual std::string path() const = 0;
};

class LocalMemoryStore final : public IMemoryStore {
public:
    explicit LocalMemoryStore(size_t budget_bytes);

    std::string text() const override;
    size_t bytes() const override;
    size_t budget() const override { return budget_; }
    MemoryStatus replace(const std::string& text, const std::string& expected_version) override;
    std::string version() const override;
    MemoryStatus append(const std::string& entry) override;
    MemoryStatus forget(const std::string& needle) override;
    std::string prompt_block() const override;
    std::string path() const override { return path_; }

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
