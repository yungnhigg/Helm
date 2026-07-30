#include "agent/registry.h"

namespace lar {

// task_complete has to be a registered tool, not a prompt convention.
//
// The grammar emits one literal alternative per registered tool with the name
// baked in, so the sampler can only ever produce a name that is in the registry.
// "Call task_complete when finished" as an instruction is unreachable: the token
// sequence is forbidden outright. An autonomous run whose only exit cannot be
// emitted burns its entire iteration budget every single time.
//
// AgentLoop intercepts the call before dispatch, so run_sync below never fires
// during an autonomous run. It stays correct for an interactive turn, where
// calling this is simply a roundabout way of answering.
void register_run_control(Registry& r) {
    Tool t;
    t.name = "task_complete";
    t.description =
        "Call this ONLY when the assigned task is genuinely finished, to end an autonomous run. "
        "Put the whole result in summary: what you did, what you found, and anything you could "
        "not complete. During an autonomous run this is the only way to stop early - an ordinary "
        "answer is treated as progress and the run continues.";
    t.params = {{"summary", ParamType::String, "Complete result of the task, in full"}};
    t.cls = ToolClass::Sync;
    t.run_sync = [](const nlohmann::json& args) -> std::string {
        return args.value("summary", std::string("Task reported complete."));
    };
    r.add(std::move(t));
}

} // namespace lar
