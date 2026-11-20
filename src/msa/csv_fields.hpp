#pragma once
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace msa {
class CsvReader {
  public:
    explicit CsvReader(const std::string &bytes);
    bool next();
    double measurement(std::size_t column) const;
    int index(std::size_t column, int maximum, const std::string &name) const;
    std::size_t row = 1;
    std::vector<std::string> cells;
  private:
    std::string_view text;
    std::size_t offset = 0, line = 1;
};
std::string csv_cell(const std::string &s);
} // namespace msa
