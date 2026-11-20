#include "msa/core.hpp"
#include "msa/xlsx.hpp"
#include "msa/xlsx_package.hpp"
#include <fstream>
#include <iostream>
#include <iterator>

namespace {
void require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
std::string bytes(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("could not read fixture " + path.u8string());
    return std::string(std::istreambuf_iterator<char>(in), {});
}
template <class Fn> void expect_config_error(Fn fn, const std::string &description) {
    try {
        fn();
    } catch (const msa::ConfigError &e) {
        if (std::string(e.what()).find(description) != std::string::npos)
            return;
        throw;
    }
    throw std::runtime_error("expected an error containing " + description);
}
// The first sheet and unused shared-string part contain invalid XML. The third sheet is external.
std::string selection_workbook(const std::string &relationship_type) {
    const std::string rels = "http://schemas.openxmlformats.org/package/2006/relationships";
    const std::string rel = "http://schemas.openxmlformats.org/officeDocument/2006/relationships";
    const std::string ns = "http://schemas.openxmlformats.org/spreadsheetml/2006/main";
    return msa::xlsx::zip_stored(
        {{"[Content_Types].xml",
          R"(<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="xml" ContentType="application/xml"/><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/></Types>)"},
         {"_rels/.rels", "<Relationships xmlns=\"" + rels + "\"><Relationship Id=\"book\" Type=\"" +
                             rel + "/officeDocument\" Target=\"workbook.xml\"/></Relationships>"},
         {"workbook.xml",
          "<workbook xmlns=\"" + ns + "\" xmlns:r=\"" + rel +
              "\"><sheets><sheet name=\"损坏页\" sheetId=\"1\" r:id=\"one\"/><sheet "
              "name=\"数据页\" sheetId=\"2\" r:id=\"two\"/><sheet name=\"外部页\" sheetId=\"3\" "
              "r:id=\"external\"/></sheets></workbook>"},
         {"_rels/workbook.xml.rels",
          "<Relationships xmlns=\"" + rels + "\"><Relationship Id=\"two\" Type=\"" + rel + "/" +
              relationship_type + "\" Target=\"two.xml\"/><Relationship Id=\"one\" Type=\"" + rel +
              "/" + relationship_type +
              "\" Target=\"one.xml\"/><Relationship Id=\"external\" Type=\"" + rel +
              "/worksheet\" Target=\"https://example.invalid/unread\" "
              "TargetMode=\"External\"/><Relationship Id=\"strings\" Type=\"" +
              rel + "/sharedStrings\" Target=\"strings.xml\"/></Relationships>"},
         {"strings.xml", "<unclosed>"},
         {"one.xml", "<unclosed>"},
         {"two.xml", "<worksheet xmlns=\"" + ns +
                         "\"><sheetData><row r=\"1\"><c "
                         "r=\"A1\"><v>42</v></c></row></sheetData></worksheet>"}});
}
void test_sheet_selection(const std::filesystem::path &fixtures,
                          const std::filesystem::path &output, const msa::Json &config) {
    const auto multi = fixtures / "xlsx-multiple-sheets.xlsx";
    const auto first = msa::read_data(multi, config);
    const auto second = msa::read_data(multi, config, "复测 数据");
    require(first.at(0).at(0) == 10.11 && second.at(0).at(0) == 110.11 && first != second,
            "chart skipped, named hidden worksheet selected");
    require(msa::read_data(multi, config, "原始记录") == first, "default workbook order");
    expect_config_error([&] { msa::read_data(multi, config, "概览图"); }, "找不到工作表");
    const std::vector<std::string> expected{"原始记录", "复测 数据"};
    bool asked = false;
    const auto copy = output / "selection-snapshot.xlsx";
    msa::atomic_text(copy, bytes(multi));
    auto result =
        msa::read_data(copy, config, [&](const auto &names) -> std::optional<std::size_t> {
            require(names == expected, "worksheet names, order and chart filtering");
            asked = true;
            std::filesystem::remove(copy);
            return 1;
        });
    require(asked && result && result->sheet_name == expected[1] && result->matrix == second,
            "selection did not read the original snapshot");
    expect_config_error([&] { msa::read_data(multi, config, [](const auto &) { return 20; }); },
                        "工作表编号无效");

    const auto single = output / "selection-single.xlsx";
    const auto csv = msa::XlsxWorkbook(bytes(multi)).csv(0);
    msa::atomic_text(single, msa::xlsx_from_csv(csv, 2));
    const msa::WorksheetSelector never_ask = [](const auto &) -> std::optional<std::size_t> {
        throw std::runtime_error("unexpected worksheet selection prompt");
    };
    result = msa::read_data(single, config, never_ask);
    require(result && result->sheet_name == "MSA" && result->matrix == first,
            "automatic single sheet");
    require(msa::read_data(single, config, "MSA") == first, "explicit single sheet");
    expect_config_error([&] { msa::read_data(single, config, "msa"); }, "找不到工作表");
    const auto csv_path = output / "selection.csv";
    msa::atomic_text(csv_path, csv);
    result = msa::read_data(csv_path, config, never_ask);
    require(result && result->sheet_name.empty() && result->matrix == first,
            "CSV selection bypass");
    expect_config_error([&] { msa::read_data(csv_path, config, "MSA"); }, "只有 XLSX");

    const auto broken = selection_workbook("worksheet");
    const msa::XlsxWorkbook workbook(broken);
    require(workbook.sheet_names() == std::vector<std::string>{"损坏页", "数据页"},
            "external worksheet was offered");
    require(workbook.csv(1) == "42\r\n", "unselected broken sheet was decoded");
    const auto path = output / "selection-broken.xlsx";
    msa::atomic_text(path, broken);
    require(!msa::read_data(path, config, [](const auto &) { return std::nullopt; }),
            "cancelled worksheet was decoded");
    expect_config_error([&] { msa::read_data(path, config, "损坏页"); }, "损坏页");
    expect_config_error([&] { msa::XlsxWorkbook empty(selection_workbook("chartsheet")); },
                        "没有可读取");
}
} // namespace
int main(int argc, char **argv) {
    try {
        if (argc != 3)
            throw std::runtime_error("expected fixture and output directories");
        const auto fixtures = std::filesystem::u8path(argv[1]);
        const auto output = std::filesystem::u8path(argv[2]);
        const std::string wide = ",1,2\r\nA,1.250,2.750\r\nB,3.000,4.500\r\n";
        const auto wb = msa::xlsx_from_csv("\xef\xbb\xbf" + wide, 3);
        require(msa::xlsx_from_csv(wide, 3) == wb, "non-deterministic workbook");
        require(msa::csv_from_xlsx(wb) == wide, "numeric wide roundtrip");
        const std::string long_csv =
            "StdOrder,Operator,Part,Trial,Measurement\r\n2,B,1,1,1.2300000\r\n";
        require(msa::csv_from_xlsx(msa::xlsx_from_csv(long_csv, 7)) == long_csv,
                "ordered long roundtrip");
        msa::atomic_text(output / "wide.xlsx", wb);
        msa::atomic_text(output / "long.xlsx", msa::xlsx_from_csv(long_csv, 7));
        require(msa::csv_from_xlsx(bytes(fixtures / "xlsx-office-inline.xlsx")) ==
                    ",1,2\r\nA,1.25,2.75\r\nB,,4.5\r\n",
                "DEFLATE, inline strings and sparse coordinates");
        const auto external = msa::csv_from_xlsx(bytes(fixtures / "xlsx-office-shared.xlsx"));
        require(external.find("\"批次,\"\"甲\"\"\"") != std::string::npos,
                "rich shared strings and CSV escaping");
        auto config = msa::default_config();
        config["design"] = {{"operators", 2}, {"replicates", 2}, {"parts", 5}};
        test_sheet_selection(fixtures, output, config);
        const auto matrix = msa::parse_export_csv(external, config);
        for (int op = 0; op < 2; ++op)
            for (int trial = 1; trial <= 2; ++trial)
                for (int part = 1; part <= 5; ++part)
                    require(matrix[op * 2 + trial - 1][part - 1] ==
                                (op + 1) * 10 + part + trial / 10.,
                            "first declared worksheet, cached values and column mapping");
        for (const auto &invalid : {std::string("text"), wb.substr(0, wb.size() - 10)}) {
            bool rejected = false;
            try {
                msa::csv_from_xlsx(invalid);
            } catch (const msa::ConfigError &) {
                rejected = true;
            }
            require(rejected, "malformed workbook accepted");
        }
        std::cout << "XLSX round-trip, deterministic output, compressed Office fixtures and bounds "
                     "passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
