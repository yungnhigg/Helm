// Throwaway phase-4 tool: dice roll with a parameter, proving argument
// plumbing and grammar-constrained integers. Sync class.
#include "agent/registry.h"
#include <random>

namespace lar {

void register_tool_dice(Registry& r) {
    Tool t;
    t.name = "roll_dice";
    t.description = "Rolls a die and returns the result.";
    t.params = { { "sides", ParamType::Integer, "Number of sides on the die, e.g. 6 or 20." } };
    t.cls = ToolClass::Sync;
    t.run_sync = [](const nlohmann::json& args) -> std::string {
        int sides = args.value("sides", 6);
        if (sides < 2) return "error: a die needs at least 2 sides";
        static std::mt19937 rng{ std::random_device{}() };
        std::uniform_int_distribution<int> d(1, sides);
        return "rolled " + std::to_string(d(rng)) + " on a d" + std::to_string(sides);
    };
    r.add(std::move(t));
}

} // namespace lar
