// Phase-5 proof: a long-running job that reports progress while the chat stays
// responsive and can be cancelled mid-run. Stand-in for the eventual disc rip,
// which registers here with the same shape and no changes above this layer.
#include "agent/registry.h"
#include "agent/jobs.h"
#include <thread>
#include <chrono>

namespace lar {

void register_tool_demo_job(Registry& r) {
    Tool t;
    t.name = "demo_job";
    t.description = "Starts a demonstration background job that takes the given number of seconds. "
                    "Use it when asked to test or demonstrate the job system.";
    t.params = { { "seconds", ParamType::Integer, "How long the job should run, 1-120." } };
    t.cls = ToolClass::Job;
    t.run_job = [](const nlohmann::json& args, JobHandle& job) -> std::string {
        int total = args.value("seconds", 10);
        if (total < 1) total = 1;
        if (total > 120) total = 120;
        for (int i = 0; i < total; ++i) {
            if (job.cancelled()) return "cancelled at " + std::to_string(i) + "s";
            job.report(i * 100 / total, std::to_string(i) + "s / " + std::to_string(total) + "s");
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        job.report(100, "complete");
        return "ran for " + std::to_string(total) + " seconds";
    };
    r.add(std::move(t));
}

} // namespace lar
