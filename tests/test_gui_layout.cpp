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
    for (float width : {588.f, 1020.f, 1232.f})
        for (int method : {0, 1}) {
            gui::state.draft.analysisMethod = method;
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
        << "options categories fit three widths, retain fixed tabs and preserve generated data\n";
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
            for (int other = 0; other < 5; ++other)
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
        items.push_back("model-" + std::to_string(i));
    items.back() += "-very-long-model-name-that-must-not-expand-the-popup";
    int selected = 63, changes = 0;
    eui::Screen screen{320, 240};
    eui::Ui ui;
    const auto compose = [&] {
        ui.begin("long-combo-menu");
        gui::combo(ui, "long.models", items, selected, 220, [&](int i) {
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
        require(ui.find("long.models")->onKeyEvent({value}), "键盘操作未处理");
        compose();
    };
    compose();
    ui.find("long.models")->onPress({}, {285, 190, 220, 40});
    compose();
    const auto *box = ui.find("menubox"), *scroll = ui.find("menuoverlay");
    require(box && scroll && box->frame.x >= 8 && box->frame.y >= 8 &&
                box->frame.x + box->frame.width <= screen.width - 7.9f &&
                box->frame.y + box->frame.height <= screen.height - 7.9f &&
                scroll->scrollMaxOffset > 0 && gui::menu.offset > 0 && visible(),
            "长列表越出窗口或打开时未显示当前项");
    require(ui.find("long.models.value")->clip && ui.find("menuitem63.text")->clip &&
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
    ui.find("long.models")->onPress({}, {8, 8, 220, 40});
    compose();
    key(eui::InputKey::Home);
    click(ui, "menuitem0");
    require(selected == 0 && changes == 2 && gui::menu.id.empty(),
            "点击菜单项未应用对应模型");
    compose();
    ui.find("long.models")->onPress({}, {8, 8, 220, 40});
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
    ui.find("long.models")->onPress({}, {8, 8, 220, 40});
    items.clear();
    compose();
    require(gui::menu.id.empty() && ui.find("long.models")->disabled &&
                !ui.find("long.models")->focusable && !ui.find("menubox") &&
                ui.find("long.models.value")->text == "暂无可选项",
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
void ai_interaction(const msa::Result &generated) {
    const auto settings = gui::aiUi.settings;
    const auto revision = gui::state.revision;
    gui::state.draft = native_ui::from_config(generated.config);
    gui::state.result = generated;
    gui::state.resultRevision = revision;
    gui::state.status = gui::Status::Success;
    gui::state.message.clear();
    gui::visual.importedSelected = false;
    gui::aiUi.requestRevision = revision;
    gui::aiUi.requestConfig = gui::aiConfig();
    const auto original = gui::aiConfig();
    const auto context = msa::Json{{"current_config", original}};
    const auto clarify = msa::ai::parse_reply(
        R"({"schema_version":1,"task":"generate","action":"clarify","message":"请补充规格。","questions":["规格上下限是多少？"],"warnings":[],"config_patch":null})",
        true, context);
    gui::acceptAiReply(0, clarify);
    if (gui::state.busy() || gui::aiConfig() != original ||
        gui::aiUi.chats[0].wireMessages.size() != 1 ||
        msa::Json::parse(gui::aiUi.chats[0].wireMessages.back().content) != clarify.structured)
        throw std::runtime_error("AI 澄清改变了研究，或历史未保存校验后的 JSON");
    gui::appendAi(0, "local", "本机提示，仅用于显示。");
    if (gui::aiUi.chats[0].wireMessages.size() != 1 ||
        gui::aiMessageRole(gui::aiUi.chats[0].messages.back()) != "本机结果")
        throw std::runtime_error("本机提示污染了模型协议历史");
    auto diagnostic = msa::ai::parse_reply(
        R"({"schema_version":1,"task":"generate","action":"diagnose","message":"可以提高精度后重试。","questions":[],"warnings":["需用户确认"],"config_patch":{"generation":{"decimal_places":5}}})",
        true, context);
    gui::acceptAiReply(0, diagnostic);
    if (!gui::aiUi.suggestion || gui::state.busy() || gui::aiConfig() != original)
        throw std::runtime_error("AI 诊断建议未等待用户确认");
    ++gui::state.revision;
    gui::pollAi();
    if (gui::aiUi.suggestion)
        throw std::runtime_error("配置变化后未丢弃过期重试建议");
    gui::acceptAiReply(0, diagnostic);
    if (gui::aiUi.suggestion || gui::state.busy())
        throw std::runtime_error("过期 AI 回复被应用");
    gui::aiUi.requestRevision = gui::state.revision;
    auto invalid = diagnostic;
    invalid.action = "generate";
    invalid.config_patch = msa::Json{{"design", {{"operators", 4}}}};
    gui::acceptAiReply(0, invalid);
    if (gui::aiConfig() != original || gui::state.busy() ||
        gui::aiUi.chats[0].errorCode != "invalid_proposal")
        throw std::runtime_error("不合法建议未保留原配置或缺少恢复提示");
    gui::aiUi.mode = 0;
    gui::showAiError(0, "response_schema", "响应格式不符", "请检查模型后重试。", true);
    ++gui::state.revision;
    gui::retryAiRequest();
    if (gui::aiUi.requesting || gui::aiUi.chats[0].canRetry ||
        gui::aiUi.chats[0].errorCode != "stale_request")
        throw std::runtime_error("重试按钮发送了旧研究请求");
    gui::aiUi.chats[0].wireMessages.assign(80, {"user", "历史问题"});
    gui::aiUi.chats[0].input = "请继续解释。";
    gui::sendAi();
    if (gui::aiUi.requesting || gui::aiUi.chats[0].input.empty() ||
        gui::aiUi.chats[0].wireMessages.size() != 80 ||
        gui::aiUi.chats[0].errorCode != "context_limit" ||
        gui::aiUi.chats[0].recovery.find("新建会话") == std::string::npos)
        throw std::runtime_error("同步上下文错误丢失恢复说明、用户输入或回滚历史");
    gui::aiUi.chats[0].wireMessages.clear();
    gui::aiUi.generating = true;
    gui::aiUi.generationRevision = gui::state.revision - 1;
    gui::aiUi.generationConfig = original;
    gui::pollAi();
    if (gui::aiUi.generating || gui::aiUi.requesting ||
        gui::aiUi.localGeneration.at("status") != "stale" ||
        gui::aiUi.localGeneration.at("config") != original)
        throw std::runtime_error("旧本机任务启动了诊断或丢失了原研究快照");
    gui::state.resultRevision = gui::state.revision;
    gui::aiUi.chats[1].datasetSignature = gui::interpretationData().dump();
    const auto evaluation = msa::ai::parse_reply(
        R"({"schema_version":1,"task":"interpret","action":"evaluate","message":"已按本机结果评价。","questions":[],"warnings":["模拟数据不能替代实际测量"],"evaluation":{"conclusion":"仅作研究方法演示。","findings":[{"metric":"GRR_pct","assessment":"参考本机指标与适用条件。"}],"recommendations":["使用现场测量数据复核。"]}})",
        false, gui::interpretationData());
    gui::acceptAiReply(1, evaluation);
    if (gui::aiUi.chats[1].wireMessages.size() != 1)
        throw std::runtime_error("有效评价未进入结构化历史");
    gui::visual.importedSelected = true;
    gui::acceptAiReply(1, evaluation);
    if (gui::aiUi.chats[1].wireMessages.size() != 1 ||
        gui::aiUi.chats[1].messages.back().content.find("快照") == std::string::npos)
        throw std::runtime_error("数据切换后的旧评价未被明确标识");
    gui::visual.importedSelected = false;
    gui::aiUi.settings.api_key = "test-mask-value";
    gui::aiUi.showKey = false;
    for (float width : {588.f, 1020.f, 1232.f}) {
        for (int mode = 0; mode < 2; ++mode)
            for (const float height : {324.f, 500.f}) {
                gui::aiUi.mode = mode;
                gui::showAiError(mode, "response_schema", "响应不符合结构要求。",
                                 "修正模型或提示词后，手动重试本轮。", true);
                eui::Ui page;
                page.begin("ai-page-layout");
                page.column("ai-host")
                    .width(width)
                    .content([&] {
                        gui::assistantPage(page, width, height);
                        page.row("ai-footer")
                            .width(width)
                            .gap(12)
                            .content([&] { gui::assistantFooter(page, width); })
                            .build();
                    })
                    .build();
                page.end();
                page.layout(width, height + 100);
                controls_fit(page, width, "AI 对话与错误恢复");
                if (!page.find("ai.error.retry") || !page.find("ai.error.recovery"))
                    throw std::runtime_error("AI 异常缺少恢复入口");
                const auto *composer = page.find("ai.composer." + std::to_string(mode));
                const auto *feedback = page.find("ai.composer.feedback");
                if (!composer || !feedback ||
                    composer->frame.y + composer->frame.height > height + .1f ||
                    feedback->frame.y + feedback->frame.height > height + .1f)
                    throw std::runtime_error("助手在最小窗口高度时输入框或反馈栏越出正文底部");
            }
        eui::Ui options;
        options.begin("ai-options-layout");
        options.column("ai-options-host")
            .width(width)
            .content([&] { gui::aiSettings(options, width); })
            .build();
        options.end();
        options.layout(width, 1600);
        controls_fit(options, width, "AI 配置");
        const auto inspect = [&](const auto &self, const core::dsl::Element &node) -> void {
            if (node.text.find("test-mask-value") != std::string::npos)
                throw std::runtime_error("隐藏密钥仍出现在可渲染文本中");
            for (const auto &child : node.children)
                self(self, *child);
        };
        for (const auto &root : options.roots())
            inspect(inspect, *root);
    }
    gui::aiUi.settings = settings;
    gui::aiUi.mode = 0;
    gui::aiUi.chats = {};
    gui::aiUi.localGeneration = nullptr;
    gui::aiUi.suggestion.reset();
    gui::state.revision = revision;
    gui::state.resultRevision = revision;
    gui::state.message.clear();
    std::cout
        << "AI 澄清、诊断确认、过期保护、非法参数恢复、数据快照、密钥掩码及三种宽度布局通过\n";
}

void ai_presentation_preferences(const msa::Result &generated) {
    const auto settings = gui::aiUi.settings;
    const auto timeout = gui::aiUi.timeout;
    const auto chats = gui::aiUi.chats;
    const auto suggestion = gui::aiUi.suggestion;
    const auto localGeneration = gui::aiUi.localGeneration;
    const auto mode = gui::aiUi.mode, promptTab = gui::aiUi.promptTab;
    const bool dirty = gui::aiUi.dirty, showKey = gui::aiUi.showKey,
               focusComposer = gui::aiUi.focusComposer;
    const auto revision = gui::state.revision;
    const auto original = gui::aiConfig();
    const auto require = [](bool value, const std::string &message) {
        if (!value)
            throw std::runtime_error("AI 界面操作：" + message);
    };
    gui::aiUi.chats = {};
    gui::aiUi.mode = 0;
    gui::aiUi.promptTab = 0;
    gui::aiUi.settings = msa::ai::defaults();
    gui::aiUi.settings.api_key = "ui-test-key-never-sent";
    gui::aiUi.dirty = false;
    eui::Ui options;
    const auto composeOptions = [&] {
        options.begin("ai-preferences-interaction");
        options.column("ai-preferences-host")
            .width(588)
            .content([&] { gui::aiSettings(options, 588); })
            .build();
        options.end();
        options.layout(588, 1800);
        controls_fit(options, 588, "AI 配置快捷操作");
    };
    const auto replaceInput = [&](const std::string &id, const std::string &value) {
        const auto *hit = options.find(id + ".hit");
        require(hit && hit->onKeyEvent && hit->onTextInput, "缺少可编辑提示词 " + id);
        eui::KeyEvent select{eui::InputKey::A};
        select.modifiers.control = true;
        hit->onKeyEvent(select);
        eui::TextInputEvent text;
        text.text = value;
        hit->onTextInput(text);
    };
    composeOptions();
    require(options.state<components::input_detail::InputModel::InputState>("ai.generation.prompt")
                    .verticalScroll == 0,
            "首次打开长系统提示词没有从开头显示");
    click(options, "ai.provider.item.0.bg");
    options.find("ai.provider.item.0.bg")->onKeyEvent({eui::InputKey::Space});
    require(gui::aiUi.settings.api_key == "ui-test-key-never-sent" && !gui::aiUi.dirty,
            "重复选择当前服务清空了密钥或产生未保存变更");
    click(options, "ai.provider.item.1.bg");
    require(gui::aiUi.settings.provider == "openai_compatible" &&
                gui::aiUi.settings.api_key.empty(),
            "更换服务没有按规则清空旧密钥");
    composeOptions();
    click(options, "ai.timeout.presets.item.1.bg");
    require(gui::aiUi.timeout == "60", "等待时限快捷项未更新输入值");
    const std::string generationDraft = "生成提示词草稿：先确认必要条件，再提出参数。";
    const std::string interpretationDraft = "解读提示词草稿：围绕本机指标说明适用范围。";
    replaceInput("ai.generation.prompt", generationDraft);
    gui::aiUi.dirty = false;
    click(options, "ai.prompts.task.item.1.bg");
    require(!gui::aiUi.dirty, "只切换提示词分类产生了配置变更");
    composeOptions();
    require(options.find("ai.interpretation.prompt.hit") &&
                !options.find("ai.generation.prompt.hit"),
            "提示词分类混排或缺少当前编辑器");
    replaceInput("ai.interpretation.prompt", interpretationDraft);
    click(options, "ai.prompts.task.item.0.bg");
    composeOptions();
    require(options.find("ai.generation.prompt.hit") &&
                !options.find("ai.interpretation.prompt.hit") &&
                gui::aiUi.settings.generation_system_prompt == generationDraft &&
                gui::aiUi.settings.interpretation_system_prompt == interpretationDraft,
            "切换提示词分类丢失或串用了草稿");
    for (int task = 0; task < 2; ++task) {
        gui::aiUi.mode = task;
        gui::aiUi.chats[task] = gui::AiConversation{};
        eui::Ui page;
        page.begin("ai-empty-example-interaction");
        gui::assistantPage(page, 588, 324);
        page.end();
        page.layout(588, 324);
        const auto id = "ai.example." + std::to_string(task);
        click(page, id + ".0.bg");
        const auto first = gui::aiUi.chats[task].input;
        require(!first.empty(), "空状态示例没有填入编辑框");
        click(page, id + ".1.bg");
        require(gui::aiUi.chats[task].input.rfind(first + "\n\n", 0) == 0 &&
                    gui::aiUi.chats[task].input.size() > first.size() + 2 &&
                    gui::aiUi.chats[task].messages.empty() &&
                    gui::aiUi.chats[task].wireMessages.empty() && !gui::aiUi.requesting &&
                    !gui::aiUi.generating && !gui::state.busy(),
                "追加示例覆盖草稿、写入协议历史或意外发送/生成");
    }
    for (int task = 0; task < 2; ++task) {
        for (int other = 0; other < 2; ++other) {
            auto &chat = gui::aiUi.chats[other];
            chat = gui::AiConversation{};
            chat.input = "保留草稿 " + std::to_string(other);
            chat.messages.push_back({"user", "保留显示消息 " + std::to_string(other)});
            chat.wireMessages.push_back({"user", "保留协议消息 " + std::to_string(other)});
            chat.datasetSignature = "数据快照 " + std::to_string(other);
        }
        const msa::Json proposal{{"generation", {{"decimal_places", 5}}}};
        const msa::Json local{{"status", "failed"}, {"reason", "保留失败诊断"}};
        gui::aiUi.suggestion = proposal;
        gui::aiUi.localGeneration = local;
        gui::aiUi.mode = task;
        eui::Ui page;
        page.begin("ai-new-conversation-isolation");
        gui::assistantPage(page, 588, 324);
        page.end();
        page.layout(588, 324);
        click(page, "ai.clear.bg");
        const auto &cleared = gui::aiUi.chats[task], &preserved = gui::aiUi.chats[1 - task];
        require(cleared.input.empty() && cleared.messages.empty() && cleared.wireMessages.empty() &&
                    cleared.datasetSignature.empty() &&
                    preserved.input == "保留草稿 " + std::to_string(1 - task) &&
                    preserved.messages.size() == 1 && preserved.wireMessages.size() == 1 &&
                    preserved.datasetSignature == "数据快照 " + std::to_string(1 - task),
                "新建会话没有只清理当前任务的输入、历史与快照");
        require(task == 0 ? (!gui::aiUi.suggestion && gui::aiUi.localGeneration.is_null())
                          : (gui::aiUi.suggestion && *gui::aiUi.suggestion == proposal &&
                             gui::aiUi.localGeneration == local),
                "新建解读会话清除了生成建议，或新建生成会话没有清理旧诊断");
    }
    require(gui::aiConfig() == original && gui::state.revision == revision &&
                gui::state.validResult() && gui::state.result->matrix == generated.matrix &&
                !gui::aiUi.requesting && !gui::aiUi.generating,
            "AI 界面设置或会话管理改变了研究、数据或请求状态");
    gui::aiUi.settings = settings;
    gui::aiUi.timeout = timeout;
    gui::aiUi.chats = chats;
    gui::aiUi.suggestion = suggestion;
    gui::aiUi.localGeneration = localGeneration;
    gui::aiUi.mode = mode;
    gui::aiUi.promptTab = promptTab;
    gui::aiUi.dirty = dirty;
    gui::aiUi.showKey = showKey;
    gui::aiUi.focusComposer = focusComposer;
    std::cout << "AI 示例只填输入、提示词草稿隔离、同值服务保钥、会话隔离及研究状态保持通过\n";
}

void ai_service_configuration(const msa::Result &generated) {
    const auto settings = gui::aiUi.settings;
    const auto models = gui::aiUi.models;
    const auto summary = gui::aiUi.serviceSummary, detail = gui::aiUi.serviceDetail;
    const bool loaded = gui::aiUi.modelsLoaded, failed = gui::aiUi.serviceFailed,
               busy = gui::aiUi.serviceBusy, dirty = gui::aiUi.dirty;
    const auto action = gui::aiUi.serviceAction;
    const auto serviceRevision = gui::aiUi.serviceRevision,
               activeRevision = gui::aiUi.activeServiceRevision;
    const float serviceOffset = gui::aiUi.serviceOffset;
    const auto optionsTab = gui::state.optionsTab;
    const float configOffset = gui::state.configOffset;
    const auto savedMenu = gui::menu;
    const auto revision = gui::state.revision;
    const auto original = gui::aiConfig();
    const auto require = [](bool value, const std::string &message) {
        if (!value)
            throw std::runtime_error("AI 连接配置：" + message);
    };
    require(!gui::aiUi.requesting && !gui::aiUi.generating && !busy && !gui::state.busy(),
            "回归开始前存在请求或生成任务");
    gui::aiUi.settings = msa::ai::defaults();
    gui::aiUi.settings.api_key = "ui-only-placeholder-never-sent";
    gui::aiUi.models.clear();
    gui::aiUi.modelsLoaded = false;
    gui::aiUi.serviceFailed = false;
    gui::aiUi.serviceSummary = "尚未验证连接";
    gui::aiUi.serviceDetail = "填写接口和密钥，拉取模型后选择并测试。";
    gui::aiUi.serviceOffset = 0;
    gui::state.optionsTab = 3;
    gui::state.configOffset = 0;
    gui::menu = {};
    eui::Ui options;
    float width = 588;
    constexpr float pageHeight = 2400;
    const auto compose = [&] {
        options.begin("ai-service-configuration");
        gui::optionsPage(options, width, pageHeight);
        gui::menuOverlay(options, {width, pageHeight});
        options.end();
        options.layout(width, pageHeight);
        controls_fit(options, width, "AI 服务连接布局");
    };
    const auto enabled = [&](const std::string &id) {
        const auto *target = options.find(id);
        require(target != nullptr, "缺少控件 " + id);
        bool found = false, result = false;
        std::function<void(const core::dsl::Element &, bool)> walk =
            [&](const core::dsl::Element &node, bool disabled) {
                disabled = disabled || node.disabled;
                // Ui 为节点 ID 添加页面前缀；按已解析的节点判断，避免误报控件缺失。
                if (&node == target) {
                    found = true;
                    result = !disabled;
                }
                for (const auto &child : node.children)
                    walk(*child, disabled);
            };
        for (const auto &root : options.roots())
            walk(*root, false);
        require(found, "缺少控件 " + id);
        return result;
    };
    const auto replaceInput = [&](const std::string &id, const std::string &value) {
        const auto *hit = options.find(id + ".hit");
        require(hit && hit->onKeyEvent && hit->onTextInput && enabled(id + ".hit"),
                "缺少可编辑连接字段 " + id);
        eui::KeyEvent select{eui::InputKey::A};
        select.modifiers.control = true;
        hit->onKeyEvent(select);
        eui::TextInputEvent input;
        input.text = value;
        hit->onTextInput(input);
    };
    const auto markFeedback = [&] {
        gui::aiUi.serviceSummary = "连接测试通过";
        gui::aiUi.serviceDetail = "旧接口的测试和余额结果";
        gui::aiUi.serviceFailed = true;
        gui::aiUi.serviceOffset = 30;
    };
    const auto invalidated = [&] {
        return !gui::aiUi.serviceFailed && gui::aiUi.serviceOffset == 0 &&
               gui::aiUi.serviceSummary == "连接配置已改变，需重新验证";
    };
    compose();
    const auto defaultModel = gui::aiUi.settings.model;
    require(gui::aiModelChoices() == std::vector<std::string>{defaultModel} &&
                options.find("ai.model.value")->text.find("当前配置，未验证") != std::string::npos &&
                enabled("ai.models.fetch.bg") && enabled("ai.connection.test.bg") &&
                enabled("ai.balance.query.bg") && !enabled("ai.service.cancel.bg"),
            "未探测服务时伪造可选模型，或初始按钮状态错误");
    for (int i = 0; i < 60; ++i)
        gui::aiUi.models.push_back("service-model-" + std::to_string(i));
    gui::aiUi.modelsLoaded = true;
    markFeedback();
    const auto beforeChoice = gui::aiUi.serviceRevision;
    compose();
    require(gui::aiModelChoices().size() == 61 && gui::aiModelChoices().front() == defaultModel,
            "已保存但服务列表缺失的模型没有保留在首项");
    options.find("ai.model")->onPress({}, {24, 300, 520, 40});
    compose();
    options.find("ai.model")->onKeyEvent({eui::InputKey::End});
    compose();
    require(gui::menu.highlighted == 60 && gui::menu.offset > 0,
            "真实模型下拉没有滚动至末项");
    click(options, "menuitem60");
    require(gui::aiUi.settings.model == "service-model-59" && gui::aiUi.models.size() == 60 &&
                gui::aiUi.modelsLoaded && invalidated() && gui::aiUi.dirty &&
                gui::aiUi.serviceRevision == beforeChoice + 1,
            "选择模型未清理旧反馈，或错误地清除了已拉取列表");
    compose();
    require(options.find("ai.model.value")->text == "service-model-59",
            "已发现的模型仍标为未验证的配置占位项");
    markFeedback();
    replaceInput("ai.key", "another-ui-placeholder-never-sent");
    require(gui::aiUi.models.empty() && !gui::aiUi.modelsLoaded && invalidated() &&
                gui::aiUi.settings.model == "service-model-59",
            "修改密钥未失效旧模型缓存，或丢失已选模型");
    compose();
    gui::aiUi.models = {"stale-model"};
    gui::aiUi.modelsLoaded = true;
    markFeedback();
    replaceInput("ai.url", "https://ui-test.invalid/v1");
    require(gui::aiUi.settings.api_key.empty() && gui::aiUi.settings.model.empty() &&
                gui::aiUi.models.empty() && !gui::aiUi.modelsLoaded && invalidated(),
            "修改服务地址未清理密钥、模型及旧反馈");
    compose();
    require(!enabled("ai.model") && !enabled("ai.connection.test.bg") &&
                !enabled("ai.balance.query.bg") &&
                options.find("ai.model.value")->text == "请先拉取模型列表",
            "空模型占位仍可选择、测试，或自定义地址错误开放官方余额查询");
    gui::aiUi.settings.api_key = "ui-only-placeholder-never-sent";
    gui::aiUi.settings.model = "stale-model";
    gui::aiUi.models = {"stale-model"};
    gui::aiUi.modelsLoaded = true;
    markFeedback();
    compose();
    click(options, "ai.provider.item.1.bg");
    require(gui::aiUi.settings.provider == "openai_compatible" &&
                gui::aiUi.settings.api_key.empty() && gui::aiUi.settings.model.empty() &&
                gui::aiUi.models.empty() && !gui::aiUi.modelsLoaded && invalidated(),
            "切换服务类型未清理旧凭据和模型缓存");
    gui::aiUi.settings = msa::ai::defaults();
    gui::aiUi.models = {gui::aiUi.settings.model};
    gui::aiUi.modelsLoaded = true;
    gui::aiUi.serviceSummary = "尚未验证连接";
    gui::aiUi.serviceDetail = "请测试所选模型。";
    gui::aiUi.serviceFailed = false;
    for (const float size : {588.f, 1232.f}) {
        width = size;
        compose();
        std::map<std::string, eui::Rect> baseline;
        for (const auto *id : {"ai.connection.card", "ai.service.status", "ai.connection.note",
                               "ai.advanced.card", "ai.prompts.card", "ai.service.cancel.bg"}) {
            const auto *node = options.find(id);
            require(node != nullptr, std::string("缺少固定布局锚点 ") + id);
            baseline[id] = {node->frame.x, node->frame.y, node->frame.width, node->frame.height};
        }
        for (int feedback = 0; feedback < 3; ++feedback) {
            gui::aiUi.serviceBusy = feedback == 0;
            gui::aiUi.serviceFailed = feedback == 2;
            gui::aiUi.serviceSummary = feedback == 0 ? "正在测试所选模型…"
                                                       : feedback == 1 ? "账户有可用余额"
                                                                       : "服务暂时无法连接";
            gui::aiUi.serviceDetail.clear();
            for (int i = 0; i < (feedback == 2 ? 20 : 2); ++i)
                gui::aiUi.serviceDetail += "连接详情与恢复建议；原配置和模型选择保留。\n";
            compose();
            require(std::fabs(options.find("ai.service.status")->frame.height - 120) < .1f,
                    "连接状态区域未保持固定高度");
            for (const auto &[id, frame] : baseline) {
                const auto &next = options.find(id)->frame;
                require(std::fabs(frame.x - next.x) < .1f && std::fabs(frame.y - next.y) < .1f &&
                            std::fabs(frame.width - next.width) < .1f &&
                            std::fabs(frame.height - next.height) < .1f,
                        "等待、余额或长错误反馈使页面布局移动：" + id);
            }
            if (gui::aiUi.serviceBusy) {
                for (const auto *id : {"ai.provider.item.0.bg", "ai.url.hit", "ai.key.hit",
                                       "ai.model", "ai.timeout.hit", "ai.prompts.reset.bg",
                                       "ai.models.fetch.bg", "ai.connection.test.bg",
                                       "ai.balance.query.bg"})
                    require(!enabled(id), std::string("请求期间字段仍可改动：") + id);
                require(enabled("ai.service.cancel.bg"), "父容器锁定阻断了取消请求按钮");
            } else
                require(enabled("ai.model") && enabled("ai.url.hit") &&
                            enabled("ai.models.fetch.bg") && !enabled("ai.service.cancel.bg"),
                        "请求结束后字段未解锁或取消按钮仍可操作");
        }
    }
    require(gui::aiConfig() == original && gui::state.revision == revision &&
                gui::state.validResult() && gui::state.result->matrix == generated.matrix &&
                !gui::aiUi.requesting && !gui::aiUi.generating &&
                gui::aiUi.serviceClient.view().status != msa::ai::Status::Running,
            "连接配置操作修改研究数据或意外发起了网络请求");
    gui::aiUi.settings = settings;
    gui::aiUi.models = models;
    gui::aiUi.modelsLoaded = loaded;
    gui::aiUi.serviceFailed = failed;
    gui::aiUi.serviceBusy = busy;
    gui::aiUi.dirty = dirty;
    gui::aiUi.serviceAction = action;
    gui::aiUi.serviceRevision = serviceRevision;
    gui::aiUi.activeServiceRevision = activeRevision;
    gui::aiUi.serviceSummary = summary;
    gui::aiUi.serviceDetail = detail;
    gui::aiUi.serviceOffset = serviceOffset;
    gui::state.optionsTab = optionsTab;
    gui::state.configOffset = configOffset;
    gui::menu = savedMenu;
    std::cout << "AI 模型选择、服务变更失效、请求锁定/取消入口及固定状态区布局通过\n";
}

void ai_request_session_isolation(const msa::Result &generated, const std::string &server) {
    if (server.rfind("http://127.0.0.1:", 0) != 0)
        throw std::runtime_error("会话回归仅允许本机模拟服务");
    gui::state.draft = native_ui::from_config(generated.config);
    gui::state.result = generated;
    gui::state.resultRevision = gui::state.revision;
    gui::state.status = gui::Status::Success;
    gui::state.message.clear();
    gui::visual.importedSelected = false;
    gui::aiUi.chats = {};
    gui::aiUi.settings = msa::ai::defaults();
    gui::aiUi.settings.provider = "openai_compatible";
    gui::aiUi.settings.model = "local-mock-model";
    gui::aiUi.settings.api_key = "gui-session-loopback-test";
    gui::aiUi.timeout = "10";
    const auto cfg = gui::aiConfig();
    const auto revision = gui::state.revision;
    gui::aiUi.requestRevision = revision;
    gui::aiUi.requestConfig = cfg;
    const auto diagnostic = msa::ai::parse_reply(
        R"({"schema_version":1,"task":"generate","action":"diagnose","message":"请确认精度调整。","questions":[],"warnings":[],"config_patch":{"generation":{"decimal_places":5}}})",
        true, msa::Json{{"current_config", cfg}});
    const auto require = [](bool value, const char *message) {
        if (!value)
            throw std::runtime_error(message);
    };
    const auto finish = [&] {
        const auto deadline = gui::Clock::now() + std::chrono::seconds(15);
        while (gui::aiUi.requesting && gui::Clock::now() < deadline) {
            gui::pollAi();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        require(!gui::aiUi.requesting, "本地会话回归请求超时");
    };
    for (const auto *route : {"success-interpret", "status-500"}) {
        gui::acceptAiReply(0, diagnostic);
        const auto proposal = gui::aiUi.suggestion;
        const auto generationMessages = gui::aiUi.chats[0].messages.size();
        gui::aiUi.mode = 1;
        gui::aiUi.chats[1] = {};
        gui::aiUi.chats[1].input = "请评价当前研究结果。";
        gui::aiUi.settings.base_url = server + "/" + route;
        gui::sendAi();
        require(gui::aiUi.requesting && gui::aiUi.chats[1].input.empty(),
                "解读请求未通过真实发送路径");
        require(gui::aiUi.suggestion == proposal, "发送解读请求清除了生成建议");
        finish();
        require(gui::aiUi.suggestion == proposal &&
                    gui::aiUi.chats[0].messages.size() == generationMessages,
                "解读成功或错误恢复污染了生成会话");
        if (std::string(route) == "success-interpret")
            require(gui::aiUi.chats[1].wireMessages.size() == 2 &&
                        gui::aiUi.chats[1].wireMessages.back().role == "assistant",
                    "本地结构化解读没有完成");
        else
            require(!gui::aiUi.chats[1].errorCode.empty() && gui::aiUi.chats[1].canRetry,
                    "失败的解读请求缺少恢复状态");
        gui::aiUi.mode = 0;
        eui::Ui footer;
        footer.begin("preserved-generation-suggestion");
        gui::assistantFooter(footer, 588);
        footer.end();
        footer.layout(588, 100);
        require(footer.find("ai.retry.bg") != nullptr,
                "解读结束后生成建议无法通过按钮继续使用");
    }
    // 同步启动失败保留建议与草稿；真正发送新的生成需求后才清除旧建议。
    gui::aiUi.mode = 0;
    gui::aiUi.chats[0].input = "请先确认研究条件。";
    gui::aiUi.settings.base_url = "invalid-url";
    gui::sendAi();
    require(gui::aiUi.suggestion.has_value() && !gui::aiUi.chats[0].input.empty() &&
                !gui::aiUi.requesting, "发送前校验失败丢失了原建议或草稿");
    gui::aiUi.settings.base_url = server + "/gui-clarify";
    gui::sendAi();
    require(gui::aiUi.requesting && !gui::aiUi.suggestion,
            "新生成请求没有清除已被取代的旧建议");
    finish();
    require(!gui::aiUi.generating && !gui::state.busy() && gui::aiConfig() == cfg &&
                gui::state.revision == revision && gui::state.result->matrix == generated.matrix,
            "会话隔离回归改变了研究或测量数据");
    gui::acceptAiReply(0, diagnostic);
    ++gui::state.revision;
    gui::pollAi();
    require(!gui::aiUi.suggestion, "研究参数改变后过期建议仍被保留");
    gui::aiUi.client.stop();
    std::cout << "AI 实际发送、成功/失败回复、诊断按钮、同步失败与过期保护的会话隔离通过\n";
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
    if (argc != 3 && !(argc == 5 && std::wstring(argv[3]) == L"--ai-sessions"))
        return 2;
    try {
        const auto font = std::filesystem::path(argv[1]).u8string();
        core::TextPrimitive::setDefaultFontFiles(font, font);
        const auto cfg = msa::load_config(std::filesystem::path(argv[2]));
        const auto generated = msa::generate(cfg);
        if (argc == 5) {
            ai_request_session_isolation(generated, std::filesystem::path(argv[4]).u8string());
            return 0;
        }
        gui::state.draft = native_ui::from_config(cfg);
        gui::state.page = gui::Page::Data;
        gui::state.resultEntered = gui::Clock::now() - std::chrono::seconds(1);
        gui::state.message.clear();
        initial_window_bounds();
        render_request_lifecycle();
        state_recovery(generated);
        combo_unchanged_value();
        long_combo_menu();
        shared_choice_controls(generated);
        shared_choice_keyboard();
        disclosure_and_reduced_motion(generated);
        runtime_keyboard_navigation();
        xbar_details(generated);
        options_navigation(generated);
        help_topics();
        scrolled_menu_anchors();
        help_destinations();
        logs_and_about_layout();
        ai_interaction(generated);
        ai_presentation_preferences(generated);
        ai_service_configuration(generated);
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
        gui::outputConfig(outputUi, 620);
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
        gui::outputConfig(longUi, 620);
        longUi.end();
        longUi.layout(620, 1800);
        longUi.find("runorder.operators.hit")->onClick();
        if (gui::state.draft.randomOperators)
            throw std::runtime_error(
                "operator randomization is active outside within-operator mode");
        longUi.find("runorder.mode.2.hit")->onClick();
        eui::Ui withinUi;
        withinUi.begin("within-settings-test");
        gui::outputConfig(withinUi, 620);
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
