#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace vfs::analytics {

using Table = std::vector<std::vector<std::string>>;

struct NumericSummary {
    std::size_t count = 0;
    double minimum = 0;
    double maximum = 0;
    double mean = 0;
    double median = 0;
    double standardDeviation = 0;
};

struct GroupSummary {
    std::string key;
    std::size_t count = 0;
    double sum = 0;
    double mean = 0;
};

struct HistogramBin {
    double lower = 0;
    double upper = 0;
    std::size_t count = 0;
};

Table parseCsv(const std::string& input);
std::string writeCsv(const Table& table);
std::size_t columnIndex(const Table& table, const std::string& nameOrIndex);
Table selectColumns(const Table& table, const std::vector<std::size_t>& columns);
Table filterRows(const Table& table, std::size_t column,
                 const std::string& operation, const std::string& value);
NumericSummary summarizeNumeric(const Table& table, std::size_t column);
std::vector<std::pair<std::string, std::size_t>> frequencies(
    const Table& table, std::size_t column);
std::vector<GroupSummary> groupCounts(const Table& table, std::size_t keyColumn);
std::vector<GroupSummary> groupNumeric(const Table& table, std::size_t keyColumn,
                                       std::size_t valueColumn);
Table innerJoin(const Table& left, const Table& right,
                std::size_t leftKey, std::size_t rightKey);
std::vector<HistogramBin> histogram(const Table& table, std::size_t column,
                                    std::size_t binCount);

} // namespace vfs::analytics
