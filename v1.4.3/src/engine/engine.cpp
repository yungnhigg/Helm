// llama.cpp isolation boundary. Written for the pinned revision in CMakeLists.
#include "engine/engine.h"
#include <llama.h>
#include <vector>
#include <algorithm>
#include <memory>
#include <thread>
#include <cctype>

namespace lar {

Engine::Engine(const Config& cfg) : cfg_(cfg) {
    llama_backend_init();
    worker_ = std::thread([this] { worker_main(); });
}

Engine::~Engine() {
    cancel();
    queue_.shutdown();
    if (worker_.joinable()) worker_.join();
    unload_sync();
    llama_backend_free();
}

void Engine::worker_main() {
    std::function<void()> task;
    while (queue_.pop(task)) {
        try { task(); }
        catch (const std::exception& e) { log(std::string("engine task exception: ") + e.what()); }
        catch (...) { log("engine task exception: unknown"); }
    }
}

bool Engine::submit(std::function<void()> task) {
    const bool accepted = queue_.push(std::move(task));
    if (!accepted) log("engine rejected task during shutdown");
    return accepted;
}

void Engine::load(std::function<void(bool, const std::string&)> done) {
    submit([this, done = std::move(done)] {
        std::string err;
        const bool ok = load_sync(err);
        if (done) done(ok, err);
    });
}

void Engine::unload(std::function<void()> done) {
    submit([this, done = std::move(done)] {
        unload_sync();
        if (done) done();
    });
}

bool Engine::load_sync(std::string& err) {
    if (loaded_) return true;
    const std::string path = cfg_.resolved_model_path();
    log("loading model: " + path);

    llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = cfg_.n_gpu_layers;
    model_ = llama_model_load_from_file(path.c_str(), mp);
    if (!model_) { err = "failed to load model: " + path; return false; }

    // Prompt format follows the weights. A GPT-OSS conversion is driven in its
    // native Harmony envelope; everything else uses the configured template.
    std::string identity = path;
    char desc[512]{};
    if (llama_model_desc(model_, desc, sizeof(desc)) > 0) identity += " " + std::string(desc);
    std::transform(identity.begin(), identity.end(), identity.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    harmony_mode_.store(identity.find("gpt-oss") != std::string::npos ||
                        identity.find("gpt oss") != std::string::npos ||
                        identity.find("gpt_oss") != std::string::npos ||
                        identity.find("gptoss") != std::string::npos);

    llama_context_params cp = llama_context_default_params();
    cp.n_ctx = static_cast<uint32_t>(std::max(512, cfg_.n_ctx));
    cp.n_batch = static_cast<uint32_t>(std::max(32, cfg_.n_batch));
    cp.n_ubatch = static_cast<uint32_t>(std::max(32, std::min(cfg_.n_ubatch, cfg_.n_batch)));
    const int hw_threads = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    cp.n_threads = cfg_.n_threads > 0 ? cfg_.n_threads : hw_threads;
    cp.n_threads_batch = cfg_.n_threads_batch > 0 ? cfg_.n_threads_batch : hw_threads;
    cp.flash_attn_type = cfg_.flash_attention == "on" ? LLAMA_FLASH_ATTN_TYPE_ENABLED
                       : cfg_.flash_attention == "off" ? LLAMA_FLASH_ATTN_TYPE_DISABLED
                       : LLAMA_FLASH_ATTN_TYPE_AUTO;
    cp.offload_kqv = cfg_.kv_cache_location != "ram";
    const ggml_type cache_type = cfg_.kv_cache_type == "q4_0" ? GGML_TYPE_Q4_0
                               : cfg_.kv_cache_type == "q8_0" ? GGML_TYPE_Q8_0
                               : GGML_TYPE_F16;
    cp.type_k = cache_type;
    cp.type_v = cache_type;
    ctx_ = llama_init_from_model(model_, cp);
    if (!ctx_) {
        llama_model_free(model_);
        model_ = nullptr;
        err = "failed to create context (n_ctx=" + std::to_string(cfg_.n_ctx) + ")";
        return false;
    }

    vocab_ = llama_model_get_vocab(model_);
    actual_n_ctx_ = static_cast<int>(llama_n_ctx(ctx_));
    actual_n_batch_ = static_cast<int>(llama_n_batch(ctx_));
    loaded_ = true;
    log("model loaded; context=" + std::to_string(actual_n_ctx_) +
        ", batch=" + std::to_string(actual_n_batch_) +
        ", KV=" + cfg_.kv_cache_type + "@" + cfg_.kv_cache_location +
        ", flash=" + cfg_.flash_attention +
        (harmony_mode_.load() ? ", prompt=harmony" : ", prompt=configured"));
    return true;
}

void Engine::unload_sync() {
    if (ctx_) { llama_free(ctx_); ctx_ = nullptr; }
    if (model_) { llama_model_free(model_); model_ = nullptr; }
    vocab_ = nullptr;
    cached_.clear();
    actual_n_ctx_ = 0;
    actual_n_batch_ = 0;
    loaded_ = false;
    harmony_mode_.store(false);
    log("model unloaded, VRAM released");
}

int Engine::count_tokens_sync(const std::string& text) {
    if (!loaded_ || !vocab_) return 0;
    const int n = llama_tokenize(vocab_, text.c_str(), static_cast<int>(text.size()), nullptr, 0, true, true);
    if (n == 0) return 0;
    return n < 0 ? -n : n;
}

static bool ends_with_any(const std::string& s, const std::vector<std::string>& stops, size_t& stop_len) {
    for (const auto& st : stops) {
        if (!st.empty() && s.size() >= st.size() && s.compare(s.size() - st.size(), st.size(), st) == 0) {
            stop_len = st.size();
            return true;
        }
    }
    return false;
}

static std::string token_piece(const llama_vocab* vocab, llama_token token) {
    char local[256];
    int n = llama_token_to_piece(vocab, token, local, sizeof(local), 0, true);
    if (n >= 0) return std::string(local, static_cast<size_t>(n));
    std::string dynamic(static_cast<size_t>(-n), '\0');
    n = llama_token_to_piece(vocab, token, dynamic.data(), static_cast<int>(dynamic.size()), 0, true);
    if (n < 0) return {};
    dynamic.resize(static_cast<size_t>(n));
    return dynamic;
}

GenResult Engine::generate_sync(const std::string& prompt, const std::string& grammar,
                                const PieceFn& on_piece, int max_tokens) {
    GenResult r;
    if (!loaded_ || !ctx_ || !vocab_) { r.error = "no model loaded"; return r; }
    // NOTE: the cancel flag is deliberately NOT cleared here. begin_turn() owns
    // it, so a stop pressed while a tool is running is still honoured when the
    // agent loop comes back for the next generation.
    if (cancel_.load()) { r.reason = StopReason::Cancelled; return r; }

    int needed = llama_tokenize(vocab_, prompt.c_str(), static_cast<int>(prompt.size()), nullptr, 0, true, true);
    if (needed >= 0) {
        if (needed == 0) { r.error = "empty prompt"; return r; }
        r.error = "tokenizer did not return required buffer size";
        return r;
    }
    std::vector<llama_token> toks(static_cast<size_t>(-needed));
    const int tokenized = llama_tokenize(vocab_, prompt.c_str(), static_cast<int>(prompt.size()),
                                         toks.data(), static_cast<int>(toks.size()), true, true);
    if (tokenized < 0) { r.error = "tokenize failed"; return r; }
    toks.resize(static_cast<size_t>(tokenized));

    const int ctx_size = actual_n_ctx_.load();
    const int batch_size = std::max(1, actual_n_batch_.load());
    if (static_cast<int>(toks.size()) >= ctx_size) {
        r.reason = StopReason::CtxFull;
        r.error = "prompt exceeds context";
        return r;
    }

    // --- KV cache reuse -------------------------------------------------
    // Previously this cleared the cache and re-decoded the entire prompt on
    // every generation. In an agent turn that is once per tool-call round, each
    // time over a longer history. Instead, keep everything that has not changed
    // and decode only the divergent suffix.
    //
    // Positions are assigned automatically for a batch with no pos array,
    // continuing from seq_pos_max + 1, so trimming the cache at the divergence
    // point makes the next decode land at exactly the right position.
    llama_memory_t mem = llama_get_memory(ctx_);

    size_t common = 0;
    const size_t max_common = std::min(cached_.size(), toks.size());
    while (common < max_common && cached_[common] == toks[common]) ++common;

    // At least one token must be decoded to produce logits to sample from.
    if (common == toks.size() && common > 0) --common;

    if (common == 0) {
        llama_memory_clear(mem, false);
        cached_.clear();
    } else if (common < cached_.size()) {
        if (!llama_memory_seq_rm(mem, 0, static_cast<int>(common), -1)) {
            // Partial removal unsupported for this memory type: start over.
            llama_memory_clear(mem, false);
            cached_.clear();
            common = 0;
        } else {
            cached_.resize(common);
        }
    }
    if (common > 0) log("KV reuse: " + std::to_string(common) + "/" +
                        std::to_string(toks.size()) + " prompt tokens retained");

    llama_sampler* chain = llama_sampler_chain_init(llama_sampler_chain_default_params());
    if (!chain) { r.error = "failed to create sampler chain"; return r; }
    auto free_chain = [&] { llama_sampler_free(chain); };

    if (!grammar.empty()) {
        llama_sampler* gs = llama_sampler_init_grammar(vocab_, grammar.c_str(), "root");
        if (!gs) {
            free_chain();
            r.error = "grammar failed to parse";
            log("GBNF rejected:\n" + grammar);
            return r;
        }
        llama_sampler_chain_add(chain, gs);
    }
    llama_sampler_chain_add(chain, llama_sampler_init_top_k(cfg_.sampling.top_k));
    llama_sampler_chain_add(chain, llama_sampler_init_min_p(cfg_.sampling.min_p, 1));
    llama_sampler_chain_add(chain, llama_sampler_init_temp(cfg_.sampling.temperature));
    llama_sampler_chain_add(chain, llama_sampler_init_dist(cfg_.sampling.seed));

    auto invalidate_cache = [&] {
        llama_memory_clear(llama_get_memory(ctx_), false);
        cached_.clear();
    };

    for (int i = static_cast<int>(common); i < static_cast<int>(toks.size()); i += batch_size) {
        const int count = std::min(batch_size, static_cast<int>(toks.size()) - i);
        llama_batch batch = llama_batch_get_one(toks.data() + i, count);
        const int rc = llama_decode(ctx_, batch);
        if (rc < 0) {
            free_chain();
            invalidate_cache();
            r.error = "llama_decode failed on prompt (" + std::to_string(rc) + ")";
            return r;
        }
        if (rc > 0) log("llama_decode prompt warning/status: " + std::to_string(rc));
        cached_.insert(cached_.end(), toks.begin() + i, toks.begin() + i + count);
        if (cancel_.load()) { free_chain(); r.reason = StopReason::Cancelled; return r; }
    }

    Utf8Buffer u8;
    std::string pending_stream;
    size_t stop_holdback = 0;
    for (const auto& s : cfg_.stop_strings) stop_holdback = std::max(stop_holdback, s.size());
    auto emit_safe = [&](bool flush_all) {
        size_t emit_n = pending_stream.size();
        if (!flush_all && stop_holdback > 0) {
            emit_n = pending_stream.size() > stop_holdback ? pending_stream.size() - stop_holdback : 0;
        }
        emit_n = utf8_safe_cut(pending_stream, emit_n);
        if (emit_n > 0) {
            std::string out = pending_stream.substr(0, emit_n);
            pending_stream.erase(0, emit_n);
            if (on_piece) on_piece(out);
        }
    };

    const int limit = max_tokens > 0 ? max_tokens : cfg_.max_gen_tokens;
    int n_pos = static_cast<int>(toks.size());
    r.reason = StopReason::MaxTokens;
    for (int i = 0; i < limit; ++i) {
        if (cancel_.load()) { r.reason = StopReason::Cancelled; break; }
        if (n_pos + 1 >= ctx_size) { r.reason = StopReason::CtxFull; break; }

        llama_token tok = llama_sampler_sample(chain, ctx_, -1);
        if (llama_vocab_is_eog(vocab_, tok)) { r.reason = StopReason::Eos; break; }

        std::string valid = u8.feed(token_piece(vocab_, tok));
        if (!valid.empty()) {
            r.text += valid;
            pending_stream += valid;
        }

        size_t stop_len = 0;
        if (ends_with_any(r.text, cfg_.stop_strings, stop_len)) {
            r.text.erase(r.text.size() - stop_len);
            if (pending_stream.size() >= stop_len)
                pending_stream.erase(pending_stream.size() - stop_len);
            emit_safe(true);
            r.reason = StopReason::StopString;
            break;
        }
        emit_safe(false);

        llama_batch batch = llama_batch_get_one(&tok, 1);
        const int rc = llama_decode(ctx_, batch);
        if (rc < 0) {
            r.reason = StopReason::Error;
            r.error = "llama_decode failed (" + std::to_string(rc) + ")";
            invalidate_cache();
            break;
        }
        if (rc > 0) log("llama_decode generation warning/status: " + std::to_string(rc));
        cached_.push_back(tok);
        ++n_pos;
    }

    std::string tail = u8.flush();
    if (!tail.empty()) { r.text += tail; pending_stream += tail; }
    if (r.reason != StopReason::StopString) emit_safe(true);
    free_chain();
    return r;
}

} // namespace lar
