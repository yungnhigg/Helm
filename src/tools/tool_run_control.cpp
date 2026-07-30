#include "agent/registry.h"

namespace lar {

// task_complete has to be a registered tool, not a convention.
//
// The grammar emits one literal alternative per registered tool with the name
// baked in, so the sampler can only ever produce a name that appears in the
// registry. A "call task_complete when finished" instruction in the prompt is
// unreachable: the token sequence is forbidden. An autonomous run whose only
// exit is unemittable burns its entire iteration budget every time.
//
// AgentLoop intercepts the call before dispatch, so the handler here never runs
// in an autonomous run. It stays meaningful for an interactive turn, where
// calling it is just an odd way to answer.
void register_run_control(Registry& registry) {
    registry.add({
        "task_complete",
        "Call this ONLY when the assigned task is genuinely finished, to end an autonomous run. "
        "Put the full result in summary: what you did, what you found, and anything you could not "
        "complete. During an autonomous run this is the only way to stop early - a normal answer is "
        "treated as progress and the run continues.",
        {{"summary", ParamType::String, "Complete result of the task, in full"}},
        ToolClass::Synchronous, {},
        [](const nlohmann::json& args, JobHandle&) {
            return args.value("summary", std::string("Task reported complete."));
        }
    });
}

} // namespace lar
