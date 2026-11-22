#include "analytics.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <numeric>
#include <stdexcept>
#include <utility>

namespace vfs::analytics {
namespace {

void validateTable(const Table& table) {
    if (table.empty()) throw std::invalid_argument("CSV file has no header row");
    const auto width = table.front().size();
    if (width == 0) throw std::invalid_argument("CSV header has no columns");
    for (std::size_t row = 1; row < table.size(); ++row) {
        if (table[row].size() != width)
            throw std::invalid_argument("CSV rows have inconsistent column counts");
    }
}

bool parseNumber(const std::string& text, double& value) {
    if (text.empty()) return false;
    char* end = nullptr;
    value = std::strtod(text.c_str(), &end);
    return end == text.c_str() + text.size() && end != text.c_str() && std::isfinite(value);
}

} // namespace

Table parseCsv(const std::string& input) {
    Table table;
    std::vector<std::string> row;
    std::string field;
    bool quoted = false;
    bool afterQuote = false;
    for (std::size_t index = 0; index < input.size(); ++index) {
        const char character = input[index];
        if (quoted) {
            if (character == '"') {
                if (index + 1 < input.size() && input[index + 1] == '"') {
                    field.push_back('"');
                    ++index;
                } else {
                    quoted = false;
                    afterQuote = true;
                }
            } else {
                field.push_back(character);
            }
        } else if (afterQuote) {
            if (character == ',') {
                row.push_back(std::move(field));
                field.clear();
                afterQuote = false;
            } else if (character == '\n' || character == '\r') {
                row.push_back(std::move(field));
                field.clear();
                table.push_back(std::move(row));
                row.clear();
                afterQuote = false;
                if (character == '\r' && index + 1 < input.size() && input[index + 1] == '\n') ++index;
            } else {
                throw std::invalid_argument("unexpected character after closing CSV quote");
            }
        } else if (character == '"') {
            if (!field.empty()) throw std::invalid_argument("quote inside an unquoted CSV field");
            quoted = true;
        } else if (character == ',') {
            row.push_back(std::move(field));
            field.clear();
        } else if (character == '\n' || character == '\r') {
            row.push_back(std::move(field));
            field.clear();
            table.push_back(std::move(row));
            row.clear();
            if (character == '\r' && index + 1 < input.size() && input[index + 1] == '\n') ++index;
        } else {
            field.push_back(character);
        }
    }
    if (quoted) throw std::invalid_argument("unterminated quoted CSV field");
    if (afterQuote || !field.empty() || !row.empty()) {
        row.push_back(std::move(field));
        table.push_back(std::move(row));
    }
    if (!table.empty()) validateTable(table);
    return table;
}

std::string writeCsv(const Table& table) {
    std::string output;
    for (const auto& row : table) {
        for (std::size_t column = 0; column < row.size(); ++column) {
            if (column != 0) output.push_back(',');
            const auto& field = row[column];
            if (field.find_first_of(",\"\r\n") == std::string::npos) {
                output += field;
                continue;
            }
            output.push_back('"');
            for (const char character : field) {
                if (character == '"') output.push_back('"');
                output.push_back(character);
            }
            output.push_back('"');
        }
        output.push_back('\n');
    }
    return output;
}

std::size_t columnIndex(const Table& table, const std::string& nameOrIndex) {
    validateTable(table);
    for (std::size_t index = 0; index < table.front().size(); ++index)
        if (table.front()[index] == nameOrIndex) return index;
    std::size_t numeric = 0;
    bool isNumeric = !nameOrIndex.empty();
    for (const char character : nameOrIndex) {
        if (character < '0' || character > '9') { isNumeric = false; break; }
        numeric = numeric * 10 + static_cast<std::size_t>(character - '0');
    }
    if (isNumeric && numeric > 0 && numeric <= table.front().size()) return numeric - 1;
    throw std::invalid_argument("unknown CSV column: " + nameOrIndex);
}

Table selectColumns(const Table& table, const std::vector<std::size_t>& columns) {
    validateTable(table);
    if (columns.empty()) throw std::invalid_argument("select requires at least one column");
    Table result;
    result.reserve(table.size());
    for (const auto& row : table) {
        std::vector<std::string> selected;
        selected.reserve(columns.size());
        for (const auto column : columns) {
            if (column >= row.size()) throw std::invalid_argument("CSV column index is out of range");
            selected.push_back(row[column]);
        }
        result.push_back(std::move(selected));
    }
    return result;
}

Table filterRows(const Table& table, std::size_t column,
                 const std::string& operation, const std::string& value) {
    validateTable(table);
    if (column >= table.front().size()) throw std::invalid_argument("CSV column index is out of range");
    const std::vector<std::string> operations{"=", "==", "!=", ">", ">=", "<", "<=", "contains"};
    if (std::find(operations.begin(), operations.end(), operation) == operations.end())
        throw std::invalid_argument("filter operator must be =, !=, >, >=, <, <=, or contains");
    Table result{table.front()};
    double expected = 0;
    const bool expectedNumeric = parseNumber(value, expected);
    for (std::size_t index = 1; index < table.size(); ++index) {
        const auto& actual = table[index][column];
        double actualNumeric = 0;
        const bool actualIsNumeric = parseNumber(actual, actualNumeric);
        bool match = false;
        if (operation == "contains") match = actual.find(value) != std::string::npos;
        else if (operation == "=" || operation == "==") match = actual == value;
        else if (operation == "!=") match = actual != value;
        else if (expectedNumeric && actualIsNumeric) {
            if (operation == ">") match = actualNumeric > expected;
            else if (operation == ">=") match = actualNumeric >= expected;
            else if (operation == "<") match = actualNumeric < expected;
            else if (operation == "<=") match = actualNumeric <= expected;
        } else {
            if (operation == ">") match = actual > value;
            else if (operation == ">=") match = actual >= value;
            else if (operation == "<") match = actual < value;
            else if (operation == "<=") match = actual <= value;
        }
        if (match) result.push_back(table[index]);
    }
    return result;
}

NumericSummary summarizeNumeric(const Table& table, std::size_t column) {
    validateTable(table);
    if (column >= table.front().size()) throw std::invalid_argument("CSV column index is out of range");
    std::vector<double> values;
    for (std::size_t row = 1; row < table.size(); ++row) {
        const auto& text = table[row][column];
        if (text.empty()) continue;
        double value = 0;
        if (!parseNumber(text, value))
            throw std::invalid_argument("column '" + table.front()[column] + "' contains non-numeric data");
        values.push_back(value);
    }
    if (values.empty()) throw std::invalid_argument("column '" + table.front()[column] + "' has no numeric values");
    NumericSummary summary;
    summary.count = values.size();
    const auto bounds = std::minmax_element(values.begin(), values.end());
    summary.minimum = *bounds.first;
    summary.maximum = *bounds.second;
    summary.mean = std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
    std::sort(values.begin(), values.end());
    const auto middle = values.size() / 2;
    summary.median = values.size() % 2 == 0
        ? (values[middle - 1] + values[middle]) / 2.0 : values[middle];
    double squaredDeviation = 0;
    for (const auto value : values) {
        const auto difference = value - summary.mean;
        squaredDeviation += difference * difference;
    }
    summary.standardDeviation = std::sqrt(squaredDeviation / static_cast<double>(values.size()));
    return summary;
}

std::vector<std::pair<std::string, std::size_t>> frequencies(
    const Table& table, std::size_t column) {
    validateTable(table);
    if (column >= table.front().size()) throw std::invalid_argument("CSV column index is out of range");
    std::map<std::string, std::size_t> counts;
    for (std::size_t row = 1; row < table.size(); ++row) ++counts[table[row][column]];
    std::vector<std::pair<std::string, std::size_t>> result(counts.begin(), counts.end());
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.second == right.second ? left.first < right.first : left.second > right.second;
    });
    return result;
}

std::vector<GroupSummary> groupCounts(const Table& table, std::size_t keyColumn) {
    validateTable(table);
    if (keyColumn >= table.front().size()) throw std::invalid_argument("CSV column index is out of range");
    std::map<std::string, std::size_t> counts;
    for (std::size_t row = 1; row < table.size(); ++row) ++counts[table[row][keyColumn]];
    std::vector<GroupSummary> result;
    for (const auto& item : counts) result.push_back({item.first, item.second, 0, 0});
    return result;
}

std::vector<GroupSummary> groupNumeric(const Table& table, std::size_t keyColumn,
                                       std::size_t valueColumn) {
    validateTable(table);
    if (keyColumn >= table.front().size() || valueColumn >= table.front().size())
        throw std::invalid_argument("CSV column index is out of range");
    std::map<std::string, GroupSummary> groups;
    for (std::size_t row = 1; row < table.size(); ++row) {
        const auto& text = table[row][valueColumn];
        if (text.empty()) continue;
        double value = 0;
        if (!parseNumber(text, value))
            throw std::invalid_argument("column '" + table.front()[valueColumn] + "' contains non-numeric data");
        auto& group = groups[table[row][keyColumn]];
        group.key = table[row][keyColumn];
        ++group.count;
        group.sum += value;
    }
    std::vector<GroupSummary> result;
    result.reserve(groups.size());
    for (auto& item : groups) {
        item.second.mean = item.second.count == 0 ? 0
            : item.second.sum / static_cast<double>(item.second.count);
        result.push_back(item.second);
    }
    return result;
}

Table innerJoin(const Table& left, const Table& right,
                std::size_t leftKey, std::size_t rightKey) {
    validateTable(left);
    validateTable(right);
    if (leftKey >= left.front().size() || rightKey >= right.front().size())
        throw std::invalid_argument("CSV column index is out of range");
    Table result;
    result.push_back(left.front());
    for (std::size_t column = 0; column < right.front().size(); ++column)
        if (column != rightKey) result.front().push_back(right.front()[column]);
    for (std::size_t leftRow = 1; leftRow < left.size(); ++leftRow) {
        for (std::size_t rightRow = 1; rightRow < right.size(); ++rightRow) {
            if (left[leftRow][leftKey] != right[rightRow][rightKey]) continue;
            auto joined = left[leftRow];
            for (std::size_t column = 0; column < right[rightRow].size(); ++column)
                if (column != rightKey) joined.push_back(right[rightRow][column]);
            result.push_back(std::move(joined));
        }
    }
    return result;
}

std::vector<HistogramBin> histogram(const Table& table, std::size_t column,
                                    std::size_t binCount) {
    validateTable(table);
    if (column >= table.front().size()) throw std::invalid_argument("CSV column index is out of range");
    if (binCount == 0 || binCount > 1000)
        throw std::invalid_argument("histogram bin count must be between 1 and 1000");
    std::vector<double> values;
    for (std::size_t row = 1; row < table.size(); ++row) {
        if (table[row][column].empty()) continue;
        double value = 0;
        if (!parseNumber(table[row][column], value))
            throw std::invalid_argument("column '" + table.front()[column] + "' contains non-numeric data");
        values.push_back(value);
    }
    if (values.empty()) throw std::invalid_argument("column '" + table.front()[column] + "' has no numeric values");
    const auto bounds = std::minmax_element(values.begin(), values.end());
    const double minimum = *bounds.first;
    const double maximum = *bounds.second;
    if (minimum == maximum) return {{minimum, maximum, values.size()}};
    std::vector<HistogramBin> bins(binCount);
    const double width = (maximum - minimum) / static_cast<double>(binCount);
    for (std::size_t index = 0; index < binCount; ++index) {
        bins[index].lower = minimum + width * static_cast<double>(index);
        bins[index].upper = index + 1 == binCount ? maximum
            : minimum + width * static_cast<double>(index + 1);
    }
    for (const auto value : values) {
        const auto position = (value - minimum) / width;
        const auto index = position >= static_cast<double>(binCount)
            ? binCount - 1 : static_cast<std::size_t>(position);
        ++bins[index].count;
    }
    return bins;
}


double correlation(const Table& table, std::size_t xColumn, std::size_t yColumn) {
    validateTable(table);
    if (xColumn >= table.front().size() || yColumn >= table.front().size())
        throw std::invalid_argument("CSV column index is out of range");
    std::vector<std::pair<double, double>> values;
    for (std::size_t row = 1; row < table.size(); ++row) {
        const auto& xText = table[row][xColumn];
        const auto& yText = table[row][yColumn];
        if (xText.empty() || yText.empty()) continue;
        double x = 0, y = 0;
        if (!parseNumber(xText, x) || !parseNumber(yText, y))
            throw std::invalid_argument("correlation columns must contain numeric values or blanks");
        values.emplace_back(x, y);
    }
    if (values.size() < 2) throw std::invalid_argument("correlation requires at least two numeric row pairs");
    double meanX = 0, meanY = 0;
    for (const auto& pair : values) { meanX += pair.first; meanY += pair.second; }
    meanX /= static_cast<double>(values.size()); meanY /= static_cast<double>(values.size());
    double covariance = 0, varianceX = 0, varianceY = 0;
    for (const auto& pair : values) {
        const double dx = pair.first - meanX, dy = pair.second - meanY;
        covariance += dx * dy; varianceX += dx * dx; varianceY += dy * dy;
    }
    if (varianceX == 0 || varianceY == 0) throw std::invalid_argument("correlation is undefined for a constant column");
    return covariance / std::sqrt(varianceX * varianceY);
}

LinearRegression regress(const Table& table, std::size_t xColumn, std::size_t yColumn) {
    validateTable(table);
    if (xColumn >= table.front().size() || yColumn >= table.front().size())
        throw std::invalid_argument("CSV column index is out of range");
    std::vector<std::pair<double, double>> values;
    for (std::size_t row = 1; row < table.size(); ++row) {
        const auto& xText = table[row][xColumn];
        const auto& yText = table[row][yColumn];
        if (xText.empty() || yText.empty()) continue;
        double x = 0, y = 0;
        if (!parseNumber(xText, x) || !parseNumber(yText, y))
            throw std::invalid_argument("regression columns must contain numeric values or blanks");
        values.emplace_back(x, y);
    }
    if (values.size() < 2) throw std::invalid_argument("regression requires at least two numeric row pairs");
    double meanX = 0, meanY = 0;
    for (const auto& pair : values) { meanX += pair.first; meanY += pair.second; }
    meanX /= static_cast<double>(values.size()); meanY /= static_cast<double>(values.size());
    double covariance = 0, varianceX = 0, varianceY = 0;
    for (const auto& pair : values) {
        const double dx = pair.first - meanX, dy = pair.second - meanY;
        covariance += dx * dy; varianceX += dx * dx; varianceY += dy * dy;
    }
    if (varianceX == 0) throw std::invalid_argument("regression is undefined for a constant predictor");
    LinearRegression model;
    model.count = values.size(); model.slope = covariance / varianceX;
    model.intercept = meanY - model.slope * meanX;
    model.rSquared = varianceY == 0 ? 1.0 : (covariance * covariance) / (varianceX * varianceY);
    return model;
}

Table dropMissing(const Table& table, const std::vector<std::size_t>& columns) {
    validateTable(table);
    if (columns.empty()) throw std::invalid_argument("select at least one column for missing-value cleanup");
    for (const auto column : columns)
        if (column >= table.front().size()) throw std::invalid_argument("CSV column index is out of range");
    Table result{table.front()};
    for (std::size_t row = 1; row < table.size(); ++row) {
        bool complete = true;
        for (const auto column : columns) if (table[row][column].empty()) { complete = false; break; }
        if (complete) result.push_back(table[row]);
    }
    return result;
}

} // namespace vfs::analytics
