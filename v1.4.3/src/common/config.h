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
    float min_p = 0.05f;
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
    int max_agent_iterations = 8;
    int ctx_reserve_tokens = 1024;

    // Conversation compression. When history overflows the context budget,
    // everything except the most recent messages is summarized by the loaded
    // model and replaced with one summary record, instead of silently dropping
    // the oldest turns.
    bool enable_compression = true;
    int compress_keep_recent = 8;     // most recent messages kept verbatim
    int compress_summary_tokens = 448; // generation cap for the summary itself

    bool allow_process_tools = false;
    std::vector<std::string> process_allowlist;
    std::string write_root;

    // External open-source tool stack installed by install_helm_tools.cmd.
    std::string tool_root = "F:\\AI Tools";
    std::string tool_python;
    std::string ffmpeg_exe;
    std::string whisper_exe;
    std::string whisper_model;
    std::string piper_exe;
    std::string piper_voice;
    std::string comfyui_url = "http://127.0.0.1:8188";
    std::string comfyui_workflow;
    bool enable_web_tools = true;
    bool enable_image_tools = true;
    bool enable_voice_tools = true;
    bool enable_document_tools = true;
    bool enable_desktop_tools = true;

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
    std::string resolved_piper_voice() const;
    std::string resolved_comfyui_workflow() const;
};

} // namespace lar
