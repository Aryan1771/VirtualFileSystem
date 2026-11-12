#include "algorithms.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <stdexcept>

namespace vfs {

BlockCache::BlockCache(std::size_t capacity) : capacity_(capacity) {}

bool BlockCache::access(std::size_t block) {
    auto found = std::find(order_.begin(), order_.end(), block);
    if (found != order_.end()) {
        ++hits_;
        if (policy_ == CachePolicy::LRU) {
            order_.erase(found);
            order_.push_back(block);
        }
        return true;
    }
    ++misses_;
    if (capacity_ == 0) {
        return false;
    }
    if (order_.size() == capacity_) {
        order_.erase(order_.begin());
    }
    order_.push_back(block);
    return false;
}

void BlockCache::setPolicy(CachePolicy policy) {
    policy_ = policy;
    clear();
}

CachePolicy BlockCache::policy() const { return policy_; }
void BlockCache::clear() { order_.clear(); hits_ = 0; misses_ = 0; }
std::size_t BlockCache::hits() const { return hits_; }
std::size_t BlockCache::misses() const { return misses_; }
std::size_t BlockCache::capacity() const { return capacity_; }

ScheduleResult scheduleRequests(const std::vector<std::size_t>& requests,
                                std::size_t initialHead,
                                std::size_t diskSize,
                                const std::string& algorithm,
                                bool movingUp) {
    if (diskSize == 0 || initialHead >= diskSize) {
        throw std::invalid_argument("disk size and initial head are out of range");
    }
    for (const auto request : requests) {
        if (request >= diskSize) {
            throw std::invalid_argument("request lies outside the disk");
        }
    }
    std::string name = algorithm;
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char value) { return static_cast<char>(std::toupper(value)); });
    ScheduleResult result;
    std::size_t head = initialHead;
    auto visit = [&result, &head](std::size_t request) {
        result.headMovement += request > head ? request - head : head - request;
        head = request;
        result.order.push_back(request);
    };
    if (name == "FCFS") {
        for (const auto request : requests) visit(request);
    } else if (name == "SSTF") {
        std::vector<std::size_t> pending = requests;
        while (!pending.empty()) {
            const auto next = std::min_element(pending.begin(), pending.end(),
                [head](std::size_t left, std::size_t right) {
                    const auto leftDistance = left > head ? left - head : head - left;
                    const auto rightDistance = right > head ? right - head : head - right;
                    return leftDistance == rightDistance ? left < right : leftDistance < rightDistance;
                });
            visit(*next);
            pending.erase(next);
        }
    } else if (name == "SCAN" || name == "C-SCAN") {
        std::vector<std::size_t> ascending = requests;
        std::sort(ascending.begin(), ascending.end());
        if (movingUp) {
            for (auto request : ascending) if (request >= head) visit(request);
            const bool hasLower = std::any_of(ascending.begin(), ascending.end(),
                [head](std::size_t request) { return request < head; });
            if (hasLower) {
                if (name == "SCAN") {
                    if (head != diskSize - 1) {
                        result.headMovement += (diskSize - 1) - head;
                        head = diskSize - 1;
                    }
                } else {
                    if (head != diskSize - 1) result.headMovement += (diskSize - 1) - head;
                    result.headMovement += diskSize - 1;
                    head = 0;
                }
                if (name == "SCAN") {
                    for (auto iterator = ascending.rbegin(); iterator != ascending.rend(); ++iterator)
                        if (*iterator < initialHead) visit(*iterator);
                } else {
                    for (auto request : ascending) if (request < initialHead) visit(request);
                }
            }
        } else {
            for (auto iterator = ascending.rbegin(); iterator != ascending.rend(); ++iterator)
                if (*iterator <= head) visit(*iterator);
            const bool hasHigher = std::any_of(ascending.begin(), ascending.end(),
                [head](std::size_t request) { return request > head; });
            if (hasHigher) {
                if (name == "SCAN") {
                    if (head != 0) { result.headMovement += head; head = 0; }
                } else {
                    result.headMovement += head;
                    result.headMovement += diskSize - 1;
                    head = diskSize - 1;
                }
                if (name == "SCAN") {
                    for (auto request : ascending) if (request > initialHead) visit(request);
                } else {
                    for (auto iterator = ascending.rbegin(); iterator != ascending.rend(); ++iterator)
                        if (*iterator > initialHead) visit(*iterator);
                }
            }
        }
    } else {
        throw std::invalid_argument("unknown disk scheduling algorithm: " + algorithm);
    }
    return result;
}

} // namespace vfs
