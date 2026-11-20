#include "xlsx.hpp"
#include "xlsx_package.hpp"
#include "core.hpp"
#include "csv_fields.hpp"
#include <algorithm>
#include <charconv>
#include <set>
#include <string_view>

namespace msa {
namespace {
using xlsx::Node;
constexpr std::size_t max_rows = 4096, max_columns = 512, max_cells = 250000;
using Rows = std::vector<std::vector<std::string>>;
constexpr char spreadsheet[] = "http://schemas.openxmlformats.org/spreadsheetml/2006/main";
constexpr char office_rel[] =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/";
constexpr char strict_rel[] = "http://purl.oclc.org/ooxml/officeDocument/relationships/";
constexpr char package_rel[] = "http://schemas.openxmlformats.org/package/2006/relationships";
std::string escaped(const std::string &s) {
    std::string result;
    for (const char c : s) {
        if (c == '&')
            result += "&amp;";
        else if (c == '<')
            result += "&lt;";
        else if (c == '>')
            result += "&gt;";
        else
            result += c;
    }
    return result;
}
std::string column_name(std::size_t col) {
    std::string name;
    while (col) {
        --col;
        name += char('A' + col % 26);
        col /= 26;
    }
    return std::string(name.rbegin(), name.rend());
}
Rows canonical_rows(const std::string &csv) {
    if (csv.size() > csv_read_limit)
        throw ConfigError("待导出的表格过大");
    Rows rows;
    CsvReader reader(csv);
    while (reader.next()) {
        const auto width = reader.cells.size();
        if (width > max_columns || rows.size() >= max_rows ||
            (!rows.empty() && width != rows.front().size()) || (rows.size() + 1) * width > max_cells)
            throw ConfigError("待导出的工作表尺寸无效");
        rows.push_back(std::move(reader.cells));
    }
    if (rows.empty())
        throw ConfigError("待导出的工作表为空");
    return rows;
}
struct Relationship {
    std::string id, type, target;
    bool external;
    bool is(const char *kind) const {
        return type == std::string(office_rel) + kind || type == std::string(strict_rel) + kind;
    }
};
std::vector<Relationship> links(const xlsx::Package &package, const std::string &source) {
    std::vector<Relationship> result;
    xlsx::read_xml(package.relationships(source), [&](const Node &n) {
        if (n.kind == Node::Open && n.name == "Relationship") {
            if (result.size() >= 8192)
                throw ConfigError("工作簿关系数量超过上限");
            result.push_back(
                {n.get("Id"), n.get("Type"), n.get("Target"), n.get("TargetMode") == "External"});
        }
    });
    return result;
}
std::string resolve(const std::string &source, const Relationship &rel) {
    if (rel.external || rel.target.empty() ||
        rel.target.find_first_of("\\:?#") != std::string::npos ||
        rel.target.compare(0, 2, "//") == 0)
        throw ConfigError("工作簿所需部件必须是文件内的关系");
    const auto name =
        rel.target.front() == '/'
            ? rel.target.substr(1)
            : (std::filesystem::u8path(source).parent_path() / std::filesystem::u8path(rel.target))
                  .generic_u8string();
    const auto normal = std::filesystem::u8path(name).lexically_normal().generic_u8string();
    if (normal.empty() || normal == "." || normal == ".." || normal.compare(0, 3, "../") == 0)
        throw ConfigError("工作簿部件路径无效");
    return normal;
}
unsigned integer(std::string_view s, unsigned maximum, const char *message) {
    unsigned value = 0;
    const auto end = s.data() + s.size();
    const auto result = std::from_chars(s.data(), end, value);
    if (result.ec != std::errc{} || result.ptr != end || value > maximum)
        throw ConfigError(message);
    return value;
}
// Pass the ancestor names with each event. Shared and inline strings can contain rich-text runs and
// phonetic annotations; only displayed <t> text belongs in the measurement table.
template <class Fn> void walk(const std::string &bytes, Fn fn) {
    std::vector<std::string> path;
    xlsx::read_xml(bytes, [&](const Node &n) {
        if (n.kind == Node::Open)
            path.push_back(n.name);
        if (!path.empty())
            fn(n, path);
        if (n.kind == Node::Close)
            path.pop_back();
    });
}
bool displayed_text(const std::vector<std::string> &path) {
    return path.back() == "t" && std::find(path.begin(), path.end(), "rPh") == path.end();
}
std::vector<std::string> shared_strings(const std::string &xml) {
    std::vector<std::string> strings;
    walk(xml, [&](const Node &n, const auto &path) {
        if (n.kind == Node::Open && path.size() == 2 && path.back() == "si") {
            if (strings.size() >= max_cells)
                throw ConfigError("工作簿共享字符串过多");
            strings.emplace_back();
        } else if (n.kind == Node::Text && path.size() >= 3 && path[1] == "si" &&
                   displayed_text(path))
            strings.back() += n.text;
    });
    return strings;
}
std::pair<unsigned, unsigned> address(const std::string &ref) {
    std::size_t split = 0;
    unsigned col = 0;
    while (split < ref.size() && ref[split] >= 'A' && ref[split] <= 'Z') {
        col = col * 26 + ref[split++] - 'A' + 1;
        if (col > 16384)
            throw ConfigError("XLSX 列号无效");
    }
    if (!col || split == ref.size())
        throw ConfigError("XLSX 单元格地址无效");
    const auto row = integer(std::string_view(ref).substr(split), 1048576, "XLSX 行号无效");
    if (!row)
        throw ConfigError("XLSX 行号无效");
    return {row, col};
}
template <class SharedStrings>
std::string sheet_csv(const std::string &xml, SharedStrings get_strings) {
    Rows rows;
    unsigned row = 0, col = 0, width = 0;
    std::size_t stored_text = 0;
    bool formula = false;
    std::string type, value, inline_value;
    walk(xml, [&](const Node &n, const auto &path) {
        if (path.size() < 2 || path[0] != "worksheet" || path[1] != "sheetData")
            return;
        if (path.size() == 3 && n.kind == Node::Open && path[2] == "row") {
            const auto id = n.get("r");
            const auto next = id.empty() ? row + 1 : integer(id, 1048576, "XLSX 行号无效");
            if (next <= row)
                throw ConfigError("XLSX 行号重复或顺序错误");
            row = next;
            col = 0;
        }
        if (path.size() < 4 || path[2] != "row" || path[3] != "c")
            return;
        if (path.size() == 4 && n.kind == Node::Open) {
            const auto ref = n.get("r");
            if (ref.empty())
                ++col;
            else {
                const auto pos = address(ref);
                if (pos.first != row || pos.second <= col)
                    throw ConfigError("XLSX 单元格位置重复或顺序错误");
                col = pos.second;
            }
            type = n.get("t");
            formula = false;
            value.clear();
            inline_value.clear();
        } else if (path.size() == 5 && path[4] == "f" && n.kind == Node::Open)
            formula = true;
        else if (path.size() == 5 && path[4] == "v" && n.kind == Node::Text)
            value += n.text;
        else if (path.size() > 5 && path[4] == "is" && n.kind == Node::Text && displayed_text(path))
            inline_value += n.text;
        else if (path.size() == 4 && n.kind == Node::Close) {
            if (formula && value.empty())
                throw ConfigError("XLSX 单元格 " + column_name(col) + std::to_string(row) +
                                  " 的公式缺少缓存值，请先在电子表格软件中重算并保存");
            if (type == "inlineStr")
                value = std::move(inline_value);
            else if (type == "s" && !value.empty()) {
                const auto &strings = get_strings();
                const auto index = integer(value, static_cast<unsigned>(strings.size()),
                                           "XLSX 共享字符串编号无效");
                if (index >= strings.size())
                    throw ConfigError("XLSX 共享字符串编号不存在");
                if (strings[index].size() > csv_read_limit - stored_text)
                    throw ConfigError("XLSX 工作表的共享文本展开后超过读取上限");
                value = strings[index];
            } else if (type == "b" && !value.empty())
                value = (value == "1" ? "TRUE" : "FALSE");
            if (value.empty())
                return;
            if (value.size() > csv_read_limit - stored_text)
                throw ConfigError("XLSX 工作表的数据超过读取上限");
            stored_text += value.size();
            width = std::max(width, col);
            if (!row || !col || row > max_rows || width > max_columns ||
                std::size_t(row) * width > max_cells)
                throw ConfigError("XLSX 工作表的有效数据超出允许尺寸");
            rows.resize(row);
            rows.back().resize(col);
            rows.back().back() = std::move(value);
        }
    });
    std::string csv;
    for (const auto &r : rows) {
        for (unsigned c = 0; c < width; ++c) {
            if (c)
                csv += ',';
            if (c < r.size())
                csv += csv_cell(r[c]);
            if (csv.size() > csv_read_limit)
                throw ConfigError("XLSX 工作表的数据超过 CSV 读取上限");
        }
        csv += "\r\n";
    }
    if (csv.empty())
        throw ConfigError("XLSX 工作表没有测量数据");
    return csv;
}
} // namespace

struct XlsxWorkbook::Impl {
    explicit Impl(const std::string &bytes) : archive(bytes) {}
    xlsx::Package archive;
    std::string book;
    std::vector<std::string> names;
    std::vector<Relationship> relationships;
    std::vector<std::size_t> worksheets;
    std::optional<std::size_t> shared;
};

XlsxWorkbook::XlsxWorkbook(const std::string &bytes) : impl(std::make_unique<Impl>(bytes)) {
    for (const auto &r : links(impl->archive, ""))
        if (!r.external && r.is("officeDocument")) {
            impl->book = resolve("", r);
            break;
        }
    if (impl->book.empty())
        throw ConfigError("XLSX 缺少工作簿关系");
    std::map<std::string, std::size_t> worksheets;
    impl->relationships = links(impl->archive, impl->book);
    for (std::size_t i = 0; i < impl->relationships.size(); ++i) {
        const auto &r = impl->relationships[i];
        if (r.external)
            continue;
        if (r.is("worksheet")) {
            if (r.id.empty() || !worksheets.emplace(r.id, i).second)
                throw ConfigError("XLSX 工作表关系编号为空或重复");
        } else if (r.is("sharedStrings"))
            impl->shared = i;
    }
    std::set<std::string> seen;
    walk(impl->archive.part(impl->book), [&](const Node &n, const auto &p) {
        if (n.kind != Node::Open || p.size() != 3 || p[0] != "workbook" || p[1] != "sheets" ||
            p[2] != "sheet")
            return;
        const auto rel = worksheets.find(n.get("id"));
        if (rel == worksheets.end())
            return;
        auto name = n.get("name");
        if (name.empty() || !seen.insert(name).second)
            throw ConfigError("XLSX 数据工作表名称为空或重复");
        if (impl->names.size() >= 2048)
            throw ConfigError("XLSX 工作表数量超过读取上限");
        impl->names.push_back(std::move(name));
        impl->worksheets.push_back(rel->second);
    });
    if (impl->names.empty())
        throw ConfigError("XLSX 没有可读取的数据工作表");
}
XlsxWorkbook::~XlsxWorkbook() = default;
const std::vector<std::string> &XlsxWorkbook::sheet_names() const { return impl->names; }
std::string XlsxWorkbook::csv(std::size_t index) const {
    if (index >= impl->names.size())
        throw ConfigError("XLSX 所选工作表编号无效");
    const auto worksheet = resolve(impl->book, impl->relationships[impl->worksheets[index]]);
    std::optional<std::vector<std::string>> strings;
    return sheet_csv(impl->archive.part(worksheet), [&]() -> const std::vector<std::string> & {
        if (!strings) {
            if (!impl->shared)
                throw ConfigError("XLSX 共享字符串部件不存在");
            const auto shared = resolve(impl->book, impl->relationships[*impl->shared]);
            strings = shared_strings(impl->archive.part(shared));
        }
        return *strings;
    });
}
std::string csv_from_xlsx(const std::string &bytes) { return XlsxWorkbook(bytes).csv(0); }

std::string xlsx_from_csv(const std::string &csv, int decimalPlaces) {
    if (decimalPlaces < 0 || decimalPlaces > 10)
        throw ConfigError("XLSX 小数位数须在 0～10 之间");
    const auto rows = canonical_rows(csv);
    const bool wide = rows.front().front().empty();
    const auto &header = rows.front();
    const auto measurement =
        std::find(header.begin(), header.end(), "Measurement") - header.begin();
    if (!wide && std::size_t(measurement) == header.size())
        throw ConfigError("XLSX 长表缺少 Measurement 列");
    const std::string ns(spreadsheet);
    std::string sheet = "<worksheet xmlns=\"" + ns + "\"><cols>";
    for (std::size_t col = 0; col < header.size(); ++col) {
        std::size_t characters = 8;
        for (const auto &row : rows)
            characters = std::max(characters, row[col].size() + 2);
        const auto index = std::to_string(col + 1);
        sheet += "<col min=\"" + index + "\" max=\"" + index + "\" width=\"" +
                 std::to_string(std::min(characters, std::size_t(255))) + "\" customWidth=\"1\"/>";
    }
    sheet += "</cols><sheetData>";
    for (std::size_t r = 0; r < rows.size(); ++r) {
        sheet += "<row r=\"" + std::to_string(r + 1) + "\">";
        for (std::size_t c = 0; c < rows[r].size(); ++c) {
            const auto &value = rows[r][c];
            if (value.empty())
                continue;
            sheet += "<c r=\"" + column_name(c + 1) + std::to_string(r + 1) + "\"";
            const bool reading = r && (wide ? c != 0 : c == std::size_t(measurement));
            const bool text = wide ? c == 0 : !r || header[c] == "Operator";
            if (text)
                sheet += " t=\"inlineStr\"><is><t>" + escaped(value) + "</t></is></c>";
            else {
                parse_number(value);
                sheet += (reading ? " s=\"1\"><v>" : "><v>") + value + "</v></c>";
            }
        }
        sheet += "</row>";
    }
    sheet += "</sheetData></worksheet>";
    const auto fmt = decimalPlaces ? "0." + std::string(decimalPlaces, '0') : "0";
    const std::string styles =
        "<styleSheet xmlns=\"" + ns +
        "\"><numFmts count=\"1\"><numFmt numFmtId=\"164\" formatCode=\"" + fmt +
        "\"/></numFmts><fonts count=\"1\"><font><sz val=\"11\"/><name "
        "val=\"Calibri\"/></font></fonts>"
        "<fills count=\"2\"><fill><patternFill patternType=\"none\"/></fill><fill><patternFill "
        "patternType=\"gray125\"/></fill></fills>"
        "<borders count=\"1\"><border/></borders><cellStyleXfs count=\"1\"><xf numFmtId=\"0\" "
        "fontId=\"0\" fillId=\"0\" borderId=\"0\"/></cellStyleXfs>"
        "<cellXfs count=\"2\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\" "
        "xfId=\"0\"/>"
        "<xf numFmtId=\"164\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\" "
        "applyNumberFormat=\"1\"/></cellXfs>"
        "<cellStyles count=\"1\"><cellStyle name=\"Normal\" xfId=\"0\" "
        "builtinId=\"0\"/></cellStyles></styleSheet>";
    const auto rel = std::string("<Relationships xmlns=\"") + package_rel + "\">";
    const auto relationship = [](const char *id, const char *type, const char *target) {
        return std::string("<Relationship Id=\"") + id + "\" Type=\"" + office_rel + type +
               "\" Target=\"" + target + "\"/>";
    };
    return xlsx::zip_stored(
        {{"[Content_Types].xml",
          "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
          "<Default Extension=\"rels\" "
          "ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
          "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
          "<Override PartName=\"/xl/workbook.xml\" "
          "ContentType=\"application/"
          "vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>"
          "<Override PartName=\"/xl/worksheets/sheet1.xml\" "
          "ContentType=\"application/"
          "vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>"
          "<Override PartName=\"/xl/styles.xml\" "
          "ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml\"/"
          "></Types>"},
         {"_rels/.rels",
          rel + relationship("rId1", "officeDocument", "xl/workbook.xml") + "</Relationships>"},
         {"xl/workbook.xml",
          "<workbook xmlns=\"" + ns +
              "\" xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">"
              "<sheets><sheet name=\"MSA\" sheetId=\"1\" r:id=\"rId1\"/></sheets></workbook>"},
         {"xl/_rels/workbook.xml.rels",
          rel + relationship("rId1", "worksheet", "worksheets/sheet1.xml") +
              relationship("rId2", "styles", "styles.xml") + "</Relationships>"},
         {"xl/worksheets/sheet1.xml", std::move(sheet)},
         {"xl/styles.xml", styles}});
}
} // namespace msa
