// 由 gui.cpp 在 gui 命名空间、通用控件之后包含。
struct VisualPreview {
    std::optional<msa::Result> snapshot;
    std::optional<msa::charts::Scene> scene;
    msa::charts::Editor editor;
    int selected = -1;
    float width = 0, height = 0;
    bool attempted = false, imported = false, draft = false;
    std::array<std::string, 4> draftInputs;
    std::string error, draftError;
};
struct ChartEdits {
    std::array<msa::charts::Editor, 7> editors, drafts;
    std::array<std::string, 7> lower, upper, ticks, bins;
    std::array<bool, 7> editDirty{};
    std::array<std::string, 7> editError;
    std::array<unsigned, 7> editEpoch{};

    explicit ChartEdits(msa::charts::Style style) {
        for (std::size_t i = 0; i < editors.size(); ++i)
            editors[i] = drafts[i] =
                msa::charts::default_editor(msa::charts::all_kinds()[i], style);
    }
};
struct VisualState {
    int selected = 0, editorTab = 0;
    float offset = 0, editorOffset = 0;
    bool importedSelected = false, editing = false;
    std::optional<msa::Result> imported;
    std::string importedName, importedSheet;
    std::array<ChartEdits, 2> styles{ChartEdits(msa::charts::Style::Default),
                                     ChartEdits(msa::charts::Style::Origin)};
    std::string errorKind, errorText, recovery;
    std::filesystem::path importedPath;
    std::filesystem::path lastExportPath;
    int exportingCount = 0, errorChart = -1;
    bool exportingReport = false;
    VisualPreview preview;
    std::future<std::filesystem::path> exporting;
    bool busy() const { return exporting.valid(); }
} visual;

enum class ImportStep { Worksheet, Design };
struct PendingStudyImport {
    std::filesystem::path path;
    Json config;
    std::unique_ptr<msa::XlsxWorkbook> workbook;
    std::string csv;
    std::vector<std::string> sheetNames;
    int sheetIndex = 0, replicates = 0;
    ImportStep step = ImportStep::Worksheet;
    unsigned generation = 0;
};
std::optional<PendingStudyImport> pendingImport;
unsigned importStepGeneration = 0;

msa::charts::Style activeChartStyle() {
    return static_cast<msa::charts::Style>(state.draft.charts.style);
}
ChartEdits &chartEdits() { return visual.styles.at(state.draft.charts.style); }
std::string chartEditId(int index) {
    return "chart.edit." + std::to_string(state.draft.charts.style) + "." + std::to_string(index) +
           "." + std::to_string(chartEdits().editEpoch[index]);
}

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
msa::StudySource insightSource() {
    return visual.importedSelected ? msa::StudySource::ImportedData : msa::StudySource::Generated;
}
std::string importedSourceName() {
    return visual.importedSheet.empty() ? visual.importedName
                                        : visual.importedName + " · " + visual.importedSheet;
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
    if (kind == "report")
        state.feedback("方法对比报告导出失败", message, Level::Error);
    else
        state.feedback("图表操作未完成，请查看页面提示", message + "。" + recovery, Level::Warning);
}
void protectExportSources(const std::filesystem::path &path) {
    if ((!visual.importedPath.empty() && msa::same_file_path(path, visual.importedPath)) ||
        msa::same_file_path(path, msa::config_path()))
        throw msa::ConfigError("导出路径与输入数据文件或配置文件相同，请另选文件名");
}
template <typename Writer>
std::filesystem::path writeVisualExport(const std::filesystem::path &initialPath,
                                        const std::optional<platform::SaveTarget> &chosen,
                                        Writer write) {
    if (chosen) {
        write(chosen->path, chosen->policy);
        return chosen->path;
    }
    for (int i = 0; i < 10000; ++i) {
        auto path = initialPath;
        if (i)
            path.replace_filename(initialPath.stem().wstring() + L"_" + std::to_wstring(i) +
                                 initialPath.extension().wstring());
        if (std::filesystem::exists(path))
            continue;
        try {
            write(path, msa::ExistingFile::Preserve);
            return path;
        } catch (const msa::FileExistsError &) {
            // 其他导出可能在路径检查后先提交了同名文件。
        }
    }
    throw std::runtime_error("同名文件过多，请修改文件名或目录");
}
template <typename Writer>
void startVisualExport(bool report, int chartCount, Writer write) {
    visual.exporting = std::async(std::launch::async, [write = std::move(write)] {
        try {
            auto path = write();
            app::requestUpdate();
            return path;
        } catch (...) {
            app::requestUpdate();
            throw;
        }
    });
    visual.exportingReport = report;
    visual.exportingCount = chartCount;
}
int selectedChartCount() {
    return state.draft.charts.scope == 0
               ? 1
               : int(std::count(state.draft.charts.included.begin(), state.draft.charts.included.end(), true));
}
bool chartSelectedForExport(int index) {
    return state.draft.charts.scope == 0 ? index == visual.selected : state.draft.charts.included[index];
}
std::optional<int> pendingChartExportEdit() {
    for (int i = 0; i < int(chartEdits().editDirty.size()); ++i)
        if (chartSelectedForExport(i) && chartEdits().editDirty[i])
            return i;
    return {};
}
const std::vector<std::string> chartFormatLabels{"PNG", "JPEG", "PDF", "SVG"};
std::string chartFooterSummary() {
    if (visual.busy())
        return visual.exportingReport ? "正在导出方法对比报告…"
                                      : "正在导出 " + std::to_string(visual.exportingCount) + " 张图表…";
    if (!visual.errorText.empty())
        return visual.errorKind == "report" ? "方法对比报告导出失败" : "图表操作未完成，请查看页面提示";
    if (!insightResult())
        return "请先生成数据或导入数据";
    if (pendingChartExportEdit())
        return state.draft.charts.scope == 0 ? "当前图表有待应用的编辑" : "待导出的组合含未应用的编辑";
    const auto &format = chartFormatLabels.at(state.draft.charts.format);
    if (state.draft.charts.scope == 0)
        return format + " · " + msa::charts::label(msa::charts::all_kinds()[visual.selected]);
    const auto count = selectedChartCount();
    return count ? format + " · 已选 " + std::to_string(count) + " / 7 张图表"
                 : "请在图表选项中勾选要导出的图表";
}
void cancelStudyImport() { pendingImport.reset(); }
void backStudyImport() {
    if (!pendingImport || pendingImport->step != ImportStep::Design || !pendingImport->workbook)
        return;
    pendingImport->csv.clear();
    pendingImport->replicates = 0;
    pendingImport->step = ImportStep::Worksheet;
    pendingImport->generation = ++importStepGeneration;
}

// Parsing only the selected sheet keeps unrelated sheets lazy. If the design is ambiguous,
// retain this exact CSV payload across UI frames, even if the user replaces the source file.
bool advanceStudyImport() {
    auto &input = *pendingImport;
    try {
        if (input.workbook && input.step == ImportStep::Worksheet) {
            input.csv = input.workbook->csv(input.sheetIndex);
            if (input.sheetNames.size() == 1)
                input.workbook.reset();
        }
        auto data = msa::import_csv_study(input.csv, input.config, [&]() -> std::optional<int> {
            if (input.replicates == 0)
                return std::nullopt;
            return input.replicates;
        });
        if (!data) {
            input.step = ImportStep::Design;
            input.generation = ++importStepGeneration;
            return false;
        }
        msa::Result result;
        result.config = std::move(data->config);
        result.matrix = std::move(data->matrix);
        result.metrics = msa::calculate(result.matrix, result.config);
        auto name = input.path.filename().u8string();
        auto sheet = input.sheetNames.empty() ? std::string{} : input.sheetNames.at(input.sheetIndex);
        visual.imported = std::move(result);
        visual.importedPath = std::move(input.path);
        visual.importedSelected = true;
        visual.importedName = std::move(name);
        visual.importedSheet = std::move(sheet);
        visual.preview.attempted = false;
        clearVisualError();
        state.feedback("测量数据已导入",
                       importedSourceName() + " · 已识别 " +
                           visual.imported->config.at("design").at("operators").dump() + " 名操作员");
        pendingImport.reset();
        return true;
    } catch (const std::exception &e) {
        std::string detail = e.what();
        if (!input.sheetNames.empty() && input.sheetIndex >= 0 &&
            static_cast<std::size_t>(input.sheetIndex) < input.sheetNames.size())
            detail = "工作表“" + input.sheetNames[input.sheetIndex] + "”：" + detail;
        pendingImport.reset();
        visualError("import", detail, "请核对文件列、测量记录及分析设置，再重新选择文件。");
        return false;
    }
}
bool confirmStudyImport() {
    if (!pendingImport || (pendingImport->step == ImportStep::Design &&
                           pendingImport->replicates != 2 && pendingImport->replicates != 3))
        return false;
    return advanceStudyImport();
}
bool loadInsightData(const std::filesystem::path &path) {
    if (pendingImport)
        return false;
    try {
        auto &input = pendingImport.emplace();
        input.path = std::filesystem::absolute(path);
        input.config = native_ui::to_config(state.draft, native_ui::ConfigUse::DataImport);
        if (msa::data_file_type(path) == "xlsx") {
            input.workbook = std::make_unique<msa::XlsxWorkbook>(
                msa::read_file_bytes(path, msa::xlsx_read_limit));
            input.sheetNames = input.workbook->sheet_names();
            if (input.sheetNames.size() > 1) {
                input.generation = ++importStepGeneration;
                return false;
            }
        } else {
            input.csv = msa::read_file_bytes(path, msa::csv_read_limit);
        }
        return advanceStudyImport();
    } catch (const std::exception &e) {
        pendingImport.reset();
        visualError("import", e.what(),
                    "请核对文件列、测量记录及分析设置，再重新选择文件。");
        return false;
    }
}
bool reanalyzeInsightData() {
    if (!visual.importedSelected || !visual.imported || state.busy())
        return false;
    try {
        auto config = native_ui::to_config(state.draft, native_ui::ConfigUse::DataImport);
        config["design"] = visual.imported->config.at("design");
        config = msa::normalize_config(config);
        auto metrics = msa::calculate(visual.imported->matrix, config);
        visual.imported->config = std::move(config);
        visual.imported->metrics = std::move(metrics);
        visual.preview.attempted = false;
        clearVisualError();
        state.feedback("测量数据已按当前设置重算", "已使用导入的原始记录重算 " + importedSourceName());
        return true;
    } catch (const std::exception &e) {
        visualError("reanalysis", e.what(), "已保留原分析结果。请调整选项后重算。");
        return false;
    }
}
void exportComparisonReport() {
    const auto *result = insightResult();
    if (!result || state.busy() || visual.busy())
        return;
    try {
        const bool pdf = state.draft.reportFormat == 0;
        const auto directory = msa::resolve_output_directory(native_ui::trim(state.draft.reportDirectory));
        const auto source = insightSource();
        const auto extension = pdf ? ".pdf" : ".html";
        const auto basename = msa::render_study_basename(state.draft.reportFilename,
            msa::ExportBasenameKind::ComparisonReport, *result, source, std::chrono::system_clock::now(), extension);
        const auto initial = directory / std::filesystem::u8path(basename + extension);
        std::optional<platform::SaveTarget> chosen;
        if (!state.draft.silentReport) {
            chosen = state.chooseExportPath(initial, pdf ? platform::ExportKind::ReportPdf
                                                        : platform::ExportKind::Html);
            if (!chosen)
                return;
            protectExportSources(chosen->path);
        }
        const std::string name = visual.importedSelected ? importedSourceName() : "";
        startVisualExport(true, 0, [initial, chosen, snapshot = *result, source, name, style = activeChartStyle()] {
            return writeVisualExport(initial, chosen, [&](const auto &path, auto policy) {
                msa::write_comparison_report(path, snapshot, source, name, policy, style);
            });
        });
        if (visual.errorKind == "report")
            clearVisualError();
        state.hint("正在导出方法对比报告…");
    } catch (const std::exception &e) {
        visualError("report", e.what(), "请检查保存位置、文件名和当前数据源后重试。");
    }
}
void importInsightData() {
    if (pendingImport)
        return;
    try {
        const auto directory = msa::resolve_output_directory(native_ui::trim(state.draft.importDirectory));
        const auto name = msa::render_import_basename(state.draft.importFilename,
            native_ui::filename_options(state.draft), insightResult(), insightSource(),
            std::chrono::system_clock::now(), "");
        if (const auto path = state.chooseImportData(directory, name))
            loadInsightData(*path);
    } catch (const std::exception &e) {
        visualError("import", e.what(), "请重新选择 CSV 或 XLSX 文件。");
    }
}
void insightSourceControls(Ui &u, const std::string &id, float w) {
    constexpr float importWidth = 124;
    const auto source = [&](float width) {
        segments(
            u, id + ".source", {"当前结果", "已导入数据"}, visual.importedSelected ? 1 : 0, width,
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
                button(u, id + ".import", "导入数据", importWidth, importInsightData, false,
                       state.busy());
            })
            .build();
    else {
        source(w);
        button(u, id + ".import", "导入数据", w, importInsightData, false, state.busy());
    }
}
void insightSourceSummary(Ui &u, const std::string &id, float w) {
    std::string summary = visual.importedSelected ? "尚未导入数据"
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
            if (visual.importedSelected) {
                const auto label = visual.importedSheet.empty() ? visual.importedName
                    : visual.importedSheet + " · " + visual.importedName;
                summary = labelThatFits(label, w, 16) + "\n" + method + " · " +
                          config.at("design").at("operators").dump() + " 人 · " +
                          std::to_string(count) + " 条记录";
            }
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
msa::charts::Editor chartEditor(int index) { return chartEdits().editors.at(index); }
void discardChartEdits(int index) {
    auto &edits = chartEdits();
    const auto &editor = edits.editors[index];
    edits.drafts[index] = editor;
    edits.lower[index] = editor.y_min ? Json(*editor.y_min).dump() : "";
    edits.upper[index] = editor.y_max ? Json(*editor.y_max).dump() : "";
    edits.ticks[index] = editor.y_tick_intervals ? std::to_string(editor.y_tick_intervals) : "";
    edits.bins[index] = editor.histogram_bins ? std::to_string(editor.histogram_bins) : "";
    edits.editDirty[index] = false;
    edits.editError[index].clear();
    ++edits.editEpoch[index];
    if (visual.errorKind == "edit" && visual.errorChart == index)
        clearVisualError();
}
msa::charts::Editor proposedChartEditor(int index) {
    const auto &edits = chartEdits();
    auto proposed = edits.drafts[index];
    const auto bound = [](const std::string &value) -> std::optional<double> {
        if (native_ui::trim(value).empty())
            return std::nullopt;
        const double parsed = msa::parse_number(value);
        if (!std::isfinite(parsed))
            throw std::runtime_error("纵轴范围需要有限数值，留空可自动设定范围");
        return parsed;
    };
    const auto count = [](const std::string &value, int low, int high, const char *message) {
        if (native_ui::trim(value).empty())
            return 0;
        try {
            const auto n = msa::parse_integer(native_ui::trim(value));
            if (n >= low && n <= high)
                return static_cast<int>(n);
        } catch (const msa::ConfigError &) {
        }
        throw std::runtime_error(message);
    };
    proposed.y_min = bound(edits.lower[index]);
    proposed.y_max = bound(edits.upper[index]);
    if (proposed.y_min && proposed.y_max && *proposed.y_min >= *proposed.y_max)
        throw std::runtime_error("纵轴下限必须小于上限");
    proposed.y_tick_intervals =
        count(edits.ticks[index], 2, 10, "纵轴刻度间隔数须为 2 至 10 的整数");
    proposed.histogram_bins = count(edits.bins[index], 3, 40, "直方图分箱数须为 3 至 40 的整数");
    if (proposed.title.size() > 600 || proposed.x_label.size() > 300 ||
        proposed.y_label.size() > 300)
        throw std::runtime_error("图表标题或坐标轴名称过长，请精简后应用");
    return proposed;
}
bool applyChartEdits(int index) {
    try {
        auto proposed = proposedChartEditor(index);
        if (const auto *result = insightResult())
            (void)msa::charts::make_scene(*result, msa::charts::all_kinds()[index], proposed, 900,
                                          560);
        chartEdits().editors[index] = std::move(proposed);
        discardChartEdits(index);
        visual.preview.attempted = false;
        state.hint("图表编辑已应用");
        return true;
    } catch (const std::exception &e) {
        chartEdits().editError[index] = e.what();
        visualError("edit", e.what(), "请修正或撤销草稿，再应用编辑。", index);
        return false;
    }
}
void resetChart(int index) {
    chartEdits().editors[index] = msa::charts::default_editor(msa::charts::all_kinds()[index], activeChartStyle());
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
    return a.style == b.style && a.title == b.title && a.x_label == b.x_label && a.y_label == b.y_label &&
           a.primary_color == b.primary_color && a.line_width == b.line_width &&
           a.point_radius == b.point_radius && a.show_grid == b.show_grid &&
           a.show_legend == b.show_legend && a.show_limits == b.show_limits && a.y_min == b.y_min &&
           a.y_max == b.y_max && a.legend_position == b.legend_position &&
           a.marker_shape == b.marker_shape && a.y_tick_intervals == b.y_tick_intervals &&
           a.histogram_bins == b.histogram_bins;
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
    const auto &edits = chartEdits();
    auto &cache = visual.preview;
    const int i = visual.selected;
    const bool draft = visual.editing && edits.editDirty[i];
    const auto &editor = draft ? edits.drafts[i] : edits.editors[i];
    const std::array<std::string, 4> inputs{edits.lower[i], edits.upper[i], edits.ticks[i],
                                            edits.bins[i]};
    const bool sameData = cache.snapshot && sameChartData(*cache.snapshot, result);
    if (cache.attempted && sameData && cache.imported == visual.importedSelected &&
        cache.selected == visual.selected && cache.width == width && cache.height == height &&
        cache.draft == draft && (!draft || cache.draftInputs == inputs) &&
        sameChartEditor(cache.editor, editor))
        return cache.scene ? &*cache.scene : nullptr;
    // 缓存持有数据副本，不保存 Result 指针；重生成、导入或切换数据源都会重新比较内容。
    if (!sameData)
        cache.snapshot = result;
    cache.imported = visual.importedSelected;
    cache.selected = visual.selected;
    cache.editor = editor;
    cache.draft = draft;
    cache.draftInputs = inputs;
    cache.draftError.clear();
    cache.width = width;
    cache.height = height;
    cache.attempted = true;
    cache.error.clear();
    cache.scene.reset();
    try {
        const auto kind = msa::charts::all_kinds()[i];
        if (draft) {
            try {
                cache.scene =
                    msa::charts::make_scene(result, kind, proposedChartEditor(i), width, height);
            } catch (const std::exception &e) {
                cache.draftError = e.what();
            }
        }
        if (!cache.scene)
            cache.scene = msa::charts::make_scene(result, kind, edits.editors[i], width, height);
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
                    const bool title = n == 2;
                    // EUI 按字体设计高度计量，导出按 em 计量。内嵌 Noto CJK 的
                    // hhea 上升/下降值为 1160/-288，每 em 为 1000 个字体单位。
                    constexpr float designHeightPerEm = (1160.f + 288.f) / 1000.f;
                    float size = p.font_size * designHeightPerEm;
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
                                      : labelThatFits(p.text, p.x2, size, weight))
                            .fontFamily(font())
                            .fontSize(size)
                            .fontWeight(weight)
                            .color(p.color)
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
                        .color(p.color)
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
                        const float x1 = p.x + dx * a, y1 = p.y + dy * a;
                        const float x2 = p.x + dx * b, y2 = p.y + dy * b;
                        // EUI shades the entire polygon frame. Bound each stroke to its ink,
                        // including the antialias fringe for dirty-region and scroll clipping.
                        const float padX = std::abs(nx) + 2, padY = std::abs(ny) + 2;
                        const float left = std::min(x1, x2) - padX, top = std::min(y1, y2) - padY;
                        u.polygon(key + "." + std::to_string(j))
                            .position(left, top)
                            .size(std::abs(x2 - x1) + 2 * padX, std::abs(y2 - y1) + 2 * padY)
                            .color(p.color)
                            .points({{x1 + nx - left, y1 + ny - top}, {x2 + nx - left, y2 + ny - top},
                                     {x2 - nx - left, y2 - ny - top}, {x1 - nx - left, y1 - ny - top}})
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
        const auto directory = msa::resolve_output_directory(native_ui::trim(state.draft.chartDirectory));
        platform::ChartExportOptions options;
        options.format = static_cast<platform::ChartFormat>(state.draft.charts.format);
        options.columns = state.draft.charts.scope == 0 ? 1 : state.draft.charts.columns + 1;
        options.panel_width = 1200 + state.draft.charts.resolution * 600;
        options.dpi = 144 + state.draft.charts.resolution * 72;
        const int count = int(scenes.size());
        const auto extension = platform::chart_extension(options.format);
        const auto basename = msa::render_study_basename(state.draft.chartFilename,
            msa::ExportBasenameKind::Chart, *insightResult(), insightSource(), std::chrono::system_clock::now(), extension);
        const auto initial = directory / std::filesystem::u8path(basename + extension);
        std::optional<platform::SaveTarget> chosen;
        if (!state.draft.silentCharts) {
            constexpr std::array kinds{platform::ExportKind::Png, platform::ExportKind::Jpeg,
                                       platform::ExportKind::ChartPdf, platform::ExportKind::Svg};
            const auto kind = kinds.at(static_cast<std::size_t>(options.format));
            chosen = state.chooseExportPath(initial, kind);
            if (!chosen)
                return;
            protectExportSources(chosen->path);
        }
        startVisualExport(false, count, [initial, chosen, scenes = std::move(scenes), options] {
            return writeVisualExport(initial, chosen, [&](const auto &path, auto policy) {
                platform::export_charts(scenes, path, options, policy);
            });
        });
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
            const auto path = visual.exporting.get();
            if (visual.errorKind == (visual.exportingReport ? "report" : "export"))
                clearVisualError();
            if (visual.exportingReport)
                state.feedback("方法对比报告已导出", "报告已保存至 " + path.u8string());
            else {
                visual.lastExportPath = path;
                state.feedback("图表已导出", "图表已保存至 " + visual.lastExportPath.u8string());
            }
        } catch (const std::exception &e) {
            visualError(visual.exportingReport ? "report" : "export", e.what(),
                        visual.exportingReport ? "可使用当前数据点击「重新导出报告」。"
                                               : "请检查保存位置或修改导出设置后重试。");
        }
}
void stopVisuals() {
    if (visual.exporting.valid())
        visual.exporting.wait();
}
void chartAppearance(Ui &u, float w, float viewportHeight = 0) {
    const int i = visual.selected;
    auto &e = chartEdits().drafts[i];
    // 图表索引隔离控件编辑状态；重置版本确保撤销后输入缓冲也恢复为已应用内容。
    const auto id = chartEditId(i);
    const auto changed = [i] {
        chartEdits().editDirty[i] = true;
        chartEdits().editError[i].clear();
        if (visual.errorKind == "edit" && visual.errorChart == i)
            clearVisualError();
    };
    const auto kind = msa::charts::all_kinds()[i];
    const bool hasPoints =
        kind != msa::charts::Kind::Histogram && kind != msa::charts::Kind::VarianceComponents;
    const auto editorContent = [&](Ui &u, float c) {
        segments(u, id + ".tabs", {"坐标轴", "图形", "图例与线"}, visual.editorTab, c, [](int tab) {
            visual.editorTab = tab;
            visual.editorOffset = 0;
            menu.id.clear();
        });
        u.column(id + ".fields")
            .width(c)
            .gap(sectionGap)
            .content([&] {
                if (visual.editorTab == 0) {
                    localField(u, id + ".title", "图表标题", e.title, c, "", changed);
                    u.row(id + ".axis.names")
                        .width(c)
                        .gap(12)
                        .content([&] {
                            localField(u, id + ".x", "横轴名称", e.x_label, (c - 12) / 2, "",
                                       changed);
                            localField(u, id + ".y", "纵轴名称", e.y_label, (c - 12) / 2, "",
                                       changed);
                        })
                        .build();
                    u.row(id + ".range")
                        .width(c)
                        .gap(12)
                        .content([&] {
                            localField(u, id + ".min", "纵轴下限", chartEdits().lower[i], (c - 12) / 2,
                                       "自动", changed);
                            localField(u, id + ".max", "纵轴上限", chartEdits().upper[i], (c - 12) / 2,
                                       "自动", changed);
                        })
                        .build();
                    localField(u, id + ".ticks", "纵轴主刻度目标间隔数", chartEdits().ticks[i], c,
                               "自动（可填 2 至 10）", changed);
                } else if (visual.editorTab == 1) {
                    const std::vector<std::string> colors{msa::charts::default_primary_color(e.style), "#16866B", "#7B51C2",
                                                          "#D47B20", "#CB4A5B"};
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
                            combo(u, id + ".color", {"蓝色", "青绿", "紫色", "琥珀", "玫红"}, color,
                                  c - 86, [i, colors, changed](int k) {
                                      chartEdits().drafts[i].primary_color = colors[k];
                                      changed();
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
                                         std::clamp(int(e.line_width) - 1, 0, 2), width,
                                         [i, changed](int k) {
                                             chartEdits().drafts[i].line_width = float(k + 1);
                                             changed();
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
                                         std::clamp(int(e.point_radius) - 2, 0, 2), width,
                                         [i, changed](int k) {
                                             chartEdits().drafts[i].point_radius = float(k + 2);
                                             changed();
                                         });
                            })
                            .build();
                    };
                    if (c >= 460 && hasPoints)
                        u.row(id + ".marks.row")
                            .width(c)
                            .gap(12)
                            .content([&] {
                                weight((c - 12) / 2);
                                points((c - 12) / 2);
                            })
                            .build();
                    else {
                        if (kind != msa::charts::Kind::VarianceComponents)
                            weight(c);
                        if (hasPoints)
                            points(c);
                    }
                    if (hasPoints) {
                        text(u, id + ".marker.label", "数据点形状", c, 21, 13);
                        combo(u, id + ".marker", {"按系列区分", "圆形", "方形", "菱形", "三角形"},
                              static_cast<int>(e.marker_shape), c, [i, changed](int k) {
                                  chartEdits().drafts[i].marker_shape =
                                      static_cast<msa::charts::MarkerShape>(k);
                                  changed();
                              });
                    }
                    if (kind == msa::charts::Kind::Histogram)
                        localField(u, id + ".bins", "直方图分箱数", chartEdits().bins[i], c,
                                   "自动（可填 3 至 40）", changed);
                } else {
                    u.row(id + ".display.row")
                        .width(c)
                        .gap(12)
                        .content([&] {
                            checkOption(u, id + ".grid", "网格", e.show_grid, (c - 12) / 2,
                                        [i, changed](bool value) {
                                            chartEdits().drafts[i].show_grid = value;
                                            changed();
                                        });
                            checkOption(u, id + ".legend", "图例", e.show_legend, (c - 12) / 2,
                                        [i, changed](bool value) {
                                            chartEdits().drafts[i].show_legend = value;
                                            changed();
                                        });
                        })
                        .build();
                    if (e.show_legend) {
                        text(u, id + ".legend.position.label", "图例位置", c, 21, 13);
                        segments(u, id + ".legend.position", {"绘图区上方", "绘图区下方"},
                                 static_cast<int>(e.legend_position), c, [i, changed](int k) {
                                     chartEdits().drafts[i].legend_position =
                                         static_cast<msa::charts::LegendPosition>(k);
                                     changed();
                                 });
                    }
                    const bool control = kind == msa::charts::Kind::RangeChart ||
                                         kind == msa::charts::Kind::MeanChart;
                    const bool noLimits = kind == msa::charts::Kind::VarianceComponents;
                    checkOption(
                        u, id + ".limits",
                        control    ? "中心线与控制限"
                        : noLimits ? "规格参考线（本图不适用）"
                                   : "规格参考线",
                        e.show_limits, c,
                        [i, changed](bool value) {
                            chartEdits().drafts[i].show_limits = value;
                            changed();
                        },
                        noLimits);
                }
            })
            .build();
        // 错误和草稿状态共用固定槽位，应用失败不推移编辑操作。
        const bool invalidPreview = chartEdits().editDirty[i] && visual.preview.selected == i &&
                                    visual.preview.draft && !visual.preview.draftError.empty();
        u.text(id + ".status")
            .size(c, 44)
            .text(!chartEdits().editError[i].empty() ? chartEdits().editError[i]
                  : invalidPreview             ? visual.preview.draftError
                  : chartEdits().editDirty[i]        ? "正在预览草稿，应用后可导出。"
                                               : "图表外观保留在本次运行期间。")
            .fontSize(16)
            .lineHeight(22)
            .color(!chartEdits().editError[i].empty() || invalidPreview ? levelColor(Level::Error)
                   : chartEdits().editDirty[i]                          ? levelColor(Level::Warning)
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
                    !chartEdits().editDirty[i]);
                button(
                    u, id + ".discard", "撤销草稿", (c - 12) / 2,
                    [i] {
                        discardChartEdits(i);
                        state.hint("已撤销图表草稿");
                    },
                    false, !chartEdits().editDirty[i] && chartEdits().editError[i].empty());
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
void openFileOptions() {
    state.optionsTab = fileOptionsTab;
    state.configOffset = 0;
    state.navigate(Page::Options);
}
void openLastChartDirectory() {
    try {
        const auto directory = visual.lastExportPath.parent_path();
        if (!std::filesystem::is_directory(directory))
            throw std::runtime_error("上次导出目录已不存在");
        platform::openDirectory(directory);
        if (visual.errorKind == "directory")
            clearVisualError();
    } catch (const std::exception &e) {
        visualError("directory", e.what(), "请在「选项 → 文件 → 目录与文件名」检查保存位置。");
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
                if (kind == "import" || kind == "reanalysis") {
                    if (kind == "reanalysis")
                        button(u, "chart.recovery.reanalyze", "重新计算", bw, reanalyzeInsightData,
                               true, state.busy() || !visual.importedSelected);
                    else
                        button(u, "chart.recovery.import", "重新选择数据", bw, importInsightData,
                               true, state.busy());
                    button(u, "chart.recovery.study", "检查研究设置", bw, [] {
                        state.optionsTab = 0;
                        state.navigate(Page::Options);
                    });
                } else if (kind == "report") {
                    button(u, "chart.recovery.report", "重新导出报告", bw, exportComparisonReport,
                           true, visual.busy() || state.busy() || !insightResult());
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
                    button(u, "chart.recovery.directory", "目录与文件名", bw, openFileOptions);
                }
            })
            .build();
    });
}
void chartStyleSettings(Ui &u, float w) {
    section(u, "chart.styles", "图表风格", w, [&](float c) {
        const auto choice = [&](float width) {
            segments(u, "chart.style", {"默认风格", "Origin 风格"}, state.draft.charts.style, width,
                     [](int index) {
                         if (index == state.draft.charts.style)
                             return;
                         state.draft.charts.style = index;
                         visual.editorOffset = 0;
                         visual.preview.attempted = false;
                         if (visual.errorKind == "edit" || visual.errorKind == "render")
                             clearVisualError();
                         menu.id.clear();
                         state.outputChanged();
                     });
        };
        const auto palette = [&](float width) {
            const auto colors = msa::charts::series_colors(
                msa::charts::default_editor(msa::charts::Kind::Interaction, activeChartStyle()));
            u.row("chart.style.palette")
                .size(width, 36)
                .gap(6)
                .alignItems(eui::Align::CENTER)
                .content([&] {
                    text(u, "chart.palette.label", "系列配色", 64, 26, 13, muted);
                    for (int i = 0; i < 3; ++i)
                        u.rect("chart.palette." + std::to_string(i))
                            .size(24, 10)
                            .radius(4)
                            .color(colors[i])
                            .build();
                })
                .build();
        };
        const auto open = [&] {
            button(u, "chart.style.open", "查看图表", 108, [] { state.navigate(Page::Charts); });
        };
        if (c < 510) {
            choice(c);
            u.row("chart.style.actions")
                .width(c)
                .gap(12)
                .content([&] {
                    palette(c - 120);
                    open();
                })
                .build();
        } else {
            const float width = std::min(360.f, c - 298);
            u.row("chart.style.actions")
                .width(c)
                .gap(12)
                .alignItems(eui::Align::CENTER)
                .content([&] {
                    choice(width);
                    palette(c - width - 132);
                    open();
                })
                .build();
        }
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
                    segments(u, "chart.format", chartFormatLabels, state.draft.charts.format, width,
                             [](int i) {
                                 state.draft.charts.format = i;
                                 state.outputChanged();
                             });
                })
                .build();
        };
        const auto scope = [&](float width) {
            u.column("chart.scope.field")
                .width(width)
                .gap(5)
                .content([&] {
                    text(u, "chart.scope.label", "导出范围", width, 21, 13);
                    segments(u, "chart.scope", {"当前单图", "多图组合"}, state.draft.charts.scope,
                             width, [](int i) {
                                 state.draft.charts.scope = i;
                                 state.outputChanged();
                             });
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
                             state.draft.charts.resolution, width, [](int i) {
                                 state.draft.charts.resolution = i;
                                 state.outputChanged();
                             });
                })
                .build();
        };
        const auto columns = [&](float width) {
            u.column("chart.columns.field")
                .width(width)
                .gap(5)
                .content([&] {
                    text(u, "chart.columns.label", "组合排版", width, 21, 13);
                    segments(u, "chart.columns", {"单列", "双列"}, state.draft.charts.columns,
                             width, [](int i) {
                                 state.draft.charts.columns = i;
                                 state.outputChanged();
                             });
                })
                .build();
        };
        const bool combination = state.draft.charts.scope == 1;
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
                        u, "chart.selection.all", "全选", 72,
                        [] {
                            state.draft.charts.included.fill(true);
                            state.outputChanged();
                        },
                        false, false, 32);
                    button(
                        u, "chart.selection.none", "清空", 72,
                        [] {
                            state.draft.charts.included.fill(false);
                            state.outputChanged();
                        },
                        false, false, 32);
                })
                .build();
            const auto item = [&](int i, float width) {
                checkOption(u, "chart.select." + std::to_string(i),
                            msa::charts::label(msa::charts::all_kinds()[i]),
                            state.draft.charts.included[i], width, [i](bool value) {
                                state.draft.charts.included[i] = value;
                                state.outputChanged();
                            });
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
        u.row("chart.directory.actions")
            .width(c)
            .gap(12)
            .content([&] {
                const float width = std::min(156.f, (c - 12) / 2);
                button(u, "chart.file.options", "目录与文件名", width, openFileOptions);
                button(u, "chart.last.folder", "上次导出目录", width, openLastChartDirectory,
                       false, visual.lastExportPath.empty());
            })
            .build();
        u.text("chart.output.note")
            .size(c, 44)
            .text(std::string(state.draft.silentCharts ? "静默导出时，同名文件会追加序号。\n"
                                                       : "另存为时可选择目录和文件名。\n") +
                  (state.draft.charts.format == 3 ? "SVG 保留矢量线条、数据点和文字。"
                   : state.draft.charts.format == 2 ? "PDF 保存为单页图像。"
                   : combination      ? "所选图表保存为一张合成图片。"
                                      : "目录留空时，图片保存在 Windows 桌面。"))
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
        const auto actions = [&](float width) {
            u.row("charts.dataset.actions")
                .width(width)
                .gap(12)
                .content([&] {
                    const float bw = std::min(188.f, (width - 12) / 2);
                    button(u, "charts.dataset.reanalyze", "按当前设置重算", bw, reanalyzeInsightData,
                           false, state.busy() || !visual.importedSelected || !visual.imported);
                    button(u, "charts.dataset.report",
                           visual.busy() && visual.exportingReport ? "报告导出中…" : "导出方法对比报告",
                           bw, exportComparisonReport, false,
                           visual.busy() || state.busy() || !insightResult());
                })
                .build();
        };
        if (c >= 910) {
            constexpr float actionWidth = 328, controlWidth = 340, gap = 16;
            u.row("charts.source.toolbar")
                .width(c)
                .gap(gap)
                .alignItems(eui::Align::CENTER)
                .content([&] {
                    summary(c - actionWidth - controlWidth - 2 * gap);
                    actions(actionWidth);
                    insightSourceControls(u, "charts.dataset", controlWidth);
                })
                .build();
            return;
        }
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
        actions(c);
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
            if (chartEdits().editDirty[visual.selected])
                badge(u, "charts.preview.state",
                      !visual.editing ? "草稿待应用"
                      : !visual.preview.draftError.empty() ? "显示已应用" : "草稿预览", draftWidth,
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
        visual.importedSelected ? "选择测量数据文件后，可查看该研究的图表。"
        : state.busy()          ? "生成完成后，可在这里查看本次研究的图表。"
        : state.result          ? "按当前研究设置重新生成数据，或导入数据查看已有研究。"
                                : "在「数据」页生成测量值，或导入数据查看已有研究。";
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
        .content([h, &u](Ui &v, float cw, float) {
            v.column("charts.content")
                .width(cw)
                .gap(sectionGap)
                .padding(0, 0, 0, sectionGap)
                .content([&] {
                    chartSourceCard(v, cw);
                    const bool sourceError = visual.errorKind == "import" ||
                                             visual.errorKind == "reanalysis" ||
                                             visual.errorKind == "report";
                    if (sourceError)
                        visualRecovery(v, cw);
                    const float plotHeight = std::clamp(h - 226.f, 304.f, 528.f);
                    const auto *result = insightResult();
                    const bool sideBySide = visual.editing && cw >= 950 && result;
                    auto preview = [&](float width) {
                        section(v, "charts.preview", "", width, [&](float c) {
                            const auto *scene = result ? chartPreview(*result, c, plotHeight) : nullptr;
                            chartPreviewToolbar(v, c);
                            if (result) {
                                if (scene) {
                                    // The scroll view first builds a separate tree to measure its
                                    // contents. Only the fixed-size plot frame affects that layout.
                                    if (&v == &u)
                                        drawChartScene(v, "charts.scene." + std::to_string(visual.selected),
                                                       *scene);
                                    else
                                        v.stack("charts.measure").size(scene->width, scene->height).build();
                                } else if (sideBySide)
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
                    if (sideBySide)
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
                    if (!sourceError)
                        visualRecovery(v, cw);
                })
                .build();
        })
        .build();
}
