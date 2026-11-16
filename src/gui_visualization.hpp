// 由 gui.cpp 在 gui 命名空间、通用控件之后包含。
struct VisualPreview {
    std::optional<msa::Result> snapshot;
    std::optional<msa::charts::Scene> scene;
    msa::charts::Editor editor;
    int selected = -1;
    float width = 0, height = 0;
    bool attempted = false, imported = false;
    std::string error;
};
struct VisualState {
    int selected = 0, format = 0, scope = 0, columns = 1, resolution = 1;
    float offset = 0, editorOffset = 0;
    bool importedSelected = false, editing = false;
    std::optional<msa::Result> imported;
    std::string importedName, directory, filename = "MSA_Charts";
    std::array<bool, 7> included{true, true, true, true, true, true, false};
    std::array<msa::charts::Editor, 7> editors, drafts;
    std::array<std::string, 7> lower, upper;
    std::array<bool, 7> editDirty{};
    std::array<std::string, 7> editError;
    std::array<unsigned, 7> editEpoch{};
    std::string errorKind, errorText, recovery;
    std::filesystem::path lastExportPath;
    int exportingCount = 0, errorChart = -1;
    VisualPreview preview;
    std::future<std::filesystem::path> exporting;
    VisualState() {
        for (int i = 0; i < 7; ++i)
            editors[i] = drafts[i] = msa::charts::default_editor(msa::charts::all_kinds()[i]);
    }
    bool busy() const { return exporting.valid(); }
} visual;

// 图表编辑使用相同的表单样式，并保留独立的变更回调。
void localField(Ui &u, const std::string &id, const std::string &label, std::string &value, float w,
                const std::string &hint = "", std::function<void()> changed = {}) {
    u.column(id + ".field")
        .width(w)
        .gap(5)
        .content([&] {
            text(u, id + ".label", label, w, 21, 13);
            inputControl(u, id, value, w, hint, changed);
        })
        .build();
}
const msa::Result *insightResult() {
    if (visual.importedSelected)
        return visual.imported ? &*visual.imported : nullptr;
    return state.displayedResult() ? &*state.result : nullptr;
}
void clearVisualError() {
    visual.errorKind.clear();
    visual.errorText.clear();
    visual.recovery.clear();
    visual.errorChart = -1;
}
void visualError(const std::string &kind, const std::string &message, const std::string &recovery,
                 int chart = -1) {
    visual.errorKind = kind;
    visual.errorText = message;
    visual.recovery = recovery;
    visual.errorChart = chart;
    state.feedback("图表操作未完成，请查看页面提示", message + "。" + recovery, Level::Warning);
}
std::filesystem::path chartOutputDirectory() {
    auto directory = native_ui::trim(visual.directory).empty()
                         ? msa::config_path().parent_path()
                         : std::filesystem::u8path(native_ui::trim(visual.directory));
    if (directory.has_root_name() && !directory.has_root_directory())
        throw std::runtime_error("导出目录不能使用 C:folder 形式，请填写完整路径");
    if (directory.is_relative())
        directory = msa::config_path().parent_path() / directory;
    return directory.lexically_normal();
}
int selectedChartCount() {
    return visual.scope == 0
               ? 1
               : int(std::count(visual.included.begin(), visual.included.end(), true));
}
bool chartSelectedForExport(int index) {
    return visual.scope == 0 ? index == visual.selected : visual.included[index];
}
std::optional<int> pendingChartExportEdit() {
    for (int i = 0; i < int(visual.editDirty.size()); ++i)
        if (chartSelectedForExport(i) && visual.editDirty[i])
            return i;
    return {};
}
std::string chartFooterSummary() {
    if (visual.busy())
        return "正在导出 " + std::to_string(visual.exportingCount) + " 张图表…";
    if (!visual.errorText.empty())
        return "图表操作未完成，请查看页面提示";
    if (!insightResult())
        return "请先生成数据或导入 CSV";
    if (pendingChartExportEdit())
        return visual.scope == 0 ? "当前图表有待应用的编辑" : "待导出的组合含未应用的编辑";
    if (visual.scope == 0)
        return std::string(msa::charts::label(msa::charts::all_kinds()[visual.selected]));
    return "已选 " + std::to_string(selectedChartCount()) + " / 7 张图表";
}
bool loadInsightCsv(const std::filesystem::path &path) {
    try {
        msa::Result result;
        result.config = native_ui::to_config(state.draft, native_ui::ConfigUse::Analysis);
        result.matrix =
            msa::parse_export_csv(msa::read_text(path, msa::csv_read_limit), result.config);
        result.metrics = msa::calculate(result.matrix, result.config);
        visual.imported = std::move(result);
        visual.importedSelected = true;
        visual.importedName = path.filename().u8string();
        visual.preview.attempted = false;
        clearVisualError();
        state.feedback("CSV 已导入", "已按导入时的研究设计和分析设置计算图表。");
        return true;
    } catch (const std::exception &e) {
        visualError("import", e.what(),
                    "请核对研究设计、CSV 列和测量记录，再重新选择文件。");
        return false;
    }
}
void importInsightCsv() {
    try {
        if (const auto path = platform::choose(false, false, L"measurement.csv"))
            loadInsightCsv(*path);
    } catch (const std::exception &e) {
        visualError("import", e.what(), "请重新选择 CSV 文件。");
    }
}
void insightSourceControls(Ui &u, const std::string &id, float w) {
    constexpr float importWidth = 108;
    const auto source = [&](float width) {
        segments(
            u, id + ".source", {"当前结果", "CSV 文件"}, visual.importedSelected ? 1 : 0, width,
            [](int i) {
                visual.importedSelected = i == 1;
                visual.preview.attempted = false;
            },
            controlHeight, true);
    };
    if (w >= 284)
        u.row(id)
            .width(w)
            .gap(sectionGap)
            .content([&] {
                source(w - importWidth - sectionGap);
                button(u, id + ".import", "导入 CSV", importWidth, importInsightCsv, false,
                       state.busy());
            })
            .build();
    else {
        source(w);
        button(u, id + ".import", "导入 CSV", w, importInsightCsv, false, state.busy());
    }
}
std::string chartLabelThatFits(const std::string &label, float width, float size, int weight = 400) {
    const auto metrics = core::TextPrimitive::measureTextMetrics(label, font(), size, weight);
    if (metrics.width <= width)
        return label;
    const float space = width - core::TextPrimitive::measureTextWidth("…", font(), size, weight);
    const auto end = std::upper_bound(metrics.caretX.begin(), metrics.caretX.end(), space);
    const auto length = end == metrics.caretX.begin()
                            ? 0
                            : metrics.byteIndices[std::distance(metrics.caretX.begin(), end) - 1];
    return label.substr(0, length) + "…";
}
void insightSourceSummary(Ui &u, const std::string &id, float w) {
    std::string summary = visual.importedSelected ? "尚未导入 CSV"
                          : state.busy()          ? "正在生成测量数据…"
                          : state.result          ? "研究参数已修改"
                                                  : "暂无生成结果";
    const char *color = muted;
    if (const auto *result = insightResult()) {
        try {
            const auto &config = result->config;
            const std::string method =
                config.at("analysis").at("method") == "crossed_anova" ? "ANOVA" : "Xbar-R";
            std::size_t count = 0;
            for (const auto &row : result->matrix)
                count += row.size();
            summary = method + "  ·  " + std::to_string(count) + " 条测量记录";
            if (visual.importedSelected)
                summary = chartLabelThatFits(visual.importedName, w, 16) + "\n" + summary;
        } catch (const std::exception &) {
            summary = "数据快照的研究信息不完整，请重新生成或导入。";
            color = levelColor(Level::Error);
        }
    }
    // 数据切换和文件名长度不改变工具栏高度，预览位置保持稳定。
    u.text(id + ".note")
        .size(w, 44)
        .text(summary)
        .fontSize(16)
        .lineHeight(22)
        .color(color)
        .verticalAlign(eui::VerticalAlign::Center)
        .wrap()
        .maxWidth(w)
        .clip()
        .build();
}
msa::charts::Editor chartEditor(int index) { return visual.editors.at(index); }
void discardChartEdits(int index) {
    visual.drafts[index] = visual.editors[index];
    visual.lower[index] =
        visual.editors[index].y_min ? Json(*visual.editors[index].y_min).dump() : "";
    visual.upper[index] =
        visual.editors[index].y_max ? Json(*visual.editors[index].y_max).dump() : "";
    visual.editDirty[index] = false;
    visual.editError[index].clear();
    ++visual.editEpoch[index];
    if (visual.errorKind == "edit" && visual.errorChart == index)
        clearVisualError();
}
bool applyChartEdits(int index) {
    try {
        auto proposed = visual.drafts[index];
        const auto bound = [](const std::string &value) -> std::optional<double> {
            if (native_ui::trim(value).empty())
                return std::nullopt;
            const double parsed = msa::parse_number(value);
            if (!std::isfinite(parsed))
                throw std::runtime_error("纵轴范围需要有限数值，留空可自动设定范围");
            return parsed;
        };
        proposed.y_min = bound(visual.lower[index]);
        proposed.y_max = bound(visual.upper[index]);
        if (proposed.y_min && proposed.y_max && *proposed.y_min >= *proposed.y_max)
            throw std::runtime_error("纵轴下限必须小于上限");
        if (proposed.title.size() > 600 || proposed.x_label.size() > 300 ||
            proposed.y_label.size() > 300)
            throw std::runtime_error("图表标题或坐标轴名称过长，请精简后应用");
        if (const auto *result = insightResult())
            (void)msa::charts::make_scene(*result, msa::charts::all_kinds()[index], proposed, 900,
                                          560);
        visual.editors[index] = std::move(proposed);
        discardChartEdits(index);
        visual.preview.attempted = false;
        state.hint("图表编辑已应用");
        return true;
    } catch (const std::exception &e) {
        visual.editError[index] = e.what();
        visualError("edit", e.what(),
                    "请修正或撤销草稿，再应用编辑。", index);
        return false;
    }
}
void resetChart(int index) {
    visual.editors[index] = msa::charts::default_editor(msa::charts::all_kinds()[index]);
    discardChartEdits(index);
    visual.preview.attempted = false;
    if (visual.errorKind == "render" && visual.errorChart == index)
        clearVisualError();
    state.hint("当前图表已恢复默认外观");
}
void resetAllCharts() {
    for (int i = 0; i < 7; ++i)
        resetChart(i);
    state.hint("全部图表已恢复默认外观");
}
bool sameChartEditor(const msa::charts::Editor &a, const msa::charts::Editor &b) {
    return a.title == b.title && a.x_label == b.x_label && a.y_label == b.y_label &&
           a.primary_color == b.primary_color && a.line_width == b.line_width &&
           a.point_radius == b.point_radius && a.show_grid == b.show_grid &&
           a.show_legend == b.show_legend && a.show_limits == b.show_limits && a.y_min == b.y_min &&
           a.y_max == b.y_max;
}
bool sameChartData(const msa::Result &a, const msa::Result &b) {
    // 无效数据中的 NaN 也应命中失败缓存，避免同一渲染错误逐帧写入日志。
    if (a.matrix.size() != b.matrix.size())
        return false;
    for (std::size_t row = 0; row < a.matrix.size(); ++row) {
        if (a.matrix[row].size() != b.matrix[row].size())
            return false;
        for (std::size_t col = 0; col < a.matrix[row].size(); ++col) {
            const auto x = a.matrix[row][col], y = b.matrix[row][col];
            if (x != y && !(std::isnan(x) && std::isnan(y)))
                return false;
        }
    }
    const auto sameJson = [](const Json &x, const Json &y) {
        return x == y || x.dump() == y.dump();
    };
    return sameJson(a.config, b.config) && sameJson(a.metrics, b.metrics);
}
const msa::charts::Scene *chartPreview(const msa::Result &result, float width, float height) {
    auto &cache = visual.preview;
    const auto editor = chartEditor(visual.selected);
    const bool sameData = cache.snapshot && sameChartData(*cache.snapshot, result);
    if (cache.attempted && sameData && cache.imported == visual.importedSelected &&
        cache.selected == visual.selected && cache.width == width && cache.height == height &&
        sameChartEditor(cache.editor, editor))
        return cache.scene ? &*cache.scene : nullptr;
    // 缓存持有数据副本，不保存 Result 指针；重生成、导入或切换数据源都会重新比较内容。
    cache.snapshot = result;
    cache.imported = visual.importedSelected;
    cache.selected = visual.selected;
    cache.editor = editor;
    cache.width = width;
    cache.height = height;
    cache.attempted = true;
    cache.error.clear();
    cache.scene.reset();
    try {
        cache.scene = msa::charts::make_scene(result, msa::charts::all_kinds()[visual.selected],
                                              editor, width, height);
        if (visual.errorKind == "render")
            clearVisualError();
    } catch (const std::exception &e) {
        cache.error = e.what();
        visualError("render", e.what(),
                    "请重新绘制、恢复当前图表的默认外观，或切换数据源。",
                    visual.selected);
    }
    return cache.scene ? &*cache.scene : nullptr;
}
void drawChartScene(Ui &u, const std::string &id, const msa::charts::Scene &scene) {
    using P = msa::charts::Primitive;
    u.stack(id)
        .size(scene.width, scene.height)
        .clip()
        .content([&] {
            int n = 0;
            for (const auto &p : scene.items) {
                const auto key = id + ".p" + std::to_string(n++);
                if (p.type == P::Type::Text) {
                    const bool title = n == 2 && p.y == 10;
                    float size = p.font_size * 1.35f;
                    const int weight = title ? 600 : 400;
                    // 刻度和规格限显示完整数值。自然语言较长时仍用省略号保留字号。
                    const bool numericLabel = p.align == P::Align::Right;
                    if (numericLabel) {
                        const float measured =
                            core::TextPrimitive::measureTextWidth(p.text, font(), size);
                        if (measured > p.x2)
                            size *= std::max(1.f, p.x2 - 1.f) / measured;
                    }
                    const bool multiline = p.y2 >= 2 * size && !title;
                    auto label =
                        u.text(key)
                            .position(p.x, p.y)
                            .size(p.x2, p.y2)
                            .text(numericLabel || multiline
                                      ? p.text
                                      : chartLabelThatFits(p.text, p.x2, size, weight))
                            .fontFamily(font())
                            .fontSize(size)
                            .fontWeight(weight)
                            .color(p.color == "#243247"   ? ink
                                   : p.color == "#69778A" ? muted
                                                          : p.color.c_str())
                            .horizontalAlign(
                                p.align == P::Align::Center  ? eui::HorizontalAlign::Center
                                : p.align == P::Align::Right ? eui::HorizontalAlign::Right
                                                             : eui::HorizontalAlign::Left)
                            .verticalAlign(multiline && !numericLabel ? eui::VerticalAlign::Top
                                                                      : eui::VerticalAlign::Center)
                            .maxWidth(p.x2)
                            .lineHeight(size)
                            .clip();
                    label.wrap(multiline && !numericLabel).build();
                } else if (p.type == P::Type::Rect) {
                    u.rect(key)
                        .position(p.x, p.y)
                        .size(p.x2, p.y2)
                        .color(p.color == "#F3F6FA" ? tableHeader : p.color.c_str())
                        .build();
                } else if (p.type == P::Type::Circle) {
                    u.rect(key)
                        .position(p.x - p.x2, p.y - p.x2)
                        .size(p.x2 * 2, p.x2 * 2)
                        .radius(p.x2)
                        .color(p.color)
                        .build();
                } else {
                    const float dx = p.x2 - p.x, dy = p.y2 - p.y;
                    const float length = std::hypot(dx, dy);
                    if (length < .001f)
                        continue;
                    const float nx = -dy / length * p.stroke_width / 2;
                    const float ny = dx / length * p.stroke_width / 2;
                    const int pieces = p.dashed ? std::max(1, int(std::ceil(length / 9))) : 1;
                    for (int j = 0; j < pieces; ++j) {
                        const float a = float(j) / pieces;
                        const float b = p.dashed ? std::min(1.f, (j + .58f) / pieces) : 1.f;
                        u.polygon(key + "." + std::to_string(j))
                            .position(0, 0)
                            .size(scene.width, scene.height)
                            .color(p.color == "#E8EDF3" ? line : p.color.c_str())
                            .points({{p.x + dx * a + nx, p.y + dy * a + ny},
                                     {p.x + dx * b + nx, p.y + dy * b + ny},
                                     {p.x + dx * b - nx, p.y + dy * b - ny},
                                     {p.x + dx * a - nx, p.y + dy * a - ny}})
                            .build();
                    }
                }
            }
            u.rect(id + ".frame")
                .size(scene.width, scene.height)
                .color("#00000000")
                .border(1, line)
                .radius(controlRadius)
                .build();
        })
        .build();
}
void exportVisuals() {
    if (visual.busy() || state.busy() || !insightResult())
        return;
    try {
        if (const auto pending = pendingChartExportEdit()) {
            const int i = *pending;
            if (visual.selected != i)
                visual.editorOffset = 0;
            visual.selected = i;
            visual.editing = true;
            visualError("edit",
                        "「" + std::string(msa::charts::label(msa::charts::all_kinds()[i])) +
                            "」还有未应用的编辑",
                        "请先应用编辑或撤销草稿，再导出。", i);
            return;
        }
        std::vector<msa::charts::Scene> scenes;
        for (int i = 0; i < int(msa::charts::all_kinds().size()); ++i)
            if (chartSelectedForExport(i))
                scenes.push_back(msa::charts::make_scene(
                    *insightResult(), msa::charts::all_kinds()[i], chartEditor(i), 900, 560));
        if (scenes.empty())
            throw std::runtime_error("请至少选择一张组合图表");
        const auto basename = native_ui::trim(visual.filename);
        if (basename.empty() || basename == "." || basename == ".." ||
            basename.find_first_of("<>:\"/\\|?*\r\n") != std::string::npos ||
            basename.back() == '.' || basename.back() == ' ')
            throw std::runtime_error("请填写合法的图表文件名（不含扩展名或路径）");
        const auto directory = chartOutputDirectory();
        platform::ChartExportOptions options;
        options.format = static_cast<platform::ChartFormat>(visual.format);
        options.columns = visual.scope == 0 ? 1 : visual.columns + 1;
        options.panel_width = 1200 + visual.resolution * 600;
        options.dpi = 144 + visual.resolution * 72;
        const int count = int(scenes.size());
        visual.exporting = std::async(
            std::launch::async, [scenes = std::move(scenes), directory, basename, options] {
                try {
                    for (int i = 0; i < 10000; ++i) {
                        const auto name = basename + (i ? "_" + std::to_string(i) : "") +
                                          platform::chart_extension(options.format);
                        const auto path = directory / std::filesystem::u8path(name);
                        if (std::filesystem::exists(path))
                            continue;
                        try {
                            const auto result = platform::export_charts(scenes, path, options);
                            app::requestUpdate();
                            return result;
                        } catch (const msa::FileExistsError &) {
                        }
                    }
                    throw std::runtime_error("同名文件过多，请修改文件名");
                } catch (...) {
                    app::requestUpdate();
                    throw;
                }
            });
        visual.exportingCount = count;
        clearVisualError();
        state.hint("正在导出 " + std::to_string(count) + " 张图表…");
    } catch (const std::exception &e) {
        visualError("export", e.what(), "请核对图表选择、文件名和目录后重试。");
    }
}
void pollVisuals() {
    if (visual.exporting.valid() &&
        visual.exporting.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        try {
            visual.lastExportPath = visual.exporting.get();
            if (visual.errorKind == "export")
                clearVisualError();
            state.feedback("图表已导出", "图表已保存至 " + visual.lastExportPath.u8string());
        } catch (const std::exception &e) {
            visualError(
                "export", e.what(),
                "请检查目录权限或修改导出设置后重试。");
        }
}
void stopVisuals() {
    if (visual.exporting.valid())
        visual.exporting.wait();
}
void chartAppearance(Ui &u, float w, float viewportHeight = 0) {
    const int i = visual.selected;
    auto &e = visual.drafts[i];
    // 图表索引隔离控件编辑状态；重置版本确保撤销后输入缓冲也恢复为已应用内容。
    const auto id = "chart.edit." + std::to_string(i) + "." + std::to_string(visual.editEpoch[i]);
    const auto changed = [i] { visual.editDirty[i] = true; };
    const auto editorContent = [&](Ui &u, float c) {
        u.column(id + ".fields")
            .width(c)
            .gap(sectionGap)
            .content([&] {
                subheading(u, id + ".axes.heading", "标题与坐标轴", "", c);
                localField(u, id + ".title", "图表标题", e.title, c, "", changed);
                u.row(id + ".axis.names")
                    .width(c)
                    .gap(12)
                    .content([&] {
                        localField(u, id + ".x", "横轴名称", e.x_label, (c - 12) / 2, "", changed);
                        localField(u, id + ".y", "纵轴名称", e.y_label, (c - 12) / 2, "", changed);
                    })
                    .build();
                u.row(id + ".range")
                    .width(c)
                    .gap(12)
                    .content([&] {
                        localField(u, id + ".min", "纵轴下限", visual.lower[i], (c - 12) / 2,
                                   "自动", changed);
                        localField(u, id + ".max", "纵轴上限", visual.upper[i], (c - 12) / 2,
                                   "自动", changed);
                    })
                    .build();
                u.rect(id + ".appearance.rule").size(c, 1).color(line).build();
                subheading(u, id + ".appearance.heading", "图形样式", "", c);
                const std::vector<std::string> colors{"#3B65E8", "#16866B", "#7B51C2", "#D47B20",
                                                      "#CB4A5B"};
                int color = 0;
                for (int k = 0; k < int(colors.size()); ++k)
                    if (e.primary_color == colors[k])
                        color = k;
                u.row(id + ".color.row")
                    .width(c)
                    .gap(12)
                    .alignItems(eui::Align::CENTER)
                    .content([&] {
                        text(u, id + ".color.label", "主色", 42, 28, 13);
                        u.rect(id + ".color.sample")
                            .size(20, 20)
                            .radius(5)
                            .color(e.primary_color)
                            .build();
                        combo(u, id + ".color", {"靛蓝", "青绿", "紫色", "琥珀", "玫红"}, color,
                              c - 86, [i, colors](int k) {
                                  visual.drafts[i].primary_color = colors[k];
                                  visual.editDirty[i] = true;
                              });
                    })
                    .build();
                const auto weight = [&](float width) {
                    u.column(id + ".weight.field")
                        .width(width)
                        .gap(5)
                        .content([&] {
                            text(u, id + ".weight.label", "线条粗细", width, 21, 13);
                            segments(u, id + ".weight", {"细线", "标准", "加粗"},
                                     std::clamp(int(e.line_width) - 1, 0, 2), width, [i](int k) {
                                         visual.drafts[i].line_width = float(k + 1);
                                         visual.editDirty[i] = true;
                                     });
                        })
                        .build();
                };
                const auto points = [&](float width) {
                    u.column(id + ".points.field")
                        .width(width)
                        .gap(5)
                        .content([&] {
                            text(u, id + ".points.label", "数据点大小", width, 21, 13);
                            segments(u, id + ".points", {"小", "标准", "大"},
                                     std::clamp(int(e.point_radius) - 2, 0, 2), width, [i](int k) {
                                         visual.drafts[i].point_radius = float(k + 2);
                                         visual.editDirty[i] = true;
                                     });
                        })
                        .build();
                };
                if (c >= 460)
                    u.row(id + ".marks.row")
                        .width(c)
                        .gap(12)
                        .content([&] {
                            weight((c - 12) / 2);
                            points((c - 12) / 2);
                        })
                        .build();
                else {
                    weight(c);
                    points(c);
                }
                u.rect(id + ".references.rule").size(c, 1).color(line).build();
                subheading(u, id + ".references.heading", "显示内容", "", c);
                u.row(id + ".display.row")
                    .width(c)
                    .gap(12)
                    .content([&] {
                        checkOption(u, id + ".grid", "网格", e.show_grid, (c - 12) / 2,
                                    [i](bool value) {
                                        visual.drafts[i].show_grid = value;
                                        visual.editDirty[i] = true;
                                    });
                        checkOption(u, id + ".legend", "图例", e.show_legend, (c - 12) / 2,
                                    [i](bool value) {
                                        visual.drafts[i].show_legend = value;
                                        visual.editDirty[i] = true;
                                    });
                    })
                    .build();
                const auto kind = msa::charts::all_kinds()[i];
                const bool control =
                    kind == msa::charts::Kind::RangeChart || kind == msa::charts::Kind::MeanChart;
                const bool noLimits = kind == msa::charts::Kind::VarianceComponents;
                checkOption(
                    u, id + ".limits",
                    control    ? "中心线与控制限"
                    : noLimits ? "规格参考线（本图不适用）"
                               : "规格参考线",
                    e.show_limits, c,
                    [i](bool value) {
                        visual.drafts[i].show_limits = value;
                        visual.editDirty[i] = true;
                    },
                    noLimits);
            })
            .build();
        // 错误和草稿状态共用固定槽位，应用失败不推移编辑操作。
        u.text(id + ".status")
            .size(c, 44)
            .text(!visual.editError[i].empty() ? visual.editError[i]
                  : visual.editDirty[i]        ? "应用编辑后更新预览。"
                                               : "编辑仅在本次运行期间保留。")
            .fontSize(16)
            .lineHeight(22)
            .color(!visual.editError[i].empty() ? levelColor(Level::Error)
                   : visual.editDirty[i]        ? levelColor(Level::Warning)
                                                : muted)
            .wrap()
            .maxWidth(c)
            .verticalAlign(eui::VerticalAlign::Center)
            .clip()
            .build();
        u.row(id + ".apply.row")
            .width(c)
            .gap(12)
            .content([&] {
                button(
                    u, id + ".apply", "应用编辑", (c - 12) / 2, [i] { applyChartEdits(i); }, true,
                    !visual.editDirty[i]);
                button(
                    u, id + ".discard", "撤销草稿", (c - 12) / 2,
                    [i] {
                        discardChartEdits(i);
                        state.hint("已撤销图表草稿");
                    },
                    false, !visual.editDirty[i] && visual.editError[i].empty());
            })
            .build();
        u.row(id + ".reset.row")
            .width(c)
            .gap(12)
            .content([&] {
                button(u, id + ".reset", "重置当前图表", (c - 12) / 2, [i] { resetChart(i); });
                button(u, id + ".resetall", "重置所有图表", (c - 12) / 2, resetAllCharts);
            })
            .build();
    };
    section(u, "chart.editor", "图表编辑", w, [&](float c) {
        if (viewportHeight <= 0) {
            editorContent(u, c);
            return;
        }
        constexpr float barWidth = 6, barGap = 4;
        components::scrollView(u, "chart.editor.scroll")
            .size(c + barWidth + barGap, viewportHeight)
            .offset(visual.editorOffset)
            .theme(tokens())
            .scrollbarWidth(barWidth)
            .scrollbarGap(barGap)
            .reserveScrollbar()
            .onChange([](float offset) {
                visual.editorOffset = offset;
                menu.id.clear();
            })
            .content([&](Ui &v, float width, float) {
                v.column("chart.editor.body")
                    .width(width)
                    .gap(13)
                    .content([&] { editorContent(v, width); })
                    .build();
            })
            .build();
    });
}
void chooseChartDirectory() {
    try {
        if (auto path = platform::choose(false, true)) {
            visual.directory = path->u8string();
            if (visual.errorKind == "directory" || visual.errorKind == "export")
                clearVisualError();
            state.hint("图表导出目录已更新");
        }
    } catch (const std::exception &e) {
        visualError("directory", e.what(), "请重新选择目录，或填写完整路径。");
    }
}
void openChartDirectory(bool last = false) {
    try {
        const auto directory = last && !visual.lastExportPath.empty()
                                   ? visual.lastExportPath.parent_path()
                                   : chartOutputDirectory();
        if (!std::filesystem::is_directory(directory))
            throw std::runtime_error("目录尚未创建。首次导出时会自动创建目录");
        platform::openDirectory(directory);
        if (visual.errorKind == "directory")
            clearVisualError();
    } catch (const std::exception &e) {
        visualError("directory", e.what(), "请检查路径，或使用「选择目录」更换位置。");
    }
}
void visualRecovery(Ui &u, float w) {
    if (visual.errorText.empty())
        return;
    const auto kind = visual.errorKind;
    const auto level = kind == "edit" || kind == "directory" ? Level::Warning : Level::Error;
    section(u, "chart.recovery", "", w, [&](float c) {
        u.row("chart.recovery.warning")
            .width(c)
            .gap(sectionGap)
            .content([&] {
                badge(u, "chart.recovery.badge", "!", 28, levelColor(level), levelSurface(level));
                const float copyWidth = c - 28 - sectionGap;
                u.column("chart.recovery.copy")
                    .width(copyWidth)
                    .gap(4)
                    .content([&] {
                        paragraph(u, "chart.recovery.reason", visual.errorText, copyWidth,
                                  levelColor(level), 18);
                        paragraph(u, "chart.recovery.action", visual.recovery, copyWidth, muted,
                                  17);
                    })
                    .build();
            })
            .build();
        u.row("chart.recovery.buttons")
            .width(c)
            .gap(12)
            .content([&] {
                const float bw = (c - 12) / 2;
                if (kind == "import") {
                    button(u, "chart.recovery.import", "重新选择 CSV", bw, importInsightCsv, true,
                           state.busy());
                    button(u, "chart.recovery.study", "检查研究设置", bw, [] {
                        state.optionsTab = 0;
                        state.navigate(Page::Options);
                    });
                } else if (kind == "edit") {
                    button(
                        u, "chart.recovery.edit", "打开对应编辑器", bw,
                        [] {
                            if (visual.errorChart >= 0 && visual.selected != visual.errorChart) {
                                visual.selected = visual.errorChart;
                                visual.editorOffset = 0;
                            }
                            visual.editing = true;
                        },
                        true);
                    button(u, "chart.recovery.discard", "撤销对应草稿", bw, [] {
                        discardChartEdits(visual.errorChart >= 0 ? visual.errorChart
                                                                 : visual.selected);
                    });
                } else if (kind == "render") {
                    button(
                        u, "chart.recovery.render", "重新绘制", bw,
                        [] {
                            visual.preview.attempted = false;
                            clearVisualError();
                        },
                        true);
                    button(u, "chart.recovery.reset", "重置当前图表", bw,
                           [] { resetChart(visual.selected); });
                } else {
                    button(u, "chart.recovery.export", "重试导出", bw, exportVisuals, true,
                           visual.busy() || state.busy() || !insightResult());
                    button(u, "chart.recovery.directory", "选择目录", bw, chooseChartDirectory);
                }
            })
            .build();
    });
}
void chartExportSettings(Ui &u, float w) {
    section(u, "chart.output", "图表导出", w, [&](float c) {
        const auto format = [&](float width) {
            u.column("chart.format.field")
                .width(width)
                .gap(5)
                .content([&] {
                    text(u, "chart.format.label", "文件格式", width, 21, 13);
                    segments(u, "chart.format", {"PNG", "JPEG", "PDF"}, visual.format, width,
                             [](int i) { visual.format = i; });
                })
                .build();
        };
        const auto scope = [&](float width) {
            u.column("chart.scope.field")
                .width(width)
                .gap(5)
                .content([&] {
                    text(u, "chart.scope.label", "导出范围", width, 21, 13);
                    segments(u, "chart.scope", {"当前单图", "多图组合"}, visual.scope, width,
                             [](int i) { visual.scope = i; });
                })
                .build();
        };
        const auto resolution = [&](float width) {
            u.column("chart.resolution.field")
                .width(width)
                .gap(5)
                .content([&] {
                    text(u, "chart.resolution.label", "单图宽度", width, 21, 13);
                    segments(u, "chart.resolution", {"1200 px", "1800 px", "2400 px"},
                             visual.resolution, width, [](int i) { visual.resolution = i; });
                })
                .build();
        };
        const auto columns = [&](float width) {
            u.column("chart.columns.field")
                .width(width)
                .gap(5)
                .content([&] {
                    text(u, "chart.columns.label", "组合排版", width, 21, 13);
                    segments(u, "chart.columns", {"单列", "双列"}, visual.columns, width,
                             [](int i) { visual.columns = i; });
                })
                .build();
        };
        const bool combination = visual.scope == 1;
        if (c >= (combination ? 940.f : 700.f)) {
            const int count = combination ? 4 : 3;
            const float width = (c - 12 * (count - 1)) / count;
            u.row("chart.output.row")
                .width(c)
                .gap(12)
                .content([&] {
                    format(width);
                    scope(width);
                    resolution(width);
                    if (combination)
                        columns(width);
                })
                .build();
        } else if (c >= 490) {
            const float width = (c - 12) / 2;
            u.row("chart.output.row")
                .width(c)
                .gap(12)
                .content([&] {
                    format(width);
                    scope(width);
                })
                .build();
            u.row("chart.output.size.row")
                .width(c)
                .gap(12)
                .content([&] {
                    resolution(combination ? width : c);
                    if (combination)
                        columns(width);
                })
                .build();
        } else {
            format(c);
            scope(c);
            resolution(c);
            if (combination)
                columns(c);
        }
        if (combination) {
            u.rect("chart.selection.rule").size(c, 1).color(line).build();
            u.row("chart.selection.actions")
                .width(c)
                .gap(8)
                .alignItems(eui::Align::CENTER)
                .content([&] {
                    text(u, "chart.selection.count",
                         "组合图表  ·  " + std::to_string(selectedChartCount()) + " / 7 张",
                         c - 160, 32, 13, selectedChartCount() ? ink : levelColor(Level::Warning));
                    button(
                        u, "chart.selection.all", "全选", 72, [] { visual.included.fill(true); },
                        false, false, 32);
                    button(
                        u, "chart.selection.none", "清空", 72, [] { visual.included.fill(false); },
                        false, false, 32);
                })
                .build();
            const auto item = [&](int i, float width) {
                checkOption(u, "chart.select." + std::to_string(i),
                            msa::charts::label(msa::charts::all_kinds()[i]), visual.included[i],
                            width, [i](bool value) { visual.included[i] = value; });
            };
            const int count = c >= 1010 ? 4 : c >= 760 ? 3 : c >= 460 ? 2 : 1;
            u.column("chart.selection.list")
                .width(c)
                .gap(4)
                .content([&] {
                    for (int first = 0; first < 7; first += count)
                        u.row("chart.select.row." + std::to_string(first))
                            .width(c)
                            .gap(12)
                            .content([&] {
                                for (int i = first; i < std::min(7, first + count); ++i)
                                    item(i, (c - 12 * (count - 1)) / count);
                            })
                            .build();
                })
                .build();
            // 空选择仅改变文案与颜色，不插入额外提示行。
            text(u, "chart.selection.empty",
                 selectedChartCount() ? "按列表顺序导出选中的图表。" : "请至少选择一张图表。", c,
                 26, 12, selectedChartCount() ? muted : levelColor(Level::Warning));
        }
        u.rect("chart.destination.rule").size(c, 1).color(line).build();
        subheading(u, "chart.destination.heading", "保存位置", "", c);
        const auto filename = [&](float width) {
            localField(u, "chart.filename", "文件名", visual.filename, width);
        };
        const auto directory = [&](float width) {
            localField(u, "chart.directory", "导出目录", visual.directory, width,
                       msa::config_path().parent_path().u8string());
        };
        if (c >= 760)
            u.row("chart.destination.fields")
                .width(c)
                .gap(12)
                .content([&] {
                    filename((c - 12) * .35f);
                    directory((c - 12) * .65f);
                })
                .build();
        else {
            filename(c);
            directory(c);
        }
        const auto last = [&](float width) {
            button(
                u, "chart.last.folder", "上次导出目录", width, [] { openChartDirectory(true); },
                false, visual.lastExportPath.empty());
        };
        constexpr float actionWidth = 106, lastWidth = 130, gap = 12;
        const bool inlineActions = c >= actionWidth * 2 + lastWidth + gap * 2;
        u.row("chart.directory.actions")
            .width(c)
            .gap(gap)
            .content([&] {
                const float width = inlineActions ? actionWidth : (c - gap) / 2;
                button(u, "chart.browse", "选择目录", width, chooseChartDirectory);
                button(u, "chart.open", "打开目录", width, [] { openChartDirectory(); });
                if (inlineActions)
                    last(lastWidth);
            })
            .build();
        // 成功提示仅使用固定底栏；目录操作始终占位，导出完成不改变正文高度。
        if (!inlineActions)
            last(c);
        u.text("chart.output.note")
            .size(c, 44)
            .text(std::string("扩展名自动添加，同名文件会追加序号。\n") +
                  (visual.format == 2 ? "PDF 保存为单页图像。"
                   : combination      ? "所选图表保存为一张合成图片。"
                                      : "目录留空时，图片保存至配置目录。"))
            .fontSize(16)
            .lineHeight(22)
            .color(muted)
            .verticalAlign(eui::VerticalAlign::Center)
            .wrap()
            .maxWidth(c)
            .clip()
            .build();
    });
}
void chartSectionHeading(Ui &u, const std::string &id, const std::string &label, float w) {
    u.row(id)
        .size(w, sectionTitleHeight)
        .gap(9)
        .alignItems(eui::Align::CENTER)
        .content([&] {
            u.rect(id + ".accent").size(3, 15).color(blue).radius(1.5f).build();
            text(u, id + ".label", label, w - 12, sectionTitleHeight, 17);
        })
        .build();
}
void chartSourceCard(Ui &u, float w) {
    section(u, "charts.source", "", w, [&](float c) {
        const auto summary = [&](float width) {
            u.column("charts.source.details")
                .width(width)
                .content([&] {
                    chartSectionHeading(u, "charts.source.heading", "数据源", width);
                    u.row("charts.source.caption")
                        .width(width)
                        .padding(12, 0, 0, 0)
                        .content([&] { insightSourceSummary(u, "charts.dataset", width - 12); })
                        .build();
                })
                .build();
        };
        if (c >= 500) {
            const float controlWidth = std::clamp(c * .34f, 284.f, 380.f);
            u.row("charts.source.toolbar")
                .width(c)
                .gap(16)
                .alignItems(eui::Align::CENTER)
                .content([&] {
                    summary(c - controlWidth - 16);
                    insightSourceControls(u, "charts.dataset", controlWidth);
                })
                .build();
        } else {
            summary(c);
            insightSourceControls(u, "charts.dataset", c);
        }
    });
}
constexpr float chartToolbarHeight = 44;

void chartPreviewToolbar(Ui &u, float w) {
    static const auto names = [] {
        std::vector<std::string> labels;
        for (const auto kind : msa::charts::all_kinds())
            labels.emplace_back(msa::charts::label(kind));
        return labels;
    }();
    constexpr float gap = 10, headingMinWidth = 108, draftWidth = 90, editWidth = 108;
    const bool inlineControls = w >= 520;
    const float controlsWidth =
        inlineControls ? std::min(w - headingMinWidth - draftWidth - 2 * gap, 416.f) : w;
    const auto controls = [&] {
        u.row("charts.source.chart.row")
            .width(controlsWidth)
            .gap(gap)
            .content([&] {
                combo(u, "charts.kind", names, visual.selected, controlsWidth - editWidth - gap,
                      [](int i) {
                          visual.selected = i;
                          visual.editorOffset = 0;
                          menu.id.clear();
                      });
                button(
                    u, "charts.edit", visual.editing ? "收起编辑" : "编辑图表", editWidth,
                    [] {
                        visual.editing = !visual.editing;
                        menu.id.clear();
                    },
                    false);
            })
            .build();
    };
    u.row("charts.preview.draft")
        .size(w, chartToolbarHeight)
        .gap(gap)
        .alignItems(eui::Align::CENTER)
        .content([&] {
            const float headingWidth =
                w - draftWidth - gap - (inlineControls ? controlsWidth + gap : 0.f);
            chartSectionHeading(u, "charts.preview.heading", "图表预览", headingWidth);
            if (visual.editDirty[visual.selected])
                badge(u, "charts.preview.state", "草稿待应用", draftWidth,
                      levelColor(Level::Warning), levelSurface(Level::Warning));
            else if (inlineControls)
                u.rect("charts.preview.spacer").size(draftWidth, 1).color("#00000000").build();
            if (inlineControls)
                controls();
        })
        .build();
    if (!inlineControls)
        controls();
}
void chartEmptyState(Ui &u, float w) {
    const bool showAction = !visual.importedSelected && !state.busy();
    constexpr float inset = 14, icon = 40, actionWidth = 108;
    const bool inlineAction = showAction && w >= 430;
    const char *description =
        visual.importedSelected ? "导入 CSV 文件后，可查看与当前研究设计对应的图表。"
        : state.busy()          ? "生成完成后，可在这里查看本次研究的图表。"
        : state.result          ? "按当前研究设置重新生成数据，或导入 CSV 查看已有研究。"
                                : "在「数据」页生成测量值，或导入 CSV 查看已有研究。";
    const auto action = [&] {
        button(u, "charts.empty.generate", "前往数据", actionWidth,
               [] { state.navigate(Page::Data); });
    };
    u.stack("charts.empty")
        .width(w)
        .content([&] {
            u.rect("charts.empty.bg")
                .width(eui::SizeValue::fill())
                .height(eui::SizeValue::fill())
                .ignoreLayout()
                .color(surfaceSoft)
                .border(1, line)
                .radius(controlRadius)
                .build();
            u.column("charts.empty.content")
                .width(w)
                .padding(inset, 12)
                .gap(sectionGap)
                .content([&] {
                    u.row("charts.empty.row")
                        .width(w - 2 * inset)
                        .gap(sectionGap)
                        .alignItems(eui::Align::CENTER)
                        .content([&] {
                            u.stack("charts.empty.mark")
                                .size(icon, icon)
                                .content([&] {
                                    u.rect("charts.empty.mark.surface")
                                        .size(icon, icon)
                                        .radius(controlRadius)
                                        .color(blueWash)
                                        .build();
                                    constexpr std::array<float, 3> heights{8, 13, 19};
                                    for (int i = 0; i < 3; ++i)
                                        u.rect("charts.empty.mark.bar" + std::to_string(i))
                                            .position(11.f + float(i) * 7.f, 29.f - heights[i])
                                            .size(4, heights[i])
                                            .radius(1.5f)
                                            .color(blue)
                                            .build();
                                })
                                .build();
                            const float textWidth = w - 2 * inset - icon - sectionGap -
                                                    (inlineAction ? actionWidth + sectionGap : 0.f);
                            u.column("charts.empty.copy")
                                .width(textWidth)
                                .gap(4)
                                .content([&] {
                                    text(u, "charts.empty.title", "暂无图表数据", textWidth, 24,
                                         15);
                                    paragraph(u, "charts.empty.description", description, textWidth,
                                              muted, 16);
                                })
                                .build();
                            if (inlineAction)
                                action();
                        })
                        .build();
                    if (showAction && !inlineAction)
                        action();
                })
                .build();
        })
        .build();
}
void chartsPage(Ui &u, float w, float h) {
    components::scrollView(u, "charts.scroll")
        .size(w, h)
        .offset(visual.offset)
        .theme(tokens())
        .reserveScrollbar()
        .scrollbarGap(scrollGutter)
        .transition(transition(.12f))
        .onChange([](float y) {
            visual.offset = y;
            menu.id.clear();
        })
        .content([h](Ui &v, float cw, float) {
            v.column("charts.content")
                .width(cw)
                .gap(sectionGap)
                .padding(0, 0, 0, sectionGap)
                .content([&] {
                    chartSourceCard(v, cw);
                    if (visual.errorKind == "import")
                        visualRecovery(v, cw);
                    const float plotHeight = std::clamp(h - 226.f, 304.f, 528.f);
                    const auto *result = insightResult();
                    auto preview = [&](float width) {
                        section(v, "charts.preview", "", width, [&](float c) {
                            chartPreviewToolbar(v, c);
                            if (result) {
                                if (const auto *scene = chartPreview(*result, c, plotHeight))
                                    drawChartScene(
                                        v, "charts.scene." + std::to_string(visual.selected),
                                        *scene);
                                else if (visual.editing && cw >= 1100)
                                    v.column("charts.invalid.panel")
                                        .size(c, plotHeight)
                                        .content([&] {
                                            paragraph(v, "charts.invalid",
                                                      "暂时无法绘制图表，请查看下方提示。", c,
                                                      levelColor(Level::Error), 18);
                                        })
                                        .build();
                                else
                                    paragraph(v, "charts.invalid",
                                              "暂时无法绘制图表，请查看下方提示。", c,
                                              levelColor(Level::Error), 18);
                            } else
                                chartEmptyState(v, c);
                        });
                    };
                    if (visual.editing && cw >= 1100 && result)
                        v.row("charts.layout")
                            .width(cw)
                            .gap(sectionGap)
                            .content([&] {
                                // Preview has 20px insets. The 360px editor uses 16px and its own
                                // heading.
                                const float editorViewport =
                                    plotHeight + chartToolbarHeight + 8 - sectionTitleHeight;
                                constexpr float editorWidth = 360;
                                preview(cw - editorWidth - sectionGap);
                                chartAppearance(v, editorWidth, editorViewport);
                            })
                            .build();
                    else {
                        preview(cw);
                        if (visual.editing)
                            chartAppearance(v, cw);
                    }
                    if (visual.errorKind != "import")
                        visualRecovery(v, cw);
                    chartExportSettings(v, cw);
                })
                .build();
        })
        .build();
}
