#include "msa/core.hpp"
#include "msa/task.hpp"
#include "ui_model.hpp"
#include <chrono>
#include <ctime>
#include <cmath>
#include <fstream>
#include <iostream>
#include <locale>
#include <limits>
#include <mutex>
#include <set>
#include <thread>
#ifdef _WIN32
#include "platform/clipboard_win.hpp"
#endif

using namespace msa;
namespace {
int passed = 0, failed = 0;
Json report = {{"tests", Json::array()}, {"baseline", Json::array()}};
void require(bool value, const std::string &why = "assertion failed") {
    if (!value)
        throw std::runtime_error(why);
}
template <class E, class F> void throws(F f) {
    bool caught = false;
    try {
        f();
    } catch (const E &) {
        caught = true;
    }
    require(caught, "expected error not thrown");
}
template <class F> void test(const std::string &name, F fn) {
    try {
        fn();
        ++passed;
        report["tests"].push_back({{"name", name}, {"passed", true}});
        std::cout << "PASS " << name << '\n';
    } catch (const std::exception &e) {
        ++failed;
        report["tests"].push_back({{"name", name}, {"passed", false}, {"error", e.what()}});
        std::cerr << "FAIL " << name << ": " << e.what() << '\n';
    }
}
Json example(int o = 3, int r = 3, int p = 10) {
    auto c = default_config();
    c["design"] = {{"operators", o}, {"replicates", r}, {"parts", p}};
    c["specification"]["lsl"] = 9;
    c["specification"]["usl"] = 11;
    c["generation"]["seed"] = 123;
    int i = 0;
    for (int v : {6, 8, 10, 10, 14})
        c["targets"][metric_keys[i++]] = {{"mode", "target"}, {"value", v}};
    return c;
}
void expect_near(double actual, double expected) {
    require(std::abs(actual - expected) <= 1e-11 + 1e-9 * std::abs(expected),
            "baseline numerical mismatch " + std::to_string(actual) + " / " +
                std::to_string(expected));
}
void clear_targets(Json &c) {
    for (auto key : metric_keys)
        c["targets"][key] = nullptr;
}
struct Comma : std::numpunct<char> {
    char do_decimal_point() const override { return ','; }
};
} // namespace
int run_tests(int argc, const std::vector<std::string> &argv) {
    if (argc == 4 && argv[1] == "--probe") {
        auto input = Json::parse(read_text(std::filesystem::u8path(argv[2])));
        Json out = Json::object();
        for (auto v : input["format"])
            out["format"].push_back(fixed(v[0], v[1]));
        for (auto v : input["grid"])
            out["grid"].push_back(grid_bound(v[0], v[1], v[2]));
        for (auto v : input["sum"])
            out["sum"].push_back(accurate_sum(v.get<std::vector<double>>()));
        for (auto v : input["calculate"])
            out["calculate"].push_back(calculate(v["matrix"].get<Matrix>(), v["config"]));
        atomic_text(std::filesystem::u8path(argv[3]), out.dump());
        return 0;
    }
    // CTest passes UTF-8 paths; use u8path rather than the Windows ANSI codepage.
    if (argc < 3) {
        std::cerr << "msa_tests BASELINE OUTPUT_DIR\n";
        return 2;
    }
    auto fixture = Json::parse(read_text(std::filesystem::u8path(argv[1])));
    auto dir = std::filesystem::u8path(argv[2]);
    std::filesystem::create_directories(dir);
    test("all_192_minitab_matrices_and_24_designs", [&] {
        require(fixture["cases"].size() == 192);
        std::set<std::string> designs;
        for (const auto &v : fixture["cases"]) {
            auto c = v["config"], native = v["native"];
            auto m = calculate(v["matrix"].get<Matrix>(), c);
            require(qualified(m, c), "baseline constraints");
            designs.insert(c["design"].dump());
            // Fixture array order is GRR, EV, AV, PV, TV (not display order).
            std::array<std::string, 5> keys{"GRR", "EV", "AV", "PV", "TV"};
            for (int i = 0; i < 5; ++i) {
                expect_near(m[keys[i]], native["sd"][i]);
                expect_near(m[keys[i]].get<double>() *
                                c["analysis"]["study_multiplier"].get<double>(),
                            native["studyvar"][i]);
            }
            for (int i = 0; i < 3; ++i)
                expect_near(m[std::array<std::string, 3>{"GRR_pct", "EV_pct", "AV_pct"}[i]],
                            native["percent"][i]);
            require(m["ndc"] == native["ndc"][0], "ndc mismatch");
            if (!native["tolerance"].empty())
                expect_near(m["PT_pct"], native["tolerance"][0]);
            report["baseline"].push_back({{"id", v["id"]}, {"metrics", m}, {"passed", true}});
        }
        require(designs.size() == 24);
        report["baseline_designs"] = designs.size();
    });
    test("native_generation_all_24_designs_csv_and_fixed_seed", [&] {
        for (int o : {2, 3})
            for (int r : {2, 3})
                for (int p = 5; p <= 10; ++p) {
                    auto c = example(o, r, p);
                    auto a = generate(c), b = generate(c);
                    require(a.matrix == b.matrix && a.metrics == b.metrics &&
                                a.attempts == b.attempts,
                            "seed reproducibility");
                    require(a.attempts >= 121 &&
                            a.attempts <= c["generation"]["max_attempts"].get<int>());
                    auto path = dir / std::filesystem::u8path("组合_" + std::to_string(o) + "_" +
                                                              std::to_string(r) + "_" +
                                                              std::to_string(p) + " 数据.csv");
                    auto m = export_csv(a, path);
                    require(m == a.metrics);
                }
    });
    test("overlapping_bands_not_nominal_conflict", [] {
        auto c = example();
        c["targets"]["GRR_pct"]["value"] = 10.15;
        c["targets"]["ndc"] = nullptr;
        require(qualified(generate(c).metrics, c));
    });
    test("proven_conflicts_distinct_from_search_exhaustion", [] {
        for (auto change : std::vector<Json>{
                 {{"GRR_pct", {{"mode", "target"}, {"value", 20}}}},
                 {{"ndc", {{"mode", "target"}, {"value", 5}}}},
                 {{"GRR_pct", {{"mode", "target"}, {"value", 0}, {"tolerance_pp", 0}}}}}) {
            auto c = example();
            c["targets"].update(change);
            throws<InfeasibleError>([&] { generate(c); });
        }
    });
    test("acceptance_four_modes_null_and_no_extra_tolerance", [] {
        auto c = example();
        c["targets"]["AV_pct"] = {{"mode", "target"}, {"value", 0}, {"tolerance_pp", .5}};
        require(acceptance(c, "AV_pct") == Bounds({0, .5}));
        c["targets"]["ndc"] = {{"mode", "target"}, {"value", 10}, {"tolerance", 1}};
        require(acceptance(c, "ndc") == Bounds({9, 11}));
        c["targets"]["GRR_pct"] = {{"mode", "between"}, {"min", 5}, {"max", 10}};
        require(acceptance(c, "GRR_pct") == Bounds({5, 10}));
        Json m = {{"EV_pct", 6.1}, {"AV_pct", .5}, {"GRR_pct", 10}, {"PT_pct", 10}, {"ndc", 11}};
        for (auto v : checks(m, c))
            require(v != false);
        m["EV_pct"] = 6.100001;
        require(checks(m, c)["EV_pct"] == false);
        c["targets"]["EV_pct"] = nullptr;
        require(checks(m, c)["EV_pct"].is_null());
        c["targets"]["GRR_pct"] = {{"mode", "at_least"}, {"value", 12}};
        require(acceptance(c, "GRR_pct") == Bounds({12, 100}));
        c["targets"]["PT_pct"] = {{"mode", "at_most"}, {"value", 20}};
        require(acceptance(c, "PT_pct") == Bounds({0, 20}));
    });
    test("ndc_tolerance_and_ndc_one", [] {
        auto c = example();
        c["targets"]["ndc"] = {{"mode", "target"}, {"value", 13}, {"tolerance", 1}};
        require(qualified(generate(c).metrics, c));
        clear_targets(c);
        c["targets"]["ndc"] = {{"mode", "target"}, {"value", 1}};
        require(generate(c).metrics["ndc"] == 1);
        c["targets"]["AV_pct"] = {{"mode", "target"}, {"value", 0}, {"tolerance_pp", .1}};
        require(qualified(generate(c).metrics, c));
    });
    test("zero_components_zero_tv_and_uncomputable", [] {
        auto c = example();
        clear_targets(c);
        for (auto k : {"EV_pct", "AV_pct", "GRR_pct", "PT_pct"})
            c["targets"][k] = {{"mode", "target"}, {"value", 0}, {"tolerance_pp", 0}};
        auto a = generate(c);
        require(a.metrics["GRR"] == 0 && a.metrics["ndc"].is_null() &&
                a.metrics["TV"].get<double>() > 0);
        auto m = calculate(Matrix(9, std::vector<double>(10, 0)), c);
        require(!qualified(m, c) && m["EV_pct"].is_null() && m["PT_pct"] == 0);
        throws<std::invalid_argument>([&] { calculate(Matrix(8, std::vector<double>(10, 0)), c); });
    });
    test("one_sided_no_specs_reference_and_relaxed", [] {
        for (int mode = 0; mode < 3; ++mode) {
            auto c = example();
            clear_targets(c);
            c["generation"]["reference_tv_sd"] = .1;
            if (mode == 0) {
                c["specification"]["lsl"] = nullptr;
                c["specification"]["usl"] = -2;
            }
            if (mode == 1) {
                c["specification"]["lsl"] = 2;
                c["specification"]["usl"] = nullptr;
            }
            if (mode == 2) {
                c["specification"]["lsl"] = nullptr;
                c["specification"]["usl"] = nullptr;
                c["specification"]["compliance"] = "relaxed";
            }
            auto a = generate(c);
            require(qualified(a.metrics, c) && a.metrics["center_error"].is_null());
            if (mode == 2)
                require(a.metrics["PT_pct"].is_null());
            else {
                const double limit = mode == 0 ? -2 : 2;
                const double width = 2 * std::abs(limit - a.metrics["mean"].get<double>());
                expect_near(a.metrics["PT_pct"], 100 * 6 * a.metrics["GRR"].get<double>() / width);
            }
            if (mode == 0)
                require(a.metrics["maximum"].get<double>() <= -2);
            if (mode == 1)
                require(a.metrics["minimum"].get<double>() >= 2);
        }
    });
    test("required_parameters", [] {
        throws<ConfigError>([] { validate_request(default_config()); });
        auto c = example();
        c["specification"]["usl"] = nullptr;
        throws<ConfigError>([&] { validate_request(c); });
        clear_targets(c);
        throws<ConfigError>([&] { validate_request(c); });
        c = example();
        c["generation"]["scale_mode"] = "reference_tv_sd";
        throws<ConfigError>([&] { validate_request(c); });
    });
    test("strict_validation_unknown_fields_and_json_roundtrip", [&] {
        for (auto edit : std::vector<Json>{
                 {{"design", {{"operators", true}}}},
                 {{"generation", {{"seed", 1.2}}}},
                 {{"generation", {{"center", "NaN"}}}},
                 {{"analysis", {{"study_multiplier", 0}}}},
                 {{"targets", {{"ndc", {{"mode", "target"}, {"value", 5.5}}}}}},
                 {{"targets", {{"EV_pct", {{"mode", "target"}, {"value", 101}}}}}},
                 {{"targets",
                   {{"EV_pct", {{"mode", "at_most"}, {"value", 5}, {"tolerance_pp", 1}}}}}},
                 {{"unknown", 1}},
                 {{"generation", {{"surprise", 0}}}},
                 {{"design", nullptr}},
                 {{"output", {{"filename", "a/b.csv"}}}},
                 {{"schema_version", true}},
                 {{"generation", {{"seed", 18446744073709551615ULL}}}}}) {
            auto c = example();
            c.update(edit);
            throws<ConfigError>([&] { normalize_config(c); });
        }
        auto path = dir / std::filesystem::u8path("中文 空格/config.json");
        save_config(example(), path);
        require(load_config(path) == example());
        atomic_text(path, "{broken");
        throws<Json::parse_error>([&] { load_config(path); });
        require(read_text(path) == "{broken");
        auto init = dir / "init.json";
        if (std::filesystem::exists(init))
            std::filesystem::remove(init);
        require(load_config(init) == default_config());
        require(normalize_config(Json::object()) == default_config());
    });
    test("precision_failure_budget_failure_and_decimal_grid", [] {
        auto c = example();
        c["specification"]["lsl"] = .00001;
        c["specification"]["usl"] = .00002;
        throws<InfeasibleError>([&] { generate(c); });
        c = example();
        c["generation"]["max_attempts"] = 1;
        c["generation"]["decimal_places"] = 0;
        throws<SearchError>([&] { generate(c); });
        require(grid_bound(.07, 2, true) == .07);
        require(grid_bound(-.071, 2, true) == -.07 && grid_bound(-.071, 2, false) == -.08);
        require(grid_bound(.071, 2, false) == .07 && grid_bound(.071, 2, true) == .08);
        require(fixed(2.675, 2) == "2.67" && fixed(2.5, 0) == "2" && fixed(3.5, 0) == "4" &&
                fixed(-2.5, 0) == "-2");
        require(accurate_sum({1e16, 1, -1e16}) == 1);
    });
    test("center_priorities_multiplier_and_outside_center", [] {
        auto c = example();
        c["generation"]["center"] = 10.2;
        auto a = generate(c);
        require(std::abs(a.metrics["center_error"].get<double>()) < .0001);
        auto d = c;
        d["analysis"]["study_multiplier"] = 5.15;
        auto b = calculate(a.matrix, d);
        require(b["GRR_pct"] == a.metrics["GRR_pct"]);
        expect_near(b["PT_pct"].get<double>() / a.metrics["PT_pct"].get<double>(), 5.15 / 6);
        c["generation"]["center"] = 20;
        require(generate(c).metrics["mean"].get<double>() < 20);
        c["generation"]["priority"] = "indicators";
        require(qualified(generate(c).metrics, c));
        c["specification"]["compliance"] = "relaxed";
        require(generate(c).metrics["above_spec"].get<int>() > 0);
    });
    test("csv_actual_bytes_locale_roundtrip_and_failure_cleanup", [&] {
        auto a = generate(example());
        auto previous = std::locale();
        std::locale::global(std::locale(std::locale::classic(), new Comma));
        auto path = dir / std::filesystem::u8path("区域 中文.csv");
        export_csv(a, path);
        std::locale::global(previous);
        std::ifstream f(path, std::ios::binary);
        std::string s((std::istreambuf_iterator<char>(f)), {});
        require(s.substr(0, 3) == "\xef\xbb\xbf");
        require(read_csv(path) == a.matrix);
        require(s.find('\r') != s.npos);
        auto bad = a;
        bad.config["generation"]["decimal_places"] = 0;
        auto target = dir / "must-not-overwrite.csv";
        atomic_text(target, "keep");
        throws<std::invalid_argument>([&] { export_csv(bad, target); });
        require(read_text(target) == "keep");
        auto folder = dir / "target-directory";
        std::filesystem::create_directories(folder);
        throws<std::runtime_error>([&] { atomic_text(folder, "x"); });
        for (auto &e : std::filesystem::directory_iterator(dir))
            require(e.path().extension() != ".tmp");
        throws<ConfigError>([] { parse_csv("1,bad\r\n"); });
    });
    test("snapshot_cancel_and_restart_background_task", [] {
        std::atomic<bool> cancel{true};
        throws<CancelledError>([&] { generate(example(), &cancel); });
        SearchTask task;
        auto c = example();
        task.start(c);
        c["generation"]["center"] = 777;
        task.cancel();
        while (task.view().status == TaskStatus::Running ||
               task.view().status == TaskStatus::Cancelling)
            std::this_thread::yield();
        require(task.view().status == TaskStatus::Cancelled);
        task.start(example());
        while (task.view().status == TaskStatus::Running)
            std::this_thread::yield();
        auto v = task.view();
        require(v.status == TaskStatus::Success && v.result &&
                v.result->config["generation"]["center"].is_null());
    });
    test("gui_model_formal_schema_roundtrip_and_zero_override", [] {
        auto c = example();
        c["targets"]["EV_pct"]["tolerance_pp"] = 0;
        auto d = native_ui::from_config(c);
        require(native_ui::to_config(d) == c);
        d.targets[0].mode = 0;
        require(native_ui::to_config(d)["targets"]["EV_pct"].is_null());
        d.seed = "9223372036854775808";
        throws<ConfigError>([&] { native_ui::to_config(d); });
    });
    test("budget_timeout_progress_and_cancel_during_generation", [] {
        auto c = example();
        clear_targets(c);
        c["targets"]["ndc"] = {{"mode", "at_least"}, {"value", 1000000}};
        c["generation"]["max_attempts"] = 1000000;
        c["generation"]["timeout_seconds"] = .01;
        throws<SearchError>([&] { generate(c); });
        std::atomic<bool> cancelled{false};
        auto easy = example();
        int last = 0;
        throws<CancelledError>([&] {
            generate(easy, &cancelled, [&](int n, double elapsed, bool) {
                require(n >= last && elapsed >= 0);
                last = n;
                cancelled = true;
            });
        });
        require(last > 0);
    });
    test("silent_export_paths_and_existing_file_replacement", [&] {
        auto cfg = example();
        require(configured_csv_path(cfg) ==
                (config_path().parent_path() / "msa_data.csv").lexically_normal());
        cfg["output"]["directory"] = "relative/subdir";
        cfg["output"]["filename"] = "中文 数据.csv";
        require(configured_csv_path(cfg) == (config_path().parent_path() / "relative/subdir" /
                                             std::filesystem::u8path("中文 数据.csv"))
                                                .lexically_normal());
        cfg["output"]["directory"] =
            (std::filesystem::absolute(dir) / std::filesystem::u8path("静默 输出")).u8string();
        auto path = configured_csv_path(cfg);
        auto result = generate(cfg);
        atomic_text(path, "old file");
        require(export_csv(result, path) == result.metrics);
        require(read_csv(path) == result.matrix);
        require(export_csv(result, path) == result.metrics);
    });
    test("filename_variables_local_time_result_values_and_validation", [&] {
        auto cfg = example();
        auto result = generate(cfg);
        auto autoSeedConfig = cfg;
        autoSeedConfig["generation"]["seed"] = nullptr;
        const auto autoSeedResult = generate(autoSeedConfig);
        require(
            render_filename("[随机种子].csv", autoSeedResult, std::chrono::system_clock::now()) ==
            std::to_string(autoSeedResult.seed) + ".csv");
        std::tm local{};
        local.tm_year = 126;
        local.tm_mon = 9;
        local.tm_mday = 24;
        local.tm_hour = 22;
        local.tm_min = 8;
        local.tm_sec = 12;
        local.tm_isdst = -1;
        const auto when = std::chrono::system_clock::from_time_t(std::mktime(&local)) +
                          std::chrono::milliseconds(37);
        require(render_filename("MSA_[YYYY]-[MM]-[DD]_[H]-[M]-[S]_[MS].csv", result, when) ==
                "MSA_2026-10-24_22-08-12_037.csv");
        require(render_filename("[YY]_[DATE]_[TIME]_[DATETIME].CSV", result, when) ==
                "26_2026-10-24_22-08-12_2026-10-24_22-08-12.CSV");
        const auto epochTime =
            std::chrono::system_clock::from_time_t(1700000000) + std::chrono::milliseconds(999);
        require(render_filename("[TIMESTAMP]_[时间戳]_[MS].csv", result, epochTime) ==
                "1700000000_1700000000_999.csv");
        require(render_filename("[TIMESTAMP].csv", result,
                                std::chrono::system_clock::from_time_t(0)) == "0.csv");
        require(render_filename("[[已复核]]_[人员数量]_[REPLICATES]_[产品件数]_[USL]_[LSL]_["
                                "研究变异倍数]_[SEED]_[小数位数]_[VERSION].csv",
                                result, when) ==
                std::string("[已复核]_3_3_10_11_9_6_123_4_") + ui_version + ".csv");
        result.metrics["GRR_pct"] = 999; // Names must use actual data, not a mutable cached label.
        const auto actual = calculate(result.matrix, cfg);
        require(render_filename("[EV_PCT]_[AV_PCT]_[GRR_PCT]_[PT_PCT]_[NDC].csv", result, when) ==
                fixed(actual["EV_pct"], 2) + "_" + fixed(actual["AV_pct"], 2) + "_" +
                    fixed(actual["GRR_pct"], 2) + "_" + fixed(actual["PT_pct"], 2) + "_" +
                    actual["ndc"].dump() + ".csv");
        require(render_filename("[CENTER].csv", result, when) == actual["mean"].dump() + ".csv" ||
                render_filename("[CENTER].csv", result, when) == "10.csv");
        result.config["specification"]["lsl"] = nullptr;
        result.config["specification"]["usl"] = nullptr;
        require(render_filename("[USL]_[LSL]_[PT_PCT].csv", result, when) == "NA_NA_NA.csv");
        for (const auto &v : filename_variables()) {
            validate_filename_template(std::string("[") + v.name + "].csv");
            if (*v.alias)
                require(render_filename(std::string("[") + v.alias + "].csv", result, when) ==
                        render_filename(std::string("[") + v.name + "].csv", result, when));
        }
        for (const std::string bad :
             {"[HH].csv", "[mm].csv", "[unknown].csv", "[].csv", "[YYYY.csv", "[A[B]].csv",
              "a].csv", "../a.csv", "CON.csv", "LPT1.data.csv", "COM¹.csv", "x\t.csv", "a.txt"})
            throws<ConfigError>([&] { validate_filename_template(bad); });
        throws<ConfigError>([&] { validate_filename_template(std::string(252, 'a') + ".csv"); });
        throws<ConfigError>(
            [&] { render_filename(std::string(248, 'a') + "[VERSION].csv", result, when); });
        auto invalid = cfg;
        invalid["output"]["existing_file"] = "invalid";
        throws<ConfigError>([&] { normalize_config(invalid); });
        require(normalize_config(Json::object())["output"]["existing_file"] == "number");
        auto draft = native_ui::from_config(cfg);
        draft.existingFile = 1;
        require(native_ui::from_config(native_ui::to_config(draft)).existingFile == 1);
    });
    test("configured_export_numbering_overwrite_unicode_and_parallel_safety", [&] {
        auto result = generate(example());
        auto settings = result.config["output"];
        const auto folder =
            std::filesystem::absolute(dir) /
            ("naming-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        settings["directory"] = folder.u8string();
        settings["filename"] = "中文_[[报告]]_[人员数量]_[SEED].csv";
        const auto first = export_configured_csv(result, settings);
        const auto original = read_text(first);
        const auto second = export_configured_csv(result, settings);
        require(first.filename().u8string() == "中文_[报告]_3_123.csv");
        require(second.filename().u8string() == "中文_[报告]_3_123_001.csv");
        require(read_text(first) == original && read_csv(second) == result.matrix);
        settings["existing_file"] = "overwrite";
        atomic_text(first, "old");
        require(export_configured_csv(result, settings) == first &&
                read_csv(first) == result.matrix);
        throws<ConfigError>([&] {
            export_configured_csv(result, settings, std::chrono::system_clock::now(), first);
        });
        settings["existing_file"] = "number";
        std::mutex mutex;
        std::set<std::filesystem::path> paths;
        std::atomic<int> failures{0};
        std::vector<std::thread> jobs;
        for (int i = 0; i < 8; ++i)
            jobs.emplace_back([&] {
                try {
                    auto path = export_configured_csv(result, settings);
                    require(read_csv(path) == result.matrix);
                    std::lock_guard<std::mutex> lock(mutex);
                    paths.insert(path);
                } catch (...) {
                    ++failures;
                }
            });
        for (auto &job : jobs)
            job.join();
        require(failures == 0 && paths.size() == 8 && read_text(first) == original);
        require(result.config["output"]["filename"] == "msa_data.csv");
    });
    test("long_filename_component_atomic_export_and_numbering", [&] {
        auto folder = std::filesystem::absolute(dir) /
                      ("long-name-" + std::to_string(
                                          std::chrono::steady_clock::now().time_since_epoch().count()));
#ifdef _WIN32
        // 排除旧式 MAX_PATH 总长限制，只检查合法文件名组件与临时名称的关系。
        auto native = folder.native();
        if (native.rfind(L"\\\\?\\", 0) != 0)
            folder = native.rfind(L"\\\\", 0) == 0 ? L"\\\\?\\UNC\\" + native.substr(2)
                                                      : L"\\\\?\\" + native;
#endif
        std::filesystem::create_directories(folder);
        const auto control = folder / (std::string(251, 'b') + ".csv");
        {
            // 独立证明文件系统可接受255字符名称，不把路径环境失败误当原子写入缺陷。
            std::ofstream direct(control, std::ios::binary);
            direct << "filesystem-control";
            direct.close();
            require(bool(direct) && read_text(control) == "filesystem-control",
                    "文件系统不接受255字符控制名称");
        }
        atomic_text(control, "atomic-replacement");
        require(read_text(control) == "atomic-replacement");
        throws<FileExistsError>([&] { atomic_text(control, "lost", ExistingFile::Preserve); });
        require(read_text(control) == "atomic-replacement");

        auto result = generate(example());
        auto output = result.config["output"];
        output["directory"] = folder.u8string();
        output["filename"] = std::string(247, 'a') + ".csv";
        const auto first = export_configured_csv(result, output);
        const auto second = export_configured_csv(result, output);
        require(first.filename().u8string().size() == 251 &&
                    second.filename().u8string().size() == 255 &&
                    read_csv(first) == result.matrix && read_csv(second) == result.matrix,
                "长文件名导出或追加序号回读失败");
        const auto original = read_text(first);
        std::mutex mutex;
        std::set<std::filesystem::path> paths;
        std::atomic<int> failures{0};
        std::vector<std::thread> jobs;
        for (int i = 0; i < 4; ++i)
            jobs.emplace_back([&] {
                try {
                    auto path = export_configured_csv(result, output);
                    require(path.filename().u8string().size() == 255 &&
                            read_csv(path) == result.matrix);
                    std::lock_guard<std::mutex> lock(mutex);
                    paths.insert(path);
                } catch (...) {
                    ++failures;
                }
            });
        for (auto &job : jobs)
            job.join();
        require(failures == 0 && paths.size() == 4 && read_text(first) == original,
                "长文件名并发导出覆盖旧文件或丢失结果");
        for (const auto &entry : std::filesystem::directory_iterator(folder))
            require(entry.path().filename().u8string().find(".msa-") != 0,
                    "长文件名导出遗留临时文件");
    });
    test("atomic_preserve_concurrent_commit_and_csv_policy", [&] {
        auto target = dir / "atomic-preserve.txt";
        std::filesystem::remove(target);
        std::atomic<int> ready{0}, winners{0}, conflicts{0}, unexpected{0};
        std::atomic<bool> go{false};
        std::vector<std::thread> workers;
        for (int i = 0; i < 8; ++i)
            workers.emplace_back([&, i] {
                ++ready;
                while (!go.load())
                    std::this_thread::yield();
                try {
                    atomic_text(target, "winner-" + std::to_string(i), ExistingFile::Preserve);
                    ++winners;
                } catch (const FileExistsError &) {
                    ++conflicts;
                } catch (...) {
                    ++unexpected;
                }
            });
        while (ready != 8)
            std::this_thread::yield();
        go = true;
        for (auto &worker : workers)
            worker.join();
        require(winners == 1 && conflicts == 7 && unexpected == 0, "atomic no-replace commit");
        const auto saved = read_text(target);
        throws<FileExistsError>([&] { atomic_text(target, "lost", ExistingFile::Preserve); });
        require(read_text(target) == saved);
        atomic_text(target, "replacement");
        require(read_text(target) == "replacement");
        auto result = generate(example());
        auto csv = dir / "policy.csv";
        atomic_text(csv, "keep");
        throws<FileExistsError>([&] { export_csv(result, csv, ExistingFile::Preserve); });
        require(read_text(csv) == "keep");
        export_csv(result, csv, ExistingFile::Replace);
        require(read_csv(csv) == result.matrix);
        std::filesystem::remove(csv);
        export_csv(result, csv, ExistingFile::Preserve);
        require(read_csv(csv) == result.matrix);
        for (auto &entry : std::filesystem::directory_iterator(dir))
            require(entry.path().extension() != ".tmp", "temporary file leaked");
    });
    test("nonfinite_pt_rejected_before_success_export_and_background_result", [&] {
        auto c = default_config();
        c["specification"] = {{"lsl", 0}, {"usl", 1e-300}, {"compliance", "relaxed"}};
        c["generation"]["scale_mode"] = "reference_tv_sd";
        c["generation"]["reference_tv_sd"] = 1e12;
        c["generation"]["seed"] = 123;
        validate_request(c); // Legal inputs; failure is numerical overflow, not infeasibility.
        throws<std::overflow_error>([&] { generate(c); });
        SearchTask task;
        task.start(c);
        while (task.view().status == TaskStatus::Running)
            std::this_thread::yield();
        auto view = task.view();
        require(view.status == TaskStatus::Failed && !view.result);
        auto normal = generate(example());
        auto m = normal.metrics;
        m["PT_pct"] = std::numeric_limits<double>::infinity();
        require(!qualified(m, normal.config));
        normal.config = c;
        for (auto &row : normal.matrix)
            for (auto &value : row)
                value *= 1e10;
        auto output = dir / "overflow-must-preserve.csv";
        atomic_text(output, "keep");
        throws<std::overflow_error>([&] { export_csv(normal, output); });
        require(read_text(output) == "keep");
    });
    test("polish_budget_counts_skipped_candidates_and_is_reproducible", [] {
        auto c = example();
        c["specification"]["lsl"] = 0;
        c["specification"]["usl"] = 1;
        c["generation"]["decimal_places"] = 6;
        c["generation"]["seed"] = 6;
        c["targets"]["PT_pct"]["value"] = 17.5;
        c["targets"]["ndc"] = nullptr;
        int first = -1, finalCount = -1;
        auto a = generate(c, nullptr, [&](int n, double, bool found) {
            if (found && first < 0)
                first = n;
            finalCount = n;
        });
        require(first == 1 && a.attempts == 121 && finalCount == 121);
        auto b = generate(c);
        require(a.matrix == b.matrix && a.metrics == b.metrics && a.attempts == b.attempts);
        c["generation"]["max_attempts"] = 10;
        require(generate(c).attempts == 10);
    });
    test("review_format_finite_null_zero_and_overflow_are_distinct", [] {
        require(native_ui::review_number(nullptr, 2) == "不可计算");
        require(native_ui::review_number(0, 2) == "0.00");
        require(native_ui::review_number(12.3456, 2) == "12.35");
        require(native_ui::review_number(14, 0) == "14");
        require(native_ui::review_number("bad", 2) == "数值异常");
        for (double x :
             {std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
              std::numeric_limits<double>::quiet_NaN()})
            require(native_ui::review_number(x, 2) == "数值异常");
        require(fixed(12.3456, 4) == "12.3456"); // Preview / CSV precision unchanged.
    });
#ifdef _WIN32
    test("clipboard_fault_injection_cleanup_and_success_ownership", [] {
        struct Fake {
            int failure, closed = 0, freed = 0, unlocked = 0, emptyCalls = 0, putCalls = 0;
            std::array<wchar_t, 128> buffer{};
            bool open(HWND) { return failure != 1; }
            void close() { ++closed; }
            HGLOBAL allocate(SIZE_T) {
                return failure == 2 ? nullptr : reinterpret_cast<HGLOBAL>(1);
            }
            void release(HGLOBAL) { ++freed; }
            void *lock(HGLOBAL) { return failure == 3 ? nullptr : buffer.data(); }
            void unlock(HGLOBAL) { ++unlocked; }
            bool empty() {
                ++emptyCalls;
                return failure != 4;
            }
            bool put(HGLOBAL) {
                ++putCalls;
                return failure != 5;
            }
        };
        for (int failure = 0; failure <= 5; ++failure) {
            Fake api{failure};
            if (failure)
                throws<std::exception>(
                    [&] { platform::writeClipboard(nullptr, L"中文 日志", api); });
            else {
                platform::writeClipboard(nullptr, L"中文 日志", api);
                require(std::wstring(api.buffer.data()) == L"中文 日志");
            }
            require(api.closed == (failure == 1 ? 0 : 1));
            require(api.freed == (failure >= 3 ? 1 : 0));
            require(api.unlocked == (failure == 0 || failure >= 4 ? 1 : 0));
            require(api.putCalls == (failure == 0 || failure == 5 ? 1 : 0));
        }
    });
#endif
    report["version"] = version;
    report["passed"] = passed;
    report["failed"] = failed;
    report["comparison_tolerance"] =
        "abs(actual - expected) <= 1e-11 + 1e-9 * abs(expected); ndc exact";
    atomic_text(dir / "native-regression.json", report.dump(2) + '\n');
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
    try {
        std::vector<std::string> args;
        for (int i = 0; i < argc; ++i)
            args.push_back(std::filesystem::path(argv[i]).u8string());
        return run_tests(argc, args);
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 2;
    }
}
#else
int main(int argc, char **argv) {
    try {
        return run_tests(argc, std::vector<std::string>(argv, argv + argc));
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 2;
    }
}
#endif
