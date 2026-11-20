// Exercise the actual presentation without starting its window/event loop.
// gui.cpp has no main; including it keeps UI internals out of the public API.
#include "../src/gui.cpp"
#include "core/dsl_runtime.h"
#include "core/app/app_runner.h"
#include <iostream>
#include <limits>
#include <map>
#include <tuple>

// There is no window loop to wake in this layout-only executable.
namespace app {
void requestUpdate() {}
} // namespace app

namespace {
struct Snapshot {
    std::map<std::string, eui::Rect> frames;
    float opacity = 0, offset = 0;
    bool generatePresent = false, exportEnabled = false;
    std::string value;
};

Snapshot capture(gui::Status status, float w, float h) {
    gui::state.status = status;
    eui::Ui ui;
    ui.begin("layout-test");
    ui.column("host")
        .width(w)
        .content([&] {
            gui::dataPage(ui, w, h);
            ui.row("footer")
                .size(w, 76)
                .gap(12)
                .justifyContent(eui::Align::END)
                .content([&] { gui::footer(ui, w); })
                .build();
        })
        .build();
    ui.end();
    ui.layout(w, h + 76);
    Snapshot result;
    for (const auto *id : {"inputcolumn", "rules.card", "targets.card", "results.card",
                           "resulttabs", "result.content"}) {
        const auto *node = ui.find(id);
        if (!node)
            throw std::runtime_error(std::string("missing UI anchor: ") + id);
        result.frames[id] = {node->frame.x, node->frame.y, node->frame.width, node->frame.height};
    }
    if (ui.find("result.caution") || ui.find("study.process.note"))
        throw std::runtime_error("结果页仍保留已移除的底部说明");
    const auto *pagination = ui.find("preview.pagination");
    if (pagination) {
        const auto &frame = pagination->frame;
        result.frames["preview.pagination"] = {frame.x, frame.y, frame.width, frame.height};
    }
    if (ui.find("split")) {
        const auto &targets = result.frames.at("targets.card");
        const auto &results = result.frames.at("results.card");
        if (std::fabs(targets.y + targets.height - results.y - results.height) > .1f)
            throw std::runtime_error("桌面数据页两栏底边未对齐");
        if (gui::state.resultTab == 1 && gui::state.displayedResult()) {
            const auto *scroll = ui.find("result.scroll");
            const auto &content = result.frames.at("result.content");
            if (!scroll || !ui.find("previewrow-1.wrap") ||
                scroll->frame.y + scroll->frame.height > content.y + content.height + .1f ||
                (pagination && pagination->frame.y < content.y + content.height - .1f))
                throw std::runtime_error("数据预览超出复核区域");
        }
    } else if (ui.find("result.scroll")) {
        throw std::runtime_error("窄窗口的复核结果应随数据页滚动");
    }
    result.opacity = ui.find("inputcolumn")->opacity;
    result.offset = ui.find("generatescroll")->scrollOffset;
    if (const auto *generate = ui.find("generate")) {
        result.generatePresent = true;
        result.frames["generate"] = {generate->frame.x, generate->frame.y, generate->frame.width,
                                     generate->frame.height};
    }
    const auto *action = ui.find("task.secondary");
    if (!action)
        action = ui.find("export");
    if (!action)
        action = ui.find("cancel");
    if (action)
        result.frames["secondary"] = {action->frame.x, action->frame.y, action->frame.width,
                                      action->frame.height};
    if (const auto *exportButton = ui.find("export.bg"))
        result.exportEnabled = !exportButton->disabled;
    if (const auto *value = ui.find("review0c3"))
        result.value = value->text;
    return result;
}

void equal(const Snapshot &before, const Snapshot &after, const std::string &stage) {
    if (after.opacity != 1)
        throw std::runtime_error(stage + ": generation dims the whole input panel");
    if (!before.generatePresent || !after.generatePresent)
        throw std::runtime_error(stage + ": generation removes/repositions the primary action");
    for (const auto &[id, a] : before.frames) {
        const auto &b = after.frames.at(id);
        if (std::fabs(a.x - b.x) > .01f || std::fabs(a.y - b.y) > .01f ||
            std::fabs(a.width - b.width) > .01f || std::fabs(a.height - b.height) > .01f)
            throw std::runtime_error(stage + ": layout changed at " + id);
    }
    if (before.offset != after.offset)
        throw std::runtime_error(stage + ": scroll position changed");
}

void controls_fit(const core::dsl::Element &node, float left, float right,
                  const std::string &stage) {
    // 检查真实可操作区域，允许滚动容器中被裁切的长文本保留自身宽度。
    if (node.interactive || node.focusable || node.onTextInput || node.onPress || node.onClick) {
        const auto &frame = node.frame;
        if (!std::isfinite(frame.x) || !std::isfinite(frame.width) || frame.width < 0 ||
            frame.x < left - .1f || frame.x + frame.width > right + .1f)
            throw std::runtime_error(stage + ": control exceeds horizontal bounds: " + node.id +
                                     " x=" + std::to_string(frame.x) +
                                     " width=" + std::to_string(frame.width));
    }
    for (const auto &child : node.children)
        controls_fit(*child, left, right, stage);
}

void controls_fit(const eui::Ui &ui, float width, const std::string &stage) {
    for (const auto &root : ui.roots())
        controls_fit(*root, 0, width, stage);
}

void click(eui::Ui &ui, const std::string &id) {
    const auto *control = ui.find(id);
    if (!control || control->disabled || !control->onClick)
        throw std::runtime_error("missing enabled click target: " + id);
    control->onClick();
}

bool contains_element(const core::dsl::Element &node, const core::dsl::Element &target) {
    // Ui::find 会解析页面命名空间；比较实际节点地址，避免把局部 ID 与完整 ID 混用。
    if (&node == &target)
        return true;
    return std::any_of(node.children.begin(), node.children.end(),
                       [&](const auto &child) { return contains_element(*child, target); });
}

void data_settings_layout() {
    const auto offset = gui::state.generateOffset;
    gui::state.generateOffset = 0;
    for (float width : {588.f, 1020.f, 1100.f, 1232.f}) {
        constexpr float height = 500;
        eui::Ui ui;
        ui.begin("data-settings-layout");
        gui::dataPage(ui, width, height);
        ui.end();
        ui.layout(width, height);
        const auto *settings = ui.find("rules.card");
        const auto *targets = ui.find("targets.card");
        if (!settings || !targets || ui.find("priority") || ui.find("compliance.hit") ||
            ui.find("generation.rules.card"))
            throw std::runtime_error("数据页生成设置分组错误");
        if (settings->frame.y + settings->frame.height + 12 > targets->frame.y + .1f)
            throw std::runtime_error("生成设置和指标约束之间的间距不足");
        controls_fit(*settings, settings->frame.x, settings->frame.x + settings->frame.width,
                     "数据页生成设置");
        std::array<core::LayoutRect, 3> frames{};
        const std::array<const char *, 3> ids{"center.hit", "lsl.hit", "usl.hit"};
        for (std::size_t i = 0; i < ids.size(); ++i) {
            const auto *field = ui.find(ids[i]);
            if (!field || !contains_element(*settings, *field))
                throw std::runtime_error(std::string("数据页生成设置缺少字段：") + ids[i]);
            const auto &frame = field->frame;
            const auto bottom = frame.y + frame.height;
            if (frame.y < settings->frame.y - .1f ||
                bottom > settings->frame.y + settings->frame.height + .1f || bottom > height)
                throw std::runtime_error(std::string("数据页生成设置字段超出可见范围：") + ids[i]);
            for (std::size_t j = 0; j < i; ++j) {
                const auto &other = frames[j];
                if (frame.x < other.x + other.width && other.x < frame.x + frame.width &&
                    frame.y < other.y + other.height && other.y < bottom)
                    throw std::runtime_error("数据页生成设置字段重叠");
            }
            frames[i] = frame;
        }
        const auto &lsl = frames[1];
        const auto &usl = frames[2];
        if (lsl.y > usl.y + .1f || (std::fabs(lsl.y - usl.y) <= .1f && lsl.x >= usl.x))
            throw std::runtime_error("数据页规格限的阅读顺序错误");
    }
    gui::state.generateOffset = offset;

    const auto previousStatus = gui::state.status;
    const auto previousMessage = gui::state.message;
    const auto previousPage = gui::state.page;
    gui::state.status = gui::Status::SearchFailed;
    gui::state.message = "搜索预算已用完，请检查指标约束或调整搜索预算。";
    gui::state.page = gui::Page::Data;
    for (const float width : {588.f, 1232.f}) {
        eui::Ui ui;
        ui.begin("responsive-notice");
        gui::notice(ui, width);
        ui.end();
        ui.layout(width, 200);
        controls_fit(ui, width, "failure notice");
        const auto *message = ui.find("notice.text");
        const auto *actions = ui.find("notice.actions");
        if (!message || !actions || !ui.find("fixoptions.bg") || !ui.find("showlogs.bg"))
            throw std::runtime_error("failure notice loses its message or recovery actions");
        if (width < 680 ? actions->frame.y < message->frame.y + message->frame.height
                        : actions->frame.x < message->frame.x + message->frame.width)
            throw std::runtime_error("failure notice covers the message with the recovery actions");
    }
    gui::state.status = previousStatus;
    gui::state.message = previousMessage;
    gui::state.page = previousPage;
}

void options_grouping(const eui::Ui &ui, int tab) {
    const auto node = [&](const std::string &id) -> const core::dsl::Element & {
        const auto *found = ui.find(id);
        if (!found)
            throw std::runtime_error("选项分组缺少控件：" + id);
        return *found;
    };
    const auto group = [&](const char *card, std::initializer_list<const char *> controls) {
        const auto &container = node(card);
        for (const auto *id : controls)
            if (!contains_element(container, node(id)))
                throw std::runtime_error(std::string("选项归类错误：") + id + " 不在 " + card);
    };
    const auto aligned = [&](const char *first, const char *second) {
        const auto &left = node(first).frame;
        const auto &right = node(second).frame;
        if (std::abs(left.y - right.y) > .1f || left.x + left.width + 8 > right.x + .1f)
            throw std::runtime_error(std::string("选项控件未按同一行对齐：") + first + " / " + second);
    };
    const std::array<std::vector<const char *>, 3> groups{{
        {"design.card", "generation.rules.card", "generation.card", "advanced.card"},
        {"analysis.card", "analysis.evaluation.card", "analysis.process.card",
         "analysis.statistics.card"},
        {"output.files.card", "export.preferences.card", "output.card", "runorder.card"},
    }};
    for (int category = 0; category < 3; ++category)
        for (const auto *id : groups[category])
            if (bool(ui.find(id)) != (category == tab))
                throw std::runtime_error(std::string("选项业务分组跨页或丢失：") + id);
    if (ui.find("precision.card"))
        throw std::runtime_error("旧的精度分组未合并到生成参数");
    if (ui.find("center.hit") || ui.find("lsl.hit") || ui.find("usl.hit") || ui.find("spec.card"))
        throw std::runtime_error("数据中心或规格范围仍出现在选项页");
    if (tab != 0 &&
        (ui.find("priority") || ui.find("compliance.hit") || ui.find("normality.hit") ||
         ui.find("part.sampling") || ui.find("part.span.hit")))
        throw std::runtime_error("生成规则控件出现在非研究选项中");
    if (tab == 0) {
        group("design.card", {"operators", "replicates", "parts", "operator.name.1",
                              "operator.name.2", "operator.name.3"});
        aligned("operator.name.1", "operator.name.2");
        aligned("operator.name.2", "operator.name.3");
        const auto &summary = node("design.summary").frame;
        if (node("operator.names").frame.y < summary.y + summary.height)
            throw std::runtime_error("默认操作员设置覆盖了研究设计记录数");
        group("generation.rules.card", {"priority", "compliance.hit", "normality.hit",
                                        "normality.note", "part.sampling", "part.sampling.note"});
        aligned("priority", "compliance.hit");
        group("generation.card", {"decimals", "scale", "seed.hit", "part.span.hit", "part.span.note"});
        const bool reference = gui::state.draft.scale == 1 ||
                               native_ui::trim(gui::state.draft.lsl).empty() ||
                               native_ui::trim(gui::state.draft.usl).empty();
        if (reference) {
            group("generation.card", {"reference.hit"});
            aligned("reference.hit", "seed.hit");
        } else if (ui.find("reference.hit"))
            throw std::runtime_error("自动双侧规格模式出现不适用的参考标准差输入");
        group("advanced.card", {"percenterror.hit", "ndcerror.hit", "advancedtoggle.bg"});
        if (gui::state.advanced)
            group("advanced.card", {"attempts.hit", "timeout.hit"});
        else if (ui.find("attempts.hit") || ui.find("timeout.hit"))
            throw std::runtime_error("搜索预算收起时仍占用字段空间");
        if (ui.find("multiplier.hit") || ui.find("analysis.tolerance.hit"))
            throw std::runtime_error("统计评价口径仍混在研究参数中");
    } else if (tab == 1) {
        group("analysis.card", {"analysis.method"});
        if (gui::state.draft.analysisMethod == 1)
            aligned("analysis.interaction", "analysis.alpha.hit");
        group("analysis.evaluation.card", {"multiplier.hit", "analysis.tolerance.hit"});
        group("analysis.process.card", {"analysis.historical.sd.hit",
                                        "analysis.historical.mean.hit", "analysis.process.source"});
        aligned("analysis.historical.sd.hit", "analysis.historical.mean.hit");
        aligned("analysis.historical.mean.hit", "analysis.process.source");
        group("analysis.statistics.card", {"analysis.misclassification.control",
                                           "analysis.confidence.control"});
        if (gui::state.draft.confidenceEnabled)
            group("analysis.statistics.card", {"analysis.confidence.method",
                                               "analysis.confidence.level.hit"});
        else if (ui.find("analysis.confidence.method") || ui.find("analysis.confidence.level.hit"))
            throw std::runtime_error("置信区间关闭时仍占用字段空间");
        aligned("analysis.misclassification.control", "analysis.confidence.control");
    } else {
        group("output.card", {"csvformat", "runorder.standard.hit"});
        group("output.files.card", {"import.directory.hit", "import.filename.hit", "directory.hit",
                                    "filename.hit", "collision", "chart.directory.hit",
                                    "chart.filename.hit", "report.directory.hit", "report.filename.hit",
                                    "import.directory.browse.bg", "directory.browse.bg",
                                    "chart.directory.browse.bg", "report.directory.browse.bg"});
        group("export.preferences.card", {"silent.csv.hit", "silent.charts.hit", "silent.report.hit",
                                          "report.format", "data.filetype"});
        aligned("data.filetype.label", "report.format.label");
        aligned("data.filetype", "report.format");
        for (const auto id : {"silent.csv.label", "silent.charts.label", "silent.report.label",
                              "report.format.label", "data.filetype.label", "report.directory.label", "import.directory.label",
                              "import.filename.label", "directory.label", "filename.label",
                              "chart.directory.label", "chart.filename.label", "report.filename.label"}) {
            const auto &label = node(id);
            if (core::TextPrimitive::measureTextWidth(label.text, label.fontFamily, label.fontSize,
                                                      label.fontWeight) > label.frame.width)
                throw std::runtime_error("导出选项标签显示不全：" + label.text);
        }
        group("runorder.card", {"runorder.mode.0.hit", "runorder.mode.1.hit",
                                "runorder.mode.2.hit", "runorder.operators.hit",
                                "runorder.seed.hit"});
        aligned("runorder.mode.0.hit", "runorder.mode.1.hit");
        aligned("runorder.mode.1.hit", "runorder.mode.2.hit");
        if (contains_element(node("runorder.card"), node("runorder.standard.hit")))
            throw std::runtime_error("附加标准序号列仍混在运行顺序中");
        if (node("runorder.standard.control").disabled != (gui::state.draft.csvFormat == 0))
            throw std::runtime_error("标准序号列的可用性未随宽表/长表格式更新");
    }
    group("configpath.card", {"path", "copypath.bg", "opendir.bg"});
    const auto &content = node("configcontent").frame;
    const auto &management = node("configpath.card").frame;
    if (content.width >= 1100) {
        if (tab == 0) {
            aligned("designrow", "generation.rules.values");
            aligned("precisionrow", "target.defaults");
        }
        const auto pair = [&](const char *leftId, const char *rightId) {
            const auto &left = node(leftId).frame;
            const auto &right = node(rightId).frame;
            if (left.x >= right.x || std::fabs(left.width - right.width) > .1f ||
                std::fabs(left.y - right.y) > .1f ||
                std::fabs(left.y + left.height - right.y - right.height) > .1f)
                throw std::runtime_error(std::string("桌面选项卡片顶边或底边未对齐：") +
                                         leftId + " / " + rightId);
        };
        if (tab == 0) {
            pair("design.card", "generation.rules.card");
            pair("generation.card", "advanced.card");
        } else if (tab == 1) {
            pair("analysis.card", "analysis.process.card");
            pair("analysis.evaluation.card", "analysis.statistics.card");
        } else {
            pair("output.card", "runorder.card");
        }
    } else {
        float end = content.y;
        for (const auto *id : groups[tab]) {
            const auto &card = node(id).frame;
            if (std::fabs(card.x - content.x) > .1f ||
                std::fabs(card.width - content.width) > .1f || card.y < end - .1f)
                throw std::runtime_error(std::string("窄窗口选项未保持单列顺序：") + id);
            end = card.y + card.height;
        }
    }
    if (std::fabs(management.x - content.x) > .1f ||
        std::fabs(management.width - content.width) > .1f)
        throw std::runtime_error("共享配置管理未保持全宽对齐");
    for (const auto *id : groups[tab]) {
        const auto &card = node(id);
        if (contains_element(card, node("configpath.card")) ||
            management.y < card.frame.y + card.frame.height + 12)
            throw std::runtime_error("共享配置管理归类错误或与业务卡片重叠");
    }
    // 任意卡片组合必须具有清晰间隔，涵盖双列到单列的真实布局切换。
    for (std::size_t a = 0; a < groups[tab].size(); ++a)
        for (std::size_t b = a + 1; b < groups[tab].size(); ++b) {
            const auto &left = node(groups[tab][a]).frame;
            const auto &right = node(groups[tab][b]).frame;
            const bool separated = left.x + left.width + 12 <= right.x ||
                                   right.x + right.width + 12 <= left.x ||
                                   left.y + left.height + 12 <= right.y ||
                                   right.y + right.height + 12 <= left.y;
            if (!separated)
                throw std::runtime_error("选项卡片重叠或间距不足");
        }
}

void xbar_details(const msa::Result &generated) {
    if (!generated.metrics.contains("study"))
        throw std::runtime_error("Xbar-R fixture lacks study statistics");
    gui::state.status = gui::Status::Success;
    gui::state.draft.analysisMethod = 0;
    gui::state.result = generated;
    gui::state.resultRevision = gui::state.revision;
    gui::state.resultTab = 0;
    eui::Ui selection;
    selection.begin("xbarr-details-selection");
    gui::results(selection, 588);
    selection.end();
    selection.layout(588, 700);
    click(selection, "resulttabs.hit.2");
    for (float width : {588.f, 1020.f, 1232.f}) {
        eui::Ui details;
        details.begin("xbarr-details-result");
        gui::results(details, width);
        details.end();
        details.layout(width, 700);
        const auto *grr = details.find("study.table.cell.2.1");
        const auto expected =
            native_ui::review_number(generated.metrics["study"]["components"]["GRR"]["sd"], 6);
        if (gui::state.resultTab != 2 || !grr || grr->text.find(expected) == std::string::npos ||
            details.find("anova.pending"))
            throw std::runtime_error("Xbar-R details tab resets or loses study results");
        const auto *sampling = details.find("search.sampling");
        const auto percent = native_ui::review_number(generated.search_diagnostics.at("part_mean_span_pct"), 2);
        if (!sampling || sampling->text.find("部件取样：默认取样。") == std::string::npos ||
            sampling->text.find(percent + "%") == std::string::npos)
            throw std::runtime_error("生成详情未显示该研究的取样方案和实际跨度占比");
        controls_fit(details, width, "Xbar-R details");
    }
    gui::state.resultTab = 0;
    gui::state.resultEntered = gui::Clock::now() - std::chrono::seconds(1);
    std::cout << "Xbar-R analysis details retain the selected tab and real study statistics\n";
}

void result_table_layout(const msa::Result &generated) {
    const auto oldResult = gui::state.result;
    const auto oldResultRevision = gui::state.resultRevision;
    const auto oldStatus = gui::state.status;
    const auto oldTab = gui::state.resultTab;
    const auto oldPage = gui::state.previewPage;
    const auto oldGenerateOffset = gui::state.generateOffset;
    const auto oldResultOffset = gui::state.resultOffset;
    gui::state.result = generated;
    gui::state.resultRevision = gui::state.revision;
    gui::state.status = gui::Status::Success;
    gui::state.generateOffset = gui::state.resultOffset = 0;
    const auto rowsEnd = [](const eui::Ui &ui, const std::string &prefix, const char *suffix,
                            std::size_t count) {
        const auto *first = ui.find(prefix + "0" + suffix);
        if (!first || first->frame.height <= 0)
            throw std::runtime_error("结果表缺少首行：" + prefix);
        const float height = first->frame.height;
        float end = first->frame.y;
        for (std::size_t i = 0; i < count; ++i) {
            const auto *row = ui.find(prefix + std::to_string(i) + suffix);
            if (!row || std::fabs(row->frame.height - height) > .1f ||
                std::fabs(row->frame.y - end) > .1f)
                throw std::runtime_error("结果表各行未等高连续排列：" + prefix);
            end = row->frame.y + row->frame.height;
        }
        return end;
    };
    const auto fillsResults = [](const eui::Ui &ui, float tableEnd, bool scrollable) {
        const auto *content = ui.find("result.content");
        if (!content)
            throw std::runtime_error("缺少结果表区域");
        const float spare = content->frame.y + content->frame.height - tableEnd;
        if (spare > .1f || (!scrollable && spare < -.1f))
            throw std::runtime_error("结果表未铺满桌面结果区域");
    };
    bool testedNarrowPaging = false, testedDesktopPaging = false;
    const auto lastPartHeader = "cell-1_" + std::to_string(generated.matrix.front().size() - 1);
    for (float width : {588.f, 1100.f, 1232.f}) {
        const float height = width < 1100 ? 332.f : 636.f;
        const auto compose = [&](eui::Ui &ui, int tab) {
            gui::state.resultTab = tab;
            ui.begin("data-result-table-layout");
            gui::dataPage(ui, width, height);
            ui.end();
            ui.layout(width, height);
        };
        eui::Ui review, preview, details;
        gui::state.previewPage = 0;
        compose(review, 0);
        compose(preview, 1);
        compose(details, 2);
        const auto *reviewHeader = review.find("reviewheader.wrap");
        const auto *previewHeader = preview.find("previewrow-1.wrap");
        const auto *detailsTable = details.find("study.table");
        const auto *detailsHeader = details.find("study.table.header.cap");
        if (!previewHeader || !detailsTable || !detailsHeader ||
            std::fabs(previewHeader->frame.x - detailsTable->frame.x) > .1f ||
            std::fabs(previewHeader->frame.y - detailsTable->frame.y) > .1f ||
            std::fabs(previewHeader->frame.width - detailsTable->frame.width) > .1f ||
            std::fabs(previewHeader->frame.height - 40.f) > .1f ||
            std::fabs(detailsHeader->frame.y - detailsTable->frame.y - 1.f) > .1f ||
            std::fabs(previewHeader->frame.height - detailsHeader->frame.height - 1.f) > .1f ||
            preview.find("previewinfo") || preview.find("csvhint") || details.find("study.basis") ||
            review.find("result.caution") || preview.find("result.caution") ||
            details.find("result.caution") || details.find("study.process.note"))
            throw std::runtime_error("结果页首张表格未对齐或仍有多余摘要");
        if (width >= 1100) {
            if (!reviewHeader || std::fabs(reviewHeader->frame.x - previewHeader->frame.x) > .1f ||
                std::fabs(reviewHeader->frame.y - previewHeader->frame.y) > .1f ||
                std::fabs(reviewHeader->frame.width - previewHeader->frame.width) > .1f ||
                std::fabs(reviewHeader->frame.height - previewHeader->frame.height) > .1f)
                throw std::runtime_error("桌面结果页的三个表头未对齐");
            fillsResults(review, rowsEnd(review, "review", ".wrap", 5), false);
            fillsResults(preview, rowsEnd(preview, "previewrow", ".wrap", generated.matrix.size()),
                         true);
            fillsResults(details, rowsEnd(details, "study.table.row.", "", 5) + 1, true);
        }

        const auto *pagination = preview.find("preview.pagination");
        const auto *previous = preview.find("prevcols.bg");
        if (preview.find(lastPartHeader)) {
            if (pagination || previous || preview.find("nextcols.bg"))
                throw std::runtime_error("部件可一次显示时仍出现翻页按钮");
            continue;
        }
        const auto *lastRow = preview.find("previewrow" + std::to_string(generated.matrix.size() - 1) +
                                           ".wrap");
        const auto *scroller = preview.find("result.scroll");
        if (!pagination || !previous || !previous->disabled || !lastRow)
            throw std::runtime_error("多组部件缺少分页或首页允许继续向前翻页");
        if (width >= 1100 && (!scroller || contains_element(*scroller, *pagination)))
            throw std::runtime_error("桌面预览的分页控件未固定在表格滚动区域外");
        const auto &body = scroller ? preview.find("result.content")->frame : lastRow->frame;
        if (pagination->frame.y < body.y + body.height - .1f)
            throw std::runtime_error("数据预览分页覆盖表格或出现在表格上方");
        gui::state.resultTab = 1;
        click(preview, "nextcols.bg");
        eui::Ui next;
        compose(next, 1);
        const auto *part = next.find("cell-1_0");
        const auto *measurement = next.find("cell0_0");
        const int index = part ? std::stoi(part->text) - 1 : -1;
        const int decimals = generated.config["generation"]["decimal_places"];
        if (gui::state.previewPage != 1 || index <= 0 ||
            index >= int(generated.matrix.front().size()) || !measurement ||
            measurement->text != msa::fixed(generated.matrix.front()[index], decimals))
            throw std::runtime_error("翻页后未显示下一组部件的真实测量值");
        click(next, "prevcols.bg");
        click(next, "prevcols.bg");
        if (gui::state.previewPage != 0)
            throw std::runtime_error("连续返回上一组后未停在第一页");
        const int lastPage = (int(generated.matrix.front().size()) - 1) / index;
        for (int step = 0; step <= lastPage; ++step)
            click(preview, "nextcols.bg");
        if (gui::state.previewPage != lastPage)
            throw std::runtime_error("连续进入下一组后未停在最后一页");
        eui::Ui last;
        compose(last, 1);
        const auto *lastMeasurement = last.find("cell0_0");
        if (!lastMeasurement ||
            lastMeasurement->text !=
                msa::fixed(generated.matrix.front()[lastPage * index], decimals))
            throw std::runtime_error("最后一页未渲染有效部件的测量值");
        testedDesktopPaging |= scroller != nullptr;
        testedNarrowPaging |= scroller == nullptr;
    }
    if (!testedNarrowPaging || !testedDesktopPaging)
        throw std::runtime_error("数据预览回归未覆盖窄窗口和桌面窗口的分页场景");
    // Five parts fit within the desktop panel even with its independent scroll viewport.
    gui::state.result->config["design"]["parts"] = 5;
    for (auto &row : gui::state.result->matrix)
        row.resize(5);
    gui::state.resultTab = 1;
    gui::state.previewPage = 1;
    eui::Ui wide;
    wide.begin("data-result-unpaged-preview");
    gui::dataPage(wide, 1232, 636);
    wide.end();
    wide.layout(1232, 636);
    if (gui::state.previewPage != 0 || !wide.find("cell-1_4") || wide.find("preview.pagination") ||
        wide.find("prevcols.bg") || wide.find("nextcols.bg"))
        throw std::runtime_error("部件可一次显示时仍在数据预览中占用分页空间");
    gui::state.result->config["design"]["operator_names"] = {
        "检测一班", "操作员的长名称需要截断以便显示所有测量轮次和数据列", ""};
    const int reps = gui::state.result->config["design"]["replicates"];
    eui::Ui named;
    named.begin("named-preview");
    gui::preview(named, 320, gui::previewColumns(*gui::state.result, 320, false), 0);
    named.end();
    named.layout(320, 500);
    const auto &shortName = named.find("rowlabel0")->text;
    const auto &longName = named.find("rowlabel" + std::to_string(reps))->text;
    if (shortName != "检测一班 / 1" || longName.find("… / 1") == std::string::npos ||
        !named.find("cell0_0"))
        throw std::runtime_error("预览中的操作员名称或轮次显示不完整：" + shortName + "、" + longName);
    gui::state.result = oldResult;
    gui::state.resultRevision = oldResultRevision;
    gui::state.status = oldStatus;
    gui::state.resultTab = oldTab;
    gui::state.previewPage = oldPage;
    gui::state.generateOffset = oldGenerateOffset;
    gui::state.resultOffset = oldResultOffset;
    std::cout << "结果页首表对齐、各行等高并铺满，同时保留按需分页和真实测量值\n";
}

void initial_window_bounds() {
    // 覆盖常见工作区及高 DPI：返回客户区连同非客户区和边距必须完全可见。
    for (const auto work :
         {std::pair<int, int>{1366, 728}, {1920, 1040}, {2560, 1400}, {1024, 720}})
        for (float scale : {1.f, 1.25f, 1.5f, 2.f}) {
            const int frameWidth = int(16 * scale), frameHeight = int(39 * scale);
            const auto size = native_ui::initial_window_size(work.first, work.second, scale,
                                                             frameWidth, frameHeight);
            if (size.first <= 0 || size.second <= 0 || size.first + frameWidth + 32 > work.first ||
                size.second + frameHeight + 32 > work.second)
                throw std::runtime_error("initial window extends beyond available work area");
        }
    const auto desktop = native_ui::initial_window_size(1920, 1040, 1, 16, 39);
    const auto hidpi = native_ui::initial_window_size(3200, 2000, 2, 32, 78);
    if (desktop != std::pair<int, int>{1280, 820} || hidpi != std::pair<int, int>{2560, 1640})
        throw std::runtime_error("initial window does not preserve its logical default size");
    std::cout << "initial window fits common work areas at 100/125/150/200 percent scaling\n";
}

void options_navigation(const msa::Result &generated) {
    const auto draft = gui::state.draft;
    gui::state.status = gui::Status::Success;
    gui::state.result = generated;
    gui::state.resultRevision = gui::state.revision;
    gui::state.resultTab = 1;
    const auto revision = gui::state.revision;
    const std::array<const char *, 3> anchors{"design.card", "analysis.card", "output.card"};
    for (float width : {588.f, 1020.f, 1100.f, 1132.f, 1232.f})
        for (int method : {0, 1}) {
            gui::state.draft.analysisMethod = method;
            gui::state.draft.scale = method;
            gui::state.draft.csvFormat = method;
            gui::state.draft.confidenceEnabled = method == 1;
            gui::state.advanced = method == 1;
            gui::state.advancedChanged = gui::Clock::now() - std::chrono::seconds(1);
            for (int tab = 0; tab < 3; ++tab) {
                gui::state.optionsTab = (tab + 2) % 3;
                gui::state.configOffset = 80;
                eui::Ui select;
                select.begin("options-navigation");
                gui::optionsPage(select, width, 500);
                select.end();
                select.layout(width, 500);
                click(select, "options.tabs.hit." + std::to_string(tab));
                if (gui::state.optionsTab != tab || gui::state.configOffset != 0 ||
                    !gui::state.validResult() || gui::state.revision != revision ||
                    gui::state.resultTab != 1)
                    throw std::runtime_error(
                        "options navigation invalidates results or keeps old scroll");
                eui::Ui page;
                page.begin("options-content");
                gui::optionsPage(page, width, 500);
                page.end();
                page.layout(width, 500);
                controls_fit(page, width, "options category " + std::to_string(tab));
                options_grouping(page, tab);
                for (int other = 0; other < 3; ++other)
                    if (bool(page.find(anchors[other])) != (other == tab))
                        throw std::runtime_error(
                            "options categories overlap or omit their controls");
                const auto *navigation = page.find("options.tabs");
                const auto *scroll = page.find("configscroll");
                const auto *toolbar = page.find("options.navigation");
                const auto *form = page.find("configcontent");
                if (!navigation || !scroll || !toolbar || !form ||
                    scroll->frame.y < navigation->frame.y + navigation->frame.height)
                    throw std::runtime_error("options content overlaps the fixed navigation");
                if (std::fabs(toolbar->frame.x - form->frame.x) > .1f ||
                    std::fabs(toolbar->frame.width - form->frame.width) > .1f)
                    throw std::runtime_error("options navigation and form cards are misaligned");
                if (tab == 0) {
                    const auto *records = page.find("design.records.text");
                    const auto total = std::atoi(gui::state.draft.operators.c_str()) *
                                       std::atoi(gui::state.draft.replicates.c_str()) *
                                       std::atoi(gui::state.draft.parts.c_str());
                    if (!records || records->text.find(std::to_string(total)) == std::string::npos)
                        throw std::runtime_error("研究设计没有展示正确的总测量记录数");
                }
                if (scroll->scrollMaxOffset > 0) {
                    const auto navFrame = navigation->frame;
                    if (!scroll->onScrollOffsetChanged)
                        throw std::runtime_error("options scroll lacks a callback");
                    scroll->onScrollOffsetChanged(std::min(80.f, scroll->scrollMaxOffset));
                    eui::Ui scrolled;
                    scrolled.begin("scrolled-options-content");
                    gui::optionsPage(scrolled, width, 500);
                    scrolled.end();
                    scrolled.layout(width, 500);
                    const auto *fixed = scrolled.find("options.tabs");
                    if (!fixed || fixed->frame.x != navFrame.x || fixed->frame.y != navFrame.y ||
                        fixed->frame.width != navFrame.width ||
                        fixed->frame.height != navFrame.height)
                        throw std::runtime_error("options navigation moves with scrolling");
                }
                if (tab == 2) {
                    const auto *format = page.find("csvformat");
                    if (!format || !format->onPress)
                        throw std::runtime_error("output category lacks CSV format selection");
                    format->onPress({}, {});
                    gui::menu.select(1);
                    gui::menu.id.clear();
                    click(page, "collision.hit.1");
                    if (gui::state.draft.csvFormat != 1 || gui::state.draft.existingFile != 1 ||
                        !gui::state.validResult() || gui::state.revision != revision)
                        throw std::runtime_error(
                            "output controls invalidate results after category navigation");
                }
            }
        }
    gui::state.draft = draft;
    gui::state.optionsTab = 0;
    gui::state.configOffset = 0;
    gui::state.advanced = false;
    gui::state.advancedFrom = 0;
    gui::state.resultTab = 0;
    std::cout << "选项卡片在五种宽度及展开、收起状态下对齐，保留固定导航和原有设置操作\n";
}

void regrouped_settings_state(const msa::Result &generated) {
    const auto draft = gui::state.draft;
    const auto page = gui::state.page;
    const auto revision = gui::state.revision;
    const auto logs = gui::state.logs;
    const auto sequence = gui::state.logSequence;
    struct FieldCase {
        int tab;
        const char *id;
        std::string native_ui::Draft::*member;
        const char *value;
    };
    constexpr int dataTab = -1;
    const std::array<FieldCase, 10> fields{{
        {dataTab, "center", &native_ui::Draft::center, "10.25"},
        {dataTab, "lsl", &native_ui::Draft::lsl, "8.5"},
        {dataTab, "usl", &native_ui::Draft::usl, "11.5"},
        {0, "seed", &native_ui::Draft::seed, "22409"},
        {0, "reference", &native_ui::Draft::reference, "0.25"},
        {0, "part.span", &native_ui::Draft::minimumPartSpanPct, "80.5"},
        {0, "percenterror", &native_ui::Draft::percentError, "0.2"},
        {0, "ndcerror", &native_ui::Draft::ndcError, "1"},
        {1, "multiplier", &native_ui::Draft::multiplier, "5.15"},
        {1, "analysis.tolerance", &native_ui::Draft::analysisTolerance, "2.5"},
    }};
    const auto reset = [&](int tab) {
        gui::state.draft = draft;
        gui::state.draft.scale = 1;
        gui::state.status = gui::Status::Success;
        gui::state.page = tab == dataTab ? gui::Page::Data : gui::Page::Options;
        gui::state.message.clear();
        gui::state.footerMessage.clear();
        gui::state.result = generated;
        gui::state.revision = revision;
        gui::state.resultRevision = revision;
        gui::state.optionsTab = std::max(0, tab);
        gui::state.configOffset = 0;
        gui::state.generateOffset = 0;
        gui::state.configDirty = false;
    };
    const auto staleResult = [&] {
        return gui::state.revision == revision + 1 && !gui::state.validResult() &&
               gui::state.configDirty && gui::state.result &&
               gui::state.result->matrix == generated.matrix &&
               gui::headerStatusPresentation().label == "待重新生成";
    };
    const auto input = [](eui::Ui &ui, const std::string &id, const std::string &value) {
        auto *node = ui.find(id + ".hit");
        if (!node || !node->onKeyEvent || !node->onTextInput)
            throw std::runtime_error("重分组后缺少可编辑字段：" + id);
        eui::KeyEvent select;
        select.key = eui::InputKey::A;
        select.modifiers.control = true;
        node->onKeyEvent(select);
        eui::TextInputEvent text;
        text.text = value;
        node->onTextInput(text);
    };
    {
        reset(0);
        gui::state.draft.operators = "3";
        gui::state.draft.operatorNames = {};
        const auto compose = [](eui::Ui &ui) {
            ui.begin("operator-names");
            gui::designSettings(ui, 580);
            ui.end();
            ui.layout(580, 440);
        };
        eui::Ui initial;
        compose(initial);
        for (int i = 0; i < 3; ++i) {
            const auto id = "operator.name." + std::to_string(i + 1);
            if (initial.find(id + ".field")->disabled ||
                initial.find(id + ".text")->text != std::string(1, char('A' + i)))
                throw std::runtime_error("三人研究应开放全部名称并显示默认占位符");
        }
        input(initial, "operator.name.3", "陈师傅");
        if (!staleResult())
            throw std::runtime_error("编辑默认操作员没有标记研究设置已修改");
        click(initial, "operators.hit.0");
        eui::Ui two;
        compose(two);
        const auto *inactive = two.find("operator.name.3.field");
        if (!inactive->disabled || inactive->opacity >= 1 ||
            two.find("operator.name.1.field")->disabled || two.find("operator.name.2.field")->disabled)
            throw std::runtime_error("两人研究应仅灰显并禁用第三个名称");
        input(two, "operator.name.1", "张工");
        const auto saved = native_ui::to_config(gui::state.draft);
        gui::state.draft = native_ui::from_config(saved);
        if (gui::state.draft.operators != "2" || gui::state.draft.operatorNames[0] != "张工" ||
            gui::state.draft.operatorNames[2] != "陈师傅" || msa::operator_names(saved["design"])[1] != "B")
            throw std::runtime_error("配置重载应保留停用名称与留空的默认值");
        click(two, "operators.hit.1");
        eui::Ui three;
        compose(three);
        if (three.find("operator.name.3.field")->disabled ||
            three.find("operator.name.3.text")->text != "陈师傅")
            throw std::runtime_error("切回三人研究没有恢复第三个操作员");
    }
    for (const auto &field : fields) {
        reset(field.tab);
        const std::string value = gui::state.draft.*field.member == field.value ? "3" : field.value;
        eui::Ui ui;
        ui.begin("regrouped-settings-input");
        if (field.tab == dataTab)
            gui::dataPage(ui, 1232, 500);
        else
            gui::optionsPage(ui, 1232, 500);
        ui.end();
        ui.layout(1232, 500);
        input(ui, field.id, value);
        if (gui::state.draft.*field.member != value || !staleResult())
            throw std::runtime_error(std::string("参数变更未正确保留并标记旧结果：") + field.id);
        input(ui, field.id, value);
        if (gui::state.revision != revision + 1)
            throw std::runtime_error("重复填写相同值导致重复失效");
    }
    const std::array<std::pair<const char *, int native_ui::Draft::*>, 2> rules{{
        {"priority.hit.1", &native_ui::Draft::priority},
        {"compliance.hit", &native_ui::Draft::compliance},
    }};
    for (const auto &[id, member] : rules) {
        reset(0);
        gui::state.draft.*member = 0;
        eui::Ui ui;
        ui.begin("regrouped-generation-rules");
        gui::optionsPage(ui, 1232, 500);
        ui.end();
        ui.layout(1232, 500);
        click(ui, id);
        if (gui::state.draft.*member != 1 || !staleResult())
            throw std::runtime_error(std::string("生成规则变更未正确保留并标记旧结果：") + id);
    }
    {
        reset(0);
        gui::state.draft.analysisMethod = 1;
        gui::state.draft.variedResiduals = false;
        eui::Ui ui;
        ui.begin("teaching-residuals");
        gui::optionsPage(ui, 588, 500);
        ui.end();
        ui.layout(588, 500);
        click(ui, "residual.varied.hit");
        if (!gui::state.draft.variedResiduals || !staleResult())
            throw std::runtime_error("ANOVA 子组波动开关未修改生成配置");
    }
    for (const bool initial : {false, true}) {
        reset(0);
        gui::state.draft.requireNormality = initial;
        eui::Ui ui;
        ui.begin("generation-normality");
        const float width = initial ? 588.f : 1232.f;
        gui::optionsPage(ui, width, 500);
        ui.end();
        ui.layout(width, 500);
        click(ui, "normality.hit");
        if (gui::state.draft.requireNormality == initial || !staleResult())
            throw std::runtime_error("正态性筛选变更未正确保留并标记旧结果");
        auto saved = native_ui::to_config(gui::state.draft);
        const auto generation =
            native_ui::to_config(gui::state.draft, native_ui::ConfigUse::Generation);
        if (saved["generation"]["require_normality"] != !initial ||
            generation["generation"]["require_normality"] != !initial ||
            native_ui::from_config(saved).requireNormality == initial)
            throw std::runtime_error("正态性筛选未随生成请求或配置保存和恢复");
        saved["generation"].erase("require_normality");
        if (native_ui::from_config(saved).requireNormality)
            throw std::runtime_error("缺少正态性筛选字段的旧配置未采用关闭默认值");
    }
    for (const auto &[width, height] : {std::pair{588.f, 332.f}, std::pair{1232.f, 636.f}}) {
        const bool compact = width < 600;
        reset(0);
        gui::state.draft.partSampling = 0;
        gui::state.draft.minimumPartSpanPct = "80.5";
        gui::state.draft.lsl = "9.5";
        gui::state.draft.usl = compact ? "" : "10.5";
        eui::Ui ui;
        const auto compose = [&] {
            ui.begin("part-sampling-and-span");
            gui::optionsPage(ui, width, height);
            gui::menuOverlay(ui, {width, height});
            ui.end();
            ui.layout(width, height);
        };
        compose();
        const auto *sampling = ui.find("part.sampling");
        const auto *span = ui.find("part.span.hit");
        const auto *spanNote = ui.find("part.span.note");
        const auto *scroll = ui.find("configscroll");
        if (!sampling || !sampling->onPress || !span || span->disabled || !spanNote || !scroll)
            throw std::runtime_error("部件取样和跨度设置应可操作");
        const float reachableBottom = scroll->frame.y + scroll->frame.height + scroll->scrollMaxOffset;
        if (sampling->frame.y + sampling->frame.height > reachableBottom + .1f ||
            span->frame.y + span->frame.height > reachableBottom + .1f ||
            (spanNote->text.find("填写上下规格限") != std::string::npos) != compact)
            throw std::runtime_error("部件取样和跨度设置超出滚动范围或缺少双侧规格提示");
        gui::state.configOffset = std::clamp(sampling->frame.y - scroll->frame.y - 40.f,
                                             0.f, scroll->scrollMaxOffset);
        compose();
        ui.find("part.sampling")->onKeyEvent({eui::InputKey::Enter});
        compose();
        const std::vector<std::string> expected{
            "默认取样", "随机取样", "分层随机", "等距覆盖", "正态分位",
            "中心加密", "两端加密", "低值加密", "高值加密", "双批取样"};
        if (!gui::menu.select || gui::menu.items != expected)
            throw std::runtime_error("部件取样下拉框缺少方案或顺序错误");
        const int selected = compact ? 9 : 8;
        if (compact) {
            const auto *menuScroll = ui.find("menuoverlay");
            if (!menuScroll || !menuScroll->onScrollOffsetChanged || menuScroll->scrollMaxOffset <= 0)
                throw std::runtime_error("窄窗口的取样菜单应支持滚动");
            menuScroll->onScrollOffsetChanged(menuScroll->scrollMaxOffset);
            compose();
            const auto &last = *ui.find("menuitem9");
            const auto item = gui::menuAnchor(ui, last);
            const auto &box = ui.find("menubox")->frame;
            if (box.y < 0 || box.y + box.height > height + .1f || item.y < box.y - .1f ||
                item.y + item.height > box.y + box.height + .1f)
                throw std::runtime_error("滚动后最后一项未完整显示在取样菜单中");
            click(ui, "menuitem9");
        } else {
            const auto *control = ui.find("part.sampling");
            control->onKeyEvent({eui::InputKey::End});
            control->onKeyEvent({eui::InputKey::Up});
            control->onKeyEvent({eui::InputKey::Enter});
        }
        compose();
        if (!gui::menu.id.empty() || gui::state.draft.partSampling != selected || !staleResult() ||
            ui.find("part.sampling.note")->text != msa::part_sampling_methods[selected].description)
            throw std::runtime_error("取样方案选择后未更新说明或标记旧结果");
        auto saved = native_ui::to_config(gui::state.draft);
        const auto generation =
            native_ui::to_config(gui::state.draft, native_ui::ConfigUse::Generation);
        const auto loaded = native_ui::from_config(saved);
        if (saved["generation"]["part_sampling"] != (compact ? "two_cluster" : "high_dense") ||
            saved["generation"]["minimum_part_span_pct"] != 80.5 ||
            generation["generation"] != saved["generation"] || loaded.partSampling != selected ||
            loaded.minimumPartSpanPct != "80.5")
            throw std::runtime_error("部件取样或跨度未随生成请求或配置保存和恢复");
        saved["generation"].erase("part_sampling");
        saved["generation"].erase("minimum_part_span_pct");
        const auto legacy = native_ui::from_config(saved);
        if (legacy.partSampling != 0 || !legacy.minimumPartSpanPct.empty())
            throw std::runtime_error("旧配置应采用默认取样和自动跨度");
        for (const auto &method : msa::part_sampling_methods) {
            saved["generation"]["part_sampling"] = method.key;
            const auto restored = native_ui::from_config(saved);
            if (expected.at(restored.partSampling) != method.label ||
                native_ui::to_config(restored)["generation"]["part_sampling"] != method.key)
                throw std::runtime_error("取样重排序后未按配置键恢复选项");
        }
        ui.find("part.sampling")->onKeyEvent({eui::InputKey::Enter});
        compose();
        const auto *control = ui.find("part.sampling");
        control->onKeyEvent({eui::InputKey::Home});
        control->onKeyEvent({eui::InputKey::Down});
        control->onKeyEvent({eui::InputKey::Enter});
        if (native_ui::to_config(gui::state.draft)["generation"]["part_sampling"] != "uniform_random")
            throw std::runtime_error("随机取样未保存为正确的配置键");
        for (const bool lower : {false, true})
            for (const bool upper : {false, true}) {
                gui::state.draft.lsl = lower ? "9.5" : "";
                gui::state.draft.usl = upper ? "10.5" : "";
                compose();
                const auto *hint = ui.find("part.sampling.limits");
                if (bool(hint) != (!lower || !upper) ||
                    (hint && hint->text.find("填齐上下规格限") == std::string::npos))
                    throw std::runtime_error("随机取样未随双侧规格限更新填写提示");
            }
    }
    reset(2);
    gui::state.draft.csvFormat = 1;
    gui::state.draft.storeStandardOrder = false;
    eui::Ui output;
    output.begin("regrouped-output-input");
    gui::optionsPage(output, 1232, 500);
    output.end();
    output.layout(1232, 500);
    for (const auto &[id, label] : std::array<std::pair<const char *, const char *>, 7>{{
             {"import.filename.label", "测量数据导入名称"}, {"filename.label", "测量数据导出名称"},
             {"chart.filename.label", "图表导出名称"}, {"report.filename.label", "方法对比报告导出名称"},
             {"directory.label", "测量数据导出目录"}, {"chart.directory.label", "图表导出目录"},
             {"report.directory.label", "方法对比报告导出目录"}}}) {
        if (!output.find(id) || output.find(id)->text != label)
            throw std::runtime_error(std::string("选项页名称或目录标签不符：") + label);
    }
    if (gui::state.draft.silentCsv || gui::state.draft.silentCharts || gui::state.draft.silentReport ||
        gui::state.draft.dataFileType != 0 || gui::state.draft.reportFormat != 0)
        throw std::runtime_error("首次使用应全部询问保存位置，测量数据默认 CSV，报告默认 PDF");
    for (const auto &[fieldId, member, entered, expected] :
         std::array<std::tuple<const char *, std::string native_ui::Draft::*, const char *, const char *>, 4>{{
             {"filename", &native_ui::Draft::filename, "测量_[YYYY].xlsx", "测量_[YYYY]"},
             {"import.filename", &native_ui::Draft::importFilename, "测量.v2.CSV", "测量.v2"},
             {"chart.filename", &native_ui::Draft::chartFilename, "控制图.jpeg", "控制图"},
             {"report.filename", &native_ui::Draft::reportFilename, "报告.pdf", "报告"}}}) {
        input(output, fieldId, entered);
        output.find(std::string(fieldId) + ".hit")->onFocusChanged(false);
        if (gui::state.draft.*member != expected)
            throw std::runtime_error(std::string("文件名输入结束后未显示基本名：") + fieldId);
    }
    click(output, "runorder.standard.hit");
    click(output, "silent.csv.hit");
    click(output, "silent.charts.hit");
    click(output, "silent.report.hit");
    click(output, "report.format.hit.1");
    click(output, "data.filetype.hit.1");
    input(output, "report.directory", "results");
    input(output, "import.directory", "输入");
    input(output, "import.filename", "来料复核");
    input(output, "chart.directory", "plots");
    input(output, "chart.filename", "测量图");
    input(output, "report.filename", "方法比对");
    const auto chooser = gui::state.chooseDirectory;
    for (const auto &field :
         std::array<std::pair<const char *, std::string native_ui::Draft::*>, 4>{{
             {"import.directory", &native_ui::Draft::importDirectory},
             {"directory", &native_ui::Draft::directory},
             {"chart.directory", &native_ui::Draft::chartDirectory},
             {"report.directory", &native_ui::Draft::reportDirectory}}}) {
        const auto current = gui::state.draft.*field.second;
        bool opened = false;
        gui::state.chooseDirectory = [&](const auto &initial) -> std::optional<std::filesystem::path> {
            opened = initial == msa::resolve_output_directory(current);
            return std::nullopt;
        };
        click(output, std::string(field.first) + ".browse.bg");
        if (!opened || gui::state.draft.*field.second != current)
            throw std::runtime_error("目录选择器未从设定目录打开，或取消后改变了设置");
    }
    gui::state.chooseDirectory = chooser;
    if (!gui::state.draft.silentCsv || !gui::state.draft.silentCharts ||
        !gui::state.draft.silentReport || gui::state.draft.reportFormat != 1 ||
        gui::state.draft.dataFileType != 1 ||
        gui::state.draft.reportDirectory != "results")
        throw std::runtime_error("导出方式、报告格式或目录未响应界面设置");
    const auto preferencesPath = std::filesystem::temp_directory_path() /
                                 ("msa-ui-export-preferences-" +
                                  std::to_string(gui::Clock::now().time_since_epoch().count()) + ".json");
    msa::save_config(native_ui::to_config(gui::state.draft), preferencesPath);
    const auto restored = native_ui::from_config(msa::load_config(preferencesPath));
    std::filesystem::remove(preferencesPath);
    if (!restored.silentCsv || !restored.silentCharts || !restored.silentReport ||
        restored.dataFileType != 1 ||
        restored.reportFormat != 1 || restored.reportDirectory != "results" ||
        restored.chartDirectory != "plots" || restored.chartFilename != "测量图" ||
        restored.importDirectory != "输入" || restored.importFilename != "来料复核" ||
        restored.reportFilename != "方法比对")
        throw std::runtime_error("刚保存的导出偏好重载后发生变化");
    if (!gui::state.draft.storeStandardOrder || !gui::state.validResult() ||
        gui::state.revision != revision || !gui::state.configDirty || !gui::state.result ||
        gui::state.result->matrix != generated.matrix)
        throw std::runtime_error("附加标准序号列变更破坏已有生成结果");
    gui::state.draft = draft;
    gui::state.page = page;
    gui::state.optionsTab = 0;
    gui::state.configDirty = true;
    gui::state.footerMessage.clear();
    gui::state.logs = logs;
    gui::state.logSequence = sequence;
    std::cout << "重分组后参数变更正确标记旧结果，附加导出列仅修改输出偏好\n";
}

void setting_dependencies() {
    native_ui::Draft draft;
    draft.lsl = "9.5";
    draft.usl = "10.5";
    draft.interactionAlpha = "0";
    draft.confidenceEnabled = true;
    draft.confidenceLevel.clear();
    draft.reference = "未完成";
    for (const auto use : {native_ui::ConfigUse::Full, native_ui::ConfigUse::Generation,
                           native_ui::ConfigUse::Analysis}) {
        const auto effective = native_ui::to_config(draft, use);
        if (effective["analysis"]["interaction_alpha"] != .05 ||
            effective["analysis"]["confidence"]["level"] != 95 ||
            !effective["generation"]["reference_tv_sd"].is_null())
            throw std::runtime_error("停用字段妨碍使用当前有效选项");
    }
    const auto rejects = [](const native_ui::Draft &value, const std::string &expected) {
        try {
            native_ui::to_config(value, native_ui::ConfigUse::Generation);
        } catch (const msa::ConfigError &error) {
            if (std::string(error.what()).find(expected) != std::string::npos)
                return;
        }
        throw std::runtime_error("重新启用后遗漏了对 " + expected + " 的检查");
    };
    auto activated = draft;
    activated.analysisMethod = 1;
    activated.confidenceEnabled = false;
    rejects(activated, "显著性水平");
    activated.interactionMode = 1;
    activated.confidenceEnabled = true;
    rejects(activated, "置信水平");
    activated = draft;
    activated.scale = 1;
    rejects(activated, "参考 TV_SD");

    draft.interactionAlpha = "0.08";
    draft.confidenceLevel = "92";
    draft.reference = ".2";
    auto configured = native_ui::to_config(draft);
    if (configured["analysis"]["interaction_alpha"] != .08 ||
        configured["analysis"]["confidence"]["level"] != 92 ||
        configured["generation"]["reference_tv_sd"] != .2)
        throw std::runtime_error("合法停用选项未保留");
    {
        auto teaching = draft;
        teaching.analysisMethod = 1;
        teaching.confidenceEnabled = false;
        teaching.variedResiduals = true;
        const auto saved = native_ui::to_config(teaching);
        if (saved["generation"]["residual_model"] != "varied" ||
            !native_ui::from_config(saved).variedResiduals)
            throw std::runtime_error("ANOVA 教学场景未随 GUI 配置保存和还原");
        teaching.analysisMethod = 0;
        if (native_ui::to_config(teaching)["generation"]["residual_model"] != "standard")
            throw std::runtime_error("Xbar-R 误用 ANOVA 教学场景");
    }

    configured["targets"]["GRR_pct"] = {{"mode", "between"}, {"min", 10}, {"max", 20}};
    draft = native_ui::from_config(configured);
    auto &target = draft.targets[2];
    target.mode = 1;
    auto config = native_ui::to_config(draft);
    if (config["targets"]["GRR_pct"].contains("tolerance_pp") ||
        msa::acceptance(config, "GRR_pct") != msa::Bounds({9.9, 10.1}))
        throw std::runtime_error("区间上限被误用为目标允许误差");
    target.tolerance = "0.2";
    target.mode = 2;
    config = native_ui::to_config(draft);
    if (config["targets"]["GRR_pct"]["max"] != 20)
        throw std::runtime_error("目标允许误差被误用为区间上限");
    target.mode = 1;
    if (native_ui::to_config(draft)["targets"]["GRR_pct"]["tolerance_pp"] != .2)
        throw std::runtime_error("切回目标模式时未恢复上次填写的允许误差");
}

void help_topics() {
    const auto revision = gui::state.revision;
    const std::array<const char *, 6> anchors{"help.workflow.card", "help.analysis.card",
                                              "help.runorder.card", "help.filename.card",
                                              "help.cli.card",      "help.charts.card"};
    const std::array<const char *, 6> shortcuts{
        "sample.bg", "help.analysis.open.bg", "help.files.open.bg", "help.filename.open.bg",
        nullptr,     "help.charts.open.bg"};
    const std::array<const char *, 6> lastSections{
        "help.troubleshooting.card", "help.software.card", "help.minitab.card",
        "help.filename.collision", "help.cli.outputs.card", "help.charts.export.card"};
    constexpr float tolerance = .1f;
    const auto bottom = [](const core::dsl::Element &node) {
        return node.frame.y + node.frame.height;
    };
    // 使用应用在最小、居中尺寸和默认窗口中留给帮助页的实际区域。
    for (const auto &[width, height] : {std::pair{588.f, 332.f}, std::pair{700.f, 420.f},
                                        std::pair{1020.f, 636.f}, std::pair{1232.f, 636.f}})
        for (int topic = 0; topic < 6; ++topic) {
            gui::state.helpTopic = (topic + 5) % 6;
            gui::state.helpOffset = 80;
            eui::Ui select;
            select.begin("help-topic-navigation");
            gui::helpPage(select, width, height);
            select.end();
            select.layout(width, height);
            if (width < 700) {
                const auto *control = select.find("help.topics");
                if (!control || !control->onPress)
                    throw std::runtime_error("compact help lacks its topic selector");
                control->onPress({}, {});
                if (gui::menu.items.size() != anchors.size() || !gui::menu.select)
                    throw std::runtime_error("compact help does not expose all topics");
                gui::menu.select(topic);
                gui::menu.id.clear();
            } else
                click(select, "help.topics.hit." + std::to_string(topic));
            if (gui::state.helpTopic != topic || gui::state.helpOffset != 0 ||
                gui::state.revision != revision)
                throw std::runtime_error(
                    "help topic selection changes study or retains old scroll");
            eui::Ui page;
            page.begin("help-topic-content");
            gui::helpPage(page, width, height);
            page.end();
            page.layout(width, height);
            controls_fit(page, width, "help topic " + std::to_string(topic));
            for (std::size_t other = 0; other < anchors.size(); ++other)
                if (bool(page.find(anchors[other])) != (other == topic))
                    throw std::runtime_error(
                        "help topic mixes unrelated content or omits its section");
            const auto *navigation = page.find("help.topics");
            const auto *scroll = page.find("helpscroll");
            const auto *toolbar = page.find("help.navigation");
            const auto *article = page.find("help.page");
            if (!navigation || !scroll || !toolbar || !article ||
                scroll->frame.y < bottom(*navigation))
                throw std::runtime_error("help text overlaps the fixed topic selector");
            if (std::abs(toolbar->frame.x - article->frame.x) > tolerance ||
                std::abs(toolbar->frame.width - article->frame.width) > tolerance ||
                std::abs(scroll->frame.x * 2 + scroll->frame.width - width) > tolerance ||
                scroll->frame.width > 940.f + tolerance)
                throw std::runtime_error("help toolbar and body do not share a centered column");
            const float reachableBottom = bottom(*scroll) + scroll->scrollMaxOffset;
            const auto *last = page.find(lastSections[topic]);
            if (!last || bottom(*last) > reachableBottom + tolerance)
                throw std::runtime_error("last help section is missing or clipped");
            const auto checkShortcut = [&](const char *id) {
                const auto *action = page.find(id);
                if (!action || !action->onClick || !action->focusable || !action->onKeyEvent ||
                    bottom(*action) > reachableBottom + tolerance)
                    throw std::runtime_error(std::string("unreachable help shortcut: ") + id);
            };
            if (const auto *shortcut = shortcuts[topic])
                checkShortcut(shortcut);
            if (topic == 0) {
                checkShortcut("help.rules.open.bg");
                checkShortcut("help.logs.open.bg");
                for (const auto &method : msa::part_sampling_methods) {
                    const auto id = std::string("help.part.sampling.") + method.key;
                    const auto *label = page.find(id + ".label");
                    const auto *copy = page.find(id + ".copy");
                    if (!label || label->text != method.label || !copy ||
                        copy->text != method.description || bottom(*copy) > reachableBottom + tolerance)
                        throw std::runtime_error("取样帮助的方案名称或说明缺失：" + id);
                }
            }
            if (topic == 4) {
                checkShortcut("help.cli.help.copy.bg");
                checkShortcut("help.cli.example.copy.bg");
                checkShortcut("help.cli.analyze.example.copy.bg");
            }
            if (topic == 3) {
                for (const auto &variable : msa::filename_variables()) {
                    const auto id = std::string("help.variable.") + variable.name;
                    for (const auto *suffix : {".copy.bg", ".alias.bg"}) {
                        if (suffix == std::string_view(".alias.bg") && !*variable.alias)
                            continue;
                        const auto *copy = page.find(id + suffix);
                        if (!copy || !copy->onClick || copy->disabled || !copy->onKeyEvent ||
                            bottom(*copy) > reachableBottom + tolerance)
                            throw std::runtime_error("filename variable cannot be copied: " + id);
                    }
                }
                const auto *milliseconds = page.find("help.variable.MS");
                const auto *timestamp = page.find("help.variable.TIMESTAMP");
                const auto *example = page.find("help.filename.example.card");
                const auto *firstVariable = page.find("help.variable.YYYY");
                const auto *yearDescription = page.find("help.variable.YYYY.description");
                const auto *aliasDescription = page.find("help.variable.MULTIPLIER.description");
                if (!milliseconds || !timestamp || timestamp->frame.y < bottom(*milliseconds))
                    throw std::runtime_error("timestamp help is not below milliseconds");
                if (!example || !firstVariable || bottom(*example) > firstVariable->frame.y)
                    throw std::runtime_error("filename template example follows its reference");
                if (!yearDescription || !aliasDescription ||
                    std::abs(yearDescription->frame.x - aliasDescription->frame.x) > tolerance ||
                    (width < 700 && yearDescription->frame.width < width / 2))
                    throw std::runtime_error("filename descriptions have an uneven or narrow column");
            }
        }
    gui::state.helpTopic = 0;
    gui::state.helpOffset = 0;
    std::cout << "help topics remain separate and accessible, with filename copy actions intact\n";
}

void scrolled_menu_anchors() {
    const auto savedPage = gui::state.page;
    const auto savedTopic = gui::state.helpTopic;
    const auto savedOffset = gui::state.helpOffset;
    const auto savedScale = gui::state.pixelScale;
    const auto savedMenu = gui::menu;
    gui::state.page = gui::Page::Help;
    gui::state.helpTopic = 0;
    const auto checkAnchor = [](eui::Ui &ui, const std::string &id, float expectedY) {
        const auto *control = ui.find(id);
        if (!control || !control->onKeyEvent || !control->onPress)
            throw std::runtime_error("滚动菜单回归缺少目标控件");
        gui::menu.id.clear();
        control->onKeyEvent({eui::InputKey::Enter});
        const auto keyboard = gui::menu.anchor;
        if (gui::menu.id != id || std::fabs(keyboard.y - expectedY) > .01f ||
            std::fabs(keyboard.x - control->frame.x) > .01f)
            throw std::runtime_error("键盘菜单锚点未跟随实际所属滚动区域：" + id);
        control->onKeyEvent({eui::InputKey::Escape});
        const float scale = gui::state.pixelScale;
        control->onPress({}, {control->frame.x * scale, expectedY * scale,
                              control->frame.width * scale, control->frame.height * scale});
        if (std::fabs(gui::menu.anchor.y - keyboard.y) > .01f ||
            std::fabs(gui::menu.anchor.x - keyboard.x) > .01f)
            throw std::runtime_error("键盘和鼠标菜单锚点不一致：" + id);
        gui::menu.id.clear();
        control->onKeyEvent({eui::InputKey::Space});
        if (std::fabs(gui::menu.anchor.y - expectedY) > .01f)
            throw std::runtime_error("空格打开菜单时滚动定位错误：" + id);
        gui::menu.id.clear();
    };
    for (float scale : {1.f, 1.25f, 1.5f, 2.f}) {
        gui::state.pixelScale = scale;
        for (float offset : {0.f, 80.f, 200.f, 100000.f}) {
            gui::state.helpOffset = offset;
            eui::Ui ui;
            ui.begin("fixed-help-menu");
            gui::helpPage(ui, 588, 500);
            ui.end();
            ui.layout(588, 500);
            checkAnchor(ui, "help.topics", ui.find("help.topics")->frame.y);
        }
        // 任意页面状态不能替代控件真实祖先；嵌套滚动需要累加两层偏移。
        eui::Ui ui;
        ui.begin("nested-menu");
        components::scrollView(ui, "outer")
            .size(400, 260).offset(45.25f)
            .content([&](eui::Ui &outer, float width, float) {
                outer.rect("outer.leading").size(width, 100).build();
                gui::combo(outer, "outer.choice", {"一", "二"}, 0, width, [](int) {});
                components::scrollView(outer, "inner")
                    .size(width, 120).offset(30.5f)
                    .content([&](eui::Ui &inner, float innerWidth, float) {
                        inner.rect("inner.leading").size(innerWidth, 100).build();
                        gui::combo(inner, "inner.choice", {"一", "二"}, 0, innerWidth,
                                   [](int) {});
                        inner.rect("inner.trailing").size(innerWidth, 400).build();
                    }).build();
                outer.rect("outer.trailing").size(width, 400).build();
            }).build();
        ui.end();
        ui.layout(400, 260);
        // 与运行时每层物理位移一致，各层分别取整，不能在相加后只取整一次。
        const float outerPixels = std::round(45.25f * scale);
        const float innerPixels = std::round(30.5f * scale);
        checkAnchor(ui, "outer.choice", ui.find("outer.choice")->frame.y - outerPixels / scale);
        checkAnchor(ui, "inner.choice", ui.find("inner.choice")->frame.y -
                                           (outerPixels + innerPixels) / scale);

        // 经 Runtime 派发真实指针事件，直接比较运行时命中矩形与键盘锚点。
        // 不把预期矩形传给 onPress，避免模拟回调掩盖 DPI/嵌套变换错误。
        for (const bool fixed : {true, false}) {
            core::dsl::Runtime runtime;
            runtime.initialize();
            eui::Ui *view = nullptr;
            gui::menu.id.clear();
            gui::state.helpOffset = 80.25f;
            const auto compose = [&] {
                runtime.compose(
                    "runtime-menu-anchors", fixed ? 588.f : 400.f, fixed ? 500.f : 260.f,
                    [&](eui::Ui &root, const core::dsl::Screen &) {
                        view = &root;
                        if (fixed) {
                            gui::helpPage(root, 588, 500);
                            return;
                        }
                        components::scrollView(root, "runtime.outer")
                            .size(400, 260)
                            .offset(45.25f)
                            .content([&](eui::Ui &outer, float width, float) {
                                outer.rect("runtime.outer.leading").size(width, 100).build();
                                gui::combo(outer, "runtime.outer.choice", {"一", "二"}, 0,
                                           width, [](int) {});
                                components::scrollView(outer, "runtime.inner")
                                    .size(width, 120)
                                    .offset(30.5f)
                                    .content([&](eui::Ui &inner, float innerWidth, float) {
                                        inner.rect("runtime.inner.leading")
                                            .size(innerWidth, 100).build();
                                        gui::combo(inner, "runtime.inner.choice", {"一", "二"},
                                                   0, innerWidth, [](int) {});
                                        inner.rect("runtime.inner.trailing")
                                            .size(innerWidth, 400).build();
                                    }).build();
                                outer.rect("runtime.outer.trailing").size(width, 400).build();
                            }).build();
                    });
            };
            const auto key = [&](core::InputKey value) {
                core::queueKeyInput(nullptr, {value});
                runtime.update(nullptr, 0, 1, scale);
                compose();
            };
            compose();
            runtime.update(nullptr, 0, 1, scale);
            const std::vector<std::string> targets =
                fixed ? std::vector<std::string>{"help.topics"}
                      : std::vector<std::string>{"runtime.outer.choice", "runtime.inner.choice"};
            for (const auto &id : targets) {
                const auto *target = view->find(id);
                if (!target)
                    throw std::runtime_error("运行时菜单回归缺少控件：" + id);
                const float pixels = fixed ? 0.f : outerPixels +
                    (id == "runtime.inner.choice" ? innerPixels : 0.f);
                const float x = (target->frame.x + target->frame.width / 2) * scale;
                const float y = (target->frame.y + target->frame.height / 2) * scale - pixels;
                core::queuePointerButton(nullptr, x, y, core::PointerButton::Left,
                                         core::PointerAction::Press, {});
                core::queuePointerButton(nullptr, x, y, core::PointerButton::Left,
                                         core::PointerAction::Release, {});
                runtime.update(nullptr, 0, 1, scale);
                if (gui::menu.id != id || !view->isFocused(id))
                    throw std::runtime_error("真实指针未打开菜单或未同步键盘焦点：" + id);
                const auto pointer = gui::menu.anchor;
                key(core::InputKey::Escape);
                if (!gui::menu.id.empty())
                    throw std::runtime_error("真实 Escape 未关闭菜单：" + id);
                for (const auto activation : {core::InputKey::Enter, core::InputKey::Space}) {
                    key(activation);
                    const auto keyboard = gui::menu.anchor;
                    if (gui::menu.id != id || std::fabs(pointer.x - keyboard.x) > .01f ||
                        std::fabs(pointer.y - keyboard.y) > .01f ||
                        std::fabs(pointer.width - keyboard.width) > .01f ||
                        std::fabs(pointer.height - keyboard.height) > .01f)
                        throw std::runtime_error("真实指针与键盘菜单锚点不一致：" + id +
                                                 "，DPI=" + std::to_string(scale));
                    key(core::InputKey::Escape);
                }
            }
            core::detail::inputQueues().erase(nullptr);
            core::detail::pointerStates().erase(nullptr);
            runtime.shutdown(false);
        }
    }
    gui::state.page = savedPage;
    gui::state.helpTopic = savedTopic;
    gui::state.helpOffset = savedOffset;
    gui::state.pixelScale = savedScale;
    gui::menu = savedMenu;
    std::cout << "固定帮助导航与单层/嵌套滚动菜单的真实 Runtime 键盘、鼠标锚点在四档 DPI 下保持一致\n";
}

void state_recovery(const msa::Result &generated) {
    gui::State state;
    state.result = generated;
    state.resultRevision = state.revision;
    for (const auto status :
         {gui::Status::Cancelled, gui::Status::SearchFailed, gui::Status::Infeasible,
          gui::Status::Error}) {
        state.status = status;
        if (!state.displayedResult() || !state.validResult() ||
            state.result->matrix != generated.matrix)
            throw std::runtime_error("retry failure/cancellation discards the previous result");
    }
    for (const auto status : {gui::Status::Running, gui::Status::Cancelling}) {
        state.status = status;
        if (!state.displayedResult() || state.validResult())
            throw std::runtime_error("active retry hides or permits exporting the previous result");
    }
    state.status = gui::Status::Error;
    state.message = "invalid parameters";
    state.configDirty = false;
    state.changed();
    if (state.displayedResult() || state.validResult() || !state.message.empty() ||
        state.status != gui::Status::Ready || !state.configDirty)
        throw std::runtime_error("parameter repair retains an error or a stale result");
    state.status = gui::Status::Running;
    state.changed();
    if (state.status != gui::Status::Running)
        throw std::runtime_error("parameter edit prematurely ends an active task");
    state.status = gui::Status::Success;
    state.resultRevision = state.revision;
    state.configDirty = false;
    const auto revision = state.revision;
    state.outputChanged();
    if (!state.validResult() || !state.configDirty || state.revision != revision)
        throw std::runtime_error("output preference edit changes study validity");

    state.draft.directory = "D:/study/output";
    state.draft.filename = "Study_[YYYY]_[SEED].csv";
    state.draft.csvFormat = 4;
    state.draft.existingFile = 1;
    state.draft.runMode = 2;
    state.draft.randomOperators = true;
    state.draft.storeStandardOrder = true;
    state.draft.runSeed = "713";
    state.draft.dataFileType = 1;
    state.draft.silentCsv = true;
    state.draft.silentCharts = true;
    state.draft.silentReport = true;
    state.draft.reportFormat = 1;
    state.draft.reportDirectory = "reports";
    state.draft.importDirectory = "inputs";
    state.draft.importFilename = "input.csv";
    state.draft.chartDirectory = "charts";
    state.draft.chartFilename = "chart-name";
    state.draft.reportFilename = "report-name";
    state.draft.analysisMethod = 1;
    state.draft.interactionMode = 2;
    state.draft.interactionAlpha = "0.1";
    const auto preferences = state.draft;
    state.sample();
    if (state.draft.directory != preferences.directory ||
        state.draft.filename != preferences.filename ||
        state.draft.csvFormat != preferences.csvFormat ||
        state.draft.existingFile != preferences.existingFile ||
        state.draft.runMode != preferences.runMode ||
        state.draft.randomOperators != preferences.randomOperators ||
        state.draft.storeStandardOrder != preferences.storeStandardOrder ||
        state.draft.runSeed != preferences.runSeed ||
        state.draft.dataFileType != preferences.dataFileType ||
        state.draft.silentCsv != preferences.silentCsv ||
        state.draft.silentCharts != preferences.silentCharts ||
        state.draft.silentReport != preferences.silentReport ||
        state.draft.reportFormat != preferences.reportFormat ||
        state.draft.reportDirectory != preferences.reportDirectory ||
        state.draft.importDirectory != preferences.importDirectory ||
        state.draft.importFilename != preferences.importFilename ||
        state.draft.chartDirectory != preferences.chartDirectory ||
        state.draft.chartFilename != preferences.chartFilename ||
        state.draft.reportFilename != preferences.reportFilename ||
        state.draft.analysisMethod != preferences.analysisMethod ||
        state.draft.interactionMode != preferences.interactionMode ||
        state.draft.interactionAlpha != preferences.interactionAlpha || state.validResult())
        throw std::runtime_error("sample replaces output preferences or retains stale data");

    state.hint("copied", gui::Level::Info);
    if (!state.visibleHint())
        throw std::runtime_error("operation feedback is not immediately visible");
    state.navigate(gui::Page::Help);
    if (!state.footerMessage.empty() || state.visibleHint())
        throw std::runtime_error("operation feedback leaks across pages");
    state.hint("copied");
    state.status = gui::Status::Running;
    if (state.visibleHint())
        throw std::runtime_error("operation feedback hides active task progress");
    state.status = gui::Status::Ready;
    state.footerEntered = gui::Clock::now() - std::chrono::seconds(7);
    if (state.visibleHint())
        throw std::runtime_error("short feedback never expires");
    std::cout << "state recovery retains valid results and preserves output preferences\n";
}

void header_status_presentation(const msa::Result &generated) {
    using Tone = gui::HeaderStatusTone;
    using Status = gui::Status;
    const auto require = [](bool condition, const std::string &message) {
        if (!condition)
            throw std::runtime_error("顶栏状态：" + message);
    };
    struct Case {
        Status status;
        std::array<const char *, 3> labels;
        std::array<Tone, 3> tones;
    };
    // 无结果、当前结果和陈旧结果分别验证，避免失败被已有数据覆盖为绿色成功。
    const std::array<Case, 8> cases{{
        {Status::Ready, {"待生成数据", "数据已生成", "待重新生成"},
         {Tone::Neutral, Tone::Success, Tone::Warning}},
        {Status::Running, {"正在生成", "正在生成", "正在生成"},
         {Tone::Working, Tone::Working, Tone::Working}},
        {Status::Cancelling, {"正在取消", "正在取消", "正在取消"},
         {Tone::Warning, Tone::Warning, Tone::Warning}},
        {Status::Success, {"待生成数据", "数据已生成", "待重新生成"},
         {Tone::Neutral, Tone::Success, Tone::Warning}},
        {Status::Cancelled, {"已取消生成", "取消 · 结果保留", "已取消生成"},
         {Tone::Neutral, Tone::Warning, Tone::Neutral}},
        {Status::Error, {"操作异常", "异常 · 结果保留", "操作异常"},
         {Tone::Error, Tone::Error, Tone::Error}},
        {Status::Infeasible, {"约束冲突", "冲突 · 结果保留", "约束冲突"},
         {Tone::Warning, Tone::Warning, Tone::Warning}},
        {Status::SearchFailed, {"未找到可行数据", "未找到 · 结果保留", "未找到可行数据"},
         {Tone::Warning, Tone::Warning, Tone::Warning}},
    }};
    gui::State business;
    gui::VisualState charts;
    business.revision = 3;
    std::vector<gui::HeaderStatusPresentation> presentations;
    for (const auto &test : cases)
        for (int result = 0; result < 3; ++result) {
            business.status = test.status;
            business.result = result ? std::optional<msa::Result>(generated) : std::nullopt;
            business.resultRevision = result == 1 ? business.revision : business.revision - 1;
            business.message = business.busy() ? "上次操作诊断" : "";
            const auto actual = gui::headerStatusPresentation(business, charts);
            const bool retained = result == 1 &&
                                  (test.status == Status::Cancelled || test.status == Status::Error ||
                                   test.status == Status::Infeasible || test.status == Status::SearchFailed);
            require(actual.label == test.labels[result] && actual.tone == test.tones[result] &&
                        actual.active == business.busy() && actual.retained == retained,
                    "业务状态或结果有效性映射错误：" + actual.label);
            business.hint("配置已保存");
            const auto transient = gui::headerStatusPresentation(business, charts);
            require(transient.label == actual.label && transient.tone == actual.tone &&
                        transient.active == actual.active && transient.retained == actual.retained,
                    "底栏短反馈覆盖了持续业务状态");
            require(business.status == test.status && business.revision == 3,
                    "读取状态改变了业务状态或数据修订号");
            presentations.push_back(actual);
        }
    business.result = generated;
    business.resultRevision = business.revision;
    business.status = Status::Success;
    business.message = "打开目录失败";
    require(gui::headerStatusPresentation(business, charts).tone == Tone::Error,
            "通用操作异常被已有结果覆盖");
    business.status = Status::Cancelled;
    const auto cancelledError = gui::headerStatusPresentation(business, charts);
    require(cancelledError.label == "操作异常" && cancelledError.tone == Tone::Error,
            "取消后发生的通用操作异常被旧取消状态覆盖");
    business.status = Status::Success;
    business.message.clear();
    business.dataExportError = "拒绝访问";
    require(gui::headerStatusPresentation(business, charts).label == "测量数据导出失败",
            "上次生成成功掩盖了 CSV 导出错误");
    business.dataExportError.clear();
    business.page = gui::Page::Charts;
    charts.importedSelected = true;
    require(gui::headerStatusPresentation(business, charts).label == "待导入数据",
            "未选取导入数据时仍显示生成结果成功");
    presentations.push_back(gui::headerStatusPresentation(business, charts));
    charts.imported = generated;
    require(gui::headerStatusPresentation(business, charts).label == "数据已载入",
            "独立导入数据来源未正确显示");
    presentations.push_back(gui::headerStatusPresentation(business, charts));
    const std::array<std::pair<const char *, const char *>, 6> chartErrors{{
        {"export", "图表导出失败"}, {"render", "图表渲染异常"}, {"import", "测量数据导入失败"},
        {"edit", "图表设置待调整"}, {"directory", "导出目录待检查"}, {"other", "图表操作异常"},
    }};
    for (const auto &[kind, label] : chartErrors) {
        charts.errorKind = kind;
        charts.errorText = "可恢复的图表错误";
        business.page = gui::Page::Charts;
        const auto actual = gui::headerStatusPresentation(business, charts);
        require(actual.label == label && actual.tone != Tone::Success,
                "图表错误被导入数据成功状态覆盖");
        presentations.push_back(actual);
        business.page = gui::Page::Data;
        require(gui::headerStatusPresentation(business, charts).label == "数据已生成",
                "图表页的局部错误泄漏到数据页");
    }
    std::promise<std::filesystem::path> exportPromise;
    charts.exporting = exportPromise.get_future();
    for (const auto page : {gui::Page::Data, gui::Page::Charts, gui::Page::Logs, gui::Page::Options,
                           gui::Page::Help, gui::Page::About}) {
        business.page = page;
        const auto actual = gui::headerStatusPresentation(business, charts);
        require(actual.label == "图表导出中" && actual.active && actual.tone == Tone::Working,
                "后台图表导出未在所有页面持续显示，或被旧错误覆盖");
        presentations.push_back(actual);
    }
    business.status = Status::Running;
    require(gui::headerStatusPresentation(business, charts).label == "正在生成",
            "生成任务未保持状态优先级");
    business.status = Status::Cancelling;
    require(gui::headerStatusPresentation(business, charts).label == "正在取消",
            "取消请求未及时显示");
    charts.exporting = {};

    const auto luminance = [](eui::Color color) {
        const auto linear = [](double value) {
            return value <= .04045 ? value / 12.92 : std::pow((value + .055) / 1.055, 2.4);
        };
        return .2126 * linear(color.r) + .7152 * linear(color.g) + .0722 * linear(color.b);
    };
    const auto themeColor = gui::tokens().primary;
    const auto iconColor = eui::Color(msa::brand_blue);
    const auto matchesIcon = [&](eui::Color color) {
        return color.r == iconColor.r && color.g == iconColor.g && color.b == iconColor.b &&
               color.a == iconColor.a;
    };
    eui::Ui brandUi;
    brandUi.begin("brand-palette");
    gui::appMark(brandUi, "brand-palette.mark", 38);
    gui::button(brandUi, "brand-palette.action", "生成数据", 120, [] {}, true);
    brandUi.end();
    brandUi.layout(220, 100);
    const auto *mark = brandUi.find("brand-palette.mark.bg");
    const auto *action = brandUi.find("brand-palette.action.bg");
    require(matchesIcon(themeColor) && mark && matchesIcon(mark->color) && action &&
                matchesIcon(action->color),
            "程序图标、软件内标识和主操作按钮应共用同一个主色");
    require((luminance(eui::Color(gui::blueWashActive)) + .05) / (luminance(themeColor) + .05) >=
                4.5,
            "选中控件的悬停与按下背景应保持文字可读");
    const auto savedMotion = gui::state.motion;
    for (bool motion : {false, true}) {
        gui::state.motion = motion;
        for (float width : {620.f, 1024.f, 1280.f}) {
            std::map<std::string, eui::Rect> baseline;
            for (const auto &presentation : presentations) {
                const auto fg = eui::Color(presentation.foreground), bg = eui::Color(presentation.background);
                const auto a = luminance(fg), b = luminance(bg);
                require(fg.a == 1 && bg.a == 1 && (std::max(a, b) + .05) / (std::min(a, b) + .05) >= 4.5,
                        "状态文字与背景对比不足：" + presentation.label);
                eui::Ui ui;
                ui.begin("header-status");
                ui.row("status.header")
                    .size(width, 64).padding(16, 10).gap(14).alignItems(eui::Align::CENTER)
                    .content([&] {
                        ui.rect("status.brand").size(312, 40).build();
                        ui.rect("status.spacer").width(eui::SizeValue::fill()).height(1).build();
                        gui::headerStatusIndicator(ui, presentation);
                    }).build();
                ui.end();
                ui.layout(width, 64);
                for (const auto *id : {"status.brand", "header.status", "header.status.dot", "header.status.text"}) {
                    const auto *node = ui.find(id);
                    require(node && node->frame.x >= 0 && node->frame.x + node->frame.width <= width,
                            std::string("状态元素超出窗口：") + id);
                    const eui::Rect rect{node->frame.x, node->frame.y, node->frame.width, node->frame.height};
                    if (!baseline.count(id)) baseline[id] = rect;
                    const auto &before = baseline.at(id);
                    require(rect.x == before.x && rect.y == before.y && rect.width == before.width && rect.height == before.height,
                            std::string("状态切换改变元素尺寸或位置：") + id);
                }
                const auto *text = ui.find("header.status.text"), *status = ui.find("header.status");
                require(status->frame.width == 176 && status->frame.height == 32 &&
                            text->text == presentation.label && text->text.find("●") == std::string::npos &&
                            text->fontSize >= 17 && !text->transition.enabled && text->opacity == 1 &&
                            !text->wrap && core::TextPrimitive::measureTextWidth(text->text, text->fontFamily,
                                                                               text->fontSize, text->fontWeight) <= text->frame.width,
                        "状态文字截断、混入图标或采用文字动效：" + presentation.label);
                for (const auto *id : {"header.status.bg", "header.status.dot"}) {
                    const auto *node = ui.find(id);
                    require(node && node->transition.properties == eui::AnimProperty::Color &&
                                (motion ? node->transition.durationSeconds > 0 : node->transition.durationSeconds == 0),
                            "状态动画影响布局或未遵守减少动效设置");
                }
            }
        }
    }
    gui::state.motion = savedMotion;
    std::cout << "顶栏状态覆盖业务/图表任务及旧结果，配色对比、固定布局、文字清晰和减少动效通过\n";
}

void render_request_lifecycle() {
    app::AppRunner runner;
    runner.beginRender();
    // 模拟首次显示窗口的刷新回调发生在绘制和提交之间。
    runner.paintRequested = true;
    runner.markRendered();
    if (!runner.paintRequested || runner.renderedFrames != 1)
        throw std::runtime_error("presenting a frame loses a redraw requested during window show");
    runner.beginRender();
    runner.markRendered();
    if (runner.paintRequested || runner.renderedFrames != 2)
        throw std::runtime_error("a completed follow-up frame remains in a repaint loop");
    std::cout << "frame presentation preserves show-time redraw requests and returns to idle\n";
}

void combo_unchanged_value() {
    gui::State state;
    state.configDirty = false;
    eui::Ui ui;
    ui.begin("same-combo-value");
    gui::combo(ui, "mode", {"First", "Second"}, 0, 200, [&](int) { state.changed(); });
    ui.end();
    ui.layout(240, 80);
    const auto *control = ui.find("mode");
    control->onPress({}, {});
    gui::menu.select(0);
    gui::menu.id.clear();
    control->onKeyEvent({core::InputKey::Enter});
    control->onKeyEvent({core::InputKey::Enter});
    if (state.revision != 0 || state.configDirty)
        throw std::runtime_error("selecting the existing dropdown value changes the study");
    control->onPress({}, {});
    gui::menu.select(1);
    gui::menu.id.clear();
    if (state.revision != 1 || !state.configDirty)
        throw std::runtime_error("a changed dropdown value is not applied once");
    std::cout << "unchanged dropdown selections do not invalidate results\n";
}

void long_combo_menu() {
    const auto savedMenu = gui::menu;
    const float savedScale = gui::state.pixelScale;
    gui::state.pixelScale = 1;
    gui::menu = {};
    const auto require = [](bool value, const std::string &message) {
        if (!value)
            throw std::runtime_error("长下拉菜单：" + message);
    };
    std::vector<std::string> items;
    for (int i = 0; i < 64; ++i)
        items.push_back("option-" + std::to_string(i));
    items.back() += "-very-long-option-label-that-must-not-expand-the-popup";
    int selected = 63, changes = 0;
    eui::Screen screen{320, 240};
    eui::Ui ui;
    const auto compose = [&] {
        ui.begin("long-combo-menu");
        gui::combo(ui, "long.options", items, selected, 220, [&](int i) {
            selected = i;
            ++changes;
        });
        gui::menuOverlay(ui, screen);
        ui.end();
        ui.layout(screen.width, screen.height);
    };
    const auto visible = [&] {
        const float top = gui::menu.highlighted * 38.f;
        return top >= gui::menu.offset - .1f &&
               top + 38 <= gui::menu.offset + gui::menu.viewportHeight + .1f;
    };
    const auto key = [&](eui::InputKey value) {
        require(ui.find("long.options")->onKeyEvent({value}), "键盘操作未处理");
        compose();
    };
    compose();
    ui.find("long.options")->onPress({}, {285, 190, 220, 40});
    compose();
    const auto *box = ui.find("menubox"), *scroll = ui.find("menuoverlay");
    require(box && scroll && box->frame.x >= 8 && box->frame.y >= 8 &&
                box->frame.x + box->frame.width <= screen.width - 7.9f &&
                box->frame.y + box->frame.height <= screen.height - 7.9f &&
                scroll->scrollMaxOffset > 0 && gui::menu.offset > 0 && visible(),
            "长列表越出窗口或打开时未显示当前项");
    require(ui.find("long.options.value")->clip && ui.find("menuitem63.text")->clip &&
                ui.find("menuoverlay.scroll.thumb") &&
                ui.find("menuoverlay.scroll.thumb")->preserveFocusOnPress,
            "长文本未裁剪，或滚动条会丢失原下拉框焦点");
    key(eui::InputKey::Home);
    require(gui::menu.highlighted == 0 && gui::menu.offset == 0 && visible(),
            "Home 未跟随至首项");
    key(eui::InputKey::Up);
    require(gui::menu.highlighted == 63 && visible(), "向上循环未显示末项");
    key(eui::InputKey::Down);
    require(gui::menu.highlighted == 0 && visible(), "向下循环未显示首项");
    key(eui::InputKey::End);
    require(gui::menu.highlighted == 63 && visible(), "End 未跟随至末项");
    const auto maximum = ui.find("menuoverlay")->scrollMaxOffset;
    ui.find("menuoverlay")->onScrollOffsetChanged(maximum / 2);
    compose();
    require(std::fabs(gui::menu.offset - maximum / 2) < .1f,
            "滚轮或拖动滚动条后错误地跳回高亮项");
    key(eui::InputKey::Up);
    require(gui::menu.highlighted == 62 && visible(), "滚动后键盘没有重新跟随高亮项");
    key(eui::InputKey::Enter);
    require(selected == 62 && changes == 1 && gui::menu.id.empty(),
            "键盘确认未只应用所选项一次");
    ui.find("long.options")->onPress({}, {8, 8, 220, 40});
    compose();
    key(eui::InputKey::Home);
    click(ui, "menuitem0");
    require(selected == 0 && changes == 2 && gui::menu.id.empty(),
            "点击菜单项未应用对应选项");
    compose();
    ui.find("long.options")->onPress({}, {8, 8, 220, 40});
    compose();
    key(eui::InputKey::End);
    items.resize(3);
    selected = 1;
    screen = {640, 480};
    compose();
    require(gui::menu.highlighted == 1 && gui::menu.offset == 0 &&
                std::fabs(ui.find("menubox")->frame.height - (3 * 38 + 8)) < .1f &&
                ui.find("menuoverlay")->scrollMaxOffset == 0 &&
                !ui.find("menuoverlay.scroll.thumb"),
            "列表缩短后未更新，或短菜单出现多余高度与滚动条");
    key(eui::InputKey::Escape);
    require(gui::menu.id.empty() && changes == 2, "Esc 修改了所选值");
    ui.find("long.options")->onPress({}, {8, 8, 220, 40});
    items.clear();
    compose();
    require(gui::menu.id.empty() && ui.find("long.options")->disabled &&
                !ui.find("long.options")->focusable && !ui.find("menubox") &&
                ui.find("long.options.value")->text == "暂无可选项",
            "空列表仍可操作或保留失效浮层");
    gui::menu = savedMenu;
    gui::state.pixelScale = savedScale;
    std::cout << "长下拉菜单屏幕边界、首尾键盘跟随、手动滚动、列表更新与空状态通过\n";
}

void shared_choice_controls(const msa::Result &generated) {
    const auto motion = gui::state.motion;
    const auto globalRevision = gui::state.revision;
    const auto require = [](bool value, const std::string &message) {
        if (!value)
            throw std::runtime_error("共享选项控件：" + message);
    };
    const std::vector<std::string> labels{"标准", "两列组合", "在操作员内部随机化"};
    for (const float width : {240.f, 292.f, 588.f, 1020.f}) {
        gui::State business;
        business.configDirty = false;
        business.result = generated;
        business.resultRevision = business.revision;
        business.status = gui::Status::Success;
        int selected = 0, choices = 0, checks = 0;
        bool checked = false, disabled = false;
        eui::Ui ui;
        const auto compose = [&] {
            ui.begin("shared-choice-controls");
            ui.column("shared.host")
                .width(width)
                .gap(12)
                .content([&] {
                    gui::subheading(ui, "shared.heading", "导出格式与运行顺序",
                                    "选择适合目标软件的导出格式。更改输出方式只影响导出排版，保留当"
                                    "前数据与统计结果。",
                                    width);
                    gui::choiceChips(
                        ui, "shared.choices", labels, selected, width,
                        [&](int next) {
                            selected = next;
                            ++choices;
                            business.outputChanged();
                        },
                        disabled);
                    gui::checkOption(
                        ui, "shared.check", "存储标准运行顺序", checked, width,
                        [&](bool next) {
                            checked = next;
                            ++checks;
                            business.outputChanged();
                        },
                        disabled);
                    ui.rect("shared.tail").size(width, 24).build();
                })
                .build();
            ui.end();
            ui.layout(width, 720);
        };
        const auto frames = [&] {
            std::map<std::string, eui::Rect> result;
            for (const auto *id : {"shared.heading", "shared.heading.title",
                                   "shared.heading.description", "shared.choices", "shared.check",
                                   "shared.check.hit", "shared.check.box", "shared.tail"}) {
                const auto *node = ui.find(id);
                require(node != nullptr, std::string("缺少控件锚点 ") + id);
                result[id] = {node->frame.x, node->frame.y, node->frame.width, node->frame.height};
            }
            for (int i = 0; i < 3; ++i)
                for (const auto *suffix : {".bg", ".text"}) {
                    const auto id = "shared.choices.item." + std::to_string(i) + suffix;
                    const auto *node = ui.find(id);
                    require(node != nullptr, "缺少选项或标签 " + id);
                    result[id] = {node->frame.x, node->frame.y, node->frame.width,
                                  node->frame.height};
                }
            return result;
        };
        const auto sameFrames = [&](const auto &before) {
            const auto after = frames();
            for (const auto &[id, value] : before) {
                const auto &current = after.at(id);
                require(std::fabs(current.x - value.x) < .01f &&
                            std::fabs(current.y - value.y) < .01f &&
                            std::fabs(current.width - value.width) < .01f &&
                            std::fabs(current.height - value.height) < .01f,
                        "选择、焦点、禁用或动效状态改变了布局：" + id);
            }
        };
        gui::state.motion = true;
        compose();
        controls_fit(ui, width, "共享选项控件窄屏布局");
        const auto baseline = frames();
        const auto *description = ui.find("shared.heading.description");
        const auto *heading = ui.find("shared.heading");
        require(description->wrap && description->frame.width <= width &&
                    description->frame.y + description->frame.height <=
                        heading->frame.y + heading->frame.height + .1f &&
                    ui.find("shared.choices")->frame.y >= heading->frame.y + heading->frame.height,
                "分组说明未换行或与后续选项重叠");
        if (width <= 292)
            require(ui.find("shared.choices.item.2.bg")->frame.y >
                        ui.find("shared.choices.item.0.bg")->frame.y,
                    "窄列短选项未自动换行");
        for (int i = 0; i < 3; ++i) {
            const auto id = "shared.choices.item." + std::to_string(i);
            const auto *item = ui.find(id + ".bg"), *label = ui.find(id + ".text");
            require(item->frame.height >= 36 && item->frame.x >= -.1f &&
                        item->frame.x + item->frame.width <= width + .1f &&
                        label->frame.x >= item->frame.x - .1f &&
                        label->frame.x + label->frame.width <=
                            item->frame.x + item->frame.width + .1f,
                    "选项命中区或标签越出所属按钮");
            for (int j = 0; j < i; ++j) {
                const auto &other =
                    ui.find("shared.choices.item." + std::to_string(j) + ".bg")->frame;
                const auto &current = item->frame;
                const float horizontal =
                    std::min(current.x + current.width, other.x + other.width) -
                    std::max(current.x, other.x);
                const float vertical =
                    std::min(current.y + current.height, other.y + other.height) -
                    std::max(current.y, other.y);
                require(horizontal <= .1f || vertical <= .1f, "换行后的可点击选项互相重叠");
            }
        }
        click(ui, "shared.choices.item.0.bg");
        require(choices == 0 && !business.configDirty, "重复选择当前项触发了配置变更");
        click(ui, "shared.choices.item.1.bg");
        require(selected == 1 && choices == 1, "选项点击没有只触发一次回调");
        click(ui, "shared.check.hit");
        click(ui, "shared.check.hit");
        require(checked && checks == 1, "同一帧复选回调未去重");
        compose();
        sameFrames(baseline);
        eui::KeyEvent release{eui::InputKey::Space};
        release.action = core::KeyAction::Release;
        require(!ui.find("shared.check.hit")->onKeyEvent(release) && checks == 1,
                "松开按键重复改变复选状态");
        require(ui.find("shared.check.hit")->onKeyEvent({eui::InputKey::Space}) && !checked &&
                    checks == 2,
                "空格不能切换复选状态");
        require(ui.find("shared.check.hit")->onKeyEvent({eui::InputKey::Enter}) && checked &&
                    checks == 3,
                "同帧连续键盘切换复用了旧状态");
        compose();
        sameFrames(baseline);
        disabled = true;
        compose();
        sameFrames(baseline);
        for (const auto *id : {"shared.choices.item.0.bg", "shared.check.hit"}) {
            const auto *hit = ui.find(id);
            require(hit->disabled && !hit->focusable, "禁用选项仍然可聚焦");
            if (hit->onClick)
                hit->onClick();
            require(!hit->onKeyEvent({eui::InputKey::Space}), "禁用选项接受了键盘操作");
        }
        require(selected == 1 && checked && choices == 1 && checks == 3,
                "禁用选项仍调用了业务回调");
        disabled = false;
        gui::state.motion = false;
        compose();
        sameFrames(baseline);
        const auto noGeometry = [&](const auto &self, const core::dsl::Element &node) -> void {
            require(!core::hasAnimProperty(node.transition.properties, core::AnimProperty::Frame) ||
                        !node.transition.enabled,
                    "常规选项错误地启用了位置或尺寸过渡");
            require(
                !core::hasAnimProperty(node.transition.properties, core::AnimProperty::Transform) ||
                    !node.transition.enabled,
                "常规选项错误地启用了缩放过渡");
            require(std::fabs(node.pressedScale - 1.f) < .001f, "按压改变了控件缩放");
            require(!node.transition.enabled || node.transition.durationSeconds == 0,
                    "关闭动效后子控件仍有过渡时长");
            for (const auto &child : node.children)
                self(self, *child);
        };
        for (const auto &root : ui.roots())
            noGeometry(noGeometry, *root);
        require(business.validResult() && business.revision == 0 &&
                    business.result->matrix == generated.matrix &&
                    business.result->metrics == generated.metrics &&
                    gui::state.revision == globalRevision,
                "仅调整展示/输出选项改变了研究状态或统计结果");
    }
    gui::state.motion = motion;
    std::cout << "共享分组、换行选项与复选框的窄屏边界、语义、禁用及无布局动效通过\n";
}

void shared_choice_keyboard() {
    for (const int initial : {-1, 0}) {
        core::dsl::Runtime runtime;
        runtime.initialize();
        eui::Ui *ui = nullptr;
        int selected = initial, changes = 0, forbidden = 0;
        bool checked = false;
        const auto compose = [&] {
            runtime.compose(
                "shared-choice-keyboard", 292, 400, [&](eui::Ui &view, const core::dsl::Screen &) {
                    ui = &view;
                    view.column("keyboard.host")
                        .width(292)
                        .gap(8)
                        .content([&] {
                            gui::choiceChips(view, "keyboard.choices",
                                             {"第一项", "第二项", "第三项"}, selected, 292,
                                             [&](int next) {
                                                 selected = next;
                                                 ++changes;
                                             });
                            gui::checkOption(view, "keyboard.check", "显示图例", checked, 292,
                                             [&](bool next) { checked = next; });
                            gui::choiceChips(
                                view, "keyboard.disabled.choices", {"不可用选项"}, 0, 292,
                                [&](int) { ++forbidden; }, true);
                            gui::checkOption(
                                view, "keyboard.disabled.check", "不可用复选框", false, 292,
                                [&](bool) { ++forbidden; }, true);
                            view.rect("keyboard.tail").size(292, 20).focusable().build();
                        })
                        .build();
                });
        };
        const auto key = [&](core::InputKey value, bool shift = false) {
            core::KeyEvent event{value};
            event.modifiers.shift = shift;
            core::queueKeyInput(nullptr, event);
            runtime.update(nullptr, 0, 1, 1);
            compose();
        };
        const auto expect = [&](int value) {
            if (selected != value ||
                !ui->isFocused("keyboard.choices.item." + std::to_string(value) + ".bg"))
                throw std::runtime_error("短选项选择状态与真实键盘焦点不一致");
        };
        compose();
        key(core::InputKey::Tab);
        if (!ui->isFocused("keyboard.choices.item.0.bg") || selected != initial || changes != 0)
            throw std::runtime_error("Tab 聚焦擅自选择了尚未选择的选项");
        key(core::InputKey::Enter);
        expect(0);
        const auto once = changes;
        key(core::InputKey::Space);
        expect(0);
        if (changes != once)
            throw std::runtime_error("重复激活短选项重复调用业务回调");
        key(core::InputKey::Right);
        expect(1);
        key(core::InputKey::End);
        expect(2);
        key(core::InputKey::Home);
        expect(0);
        key(core::InputKey::Left);
        expect(2);
        key(core::InputKey::Tab);
        if (!ui->isFocused("keyboard.check.hit"))
            throw std::runtime_error("Tab 没有离开互斥选项组");
        key(core::InputKey::Space);
        if (!checked)
            throw std::runtime_error("真实键盘派发不能勾选复选框");
        key(core::InputKey::Enter);
        if (checked)
            throw std::runtime_error("真实键盘派发不能取消勾选复选框");
        key(core::InputKey::Tab);
        if (!ui->isFocused("keyboard.tail") || forbidden)
            throw std::runtime_error("Tab 访问了禁用选项或错误触发禁用回调");
        key(core::InputKey::Tab, true);
        key(core::InputKey::Tab, true);
        expect(2);
        const auto target = ui->find("keyboard.choices.item.0.bg")->frame;
        core::queuePointerButton(nullptr, target.x + target.width / 2, target.y + target.height / 2,
                                 core::PointerButton::Left, core::PointerAction::Press, {});
        core::queuePointerButton(nullptr, target.x + target.width / 2, target.y + target.height / 2,
                                 core::PointerButton::Left, core::PointerAction::Release, {});
        runtime.update(nullptr, 0, 1, 1);
        compose();
        expect(0);
        core::queueKeyInput(nullptr, {core::InputKey::Right});
        core::queueKeyInput(nullptr, {core::InputKey::Right});
        runtime.update(nullptr, 0, 1, 1);
        compose();
        expect(2);
        ui->requestFocus("");
        runtime.update(nullptr, 0, 1, 1);
        core::detail::inputQueues().erase(nullptr);
        core::detail::pointerStates().erase(nullptr);
        runtime.shutdown(false);
    }
    std::cout << "短选项与复选框真实鼠标/键盘派发、单一Tab焦点、未选中和禁用语义通过\n";
}

void disclosure_and_reduced_motion(const msa::Result &generated) {
    const auto savedResult = gui::state.result;
    const auto savedStatus = gui::state.status;
    const auto savedResultRevision = gui::state.resultRevision;
    const auto savedAdvanced = gui::state.advanced;
    const auto savedFrom = gui::state.advancedFrom;
    const auto savedChanged = gui::state.advancedChanged;
    const auto savedMotion = gui::state.motion;
    const auto revision = gui::state.revision;
    gui::state.result = generated;
    gui::state.resultRevision = revision;
    gui::state.status = gui::Status::Success;
    gui::state.motion = true;
    gui::state.advanced = false;
    gui::state.advancedFrom = 0;
    gui::state.advancedChanged = gui::Clock::now() - std::chrono::seconds(1);
    eui::Ui ui;
    const auto compose = [&] {
        ui.begin("search-budget-disclosure");
        gui::searchSettings(ui, 588);
        ui.end();
        ui.layout(588, 800);
        controls_fit(ui, 588, "搜索预算展开布局");
    };
    compose();
    if (ui.find("advanced.reveal") || !ui.find("advancedtoggle.bg"))
        throw std::runtime_error("搜索预算收起状态错误或缺少展开按钮");
    click(ui, "advancedtoggle.bg");
    gui::state.advancedChanged = gui::Clock::now() + std::chrono::seconds(1);
    compose();
    const auto *opening = ui.find("advanced.reveal");
    if (!gui::state.advanced || !opening || !opening->disabled || opening->opacity != 1)
        throw std::runtime_error("展开过渡中输入未禁用或正文发生淡入");
    gui::state.advancedChanged = gui::Clock::now() - std::chrono::seconds(1);
    compose();
    const auto *expanded = ui.find("advanced.reveal");
    if (!expanded || expanded->disabled || !ui.find("attempts.hit") || !ui.find("timeout.hit"))
        throw std::runtime_error("展开结束后搜索参数不可操作");
    click(ui, "advancedtoggle.bg");
    gui::state.advancedChanged = gui::Clock::now() + std::chrono::seconds(1);
    compose();
    const auto *closing = ui.find("advanced.reveal");
    if (gui::state.advanced || !closing || !closing->disabled || closing->opacity != 1)
        throw std::runtime_error("收起过渡中的隐藏输入仍可操作或整组淡出");
    gui::state.motion = false;
    compose();
    if (ui.find("advanced.reveal"))
        throw std::runtime_error("关闭动效后收起搜索预算仍保留过渡内容");
    click(ui, "advancedtoggle.bg");
    compose();
    if (!ui.find("advanced.reveal") || ui.find("advanced.reveal")->disabled)
        throw std::runtime_error("关闭动效后展开搜索参数未立即可用");
    if (!gui::state.validResult() || gui::state.revision != revision ||
        gui::state.result->matrix != generated.matrix)
        throw std::runtime_error("显示或隐藏搜索预算改变了已有研究结果");
    int changes = 0;
    eui::Ui reduced;
    reduced.begin("run-order-reduced-motion");
    reduced.column("run-order-host")
        .width(588)
        .gap(8)
        .content([&] {
            gui::runOrderChoice(reduced, "reduced.radio", "不随机化", true, true, false, 588,
                                [&](bool) { ++changes; });
            gui::runOrderChoice(reduced, "reduced.check", "存储标准运行顺序", true, false, false,
                                588, [&](bool) { ++changes; });
            gui::runOrderChoice(reduced, "reduced.disabled", "不可用运行顺序", true, true, true,
                                588, [&](bool) { ++changes; });
        })
        .build();
    reduced.end();
    reduced.layout(588, 300);
    for (const auto *id :
         {"reduced.radio.inner", "reduced.check.mark.short", "reduced.check.mark.long"})
        if (!reduced.find(id) || reduced.find(id)->transition.durationSeconds != 0)
            throw std::runtime_error("关闭动效后运行顺序标记仍保留独立动画");
    const auto *disabled = reduced.find("reduced.disabled.hit");
    if (!disabled || disabled->focusable || disabled->onKeyEvent({eui::InputKey::Enter}))
        throw std::runtime_error("禁用运行顺序仍可聚焦或键盘激活");
    disabled->onClick();
    if (changes)
        throw std::runtime_error("禁用运行顺序触发了业务回调");
    gui::state.result = savedResult;
    gui::state.resultRevision = savedResultRevision;
    gui::state.status = savedStatus;
    gui::state.advanced = savedAdvanced;
    gui::state.advancedFrom = savedFrom;
    gui::state.advancedChanged = savedChanged;
    gui::state.motion = savedMotion;
    std::cout << "搜索预算展开/收起保持数据，隐藏输入禁用，减少动效同时关闭运行顺序标记动画\n";
}

void logs_and_about_layout() {
    const auto savedLogs = gui::state.logs;
    const auto savedFilter = gui::state.logFilter;
    const auto savedOffset = gui::state.logOffset;
    const auto savedPageOffset = gui::state.logPageOffset;
    const auto savedFollow = gui::state.followLogs;
    const auto savedAboutOffset = gui::state.aboutOffset;
    gui::state.aboutOffset = 0;
    std::error_code licenseError;
    const bool hasLicenseDirectory = std::filesystem::is_directory(
        platform::executableDirectory() / "licenses", licenseError);
    gui::state.logs = {{201, "12:00:01", gui::Level::Info, "Information record"},
                       {202, "12:00:02", gui::Level::Warning, "Warning record"},
                       {203, "12:00:03", gui::Level::Error, "Error record"}};
    const auto shortLogs = gui::state.logs;
    const auto bottom = [](const core::dsl::Element &node) {
        return node.frame.y + node.frame.height;
    };
    constexpr float logTolerance = .1f;
    const auto checkLogLayout = [&](const eui::Ui &ui, float width, float height, bool compact) {
        controls_fit(ui, width, "log page");
        const auto *title = ui.find("logs.heading.copy");
        const auto *controls = ui.find("logs.controls.card");
        const auto *toolbar = ui.find("logs.toolbar");
        const auto *table = ui.find("logs.card");
        const auto *header = ui.find("logs.table.heading");
        const auto *scroll = ui.find("logscroll");
        if (ui.find("logs.page.scroll") || !title || !controls || !toolbar || !table || !header ||
            !scroll || !contains_element(*controls, *title) ||
            !contains_element(*controls, *toolbar) ||
            bottom(*controls) > table->frame.y + logTolerance ||
            bottom(*header) > scroll->frame.y + logTolerance ||
            bottom(*scroll) > bottom(*table) + logTolerance ||
            bottom(*table) > height + logTolerance || contains_element(*scroll, *header))
            throw std::runtime_error("log title, toolbar, and fixed table header do not fit");
        const auto *filter = ui.find("logfilter");
        const auto *follow = ui.find("logfollow.hit");
        const auto *copy = ui.find("copylogs.bg");
        if (!filter || !follow || !copy || !follow->onKeyEvent ||
            !contains_element(*toolbar, *filter) || !contains_element(*toolbar, *follow) ||
            !contains_element(*toolbar, *copy))
            throw std::runtime_error("log toolbar omits filter, follow, or copy controls");
        if (compact ? bottom(*title) > toolbar->frame.y + logTolerance
                    : (title->frame.x + title->frame.width > toolbar->frame.x + logTolerance ||
                       std::max(title->frame.y, toolbar->frame.y) >=
                           std::min(bottom(*title), bottom(*toolbar))))
            throw std::runtime_error("log toolbar does not adapt to window width");
    };
    for (float width : {588.f, 1020.f, 1232.f}) {
        const bool compact = width < 700;
        const bool compactLog = width < 880;
        const float logHeight = compactLog ? 332.f : 636.f;
        float shortTableHeight = 0;
        for (int filter = 0; filter < 3; ++filter) {
            gui::state.logFilter = (filter + 1) % 3;
            gui::state.followLogs = true;
            gui::state.logOffset = 60;
            eui::Ui select;
            select.begin("log-filter-selection");
            gui::logsPage(select, width, logHeight);
            select.end();
            select.layout(width, logHeight);
            click(select, "logfilter.hit." + std::to_string(filter));
            if (gui::state.logFilter != filter || gui::state.followLogs ||
                gui::state.logOffset != 0)
                throw std::runtime_error("log filtering retains old scroll/follow state");
            eui::Ui list;
            list.begin("log-filter-content");
            gui::logsPage(list, width, logHeight);
            list.end();
            list.layout(width, logHeight);
            checkLogLayout(list, width, logHeight, compactLog);
            for (int level = 0; level < 3; ++level) {
                const auto id = "log." + std::to_string(201 + level) + ".message";
                const auto *message = list.find(id);
                if (bool(message) != (filter == 0 || level == filter))
                    throw std::runtime_error("log filter shows a record from the wrong severity");
                const auto *messageHeading = list.find("logs.table.message");
                if (message && (!messageHeading ||
                                std::fabs(message->frame.x - messageHeading->frame.x) > logTolerance))
                    throw std::runtime_error("log message column is misaligned with its header");
                const auto *label = list.find("logfilter.label." + std::to_string(level));
                constexpr std::array<const char *, 3> labels{"全部 3", "警告 1", "错误 1"};
                if (!label || label->text != labels[level])
                    throw std::runtime_error("log filter counts do not match the available records");
            }
            const auto *copy = list.find("copylogs.bg");
            if (!copy || copy->disabled)
                throw std::runtime_error("nonempty log selection cannot be copied");
            if ((!compactLog || filter != 0) &&
                list.find("logscroll")->scrollMaxOffset > logTolerance)
                throw std::runtime_error("a few short log records should fit without scrolling");
            if (filter != 0)
                shortTableHeight = std::max(shortTableHeight, list.find("logs.card")->frame.height);
        }

        std::string longMessage;
        for (int i = 0; i < 16; ++i)
            longMessage += "无法导出测量记录，请检查保存路径及文件权限。";
        gui::state.logs.front().message = longMessage;
        for (unsigned sequence = 204; sequence < 228; ++sequence)
            gui::state.logs.push_back({sequence, "12:00:04", gui::Level::Info, "Additional record"});
        gui::state.logFilter = 0;
        gui::state.logOffset = 0;
        gui::state.followLogs = false;
        eui::Ui longList;
        longList.begin("log-scroll-content");
        gui::logsPage(longList, width, logHeight);
        longList.end();
        longList.layout(width, logHeight);
        checkLogLayout(longList, width, logHeight, compactLog);
        const auto *scroll = longList.find("logscroll");
        const auto *wrapped = longList.find("log.201.message");
        const auto *next = longList.find("log.202.message");
        if (scroll->scrollMaxOffset <= 0 || longList.find("logs.card")->frame.height <= shortTableHeight ||
            !wrapped || !next || !wrapped->wrap || wrapped->frame.height <= next->frame.height ||
            wrapped->text != longMessage || bottom(*wrapped) > next->frame.y + logTolerance ||
            !scroll->onScrollOffsetChanged)
            throw std::runtime_error("long log messages do not wrap or stay inside a scrolling table");
        scroll->onScrollOffsetChanged(scroll->scrollMaxOffset);
        if (gui::state.logOffset != scroll->scrollMaxOffset || !gui::state.followLogs)
            throw std::runtime_error("scrolling down to the latest log does not enable following");
        gui::state.logs = shortLogs;

        const float aboutHeight = compact ? 332.f : 636.f;
        eui::Ui about;
        about.begin("about-responsive-layout");
        gui::aboutPage(about, width, aboutHeight);
        about.end();
        about.layout(width, aboutHeight);
        controls_fit(about, width, "about page");
        const auto *viewport = about.find("aboutscroll");
        const auto *mark = about.find("about.mark");
        const auto *title = about.find("about.title");
        const auto *version = about.find("about.version.text");
        const auto *platform = about.find("about.platform.text");
        const auto *project = about.find("about.project.card");
        const auto *dependencies = about.find("about.dependencies.card");
        const auto *environment = about.find("about.environment.card");
        const auto *dpi = about.find("about.dpi");
        if (!viewport || !project || !dependencies || !environment || project->frame.height <= 0 ||
            dependencies->frame.height <= 0 || environment->frame.height <= 0)
            throw std::runtime_error("About page is missing a card or its scroll region");
        if (!mark || mark->frame.width < 48 || mark->frame.height < 48 || !title ||
            title->fontSize < 25 || title->text.find("MSA 数据生成器") == std::string::npos ||
            !version || version->text.find(MSA_UI_VERSION) == std::string::npos || !platform ||
            platform->text.find("Windows x64") == std::string::npos || !dpi ||
            dpi->text.find(std::to_string(gui::state.systemDpi)) == std::string::npos)
            throw std::runtime_error("About header or environment loses readable identity metadata");
        const auto &p = project->frame;
        const auto &d = dependencies->frame;
        const auto &e = environment->frame;
        constexpr float tolerance = .1f;
        if (std::abs(2 * viewport->frame.x + viewport->frame.width - width) > tolerance ||
            std::abs(p.x - viewport->frame.x) > tolerance)
            throw std::runtime_error("About scroll region is detached from the centered cards");
        const float visibleBottom = viewport->frame.y + viewport->frame.height;
        const float reachableBottom = visibleBottom + viewport->scrollMaxOffset;
        const float detailRight = std::max(p.x + p.width, d.x + d.width);
        if (compact) {
            if (std::fabs(p.x - d.x) > tolerance || std::fabs(p.width - d.width) > tolerance ||
                d.y < p.y + p.height - tolerance)
                throw std::runtime_error("compact About cards do not stack in one column");
        } else if (std::fabs(p.y - d.y) > tolerance || std::fabs(p.height - d.height) > tolerance ||
                   d.x < p.x + p.width - tolerance) {
            throw std::runtime_error("desktop About cards do not share aligned top and bottom edges");
        }
        if (std::fabs(p.x - e.x) > tolerance ||
            std::fabs(detailRight - e.x - e.width) > tolerance ||
            e.y < std::max(p.y + p.height, d.y + d.height) - tolerance ||
            e.y + e.height > reachableBottom + tolerance)
            throw std::runtime_error("About environment card does not align below the detail cards");
        for (const auto *id : {"copyversion.bg", "about.author.row.open.bg",
                               "about.repository.row.open.bg"}) {
            const auto *action = about.find(id);
            if (!action || action->disabled || !action->onClick || !action->focusable ||
                !action->onKeyEvent || action->frame.height < 32 ||
                action->frame.y < viewport->frame.y - tolerance ||
                action->frame.y + action->frame.height > reachableBottom + tolerance)
                throw std::runtime_error(std::string("unreachable About page action: ") + id);
        }
        const auto *copyVersion = about.find("copyversion.bg");
        if (copyVersion->frame.y + copyVersion->frame.height > visibleBottom + tolerance)
            throw std::runtime_error("copying version information requires scrolling past the header");
        const auto *openLicense = about.find("about.license.row.open.bg");
        if (!openLicense || openLicense->disabled == hasLicenseDirectory ||
            (hasLicenseDirectory && (!openLicense->onClick || !openLicense->onKeyEvent)) ||
            openLicense->frame.y + openLicense->frame.height > reachableBottom + tolerance)
            throw std::runtime_error("About license directory action does not match availability");
        const auto *author = about.find("about.author");
        const auto *repository = about.find("about.repository");
        if (!author || !repository || author->text.find(msa::project_author) == std::string::npos ||
            repository->text.find(msa::project_repository) == std::string::npos)
            throw std::runtime_error("responsive About page loses project attribution");
        const auto *license = about.find("about.license.row.title");
        const auto *licenseSummary = about.find("about.projectlicense");
        if (!license || license->text != "MIT License" || !licenseSummary ||
            licenseSummary->text.find("允许商业使用") == std::string::npos ||
            licenseSummary->text.find("版权声明与许可声明") == std::string::npos ||
            licenseSummary->text.find("须公开") != std::string::npos)
            throw std::runtime_error("About page does not describe MIT permissions and notices");
    }
    constexpr float smallWidth = 456.f, smallHeight = 137.f;
    gui::state.logs = shortLogs;
    gui::state.logFilter = 0;
    gui::state.logOffset = 0;
    gui::state.logPageOffset = 0;
    const auto smallLogs = [smallWidth, smallHeight](eui::Ui &ui) {
        ui.begin("log-content-in-small-work-area");
        gui::logsPage(ui, smallWidth, smallHeight);
        ui.end();
        ui.layout(smallWidth, smallHeight);
    };
    eui::Ui smallUi, scrolled;
    smallLogs(smallUi);
    controls_fit(smallUi, smallWidth, "high DPI log page");
    const auto *outer = smallUi.find("logs.page.scroll");
    const auto *inner = smallUi.find("logscroll");
    const auto *filters = smallUi.find("logfilter");
    const auto *actions = smallUi.find("logs.toolbar.actions");
    if (!outer || !inner || !outer->onScrollOffsetChanged || outer->frame.height != smallHeight ||
        outer->scrollMaxOffset <= 0 || inner->scrollMaxOffset <= 0 ||
        !contains_element(*outer, *inner) ||
        bottom(*inner) - outer->scrollMaxOffset > bottom(*outer) + logTolerance)
        throw std::runtime_error("高 DPI 小工作区无法滚动到完整的日志记录区");
    if (!filters || !actions || filters->frame.width != actions->frame.width ||
        bottom(*filters) > actions->frame.y + logTolerance)
        throw std::runtime_error("高 DPI 小工作区的日志筛选和操作未垂直排列");
    outer->onScrollOffsetChanged(outer->scrollMaxOffset);
    smallLogs(scrolled);
    const auto *moved = scrolled.find("logs.page.scroll");
    if (!moved || moved->scrollOffset != outer->scrollMaxOffset || gui::state.logOffset != 0)
        throw std::runtime_error("日志页外层滚动偏移未保留或覆盖了记录区滚动偏移");
    gui::state.logs.resize(1);
    gui::state.logFilter = 2;
    gui::state.logOffset = 0;
    eui::Ui empty;
    empty.begin("empty-log-selection");
    gui::logsPage(empty, 588, 332);
    empty.end();
    empty.layout(588, 332);
    checkLogLayout(empty, 588, 332, true);
    if (!empty.find("logs.empty") || !empty.find("copylogs.bg")->disabled ||
        empty.find("logscroll")->scrollMaxOffset > logTolerance)
        throw std::runtime_error(
            "empty log filter does not disable copy or explain its empty state");
    gui::state.logs = savedLogs;
    gui::state.logFilter = savedFilter;
    gui::state.logOffset = savedOffset;
    gui::state.logPageOffset = savedPageOffset;
    gui::state.followLogs = savedFollow;
    gui::state.aboutOffset = savedAboutOffset;
    std::cout << "log filters and responsive About cards fit tested widths and retain attribution\n";
}

void help_destinations() {
    const auto page = gui::state.page;
    const auto topic = gui::state.helpTopic;
    const auto tab = gui::state.optionsTab;
    const auto revision = gui::state.revision;
    const auto configOffset = gui::state.configOffset;
    const auto helpOffset = gui::state.helpOffset;
    const std::array<const char *, 4> buttons{"help.rules.open.bg", "help.analysis.open.bg",
                                              "help.files.open.bg", "help.filename.open.bg"};
    for (int i = 0; i < int(buttons.size()); ++i) {
        gui::state.navigate(gui::Page::Help);
        gui::state.helpTopic = i;
        gui::state.helpOffset = 0;
        gui::state.configOffset = 120;
        gui::state.hint("copied");
        eui::Ui help;
        help.begin("help-destination");
        gui::helpPage(help, 588, 500);
        help.end();
        help.layout(588, 500);
        click(help, buttons[i]);
        if (gui::state.page != gui::Page::Options || gui::state.optionsTab != std::min(i, 2) ||
            gui::state.configOffset != 0 || !gui::state.footerMessage.empty() ||
            gui::state.revision != revision)
            throw std::runtime_error(
                "help shortcut misses its settings category or changes the study");
    }
    gui::state.navigate(page);
    gui::state.helpTopic = topic;
    gui::state.optionsTab = tab;
    gui::state.configOffset = configOffset;
    gui::state.helpOffset = helpOffset;
    std::cout << "help shortcuts reach the matching settings without changing the study\n";
}

void runtime_keyboard_navigation() {
    // 使用真实 Runtime 输入派发，避免只调用回调而漏掉原生焦点不同步的问题。
    for (const bool segmented : {false, true}) {
        core::dsl::Runtime runtime;
        runtime.initialize();
        eui::Ui *ui = nullptr;
        int selected = 0;
        const auto compose = [&] {
            runtime.compose(
                "keyboard-navigation", 420, 240, [&](eui::Ui &view, const core::dsl::Screen &) {
                    ui = &view;
                    view.column("content")
                        .width(420)
                        .gap(8)
                        .content([&] {
                            const auto change = [&](int next) { selected = next; };
                            if (segmented)
                                gui::segments(view, "navigation", {"One", "Two", "Three"}, selected,
                                              360, change);
                            else
                                gui::tabs(view, "navigation", {"One", "Two", "Three"}, selected,
                                          360, change);
                            view.column("disabled")
                                .disabled()
                                .content([&] {
                                    view.rect("disabled.target").size(100, 20).focusable().build();
                                })
                                .build();
                            view.column("hidden")
                                .opacity(0)
                                .content([&] {
                                    view.rect("hidden.target").size(100, 20).focusable().build();
                                })
                                .build();
                            view.rect("decoration").size(100, 20).build();
                            view.rect("tail").size(100, 20).focusable().build();
                        })
                        .build();
                });
        };
        const auto key = [&](core::InputKey value, bool shift = false) {
            core::KeyEvent event{value};
            event.modifiers.shift = shift;
            core::queueKeyInput(nullptr, event);
            runtime.update(nullptr, 0, 1, 1);
            compose();
        };
        const auto expect = [&](int expected) {
            if (selected != expected ||
                !ui->isFocused("navigation.hit." + std::to_string(expected)))
                throw std::runtime_error("tab/segment selection and runtime focus diverge");
        };
        compose();
        key(core::InputKey::Tab);
        expect(0);
        key(core::InputKey::Right);
        expect(1);
        key(core::InputKey::Space);
        expect(1);
        key(core::InputKey::End);
        expect(2);
        key(core::InputKey::Home);
        expect(0);
        key(core::InputKey::Left);
        expect(2);
        key(core::InputKey::Tab);
        if (!ui->isFocused("tail"))
            throw std::runtime_error("Tab visits unselected, disabled or hidden controls");
        key(core::InputKey::Tab, true);
        expect(2);

        for (const auto *target : {"disabled.target", "hidden.target", "decoration", "missing"}) {
            ui->requestFocus(target);
            runtime.update(nullptr, 0, 1, 1);
            expect(2);
        }
        const auto frame = ui->find("navigation.hit.0")->frame;
        core::queuePointerButton(nullptr, frame.x + frame.width / 2, frame.y + frame.height / 2,
                                 core::PointerButton::Left, core::PointerAction::Press, {});
        core::queuePointerButton(nullptr, frame.x + frame.width / 2, frame.y + frame.height / 2,
                                 core::PointerButton::Left, core::PointerAction::Release, {});
        runtime.update(nullptr, 0, 1, 1);
        compose();
        expect(0);
        key(core::InputKey::Space);
        expect(0);
        // 同一帧可能积累多个按键，后一个按键必须使用前一个操作后的选中状态。
        core::queueKeyInput(nullptr, {core::InputKey::Right});
        core::queueKeyInput(nullptr, {core::InputKey::Right});
        runtime.update(nullptr, 0, 1, 1);
        compose();
        expect(2);
        core::queueKeyInput(nullptr, {core::InputKey::Left});
        core::queueKeyInput(nullptr, {core::InputKey::Space});
        runtime.update(nullptr, 0, 1, 1);
        compose();
        expect(1);
        ui->requestFocus("");
        runtime.update(nullptr, 0, 1, 1);
        if (ui->isFocused("navigation.hit.1"))
            throw std::runtime_error("an explicit focus clear is ignored");
        core::detail::inputQueues().erase(nullptr);
        core::detail::pointerStates().erase(nullptr);
        runtime.shutdown(false);
    }
    std::cout
        << "runtime keyboard and pointer navigation preserve focus and skip disabled controls\n";
}
void chart_interaction(const msa::Result &generated, const std::filesystem::path &multiSheetWorkbook) {
    const auto savedDraft = gui::state.draft;
    const auto savedPage = gui::state.page;
    const auto revision = gui::state.revision;
    gui::visual = gui::VisualState{};
    gui::state.draft = native_ui::from_config(generated.config);
    gui::state.result = generated;
    gui::state.resultRevision = revision;
    gui::state.status = gui::Status::Success;
    gui::state.page = gui::Page::Charts;
    gui::state.message.clear();
    const auto require = [](bool value, const std::string &message) {
        if (!value)
            throw std::runtime_error("图表交互：" + message);
    };
    // GUI 的中文字体遵循项目统一的字号换算；逻辑行高和场景文字框保持不变。
    msa::charts::Scene typeScene{400, 300, {}};
    msa::charts::Primitive typeLabel;
    typeLabel.type = msa::charts::Primitive::Type::Text;
    typeLabel.x = 20;
    typeLabel.y = 10;
    typeLabel.x2 = 250;
    typeLabel.y2 = 36;
    typeLabel.font_size = 16;
    typeLabel.text = "中文图表字号检查";
    typeScene.items.push_back(typeLabel);
    eui::Ui typeUi;
    typeUi.begin("chart-typography");
    gui::drawChartScene(typeUi, "chart-type", typeScene);
    typeUi.end();
    typeUi.layout(400, 300);
    const auto *typeNode = typeUi.find("chart-type.p0");
    require(typeNode && std::fabs(typeNode->fontSize - 16.f * 1.35f) < .001f &&
                std::fabs(typeNode->lineHeight - 16.f * 1.35f) < .001f &&
                std::fabs(typeNode->frame.height - 36.f) < .001f,
            "预览文字未使用统一字号，或换算误改了场景文字框高度");
    // 长文件名、空数据和异常快照都占用同一个说明槽，不推移后续图表。
    for (const float width : {292.f, 588.f, 1020.f}) {
        float baseline = 0;
        for (int source = 0; source < 4; ++source) {
            gui::visual.importedSelected = source != 0;
            gui::visual.imported = generated;
            gui::visual.importedName = std::string(300, 'a') + ".csv";
            if (source == 2)
                gui::visual.imported.reset();
            else if (source == 3)
                gui::visual.imported->config = msa::Json::object();
            eui::Ui ui;
            ui.begin("chart-source-summary");
            ui.column("summary-host")
                .width(width)
                .gap(8)
                .content([&] {
                    gui::insightSourceSummary(ui, "summary", width);
                    gui::button(ui, "summary.next", "后续图表", width, [] {});
                })
                .build();
            ui.end();
            ui.layout(width, 200);
            const auto *summary = ui.find("summary.note"), *next = ui.find("summary.next.bg");
            require(summary && next && std::fabs(summary->frame.height - 44.f) < .01f,
                    "数据源说明未保留固定高度");
            if (!source)
                baseline = next->frame.y;
            require(std::fabs(next->frame.y - baseline) < .01f,
                    "切换数据源或长文件名推移了后续内容");
            controls_fit(ui, width, "图表数据源说明");
        }
    }
    gui::visual.imported.reset();
    gui::visual.importedName.clear();
    gui::visual.importedSelected = false;
    const auto editId = [](int index) {
        return "chart.edit." + std::to_string(index) + "." +
               std::to_string(gui::visual.editEpoch[index]);
    };
    eui::Ui editorUi;
    const auto composeEditor = [&](int index) {
        gui::visual.selected = index;
        editorUi.begin("chart-edit-interaction");
        gui::chartAppearance(editorUi, 620);
        editorUi.end();
        editorUi.layout(620, 1800);
    };
    const auto input = [&](const std::string &id, const std::string &value) {
        auto *node = editorUi.find(id + ".hit");
        require(node && node->onKeyEvent && node->onTextInput, "编辑输入控件不存在");
        eui::KeyEvent select;
        select.key = eui::InputKey::A;
        select.modifiers.control = true;
        node->onKeyEvent(select);
        eui::TextInputEvent text;
        text.text = value;
        node->onTextInput(text);
    };
    const auto originalEditor = gui::chartEditor(0);
    gui::visual.editing = true;
    const auto sceneFrames = [&] {
        std::array<eui::Rect, 3> frames;
        const std::array<float, 3> widths{588, 1020, 1232};
        for (std::size_t i = 0; i < widths.size(); ++i) {
            eui::Ui ui;
            ui.begin("chart-draft-stable-preview");
            gui::chartsPage(ui, widths[i], 560);
            ui.end();
            ui.layout(widths[i], 560);
            const auto *scene = ui.find("charts.scene.0");
            const auto *status = ui.find("charts.preview.draft");
            require(scene && status && std::fabs(status->frame.height - 44.f) < .01f,
                    "预览缺少固定高度的草稿状态槽");
            frames[i] = {scene->frame.x, scene->frame.y, scene->frame.width, scene->frame.height};
        }
        return frames;
    };
    const auto sameSceneFrames = [&](const auto &before, const auto &after) {
        for (std::size_t i = 0; i < before.size(); ++i)
            require(std::fabs(before[i].x - after[i].x) < .01f &&
                        std::fabs(before[i].y - after[i].y) < .01f &&
                        std::fabs(before[i].width - after[i].width) < .01f &&
                        std::fabs(before[i].height - after[i].height) < .01f,
                    "草稿输入或应用导致预览图形下跳或改变尺寸");
    };
    const auto beforeDraftFrames = sceneFrames();
    composeEditor(0);
    input(editId(0) + ".title", "已应用的研究图表");
    input(editId(0) + ".min", "-");
    require(gui::visual.editDirty[0] && gui::visual.lower[0] == "-" &&
                gui::sameChartEditor(gui::chartEditor(0), originalEditor),
            "半输入坐标或标题修改了已生效设置");
    const auto draftEpoch = gui::visual.editEpoch[0];
    for (const bool expected : {false, true}) {
        eui::Ui page;
        page.begin("chart-edit-disclosure");
        gui::chartsPage(page, 1020, 560);
        page.end();
        page.layout(1020, 560);
        click(page, "charts.edit.bg");
        require(gui::visual.editing == expected && gui::visual.editDirty[0] &&
                    gui::visual.drafts[0].title == "已应用的研究图表" &&
                    gui::visual.lower[0] == "-" && gui::visual.editEpoch[0] == draftEpoch &&
                    gui::sameChartEditor(gui::chartEditor(0), originalEditor),
                "收起或展开图表编辑丢失草稿或应用了未完成参数");
    }
    sameSceneFrames(beforeDraftFrames, sceneFrames());
    require(gui::chartPreview(generated, 720, 448) != nullptr, "未完成坐标输入使原预览消失");
    require(!gui::applyChartEdits(0) && !gui::visual.editError[0].empty() &&
                gui::sameChartEditor(gui::chartEditor(0), originalEditor) &&
                gui::state.result->matrix == generated.matrix,
            "非法草稿未保留数据和已生效外观");
    composeEditor(0);
    input(editId(0) + ".min", "0");
    input(editId(0) + ".max", "120");
    require(gui::applyChartEdits(0) && !gui::visual.editDirty[0] &&
                gui::chartEditor(0).title == "已应用的研究图表" && gui::chartEditor(0).y_min == 0 &&
                gui::chartEditor(0).y_max == 120,
            "有效编辑未正确应用");
    sameSceneFrames(beforeDraftFrames, sceneFrames());
    composeEditor(1);
    require(editorUi.find(editId(1) + ".title.hit") && !editorUi.find(editId(0) + ".title.hit") &&
                gui::visual.drafts[1].title == gui::visual.editors[1].title,
            "切换图表复用了另一图表输入状态");
    input(editId(1) + ".title", "第二张图的独立草稿");
    composeEditor(0);
    require(gui::visual.drafts[0].title == "已应用的研究图表" &&
                gui::visual.drafts[1].title == "第二张图的独立草稿",
            "跨图切换丢失或混用了草稿");
    const auto oldEpoch = gui::visual.editEpoch[1];
    gui::discardChartEdits(1);
    require(!gui::visual.editDirty[1] && gui::visual.editEpoch[1] > oldEpoch &&
                gui::visual.drafts[1].title == gui::visual.editors[1].title &&
                gui::visual.editors[0].title == "已应用的研究图表",
            "撤销未隔离当前图表或未重置输入控件状态");

    gui::visual.selected = 1;
    require(gui::chartPreview(generated, 720, 448) != nullptr, "有效预览未建立");
    auto changed = generated;
    changed.matrix[0][0] += .01;
    changed.metrics = msa::calculate(changed.matrix, changed.config);
    require(gui::chartPreview(changed, 720, 448) &&
                gui::visual.preview.snapshot->matrix == changed.matrix,
            "同配置新数据未使缓存失效");
    gui::visual.importedSelected = true;
    require(gui::chartPreview(generated, 720, 448) && gui::visual.preview.imported &&
                gui::visual.preview.snapshot->matrix == generated.matrix,
            "切换来源未刷新缓存内容");
    auto invalid = generated;
    invalid.matrix[0][0] = std::numeric_limits<double>::quiet_NaN();
    const auto logsBefore = gui::state.logSequence;
    require(gui::chartPreview(invalid, 720, 448) == nullptr, "无效图表数据未被拒绝");
    const auto logsAfter = gui::state.logSequence;
    require(logsAfter == logsBefore + 1, "首次渲染错误缺少日志或被重复记录");
    for (int frame = 0; frame < 4; ++frame)
        require(gui::chartPreview(invalid, 720, 448) == nullptr, "失败缓存出现旧图");
    require(gui::state.logSequence == logsAfter, "同一渲染错误逐帧重复写日志");
    gui::visual.importedSelected = false;
    require(gui::chartPreview(generated, 720, 448) && gui::visual.errorKind.empty(),
            "恢复有效数据后渲染错误未清除");

    gui::visual.scope = 1;
    eui::Ui outputUi;
    const auto composeOutput = [&] {
        outputUi.begin("chart-selection");
        gui::chartExportSettings(outputUi, 620);
        outputUi.end();
        outputUi.layout(620, 2200);
    };
    const auto outputAnchors = [&] {
        std::array<eui::Rect, 3> frames;
        const std::array<const char *, 3> ids{"chart.selection.empty", "chart.file.options.bg",
                                              "chart.output.note"};
        for (std::size_t i = 0; i < ids.size(); ++i) {
            const auto *node = outputUi.find(ids[i]);
            require(node != nullptr, "组合选择缺少固定提示或保存位置");
            frames[i] = {node->frame.x, node->frame.y, node->frame.width, node->frame.height};
        }
        require(std::fabs(frames[0].height - 26.f) < .01f &&
                    std::fabs(frames[2].height - 44.f) < .01f,
                "组合提示或格式说明未保留固定高度");
        return frames;
    };
    composeOutput();
    const auto populatedOutput = outputAnchors();
    click(outputUi, "chart.selection.none.bg");
    require(gui::selectedChartCount() == 0, "清空组合未生效");
    composeOutput();
    sameSceneFrames(populatedOutput, outputAnchors());
    eui::Ui footerUi;
    footerUi.begin("chart-empty-selection-footer");
    gui::footer(footerUi, 620);
    footerUi.end();
    footerUi.layout(620, 76);
    const auto *disabledExport = footerUi.find("charts.export.bg");
    const auto neutralBorder = eui::Color(gui::line);
    require(disabledExport && disabledExport->disabled && !disabledExport->focusable &&
                disabledExport->border.color.r == neutralBorder.r &&
                disabledExport->border.color.g == neutralBorder.g &&
                disabledExport->border.color.b == neutralBorder.b,
            "空组合的导出按钮仍可操作或使用启用态边框");
    gui::exportVisuals();
    require(!gui::visual.busy() && gui::visual.errorKind == "export", "空组合启动了导出");
    click(outputUi, "chart.selection.all.bg");
    require(gui::selectedChartCount() == 7, "全选组合未生效");
    composeOutput();
    click(outputUi, "chart.select.2.hit");
    require(gui::selectedChartCount() == 6 && !gui::visual.included[2], "组合图表复选框未更新选择");
    click(outputUi, "chart.format.hit.2");
    composeOutput();
    require(gui::visual.format == 2 &&
                outputUi.find("chart.output.note")->text.find("PDF") != std::string::npos,
            "导出格式选择未更新格式说明");
    sameSceneFrames(populatedOutput, outputAnchors());
    click(outputUi, "chart.scope.hit.0");
    composeOutput();
    require(gui::visual.scope == 0 && !outputUi.find("chart.select.2.hit"),
            "单图模式仍显示组合选择");
    click(outputUi, "chart.scope.hit.1");
    composeOutput();
    require(gui::visual.scope == 1 && !gui::visual.included[2] && gui::state.revision == revision &&
                gui::state.validResult() && gui::state.result->matrix == generated.matrix &&
                gui::state.result->metrics == generated.metrics,
            "切换导出范围丢失组合选择或改变了有效数据");
    click(outputUi, "chart.selection.all.bg");
    const auto included = gui::visual.included;
    gui::resetAllCharts();
    require(gui::visual.included == included && !gui::visual.editDirty[0] &&
                gui::sameChartEditor(gui::chartEditor(0), originalEditor),
            "重置全部外观改变了组合选择或未恢复默认值");
    gui::clearVisualError();

    for (const float width : {588.f, 1020.f, 1232.f}) {
        for (const bool editing : {false, true}) {
            gui::visual.editing = editing;
            gui::visual.scope = 1;
            eui::Ui page;
            page.begin("chart-page-layout");
            page.column("chart-host")
                .width(width)
                .content([&] {
                    gui::chartsPage(page, width, 560);
                    page.row("chart-footer")
                        .width(width)
                        .gap(12)
                        .content([&] { gui::footer(page, width); })
                        .build();
                })
                .build();
            page.end();
            page.layout(width, 636);
            controls_fit(page, width, "图表页");
            require(page.find("charts.preview.card") && page.find("chart.selection.count") &&
                        page.find("chart.file.options.bg") && !page.find("chart.directory.hit") &&
                        !page.find("chart.filename.hit"),
                    "图表页缺少预览、选择计数或目录操作");
            const auto *editorScroll = page.find("chart.editor.scroll");
            if (editing && width >= 1100) {
                const auto &figure = page.find("charts.preview.card")->frame;
                const auto *editor = page.find("chart.editor.card");
                require(editor && editorScroll && editorScroll->scrollMaxOffset > 0 &&
                            std::fabs(figure.y - editor->frame.y) < .1f &&
                            std::fabs(figure.height - editor->frame.height) < .1f,
                        "桌面编辑未使用与预览齐边的独立滚动区");
            } else {
                require(!editorScroll, "窄窗编辑应随图表页单列滚动");
            }
        }
    }

    const auto folder =
        std::filesystem::temp_directory_path() /
        ("msa-chart-ui-" + std::to_string(gui::Clock::now().time_since_epoch().count()));
    const auto csv = folder / "study.csv";
    std::filesystem::create_directories(folder);
    msa::atomic_text(csv, msa::export_csv_text(generated, generated.config.at("output")));
    const auto xlsx = folder / "study.xlsx";
    require(platform::importFilenameSuggestion(folder, "study") == "study.csv" &&
                platform::importFilenameSuggestion(folder, "").empty(),
            "仅有 CSV 或未设置导入名时预填错误");
    auto workbookOutput = generated.config.at("output");
    workbookOutput["file_type"] = "xlsx";
    msa::export_data(generated, xlsx, workbookOutput, msa::ExistingFile::Preserve);
    require(platform::importFilenameSuggestion(folder, "study") == "study",
            "同一基本名有 CSV 与 XLSX 时预填不应锁定任一扩展名");
    std::filesystem::copy_file(xlsx, folder / L"独立工作簿.xlsx");
    require(platform::importFilenameSuggestion(folder, "独立工作簿") == L"独立工作簿.xlsx",
            "只有 XLSX 时预填了非工作簿名称");
    gui::state.draft.filename = "[unclosed.csv";
    gui::state.draft.attempts = "未填写";
    gui::state.draft.targets[0] = {1, "待定", ""};
    const auto importPicker = gui::state.chooseImportData;
    gui::state.draft.importDirectory.clear();
    gui::state.draft.importFilename.clear();
    bool requested = false;
    gui::state.chooseImportData = [&](const auto &initial, const auto &filename)
        -> std::optional<std::filesystem::path> {
        requested = initial == msa::desktop_directory() && filename.empty();
        return std::nullopt;
    };
    gui::importInsightData();
    require(requested, "默认导入应从系统桌面打开并留空文件名");
    gui::state.draft.importDirectory = "自定义导入位置";
    gui::state.draft.importFilename = "来料_[人员数量]_[[复核]]";
    requested = false;
    gui::state.chooseImportData = [&](const auto &initial, const auto &filename)
        -> std::optional<std::filesystem::path> {
        requested = initial == msa::desktop_directory() / L"自定义导入位置" &&
                    filename == "来料_" + gui::state.draft.operators + "_[复核]";
        return csv;
    };
    gui::importInsightData();
    gui::state.chooseImportData = importPicker;
    require(requested && gui::state.draft.importDirectory == "自定义导入位置" &&
                gui::state.draft.importFilename == "来料_[人员数量]_[[复核]]" && gui::visual.importedSelected &&
                gui::visual.imported->matrix == generated.matrix &&
                gui::visual.imported->config["analysis"] == generated.config["analysis"] &&
                gui::visual.imported->config["targets"]["EV_pct"].is_null(),
            "无关的生成或导出草稿影响了 CSV 导入与分析");
    require(gui::loadInsightData(xlsx) && gui::visual.importedName == "study.xlsx" &&
                gui::visual.imported->matrix == generated.matrix &&
                gui::visual.imported->metrics == msa::calculate(generated.matrix, gui::visual.imported->config),
            "从 XLSX 导入的测量记录或分析结果与原矩阵不一致");
    require(gui::loadInsightData(csv), "切回 CSV 数据源失败");
    const auto importedConfig = gui::visual.imported->config;
    gui::state.draft.analysisMethod = gui::state.draft.analysisMethod ? 0 : 1;
    gui::state.draft.operatorNames = {"新操作员", "", ""};
    require(gui::insightResult()->config == importedConfig, "选项变化悄悄改变了已导入快照");
    auto heldCsv = folder / "源文件暂时移开.csv";
    std::filesystem::rename(csv, heldCsv);
    eui::Ui sourceUi;
    sourceUi.begin("reanalyze-import");
    gui::chartSourceCard(sourceUi, 588);
    sourceUi.end();
    sourceUi.layout(588, 500);
    require(sourceUi.find("charts.dataset.report.bg") &&
                !sourceUi.find("charts.dataset.report.bg")->disabled,
            "当前数据源缺少可用的方法报告入口");
    click(sourceUi, "charts.dataset.reanalyze.bg");
    require(gui::visual.imported->matrix == generated.matrix &&
                gui::visual.imported->config["design"] == importedConfig["design"] &&
                gui::visual.imported->config["analysis"]["method"] != importedConfig["analysis"]["method"] &&
                gui::visual.imported->metrics == msa::calculate(generated.matrix,
                                                                gui::visual.imported->config),
            "重算没有在内存中的原矩阵应用当前分析方法");
    const auto recalculated = *gui::visual.imported;
    gui::state.draft.parts = gui::state.draft.parts == "5" ? "6" : "5";
    require(!gui::reanalyzeInsightData() && gui::visual.imported->config == recalculated.config &&
                gui::visual.imported->metrics == recalculated.metrics &&
                gui::visual.errorKind == "reanalysis",
            "不匹配的设计破坏了原快照");
    std::filesystem::rename(heldCsv, csv);
    const auto finishExport = [&] {
        const auto deadline = gui::Clock::now() + std::chrono::seconds(60);
        while (gui::visual.busy() && gui::Clock::now() < deadline) {
            gui::pollVisuals();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        require(!gui::visual.busy(), "异步导出超过测试时限");
    };
    using Kind = platform::ExportKind;
    using Selection = std::optional<platform::SaveTarget>;
    gui::state.draft.reportDirectory = folder.u8string();
    gui::state.draft.reportFormat = 0;
    gui::state.draft.reportFilename = "方法对比_[SEED]";
    int pickerCalls = 0;
    auto picker = gui::state.chooseExportPath;
    Selection next;
    gui::state.chooseExportPath = [&](const auto &initial, Kind format) {
        ++pickerCalls;
        const std::string seed = gui::visual.importedSelected ? "NA" : std::to_string(generated.seed);
        require(initial == folder / std::filesystem::u8path("方法对比_" + seed + ".pdf") && format == Kind::ReportPdf,
                "另存为未预填默认报告格式和目录");
        return next;
    };
    gui::exportComparisonReport();
    require(pickerCalls == 1 && !gui::visual.busy(), "取消报告另存为启动了导出");
    auto verifyReport = [&](bool imported) {
        gui::visual.importedSelected = imported;
        const auto report = folder / (imported ? "imported.html" : "generated.htm");
        next = platform::SaveTarget{report, Kind::Html, msa::ExistingFile::Preserve};
        gui::exportComparisonReport();
        require(gui::visual.busy() && gui::visual.exportingReport &&
                    gui::headerStatusPresentation().label == "报告导出中",
                "报告未在后台使用独立状态");
        gui::visual.importedSelected = !imported;
        finishExport();
        const auto html = msa::read_text(report);
        require(html.find(imported ? "数据导入" : "模拟生成") != html.npos &&
                    html.find("Xbar-R") != html.npos && html.find("ANOVA") != html.npos,
                "方法报告未采用选中数据源");
    };
    verifyReport(false);
    verifyReport(true);
    gui::visual.importedSelected = false;
    gui::state.draft.silentReport = true;
    gui::state.draft.reportFormat = 1;
    for (int i = 0; i < 2; ++i) {
        gui::exportComparisonReport();
        finishExport();
    }
    require(pickerCalls == 3 && gui::visual.lastExportPath.empty() &&
                std::filesystem::exists(folder / std::filesystem::u8path("方法对比_" + std::to_string(generated.seed) + ".html")) &&
                std::filesystem::exists(folder / std::filesystem::u8path("方法对比_" + std::to_string(generated.seed) + "_1.html")),
            "静默报告弹窗、覆盖同名文件或修改了图表成功路径");
    gui::state.chooseExportPath = picker;
    gui::visual.importedSelected = true;
    gui::state.draft = native_ui::from_config(generated.config);
    require(gui::reanalyzeInsightData() && gui::visual.imported->config == importedConfig,
            "原研究设计恢复后不能重新分析");
    const auto badCsv = folder / "invalid.csv";
    msa::atomic_text(badCsv, "not,a,measurement\r\n");
    require(!gui::loadInsightData(badCsv) && gui::visual.imported->matrix == generated.matrix &&
                gui::visual.imported->config == importedConfig && gui::visual.errorKind == "import",
            "测量数据导入失败丢失旧数据或缺少恢复状态");
    gui::clearVisualError();
    gui::visual.importedSelected = false;
    gui::visual.scope = 0;
    gui::visual.selected = 0;
    gui::visual.format = 0;
    gui::visual.resolution = 0;
    gui::state.draft.chartDirectory = folder.u8string();
    gui::state.draft.chartFilename = "图表_[PARTS]件";
    gui::state.draft.silentCharts = true;
    const auto exportFrames = [&](bool folderEnabled) {
        eui::Ui ui;
        ui.begin("chart-export-stable-layout");
        gui::chartExportSettings(ui, 620);
        ui.end();
        ui.layout(620, 1800);
        std::array<eui::Rect, 3> frames;
        const std::array<const char *, 3> ids{"chart.output.card", "chart.last.folder.bg",
                                              "chart.output.note"};
        for (std::size_t i = 0; i < ids.size(); ++i) {
            const auto *node = ui.find(ids[i]);
            require(node != nullptr, "导出状态变化移除了固定内容槽位");
            frames[i] = {node->frame.x, node->frame.y, node->frame.width, node->frame.height};
        }
        require(ui.find("chart.last.folder.bg")->disabled != folderEnabled,
                "上次导出目录按钮的可用状态错误");
        require(!ui.find("chart.export.status"), "正文重复显示导出成功状态");
        return frames;
    };
    const auto sameExportFrames = [&](const auto &before, const auto &after) {
        for (std::size_t i = 0; i < before.size(); ++i)
            require(std::fabs(before[i].x - after[i].x) < .01f &&
                        std::fabs(before[i].y - after[i].y) < .01f &&
                        std::fabs(before[i].width - after[i].width) < .01f &&
                        std::fabs(before[i].height - after[i].height) < .01f,
                    "导出前、进行中或完成后的输出卡布局发生变化");
    };
    const auto beforeExportFrames = exportFrames(false);
    gui::exportVisuals();
    require(gui::visual.busy(), "有效设置没有启动真实导出");
    sameExportFrames(beforeExportFrames, exportFrames(false));
    finishExport();
    sameExportFrames(beforeExportFrames, exportFrames(true));
    const auto first = gui::visual.lastExportPath;
    require(gui::visual.errorText.empty() && std::filesystem::exists(first) &&
                msa::read_text(first).rfind("\x89PNG", 0) == 0,
            "真实 PNG 导出失败或完成状态未记录");
    const auto originalBytes = msa::read_text(first);
    gui::exportVisuals();
    finishExport();
    const auto second = gui::visual.lastExportPath;
    require(first != second && second.stem().u8string() == "图表_" + generated.config["design"]["parts"].dump() + "件_1" &&
                msa::read_text(first) == originalBytes,
            "同名导出没有追加序号或覆盖了原文件");
    const auto blocked = folder / "occupied-file";
    msa::atomic_text(blocked, "preserve");
    gui::state.draft.chartDirectory = blocked.u8string();
    gui::exportVisuals();
    finishExport();
    require(gui::visual.errorKind == "export" && gui::visual.lastExportPath == second &&
                gui::state.result->matrix == generated.matrix &&
                msa::read_text(blocked) == "preserve",
            "导出失败未保留数据、已有文件或上次成功路径");
    sameExportFrames(beforeExportFrames, exportFrames(true));
    gui::state.draft.chartDirectory = folder.u8string();
    gui::exportVisuals();
    finishExport();
    require(gui::visual.errorText.empty() && gui::visual.lastExportPath != second &&
                gui::state.revision == revision,
            "修改导出目录后未恢复，或图表操作使研究失效");
    gui::visual.scope = 1;
    gui::visual.included.fill(false);
    gui::visual.included[0] = gui::visual.included[3] = true;
    gui::visual.selected = 2;
    gui::visual.editDirty[2] = true;
    require(gui::chartFooterSummary().find("未应用") == std::string::npos,
            "组合导出误报未勾选图表的草稿");
    gui::visual.editDirty[3] = true;
    require(gui::chartFooterSummary().find("未应用") != std::string::npos,
            "组合导出未提示已勾选图表的草稿");
    gui::exportVisuals();
    require(!gui::visual.busy() && gui::visual.selected == 3 && gui::visual.errorChart == 3,
            "组合导出和底栏使用的草稿检查范围不一致");
    gui::visual.editDirty[2] = gui::visual.editDirty[3] = false;
    gui::clearVisualError();
    gui::visual.format = 2;
    gui::state.draft.chartFilename = "两图组合快照";
    gui::exportVisuals();
    require(gui::visual.busy() && gui::visual.exportingCount == 2, "组合没有捕获两张已选图表");
    gui::visual.included.fill(false);
    gui::visual.format = 0;
    gui::state.draft.chartFilename = "下一次导出";
    finishExport();
    require(gui::visual.errorText.empty() &&
                gui::visual.lastExportPath.filename().u8string() == "两图组合快照.pdf" &&
                msa::read_text(gui::visual.lastExportPath).rfind("%PDF-", 0) == 0,
            "导出中的编辑改变了本次组合或格式快照");
    gui::visual.scope = 0;
    gui::visual.selected = 0;
    gui::state.draft.silentCharts = false;
    const auto prior = gui::visual.lastExportPath;
    gui::state.draft.chartFilename = "图表备份_[SEED]";
    const std::array<Kind, 3> chartFormats{Kind::Png, Kind::Jpeg, Kind::ChartPdf};
    const std::array<std::string, 3> extensions{".png", ".jpeg", ".pdf"};
    const std::array<std::string, 3> signatures{"\x89PNG", "\xFF\xD8", "%PDF-"};
    for (int i = 0; i < 3; ++i) {
        gui::visual.format = i;
        const auto target = folder / ("chosen-chart" + extensions[i]);
        bool cancelled = true;
        gui::state.chooseExportPath = [&](const auto &initial, Kind format) -> Selection {
            require(initial.parent_path() == folder && format == chartFormats[i] &&
                        initial.stem().u8string() == "图表备份_" + std::to_string(generated.seed),
                    "图表另存为默认目录或格式错误");
            if (cancelled)
                return std::nullopt;
            return platform::SaveTarget{target, format, msa::ExistingFile::Replace};
        };
        gui::exportVisuals();
        require(!gui::visual.busy() && !std::filesystem::exists(target),
                "取消图表另存为启动了导出");
        cancelled = false;
        msa::atomic_text(target, "before");
        gui::exportVisuals();
        finishExport();
        require(gui::visual.lastExportPath == target &&
                    msa::read_text(target).rfind(signatures[i], 0) == 0,
                "图表另存为忽略选定名称、格式或覆盖决定");
    }
    require(std::filesystem::exists(prior), "另存为覆盖了上次图表");
    click(outputUi, "chart.file.options.bg");
    require(gui::state.page == gui::Page::Options && gui::state.optionsTab == 2 &&
                gui::state.configOffset == 0,
            "图表页未跳转到集中管理目录与文件名的位置");
    gui::state.chooseExportPath = picker;
    const auto chooser = gui::state.chooseWorksheet;
    gui::state.draft.operators = "2";
    gui::state.draft.replicates = "2";
    gui::state.draft.parts = "5";
    int sheetRequests = 0;
    bool cancelSheet = false;
    gui::state.chooseWorksheet = [&](const std::vector<std::string> &names) -> std::optional<std::size_t> {
        ++sheetRequests;
        require(names == std::vector<std::string>{"原始记录", "复测 数据"}, "工作表名称或顺序错误");
        return cancelSheet ? std::nullopt : std::optional<std::size_t>(1);
    };
    require(gui::loadInsightData(multiSheetWorkbook) && gui::visual.importedSheet == "复测 数据" &&
                gui::visual.imported->matrix[0][0] == 110.11 && sheetRequests == 1,
            "未导入选择的第二张工作表");
    const auto oldMatrix = gui::visual.imported->matrix;
    const auto oldFeedback = gui::state.footerMessage;
    const auto oldChart = gui::visual.selected;
    gui::visual.preview.attempted = true;
    cancelSheet = true;
    require(!gui::loadInsightData(multiSheetWorkbook) && sheetRequests == 2 &&
                gui::visual.imported->matrix == oldMatrix && gui::visual.importedSelected &&
                gui::visual.importedSheet == "复测 数据" && gui::visual.selected == oldChart &&
                gui::visual.preview.attempted && gui::state.footerMessage == oldFeedback,
            "取消工作表选择改变了当前研究、图表或提示");
    gui::state.draft.silentReport = true;
    gui::state.draft.reportDirectory = folder.u8string();
    gui::state.draft.reportFilename = "工作表报告_[PARTS]_[SEED]";
    gui::state.draft.reportFormat = 1;
    gui::exportComparisonReport();
    finishExport();
    const auto worksheetReport = msa::read_text(folder / L"工作表报告_5_NA.html");
    require(worksheetReport.find("复测 数据") != worksheetReport.npos, "方法对比报告遗漏工作表来源");
    gui::state.chooseWorksheet = chooser;
    std::cout << "图表草稿/应用/撤销、切图隔离、缓存、错误恢复、选择与布局及真实导出通过："
              << folder.u8string() << '\n';
    gui::visual = gui::VisualState{};
    gui::state.draft = savedDraft;
    gui::state.page = savedPage;
    gui::state.message.clear();
}
} // namespace

int wmain(int argc, wchar_t **argv) {
    if (argc != 4)
        return 2;
    try {
        const auto font = std::filesystem::path(argv[1]).u8string();
        core::TextPrimitive::setDefaultFontFiles(font, font);
        gui::initializeFont();
        const auto cfg = msa::load_config(std::filesystem::path(argv[2]));
        const auto generated = msa::generate(cfg);
        gui::state.draft = native_ui::from_config(cfg);
        gui::state.page = gui::Page::Data;
        gui::state.resultEntered = gui::Clock::now() - std::chrono::seconds(1);
        gui::state.message.clear();
        initial_window_bounds();
        render_request_lifecycle();
        state_recovery(generated);
        header_status_presentation(generated);
        combo_unchanged_value();
        long_combo_menu();
        shared_choice_controls(generated);
        shared_choice_keyboard();
        disclosure_and_reduced_motion(generated);
        runtime_keyboard_navigation();
        xbar_details(generated);
        result_table_layout(generated);
        data_settings_layout();
        options_navigation(generated);
        regrouped_settings_state(generated);
        setting_dependencies();
        help_topics();
        scrolled_menu_anchors();
        help_destinations();
        logs_and_about_layout();
        chart_interaction(generated, std::filesystem::path(argv[3]));
        for (float w : {1232.f, 1100.f, 1020.f, 588.f})
            for (float h : {500.f, 700.f}) {
                gui::state.generateOffset = 0;
                gui::state.resultTab = 0;
                gui::state.result.reset();
                const auto ready = capture(gui::Status::Ready, w, h);
                const auto starting = capture(gui::Status::Running, w, h);
                if (ready.exportEnabled || starting.exportEnabled)
                    throw std::runtime_error("export enabled without a completed result");
                equal(ready, starting, "first generation start");
                gui::state.result = generated;
                gui::state.resultRevision = gui::state.revision;
                const auto completed = capture(gui::Status::Success, w, h);
                if (!completed.exportEnabled)
                    throw std::runtime_error("completed result cannot be exported");
                equal(starting, completed, "first generation finish");
                const auto regenerating = capture(gui::Status::Running, w, h);
                if (regenerating.exportEnabled)
                    throw std::runtime_error("previous result can be exported during generation");
                equal(completed, regenerating, "regeneration start");
                equal(regenerating, capture(gui::Status::Cancelling, w, h), "cancel request");
                if (completed.value != regenerating.value)
                    throw std::runtime_error("regeneration clears the visible previous result");
                for (const auto stopped : {gui::Status::Cancelled, gui::Status::SearchFailed}) {
                    const auto retained = capture(stopped, w, h);
                    equal(regenerating, retained, "retry stopped with a retained result");
                    if (!retained.exportEnabled || retained.value != completed.value)
                        throw std::runtime_error(
                            "retry stop hides or disables the previous result");
                }
                equal(regenerating, capture(gui::Status::Success, w, h), "regeneration finish");
                gui::state.resultTab = 1;
                const auto preview = capture(gui::Status::Success, w, h);
                equal(preview, capture(gui::Status::Running, w, h), "preview regeneration");
                gui::state.generateOffset = 100;
                const auto scrolledPreview = capture(gui::Status::Success, w, h);
                equal(scrolledPreview, capture(gui::Status::Running, w, h),
                      "scrolled preview regeneration");
                std::cout << w << 'x' << h << ": stable generation and regeneration layout\n";
            }
        eui::Ui aboutUi;
        aboutUi.begin("project-attribution-test");
        gui::aboutPage(aboutUi, 1232, 500);
        aboutUi.end();
        aboutUi.layout(1232, 500);
        const auto *author = aboutUi.find("about.author");
        const auto *repository = aboutUi.find("about.repository");
        if (!author || !repository || author->text.find(msa::project_author) == std::string::npos ||
            repository->text.find(msa::project_repository) == std::string::npos)
            throw std::runtime_error("About page lacks author and repository information");
        // Exercise the actual output controls and GUI export, not just Draft fields.
        gui::state.status = gui::Status::Success;
        gui::state.result = generated;
        gui::state.resultRevision = gui::state.revision;
        gui::state.resultTab = 1;
        gui::state.draft.filename.clear();
        gui::state.draft.directory.clear();
        gui::state.draft.existingFile = 0;
        const auto revision = gui::state.revision;
        const auto folder =
            std::filesystem::temp_directory_path() /
            ("msa-output-ui-" + std::to_string(gui::Clock::now().time_since_epoch().count()));
        eui::Ui outputUi;
        outputUi.begin("export-edit-test");
        gui::state.optionsTab = 2;
        gui::config(outputUi, 620);
        outputUi.end();
        outputUi.layout(620, 1200);
        eui::TextInputEvent filenameEvent, directoryEvent;
        gui::state.draft.silentCsv = true;
        filenameEvent.text = "研究_[人员数量]_[SEED]";
        directoryEvent.text = folder.u8string();
        outputUi.find("filename.hit")->onTextInput(filenameEvent);
        outputUi.find("directory.hit")->onTextInput(directoryEvent);
        outputUi.find("collision.hit.1")->onClick();
        if (!gui::state.validResult() || gui::state.revision != revision ||
            gui::state.resultTab != 1 || gui::state.draft.existingFile != 1 ||
            outputUi.find("help.filename"))
            throw std::runtime_error(
                "output edits invalidate data or put variable help on Options");
        gui::exportData();
        const auto exported =
            folder /
            std::filesystem::u8path("研究_" + generated.config["design"]["operators"].dump() + "_" +
                                    std::to_string(generated.seed) + ".csv");
        if (!gui::state.validResult() || msa::read_csv(exported) != generated.matrix)
            throw std::runtime_error("GUI export ignored current output settings or changed data");
        const auto chosenCsv = folder / "explicit.csv";
        gui::state.draft.silentCsv = false;
        bool cancelCsv = true;
        const auto picker = gui::state.chooseExportPath;
        gui::state.chooseExportPath = [&](const auto &initial, auto kind)
            -> std::optional<platform::SaveTarget> {
            if (initial != exported || kind != platform::ExportKind::Csv)
                throw std::runtime_error("CSV 另存为未使用配置目录和展开的文件名模板");
            if (cancelCsv)
                return std::nullopt;
            return platform::SaveTarget{chosenCsv, kind, msa::ExistingFile::Replace};
        };
        gui::exportData();
        if (std::filesystem::exists(chosenCsv) || !gui::state.dataExportError.empty())
            throw std::runtime_error("取消 CSV 另存为产生了文件或错误");
        cancelCsv = false;
        msa::atomic_text(chosenCsv, "before");
        gui::exportData();
        if (msa::read_csv(chosenCsv) != generated.matrix || !gui::state.dataExportError.empty())
            throw std::runtime_error("CSV 另存为未执行指定目的地和覆盖选择");
        const auto chosenXlsx = folder / L"交叉选择.v2.xlsx";
        gui::state.chooseExportPath = [&](const auto &initial, auto kind)
            -> std::optional<platform::SaveTarget> {
            if (initial != exported || kind != platform::ExportKind::Csv)
                throw std::runtime_error("数据导出对话框未采用配置中的默认类型");
            return platform::SaveTarget{chosenXlsx, platform::ExportKind::Xlsx, msa::ExistingFile::Preserve};
        };
        gui::exportData();
        if (msa::read_data(chosenXlsx, generated.config) != generated.matrix ||
            gui::state.draft.dataFileType != 0 || !gui::state.dataExportError.empty())
            throw std::runtime_error("另存为选择 XLSX 后没有生成正确工作簿，或临时选择改变了默认类型");
        gui::state.draft.silentCsv = true;
        gui::state.chooseExportPath = picker;
        outputUi.find("runorder.mode.1.hit")->onClick();
        if (gui::state.draft.runMode != 0 || !outputUi.find("runorder.seed.control")->disabled)
            throw std::runtime_error("wide-format run-order controls remain active");
        outputUi.find("csvformat")->onPress({}, {});
        if (gui::menu.items.size() != 6)
            throw std::runtime_error("CSV format dropdown does not expose all six formats");
        gui::menu.select(1);
        gui::menu.id.clear();
        eui::Ui longUi;
        longUi.begin("long-settings-test");
        gui::config(longUi, 620);
        longUi.end();
        longUi.layout(620, 1800);
        longUi.find("runorder.operators.hit")->onClick();
        if (gui::state.draft.randomOperators)
            throw std::runtime_error(
                "operator randomization is active outside within-operator mode");
        longUi.find("runorder.mode.2.hit")->onClick();
        eui::Ui withinUi;
        withinUi.begin("within-settings-test");
        gui::config(withinUi, 620);
        withinUi.end();
        withinUi.layout(620, 1800);
        withinUi.find("runorder.operators.hit")->onClick();
        withinUi.find("runorder.standard.hit")->onClick();
        eui::TextInputEvent seedEvent;
        seedEvent.text = "321";
        withinUi.find("runorder.seed.hit")->onTextInput(seedEvent);
        if (!gui::state.validResult() || gui::state.revision != revision ||
            gui::state.resultTab != 1 || !gui::state.draft.randomOperators ||
            !gui::state.draft.storeStandardOrder || gui::state.draft.runSeed != "321")
            throw std::runtime_error(
                "run-order controls invalidate data or fail to update settings");
        gui::exportData();
        const auto longBytes = msa::read_text(exported);
        if (longBytes.rfind("StdOrder,Operator,Part,Trial,Measurement\r\n", 0) != 0 ||
            msa::parse_export_csv(longBytes, generated.config) != generated.matrix)
            throw std::runtime_error("GUI export ignored selected format/run-order fields");
        withinUi.find("data.filetype.hit.1")->onClick();
        gui::exportData();
        auto workbookPath = exported;
        workbookPath.replace_extension(".xlsx");
        if (msa::read_data(workbookPath, generated.config) != generated.matrix ||
            !gui::state.validResult() || gui::state.revision != revision)
            throw std::runtime_error("静默导出 XLSX 长表未按模板还原原始研究");
        withinUi.find("data.filetype.hit.0")->onClick();
        withinUi.find("csvformat")->onPress({}, {});
        gui::menu.select(0);
        gui::menu.id.clear();
        if (native_ui::to_config(gui::state.draft)["output"]["run_order"]["seed"] != 321)
            throw std::runtime_error("保存配置丢失了有效的禁用种子");
        gui::state.draft.runSeed = "未填写";
        gui::exportData();
        if (!gui::state.validResult() || msa::read_csv(exported) != generated.matrix)
            throw std::runtime_error("默认宽表导出被禁用的运行种子阻塞或改变了数据");
        auto standardDraft = gui::state.draft;
        standardDraft.csvFormat = 1;
        standardDraft.runMode = 0;
        if (!native_ui::output_config(standardDraft)["run_order"]["seed"].is_null() ||
            !native_ui::to_config(standardDraft)["output"]["run_order"]["seed"].is_null() ||
            standardDraft.runSeed != "未填写")
            throw std::runtime_error("标准顺序读取或改写了禁用种子的草稿");
        for (auto status : {gui::Status::Cancelled, gui::Status::SearchFailed}) {
            gui::state.status = status;
            gui::state.message = status == gui::Status::SearchFailed ? "已耗尽搜索次数" : "";
            const auto diagnostic = gui::state.message;
            gui::exportData();
            if (!gui::state.retainedResult() || gui::state.status != status ||
                gui::state.message != diagnostic || gui::state.footerMessage != "测量数据已导出")
                throw std::runtime_error("导出保留的结果覆盖了最后一轮生成状态");
        }
        gui::state.draft.filename = "[bad].csv";
        gui::exportData();
        if (gui::state.dataExportError.empty() || !gui::state.retainedResult())
            throw std::runtime_error("invalid output template discarded the generated result");
        gui::state.draft.filename = "repaired.csv";
        gui::state.outputChanged();
        if (!gui::state.dataExportError.empty() || !gui::state.retainedResult() ||
            gui::state.revision != revision)
            throw std::runtime_error("repairing an output error requires regeneration");
        gui::state.changed();
        if (gui::state.validResult())
            throw std::runtime_error("generation parameter changes must still invalidate data");
        gui::state.draft = native_ui::from_config(cfg);
        gui::state.message.clear();
        std::cout << "output controls preserve generated data and export current settings\n";
        // Run a real task through the same State::start/poll path used by both
        // buttons, so task completion cannot silently reintroduce a tab reset
        // or a result-table entrance animation.
        gui::state.status = gui::Status::Success;
        gui::state.result = generated;
        gui::state.resultRevision = gui::state.revision;
        gui::state.resultTab = 1;
        gui::state.page = gui::Page::Data;
        gui::state.draft.filename = "[unclosed.csv";
        gui::state.draft.csvFormat = 1;
        gui::state.draft.runMode = 1;
        gui::state.draft.runSeed = "未填写";
        const auto entered = gui::state.resultEntered;
        eui::Ui startActions;
        startActions.begin("generation-start-test");
        gui::footer(startActions, 1232);
        startActions.end();
        const auto *start = startActions.find("generate.bg");
        const eui::KeyEvent pressed{eui::InputKey::Enter};
        const eui::KeyEvent repeated{eui::InputKey::Enter, core::KeyAction::Repeat};
        if (!start || !start->onKeyEvent || !start->onClick || start->onKeyEvent(repeated) ||
            gui::state.busy() || !start->onKeyEvent(pressed) ||
            gui::state.status != gui::Status::Running)
            throw std::runtime_error("生成被无关的导出草稿阻塞或错误响应按键重复");
        start->onClick();
        if (gui::state.status != gui::Status::Running || gui::state.cancel.load())
            throw std::runtime_error("同一帧的生成按钮将重复操作解释成取消");
        if (!gui::state.displayedResult() || gui::state.validResult())
            throw std::runtime_error("starting a task clears or exports the previous result");
        const auto deadline = gui::Clock::now() + std::chrono::seconds(5);
        while (gui::state.busy() && gui::Clock::now() < deadline) {
            gui::state.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!gui::state.validResult() || gui::state.resultTab != 1 ||
            gui::state.resultEntered != entered)
            throw std::runtime_error("task completion resets/fades the selected result view");
        std::cout << "real regeneration preserves the preview and result animation state\n";
        for (auto &target : gui::state.draft.targets)
            target = {};
        gui::state.draft.targets[4] = {4, "1000000", ""};
        gui::state.draft.attempts = "1000000";
        gui::state.draft.timeout = "30";
        gui::state.changed();
        gui::state.start();
        if (!gui::state.busy())
            throw std::runtime_error("cancellation fixture did not start a real search");
        eui::Ui actions;
        actions.begin("cancel-test");
        gui::footer(actions, 1232);
        actions.end();
        auto *cancel = actions.find("generate.bg");
        if (!cancel || !cancel->onClick || !cancel->onKeyEvent || cancel->disabled)
            throw std::runtime_error("generation action does not expose cancellation");
        if (cancel->onKeyEvent(repeated) || gui::state.status != gui::Status::Running)
            throw std::runtime_error("长按 Enter 取消了已启动的生成任务");
        cancel->onClick();
        if (gui::state.status != gui::Status::Cancelling || !gui::state.cancel.load())
            throw std::runtime_error("generation action did not request cancellation");
        const auto cancelDeadline = gui::Clock::now() + std::chrono::seconds(5);
        while (gui::state.busy() && gui::Clock::now() < cancelDeadline) {
            gui::state.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (gui::state.status != gui::Status::Cancelled)
            throw std::runtime_error("real search did not cancel from the fixed action slot");
        std::cout << "fixed generation action cancels a real search\n";
        gui::state.status = gui::Status::Ready;
    } catch (const std::exception &error) {
        gui::state.status = gui::Status::Ready;
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
