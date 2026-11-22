#include "analytics.hpp"
#include "vfs.hpp"
#include "worker_pool.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
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
            if (character == 'n') token.push_back('\n');
            else if (character == 'r') token.push_back('\r');
            else if (character == 't') token.push_back('\t');
            else token.push_back(character);
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

std::string resolvePath(const std::string& path, const std::string& currentDirectory) {
    if (path.empty()) throw std::invalid_argument("path cannot be empty");
    const std::string combined = path.front() == '/' ? path : currentDirectory + "/" + path;
    std::vector<std::string> components;
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
            components.push_back(component);
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    std::string result;
    for (const auto& component : components) result += "/" + component;
    return result.empty() ? "/" : result;
}

std::string baseName(const std::string& path) {
    if (path == "/") return path;
    const auto separator = path.find_last_of('/');
    return path.substr(separator == std::string::npos ? 0 : separator + 1);
}

std::vector<std::string> split(const std::string& value, char delimiter) {
    std::vector<std::string> pieces;
    std::stringstream stream(value);
    std::string piece;
    while (std::getline(stream, piece, delimiter)) {
        if (piece.empty()) throw std::invalid_argument("list contains an empty item");
        pieces.push_back(std::move(piece));
    }
    if (value.empty() || value.back() == delimiter)
        throw std::invalid_argument("list contains an empty item");
    return pieces;
}

std::vector<std::string> splitFields(const std::string& value, char delimiter) {
    std::vector<std::string> fields;
    std::size_t start = 0;
    while (true) {
        const auto end = value.find(delimiter, start);
        fields.push_back(value.substr(start, end == std::string::npos
            ? std::string::npos : end - start));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return fields;
}

std::vector<std::string> readLines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) lines.push_back(std::move(line));
    return lines;
}

void printCsv(const vfs::analytics::Table& table) {
    std::cout << vfs::analytics::writeCsv(table);
}

void showHelp() {
    std::cout
        << "Commands:\n"
        << "  help                                      Show commands\n"
        << "  useradd NAME                              Add a user (admin only)\n"
        << "  su USER                                   Switch active user\n"
        << "  whoami                                    Show active user\n"
        << "  pwd                                       Show the current virtual directory\n"
        << "  cd [PATH]                                 Change directory (default: /)\n"
        << "  mkdir PATH                                Create a directory\n"
        << "  touch PATH                                Create an empty file\n"
        << "  write PATH \"TEXT\"                        Replace file contents\n"
        << "  append PATH \"TEXT\"                       Append to a file\n"
        << "  cat PATH                                  Read a file\n"
        << "  head [-n COUNT] PATH                      Show the first lines of a file\n"
        << "  tail [-n COUNT] PATH                      Show the last lines of a file\n"
        << "  wc PATH                                   Count lines, words, and bytes\n"
        << "  echo TEXT                                Print text\n"
        << "  grep [-i] PATTERN PATH                    Search a virtual text file\n"
        << "  sort [-r] PATH                            Sort text lines\n"
        << "  uniq [-c] PATH                            Collapse adjacent duplicate lines\n"
        << "  cut -d DELIM -f FIELDS PATH               Select delimited fields\n"
        << "  ls [-l] [PATH]                            List entries and show the directory\n"
        << "  tree [PATH]                               Display nested virtual entries\n"
        << "  find [PATH] NAME                          Find entries by name substring\n"
        << "  cp SOURCE DEST                            Copy a virtual file\n"
        << "  rmdir PATH                                Remove an empty directory\n"
        << "  stat PATH                                 Show file or directory metadata\n"
        << "  mv SOURCE DESTINATION                     Rename or move an entry\n"
        << "  rm PATH | rm -r PATH                      Remove a file or directory tree\n"
        << "  chmod PATH rwx                            Set permission bits (use - to clear)\n"
        << "  access PATH r|w|x                         Check a permission bit\n"
        << "  df                                        Show block use and fragmentation\n"
        << "  cache-policy FIFO|LRU                     Select cache replacement policy\n"
        << "  cache-stats                               Show cache hits and misses\n"
        << "  schedule ALG HEAD R1,R2,... [up|down]     Simulate disk scheduling\n"
        << "  concurrent-read PATH [PATH ...]           Read files on worker threads\n"
        << "  log                                       Show this session's operation log\n"
        << "  license                                   Display the bundled GPL-3.0 license\n"
        << "  man [COMMAND]                             Show built-in command documentation\n"
        << "  clear                                     Clear the terminal display\n"
        << "  csv-info PATH                             Show CSV dimensions and columns\n"
        << "  describe PATH [COLUMN]                    Summarize numeric CSV columns\n"
        << "  freq PATH COLUMN                          Count categorical values\n"
        << "  filter PATH COLUMN OP VALUE               Filter CSV rows\n"
        << "  select PATH COLUMN[,COLUMN...]            Select CSV columns\n"
        << "  groupby PATH KEY [VALUE_COLUMN]            Count groups or aggregate numbers\n"
        << "  histogram PATH COLUMN [BINS]               Bin numeric CSV values\n"
        << "  join LEFT RIGHT KEY                       Inner-join CSV files on a shared column\n"
        << "  crash-write PATH \"TEXT\"                   Leave a journal for next-run recovery\n"
        << "  exit                                      Quit\n";
}

void showMan(const std::string& command) {
    if (command.empty()) {
        std::cout << "Virtual File System manual\n"
                  << "Pages: cd pwd ls tree find mkdir touch cp cat head tail wc echo grep sort uniq cut write append mv rm rmdir "
                     "chmod stat useradd su whoami access df schedule cache-policy cache-stats "
                     "concurrent-read csv-info describe freq filter select groupby histogram join log crash-write clear license help exit\n"
                  << "Use man COMMAND for syntax. Paths are rooted at / inside the virtual disk.\n";
        return;
    }
    if (command == "cd") std::cout << "cd [PATH] - change directory; no path returns to /. Supports . and ..; paths cannot escape virtual root.\n";
    else if (command == "pwd") std::cout << "pwd - print the current virtual directory.\n";
    else if (command == "ls") std::cout << "ls [-l] [PATH] - list a directory (defaults to current directory) and print its path. -l shows metadata.\n";
    else if (command == "tree") std::cout << "tree [PATH] - recursively display virtual files and directories.\n";
    else if (command == "find") std::cout << "find [PATH] NAME - recursively find entries whose basename contains NAME.\n";
    else if (command == "mkdir") std::cout << "mkdir PATH - create one virtual directory; its parent must exist.\n";
    else if (command == "touch") std::cout << "touch PATH - create an empty virtual file; existing files are not truncated.\n";
    else if (command == "cp") std::cout << "cp SOURCE DEST - copy a virtual file to a new path.\n";
    else if (command == "cat") std::cout << "cat PATH - print the contents of a virtual text file.\n";
    else if (command == "head") std::cout << "head [-n COUNT] PATH - print the first COUNT lines (default 10) from a virtual file.\n";
    else if (command == "tail") std::cout << "tail [-n COUNT] PATH - print the last COUNT lines (default 10) from a virtual file.\n";
    else if (command == "wc") std::cout << "wc PATH - print newline count, whitespace-delimited word count, and byte count.\n";
    else if (command == "echo") std::cout << "echo TEXT - print the supplied arguments separated by spaces. No shell redirection is performed.\n";
    else if (command == "grep") std::cout << "grep [-i] PATTERN PATH - print virtual text-file lines containing PATTERN; -i ignores ASCII case.\n";
    else if (command == "sort") std::cout << "sort [-r] PATH - print text-file lines in lexical order, optionally reversed.\n";
    else if (command == "uniq") std::cout << "uniq [-c] PATH - collapse adjacent repeated lines; -c prefixes each run with its count.\n";
    else if (command == "cut") std::cout << "cut -d DELIM -f FIELDS PATH - print selected 1-based fields from delimiter-separated lines.\n";
    else if (command == "write") std::cout << "write PATH \"TEXT\" - replace virtual file contents.\n";
    else if (command == "append") std::cout << "append PATH \"TEXT\" - append text to a virtual file.\n";
    else if (command == "mv") std::cout << "mv SOURCE DEST - rename or move a virtual entry.\n";
    else if (command == "rm") std::cout << "rm PATH removes a file; rm -r PATH removes a directory tree.\n";
    else if (command == "rmdir") std::cout << "rmdir PATH - remove an empty virtual directory.\n";
    else if (command == "chmod") std::cout << "chmod PATH rwx - set the simulator's owner read/write/execute bits, such as rw-. This is not full Unix mode handling.\n";
    else if (command == "stat") std::cout << "stat PATH - show metadata for a virtual entry.\n";
    else if (command == "useradd") std::cout << "useradd NAME - add a simulated user (admin only).\n";
    else if (command == "su") std::cout << "su USER - switch the active simulated user.\n";
    else if (command == "whoami") std::cout << "whoami - print the active simulated user.\n";
    else if (command == "access") std::cout << "access PATH r|w|x - check a permission on a virtual entry.\n";
    else if (command == "df") std::cout << "df - report virtual disk usage and free-space extents.\n";
    else if (command == "schedule") std::cout << "schedule ALG HEAD R1,R2,... [up|down] - simulate FCFS, SSTF, SCAN, or C-SCAN.\n";
    else if (command == "cache-policy") std::cout << "cache-policy FIFO|LRU - select a simulated block-cache replacement policy.\n";
    else if (command == "cache-stats") std::cout << "cache-stats - show simulated cache hit and miss counters.\n";
    else if (command == "csv-info") std::cout << "csv-info PATH - report CSV data rows, columns, and header names.\n";
    else if (command == "describe") std::cout << "describe PATH [COLUMN] - report count, min, max, mean, median, and population standard deviation for numeric CSV columns.\n";
    else if (command == "freq") std::cout << "freq PATH COLUMN - show categorical value counts in descending frequency order.\n";
    else if (command == "filter") std::cout << "filter PATH COLUMN OP VALUE - filter CSV rows using =, !=, >, >=, <, <=, or contains. Numeric comparisons are numeric when both values parse as numbers.\n";
    else if (command == "select") std::cout << "select PATH COLUMN[,COLUMN...] - project CSV columns by header name or 1-based index.\n";
    else if (command == "groupby") std::cout << "groupby PATH KEY [VALUE_COLUMN] - count groups or report per-group count, sum, and mean for a numeric column.\n";
    else if (command == "histogram") std::cout << "histogram PATH COLUMN [BINS] - print a numeric histogram with 1 to 1000 bins (default 10).\n";
    else if (command == "join") std::cout << "join LEFT RIGHT KEY - inner-join two CSV files using the same named key column in both.\n";
    else if (command == "concurrent-read") std::cout << "concurrent-read PATH [PATH ...] - read virtual files using worker threads.\n";
    else if (command == "log") std::cout << "log - display this session's operation log.\n";
    else if (command == "crash-write") std::cout << "crash-write PATH \"TEXT\" - journal a write and exit to demonstrate startup recovery.\n";
    else if (command == "clear") std::cout << "clear - emit terminal control codes to clear a compatible display.\n";
    else if (command == "license") std::cout << "license - display the bundled GNU GPL version 3 license.\n";
    else if (command == "help") std::cout << "help - list available simulator commands.\n";
    else if (command == "exit" || command == "quit") std::cout << "exit - close the virtual filesystem session.\n";
    else if (command == "man") std::cout << "man [COMMAND] - show this manual index or a command page.\n";
    else std::cout << "No manual entry for " << command << ". Type help for available commands.\n";
}

void showTree(vfs::VirtualFileSystem& filesystem, const std::string& path,
              const std::string& prefix = "") {
    const auto entries = filesystem.list(path);
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto& entry = entries[index];
        const bool last = index + 1 == entries.size();
        std::cout << prefix << (last ? "`-- " : "|-- ") << baseName(entry.path)
                  << (entry.directory ? "/" : "") << '\n';
        if (entry.directory) showTree(filesystem, entry.path, prefix + (last ? "    " : "|   "));
    }
}

void findEntries(vfs::VirtualFileSystem& filesystem, const std::string& path,
                 const std::string& name) {
    for (const auto& entry : filesystem.list(path)) {
        if (baseName(entry.path).find(name) != std::string::npos)
            std::cout << entry.path << (entry.directory ? "/" : "") << '\n';
        if (entry.directory) findEntries(filesystem, entry.path, name);
    }
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
             const std::vector<std::string>& tokens,
             std::string& currentDirectory) {
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
    } else if (command == "pwd") {
        requireCount(tokens, 1, "pwd");
        std::cout << currentDirectory << '\n';
    } else if (command == "cd") {
        if (tokens.size() > 2) throw std::invalid_argument("usage: cd [PATH]");
        const auto destination = resolvePath(tokens.size() == 2 ? tokens[1] : "/", currentDirectory);
        const auto entry = filesystem.stat(destination);
        if (!entry.directory) throw std::runtime_error("not a directory: " + destination);
        filesystem.checkAccess(destination, 1);
        currentDirectory = destination;
    } else if (command == "mkdir") {
        requireCount(tokens, 2, "mkdir PATH");
        filesystem.makeDirectory(resolvePath(tokens[1], currentDirectory));
        std::cout << "directory created\n";
    } else if (command == "touch") {
        requireCount(tokens, 2, "touch PATH");
        filesystem.createFile(resolvePath(tokens[1], currentDirectory));
        std::cout << "file created\n";
    } else if (command == "write" || command == "append") {
        requireCount(tokens, 3, command + " PATH \"TEXT\"");
        const auto path = resolvePath(tokens[1], currentDirectory);
        if (command == "write") filesystem.writeFile(path, tokens[2]);
        else filesystem.appendFile(path, tokens[2]);
        std::cout << "saved " << tokens[2].size() << " bytes\n";
    } else if (command == "cat") {
        requireCount(tokens, 2, "cat PATH");
        std::cout << filesystem.readFile(resolvePath(tokens[1], currentDirectory)) << '\n';
    } else if (command == "head" || command == "tail") {
        std::size_t count = 10;
        std::size_t pathIndex = 1;
        if (tokens.size() == 4 && tokens[1] == "-n") {
            count = parseSize(tokens[2], "line count");
            pathIndex = 3;
        } else if (tokens.size() != 2) {
            throw std::invalid_argument("usage: " + command + " [-n COUNT] PATH");
        }
        const auto contents = filesystem.readFile(resolvePath(tokens[pathIndex], currentDirectory));
        std::istringstream stream(contents);
        std::string line;
        if (command == "head") {
            for (std::size_t index = 0; index < count && std::getline(stream, line); ++index)
                std::cout << line << '\n';
        } else {
            std::deque<std::string> lines;
            while (std::getline(stream, line)) {
                lines.push_back(line);
                if (lines.size() > count) lines.pop_front();
            }
            for (const auto& item : lines) std::cout << item << '\n';
        }
    } else if (command == "wc") {
        requireCount(tokens, 2, "wc PATH");
        const auto contents = filesystem.readFile(resolvePath(tokens[1], currentDirectory));
        std::size_t words = 0;
        bool inWord = false;
        for (const unsigned char character : contents) {
            if (std::isspace(character)) inWord = false;
            else if (!inWord) { ++words; inWord = true; }
        }
        const auto lines = static_cast<std::size_t>(std::count(contents.begin(), contents.end(), '\n'));
        std::cout << lines << ' ' << words << ' ' << contents.size() << ' '
                  << resolvePath(tokens[1], currentDirectory) << '\n';
    } else if (command == "echo") {
        for (std::size_t index = 1; index < tokens.size(); ++index) {
            if (index != 1) std::cout << ' ';
            std::cout << tokens[index];
        }
        std::cout << '\n';
    } else if (command == "grep") {
        const bool ignoreCase = tokens.size() == 4 && tokens[1] == "-i";
        if ((!ignoreCase && tokens.size() != 3) || (ignoreCase && tokens.size() != 4))
            throw std::invalid_argument("usage: grep [-i] PATTERN PATH");
        const auto pattern = tokens[ignoreCase ? 2 : 1];
        const auto path = resolvePath(tokens[ignoreCase ? 3 : 2], currentDirectory);
        const auto lines = readLines(filesystem.readFile(path));
        std::string needle = pattern;
        if (ignoreCase) std::transform(needle.begin(), needle.end(), needle.begin(),
            [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
        for (const auto& line : lines) {
            auto candidate = line;
            if (ignoreCase) std::transform(candidate.begin(), candidate.end(), candidate.begin(),
                [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
            if (candidate.find(needle) != std::string::npos) std::cout << line << '\n';
        }
    } else if (command == "sort") {
        const bool reverse = tokens.size() == 3 && tokens[1] == "-r";
        if ((!reverse && tokens.size() != 2) || (reverse && tokens.size() != 3))
            throw std::invalid_argument("usage: sort [-r] PATH");
        const auto path = resolvePath(tokens[reverse ? 2 : 1], currentDirectory);
        auto lines = readLines(filesystem.readFile(path));
        std::stable_sort(lines.begin(), lines.end());
        if (reverse) std::reverse(lines.begin(), lines.end());
        for (const auto& line : lines) std::cout << line << '\n';
    } else if (command == "uniq") {
        const bool counts = tokens.size() == 3 && tokens[1] == "-c";
        if ((!counts && tokens.size() != 2) || (counts && tokens.size() != 3))
            throw std::invalid_argument("usage: uniq [-c] PATH");
        const auto path = resolvePath(tokens[counts ? 2 : 1], currentDirectory);
        const auto lines = readLines(filesystem.readFile(path));
        for (std::size_t start = 0; start < lines.size();) {
            std::size_t end = start + 1;
            while (end < lines.size() && lines[end] == lines[start]) ++end;
            if (counts) std::cout << std::setw(7) << (end - start) << ' ';
            std::cout << lines[start] << '\n';
            start = end;
        }
    } else if (command == "cut") {
        requireCount(tokens, 6, "cut -d DELIM -f FIELDS PATH");
        if (tokens[1] != "-d" || tokens[3] != "-f" || tokens[2].size() != 1)
            throw std::invalid_argument("usage: cut -d DELIM -f FIELDS PATH (DELIM must be one character)");
        std::vector<std::size_t> fields;
        for (const auto& field : split(tokens[4], ',')) {
            const auto number = parseSize(field, "field number");
            if (number == 0) throw std::invalid_argument("field numbers start at 1");
            fields.push_back(number - 1);
        }
        const auto path = resolvePath(tokens[5], currentDirectory);
        const auto lines = readLines(filesystem.readFile(path));
        const char delimiter = tokens[2][0];
        for (const auto& line : lines) {
            const auto values = splitFields(line, delimiter);
            for (std::size_t index = 0; index < fields.size(); ++index) {
                if (fields[index] >= values.size()) throw std::runtime_error("requested field is missing from a line");
                if (index != 0) std::cout << delimiter;
                std::cout << values[fields[index]];
            }
            std::cout << '\n';
        }
    } else if (command == "csv-info") {
        requireCount(tokens, 2, "csv-info PATH");
        const auto path = resolvePath(tokens[1], currentDirectory);
        const auto table = vfs::analytics::parseCsv(filesystem.readFile(path));
        if (table.empty()) throw std::runtime_error("CSV file is empty");
        std::cout << "file: " << path << "\nrows: " << (table.size() - 1)
                  << "\ncolumns: " << table.front().size() << "\nheader:\n";
        for (std::size_t index = 0; index < table.front().size(); ++index)
            std::cout << "  " << (index + 1) << ". " << table.front()[index] << '\n';
    } else if (command == "describe") {
        if (tokens.size() < 2 || tokens.size() > 3) throw std::invalid_argument("usage: describe PATH [COLUMN]");
        const auto path = resolvePath(tokens[1], currentDirectory);
        const auto table = vfs::analytics::parseCsv(filesystem.readFile(path));
        if (table.empty()) throw std::runtime_error("CSV file is empty");
        std::vector<std::size_t> columns;
        if (tokens.size() == 3) columns.push_back(vfs::analytics::columnIndex(table, tokens[2]));
        else for (std::size_t index = 0; index < table.front().size(); ++index) columns.push_back(index);
        std::cout << "column,count,min,max,mean,median,stddev\n" << std::setprecision(6);
        bool displayed = false;
        for (const auto column : columns) {
            try {
                const auto summary = vfs::analytics::summarizeNumeric(table, column);
                std::cout << vfs::analytics::writeCsv({{table.front()[column], std::to_string(summary.count),
                    std::to_string(summary.minimum), std::to_string(summary.maximum),
                    std::to_string(summary.mean), std::to_string(summary.median),
                    std::to_string(summary.standardDeviation)}});
                displayed = true;
            } catch (const std::invalid_argument&) {
                if (tokens.size() == 3) throw;
            }
        }
        if (!displayed) std::cout << "(no numeric columns)\n";
    } else if (command == "freq") {
        requireCount(tokens, 3, "freq PATH COLUMN");
        const auto table = vfs::analytics::parseCsv(filesystem.readFile(
            resolvePath(tokens[1], currentDirectory)));
        const auto column = vfs::analytics::columnIndex(table, tokens[2]);
        vfs::analytics::Table output{{"value", "count"}};
        for (const auto& item : vfs::analytics::frequencies(table, column))
            output.push_back({item.first, std::to_string(item.second)});
        printCsv(output);
    } else if (command == "filter") {
        requireCount(tokens, 5, "filter PATH COLUMN OP VALUE");
        const auto table = vfs::analytics::parseCsv(filesystem.readFile(
            resolvePath(tokens[1], currentDirectory)));
        const auto column = vfs::analytics::columnIndex(table, tokens[2]);
        printCsv(vfs::analytics::filterRows(table, column, tokens[3], tokens[4]));
    } else if (command == "select") {
        requireCount(tokens, 3, "select PATH COLUMN[,COLUMN...]");
        const auto table = vfs::analytics::parseCsv(filesystem.readFile(
            resolvePath(tokens[1], currentDirectory)));
        std::vector<std::size_t> columns;
        for (const auto& name : split(tokens[2], ','))
            columns.push_back(vfs::analytics::columnIndex(table, name));
        printCsv(vfs::analytics::selectColumns(table, columns));
    } else if (command == "groupby") {
        if (tokens.size() < 3 || tokens.size() > 4)
            throw std::invalid_argument("usage: groupby PATH KEY [VALUE_COLUMN]");
        const auto table = vfs::analytics::parseCsv(filesystem.readFile(
            resolvePath(tokens[1], currentDirectory)));
        const auto keyColumn = vfs::analytics::columnIndex(table, tokens[2]);
        if (tokens.size() == 3) {
            vfs::analytics::Table output{{table.front()[keyColumn], "count"}};
            for (const auto& group : vfs::analytics::groupCounts(table, keyColumn))
                output.push_back({group.key, std::to_string(group.count)});
            printCsv(output);
        } else {
            const auto valueColumn = vfs::analytics::columnIndex(table, tokens[3]);
            vfs::analytics::Table output{{table.front()[keyColumn], "count", "sum", "mean"}};
            for (const auto& group : vfs::analytics::groupNumeric(table, keyColumn, valueColumn))
                output.push_back({group.key, std::to_string(group.count),
                    std::to_string(group.sum), std::to_string(group.mean)});
            printCsv(output);
        }
    } else if (command == "histogram") {
        if (tokens.size() < 3 || tokens.size() > 4)
            throw std::invalid_argument("usage: histogram PATH COLUMN [BINS]");
        const auto table = vfs::analytics::parseCsv(filesystem.readFile(
            resolvePath(tokens[1], currentDirectory)));
        const auto column = vfs::analytics::columnIndex(table, tokens[2]);
        const auto count = tokens.size() == 4 ? parseSize(tokens[3], "bin count") : 10;
        vfs::analytics::Table output{{"lower", "upper", "count"}};
        for (const auto& bin : vfs::analytics::histogram(table, column, count))
            output.push_back({std::to_string(bin.lower), std::to_string(bin.upper),
                              std::to_string(bin.count)});
        printCsv(output);
    } else if (command == "join") {
        requireCount(tokens, 4, "join LEFT RIGHT KEY");
        const auto left = vfs::analytics::parseCsv(filesystem.readFile(
            resolvePath(tokens[1], currentDirectory)));
        const auto right = vfs::analytics::parseCsv(filesystem.readFile(
            resolvePath(tokens[2], currentDirectory)));
        const auto leftKey = vfs::analytics::columnIndex(left, tokens[3]);
        const auto rightKey = vfs::analytics::columnIndex(right, tokens[3]);
        printCsv(vfs::analytics::innerJoin(left, right, leftKey, rightKey));
    } else if (command == "ls") {
        std::size_t argument = 1;
        const bool longFormat = tokens.size() > 1 && tokens[1] == "-l";
        if (longFormat) ++argument;
        if (tokens.size() > argument + 1) throw std::invalid_argument("usage: ls [-l] [PATH]");
        const auto path = resolvePath(tokens.size() == argument + 1 ? tokens[argument] : currentDirectory,
                                      currentDirectory);
        const auto entries = filesystem.list(path);
        std::cout << "Directory: " << path << '\n';
        for (const auto& entry : entries) {
            if (longFormat) printEntry(entry);
            else std::cout << baseName(entry.path) << (entry.directory ? "/" : "") << '\n';
        }
        if (entries.empty()) std::cout << "(empty)\n";
    } else if (command == "tree") {
        if (tokens.size() > 2) throw std::invalid_argument("usage: tree [PATH]");
        const auto path = resolvePath(tokens.size() == 2 ? tokens[1] : currentDirectory,
                                      currentDirectory);
        std::cout << (path == "/" ? "/" : path + "/") << '\n';
        showTree(filesystem, path);
    } else if (command == "find") {
        if (tokens.size() < 2 || tokens.size() > 3) throw std::invalid_argument("usage: find [PATH] NAME");
        const auto path = resolvePath(tokens.size() == 3 ? tokens[1] : currentDirectory,
                                      currentDirectory);
        findEntries(filesystem, path, tokens[tokens.size() - 1]);
    } else if (command == "cp") {
        requireCount(tokens, 3, "cp SOURCE DEST");
        const auto source = resolvePath(tokens[1], currentDirectory);
        const auto destination = resolvePath(tokens[2], currentDirectory);
        if (filesystem.stat(source).directory) throw std::runtime_error("cp supports files only");
        const auto contents = filesystem.readFile(source);
        filesystem.createFile(destination);
        try {
            filesystem.writeFile(destination, contents);
        } catch (...) {
            filesystem.removePath(destination);
            throw;
        }
        std::cout << "copied " << source << " to " << destination << '\n';
    } else if (command == "rmdir") {
        requireCount(tokens, 2, "rmdir PATH");
        const auto path = resolvePath(tokens[1], currentDirectory);
        if (!filesystem.stat(path).directory) throw std::runtime_error("not a directory: " + path);
        if (!filesystem.list(path).empty()) throw std::runtime_error("directory is not empty: " + path);
        filesystem.removePath(path);
        if (currentDirectory == path || currentDirectory.compare(0, path.size() + 1, path + "/") == 0)
            currentDirectory = "/";
    } else if (command == "stat") {
        requireCount(tokens, 2, "stat PATH");
        printEntry(filesystem.stat(resolvePath(tokens[1], currentDirectory)));
    } else if (command == "mv") {
        requireCount(tokens, 3, "mv SOURCE DESTINATION");
        filesystem.renamePath(resolvePath(tokens[1], currentDirectory),
                              resolvePath(tokens[2], currentDirectory));
        std::cout << "entry moved\n";
    } else if (command == "rm") {
        if (tokens.size() == 2) {
            const auto path = resolvePath(tokens[1], currentDirectory);
            if (filesystem.stat(path).directory)
                throw std::runtime_error("refusing to remove a directory without -r");
            filesystem.removePath(path);
        } else if (tokens.size() == 3 && tokens[1] == "-r") {
            const auto path = resolvePath(tokens[2], currentDirectory);
            filesystem.removePath(path);
            if (currentDirectory == path || currentDirectory.compare(0, path.size() + 1, path + "/") == 0)
                currentDirectory = "/";
        } else {
            throw std::invalid_argument("usage: rm PATH | rm -r PATH");
        }
        std::cout << "entry removed\n";
    } else if (command == "chmod") {
        requireCount(tokens, 3, "chmod PATH rwx");
        filesystem.chmod(resolvePath(tokens[1], currentDirectory), parsePermissions(tokens[2]));
        std::cout << "permissions updated\n";
    } else if (command == "access") {
        requireCount(tokens, 3, "access PATH r|w|x");
        const auto path = resolvePath(tokens[1], currentDirectory);
        if (tokens[2] == "r") filesystem.checkAccess(path, 4);
        else if (tokens[2] == "w") filesystem.checkAccess(path, 2);
        else if (tokens[2] == "x") filesystem.checkAccess(path, 1);
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
    } else if (command == "man") {
        if (tokens.size() > 2) throw std::invalid_argument("usage: man [COMMAND]");
        showMan(tokens.size() == 2 ? tokens[1] : "");
    } else if (command == "clear") {
        requireCount(tokens, 1, "clear");
        std::cout << "\033[2J\033[H" << std::flush;
    } else if (command == "crash-write") {
        requireCount(tokens, 3, "crash-write PATH \"TEXT\"");
        filesystem.simulateCrashWrite(resolvePath(tokens[1], currentDirectory), tokens[2]);
    } else if (command == "exit" || command == "quit") {
        requireCount(tokens, 1, "exit");
        throw std::runtime_error("__VFS_EXIT__");
    } else {
        throw std::invalid_argument("unknown command: " + command + " (type help)");
    }
}

void executeWithRedirection(vfs::VirtualFileSystem& filesystem,
                            const std::vector<std::string>& tokens,
                            std::string& currentDirectory) {
    const auto redirect = std::find_if(tokens.begin(), tokens.end(), [](const std::string& token) {
        return token == ">" || token == ">>";
    });
    if (redirect == tokens.end()) {
        execute(filesystem, tokens, currentDirectory);
        return;
    }
    const auto position = static_cast<std::size_t>(std::distance(tokens.begin(), redirect));
    if (position == 0 || position + 2 != tokens.size())
        throw std::invalid_argument("redirection usage: COMMAND > PATH or COMMAND >> PATH");
    const bool append = *redirect == ">>";
    const auto destination = resolvePath(tokens[position + 1], currentDirectory);
    std::vector<std::string> command(tokens.begin(), redirect);
    std::ostringstream captured;
    auto* previous = std::cout.rdbuf(captured.rdbuf());
    try {
        execute(filesystem, command, currentDirectory);
    } catch (...) {
        std::cout.rdbuf(previous);
        throw;
    }
    std::cout.rdbuf(previous);
    const auto output = captured.str();
    bool created = false;
    try {
        filesystem.createFile(destination);
        created = true;
    } catch (const std::runtime_error& error) {
        if (std::string(error.what()) != "path already exists") throw;
    }
    try {
        if (append && !created) filesystem.appendFile(destination, output);
        else filesystem.writeFile(destination, output);
    } catch (...) {
        if (created) filesystem.removePath(destination);
        throw;
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
        std::string currentDirectory = "/";
        std::cout << "Virtual File System (GNU GPL version 3; NO WARRANTY). "
                     "Type 'license' for license terms or 'help' for commands.\n";
        std::string line;
        while (std::cout << "vfs[" << filesystem.currentUser() << "]:" << currentDirectory << "> " &&
               std::getline(std::cin, line)) {
            try {
                executeWithRedirection(filesystem, tokenize(line), currentDirectory);
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
