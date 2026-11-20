#pragma once
#include "msa/core.hpp"
#include "msa/xlsx_package.hpp"

namespace test_fixture {
// By default the first sheet and unused shared-string part contain invalid XML.
inline std::string selection_workbook(const std::string &relationship_type,
                                      const std::string &first_sheet = "<unclosed>",
                                      const std::string &second_sheet = {},
                                      bool include_first = true) {
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
              "\"><sheets>" + (include_first ? "<sheet name=\"损坏页\" sheetId=\"1\" r:id=\"one\"/>" : "") +
              "<sheet "
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
         {"one.xml", first_sheet},
         {"two.xml", second_sheet.empty()
                         ? "<worksheet xmlns=\"" + ns +
                               "\"><sheetData><row r=\"1\"><c "
                               "r=\"A1\"><v>42</v></c></row></sheetData></worksheet>"
                         : second_sheet}});
}

inline std::string numeric_sheet(const msa::Matrix &readings) {
    std::string xml =
        R"(<worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main"><sheetData>)";
    for (const auto &row : readings) {
        xml += "<row>";
        for (double value : row)
            xml += "<c><v>" + std::to_string(value) + "</v></c>";
        xml += "</row>";
    }
    return xml + "</sheetData></worksheet>";
}
} // namespace test_fixture
