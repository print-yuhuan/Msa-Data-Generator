#include "core.hpp"
#include <algorithm>
#include <string_view>

namespace msa {
namespace {
std::string location(std::size_t row, std::size_t column) {
    return "数据第 " + std::to_string(row) + " 行，第 " + std::to_string(column + 1) + " 列";
}
std::string trim_field(const std::string &value) {
    const auto start = value.find_first_not_of(" \t");
    return start == value.npos ? ""
                               : value.substr(start, value.find_last_not_of(" \t") - start + 1);
}
class CsvReader {
  public:
    explicit CsvReader(const std::string &bytes) : text(bytes) {
        if (text.substr(0, 3) == "\xef\xbb\xbf")
            text.remove_prefix(3);
    }
    std::size_t row = 1;
    std::vector<std::string> cells;

    bool next() {
        if (offset == text.size())
            return false;
        row = line;
        cells.clear();
        for (;;) {
            std::string cell;
            if (offset < text.size() && text[offset] == '"') {
                ++offset;
                for (;;) {
                    if (offset == text.size())
                        throw ConfigError(location(row, cells.size()) + "：引号未闭合");
                    char c = text[offset++];
                    if (c == '"') {
                        if (offset == text.size() || text[offset] != '"')
                            break;
                        ++offset;
                    }
                    if (c == '\n' || (c == '\r' && (offset == text.size() || text[offset] != '\n')))
                        ++line;
                    cell += c;
                }
                while (offset < text.size() && (text[offset] == ' ' || text[offset] == '\t'))
                    ++offset;
            } else {
                const auto start = offset;
                while (offset < text.size() && text[offset] != ',' && text[offset] != '\n' &&
                       text[offset] != '\r' && text[offset] != '"')
                    ++offset;
                cell.assign(text.substr(start, offset - start));
            }
            cells.push_back(trim_field(cell));
            if (offset == text.size())
                return true;
            const auto separator = text[offset++];
            if (separator == ',')
                continue;
            if (separator != '\n' && separator != '\r')
                throw ConfigError(location(row, cells.size() - 1) + "：字段引号或分隔符无效");
            if (separator == '\r' && offset < text.size() && text[offset] == '\n')
                ++offset;
            ++line;
            return true;
        }
    }

    double measurement(std::size_t column) const {
        try {
            return parse_number(cells[column]);
        } catch (const ConfigError &) {
            throw ConfigError(location(row, column) + "：测量值必须是有限数字，收到「" +
                              cells[column].substr(0, 80) + "」");
        }
    }

    int index(std::size_t column, int maximum, const std::string &name) const {
        std::int64_t value = 0;
        try {
            value = parse_integer(cells[column]);
        } catch (const ConfigError &) {
            // The field-specific diagnostic below includes the expected range.
        }
        if (value < 1 || value > maximum)
            throw ConfigError(location(row, column) + "：" + name + " 须为 1～" +
                              std::to_string(maximum) + " 的整数，收到「" +
                              cells[column].substr(0, 80) + "」");
        return static_cast<int>(value);
    }

  private:
    std::string_view text;
    std::size_t offset = 0, line = 1;
};
void check_width(const CsvReader &reader, std::size_t expected) {
    if (reader.cells.size() != expected)
        throw ConfigError(location(reader.row, std::min(expected, reader.cells.size())) +
                          "：应有 " + std::to_string(expected) + " 列，实际 " +
                          std::to_string(reader.cells.size()) + " 列");
}
Matrix wide(CsvReader &reader, const Json *design) {
    const bool labeled = reader.cells.front().empty();
    const auto parts = labeled ? reader.cells.size() - 1 : reader.cells.size();
    std::vector<int> operator_rows;
    Matrix matrix;
    const auto expected_rows = design ? design->at("operators").get<std::size_t>() *
                                            design->at("replicates").get<std::size_t>()
                                      : 0;
    if (design && parts != design->at("parts").get<std::size_t>())
        throw ConfigError("数据第 1 行：实际 " + std::to_string(parts) + " 个部件，研究设计要求 " +
                          design->at("parts").dump() + " 个");
    if (labeled) {
        for (std::size_t col = 1; col < reader.cells.size(); ++col)
            if (reader.cells[col] != std::to_string(col))
                throw ConfigError(location(reader.row, col) + "：宽表列号须从 1 开始连续排列");
        if (!reader.next())
            throw ConfigError("宽表缺少测量记录");
    }
    do {
        if (design && matrix.size() >= expected_rows)
            throw ConfigError("数据第 " + std::to_string(reader.row) + " 行：已有 " +
                              std::to_string(matrix.size() + 1) + " 行数据，研究设计要求 " +
                              std::to_string(expected_rows) + " 行");
        if (labeled || design)
            check_width(reader, parts + (labeled ? 1 : 0));
        if (labeled) {
            const auto &label = reader.cells.front();
            if (label.size() != 1 || label.front() < 'A' || label.front() > 'Z')
                throw ConfigError(location(reader.row, 0) + "：宽表人员标记无效");
            const auto op = static_cast<std::size_t>(label.front() - 'A');
            if (design && op != matrix.size() / design->at("replicates").get<std::size_t>())
                throw ConfigError(
                    location(reader.row, 0) + "：实际人员为 " + label + "，研究设计要求 " +
                    char('A' + matrix.size() / design->at("replicates").get<std::size_t>()));
            if (op == operator_rows.size())
                operator_rows.push_back(0);
            else if (op + 1 != operator_rows.size())
                throw ConfigError(location(reader.row, 0) + "：人员须从 A 开始分组连续排列");
            ++operator_rows.back();
        }
        std::vector<double> values;
        for (std::size_t col = labeled ? 1 : 0; col < reader.cells.size(); ++col)
            values.push_back(reader.measurement(col));
        matrix.push_back(std::move(values));
    } while (reader.next());
    if (labeled && !std::all_of(operator_rows.begin(), operator_rows.end(),
                                [&](int n) { return n == operator_rows.front(); }))
        throw ConfigError("宽表各人员的重复测量次数不一致");
    if (design && matrix.size() != expected_rows)
        throw ConfigError("宽表共 " + std::to_string(matrix.size()) +
                          " 行数据，研究设计要求 " + std::to_string(expected_rows) + " 行");
    return matrix;
}
constexpr std::array<std::string_view, 5> names{"Operator", "Part", "Trial", "Measurement",
                                                "StdOrder"};
Matrix long_table(CsvReader &reader, const Json &design) {
    const auto width = reader.cells.size();
    std::array<std::size_t, 5> columns;
    columns.fill(width);
    for (std::size_t col = 0; col < width; ++col) {
        const auto found = std::find(names.begin(), names.end(), reader.cells[col]);
        if (found == names.end())
            continue;
        auto &index = columns[found - names.begin()];
        if (index != width)
            throw ConfigError(location(reader.row, col) + "：重复列名 " + reader.cells[col]);
        index = col;
    }
    for (std::size_t key = 0; key < 4; ++key)
        if (columns[key] == width)
            throw ConfigError("数据第 1 行缺少必需列 " + std::string(names[key]));
    const int ops = design.at("operators"), trials = design.at("replicates"),
              parts = design.at("parts");
    Matrix matrix(ops * trials, std::vector<double>(parts));
    std::vector<bool> seen(ops * trials * parts);
    while (reader.next()) {
        check_width(reader, width);
        const auto &label = reader.cells[columns[0]];
        if (label.size() != 1 || label[0] < 'A' || label[0] >= 'A' + ops)
            throw ConfigError(location(reader.row, columns[0]) + "：Operator 须为 A～" +
                              char('A' + ops - 1) + "，收到「" + label.substr(0, 80) + "」");
        const int op = label[0] - 'A';
        const int part = reader.index(columns[1], parts, "Part") - 1;
        const int trial = reader.index(columns[2], trials, "Trial") - 1;
        const int index = (trial * parts + part) * ops + op;
        if (seen[index])
            throw ConfigError(location(reader.row, columns[0]) +
                              "：重复的 Operator/Part/Trial 组合");
        if (columns[4] != width &&
            reader.index(columns[4], ops * trials * parts, "StdOrder") != index + 1)
            throw ConfigError(location(reader.row, columns[4]) + "：StdOrder 与测量记录不对应");
        seen[index] = true;
        matrix[op * trials + trial][part] = reader.measurement(columns[3]);
    }
    const auto missing = std::find(seen.begin(), seen.end(), false);
    if (missing != seen.end()) {
        const auto index = missing - seen.begin();
        throw ConfigError("长表缺少记录：Operator=" + std::string(1, char('A' + index % ops)) +
                          "，Part=" + std::to_string(index / ops % parts + 1) +
                          "，Trial=" + std::to_string(index / ops / parts + 1));
    }
    return matrix;
}
} // namespace
Matrix parse_csv(const std::string &bytes) {
    CsvReader reader(bytes);
    return reader.next() ? wide(reader, nullptr) : Matrix{};
}
Matrix parse_export_csv(const std::string &bytes, const Json &raw) {
    CsvReader reader(bytes);
    if (!reader.next())
        throw ConfigError("数据文件为空");
    const auto design = normalize_config(raw).at("design");
    if (std::any_of(reader.cells.begin(), reader.cells.end(), [](const auto &name) {
            return std::find(names.begin(), names.end(), name) != names.end();
        }))
        return long_table(reader, design);
    return wide(reader, &design);
}
} // namespace msa
