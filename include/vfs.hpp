#pragma once

#include "algorithms.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace vfs {

constexpr std::size_t BlockSize = 512;
constexpr std::size_t DefaultBlockCount = 512;

class SimulatedCrash : public std::runtime_error {
public:
    SimulatedCrash() : std::runtime_error("simulated crash after journal commit") {}
};

struct EntryInfo {
    std::string path;
    bool directory = false;
    std::string owner;
    std::uint8_t permissions = 0;
    std::uint64_t size = 0;
    std::size_t allocatedBlocks = 0;
    std::string group;
    std::uint8_t groupPermissions = 0;
    std::uint8_t otherPermissions = 0;
    std::uint64_t createdMs = 0;
    std::uint64_t modifiedMs = 0;
    std::uint32_t checksum = 0;
};

struct JournalReport { bool pending = false; bool valid = false; std::string detail; };

struct DiskReport {
    std::size_t totalBlocks = 0;
    std::size_t usedBlocks = 0;
    std::size_t freeBlocks = 0;
    std::size_t freeExtents = 0;
    std::size_t largestFreeExtent = 0;
};

class VirtualFileSystem {
public:
    explicit VirtualFileSystem(std::filesystem::path diskPath,
                               std::size_t blockCount = DefaultBlockCount,
                               std::size_t cacheCapacity = 32);
    void login(const std::string& user);
    std::string currentUser() const;
    void addUser(const std::string& name);
    void addGroup(const std::string& name);
    void addUserToGroup(const std::string& user, const std::string& group);
    std::vector<std::string> groups() const;
    void makeDirectory(const std::string& path);
    void createFile(const std::string& path);
    void writeFile(const std::string& path, const std::string& data);
    void appendFile(const std::string& path, const std::string& data);
    std::string readFile(const std::string& path);
    void checkAccess(const std::string& path, std::uint8_t permission) const;
    std::vector<EntryInfo> list(const std::string& path = "/") const;
    EntryInfo stat(const std::string& path) const;
    void renamePath(const std::string& from, const std::string& to);
    void removePath(const std::string& path);
    void chgrp(const std::string& path, const std::string& group);
    void chmod(const std::string& path, std::uint8_t permissions);
    void chmod(const std::string& path, std::uint8_t owner, std::uint8_t group, std::uint8_t other);
    std::vector<std::string> checkIntegrity() const;
    JournalReport inspectJournal() const;
    DiskReport diskReport() const;
    std::vector<std::string> operationLog() const;
    void setCachePolicy(CachePolicy policy);
    CachePolicy cachePolicy() const;
    std::size_t cacheHits() const;
    std::size_t cacheMisses() const;
    std::size_t blockCount() const;
    void simulateCrashWrite(const std::string& path, const std::string& data);

private:
    struct Entry {
        bool directory = false;
        std::string owner;
        std::uint8_t permissions = 0;
        std::uint64_t size = 0;
        std::vector<std::uint32_t> blocks;
        std::string group = "users";
        std::uint8_t groupPermissions = 0;
        std::uint8_t otherPermissions = 0;
        std::uint64_t createdMs = 0;
        std::uint64_t modifiedMs = 0;
    };
    struct State {
        std::vector<std::array<char, BlockSize>> blocks;
        std::vector<bool> allocated;
        std::map<std::string, Entry> entries;
        std::set<std::string> users;
        std::map<std::string, std::set<std::string>> groups;
    };

    static std::string normalizePath(const std::string& path, const std::string& cwd);
    static std::string parentPath(const std::string& path);
    static std::string leafName(const std::string& path);
    static std::uint32_t checksum(const std::string& data);
    bool userInGroup(const std::string& user, const std::string& group) const;
    static std::uint64_t nowMilliseconds();
    void recoverOrInitialize(std::size_t blockCount);
    void commit(State candidate, const std::string& operation, bool simulateCrash = false);
    void writeAtomic(const std::filesystem::path& path, const std::string& data) const;
    std::string serializeImage(const State& state) const;
    State parseImage(const std::string& image) const;
    void requirePermission(const Entry& entry, std::uint8_t permission,
                           const std::string& path) const;
    void requireDirectory(const std::string& path) const;
    std::vector<std::uint32_t> allocate(State& state, std::size_t byteCount) const;
    static void release(State& state, const Entry& entry);
    std::string readUnlocked(const std::string& path) const;
    void writeUnlocked(State& candidate, const std::string& path,
                       const std::string& data, bool append) const;
    void record(const std::string& operation);
    std::string resolved(const std::string& path) const;

    std::filesystem::path diskPath_;
    std::filesystem::path journalPath_;
    mutable std::mutex mutex_;
    State state_;
    std::string currentUser_ = "admin";
    mutable BlockCache cache_;
    std::vector<std::string> log_;
};

} // namespace vfs
