#include "task.hpp"
#include <chrono>
namespace msa {
SearchTask::~SearchTask() { stop(); }
void SearchTask::stop() {
    cancel_ = true;
    if (worker_.joinable())
        worker_.join();
}
void SearchTask::cancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_.status == TaskStatus::Running) {
        state_.status = TaskStatus::Cancelling;
        cancel_ = true;
    }
}
void SearchTask::start(const Json &config) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_.status == TaskStatus::Running || state_.status == TaskStatus::Cancelling)
            throw std::logic_error("已有生成任务运行中");
    }
    if (worker_.joinable())
        worker_.join();
    const Json snapshot = validate_request(config);
    std::uint64_t id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        id = state_.id + 1;
        state_ = TaskView{};
        state_.id = id;
        state_.status = TaskStatus::Running;
        started_ = std::chrono::steady_clock::now();
    }
    cancel_ = false;
    try {
        worker_ = std::thread([this, snapshot, id] {
            auto finish = [&](TaskStatus s, std::string e, std::optional<Result> r = {}) {
                std::lock_guard<std::mutex> lock(mutex_);
                if (id != state_.id)
                    return;
                state_.status = s;
                state_.error = std::move(e);
                state_.result = std::move(r);
                state_.elapsed =
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - started_)
                        .count();
                if (state_.result) {
                    state_.attempts = state_.result->attempts;
                    state_.found = true;
                }
            };
            try {
                auto result = generate(snapshot, &cancel_, [&](int n, double elapsed, bool found) {
                    // Every 100 candidates, plus the final count. UI wakeups are
                    // separately limited to 20 Hz by the application bridge.
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (id == state_.id) {
                        state_.attempts = n;
                        state_.elapsed = elapsed;
                        state_.found = found;
                    }
                });
                if (cancel_)
                    finish(TaskStatus::Cancelled, "已取消，未导出数据。");
                else
                    finish(TaskStatus::Success, "", std::move(result));
            } catch (const CancelledError &e) {
                finish(TaskStatus::Cancelled, e.what());
            } catch (const ConfigError &e) {
                finish(TaskStatus::ConfigError, e.what());
            } catch (const InfeasibleError &e) {
                finish(TaskStatus::Infeasible, e.what());
            } catch (const SearchError &e) {
                finish(TaskStatus::SearchFailed, e.what());
            } catch (const std::exception &e) {
                finish(TaskStatus::Failed, e.what());
            }
        });
    } catch (const std::exception &e) {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.status = TaskStatus::Failed;
        state_.error = e.what();
        throw;
    }
}
TaskView SearchTask::view() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto v = state_;
    if (v.status == TaskStatus::Running || v.status == TaskStatus::Cancelling)
        v.elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
    return v;
}
} // namespace msa
