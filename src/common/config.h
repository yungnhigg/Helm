#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace lar {

struct TemplateConfig {
    std::string system_prefix, system_suffix;
    std::string user_prefix, user_suffix;
    std::string assistant_prefix, assistant_suffix;
    std::string tool_result_role = "user";
    std::string tool_result_open, tool_result_close;
};

struct SamplingConfig {
    float temperature = 0.7f;
    int top_k = 40;
    float top_p = 1.0f;          // 1.0 = disabled (no nucleus cutoff)
    float min_p = 0.05f;
    // Repetition penalty: discourages resampling recent tokens. 1.0 = off.
    // This is the control that stops a model looping on one token forever
    // (e.g. an MoE degenerating into a wall of newlines) - raising it to
    // ~1.05-1.1 is the standard fix for that failure.
    float repeat_penalty = 1.0f;
    int repeat_last_n = 64;      // how many recent tokens the penalty considers
    uint32_t seed = 0xFFFFFFFF;
};

struct ModelProfile {
    std::string id;
    std::string name;
    std::string path;
};

struct Config {
    std::string model_path;
    std::string active_model_id;
    std::vector<ModelProfile> models;

    // llama.cpp runtime controls. These are editable in Helm's Settings panel
    // and persisted separately from the immutable shipped defaults.
    int n_ctx = 8192;
    int n_gpu_layers = 999;
    int n_batch = 512;
    int n_ubatch = 512;
    int n_threads = 0;       // 0 = llama.cpp / hardware default
    int n_threads_batch = 0; // 0 = llama.cpp / hardware default
    std::string flash_attention = "auto"; // auto | on | off
    std::string kv_cache_location = "vram"; // vram | ram
    std::string kv_cache_type = "f16"; // f16 | q8_0 | q4_0

    int max_gen_tokens = 2048;
    int ctx_reserve_tokens = 1024;

    // Conversation compression. When history overflows the context budget,
    // everything except the most recent messages is summarized by the loaded
    // model and replaced with one summary record, instead of silently dropping
    // the oldest turns.
    bool enable_compression = true;
    // Compact when the prompt reaches this fraction of budget, instead of waiting
    // for overflow. 0.80 = fold old turns at 80% full. 0 or >=1 disables the
    // early trigger (compact-on-overflow only).
    double compress_at_fraction = 0.80;
    int compress_keep_recent = 8;     // most recent messages kept verbatim
    int compress_summary_tokens = 448; // generation cap for the summary itself

    bool allow_process_tools = false;
    std::vector<std::string> process_allowlist;
    std::string write_root;
    // Where generate_image saves output. Empty means the default under
    // %LOCALAPPDATA%\Helm\generated. A resolver applies the fallback so
    // callers never need to know the default path themselves.
    std::string image_output_dir;
    std::string resolved_image_output_dir() const;

    // External open-source tool stack installed by install_helm_tools.cmd.
    std::string tool_root = "F:\\AI Tools";
    std::string tool_python;
    std::string ffmpeg_exe;
    std::string whisper_exe;
    std::string whisper_model;
    std::string piper_exe;
    std::string piper_voice;
    // Vision (image description). Not built into the Engine class - the
    // Engine is single-model and not re-entrant, so a second model runs as an
    // external CLI process instead, same pattern as ffmpeg/whisper/piper.
    // CPU-only by design: this must never compete with the main model's VRAM.
    std::string vision_cli_exe;
    std::string vision_model;
    std::string vision_mmproj;
    bool enable_vision_tools = false;
    // Offline archive: SQLite FTS5 index over the Wikipedia JSONL shards.
    // Empty disables the search_archive tool rather than failing at call time.
    std::string archive_db;
    std::string archive_shards;

    std::string comfyui_url = "http://127.0.0.1:8188";
    std::string comfyui_workflow;
    bool enable_web_tools = true;
    bool enable_image_tools = true;
    bool enable_voice_tools = true;
    bool enable_document_tools = true;
    // Off by default: this group can drive the mouse and keyboard, which no
    // research or coding task needs. Opt in from Settings when it is wanted.
    bool enable_desktop_tools = false;
    bool enable_archive_tools = true;

    // Prompt envelope. "chatml" is the only format implemented; the field
    // exists so a Harmony renderer (gpt-oss) can be added as a new file rather
    // than a refactor of the prompt builder.
    std::string prompt_format = "chatml";
    bool enable_thinking = true;

    // Global long-term memory. The whole file is injected into every system
    // prompt, so it is budgeted; over budget, writes are refused rather than
    // silently truncated.
    bool enable_memory = true;
    int memory_budget_bytes = 8192;

    std::string system_prompt;
    std::vector<std::string> stop_strings;
    TemplateConfig tmpl;
    SamplingConfig sampling;

    bool load(std::string& err);
    std::string resolved_model_path() const;
    bool select_model(const std::string& id);
    ModelProfile add_model(const std::string& path);
    bool remove_model(const std::string& id);
    void persist_model_catalog() const;
    void persist_runtime_settings() const;
    const ModelProfile* active_model() const;

    std::string resolved_tool_python() const;
    std::string resolved_ffmpeg() const;
    std::string resolved_whisper() const;
    std::string resolved_whisper_model() const;
    std::string resolved_piper() const;
    std::string resolved_vision_cli() const;
    std::string resolved_piper_voice() const;
    std::string resolved_comfyui_workflow() const;
};

} // namespace lar
