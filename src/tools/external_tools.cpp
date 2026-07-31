#include "tools/external_tools.h"
#include "agent/registry.h"
#include "agent/jobs.h"
#include "common/config.h"
#include "common/util.h"
#include "tools/file_guard.h"
#include <windows.h>
#include <mmsystem.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <chrono>
#include <algorithm>
#include <stdexcept>

namespace fs = std::filesystem;

namespace lar {

static std::wstring quote_arg(const std::wstring& s) {
    if (s.empty()) return L"\"\"";
    if (s.find_first_of(L" \t\"") == std::wstring::npos) return s;
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t c : s) {
        if (c == L'\\') { ++slashes; continue; }
        if (c == L'\"') { out.append(slashes * 2 + 1, L'\\'); out += c; slashes = 0; continue; }
        out.append(slashes, L'\\'); slashes = 0; out += c;
    }
    out.append(slashes * 2, L'\\');
    out += L'\"';
    return out;
}

static void drain_pipe(HANDLE pipe, std::string& output) {
    char buffer[8192];
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) || available == 0) break;
        DWORD read = 0;
        const DWORD request = std::min<DWORD>(available, static_cast<DWORD>(sizeof(buffer)));
        if (!ReadFile(pipe, buffer, request, &read, nullptr) || read == 0) break;
        output.append(buffer, buffer + read);
        if (output.size() > 1000000) output.erase(0, output.size() - 1000000);
    }
}

ProcessCaptureResult run_process_capture(const std::wstring& executable,
                                         const std::vector<std::wstring>& arguments,
                                         const std::wstring& working_directory,
                                         int timeout_seconds,
                                         JobHandle* job,
                                         const std::string& stdin_text) {
    ProcessCaptureResult result;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE read_pipe = nullptr, write_pipe = nullptr;
    HANDLE stdin_read = nullptr, stdin_write = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) throw std::runtime_error("CreatePipe(stdout) failed");
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);
    if (!CreatePipe(&stdin_read, &stdin_write, &sa, 0)) {
        CloseHandle(read_pipe); CloseHandle(write_pipe);
        throw std::runtime_error("CreatePipe(stdin) failed");
    }
    SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write_pipe;
    si.hStdError = write_pipe;
    si.hStdInput = stdin_read;
    PROCESS_INFORMATION pi{};

    std::wstring command = quote_arg(executable);
    for (const auto& arg : arguments) command += L" " + quote_arg(arg);
    std::vector<wchar_t> mutable_cmd(command.begin(), command.end());
    mutable_cmd.push_back(L'\0');

    const BOOL ok = CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr,
                                   working_directory.empty() ? nullptr : working_directory.c_str(), &si, &pi);
    CloseHandle(write_pipe);
    CloseHandle(stdin_read);
    if (!ok) {
        CloseHandle(read_pipe); CloseHandle(stdin_write);
        throw std::runtime_error("CreateProcess failed: " + std::to_string(GetLastError()));
    }

    if (!stdin_text.empty()) {
        DWORD written = 0;
        WriteFile(stdin_write, stdin_text.data(), static_cast<DWORD>(stdin_text.size()), &written, nullptr);
    }
    CloseHandle(stdin_write);

    timeout_seconds = std::clamp(timeout_seconds, 1, 3600);
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        drain_pipe(read_pipe, result.output);
        const DWORD state = WaitForSingleObject(pi.hProcess, 100);
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start).count();
        if (job) job->report(std::min(99, static_cast<int>(elapsed * 100 / timeout_seconds)), "running");
        if (job && job->cancelled()) {
            result.cancelled = true;
            TerminateProcess(pi.hProcess, 2);
            WaitForSingleObject(pi.hProcess, 3000);
            break;
        }
        if (elapsed >= timeout_seconds) {
            result.timed_out = true;
            TerminateProcess(pi.hProcess, 3);
            WaitForSingleObject(pi.hProcess, 3000);
            break;
        }
        if (state == WAIT_OBJECT_0) break;
    }
    drain_pipe(read_pipe, result.output);
    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    result.exit_code = static_cast<int>(exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(read_pipe);
    return result;
}

static std::wstring archive_script() {
    return utf8_to_wide(exe_dir() + "tools_runtime\\archive_index.py");
}

static std::wstring helper_script() {
    return utf8_to_wide(exe_dir() + "tools_runtime\\helm_tools.py");
}

static std::string require_success(const ProcessCaptureResult& r, const std::string& name) {
    if (r.cancelled) return name + " cancelled";
    if (r.timed_out) return "error: " + name + " timed out";
    if (r.exit_code != 0) return "error: " + name + " failed (exit " + std::to_string(r.exit_code) + ")\n" + r.output;
    return r.output;
}

static ProcessCaptureResult run_helper(const Config& cfg, const std::vector<std::wstring>& args,
                                       int timeout, JobHandle* job = nullptr) {
    const std::string python = cfg.resolved_tool_python();
    if (!fs::exists(utf8_to_wide(python))) {
        throw std::runtime_error("Python tool runtime not found at " + python +
                                 ". Run install_helm_tools.cmd (next to Helm.exe) and make sure the "
                                 "Tool root in Settings matches where it installed.");
    }
    std::vector<std::wstring> full{helper_script()};
    full.insert(full.end(), args.begin(), args.end());
    return run_process_capture(utf8_to_wide(python), full, L"", timeout, job);
}

static ProcessCaptureResult run_archive(const Config& cfg, const std::vector<std::wstring>& args,
                                        int timeout, JobHandle* job = nullptr) {
    const std::string python = cfg.resolved_tool_python();
    if (!fs::exists(utf8_to_wide(python)))
        throw std::runtime_error("Python tool runtime not found at " + python +
                                 ". Run install_helm_tools.cmd.");
    std::vector<std::wstring> full{archive_script()};
    full.insert(full.end(), args.begin(), args.end());
    return run_process_capture(utf8_to_wide(python), full, L"", timeout, job);
}

std::string transcribe_audio_file(const Config& cfg, const std::string& input_path, JobHandle* job) {
    fs::path input = utf8_to_wide(input_path);
    fs::path wav = input;
    wav.replace_extension(L".wav");
    if (job) job->report(5, "converting microphone audio");
    auto convert = run_process_capture(utf8_to_wide(cfg.resolved_ffmpeg()),
        {L"-y", L"-i", input.wstring(), L"-ar", L"16000", L"-ac", L"1", wav.wstring()}, L"", 120, job);
    std::string converted = require_success(convert, "FFmpeg conversion");
    if (convert.exit_code != 0 || convert.timed_out || convert.cancelled) return converted;

    fs::path out_base = wav;
    out_base.replace_extension();
    std::string whisper_path = cfg.resolved_whisper();
    if (!fs::exists(utf8_to_wide(whisper_path))) {
        fs::path fallback = utf8_to_wide(cfg.tool_root + "\\whisper.cpp\\build\\bin\\Release\\main.exe");
        if (fs::exists(fallback)) whisper_path = wide_to_utf8(fallback.wstring());
    }
    if (job) job->report(25, "transcribing with Whisper");
    auto transcription = run_process_capture(utf8_to_wide(whisper_path),
        {L"-m", utf8_to_wide(cfg.resolved_whisper_model()), L"-f", wav.wstring(),
         L"-otxt", L"-of", out_base.wstring(), L"-nt"}, L"", 900, job);
    std::string transcribed = require_success(transcription, "Whisper transcription");
    if (transcription.exit_code != 0 || transcription.timed_out || transcription.cancelled) return transcribed;

    const fs::path transcript_path = out_base.wstring() + L".txt";
    std::ifstream f(transcript_path, std::ios::binary);
    if (!f) return "error: Whisper completed but did not create a transcript";
    std::ostringstream text;
    text << f.rdbuf();
    std::error_code ec;
    fs::remove(wav, ec);
    fs::remove(transcript_path, ec);
    return text.str();
}

std::string extract_document_text(const Config& cfg, const std::string& path, size_t max_chars) {
    if (!cfg.enable_document_tools) return {};
    const fs::path source = utf8_to_wide(path);
    const fs::path cache = source.wstring() + L".helm-extracted.txt";
    std::error_code ec;
    if (fs::exists(cache, ec) && fs::last_write_time(cache, ec) >= fs::last_write_time(source, ec)) {
        std::ifstream f(cache, std::ios::binary);
        std::string out(max_chars, '\0');
        f.read(out.data(), static_cast<std::streamsize>(max_chars));
        out.resize(static_cast<size_t>(f.gcount()));
        return out;
    }
    try {
        auto r = run_helper(cfg, {L"extract-document", L"--path", source.wstring(), L"--max-chars", std::to_wstring(max_chars)}, 300);
        if (r.exit_code != 0 || r.timed_out || r.cancelled) {
            log("document extraction failed for " + path + ": " + r.output);
            return {};
        }
        atomic_write_text(cache, r.output);
        return r.output;
    } catch (const std::exception& e) {
        log("document extraction could not start for " + path + ": " + e.what());
        return {};
    }
}

static std::string validate_range(const nlohmann::json& a, const char* key, long long lo, long long hi) {
    if (!a.contains(key)) return {};
    if (!a[key].is_number_integer())
        return std::string("error: ") + key + " must be an integer between " +
               std::to_string(lo) + " and " + std::to_string(hi) + " inclusive";
    const long long v = a[key].get<long long>();
    if (v < lo || v > hi)
        return std::string("error: ") + key + " must be between " + std::to_string(lo) +
               " and " + std::to_string(hi) + " inclusive; received " + std::to_string(v);
    return {};
}

void register_external_tools(Registry& r, const Config& cfg) {
    const Config* c = &cfg;

    r.add({
        "archive_seen",
        "Track which items an ongoing run has already processed, so a perpetual run does not repeat "
        "work after its context is cleared. Call with add set to a comma-separated list of identifiers "
        "(e.g. repository full names) to record them; call with add empty to read back everything "
        "recorded so far and skip anything already in the list.",
        {{"file", ParamType::String, "Absolute path to the seen-list file, e.g. F:\\AgentScratch\\_seen.txt"},
         {"add", ParamType::String, "Comma-separated identifiers to record, or empty to read the list"}},
        ToolClass::Sync,
        [c](const nlohmann::json& a) -> std::string {
            const std::string add = a.value("add", std::string());
            const std::string scoped_root = a.value("_helm_fs_root", std::string{});
            const auto resolved = resolve_tool_file_path(
                a.at("file").get<std::string>(), c->write_root, scoped_root,
                add.empty() ? FileAccessMode::Read : FileAccessMode::Write);
            if (!resolved.ok) return resolved.error;

            const std::string python = c->resolved_tool_python();
            if (!fs::exists(utf8_to_wide(python)))
                return std::string("error: Python tool runtime not found. Run install_helm_tools.cmd.");
            std::vector<std::wstring> args{helper_script(), L"seen-list",
                L"--file", resolved.path.wstring(),
                L"--add", utf8_to_wide(add)};
            auto res = run_process_capture(utf8_to_wide(python), args, L"", 30, nullptr);
            return require_success(res, "seen list");
        },
        {}
    });

    r.add({
        "github_search",
        "Search GitHub repositories and get verified details back: real URL, star count, license, "
        "last-push date, primary language, archived flag, open issue count, and topics. ALWAYS prefer "
        "this over search_web for finding code repositories. The URL and every field come straight "
        "from the GitHub API - never construct or guess a repository URL yourself, and do not fetch "
        "the page unless you need README detail beyond the description.",
        {{"query", ParamType::String, "GitHub search query, e.g. 'unreal engine 5 ability system language:C++'"},
         {"max_results", ParamType::Integer, "How many repositories to return, 1 to 50"},
         {"sort", ParamType::String, "One of: stars, updated, forks, best-match"}},
        ToolClass::Sync,
        [c](const nlohmann::json& a) -> std::string {
            if (!c->enable_web_tools) return std::string("error: web tools are disabled in Settings");
            const std::string python = c->resolved_tool_python();
            if (!fs::exists(utf8_to_wide(python)))
                return std::string("error: Python tool runtime not found. Run install_helm_tools.cmd.");
            const int n = std::clamp(a.value("max_results", 15), 1, 50);
            std::vector<std::wstring> args{helper_script(), L"github-search",
                L"--query", utf8_to_wide(a.at("query").get<std::string>()),
                L"--max-results", std::to_wstring(n),
                L"--sort", utf8_to_wide(a.value("sort", std::string("best-match")))};
            auto res = run_process_capture(utf8_to_wide(python), args, L"", 60, nullptr);
            return require_success(res, "github search");
        },
        {}
    });

    r.add({
        "search_web",
        "Search the current public web, automatically fetch the top result pages (including a JavaScript browser fallback), and return their readable text. Use it for recent or external information. Do not stop at links or ask permission to continue researching.",
        {{"query", ParamType::String, "Specific search query"},
         {"max_results", ParamType::Integer, "Number of results from 1 to 12"}},
        ToolClass::Job, {},
        [c](const nlohmann::json& a, JobHandle& job) {
            if (!c->enable_web_tools) return std::string("error: web tools are disabled in Settings");
            const int count = std::clamp(a.at("max_results").get<int>(), 1, 12);
            auto result = run_helper(*c, {L"web-search", L"--query", utf8_to_wide(a.at("query").get<std::string>()),
                                          L"--max-results", std::to_wstring(count)}, 110, &job);
            return require_success(result, "web search");
        }
    });

    r.add({
        "search_archive",
        "Search the local offline Wikipedia archive stored on this machine. Use it before search_web for "
        "encyclopedic, historical, scientific, or biographical questions: it is faster, works with no "
        "network, and every result resolves to a real article. Fall back to the web only for current "
        "events or when the archive has nothing relevant.",
        {{"query", ParamType::String, "Search terms; plain keywords work best"},
         {"max_results", ParamType::Integer, "Number of chunks to return, from 1 to 20"}},
        ToolClass::Job, {},
        [c](const nlohmann::json& a, JobHandle& job) {
            if (!c->enable_archive_tools) return std::string("error: archive tools are disabled in Settings");
            if (c->archive_db.empty())
                return std::string("error: no archive index configured. Build one with "
                                   "archive_index.py and set Archive index in Settings.");
            if (!fs::exists(utf8_to_wide(c->archive_db)))
                return std::string("error: archive index not found at " + c->archive_db);
            const int count = std::clamp(a.at("max_results").get<int>(), 1, 20);
            std::vector<std::wstring> args{L"search", L"--db", utf8_to_wide(c->archive_db),
                L"--query", utf8_to_wide(a.at("query").get<std::string>()),
                L"-n", std::to_wstring(count)};
            // Recorded at index time; only needed when the shards have moved.
            if (!c->archive_shards.empty()) {
                args.push_back(L"--source");
                args.push_back(utf8_to_wide(c->archive_shards));
            }
            // Local disk, so this is fast; the ceiling only catches a cold cache
            // on a multi-gigabyte index.
            auto result = run_archive(*c, args, 60, &job);
            return require_success(result, "archive search");
        }
    });

    r.add({
        "fetch_web_page",
        "Fetch and extract one public web page. Falls back to headless Chromium when the static HTML is empty or JavaScript-rendered. Use it immediately when search results are incomplete; do not ask permission first.",
        {{"url", ParamType::String, "Absolute http or https URL"},
         {"max_chars", ParamType::Integer, "Readable characters to return, 1000 to 20000. Prefer 8000 or less; large values overflow the context window and the result gets truncated anyway."}},
        ToolClass::Job, {},
        [c](const nlohmann::json& a, JobHandle& job) {
            if (!c->enable_web_tools) return std::string("error: web tools are disabled in Settings");
            // 100000 was reachable and guaranteed an overflow. The loop clamps by
            // context as well, but a sane ceiling here keeps the fetch itself cheap.
            const int limit = std::clamp(a.at("max_chars").get<int>(), 1000, 20000);
            auto result = run_helper(*c, {L"web-fetch", L"--url", utf8_to_wide(a.at("url").get<std::string>()),
                                          L"--max-chars", std::to_wstring(limit)}, 75, &job);
            return require_success(result, "web page fetch");
        }
    });

    // Built at registration time so the description names the actual
    // configured output directory - same reasoning as write_text_file: the
    // model should know where the file lands before it acts, not have to
    // infer or guess it afterward.
    std::string image_desc =
        "Generate an image through local ComfyUI. Helm supplies a starter SDXL workflow and "
        "starts ComfyUI automatically when it is installed but not running. Saved images are "
        "written to " + cfg.resolved_image_output_dir() + " - if you need to reference, move, or "
        "open the file afterward, that is the directory to look in. You cannot see the generated "
        "image yourself; call describe_image on the saved path afterward if you want to verify what "
        "it actually looks like before reporting success.";
    r.add({
        "generate_image",
        image_desc,
        {{"prompt", ParamType::String, "Positive image prompt"},
         {"negative_prompt", ParamType::String, "Negative prompt, or an empty string"},
         {"width", ParamType::Integer, "Image width in pixels"},
         {"height", ParamType::Integer, "Image height in pixels"}},
        ToolClass::Job, {},
        [c](const nlohmann::json& a, JobHandle& job) {
            if (!c->enable_image_tools) return std::string("error: image tools are disabled in Settings");
            const fs::path output = utf8_to_wide(c->resolved_image_output_dir());
            fs::create_directories(output);
            auto result = run_helper(*c, {L"comfy-generate", L"--url", utf8_to_wide(c->comfyui_url),
                L"--workflow", utf8_to_wide(c->resolved_comfyui_workflow()),
                L"--prompt", utf8_to_wide(a.at("prompt").get<std::string>()),
                L"--negative-prompt", utf8_to_wide(a.at("negative_prompt").get<std::string>()),
                L"--width", std::to_wstring(std::clamp(a.at("width").get<int>(), 256, 4096)),
                L"--height", std::to_wstring(std::clamp(a.at("height").get<int>(), 256, 4096)),
                L"--output-dir", output.wstring(),
                L"--start-command", utf8_to_wide(c->tool_root + "\\START_COMFYUI.cmd")}, 1200, &job);
            return require_success(result, "ComfyUI image generation");
        }
    });

    r.add({
        "describe_image",
        "Describe the contents of a local image file using a small CPU-only vision model. This is "
        "the only way to know what an image actually looks like - you cannot see images otherwise, "
        "including ones you just generated with generate_image. Runs entirely on CPU so it never "
        "competes with the main model for VRAM. The description is a short paragraph, not an "
        "exhaustive account, so use it to check a generation succeeded or to identify a file, not as "
        "a substitute for real vision on fine detail.",
        {{"path", ParamType::String, "Absolute path to a local image file (PNG or JPEG)"}},
        ToolClass::Job, {},
        [c](const nlohmann::json& a, JobHandle& job) -> std::string {
            if (!c->enable_vision_tools) return "error: vision tools are disabled in Settings";
            const fs::path exe = utf8_to_wide(c->resolved_vision_cli());
            if (!fs::exists(exe)) return "error: vision CLI not found at " + c->resolved_vision_cli() +
                " - set it in Settings, or build/download llama-mtmd-cli.exe.";
            const fs::path model = utf8_to_wide(c->vision_model);
            const fs::path mmproj = utf8_to_wide(c->vision_mmproj);
            if (c->vision_model.empty() || !fs::exists(model))
                return "error: vision model path is not set or does not exist. Configure it in Settings.";
            if (c->vision_mmproj.empty() || !fs::exists(mmproj))
                return "error: vision mmproj (projector) path is not set or does not exist. Configure it in Settings.";
            const std::string scoped_root = a.value("_helm_fs_root", std::string{});
            const auto resolved = resolve_tool_file_path(a.at("path").get<std::string>(),
                                                         c->write_root, scoped_root,
                                                         FileAccessMode::Read);
            if (!resolved.ok) return resolved.error;
            const fs::path image = resolved.path;
            if (!fs::exists(image)) return "error: image file does not exist at that path";

            // CPU-only (-ngl 0, --no-mmproj-offload): this must never touch
            // the VRAM the main text model is using. -c 4096 is generous
            // headroom for one image's tokens plus a short prompt - it is the
            // CLI's OWN internal context and cannot overflow regardless of
            // which small vision model is configured. -n caps the model's
            // own output length, which is what actually protects the CALLING
            // agent's context: a short, bounded caption comes back as the
            // tool result instead of an unbounded ramble. clamp_tool_result
            // is still applied on the result as a second, independent net.
            auto result = run_process_capture(exe.wstring(), {
                L"-m", model.wstring(),
                L"--mmproj", mmproj.wstring(),
                L"--image", image.wstring(),
                L"-p", L"Describe this image in three sentences or fewer: what is depicted, the "
                       L"overall composition, and anything that looks visually wrong or malformed.",
                L"-ngl", L"0",
                L"--no-mmproj-offload",
                L"-c", L"4096",
                L"-n", L"220"
            }, L"", 180, &job);
            return require_success(result, "image description");
        }
    });

    r.add({
        "speak_text",
        "Speak text aloud through the local Piper voice and save the generated WAV file.",
        {{"text", ParamType::String, "Text to speak"}},
        ToolClass::Job, {},
        [c](const nlohmann::json& a, JobHandle& job) {
            if (!c->enable_voice_tools) return std::string("error: voice tools are disabled in Settings");
            const fs::path dir = utf8_to_wide(app_data_dir() + "voice");
            fs::create_directories(dir);
            const fs::path output = dir / utf8_to_wide("piper-" + new_uuid() + ".wav");
            // piper1-gpl (pip install piper-tts) takes -m/-f and the text as a
            // trailing argument after "--". The older standalone rhasspy binary
            // used --model/--output_file with text on stdin; passing those to
            // the current CLI fails outright. stdin is still supplied so a
            // legacy binary at the configured path keeps working.
            auto result = run_process_capture(utf8_to_wide(c->resolved_piper()),
                {L"-m", utf8_to_wide(c->resolved_piper_voice()),
                 L"-f", output.wstring(),
                 L"--", utf8_to_wide(a.at("text").get<std::string>())},
                L"", 300, &job, a.at("text").get<std::string>() + "\n");
            const std::string status = require_success(result, "Piper speech");
            if (result.exit_code != 0 || result.timed_out || result.cancelled) return status;
            PlaySoundW(output.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
            return "speaking; WAV saved to " + wide_to_utf8(output.wstring());
        }
    });

    r.add({
        "extract_document",
        "Extract text from a local PDF, DOCX, XLSX, or PPTX file using the configured local document tools.",
        {{"path", ParamType::String, "Absolute local document path"},
         {"max_chars", ParamType::Integer, "Maximum characters from 1000 to 500000"}},
        ToolClass::Job, {},
        [c](const nlohmann::json& a, JobHandle& job) {
            if (!c->enable_document_tools) return std::string("error: document tools are disabled in Settings");
            job.report(10, "extracting document text");
            const std::string scoped_root = a.value("_helm_fs_root", std::string{});
            const auto resolved = resolve_tool_file_path(a.at("path").get<std::string>(),
                                                         c->write_root, scoped_root,
                                                         FileAccessMode::Read);
            if (!resolved.ok) return resolved.error;
            const size_t limit = static_cast<size_t>(std::clamp(a.at("max_chars").get<int>(), 1000, 500000));
            const std::string text = extract_document_text(*c, wide_to_utf8(resolved.path.wstring()), limit);
            return text.empty() ? std::string("error: no text could be extracted") : text;
        }
    });

    r.add({
        "desktop_screenshot",
        "Capture the Windows desktop to a PNG and return its local path. Inspect before clicking unfamiliar interfaces.",
        {{"monitor", ParamType::Integer, "0 for the full virtual desktop, or a monitor index"}},
        ToolClass::Job, {},
        [c](const nlohmann::json& a, JobHandle& job) {
            if (!c->enable_desktop_tools) return std::string("error: desktop tools are disabled in Settings");
            const fs::path dir = utf8_to_wide(app_data_dir() + "screenshots");
            fs::create_directories(dir);
            const fs::path output = dir / utf8_to_wide("desktop-" + new_uuid() + ".png");
            auto result = run_helper(*c, {L"desktop-screenshot", L"--output", output.wstring(), L"--monitor",
                                          std::to_wstring(a.at("monitor").get<int>())}, 60, &job);
            return require_success(result, "desktop screenshot");
        }
    });

    r.add({
        "desktop_click",
        "Click a desktop coordinate. Take a screenshot first and avoid destructive actions without clear user intent.",
        {{"x", ParamType::Integer, "Horizontal screen coordinate"},
         {"y", ParamType::Integer, "Vertical screen coordinate"},
         {"button", ParamType::String, "left, right, or middle"},
         {"clicks", ParamType::Integer, "Number of clicks from 1 to 3"}},
        ToolClass::Job, {},
        [c](const nlohmann::json& a, JobHandle& job) {
            if (!c->enable_desktop_tools) return std::string("error: desktop tools are disabled in Settings");
            auto result = run_helper(*c, {L"desktop-click", L"--x", std::to_wstring(a.at("x").get<int>()),
                L"--y", std::to_wstring(a.at("y").get<int>()), L"--button", utf8_to_wide(a.at("button").get<std::string>()),
                L"--clicks", std::to_wstring(std::clamp(a.at("clicks").get<int>(), 1, 3))}, 30, &job);
            return require_success(result, "desktop click");
        }
    });

    r.add({
        "desktop_type",
        "Type text into the currently focused desktop control.",
        {{"text", ParamType::String, "Text to type"},
         {"interval_ms", ParamType::Integer, "Delay between keystrokes from 0 to 500 ms"}},
        ToolClass::Job, {},
        [c](const nlohmann::json& a, JobHandle& job) {
            if (!c->enable_desktop_tools) return std::string("error: desktop tools are disabled in Settings");
            auto result = run_helper(*c, {L"desktop-type", L"--text", utf8_to_wide(a.at("text").get<std::string>()),
                L"--interval-ms", std::to_wstring(std::clamp(a.at("interval_ms").get<int>(), 0, 500))}, 120, &job);
            return require_success(result, "desktop typing");
        }
    });

    r.add({
        "desktop_hotkey",
        "Press a keyboard shortcut in the active desktop application.",
        {{"keys", ParamType::String, "Comma-separated keys, for example ctrl,l or alt,tab"}},
        ToolClass::Job, {},
        [c](const nlohmann::json& a, JobHandle& job) {
            if (!c->enable_desktop_tools) return std::string("error: desktop tools are disabled in Settings");
            auto result = run_helper(*c, {L"desktop-hotkey", L"--keys", utf8_to_wide(a.at("keys").get<std::string>())}, 30, &job);
            return require_success(result, "desktop hotkey");
        }
    });

    // ---------------------------------------------------------------------
    // OSINT pack. Public registries and transparency logs only. RDAP has
    // replaced port-43 WHOIS and returns structured JSON, so there is no
    // per-TLD parser here. crt.sh is job-class because it routinely takes
    // tens of seconds and is often overloaded.
    // ---------------------------------------------------------------------

    r.add({
        "domain_whois",
        "Registration record for a domain via RDAP, the structured JSON successor to WHOIS. "
        "Returns registrar, creation/expiry/updated dates, status codes, nameservers and DNSSEC. "
        "Registrant contact fields are usually redacted by the registry under GDPR - that is "
        "expected, not an error. A few ccTLDs (.io and .dev among them) publish no RDAP server yet.",
        {{"domain", ParamType::String, "Domain name, e.g. example.com"}},
        ToolClass::Sync,
        [c](const nlohmann::json& a) -> std::string {
            if (!c->enable_osint_tools) return std::string("error: OSINT tools are disabled in Settings");
            auto res = run_helper(*c, {L"rdap", L"--target",
                utf8_to_wide(a.at("domain").get<std::string>())}, 45);
            return require_success(res, "RDAP domain lookup");
        },
        {}
    });

    r.add({
        "ip_info",
        "Registration data for an IP address via RDAP: the allocating registry, network range, "
        "owning organisation, country and status. This is authoritative registry data, not "
        "city-level geolocation - no keyless source provides reliable city geo.",
        {{"ip", ParamType::String, "IPv4 or IPv6 address"}},
        ToolClass::Sync,
        [c](const nlohmann::json& a) -> std::string {
            if (!c->enable_osint_tools) return std::string("error: OSINT tools are disabled in Settings");
            auto res = run_helper(*c, {L"rdap", L"--target",
                utf8_to_wide(a.at("ip").get<std::string>())}, 45);
            return require_success(res, "RDAP IP lookup");
        },
        {}
    });

    r.add({
        "dns_lookup",
        "Resolve DNS records for a domain over DNS-over-HTTPS. Use it to confirm what a host "
        "actually points at before drawing conclusions from other sources.",
        {{"domain", ParamType::String, "Domain name to resolve"},
         {"record_types", ParamType::String, "Comma-separated record types, e.g. A,AAAA,MX,NS,TXT"}},
        ToolClass::Sync,
        [c](const nlohmann::json& a) -> std::string {
            if (!c->enable_osint_tools) return std::string("error: OSINT tools are disabled in Settings");
            std::string types = a.value("record_types", std::string("A"));
            if (types.empty()) types = "A";
            auto res = run_helper(*c, {L"dns-lookup",
                L"--domain", utf8_to_wide(a.at("domain").get<std::string>()),
                L"--types", utf8_to_wide(types)}, 45);
            return require_success(res, "DNS lookup");
        },
        {}
    });

    r.add({
        "edgar_company",
        "SEC EDGAR filer profile and recent filings for a US public company. Accepts a ticker "
        "(exact match preferred) or company name. Returns CIK, exchanges, SIC description and "
        "a list of recent filings with direct document URLs.",
        {{"query", ParamType::String, "Ticker symbol or company name, e.g. AAPL"},
         {"max_results", ParamType::Integer, "How many recent filings to list, 1 to 40"}},
        ToolClass::Sync,
        [c](const nlohmann::json& a) -> std::string {
            if (!c->enable_osint_tools) return std::string("error: OSINT tools are disabled in Settings");
            const int n = std::clamp(a.value("max_results", 15), 1, 40);
            auto res = run_helper(*c, {L"edgar-company",
                L"--query", utf8_to_wide(a.at("query").get<std::string>()),
                L"--max-results", std::to_wstring(n)}, 60);
            return require_success(res, "EDGAR company lookup");
        },
        {},
        [](const nlohmann::json& a) -> std::string {
            return validate_range(a, "max_results", 1, 40);
        }
    });

    r.add({
        "edgar_search",
        "Full-text search across the body of every SEC filing since 2001. Use it to find which "
        "companies disclosed a given term, not to look up one known company.",
        {{"query", ParamType::String, "Search phrase; quote it for an exact phrase"},
         {"forms", ParamType::String, "Form filter such as 10-K or 8-K, or empty for all forms"},
         {"max_results", ParamType::Integer, "How many filings to return, 1 to 30"}},
        ToolClass::Sync,
        [c](const nlohmann::json& a) -> std::string {
            if (!c->enable_osint_tools) return std::string("error: OSINT tools are disabled in Settings");
            const int n = std::clamp(a.value("max_results", 15), 1, 30);
            auto res = run_helper(*c, {L"edgar-search",
                L"--query", utf8_to_wide(a.at("query").get<std::string>()),
                L"--forms", utf8_to_wide(a.value("forms", std::string(""))),
                L"--max-results", std::to_wstring(n)}, 75);
            return require_success(res, "EDGAR full-text search");
        },
        {},
        [](const nlohmann::json& a) -> std::string {
            return validate_range(a, "max_results", 1, 30);
        }
    });

    r.add({
        "breach_check",
        "Check whether an email address appears in known data breaches (Have I Been Pwned). "
        "Requires a paid HIBP subscription key in the HIBP_API_KEY environment variable - every "
        "HIBP endpoint that searches by address needs one. Intended for accounts you control.",
        {{"account", ParamType::String, "Email address to check"}},
        ToolClass::Sync,
        [c](const nlohmann::json& a) -> std::string {
            if (!c->enable_osint_tools) return std::string("error: OSINT tools are disabled in Settings");
            auto res = run_helper(*c, {L"hibp-account", L"--account",
                utf8_to_wide(a.at("account").get<std::string>())}, 40);
            return require_success(res, "HIBP breach check");
        },
        {}
    });

    r.add({
        "company_registry_search",
        "Search corporate registry records across jurisdictions (OpenCorporates): legal name, "
        "company number, jurisdiction, status and incorporation date. Requires an API token in "
        "the OPENCORPORATES_API_TOKEN environment variable.",
        {{"query", ParamType::String, "Company name to search for"},
         {"jurisdiction", ParamType::String, "Jurisdiction code such as gb or us_de, or empty for all"},
         {"max_results", ParamType::Integer, "How many companies to return, 1 to 30"}},
        ToolClass::Sync,
        [c](const nlohmann::json& a) -> std::string {
            if (!c->enable_osint_tools) return std::string("error: OSINT tools are disabled in Settings");
            const int n = std::clamp(a.value("max_results", 15), 1, 30);
            auto res = run_helper(*c, {L"opencorporates",
                L"--query", utf8_to_wide(a.at("query").get<std::string>()),
                L"--jurisdiction", utf8_to_wide(a.value("jurisdiction", std::string(""))),
                L"--max-results", std::to_wstring(n)}, 45);
            return require_success(res, "OpenCorporates search");
        },
        {},
        [](const nlohmann::json& a) -> std::string {
            return validate_range(a, "max_results", 1, 30);
        }
    });

    r.add({
        "cert_subdomains",
        "Enumerate subdomains from public Certificate Transparency logs (crt.sh). CT logs record "
        "every issued certificate, so this surfaces hosts that were never published in DNS. Slow "
        "by nature - crt.sh commonly takes tens of seconds and is frequently overloaded.",
        {{"domain", ParamType::String, "Root domain, e.g. example.com"},
         {"max_results", ParamType::Integer, "How many subdomains to return, 1 to 1000"}},
        ToolClass::Job, {},
        [c](const nlohmann::json& a, JobHandle& job) -> std::string {
            if (!c->enable_osint_tools) return std::string("error: OSINT tools are disabled in Settings");
            const int n = std::clamp(a.value("max_results", 200), 1, 1000);
            job.report(10, "querying certificate transparency logs");
            auto res = run_helper(*c, {L"crtsh",
                L"--domain", utf8_to_wide(a.at("domain").get<std::string>()),
                L"--max-results", std::to_wstring(n),
                L"--timeout", L"120"}, 150, &job);
            job.report(100, "complete");
            return require_success(res, "certificate transparency query");
        },
        [](const nlohmann::json& a) -> std::string {
            return validate_range(a, "max_results", 1, 1000);
        }
    });

    r.add({
        "domain_recon",
        "Full passive reconnaissance on one domain: RDAP registration record, DNS records, and "
        "Certificate Transparency subdomains, in one job. Passive only - it reads public "
        "registries and logs and never contacts the target's own hosts.",
        {{"domain", ParamType::String, "Root domain, e.g. example.com"},
         {"max_subdomains", ParamType::Integer, "Cap on subdomains reported, 1 to 500"}},
        ToolClass::Job, {},
        [c](const nlohmann::json& a, JobHandle& job) -> std::string {
            if (!c->enable_osint_tools) return std::string("error: OSINT tools are disabled in Settings");
            const std::string domain = a.at("domain").get<std::string>();
            const std::wstring wdomain = utf8_to_wide(domain);
            const int n = std::clamp(a.value("max_subdomains", 100), 1, 500);
            std::string report = "=== domain_recon: " + domain + " ===\n";

            job.report(10, "RDAP registration record");
            if (job.cancelled()) return report + "cancelled";
            auto rdap = run_helper(*c, {L"rdap", L"--target", wdomain}, 45, &job);
            report += "\n--- registration (RDAP) ---\n" + require_success(rdap, "RDAP lookup") + "\n";

            job.report(40, "DNS records");
            if (job.cancelled()) return report + "cancelled";
            auto dns = run_helper(*c, {L"dns-lookup", L"--domain", wdomain,
                L"--types", L"A,AAAA,MX,NS,TXT,CNAME"}, 45, &job);
            report += "\n--- DNS ---\n" + require_success(dns, "DNS lookup") + "\n";

            job.report(65, "certificate transparency subdomains");
            if (job.cancelled()) return report + "cancelled";
            auto ct = run_helper(*c, {L"crtsh", L"--domain", wdomain,
                L"--max-results", std::to_wstring(n), L"--timeout", L"120"}, 150, &job);
            report += "\n--- subdomains (certificate transparency) ---\n"
                   + require_success(ct, "certificate transparency query") + "\n";

            job.report(100, "complete");
            return report;
        },
        [](const nlohmann::json& a) -> std::string {
            return validate_range(a, "max_subdomains", 1, 500);
        }
    });

    // ---------------------------------------------------------------------
    // Free-source tools. No API key required by any of these.
    // ---------------------------------------------------------------------

    r.add({
        "phone_lookup",
        "Identify an unknown US or Canadian phone number: the carrier that was assigned the "
        "number block, rate center, LATA, switch, and the rate centre coordinates. A "
        "wholesale VoIP carrier on an unfamiliar inbound call is the strongest available spam "
        "signal. Does NOT return a subscriber name - no free source provides one. Pair it with "
        "search_web on the formatted number, which finds business listings and complaint reports.",
        {{"number", ParamType::String, "US or Canada phone number in any format"}},
        ToolClass::Sync,
        [c](const nlohmann::json& a) -> std::string {
            if (!c->enable_osint_tools) return std::string("error: OSINT tools are disabled in Settings");
            auto res = run_helper(*c, {L"phone-lookup", L"--number",
                utf8_to_wide(a.at("number").get<std::string>())}, 45);
            return require_success(res, "phone lookup");
        },
        {}
    });

    r.add({
        "geocode_address",
        "Convert a US street address to latitude and longitude using the Census Bureau "
        "geocoder. Free and unlimited. Use it before parcel_lookup when you only have an "
        "address, or any time coordinates are needed.",
        {{"address", ParamType::String, "Full US street address including city and state"}},
        ToolClass::Sync,
        [c](const nlohmann::json& a) -> std::string {
            if (!c->enable_osint_tools) return std::string("error: OSINT tools are disabled in Settings");
            auto res = run_helper(*c, {L"geocode", L"--address",
                utf8_to_wide(a.at("address").get<std::string>())}, 45);
            return require_success(res, "geocode");
        },
        {}
    });

    r.add({
        "parcel_lookup",
        "Find the property owner of record by street address or by latitude/longitude, using "
        "county assessor data published by state GIS agencies. Coverage comes from a local "
        "reference database of parcel services; if a state is not registered the error names "
        "the ones that are. Pass an address OR coordinates - with coordinates you must also "
        "give the two-letter state, since there is no address to derive it from.",
        {{"address", ParamType::String, "Street address, or empty string if using coordinates"},
         {"state", ParamType::String, "Two-letter state code, required with coordinates, otherwise empty"},
         {"latitude", ParamType::Number, "Latitude, or 0 if using an address"},
         {"longitude", ParamType::Number, "Longitude, or 0 if using an address"}},
        ToolClass::Sync,
        [c](const nlohmann::json& a) -> std::string {
            if (!c->enable_osint_tools) return std::string("error: OSINT tools are disabled in Settings");
            const std::string addr = a.value("address", std::string(""));
            const std::string state = a.value("state", std::string(""));
            const double lat = a.value("latitude", 0.0);
            const double lon = a.value("longitude", 0.0);
            std::vector<std::wstring> args{L"parcel-lookup"};
            if (!addr.empty()) {
                args.push_back(L"--address");
                args.push_back(utf8_to_wide(addr));
            } else if (lat != 0.0 && lon != 0.0) {
                args.push_back(L"--latitude");
                args.push_back(utf8_to_wide(std::to_string(lat)));
                args.push_back(L"--longitude");
                args.push_back(utf8_to_wide(std::to_string(lon)));
            } else {
                return std::string("error: supply either an address or a latitude/longitude pair");
            }
            if (!state.empty()) {
                args.push_back(L"--state");
                args.push_back(utf8_to_wide(state));
            }
            auto res = run_helper(*c, args, 60);
            return require_success(res, "parcel lookup");
        },
        {},
        [](const nlohmann::json& a) -> std::string {
            const bool has_addr = !a.value("address", std::string()).empty();
            const double lat = a.value("latitude", 0.0);
            const double lon = a.value("longitude", 0.0);
            const bool has_coords = lat != 0.0 && lon != 0.0;
            if (!has_addr && !has_coords)
                return std::string("error: supply either address, or both latitude and longitude");
            if (has_addr && has_coords)
                return std::string("error: supply address OR coordinates, not both");
            if (has_coords && a.value("state", std::string()).empty())
                return std::string("error: state is required when querying by coordinates");
            if (has_coords && (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0))
                return std::string("error: latitude must be -90..90 and longitude -180..180");
            return {};
        }
    });

    r.add({
        "broker_registry",
        "Search the California data broker registry: every company that collects and sells "
        "personal data must register annually and disclose what categories it collects and how "
        "to opt out. This is the authoritative public list of who holds consumer data. Useful "
        "from any state, since the brokers operate nationally. Search by company name, or by a "
        "data category such as 'geolocation' or 'reproductive' to find who claims to collect it. "
        "Empty query returns the head of the full list.",
        {{"query", ParamType::String, "Company name or data category, or empty for the full list"},
         {"max_results", ParamType::Integer, "How many brokers to return, 1 to 50"}},
        ToolClass::Job, {},
        [c](const nlohmann::json& a, JobHandle& job) -> std::string {
            if (!c->enable_osint_tools) return std::string("error: OSINT tools are disabled in Settings");
            const int n = std::clamp(a.value("max_results", 20), 1, 50);
            job.report(15, "downloading California data broker registry");
            auto res = run_helper(*c, {L"broker-registry",
                L"--query", utf8_to_wide(a.value("query", std::string(""))),
                L"--max-results", std::to_wstring(n)}, 120, &job);
            job.report(100, "complete");
            return require_success(res, "broker registry search");
        },
        [](const nlohmann::json& a) -> std::string {
            return validate_range(a, "max_results", 1, 50);
        }
    });

    r.add({
        "parcel_source_discover",
        "Find and verify a public parcel service for a US state, then register it so "
        "parcel_lookup can use it. Searches the public ArcGIS Online item index, walks each "
        "candidate service's layers, and keeps only polygon layers that expose owner and "
        "parcel-id fields AND answer a live anonymous query with a plausible feature count. "
        "Run with commit=false first to inspect what it found; commit=true registers the "
        "strongest candidate. Use this whenever parcel_lookup reports a state is unregistered.",
        {{"state", ParamType::String, "Two-letter continental US state code, e.g. MT"},
         {"commit", ParamType::Boolean, "true to register the best verified layer, false to only report"}},
        ToolClass::Job, {},
        [c](const nlohmann::json& a, JobHandle& job) -> std::string {
            if (!c->enable_osint_tools) return std::string("error: OSINT tools are disabled in Settings");
            std::vector<std::wstring> args{L"parcel-discover", L"--state",
                utf8_to_wide(a.at("state").get<std::string>())};
            if (a.value("commit", false)) args.push_back(L"--commit");
            job.report(10, "searching ArcGIS Online for candidate parcel services");
            auto res = run_helper(*c, args, 300, &job);
            job.report(100, "complete");
            return require_success(res, "parcel source discovery");
        },
        [](const nlohmann::json& a) -> std::string {
            const std::string s = a.value("state", std::string());
            if (s.size() != 2)
                return std::string("error: state must be a two-letter code, e.g. MT");
            return {};
        }
    });

    r.add({
        "parcel_sources",
        "List the parcel services currently registered in the local reference database, which "
        "states are covered, and which are still missing. Check this before assuming "
        "parcel_lookup can answer for a given state.",
        {},
        ToolClass::Sync,
        [c](const nlohmann::json&) -> std::string {
            if (!c->enable_osint_tools) return std::string("error: OSINT tools are disabled in Settings");
            auto res = run_helper(*c, {L"parcel-sources"}, 30);
            return require_success(res, "parcel source list");
        },
        {}
    });

    r.add({
        "entity_search",
        "Look up a business entity in a state business registry: legal name, status, entity "
        "type, filing number, and registered agent with address. Use it when a property's "
        "owner of record is an LLC, trust, or partnership rather than a person - farm and "
        "rental property is usually held this way. Search by entity name, or by registered "
        "agent name to find every entity that agent represents. Defaults to Arkansas.",
        {{"name", ParamType::String, "Entity name to search for, or empty if searching by agent"},
         {"agent", ParamType::String, "Registered agent name, or empty if searching by entity name"},
         {"state", ParamType::String, "Two-letter state code; empty defaults to AR"},
         {"max_results", ParamType::Integer, "How many entities to return, 1 to 30"}},
        ToolClass::Sync,
        [c](const nlohmann::json& a) -> std::string {
            if (!c->enable_osint_tools) return std::string("error: OSINT tools are disabled in Settings");
            const int n = std::clamp(a.value("max_results", 15), 1, 30);
            std::string state = a.value("state", std::string(""));
            if (state.empty()) state = "AR";
            auto res = run_helper(*c, {L"entity-search",
                L"--name", utf8_to_wide(a.value("name", std::string(""))),
                L"--agent", utf8_to_wide(a.value("agent", std::string(""))),
                L"--state", utf8_to_wide(state),
                L"--max-results", std::to_wstring(n)}, 60);
            return require_success(res, "entity search");
        },
        {},
        [](const nlohmann::json& a) -> std::string {
            if (auto e = validate_range(a, "max_results", 1, 30); !e.empty()) return e;
            const bool has_name = !a.value("name", std::string()).empty();
            const bool has_agent = !a.value("agent", std::string()).empty();
            if (!has_name && !has_agent)
                return std::string("error: supply either name or agent; both were empty");
            return {};
        }
    });
}

} // namespace lar
