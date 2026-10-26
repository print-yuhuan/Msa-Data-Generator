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
        mls_confidence_domain();
        historical_one_sided_confidence();
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
