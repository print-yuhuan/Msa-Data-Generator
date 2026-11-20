#pragma once
#include "core.hpp"

namespace msa::charts {
enum class Kind {
    VarianceComponents,
    MeasurementByPart,
    MeasurementByOperator,
    RangeChart,
    MeanChart,
    Interaction,
    Histogram
};
enum class LegendPosition { Top, Bottom };
enum class MarkerShape { BySeries, Circle, Square, Diamond, Triangle };
enum class Style { Default, Origin };
const char *default_primary_color(Style style);
struct Editor {
    std::string title, x_label, y_label, primary_color = default_primary_color(Style::Default);
    float line_width = 2, point_radius = 3;
    bool show_grid = true, show_legend = true, show_limits = true;
    std::optional<double> y_min, y_max;
    LegendPosition legend_position = LegendPosition::Top;
    MarkerShape marker_shape = MarkerShape::BySeries;
    // 0 为自动。目标刻度数仍受画布高度及整数频数约束。
    int y_tick_intervals = 0, histogram_bins = 0;
    Style style = Style::Default;
};
// 图例、数据系列和选项页色样共用同一色序，第一项为编辑器的当前主色。
std::vector<std::string> series_colors(const Editor &editor);
// 使用逻辑像素和不透明 RGB 颜色；所有绘制后端共享同一图形场景。
struct Primitive {
    enum class Type { Line, Rect, Circle, Text };
    enum class Align { Left, Center, Right };
    Type type = Type::Line;
    // Line: 两端点；Rect/Text: 左上角与宽高；Circle: 圆心与 x2 半径。
    float x = 0, y = 0, x2 = 0, y2 = 0;
    std::string color = "#20334B";
    float stroke_width = 1, font_size = 12;
    std::string text;
    Align align = Align::Left;
    bool dashed = false;
};
struct Scene {
    float width = 0, height = 0;
    std::vector<Primitive> items;
};
const std::vector<Kind> &all_kinds();
const char *label(Kind kind);
const char *description(Kind kind);
Editor default_editor(Kind kind, Style style = Style::Default);
// 编辑只改变图形表达，不修改测量数据、分析配置或统计结果。
Scene make_scene(const Result &result, Kind kind, const Editor &editor, float width, float height);
} // namespace msa::charts
