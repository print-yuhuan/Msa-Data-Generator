#pragma once
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace msa {
inline constexpr std::size_t xlsx_read_limit = 16 * 1024 * 1024;
class XlsxWorkbook {
  public:
    explicit XlsxWorkbook(const std::string &bytes);
    ~XlsxWorkbook();
    const std::vector<std::string> &sheet_names() const;
    std::string csv(std::size_t index) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
std::string xlsx_from_csv(const std::string &canonicalCsv, int decimalPlaces);
std::string csv_from_xlsx(const std::string &bytes);
} // namespace msa
