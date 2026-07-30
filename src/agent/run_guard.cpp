#include "agent/run_guard.h"
#include <algorithm>

namespace lar {

std::string canonical_tool_call_signature(const std::string& name,
                                          const nlohmann::json& arguments) {
    return name + " args:" + arguments.dump();
}

bool remember_tool_call(std::vector<std::string>& seen,
                        const std::string& signature,
                        std::size_t max_entries) {
    if (std::find(seen.begin(), seen.end(), signature) != seen.end()) return false;
    if (max_entries == 0) return true;
    if (seen.size() >= max_entries) {
        const std::size_t remove = std::max<std::size_t>(1, seen.size() / 2);
        seen.erase(seen.begin(), seen.begin() + static_cast<std::ptrdiff_t>(remove));
    }
    seen.push_back(signature);
    return true;
}

} // namespace lar
