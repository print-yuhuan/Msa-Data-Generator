#include "core.hpp"
#include "csv_fields.hpp"
#include <algorithm>
#include <map>
#include <string_view>
#include <utility>

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
} // namespace
CsvReader::CsvReader(const std::string &bytes) : text(bytes) {
    if (text.substr(0, 3) == "\xef\xbb\xbf")
        text.remove_prefix(3);
}
bool CsvReader::next() {
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
double CsvReader::measurement(std::size_t column) const {
    try {
        return parse_number(cells[column]);
    } catch (const ConfigError &) {
        throw ConfigError(location(row, column) + "：测量值必须是有限数字，收到「" +
                          cells[column].substr(0, 80) + "」");
    }
}

int CsvReader::index(std::size_t column, int maximum, const std::string &name) const {
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
namespace {
const auto legacy_names = operator_names(Json::object());
void check_width(const CsvReader &reader, std::size_t expected) {
    if (reader.cells.size() != expected)
        throw ConfigError(location(reader.row, std::min(expected, reader.cells.size())) +
                          "：应有 " + std::to_string(expected) + " 列，实际 " +
                          std::to_string(reader.cells.size()) + " 列");
}
Matrix wide(CsvReader &reader, const Json *design, std::vector<std::string> *file_names = nullptr) {
    const bool labeled = reader.cells.front().empty();
    const auto parts = labeled ? reader.cells.size() - 1 : reader.cells.size();
    std::map<std::string, std::size_t> group_sizes;
    std::string previous_label;
    const auto configured = design ? operator_names(*design) : legacy_names;
    bool matches_config = true, matches_legacy = true;
    Matrix matrix;
    const auto expected_rows = design ? design->at("operators").get<std::size_t>() *
                                            design->at("replicates").get<std::size_t>()
                                      : 0;
    if (design && parts != design->at("parts").get<std::size_t>())
        throw ConfigError("数据第 1 行：实际 " + std::to_string(parts) + " 个部件，研究设计要求 " +
                          design->at("parts").dump() + " 个");
    if (file_names && (parts < 5 || parts > 10))
        throw ConfigError("数据第 1 行：宽表须包含 5～10 个部件列");
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
        if (file_names && matrix.size() == 9)
            throw ConfigError(location(reader.row, 0) + "：宽表最多支持 3 人各重复测量 3 次");
        if (labeled || design || file_names)
            check_width(reader, parts + (labeled ? 1 : 0));
        if (labeled) {
            const auto &label = reader.cells.front();
            if (design) {
                const auto op = matrix.size() / design->at("replicates").get<std::size_t>();
                matches_config &= label == configured[op];
                matches_legacy &= label == legacy_names[op];
                if (!matches_config && !matches_legacy)
                    throw ConfigError(location(reader.row, 0) + "：实际操作员为「" + label +
                                      "」，与研究设计的操作员顺序不符");
            } else {
                if (label.empty() || (label != previous_label && group_sizes.count(label)))
                    throw ConfigError(location(reader.row, 0) + "：操作员名称须非空且分组连续排列");
                if (file_names && label != previous_label)
                    file_names->push_back(label);
                ++group_sizes[label];
                previous_label = label;
            }
        }
        std::vector<double> values;
        for (std::size_t col = labeled ? 1 : 0; col < reader.cells.size(); ++col)
            values.push_back(reader.measurement(col));
        matrix.push_back(std::move(values));
    } while (reader.next());
    if (!std::all_of(group_sizes.begin(), group_sizes.end(),
                     [&](const auto &group) { return group.second == group_sizes.begin()->second; }))
        throw ConfigError("宽表各人员的重复测量次数不一致");
    if (design && matrix.size() != expected_rows)
        throw ConfigError("宽表共 " + std::to_string(matrix.size()) +
                          " 行数据，研究设计要求 " + std::to_string(expected_rows) + " 行");
    return matrix;
}
constexpr std::array<std::string_view, 5> names{"Operator", "Part", "Trial", "Measurement",
                                                "StdOrder"};
bool is_long_header(const CsvReader &reader) {
    return std::any_of(reader.cells.begin(), reader.cells.end(), [](const auto &name) {
        return std::find(names.begin(), names.end(), name) != names.end();
    });
}
using Columns = std::array<std::size_t, 5>;
Columns long_columns(const CsvReader &reader) {
    const auto width = reader.cells.size();
    Columns columns;
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
    return columns;
}
Matrix long_table(CsvReader &reader, const Json &design, const Columns &columns) {
    const auto width = reader.cells.size();
    const int ops = design.at("operators"), trials = design.at("replicates"),
              parts = design.at("parts");
    const auto configured = operator_names(design);
    const int total = ops * trials * parts;
    struct Row {
        int configured_op, legacy_op, part, trial;
        double value;
        std::size_t line;
    };
    std::vector<Row> records;
    bool matches_config = true, matches_legacy = true;
    while (reader.next()) {
        check_width(reader, width);
        const auto &label = reader.cells[columns[0]];
        const auto locate = [&](const auto &labels) {
            return int(std::find(labels.begin(), labels.begin() + ops, label) - labels.begin());
        };
        const int configured_op = locate(configured), legacy_op = locate(legacy_names);
        matches_config &= configured_op < ops;
        matches_legacy &= legacy_op < ops;
        if (!matches_config && !matches_legacy)
            throw ConfigError(location(reader.row, columns[0]) +
                              "：Operator 与当前研究设计或旧版 A/B/C 标记不符，收到「" +
                              label.substr(0, 80) + "」");
        const int part = reader.index(columns[1], parts, "Part") - 1;
        const int trial = reader.index(columns[2], trials, "Trial") - 1;
        if (columns[4] != width) {
            const int order = reader.index(columns[4], total, "StdOrder") - 1;
            const int base = (trial * parts + part) * ops;
            matches_config &= order == base + configured_op;
            matches_legacy &= order == base + legacy_op;
            if (!matches_config && !matches_legacy)
                throw ConfigError(location(reader.row, columns[4]) + "：StdOrder 与测量记录不对应");
        }
        if (records.size() >= static_cast<std::size_t>(total))
            throw ConfigError(location(reader.row, columns[0]) + "：测量记录数超过研究设计");
        records.push_back({configured_op, legacy_op, part, trial, reader.measurement(columns[3]),
                           reader.row});
    }
    // A custom name can itself be another operator's legacy letter. Use one mapping
    // for the entire table, including any available StdOrder identities.
    Matrix matrix(ops * trials, std::vector<double>(parts));
    std::vector<bool> seen(total);
    for (const auto &record : records) {
        const int op = matches_config ? record.configured_op : record.legacy_op;
        const int index = (record.trial * parts + record.part) * ops + op;
        if (seen[index])
            throw ConfigError(location(record.line, columns[0]) +
                              "：重复的 Operator/Part/Trial 组合");
        seen[index] = true;
        matrix[op * trials + record.trial][record.part] = record.value;
    }
    const auto missing = std::find(seen.begin(), seen.end(), false);
    if (missing != seen.end()) {
        const auto index = missing - seen.begin();
        throw ConfigError("长表缺少记录：Operator=" +
                          (matches_config ? configured : legacy_names)[index % ops] +
                          "，Part=" + std::to_string(index / ops % parts + 1) +
                          "，Trial=" + std::to_string(index / ops / parts + 1));
    }
    return matrix;
}
struct FileDesign {
    std::vector<std::string> operators;
    int parts = 0, trials = 0;
};
FileDesign detect_long_design(CsvReader &reader, const Columns &columns) {
    FileDesign result;
    const auto width = reader.cells.size();
    std::array<int, 3> first_orders{};
    std::size_t records = 0;
    while (reader.next()) {
        check_width(reader, width);
        if (++records > 90)
            throw ConfigError(location(reader.row, columns[0]) + "：长表最多支持 90 次测量");
        const auto &label = reader.cells[columns[0]];
        if (label.empty())
            throw ConfigError(location(reader.row, columns[0]) + "：Operator 名称不能为空");
        const auto known = std::find(result.operators.begin(), result.operators.end(), label);
        if (known == result.operators.end()) {
            if (result.operators.size() == 3)
                throw ConfigError(location(reader.row, columns[0]) + "：长表最多支持 3 名操作员");
            if (columns[4] < width)
                first_orders[result.operators.size()] = reader.index(columns[4], 90, "StdOrder") - 1;
            result.operators.push_back(label);
        }
        result.parts = std::max(result.parts, reader.index(columns[1], 10, "Part"));
        result.trials = std::max(result.trials, reader.index(columns[2], 3, "Trial"));
    }
    if (result.operators.size() < 2 || result.parts < 5 || result.trials < 2)
        throw ConfigError("长表须包含 2～3 名操作员、5～10 个部件和每组 2～3 次重复测量");
    if (columns[4] < width) {
        // Once the operator count is known, the remainder of each zero-based StdOrder
        // identifies the operator slot. The matrix parser checks every record against it.
        const auto count = result.operators.size();
        std::vector<std::string> ordered(count);
        for (std::size_t i = 0; i < count; ++i) {
            auto &slot = ordered[first_orders[i] % count];
            if (!slot.empty())
                throw ConfigError("长表 StdOrder 将不同操作员分配到同一人员编号");
            slot = result.operators[i];
        }
        result.operators = std::move(ordered);
    }
    return result;
}
std::optional<int> numeric_wide_trials(std::size_t rows, const ReplicateSelector &choose_replicates) {
    if (rows == 4)
        return 2;
    if (rows == 9)
        return 3;
    if (rows != 6)
        throw ConfigError("纯数值宽表须有 4、6 或 9 行，才能组成两人或三人的平衡研究");
    if (!choose_replicates)
        throw ConfigError("6 行无操作员名称的纯数值宽表有两种研究设计。请指定 "
                          "--replicates 3（2 人各测 3 次）或 --replicates 2（3 人各测 2 次）。");
    const auto trials = choose_replicates();
    if (trials && *trials != 2 && *trials != 3)
        throw ConfigError("6 行纯数值宽表的重复测量次数须为 2 或 3");
    return trials;
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
    if (is_long_header(reader))
        return long_table(reader, design, long_columns(reader));
    return wide(reader, &design);
}
ImportedData import_csv_study(const std::string &bytes, const Json &raw) {
    return import_csv_study(bytes, raw, {}).value();
}
std::optional<ImportedData> import_csv_study(const std::string &bytes, const Json &raw,
                                             const ReplicateSelector &choose_replicates) {
    CsvReader reader(bytes);
    if (!reader.next())
        throw ConfigError("数据文件为空");
    // A named first column followed by part numbers is also a labeled wide table.
    if ((reader.cells.front() == "Operator" || reader.cells.front() == "操作员") &&
        reader.cells.size() > 1) {
        std::size_t col = 1;
        while (col < reader.cells.size() && reader.cells[col] == std::to_string(col))
            ++col;
        if (col == reader.cells.size())
            reader.cells.front().clear();
    }
    if (!raw.is_object() || (raw.contains("design") && !raw.at("design").is_object()))
        throw ConfigError("配置及 design 必须是 JSON 对象");
    Json config = raw;
    if (!config.contains("design"))
        config["design"] = Json::object();
    auto &design = config.at("design");
    FileDesign file;
    Matrix matrix;
    const bool is_long = is_long_header(reader);
    Columns columns{};
    if (is_long) {
        columns = long_columns(reader);
        file = detect_long_design(reader, columns);
    } else {
        matrix = wide(reader, nullptr, &file.operators);
        file.parts = static_cast<int>(matrix.front().size());
        if (file.operators.empty()) {
            const auto trials = numeric_wide_trials(matrix.size(), choose_replicates);
            if (!trials)
                return std::nullopt;
            file.trials = *trials;
            const auto count = matrix.size() / file.trials;
            file.operators.assign(legacy_names.begin(), legacy_names.begin() + count);
        } else {
            if (file.operators.size() < 2 || file.operators.size() > 3)
                throw ConfigError("宽表须包含 2～3 名不同的操作员");
            file.trials = static_cast<int>(matrix.size() / file.operators.size());
            if (file.trials < 2 || file.trials > 3)
                throw ConfigError("宽表每名操作员须有 2～3 行重复测量");
        }
    }
    design["operators"] = file.operators.size();
    design["parts"] = file.parts;
    design["replicates"] = file.trials;
    file.operators.resize(3);
    design["operator_names"] = file.operators;
    // File labels become JSON report metadata. Reject malformed input before exposing the study.
    try {
        design.at("operator_names").dump();
    } catch (const Json::type_error &) {
        throw ConfigError("操作员名称含无效 UTF-8 字符，请将数据文件另存为 UTF-8 CSV 或 XLSX");
    }
    config = normalize_config(config);
    if (is_long) {
        CsvReader records(bytes);
        records.next();
        matrix = long_table(records, config.at("design"), columns);
    }
    return ImportedData{std::move(matrix), {}, std::move(config)};
}
} // namespace msa
