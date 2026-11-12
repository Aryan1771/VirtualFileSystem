#include "algorithms.hpp"
#include "vfs.hpp"
#include "worker_pool.hpp"

#include <filesystem>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int assertions = 0;

void check(bool condition, const std::string& message) {
    ++assertions;
    if (!condition) throw std::runtime_error("assertion failed: " + message);
}
template <typename Function>
void expectFailure(Function&& function, const std::string& message) {
    ++assertions;
    try {
        function();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error("expected failure: " + message);
}

std::filesystem::path testDisk(const std::string& name) {
    auto path = std::filesystem::temp_directory_path() / ("virtualfs_" + name + ".dat");
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(path.string() + ".journal", ignored);
    std::filesystem::remove(path.string() + ".tmp", ignored);
    std::filesystem::remove(path.string() + ".journal.tmp", ignored);
    return path;
}

void cleanDisk(const std::filesystem::path& path) {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(path.string() + ".journal", ignored);
    std::filesystem::remove(path.string() + ".tmp", ignored);
    std::filesystem::remove(path.string() + ".journal.tmp", ignored);
}

void testPersistenceAndPaths() {
    const auto path = testDisk("persist");
    const std::string contents(900, 'x');
    {
        vfs::VirtualFileSystem filesystem(path, 16, 2);
        filesystem.makeDirectory("/docs");
        filesystem.makeDirectory("/docs/nested");
        filesystem.createFile("/docs/nested/note.txt");
        filesystem.writeFile("/docs/nested/note.txt", contents);
        check(filesystem.readFile("/docs/nested/note.txt") == contents,
              "multi-block contents round-trip");
        check(filesystem.diskReport().usedBlocks == 2, "two blocks allocated");
        filesystem.renamePath("/docs/nested", "/docs/archive");
        check(filesystem.stat("/docs/archive/note.txt").size == contents.size(),
              "directory rename preserves children");
        expectFailure([&] { filesystem.renamePath("/docs", "/docs/archive/self"); },
                      "directory cannot move into itself");
        expectFailure([&] { filesystem.readFile("/../../outside"); },
                      "path cannot escape root");
    }
    {
        vfs::VirtualFileSystem filesystem(path, 16, 2);
        check(filesystem.readFile("/docs/archive/note.txt") == contents,
              "data survives remount");
        filesystem.removePath("/docs/archive");
        check(filesystem.diskReport().usedBlocks == 0, "recursive delete releases blocks");
        check(filesystem.list("/docs").empty(), "deleted directory is absent");
    }
    cleanDisk(path);
}

void testPermissions() {
    const auto path = testDisk("permissions");
    vfs::VirtualFileSystem filesystem(path, 8);
    filesystem.addUser("student");
    filesystem.makeDirectory("/private");
    filesystem.createFile("/private/report.txt");
    filesystem.writeFile("/private/report.txt", "readable");
    filesystem.chmod("/private/report.txt", 4);
    filesystem.login("student");
    check(filesystem.readFile("/private/report.txt") == "readable",
          "granted read is allowed");
    expectFailure([&] { filesystem.writeFile("/private/report.txt", "denied"); },
                  "missing write is denied");
    filesystem.login("admin");
    filesystem.chmod("/private/report.txt", 1);
    filesystem.login("student");
    filesystem.checkAccess("/private/report.txt", 1);
    expectFailure([&] { filesystem.readFile("/private/report.txt"); },
                  "execute permission does not grant read access");
    expectFailure([&] { filesystem.addUser("intruder"); },
                  "non-admin cannot add users");
    filesystem.login("admin");
    filesystem.chmod("/private", 0);
    filesystem.login("student");
    expectFailure([&] { filesystem.readFile("/private/report.txt"); },
                  "missing directory traversal permission is denied");
    cleanDisk(path);
}

void testRecovery() {
    const auto path = testDisk("recovery");
    {
        vfs::VirtualFileSystem filesystem(path, 8);
        filesystem.createFile("/pending.txt");
        try {
            filesystem.simulateCrashWrite("/pending.txt", "recovered content");
            check(false, "simulated crash must throw");
        } catch (const vfs::SimulatedCrash&) {
            check(std::filesystem::exists(path.string() + ".journal"),
                  "journal remains after simulated crash");
        }
    }
    {
        vfs::VirtualFileSystem filesystem(path, 8);
        check(filesystem.readFile("/pending.txt") == "recovered content",
              "journal replays pending write on startup");
        check(!std::filesystem::exists(path.string() + ".journal"),
              "recovery clears journal");
    }
    cleanDisk(path);
}

void testCacheAndDiskFull() {
    const auto path = testDisk("cache");
    vfs::VirtualFileSystem filesystem(path, 1, 1);
    filesystem.createFile("/one");
    filesystem.writeFile("/one", "first");
    filesystem.readFile("/one");
    filesystem.readFile("/one");
    check(filesystem.cacheMisses() == 1 && filesystem.cacheHits() == 1,
          "cache hit/miss counters");
    filesystem.setCachePolicy(vfs::CachePolicy::FIFO);
    check(filesystem.cacheMisses() == 0 && filesystem.cacheHits() == 0,
          "changing policy resets cache counters");
    filesystem.writeFile("/one", std::string(512, 'z'));
    expectFailure([&] { filesystem.appendFile("/one", "x"); },
                  "disk-full append fails");
    check(filesystem.readFile("/one") == std::string(512, 'z'),
          "failed write leaves prior file contents intact");
    cleanDisk(path);
}

void testSchedulers() {
    const std::vector<std::size_t> requests{40, 10, 20};
    const auto fcfs = vfs::scheduleRequests(requests, 50, 100, "FCFS");
    check(fcfs.order == requests && fcfs.headMovement == 50, "FCFS order and movement");
    const auto sstf = vfs::scheduleRequests(requests, 50, 100, "SSTF");
    check(sstf.order == std::vector<std::size_t>({40, 20, 10}),
          "SSTF selects closest request");
    const auto scan = vfs::scheduleRequests({10, 40, 80}, 50, 100, "SCAN");
    check(scan.order == std::vector<std::size_t>({80, 40, 10}) && scan.headMovement == 138,
          "SCAN services upward then downward");
    const auto cscan = vfs::scheduleRequests({10, 40, 80}, 50, 100, "C-SCAN");
    check(cscan.order == std::vector<std::size_t>({80, 10, 40}) && cscan.headMovement == 188,
          "C-SCAN wraps to the start");
    const auto reverseScan = vfs::scheduleRequests({10, 40, 80}, 50, 100, "SCAN", false);
    check(reverseScan.order == std::vector<std::size_t>({40, 10, 80}) &&
          reverseScan.headMovement == 130,
          "reverse SCAN services upward after reaching the lower edge");
    expectFailure([&] { vfs::scheduleRequests({100}, 50, 100, "FCFS"); },
                  "out-of-range request rejected");
}

void testCachePolicies() {
    vfs::BlockCache fifo(2);
    fifo.setPolicy(vfs::CachePolicy::FIFO);
    check(!fifo.access(1) && !fifo.access(2), "FIFO initial misses");
    check(fifo.access(1), "FIFO hit does not change insertion order");
    check(!fifo.access(3) && !fifo.access(1), "FIFO evicts the oldest inserted block");
    vfs::BlockCache lru(2);
    check(!lru.access(1) && !lru.access(2), "LRU initial misses");
    check(lru.access(1), "LRU access refreshes recency");
    check(!lru.access(3) && lru.access(1), "LRU evicts the least recently used block");
}

void testWorkerPool() {
    const auto path = testDisk("concurrent");
    vfs::VirtualFileSystem filesystem(path, 8);
    filesystem.createFile("/shared.txt");
    filesystem.writeFile("/shared.txt", "thread-safe read");
    vfs::WorkerPool pool(3);
    std::vector<std::future<int>> results;
    for (int value = 0; value < 20; ++value) {
        results.push_back(pool.submit([value] { return value * value; }));
    }
    for (int value = 0; value < 20; ++value) {
        check(results[static_cast<std::size_t>(value)].get() == value * value,
              "worker returns result");
    }
    std::vector<std::future<std::string>> reads;
    for (int value = 0; value < 8; ++value) {
        reads.push_back(pool.submit([&filesystem] {
            return filesystem.readFile("/shared.txt");
        }));
    }
    for (auto& read : reads) {
        check(read.get() == "thread-safe read", "concurrent filesystem read is consistent");
    }
    cleanDisk(path);
}

} // namespace

int main() {
    try {
        testPersistenceAndPaths();
        testPermissions();
        testRecovery();
        testCacheAndDiskFull();
        testSchedulers();
        testCachePolicies();
        testWorkerPool();
        std::cout << "All " << assertions << " assertions passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
