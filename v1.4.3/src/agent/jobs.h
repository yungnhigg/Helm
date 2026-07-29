#pragma once
// Job infrastructure. Job-class tools return an id immediately, run on a small
// tool pool, report progress on their own channel, and notify on completion.
// Built in from the start because retrofitting it means rewriting the agent loop.
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <nlohmann/json.hpp>

namespace lar {

struct Tool;

enum class JobStatus { Running, Done, Failed, Cancelled };

class JobHandle {
public:
    // Called by the tool body to report progress. pct 0..100.
    void report(int pct, const std::string& note);
    // Tool bodies must poll this in their loops; cancellation everywhere.
    bool cancelled() const { return cancel_.load(); }

    int id() const { return id_; }
    std::string name() const { return name_; }

private:
    friend class JobManager;
    int id_ = 0;
    std::string name_;
    std::atomic<bool> cancel_{false};
    std::atomic<JobStatus> status_{JobStatus::Running};
    std::function<void(int id, int pct, const std::string& note)> on_progress_;
};

class JobManager {
public:
    // pool_size small on purpose: a rip occupies one slot for ~40 minutes.
    explicit JobManager(int pool_size = 2);
    ~JobManager();

    using ProgressFn = std::function<void(int id, const std::string& name, int pct, const std::string& note)>;
    using DoneFn     = std::function<void(int id, const std::string& name, JobStatus, const std::string& result)>;

    // Fire-and-track. Returns the job id immediately.
    int start(const Tool& tool, const nlohmann::json& args, ProgressFn on_progress, DoneFn on_done);
    int start_task(const std::string& name, std::function<std::string(JobHandle&)> body,
                   ProgressFn on_progress, DoneFn on_done);
    bool cancel(int id);
    size_t active_count() const;

private:
    void pool_main();

    mutable std::mutex m_;
    std::condition_variable cv_;
    bool stop_ = false;
    int next_id_ = 1;
    struct Pending {
        std::shared_ptr<JobHandle> handle;
        std::function<void()> run;
    };
    std::vector<Pending> queue_;
    std::vector<std::shared_ptr<JobHandle>> live_;
    std::vector<std::thread> pool_;
};

} // namespace lar
