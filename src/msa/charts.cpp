#include "charts.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <iterator>
#include <limits>

namespace msa::charts {
namespace {
constexpr const char *ink = "#263543", *muted = "#576675", *grid = "#E4E9EE", *axis = "#536271",
                     *green = "#18785C", *red = "#B13E48", *amber = "#9A6016";

std::string number(double value, int precision = 6) {
    if (!std::isfinite(value))
        return "—";
    char buffer[96]{};
    const auto converted =
        std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::general, precision);
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
// 仅在标签框放不下时缩略，切点始终位于 UTF-8 字符边界。
std::string operator_label(const std::string &name, float width, float size) {
    float used = 0;
    std::size_t cut = 0;
    for (std::size_t i = 0; i < name.size(); ++i) {
        const auto byte = static_cast<unsigned char>(name[i]);
        if ((byte & 0xc0) == 0x80)
            continue;
        if (used + size <= width)
            cut = i;
        used += size * (byte < 128 && byte != 'W' && byte != 'M' ? .7f : 1.f);
        if (used > width)
            return name.substr(0, cut) + "…";
    }
    return name;
}
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
MarkerShape series_shape(const Editor &e, int series) {
    constexpr MarkerShape shapes[]{MarkerShape::Square, MarkerShape::Circle, MarkerShape::Triangle};
    return e.marker_shape == MarkerShape::BySeries ? shapes[series % 3] : e.marker_shape;
}
void marker(Scene &s, MarkerShape shape, float x, float y, float r, const std::string &color) {
    if (shape == MarkerShape::Square) {
        rect(s, x - r, y - r, 2 * r, 2 * r, color);
    } else if (shape == MarkerShape::Diamond) {
        const float edge = std::max(1.f, r * .45f);
        line(s, x, y - r, x + r, y, color, edge);
        line(s, x + r, y, x, y + r, color, edge);
        line(s, x, y + r, x - r, y, color, edge);
        line(s, x - r, y, x, y - r, color, edge);
    } else if (shape == MarkerShape::Triangle) {
        const float edge = std::max(1.f, r * .55f);
        line(s, x, y - r, x + r, y + r * .8f, color, edge);
        line(s, x + r, y + r * .8f, x - r, y + r * .8f, color, edge);
        line(s, x - r, y + r * .8f, x, y - r, color, edge);
    } else {
        Primitive p;
        p.type = Primitive::Type::Circle;
        p.x = x;
        p.y = y;
        p.x2 = r;
        p.color = color;
        s.items.push_back(std::move(p));
    }
}
struct Frame {
    Scene &scene;
    float margin, plot_left, plot_right, legend_top, axis_label_top, plot_top, plot_bottom,
        x_label_top, note_top;
    bool compact;
    Frame(Scene &s, const Result &result, const Editor &editor, Kind kind, int operators,
          int parts, int repeats)
        : scene(s), margin(s.width < 600 ? 14.f : 24.f), compact(s.height < 360 || s.width < 500) {
        const bool anova = result.config.at("analysis").at("method") == "crossed_anova";
        plot_left = compact ? 58.f : 76.f;
        plot_right = s.width - margin;
        const float titleSize = compact ? 16.f : 21.f;
        const float inner = s.width - 2 * margin;
        text(s, margin, compact ? 4.f : 10.f, inner, titleSize + 8,
             operator_label(editor.title, inner, titleSize), titleSize, ink,
             Primitive::Align::Center);
        const std::string design = std::to_string(operators) + " 名操作员 × " +
                                   std::to_string(parts) + " 部件 × " +
                                   std::to_string(repeats) + " 次重复 · N = " +
                                   std::to_string(operators * parts * repeats);
        const std::string method = anova ? "ANOVA（方差分析法）" : "Xbar-R（均值极差法）";
        float y = compact ? 27.f : 41.f;
        const std::string methodLabel = s.width < 600 ? (anova ? "ANOVA" : "Xbar-R") : method;
        text(s, margin, y, inner, 17, methodLabel + "  ·  " + design, compact ? 10.f : 12.f, muted,
             Primitive::Align::Center);
        y += compact ? 17 : 21;
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
            text(s, margin, y, s.width - 2 * margin, 17, modelNote, compact ? 10.f : 11.f, muted,
                 Primitive::Align::Center);
            y += compact ? 15 : 18;
        }
        const float legendHeight = editor.show_legend ? (compact ? 19.f : 23.f) : 0.f;
        const bool legendBelow = editor.legend_position == LegendPosition::Bottom;
        note_top = s.height - (compact ? 17.f : 24.f);
        legend_top = legendBelow ? note_top - legendHeight - 3 : y;
        x_label_top = note_top - (legendBelow ? legendHeight : 0) - (compact ? 22.f : 27.f);
        plot_bottom = x_label_top - (compact ? 19.f : 22.f);
        axis_label_top = y + (legendBelow ? 0 : legendHeight);
        plot_top = axis_label_top + (compact ? 18.f : 22.f);
        if (kind == Kind::RangeChart || kind == Kind::MeanChart)
            plot_top += (editor.show_limits ? 17.f : 0.f) + 19.f;
        if (plot_bottom - plot_top < 28)
            throw ConfigError("当前图表高度不足以显示标题、图例与坐标，请增大图表高度");
    }
};
struct Data {
    int operators, repeats, parts;
    std::array<std::string, 3> names;
    std::vector<double> all;
    std::vector<std::vector<double>> by_part, by_operator, cells, cell_means, cell_ranges;
    std::vector<double> operator_mean, operator_range;
    explicit Data(const Result &result) {
        const auto &design = result.config.at("design");
        names = operator_names(design);
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
    float left, top, width, height;
    double xlow, xhigh, low, high, origin = 0, step = 0;
    Plot(Frame &f, const Editor &e, double minx, double maxx, double miny, double maxy,
         bool zero = false, bool integer = false, float panelLeft = -1, float panelWidth = -1,
         bool showY = true)
        : frame(f), scene(f.scene), editor(e), left(f.plot_left), top(f.plot_top),
          width(f.plot_right - f.plot_left), height(f.plot_bottom - f.plot_top), xlow(minx),
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
            const double margin = (high - low) * .06;
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
        // 取接近数据的整刻度作偏移，而非带有留白和浮点尾数的坐标下界。
        if (std::abs(low) > (high - low) * 10000) {
            const double unit = nice_step(high - low, 1);
            const double rounded = std::nearbyint(low / unit) * unit;
            origin = std::isfinite(rounded) ? rounded : low;
        }
        // 小尺寸分面仍要留下足够的刻度文字间距，不能沿用正常画布的刻度数量。
        const int intervals = e.y_tick_intervals
                                  ? std::min(e.y_tick_intervals, std::max(1, int(height / 21)))
                                  : std::clamp(int(height / 38), 2, 7);
        step = nice_step(high - low, intervals);
        if (integer)
            step = std::max(1., std::ceil(step));
        if (!(step > 0) || !std::isfinite(step))
            throw ConfigError("图表刻度间距超出有效范围");
        // 仅在离外侧整刻度较近时延伸边界，避免上下各增加一整格空白。
        const double floorTick = origin + std::floor((low - origin) / step) * step;
        const double ceilTick = origin + std::ceil((high - origin) / step) * step;
        if (!e.y_min && (zero || low - floorTick <= step * .35))
            low = floorTick;
        if (!e.y_max && (zero || ceilTick - high <= step * .35))
            high = ceilTick;
        if (!(high > low) || !std::isfinite(high - low))
            throw ConfigError("图表自动刻度无法表示当前数值范围");
        const double first = std::ceil((low - origin) / step) * step;
        for (int i = 0; i < 20; ++i) {
            const double offset = first + i * step;
            const double value = origin + offset;
            if (value > high + step * 1e-8)
                break;
            const float yy = y(value);
            if (e.show_grid)
                line(scene, left, yy, left + width, yy, grid);
            if (showY) {
                line(scene, left - 4, yy, left, yy, axis);
                const double shown = std::abs(offset) < step * 1e-9 ? 0 : offset;
                text(scene, 2, yy - 8, left - 10, 17, number(shown), 11, ink,
                     Primitive::Align::Right);
            }
            if (!integer && showY && y(value) - y(value + step) >= 35 && value + step / 2 <= high) {
                const float minor = y(value + step / 2);
                line(scene, left - 2, minor, left, minor, axis);
            }
        }
        line(scene, left, top, left + width, top, "#A8B4BF");
        line(scene, left + width, top, left + width, top + height, "#A8B4BF");
        line(scene, left, top, left, top + height, showY ? axis : grid);
        line(scene, left, top + height, left + width, top + height, axis);
        if (showY) {
            const float available = f.plot_right - f.plot_left;
            const auto ylabel =
                e.y_label + (origin ? "（值 = 刻度 + " + exact_number(origin) + "）" : "");
            text(scene, f.plot_left, f.axis_label_top, available, 18,
                 operator_label(ylabel, available, 12), 12, ink);
            text(scene, f.plot_left, f.x_label_top, available, 19,
                 operator_label(e.x_label, available, 12), 12, ink, Primitive::Align::Center);
        }
    }
    float x(double value) const { return left + float((value - xlow) / (xhigh - xlow)) * width; }
    float y(double value) const { return top + float((high - value) / (high - low)) * height; }
    void tick(double value, const std::string &label, float box = 56) {
        const float xx = x(value);
        line(scene, xx, top + height, xx, top + height + 4, axis);
        if (std::all_of(label.begin(), label.end(), [](unsigned char c) { return c < 128; }))
            box = std::min(box, 8.f * label.size() + 4.f);
        const float labelX = std::clamp(xx - box / 2, 0.f, scene.width - box);
        text(scene, labelX, top + height + 4, box, 16, label, 11, ink, Primitive::Align::Center);
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
    void point(double xx, double yy, const std::string &color, float radius = -1,
               MarkerShape shape = MarkerShape::Circle) {
        if (xx < xlow || xx > xhigh || yy < low || yy > high)
            return;
        marker(scene, shape, x(xx), y(yy), radius < 0 ? editor.point_radius : radius, color);
    }
    void bar(double a, double b, double c, double d, const std::string &color) {
        const double ax = std::max(xlow, std::min(a, c)), bx = std::min(xhigh, std::max(a, c)),
                     ay = std::max(low, std::min(b, d)), by = std::min(high, std::max(b, d));
        if (bx > ax && by > ay)
            rect(scene, x(ax), y(by), x(bx) - x(ax), y(ay) - y(by), color);
    }
};
enum class Key { Bar, Line, Dash, Marker, LineMarker };
struct LegendItem {
    std::string label, color;
    Key key = Key::Bar;
    MarkerShape shape = MarkerShape::Circle;
};
void legend(Frame &frame, const Editor &e, const std::vector<LegendItem> &items) {
    if (!e.show_legend || items.empty())
        return;
    auto &s = frame.scene;
    const float size = frame.compact ? 10.f : 12.f;
    const float maxCell = (s.width - 2 * frame.margin) / float(items.size());
    std::vector<float> cells;
    float total = 0;
    for (const auto &item : items) {
        float length = 0;
        for (unsigned char c : item.label)
            if ((c & 0xc0) != 0x80)
                length += (c < 128 ? .7f : 1.f) * size;
        cells.push_back(std::min(maxCell, length + 40));
        total += cells.back();
    }
    float x = (s.width - total) / 2;
    for (std::size_t i = 0; i < items.size(); ++i) {
        const auto &item = items[i];
        const float y = frame.legend_top + 8;
        if (item.key == Key::Bar)
            rect(s, x + 4, y - 4, 12, 8, item.color);
        if (item.key == Key::Line || item.key == Key::Dash || item.key == Key::LineMarker)
            line(s, x + 1, y, x + 19, y, item.color,
                 item.key == Key::LineMarker ? e.line_width : 1.5f, item.key == Key::Dash);
        if (item.key == Key::Marker || item.key == Key::LineMarker)
            marker(s, item.shape, x + 10, y, std::min(e.point_radius + .5f, 6.f), item.color);
        text(s, x + 24, frame.legend_top, cells[i] - 27, 17,
             operator_label(item.label, cells[i] - 27, size), size, ink);
        x += cells[i];
    }
}
std::array<int, 3> rgb(const std::string &color) {
    return {std::stoi(color.substr(1, 2), nullptr, 16), std::stoi(color.substr(3, 2), nullptr, 16),
            std::stoi(color.substr(5, 2), nullptr, 16)};
}
std::string light_fill(const std::string &color) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string tint = "#";
    for (const int channel : rgb(color)) {
        const int mixed = (channel + 255) / 2;
        tint += hex[mixed / 16];
        tint += hex[mixed % 16];
    }
    return tint;
}
std::vector<std::string> colors(const Editor &e) {
    std::vector<std::string> palette{e.primary_color};
    const auto primary = rgb(e.primary_color);
    // 使用固定的色序，在自选主色接近候选色时跳过该色。
    for (const char *candidate :
         {"#C85F22", "#10857D", "#795991", "#2F65A4", "#9C433B", "#BB9134", "#415346"}) {
        const auto c = rgb(candidate);
        const int dr = c[0] - primary[0], dg = c[1] - primary[1], db = c[2] - primary[2];
        if (dr * dr + dg * dg + db * db > 56 * 56)
            palette.emplace_back(candidate);
        if (palette.size() == 4)
            break;
    }
    return palette;
}
void operators_legend(Frame &frame, const Editor &e, const Data &data, bool averages = false) {
    std::vector<LegendItem> items;
    const auto palette = colors(e);
    for (int i = 0; i < data.operators; ++i)
        items.push_back({"操作员 " + data.names[i], palette[i],
                         averages ? Key::Marker : Key::LineMarker, series_shape(e, i)});
    if (averages)
        items.push_back({"均值", ink, Key::LineMarker, MarkerShape::Diamond});
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
    auto limits = specs(result);
    float previousTop = plot.top + plot.height + 16;
    for (const auto &limit : limits) {
        plot.segment(plot.xlow, limit.second, plot.xhigh, limit.second, amber, 1, true);
        if (limit.second < plot.low || limit.second > plot.high)
            continue;
        float top = std::min(plot.y(limit.second) - 15, previousTop - 15);
        const bool upperWillFit =
            limits.size() == 2 && limit.first == "LSL" && limits.back().second <= plot.high;
        top = std::clamp(top, plot.top + (upperWillFit ? 15.f : 0.f), plot.top + plot.height - 15);
        text(plot.scene, plot.left + 2, top, plot.width - 4, 15,
             limit.first + " " + exact_number(limit.second), 10, amber, Primitive::Align::Right);
        previousTop = top;
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
    std::vector<LegendItem> entries;
    const auto palette = colors(e);
    if (!sd.empty()) {
        series.push_back(sd);
        series.push_back(variance);
        entries.push_back({"研究变异 %", palette[0]});
        entries.push_back({"方差贡献 %", palette[1]});
    }
    if (tolerance.size() == 4) {
        series.push_back(tolerance);
        entries.push_back({"公差占比 %", palette[2]});
    } else if (!tolerance.empty())
        throw ConfigError("公差占比缺少完整的变异分量，请重新分析数据");
    if (process.size() == 4) {
        series.push_back(process);
        entries.push_back({"过程占比 %", palette[3]});
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
            p.bar(x + spacing * .08, 0, x + spacing * .92, value, entries[j].color);
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
    operators_legend(frame, e, data, true);
    specification_lines(p, result);
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
                p.point(part + offset, data.cells[op * data.parts + part][trial], palette[op],
                        e.point_radius, series_shape(e, op));
            }
        p.point(part, center, "#FFFFFF", e.point_radius + 2);
        p.point(part, center, ink, e.point_radius + 1, MarkerShape::Diamond);
    }
}
void by_operator(Frame &frame, const Result &result, const Data &data, const Editor &e) {
    const auto bounds = measurement_bounds(data, result, e);
    Plot p(frame, e, -.7, data.operators - .3, bounds.first, bounds.second);
    operators_legend(frame, e, data, true);
    specification_lines(p, result);
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
        p.bar(box - .14, q1, box + .14, q3, light_fill(palette[op]));
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
            p.point(op + offset, data.by_operator[op][j], palette[op], e.point_radius * .75f,
                    series_shape(e, op));
        }
        if (op)
            p.segment(box - 1, data.operator_mean[op - 1], box, data.operator_mean[op], ink, 1);
        p.point(box, data.operator_mean[op], "#FFFFFF", e.point_radius + 2);
        p.point(box, data.operator_mean[op], ink, e.point_radius + 1, MarkerShape::Diamond);
        const float tickWidth = p.x(1) - p.x(0) - 4;
        p.tick(op, operator_label(data.names[op], tickWidth, 11), tickWidth);
    }
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
    std::vector<LegendItem> entries{
        {ranges ? "子组极差 R" : "子组均值", e.primary_color, Key::LineMarker, series_shape(e, 0)}};
    if (e.show_limits) {
        entries.push_back({"中心线 CL", green, Key::Line});
        entries.push_back({"控制限 / 超限点", red, Key::Dash});
    }
    legend(frame, e, entries);
    if (e.show_limits) {
        int precision = 6;
        const double smallestGap = (upper == center || lower == center)
                                       ? upper - lower
                                       : std::min(upper - center, center - lower);
        const double magnitude = std::max(std::abs(upper), std::abs(lower));
        if (smallestGap > 0 && magnitude > 0)
            precision = std::clamp(int(std::ceil(std::log10(magnitude) -
                                                 std::log10(smallestGap))) + 3,
                                   precision, std::numeric_limits<double>::max_digits10);
        const auto caption = "UCL = " + number(upper, precision) + "    CL = " +
                             number(center, precision) + "    LCL = " + number(lower, precision);
        const float left = frame.compact ? frame.margin : frame.plot_left;
        const float width = s.width - left - frame.margin;
        const float size = std::min(frame.compact ? 9.f : 11.f,
                                    width / (.65f * static_cast<float>(caption.size())));
        text(s, left, frame.axis_label_top + 17, width, 17, caption, size, muted);
    }
    const float gap = frame.compact ? 7.f : 13.f, available = frame.plot_right - frame.plot_left;
    const float panelWidth = (available - gap * (data.operators - 1)) / data.operators;
    const int skip = std::max(1, int(std::ceil(data.parts * 21.f / panelWidth)));
    for (int op = 0; op < data.operators; ++op) {
        const float left = frame.plot_left + op * (panelWidth + gap);
        Plot p(frame, e, -.45, data.parts - .55, low, high, ranges, false, left, panelWidth,
               op == 0);
        rect(s, left, p.top - 17, panelWidth, 16, "#F3F6FA");
        text(s, left, p.top - 16, panelWidth, 16,
             operator_label("操作员 " + data.names[op], panelWidth - 4, 10.5f), 10.5f, ink,
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
            p.point(part, value, beyond ? red : e.primary_color,
                    e.point_radius + (beyond ? 1.f : 0.f), series_shape(e, 0));
            if (part == data.parts - 1 || (part % skip == 0 && part <= data.parts - 1 - skip))
                p.tick(part, std::to_string(part + 1), 21);
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
    operators_legend(frame, e, data);
    specification_lines(p, result);
    const auto palette = colors(e);
    for (int part = 0; part < data.parts; ++part)
        p.tick(part, std::to_string(part + 1));
    for (int op = 0; op < data.operators; ++op)
        for (int part = 1; part < data.parts; ++part)
            p.segment(part - 1, data.cell_means[op][part - 1], part, data.cell_means[op][part],
                      palette[op]);
    for (int op = 0; op < data.operators; ++op)
        for (int part = 0; part < data.parts; ++part)
            p.point(part, data.cell_means[op][part], palette[op], e.point_radius,
                    series_shape(e, op));
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
    const int count = e.histogram_bins
                          ? e.histogram_bins
                          : std::clamp(int(std::ceil(std::sqrt(data.all.size()))), 5, 20);
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
    const auto fill = light_fill(e.primary_color);
    std::vector<LegendItem> entries{{"测量值频数", fill}};
    if (e.show_limits && !specs(result).empty())
        entries.push_back({"规格限", amber, Key::Dash});
    legend(frame, e, entries);
    for (int i = 0; i < count; ++i) {
        if (!bins[i])
            continue;
        const double left = binLow + step * i;
        const double right = i == count - 1 ? binHigh : binLow + step * (i + 1);
        p.bar(left, 0, right, bins[i], fill);
        const float stroke = e.line_width * .65f;
        p.segment(left, 0, left, bins[i], e.primary_color, stroke);
        p.segment(left, bins[i], right, bins[i], e.primary_color, stroke);
        p.segment(right, bins[i], right, 0, e.primary_color, stroke);
    }
    const double tickStep = nice_step(bounds.second - bounds.first, s.width < 650 ? 3 : 5);
    const double first = std::ceil((bounds.first - origin) / tickStep) * tickStep;
    for (int i = 0; i < 20; ++i) {
        const double tick = first + tickStep * i;
        const double value = origin + tick;
        if (value > bounds.second + tickStep * 1e-8)
            break;
        p.tick(value, number(std::abs(tick) < tickStep * 1e-9 ? 0 : tick), 78);
    }
    if (e.show_limits) {
        float lastRight = -1;
        for (const auto &limit : specs(result)) {
            p.segment(limit.second, p.low, limit.second, p.high, amber, 1, true);
            const auto label = limit.first + " " + exact_number(limit.second);
            const float width = std::min(p.width, std::max(84.f, 7.f * static_cast<float>(label.size())));
            const float x =
                std::clamp(p.x(limit.second) - width / 2, p.left, p.left + p.width - width);
            text(s, x, p.top + (x < lastRight ? 21.f : 4.f), width, 16, label, 10, amber,
                 Primitive::Align::Center);
            lastRight = x + width;
        }
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
    e.show_limits = kind != Kind::Interaction && kind != Kind::Histogram;
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
    if ((editor.y_tick_intervals != 0 &&
         (editor.y_tick_intervals < 2 || editor.y_tick_intervals > 10)) ||
        (editor.histogram_bins != 0 && (editor.histogram_bins < 3 || editor.histogram_bins > 40)))
        throw ConfigError("纵轴目标间隔数应为 2～10，直方图分箱数应为 3～40，自动均为 0");
    if ((editor.legend_position != LegendPosition::Top &&
         editor.legend_position != LegendPosition::Bottom) ||
        editor.marker_shape < MarkerShape::BySeries || editor.marker_shape > MarkerShape::Triangle)
        throw ConfigError("图例位置或数据点形状无效");
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
