#include "msa/core.hpp"
#include "ui_model.hpp"
#include <cmath>
#include <iostream>
using namespace msa;
namespace {
void require(bool ok, const char *why) {
    if (!ok)
        throw std::runtime_error(why);
}
void near(double a, double b, double tolerance = 1e-10) {
    require(std::abs(a - b) <= tolerance * (1 + std::abs(b)), "分析参数结果与解析解不一致");
}
Matrix data() {
    Matrix x(4, std::vector<double>(5));
    for (int o = 0; o < 2; ++o)
        for (int t = 0; t < 2; ++t)
            for (int p = 0; p < 5; ++p)
                x[o * 2 + t][p] =
                    100 + 3 * (p - 2) + (o ? 1 : -1) + (o ? 1 : -1) * (p - 2) + (t ? 1 : -1);
    return x;
}
void narrow_specification_with_analysis_tolerance() {
    for (const char *method : {"crossed_xbarr", "crossed_anova"}) {
        auto cfg = default_config();
        cfg["design"] = {{"operators", 2}, {"replicates", 2}, {"parts", 5}};
        cfg["specification"]["lsl"] = 0;
        cfg["specification"]["usl"] = 1e-309;
        cfg["analysis"]["method"] = method;
        cfg["analysis"]["tolerance"] = 2;
        const auto metrics = calculate(data(), normalize_config(cfg));
        require(metrics["study"]["tolerance_source"] == "interval", "独立容差未优先使用");
        near(metrics["PT_pct"], 300 * metrics["GRR"].get<double>());
        if (metrics.contains("anova")) {
            const auto anova = crossed_anova(data(), cfg);
            require(anova["tolerance_pct"] == metrics["anova"]["tolerance_pct"],
                    "ANOVA 直接调用没有使用相同的独立容差");
        }
    }
}
void narrow_normal_classification_interval() {
    auto cfg = default_config();
    cfg["design"] = {{"operators", 2}, {"replicates", 2}, {"parts", 5}};
    cfg["analysis"]["method"] = "crossed_anova";
    cfg["analysis"]["historical_mean"] = 0;
    cfg["analysis"]["misclassification"] = true;
    for (double limit : {1e-15, 1e-20}) {
        cfg["specification"]["lsl"] = -limit;
        cfg["specification"]["usl"] = limit;
        const auto result = calculate(data(), cfg)["misclassification"];
        const double expected_good =
            std::erf(limit / (std::sqrt(2.) * result["part_sd"].get<double>()));
        require(result["reject_given_good"].is_number(), "非零合格概率被舍入为零");
        near(result["reject_given_good"], 1);
        near(result["good_rejected"].get<double>() / expected_good, 1);
    }
}
void normal_classification_origin_invariance() {
    auto cfg = default_config();
    cfg["design"] = {{"operators", 2}, {"replicates", 2}, {"parts", 5}};
    cfg["analysis"]["method"] = "crossed_anova";
    cfg["analysis"]["misclassification"] = true;
    cfg["analysis"]["historical_sd"] = 1e-8;

    auto tiny = data();
    for (auto &row : tiny)
        for (double &value : row)
            value *= 1e-10;
    constexpr double shift = 1e12;
    for (const char *side : {"lsl", "usl"}) {
        auto single = cfg;
        for (double origin : {0., shift, -shift}) {
            single["analysis"]["historical_mean"] = origin;
            single["specification"][side] = origin;
            const auto result = calculate(tiny, single)["misclassification"];
            // At the mean, conditional errors are equal for a single limit.
            const double expected = std::atan(result["gage_sd"].get<double>() /
                                              result["part_sd"].get<double>()) / std::acos(-1.);
            near(result["accept_given_bad"], expected);
            near(result["reject_given_good"], expected);
        }
    }

    auto interval_data = data();
    for (auto &row : interval_data)
        for (double &value : row)
            value *= 0x1p-16;
    cfg["analysis"]["historical_sd"] = 0x1p-12;
    const auto two_sided = [&](double origin) {
        auto both = cfg;
        both["analysis"]["historical_mean"] = origin;
        both["specification"]["lsl"] = origin - 0x1p-12;
        both["specification"]["usl"] = origin + 0x1p-11;
        return calculate(interval_data, both)["misclassification"];
    };
    const auto centered = two_sided(0);
    require(centered["bad_accepted"] > 0 && centered["good_rejected"] > 0,
            "平移测试缺少可比较的误分类概率");
    for (double origin : {shift, -shift}) {
        const auto shifted = two_sided(origin);
        for (const char *key : {"bad_accepted", "good_rejected", "accept_given_bad", "reject_given_good"})
            near(shifted[key], centered[key]);
    }

    cfg["analysis"]["historical_mean"] = shift;
    cfg["specification"]["lsl"] = shift;
    cfg["analysis"]["historical_sd"] = calculate(tiny, cfg)["GRR"];
    const auto no_part = calculate(tiny, cfg)["misclassification"];
    require(no_part["part_sd"] == 0 && no_part["accept_given_bad"].is_null(),
            "零部件变异时，单侧边界上的部件应属于合格总体");
    near(no_part["bad_accepted"], 0);
    near(no_part["reject_given_good"], .5);

    const auto no_gage = calculate(Matrix(4, std::vector<double>{0, 1, 2, 3, 4}),
                                   cfg)["misclassification"];
    require(no_gage["gage_sd"] == 0, "误分类边界测试的量具变异应为零");
    near(no_gage["accept_given_bad"], 0);
    near(no_gage["reject_given_good"], 0);
}
void historical_variation_with_automatic_sample_scale() {
    for (const char *method : {"crossed_xbarr", "crossed_anova"}) {
        auto cfg = default_config();
        cfg["analysis"]["method"] = method;
        cfg["analysis"]["historical_sd"] = 5;
        cfg["specification"]["lsl"] = 9.5;
        cfg["specification"]["usl"] = 10.5;
        cfg["generation"]["seed"] = 123;
        cfg["generation"]["decimal_places"] = 6;
        for (bool constrained : {false, true}) {
            if (constrained)
                cfg["targets"]["PT_pct"] = {{"mode", "target"}, {"value", 10}};
            const auto result = generate(cfg);
            require(qualified(result.metrics, result.config), "历史标准差阻塞了双侧自动尺度");
            require(result.metrics["study"]["historical_applied"] == true &&
                        result.metrics["TV"] == 5,
                    "自动样本尺度改变了历史过程口径");
            const double sample_tv = result.metrics["study"]["sample_tv"];
            require(sample_tv > 0 && sample_tv < 5, "双侧自动尺度没有生成有效的样本 TV");
        }

        cfg["targets"]["PT_pct"] = nullptr;
        cfg["analysis"]["historical_sd"] = .5;
        cfg["specification"]["lsl"] = .1;
        cfg["specification"]["usl"] = 2.9;
        cfg["generation"]["decimal_places"] = 0;
        const auto coarse = generate(cfg);
        require(qualified(coarse.metrics, coarse.config) && coarse.metrics["minimum"] == 1 &&
                    coarse.metrics["maximum"] == 2,
                "双侧自动尺度没有适应实际可用的输出精度网格");
    }
}
void optional_reports_preserve_selected_sample() {
    auto cfg = default_config();
    cfg["design"] = {{"operators", 2}, {"replicates", 2}, {"parts", 5}};
    cfg["analysis"]["method"] = "crossed_anova";
    cfg["specification"]["lsl"] = 9;
    cfg["specification"]["usl"] = 11;
    cfg["generation"]["seed"] = 123;
    cfg["targets"]["GRR_pct"] = {{"mode", "target"}, {"value", 10}};
    cfg["targets"]["PT_pct"] = {{"mode", "target"}, {"value", 10}};
    const auto baseline = generate(cfg);
    cfg["analysis"]["confidence"]["enabled"] = true;
    cfg["analysis"]["misclassification"] = true;
    auto enriched = generate(cfg);
    require(enriched.matrix == baseline.matrix && enriched.attempts == baseline.attempts,
            "可选报告改变了固定种子选出的样本");
    require(enriched.metrics.contains("confidence") &&
                enriched.metrics.contains("misclassification") &&
                enriched.metrics == calculate(enriched.matrix, enriched.config),
            "最终样本缺少可选统计报告");
    enriched.metrics.erase("confidence");
    enriched.metrics.erase("misclassification");
    require(enriched.metrics == baseline.metrics, "可选报告改变了点估计");
}
void confidence_unit_scaling() {
    // 只改变测量单位，置信限应按对应方差/标准差单位同比缩放。
    for (const char *method : {"mls", "satterthwaite"})
        for (const char *interaction : {"include", "exclude"})
            for (const char *bound : {"both", "lower", "upper"}) {
                auto cfg = default_config();
                cfg["design"] = {{"operators", 2}, {"replicates", 2}, {"parts", 5}};
                cfg["analysis"]["method"] = "crossed_anova";
                cfg["analysis"]["interaction"] = interaction;
                cfg["analysis"]["confidence"] = {{"enabled", true}, {"level", 95},
                                                    {"bound", bound}, {"method", method}};
                const auto reference = calculate(data(), cfg)["confidence"];
                for (double scale : {1e80, 1e-100}) {
                    auto matrix = data();
                    for (auto &row : matrix)
                        for (auto &value : row)
                            value *= scale;
                    const auto actual = calculate(matrix, cfg)["confidence"];
                    for (const char *quantity : {"variance_components", "stddev", "study_variation"}) {
                        const double units = std::string(quantity) == "variance_components"
                                                 ? scale * scale : scale;
                        for (const auto &component : reference[quantity].items())
                            for (const char *side : {"lower", "upper"}) {
                                const auto &expected = component.value()[side];
                                const auto &observed = actual[quantity][component.key()][side];
                                require(expected.is_null() == observed.is_null(),
                                        "测量单位改变了置信限的可计算状态");
                                if (!expected.is_null()) {
                                    require(std::isfinite(observed.get<double>()),
                                            "置信区间包含非有限数值");
                                    near(observed.get<double>() / units, expected);
                                }
                            }
                    }
                }
            }
}
void mls_confidence_domain() {
    for (int operators : {2, 3}) {
        auto config = default_config();
        config["design"] = {{"operators", operators}, {"replicates", 2}, {"parts", 5}};
        config["analysis"]["method"] = "crossed_anova";
        config["analysis"]["interaction"] = "include";
        auto &ci = config["analysis"]["confidence"];
        ci["enabled"] = true;
        const Matrix measurements(operators * 2, std::vector<double>{94, 97, 100, 103, 106});
        const auto rejected = [](auto action) {
            try {
                action();
            } catch (const ConfigError &error) {
                require(std::string(error.what()).find("Satterthwaite") != std::string::npos,
                        "MLS有效域错误缺少可执行恢复建议");
                return;
            }
            throw std::runtime_error("不受支持的低置信水平被MLS接受");
        };
        for (const char *bound : {"both", "lower", "upper"}) {
            ci["bound"] = bound;
            ci["level"] = std::string(bound) == "both" ? .01 : 50.;
            ci["method"] = "mls";
            rejected([&] { normalize_config(config); });
            rejected([&] { native_ui::to_config(native_ui::from_config(config)); });
            rejected([&] { calculate(measurements, config); });

            // 显式选择另一算法才可计算域外水平，不能悄悄替换 MLS。
            ci["method"] = "satterthwaite";
            const auto alternative = calculate(measurements, normalize_config(config));
            if (std::string(bound) == "lower")
                near(alternative["confidence"]["variance_components"]["PV"]["lower"],
                     26.812095631996385);
            if (std::string(bound) == "both")
                near(alternative["confidence"]["variance_components"]["PV"]["lower"],
                     26.809546414996706);

            ci["method"] = "mls";
            ci["level"] = std::string(bound) == "both"
                              ? (operators == 2 ? 36.537899 : 26.424112)
                              : (operators == 2 ? 68.268950 : 63.212056);
            (void)calculate(measurements, normalize_config(config));
            ci["level"] = 95;
            const auto normal = calculate(measurements, normalize_config(config));
            if (std::string(bound) == "lower") {
                // 交互、误差均方为零时，PV区间退化为DF=4的精确卡方区间。
                // 独立分位数：chi2.ppf(.95,4)=9.487729036781154。
                near(normal["confidence"]["variance_components"]["PV"]["lower"],
                     22.5 * 4 / 9.487729036781154);
            }
        }
        ci["level"] = .01;
        ci["enabled"] = false;
        require(normalize_config(config)["analysis"]["confidence"]["level"] == .01,
                "未启用的置信配置不应阻止保存");
        ci["enabled"] = true;
        config["analysis"]["method"] = "crossed_xbarr";
        (void)normalize_config(config);
    }
}
void historical_one_sided_confidence() {
    // 同一尾概率下，95%单侧界限应等于90%双侧区间的对应端点。
    // 这个关系同时检查历史TV-PV变换后的方向及派生量，不依赖重复实现MLS公式。
    for (const char *method : {"mls", "satterthwaite"})
      for (const char *interaction : {"include", "exclude"})
        for (const char *process : {"historical", "study"})
            for (double level : {90., 95., 99.}) {
                auto config = default_config();
                config["design"] = {{"operators", 2}, {"replicates", 2}, {"parts", 5}};
                config["analysis"]["method"] = "crossed_anova";
                config["analysis"]["interaction"] = interaction;
                config["analysis"]["historical_sd"] = 10;
                config["analysis"]["process_variation"] = process;
                config["analysis"]["study_multiplier"] = 5.15;
                config["specification"]["lsl"] = 0;
                config["specification"]["usl"] = 200;
                auto &ci = config["analysis"]["confidence"];
                ci = {{"enabled", true}, {"level", 2 * level - 100}, {"bound", "both"},
                      {"method", method}};
                const auto paired = calculate(data(), config)["confidence"];
                if (std::string(method) == "satterthwaite") {
                    const auto &df = paired["effective_df"];
                    if (std::string(process) == "historical") {
                        require(df["TV"].is_null() && df["PV"].is_null() && df["GRR"].is_number(),
                                "历史变换区间保留了研究估计的自由度");
                        require(paired.at("interval_source")["TV"] == "fixed_historical_variance" &&
                                    paired["interval_source"]["PV"] == "historical_tv_minus_grr_variance",
                                "历史区间的来源不明确");
                    } else
                        require(df["TV"].is_number() && df["PV"].is_number(),
                                "研究估计的自由度不应被清除");
                }
                for (const char *bound : {"lower", "upper"}) {
                    ci["level"] = level;
                    ci["bound"] = bound;
                    const auto actual = calculate(data(), config)["confidence"];
                    const auto other = std::string(bound) == "lower" ? "upper" : "lower";
                    for (const char *quantity : {"variance_components", "stddev", "study_variation",
                                                 "tolerance_pct", "process_pct"})
                        for (const auto &component : paired[quantity].items()) {
                            const auto &interval = actual[quantity][component.key()];
                            if (interval[bound].is_null() || !interval[other].is_null())
                                throw std::runtime_error("历史过程单侧置信限方向错误：" +
                                                         std::string(quantity) + "." +
                                                         component.key() + "." + bound);
                            near(interval[bound], component.value()[bound]);
                        }
                }
            }
}
void native_gui_reference(const std::filesystem::path &path) {
    const auto reference = Json::parse(read_text(path));
    const auto matrices =
        Json::parse(read_text(path.parent_path() / reference["matrix_fixture"].get<std::string>()));
    const auto found =
        std::find_if(matrices["cases"].begin(), matrices["cases"].end(),
                     [&](const Json &value) { return value["id"] == reference["matrix_case"]; });
    require(found != matrices["cases"].end(), "找不到原生 GUI 对标的数据矩阵");
    const Matrix measurements = (*found)["matrix"].get<Matrix>();
    std::size_t checked = 0;
    for (const auto &scenario : reference["cases"]) {
        auto config = normalize_config((*found)["config"]);
        config["analysis"].update(scenario["analysis"], true);
        config["specification"].update(scenario["specification"], true);
        const auto metrics = calculate(measurements, normalize_config(config));
        for (const auto &check : scenario["checks"]) {
            const Json::json_pointer pointer(check["path"].get<std::string>());
            const double actual = metrics.at(pointer).get<double>();
            if (std::abs(actual - check["expected"].get<double>()) >
                check["tolerance"].get<double>())
                throw std::runtime_error("原生 GUI 基准不一致：" +
                                         scenario["id"].get<std::string>() + " " +
                                         check["path"].get<std::string>());
            ++checked;
        }
    }
    require(checked == reference.at("check_count").get<std::size_t>(),
            "原生 GUI 对标检查数量不完整");
    std::cout << checked << " 项原生 GUI 显示精度基准通过\n";
}
} // namespace
int main(int argc, char **argv) {
    try {
        narrow_specification_with_analysis_tolerance();
        narrow_normal_classification_interval();
        normal_classification_origin_invariance();
        historical_variation_with_automatic_sample_scale();
        optional_reports_preserve_selected_sample();
        mls_confidence_domain();
        historical_one_sided_confidence();
        confidence_unit_scaling();
        auto cfg = default_config();
        cfg["design"] = {{"operators", 2}, {"replicates", 2}, {"parts", 5}};
        cfg["specification"]["lsl"] = 0;
        cfg["specification"]["usl"] = 200;
        cfg["analysis"]["method"] = "crossed_anova";
        auto baseline = calculate(data(), cfg);
        near(chi_square_quantile(.95, 2), -2 * std::log(.05));
        near(chi_square_quantile(.025, 10), 3.2469727802368413);
        near(chi_square_quantile(.975, 10), 20.483177350807388);
        cfg["analysis"]["confidence"]["enabled"] = true;
        auto confidence = calculate(data(), cfg)["confidence"];
        near(confidence["variance_components"]["EV"]["lower"], 20 / 20.483177350807388);
        near(confidence["variance_components"]["EV"]["upper"], 20 / 3.2469727802368413);
        for (const char *bound : {"both", "lower", "upper"}) {
            cfg["analysis"]["confidence"]["bound"] = bound;
            const auto value = calculate(data(), cfg);
            require(value["anova"] == baseline["anova"], "置信区间改变 ANOVA 点估计");
            require(value["confidence"]["variance_components"]["GRR"]["lower"].is_null() ==
                        (std::string(bound) == "upper"),
                    "单侧下限设置错误");
            require(value["confidence"]["variance_components"]["GRR"]["upper"].is_null() ==
                        (std::string(bound) == "lower"),
                    "单侧上限设置错误");
        }
        cfg["analysis"]["confidence"]["bound"] = "both";
        cfg["analysis"]["historical_sd"] = 10;
        auto historical = calculate(data(), cfg);
        near(historical["EV"], std::sqrt(2.));
        near(historical["AV"], std::sqrt(5.));
        near(historical["GRR"], std::sqrt(7.));
        near(historical["TV"], 10);
        near(historical["PV"], std::sqrt(93.));
        near(historical["GRR_pct"], 10 * std::sqrt(7.));
        near(historical["confidence"]["variance_components"]["TV"]["lower"], 100);
        near(historical["confidence"]["variance_components"]["TV"]["upper"], 100);
        require(historical["ndc"] == 5, "历史过程 ndc 错误");
        require(historical["anova"]["table"] == baseline["anova"]["table"],
                "历史过程参数改变了 ANOVA 原始表");
        cfg["analysis"]["process_variation"] = "study";
        auto study = calculate(data(), cfg);
        near(study["TV"], baseline["TV"]);
        near(study["study"]["components"]["GRR"]["process_pct"], 10 * std::sqrt(7.));
        cfg["analysis"]["historical_sd"] = 1;
        cfg["analysis"]["process_variation"] = "historical";
        near(calculate(data(), cfg)["TV"], baseline["TV"]);
        cfg["analysis"]["historical_sd"] = nullptr;
        cfg["specification"]["usl"] = nullptr;
        near(calculate(data(), cfg)["PT_pct"], baseline["PT_pct"]);
        cfg["analysis"]["historical_mean"] = 90;
        near(calculate(data(), cfg)["PT_pct"], 100 * 6 * std::sqrt(7.) / 180);
        cfg["analysis"]["tolerance"] = 50;
        near(calculate(data(), cfg)["PT_pct"], 100 * 6 * std::sqrt(7.) / 50);
        require(native_ui::to_config(native_ui::from_config(cfg)) == normalize_config(cfg),
                "新增分析配置 GUI 往返丢失字段");
        cfg["analysis"]["confidence"]["method"] = "satterthwaite";
        require(native_ui::to_config(native_ui::from_config(cfg)) == normalize_config(cfg),
                "Satterthwaite算法配置GUI往返丢失字段");
        const auto satterthwaite = calculate(data(), cfg);
        require(satterthwaite["anova"] == calculate(data(), [&] {
                    auto original = cfg;
                    original["analysis"]["confidence"]["method"] = "mls";
                    return original;
                }())["anova"], "置信区间算法改变ANOVA点估计");
        auto boundary_config = cfg;
        boundary_config["analysis"]["process_variation"] = "historical";
        boundary_config["analysis"]["historical_sd"] = 10;
        for (const char *bound : {"both", "lower", "upper"}) {
            boundary_config["analysis"]["confidence"]["bound"] = bound;
            const auto boundary = calculate(Matrix(4, std::vector<double>(5, 100.)),
                                            boundary_config);
            require(boundary["confidence"]["variance_components"]["GRR"]["upper"].is_null(),
                    "零方差Satterthwaite区间不应伪造确定上限");
            require(boundary["confidence"]["variance_components"]["PV"]["lower"].is_null(),
                    "历史PV不能从不可计算的GRR上限构造下限");
        }
        cfg["analysis"]["historical_mean"] = nullptr;
        cfg["analysis"]["tolerance"] = nullptr;
        cfg["analysis"]["misclassification"] = true;
        cfg["specification"]["lsl"] = 100;
        auto classification = calculate(data(), cfg)["misclassification"];
        // 均值处单侧规格：两种联合误判率均为 acos(rho)/(2*pi)。
        const double expected = std::acos(std::sqrt(20. / 27)) / (2 * std::acos(-1.));
        near(classification["bad_accepted"], expected);
        near(classification["good_rejected"], expected);
        near(classification["accept_given_bad"], 2 * expected);
        near(classification["reject_given_good"], 2 * expected);
        for (const char *method : {"crossed_xbarr", "crossed_anova"}) {
            auto generation = default_config();
            generation["analysis"]["method"] = method;
            generation["analysis"]["historical_sd"] = .333333333333;
            generation["specification"]["lsl"] = 9;
            generation["specification"]["usl"] = 11;
            generation["generation"]["seed"] = 123;
            generation["generation"]["decimal_places"] = 6;
            generation["targets"]["GRR_pct"] = {{"mode", "target"}, {"value", 10}};
            generation["targets"]["PT_pct"] = {{"mode", "target"}, {"value", 10}};
            auto result = generate(generation);
            require(qualified(result.metrics, result.config), "历史标准差约束生成失败");
            auto independent_scale = generation;
            independent_scale["analysis"]["historical_sd"] = .5;
            independent_scale["generation"]["scale_mode"] = "reference_tv_sd";
            independent_scale["generation"]["reference_tv_sd"] = .333333333333;
            independent_scale["targets"]["PT_pct"]["value"] = 15;
            const auto independent = generate(independent_scale);
            require(qualified(independent.metrics, independent.config),
                    "历史与参考尺度组合生成失败");
            near(independent.metrics["study"]["sample_tv"], .333333333333, 2e-5);
            generation["analysis"]["historical_sd"] = nullptr;
            generation["specification"]["usl"] = nullptr;
            generation["generation"]["reference_tv_sd"] = .333333333333;
            require(qualified(generate(generation).metrics, generation), "单侧 P/T 约束生成失败");
        }
        if (argc > 1)
            native_gui_reference(std::filesystem::u8path(argv[1]));
        std::cout << "分析参数解析解、配置往返、历史尺度与单侧生成测试通过\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
