#include "common/config.h"
#include "common/util.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <algorithm>

using nlohmann::json;
namespace fs = std::filesystem;

namespace lar {

static void load_runtime_fields(const json& j, Config& c) {
    c.n_ctx = j.value("n_ctx", c.n_ctx);
    c.n_gpu_layers = j.value("n_gpu_layers", c.n_gpu_layers);
    c.n_batch = j.value("n_batch", c.n_batch);
    c.n_ubatch = j.value("n_ubatch", c.n_ubatch);
    c.n_threads = j.value("n_threads", c.n_threads);
    c.n_threads_batch = j.value("n_threads_batch", c.n_threads_batch);
    c.flash_attention = j.value("flash_attention", c.flash_attention);
    c.kv_cache_location = j.value("kv_cache_location", c.kv_cache_location);
    c.kv_cache_type = j.value("kv_cache_type", c.kv_cache_type);

    c.sampling.temperature = std::clamp(j.value("temperature", c.sampling.temperature), 0.0f, 5.0f);
    c.sampling.top_k = std::clamp(j.value("top_k", c.sampling.top_k), 0, 1000);
    c.sampling.top_p = std::clamp(j.value("top_p", c.sampling.top_p), 0.0f, 1.0f);
    c.sampling.min_p = std::clamp(j.value("min_p", c.sampling.min_p), 0.0f, 1.0f);
    c.sampling.repeat_penalty = std::clamp(j.value("repeat_penalty", c.sampling.repeat_penalty), 0.5f, 2.0f);
    c.sampling.repeat_last_n = std::clamp(j.value("repeat_last_n", c.sampling.repeat_last_n), 0, 4096);

    c.tool_root = j.value("tool_root", c.tool_root);
    c.tool_python = j.value("tool_python", c.tool_python);
    c.ffmpeg_exe = j.value("ffmpeg_exe", c.ffmpeg_exe);
    c.whisper_exe = j.value("whisper_exe", c.whisper_exe);
    c.whisper_model = j.value("whisper_model", c.whisper_model);
    c.piper_exe = j.value("piper_exe", c.piper_exe);
    c.piper_voice = j.value("piper_voice", c.piper_voice);
    c.max_autonomous_iterations = std::clamp(j.value("max_autonomous_iterations", c.max_autonomous_iterations), 4, 400);
    c.write_root = j.value("write_root", c.write_root);
    c.image_output_dir = j.value("image_output_dir", c.image_output_dir);
    c.archive_db = j.value("archive_db", c.archive_db);
    c.archive_shards = j.value("archive_shards", c.archive_shards);
    c.comfyui_url = j.value("comfyui_url", c.comfyui_url);
    c.comfyui_workflow = j.value("comfyui_workflow", c.comfyui_workflow);
    c.enable_web_tools = j.value("enable_web_tools", c.enable_web_tools);
    c.enable_image_tools = j.value("enable_image_tools", c.enable_image_tools);
    c.enable_voice_tools = j.value("enable_voice_tools", c.enable_voice_tools);
    c.enable_document_tools = j.value("enable_document_tools", c.enable_document_tools);
    c.enable_desktop_tools = j.value("enable_desktop_tools", c.enable_desktop_tools);
    c.enable_archive_tools = j.value("enable_archive_tools", c.enable_archive_tools);
    c.enable_compression = j.value("enable_compression", c.enable_compression);
    c.compress_at_fraction = std::clamp(j.value("compress_at_fraction", c.compress_at_fraction), 0.0, 1.0);
    c.compress_keep_recent = std::clamp(j.value("compress_keep_recent", c.compress_keep_recent), 2, 64);
    c.compress_summary_tokens = std::clamp(j.value("compress_summary_tokens", c.compress_summary_tokens), 64, 4096);
}

bool Config::load(std::string& err) {
    const fs::path user_path = utf8_to_wide(app_data_dir() + "app.json");
    const fs::path default_path = utf8_to_wide(exe_dir() + "config\\app.json");
    std::error_code ec;
    if (!fs::exists(user_path, ec) && fs::exists(default_path, ec)) {
        fs::copy_file(default_path, user_path, fs::copy_options::overwrite_existing, ec);
    }
    std::ifstream f(user_path);
    if (!f) { err = "Helm configuration not found in LocalAppData or beside the executable"; return false; }
    json j;
    try { j = json::parse(f, nullptr, true, true); }
    catch (const std::exception& e) { err = std::string("config/app.json parse error: ") + e.what(); return false; }

    try {
        model_path = j.value("model_path", model_path);
        active_model_id = j.value("active_model_id", active_model_id);
        load_runtime_fields(j, *this);
        max_gen_tokens = j.value("max_gen_tokens", max_gen_tokens);
        max_agent_iterations = j.value("max_agent_iterations", max_agent_iterations);
        ctx_reserve_tokens = j.value("ctx_reserve_tokens", ctx_reserve_tokens);
        allow_process_tools = j.value("allow_process_tools", allow_process_tools);
        if (j.contains("process_allowlist")) process_allowlist = j["process_allowlist"].get<std::vector<std::string>>();
        write_root = j.value("write_root", write_root);
        prompt_format = j.value("prompt_format", prompt_format);
        enable_thinking = j.value("enable_thinking", enable_thinking);
        enable_memory = j.value("enable_memory", enable_memory);
        memory_budget_bytes = j.value("memory_budget_bytes", memory_budget_bytes);
        system_prompt = j.value("system_prompt", system_prompt);
        if (j.contains("stop_strings")) stop_strings = j["stop_strings"].get<std::vector<std::string>>();

        models.clear();
        if (j.contains("models") && j["models"].is_array()) {
            for (const auto& m : j["models"]) {
                ModelProfile p{m.value("id", ""), m.value("name", "Local model"), m.value("path", "")};
                if (!p.id.empty() && !p.path.empty()) models.push_back(std::move(p));
            }
        }
        {
            std::ifstream uf(utf8_to_wide(app_data_dir() + "models.json"), std::ios::binary);
            if (uf) {
                try {
                    json uj = json::parse(uf);
                    for (const auto& m : uj.value("models", json::array())) {
                        ModelProfile p{m.value("id", ""), m.value("name", "Local model"), m.value("path", "")};
                        if (!p.id.empty() && !p.path.empty() &&
                            std::none_of(models.begin(), models.end(), [&](const auto& x) { return x.id == p.id; }))
                            models.push_back(std::move(p));
                    }
                    const std::string user_active = uj.value("active_model_id", std::string{});
                    if (!user_active.empty()) active_model_id = user_active;
                } catch (...) { log("ignoring invalid LocalAppData model catalog"); }
            }
        }
        // UI-edited runtime values override both shipped defaults and the user
        // app.json without rewriting either file.
        {
            std::ifstream rf(utf8_to_wide(app_data_dir() + "runtime.json"), std::ios::binary);
            if (rf) {
                try { load_runtime_fields(json::parse(rf), *this); }
                catch (...) { log("ignoring invalid LocalAppData runtime settings"); }
            }
        }

        if (models.empty() && !model_path.empty()) models.push_back({"default", "Local model", model_path});
        if (active_model_id.empty() && !models.empty()) active_model_id = models.front().id;
        if (!select_model(active_model_id) && !models.empty()) select_model(models.front().id);

        if (j.contains("template")) {
            const auto& t = j["template"];
            tmpl.system_prefix = t.value("system_prefix", "");
            tmpl.system_suffix = t.value("system_suffix", "");
            tmpl.user_prefix = t.value("user_prefix", "");
            tmpl.user_suffix = t.value("user_suffix", "");
            tmpl.assistant_prefix = t.value("assistant_prefix", "");
            tmpl.assistant_suffix = t.value("assistant_suffix", "");
            tmpl.tool_result_role = t.value("tool_result_role", tmpl.tool_result_role);
            tmpl.tool_result_open = t.value("tool_result_open", "");
            tmpl.tool_result_close = t.value("tool_result_close", "");
        }
        if (j.contains("sampling")) {
            const auto& s = j["sampling"];
            sampling.temperature = s.value("temperature", sampling.temperature);
            sampling.top_k = s.value("top_k", sampling.top_k);
            sampling.min_p = s.value("min_p", sampling.min_p);
            sampling.seed = s.value("seed", sampling.seed);
        }
    } catch (const std::exception& e) {
        err = std::string("config/app.json field error: ") + e.what();
        return false;
    }
    return true;
}

bool Config::select_model(const std::string& id) {
    for (const auto& m : models) {
        if (m.id == id) { active_model_id = m.id; model_path = m.path; return true; }
    }
    return false;
}

ModelProfile Config::add_model(const std::string& path) {
    fs::path p = utf8_to_wide(path);
    ModelProfile m{new_uuid(), wide_to_utf8(p.stem().wstring()), path};
    models.push_back(m);
    active_model_id = m.id;
    model_path = m.path;
    persist_model_catalog();
    return m;
}

bool Config::remove_model(const std::string& id) {
    auto it = std::find_if(models.begin(), models.end(), [&](const auto& m) { return m.id == id; });
    if (it == models.end()) return false;
    models.erase(it);
    if (active_model_id == id) {
        active_model_id.clear(); model_path.clear();
        if (!models.empty()) select_model(models.front().id);
    }
    persist_model_catalog();
    return true;
}

void Config::persist_model_catalog() const {
    json j;
    j["active_model_id"] = active_model_id;
    j["models"] = json::array();
    for (const auto& m : models) j["models"].push_back({{"id", m.id}, {"name", m.name}, {"path", m.path}});
    atomic_write_text(utf8_to_wide(app_data_dir() + "models.json"), j.dump(1));
}

void Config::persist_runtime_settings() const {
    json j = {
        {"n_ctx", n_ctx}, {"n_gpu_layers", n_gpu_layers}, {"n_batch", n_batch}, {"n_ubatch", n_ubatch},
        {"n_threads", n_threads}, {"n_threads_batch", n_threads_batch},
        {"flash_attention", flash_attention}, {"kv_cache_location", kv_cache_location}, {"kv_cache_type", kv_cache_type},
        {"temperature", sampling.temperature}, {"top_k", sampling.top_k}, {"top_p", sampling.top_p},
        {"min_p", sampling.min_p}, {"repeat_penalty", sampling.repeat_penalty}, {"repeat_last_n", sampling.repeat_last_n},
        {"tool_root", tool_root}, {"tool_python", tool_python}, {"ffmpeg_exe", ffmpeg_exe},
        {"whisper_exe", whisper_exe}, {"whisper_model", whisper_model},
        {"piper_exe", piper_exe}, {"piper_voice", piper_voice},
        {"max_autonomous_iterations", max_autonomous_iterations},
        {"write_root", write_root}, {"image_output_dir", image_output_dir},
        {"archive_db", archive_db}, {"archive_shards", archive_shards},
        {"comfyui_url", comfyui_url}, {"comfyui_workflow", comfyui_workflow},
        {"enable_web_tools", enable_web_tools}, {"enable_image_tools", enable_image_tools},
        {"enable_voice_tools", enable_voice_tools}, {"enable_document_tools", enable_document_tools},
        {"enable_desktop_tools", enable_desktop_tools},
        {"enable_archive_tools", enable_archive_tools},
        {"enable_compression", enable_compression},
        {"compress_at_fraction", compress_at_fraction},
        {"compress_keep_recent", compress_keep_recent}, {"compress_summary_tokens", compress_summary_tokens}
    };
    atomic_write_text(utf8_to_wide(app_data_dir() + "runtime.json"), j.dump(1));
}

const ModelProfile* Config::active_model() const {
    for (const auto& m : models) if (m.id == active_model_id) return &m;
    return nullptr;
}

std::string Config::resolved_model_path() const {
    fs::path p = utf8_to_wide(model_path);
    if (p.is_absolute()) return model_path;
    return exe_dir() + model_path;
}

static std::string fallback_path(const std::string& configured, const std::string& fallback) {
    return configured.empty() ? fallback : configured;
}

std::string Config::resolved_tool_python() const { return fallback_path(tool_python, tool_root + "\\HelmToolRuntime\\Scripts\\python.exe"); }
std::string Config::resolved_ffmpeg() const { return fallback_path(ffmpeg_exe, tool_root + "\\FFmpeg\\bin\\ffmpeg.exe"); }
std::string Config::resolved_whisper() const { return fallback_path(whisper_exe, tool_root + "\\whisper.cpp\\build\\bin\\Release\\whisper-cli.exe"); }
std::string Config::resolved_whisper_model() const { return fallback_path(whisper_model, tool_root + "\\Models\\ggml-base.en.bin"); }
std::string Config::resolved_piper() const { return fallback_path(piper_exe, tool_root + "\\HelmToolRuntime\\Scripts\\piper.exe"); }
std::string Config::resolved_piper_voice() const { return fallback_path(piper_voice, tool_root + "\\Voices\\en_US-lessac-medium.onnx"); }
std::string Config::resolved_image_output_dir() const {
    return fallback_path(image_output_dir, app_data_dir() + "generated");
}

std::string Config::resolved_comfyui_workflow() const {
    // The installer drops a starter SDXL workflow here, so image generation
    // works out of the box instead of requiring a hand-exported API workflow.
    return fallback_path(comfyui_workflow, tool_root + "\\Helm\\workflows\\sdxl-api.json");
}

} // namespace lar
