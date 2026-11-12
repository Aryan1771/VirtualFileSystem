#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace vfs {

class WorkerPool {
public:
    explicit WorkerPool(std::size_t threadCount);
    ~WorkerPool();
    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    template <typename Function>
    auto submit(Function&& function) -> std::future<typename std::invoke_result_t<Function>> {
        using Result = typename std::invoke_result_t<Function>;
        auto task = std::make_shared<std::packaged_task<Result()>>(
            std::forward<Function>(function));
        auto result = task->get_future();
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (stopping_) {
                throw std::runtime_error("worker pool is stopping");
            }
            tasks_.emplace([task] { (*task)(); });
        }
        ready_.notify_one();
        return result;
    }

private:
    void run();
    std::mutex mutex_;
    std::condition_variable ready_;
    std::queue<std::function<void()>> tasks_;
    std::vector<std::thread> workers_;
    bool stopping_ = false;
};

} // namespace vfs
