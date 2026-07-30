#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace lar {

class Registry;
class JobHandle;
struct Config;

struct ProcessCaptureResult {
    int exit_code = -1;
    bool timed_out = false;
    bool cancelled = false;
    std::string output;
};

ProcessCaptureResult run_process_capture(const std::wstring& executable,
                                         const std::vector<std::wstring>& arguments,
                                         const std::wstring& working_directory,
                                         int timeout_seconds,
                                         JobHandle* job = nullptr,
                                         const std::string& stdin_text = {});

std::string transcribe_audio_file(const Config& cfg, const std::string& input_path, JobHandle* job = nullptr);
std::string extract_document_text(const Config& cfg, const std::string& path, size_t max_chars = 200000);
void register_external_tools(Registry& registry, const Config& cfg);

} // namespace lar
