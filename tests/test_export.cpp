#include "msa/core.hpp"
#include "msa/xlsx.hpp"
#include "ui_model.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <sstream>
#include <thread>

using namespace msa;
namespace {
void check(bool ok, const std::string &message) {
    if (!ok)
        throw std::runtime_error(message);
}
template <class F> void rejects(F action) {
    bool failed = false;
    try {
        action();
    } catch (const ConfigError &) {
        failed = true;
    }
    check(failed, "invalid export configuration/record accepted");
}
Json config(int operators, int trials, int parts, int dp = 4) {
    auto c = default_config();
    c["design"] = {{"operators", operators}, {"replicates", trials}, {"parts", parts}};
    c["specification"]["compliance"] = "relaxed";
    c["generation"]["reference_tv_sd"] = 1;
    c["generation"]["decimal_places"] = dp;
    c["generation"]["seed"] = 123;
    return normalize_config(c);
}
Result study(int operators, int trials, int parts, int dp = 4) {
    auto c = config(operators, trials, parts, dp);
    Matrix matrix;
    for (int op = 1; op <= operators; ++op)
        for (int t = 1; t <= trials; ++t) {
            std::vector<double> row;
            for (int p = 1; p <= parts; ++p)
                row.push_back(parse_number(fixed(op * 100 + t * 10 + p + .12345678901, dp)));
            matrix.push_back(row);
        }
    return Result{matrix, calculate(matrix, c), c, 123};
}
std::vector<int> ids(const std::vector<MeasurementRecord> &records) {
    std::vector<int> result;
    for (const auto &r : records)
        result.push_back(r.standard_order);
    return result;
}
void record_checks(const Result &s, const Json &output) {
    const auto records = export_records(s, output);
    const int o = s.config["design"]["operators"], t = s.config["design"]["replicates"],
              p = s.config["design"]["parts"];
    check(int(records.size()) == o * t * p, "record count");
    std::set<int> standard;
    for (const auto &r : records) {
        const int expected = ((r.trial - 1) * p + r.part - 1) * o + r.operator_number;
        check(r.standard_order == expected, "StdOrder identity");
        check(r.measurement == s.matrix[(r.operator_number - 1) * t + r.trial - 1][r.part - 1],
              "measurement identity");
        standard.insert(r.standard_order);
    }
    check(int(standard.size()) == o * t * p && *standard.begin() == 1 &&
              *standard.rbegin() == o * t * p,
          "StdOrder permutation");
    const auto mode = output["run_order"]["mode"];
    if (mode == "standard")
        for (int i = 0; i < int(records.size()); ++i)
            check(records[i].standard_order == i + 1, "standard order");
    if (mode == "within_operator") {
        std::vector<int> operatorOrder;
        for (int trial = 1; trial <= t; ++trial)
            for (int block = 0; block < o; ++block) {
                const int start = ((trial - 1) * o + block) * p;
                const int op = records[start].operator_number;
                if (trial == 1)
                    operatorOrder.push_back(op);
                check(op == operatorOrder[block], "operator order changes between trials");
                if (!output["run_order"]["randomize_operators"].get<bool>())
                    check(op == block + 1, "operator order should be fixed");
                std::set<int> parts;
                for (int i = 0; i < p; ++i) {
                    const auto &r = records[start + i];
                    check(r.trial == trial && r.operator_number == op,
                          "within-operator block boundaries");
                    parts.insert(r.part);
                }
                check(int(parts.size()) == p, "within-operator parts missing");
            }
    }
}
Matrix default_reference(const std::filesystem::path &path) {
    return parse_export_csv(read_text(path), config(3, 3, 10));
}
void wide_checks(const Result &s, const std::string &bytes) {
    check(bytes.compare(0, 3, "\xef\xbb\xbf") == 0, "wide table BOM");
    const int parts = s.config["design"]["parts"], trials = s.config["design"]["replicates"],
              dp = s.config["generation"]["decimal_places"];
    std::istringstream input(bytes.substr(3));
    std::string line, header;
    for (int part = 1; part <= parts; ++part)
        header += ',' + std::to_string(part);
    std::getline(input, line);
    check(line == header + '\r', "wide part column numbers or empty corner");
    std::size_t row = 0;
    while (std::getline(input, line)) {
        check(row < s.matrix.size(), "extra wide row");
        const auto expected =
            std::string(1, char('A' + row / trials)) + ',' + csv_text(Matrix{s.matrix[row]}, dp);
        check(line + '\n' == expected, "wide operator label, mapping or decimal places");
        ++row;
    }
    check(row == s.matrix.size(), "missing wide row");
    check(parse_export_csv(bytes, s.config) == s.matrix && parse_csv(bytes) == s.matrix,
          "labeled wide roundtrip");
    check(parse_csv(csv_text(s.matrix, dp)) == s.matrix, "legacy numeric wide reader");
}
void operator_name_checks(const std::filesystem::path &out) {
    auto sample = study(3, 2, 5);
    auto &design = sample.config["design"];
    design.erase("operator_names");
    check(operator_names(design) == std::array<std::string, 3>{"A", "B", "C"} &&
              normalize_config(sample.config)["design"]["operator_names"] == Json({"", "", ""}),
          "legacy operator defaults");
    design["operator_names"] = {"  张, \"伟\" <&>  ", "  ", "001"};
    auto normalized = normalize_config(sample.config);
    check(normalized["design"]["operator_names"] == Json({"张, \"伟\" <&>", "", "001"}) &&
              operator_names(normalized["design"])[1] == "B", "operator names not normalized");
    for (const char *format : {"default", "minitab"}) {
        auto settings = sample.config["output"];
        settings["format"] = format;
        settings["run_order"]["mode"] = "all";
        settings["run_order"]["seed"] = 123;
        settings["run_order"]["store_standard_order"] = true;
        const auto csv = export_csv_text(sample, settings);
        check(csv.find("\"张, \"\"伟\"\" <&>\"") != std::string::npos &&
                  csv.find("001,") != std::string::npos &&
                  parse_export_csv(csv, sample.config) == sample.matrix,
              "quoted operator names in CSV");
        if (settings["format"] == "default")
            check(parse_csv(csv) == sample.matrix, "generic wide reader lost named groups");
        for (const char *ext : {".xlsx", ".csv"}) {
            const auto file = out / (std::string("operator-") + format + ext);
            Json receipt;
            export_data(sample, file, settings, ExistingFile::Replace, &receipt);
            check(read_data(file, normalized) == sample.matrix, "named data file did not roundtrip");
            if (data_file_type(file) == "xlsx")
                check(csv_from_xlsx(read_text(file)).find("\"张, \"\"伟\"\" <&>\"") != std::string::npos,
                      "workbook changed the operator label");
            auto report = sample.as_json();
            report["export"] = receipt;
            const auto replay = out / (std::string("operator-replay") + ext);
            replay_data(report, replay, ExistingFile::Replace);
            check(read_text(replay) == read_text(file), "named report replay changed the export");
        }
    }
    design["operator_names"] = {"B", "", "003"};
    rejects([&] { normalize_config(sample.config); }); // Explicit B collides with the fallback.
    design["operators"] = 2;
    design["operator_names"] = {"B", "甲", "甲"};
    check(normalize_config(sample.config)["design"]["operator_names"][2] == "甲",
          "inactive third name was lost or rejected");
    design["operators"] = 3;
    rejects([&] { normalize_config(sample.config); });

    const auto old = study(2, 2, 5);
    auto renamed = old;
    for (const auto &names : {Json({"B", "甲", ""}), Json({"B", "A", ""})}) {
        renamed.config["design"]["operator_names"] = names;
        for (const char *format : {"default", "minitab"}) {
            auto settings = old.config["output"];
            settings["format"] = format;
            settings["run_order"]["mode"] = "all";
            settings["run_order"]["seed"] = 7;
            for (bool standard_order : {false, true}) {
                settings["run_order"]["store_standard_order"] = standard_order;
                const auto custom_csv = export_csv_text(renamed, settings);
                check(parse_export_csv(custom_csv, renamed.config) == old.matrix,
                      "custom labels confused with legacy letters");
                if (settings["format"] == "default" || standard_order || names[1] != "A")
                    check(parse_export_csv(export_csv_text(old, settings), renamed.config) == old.matrix,
                          "legacy identities changed when custom labels overlap legacy letters");
            }
        }
    }
}
void imported_operator_checks(const std::filesystem::path &out) {
    auto preferences = config(3, 3, 10);
    preferences["design"]["operator_names"] = {"默认甲", "默认乙", "默认丙"};
    preferences["analysis"]["study_multiplier"] = 5.15;
    const auto saved = preferences;
    auto source = study(2, 2, 5);
    source.config["design"]["operator_names"] = {"B", "001, \"张\" & 李", ""};
    const auto original_names = operator_names(source.config["design"]);
    for (const char *format : {"default", "minitab"}) {
        auto settings = source.config["output"];
        settings["format"] = format;
        settings["run_order"]["mode"] = "all";
        settings["run_order"]["seed"] = 3; // The shuffled table starts with the second operator.
        for (bool numbered : {false, true}) {
            settings["run_order"]["store_standard_order"] = numbered;
            for (const auto *extension : {".csv", ".xlsx"}) {
                const auto path = out / (std::string("import-operators-") + format + extension);
                export_data(source, path, settings);
                const auto data = read_study_data(path, preferences);
                const auto &design = data.config.at("design");
                check(design.at("operators") == 2 && design.at("parts") == 5 &&
                          design.at("replicates") == 2 && design.at("operator_names")[2] == "",
                      "import did not infer the file's design or retained an inactive default");
                const auto names = operator_names(design);
                const bool shuffled = settings["format"] != "default" && !numbered;
                const int first = shuffled
                                      ? export_records(source, settings).front().operator_number - 1
                                      : 0;
                check(names[0] == original_names[first] && names[1] == original_names[1 - first],
                      "imported operator order or spelling differs from the file");
                for (int op = 0; op < 2; ++op) {
                    const int source_op = names[op] == original_names[0] ? 0 : 1;
                    for (int trial = 0; trial < 2; ++trial)
                        check(data.matrix.at(op * 2 + trial) ==
                                  source.matrix.at(source_op * 2 + trial),
                              "imported name attached to the wrong readings");
                }
                check(data.config.at("analysis") == preferences.at("analysis") &&
                          preferences == saved,
                      "import changed analysis settings or source preferences");
            }
        }
    }
    const auto invalid_wide = [&](std::vector<std::string> labels) {
        std::string csv = ",1,2,3,4,5\r\n";
        for (const auto &label : labels)
            csv += label + ",0,1,2,3,4\r\n";
        rejects([&] { import_csv_study(csv, saved); });
    };
    invalid_wide({"甲", "甲", "乙", ""});
    invalid_wide({"甲", "乙", "甲", "乙"});
    invalid_wide({"甲", "甲", "乙", "乙", "丙"});
    invalid_wide({"甲", "甲", "乙", "乙", "丙", "丙", "丁", "丁"});
    auto ordered = source.config["output"];
    ordered["format"] = "minitab";
    ordered["run_order"]["store_standard_order"] = true;
    const auto csv = export_csv_text(source, ordered);
    const auto first = csv.find("\r\n") + 2;
    auto wrong_order = csv;
    wrong_order[first] = '2';
    rejects([&] { import_csv_study(wrong_order, preferences); });
    auto duplicate = csv + csv.substr(first, csv.find("\r\n", first) + 2 - first);
    rejects([&] { import_csv_study(duplicate, preferences); });
    const auto missing = csv.substr(0, csv.rfind("\r\n", csv.size() - 3) + 2);
    rejects([&] { import_csv_study(missing, preferences); });
}
void six_row_import_checks(const std::filesystem::path &out) {
    auto preferences = config(3, 2, 10);
    preferences["design"]["operator_names"] = {"默认甲", "默认乙", "默认丙"};
    const auto saved = preferences;
    const auto sample = study(2, 3, 5);
    const auto plain = out / "six-numeric.csv";
    atomic_text(plain, csv_text(sample.matrix, 4));

    int prompts = 0;
    std::optional<int> answer;
    const auto choose = [&] {
        ++prompts;
        return answer;
    };
    check(!read_study_data(plain, preferences, WorksheetSelector{}, choose) && prompts == 1,
          "cancelled numeric CSV import must return no study");
    for (int repeats : {3, 2}) {
        answer = repeats;
        const auto chosen = read_study_data(plain, preferences, WorksheetSelector{}, choose);
        const auto specified = read_study_data(plain, preferences, repeats);
        const Json names = repeats == 3 ? Json({"A", "B", ""}) : Json({"A", "B", "C"});
        const Json design = {{"operators", 6 / repeats}, {"replicates", repeats},
                             {"parts", 5}, {"operator_names", names}};
        check(chosen && chosen->matrix == sample.matrix && chosen->config.at("design") == design &&
                  specified.matrix == chosen->matrix && specified.config == chosen->config,
              "six-row numeric CSV must use the explicit choice and preserve row order");
    }
    check(prompts == 3 && preferences == saved,
          "a choice leaked into another import or the saved design");
    try {
        read_study_data(plain, preferences);
        throw std::runtime_error("six-row numeric CSV accepted without an explicit choice");
    } catch (const ConfigError &e) {
        const std::string message = e.what();
        check(message.find("--replicates 2") != message.npos &&
                  message.find("--replicates 3") != message.npos,
              "six-row CLI diagnostic should explain both choices");
    }

    const ReplicateSelector unexpected = []() -> std::optional<int> {
        throw std::runtime_error("unexpected design prompt for an unambiguous table");
    };
    for (const int operators : {2, 3}) {
        const auto unambiguous = study(operators, operators, 5);
        const auto data = import_csv_study(csv_text(unambiguous.matrix, 4), preferences, unexpected);
        check(data && data->config.at("design").at("operators") == operators &&
                  data->config.at("design").at("replicates") == operators &&
                  data->matrix == unambiguous.matrix, "incorrect automatic four- or nine-row import");
    }
    for (const char *format : {"default", "minitab"}) {
        auto settings = sample.config.at("output");
        settings["format"] = format;
        const auto data = import_csv_study(export_csv_text(sample, settings), preferences, unexpected);
        check(data && data->matrix == sample.matrix && data->config.at("design").at("operators") == 2,
              "six-row labeled study must determine its own design");
    }
}
void inferred_design_edge_checks(const std::filesystem::path &out) {
    auto settings = config(2, 2, 5);
    settings["analysis"]["method"] = "crossed_anova";
    settings["analysis"]["confidence"]["enabled"] = true;
    settings["analysis"]["confidence"]["level"] = 30;
    rejects([&] { normalize_config(settings); }); // MLS at 30% needs three operators.
    const auto three = study(3, 2, 5);
    const auto csv = export_csv_text(three, three.config.at("output"));
    for (const auto *title : {"Operator", "操作员"}) {
        auto titled = csv;
        titled.insert(titled.find(','), title);
        const auto path = out / "external-wide.csv";
        atomic_text(path, titled);
        const auto data = read_study_data(path, settings);
        check(data.matrix == three.matrix && data.config.at("design").at("operators") == 3 &&
                  data.config.at("analysis").at("confidence").at("level") == 30,
              "external wide operator header or post-detection MLS validation failed");
    }
    settings["design"]["operators"] = 3;
    const auto two = study(2, 2, 5);
    rejects([&] { import_csv_study(export_csv_text(two, two.config.at("output")), settings); });

    std::string gap = "Operator,Part,Trial,Measurement\n";
    for (int trial : {1, 2})
        for (int part : {1, 3, 4, 5})
            for (char op : {'A', 'B'})
                gap += std::string(1, op) + "," + std::to_string(part) + "," +
                       std::to_string(trial) + ",10.5\n";
    rejects([&] { import_csv_study(gap, Json::object()); });

    auto malformed = csv;
    for (std::size_t pos = 0; (pos = malformed.find("\nA,", pos)) != std::string::npos; ++pos)
        malformed[pos + 1] = static_cast<char>(0xff);
    rejects([&] { import_csv_study(malformed, Json::object()); });
}
int run(const std::filesystem::path &fixtures, const std::filesystem::path &out) {
    std::filesystem::create_directories(out);
    operator_name_checks(out);
    imported_operator_checks(out);
    six_row_import_checks(out);
    inferred_design_edge_checks(out);
    auto reference = default_reference(fixtures / std::filesystem::u8path("默认.csv"));
    auto refCfg = config(3, 3, 10);
    const std::vector<std::string> referenceFiles{"Minitab.csv", "SPSS.csv", "JMP 19.csv",
                                                  "Origin.csv", "Matlab.csv"};
    std::string first;
    for (const auto &file : referenceFiles) {
        std::ifstream in(fixtures / file, std::ios::binary);
        check(bool(in), "cannot open reference long table: " + file);
        const std::string bytes{std::istreambuf_iterator<char>(in),
                                std::istreambuf_iterator<char>{}};
        check(!in.bad(), "cannot read reference long table: " + file);
        check(bytes.compare(0, 3, "\xef\xbb\xbf") != 0, "reference long table BOM");
        check(parse_export_csv(bytes, refCfg) == reference,
              "six references have different measurements");
        if (first.empty())
            first = bytes;
        else
            check(first == bytes, "five long references differ");
    }
    Result ref{reference, calculate(reference, refCfg), refCfg, 123};
    for (const auto *format : native_ui::csv_formats) {
        auto output = refCfg["output"];
        output["format"] = format;
        const auto path = out / (std::string(format) + ".csv");
        check(export_csv(ref, path, output) == ref.metrics, "written reference metrics");
        check(parse_export_csv(read_text(path), refCfg) == reference, "written reference mapping");
    }
    for (int dp : {0, 4, 10}) {
        const auto sample = study(3, 2, 7, dp);
        for (const char *format : native_ui::csv_formats) {
            auto settings = sample.config.at("output");
            settings["format"] = format;
            settings["file_type"] = "xlsx";
            settings["run_order"] = {{"mode", "all"}, {"seed", nullptr},
                                     {"store_standard_order", true}, {"randomize_operators", false}};
            const auto xlsx = out / (std::string(format) + "_roundtrip.XLSX");
            Json receipt;
            check(export_data(sample, xlsx, settings, ExistingFile::Replace, &receipt) == sample.metrics &&
                      read_data(xlsx, sample.config) == sample.matrix,
                  "workbook did not roundtrip through normal file APIs");
            const auto bytes = read_text(xlsx);
            auto ordered = export_csv_text(sample, settings);
            if (ordered.compare(0, 3, "\xef\xbb\xbf") == 0)
                ordered.erase(0, 3);
            check(csv_from_xlsx(bytes) == ordered,
                  "workbook rows, labels, ordering or numeric display precision changed");
            check(bytes.rfind("PK", 0) == 0 && receipt["file_type"] == "xlsx" &&
                      receipt["data_path"] == xlsx.u8string() && receipt["sha256"] == sha256(bytes),
                  "workbook receipt does not describe the saved bytes");
            auto report = sample.as_json();
            report["config"]["design"].erase("operator_names");
            report["config"]["schema_version"] = 1;
            report["config"]["input"]["filename"] = "[DATE].csv";
            report["export"] = receipt;
            const auto replay = out / "workbook-replay.xlsx";
            replay_data(report, replay, ExistingFile::Replace);
            check(read_text(replay) == bytes, "workbook replay was not byte-identical");
            report["export"]["sha256"] = std::string(64, '0');
            rejects([&] { replay_data(report, replay, ExistingFile::Replace); });
            check(read_text(replay) == bytes, "failed hash verification replaced the prior workbook");
        }
    }
    {
        auto settings = refCfg.at("output");
        settings["file_type"] = "xlsx";
        settings["filename"] = "[SEED]_数据.CSV";
        settings["directory"] = std::filesystem::absolute(out).u8string();
        settings["existing_file"] = "overwrite";
        auto dest = export_configured_data(ref, settings);
        check(dest.filename() == std::filesystem::u8path("123_数据.xlsx") &&
                  read_data(dest, refCfg) == ref.matrix,
              "configured workbook naming or format selection failed");
        const auto legacy = out / "legacy.data";
        Json receipt;
        export_data(ref, legacy, settings, ExistingFile::Replace, &receipt);
        check(receipt["file_type"] == "csv" && receipt["csv_path"] == legacy.u8string() &&
                  read_data(legacy, refCfg) == ref.matrix,
              "explicit non-workbook path lost legacy CSV semantics");
        auto report = ref.as_json();
        receipt.erase("file_type");
        receipt.erase("data_path");
        report["export"] = receipt;
        report["config"]["output"]["filename"] = "legacy.csv";
        report["config"]["schema_version"] = 1;
        report["config"]["output"]["charts"]["filename"] = "旧[图表]";
        report["config"]["output"].erase("file_type");
        replay_data(report, out / "legacy-replay.csv", ExistingFile::Replace);
        check(read_text(out / "legacy-replay.csv") == read_text(legacy),
              "old CSV receipt could not be replayed");
        rejects([&] { replay_data(report, out / "wrong-type.xlsx", ExistingFile::Replace); });
    }
    int configurations = 0;
    for (int o : {2, 3})
        for (int t : {2, 3})
            for (int p = 5; p <= 10; ++p)
                for (int dp : {0, 4, 10}) {
                    auto s = study(o, t, p, dp);
                    const auto original = s.matrix;
                    const auto wide = export_csv_text(s, s.config["output"]);
                    wide_checks(s, wide);
                    auto output = s.config["output"];
                    output["run_order"] = {{"mode", "all"},
                                           {"seed", 7},
                                           {"randomize_operators", true},
                                           {"store_standard_order", true}};
                    check(export_csv_text(s, output) == wide,
                          "run-order options changed wide layout");
                    for (const char *mode : native_ui::run_order_modes)
                        for (bool randomOps : {false, true})
                            for (bool standard : {false, true}) {
                                output["run_order"] = {{"mode", mode},
                                                       {"seed", 12345},
                                                       {"randomize_operators", randomOps},
                                                       {"store_standard_order", standard}};
                                std::string expected;
                                for (std::size_t f = 1; f < native_ui::csv_formats.size(); ++f) {
                                    output["format"] = native_ui::csv_formats[f];
                                    record_checks(s, output);
                                    const auto bytes = export_csv_text(s, output);
                                    check(bytes.compare(0, 3, "\xef\xbb\xbf") != 0,
                                          "long table BOM");
                                    for (std::size_t i = 0; i < bytes.size(); ++i)
                                        if (bytes[i] == '\n')
                                            check(i > 0 && bytes[i - 1] == '\r',
                                                  "non-CRLF newline");
                                    const auto rebuilt = parse_export_csv(bytes, s.config);
                                    check(rebuilt == s.matrix &&
                                              calculate(rebuilt, s.config) == s.metrics,
                                          "roundtrip precision or Xbar-R metrics changed");
                                    check(bytes == export_csv_text(s, output),
                                          "fixed seed is not reproducible");
                                    if (expected.empty())
                                        expected = bytes;
                                    else
                                        check(bytes == expected,
                                              "software format changed ordering/content");
                                    std::istringstream lines(bytes);
                                    std::string line;
                                    std::getline(lines, line);
                                    int records = 0;
                                    while (std::getline(lines, line)) {
                                        if (!line.empty() && line.back() == '\r')
                                            line.pop_back();
                                        const auto measurement = line.substr(line.rfind(',') + 1);
                                        const auto dot = measurement.find('.');
                                        check(dp == 0 ? dot == std::string::npos
                                                      : dot != std::string::npos &&
                                                            measurement.size() - dot - 1 ==
                                                                std::size_t(dp),
                                              "decimal places not retained");
                                        ++records;
                                    }
                                    check(records == o * t * p, "physical CSV record count");
                                }
                                ++configurations;
                            }
                    check(s.matrix == original, "randomization mutated original data");
                }
    auto s = study(3, 3, 10);
    for (const auto *invalid : {"+-1", "++1", "--1", "-+1", "+", "-"}) {
        rejects([&] { parse_number(invalid); });
        rejects([&] { parse_integer(invalid); });
        rejects([&] { native_ui::numeric(invalid, "种子", true); });
    }
    check(parse_number("+.25e-1") == .025 && parse_integer("+1") == 1 &&
              parse_number("-1") == -1, "valid signed values");
    check(sha256("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" &&
              sha256("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "system SHA-256 vectors");
    auto external_output = s.config["output"];
    external_output["format"] = "minitab";
    external_output["run_order"]["store_standard_order"] = true;
    std::string external = "\"Trial\",\"备注\",\"Measurement\",\"StdOrder\",\"Part\",\"Operator\"\r\n";
    for (const auto &r : export_records(s, external_output))
        external += "\"" + std::to_string(r.trial) + "\",\"a,\"\"b\"\"\n续行\",\"" +
                    fixed(r.measurement, 4) + "\",\"" + std::to_string(r.standard_order) +
                    "\",\"" + std::to_string(r.part) + "\",\"" +
                    char('A' + r.operator_number - 1) + "\"\r\n";
    check(parse_export_csv(external, s.config) == s.matrix,
          "quoted, reordered long CSV with multiline metadata");
    rejects([&] { parse_export_csv("Trial,Part,Measurement,Trial,Operator\n", s.config); });
    rejects([&] { parse_export_csv("Operator,Trial,Measurement\n", s.config); });
    const auto check_diagnostic = [&](const std::string &csv, const std::string &location) {
        try {
            parse_export_csv(csv, s.config);
        } catch (const ConfigError &e) {
            check(std::string(e.what()).find(location) != std::string::npos,
                  "CSV diagnostic lacks physical row/column");
            return;
        }
        throw std::runtime_error("malformed CSV was accepted");
    };
    check_diagnostic("Trial,Part,Measurement,Operator\n1,1,+-2,A\n", "第 2 行，第 3 列");
    check_diagnostic("Trial,Part,Measurement,Operator\n1,1,\"2,A\n", "第 2 行，第 3 列");
    check_diagnostic("1,2,3,4,5,6,7,8,+-9,10\n", "第 1 行，第 9 列");
    const auto labeled = export_csv_text(s, s.config["output"]);
    auto badHeader = labeled;
    badHeader.replace(badHeader.find(",2,"), 3, ",3,");
    rejects([&] { parse_export_csv(badHeader, s.config); });
    auto badLabel = labeled;
    badLabel[badLabel.find("A,")] = 'B';
    rejects([&] { parse_export_csv(badLabel, s.config); });
    // Identical matrix dimensions can represent different operator/trial designs.
    // Labels must match the requested design, not just its total row count.
    const auto alternate = study(2, 3, 5);
    rejects([&] {
        parse_export_csv(export_csv_text(alternate, alternate.config["output"]), config(3, 2, 5));
    });
    rejects([&] { parse_csv(",1,2\r\nA,1,2\r\nA,3,4\r\nB,5,6\r\n"); });
    rejects([&] { parse_csv(",1,2\r\nA,1\r\n"); });
    auto output = s.config["output"];
    output["format"] = "minitab";
    output["run_order"]["mode"] = "all";
    const auto automatic = export_csv_text(s, output);
    auto copy = s;
    for (std::size_t i = 1; i < native_ui::csv_formats.size(); ++i) {
        output["format"] = native_ui::csv_formats[i];
        check(export_csv_text(copy, output) == automatic,
              "blank seed rerandomized same study across software");
    }
    std::vector<std::string> parallel(8);
    std::vector<std::thread> jobs;
    for (int i = 0; i < 8; ++i)
        jobs.emplace_back([&, i] { parallel[i] = export_csv_text(copy, output); });
    for (auto &job : jobs)
        job.join();
    for (const auto &text : parallel)
        check(text == automatic, "parallel exports rerandomized study");
    output["run_order"]["seed"] = 12345;
    auto shuffled = ids(export_records(s, output));
    check(!std::is_sorted(shuffled.begin(), shuffled.end()), "all mode was not shuffled");
    output["run_order"]["mode"] = "within_operator";
    check(ids(export_records(s, output)) != shuffled, "all and within mode are the same schedule");
    for (const Json invalid :
         {Json(0), Json(-1), Json(1.5), Json("1"), Json(std::uint64_t(INT64_MAX) + 1)}) {
        auto bad = output;
        bad["run_order"]["seed"] = invalid;
        rejects([&] { normalize_output(bad); });
    }
    auto bad = output;
    bad["format"] = "unknown";
    rejects([&] { normalize_output(bad); });
    bad = output;
    bad["run_order"]["mode"] = "unknown";
    rejects([&] { normalize_output(bad); });
    bad = output;
    bad["run_order"]["randomize_operators"] = 1;
    rejects([&] { normalize_output(bad); });
    auto legacy = output;
    legacy.erase("silent");
    legacy.erase("comparison_report");
    legacy.erase("charts");
    legacy.erase("file_type");
    legacy["filename"] = "msa_data.csv";
    const auto upgraded = normalize_output(legacy);
    check(upgraded["file_type"] == "csv" && upgraded["filename"] == "msa_data" &&
              upgraded["silent"] == Json{{"csv", false}, {"charts", false}, {"comparison_report", false}} &&
              upgraded["comparison_report"] == Json{{"format", "pdf"}, {"directory", ""},
                                                    {"filename", "MSA_Comparison"}} &&
              upgraded["charts"] ==
                  Json{{"directory", ""}, {"filename", "MSA_Charts"}, {"style", "default"},
                       {"format", "png"}, {"scope", "current"}, {"columns", 2}, {"panel_width", 1800},
                       {"included", {"variance_components", "measurement_by_part",
                                     "measurement_by_operator", "range_chart", "mean_chart", "interaction"}}},
          "legacy output settings changed export behavior");
    auto oldStudy = s.config;
    oldStudy.erase("input");
    oldStudy["output"] = legacy;
    oldStudy["output"]["charts"] = {{"filename", "历史曲线"}, {"directory", "以往图表"}};
    const auto oldDraft = native_ui::from_config(oldStudy);
    check(oldDraft.importDirectory.empty() && oldDraft.importFilename.empty() &&
              oldDraft.chartFilename == "历史曲线" && oldDraft.chartDirectory == "以往图表" &&
              oldDraft.reportFilename == "MSA_Comparison" && oldDraft.charts.style == 0 &&
              oldDraft.charts.included == ChartPreferences{}.included,
          "old study lost default filenames or directory");
    legacy["silent"] = {{"charts", true}};
    legacy["comparison_report"] = {{"format", "html"}};
    const auto partial = normalize_output(legacy);
    check(partial["silent"]["charts"] == true && partial["silent"]["csv"] == false &&
              partial["silent"]["comparison_report"] == false &&
              partial["comparison_report"]["directory"] == "",
          "partial export preferences did not inherit missing fields");
    const auto chartsPrompt = native_ui::from_config(Json{{"output", legacy}});
    check(!chartsPrompt.silentCsv && chartsPrompt.silentCharts && !chartsPrompt.silentReport &&
              chartsPrompt.reportFormat == 1 &&
              native_ui::output_config(chartsPrompt)["silent"] == partial["silent"],
          "chart prompt affects another export type");
    for (const char *key : {"csv", "charts", "comparison_report"}) {
        bad = output;
        bad["silent"][key] = "false";
        rejects([&] { normalize_output(bad); });
    }
    bad = output;
    bad["comparison_report"]["format"] = "png";
    rejects([&] { normalize_output(bad); });
    for (const Json patch : {Json{{"style", "unknown"}}, Json{{"format", "html"}},
                             Json{{"scope", 1}}, Json{{"columns", 3}}, Json{{"columns", 1.0}},
                             Json{{"panel_width", "1200"}}, Json{{"panel_width", 640}},
                             Json{{"included", "histogram"}}, Json{{"included", {"unknown"}}},
                             Json{{"included", {"interaction", "interaction"}}}}) {
        bad = output;
        bad["charts"] = patch;
        rejects([&] { normalize_output(bad); });
    }
    auto emptyCombination = oldDraft;
    emptyCombination.charts.scope = 1;
    emptyCombination.charts.included.fill(false);
    check(native_ui::from_config(native_ui::to_config(emptyCombination)).charts.included ==
              emptyCombination.charts.included,
          "empty combination was not preserved");
    bad = output;
    bad["comparison_report"]["directory"] = nullptr;
    rejects([&] { normalize_output(bad); });
    bad["comparison_report"]["directory"] = std::string("reports\0hidden", 14);
    rejects([&] { normalize_output(bad); });
    for (const char *field : {"charts", "comparison_report"}) {
        for (const char *invalid : {"", ".", "..", "LPT².note", "con.txt", "A:B", "../report", "x/y"}) {
            bad = output;
            bad[field]["filename"] = invalid;
            rejects([&] { normalize_output(bad); });
        }
        bad = output;
        bad[field]["filename"] = std::string(252, 'a');
        rejects([&] { normalize_output(bad); });
    }
    check(normalize_import_filename("") == "" && normalize_import_filename("  ") == "" &&
              normalize_import_filename(" 来料复核.v2 ") == "来料复核.v2" &&
              normalize_import_filename("测量.CSV") == "测量" &&
              normalize_import_filename("测量.XLSX") == "测量",
          "import preset did not normalize the data basename");
    for (const char *invalid : {"../old.csv", "NUL.csv", ".csv", "x/y", "..", "a?.csv"})
        rejects([&] { normalize_config(Json{{"input", {{"filename", invalid}}}}); });
    rejects([&] { normalize_config(Json{{"input", {{"directory", false}}}}); });
    output["silent"] = {{"csv", false}, {"charts", true}, {"comparison_report", true}};
    output["file_type"] = "xlsx";
    output["charts"] = {{"directory", "质量部/图表"}, {"filename", "极差图.v2.JPG"},
                        {"style", "origin"}, {"format", "svg"}, {"scope", "combined"},
                        {"columns", 1}, {"panel_width", 2400}, {"included", {"histogram", "range_chart"}}};
    output["comparison_report"] = {{"format", "pdf"}, {"directory", "质量部/对照报告"},
                                   {"filename", " 研究对照.v2.HTML "}};
    auto saved = s.config;
    saved["input"] = {{"directory", "质量部/原始记录"}, {"filename", "来料"}};
    saved["output"] = normalize_output(output);
    const auto configPath = out / "saved-config.json";
    save_config(saved, configPath);
    check(load_config(configPath) == normalize_config(saved), "settings persistence");
    check(native_ui::output_config(native_ui::from_config(saved)) == saved["output"],
          "UI settings roundtrip");
    const auto restored = native_ui::from_config(load_config(configPath));
    check(!restored.silentCsv && restored.silentCharts && restored.silentReport &&
              restored.reportFormat == 0 && restored.dataFileType == 1 && restored.reportDirectory == "质量部/对照报告" &&
              restored.reportFilename == "研究对照.v2" && restored.chartDirectory == "质量部/图表" &&
              restored.chartFilename == "极差图.v2" && restored.importDirectory == "质量部/原始记录" &&
              restored.importFilename == "来料" && restored.charts.style == 1 &&
              restored.charts.format == 3 && restored.charts.scope == 1 &&
              restored.charts.columns == 0 && restored.charts.resolution == 2 &&
              restored.charts.included == std::array<bool, 7>{false, false, false, true, false, false, true} &&
              saved["output"]["charts"]["included"] == Json::array({"range_chart", "histogram"}) &&
              native_ui::to_config(restored) == normalize_config(saved),
          "saved GUI export preferences were lost");
    auto invalidReport = restored;
    invalidReport.reportFormat = -1;
    invalidReport.reportFilename = "../outside";
    invalidReport.chartFilename = "";
    invalidReport.importFilename = "CON.csv";
    rejects([&] { native_ui::output_config(invalidReport); });
    check(native_ui::csv_output_config(invalidReport) == native_ui::csv_output_config(restored),
          "unrelated unfinished fields affected CSV export");
    check(native_ui::to_config(restored, native_ui::ConfigUse::Generation) ==
              native_ui::to_config(invalidReport, native_ui::ConfigUse::Generation),
          "report format affected GUI generation snapshot");
    check(native_ui::to_config(restored, native_ui::ConfigUse::Analysis) ==
              native_ui::to_config(invalidReport, native_ui::ConfigUse::Analysis),
          "report format affected GUI analysis snapshot");
    for (const std::string seed : {"0", "-1", "1.5", "abc", "9223372036854775808"}) {
        auto draft = native_ui::from_config(saved);
        draft.runSeed = seed;
        rejects([&] { native_ui::output_config(draft); });
    }
    const auto defaults = native_ui::output_config(native_ui::Draft{});
    check(defaults["format"] == "default" && defaults["run_order"]["mode"] == "standard" &&
              defaults["run_order"]["seed"].is_null() &&
              !defaults["run_order"]["randomize_operators"].get<bool>() &&
              !defaults["run_order"]["store_standard_order"].get<bool>(),
          "restore defaults");
    const auto good = export_csv_text(s, output);
    auto lineEnd = good.find("\r\n", good.find("\r\n") + 2);
    rejects([&] {
        parse_export_csv(good + good.substr(good.find("\r\n") + 2, lineEnd - good.find("\r\n")),
                         s.config);
    });
    rejects(
        [&] { parse_export_csv(good.substr(0, good.rfind('\n', good.size() - 3) + 1), s.config); });
    auto generatedCfg = config(3, 3, 10);
    generatedCfg["generation"]["seed"] = 123;
    const auto before = generate(generatedCfg);
    export_csv_text(before, output);
    generatedCfg["output"]["silent"] = output["silent"];
    generatedCfg["input"] = saved["input"];
    generatedCfg["output"]["charts"] = output["charts"];
    generatedCfg["output"]["comparison_report"] = output["comparison_report"];
    const auto after = generate(generatedCfg);
    check(before.matrix == after.matrix && before.metrics == after.metrics &&
              before.attempts == after.attempts,
          "export preferences or RNG influenced data generation");
    atomic_text(out / "export-verification.json",
                Json{{"passed", true},
                     {"version", version},
                     {"reference_files", 6},
                     {"design_combinations", 24},
                     {"decimal_places_tested", {0, 4, 10}},
                     {"order_configurations", configurations},
                     {"software_formats", 5},
                     {"default_column_numbers_and_operator_labels_valid", true},
                     {"record_mapping_and_stdorder_valid", true},
                     {"fixed_and_automatic_seed_reuse_valid", true},
                     {"xbar_r_statistics_unchanged", true}}
                        .dump(2) +
                    "\n");
    std::cout
        << "PASS: six references; " << configurations
        << " design/precision/order combinations; five identical software formats; labeled default "
           "wide table; mapping, StdOrder, fixed/automatic seeds and Xbar-R invariance\n";
    return 0;
}
int same_study(const std::filesystem::path &configPath, const std::filesystem::path &out) {
    std::filesystem::create_directories(out);
    // Exactly one generation; all three exports consume this same Result object.
    const auto result = generate(load_config(configPath));
    const auto originalMatrix = result.matrix;
    const auto originalMetrics = result.metrics;
    atomic_text(out / "study.json", result.as_json().dump(2) + '\n');
    Json files = Json::array();
    for (const char *mode : native_ui::run_order_modes) {
        auto output = result.config["output"];
        output["format"] = "minitab";
        output["run_order"] = {{"mode", mode},
                               {"seed", 321},
                               {"randomize_operators", std::string(mode) == "within_operator"},
                               {"store_standard_order", true}};
        record_checks(result, output);
        const auto path = out / (std::string(mode) + ".csv");
        check(export_csv(result, path, output) == originalMetrics, "same-study export metrics");
        check(parse_export_csv(read_text(path), result.config) == originalMatrix,
              "same-study export matrix");
        check(result.matrix == originalMatrix && result.metrics == originalMetrics,
              "export changed immutable study");
        files.push_back({{"mode", mode},
                         {"file", path.filename().u8string()},
                         {"output", output},
                         {"standard_order_ids", ids(export_records(result, output))}});
    }
    atomic_text(out / "exports.json", Json{{"generation_calls", 1},
                                           {"source_matrix_unchanged", true},
                                           {"native_metrics_unchanged", true},
                                           {"files", files}}
                                              .dump(2) +
                                          '\n');
    std::cout << "PASS: one generated study exported in three run orders\n";
    return 0;
}
} // namespace
#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
#else
int main(int argc, char **argv) {
#endif
    try {
        if (argc == 4 && std::filesystem::path(argv[1]) == "--same-study")
            return same_study(std::filesystem::path(argv[2]), std::filesystem::path(argv[3]));
        if (argc != 3)
            return 2;
        return run(std::filesystem::path(argv[1]), std::filesystem::path(argv[2]));
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
