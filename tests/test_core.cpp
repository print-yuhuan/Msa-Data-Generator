#include "msa/core.hpp"
#include "msa/task.hpp"
#include "ui_model.hpp"
#include <algorithm>
#include <chrono>
#include <ctime>
#include <cmath>
#include <fstream>
#include <iostream>
#include <locale>
#include <limits>
#include <mutex>
#include <set>
#include <string_view>
#include <thread>
#ifdef _WIN32
#include "platform/clipboard_win.hpp"
#include <shlobj.h>
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
    c["design"]["operators"] = o;
    c["design"]["replicates"] = r;
    c["design"]["parts"] = p;
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
std::vector<double> sorted_part_means(const Matrix &matrix) {
    std::vector<double> means(matrix.front().size());
    for (const auto &row : matrix)
        for (std::size_t part = 0; part < row.size(); ++part)
            means[part] += row[part];
    for (auto &mean : means)
        mean /= matrix.size();
    std::sort(means.begin(), means.end());
    return means;
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
            out["calculate"].push_back(calculate(v["matrix"].get<Matrix>(), normalize_config(v["config"])));
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
            auto c = normalize_config(v["config"]), native = v["native"];
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
                    auto m = export_data(a, path);
                    require(m == a.metrics);
                }
    });
    test("normality_rule_rejects_poor_fit_without_relaxing_generation_constraints", [] {
        auto c = example();
        c["specification"]["lsl"] = 9.5;
        c["specification"]["usl"] = 10.5;
        c["targets"] = {
            {"EV_pct", {{"mode", "target"}, {"value", 10}, {"tolerance_pp", .1}}},
            {"AV_pct", {{"mode", "at_most"}, {"value", 10}}},
            {"GRR_pct", {{"mode", "between"}, {"min", 10}, {"max", 20}}},
            {"PT_pct", {{"mode", "at_most"}, {"value", 20}}},
            {"ndc", {{"mode", "at_least"}, {"value", 5}}}};
        for (const bool anova : {false, true}) {
            c["analysis"]["method"] = anova ? "crossed_anova" : "crossed_xbarr";
            c["generation"]["seed"] = anova ? 7 : 6;
            c["generation"]["max_attempts"] = 1;
            c["generation"].erase("require_normality");
            const auto original = generate(c);
            require(qualified(original.metrics, c));
            require(original.search_diagnostics["anderson_darling"]["adjusted"].get<double>() > 1);
            c["generation"]["require_normality"] = false;
            require(generate(c).matrix == original.matrix, "normality off changed seeded generation");
            // The first rounded candidate has adjusted A² above 1 for both seeds.
            c["generation"]["require_normality"] = true;
            throws<SearchError>([&] { generate(c); });
            c["generation"]["max_attempts"] = 5000;
            const auto filtered = generate(c);
            require(filtered.matrix != original.matrix && qualified(filtered.metrics, c));
            const auto &ad = filtered.search_diagnostics["anderson_darling"];
            require(ad["required"] == true && ad["adjusted"].get<double>() <= ad["threshold"].get<double>());
            require(generate(c).matrix == filtered.matrix, "normality screening is not reproducible");
        }
    });
    test("part_sampling_shapes_and_minimum_span_use_final_measurements", [] {
        for (const char *method : {"crossed_xbarr", "crossed_anova"}) {
            auto c = example();
            c["analysis"]["method"] = method;
            clear_targets(c);
            const auto standard = generate(c).matrix;
            c["generation"].erase("part_sampling");
            c["generation"].erase("minimum_part_span_pct");
            require(generate(c).matrix == standard, "omitting new settings changes the default");
            c["generation"]["minimum_part_span_pct"] = 80;
            for (const char *mode : {"even", "stratified", "center_dense", "end_dense"}) {
                c["generation"]["part_sampling"] = mode;
                const auto result = generate(c);
                const auto means = sorted_part_means(result.matrix);
                require(qualified(result.metrics, c) && means.back() - means.front() + 1e-12 >= 1.6,
                        std::string("specification or measured part span not met: ") + mode);
                const double outer_gap = (means[1] - means[0] + means[9] - means[8]) / 2;
                const double middle_gap = means[5] - means[4];
                if (std::string_view(mode) == "even")
                    require(std::abs(outer_gap - middle_gap) < .0003, "equal spacing changed");
                if (std::string_view(mode) == "center_dense")
                    require(outer_gap > middle_gap * 1.5, "center is not denser");
                if (std::string_view(mode) == "end_dense")
                    require(middle_gap > outer_gap * 1.5, "ends are not denser");
                if (std::string_view(mode) == "stratified")
                    require(generate(c).matrix == result.matrix, "stratified seed is not reproducible");
            }
        }
        auto c = example();
        clear_targets(c);
        c["specification"] = {{"lsl", 9.49}, {"usl", 10.51}, {"compliance", "strict"}};
        c["generation"]["decimal_places"] = 1;
        c["generation"]["max_attempts"] = 100;
        c["generation"]["minimum_part_span_pct"] = 100;
        throws<InfeasibleError>([&] { validate_request(c); });
        c["specification"] = {{"lsl", 9.5}, {"usl", 10.5}, {"compliance", "strict"}};
        require(validate_request(c)["generation"]["minimum_part_span_pct"] == 100,
                "the threshold equal to the usable grid span was rejected");
        c["specification"]["lsl"] = nullptr;
        throws<ConfigError>([&] { validate_request(c); });
    });
    test("existing_part_sampling_preserves_1_0_2_seeded_measurements", [] {
        // First operator/trial on part 3, and last operator/trial on the last part,
        // from the released 1.0.2 CLI (seed 123, 2 operators x 2 trials x 5 parts).
        struct Baseline {
            const char *mode;
            std::array<double, 2> xbarr, anova;
        };
        const Baseline baselines[]{
            {"default", {9.7607, 9.3398}, {10.2982, 9.2614}},
            {"even", {9.7611, 10.5130}, {10.3308, 9.7464}},
            {"stratified", {9.7495, 10.2981}, {10.5119, 10.1819}},
            {"center_dense", {9.8656, 10.5130}, {10.2247, 9.8480}},
            {"end_dense", {9.6218, 10.5130}, {10.4713, 9.6143}}};
        auto c = example(2, 2, 5);
        clear_targets(c);
        for (const auto &baseline : baselines) {
            c["generation"]["part_sampling"] = baseline.mode;
            for (const bool anova : {false, true}) {
                c["analysis"]["method"] = anova ? "crossed_anova" : "crossed_xbarr";
                const auto matrix = generate(c).matrix;
                const auto &expected = anova ? baseline.anova : baseline.xbarr;
                expect_near(matrix.front()[2], expected[0]);
                expect_near(matrix.back().back(), expected[1]);
            }
        }
    });
    test("new_part_sampling_shapes_on_rounded_measurements", [] {
        // Independently tabulated standard-normal quantiles at p = 1 - 0.5 / n.
        constexpr std::array<double, 6> upper_z{
            1.2815515655, 1.3829941271, 1.4652337927, 1.5341205444, 1.5932188180, 1.6448536270};
        constexpr double tolerance = 1e-6;
        for (const char *method : {"crossed_xbarr", "crossed_anova"}) {
            for (int n = 5; n <= 10; ++n) {
                auto c = example(2, 3, n);
                clear_targets(c);
                c["analysis"]["method"] = method;
                c["generation"]["minimum_part_span_pct"] = 80;
                c["generation"]["decimal_places"] = 8;
                c["targets"]["GRR_pct"] = {{"mode", "at_most"}, {"value", 15}};
                std::vector<double> low_positions;
                for (const std::string mode : {"normal_quantile", "low_dense", "high_dense", "two_cluster"}) {
                    c["generation"]["part_sampling"] = mode;
                    const auto study = generate(c);
                    const auto tag = std::string(method) + "/" + mode + "/" + std::to_string(n);
                    require(qualified(study.metrics, c) && study.metrics["GRR_pct"] <= 15, tag);
                    require(study.matrix == generate(c).matrix, "seed not reproducible: " + tag);
                    auto x = sorted_part_means(study.matrix);
                    const double low = x.front(), span = x.back() - low;
                    require(span >= 1.6 - 1e-12, "80% measured span: " + tag);
                    expect_near(study.search_diagnostics.at("part_mean_span"), span);
                    for (auto &value : x)
                        value = (value - low) / span;
                    if (mode == "normal_quantile") {
                        for (int i = 0; i < n; ++i) {
                            const double z = (2 * x[i] - 1) * upper_z[n - 5];
                            const double probability = .5 * std::erfc(-z / std::sqrt(2.));
                            require(std::abs(probability - (i + .5) / n) < tolerance,
                                    "normal midpoint quantile: " + tag);
                        }
                    } else if (mode == "low_dense") {
                        for (int i = 1; i + 1 < n; ++i)
                            require(x[i] - x[i - 1] < x[i + 1] - x[i], "low-end spacing: " + tag);
                        low_positions = x;
                    } else if (mode == "high_dense") {
                        for (int i = 0; i < n; ++i)
                            require(std::abs(x[i] + low_positions[n - 1 - i] - 1.) < tolerance,
                                    "high/low mirrored positions: " + tag);
                    } else {
                        const int split = int(std::lower_bound(x.begin(), x.end(), .5) - x.begin());
                        require(std::abs(n - 2 * split) <= 1, "unequal batch counts: " + tag);
                        require(x[split] - x[split - 1] >= .5 - tolerance, "missing inter-batch gap: " + tag);
                        require(x[split - 1] > tolerance && x[split] < 1 - tolerance,
                                "no within-batch spread: " + tag);
                    }
                }
            }
        }
    });
    test("two_cluster_varies_odd_batch_assignment_and_within_batch_positions", [] {
        for (const char *method : {"crossed_xbarr", "crossed_anova"}) {
            auto c = example(2, 2, 7);
            clear_targets(c);
            c["analysis"]["method"] = method;
            c["generation"]["part_sampling"] = "two_cluster";
            c["generation"]["decimal_places"] = 8;
            std::array<std::vector<double>, 2> interior_positions;
            for (int seed = 1; seed <= 16; ++seed) {
                c["generation"]["seed"] = seed;
                const auto x = sorted_part_means(generate(c).matrix);
                const auto midpoint = (x.front() + x.back()) / 2;
                const auto split = std::lower_bound(x.begin(), x.end(), midpoint) - x.begin();
                require(split == 3 || split == 4, "seven parts must split 3/4 or 4/3");
                const auto width = x[split - 1] - x[0];
                require(width > 0, "lower batch collapsed");
                interior_positions[split - 3].push_back((x[1] - x[0]) / width);
            }
            for (const auto &positions : interior_positions) {
                require(!positions.empty(), "odd extra part always assigned to the same batch");
                const auto [low, high] = std::minmax_element(positions.begin(), positions.end());
                require(*high - *low > .001, "seed did not vary positions within a batch");
            }
        }
    });
    test("uniform_random_limits_are_required_only_for_generation", [] {
        auto c = example(2, 2, 5);
        clear_targets(c);
        const auto matrix = generate(c).matrix;
        c["generation"]["part_sampling"] = "uniform_random";
        c["generation"]["reference_tv_sd"] = .2;
        for (const char *compliance : {"strict", "relaxed"}) {
            c["specification"]["compliance"] = compliance;
            for (int mask = 0; mask < 3; ++mask) {
                c["specification"]["lsl"] = mask & 1 ? Json(9) : Json(nullptr);
                c["specification"]["usl"] = mask & 2 ? Json(11) : Json(nullptr);
                require(normalize_config(c)["generation"]["part_sampling"] == "uniform_random");
                require(calculate(matrix, c)["GRR_pct"].is_number(), "analysis required sampling limits");
                throws<ConfigError>([&] { generate(c); });
            }
        }
        c["specification"]["lsl"] = 11;
        c["specification"]["usl"] = 9;
        throws<ConfigError>([&] { validate_request(c); });
    });
    test("uniform_random_draws_independent_part_sizes_across_the_specification", [] {
        // Zero GRR makes the rounded readings equal to the sampled sizes. No minimum
        // span or target center is requested, so the first draw is observable.
        for (const char *method : {"crossed_xbarr", "crossed_anova"}) {
            auto c = example(2, 2, 5);
            clear_targets(c);
            c["analysis"]["method"] = method;
            c["specification"] = {{"lsl", 31.5}, {"usl", 35.5}, {"compliance", "relaxed"}};
            c["generation"]["part_sampling"] = "uniform_random";
            c["generation"]["decimal_places"] = 6;
            c["generation"]["max_attempts"] = 1;
            c["targets"]["GRR_pct"] = {{"mode", "at_most"}, {"value", 0}};
            std::array<int, 4> quarters{};
            bool all_above = false, all_below = false, near_low = false, near_high = false;
            for (int seed = 1; seed <= 128; ++seed) {
                c["generation"]["seed"] = seed;
                const auto study = generate(c);
                require(study.attempts == 1 && qualified(study.metrics, c));
                for (const auto &row : study.matrix)
                    require(row == study.matrix.front(), "zero-GRR replicates changed sampled sizes");
                const auto [low, high] = std::minmax_element(study.matrix.front().begin(),
                                                             study.matrix.front().end());
                require(*low >= 31.5 && *high <= 35.5, "uniform draw outside the supplied limits");
                for (double size : study.matrix.front())
                    ++quarters[std::min(3, static_cast<int>(size - 31.5))];
                all_above |= *low > 31.5 + .4 * 4;
                all_below |= *high < 31.5 + .6 * 4;
                near_low |= *low < 31.5 + .02 * 4;
                near_high |= *high > 31.5 + .98 * 4;
                if (seed == 1)
                    require(study.matrix == generate(c).matrix, "uniform draw is not reproducible");
            }
            require(all_above && all_below, "every draw was centered, stratified or forced to span the limits");
            require(near_low && near_high, "uniform sampler failed to reach the tails of the specified interval");
            // For these 640 fixed, unfiltered draws the expected count per quarter is 160.
            for (int count : quarters)
                require(count >= 115 && count <= 205, "uniform part sizes concentrate in particular quarters");
        }
    });
    test("uniform_random_verifies_final_span_rounding_and_compliance", [] {
        for (const char *method : {"crossed_xbarr", "crossed_anova"}) {
            auto c = example(2, 3, 7);
            clear_targets(c);
            c["analysis"]["method"] = method;
            c["generation"]["part_sampling"] = "uniform_random";
            c["generation"]["decimal_places"] = 3;
            c["generation"]["minimum_part_span_pct"] = 65;
            c["targets"]["GRR_pct"] = {{"mode", "at_most"}, {"value", 15}};
            const auto study = generate(c);
            require(qualified(calculate(study.matrix, c), c), "rounded measurements failed the target");
            for (const auto &row : study.matrix)
                for (double value : row) {
                    require(value >= 9 && value <= 11, "strict reading outside specification");
                    require(value == parse_number(fixed(value, 3)), "incorrect output precision");
                }
            const auto means = sorted_part_means(study.matrix);
            const double span = means.back() - means.front();
            require(span >= 1.3 - 1e-12, "minimum span lost after rounding");
            expect_near(study.search_diagnostics.at("part_mean_span_pct"), 50 * span);

            clear_targets(c);
            c["generation"]["minimum_part_span_pct"] = 100;
            c["generation"]["max_attempts"] = 1;
            c["generation"]["decimal_places"] = 8;
            throws<SearchError>([&] { generate(c); }); // No manufactured endpoint parts.
            c["generation"]["decimal_places"] = 0;
            c["generation"]["max_attempts"] = 200;
            const auto at_limits = generate(c);
            require(at_limits.search_diagnostics.at("part_mean_span_pct") == 100,
                    "an attainable rounded span was rejected");

            c["specification"]["compliance"] = "relaxed";
            c["generation"]["minimum_part_span_pct"] = nullptr;
            c["generation"]["decimal_places"] = 8;
            c["generation"]["max_attempts"] = 1;
            c["generation"]["scale_mode"] = "reference_tv_sd";
            c["generation"]["reference_tv_sd"] = 100;
            const auto relaxed = generate(c);
            bool outside = false;
            for (const auto &row : relaxed.matrix)
                for (double value : row)
                    outside |= value < 9 || value > 11;
            const auto relaxed_means = sorted_part_means(relaxed.matrix);
            require(outside && relaxed_means.front() >= 9 - 1e-8 && relaxed_means.back() <= 11 + 1e-8,
                    "relaxed measurement noise or independently sampled part locations were lost");
        }
    });
    test("overlapping_bands_not_nominal_conflict", [] {
        auto c = example();
        c["targets"]["GRR_pct"]["value"] = 10.15;
        c["targets"]["ndc"] = nullptr;
        require(qualified(generate(c).metrics, c));
    });
    test("strict_full_part_span_has_variable_interior_parts", [] {
        for (const char *method : {"crossed_xbarr", "crossed_anova"}) {
            auto c = example();
            clear_targets(c);
            c["analysis"]["method"] = method;
            c["generation"]["minimum_part_span_pct"] = 100;
            c["generation"]["decimal_places"] = 8;
            c["targets"]["EV_pct"] = {{"mode", "at_least"}, {"value", 1}};
            const auto result = generate(c);
            bool anchored_low = false, anchored_high = false;
            for (std::size_t j = 0; j < result.matrix.front().size(); ++j) {
                const auto at_limit = [&](double limit) {
                    return std::all_of(result.matrix.begin(), result.matrix.end(),
                                       [&](const auto &row) { return row[j] == limit; });
                };
                anchored_low |= at_limit(9);
                anchored_high |= at_limit(11);
            }
            require(anchored_low && anchored_high && qualified(result.metrics, c));
            require(result.search_diagnostics["part_mean_span_pct"] == 100);
            require(result.metrics["EV_pct"].get<double>() >= 1);
            require(generate(c).matrix == result.matrix);
        }
    });
    test("anova_subgroup_residual_model_preserves_targets_and_standard_default", [] {
        for (int r : {2, 3}) {
            auto c = example(3, r, 10);
            c["analysis"]["method"] = "crossed_anova";
            clear_targets(c);
            c["targets"]["EV_pct"] = {{"mode", "between"}, {"min", 5}, {"max", 10}};
            c["generation"]["decimal_places"] = 8;
            c["generation"].erase("residual_model");
            const auto standard = generate(c);
            c["generation"]["residual_model"] = "standard";
            require(generate(c).matrix == standard.matrix);
            c["generation"]["residual_model"] = "varied";
            const auto varied = generate(c);
            require(qualified(varied.metrics, c) && varied.matrix == generate(c).matrix);
            std::vector<double> ranges;
            for (int op = 0; op < 3; ++op)
                for (int j = 0; j < 10; ++j) {
                    double low = varied.matrix[op * r][j], high = low;
                    for (int trial = 1; trial < r; ++trial) {
                        low = std::min(low, varied.matrix[op * r + trial][j]);
                        high = std::max(high, varied.matrix[op * r + trial][j]);
                    }
                    ranges.push_back(high - low);
                }
            auto [low, high] = std::minmax_element(ranges.begin(), ranges.end());
            require(*high > 2 * *low, "varied ANOVA subgroups still have nearly equal ranges");
            c["generation"]["residual_model"] = "invalid";
            throws<ConfigError>([&] { normalize_config(c); });
        }
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
        c["targets"]["ndc"] = {{"mode", "at_least"}, {"value", 1}};
        throws<InfeasibleError>([&] { validate_request(c); });
        for (auto key : {"EV_pct", "AV_pct", "GRR_pct"})
            c["targets"][key] = nullptr;
        throws<InfeasibleError>([&] { validate_request(c); });
    });
    test("historical_variation_does_not_qualify_constant_samples", [&] {
        for (const char *method : {"crossed_xbarr", "crossed_anova"}) {
            auto c = example(2, 2, 5);
            clear_targets(c);
            c["analysis"]["method"] = method;
            c["analysis"]["historical_sd"] = .1;
            c["specification"]["lsl"] = 0;
            c["specification"]["usl"] = 1;
            c["generation"]["center"] = 0;
            c["generation"]["decimal_places"] = 0;
            c["generation"]["scale_mode"] = "reference_tv_sd";
            c["generation"]["reference_tv_sd"] = .1;
            c["generation"]["max_attempts"] = 1;

            Matrix constant(4, std::vector<double>(5, 0));
            auto metrics = calculate(constant, c);
            require(metrics["TV"] == .1 && metrics["study"]["sample_tv"] == 0);
            require(!qualified(metrics, c));
            throws<SearchError>([&] { generate(c); });
            const auto output = dir / (std::string("constant-") + method + ".csv");
            atomic_text(output, "keep");
            throws<std::invalid_argument>(
                [&] { export_data(Result{constant, metrics, c, 123}, output); });
            require(read_text(output) == "keep");
        }
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
    test("one_sided_pt_uses_final_mean_when_soft_center_lies_at_the_limit", [] {
        for (const auto *method : {"crossed_xbarr", "crossed_anova"})
            for (const bool lower : {true, false}) {
                auto c = example();
                clear_targets(c);
                const double limit = lower ? 10. : -10.;
                c["analysis"]["method"] = method;
                c["specification"][lower ? "usl" : "lsl"] = nullptr;
                c["specification"][lower ? "lsl" : "usl"] = limit;
                c["generation"]["reference_tv_sd"] = .1;
                c["generation"]["max_attempts"] = 1;
                c["targets"]["PT_pct"] = {{"mode", "at_most"}, {"value", 1000}};
                for (const double epsilon : {0., 1e-10}) {
                    c["generation"]["center"] = lower ? limit + epsilon : limit - epsilon;
                    auto result = generate(c);
                    const double expected_mean = result.metrics["mean"];
                    require(qualified(result.metrics, c) &&
                                (lower ? expected_mean > limit : expected_mean < limit),
                            "one-sided placement did not produce a valid nonzero P/T denominator");
                    expect_near(result.metrics["PT_pct"],
                                300 * result.metrics["GRR"].get<double>() / std::abs(limit - expected_mean));
                }
                c["generation"]["center"] = limit;
                c["analysis"]["historical_mean"] = limit;
                throws<InfeasibleError>([&] { validate_request(c); });
                c["analysis"]["tolerance"] = 1;
                require(qualified(generate(c).metrics, c),
                        "independent tolerance did not override zero historical distance");
            }
    });
    test("automatic_reference_scale_uses_entered_reference_before_history", [] {
        for (const auto *method : {"crossed_xbarr", "crossed_anova"})
            for (const bool has_limit : {true, false}) {
                auto c = example();
                clear_targets(c);
                c["analysis"]["method"] = method;
                c["analysis"]["historical_sd"] = 5;
                c["specification"]["lsl"] = has_limit ? Json(10) : Json(nullptr);
                c["specification"]["usl"] = nullptr;
                c["specification"]["compliance"] = has_limit ? "strict" : "relaxed";
                c["generation"]["reference_tv_sd"] = .1;
                c["generation"]["decimal_places"] = 6;
                const auto automatic = generate(c);
                require(automatic.metrics["TV"] == 5 &&
                            automatic.metrics["study"]["historical_applied"] == true &&
                            std::abs(automatic.metrics["study"]["sample_tv"].get<double>() - .1) < 1e-6,
                        "historical TV replaced the entered generation scale");
                c["generation"]["scale_mode"] = "reference_tv_sd";
                const auto explicit_reference = generate(c);
                require(explicit_reference.matrix == automatic.matrix &&
                            explicit_reference.metrics == automatic.metrics,
                        "implicit and explicit reference scale produced different results");
            }
    });
    test("historical_reference_generation_can_use_proven_sample_tv_fallback", [] {
        for (const bool one_sided : {false, true}) {
            auto c = example();
            clear_targets(c);
            if (one_sided) {
                c["specification"]["usl"] = nullptr;
                c["analysis"]["historical_mean"] = 10;
            }
            c["analysis"]["historical_sd"] = .01;
            c["generation"]["scale_mode"] = "reference_tv_sd";
            c["generation"]["reference_tv_sd"] = .2;
            c["targets"]["GRR_pct"] = {{"mode", "between"}, {"min", 45}, {"max", 55}};
            c["targets"]["PT_pct"] = {{"mode", "between"}, {"min", 29}, {"max", 31}};
            const auto result = generate(c);
            require(qualified(result.metrics, c) &&
                        result.metrics["study"]["historical_applied"] == false,
                    "the fixed P/T denominator requires GRR above historical SD");
            auto study = c;
            study["analysis"]["process_variation"] = "study";
            const auto witness = generate(study);
            require(qualified(calculate(witness.matrix, c), c),
                    "study-generated data should qualify after history falls back to sample TV");
        }
        auto c = example();
        clear_targets(c);
        c["specification"]["usl"] = nullptr;
        c["analysis"]["historical_mean"] = 11;
        c["analysis"]["historical_sd"] = .15;
        c["analysis"]["tolerance"] = 2;
        c["generation"]["reference_tv_sd"] = .2;
        c["targets"]["GRR_pct"] = {{"mode", "between"}, {"min", 65}, {"max", 69}};
        c["targets"]["PT_pct"] = {{"mode", "between"}, {"min", 29}, {"max", 31}};
        const auto result = generate(c);
        require(qualified(result.metrics, c) && result.metrics["TV"] == .15 &&
                    result.metrics["study"]["historical_applied"] == true,
                "independent tolerance preserves the valid historical process variation");
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
                 {{"generation", {{"require_normality", "true"}}}},
                 {{"generation", {{"part_sampling", "unknown"}}}},
                 {{"generation", {{"minimum_part_span_pct", 0}}}},
                 {{"generation", {{"minimum_part_span_pct", 101}}}},
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
        export_data(a, path);
        std::locale::global(previous);
        std::ifstream f(path, std::ios::binary);
        std::string s((std::istreambuf_iterator<char>(f)), {});
        require(s.substr(0, 3) == "\xef\xbb\xbf");
        require(read_data(path, a.config) == a.matrix);
        require(s.find('\r') != s.npos);
        auto bad = a;
        bad.config["generation"]["decimal_places"] = 0;
        auto target = dir / "must-not-overwrite.csv";
        atomic_text(target, "keep");
        throws<std::invalid_argument>([&] { export_data(bad, target); });
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
        require(task.view().status == TaskStatus::Cancelled && !task.view().result);
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

        // Cancel when the final progress report is about to publish a valid result.
        easy = example();
        clear_targets(easy);
        easy["generation"]["max_attempts"] = 1;
        cancelled = false;
        bool first_update = true;
        throws<CancelledError>([&] {
            generate(easy, &cancelled, [&](int n, double, bool found) {
                if (!first_update) {
                    require(n == 1);
                    require(found);
                    cancelled = true;
                }
                first_update = false;
            });
        });
    });
    test("search_reports_all_rejections_and_optimal_stop", [] {
        auto c = example();
        clear_targets(c);
        const auto easy = generate(c);
        require(easy.attempts == 1 && easy.search_diagnostics["stop_reason"] == "optimal_score");
        require(easy.search_diagnostics["accepted"] == 1);
        require(easy.as_json()["search"] == easy.search_diagnostics);
        c["generation"]["minimum_part_span_pct"] = 99;
        c["targets"]["EV_pct"] = {{"mode", "at_least"}, {"value", 90}};
        c["generation"]["max_attempts"] = 201;
        std::vector<int> progress;
        try {
            generate(c, nullptr, [&](int n, double, bool) { progress.push_back(n); });
            throw std::runtime_error("expected placement budget exhaustion");
        } catch (const SearchError &e) {
            const auto &d = e.diagnostics;
            require(d["stop_reason"] == "max_attempts" && d["attempts"] == 201);
            require(d["accepted"] == 0 && d["rejected"]["placement"] == 201);
            require(std::find(progress.begin(), progress.end(), 101) != progress.end());
        }
        c["generation"]["timeout_seconds"] = .01;
        try {
            generate(c, nullptr, [](int n, double, bool) {
                if (n == 1)
                    std::this_thread::sleep_for(std::chrono::milliseconds(15));
            });
            throw std::runtime_error("expected time budget exhaustion");
        } catch (const SearchError &e) {
            require(e.diagnostics["stop_reason"] == "timeout");
        }
    });
    test("silent_export_paths_and_existing_file_replacement", [&] {
        auto cfg = example();
        const auto desktop = desktop_directory();
        require(desktop.is_absolute() && resolve_output_directory("") == desktop);
#ifdef _WIN32
        PWSTR raw = nullptr;
        const auto hr = SHGetKnownFolderPath(FOLDERID_Desktop, KF_FLAG_DONT_VERIFY, nullptr, &raw);
        const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> system_folder(raw, &CoTaskMemFree);
        require(SUCCEEDED(hr) && raw && desktop == std::filesystem::path(raw),
                "默认数据目录须采用 Windows 实际桌面位置");
        throws<ConfigError>([] { resolve_output_directory("C:relative"); });
        throws<ConfigError>([] { resolve_output_directory("\\relative"); });
#endif
        require(config_path().parent_path().filename() == std::filesystem::u8path("MSA数据生成器") &&
                    config_path().filename() == "msa_generator.json",
                "配置文件沿用原位置");
        require(configured_data_path(cfg) == desktop / "msa_data.csv");
        cfg["output"]["directory"] = "relative/subdir";
        cfg["output"]["filename"] = "中文 数据";
        require(configured_data_path(cfg) == (desktop / "relative/subdir" /
                                             std::filesystem::u8path("中文 数据.csv"))
                                                .lexically_normal());
        cfg["output"]["directory"] =
            (std::filesystem::absolute(dir) / std::filesystem::u8path("静默 输出")).u8string();
        auto path = configured_data_path(cfg);
        auto result = generate(cfg);
        atomic_text(path, "old file");
        require(export_data(result, path) == result.metrics);
        require(read_data(path, result.config) == result.matrix);
        require(export_data(result, path) == result.metrics);
    });
    test("filename_variables_local_time_result_values_and_validation", [&] {
        auto cfg = example();
        auto result = generate(cfg);
        auto autoSeedConfig = cfg;
        autoSeedConfig["generation"]["seed"] = nullptr;
        const auto autoSeedResult = generate(autoSeedConfig);
        const auto data_name = [](const std::string &pattern, const Result &study,
                                  std::chrono::system_clock::time_point when) {
            return render_study_basename(pattern, ExportBasenameKind::Data, study,
                                         StudySource::Generated, when, ".csv");
        };
        require(
            data_name("[随机种子]", autoSeedResult, std::chrono::system_clock::now()) ==
            std::to_string(autoSeedResult.seed));
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
        require(data_name("MSA_[YYYY]-[MM]-[DD]_[H]-[M]-[S]_[MS]", result, when) ==
                "MSA_2026-10-24_22-08-12_037");
        require(data_name("[YY]_[DATE]_[TIME]_[DATETIME]", result, when) ==
                "26_2026-10-24_22-08-12_2026-10-24_22-08-12");
        const auto epochTime =
            std::chrono::system_clock::from_time_t(1700000000) + std::chrono::milliseconds(999);
        require(data_name("[TIMESTAMP]_[时间戳]_[MS]", result, epochTime) ==
                "1700000000_1700000000_999");
        require(data_name("[TIMESTAMP]", result,
                                std::chrono::system_clock::from_time_t(0)) == "0");
        require(data_name("[[已复核]]_[人员数量]_[REPLICATES]_[产品件数]_[USL]_[LSL]_["
                                "研究变异倍数]_[SEED]_[小数位数]_[VERSION]",
                                result, when) ==
                std::string("[已复核]_3_3_10_11_9_6_123_4_") + ui_version);
        result.metrics["GRR_pct"] = 999; // Names must use actual data, not a mutable cached label.
        const auto actual = calculate(result.matrix, cfg);
        require(data_name("[EV_PCT]_[AV_PCT]_[GRR_PCT]_[PT_PCT]_[NDC]", result, when) ==
                fixed(actual["EV_pct"], 2) + "_" + fixed(actual["AV_pct"], 2) + "_" +
                    fixed(actual["GRR_pct"], 2) + "_" + fixed(actual["PT_pct"], 2) + "_" +
                    actual["ndc"].dump());
        require(data_name("[CENTER]", result, when) == actual["mean"].dump() ||
                data_name("[CENTER]", result, when) == "10");
        result.config["specification"]["lsl"] = nullptr;
        result.config["specification"]["usl"] = nullptr;
        require(data_name("[USL]_[LSL]_[PT_PCT]", result, when) == "NA_NA_NA");
        for (const auto &v : filename_variables()) {
            validate_filename_template(std::string("[") + v.name + "]");
            if (*v.alias)
                require(data_name(std::string("[") + v.alias + "]", result, when) ==
                        data_name(std::string("[") + v.name + "]", result, when));
        }
        for (const std::string bad :
             {"[HH]", "[mm]", "[unknown]", "[]", "[YYYY", "[A[B]]",
              "a]", "../a", "CON", "LPT1.data", "COM¹", "x\t", ".xlsx", "batch.csv", "batch.XLSX"})
            throws<ConfigError>([&] { validate_filename_template(bad); });
        throws<ConfigError>([&] { validate_filename_template(std::string(252, 'a')); });
        const auto fullCsvBase = std::string(251, 'a');
        require(normalize_import_filename(fullCsvBase) == fullCsvBase);
        require(normalize_output({{"filename", fullCsvBase}})["filename"] == fullCsvBase);
        throws<ConfigError>([&] { normalize_output({{"filename", fullCsvBase}, {"file_type", "xlsx"}}); });
        require(normalize_output({{"filename", std::string(250, 'a')}, {"file_type", "xlsx"}})["file_type"] == "xlsx");
        throws<ConfigError>(
            [&] { data_name(std::string(248, 'a') + "[VERSION]", result, when); });
        auto invalid = cfg;
        invalid["output"]["existing_file"] = "invalid";
        throws<ConfigError>([&] { normalize_config(invalid); });
        require(normalize_config(Json::object())["output"]["existing_file"] == "number");
        auto draft = native_ui::from_config(cfg);
        draft.existingFile = 1;
        require(native_ui::from_config(native_ui::to_config(draft)).existingFile == 1);
    });
    test("filename_templates_follow_operation_time_study_source_and_draft", [] {
        const auto when = std::chrono::system_clock::from_time_t(1700000000) +
                          std::chrono::milliseconds(37);
        auto result = generate(example());
        const auto measured = calculate(result.matrix, result.config);
        const auto grr = fixed(measured["GRR_pct"].get<double>(), 2);
        result.metrics["GRR_pct"] = -2; // The selected matrix is authoritative.
        const auto generated = StudySource::Generated, imported = StudySource::ImportedData;
        const auto chart = ExportBasenameKind::Chart, report = ExportBasenameKind::ComparisonReport;
        throws<ConfigError>([&] { normalize_export_basename("交互图_[SEED].SVG", chart, ".png"); });
        throws<ConfigError>([&] { normalize_export_basename("方法对比.pdf", report, ".html"); });
        throws<ConfigError>([&] { normalize_import_filename("测量数据.xlsx"); });
        const std::string pattern = "[[检]]_[时间戳]_[MS]_[随机种子]_[GRR实际值]";
        const std::string prefix = "[检]_1700000000_037_";
        for (auto kind : {ExportBasenameKind::Data, chart, report}) {
            const char *ext = kind == chart ? ".png" : kind == report ? ".html" : ".xlsx";
            require(render_study_basename(pattern, kind, result, generated, when, ext) ==
                    prefix + std::to_string(result.seed) + "_" + grr);
            require(render_study_basename(pattern, kind, result, imported, when, ext) ==
                    prefix + "NA_" + grr);
        }
        native_ui::Draft draft;
        draft.operators = "2";
        draft.seed = "9223372036854775807";
        draft.center = "13.5";
        draft.lsl = "-";
        draft.confidenceLevel = "编辑中";
        draft.filename = "[未完成";
        const auto options = native_ui::filename_options(draft);
        const std::string prefill = "[人员数量]_[SEED]_[CENTER]_[LSL]_[GRR_PCT]";
        require(render_import_basename(prefill, options, nullptr, generated, when) ==
                "2_9223372036854775807_13.5_NA_NA");
        require(render_import_basename(prefill, options, &result, generated, when) ==
                "2_" + std::to_string(result.seed) + "_13.5_NA_" + grr);
        require(render_import_basename(prefill, options, &result, imported, when) ==
                "2_NA_13.5_NA_" + grr);
        require(render_import_basename("", options, nullptr, generated, when).empty());
        require(render_import_basename("[CENTER]_[SEED]_[NDC]", Json::object(), nullptr, imported, when) ==
                "NA_NA_NA");
        require(render_import_basename("[CENTER]", Json::object(), &result, imported, when) ==
                render_study_basename("[CENTER]", chart, result, imported, when, ".png"));
        for (const char *bad : {"[UNKNOWN]", "[DATE", "x]", "NUL", "a/b"}) {
            throws<ConfigError>([&] { render_import_basename(bad, options, nullptr, generated, when); });
            throws<ConfigError>([&] { render_study_basename(bad, chart, result, generated, when, ".jpg"); });
            throws<ConfigError>([&] { render_study_basename(bad, report, result, imported, when, ".pdf"); });
        }
        const auto long_template = std::string(241, 'x') + "[时间戳]";
        require(render_study_basename(long_template, chart, result, generated, when, ".png").size() == 251);
        throws<ConfigError>([&] { render_study_basename(long_template, report, result, imported, when, ".html"); });
    });
    test("current_config_version_and_literal_name_variables", [] {
        auto cfg = default_config();
        cfg["input"]["filename"] = "[[DATE]]_[OPERATORS]";
        require(normalize_config(cfg) == cfg);
        require(render_import_basename(cfg["input"]["filename"], cfg, nullptr,
                                       StudySource::Generated, std::chrono::system_clock::now()) ==
                "[DATE]_3");
        for (int version : {0, 1, 3})
            throws<ConfigError>([&] { normalize_config({{"schema_version", version}}); });
    });
    test("configured_export_numbering_overwrite_unicode_and_parallel_safety", [&] {
        auto result = generate(example());
        auto settings = result.config["output"];
        const auto folder =
            std::filesystem::absolute(dir) /
            ("naming-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        settings["directory"] = folder.u8string();
        settings["filename"] = "中文_[[报告]]_[人员数量]_[SEED]";
        const auto first = export_configured_data(result, settings);
        const auto original = read_text(first);
        const auto second = export_configured_data(result, settings);
        require(first.filename().u8string() == "中文_[报告]_3_123.csv");
        require(second.filename().u8string() == "中文_[报告]_3_123_001.csv");
        require(read_text(first) == original && read_data(second, result.config) == result.matrix);
        settings["existing_file"] = "overwrite";
        atomic_text(first, "old");
        require(export_configured_data(result, settings) == first &&
                read_data(first, result.config) == result.matrix);
        throws<ConfigError>([&] {
            export_configured_data(result, settings, std::chrono::system_clock::now(), first);
        });
        settings["existing_file"] = "number";
        std::mutex mutex;
        std::set<std::filesystem::path> paths;
        std::atomic<int> failures{0};
        std::vector<std::thread> jobs;
        for (int i = 0; i < 8; ++i)
            jobs.emplace_back([&] {
                try {
                    auto path = export_configured_data(result, settings);
                    require(read_data(path, result.config) == result.matrix);
                    std::lock_guard<std::mutex> lock(mutex);
                    paths.insert(path);
                } catch (...) {
                    ++failures;
                }
            });
        for (auto &job : jobs)
            job.join();
        require(failures == 0 && paths.size() == 8 && read_text(first) == original);
        require(result.config["output"]["filename"] == "msa_data");
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
        output["filename"] = std::string(247, 'a');
        const auto first = export_configured_data(result, output);
        const auto second = export_configured_data(result, output);
        require(first.filename().u8string().size() == 251 &&
                    second.filename().u8string().size() == 255 &&
                    read_data(first, result.config) == result.matrix && read_data(second, result.config) == result.matrix,
                "长文件名导出或追加序号回读失败");
        const auto original = read_text(first);
        std::mutex mutex;
        std::set<std::filesystem::path> paths;
        std::atomic<int> failures{0};
        std::vector<std::thread> jobs;
        for (int i = 0; i < 4; ++i)
            jobs.emplace_back([&] {
                try {
                    auto path = export_configured_data(result, output);
                    require(path.filename().u8string().size() == 255 &&
                            read_data(path, result.config) == result.matrix);
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
        throws<FileExistsError>([&] { export_data(result, csv, ExistingFile::Preserve); });
        require(read_text(csv) == "keep");
        export_data(result, csv, ExistingFile::Replace);
        require(read_data(csv, result.config) == result.matrix);
        std::filesystem::remove(csv);
        export_data(result, csv, ExistingFile::Preserve);
        require(read_data(csv, result.config) == result.matrix);
        for (auto &entry : std::filesystem::directory_iterator(dir))
            require(entry.path().extension() != ".tmp", "temporary file leaked");
    });
    test("unrepresentable_variance_is_rejected_before_json_serialization", [] {
        auto cfg = default_config();
        cfg["design"] = {{"operators", 2}, {"replicates", 2}, {"parts", 5}};
        // 标准差仍有限，只有其平方溢出；不能返回成功并由 JSON 把无穷转成 null。
        const Matrix matrix(4, std::vector<double>{-1e200, -5e199, 0, 5e199, 1e200});
        throws<std::overflow_error>([&] { calculate(matrix, cfg); });
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
        throws<std::overflow_error>([&] { export_data(normal, output); });
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
