#include "msa/core.hpp"
#include "ui_model.hpp"
#include <cmath>
#include <iostream>
#include <set>
using namespace msa;
namespace {
void require(bool condition, const char *why) {
    if (!condition)
        throw std::runtime_error(why);
}
void near(double a, double b) {
    require(std::abs(a - b) <= 1e-10 + 1e-9 * std::abs(b), "数值与解析解不一致");
}
Matrix fixture(double interaction, bool operator_effect = true) {
    Matrix x(4, std::vector<double>(5));
    for (int o = 0; o < 2; ++o)
        for (int t = 0; t < 2; ++t)
            for (int p = 0; p < 5; ++p)
                x[o * 2 + t][p] = 100 + 3 * (p - 2) + (operator_effect ? (o ? 1 : -1) : 0) +
                                  interaction * (o ? 1 : -1) * (p - 2) + (t ? 1 : -1);
    return x;
}
} // namespace
int main(int argc, char **argv) {
    try {
        auto c = default_config();
        c["analysis"]["method"] = "crossed_anova";
        c["design"] = {{"operators", 2}, {"replicates", 2}, {"parts", 5}};
        c["specification"] = {{"lsl", 0}, {"usl", 200}, {"compliance", "strict"}};
        near(f_survival(1, 2, 2), .5);
        near(f_survival(9, 1, 1), 2 * std::atan(1. / 3) / std::acos(-1.));
        near(f_survival(0, 3, 8), 1);
        auto pooled = calculate(fixture(0), c);
        require(!pooled["anova"]["interaction_included"].get<bool>(), "不显著交互未合并");
        const auto &v = pooled["anova"]["variance_components"];
        near(v["EV"], 10. / 7);
        near(v["Operator"], 13. / 7);
        near(v["PV"], 155. / 7);
        auto full = calculate(fixture(1), c);
        require(full["anova"]["interaction_included"].get<bool>(), "显著交互未保留");
        const auto &f = full["anova"]["variance_components"];
        near(f["EV"], 2);
        near(f["Operator"], 1);
        near(f["OperatorPart"], 4);
        near(f["AV"], 5);
        near(f["PV"], 20);
        c["analysis"]["interaction"] = "include";
        auto negative = calculate(fixture(1, false), c);
        near(negative["anova"]["variance_components"]["Operator"], 0);
        auto zero = calculate(Matrix(4, std::vector<double>(5, 1)), c);
        require(zero["ndc"].is_null() && zero["EV_pct"].is_null(), "零变异不应产生百分比或ndc");
        auto shifted = fixture(1);
        for (auto &row : shifted)
            for (double &n : row)
                n += 1e9;
        near(calculate(shifted, c)["GRR"], full["GRR"]);
        auto exact = default_config();
        exact["analysis"]["method"] = "crossed_anova";
        exact["design"]["parts"] = 5;
        Matrix identical(9, std::vector<double>{.1, .13, .27, .33, .71});
        auto exact_metrics = calculate(identical, exact);
        require(exact_metrics["GRR"] == 0 && exact_metrics["ndc"].is_null() &&
                    !exact_metrics["anova"]["interaction_included"].get<bool>(),
                "相同小数测量被舍入残差误判为交互");
        for (int i = 0; i < 3; ++i)
            for (int t = 0; t < 3; ++t)
                for (double &measurement : identical[i * 3 + t])
                    measurement += (t - 1) * 1e-12;
        require(calculate(identical, exact)["EV"].get<double>() > 1e-13,
                "真实微小重复性被错误归零");
        auto overflow = c;
        overflow["specification"]["usl"] = 1e-300;
        Matrix no_error(4, std::vector<double>{0, 1e12, 2e12, 3e12, 4e12});
        bool overflow_rejected = false;
        try {
            (void)calculate(no_error, overflow);
        } catch (const std::overflow_error &) {
            overflow_rejected = true;
        }
        require(overflow_rejected, "ANOVA分量P/T溢出不能静默序列化为null");
        for (int dp : {0, 10}) {
            auto precision = default_config();
            precision["analysis"]["method"] = "crossed_anova";
            precision["analysis"]["study_multiplier"] = 5.15;
            precision["specification"]["compliance"] = "relaxed";
            precision["generation"]["scale_mode"] = "reference_tv_sd";
            precision["generation"]["reference_tv_sd"] = dp ? 0.01 : 100;
            precision["generation"]["decimal_places"] = dp;
            precision["generation"]["seed"] = 123;
            auto value = generate(precision);
            require(value.metrics["PT_pct"].is_null(), "无双侧规格的P/T应不可计算");
            near(value.metrics["anova"]["study_variation"]["GRR"],
                 5.15 * value.metrics["GRR"].get<double>());
            auto output = value.config["output"];
            output["format"] = "matlab";
            require(parse_export_csv(export_csv_text(value, output), value.config) == value.matrix,
                    "ANOVA小数位极限回读失败");
        }
        int generated = 0;
        for (int o : {2, 3})
            for (int r : {2, 3})
                for (int p = 5; p <= 10; ++p)
                    for (const char *mode : {"auto", "include", "exclude"}) {
                        auto cfg = default_config();
                        cfg["analysis"]["method"] = "crossed_anova";
                        cfg["analysis"]["interaction"] = mode;
                        cfg["design"] = {{"operators", o}, {"replicates", r}, {"parts", p}};
                        cfg["specification"] = {{"lsl", 9}, {"usl", 11}, {"compliance", "strict"}};
                        cfg["generation"]["seed"] = 123;
                        cfg["generation"]["max_attempts"] = 1000;
                        for (const auto &entry : std::vector<std::pair<const char *, double>>{
                                 {"EV_pct", 6}, {"AV_pct", 8}, {"GRR_pct", 10}, {"PT_pct", 10}})
                            cfg["targets"][entry.first] = {
                                {"mode", "target"}, {"value", entry.second}, {"tolerance_pp", .1}};
                        cfg["targets"]["ndc"] = {{"mode", "target"}, {"value", 14}};
                        auto result = generate(cfg);
                        require(qualified(result.metrics, result.config), "ANOVA生成约束失败");
                        require(native_ui::to_config(native_ui::from_config(cfg)) ==
                                    normalize_config(cfg),
                                "GUI配置往返失败");
                        const auto original = result.matrix;
                        for (const char *order : {"standard", "all", "within_operator"}) {
                            auto output = result.config["output"];
                            output["format"] = "minitab";
                            output["run_order"] = {{"mode", order},
                                                   {"seed", 321},
                                                   {"randomize_operators", true},
                                                   {"store_standard_order", true}};
                            auto text = export_csv_text(result, output);
                            for (const auto *format : {"spss", "jmp19", "origin", "matlab"}) {
                                auto alternative = output;
                                alternative["format"] = format;
                                require(export_csv_text(result, alternative) == text,
                                        "五种长表导出内容不同");
                            }
                            auto restored = parse_export_csv(text, result.config);
                            require(restored == original, "随机顺序损坏对应关系或精度");
                            require(calculate(restored, result.config) == result.metrics,
                                    "重排改变ANOVA统计");
                            std::set<int> ids;
                            for (const auto &record : export_records(result, output))
                                ids.insert(record.standard_order);
                            require(ids.size() == std::size_t(o * r * p) && *ids.begin() == 1 &&
                                        *ids.rbegin() == o * r * p,
                                    "StdOrder不完整");
                        }
                        ++generated;
                    }
        int baselines = 0;
        if (argc > 2) {
            const auto fixture = Json::parse(read_text(std::filesystem::u8path(argv[2])));
            for (const auto &entry : fixture["cases"]) {
                const auto measured =
                    calculate(entry["matrix"].get<Matrix>(), normalize_config(entry["config"]));
                const auto &a = measured["anova"];
                require(a["interaction_included"] == entry["interaction_included"],
                        "软件参考交互模型不一致");
                near(a["interaction_p"], entry["interaction_p"]);
                require(measured["ndc"] == entry["ndc"], "软件参考ndc不一致");
                for (const auto &component : entry["variance_components"].items())
                    near(a["variance_components"][component.key()], component.value());
                for (std::size_t i = 0; i < entry["final_table"].size(); ++i) {
                    require(a["table"][i]["df"] == entry["final_table"][i]["df"],
                            "软件参考自由度不一致");
                    for (const auto *field : {"ss", "ms", "f", "p"})
                        near(a["table"][i][field], entry["final_table"][i][field]);
                }
                ++baselines;
            }
            require(baselines == 27, "软件参考案例数不完整");
        }
        if (argc > 1)
            atomic_text(std::filesystem::u8path(argv[1]), Json{{"passed", true},
                                                               {"generated_cases", generated},
                                                               {"reordered_exports", generated * 3},
                                                               {"analytical_cases", 4},
                                                               {"f_distribution_checks", 3},
                                                               {"software_baselines", baselines},
                                                               {"precision_cases", 2},
                                                               {"zero_grr_decimal_case", true},
                                                               {"overflow_case", true}}
                                                              .dump(2));
        std::cout << "PASS ANOVA: analytical solutions, zero/negative components, 72 "
                     "designs/models, 216 reordered exports\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
