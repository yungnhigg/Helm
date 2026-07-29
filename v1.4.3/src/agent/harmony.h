#pragma once
#include <nlohmann/json.hpp>
#include <string>

namespace lar {

// Parse a GPT-OSS Harmony response into Helm's internal reply/tool envelope.
// Accepts native Harmony output and a JSON-envelope fallback used by some GGUF
// conversions.
bool parse_harmony_response(const std::string& raw, nlohmann::json& output);

} // namespace lar
