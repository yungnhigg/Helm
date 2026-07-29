// Throwaway phase-4 tool: current local time. Sync class.
#include "agent/registry.h"
#include <chrono>
#include <format>

namespace lar {

void register_tool_time(Registry& r) {
    Tool t;
    t.name = "get_time";
    t.description = "Returns the current local date and time on this machine.";
    t.cls = ToolClass::Sync;
    t.run_sync = [](const nlohmann::json&) -> std::string {
        auto now = std::chrono::current_zone()->to_local(std::chrono::system_clock::now());
        return std::format("{:%A %F %T}", std::chrono::floor<std::chrono::seconds>(now));
    };
    r.add(std::move(t));
}

} // namespace lar
