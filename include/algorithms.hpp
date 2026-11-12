#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace vfs {

enum class CachePolicy { FIFO, LRU };

class BlockCache {
public:
    explicit BlockCache(std::size_t capacity = 32);
    bool access(std::size_t block);
    void setPolicy(CachePolicy policy);
    CachePolicy policy() const;
    void clear();
    std::size_t hits() const;
    std::size_t misses() const;
    std::size_t capacity() const;
private:
    std::size_t capacity_;
    CachePolicy policy_ = CachePolicy::LRU;
    std::size_t hits_ = 0;
    std::size_t misses_ = 0;
    std::vector<std::size_t> order_;
};

struct ScheduleResult {
    std::vector<std::size_t> order;
    std::size_t headMovement = 0;
};

ScheduleResult scheduleRequests(const std::vector<std::size_t>& requests,
                                std::size_t initialHead,
                                std::size_t diskSize,
                                const std::string& algorithm,
                                bool movingUp = true);

} // namespace vfs
