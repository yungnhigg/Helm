// Phase-5 proof: a long-running job that reports progress while the chat stays
// responsive and can be cancelled mid-run. Stand-in for the eventual disc rip,
// which registers here with the same shape and no changes above this layer.
#include "agent/registry.h"
#include "agent/jobs.h"
#include <thread>
#include <chrono>
#include <utility>

namespace lar {

void register_tool_demo_job(Registry& r) {
    Tool t;
    t.name = "demo_job";
    t.description = "Starts a demonstration background job that takes the given number of seconds. "
                    "seconds must be an integer from 1 through 120; out-of-range values are rejected. "
                    "Use it when asked to test or demonstrate the job system.";
    t.params = { { "seconds", ParamType::Integer, "How long the job should run, 1-120." } };
    t.cls = ToolClass::Job;
    t.validate = [](const nlohmann::json& args) -> std::string {
        if (!args.contains("seconds") || !args["seconds"].is_number_integer())
            return "error: seconds must be an integer between 1 and 120 inclusive";
        const long long requested = args["seconds"].get<long long>();
        if (requested < 1 || requested > 120)
            return "error: seconds must be between 1 and 120 inclusive; received " + std::to_string(requested);
        return {};
    };
    t.run_job = [](const nlohmann::json& args, JobHandle& job) -> std::string {
        const int total = static_cast<int>(args.at("seconds").get<long long>());
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
