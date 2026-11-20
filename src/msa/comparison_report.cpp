#include "comparison_report.hpp"
#include "chart_svg.hpp"
#include "report_pdf.hpp"
#include <iomanip>
#include <locale>
#include <sstream>

namespace msa {
namespace {
std::string shown(const Json &value) {
    if (value.is_null())
        return "—";
    if (value.is_number_integer())
        return value.dump();
    std::ostringstream text;
    text.imbue(std::locale::classic());
    text << std::setprecision(10) << value.get<double>();
    return text.str();
}

void numeric_cell(std::ostream &out, const Json &value) {
    out << "<td data-value=\"" << value.dump() << "\">" << shown(value) << "</td>";
}

std::string tolerance_name(const Json &study) {
    const auto &source = study.at("tolerance_source");
    if (source == "interval")
        return "独立分析容差";
    if (source == "two_sided")
        return "双侧规格宽度";
    if (source == "lower_only")
        return "参考均值到下限距离的两倍";
    if (source == "upper_only")
        return "参考均值到上限距离的两倍";
    return "未定义";
}

void chart(std::ostream &out, const Result &result, charts::Kind kind) {
    const auto scene = charts::make_scene(result, kind, charts::default_editor(kind), 720, 425);
    out << "<figure><svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 " << scene.width << ' '
        << scene.height << "\" role=\"img\"><title>" << charts::xml_text(charts::label(kind))
        << "</title>\n";
    charts::write_svg_items(out, scene, "chart-" + std::to_string(static_cast<int>(kind)));
    out << "</svg></figure>\n";
}

constexpr auto page_start = R"HTML(<!doctype html>
<html lang="zh-CN"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>交叉量具 R&amp;R 双方法对比报告</title><style>
*{box-sizing:border-box}body{margin:0;background:#f4f6f8;color:#192b40;font:16px/1.55 "Microsoft YaHei UI","Noto Sans CJK SC",sans-serif}
main{max-width:960px;margin:28px auto;padding:36px 40px;background:white}h1{font-size:1.8em;margin:0 0 .3em}
h2{font-size:1.2em;margin:1.4em 0 .45em}p{margin:.35em 0}header{border-bottom:2px solid #38536c;padding-bottom:14px}
table{width:100%;border-collapse:collapse;font-variant-numeric:tabular-nums}th,td{padding:7px 12px;border-bottom:1px solid #cbd5dd;text-align:right}
th:first-child{text-align:left}thead{background:#edf2f6}tbody th{font-weight:500}figure{margin:12px 0;break-inside:avoid}svg{display:block;width:100%;height:auto}
small{color:#435a6d}section{overflow-wrap:anywhere}
@media(max-width:600px){main{margin:0;padding:16px}th,td{padding:5px;font-size:.85em}}
@page{size:A4;margin:14mm}@media print{body{background:white;font-size:10pt}main{margin:0;padding:0;max-width:none}table,tr{break-inside:avoid}.plots{break-before:page}h2{break-after:avoid}}
</style></head><body><main><header><h1>交叉量具 R&amp;R 双方法对比报告</h1>
)HTML";
} // namespace

void write_comparison_report(const std::filesystem::path &path, const Result &study,
                             StudySource source, const std::string &source_name,
                             ExistingFile policy) {
    Result reference;
    reference.matrix = study.matrix;
    auto cfg = study.config;
    // The comparison uses point estimates. An inactive inference preference in
    // an Xbar-R study must not block calculating the companion ANOVA estimate.
    cfg["analysis"]["confidence"]["enabled"] = false;
    cfg["analysis"]["misclassification"] = false;
    cfg["analysis"]["method"] = "crossed_xbarr";
    reference.config = normalize_config(cfg);
    reference.metrics = calculate(reference.matrix, reference.config);
    cfg["analysis"]["method"] = "crossed_anova";
    const auto anova = calculate(reference.matrix, normalize_config(cfg));
    const auto &xbarr = reference.metrics;
    const auto &model = anova.at("anova");
    const auto &design = reference.config.at("design");
    const auto &spec = reference.config.at("specification");
    const auto &analysis = reference.config.at("analysis");
    const auto &xstudy = xbarr.at("study");
    const auto &astudy = anova.at("study");
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << page_start << "<p>" << (source == StudySource::Generated ? "模拟生成" : "数据导入");
    if (!source_name.empty())
        out << " · " << charts::xml_text(source_name);
    if (source == StudySource::Generated)
        out << " · 数据生成种子 " << study.seed;
    out << " · MSA " << charts::xml_text(version) << "</p></header>\n<section><h2>研究设置</h2><p>"
        << design.at("operators") << " 名操作员 × " << design.at("parts") << " 个部件 × "
        << design.at("replicates") << " 次重复测量，共 "
        << study.matrix.size() * design.at("parts").get<int>() << " 个读数。操作员为";
    const auto names = operator_names(design);
    for (int op = 0; op < design.at("operators").get<int>(); ++op)
        out << (op ? "、" : "") << charts::xml_text(names[op]);
    out << "。</p><p>规格下限 " << shown(spec.at("lsl")) << "，规格上限 " << shown(spec.at("usl"))
        << "，研究变异倍数 "
        << shown(analysis.at("study_multiplier")) << "，历史过程标准差 "
        << shown(analysis.at("historical_sd")) << "。</p>";
    if (source == StudySource::Generated &&
        study.config.at("analysis").at("method") == "crossed_anova")
        out << "<p>生成时的子组波动模式为"
            << (study.config.at("generation").value("residual_model", std::string("standard")) ==
                        "varied"
                    ? "教学波动"
                    : "标准")
            << "。</p>";
    out << "</section><section><h2>同一矩阵的统计结果</h2>"
           "<table><thead><tr><th>指标</th><th>Xbar-R</th><th>ANOVA</th></tr></thead><tbody>\n";
    struct Metric {
        const char *key;
        const char *name;
    };
    for (const Metric m : {Metric{"EV_pct", "重复性 EV/TV (%)"},
                           {"AV_pct", "再现性 AV/TV (%)"},
                           {"GRR_pct", "总量具 GRR/TV (%)"},
                           {"PT_pct", "P/T (%)"},
                           {"ndc", "可区分类别数 ndc"},
                           {"GRR", "量具标准差 GRR"},
                           {"PV", "部件标准差 PV"},
                           {"TV", "总标准差 TV"}}) {
        out << "<tr data-metric=\"" << m.key << "\"><th scope=\"row\">" << m.name << "</th>";
        numeric_cell(out, xbarr.at(m.key));
        numeric_cell(out, anova.at(m.key));
        out << "</tr>\n";
    }
    out << "<tr><th scope=\"row\">TV 来源</th>";
    for (const auto *item : {&xstudy, &astudy})
        out << "<td>" << (item->at("historical_applied").get<bool>() ? "历史标准差" : "研究数据")
            << "</td>";
    out << "</tr></tbody></table><p><small>— 表示当前数据或设置下无法计算。</small></p>"
           "</section><section><h2>分析口径</h2><p>P/T 的分母取"
        << tolerance_name(xstudy) << "，有效容差 " << shown(xstudy.at("effective_tolerance"))
        << "，参考均值 " << shown(xstudy.at("reference_mean")) << "。</p><p>ANOVA 交互项策略为";
    const auto &mode = model.at("interaction_mode");
    out << (mode == "auto"      ? "自动选择"
            : mode == "include" ? "强制保留"
                                : "强制合并")
        << "，α = " << shown(model.at("interaction_alpha"))
        << "，完整模型 P = " << shown(model.at("interaction_p")) << "。最终模型"
        << (model.at("interaction_included").get<bool>() ? "保留部件与操作员的交互项"
                                                         : "将交互项合并入误差")
        << "。</p></section><section class=\"plots\"><h2>测量子组控制图</h2>"
           "<p>两种分析使用同一组读数。图中按操作员分面，控制限采用均值极差法。</p>";
    chart(out, reference, charts::Kind::RangeChart);
    chart(out, reference, charts::Kind::MeanChart);
    out << "</section></main></body></html>\n";
    auto content = out.str();
    auto extension = path.extension().u8string();
    for (auto &letter : extension)
        if (letter >= 'A' && letter <= 'Z')
            letter = static_cast<char>(letter - 'A' + 'a');
    if (extension == ".pdf")
        content = print_report_pdf(content);
    atomic_text(path, content, policy);
}
} // namespace msa
