#include "msa/core.hpp"
#include "ui_model.hpp"
#include <algorithm>
#include <iostream>
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
int run(const std::filesystem::path &fixtures, const std::filesystem::path &out) {
    std::filesystem::create_directories(out);
    auto reference = default_reference(fixtures / std::filesystem::u8path("默认.csv"));
    auto refCfg = config(3, 3, 10);
    const std::vector<std::string> referenceFiles{"Minitab.csv", "SPSS.csv", "JMP 19.csv",
                                                  "Origin.csv", "Matlab.csv"};
    std::string first;
    for (const auto &file : referenceFiles) {
        const auto bytes = read_text(fixtures / file);
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
    auto saved = s.config;
    saved["output"] = normalize_output(output);
    const auto configPath = out / "saved-config.json";
    save_config(saved, configPath);
    check(load_config(configPath) == normalize_config(saved), "settings persistence");
    check(native_ui::output_config(native_ui::from_config(saved)) == saved["output"],
          "UI settings roundtrip");
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
    const auto after = generate(generatedCfg);
    check(before.matrix == after.matrix && before.metrics == after.metrics &&
              before.attempts == after.attempts,
          "export RNG influenced data generation");
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
