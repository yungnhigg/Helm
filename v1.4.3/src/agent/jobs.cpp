#include "agent/jobs.h"
#include "agent/registry.h"
#include "common/util.h"
#include <algorithm>
#include <stdexcept>

namespace lar {

void JobHandle::report(int pct, const std::string& note) {
    pct = std::clamp(pct, 0, 100);
    try { if (on_progress_) on_progress_(id_, pct, note); }
    catch (const std::exception& e) { log(std::string("job progress callback failed: ") + e.what()); }
    catch (...) { log("job progress callback failed: unknown"); }
}

JobManager::JobManager(int pool_size) {
    for (int i = 0; i < std::max(1, pool_size); ++i)
        pool_.emplace_back([this] { pool_main(); });
}

JobManager::~JobManager() {
    {
        std::lock_guard lk(m_);
        stop_ = true;
        for (auto& h : live_) h->cancel_.store(true);
    }
    cv_.notify_all();
    for (auto& t : pool_) if (t.joinable()) t.join();
}

int JobManager::start(const Tool& tool, const nlohmann::json& args, ProgressFn on_progress, DoneFn on_done) {
    const auto body = tool.run_job;
    return start_task(tool.name,
        [body, args](JobHandle& handle) {
            if (!body) throw std::runtime_error("job tool has no implementation");
            return body(args, handle);
        },
        std::move(on_progress), std::move(on_done));
}

int JobManager::start_task(const std::string& name, std::function<std::string(JobHandle&)> body,
                           ProgressFn on_progress, DoneFn on_done) {
    auto handle = std::make_shared<JobHandle>();

    std::lock_guard lk(m_);
    handle->id_ = next_id_++;
    handle->name_ = name;
    handle->on_progress_ = [on_progress = std::move(on_progress), name](int id, int pct, const std::string& note) {
        if (on_progress) on_progress(id, name, pct, note);
    };
    const int id = handle->id_;
    live_.push_back(handle);

    queue_.push_back({handle, [this, handle, body = std::move(body), on_done = std::move(on_done), name] {
        std::string result;
        JobStatus status = JobStatus::Done;
        try {
            if (!body) throw std::runtime_error("job has no implementation");
            result = body(*handle);
            if (handle->cancelled()) status = JobStatus::Cancelled;
        } catch (const std::exception& e) {
            status = handle->cancelled() ? JobStatus::Cancelled : JobStatus::Failed;
            result = e.what();
        } catch (...) {
            status = handle->cancelled() ? JobStatus::Cancelled : JobStatus::Failed;
            result = "unknown job failure";
        }
        handle->status_.store(status);
        try { if (on_done) on_done(handle->id_, name, status, result); }
        catch (const std::exception& e) { log(std::string("job completion callback failed: ") + e.what()); }
        catch (...) { log("job completion callback failed: unknown"); }
        std::lock_guard lk2(m_);
        live_.erase(std::remove(live_.begin(), live_.end(), handle), live_.end());
    }});
    cv_.notify_one();
    return id;
}

size_t JobManager::active_count() const {
    std::lock_guard lk(m_);
    return live_.size();
}

bool JobManager::cancel(int id) {
    std::lock_guard lk(m_);
    for (auto& h : live_) {
        if (h->id_ == id) {
            h->cancel_.store(true);
            return true;
        }
    }
    return false;
}

void JobManager::pool_main() {
    for (;;) {
        Pending pending;
        {
            std::unique_lock lk(m_);
            cv_.wait(lk, [&] { return stop_ || !queue_.empty(); });
            if (stop_ && queue_.empty()) return;
            pending = std::move(queue_.front());
            queue_.erase(queue_.begin());
        }
        try { pending.run(); }
        catch (const std::exception& e) { log(std::string("job worker escaped exception: ") + e.what()); }
        catch (...) { log("job worker escaped exception: unknown"); }
    }
}

} // namespace lar
