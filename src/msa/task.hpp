#pragma once
#include "core.hpp"
#include <chrono>
#include <mutex>
#include <thread>

namespace msa {
enum class TaskStatus {
    Ready,
    Running,
    Cancelling,
    Success,
    Cancelled,
    ConfigError,
    Infeasible,
    SearchFailed,
    Failed
};
struct TaskView {
    std::uint64_t id = 0;
    TaskStatus status = TaskStatus::Ready;
    int attempts = 0;
    double elapsed = 0;
    bool found = false;
    std::string error;
    std::optional<Result> result;
};
// Worker owns its configuration snapshot. No GUI calls or UI objects are used.
class SearchTask {
  public:
    ~SearchTask();
    void start(const Json &config);
    void cancel();
    void stop();
    TaskView view() const;

  private:
    mutable std::mutex mutex_;
    TaskView state_;
    std::thread worker_;
    std::atomic<bool> cancel_{false};
    std::chrono::steady_clock::time_point started_{};
};
} // namespace msa
