// Exercise the actual presentation without starting its window/event loop.
// gui.cpp has no main; including it keeps UI internals out of the public API.
#include "../src/gui.cpp"
#include "core/dsl_runtime.h"
#include "core/app/app_runner.h"
#include <iostream>
#include <limits>
#include <map>

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
                           "resulttabs", "result.content", "result.caution"}) {
        const auto *node = ui.find(id);
        if (!node)
            throw std::runtime_error(std::string("missing UI anchor: ") + id);
        result.frames[id] = {node->frame.x, node->frame.y, node->frame.width, node->frame.height};
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
    const std::array<std::vector<const char *>, 3> groups{{
        {"design.card", "generation.rules.card", "generation.card", "advanced.card"},
        {"analysis.card", "analysis.evaluation.card", "analysis.process.card",
         "analysis.statistics.card"},
        {"output.card", "output.files.card", "runorder.card"},
    }};
    for (int category = 0; category < 3; ++category)
        for (const auto *id : groups[category])
            if (bool(ui.find(id)) != (category == tab))
                throw std::runtime_error(std::string("选项业务分组跨页或丢失：") + id);
    if (ui.find("precision.card"))
        throw std::runtime_error("旧的精度分组未合并到生成参数");
    if (ui.find("center.hit") || ui.find("lsl.hit") || ui.find("usl.hit") || ui.find("spec.card"))
        throw std::runtime_error("数据中心或规格范围仍出现在选项页");
    if (tab != 0 && (ui.find("priority") || ui.find("compliance.hit")))
        throw std::runtime_error("生成规则控件出现在非研究选项中");
    if (tab == 0) {
        group("design.card", {"operators", "replicates", "parts"});
        group("generation.rules.card", {"priority", "compliance.hit"});
        group("generation.card", {"decimals", "scale", "seed.hit"});
        const bool reference = gui::state.draft.scale == 1 ||
                               native_ui::trim(gui::state.draft.lsl).empty() ||
                               native_ui::trim(gui::state.draft.usl).empty();
        if (reference)
            group("generation.card", {"reference.hit"});
        else if (ui.find("reference.hit"))
            throw std::runtime_error("自动双侧规格模式出现不适用的参考标准差输入");
        group("advanced.card", {"percenterror.hit", "ndcerror.hit", "advancedtoggle.bg",
                                "attempts.hit", "timeout.hit"});
        if (ui.find("multiplier.hit") || ui.find("analysis.tolerance.hit"))
            throw std::runtime_error("统计评价口径仍混在研究参数中");
    } else if (tab == 1) {
        group("analysis.card", {"analysis.method"});
        group("analysis.evaluation.card", {"multiplier.hit", "analysis.tolerance.hit"});
        group("analysis.process.card", {"analysis.historical.sd.hit",
                                        "analysis.historical.mean.hit", "analysis.process.source"});
        group("analysis.statistics.card", {"analysis.misclassification.control",
                                           "analysis.confidence.control",
                                           "analysis.confidence.method",
                                           "analysis.confidence.level.hit"});
    } else {
        group("output.card", {"csvformat", "runorder.standard.hit"});
        group("output.files.card", {"directory.hit", "filename.hit", "collision"});
        group("runorder.card", {"runorder.mode.0.hit", "runorder.mode.1.hit",
                                "runorder.mode.2.hit", "runorder.operators.hit",
                                "runorder.seed.hit"});
        if (contains_element(node("runorder.card"), node("runorder.standard.hit")))
            throw std::runtime_error("附加标准序号列仍混在运行顺序中");
        if (node("runorder.standard.control").disabled != (gui::state.draft.csvFormat == 0))
            throw std::runtime_error("标准序号列的可用性未随宽表/长表格式更新");
    }
    group("configpath.card", {"path", "copypath.bg", "opendir.bg"});
    const auto &content = node("configcontent").frame;
    const auto &management = node("configpath.card").frame;
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
        controls_fit(details, width, "Xbar-R details");
    }
    gui::state.resultTab = 0;
    gui::state.resultEntered = gui::Clock::now() - std::chrono::seconds(1);
    std::cout << "Xbar-R analysis details retain the selected tab and real study statistics\n";
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
            gui::state.draft.confidenceEnabled = true;
            gui::state.advanced = true;
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
                if (!navigation || !scroll ||
                    scroll->frame.y < navigation->frame.y + navigation->frame.height)
                    throw std::runtime_error("options content overlaps the fixed navigation");
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
    std::cout
        << "选项在五种宽度下分类清晰、无重叠，共享配置管理与固定导航保持对齐\n";
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
    const std::array<FieldCase, 9> fields{{
        {dataTab, "center", &native_ui::Draft::center, "10.25"},
        {dataTab, "lsl", &native_ui::Draft::lsl, "8.5"},
        {dataTab, "usl", &native_ui::Draft::usl, "11.5"},
        {0, "seed", &native_ui::Draft::seed, "22409"},
        {0, "reference", &native_ui::Draft::reference, "0.25"},
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
    reset(2);
    gui::state.draft.csvFormat = 1;
    gui::state.draft.storeStandardOrder = false;
    eui::Ui output;
    output.begin("regrouped-output-input");
    gui::optionsPage(output, 1232, 500);
    output.end();
    output.layout(1232, 500);
    click(output, "runorder.standard.hit");
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

void help_topics() {
    const auto revision = gui::state.revision;
    const std::array<const char *, 6> anchors{"help.workflow.card", "help.analysis.card",
                                              "help.runorder.card", "help.filename.card",
                                              "help.cli.card",      "help.charts.card"};
    for (float width : {588.f, 1020.f, 1232.f})
        for (int topic = 0; topic < 6; ++topic) {
            gui::state.helpTopic = (topic + 5) % 6;
            gui::state.helpOffset = 80;
            eui::Ui select;
            select.begin("help-topic-navigation");
            gui::helpPage(select, width, 500);
            select.end();
            select.layout(width, 500);
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
            gui::helpPage(page, width, 500);
            page.end();
            page.layout(width, 500);
            controls_fit(page, width, "help topic " + std::to_string(topic));
            for (std::size_t other = 0; other < anchors.size(); ++other)
                if (bool(page.find(anchors[other])) != (other == topic))
                    throw std::runtime_error(
                        "help topic mixes unrelated content or omits its section");
            const auto *navigation = page.find("help.topics");
            const auto *scroll = page.find("helpscroll");
            if (!navigation || !scroll ||
                scroll->frame.y < navigation->frame.y + navigation->frame.height)
                throw std::runtime_error("help text overlaps the fixed topic selector");
            if (topic == 3) {
                for (const auto &variable : msa::filename_variables()) {
                    const auto id = std::string("help.variable.") + variable.name;
                    const auto *copy = page.find(id + ".copy.bg");
                    if (!copy || !copy->onClick || copy->disabled)
                        throw std::runtime_error("filename variable cannot be copied: " + id);
                }
                const auto *milliseconds = page.find("help.variable.MS");
                const auto *timestamp = page.find("help.variable.TIMESTAMP");
                if (!milliseconds || !timestamp ||
                    timestamp->frame.y < milliseconds->frame.y + milliseconds->frame.height)
                    throw std::runtime_error("timestamp help is not below milliseconds");
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
          gui::Status::Error, gui::Status::ExportError}) {
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
    const std::array<Case, 9> cases{{
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
        {Status::ExportError, {"CSV 导出失败", "CSV 导出失败", "CSV 导出失败"},
         {Tone::Error, Tone::Error, Tone::Error}},
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
    business.page = gui::Page::Charts;
    charts.importedSelected = true;
    require(gui::headerStatusPresentation(business, charts).label == "待载入 CSV",
            "未选取导入数据时仍显示生成结果成功");
    presentations.push_back(gui::headerStatusPresentation(business, charts));
    charts.imported = generated;
    require(gui::headerStatusPresentation(business, charts).label == "数据已载入",
            "独立导入数据来源未正确显示");
    presentations.push_back(gui::headerStatusPresentation(business, charts));
    const std::array<std::pair<const char *, const char *>, 6> chartErrors{{
        {"export", "图表导出失败"}, {"render", "图表渲染异常"}, {"import", "CSV 导入失败"},
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
    const auto savedFollow = gui::state.followLogs;
    gui::state.logs = {{201, "12:00:01", gui::Level::Info, "Information record"},
                       {202, "12:00:02", gui::Level::Warning, "Warning record"},
                       {203, "12:00:03", gui::Level::Error, "Error record"}};
    for (float width : {588.f, 1020.f, 1232.f}) {
        for (int filter = 0; filter < 3; ++filter) {
            gui::state.logFilter = (filter + 1) % 3;
            gui::state.followLogs = true;
            gui::state.logOffset = 60;
            eui::Ui select;
            select.begin("log-filter-selection");
            gui::logsPage(select, width, 500);
            select.end();
            select.layout(width, 500);
            click(select, "logfilter.hit." + std::to_string(filter));
            if (gui::state.logFilter != filter || gui::state.followLogs ||
                gui::state.logOffset != 0)
                throw std::runtime_error("log filtering retains old scroll/follow state");
            eui::Ui list;
            list.begin("log-filter-content");
            gui::logsPage(list, width, 500);
            list.end();
            list.layout(width, 500);
            controls_fit(list, width, "log page");
            for (int level = 0; level < 3; ++level) {
                const auto id = "log." + std::to_string(201 + level) + ".message";
                if (bool(list.find(id)) != (filter == 0 || level == filter))
                    throw std::runtime_error("log filter shows a record from the wrong severity");
            }
            const auto *copy = list.find("copylogs.bg");
            if (!copy || copy->disabled)
                throw std::runtime_error("nonempty log selection cannot be copied");
        }
        eui::Ui about;
        about.begin("about-responsive-layout");
        gui::aboutPage(about, width, 500);
        about.end();
        about.layout(width, 500);
        controls_fit(about, width, "about page");
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
    gui::state.logs.resize(1);
    gui::state.logFilter = 2;
    eui::Ui empty;
    empty.begin("empty-log-selection");
    gui::logsPage(empty, 588, 500);
    empty.end();
    empty.layout(588, 500);
    if (!empty.find("logs.empty") || !empty.find("copylogs.bg")->disabled)
        throw std::runtime_error(
            "empty log filter does not disable copy or explain its empty state");
    gui::state.logs = savedLogs;
    gui::state.logFilter = savedFilter;
    gui::state.logOffset = savedOffset;
    gui::state.followLogs = savedFollow;
    std::cout << "log filters and About page fit all tested widths and retain attribution\n";
}

void help_destinations() {
    const auto page = gui::state.page;
    const auto topic = gui::state.helpTopic;
    const auto tab = gui::state.optionsTab;
    const auto revision = gui::state.revision;
    const auto configOffset = gui::state.configOffset;
    const auto helpOffset = gui::state.helpOffset;
    const std::array<const char *, 3> buttons{"help.analysis.open.bg", "help.files.open.bg",
                                              "help.filename.open.bg"};
    for (int i = 0; i < 3; ++i) {
        gui::state.navigate(gui::Page::Help);
        gui::state.helpTopic = i + 1;
        gui::state.helpOffset = 0;
        gui::state.configOffset = 120;
        gui::state.hint("copied");
        eui::Ui help;
        help.begin("help-destination");
        gui::helpPage(help, 588, 500);
        help.end();
        help.layout(588, 500);
        click(help, buttons[i]);
        if (gui::state.page != gui::Page::Options || gui::state.optionsTab != (i == 0 ? 1 : 2) ||
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
void chart_interaction(const msa::Result &generated) {
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
        const std::array<const char *, 3> ids{"chart.selection.empty", "chart.filename.field",
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
    require(footerUi.find("charts.export.bg") && footerUi.find("charts.export.bg")->disabled,
            "空组合仍可点击导出");
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
                        page.find("chart.open.bg"),
                    "图表页缺少预览、选择计数或目录操作");
        }
    }

    const auto folder =
        std::filesystem::temp_directory_path() /
        ("msa-chart-ui-" + std::to_string(gui::Clock::now().time_since_epoch().count()));
    const auto csv = folder / "study.csv";
    std::filesystem::create_directories(folder);
    msa::atomic_text(csv, msa::export_csv_text(generated, generated.config.at("output")));
    require(gui::loadInsightCsv(csv) && gui::visual.importedSelected &&
                gui::visual.imported->matrix == generated.matrix,
            "CSV 导入未保留字段对应关系或有效数据");
    const auto importedConfig = gui::visual.imported->config;
    gui::state.draft.analysisMethod = gui::state.draft.analysisMethod ? 0 : 1;
    require(gui::insightResult()->config == importedConfig, "选项变化悄悄改变了已导入快照");
    gui::state.draft = native_ui::from_config(generated.config);
    const auto badCsv = folder / "invalid.csv";
    msa::atomic_text(badCsv, "not,a,measurement\r\n");
    require(!gui::loadInsightCsv(badCsv) && gui::visual.imported->matrix == generated.matrix &&
                gui::visual.imported->config == importedConfig && gui::visual.errorKind == "import",
            "CSV 导入失败丢失旧数据或缺少恢复状态");
    gui::clearVisualError();
    gui::visual.importedSelected = false;
    gui::visual.scope = 0;
    gui::visual.selected = 0;
    gui::visual.format = 0;
    gui::visual.resolution = 0;
    gui::visual.directory = folder.u8string();
    gui::visual.filename = "图表交互回归";
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
    const auto finishExport = [&] {
        const auto deadline = gui::Clock::now() + std::chrono::seconds(20);
        while (gui::visual.busy() && gui::Clock::now() < deadline) {
            gui::pollVisuals();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        require(!gui::visual.busy(), "异步导出超过测试时限");
    };
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
    require(first != second && second.stem().u8string() == "图表交互回归_1" &&
                msa::read_text(first) == originalBytes,
            "同名导出没有追加序号或覆盖了原文件");
    const auto blocked = folder / "occupied-file";
    msa::atomic_text(blocked, "preserve");
    gui::visual.directory = blocked.u8string();
    gui::exportVisuals();
    finishExport();
    require(gui::visual.errorKind == "export" && gui::visual.lastExportPath == second &&
                gui::state.result->matrix == generated.matrix &&
                msa::read_text(blocked) == "preserve",
            "导出失败未保留数据、已有文件或上次成功路径");
    sameExportFrames(beforeExportFrames, exportFrames(true));
    gui::visual.directory = folder.u8string();
    gui::exportVisuals();
    finishExport();
    require(gui::visual.errorText.empty() && gui::visual.lastExportPath != second &&
                gui::state.revision == revision,
            "修改导出目录后未恢复，或图表操作使研究失效");
    gui::visual.scope = 1;
    gui::visual.included.fill(false);
    gui::visual.included[0] = gui::visual.included[3] = true;
    gui::visual.format = 2;
    gui::visual.filename = "两图组合快照";
    gui::exportVisuals();
    require(gui::visual.busy() && gui::visual.exportingCount == 2, "组合没有捕获两张已选图表");
    gui::visual.included.fill(false);
    gui::visual.format = 0;
    gui::visual.filename = "下一次导出";
    finishExport();
    require(gui::visual.errorText.empty() &&
                gui::visual.lastExportPath.filename().u8string() == "两图组合快照.pdf" &&
                msa::read_text(gui::visual.lastExportPath).rfind("%PDF-", 0) == 0,
            "导出中的编辑改变了本次组合或格式快照");
    std::cout << "图表草稿/应用/撤销、切图隔离、缓存、错误恢复、选择与布局及真实导出通过："
              << folder.u8string() << '\n';
    gui::visual = gui::VisualState{};
    gui::state.draft = savedDraft;
    gui::state.page = savedPage;
    gui::state.message.clear();
}
} // namespace

int wmain(int argc, wchar_t **argv) {
    if (argc != 3)
        return 2;
    try {
        const auto font = std::filesystem::path(argv[1]).u8string();
        core::TextPrimitive::setDefaultFontFiles(font, font);
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
        data_settings_layout();
        options_navigation(generated);
        regrouped_settings_state(generated);
        help_topics();
        scrolled_menu_anchors();
        help_destinations();
        logs_and_about_layout();
        chart_interaction(generated);
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
        filenameEvent.text = "研究_[人员数量]_[SEED].csv";
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
        withinUi.find("csvformat")->onPress({}, {});
        gui::menu.select(0);
        gui::menu.id.clear();
        gui::exportData();
        if (!gui::state.validResult() || msa::read_csv(exported) != generated.matrix)
            throw std::runtime_error(
                "switching back to wide format requires regeneration or changes data");
        gui::state.draft.filename = "[bad].csv";
        gui::exportData();
        if (gui::state.status != gui::Status::ExportError || !gui::state.result)
            throw std::runtime_error("invalid output template discarded the generated result");
        gui::state.draft.filename = "repaired.csv";
        gui::state.outputChanged();
        if (!gui::state.validResult() || gui::state.revision != revision)
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
        const auto entered = gui::state.resultEntered;
        gui::state.start();
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
        if (!cancel || !cancel->onClick || cancel->disabled)
            throw std::runtime_error("generation action does not expose cancellation");
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
