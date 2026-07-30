#pragma once
#include <nlohmann/json.hpp>
#include <cstddef>
#include <string>
#include <vector>

namespace lar {

// Stable identity for exact-repeat detection. The complete argument object is
// included so a genuinely different fetch, write, or process call remains valid.
std::string canonical_tool_call_signature(const std::string& name,
                                          const nlohmann::json& arguments);

// Returns true and records a new call, or false when the signature was already
// seen. Storage is bounded without clearing the entire history at once.
bool remember_tool_call(std::vector<std::string>& seen,
                        const std::string& signature,
                        std::size_t max_entries = 5000);

} // namespace lar
