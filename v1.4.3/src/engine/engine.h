#pragma once
#include "common/config.h"
#include "common/util.h"
#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

struct llama_model;
struct llama_context;
struct llama_vocab;

namespace lar {

enum class StopReason { Eos, StopString, Cancelled, MaxTokens, CtxFull, Error };

struct GenResult {
    std::string text;
    StopReason reason = StopReason::Error;
    std::string error;
};

class Engine {
public:
    using PieceFn = std::function<void(const std::string&)>;

    explicit Engine(const Config& cfg);
    ~Engine();

    bool submit(std::function<void()> task);
    void load(std::function<void(bool, const std::string&)> done);
    void unload(std::function<void()> done);
    void cancel() { cancel_.store(true); }
    // Clears the cancel flag. Called once at the start of a turn, not per
    // generation: an agent turn is several generations with tool work between
    // them, and a stop pressed during a tool must survive into the next step.
    void begin_turn() { cancel_.store(false); }
    bool cancelled() const { return cancel_.load(); }

    bool loaded() const { return loaded_.load(); }
    // True when the loaded weights are a GPT-OSS conversion, which speaks the
    // Harmony envelope natively. Detected from the model, not configured.
    bool harmony_mode() const { return harmony_mode_.load(); }
    bool load_sync(std::string& err);
    void unload_sync();
    GenResult generate_sync(const std::string& prompt, const std::string& grammar,
                            const PieceFn& on_piece, int max_tokens = -1);
    int count_tokens_sync(const std::string& text);
    int n_ctx() const { return actual_n_ctx_.load(); }
    int n_batch() const { return actual_n_batch_.load(); }

private:
    void worker_main();

    const Config& cfg_;
    TaskQueue queue_;
    std::thread worker_;
    std::atomic<bool> cancel_{false};
    std::atomic<bool> loaded_{false};
    std::atomic<bool> harmony_mode_{false};
    std::atomic<int> actual_n_ctx_{0};
    std::atomic<int> actual_n_batch_{0};

    llama_model* model_ = nullptr;
    llama_context* ctx_ = nullptr;
    const llama_vocab* vocab_ = nullptr;

    // Tokens currently resident in the KV cache for sequence 0, prompt and
    // generated alike. Each generation reuses the longest common prefix with
    // the new prompt and only decodes what actually changed.
    std::vector<int32_t> cached_;
};

} // namespace lar
