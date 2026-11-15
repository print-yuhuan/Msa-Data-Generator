#include "charts.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <iterator>
#include <limits>

namespace msa::charts {
namespace {
constexpr const char *ink = "#243247", *muted = "#69778A", *grid = "#E8EDF3", *green = "#16866B",
                     *red = "#BC4049", *amber = "#B87918";

std::string number(double value) {
    if (!std::isfinite(value))
        return "—";
    char buffer[96]{};
    const auto converted =
        std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::general, 6);
    return converted.ec == std::errc{} ? std::string(buffer, converted.ptr) : "—";
}
std::string exact_number(double value) {
    char buffer[96]{};
    const auto converted = std::to_chars(buffer, buffer + sizeof(buffer), value);
    return converted.ec == std::errc{} ? std::string(buffer, converted.ptr) : number(value);
}
std::string bar_number(double value) {
    if (value == 0)
        return "0";
    if (value < .01)
        return "<0.01";
    if (value < 1000)
        return fixed(value, 2);
    char buffer[64]{};
    const auto converted =
        std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::general, 3);
    return converted.ec == std::errc{} ? std::string(buffer, converted.ptr) : number(value);
}
std::string operator_name(int i) { return std::string(1, char('A' + i)); }
double mean(const std::vector<double> &values) {
    if (values.empty())
        throw ConfigError("图表数据不能为空");
    std::vector<double> offsets;
    for (double value : values)
        offsets.push_back(value - values.front());
    const double value = values.front() + accurate_sum(offsets) / double(values.size());
    if (!std::isfinite(value))
        throw ConfigError("图表均值超出有限数值范围");
    return value;
}
double numeric(const Json &value, const std::string &name, bool nonnegative = false) {
    if (!value.is_number())
        throw ConfigError(name + "必须为有限数值");
    const double result = value.get<double>();
    if (!std::isfinite(result) || (nonnegative && result < 0))
        throw ConfigError(name + "包含无效数值");
    return result;
}
double nice_step(double span, int intervals) {
    const double rough = span / std::max(1, intervals);
    const double power = std::pow(10., std::floor(std::log10(rough)));
    if (!(power > 0) || !std::isfinite(power))
        return rough;
    const double fraction = rough / power;
    for (double step : {1., 2., 2.5, 5., 10.})
        if (fraction <= step)
            return step * power;
    return 10 * power;
}
bool valid_color(const std::string &color) {
    return color.size() == 7 && color[0] == '#' &&
           std::all_of(color.begin() + 1, color.end(), [](unsigned char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
           });
}
void line(Scene &s, float x, float y, float x2, float y2, const std::string &color, float width = 1,
          bool dashed = false) {
    Primitive p;
    p.type = Primitive::Type::Line;
    p.x = x;
    p.y = y;
    p.x2 = x2;
    p.y2 = y2;
    p.color = color;
    p.stroke_width = width;
    p.dashed = dashed;
    s.items.push_back(std::move(p));
}
void rect(Scene &s, float x, float y, float w, float h, const std::string &color) {
    Primitive p;
    p.type = Primitive::Type::Rect;
    p.x = x;
    p.y = y;
    p.x2 = w;
    p.y2 = h;
    p.color = color;
    s.items.push_back(std::move(p));
}
void text(Scene &s, float x, float y, float w, float h, const std::string &value, float size = 12,
          const std::string &color = ink, Primitive::Align align = Primitive::Align::Left) {
    Primitive p;
    p.type = Primitive::Type::Text;
    p.x = x;
    p.y = y;
    p.x2 = w;
    p.y2 = h;
    p.text = value;
    p.font_size = size;
    p.color = color;
    p.align = align;
    s.items.push_back(std::move(p));
}
struct Frame {
    Scene &scene;
    float margin, legend_top, axis_label_top, plot_top, plot_bottom, x_label_top, note_top;
    bool compact;
    Frame(Scene &s, const Result &result, const Editor &editor, Kind kind, int operators,
          int parts, int repeats)
        : scene(s), margin(s.width < 600 ? 14.f : 24.f), compact(s.height < 360 || s.width < 500) {
        const bool anova = result.config.at("analysis").at("method") == "crossed_anova";
        const float titleSize = compact ? 17.f : 21.f;
        text(s, margin, 10, s.width - 2 * margin, titleSize + 8, editor.title, titleSize);
        const std::string design = std::to_string(operators) + " 名操作员 × " +
                                   std::to_string(parts) + " 部件 × " +
                                   std::to_string(repeats) + " 次重复 · N = " +
                                   std::to_string(operators * parts * repeats);
        const std::string method = anova ? "ANOVA（方差分析法）" : "Xbar-R（均值极差法）";
        float y = compact ? 36.f : 43.f;
        if (s.width >= 720) {
            text(s, margin, y, s.width - 2 * margin, 18, method + "  ·  " + design, 12, muted);
            y += 20;
        } else {
            text(s, margin, y, s.width - 2 * margin, 16, method, 11, muted);
            text(s, margin, y + 17, s.width - 2 * margin, 16, design, 10.5f, muted);
            y += 34;
        }
        if (anova) {
            const auto &model = result.metrics.at("anova");
            if (!model.at("interaction_included").is_boolean())
                throw ConfigError("ANOVA 最终模型的交互项状态无效");
            const bool included = model.at("interaction_included").get<bool>();
            const auto &pv = model.at("interaction_p");
            if (!pv.is_null() && numeric(pv, "交互项 P 值", true) > 1)
                throw ConfigError("ANOVA 交互项 P 值必须在 0～1 范围内");
            const std::string p = pv.is_null() ? "不可计算" : number(numeric(pv, "交互项 P 值", true));
            const std::string modelNote =
                s.width < 600
                    ? std::string("最终模型：") + (included ? "含交互" : "合并交互") +
                          " · 全模型交互 P = " + p
                    : std::string("最终模型：") +
                          (included ? "保留交互项" : "交互项合并到误差") +
                          " · 完整模型交互 P = " + p;
            text(s, margin, y, s.width - 2 * margin, 17, modelNote, compact ? 10.f : 11.f,
                 muted);
            y += 18;
        }
        legend_top = y + 2;
        axis_label_top = legend_top + (editor.show_legend ? 22.f : 3.f);
        plot_top = axis_label_top + 20;
        if (kind == Kind::RangeChart || kind == Kind::MeanChart)
            plot_top += 34;
        plot_bottom = s.height - (compact ? 66.f : 77.f);
        if (plot_bottom - plot_top < 28)
            throw ConfigError("当前图表高度不足以显示标题、图例与坐标，请增大图表高度");
        x_label_top = s.height - (compact ? 42.f : 49.f);
        note_top = s.height - 22;
    }
};
struct Data {
    int operators, repeats, parts;
    std::vector<double> all;
    std::vector<std::vector<double>> by_part, by_operator, cells, cell_means, cell_ranges;
    std::vector<double> operator_mean, operator_range;
    explicit Data(const Result &result) {
        const auto &design = result.config.at("design");
        operators = design.at("operators").get<int>();
        repeats = design.at("replicates").get<int>();
        parts = design.at("parts").get<int>();
        if (operators < 2 || operators > 3 || repeats < 2 || repeats > 3 || parts < 5 ||
            parts > 10 || result.matrix.size() != std::size_t(operators * repeats))
            throw ConfigError("图表需要完整的平衡交叉研究数据");
        by_part.resize(parts);
        by_operator.resize(operators);
        cells.resize(operators * parts);
        cell_means.resize(operators);
        cell_ranges.resize(operators);
        for (int op = 0; op < operators; ++op)
            for (int trial = 0; trial < repeats; ++trial) {
                const auto &row = result.matrix[op * repeats + trial];
                if (row.size() != std::size_t(parts))
                    throw ConfigError("图表数据的部件数量与研究设置不符");
                for (int part = 0; part < parts; ++part) {
                    const double v = row[part];
                    if (!std::isfinite(v))
                        throw ConfigError("图表中的测量值必须为有限数字");
                    all.push_back(v);
                    by_part[part].push_back(v);
                    by_operator[op].push_back(v);
                    cells[op * parts + part].push_back(v);
                }
            }
        for (int op = 0; op < operators; ++op) {
            for (int part = 0; part < parts; ++part) {
                const auto &cell = cells[op * parts + part];
                const auto bounds = std::minmax_element(cell.begin(), cell.end());
                cell_means[op].push_back(mean(cell));
                const double range = *bounds.second - *bounds.first;
                if (!std::isfinite(range))
                    throw ConfigError("图表子组极差超出有限数值范围");
                cell_ranges[op].push_back(range);
            }
            operator_mean.push_back(mean(by_operator[op]));
            operator_range.push_back(mean(cell_ranges[op]));
        }
    }
};
struct Plot {
    Frame &frame;
    Scene &scene;
    const Editor &editor;
    float left = 72, top, width, height;
    double xlow, xhigh, low, high, origin = 0, step = 0;
    Plot(Frame &f, const Editor &e, double minx, double maxx, double miny, double maxy,
         bool zero = false, bool integer = false, float panelLeft = -1, float panelWidth = -1,
         bool showY = true)
        : frame(f), scene(f.scene), editor(e), top(f.plot_top),
          width(f.scene.width - 96), height(f.plot_bottom - f.plot_top), xlow(minx),
          xhigh(maxx), low(miny), high(maxy) {
        if (panelLeft >= 0)
            left = panelLeft;
        if (panelWidth > 0)
            width = panelWidth;
        if (!std::isfinite(low) || !std::isfinite(high) || !std::isfinite(xlow) ||
            !std::isfinite(xhigh))
            throw ConfigError("图表坐标超出有限数值范围");
        if (!(high > low)) {
            const double span = low == 0 ? 1. : std::max(1e-6, std::abs(low) * .01);
            low -= span;
            high += span;
        } else {
            const double margin = (high - low) * .08;
            low -= margin;
            high += margin;
        }
        if (zero)
            low = 0;
        if (e.y_min)
            low = *e.y_min;
        if (e.y_max)
            high = *e.y_max;
        if (!(high > low) || !std::isfinite(high - low))
            throw ConfigError("纵轴最小值必须小于最大值，且范围必须为有限数值");
        if (!(xhigh > xlow) || !std::isfinite(xhigh - xlow))
            throw ConfigError("横轴范围无效");
        // 大中心值与微小变异同时出现时显式标注偏移，避免刻度舍入成相同文本。
        origin = std::abs(low) > (high - low) * 10000 ? low : 0;
        // 小尺寸分面仍要留下足够的刻度文字间距，不能沿用正常画布的刻度数量。
        step = nice_step(high - low, std::clamp(int(height / 42), 1, 6));
        if (integer)
            step = std::max(1., std::ceil(step));
        if (!(step > 0) || !std::isfinite(step))
            throw ConfigError("图表刻度间距超出有效范围");
        if (!e.y_min)
            low = origin + std::floor((low - origin) / step) * step;
        if (!e.y_max)
            high = origin + std::ceil((high - origin) / step) * step;
        if (!(high > low) || !std::isfinite(high - low))
            throw ConfigError("图表自动刻度无法表示当前数值范围");
        const double first = std::ceil((low - origin) / step) * step;
        for (int i = 0; i < 20; ++i) {
            const double value = origin + first + i * step;
            if (value > high + step * 1e-8)
                break;
            const float yy = y(value);
            if (e.show_grid)
                line(scene, left, yy, left + width, yy, grid);
            if (showY) {
                line(scene, left - 4, yy, left, yy, muted);
                const double shown = std::abs(value - origin) < step * 1e-9 ? 0 : value - origin;
                text(scene, 3, yy - 8, left - 12, 18, number(shown), 11, muted,
                     Primitive::Align::Right);
            }
        }
        line(scene, left, top, left, top + height, showY ? muted : grid);
        line(scene, left, top + height, left + width, top + height, muted);
        if (showY) {
            text(scene, 72, f.axis_label_top, scene.width - 96, 18,
                 e.y_label + (origin ? "（值 = 刻度 + " + exact_number(origin) + "）" : ""), 11,
                 muted);
            text(scene, 72, f.x_label_top, scene.width - 96, 20, e.x_label, 12, muted,
                 Primitive::Align::Center);
        }
    }
    float x(double value) const { return left + float((value - xlow) / (xhigh - xlow)) * width; }
    float y(double value) const { return top + float((high - value) / (high - low)) * height; }
    void tick(double value, const std::string &label, float box = 56) {
        const float xx = x(value);
        line(scene, xx, top + height, xx, top + height + 4, muted);
        text(scene, xx - box / 2, top + height + 5, box, 18, label, 11, muted,
             Primitive::Align::Center);
    }
    // 裁剪数据线段而不是夹紧端点，避免用户缩窄坐标轴后伪造边界平台。
    void segment(double a, double b, double c, double d, const std::string &color,
                 float stroke = -1, bool dashed = false) {
        double x1 = a, y1 = b, x2 = c, y2 = d;
        double begin = 0, end = 1;
        const double dx = x2 - x1, dy = y2 - y1;
        const auto clip = [&](double p, double q) {
            if (p == 0)
                return q >= 0;
            const double t = q / p;
            if (p < 0)
                begin = std::max(begin, t);
            else
                end = std::min(end, t);
            return begin <= end;
        };
        if (!clip(-dx, x1 - xlow) || !clip(dx, xhigh - x1) || !clip(-dy, y1 - low) ||
            !clip(dy, high - y1))
            return;
        line(scene, x(x1 + begin * dx), y(y1 + begin * dy), x(x1 + end * dx), y(y1 + end * dy),
             color, stroke < 0 ? editor.line_width : stroke, dashed);
    }
    void point(double xx, double yy, const std::string &color, float radius = -1) {
        if (xx < xlow || xx > xhigh || yy < low || yy > high)
            return;
        Primitive p;
        p.type = Primitive::Type::Circle;
        p.x = x(xx);
        p.y = y(yy);
        p.x2 = radius < 0 ? editor.point_radius : radius;
        // 边界上的零值仍保留标记中心，避免控制图中的零极差点消失。
        p.color = color;
        scene.items.push_back(std::move(p));
    }
    void diamond(double xx, double yy, const std::string &color, float radius = 4) {
        if (xx < xlow || xx > xhigh || yy < low || yy > high)
            return;
        const float px = x(xx), py = y(yy);
        line(scene, px, py - radius, px + radius, py, color, 1.5f);
        line(scene, px + radius, py, px, py + radius, color, 1.5f);
        line(scene, px, py + radius, px - radius, py, color, 1.5f);
        line(scene, px - radius, py, px, py - radius, color, 1.5f);
    }
    void bar(double a, double b, double c, double d, const std::string &color) {
        const double ax = std::max(xlow, std::min(a, c)), bx = std::min(xhigh, std::max(a, c)),
                     ay = std::max(low, std::min(b, d)), by = std::min(high, std::max(b, d));
        if (bx > ax && by > ay)
            rect(scene, x(ax), y(by), x(bx) - x(ax), y(ay) - y(by), color);
    }
};
void legend(Frame &frame, const Editor &e,
            const std::vector<std::pair<std::string, std::string>> &items) {
    if (!e.show_legend || items.empty())
        return;
    auto &s = frame.scene;
    const float cell = (s.width - 2 * frame.margin) / float(items.size());
    for (std::size_t i = 0; i < items.size(); ++i) {
        const float x = frame.margin + i * cell;
        line(s, x, frame.legend_top + 8, x + 13, frame.legend_top + 8, items[i].second, 2);
        text(s, x + 18, frame.legend_top, cell - 20, 18, items[i].first,
             frame.compact ? 10.f : 11.f, muted);
    }
}
std::vector<std::string> colors(const Editor &e) {
    const auto rgb = [](const std::string &color) {
        return std::array<int, 3>{std::stoi(color.substr(1, 2), nullptr, 16),
                                  std::stoi(color.substr(3, 2), nullptr, 16),
                                  std::stoi(color.substr(5, 2), nullptr, 16)};
    };
    const std::array<std::string, 5> candidates{"#16866B", "#C48428", "#7B51C2", "#CB4A5B",
                                                "#3B65E8"};
    std::vector<std::string> palette{e.primary_color};
    // 后续序列从候选色中选与已有颜色距离最大的颜色，避免自选主色使操作员同色。
    while (palette.size() < 4) {
        double best = -1;
        std::string choice;
        for (const auto &candidate : candidates) {
            const auto a = rgb(candidate);
            double nearest = std::numeric_limits<double>::max();
            for (const auto &used : palette) {
                const auto b = rgb(used);
                const double dr = a[0] - b[0], dg = a[1] - b[1], db = a[2] - b[2];
                nearest = std::min(nearest, dr * dr + dg * dg + db * db);
            }
            if (nearest > best) {
                best = nearest;
                choice = candidate;
            }
        }
        palette.push_back(choice);
    }
    return palette;
}
void operators_legend(Frame &frame, const Editor &e, int count, bool averages = false) {
    std::vector<std::pair<std::string, std::string>> items;
    const auto palette = colors(e);
    for (int i = 0; i < count; ++i)
        items.emplace_back("操作员 " + operator_name(i), palette[i]);
    if (averages)
        items.emplace_back("均值", ink);
    legend(frame, e, items);
}
std::vector<std::pair<std::string, double>> specs(const Result &r) {
    std::vector<std::pair<std::string, double>> out;
    const auto &s = r.config.at("specification");
    for (const char *key : {"lsl", "usl"})
        if (s.contains(key) && !s[key].is_null()) {
            const double v = numeric(s[key], "规格限");
            out.emplace_back(std::string(key) == "lsl" ? "LSL" : "USL", v);
        }
    return out;
}
std::pair<double, double> measurement_bounds(const Data &data, const Result &result,
                                             const Editor &editor) {
    auto bounds = std::minmax_element(data.all.begin(), data.all.end());
    double low = *bounds.first, high = *bounds.second;
    if (editor.show_limits)
        for (const auto &limit : specs(result)) {
            low = std::min(low, limit.second);
            high = std::max(high, limit.second);
        }
    return {low, high};
}
void specification_lines(Plot &plot, const Result &result) {
    if (!plot.editor.show_limits)
        return;
    for (const auto &limit : specs(result)) {
        plot.segment(plot.xlow, limit.second, plot.xhigh, limit.second, amber, 1, true);
        if (limit.second >= plot.low && limit.second <= plot.high)
            text(plot.scene, plot.left + plot.width - 92, plot.y(limit.second) - 15, 90, 15,
                 limit.first + " " + number(limit.second), 10, amber, Primitive::Align::Right);
    }
}
double percentile(const std::vector<double> &sorted, double q) {
    const double index = (sorted.size() - 1) * q;
    const auto below = std::size_t(std::floor(index)), above = std::size_t(std::ceil(index));
    return sorted[below] + (sorted[above] - sorted[below]) * (index - below);
}

void components(Frame &frame, const Result &result, const Editor &e) {
    auto &s = frame.scene;
    const char *keys[]{"GRR", "EV", "AV", "PV"};
    const double total = numeric(result.metrics.at("TV"), "总标准差 TV", true);
    std::vector<double> sd, variance, tolerance, process;
    for (const char *key : keys) {
        const double sigma = numeric(result.metrics.at(key), std::string(key) + " 标准差", true);
        const Json *component = nullptr;
        if (result.metrics.contains("study") && result.metrics["study"].contains("components")) {
            const auto &all = result.metrics["study"]["components"];
            if (all.contains(key))
                component = &all[key];
        }
        if (total > 0) {
            const double ratio = sigma / total;
            if (!std::isfinite(100 * ratio * ratio))
                throw ConfigError("变异分量百分比超出有限范围");
            sd.push_back(component && component->contains("study_variation_pct")
                             ? numeric(component->at("study_variation_pct"), "研究变异占比", true)
                             : 100 * ratio);
            variance.push_back(component && component->contains("contribution_pct")
                                   ? numeric(component->at("contribution_pct"), "方差贡献率", true)
                                   : 100 * ratio * ratio);
        }
        if (component) {
            if (component->contains("tolerance_pct") && !component->at("tolerance_pct").is_null())
                tolerance.push_back(numeric(component->at("tolerance_pct"), "公差占比", true));
            if (component->contains("process_pct") && !component->at("process_pct").is_null())
                process.push_back(numeric(component->at("process_pct"), "过程占比", true));
        }
    }
    std::vector<std::vector<double>> series;
    std::vector<std::pair<std::string, std::string>> entries;
    const auto palette = colors(e);
    if (!sd.empty()) {
        series.push_back(sd);
        series.push_back(variance);
        entries.emplace_back("研究变异 %", palette[0]);
        entries.emplace_back("方差贡献 %", palette[1]);
    }
    if (tolerance.size() == 4) {
        series.push_back(tolerance);
        entries.emplace_back("公差占比 %", palette[2]);
    } else if (!tolerance.empty())
        throw ConfigError("公差占比缺少完整的变异分量，请重新分析数据");
    if (process.size() == 4) {
        series.push_back(process);
        entries.emplace_back("过程占比 %", palette[3]);
    } else if (!process.empty())
        throw ConfigError("历史过程占比缺少完整的变异分量，请重新分析数据");
    if (series.empty()) {
        text(s, frame.margin, frame.plot_top, s.width - 2 * frame.margin, 50,
             "总变异为零，研究变异百分比与方差贡献率不可计算；当前无有效公差或历史过程占比。", 13,
             muted);
        return;
    }
    double ymax = sd.empty() ? 0 : 100;
    for (const auto &values : series)
        ymax = std::max(ymax, *std::max_element(values.begin(), values.end()));
    Editor axes = e;
    if (total == 0)
        axes.y_label += "（TV = 0，仅显示可计算占比）";
    Plot p(frame, axes, -.6, 3.6, 0, ymax, true);
    legend(frame, e, entries);
    const double spacing = .72 / series.size();
    for (int i = 0; i < 4; ++i) {
        for (std::size_t j = 0; j < series.size(); ++j) {
            const double x = i - .36 + j * spacing, value = series[j][i];
            p.bar(x + spacing * .08, 0, x + spacing * .92, value, entries[j].second);
            if (s.width >= 700 && value >= p.low && value <= p.high && p.y(value) - 17 >= p.top)
                text(s, p.x(x), p.y(value) - 17, p.x(x + spacing) - p.x(x), 16,
                     bar_number(value), series.size() == 4 ? 9.f : 10.f, muted,
                     Primitive::Align::Center);
        }
        p.tick(i, keys[i]);
    }
}
void by_part(Frame &frame, const Result &result, const Data &data, const Editor &e) {
    const auto bounds = measurement_bounds(data, result, e);
    Plot p(frame, e, -.6, data.parts - .4, bounds.first, bounds.second);
    operators_legend(frame, e, data.operators, true);
    const auto palette = colors(e);
    double previous = 0;
    for (int part = 0; part < data.parts; ++part) {
        p.tick(part, std::to_string(part + 1));
        const double center = mean(data.by_part[part]);
        if (part)
            p.segment(part - 1, previous, part, center, ink, 1.2f);
        previous = center;
        for (int op = 0; op < data.operators; ++op)
            for (int trial = 0; trial < data.repeats; ++trial) {
                const double offset = (op - (data.operators - 1) / 2.) * .17 +
                                      (trial - (data.repeats - 1) / 2.) * .045;
                p.point(part + offset, data.cells[op * data.parts + part][trial], palette[op]);
            }
        p.point(part, center, "#FFFFFF", e.point_radius + 2);
        p.diamond(part, center, ink, e.point_radius + 1);
    }
    specification_lines(p, result);
}
void by_operator(Frame &frame, const Result &result, const Data &data, const Editor &e) {
    const auto bounds = measurement_bounds(data, result, e);
    Plot p(frame, e, -.7, data.operators - .3, bounds.first, bounds.second);
    operators_legend(frame, e, data.operators, true);
    const auto palette = colors(e);
    for (int op = 0; op < data.operators; ++op) {
        auto values = data.by_operator[op];
        std::sort(values.begin(), values.end());
        const double q1 = percentile(values, .25), med = percentile(values, .5),
                     q3 = percentile(values, .75), iqr = q3 - q1;
        const auto lower = std::lower_bound(values.begin(), values.end(), q1 - 1.5 * iqr);
        const auto upper = std::upper_bound(values.begin(), values.end(), q3 + 1.5 * iqr);
        const double lo = lower == values.end() ? q1 : *lower,
                     hi = upper == values.begin() ? q3 : *std::prev(upper);
        const double box = op - .12;
        p.bar(box - .14, q1, box + .14, q3, "#F0F4FA");
        p.segment(box - .14, q1, box + .14, q1, palette[op]);
        p.segment(box - .14, q3, box + .14, q3, palette[op]);
        p.segment(box - .14, q1, box - .14, q3, palette[op]);
        p.segment(box + .14, q1, box + .14, q3, palette[op]);
        p.segment(box - .14, med, box + .14, med, palette[op]);
        p.segment(box, lo, box, q1, palette[op]);
        p.segment(box, q3, box, hi, palette[op]);
        p.segment(box - .08, lo, box + .08, lo, palette[op]);
        p.segment(box - .08, hi, box + .08, hi, palette[op]);
        for (std::size_t j = 0; j < data.by_operator[op].size(); ++j) {
            const double offset = .20 + (double(j % 7) - 3) * .025;
            p.point(op + offset, data.by_operator[op][j], palette[op], e.point_radius * .65f);
        }
        if (op)
            p.segment(box - 1, data.operator_mean[op - 1], box, data.operator_mean[op], ink, 1);
        p.point(box, data.operator_mean[op], "#FFFFFF", e.point_radius + 2);
        p.diamond(box, data.operator_mean[op], ink, e.point_radius + 1);
        p.tick(op, operator_name(op));
    }
    specification_lines(p, result);
}
void control_chart(Frame &frame, const Data &data, const Editor &e, bool ranges) {
    auto &s = frame.scene;
    const auto &values = ranges ? data.cell_ranges : data.cell_means;
    // 平衡交叉研究的全部操作员×部件子组合并估计，共享中心与控制限。
    const double a2 = data.repeats == 2 ? 1.880 : 1.023, d4 = data.repeats == 2 ? 3.267 : 2.574;
    const double rbar = mean(data.operator_range), grand = mean(data.all);
    const double center = ranges ? rbar : grand;
    const double lower = ranges ? 0. : grand - a2 * rbar;
    const double upper = ranges ? d4 * rbar : grand + a2 * rbar;
    if (!std::isfinite(lower) || !std::isfinite(upper))
        throw ConfigError("控制限超出有限数值范围");
    double low = std::numeric_limits<double>::max(), high = -low;
    for (int op = 0; op < data.operators; ++op) {
        for (double value : values[op]) {
            low = std::min(low, value);
            high = std::max(high, value);
        }
    }
    if (e.show_limits) {
        low = std::min(low, lower);
        high = std::max(high, upper);
    }
    std::vector<std::pair<std::string, std::string>> entries{
        {ranges ? "子组极差 R" : "子组均值", e.primary_color}};
    if (e.show_limits) {
        entries.emplace_back("中心线 CL", green);
        entries.emplace_back("控制限 / 超限点", red);
    }
    legend(frame, e, entries);
    if (e.show_limits)
        text(s, 72, frame.axis_label_top + 17, s.width - 96, 17,
             "UCL = " + number(upper) + "    CL = " + number(center) + "    LCL = " +
                 number(lower),
             frame.compact ? 9.f : 11.f, muted);
    const float gap = 12, available = s.width - 96;
    const float panelWidth = (available - gap * (data.operators - 1)) / data.operators;
    const int skip = std::max(1, int(std::ceil(data.parts * 21.f / panelWidth)));
    for (int op = 0; op < data.operators; ++op) {
        const float left = 72 + op * (panelWidth + gap);
        Plot p(frame, e, -.45, data.parts - .55, low, high, ranges, false, left, panelWidth,
               op == 0);
        rect(s, left, p.top - 17, panelWidth, 16, "#F3F6FA");
        text(s, left, p.top - 16, panelWidth, 16, "操作员 " + operator_name(op), 10.5f, ink,
             Primitive::Align::Center);
        if (e.show_limits) {
            p.segment(p.xlow, center, p.xhigh, center, green, 1.2f);
            p.segment(p.xlow, lower, p.xhigh, lower, red, 1, true);
            p.segment(p.xlow, upper, p.xhigh, upper, red, 1, true);
        }
        for (int part = 0; part < data.parts; ++part) {
            if (part)
                p.segment(part - 1, values[op][part - 1], part, values[op][part], e.primary_color);
            const double value = values[op][part];
            const bool beyond = e.show_limits && (value < lower || value > upper);
            p.point(part, value, e.primary_color);
            if (beyond && value >= p.low && value <= p.high) {
                const float radius = e.point_radius + 1;
                rect(s, p.x(part) - radius, p.y(value) - radius, 2 * radius, 2 * radius, red);
            }
            if (part % skip == 0 || part == data.parts - 1)
                p.tick(part, std::to_string(part + 1), 22);
        }
    }
}
void interaction(Frame &frame, const Result &result, const Data &data, const Editor &e) {
    double low = std::numeric_limits<double>::max(), high = -low;
    for (const auto &values : data.cell_means)
        for (double value : values) {
            low = std::min(low, value);
            high = std::max(high, value);
        }
    if (e.show_limits)
        for (const auto &limit : specs(result)) {
            low = std::min(low, limit.second);
            high = std::max(high, limit.second);
        }
    Plot p(frame, e, -.4, data.parts - .6, low, high);
    operators_legend(frame, e, data.operators);
    const auto palette = colors(e);
    for (int part = 0; part < data.parts; ++part)
        p.tick(part, std::to_string(part + 1));
    for (int op = 0; op < data.operators; ++op)
        for (int part = 0; part < data.parts; ++part) {
            if (part)
                p.segment(part - 1, data.cell_means[op][part - 1], part, data.cell_means[op][part],
                          palette[op]);
            p.point(part, data.cell_means[op][part], palette[op]);
        }
    specification_lines(p, result);
}
void histogram(Frame &frame, const Result &result, const Data &data, const Editor &e) {
    auto &s = frame.scene;
    const auto observed = std::minmax_element(data.all.begin(), data.all.end());
    double binLow = *observed.first, binHigh = *observed.second;
    if (!(binHigh > binLow)) {
        const double width = binLow == 0 ? 1. : std::max(1e-6, std::abs(binLow) * .01);
        binLow -= width;
        binHigh += width;
    }
    const int count = std::clamp(int(std::ceil(std::sqrt(data.all.size()))), 5, 20);
    const double step = (binHigh - binLow) / count;
    if (!(step > 0) || !std::isfinite(step))
        throw ConfigError("直方图分箱范围超出有效数值范围");
    std::vector<int> bins(count);
    for (double value : data.all) {
        const int index = std::clamp(int((value - binLow) / step), 0, count - 1);
        ++bins[index];
    }
    // 规格仅扩大显示范围，不改变数据分箱和频数。
    auto bounds = measurement_bounds(data, result, e);
    bounds.first = std::min(bounds.first, binLow);
    bounds.second = std::max(bounds.second, binHigh);
    const double origin =
        std::abs(bounds.first) > (bounds.second - bounds.first) * 10000 ? bounds.first : 0;
    Editor axes = e;
    if (origin)
        axes.x_label += "（值 = 刻度 + " + exact_number(origin) + "）";
    Plot p(frame, axes, bounds.first, bounds.second, 0,
           *std::max_element(bins.begin(), bins.end()), true, true);
    std::vector<std::pair<std::string, std::string>> entries{{"测量值频数", e.primary_color}};
    if (e.show_limits && !specs(result).empty())
        entries.emplace_back("规格限", amber);
    legend(frame, e, entries);
    for (int i = 0; i < count; ++i)
        p.bar(binLow + step * (i + .02), 0, binLow + step * (i + .98), bins[i], e.primary_color);
    const double tickStep = nice_step(bounds.second - bounds.first, s.width < 650 ? 3 : 5);
    const double first = std::ceil((bounds.first - origin) / tickStep) * tickStep;
    for (int i = 0; i < 20; ++i) {
        const double value = origin + first + tickStep * i;
        if (value > bounds.second + tickStep * 1e-8)
            break;
        p.tick(value, number(value - origin), 78);
    }
    if (e.show_limits)
        for (const auto &limit : specs(result)) {
            p.segment(limit.second, p.low, limit.second, p.high, amber, 1, true);
            text(s, std::clamp(p.x(limit.second) - 42, p.left, p.left + p.width - 84), p.top + 5,
                 84, 17, limit.first + " " + number(limit.second), 10, amber,
                 Primitive::Align::Center);
        }
}
} // namespace

const std::vector<Kind> &all_kinds() {
    static const std::vector<Kind> kinds{
        Kind::VarianceComponents, Kind::MeasurementByPart, Kind::MeasurementByOperator,
        Kind::RangeChart,         Kind::MeanChart,         Kind::Interaction,
        Kind::Histogram};
    return kinds;
}
const char *label(Kind kind) {
    switch (kind) {
    case Kind::VarianceComponents:
        return "变异分量";
    case Kind::MeasurementByPart:
        return "测量值与部件";
    case Kind::MeasurementByOperator:
        return "操作员分布";
    case Kind::RangeChart:
        return "极差控制图";
    case Kind::MeanChart:
        return "均值控制图";
    case Kind::Interaction:
        return "部件 × 操作员交互图";
    case Kind::Histogram:
        return "测量值直方图";
    }
    throw ConfigError("未知图表类型");
}
const char *description(Kind kind) {
    switch (kind) {
    case Kind::VarianceComponents:
        return "GRR / EV / AV / PV：沿用本机研究变异与贡献率；存在有效容差或历史标准差时显示相应公差/过程占比。";
    case Kind::MeasurementByPart:
        return "彩色点为完整测量记录；菱形及连线为部件均值；轻微横向偏移仅用于区分重叠。";
    case Kind::MeasurementByOperator:
        return "箱体为 Q1–Q3、中线为中位数、须线为 1.5 IQR 内观测；菱形为均值；四分位数采用线性插值。";
    case Kind::RangeChart:
        return "按操作员分面，共享全部子组平均极差；两种分析法均用传统 D3 / D4 控制限，不等同于所有软件的默认设置。";
    case Kind::MeanChart:
        return "共享全体测量总均值与合并平均极差；两种分析法均用传统 A2 控制限，超限不等同于量具不合格。";
    case Kind::Interaction:
        return "每点为一名操作员对一个部件的重复测量均值；图形不替代交互项显著性检验。";
    case Kind::Histogram:
        return "按样本范围等宽分箱；规格限不改变分箱或频数；不包含正态拟合或过程能力判定。";
    }
    throw ConfigError("未知图表类型");
}
Editor default_editor(Kind kind) {
    Editor e;
    e.title = label(kind);
    e.y_label = kind == Kind::VarianceComponents ? "百分比（%）"
                : kind == Kind::Histogram        ? "频数"
                : kind == Kind::RangeChart       ? "子组极差 R"
                : kind == Kind::MeanChart        ? "子组均值"
                : kind == Kind::Interaction      ? "平均测量值"
                                                 : "测量值";
    e.x_label = kind == Kind::VarianceComponents                      ? "变异来源"
                : kind == Kind::MeasurementByOperator                 ? "操作员"
                : kind == Kind::RangeChart || kind == Kind::MeanChart ? "部件（各操作员分面）"
                : kind == Kind::Histogram                             ? "测量值"
                                                                      : "部件";
    return e;
}
Scene make_scene(const Result &result, Kind kind, const Editor &editor, float width, float height) {
    (void)label(kind);
    if (!std::isfinite(width) || !std::isfinite(height) || width < 360 || height < 260 ||
        width > 10000 || height > 10000)
        throw ConfigError("单张图表尺寸须在 360×260 至 10000×10000 范围内");
    if (!valid_color(editor.primary_color) || !std::isfinite(editor.line_width) ||
        editor.line_width < .5f || editor.line_width > 6 || !std::isfinite(editor.point_radius) ||
        editor.point_radius < 1 || editor.point_radius > 8)
        throw ConfigError("图表颜色、线宽或点大小无效");
    if ((editor.y_min && !std::isfinite(*editor.y_min)) ||
        (editor.y_max && !std::isfinite(*editor.y_max)))
        throw ConfigError("纵轴范围必须为有限数字");
    if (editor.y_min && editor.y_max && !(*editor.y_max > *editor.y_min))
        throw ConfigError("纵轴最小值必须小于最大值");
    if (editor.title.size() > 600 || editor.x_label.size() > 300 || editor.y_label.size() > 300)
        throw ConfigError("图表标题或坐标轴标签过长");
    try {
        const auto &method = result.config.at("analysis").at("method");
        if (method != "crossed_anova" && method != "crossed_xbarr")
            throw ConfigError("图表分析方法不受支持");
        const Data data(result);
        Scene scene{width, height, {}};
        rect(scene, 0, 0, width, height, "#FFFFFF");
        Frame frame(scene, result, editor, kind, data.operators, data.parts, data.repeats);
        const std::string note =
            kind == Kind::RangeChart || kind == Kind::MeanChart
                ? "传统 A2 / D3 / D4 · 全部子组合并估计控制限"
            : kind == Kind::MeasurementByOperator ? "箱线与完整观测；菱形为均值，横线为中位数"
            : kind == Kind::VarianceComponents ? "研究变异、贡献率、公差及历史过程占比分别解释"
            : kind == Kind::Interaction ? "子组均值；交互形态不替代显著性检验"
            : kind == Kind::Histogram ? "样本范围等宽分箱；未拟合分布或评估过程能力"
                                      : "彩色点为测量值；菱形和连线为部件均值";
        text(scene, frame.margin, frame.note_top, width - 2 * frame.margin, 18, note,
             frame.compact ? 9.f : 10.f, muted);
        switch (kind) {
        case Kind::VarianceComponents:
            components(frame, result, editor);
            break;
        case Kind::MeasurementByPart:
            by_part(frame, result, data, editor);
            break;
        case Kind::MeasurementByOperator:
            by_operator(frame, result, data, editor);
            break;
        case Kind::RangeChart:
            control_chart(frame, data, editor, true);
            break;
        case Kind::MeanChart:
            control_chart(frame, data, editor, false);
            break;
        case Kind::Interaction:
            interaction(frame, result, data, editor);
            break;
        case Kind::Histogram:
            histogram(frame, result, data, editor);
            break;
        }
        return scene;
    } catch (const Json::exception &) {
        throw ConfigError("图表所需的研究配置或统计结果缺少字段，或字段类型不正确，请重新分析数据");
    }
}
} // namespace msa::charts
