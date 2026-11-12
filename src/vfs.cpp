#include "vfs.hpp"

#include <algorithm>
#include <fstream>
#include <limits>
#include <sstream>
#include <system_error>
#include <type_traits>
#include <utility>

namespace vfs {
namespace {

template <typename Integer>
void appendInteger(std::string& output, Integer value) {
    static_assert(std::is_integral<Integer>::value, "integer required");
    using Unsigned = typename std::make_unsigned<Integer>::type;
    const auto converted = static_cast<Unsigned>(value);
    for (std::size_t index = 0; index < sizeof(Integer); ++index) {
        output.push_back(static_cast<char>((converted >> (index * 8U)) & 0xffU));
    }
}

template <typename Integer>
Integer readInteger(const std::string& input, std::size_t& offset) {
    static_assert(std::is_integral<Integer>::value, "integer required");
    if (input.size() - offset < sizeof(Integer)) {
        throw std::runtime_error("disk image is truncated");
    }
    using Unsigned = typename std::make_unsigned<Integer>::type;
    Unsigned value = 0;
    for (std::size_t index = 0; index < sizeof(Integer); ++index) {
        value |= static_cast<Unsigned>(
            static_cast<unsigned char>(input[offset++])) << (index * 8U);
    }
    return static_cast<Integer>(value);
}

void appendString(std::string& output, const std::string& value) {
    if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("string is too long to store");
    }
    appendInteger<std::uint32_t>(output, static_cast<std::uint32_t>(value.size()));
    output.append(value);
}

std::string readString(const std::string& input, std::size_t& offset) {
    const auto length = readInteger<std::uint32_t>(input, offset);
    if (length > input.size() - offset) {
        throw std::runtime_error("disk image contains an invalid string length");
    }
    auto result = input.substr(offset, length);
    offset += length;
    return result;
}

bool beginsWithPath(const std::string& value, const std::string& prefix) {
    if (prefix == "/") return !value.empty() && value.front() == '/';
    return value == prefix ||
           (value.size() > prefix.size() && value.compare(0, prefix.size(), prefix) == 0 &&
            value[prefix.size()] == '/');
}

} // namespace

VirtualFileSystem::VirtualFileSystem(std::filesystem::path diskPath,
                                     std::size_t blockCount,
                                     std::size_t cacheCapacity)
    : diskPath_(std::move(diskPath)),
      journalPath_(diskPath_.string() + ".journal"),
      cache_(cacheCapacity) {
    if (diskPath_.empty()) {
        throw std::invalid_argument("disk path cannot be empty");
    }
    recoverOrInitialize(blockCount);
}

void VirtualFileSystem::recoverOrInitialize(std::size_t blockCount) {
    if (std::filesystem::exists(journalPath_)) {
        std::ifstream journal(journalPath_, std::ios::binary);
        if (!journal) throw std::runtime_error("cannot open recovery journal");
        const std::string image((std::istreambuf_iterator<char>(journal)),
                                std::istreambuf_iterator<char>());
        journal.close();
        state_ = parseImage(image);
        writeAtomic(diskPath_, image);
        std::error_code error;
        std::filesystem::remove(journalPath_, error);
        if (error) throw std::runtime_error("recovered disk but could not clear journal: " + error.message());
        record("recovered pending transaction");
        return;
    }
    if (std::filesystem::exists(diskPath_)) {
        std::ifstream disk(diskPath_, std::ios::binary);
        if (!disk) throw std::runtime_error("cannot open virtual disk");
        const std::string image((std::istreambuf_iterator<char>(disk)),
                                std::istreambuf_iterator<char>());
        state_ = parseImage(image);
        if (state_.blocks.size() != blockCount && blockCount != DefaultBlockCount) {
            throw std::runtime_error("requested block count does not match existing disk image");
        }
        record("mounted existing virtual disk");
        return;
    }
    if (blockCount == 0 || blockCount > 16384) {
        throw std::invalid_argument("block count must be between 1 and 16384");
    }
    state_.blocks.resize(blockCount);
    state_.allocated.assign(blockCount, false);
    state_.users.insert("admin");
    Entry root;
    root.directory = true;
    root.owner = "admin";
    root.permissions = 7;
    state_.entries.emplace("/", root);
    writeAtomic(diskPath_, serializeImage(state_));
    record("formatted new virtual disk");
}

std::uint32_t VirtualFileSystem::checksum(const std::string& data) {
    std::uint32_t hash = 2166136261U;
    for (const unsigned char byte : data) {
        hash ^= byte;
        hash *= 16777619U;
    }
    return hash;
}

std::string VirtualFileSystem::serializeImage(const State& state) const {
    std::string body;
    appendInteger<std::uint32_t>(body, static_cast<std::uint32_t>(state.blocks.size()));
    appendInteger<std::uint32_t>(body, static_cast<std::uint32_t>(state.entries.size()));
    appendInteger<std::uint32_t>(body, static_cast<std::uint32_t>(state.users.size()));
    for (const auto& user : state.users) appendString(body, user);
    for (std::size_t index = 0; index < state.blocks.size(); ++index) {
        body.push_back(state.allocated[index] ? '\x01' : '\x00');
        body.append(state.blocks[index].data(), state.blocks[index].size());
    }
    for (const auto& pair : state.entries) {
        appendString(body, pair.first);
        body.push_back(pair.second.directory ? '\x01' : '\x00');
        body.push_back(static_cast<char>(pair.second.permissions));
        appendString(body, pair.second.owner);
        appendInteger<std::uint64_t>(body, pair.second.size);
        appendInteger<std::uint32_t>(body, static_cast<std::uint32_t>(pair.second.blocks.size()));
        for (const auto block : pair.second.blocks) appendInteger<std::uint32_t>(body, block);
    }
    std::string image = "VFS2";
    appendInteger<std::uint32_t>(image, 2);
    appendInteger<std::uint64_t>(image, static_cast<std::uint64_t>(body.size()));
    appendInteger<std::uint32_t>(image, checksum(body));
    image.append(body);
    return image;
}

VirtualFileSystem::State VirtualFileSystem::parseImage(const std::string& image) const {
    if (image.size() < 20 || image.compare(0, 4, "VFS2") != 0) {
        throw std::runtime_error("invalid virtual disk signature");
    }
    std::size_t offset = 4;
    const auto version = readInteger<std::uint32_t>(image, offset);
    const auto bodySize = readInteger<std::uint64_t>(image, offset);
    const auto expectedChecksum = readInteger<std::uint32_t>(image, offset);
    if (version != 2 || bodySize != image.size() - offset) {
        throw std::runtime_error("unsupported or malformed virtual disk image");
    }
    const auto body = image.substr(offset);
    if (checksum(body) != expectedChecksum) {
        throw std::runtime_error("virtual disk checksum mismatch");
    }
    offset = 0;
    State loaded;
    const auto blockCount = readInteger<std::uint32_t>(body, offset);
    const auto entryCount = readInteger<std::uint32_t>(body, offset);
    const auto userCount = readInteger<std::uint32_t>(body, offset);
    if (blockCount == 0 || blockCount > 16384 || entryCount > 100000 ||
        userCount == 0 || userCount > 10000) {
        throw std::runtime_error("virtual disk dimensions are invalid");
    }
    loaded.users.clear();
    for (std::uint32_t index = 0; index < userCount; ++index) {
        const auto user = readString(body, offset);
        if (user.empty() || !loaded.users.insert(user).second) {
            throw std::runtime_error("virtual disk contains an invalid user table");
        }
    }
    loaded.blocks.resize(blockCount);
    loaded.allocated.resize(blockCount);
    for (std::size_t index = 0; index < blockCount; ++index) {
        if (offset >= body.size()) throw std::runtime_error("disk image is truncated");
        loaded.allocated[index] = body[offset++] != '\x00';
        if (body.size() - offset < BlockSize) throw std::runtime_error("disk blocks are truncated");
        std::copy_n(body.data() + offset, BlockSize, loaded.blocks[index].data());
        offset += BlockSize;
    }
    for (std::uint32_t index = 0; index < entryCount; ++index) {
        const auto path = readString(body, offset);
        if (offset + 2 > body.size()) throw std::runtime_error("disk entry is truncated");
        Entry entry;
        entry.directory = body[offset++] != '\x00';
        entry.permissions = static_cast<std::uint8_t>(body[offset++]);
        entry.owner = readString(body, offset);
        entry.size = readInteger<std::uint64_t>(body, offset);
        const auto count = readInteger<std::uint32_t>(body, offset);
        if (count > blockCount || entry.permissions > 7 || loaded.users.count(entry.owner) == 0) {
            throw std::runtime_error("disk entry metadata is invalid");
        }
        entry.blocks.reserve(count);
        for (std::uint32_t block = 0; block < count; ++block) {
            const auto id = readInteger<std::uint32_t>(body, offset);
            if (id >= blockCount || !loaded.allocated[id]) {
                throw std::runtime_error("file points to an invalid or free block");
            }
            entry.blocks.push_back(id);
        }
        if (entry.directory && (!entry.blocks.empty() || entry.size != 0)) {
            throw std::runtime_error("directory contains file data");
        }
        if (!entry.directory && entry.size > static_cast<std::uint64_t>(entry.blocks.size()) * BlockSize) {
            throw std::runtime_error("file size exceeds allocated storage");
        }
        if (!loaded.entries.emplace(path, std::move(entry)).second) {
            throw std::runtime_error("duplicate path in virtual disk");
        }
    }
    if (offset != body.size() || loaded.entries.count("/") == 0 ||
        !loaded.entries.at("/").directory) {
        throw std::runtime_error("virtual disk has trailing data or no root directory");
    }
    for (const auto& pair : loaded.entries) {
        if (pair.first != "/" && loaded.entries.count(parentPath(pair.first)) == 0) {
            throw std::runtime_error("virtual disk contains an orphaned entry");
        }
        if (pair.first != "/" && !loaded.entries.at(parentPath(pair.first)).directory) {
            throw std::runtime_error("virtual disk contains an entry below a file");
        }
    }
    std::vector<bool> referenced(blockCount, false);
    for (const auto& pair : loaded.entries) {
        for (const auto block : pair.second.blocks) {
            if (referenced[block]) throw std::runtime_error("multiple files reference one disk block");
            referenced[block] = true;
        }
    }
    if (referenced != loaded.allocated) {
        throw std::runtime_error("allocation bitmap does not match file block references");
    }
    return loaded;
}

void VirtualFileSystem::writeAtomic(const std::filesystem::path& path,
                                    const std::string& data) const {
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path());
    }
    auto temporary = path;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("cannot create file: " + temporary.string());
        output.write(data.data(), static_cast<std::streamsize>(data.size()));
        output.flush();
        if (!output) throw std::runtime_error("failed writing file: " + temporary.string());
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(path, error);
        error.clear();
        std::filesystem::rename(temporary, path, error);
    }
    if (error) {
        std::filesystem::remove(temporary);
        throw std::runtime_error("cannot replace " + path.string() + ": " + error.message());
    }
}

void VirtualFileSystem::commit(State candidate, const std::string& operation,
                               bool simulateCrash) {
    const auto image = serializeImage(candidate);
    writeAtomic(journalPath_, image);
    if (simulateCrash) {
        throw SimulatedCrash();
    }
    state_ = std::move(candidate);
    cache_.clear();
    record(operation);
    try {
        writeAtomic(diskPath_, image);
    } catch (const std::exception&) {
        return;
    }
    std::error_code error;
    std::filesystem::remove(journalPath_, error);
}

void VirtualFileSystem::record(const std::string& operation) {
    log_.push_back(operation);
    if (log_.size() > 1000) log_.erase(log_.begin());
}

std::string VirtualFileSystem::normalizePath(const std::string& path,
                                             const std::string& cwd) {
    if (path.empty()) throw std::invalid_argument("path cannot be empty");
    if (path.find('\\') != std::string::npos || path.find('\0') != std::string::npos) {
        throw std::invalid_argument("virtual paths use forward slashes only");
    }
    std::vector<std::string> components;
    const std::string combined = path.front() == '/' ? path : cwd + "/" + path;
    std::size_t start = 0;
    while (start < combined.size()) {
        while (start < combined.size() && combined[start] == '/') ++start;
        if (start == combined.size()) break;
        const auto end = combined.find('/', start);
        const auto component = combined.substr(start, end == std::string::npos
            ? std::string::npos : end - start);
        if (component == ".") {
        } else if (component == "..") {
            if (components.empty()) throw std::invalid_argument("path escapes the virtual root");
            components.pop_back();
        } else {
            if (component.size() > 255 || component.find(':') != std::string::npos) {
                throw std::invalid_argument("invalid virtual path component");
            }
            for (const unsigned char character : component) {
                if (character < 32) throw std::invalid_argument("control characters are not allowed in paths");
            }
            components.push_back(component);
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    std::string result;
    for (const auto& component : components) result += "/" + component;
    return result.empty() ? "/" : result;
}

std::string VirtualFileSystem::parentPath(const std::string& path) {
    if (path == "/") return "/";
    const auto separator = path.find_last_of('/');
    return separator == 0 ? "/" : path.substr(0, separator);
}

std::string VirtualFileSystem::leafName(const std::string& path) {
    if (path == "/") return "/";
    return path.substr(path.find_last_of('/') + 1);
}

std::string VirtualFileSystem::resolved(const std::string& path) const {
    const auto normalized = normalizePath(path, "/");
    for (const auto& pair : state_.entries) {
        if (pair.second.directory && pair.first != normalized &&
            beginsWithPath(normalized, pair.first)) {
            requirePermission(pair.second, 1, pair.first);
        }
    }
    return normalized;
}

void VirtualFileSystem::requirePermission(const Entry& entry, std::uint8_t permission,
                                          const std::string& path) const {
    if (currentUser_ != "admin" && (entry.permissions & permission) != permission) {
        throw std::runtime_error("permission denied: " + path);
    }
}

void VirtualFileSystem::requireDirectory(const std::string& path) const {
    const auto found = state_.entries.find(path);
    if (found == state_.entries.end() || !found->second.directory) {
        throw std::runtime_error("directory does not exist: " + path);
    }
    requirePermission(found->second, 1, path);
}

std::vector<std::uint32_t> VirtualFileSystem::allocate(State& state,
                                                       std::size_t byteCount) const {
    if (byteCount > state.blocks.size() * BlockSize) {
        throw std::runtime_error("virtual disk is full");
    }
    const std::size_t required = (byteCount + BlockSize - 1) / BlockSize;
    const auto free = static_cast<std::size_t>(std::count(state.allocated.begin(),
                                                          state.allocated.end(), false));
    if (required > free) throw std::runtime_error("virtual disk is full");
    std::vector<std::uint32_t> result;
    result.reserve(required);
    for (std::size_t index = 0; index < state.allocated.size() && result.size() < required; ++index) {
        if (!state.allocated[index]) {
            state.allocated[index] = true;
            state.blocks[index].fill('\0');
            result.push_back(static_cast<std::uint32_t>(index));
        }
    }
    return result;
}

void VirtualFileSystem::release(State& state, const Entry& entry) {
    for (const auto block : entry.blocks) {
        if (block < state.allocated.size()) {
            state.allocated[block] = false;
            state.blocks[block].fill('\0');
        }
    }
}

std::string VirtualFileSystem::readUnlocked(const std::string& path) const {
    const auto found = state_.entries.find(path);
    if (found == state_.entries.end() || found->second.directory) {
        throw std::runtime_error("file does not exist: " + path);
    }
    requirePermission(found->second, 4, path);
    std::string result;
    result.reserve(static_cast<std::size_t>(found->second.size));
    std::size_t remaining = static_cast<std::size_t>(found->second.size);
    for (const auto block : found->second.blocks) {
        cache_.access(block);
        const auto amount = std::min(remaining, BlockSize);
        result.append(state_.blocks[block].data(), amount);
        remaining -= amount;
    }
    return result;
}

void VirtualFileSystem::writeUnlocked(State& candidate, const std::string& path,
                                      const std::string& data, bool append) const {
    auto found = candidate.entries.find(path);
    if (found == candidate.entries.end() || found->second.directory) {
        throw std::runtime_error("file does not exist: " + path);
    }
    requirePermission(found->second, 2, path);
    std::string contents = append ? readUnlocked(path) : std::string();
    if (data.size() > std::numeric_limits<std::size_t>::max() - contents.size()) {
        throw std::runtime_error("file is too large");
    }
    contents += data;
    release(candidate, found->second);
    auto blocks = allocate(candidate, contents.size());
    std::size_t offset = 0;
    for (const auto block : blocks) {
        const auto amount = std::min(BlockSize, contents.size() - offset);
        std::copy_n(contents.data() + offset, amount, candidate.blocks[block].data());
        offset += amount;
    }
    found = candidate.entries.find(path);
    found->second.blocks = std::move(blocks);
    found->second.size = contents.size();
}

void VirtualFileSystem::login(const std::string& user) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (state_.users.count(user) == 0) throw std::runtime_error("unknown user: " + user);
    currentUser_ = user;
    record("session switched to " + user);
}

std::string VirtualFileSystem::currentUser() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return currentUser_;
}

void VirtualFileSystem::addUser(const std::string& name) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (currentUser_ != "admin") throw std::runtime_error("only admin can add users");
    if (name.empty() || name.size() > 64 || name.find_first_of("/\\:") != std::string::npos) {
        throw std::invalid_argument("invalid user name");
    }
    State candidate = state_;
    if (!candidate.users.insert(name).second) throw std::runtime_error("user already exists");
    commit(std::move(candidate), "created user " + name);
}

void VirtualFileSystem::makeDirectory(const std::string& input) {
    std::lock_guard<std::mutex> guard(mutex_);
    const auto path = resolved(input);
    if (path == "/" || state_.entries.count(path) != 0) throw std::runtime_error("path already exists");
    const auto parent = state_.entries.find(parentPath(path));
    if (parent == state_.entries.end() || !parent->second.directory) throw std::runtime_error("parent directory does not exist");
    requirePermission(parent->second, 3, parentPath(path));
    State candidate = state_;
    Entry entry;
    entry.directory = true;
    entry.owner = currentUser_;
    entry.permissions = 7;
    candidate.entries.emplace(path, entry);
    commit(std::move(candidate), "created directory " + path);
}

void VirtualFileSystem::createFile(const std::string& input) {
    std::lock_guard<std::mutex> guard(mutex_);
    const auto path = resolved(input);
    if (path == "/" || state_.entries.count(path) != 0) throw std::runtime_error("path already exists");
    const auto parent = state_.entries.find(parentPath(path));
    if (parent == state_.entries.end() || !parent->second.directory) throw std::runtime_error("parent directory does not exist");
    requirePermission(parent->second, 3, parentPath(path));
    State candidate = state_;
    Entry entry;
    entry.owner = currentUser_;
    entry.permissions = 6;
    candidate.entries.emplace(path, entry);
    commit(std::move(candidate), "created file " + path);
}

void VirtualFileSystem::writeFile(const std::string& input, const std::string& data) {
    std::lock_guard<std::mutex> guard(mutex_);
    const auto path = resolved(input);
    State candidate = state_;
    writeUnlocked(candidate, path, data, false);
    commit(std::move(candidate), "wrote " + path + " (" + std::to_string(data.size()) + " bytes)");
}

void VirtualFileSystem::appendFile(const std::string& input, const std::string& data) {
    std::lock_guard<std::mutex> guard(mutex_);
    const auto path = resolved(input);
    State candidate = state_;
    writeUnlocked(candidate, path, data, true);
    commit(std::move(candidate), "appended " + path + " (" + std::to_string(data.size()) + " bytes)");
}

std::string VirtualFileSystem::readFile(const std::string& input) {
    std::lock_guard<std::mutex> guard(mutex_);
    return readUnlocked(resolved(input));
}

void VirtualFileSystem::checkAccess(const std::string& input,
                                    std::uint8_t permission) const {
    std::lock_guard<std::mutex> guard(mutex_);
    if (permission == 0 || permission > 7) {
        throw std::invalid_argument("access check requires a permission bit");
    }
    const auto path = resolved(input);
    const auto found = state_.entries.find(path);
    if (found == state_.entries.end()) throw std::runtime_error("path does not exist: " + path);
    requirePermission(found->second, permission, path);
}

std::vector<EntryInfo> VirtualFileSystem::list(const std::string& input) const {
    std::lock_guard<std::mutex> guard(mutex_);
    const auto path = resolved(input);
    const auto found = state_.entries.find(path);
    if (found == state_.entries.end() || !found->second.directory) throw std::runtime_error("directory does not exist");
    requirePermission(found->second, 5, path);
    std::vector<EntryInfo> result;
    for (const auto& pair : state_.entries) {
        if (pair.first != path && parentPath(pair.first) == path) {
            result.push_back({pair.first, pair.second.directory, pair.second.owner,
                              pair.second.permissions, pair.second.size, pair.second.blocks.size()});
        }
    }
    return result;
}

EntryInfo VirtualFileSystem::stat(const std::string& input) const {
    std::lock_guard<std::mutex> guard(mutex_);
    const auto path = resolved(input);
    const auto found = state_.entries.find(path);
    if (found == state_.entries.end()) throw std::runtime_error("path does not exist: " + path);
    requirePermission(found->second, found->second.directory ? 1 : 4, path);
    return {path, found->second.directory, found->second.owner,
            found->second.permissions, found->second.size, found->second.blocks.size()};
}

void VirtualFileSystem::renamePath(const std::string& sourceInput,
                                   const std::string& destinationInput) {
    std::lock_guard<std::mutex> guard(mutex_);
    const auto source = resolved(sourceInput);
    const auto destination = resolved(destinationInput);
    if (source == "/" || destination == "/" || beginsWithPath(destination, source)) {
        throw std::runtime_error("cannot rename root or move a directory into itself");
    }
    if (state_.entries.count(source) == 0) throw std::runtime_error("source path does not exist");
    if (state_.entries.count(destination) != 0) throw std::runtime_error("destination path already exists");
    const auto parent = state_.entries.find(parentPath(destination));
    if (parent == state_.entries.end() || !parent->second.directory) throw std::runtime_error("destination parent does not exist");
    const auto oldParent = state_.entries.find(parentPath(source));
    requirePermission(oldParent->second, 3, parentPath(source));
    requirePermission(parent->second, 3, parentPath(destination));
    requirePermission(state_.entries.at(source), 2, source);
    State candidate = state_;
    std::vector<std::pair<std::string, Entry>> moved;
    for (auto iterator = candidate.entries.begin(); iterator != candidate.entries.end();) {
        if (beginsWithPath(iterator->first, source)) {
            auto newPath = destination + iterator->first.substr(source.size());
            moved.emplace_back(std::move(newPath), iterator->second);
            iterator = candidate.entries.erase(iterator);
        } else {
            ++iterator;
        }
    }
    for (auto& pair : moved) candidate.entries.emplace(std::move(pair.first), std::move(pair.second));
    commit(std::move(candidate), "renamed " + source + " to " + destination);
}

void VirtualFileSystem::removePath(const std::string& input) {
    std::lock_guard<std::mutex> guard(mutex_);
    const auto path = resolved(input);
    if (path == "/") throw std::runtime_error("cannot remove the virtual root");
    const auto found = state_.entries.find(path);
    if (found == state_.entries.end()) throw std::runtime_error("path does not exist: " + path);
    const auto parent = state_.entries.find(parentPath(path));
    requirePermission(parent->second, 3, parentPath(path));
    requirePermission(found->second, 2, path);
    State candidate = state_;
    for (auto iterator = candidate.entries.begin(); iterator != candidate.entries.end();) {
        if (beginsWithPath(iterator->first, path)) {
            release(candidate, iterator->second);
            iterator = candidate.entries.erase(iterator);
        } else {
            ++iterator;
        }
    }
    commit(std::move(candidate), "removed " + path);
}

void VirtualFileSystem::chmod(const std::string& input, std::uint8_t permissions) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (permissions > 7) throw std::invalid_argument("permissions must be between 0 and 7");
    const auto path = resolved(input);
    auto found = state_.entries.find(path);
    if (found == state_.entries.end()) throw std::runtime_error("path does not exist: " + path);
    if (currentUser_ != "admin" && currentUser_ != found->second.owner) {
        throw std::runtime_error("only the owner or admin can change permissions");
    }
    State candidate = state_;
    candidate.entries.at(path).permissions = permissions;
    commit(std::move(candidate), "changed permissions on " + path);
}

DiskReport VirtualFileSystem::diskReport() const {
    std::lock_guard<std::mutex> guard(mutex_);
    DiskReport report;
    report.totalBlocks = state_.blocks.size();
    report.usedBlocks = static_cast<std::size_t>(std::count(state_.allocated.begin(),
                                                             state_.allocated.end(), true));
    report.freeBlocks = report.totalBlocks - report.usedBlocks;
    std::size_t run = 0;
    for (const bool allocated : state_.allocated) {
        if (!allocated) {
            ++run;
        } else if (run != 0) {
            ++report.freeExtents;
            report.largestFreeExtent = std::max(report.largestFreeExtent, run);
            run = 0;
        }
    }
    if (run != 0) {
        ++report.freeExtents;
        report.largestFreeExtent = std::max(report.largestFreeExtent, run);
    }
    return report;
}

std::vector<std::string> VirtualFileSystem::operationLog() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return log_;
}

void VirtualFileSystem::setCachePolicy(CachePolicy policy) {
    std::lock_guard<std::mutex> guard(mutex_);
    cache_.setPolicy(policy);
}

CachePolicy VirtualFileSystem::cachePolicy() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return cache_.policy();
}

std::size_t VirtualFileSystem::cacheHits() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return cache_.hits();
}

std::size_t VirtualFileSystem::cacheMisses() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return cache_.misses();
}

std::size_t VirtualFileSystem::blockCount() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return state_.blocks.size();
}

void VirtualFileSystem::simulateCrashWrite(const std::string& input,
                                           const std::string& data) {
    std::lock_guard<std::mutex> guard(mutex_);
    const auto path = resolved(input);
    State candidate = state_;
    writeUnlocked(candidate, path, data, false);
    commit(std::move(candidate), "simulated interrupted write " + path, true);
}

} // namespace vfs
