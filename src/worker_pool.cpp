#include "worker_pool.hpp"

#include <algorithm>
#include <stdexcept>

namespace vfs {

WorkerPool::WorkerPool(std::size_t threadCount) {
    if (threadCount == 0) {
        throw std::invalid_argument("worker count must be positive");
    }
    workers_.reserve(threadCount);
    for (std::size_t index = 0; index < threadCount; ++index) {
        workers_.emplace_back([this] { run(); });
    }
}

WorkerPool::~WorkerPool() {
    {
        std::lock_guard<std::mutex> guard(mutex_);
        stopping_ = true;
    }
    ready_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

void WorkerPool::run() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            ready_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
            if (stopping_ && tasks_.empty()) {
                return;
            }
            task = std::move(tasks_.front());
            tasks_.pop();
        }
        task();
    }
}

} // namespace vfs
