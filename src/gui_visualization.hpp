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
    float offset = 0;
    bool importedSelected = false, editing = false;
    std::optional<msa::Result> imported;
    std::string importedName, directory, filename = "MSA_Charts";
    std::array<bool, 7> included{true, true, true, true, true, true, false};
    std::array<msa::charts::Editor, 7> editors, drafts;
    std::array<std::string, 7> lower, upper;
    std::array<bool, 7> editDirty{};
    std::array<std::string, 7> editError;
    std::array<unsigned, 7> editEpoch{};
    std::string errorKind, errorText, recovery, exportSummary;
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

// 与业务表单分离，编辑图表或对话不会使数据失效。
void localInput(Ui &u, const std::string &id, std::string &value, float w,
                const std::string &hint = "", float h = 40, std::function<void()> changed = {},
                bool secret = false) {
    components::input(u, id)
        .size(w, h)
        .value(value)
        .placeholder(hint)
        .fontSize(19)
        .fontFamily(font())
        .inset(10)
        .theme(tokens())
        .multiline(h > 40)
        .scrollbar(h > 40)
        .password(secret)
        .transition(transition(.14f))
        .onChange([&value, changed](const std::string &next) {
            if (next != value) {
                value = next;
                if (changed)
                    changed();
            }
        })
        .build();
}
void localField(Ui &u, const std::string &id, const std::string &label, std::string &value, float w,
                const std::string &hint = "", std::function<void()> changed = {},
                bool secret = false) {
    u.column(id + ".field")
        .width(w)
        .gap(5)
        .content([&] {
            text(u, id + ".label", label, w, 22, 13, muted);
            localInput(u, id, value, w, hint, 40, changed, secret);
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
    state.feedback("图表操作未完成，请查看页内说明", message + "；" + recovery, Level::Warning);
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
std::string chartFooterSummary() {
    if (visual.busy())
        return "正在导出 " + std::to_string(visual.exportingCount) + " 张图表，请稍候…";
    if (!visual.errorText.empty())
        return "图表操作未完成，可按页内提示恢复";
    if (!insightResult())
        return "先生成数据或导入 CSV，即可预览与导出";
    if (visual.editDirty[visual.selected])
        return "当前编辑尚未应用；预览保留已生效设置";
    return visual.scope == 0
               ? "导出当前图表；同名文件自动追加序号"
               : "组合已选 " + std::to_string(selectedChartCount()) + " / 7 张；按图表列表顺序导出";
}
bool loadInsightCsv(const std::filesystem::path &path) {
    try {
        if (std::filesystem::file_size(path) > 4 * 1024 * 1024)
            throw std::runtime_error("CSV 超过 4 MiB，请导入本研究的完整测量记录");
        msa::Result result;
        result.config = native_ui::to_config(state.draft);
        result.matrix = msa::parse_export_csv(msa::read_text(path), result.config);
        result.metrics = msa::calculate(result.matrix, result.config);
        visual.imported = std::move(result);
        visual.importedSelected = true;
        visual.importedName = path.filename().u8string();
        visual.preview.attempted = false;
        clearVisualError();
        state.feedback("CSV 已载入并分析", "图表和 AI 解读使用载入时的研究设计与分析设置。");
        return true;
    } catch (const std::exception &e) {
        visualError("import", e.what(),
                    "原有数据与图表编辑已保留。请核对研究设计、CSV 列和测量记录，再重新选择文件。");
        return false;
    }
}
void importInsightCsv() {
    try {
        if (const auto path = platform::choose(false, false, L"measurement.csv"))
            loadInsightCsv(*path);
    } catch (const std::exception &e) {
        visualError("import", e.what(), "原有数据与编辑设置已保留，可重新打开文件选择窗口。");
    }
}
void insightSourceControls(Ui &u, const std::string &id, float w) {
    const auto source = [&](float width) {
        segments(u, id + ".source", {"当前结果", "导入 CSV"}, visual.importedSelected ? 1 : 0,
                 width, [](int i) {
                     visual.importedSelected = i == 1;
                     visual.preview.attempted = false;
                 });
    };
    if (w >= 340)
        u.row(id)
            .width(w)
            .gap(12)
            .content([&] {
                source(w - 124);
                button(u, id + ".import", "选择 CSV", 112, importInsightCsv, false, state.busy());
            })
            .build();
    else {
        source(w);
        button(u, id + ".import", "选择 CSV", w, importInsightCsv, false, state.busy());
    }
}
void insightSourceSummary(Ui &u, const std::string &id, float w) {
    std::string summary = visual.importedSelected
                              ? "请按当前研究设计与分析方法，选择完整的 CSV 测量记录。"
                              : "使用当前有效结果；也可导入 CSV 查看已有研究。";
    const char *color = muted;
    if (const auto *result = insightResult()) {
        try {
            const auto &config = result->config;
            const std::string method =
                config.at("analysis").at("method") == "crossed_anova" ? "ANOVA" : "Xbar-R";
            const auto &design = config.at("design");
            summary = method + " · " + std::to_string(design.at("operators").get<int>()) +
                      " 名操作员 × " + std::to_string(design.at("parts").get<int>()) +
                      " 个部件 × " + std::to_string(design.at("replicates").get<int>()) + " 次重复";
            summary += visual.importedSelected
                           ? "\nCSV：" + visual.importedName + "（使用导入时的分析设置）"
                           : "\n当前生成结果；图表编辑不修改测量值与统计结果。";
        } catch (const std::exception &) {
            summary = "数据快照的研究信息不完整，请重新生成或导入。";
            color = "#AF3525";
        }
    }
    // 数据切换和文件名长度不改变工具栏高度，预览位置保持稳定。
    u.text(id + ".note")
        .size(w, 44)
        .text(summary)
        .fontSize(16)
        .lineHeight(22)
        .color(color)
        .wrap()
        .maxWidth(w)
        .clip()
        .build();
}
void insightSource(Ui &u, const std::string &id, float w) {
    insightSourceControls(u, id, w);
    insightSourceSummary(u, id, w);
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
                throw std::runtime_error("坐标范围必须为有限数字；留空表示自动范围");
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
        state.hint("图表编辑已应用，测量数据保持不变");
        return true;
    } catch (const std::exception &e) {
        visual.editError[index] = e.what();
        visualError("edit", e.what(),
                    "预览和导出设置仍为上次有效值。请修正草稿，或撤销未应用编辑。", index);
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
    state.hint("全部图表已恢复默认外观，组合选择保持不变");
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
                    "数据与编辑设置已保留，可重试渲染、恢复当前图表或切换数据源。",
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
            u.rect(id + ".paper").size(scene.width, scene.height).color("#FFFFFF").build();
            int n = 0;
            for (const auto &p : scene.items) {
                const auto key = id + ".p" + std::to_string(n++);
                if (p.type == P::Type::Text) {
                    u.text(key)
                        .position(p.x, p.y)
                        .size(p.x2, p.y2)
                        .text(p.text)
                        .fontFamily(font())
                        .fontSize(p.font_size * 1.35f)
                        .color(p.color)
                        .horizontalAlign(p.align == P::Align::Center  ? eui::HorizontalAlign::Center
                                         : p.align == P::Align::Right ? eui::HorizontalAlign::Right
                                                                      : eui::HorizontalAlign::Left)
                        .verticalAlign(eui::VerticalAlign::Top)
                        .wrap()
                        .maxWidth(p.x2)
                        .lineHeight(p.font_size * 1.35f)
                        .clip()
                        .build();
                } else if (p.type == P::Type::Rect) {
                    u.rect(key).position(p.x, p.y).size(p.x2, p.y2).color(p.color).build();
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
                            .color(p.color)
                            .points({{p.x + dx * a + nx, p.y + dy * a + ny},
                                     {p.x + dx * b + nx, p.y + dy * b + ny},
                                     {p.x + dx * b - nx, p.y + dy * b - ny},
                                     {p.x + dx * a - nx, p.y + dy * a - ny}})
                            .build();
                    }
                }
            }
        })
        .build();
}
void exportVisuals() {
    if (visual.busy() || state.busy() || !insightResult())
        return;
    try {
        std::vector<msa::charts::Scene> scenes;
        for (int i = 0; i < 7; ++i) {
            if (visual.scope == 0 ? i == visual.selected : visual.included[i]) {
                if (visual.editDirty[i]) {
                    visual.selected = i;
                    visual.editing = true;
                    visualError("edit",
                                "“" + std::string(msa::charts::label(msa::charts::all_kinds()[i])) +
                                    "”还有未应用的编辑",
                                "请先应用或撤销该图表草稿，再导出；不会静默忽略编辑内容。", i);
                    return;
                }
                scenes.push_back(msa::charts::make_scene(
                    *insightResult(), msa::charts::all_kinds()[i], chartEditor(i), 900, 560));
            }
        }
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
        visual.exportSummary =
            "正在导出 " + std::to_string(count) + " 张图表，使用开始导出时的设置。";
        clearVisualError();
        state.hint("正在导出 " + std::to_string(count) + " 张图表…");
    } catch (const std::exception &e) {
        visualError("export", e.what(),
                    "数据与图表设置已保留。请核对图表选择、文件名和目录后重试。");
    }
}
void pollVisuals() {
    if (visual.exporting.valid() &&
        visual.exporting.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        try {
            visual.lastExportPath = visual.exporting.get();
            visual.exportSummary = "已导出 " + std::to_string(visual.exportingCount) + " 张图表：" +
                                   visual.lastExportPath.filename().u8string();
            if (visual.errorKind == "export")
                clearVisualError();
            state.feedback("图表已导出", "图表已保存：" + visual.lastExportPath.u8string());
        } catch (const std::exception &e) {
            visual.exportSummary = "本次导出未完成；可修改设置后重试。";
            visualError(
                "export", e.what(),
                "原有数据、图表设置与已导出文件均保留。请检查目录权限或修改导出设置后重试。");
        }
}
void chartAppearance(Ui &u, float w) {
    const int i = visual.selected;
    auto &e = visual.drafts[i];
    // 图表索引隔离控件编辑状态；重置版本确保撤销后输入缓冲也恢复为已应用内容。
    const auto id = "chart.edit." + std::to_string(i) + "." + std::to_string(visual.editEpoch[i]);
    const auto changed = [i] { visual.editDirty[i] = true; };
    section(u, "chart.editor", "图表编辑", w, [&](float c) {
        u.column(id + ".fields")
            .width(c)
            .gap(10)
            .content([&] {
                subheading(u, id + ".axes.heading", "标题与坐标", "纵轴范围留空时自动适配。", c);
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
                subheading(u, id + ".appearance.heading", "图形外观", "", c);
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
                        u.rect(id + ".color.sample")
                            .size(20, 20)
                            .radius(10)
                            .color(e.primary_color)
                            .build();
                        combo(u, id + ".color",
                              {"主色 · 靛蓝", "主色 · 青绿", "主色 · 紫色", "主色 · 琥珀",
                               "主色 · 玫红"},
                              color, c - 32, [i, colors](int k) {
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
                            text(u, id + ".weight.label", "线条粗细", width, 22, 13, muted);
                            choiceChips(u, id + ".weight", {"细线", "标准", "加粗"},
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
                            text(u, id + ".points.label", "数据点大小", width, 22, 13, muted);
                            choiceChips(u, id + ".points", {"小", "标准", "大"},
                                        std::clamp(int(e.point_radius) - 2, 0, 2), width,
                                        [i](int k) {
                                            visual.drafts[i].point_radius = float(k + 2);
                                            visual.editDirty[i] = true;
                                        });
                        })
                        .build();
                };
                if (c >= 680)
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
                subheading(u, id + ".references.heading", "图例与参考线", "", c);
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
            .text(!visual.editError[i].empty() ? visual.editError[i] + "；已生效设置未改变。"
                  : visual.editDirty[i]        ? "草稿未应用。应用后同步更新预览与导出。"
                                               : "当前设置已应用；切换图表保留各自草稿。")
            .fontSize(16)
            .lineHeight(22)
            .color(!visual.editError[i].empty() ? "#AF3525"
                   : visual.editDirty[i]        ? blue
                                                : muted)
            .wrap()
            .maxWidth(c)
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
                        state.hint("未应用的图表编辑已撤销");
                    },
                    false, !visual.editDirty[i] && visual.editError[i].empty());
            })
            .build();
        u.row(id + ".reset.row")
            .width(c)
            .gap(12)
            .content([&] {
                button(u, id + ".reset", "重置当前", (c - 12) / 2, [i] { resetChart(i); });
                button(u, id + ".resetall", "重置全部", (c - 12) / 2, resetAllCharts);
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
        visualError("directory", e.what(), "原目录设置已保留，可重新选择目录或直接填写完整路径。");
    }
}
void openChartDirectory(bool last = false) {
    try {
        const auto directory = last && !visual.lastExportPath.empty()
                                   ? visual.lastExportPath.parent_path()
                                   : chartOutputDirectory();
        if (!std::filesystem::is_directory(directory))
            throw std::runtime_error("目录尚不存在；成功导出时会创建，也可先选择一个现有目录");
        platform::openDirectory(directory);
        if (visual.errorKind == "directory")
            clearVisualError();
    } catch (const std::exception &e) {
        visualError("directory", e.what(), "请检查路径，或使用“选择目录”更换位置。");
    }
}
void visualRecovery(Ui &u, float w) {
    if (visual.errorText.empty())
        return;
    section(u, "chart.recovery", "操作未完成", w, [&](float c) {
        paragraph(u, "chart.recovery.reason", visual.errorText, c, "#AF3525", 18);
        paragraph(u, "chart.recovery.action", visual.recovery, c, muted, 17);
        const auto kind = visual.errorKind;
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
                            if (visual.errorChart >= 0)
                                visual.selected = visual.errorChart;
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
                    text(u, "chart.format.label", "文件格式", width, 22, 13, muted);
                    segments(
                        u, "chart.format", {"PNG", "JPEG", "PDF"}, visual.format, width,
                        [](int i) { visual.format = i; }, 36, true);
                })
                .build();
        };
        const auto scope = [&](float width) {
            u.column("chart.scope.field")
                .width(width)
                .gap(5)
                .content([&] {
                    text(u, "chart.scope.label", "导出范围", width, 22, 13, muted);
                    segments(
                        u, "chart.scope", {"当前单图", "多图组合"}, visual.scope, width,
                        [](int i) { visual.scope = i; }, 36, true);
                })
                .build();
        };
        if (c >= 620)
            u.row("chart.output.row")
                .width(c)
                .gap(12)
                .content([&] {
                    format((c - 12) / 2);
                    scope((c - 12) / 2);
                })
                .build();
        else {
            format(c);
            scope(c);
        }
        const auto resolution = [&](float width) {
            u.column("chart.resolution.field")
                .width(width)
                .gap(5)
                .content([&] {
                    text(u, "chart.resolution.label", "单图宽度", width, 22, 13, muted);
                    segments(
                        u, "chart.resolution", {"1200 px", "1800 px", "2400 px"}, visual.resolution,
                        width, [](int i) { visual.resolution = i; }, 36, true);
                })
                .build();
        };
        const auto columns = [&](float width) {
            u.column("chart.columns.field")
                .width(width)
                .gap(5)
                .content([&] {
                    text(u, "chart.columns.label", "组合排版", width, 22, 13, muted);
                    segments(
                        u, "chart.columns", {"单列", "双列"}, visual.columns, width,
                        [](int i) { visual.columns = i; }, 36, true);
                })
                .build();
        };
        if (visual.scope == 1 && c >= 620)
            u.row("chart.output.size.row")
                .width(c)
                .gap(12)
                .content([&] {
                    resolution((c - 12) / 2);
                    columns((c - 12) / 2);
                })
                .build();
        else {
            resolution(c);
            if (visual.scope == 1)
                columns(c);
        }
        if (visual.scope == 1) {
            subheading(u, "chart.selection.heading", "组合图表", "按下方顺序排列。", c);
            u.row("chart.selection.actions")
                .width(c)
                .gap(8)
                .alignItems(eui::Align::CENTER)
                .content([&] {
                    text(u, "chart.selection.count",
                         "已选 " + std::to_string(selectedChartCount()) + " / 7", c - 216, 32, 13,
                         selectedChartCount() ? ink : "#AF3525");
                    button(
                        u, "chart.selection.all", "全选", 100, [] { visual.included.fill(true); },
                        false, false, 32);
                    button(
                        u, "chart.selection.none", "清空", 100, [] { visual.included.fill(false); },
                        false, false, 32);
                })
                .build();
            const auto item = [&](int i, float width) {
                checkOption(u, "chart.select." + std::to_string(i),
                            msa::charts::label(msa::charts::all_kinds()[i]), visual.included[i],
                            width, [i](bool value) { visual.included[i] = value; });
            };
            const int count = c >= 840 ? 3 : c >= 500 ? 2 : 1;
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
                 selectedChartCount() ? "可独立编辑每张图，导出使用已应用设置。"
                                      : "至少选择一张图表后才能导出组合。",
                 c, 26, 12, selectedChartCount() ? muted : "#AF3525");
        } else {
            text(u, "chart.selection.current",
                 "当前单图：" +
                     std::string(msa::charts::label(msa::charts::all_kinds()[visual.selected])),
                 c, 26, 12, muted);
        }
        subheading(u, "chart.destination.heading", "保存位置", "", c);
        const auto filename = [&](float width) {
            localField(u, "chart.filename", "文件名（自动添加扩展名）", visual.filename, width);
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
        u.row("chart.directory.actions")
            .width(c)
            .gap(12)
            .content([&] {
                const float width = c >= 440 ? (c - 24) / 3 : (c - 12) / 2;
                button(u, "chart.browse", "选择目录", width, chooseChartDirectory);
                button(u, "chart.open", "打开目录", width, [] { openChartDirectory(); });
                if (c >= 440)
                    last(width);
            })
            .build();
        // 成功提示仅使用固定底栏；目录操作始终占位，导出完成不改变正文高度。
        if (c < 440)
            last(c);
        u.text("chart.output.note")
            .size(c, 44)
            .text(std::string("使用已应用设置；同名自动追加序号。\n") +
                  (visual.format == 2 ? "PDF：单页图像，文字不可选择。"
                                      : "组合为单张图片，保留所选顺序。"))
            .fontSize(16)
            .lineHeight(22)
            .color(muted)
            .wrap()
            .maxWidth(c)
            .clip()
            .build();
    });
}
void chartsPage(Ui &u, float w, float h) {
    components::scrollView(u, "charts.scroll")
        .size(w, h)
        .offset(visual.offset)
        .theme(tokens())
        .reserveScrollbar()
        .scrollbarGap(8)
        .onChange([](float y) {
            visual.offset = y;
            menu.id.clear();
        })
        .content([h](Ui &v, float cw, float) {
            v.column("charts.content")
                .width(cw)
                .gap(14)
                .padding(0, 0, 0, 16)
                .content([&] {
                    section(v, "charts.source", "数据与图表", cw, [&](float c) {
                        std::vector<std::string> names;
                        for (auto k : msa::charts::all_kinds())
                            names.emplace_back(msa::charts::label(k));
                        const auto kind = [&](float width) {
                            combo(v, "charts.kind", names, visual.selected, width, [](int i) {
                                visual.selected = i;
                                menu.id.clear();
                            });
                        };
                        const auto edit = [&](float width) {
                            button(
                                v, "charts.edit", visual.editing ? "收起编辑" : "编辑图表", width,
                                [] {
                                    visual.editing = !visual.editing;
                                    menu.id.clear();
                                },
                                visual.editing);
                        };
                        v.column("charts.source.controls")
                            .width(c)
                            .gap(8)
                            .content([&] {
                                if (c >= 900) {
                                    const float sourceWidth = std::max(340.f, (c - 140) * .44f);
                                    v.row("charts.source.toolbar")
                                        .width(c)
                                        .gap(12)
                                        .content([&] {
                                            insightSourceControls(v, "charts.dataset", sourceWidth);
                                            kind(c - sourceWidth - 152);
                                            edit(128);
                                        })
                                        .build();
                                } else {
                                    insightSourceControls(v, "charts.dataset", c);
                                    v.row("charts.source.chart.row")
                                        .width(c)
                                        .gap(12)
                                        .content([&] {
                                            kind(c - 140);
                                            edit(128);
                                        })
                                        .build();
                                }
                                insightSourceSummary(v, "charts.dataset", c);
                            })
                            .build();
                    });
                    auto preview = [&](float width) {
                        section(v, "charts.preview", "", width, [&](float c) {
                            if (const auto *result = insightResult()) {
                                // 固定状态栏保留原有高度，草稿变化不会推移图形。
                                v.row("charts.preview.draft")
                                    .size(c, 44)
                                    .gap(12)
                                    .alignItems(eui::Align::CENTER)
                                    .content([&] {
                                        const bool dirty = visual.editDirty[visual.selected];
                                        badge(v, "charts.preview.state",
                                              dirty ? "未应用" : "已应用", 76, dirty ? blue : green,
                                              dirty ? "#EDF2FF" : "#E7F5F0");
                                        v.text("charts.preview.summary")
                                            .size(c - 88, 44)
                                            .text(dirty ? "草稿未应用；下图保留上次有效外观。"
                                                        : "外观已应用，预览与导出使用相同设置。")
                                            .fontSize(16)
                                            .lineHeight(22)
                                            .color(muted)
                                            .verticalAlign(eui::VerticalAlign::Center)
                                            .wrap()
                                            .maxWidth(c - 88)
                                            .clip()
                                            .build();
                                    })
                                    .build();
                                if (const auto *scene = chartPreview(
                                        *result, c, std::clamp(h - 310.f, 340.f, 620.f)))
                                    drawChartScene(
                                        v, "charts.scene." + std::to_string(visual.selected),
                                        *scene);
                                else
                                    paragraph(
                                        v, "charts.invalid",
                                        "本图暂时无法显示。数据与设置已保留，请使用下方恢复操作。",
                                        c, "#AF3525", 19);
                            } else
                                paragraph(v, "charts.empty",
                                          "暂无有效数据。请先生成数据，或选择 CSV 进行分析。", c,
                                          muted, 19);
                        });
                    };
                    if (visual.editing && cw >= 1100)
                        v.row("charts.layout")
                            .width(cw)
                            .gap(16)
                            .content([&] {
                                preview(cw - 376);
                                chartAppearance(v, 360);
                            })
                            .build();
                    else {
                        preview(cw);
                        if (visual.editing)
                            chartAppearance(v, cw);
                    }
                    visualRecovery(v, cw);
                    chartExportSettings(v, cw);
                })
                .build();
        })
        .build();
}
