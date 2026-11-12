#include "vfs.hpp"
#include "worker_pool.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::vector<std::string> tokenize(const std::string& line) {
    std::vector<std::string> tokens;
    std::string token;
    bool quoted = false;
    bool escaped = false;
    bool started = false;
    for (const char character : line) {
        if (escaped) {
            token.push_back(character);
            escaped = false;
            started = true;
        } else if (quoted && character == '\\') {
            escaped = true;
        } else if (character == '"') {
            quoted = !quoted;
            started = true;
        } else if (!quoted && std::isspace(static_cast<unsigned char>(character))) {
            if (started) {
                tokens.push_back(std::move(token));
                token.clear();
                started = false;
            }
        } else {
            token.push_back(character);
            started = true;
        }
    }
    if (escaped || quoted) throw std::invalid_argument("unterminated quoted argument");
    if (started) tokens.push_back(std::move(token));
    return tokens;
}
std::size_t parseSize(const std::string& text, const std::string& label) {
    std::size_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc() || result.ptr != text.data() + text.size()) {
        throw std::invalid_argument("invalid " + label + ": " + text);
    }
    return value;
}

std::vector<std::size_t> parseRequests(const std::string& text) {
    std::vector<std::size_t> result;
    std::stringstream stream(text);
    std::string item;
    while (std::getline(stream, item, ',')) {
        if (item.empty()) throw std::invalid_argument("request list contains an empty value");
        result.push_back(parseSize(item, "disk request"));
    }
    if (result.empty()) throw std::invalid_argument("request list cannot be empty");
    return result;
}

std::uint8_t parsePermissions(const std::string& text) {
    if (text.size() != 3) throw std::invalid_argument("permissions must be three characters, such as rw-");
    std::uint8_t value = 0;
    const char expected[] = {'r', 'w', 'x'};
    const std::uint8_t bits[] = {4, 2, 1};
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == expected[index]) value |= bits[index];
        else if (text[index] != '-') throw std::invalid_argument("permissions use r, w, x, or - in that order");
    }
    return value;
}

std::string permissionText(std::uint8_t permissions) {
    std::string value = "---";
    if ((permissions & 4U) != 0U) value[0] = 'r';
    if ((permissions & 2U) != 0U) value[1] = 'w';
    if ((permissions & 1U) != 0U) value[2] = 'x';
    return value;
}

void requireCount(const std::vector<std::string>& tokens,
                  std::size_t count, const std::string& usage) {
    if (tokens.size() != count) throw std::invalid_argument("usage: " + usage);
}

void showHelp() {
    std::cout
        << "Commands:\n"
        << "  help                                      Show commands\n"
        << "  useradd NAME                              Add a user (admin only)\n"
        << "  su USER                                   Switch active user\n"
        << "  whoami                                    Show active user\n"
        << "  mkdir PATH                                Create a directory\n"
        << "  touch PATH                                Create an empty file\n"
        << "  write PATH \"TEXT\"                        Replace file contents\n"
        << "  append PATH \"TEXT\"                       Append to a file\n"
        << "  cat PATH                                  Read a file\n"
        << "  ls [PATH]                                 List a directory\n"
        << "  stat PATH                                 Show file or directory metadata\n"
        << "  mv SOURCE DESTINATION                     Rename or move an entry\n"
        << "  rm PATH                                   Remove a file or directory tree\n"
        << "  chmod PATH rwx                            Set permission bits (use - to clear)\n"
        << "  access PATH r|w|x                         Check a permission bit\n"
        << "  df                                        Show block use and fragmentation\n"
        << "  cache-policy FIFO|LRU                     Select cache replacement policy\n"
        << "  cache-stats                               Show cache hits and misses\n"
        << "  schedule ALG HEAD R1,R2,... [up|down]     Simulate disk scheduling\n"
        << "  concurrent-read PATH [PATH ...]           Read files on worker threads\n"
        << "  log                                       Show this session's operation log\n"
        << "  license                                   Display the bundled GPL-3.0 license\n"
        << "  crash-write PATH \"TEXT\"                   Leave a journal for next-run recovery\n"
        << "  exit                                      Quit\n";
}

void showLicense() {
    std::ifstream input("LICENSE");
    if (!input) {
        std::cout << "This program is distributed under GNU GPL version 3. See LICENSE.\n";
        return;
    }
    std::cout << input.rdbuf();
}

void printEntry(const vfs::EntryInfo& entry) {
    const char type = entry.directory ? 'd' : '-';
    std::cout << type << permissionText(entry.permissions) << "  "
              << entry.owner << "  " << entry.size << " B  "
              << entry.allocatedBlocks << " blocks  " << entry.path << '\n';
}

void execute(vfs::VirtualFileSystem& filesystem,
             const std::vector<std::string>& tokens) {
    if (tokens.empty()) return;
    const auto& command = tokens[0];
    if (command == "help") {
        requireCount(tokens, 1, "help");
        showHelp();
    } else if (command == "useradd") {
        requireCount(tokens, 2, "useradd NAME");
        filesystem.addUser(tokens[1]);
        std::cout << "created user " << tokens[1] << '\n';
    } else if (command == "su" || command == "login") {
        requireCount(tokens, 2, "su USER");
        filesystem.login(tokens[1]);
        std::cout << "active user: " << filesystem.currentUser() << '\n';
    } else if (command == "whoami") {
        requireCount(tokens, 1, "whoami");
        std::cout << filesystem.currentUser() << '\n';
    } else if (command == "mkdir") {
        requireCount(tokens, 2, "mkdir PATH");
        filesystem.makeDirectory(tokens[1]);
        std::cout << "directory created\n";
    } else if (command == "touch") {
        requireCount(tokens, 2, "touch PATH");
        filesystem.createFile(tokens[1]);
        std::cout << "file created\n";
    } else if (command == "write" || command == "append") {
        requireCount(tokens, 3, command + " PATH \"TEXT\"");
        if (command == "write") filesystem.writeFile(tokens[1], tokens[2]);
        else filesystem.appendFile(tokens[1], tokens[2]);
        std::cout << "saved " << tokens[2].size() << " bytes\n";
    } else if (command == "cat") {
        requireCount(tokens, 2, "cat PATH");
        std::cout << filesystem.readFile(tokens[1]) << '\n';
    } else if (command == "ls") {
        if (tokens.size() > 2) throw std::invalid_argument("usage: ls [PATH]");
        const auto entries = filesystem.list(tokens.size() == 2 ? tokens[1] : "/");
        for (const auto& entry : entries) printEntry(entry);
        if (entries.empty()) std::cout << "(empty)\n";
    } else if (command == "stat") {
        requireCount(tokens, 2, "stat PATH");
        printEntry(filesystem.stat(tokens[1]));
    } else if (command == "mv") {
        requireCount(tokens, 3, "mv SOURCE DESTINATION");
        filesystem.renamePath(tokens[1], tokens[2]);
        std::cout << "entry moved\n";
    } else if (command == "rm") {
        requireCount(tokens, 2, "rm PATH");
        filesystem.removePath(tokens[1]);
        std::cout << "entry removed\n";
    } else if (command == "chmod") {
        requireCount(tokens, 3, "chmod PATH rwx");
        filesystem.chmod(tokens[1], parsePermissions(tokens[2]));
        std::cout << "permissions updated\n";
    } else if (command == "access") {
        requireCount(tokens, 3, "access PATH r|w|x");
        if (tokens[2] == "r") filesystem.checkAccess(tokens[1], 4);
        else if (tokens[2] == "w") filesystem.checkAccess(tokens[1], 2);
        else if (tokens[2] == "x") filesystem.checkAccess(tokens[1], 1);
        else throw std::invalid_argument("access bit must be r, w, or x");
        std::cout << "access allowed\n";
    } else if (command == "df") {
        requireCount(tokens, 1, "df");
        const auto report = filesystem.diskReport();
        std::cout << "blocks: " << report.totalBlocks
                  << ", used: " << report.usedBlocks
                  << ", free: " << report.freeBlocks
                  << ", free extents: " << report.freeExtents
                  << ", largest free extent: " << report.largestFreeExtent << '\n';
    } else if (command == "cache-policy") {
        requireCount(tokens, 2, "cache-policy FIFO|LRU");
        if (tokens[1] == "FIFO") filesystem.setCachePolicy(vfs::CachePolicy::FIFO);
        else if (tokens[1] == "LRU") filesystem.setCachePolicy(vfs::CachePolicy::LRU);
        else throw std::invalid_argument("cache policy must be FIFO or LRU");
        std::cout << "cache policy set to " << tokens[1] << '\n';
    } else if (command == "cache-stats") {
        requireCount(tokens, 1, "cache-stats");
        std::cout << "policy: " << (filesystem.cachePolicy() == vfs::CachePolicy::FIFO ? "FIFO" : "LRU")
                  << ", hits: " << filesystem.cacheHits()
                  << ", misses: " << filesystem.cacheMisses() << '\n';
    } else if (command == "schedule") {
        if (tokens.size() < 4 || tokens.size() > 5) {
            throw std::invalid_argument("usage: schedule ALG HEAD R1,R2,... [up|down]");
        }
        const auto direction = tokens.size() == 5 ? tokens[4] : "up";
        if (direction != "up" && direction != "down") throw std::invalid_argument("direction must be up or down");
        const auto requests = parseRequests(tokens[3]);
        const auto result = vfs::scheduleRequests(requests, parseSize(tokens[2], "head"),
                                                   filesystem.blockCount(), tokens[1], direction == "up");
        std::cout << "service order:";
        for (const auto request : result.order) std::cout << ' ' << request;
        std::cout << "\nhead movement: " << result.headMovement << " blocks\n";
    } else if (command == "concurrent-read") {
        if (tokens.size() < 2) throw std::invalid_argument("usage: concurrent-read PATH [PATH ...]");
        vfs::WorkerPool workers(std::min<std::size_t>(4, tokens.size() - 1));
        std::vector<std::future<std::string>> results;
        for (std::size_t index = 1; index < tokens.size(); ++index) {
            const auto path = tokens[index];
            results.push_back(workers.submit([&filesystem, path] {
                return filesystem.readFile(path);
            }));
        }
        for (std::size_t index = 0; index < results.size(); ++index) {
            std::cout << tokens[index + 1] << ": " << results[index].get() << '\n';
        }
    } else if (command == "log") {
        requireCount(tokens, 1, "log");
        const auto entries = filesystem.operationLog();
        for (const auto& entry : entries) std::cout << entry << '\n';
        if (entries.empty()) std::cout << "(no operations)\n";
    } else if (command == "license") {
        requireCount(tokens, 1, "license");
        showLicense();
    } else if (command == "crash-write") {
        requireCount(tokens, 3, "crash-write PATH \"TEXT\"");
        filesystem.simulateCrashWrite(tokens[1], tokens[2]);
    } else if (command == "exit" || command == "quit") {
        requireCount(tokens, 1, "exit");
        throw std::runtime_error("__VFS_EXIT__");
    } else {
        throw std::invalid_argument("unknown command: " + command + " (type help)");
    }
}

} // namespace

int main(int argc, char* argv[]) {
    std::filesystem::path diskPath = "virtual_disk.dat";
    std::size_t blocks = vfs::DefaultBlockCount;
    try {
        for (int index = 1; index < argc; ++index) {
            const std::string option = argv[index];
            if (option == "--disk" && index + 1 < argc) {
                diskPath = argv[++index];
            } else if (option == "--blocks" && index + 1 < argc) {
                blocks = parseSize(argv[++index], "block count");
            } else if (option == "--help") {
                showHelp();
                return 0;
            } else {
                throw std::invalid_argument("usage: vfs [--disk FILE] [--blocks COUNT]");
            }
        }
        vfs::VirtualFileSystem filesystem(diskPath, blocks);
        std::cout << "Virtual File System (GNU GPL version 3; NO WARRANTY). "
                     "Type 'license' for license terms or 'help' for commands.\n";
        std::string line;
        while (std::cout << "vfs[" << filesystem.currentUser() << "]> " &&
               std::getline(std::cin, line)) {
            try {
                execute(filesystem, tokenize(line));
            } catch (const vfs::SimulatedCrash&) {
                std::cout << "simulated crash: pending transaction is journaled; restart to recover\n";
                return 77;
            } catch (const std::runtime_error& error) {
                if (std::string(error.what()) == "__VFS_EXIT__") return 0;
                std::cerr << "error: " << error.what() << '\n';
            } catch (const std::exception& error) {
                std::cerr << "error: " << error.what() << '\n';
            }
        }
    } catch (const std::exception& error) {
        std::cerr << "fatal: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
