// Native MSA application: EUI-NEO presentation; shared, GUI-free business kernel.
#include "eui_neo.h"
#include "core/render/text.h"
#include "nlohmann/json.hpp"
#include "platform_win.hpp"
#include "platform/embedded_resources_win.hpp"
#include "ui_model.hpp"
#include "msa/brand.hpp"
#include "msa/task.hpp"
#include "msa/project.hpp"
#include "msa/charts.hpp"
#include "msa/comparison_report.hpp"
#include "platform/chart_export.hpp"
#include <dwmapi.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <future>
#include <iomanip>
#include <locale>
#include <sstream>
#include <thread>

namespace gui {
using eui::Ui;
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
constexpr const char *ink = "#182C45", *muted = "#52647A", *blue = msa::brand_blue, *line = "#DCE4EE",
                     *bg = "#F2F5F9", *surface = "#FFFFFF", *surfaceSoft = "#F5F8FC",
                     *surfaceHover = "#EDF3FA", *surfacePressed = "#DFEAF8",
                     *surfaceDisabled = "#EFF3F7", *inkDisabled = "#64748B", *blueWash = "#F3F6FE",
                     *blueWashActive = "#F1F4FE",
                     *green = "#176C52", *greenWash = "#E8F6EF", *tableHeader = "#EEF3F9";
const eui::Color brandColor(blue);
const auto brandBorder = core::mixColor(eui::Color(surface), brandColor, .20f);
constexpr const char *tableStripe = surfaceSoft;
constexpr float cardRadius = 12.f, controlRadius = 8.f, controlHeight = 40.f, sectionGap = 12.f,
                scrollGutter = 8.f, controlFont = 19.f, tableLabelFont = 16.f,
                tableValueFont = 17.5f;
constexpr eui::Color headerSurface = eui::Color::fromHex(0xFFFFFF);
constexpr COLORREF headerBackground =
    RGB(headerSurface.r * 255.f + .5f, headerSurface.g * 255.f + .5f,
        headerSurface.b * 255.f + .5f);
const std::array<std::string, 5> metricNames{"EV / TV %", "AV / TV %", "GRR / TV %", "P/T %",
                                             "ndc"};
const std::vector<std::string> modes{"不约束", "目标值", "区间", "不高于", "不低于"};
using native_ui::Draft;
using native_ui::Target;
enum class Status {
    Ready,
    Running,
    Cancelling,
    Success,
    Cancelled,
    Error,
    Infeasible,
    SearchFailed
};
enum class Page { Data, Charts, Logs, Options, Help, About };
constexpr int chartOptionsTab = 2, fileOptionsTab = 3;
enum class Level { Info, Warning, Error };
const char *levelColor(Level level) {
    return level == Level::Error ? "#9F3229" : level == Level::Warning ? "#80580B" : blue;
}
const char *levelSurface(Level level) {
    return level == Level::Error ? "#FCEDEA" : level == Level::Warning ? "#FFF3D9" : blueWash;
}
struct LogEntry {
    unsigned sequence;
    std::string time;
    Level level;
    std::string message;
};
constexpr std::size_t logLimit = 100;
std::string searchSummary(const Json &diagnostics) {
    if (diagnostics.empty())
        return {};
    const std::string reason = diagnostics.at("stop_reason");
    const auto &rejected = diagnostics.at("rejected");
    const std::string label = reason == "optimal_score"    ? "已达到最优评分"
                              : reason == "refinement_limit" ? "已完成候选比较"
                              : reason == "timeout"       ? "到达时间上限"
                                                           : "到达次数上限";
    return label + "。共尝试 " + diagnostics.at("attempts").dump() + " 次，合格 " +
           diagnostics.at("accepted").dump() + " 次。淘汰次数：构造 " +
           rejected.at("construction").dump() + "，缩放放置 " + rejected.at("placement").dump() +
           "，规格及指标 " + rejected.at("indicators").dump() + "，部件跨度 " +
           rejected.at("part_span").dump() + "，正态筛选 " + rejected.at("normality").dump() + "。";
}
struct State {
    Draft draft;
    Status status = Status::Ready;
    Page page = Page::Data;
    int resultTab = 0, previewPage = 0, logFilter = 0;
    int optionsTab = 0, helpTopic = 0;
    bool advanced = false, followLogs = true, logsCopied = false, versionCopied = false;
    bool motion = true;
    bool configDirty = true;
    Level footerLevel = Level::Info;
    Page footerPage = Page::Data;
    Clock::time_point footerEntered{};
    Clock::time_point resultEntered = Clock::now(), advancedChanged = Clock::now();
    float advancedFrom = 0;
    float generateOffset = 0, resultOffset = 0, configOffset = 0, pixelScale = 1;
    float previousWidth = 0, previousHeight = 0, limitScale = 0;
    unsigned systemDpi = 96;
    std::string message, dataExportError, footerMessage;
    std::function<decltype(platform::chooseExportPath)> chooseExportPath = platform::chooseExportPath;
    std::function<decltype(platform::chooseImportData)> chooseImportData = platform::chooseImportData;
    std::function<decltype(platform::chooseWorksheet)> chooseWorksheet = platform::chooseWorksheet;
    msa::ReplicateSelector chooseReplicates = platform::chooseReplicates;
    std::function<decltype(platform::chooseDirectory)> chooseDirectory = platform::chooseDirectory;
    float logOffset = 0, logPageOffset = 0, helpOffset = 0, aboutOffset = 0;
    unsigned logSequence = 0;
    std::vector<LogEntry> logs;
    unsigned revision = 0, resultRevision = 0, taskRevision = 0;
    std::thread worker;
    std::atomic<int> ticks{0};
    msa::SearchTask task;
    std::optional<msa::Result> result;
    bool found = false;
    std::atomic<bool> cancel{false};
    Clock::time_point started{};
    double elapsed = 0;
    State() {
        log("程序已就绪，可使用均值极差法（Xbar-R）或方差分析法（ANOVA）生成交叉量具 R&R 数据。");
    }
    ~State() { stop(); }
    void stop() {
        cancel = true;
        task.cancel();
        if (worker.joinable())
            worker.join();
        task.stop();
    }
    bool busy() const { return status == Status::Running || status == Status::Cancelling; }
    void requestCancel() {
        if (status != Status::Running)
            return;
        task.cancel();
        status = Status::Cancelling;
        cancel = true;
    }
    bool validResult() const { return !busy() && displayedResult(); }
    bool displayedResult() const { return result.has_value() && resultRevision == revision; }
    bool retainedResult() const {
        return displayedResult() &&
               (status == Status::Cancelled || status == Status::Error ||
                status == Status::Infeasible || status == Status::SearchFailed);
    }
    void navigate(Page next) {
        if (next == page)
            return;
        page = next;
        footerMessage.clear();
    }
    void hint(std::string summary, Level level = Level::Info) {
        footerMessage = std::move(summary);
        footerLevel = level;
        footerPage = page;
        footerEntered = Clock::now();
    }
    bool visibleHint() const {
        return !busy() && !footerMessage.empty() && footerPage == page &&
               Clock::now() - footerEntered < std::chrono::seconds(6);
    }
    void log(std::string s, Level level = Level::Info) {
        SYSTEMTIME now{};
        GetLocalTime(&now);
        char time[16]{};
        std::snprintf(time, sizeof(time), "%02u:%02u:%02u", now.wHour, now.wMinute, now.wSecond);
        if (logs.size() >= logLimit)
            logs.erase(logs.begin());
        logs.push_back({++logSequence, time, level, std::move(s)});
        logsCopied = false;
        if (followLogs)
            logOffset = 1000000.0f;
    }
    void feedback(std::string summary, std::string detail = {}, Level level = Level::Info) {
        hint(summary, level);
        log(detail.empty() ? std::move(summary) : std::move(detail), level);
    }
    void changed() {
        const bool invalidate = displayedResult();
        footerMessage.clear();
        message.clear();
        dataExportError.clear();
        configDirty = true;
        if (!busy())
            status = Status::Ready;
        ++revision;
        resultTab = 0;
        previewPage = 0;
        resultOffset = 0;
        if (invalidate)
            feedback("参数已修改，请重新生成", "研究参数已修改。请重新生成数据以更新结果。");
    }
    void outputChanged() {
        footerMessage.clear();
        configDirty = true;
        dataExportError.clear();
    }
    void sample() {
        const int method = draft.analysisMethod, interaction = draft.interactionMode;
        const std::string alpha = draft.interactionAlpha;
        const auto previous = draft;
        draft = Draft{};
        // 示例使用当前操作员名称和导出偏好。
        draft.operatorNames = previous.operatorNames;
        draft.directory = previous.directory;
        draft.filename = previous.filename;
        draft.existingFile = previous.existingFile;
        draft.csvFormat = previous.csvFormat;
        draft.runMode = previous.runMode;
        draft.randomOperators = previous.randomOperators;
        draft.storeStandardOrder = previous.storeStandardOrder;
        draft.runSeed = previous.runSeed;
        draft.dataFileType = previous.dataFileType;
        draft.silentCsv = previous.silentCsv;
        draft.silentCharts = previous.silentCharts;
        draft.silentReport = previous.silentReport;
        draft.reportFormat = previous.reportFormat;
        draft.reportDirectory = previous.reportDirectory;
        draft.importDirectory = previous.importDirectory;
        draft.importFilename = previous.importFilename;
        draft.chartDirectory = previous.chartDirectory;
        draft.chartFilename = previous.chartFilename;
        draft.charts = previous.charts;
        draft.reportFilename = previous.reportFilename;
        draft.analysisMethod = method;
        draft.interactionMode = interaction;
        draft.interactionAlpha = alpha;
        draft.lsl = "9.5";
        draft.usl = "10.5";
        draft.targets[0] = {1, "10", "", "0.1"};
        draft.targets[1] = {3, "10"};
        draft.targets[2] = {2, "10", "20"};
        draft.targets[3] = {3, "20"};
        draft.targets[4] = {4, "5"};
        changed();
        status = Status::Ready;
        feedback("示例设置已填入", "已载入示例设置，可生成模拟测量数据并复核指标。");
        message.clear();
    }
    void loadInitial() {
        try {
            const auto path = msa::config_path();
            if (std::filesystem::exists(path)) {
                draft = native_ui::from_config(msa::load_config(path));
                configDirty = false;
                log("已载入用户配置 " + path.u8string());
            } else
                log("当前使用默认设置。点击「保存配置」可供下次启动使用。");
        } catch (const std::exception &e) {
            message = std::string("无法载入用户配置。") + e.what();
            status = Status::Error;
            feedback("配置载入失败", message, Level::Error);
        }
    }
    void start() {
        if (busy())
            return;
        if (worker.joinable())
            worker.join();
        message.clear();
        dataExportError.clear();
        footerMessage.clear();
        try {
            const auto snapshot = msa::validate_request(
                native_ui::to_config(draft, native_ui::ConfigUse::Generation));
            task.start(snapshot);
        } catch (const msa::InfeasibleError &e) {
            status = Status::Infeasible;
            message = e.what();
            feedback("约束冲突，请调整参数", message, Level::Error);
            return;
        } catch (const std::exception &e) {
            status = Status::Error;
            message = e.what();
            feedback("参数校验失败", message, Level::Error);
            return;
        }
        status = Status::Running;
        cancel = false;
        ticks = 0;
        found = false;
        taskRevision = revision;
        started = Clock::now();
        log(draft.analysisMethod ? "开始生成模拟测量数据，使用 ANOVA 分析。"
                                 : "开始生成模拟测量数据，使用 Xbar-R 分析。");
        try {
            worker = std::thread([this] {
                for (;;) {
                    if (cancel.load())
                        task.cancel();
                    auto v = task.view();
                    app::requestUpdate();
                    if (v.status != msa::TaskStatus::Running &&
                        v.status != msa::TaskStatus::Cancelling)
                        break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
            });
        } catch (const std::exception &e) {
            task.stop();
            status = Status::Error;
            message = std::string("无法启动任务通知线程：") + e.what();
            feedback("生成启动失败", message, Level::Error);
        }
    }
    void poll() {
        if (!busy())
            return;
        if (cancel.load())
            task.cancel();
        auto v = task.view();
        ticks = v.attempts;
        elapsed = v.elapsed;
        found = v.found;
        if (v.status == msa::TaskStatus::Running || v.status == msa::TaskStatus::Cancelling)
            return;
        if (worker.joinable())
            worker.join();
        if (v.status == msa::TaskStatus::Cancelled) {
            status = Status::Cancelled;
            feedback(displayedResult() ? "已取消，保留上次结果" : "已取消生成",
                     displayedResult() ? "本次生成已取消。仍可复核或导出上次生成的数据。"
                                       : "本次生成已取消。可以再次生成。",
                     Level::Warning);
        } else if (taskRevision != revision) {
            status = Status::Ready;
            feedback("参数已修改，请重新生成", "生成期间研究参数有变更，请重新生成数据。");
        } else if (v.status == msa::TaskStatus::Success && v.result) {
            result = std::move(v.result);
            status = Status::Success;
            resultRevision = revision;
            if (!result->search_diagnostics.empty())
                log(searchSummary(result->search_diagnostics));
            feedback("数据已生成", "已找到满足生成约束的数据。尝试 " +
                                       std::to_string(result->attempts) + " 次，种子 " +
                                       std::to_string(result->seed) + "。可导出数据复验。");
        } else {
            status = v.status == msa::TaskStatus::Infeasible     ? Status::Infeasible
                     : v.status == msa::TaskStatus::SearchFailed ? Status::SearchFailed
                                                                 : Status::Error;
            message = v.error;
            feedback("生成失败，请查看日志", message, Level::Error);
        }
    }

} state;
std::string font() { return "memory:msa-noto-cjk"; }

std::future<std::shared_ptr<const platform::FontBytes>> &fontLoad() {
    static auto loading = [] {
        const auto load = [] { return platform::embeddedFont(); };
        try {
            return std::async(std::launch::async, load);
        } catch (const std::system_error &) {
            // 线程资源不足时保留同步加载路径，字体仍在首帧前完成注册。
            return std::async(std::launch::deferred, load);
        }
    }();
    return loading;
}

void initializeFont() {
    static const bool initialized = [] {
        const auto bytes = fontLoad().get();
        if (!core::TextPrimitive::registerMemoryFontShared(font(), bytes))
            throw std::runtime_error("无法加载内嵌中文字体");
        return true;
    }();
    (void)initialized;
}
auto tokens() {
    auto t = components::theme::light();
    t.primary = brandColor;
    t.background = eui::Color(bg);
    t.surface = eui::Color(surfaceSoft);
    t.surfaceHover = eui::Color(surfaceHover);
    t.surfaceActive = eui::Color(surfacePressed);
    t.text = eui::Color(ink);
    t.border = eui::Color(line);
    return t;
}
eui::Transition transition(float seconds = .18f) {
    auto value = eui::Transition::make(state.motion ? seconds : 0.0f, eui::Ease::OutCubic);
    // 常规控件仅过渡颜色；几何过渡交由分段指示条、开关和进度条明确启用。
    value.properties =
        eui::AnimProperty::Color | eui::AnimProperty::TextColor | eui::AnimProperty::Border;
    return value;
}
float reveal(Clock::time_point since, float duration = .18f) {
    if (!state.motion)
        return 1;
    const float t = std::clamp(
        std::chrono::duration<float>(Clock::now() - since).count() / duration, 0.0f, 1.0f);
    return 1 - (1 - t) * (1 - t) * (1 - t);
}
// Attach keyboard behavior to the component's actual hit area, so pointer and Tab
// focus use the same element. Keep this adaptation local to the application.
void keyboard(Ui &u, const std::string &id, std::function<bool(const eui::KeyEvent &)> fn) {
    if (auto *hit = u.find(id)) {
        hit->focusable = true;
        hit->interactive = true;
        hit->onKeyEvent = std::move(fn);
        if (!hit->disabled && u.isFocused(id))
            hit->border = {2, eui::Color(blue)};
    }
}
bool activate(const eui::KeyEvent &key) {
    return key.action == core::KeyAction::Press &&
           (key.key == eui::InputKey::Enter || key.key == eui::InputKey::Space);
}
void segments(Ui &u, const std::string &id, const std::vector<std::string> &items, int selected,
              float w, std::function<void(int)> select, float h = controlHeight,
              bool soft = false) {
    auto &choice = u.state<int>(id + ".choice");
    choice = selected;
    select = [&u, id, &choice, callback = std::move(select)](int next) {
        if (next != choice) {
            choice = next;
            callback(next);
        }
        u.requestFocus(id + ".hit." + std::to_string(next));
    };
    auto style = components::SegmentedStyle(tokens());
    if (soft) {
        style.selected = eui::Color(surface);
        style.selectedText = eui::Color(blue);
    }
    components::segmented(u, id)
        .size(w, h)
        .items(items)
        .selected(selected)
        .theme(tokens())
        .style(style)
        .fontSize(controlFont)
        .transition(transition())
        .onChange(select)
        .build();
    for (int i = 0; i < int(items.size()); ++i) {
        const auto hitId = id + ".hit." + std::to_string(i);
        keyboard(u, hitId,
                 [select, &choice, i, count = int(items.size())](const eui::KeyEvent &key) {
                     if (activate(key)) {
                         select(i);
                         return true;
                     }
                     if (!key.isDown())
                         return false;
                     int next = choice;
                     if (key.key == eui::InputKey::Right || key.key == eui::InputKey::Down)
                         next = (choice + 1) % count;
                     else if (key.key == eui::InputKey::Left || key.key == eui::InputKey::Up)
                         next = (choice + count - 1) % count;
                     else if (key.key == eui::InputKey::Home)
                         next = 0;
                     else if (key.key == eui::InputKey::End)
                         next = count - 1;
                     else
                         return false;
                     select(next);
                     return true;
                 });
        if (auto *hit = u.find(hitId))
            hit->focusable = i == selected;
    }
}
void tabs(Ui &u, const std::string &id, const std::vector<std::string> &items, int selected,
          float w, std::function<void(int)> select, float h = controlHeight) {
    auto &choice = u.state<int>(id + ".choice");
    choice = selected;
    select = [&u, id, &choice, callback = std::move(select)](int next) {
        if (next != choice) {
            choice = next;
            callback(next);
        }
        u.requestFocus(id + ".hit." + std::to_string(next));
    };
    auto style = components::TabsStyle(tokens());
    style.text = eui::Color(muted);
    components::tabs(u, id)
        .size(w, h)
        .items(items)
        .selected(selected)
        .fontSize(controlFont)
        .theme(tokens())
        .style(style)
        .transition(transition())
        .onChange(select)
        .build();
    for (int i = 0; i < int(items.size()); ++i) {
        const auto hitId = id + ".hit." + std::to_string(i);
        if (auto *hit = u.find(hitId)) {
            if (i == selected)
                hit->color = eui::Color(blueWash);
            hit->hoverColor = eui::Color(i == selected ? blueWashActive : surfaceHover);
            hit->pressedColor = eui::Color(i == selected ? blueWashActive : surfacePressed);
            hit->transition = transition(.12f);
        }
        keyboard(u, hitId,
                 [select, &choice, i, count = int(items.size())](const eui::KeyEvent &key) {
                     if (!key.isDown())
                         return false;
                     int next = choice;
                     if (activate(key))
                         next = i;
                     else if (key.key == eui::InputKey::Right || key.key == eui::InputKey::Down)
                         next = (choice + 1) % count;
                     else if (key.key == eui::InputKey::Left || key.key == eui::InputKey::Up)
                         next = (choice + count - 1) % count;
                     else if (key.key == eui::InputKey::Home)
                         next = 0;
                     else if (key.key == eui::InputKey::End)
                         next = count - 1;
                     else
                         return false;
                     select(next);
                     return true;
                 });
        if (auto *hit = u.find(hitId))
            hit->focusable = i == selected;
    }
}
void toggle(Ui &u, const std::string &id, const std::string &label, bool value, float w,
            std::function<void(bool)> change, bool disabled = false) {
    auto style = components::SwitchStyle(tokens());
    if (disabled) {
        style.off = eui::Color("#E2E7EF");
        style.on = eui::Color("#A7B7D9");
        style.text = eui::Color(inkDisabled);
        style.rowHover = style.rowPressed = eui::Color("#00000000");
    }
    components::toggleSwitch(u, id)
        .size(w, controlHeight)
        .checked(value)
        .text(label)
        .fontSize(controlFont)
        .trackSize(38, 22)
        .theme(tokens())
        .style(style)
        .transition(transition())
        .onChange([change, disabled](bool next) {
            if (!disabled)
                change(next);
        })
        .build();
    keyboard(u, id + ".hit", [change, value, disabled](const eui::KeyEvent &key) {
        if (disabled || !activate(key))
            return false;
        change(!value);
        return true;
    });
    if (auto *hit = u.find(id + ".hit"))
        hit->disabled = disabled;
}
void stepper(Ui &u, const std::string &id, std::string &value, float w, int lo, int hi) {
    const int current = std::clamp(std::atoi(value.c_str()), lo, hi);
    auto change = [&value](long long n) {
        const auto next = std::to_string(n);
        if (next != value) {
            value = next;
            state.changed();
        }
    };
    components::stepper(u, id)
        .size(w, 40)
        .value(current)
        .min(lo)
        .max(hi)
        .fontSize(20)
        .theme(tokens())
        .transition(transition())
        .onChange(change)
        .build();
    for (const auto &part : {std::string(".minus.bg"), std::string(".plus.bg")})
        keyboard(u, id + part, [change, current, lo, hi, part](const eui::KeyEvent &key) {
            int next = current;
            if (activate(key))
                next += part == ".minus.bg" ? -1 : 1;
            else if (key.isDown() &&
                     (key.key == eui::InputKey::Up || key.key == eui::InputKey::Right))
                ++next;
            else if (key.isDown() &&
                     (key.key == eui::InputKey::Down || key.key == eui::InputKey::Left))
                --next;
            else
                return false;
            change(std::clamp(next, lo, hi));
            return true;
        });
}
std::string number(double n, int dp = 1) {
    std::ostringstream s;
    s.imbue(std::locale::classic());
    s << std::fixed << std::setprecision(dp) << n;
    return s.str();
}
void text(Ui &u, const std::string &id, const std::string &s, float w, float h = 24,
          float size = 15, const char *c = ink) {
    u.text(id)
        .size(w, h)
        .text(s)
        .fontSize(size * 1.35f)
        .color(c)
        .verticalAlign(eui::VerticalAlign::Center)
        .build();
}
void paragraph(Ui &u, const std::string &id, const std::string &content, float w,
               const char *color = muted, float fontSize = 19) {
    u.text(id).width(w).text(content).wrap().maxWidth(w).fontSize(fontSize).color(color).build();
}
void badge(Ui &u, const std::string &id, const std::string &label, float w,
           const char *color = blue, const char *fill = blueWash) {
    u.stack(id)
        .size(w, 28)
        .content([&] {
            u.rect(id + ".bg").size(w, 28).color(fill).radius(14).build();
            u.text(id + ".text")
                .size(w, 28)
                .text(label)
                .fontSize(16)
                .color(color)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();
        })
        .build();
}
void button(Ui &u, const std::string &id, const std::string &s, float w, std::function<void()> fn,
            bool primary = false, bool disabled = false, float h = controlHeight) {
    const bool focus = !disabled && u.isFocused(id + ".bg");
    constexpr auto black = eui::Color::fromHex(0x000000);
    components::button(u, id)
        .size(w, h)
        .text(s)
        .fontSize(controlFont)
        .theme(tokens(), primary)
        .colors(disabled  ? surfaceDisabled
                : primary ? blue
                          : surface,
                primary ? core::mixColor(brandColor, black, .10f) : eui::Color(surfaceHover),
                primary ? core::mixColor(brandColor, black, .20f) : eui::Color(surfacePressed))
        .textColor(disabled  ? inkDisabled
                   : primary ? surface
                             : ink)
        .radius(controlRadius)
        .border(focus ? 2.0f : 1.0f, !disabled && (focus || primary) ? blue : line)
        .shadow(primary && !disabled ? 7.0f : 0.0f, 0, 2,
                components::theme::withAlpha(brandColor, .13f))
        .pressScale(1.0f)
        .transition(transition(.14f))
        .disabled(disabled)
        .onClick(fn)
        .build();
    keyboard(u, id + ".bg", [fn, disabled](const eui::KeyEvent &key) {
        if (disabled || !activate(key))
            return false;
        fn();
        return true;
        });
    if (auto *hit = u.find(id + ".bg"); hit && disabled)
        hit->focusable = false;
}
// 短选项直接呈现，按文字宽度换行；只过渡颜色，不移动控件或输入焦点。
void choiceChips(Ui &u, const std::string &id, const std::vector<std::string> &items, int selected,
                 float w, std::function<void(int)> select, bool disabled = false) {
    if (items.empty())
        return;
    auto &choice = u.state<int>(id + ".choice");
    choice = selected;
    const int count = static_cast<int>(items.size());
    float minimum = 80;
    for (const auto &item : items)
        minimum = std::max(minimum,
                           core::TextPrimitive::measureTextWidth(item, {}, controlFont, 400) + 36);
    const int columns = std::clamp(static_cast<int>((w + 8) / (minimum + 8)), 1, count);
    const float cell = (w - 8 * (columns - 1)) / columns;
    auto choose = [&u, id, &choice, disabled, callback = std::move(select)](int next) {
        if (disabled)
            return;
        if (next != choice) {
            choice = next;
            callback(next);
        }
        u.requestFocus(id + ".item." + std::to_string(next) + ".bg");
    };
    u.column(id)
        .width(w)
        .gap(8)
        .content([&] {
            for (int first = 0; first < count; first += columns)
                u.row(id + ".row." + std::to_string(first))
                    .width(w)
                    .gap(8)
                    .content([&] {
                        for (int i = first; i < std::min(count, first + columns); ++i) {
                            const auto itemId = id + ".item." + std::to_string(i);
                            const bool active = selected == i;
                            const bool focus = !disabled && u.isFocused(itemId + ".bg");
                            components::button(u, itemId)
                                .size(cell, 36)
                                .text(items[i])
                                .fontSize(controlFont)
                                .theme(tokens())
                                .colors(disabled ? surfaceDisabled
                                        : active ? blueWash
                                                 : surface,
                                        active ? blueWashActive : surfaceHover,
                                        active ? blueWashActive : surfacePressed)
                                .textColor(disabled ? inkDisabled
                                           : active ? blue
                                                    : ink)
                                .radius(controlRadius)
                                .border(focus ? 2.f : 1.f, !disabled && (focus || active) ? blue : line)
                                .pressScale(1)
                                .transition(transition(.14f))
                                .disabled(disabled)
                                .onClick([choose, i] { choose(i); })
                                .build();
                            keyboard(
                                u, itemId + ".bg",
                                [choose, &choice, i, count, disabled](const eui::KeyEvent &key) {
                                    if (disabled || !key.isDown())
                                        return false;
                                    int next = choice >= 0 ? choice : i;
                                    if (activate(key))
                                        next = i;
                                    else if (key.key == eui::InputKey::Right ||
                                             key.key == eui::InputKey::Down)
                                        next = (next + 1) % count;
                                    else if (key.key == eui::InputKey::Left ||
                                             key.key == eui::InputKey::Up)
                                        next = (next + count - 1) % count;
                                    else if (key.key == eui::InputKey::Home)
                                        next = 0;
                                    else if (key.key == eui::InputKey::End)
                                        next = count - 1;
                                    else
                                        return false;
                                    choose(next);
                                    return true;
                                });
                            if (auto *hit = u.find(itemId + ".bg"))
                                hit->focusable = !disabled && i == std::max(0, selected);
                        }
                    })
                    .build();
        })
        .build();
}
void checkOption(Ui &u, const std::string &id, const std::string &label, bool value, float w,
                 std::function<void(bool)> change, bool disabled = false) {
    auto style = components::CheckboxStyle(tokens());
    style.radius = 5;
    if (disabled) {
        style.text = eui::Color(inkDisabled);
        style.checked = style.checkedHover = style.checkedPressed = eui::Color("#A7B7D9");
        style.rowHover = style.rowPressed = eui::Color("#00000000");
    }
    auto &checked = u.state<bool>(id + ".checked");
    checked = value;
    auto changeValue = [&checked, disabled, change](bool next) {
        if (!disabled && next != checked) {
            checked = next;
            change(next);
        }
    };
    components::checkbox(u, id)
        .size(w, 36)
        .text(label)
        .checked(value)
        .boxSize(20)
        .fontSize(controlFont)
        .theme(tokens())
        .style(style)
        .transition(transition(.14f))
        .onChange(changeValue)
        .build();
    keyboard(u, id + ".hit", [changeValue, &checked, disabled](const eui::KeyEvent &key) {
        if (disabled || !activate(key))
            return false;
        changeValue(!checked);
        return true;
    });
    if (auto *hit = u.find(id + ".hit")) {
        hit->disabled = disabled;
        hit->focusable = !disabled;
    }
    // EUI 的勾选标记有独立时长；系统关闭动画时一并关闭。
    for (const auto *suffix : {".mark.short", ".mark.long"})
        if (auto *mark = u.find(id + suffix))
            mark->transition.durationSeconds = state.motion ? .12f : 0.f;
}
void subheading(Ui &u, const std::string &id, const std::string &title,
                const std::string &description, float w) {
    u.column(id)
        .width(w)
        .gap(4)
        .content([&] {
            text(u, id + ".title", title, w, 24, 14, ink);
            if (!description.empty())
                u.text(id + ".description")
                    .width(w)
                    .text(description)
                    .fontSize(17)
                    .lineHeight(23)
                    .color(muted)
                    .wrap()
                    .maxWidth(w)
                    .build();
        })
        .build();
}
struct Menu {
    std::string id;
    std::vector<std::string> items;
    std::function<void(int)> select;
    eui::Rect anchor{};
    int highlighted = 0;
    float offset = 0, viewportHeight = 0;
    bool revealHighlighted = true;
} menu;
eui::Rect menuAnchor(const Ui &ui, const core::dsl::Element &target) {
    eui::Rect anchor{target.frame.x, target.frame.y, target.frame.width, target.frame.height};
    // 布局坐标不含滚动变换；仅累加目标实际所属的滚动内容祖先。
    // 固定导航不受正文滚动影响，嵌套滚动则分别应用各自的偏移。
    const auto visit = [&](const auto &self, const core::dsl::Element &node,
                           float offset) -> bool {
        if (!node.scrollContentSourceId.empty())
            if (const auto *owner = ui.find(node.scrollContentSourceId))
                offset += std::round(std::clamp(owner->scrollOffset, 0.f, owner->scrollMaxOffset) *
                                     state.pixelScale) / state.pixelScale;
        if (&node == &target) {
            anchor.y -= offset;
            return true;
        }
        for (const auto &child : node.children)
            if (self(self, *child, offset))
                return true;
        return false;
    };
    for (const auto &root : ui.roots())
        if (visit(visit, *root, 0.f))
            break;
    return anchor;
}
void combo(Ui &u, const std::string &id, const std::vector<std::string> &items, int selected,
           float w, std::function<void(int)> select) {
    selected = items.empty() ? 0 : std::clamp(selected, 0, int(items.size()) - 1);
    select = [selected, count = int(items.size()), callback = std::move(select)](int next) {
        if (next >= 0 && next < count && next != selected)
            callback(next);
    };
    if (menu.id == id) {
        if (items.empty())
            menu.id.clear();
        else {
            if (menu.items != items) {
                menu.items = items;
                menu.highlighted = selected;
                menu.offset = 0;
                menu.revealHighlighted = true;
            }
            menu.select = select;
        }
    }
    bool f = u.isFocused(id);
    u.stack(id + ".wrap")
        .size(w, controlHeight)
        .content([&] {
            u.rect(id)
                .size(w, controlHeight)
                .states(surface, surfaceHover, blueWash)
                .border(f ? 2.0f : 1.0f, f ? blue : line)
                .radius(controlRadius)
                .transition(transition(.14f))
                .focusable()
                .onPress(
                    [id, items, select, selected](const eui::PointerEvent &, const eui::Rect &r) {
                        if (items.empty())
                            return;
                        if (menu.id == id) {
                            menu.id.clear();
                            return;
                        }
                        menu = {id,
                                items,
                                select,
                                {r.x / state.pixelScale, r.y / state.pixelScale,
                                 r.width / state.pixelScale, r.height / state.pixelScale},
                                selected};
                    })
                .onKeyEvent([id, items, select, selected, &u](const eui::KeyEvent &k) {
                    if (!k.isDown() || items.empty())
                        return false;
                    bool open = menu.id == id;
                    if (k.key == eui::InputKey::Escape) {
                        menu.id.clear();
                        return true;
                    }
                    if (k.key == eui::InputKey::Tab) {
                        menu.id.clear();
                        return false;
                    }
                    if (k.key == eui::InputKey::Down || k.key == eui::InputKey::Up ||
                        k.key == eui::InputKey::Home || k.key == eui::InputKey::End) {
                        int i = open ? menu.highlighted : selected;
                        i = k.key == eui::InputKey::Home ? 0
                            : k.key == eui::InputKey::End
                                ? int(items.size()) - 1
                                : (i + (k.key == eui::InputKey::Down ? 1 : -1) +
                                   int(items.size())) %
                                      int(items.size());
                        if (open) {
                            menu.highlighted = i;
                            menu.revealHighlighted = true;
                        } else
                            select(i);
                        return true;
                    }
                    if (activate(k)) {
                        if (open) {
                            auto fn = menu.select;
                            int i = menu.highlighted;
                            menu.id.clear();
                            if (fn)
                                fn(i);
                        } else if (const auto *e = u.find(id))
                            menu = {id, items, select, menuAnchor(u, *e), selected};
                        return true;
                    }
                    return false;
                })
                .build();
            u.row(id + ".caption")
                .size(w, controlHeight)
                .padding(10)
                .alignItems(eui::Align::CENTER)
                .content([&] {
                    u.text(id + ".value")
                        .size(w - 40, 20)
                        .text(items.empty() ? "暂无可选项" : items.at(selected))
                        .fontSize(controlFont)
                        .color(items.empty() ? muted : ink)
                        .verticalAlign(eui::VerticalAlign::Center)
                        .clip()
                        .build();
                    text(u, id + ".arrow", "⌄", 20, 20, 16, muted);
                })
                .build();
        })
        .build();
    if (auto *control = u.find(id); control && items.empty()) {
        control->disabled = true;
        control->focusable = false;
    }
}
void inputControl(Ui &u, const std::string &id, std::string &value, float w,
                  const std::string &hint, std::function<void()> changed) {
    auto style = components::InputStyle(tokens());
    style.background = eui::Color(surfaceSoft);
    style.focused = eui::Color(surface);
    style.border = eui::Color(line);
    style.placeholder = eui::Color(muted);
    style.radius = controlRadius;
    components::input(u, id)
        .size(w, controlHeight)
        .value(value)
        .placeholder(hint)
        .fontSize(controlFont)
        .inset(10)
        .theme(tokens())
        .style(style)
        .transition(transition(.14f))
        .onChange([&value, changed = std::move(changed)](const std::string &s) {
            if (value == s)
                return;
            value = s;
            if (changed)
                changed();
        })
        .build();
}
void input(Ui &u, const std::string &id, std::string &value, float w, const std::string &hint = "",
           bool exportOnly = false) {
    inputControl(u, id, value, w, hint, [exportOnly] {
        if (exportOnly)
            state.outputChanged();
        else
            state.changed();
    });
}
void field(Ui &u, const std::string &id, const std::string &label, std::string &v, float w,
           const std::string &hint = "", bool exportOnly = false, bool disabled = false) {
    u.column(id + ".field")
        .width(w)
        .disabled(disabled)
        .opacity(disabled ? .5f : 1.f)
        .gap(5)
        .content([&] {
            text(u, id + ".label", label, w, 21, 13);
            input(u, id, v, w, hint, exportOnly);
        })
        .build();
}
eui::Shadow cardShadow() {
    eui::Shadow shadow;
    shadow.enabled = true;
    shadow.offset = {0, 2};
    shadow.blur = 10;
    shadow.color = eui::Color("#203B5B0B");
    return shadow;
}
constexpr float secondaryNavHeight = 50.f, secondaryNavInset = 12.f, secondaryNavGap = 14.f;
void navigationCard(Ui &u, const std::string &id, const std::string &rowId, float w,
                    const std::function<void()> &content) {
    components::card(u, id)
        .size(w, secondaryNavHeight)
        .theme(tokens())
        .padding(0)
        .radius(cardRadius)
        .color(surface)
        .border(1, line)
        .shadow(cardShadow())
        .content([&] {
            u.row(rowId)
                .size(w, secondaryNavHeight)
                .padding(secondaryNavInset, 0)
                .gap(secondaryNavGap)
                .alignItems(eui::Align::CENTER)
                .content(content)
                .build();
        })
        .build();
}
constexpr float resultInset = 16, resultGap = 9, sectionTitleHeight = 27;
constexpr float resultSummaryHeight = 72, resultTabHeight = 40;
constexpr float resultTableHeaderHeight = 40, previewPaginationHeight = 32;
constexpr float resultFixedHeight = 2 * resultInset + sectionTitleHeight + resultSummaryHeight +
                                    resultTabHeight + 3 * resultGap;

void section(Ui &u, const std::string &id, const std::string &title, float w,
             std::function<void(float)> fn) {
    const bool compact = id == "rules" || id == "targets" || id == "results";
    const float inset = compact || w < 610 ? resultInset : 20.0f;
    const float verticalInset = id == "summary" || id == "rules" ? 12.f
                                : id == "targets"                ? 14.f
                                                                 : inset;
    const float contentGap = id == "summary" ? 4.f
                             : id == "rules" ? 7.f
                             : compact       ? resultGap
                                             : 13.f;
    components::card(u, id + ".card")
        .width(w)
        .wrapContentHeight()
        .theme(tokens())
        .padding(0)
        .color(surface)
        .radius(cardRadius)
        .border(1, line)
        .shadow(cardShadow())
        .content([&] {
            u.column(id)
                .width(w)
                .padding(inset, verticalInset)
                .gap(contentGap)
                .content([&] {
                    if (id != "summary" && !title.empty())
                        u.row(id + ".heading")
                            .width(w - inset * 2)
                            .gap(9)
                            .alignItems(eui::Align::CENTER)
                            .content([&] {
                                u.rect(id + ".accent").size(3, 15).color(blue).radius(1.5f).build();
                                text(u, id + ".title", title, w - inset * 2 - 12,
                                     sectionTitleHeight, 17);
                            })
                            .build();
                    fn(w - inset * 2);
                })
                .build();
        })
        .build();
}
// Errors stay beside the form until resolved.
void notice(Ui &u, float w) {
    const bool exportFailed = !state.dataExportError.empty();
    if (!exportFailed && state.message.empty())
        return;
    const bool softFailure =
        !exportFailed && (state.status == Status::Infeasible || state.status == Status::SearchFailed);
    const auto level = softFailure ? Level::Warning : Level::Error;
    const bool stacked = w < 680;
    const bool offerOptions = state.page != Page::Options;
    constexpr float inset = 14, gap = 10;
    const float contentWidth = w - 2 * inset;
    const float actionsWidth = offerOptions ? 194.f : 92.f;
    const float messageWidth = stacked ? contentWidth : contentWidth - actionsWidth - gap;
    const auto message = [&] {
        u.column("notice.copy")
            .width(messageWidth)
            .gap(4)
            .content([&] {
                text(u, "notice.title",
                     exportFailed                           ? "测量数据导出失败"
                     : state.status == Status::Infeasible   ? "约束冲突"
                     : state.status == Status::SearchFailed ? "未找到满足约束的数据"
                                                            : "操作失败",
                     messageWidth, 24, 15, levelColor(level));
                u.text("notice.text")
                    .width(messageWidth)
                    .text(exportFailed ? state.dataExportError : state.message)
                    .fontSize(18)
                    .lineHeight(24)
                    .color(ink)
                    .wrap()
                    .maxWidth(messageWidth)
                    .build();
            })
            .build();
    };
    const auto actions = [&] {
        u.row("notice.actions")
            .width(stacked ? contentWidth : actionsWidth)
            .gap(gap)
            .justifyContent(eui::Align::END)
            .content([&] {
                if (offerOptions)
                    button(
                        u, "fixoptions", "打开选项", 92,
                        [] {
                            state.navigate(Page::Options);
                            state.configOffset = 0;
                        },
                        false, state.busy(), 36);
                button(
                    u, "showlogs", "查看日志", 92, [] { state.navigate(Page::Logs); }, false, false,
                    36);
            })
            .build();
    };
    u.stack("notice.card")
        .width(w)
        .content([&] {
            u.rect("notice.bg")
                .width(eui::SizeValue::fill())
                .height(eui::SizeValue::fill())
                .ignoreLayout()
                .color(levelSurface(level))
                .border(1, softFailure ? "#E6D4A9" : "#EBC8C1")
                .radius(cardRadius)
                .build();
            (stacked ? u.column("notice.row") : u.row("notice.row"))
                .width(w)
                .padding(inset, 12)
                .gap(gap)
                .alignItems(eui::Align::CENTER)
                .content([&] {
                    message();
                    actions();
                })
                .build();
        })
        .build();
}
void compactField(Ui &u, const std::string &id, const std::string &label, std::string &value,
                  float w, const std::string &hint = "") {
    u.column(id + ".field")
        .width(w)
        .gap(3)
        .content([&] {
            text(u, id + ".label", label, w, 18, 12, muted);
            input(u, id, value, w, hint);
        })
        .build();
}
void inputs(Ui &u, float w) {
    section(u, "rules", "生成设置", w, [&](float c) {
        const float limitWidth = (c - 12) / 2;
        field(u, "center", "数据中心", state.draft.center, c, "留空时自动确定");
        u.row("limits")
            .width(c)
            .gap(12)
            .content([&] {
                field(u, "lsl", "规格下限 LSL", state.draft.lsl, limitWidth, "输入数值");
                field(u, "usl", "规格上限 USL", state.draft.usl, limitWidth, "输入数值");
            })
            .build();
        text(u, "spechelp",
             state.draft.compliance == 0 ? "规格内生成需要至少填写一侧规格限。"
                                         : "当前允许测量值超出规格限。",
             c, 20, 12, muted);
    });
    section(u, "targets", "指标约束", w, [&](float c) {
        u.column("constraintrows")
            .width(c)
            .gap(2)
            .content([&] {
                for (int i = 0; i < 5; ++i) {
                    auto &t = state.draft.targets[i];
                    const std::string id = "target" + std::to_string(i);
                    const float labelWidth = 84, modeWidth = 116,
                                valueWidth = c - labelWidth - modeWidth - 24;
                    u.row(id + ".row")
                        .size(c, 61)
                        .gap(12)
                        .alignItems(eui::Align::CENTER)
                        .content([&] {
                            text(u, id + ".name", metricNames[i], labelWidth, 40, 14);
                            combo(u, id + ".mode", modes, t.mode, modeWidth, [i](int mode) {
                                if (state.busy() || state.draft.targets[i].mode == mode)
                                    return;
                                state.draft.targets[i].mode = mode;
                                state.changed();
                            });
                            if (t.mode) {
                                const bool two = t.mode == 1 || t.mode == 2;
                                const float f = two ? (valueWidth - 8) / 2 : valueWidth;
                                u.row(id + ".values")
                                    .width(valueWidth)
                                    .gap(8)
                                    .content([&] {
                                        const std::string unit = i == 4 ? "" : " %";
                                        compactField(u, id + ".first",
                                                     (t.mode == 1   ? "目标值"
                                                      : t.mode == 2 ? "下限"
                                                                    : "阈值") +
                                                         unit,
                                                     t.first, f);
                                        if (two)
                                            compactField(
                                                u, id + ".second",
                                                t.mode == 1
                                                    ? (i == 4 ? "误差（整数）" : "误差（百分点）")
                                                    : ("上限" + unit),
                                                t.mode == 1 ? t.tolerance : t.upper, f,
                                                t.mode == 1 ? (i == 4 ? state.draft.ndcError
                                                                      : state.draft.percentError)
                                                            : "");
                                    })
                                    .build();
                            } else
                                text(u, id + ".unconstrained", "—", valueWidth, 40, 13, muted);
                        })
                        .build();
                    if (i < 4)
                        u.rect(id + ".divider").size(c, 1).color(line).build();
                }
            })
            .build();
    });
}
std::string condition(int i) {
    const auto &t = state.draft.targets[i];
    if (!t.mode)
        return "不约束";
    auto display = [&](const std::string &value) {
        try {
            return msa::fixed(msa::parse_number(native_ui::trim(value)), i == 4 ? 0 : 2);
        } catch (const std::exception &) {
            return std::string("—");
        }
    };
    const std::string first = display(t.first);
    if (t.mode == 1)
        return first + " ± " +
               display(native_ui::trim(t.tolerance).empty()
                           ? (i == 4 ? state.draft.ndcError : state.draft.percentError)
                           : t.tolerance);
    if (t.mode == 2)
        return first + " ～ " + display(t.upper);
    return std::string(t.mode == 3 ? "≤ " : "≥ ") + first;
}
std::string boundText(double v, int dp) { return std::isfinite(v) ? msa::fixed(v, dp) : "∞"; }
void review(Ui &u, float w, float tableHeight = 0) {
    const bool hasResult = state.displayedResult();
    Json cfg;
    try {
        cfg = hasResult ? state.result->config
                        : native_ui::to_config(state.draft, native_ui::ConfigUse::Generation);
    } catch (const std::exception &) {
        cfg = nullptr;
    }
    const Json m = hasResult ? state.result->metrics : Json::object();
    const Json status = hasResult ? msa::checks(m, cfg) : Json::object();
    const bool detail = tableHeight > 0 || w >= 600;
    const float rowHeight =
        tableHeight > 0 ? (tableHeight - resultTableHeaderHeight) / metricNames.size() : 40.f;
    constexpr std::array<float, 6> ratios{.15f, .21f, .21f, .14f, .12f, .17f};
    constexpr auto left = eui::HorizontalAlign::Left;
    constexpr auto right = eui::HorizontalAlign::Right;
    constexpr auto center = eui::HorizontalAlign::Center;
    auto verdictChip = [&](const std::string &id, const std::string &verdict, float width) {
        const bool ok = verdict == "满足", neutral = verdict == "未约束";
        badge(u, id, verdict, width,
              ok        ? green
              : neutral ? muted
                        : levelColor(Level::Error),
              ok        ? greenWash
              : neutral ? surfaceDisabled
                        : levelSurface(Level::Error));
    };
    auto row = [&](std::string id, std::array<std::string, 6> cells, bool header) {
        const float height = header ? resultTableHeaderHeight : rowHeight;
        u.stack(id + ".wrap")
            .size(w, height)
            .content([&] {
                u.rect(id + ".bg")
                    .size(w, height)
                    .color(header ? tableHeader : (id.back() % 2 ? tableStripe : surface))
                    .radius(header ? controlRadius : 0.f)
                    .build();
                u.row(id)
                    .size(w, height)
                    .padding(8)
                    .alignItems(eui::Align::CENTER)
                    .content([&] {
                        for (int j = 0; j < 6; ++j) {
                            const float width = (w - 16) * ratios[j];
                            if (j == 5 && !header && cells[j] != "—") {
                                u.row(id + ".verdict.cell")
                                    .size(width, 28)
                                    .justifyContent(eui::Align::CENTER)
                                    .content([&] {
                                        verdictChip(id + ".verdict", cells[j], width - 14);
                                    })
                                    .build();
                            } else {
                                u.text(id + "c" + std::to_string(j))
                                    .size(width - 8, 26)
                                    .margin(0, 0, 8, 0)
                                    .text(cells[j])
                                    .fontSize(header ? tableLabelFont : tableValueFont)
                                    .color(header || j == 5 ? muted
                                           : j == 3         ? blue
                                                            : ink)
                                    .horizontalAlign(j == 5   ? center
                                                     : j >= 3 ? right
                                                              : left)
                                    .verticalAlign(eui::VerticalAlign::Center)
                                    .clip()
                                    .build();
                            }
                        }
                    })
                    .build();
            })
            .build();
    };
    if (detail)
        row("reviewheader", {"指标", "目标条件", "接受范围", "实际值", "偏差", "判定"}, true);
    for (int i = 0; i < 5; ++i) {
        auto key = msa::metric_keys[i];
        auto b = cfg.is_null() ? msa::Bounds{} : msa::acceptance(cfg, key);
        const Json v = hasResult ? m[key] : Json(nullptr);
        const Json t = cfg.is_null() ? Json(nullptr) : cfg["targets"][key];
        const int dp = i == 4 ? 0 : 2;
        std::string range = b ? boundText(b->first, dp) + "～" + boundText(b->second, dp) : "—",
                    delta = "—";
        if (native_ui::finite_metric(v) && !t.is_null() &&
            (t["mode"] == "target" || t["mode"] == "between")) {
            double preferred = t["mode"] == "target"
                                   ? t["value"].get<double>()
                                   : (t["min"].get<double>() + t["max"].get<double>()) / 2;
            delta = native_ui::review_number(v.get<double>() - preferred, dp);
            if (delta == "-0.00")
                delta = "0.00";
        }
        std::string verdict = !hasResult                                     ? "—"
                              : !v.is_null() && !native_ui::finite_metric(v) ? "数值异常"
                              : status[key].is_null()                        ? "未约束"
                              : v.is_null()                                  ? "不可计算"
                              : status[key] == true                          ? "满足"
                                                                             : "不满足";
        auto actual = !hasResult ? std::string("—") : native_ui::review_number(v, dp);
        if (detail)
            row("review" + std::to_string(i),
                {metricNames[i], condition(i), range, actual, delta, verdict}, false);
        else {
            const auto id = "compact" + std::to_string(i);
            u.stack(id)
                .width(w)
                .content([&] {
                    u.rect(id + ".bg")
                        .width(eui::SizeValue::fill())
                        .height(eui::SizeValue::fill())
                        .ignoreLayout()
                        .color(i % 2 == 0 ? surfaceSoft : surface)
                        .radius(controlRadius)
                        .build();
                    u.column(id + ".content")
                        .width(w)
                        .padding(12, 6)
                        .gap(3)
                        .content([&] {
                            u.row(id + ".headline")
                                .size(w - 24, 28)
                                .gap(10)
                                .alignItems(eui::Align::CENTER)
                                .content([&] {
                                    text(u, "metric" + std::to_string(i), metricNames[i], 104, 28, 14);
                                    text(u, id + ".actual", "实际值  " + actual, w - 248, 28,
                                         14, hasResult ? blue : muted);
                                    if (hasResult)
                                        verdictChip(id + ".verdict", verdict, 96);
                                })
                                .build();
                            u.text("condition" + std::to_string(i))
                                .width(w - 24)
                                .text("目标 " + condition(i) + "  ·  范围 " + range + "  ·  偏差 " +
                                      delta)
                                .wrap()
                                .maxWidth(w - 24)
                                .fontSize(16)
                                .color(muted)
                                .build();
                        })
                        .build();
                })
                .build();
        }
    }
}
void emptyDocumentIcon(Ui &u, const std::string &id) {
    u.stack(id)
        .size(40, 40)
        .content([&] {
            u.rect(id + ".bg").size(40, 40).radius(controlRadius).color(blueWash).build();
            u.rect(id + ".page")
                .position(11, 10)
                .size(18, 20)
                .radius(3)
                .color(surface)
                .border(1, blue)
                .build();
            u.rect(id + ".line1").position(15, 16).size(10, 1).color(blue).build();
            u.rect(id + ".line2").position(15, 22).size(7, 1).color(blue).build();
        })
        .build();
}
void resultEmpty(Ui &u, const std::string &id, float w, const std::string &hint) {
    constexpr float height = 85, inset = 14, gap = 12;
    const float copyWidth = w - 2 * inset - 40 - gap;
    u.stack(id)
        .size(w, height)
        .content([&] {
            u.rect(id + ".bg")
                .size(w, height)
                .color(surfaceSoft)
                .border(1, line)
                .radius(controlRadius)
                .build();
            u.row(id + ".content")
                .size(w, height)
                .padding(inset, 0)
                .gap(gap)
                .alignItems(eui::Align::CENTER)
                .content([&] {
                    emptyDocumentIcon(u, id + ".icon");
                    u.column(id + ".copy")
                        .width(copyWidth)
                        .gap(4)
                        .content([&] {
                            text(u, id + ".title",
                                 state.busy() ? "正在生成测量数据" : "暂无测量结果", copyWidth, 24,
                                 15);
                            text(u, id + ".hint", hint, copyWidth, 24, 13, muted);
                        })
                        .build();
                })
                .build();
        })
        .build();
}
struct PreviewColumns {
    int first = 0, count = 0, pages = 0;
    float labelWidth = 0;
};
std::string labelThatFits(const std::string &label, float width, float size, int weight = 400) {
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
PreviewColumns previewColumns(const msa::Result &res, float w, bool fixedPanel) {
    const auto &design = res.config.at("design");
    const int parts = design.at("parts");
    const auto names = msa::operator_names(design);
    float labelWidth = fixedPanel ? 96.f : 106.f;
    for (int i = 0; i < design.at("operators").get<int>(); ++i) {
        const float natural = core::TextPrimitive::measureTextWidth(
            names[i] + " / " + design.at("replicates").dump(), font(), tableLabelFont) + 16;
        labelWidth = std::max(labelWidth, std::min(w * .38f, natural));
    }
    std::size_t maxDigits = 1;
    for (const auto &row : res.matrix)
        for (double v : row)
            maxDigits = std::max(maxDigits,
                                 msa::fixed(v, res.config["generation"]["decimal_places"]).size());
    const float numericWidth =
        core::TextPrimitive::measureTextWidth(std::string(maxDigits, '0'), {}, 17.5f, 400);
    const int cols = std::clamp(
        int((w - labelWidth) / std::max(58.f, numericWidth + 10.f)), 1, parts);
    const int pages = (parts + cols - 1) / cols;
    state.previewPage = std::clamp(state.previewPage, 0, pages - 1);
    const int first = state.previewPage * cols;
    return {first, std::min(cols, parts - first), pages, labelWidth};
}
void preview(Ui &u, float w, const PreviewColumns &columns, float availableHeight) {
    if (!state.displayedResult()) {
        resultEmpty(u, "previewempty", w, "生成数据后，按配置的小数位查看测量值。");
        return;
    }
    const auto &res = *state.result;
    const int reps = res.config["design"]["replicates"];
    const bool fixedPanel = availableHeight > 0;
    const auto names = msa::operator_names(res.config.at("design"));
    const float labelWidth = columns.labelWidth;
    const float cell = (w - labelWidth) / columns.count;
    const int rows = int(res.matrix.size());
    const float minimumRowHeight = fixedPanel ? 26.f : 34.f;
    const float tableHeight =
        std::max(availableHeight, resultTableHeaderHeight + rows * minimumRowHeight);
    const float cellHeight = (tableHeight - resultTableHeaderHeight) / std::max(1, rows);
    const auto row = [&](int r) {
        const auto id = "previewrow" + std::to_string(r);
        const float rowHeight = r < 0 ? resultTableHeaderHeight : cellHeight;
        u.stack(id + ".wrap")
            .size(w, rowHeight)
            .content([&] {
                u.rect(id + ".bg")
                    .size(w, rowHeight)
                    .color(r < 0   ? tableHeader
                           : r % 2 ? tableStripe
                                   : surface)
                    .radius(r < 0 ? controlRadius : 0.f)
                    .build();
                if (r >= 0 && r % reps == 0)
                    u.rect(id + ".rule").size(w, 1).color(line).build();
                u.row(id)
                    .size(w, rowHeight)
                    .content([&] {
                        std::string label = "操作员 / 次";
                        if (r >= 0) {
                            const auto trial = " / " + std::to_string(r % reps + 1);
                            const float suffixWidth =
                                core::TextPrimitive::measureTextWidth(trial, font(), tableLabelFont);
                            label = labelThatFits(names[r / reps], labelWidth - 16 - suffixWidth,
                                                  tableLabelFont) + trial;
                        }
                        u.text("rowlabel" + std::to_string(r))
                            .size(labelWidth - 8, rowHeight)
                            .margin(8, 0, 0, 0)
                            .text(label)
                            .fontSize(tableLabelFont)
                            .color(muted)
                            .verticalAlign(eui::VerticalAlign::Center)
                            .clip()
                            .build();
                        for (int c = 0; c < columns.count; ++c)
                            u.text("cell" + std::to_string(r) + "_" + std::to_string(c))
                                .size(cell - 8, rowHeight)
                                .margin(0, 0, 8, 0)
                                .text(r < 0
                                          ? std::to_string(columns.first + c + 1)
                                          : msa::fixed(res.matrix[r][columns.first + c],
                                                       res.config["generation"]["decimal_places"]))
                                .fontSize(r < 0 ? tableLabelFont : tableValueFont)
                                .color(r < 0 ? muted : ink)
                                .horizontalAlign(eui::HorizontalAlign::Right)
                                .verticalAlign(eui::VerticalAlign::Center)
                                .clip()
                                .build();
                    })
                    .build();
            })
            .build();
    };
    u.column("preview.table")
        .size(w, tableHeight)
        .content([&] {
            for (int r = -1; r < rows; ++r)
                row(r);
        })
        .build();
}
void studySummary(Ui &u, float w) {
    section(u, "summary", "", w, [&](float c) {
        u.row("study.summary")
            .width(c)
            .gap(10)
            .alignItems(eui::Align::CENTER)
            .content([&] {
                u.column("study.caption")
                    .width(c - 242)
                    .gap(2)
                    .content([&] {
                        text(u, "study.label",
                             state.draft.analysisMethod ? "交叉量具 R&R  /  ANOVA"
                                                        : "交叉量具 R&R  /  Xbar-R",
                             c - 242, 20, 12, blue);
                        text(u, "study.design",
                             state.draft.operators + " 人 × " + state.draft.replicates + " 次 × " +
                                 state.draft.parts + " 件",
                             c - 242, 31, 20);
                    })
                    .build();
                button(
                    u, "study.sample", "载入示例", 106, [] { state.sample(); }, false, state.busy(),
                    36);
                button(
                    u, "study.options", "研究选项  →", 116,
                    [] {
                        state.navigate(Page::Options);
                        state.optionsTab = 0;
                        state.configOffset = 0;
                    },
                    false, false, 36);
            })
            .build();
    });
}
void detailTable(Ui &u, const std::string &id, float w, const std::vector<std::string> &columns,
                 const std::vector<std::vector<std::string>> &rows, float availableHeight = 0) {
    auto theme = tokens();
    theme.metrics.typography.label = tableValueFont;
    theme.metrics.spacing.section = 7;
    theme.metrics.control.segmented =
        resultTableHeaderHeight - theme.metrics.spacing.hairline - theme.metrics.spacing.micro;
    auto style = components::DataTableStyle(theme);
    style.header = eui::Color(tableHeader);
    style.row = eui::Color(surface);
    style.rowAlt = eui::Color(tableStripe);
    style.mutedText = eui::Color(muted);
    style.accent = style.mutedText;
    style.divider = eui::Color(line);
    style.radius = controlRadius;
    const float minimumHeight =
        resultTableHeaderHeight + theme.metrics.spacing.hairline + float(rows.size()) * 42;
    components::dataTable(u, id)
        .size(w, std::max(availableHeight, minimumHeight))
        .columns(columns)
        .rows(rows)
        .theme(theme)
        .style(style)
        .transition(transition())
        .build();
    for (size_t column = 0; column < columns.size(); ++column) {
        if (auto *header = u.find(id + ".header." + std::to_string(column))) {
            header->fontSize = tableLabelFont;
            if (column > 0)
                header->horizontalAlign = eui::HorizontalAlign::Right;
        }
        for (size_t row = 0; row < rows.size(); ++row)
            if (auto *cell =
                    u.find(id + ".cell." + std::to_string(row) + "." + std::to_string(column))) {
                cell->wrap = true;
                if (column > 0)
                    cell->horizontalAlign = eui::HorizontalAlign::Right;
            }
    }
}
std::string componentName(const std::string &key) {
    if (key == "EV")
        return "重复性 EV";
    if (key == "AV")
        return "再现性 AV";
    if (key == "GRR")
        return "量具 R&R";
    if (key == "PV")
        return "部件间 PV";
    if (key == "TV")
        return "总变异 TV";
    return key == "Operator" ? "操作员" : "交互作用";
}
void results(Ui &u, float w, bool summary = true, float contentHeight = 0) {
    if (summary)
        studySummary(u, w);
    const bool hasResult = state.displayedResult();
    const std::string title = state.retainedResult() ? "复核结果 · 上次成功结果"
                              : !state.busy()        ? "复核结果"
                              : hasResult            ? "复核结果 · 上次结果（更新中）"
                                                     : "复核结果 · 正在生成";
    section(u, "results", title, w, [&](float c) {
        // Keep the same summary height and table throughout a task. A search
        // updates the local budget rail instead of replacing the entire panel.
        u.stack("result.summary")
            .size(c, resultSummaryHeight)
            .content([&] {
                u.row("result.overview")
                    .width(c)
                    .gap(12)
                    .content([&] {
                        const std::array<int, 3> metrics{2, 3, 4};
                        for (int i : metrics) {
                            const auto id = "result.metric." + std::to_string(i);
                            const float width = (c - 24) / 3;
                            u.stack(id)
                                .size(width, resultSummaryHeight)
                                .content([&] {
                                    u.rect(id + ".bg")
                                        .size(width, resultSummaryHeight)
                                        .color(i == 2 ? blueWash : surfaceSoft)
                                        .border(1, i == 2 ? brandBorder : eui::Color(line))
                                        .radius(controlRadius)
                                        .build();
                                    u.column(id + ".content")
                                        .width(width)
                                        .padding(12, 8)
                                        .gap(2)
                                        .content([&] {
                                            text(u, id + ".label", metricNames[i], width - 24, 20,
                                                 12, muted);
                                            text(u, id + ".value",
                                                 hasResult ? native_ui::review_number(
                                                                 state.result
                                                                     ->metrics[msa::metric_keys[i]],
                                                                 i == 4 ? 0 : 2)
                                                           : "—",
                                                 width - 24, 34, 24,
                                                 !hasResult ? muted
                                                 : i == 2   ? blue
                                                            : ink);
                                        })
                                        .build();
                                })
                                .build();
                        }
                    })
                    .build();
                u.stack("result.budget")
                    .position(0, resultSummaryHeight - 3)
                    .size(c, 3)
                    .opacity(state.busy() ? 1.0f : 0.0f)
                    .content([&] {
                        // This shows the consumed candidate budget, not completion probability.
                        const float budget =
                            std::max(1.0f, float(std::atof(state.draft.attempts.c_str())));
                        components::progress(u, "searchbudget")
                            .size(c, 3)
                            .value(state.ticks.load() / budget)
                            .theme(tokens())
                            .transition(transition())
                            .build();
                    })
                    .build();
            })
            .build();
        tabs(
            u, "resulttabs",
            state.draft.analysisMethod
                ? std::vector<std::string>{"指标复核", "数据预览", "ANOVA 详情"}
                : std::vector<std::string>{"指标复核", "数据预览", "分析详情"},
            state.resultTab, c,
            [](int i) {
                state.resultTab = i;
                state.resultOffset = 0;
                menu.id.clear();
                state.resultEntered = Clock::now();
            },
            resultTabHeight);
        const bool fixedPanel = contentHeight > 0;
        const auto columns = hasResult && state.resultTab == 1
                                 ? previewColumns(*state.result, c, fixedPanel)
                                 : PreviewColumns{};
        const bool paginated = columns.pages > 1;
        const float tableHeight =
            contentHeight - (fixedPanel && paginated ? previewPaginationHeight + resultGap : 0.f);
        const float progress = reveal(state.resultEntered);
        auto content = u.column("result.content").width(c).opacity(.96f + .04f * progress);
        if (progress < 1)
            content.onFrame([](float) {});
        const auto resultTab = [&](Ui &u, float c) {
            if (state.resultTab == 0)
                review(u, c, tableHeight);
            else if (state.resultTab == 1)
                preview(u, c, columns, tableHeight);
            else {
                if (!hasResult) {
                    resultEmpty(u, "anova.pending", c, "生成后查看所选分析方法的统计结果。");
                } else {
                    const auto &metrics = state.result->metrics;
                    if (metrics.contains("study")) {
                        const auto &study = metrics["study"];
                        std::vector<std::vector<std::string>> rows;
                        for (const auto *key : {"EV", "AV", "GRR", "PV", "TV"}) {
                            const auto &row = study["components"][key];
                            rows.push_back({componentName(key),
                                            native_ui::review_number(row["sd"], 6),
                                            native_ui::review_number(row["study_variation_pct"], 2),
                                            row["process_pct"].is_null()
                                                ? "—"
                                                : native_ui::review_number(row["process_pct"], 2)});
                        }
                        const bool hasMoreDetails = metrics.contains("misclassification") ||
                                                    metrics.contains("confidence") ||
                                                    metrics.contains("anova");
                        detailTable(u, "study.table", c,
                                    {"变异来源", "标准差 SD", "研究变异 %", "过程 %"}, rows,
                                    hasMoreDetails ? 0.f : tableHeight);
                    }
                    if (metrics.contains("misclassification")) {
                        const auto &probability = metrics["misclassification"];
                        text(u, "misclassification.title", "误分类概率（正态模型）", c, 30, 14);
                        const std::array<const char *, 4> keys{"bad_accepted", "good_rejected",
                                                               "accept_given_bad",
                                                               "reject_given_good"};
                        const std::array<const char *, 4> labels{"不合格且被接受", "合格且被拒绝",
                                                                 "不合格品被接受（条件）",
                                                                 "合格品被拒绝（条件）"};
                        for (std::size_t i = 0; i < keys.size(); ++i) {
                            const auto &value = probability[keys[i]];
                            text(u, std::string("misclassification.") + keys[i],
                                 std::string(labels[i]) + "：" +
                                     (value.is_null()
                                          ? "不可计算"
                                          : msa::fixed(100 * value.get<double>(), 6) + "%"),
                                 c, 28, 13);
                        }
                    }
                    if (metrics.contains("confidence")) {
                        const auto &ci = metrics["confidence"];
                        text(u, "confidence.title",
                             "标准差置信限（" + native_ui::review_number(ci["level"], 2) + "% · " +
                                 (ci["method"] == "Satterthwaite_variance_components"
                                      ? "Satterthwaite"
                                      : "MLS") +
                                 "）",
                             c, 30, 14);
                        const auto limit = [](const msa::Json &v) {
                            return v.is_null() ? std::string("—") : native_ui::review_number(v, 6);
                        };
                        std::vector<std::vector<std::string>> rows;
                        for (const auto *key : {"EV", "AV", "GRR", "PV", "TV"}) {
                            const auto &bounds = ci["stddev"][key];
                            rows.push_back({componentName(key), limit(bounds["lower"]),
                                            limit(bounds["upper"])});
                        }
                        detailTable(u, "confidence.table", c, {"变异来源", "置信下限", "置信上限"},
                                    rows);
                    }
                    if (metrics.contains("anova")) {
                        const auto &a = state.result->metrics["anova"];
                        text(u, "anova.model",
                             a["interaction_included"].get<bool>()
                                 ? "最终模型保留部件 × 操作员交互项"
                                 : "最终模型将交互项并入重复性误差",
                             c, 30, 14);
                        text(u, "anova.p",
                             "完整模型交互项 P = " +
                                 native_ui::review_number(a["interaction_p"], 6),
                             c, 28, 13, muted);
                        std::vector<std::vector<std::string>> anovaRows;
                        for (const auto &row : a["table"]) {
                            const auto source = row["source"].get<std::string>();
                            std::vector<std::string> cells{
                                source == "Part"            ? "部件"
                                : source == "Operator"      ? "操作员"
                                : source == "Part*Operator" ? "部件 × 操作员"
                                : source == "Repeatability" ? "重复性误差"
                                : source == "Total"         ? "合计"
                                                            : source,
                                row["df"].dump()};
                            for (const auto *key : {"ss", "ms", "f", "p"})
                                cells.push_back(row[key].is_null()
                                                    ? "—"
                                                    : native_ui::review_number(row[key], 6));
                            anovaRows.push_back(std::move(cells));
                        }
                        if (c >= 900)
                            detailTable(
                                u, "anova.table", c,
                                {"变异来源", "自由度 DF", "平方和 SS", "均方 MS", "F 值", "P 值"},
                                anovaRows);
                        else {
                            // 窄面板按来源分组，数值不压缩、不省略有效位。
                            for (size_t i = 0; i < anovaRows.size(); ++i) {
                                const auto id = "anova.row" + std::to_string(i);
                                const auto &cells = anovaRows[i];
                                u.column(id)
                                    .width(c)
                                    .gap(6)
                                    .padding(0, 6)
                                    .content([&] {
                                        text(u, id + ".source", cells[0] + " · DF = " + cells[1], c,
                                             24, 14);
                                        const std::array<const char *, 4> names{
                                            "平方和 SS", "均方 MS", "F 值", "P 值"};
                                        for (int pair = 0; pair < 2; ++pair)
                                            u.row(id + ".pair" + std::to_string(pair))
                                                .width(c)
                                                .gap(12)
                                                .content([&] {
                                                    for (int j = pair * 2; j < pair * 2 + 2; ++j)
                                                        u.text(id + ".value" + std::to_string(j))
                                                            .width((c - 12) / 2)
                                                            .text(std::string(names[j]) + "  " +
                                                                  cells[j + 2])
                                                            .fontSize(16)
                                                            .color(muted)
                                                            .wrap()
                                                            .maxWidth((c - 12) / 2)
                                                            .build();
                                                })
                                                .build();
                                        u.rect(id + ".line").size(c, 1).color(line).build();
                                    })
                                    .build();
                            }
                        }
                        text(u, "anova.components", "方差分量", c, 30, 14);
                        std::vector<std::vector<std::string>> rows;
                        for (const auto *key :
                             {"EV", "Operator", "OperatorPart", "AV", "GRR", "PV", "TV"}) {
                            rows.push_back(
                                {componentName(key),
                                 native_ui::review_number(a["variance_components"][key], 6),
                                 native_ui::review_number(a["stddev"][key], 6),
                                 native_ui::review_number(a["contribution_pct"][key], 2)});
                        }
                        detailTable(u, "anova.components.table", c,
                                    {"变异来源", "方差分量", "标准差 SD", "%贡献"}, rows);
                    }
                    if (!state.result->search_diagnostics.empty()) {
                        text(u, "search.summary.title", "生成搜索记录", c, 28, 14);
                        const auto &diagnostics = state.result->search_diagnostics;
                        if (diagnostics.contains("part_mean_span")) {
                            const auto &settings = state.result->config.at("generation");
                            const auto *method = msa::find_part_sampling(
                                settings.value("part_sampling", std::string("default")));
                            std::ostringstream summary;
                            summary.imbue(std::locale::classic());
                            if (method)
                                summary << "部件取样：" << method->label << "。";
                            summary << "实际部件均值跨度 " << std::setprecision(8)
                                    << diagnostics.at("part_mean_span").get<double>();
                            const auto pct = diagnostics.find("part_mean_span_pct");
                            if (pct != diagnostics.end() && native_ui::finite_metric(*pct))
                                summary << "，占规格宽度的 " << native_ui::review_number(*pct, 2) << "%";
                            paragraph(u, "search.sampling", summary.str() + "。", c, ink, 17.5f);
                        }
                        u.text("search.summary")
                            .width(c)
                            .text(searchSummary(diagnostics))
                            .fontSize(16)
                            .lineHeight(24)
                            .wrap()
                            .maxWidth(c)
                            .color(muted)
                            .build();
                    }
                }
            }
        };
        content
            .content([&] {
                if (!fixedPanel || state.resultTab == 0) {
                    resultTab(u, c);
                    return;
                }
                // Keep all table edges aligned. The scroll rail fits in the card's right inset.
                constexpr float scrollWidth = 6, scrollGap = 4;
                components::scrollView(u, "result.scroll")
                    .size(c + scrollWidth + scrollGap, tableHeight)
                    .offset(state.resultOffset)
                    .theme(tokens())
                    .scrollbarWidth(scrollWidth)
                    .scrollbarGap(scrollGap)
                    .reserveScrollbar()
                    .onChange([](float offset) { state.resultOffset = offset; })
                    .content([&](Ui &v, float width, float) { resultTab(v, width); })
                    .build();
            })
            .build();
        if (paginated) {
            u.row("preview.pagination")
                .size(c, previewPaginationHeight)
                .gap(8)
                .justifyContent(eui::Align::END)
                .content([&] {
                    button(
                        u, "prevcols", "上一组", 88,
                        [] {
                            if (state.previewPage > 0)
                                --state.previewPage;
                        },
                        false, state.previewPage == 0, previewPaginationHeight);
                    button(
                        u, "nextcols", "下一组", 88,
                        [lastPage = columns.pages - 1] {
                            if (state.previewPage < lastPage)
                                ++state.previewPage;
                        },
                        false, state.previewPage == columns.pages - 1, previewPaginationHeight);
                })
                .build();
        }
    });
}
void safe(std::function<void()> fn) {
    try {
        fn();
    } catch (const std::exception &e) {
        state.message = e.what();
        state.feedback("操作失败，请查看日志", state.message, Level::Error);
    }
}
void saveSettings() {
    safe([] {
        auto cfg = native_ui::to_config(state.draft);
        const auto path = msa::config_path();
        msa::save_config(cfg, path);
        state.draft.operatorNames = cfg.at("design").at("operator_names").get<decltype(state.draft.operatorNames)>();
        state.draft.importFilename = cfg.at("input").at("filename");
        const auto &output = cfg.at("output");
        state.draft.filename = output.at("filename");
        state.draft.chartFilename = output.at("charts").at("filename");
        state.draft.reportFilename = output.at("comparison_report").at("filename");
        state.configDirty = false;
        state.message.clear();
        state.feedback("配置已保存", "配置已保存：" + path.u8string());
    });
}
void protectExportSources(const std::filesystem::path &path);
void exportData() {
    if (!state.validResult())
        return;
    try {
        const auto snapshot = *state.result;
        auto settings = native_ui::data_output_config(state.draft);
        const auto now = std::chrono::system_clock::now();
        std::filesystem::path output;
        if (state.draft.silentCsv) {
            if (settings.at("existing_file") == "overwrite")
                protectExportSources(msa::configured_data_path(snapshot, settings, now));
            output = msa::export_configured_data(snapshot, settings, now, {}, msa::config_path());
        } else {
            const auto target = state.chooseExportPath(msa::configured_data_path(snapshot, settings, now),
                state.draft.dataFileType == 1 ? platform::ExportKind::Xlsx : platform::ExportKind::Csv);
            if (!target)
                return;
            protectExportSources(target->path);
            settings["file_type"] = target->format == platform::ExportKind::Xlsx ? "xlsx" : "csv";
            msa::export_data(snapshot, target->path, settings, target->policy);
            output = target->path;
        }
        state.dataExportError.clear();
        state.feedback("测量数据已导出", "测量数据已导出并复验：" + output.u8string());
    } catch (const std::exception &e) {
        state.dataExportError = std::string("导出失败：") + e.what();
        state.feedback("测量数据导出失败", state.dataExportError, Level::Error);
    }
}
void methodSettings(Ui &u, float w) {
    section(u, "analysis", "分析方法", w, [&](float c) {
        choiceChips(u, "analysis.method", {"Xbar-R（均值极差法）", "ANOVA（方差分析法）"},
                    state.draft.analysisMethod, c, [](int i) {
                        if (state.busy() || state.draft.analysisMethod == i)
                            return;
                        state.draft.analysisMethod = i;
                        state.resultTab = 0;
                        state.changed();
                    });
        if (!state.draft.analysisMethod)
            paragraph(
                u, "analysis.xbarr.note",
                "用均值和极差估计重复性、再现性和部件间变异。交互项可用 ANOVA 分析。",
                c, muted, 17.5f);
        if (state.draft.analysisMethod) {
            const float modeWidth = (c - 12) * .56f;
            u.row("analysis.interaction.values")
                .width(c)
                .gap(12)
                .content([&] {
                    u.column("analysis.interaction.field")
                        .width(modeWidth)
                        .gap(5)
                        .content([&] {
                            text(u, "analysis.interaction.label", "部件 × 操作员交互项", modeWidth,
                                 21, 13);
                            combo(u, "analysis.interaction",
                                  {"按 P 值判断", "保留交互项", "并入重复性误差"},
                                  state.draft.interactionMode, modeWidth, [](int i) {
                                      if (state.busy() || !state.draft.analysisMethod ||
                                          state.draft.interactionMode == i)
                                          return;
                                      state.draft.interactionMode = i;
                                      state.changed();
                                  });
                        })
                        .build();
                    const float alphaWidth = c - modeWidth - 12;
                    u.column("analysis.alpha.control")
                        .width(alphaWidth)
                        .disabled(state.draft.interactionMode != 0)
                        .content([&] {
                            field(u, "analysis.alpha", "显著性水平 α", state.draft.interactionAlpha,
                                  alphaWidth, "0 < α < 1");
                        })
                        .build();
                })
                .build();
            paragraph(u, "analysis.note",
                      state.draft.interactionMode == 0
                          ? "P 值小于 α 时保留交互项，并计入再现性（AV）。否则将其并入重复性误差。"
                          : "选择「按 P 值判断」时可设置显著性水平。",
                      c, muted, 17.5f);
        }
    });
}
void designSettings(Ui &u, float w) {
    section(u, "design", "研究设计", w, [&](float c) {
        const float f = (c - 24) / 3;
        u.row("designrow")
            .width(c)
            .gap(12)
            .content([&] {
                u.column("ocol")
                    .width(f)
                    .gap(5)
                    .content([&] {
                        text(u, "olabel", "操作员数", f, 21, 13);
                        segments(u, "operators", {"2 人", "3 人"},
                                 state.draft.operators == "2" ? 0 : 1, f, [](int i) {
                                     if (state.busy() ||
                                         state.draft.operators == std::to_string(i + 2))
                                         return;
                                     state.draft.operators = std::to_string(i + 2);
                                     state.changed();
                                 });
                    })
                    .build();
                u.column("rcol")
                    .width(f)
                    .gap(5)
                    .content([&] {
                        text(u, "rlabel", "重复测量次数", f, 21, 13);
                        segments(u, "replicates", {"2 次", "3 次"},
                                 state.draft.replicates == "2" ? 0 : 1, f, [](int i) {
                                     if (state.busy() ||
                                         state.draft.replicates == std::to_string(i + 2))
                                         return;
                                     state.draft.replicates = std::to_string(i + 2);
                                     state.changed();
                                 });
                    })
                    .build();
                u.column("pcol")
                    .width(f)
                    .gap(5)
                    .content([&] {
                        text(u, "plabel", "部件数", f, 21, 13);
                        stepper(u, "parts", state.draft.parts, f, 5, 10);
                    })
                    .build();
            })
            .build();
        const int records = std::atoi(state.draft.operators.c_str()) *
                            std::atoi(state.draft.replicates.c_str()) *
                            std::atoi(state.draft.parts.c_str());
        u.row("design.summary")
            .width(c)
            .gap(12)
            .alignItems(eui::Align::CENTER)
            .content([&] {
                paragraph(u, "design.note", "每位操作员按相同次数测量所有部件。", c - 148,
                          muted, 17.5f);
                badge(u, "design.records", "共 " + std::to_string(records) + " 次测量", 136);
            })
            .build();
        u.column("operator.names")
            .width(c)
            .gap(6)
            .content([&] {
                text(u, "operator.names.title", "默认操作员", c, 22, 15);
                u.row("operator.names.row")
                    .width(c)
                    .gap(12)
                    .content([&] {
                        for (int i = 0; i < 3; ++i) {
                            const auto number = std::to_string(i + 1);
                            field(u, "operator.name." + number, "操作员 " + number,
                                  state.draft.operatorNames[i], f, std::string(1, char('A' + i)),
                                  false, i == 2 && state.draft.operators == "2");
                        }
                    })
                    .build();
                text(u, "operator.names.hint", "名称留空时，依次使用 A、B、C。", c, 22, 13, muted);
            })
            .build();
    });
}
void generationRules(Ui &u, float w) {
    static const auto samplingNames = [] {
        std::vector<std::string> names;
        for (const auto &method : msa::part_sampling_methods)
            names.emplace_back(method.label);
        return names;
    }();
    section(u, "generation.rules", "生成规则", w, [&](float c) {
        const float f = (c - 16) / 2;
        u.row("generation.rules.values")
            .width(c)
            .gap(16)
            .content([&] {
                u.column("prioritycol")
                    .width(f)
                    .gap(5)
                    .content([&] {
                        text(u, "prioritylabel", "生成优先级", f, 21, 13);
                        segments(u, "priority", {"中心优先", "指标优先"}, state.draft.priority, f,
                                 [](int i) {
                                     if (state.busy() || state.draft.priority == i)
                                         return;
                                     state.draft.priority = i;
                                     state.changed();
                                 });
                    })
                    .build();
                u.column("strictrow")
                    .width(f)
                    .gap(5)
                    .content([&] {
                        text(u, "compliance.label.heading", "生成范围", f, 21, 13);
                        toggle(u, "compliance", "规格内生成", state.draft.compliance == 0, f,
                               [](bool strict) {
                                   const int mode = strict ? 0 : 1;
                                   if (state.busy() || state.draft.compliance == mode)
                                       return;
                                   state.draft.compliance = mode;
                                   state.changed();
                               });
                    })
                    .build();
            })
            .build();
        u.row("generation.rules.hints")
            .width(c)
            .gap(16)
            .content([&] {
                paragraph(u, "priority.note",
                          state.draft.priority == 0 && !native_ui::trim(state.draft.center).empty()
                              ? "优先接近设定的数据中心。"
                          : state.draft.priority == 0 ? "在「数据」页填写数据中心后生效。"
                                                      : "优先接近设定的目标指标。",
                          f, muted, 17.5f);
                paragraph(u, "stricthint",
                          state.draft.compliance == 0 ? "在「数据」页填写至少一侧规格限。"
                                                      : "允许测量值超出规格限。",
                          f, muted, 17.5f);
            })
            .build();
        u.row("generation.rules.normality")
            .width(c)
            .gap(16)
            .alignItems(eui::Align::CENTER)
            .content([&] {
                toggle(u, "normality", "要求近似正态", state.draft.requireNormality, f,
                       [](bool enabled) {
                           if (state.busy() || state.draft.requireNormality == enabled)
                               return;
                           state.draft.requireNormality = enabled;
                           state.changed();
                       });
                paragraph(u, "normality.note", "筛选最终测量值的正态性，规格和指标约束同时生效。",
                          f, muted, 17.5f);
            })
            .build();
        u.column("generation.rules.sampling")
            .width(c)
            .gap(5)
            .content([&] {
                text(u, "part.sampling.label", "部件取样", f, 21, 13);
                const int selected =
                    std::clamp(state.draft.partSampling, 0, int(samplingNames.size()) - 1);
                combo(u, "part.sampling", samplingNames, selected, f, [](int i) {
                    if (state.busy() || state.draft.partSampling == i)
                        return;
                    state.draft.partSampling = i;
                    state.changed();
                });
                const auto &method = msa::part_sampling_methods[selected];
                paragraph(u, "part.sampling.note", method.description, c, muted, 17.5f);
                if (method.mode == msa::PartSampling::UniformRandom &&
                    (native_ui::trim(state.draft.lsl).empty() || native_ui::trim(state.draft.usl).empty()))
                    paragraph(u, "part.sampling.limits", "请在「数据」页填齐上下规格限，再生成随机取样数据。",
                              c, levelColor(Level::Warning), 17.5f);
            })
            .build();
        if (state.draft.analysisMethod == 1) {
            toggle(u, "residual.varied", "教学场景：子组波动不等", state.draft.variedResiduals, c,
                   [](bool value) {
                       if (!state.busy() && state.draft.variedResiduals != value) {
                           state.draft.variedResiduals = value;
                           state.changed();
                       }
                   });
            paragraph(u, "residual.note",
                      "改变操作员与部件组合内的重复测量波动，可在极差控制图中比较各子组。",
                      c, muted, 17.5f);
        }
    });
}
void evaluationSettings(Ui &u, float w) {
    section(u, "analysis.evaluation", "评价口径", w, [&](float c) {
        u.row("analysis.evaluation.values")
            .width(c)
            .gap(12)
            .content([&] {
                field(u, "multiplier", "研究变异倍数 k", state.draft.multiplier, (c - 12) / 2,
                      "默认 6");
                field(u, "analysis.tolerance", "分析容差", state.draft.analysisTolerance,
                      (c - 12) / 2, "可选，用于 P/T");
            })
            .build();
        paragraph(u, "analysis.tolerance.note",
                  "研究变异 = k × 标准差。分析容差用于计算 P/T，留空时按规格限计算，单侧规格使用半研究变异。",
                  c, muted, 17.5f);
    });
}
void processSettings(Ui &u, float w) {
    section(u, "analysis.process", "历史过程参考", w, [&](float c) {
        const float f = (c - 24) * .29f, sourceWidth = c - 24 - 2 * f;
        u.row("analysis.historical.values")
            .width(c)
            .gap(12)
            .content([&] {
                field(u, "analysis.historical.sd", "历史标准差（1σ）", state.draft.historicalSd,
                      f, "选填，须大于 0");
                field(u, "analysis.historical.mean", "历史均值", state.draft.historicalMean, f,
                      "默认测量均值");
                u.column("analysis.process.field")
                    .width(sourceWidth)
                    .gap(5)
                    .content([&] {
                        text(u, "analysis.process.label", "过程变异估计来源", sourceWidth, 21, 13);
                        combo(u, "analysis.process.source", {"优先使用历史标准差", "按本次研究估计"},
                              state.draft.processVariation, sourceWidth, [](int i) {
                                  if (state.busy() || state.draft.processVariation == i)
                                      return;
                                  state.draft.processVariation = i;
                                  state.changed();
                              });
                    })
                    .build();
            })
            .build();
        paragraph(u, "analysis.process.note",
                  "历史标准差留空或小于量具 R&R 的标准差时，采用本次研究的估计。历史均值用于误分类概率和单侧规格评价。",
                  c, muted, 17.5f);
    });
}
void generationSettings(Ui &u, float w) {
    section(u, "generation", "生成参数", w, [&](float c) {
        const float f = (c - 12) / 2;
        u.row("precisionrow")
            .width(c)
            .gap(12)
            .content([&] {
                u.column("decimals.field")
                    .width(f)
                    .gap(5)
                    .content([&] {
                        text(u, "decimals.label", "小数位数", f, 21, 13);
                        stepper(u, "decimals", state.draft.decimals, f, 0, 10);
                    })
                    .build();
                u.column("scalecol")
                    .width(f)
                    .gap(5)
                    .content([&] {
                        text(u, "scalelabel", "生成尺度", f, 21, 13);
                        combo(u, "scale", {"自动确定", "参考总变异"}, state.draft.scale, f,
                              [](int i) {
                                  if (state.busy() || state.draft.scale == i)
                                      return;
                                  state.draft.scale = i;
                                  state.changed();
                              });
                    })
                    .build();
            })
            .build();
        const bool hasBothLimits = !native_ui::trim(state.draft.lsl).empty() &&
                                   !native_ui::trim(state.draft.usl).empty();
        u.row("generation.part.span")
            .width(c)
            .gap(12)
            .alignItems(eui::Align::CENTER)
            .content([&] {
                paragraph(u, "part.span.note",
                          hasBothLimits ? "部件均值的最大差，占规格宽度的百分比。"
                                        : "设置跨度前，请在「数据」页填写上下规格限。",
                          f, muted, 17.5f);
                field(u, "part.span", "最低部件跨度（%）", state.draft.minimumPartSpanPct, f,
                      "留空按生成尺度");
            })
            .build();
        const bool showReference = state.draft.scale == 1 || !hasBothLimits;
        u.row("generation.references")
            .width(c)
            .gap(12)
            .content([&] {
                if (showReference)
                    field(u, "reference", "参考总变异标准差（TV_SD）", state.draft.reference, f,
                          "留空时使用历史标准差");
                field(u, "seed", "数据生成种子", state.draft.seed, showReference ? f : c,
                      "留空则自动选取");
            })
            .build();
        paragraph(u, "generation.note",
                  "小数位数用于生成测量值和导出数据。按相同参数生成时，固定种子可复现数据。",
                  c, muted, 17.5f);
    });
}
void statisticsSettings(Ui &u, float w) {
    section(u, "analysis.statistics", "统计推断", w, [&](float c) {
        const bool anova = state.draft.analysisMethod == 1;
        const bool hasSpec =
            !native_ui::trim(state.draft.lsl).empty() || !native_ui::trim(state.draft.usl).empty();
        const float f = (c - 12) / 2;
        u.row("analysis.statistics.switches")
            .width(c)
            .gap(12)
            .content([&] {
                u.column("analysis.misclassification.control")
                    .width(f)
                    .disabled(!anova || !hasSpec)
                    .content([&] {
                        toggle(
                            u, "analysis.misclassification", "计算误分类概率",
                            state.draft.misclassification, f,
                            [](bool enabled) {
                                if (state.busy() || state.draft.analysisMethod != 1 ||
                                    (native_ui::trim(state.draft.lsl).empty() &&
                                     native_ui::trim(state.draft.usl).empty()) ||
                                    state.draft.misclassification == enabled)
                                    return;
                                state.draft.misclassification = enabled;
                                state.changed();
                            },
                            !anova || !hasSpec);
                    })
                    .build();
                u.column("analysis.confidence.control")
                    .width(f)
                    .disabled(!anova)
                    .content([&] {
                        toggle(
                            u, "analysis.confidence", "计算置信区间", state.draft.confidenceEnabled,
                            f, [](bool enabled) {
                                if (state.busy() || state.draft.analysisMethod != 1 ||
                                    state.draft.confidenceEnabled == enabled)
                                    return;
                                state.draft.confidenceEnabled = enabled;
                                state.changed();
                            },
                            !anova);
                    })
                    .build();
            })
            .build();
        if (!anova)
            paragraph(u, "statistics.note", "切换到 ANOVA 后可设置误分类概率和置信区间。", c,
                      muted, 17.5f);
        else
            u.row("analysis.statistics.hints")
                .width(c)
                .gap(12)
                .content([&] {
                    paragraph(u, "statistics.note",
                              hasSpec ? "估计测量判定与实际合格状态不一致的概率。"
                                      : "在「数据」页填写规格限后可用。",
                              f, muted, 17.5f);
                    if (!state.draft.confidenceEnabled)
                        paragraph(u, "analysis.confidence.note", "开启后可设置信水平和估计方法。",
                                  f, muted, 17.5f);
                })
                .build();
        if (state.draft.confidenceEnabled) {
            u.column("analysis.confidence.options")
                .width(c)
                .gap(13)
                .disabled(!anova)
                .content([&] {
                    constexpr float gap = 12;
                    const bool inlineBounds = c >= 500;
                    const float boundWidth = inlineBounds ? std::max(204.f, c * .39f) : c;
                    const float levelWidth = inlineBounds ? 88.f : (c - gap) * .4f;
                    const float methodWidth =
                        c - levelWidth - gap - (inlineBounds ? boundWidth + gap : 0.f);
                    const auto boundField = [&] {
                        u.column("analysis.confidence.bound.field")
                            .width(boundWidth)
                            .gap(5)
                            .content([&] {
                                text(u, "analysis.confidence.bound.label", "置信限类型", boundWidth,
                                     21, 13);
                                segments(u, "analysis.confidence.bound",
                                         {"双侧区间", "单侧下限", "单侧上限"},
                                         state.draft.confidenceBound, boundWidth, [](int i) {
                                             if (state.busy() || state.draft.analysisMethod != 1 ||
                                                 !state.draft.confidenceEnabled ||
                                                 state.draft.confidenceBound == i)
                                                 return;
                                             state.draft.confidenceBound = i;
                                             state.changed();
                                         });
                            })
                            .build();
                    };
                    u.row("analysis.confidence.values")
                        .width(c)
                        .gap(gap)
                        .content([&] {
                            u.column("analysis.confidence.method.field")
                                .width(methodWidth)
                                .gap(5)
                                .content([&] {
                                    text(u, "analysis.confidence.method.label", "置信区间估计方法",
                                         methodWidth, 21, 13);
                                    combo(u, "analysis.confidence.method",
                                          {"MLS（Minitab）", "Satterthwaite（JMP）"},
                                          state.draft.confidenceMethod, methodWidth, [](int i) {
                                              if (state.busy() || state.draft.analysisMethod != 1 ||
                                                  !state.draft.confidenceEnabled ||
                                                  state.draft.confidenceMethod == i)
                                                  return;
                                              state.draft.confidenceMethod = i;
                                              state.changed();
                                          });
                                })
                                .build();
                            field(u, "analysis.confidence.level", "置信水平（%）",
                                  state.draft.confidenceLevel, levelWidth, "0.01～99.99");
                            if (inlineBounds)
                                boundField();
                        })
                        .build();
                    if (!inlineBounds)
                        boundField();
                })
                .build();
        }
    });
}
void runOrderChoice(Ui &u, const std::string &id, const std::string &label, bool value, bool radio,
                    bool disabled, float w, std::function<void(bool)> change) {
    u.column(id + ".control")
        .width(w)
        .disabled(disabled)
        .content([&] {
            if (!radio) {
                checkOption(u, id, label, value, w, change, disabled);
                return;
            }
            auto style = components::RadioStyle(tokens());
            if (disabled) {
                style.text = eui::Color(muted);
                style.selected = eui::Color("#91A7C9");
                style.outerHover = style.outer;
                style.rowHover = style.rowPressed = eui::Color("#00000000");
            }
            const auto select = [disabled, change](bool next) {
                if (!disabled)
                    change(next);
            };
            components::radio(u, id)
                .size(w, 36)
                .selected(value)
                .text(label)
                .fontSize(18)
                .dotSize(18)
                .theme(tokens())
                .style(style)
                .transition(transition(.14f))
                .onChange(select)
                .build();
            keyboard(u, id + ".hit", [select, disabled](const eui::KeyEvent &key) {
                if (disabled || !activate(key))
                    return false;
                select(true);
                return true;
            });
            if (auto *hit = u.find(id + ".hit"))
                hit->focusable = !disabled;
            if (!state.motion)
                if (auto *mark = u.find(id + ".inner"))
                    mark->transition.durationSeconds = 0;
        })
        .build();
}
void searchSettings(Ui &u, float w, float elapsed = reveal(state.advancedChanged)) {
    section(u, "advanced", "约束与搜索", w, [&](float c) {
        const float f = (c - 12) / 2;
        u.row("target.defaults")
            .width(c)
            .gap(12)
            .content([&] {
                field(u, "percenterror", "百分比误差（百分点）", state.draft.percentError, f);
                field(u, "ndcerror", "ndc 允许误差", state.draft.ndcError, f);
            })
            .build();
        paragraph(u, "target.defaults.note",
                  "目标值未单独填写误差时，使用这里的默认值。ndc 的允许误差须为整数。",
                  c, muted, 17.5f);
        const float expanded =
            state.advancedFrom + ((state.advanced ? 1.0f : 0.0f) - state.advancedFrom) * elapsed;
        u.row("advanced.header")
            .width(c)
            .gap(12)
            .alignItems(eui::Align::CENTER)
            .content([&] {
                button(
                    u, "advancedtoggle", state.advanced ? "收起搜索预算  ▴" : "展开搜索预算  ▾", 156,
                    [expanded] {
                        if (state.busy())
                            return;
                        state.advancedFrom = expanded;
                        state.advanced = !state.advanced;
                        state.advancedChanged = Clock::now();
                    },
                    false, state.busy(), 36);
                paragraph(u, "advanced.note", "可设置候选数据集上限和搜索时限。", c - 168, muted,
                          17.5f);
            })
            .build();
        if (expanded > 0 || state.advanced) {
            constexpr float fieldRowHeight = 66;
            auto panel = u.column("advanced.reveal")
                             .size(c, fieldRowHeight * expanded)
                             .clip()
                             .disabled(!state.advanced || expanded < .999f);
            if (elapsed < 1)
                panel.onFrame([](float) {});
            panel
                .content([&] {
                    u.row("advancedrow")
                        .width(c)
                        .gap(12)
                        .content([&] {
                            field(u, "attempts", "候选数据集上限", state.draft.attempts, f);
                            field(u, "timeout", "搜索时限（秒）", state.draft.timeout, f);
                        })
                        .build();
                })
                .build();
        }
    });
}
void exportSettings(Ui &u, float w) {
    section(u, "output", "数据布局", w, [&](float c) {
        const bool wideFormat = state.draft.csvFormat == 0;
        u.column("csvformat.field")
            .width(c)
            .gap(5)
            .content([&] {
                text(u, "csvformat.label", "导出布局", c, 21, 13);
                combo(u, "csvformat", {"默认（宽表）", "Minitab", "SPSS", "JMP 19", "Origin", "MATLAB"},
                      state.draft.csvFormat, c, [](int i) {
                          if (state.busy() || state.draft.csvFormat == i)
                              return;
                          state.draft.csvFormat = i;
                          state.outputChanged();
                      });
            })
            .build();
        paragraph(u, "outputformat",
                  wideFormat
                      ? "宽表按部件分列。每行包含一位操作员在同一轮测得的全部数值。"
                      : "长表每行记录一次测量，按实际运行顺序排列。五种软件共用此布局。",
                  c, muted, 17.5f);
        const std::string columns = std::string(state.draft.storeStandardOrder ? "StdOrder  ·  " : "") +
                                    "Operator  ·  Part  ·  Trial  ·  Measurement";
        if (!wideFormat)
            paragraph(u, "output.columnNames", columns, c, ink, 17.5f);
        u.rect("output.divider").size(c, 1).color(line).build();
        runOrderChoice(u, "runorder.standard", "保留标准运行序号（StdOrder）",
                       state.draft.storeStandardOrder, false, wideFormat, c, [](bool checked) {
                           if (state.busy() || state.draft.csvFormat == 0 ||
                               state.draft.storeStandardOrder == checked)
                               return;
                           state.draft.storeStandardOrder = checked;
                           state.outputChanged();
                       });
        paragraph(u, "runorder.standard.note",
                  wideFormat ? "选择任一长表格式后可添加 StdOrder。"
                             : "StdOrder 记录随机化前的序号，Trial 记录实际重复测量轮次。",
                  c, muted, 17.5f);
    });
}
void fileSettings(Ui &u, float w) {
    section(u, "output.files", "目录与文件名", w, [&](float c) {
        paragraph(u, "files.desktop", "目录留空时使用 Windows 系统桌面，相对路径从桌面开始。",
                  c, muted, 17.5f);
        const auto directory = [&](const std::string &id, const std::string &label,
                                   std::string Draft::*member, float width) {
            u.column(id + ".field")
                .width(width)
                .gap(5)
                .content([&] {
                    text(u, id + ".label", label, width, 21, 13);
                    u.row(id + ".row")
                        .width(width)
                        .gap(10)
                        .content([&] {
                            input(u, id, state.draft.*member, width - 106, "留空使用系统桌面", true);
                            button(u, id + ".browse", "选择目录", 96, [member] {
                                safe([member] {
                                    const auto initial = msa::resolve_output_directory(
                                        native_ui::trim(state.draft.*member));
                                    if (const auto p = state.chooseDirectory(initial)) {
                                        if (state.draft.*member == p->u8string())
                                            return;
                                        state.draft.*member = p->u8string();
                                        state.outputChanged();
                                    }
                                });
                            }, false, state.busy());
                        })
                        .build();
                })
                .build();
        };
        const auto output = [&](const std::string &directoryId, const char *directoryLabel,
                                std::string Draft::*directoryMember, const char *nameId,
                                const char *nameLabel, std::string &name, const char *hint,
                                std::function<std::string(const std::string &)> normalizeName) {
            if (c >= 760) {
                u.row(directoryId + ".filename.row").width(c).gap(20).content([&] {
                    directory(directoryId, directoryLabel, directoryMember, (c - 20) * .6f);
                    field(u, nameId, nameLabel, name, (c - 20) * .4f, hint, true);
                }).build();
            } else {
                directory(directoryId, directoryLabel, directoryMember, c);
                field(u, nameId, nameLabel, name, c, hint, true);
            }
            u.find(std::string(nameId) + ".hit")->onFocusChanged =
                [&name, normalizeName = std::move(normalizeName)](bool focused) {
                    if (!focused) {
                        try {
                            name = normalizeName(name);
                        } catch (const msa::ConfigError &) {
                            // Keep an unfinished template editable. Save and export report validation errors.
                        }
                    }
                };
        };
        output("import.directory", "测量数据导入目录", &Draft::importDirectory, "import.filename",
               "测量数据导入名称", state.draft.importFilename, "选填，例：来料复核",
               msa::normalize_import_filename);
        output("directory", "测量数据导出目录", &Draft::directory, "filename",
               "测量数据导出名称", state.draft.filename, "msa_data", [](const std::string &name) {
                   return msa::normalize_filename_template(name, state.draft.dataFileType ? "xlsx" : "csv");
               });
        const auto collision = [&](float width) {
            u.column("output.collision.field")
                .width(width)
                .gap(5)
                .content([&] {
                    text(u, "collision.label", "测量数据同名文件处理", width, 21, 13);
                    segments(u, "collision", {"追加序号", "覆盖文件"}, state.draft.existingFile,
                             width, [](int i) {
                                 if (state.busy() || state.draft.existingFile == i)
                                     return;
                                 state.draft.existingFile = i;
                                 state.outputChanged();
                             });
                })
                .build();
        };
        const auto note = [&](float width) {
            paragraph(u, "silentoutput",
                      !state.draft.silentCsv
                          ? "另存为会预填目录和文件名。所选文件已存在时由保存窗口确认覆盖。"
                      : state.draft.existingFile == 0
                          ? "静默保存测量数据时，同名文件追加序号。"
                          : "静默保存测量数据时，同名文件将被替换。",
                      width, muted, 17.5f);
        };
        if (c >= 760) {
            u.row("output.files.policy")
                .width(c)
                .gap(20)
                .alignItems(eui::Align::CENTER)
                .content([&] {
                    collision(340);
                    note(c - 360);
                })
                .build();
        } else {
            collision(c);
            note(c);
        }
        output("chart.directory", "图表导出目录", &Draft::chartDirectory, "chart.filename",
               "图表导出名称", state.draft.chartFilename, "MSA_Charts", [](const std::string &name) {
                   return msa::normalize_export_basename(name, msa::ExportBasenameKind::Chart);
               });
        output("report.directory", "方法对比报告导出目录", &Draft::reportDirectory,
               "report.filename", "方法对比报告导出名称", state.draft.reportFilename, "MSA_Comparison",
               [](const std::string &name) {
                   return msa::normalize_export_basename(name, msa::ExportBasenameKind::ComparisonReport);
               });
        paragraph(u, "files.extensions",
                  "四个名称均支持「帮助 → 名称变量」中的变量，只需填写基本名称。导出时自动添加扩展名。",
                  c, muted, 17.5f);
    });
}
void exportPreferences(Ui &u, float w) {
    section(u, "export.preferences", "导出方式", w, [&](float c) {
        paragraph(u, "export.preferences.note",
                  "开启静默导出后直接保存到设定目录。关闭后，每次导出都会打开「另存为」。",
                  c, muted, 17.5f);
        const auto switches = [&](float width) {
            auto option = [&](const char *id, const char *label, bool Draft::*member) {
                toggle(u, id, label, state.draft.*member, width, [member](bool enabled) {
                    if (state.busy() || state.draft.*member == enabled)
                        return;
                    state.draft.*member = enabled;
                    state.outputChanged();
                });
            };
            option("silent.csv", "静默导出测量数据", &Draft::silentCsv);
            option("silent.charts", "静默导出图表", &Draft::silentCharts);
            option("silent.report", "静默导出方法对比报告", &Draft::silentReport);
        };
        if (c >= 900)
            u.row("export.preferences.switches").width(c).gap(12)
                .content([&] { switches((c - 24) / 3); }).build();
        else
            switches(c);
        constexpr float gap = 12;
        const float fieldWidth = std::min(300.f, (c - gap) / 2);
        const auto formatField = [&](const std::string &id, const char *label,
                                     const std::vector<std::string> &formats, int Draft::*setting) {
            u.column(id + ".field")
                .width(fieldWidth)
                .gap(5)
                .content([&] {
                    text(u, id + ".label", label, fieldWidth, 21, 13);
                    segments(u, id, formats, state.draft.*setting, fieldWidth, [setting](int i) {
                        if (state.busy() || state.draft.*setting == i)
                            return;
                        state.draft.*setting = i;
                        state.outputChanged();
                    });
                })
                .build();
        };
        u.row("export.preferences.formats")
            .width(c)
            .gap(gap)
            .content([&] {
                formatField("data.filetype", "测量数据默认文件类型", {"CSV", "XLSX"}, &Draft::dataFileType);
                formatField("report.format", "方法对比报告默认格式", {"PDF", "HTML"}, &Draft::reportFormat);
            })
            .build();
    });
}
void runOrderSettings(Ui &u, float w) {
    section(u, "runorder", "运行顺序", w, [&](float c) {
        const bool disabled = state.draft.csvFormat == 0;
        const std::array<std::string, 3> names{"标准顺序", "全部随机化", "操作员内随机化"};
        const std::array<float, 3> widths{c * .27f, c * .31f, c - 16 - c * .58f};
        u.row("runorder.modes")
            .width(c)
            .gap(8)
            .content([&] {
                for (int i = 0; i < 3; ++i)
                    runOrderChoice(u, "runorder.mode." + std::to_string(i), names[i],
                                   state.draft.runMode == i, true, disabled, widths[i],
                                   [i](bool selected) {
                                       if (!selected || state.busy() || state.draft.csvFormat == 0 ||
                                           state.draft.runMode == i)
                                           return;
                                       state.draft.runMode = i;
                                       state.outputChanged();
                                   });
            })
            .build();
        paragraph(u, "runorder.explanation",
                  disabled ? "请选择任一长表格式，以设置逐条测量的运行顺序。"
                  : state.draft.runMode == 0 ? "依次按重复测量轮次、部件、操作员排列。"
                  : state.draft.runMode == 1 ? "打乱所有测量记录，可跨轮次和操作员排列。"
                                              : "每轮按操作员分组，各组独立打乱部件顺序。",
                  c, muted, 17.5f);
        u.rect("runorder.divider").size(c, 1).color(line).build();
        u.row("runorder.values")
            .width(c)
            .gap(12)
            .alignItems(eui::Align::END)
            .content([&] {
                constexpr float operatorsWidth = 204;
                runOrderChoice(u, "runorder.operators", "随机化操作员顺序", state.draft.randomOperators,
                               false, disabled || state.draft.runMode != 2, operatorsWidth,
                               [](bool checked) {
                                   if (state.busy() || state.draft.csvFormat == 0 ||
                                       state.draft.runMode != 2 || state.draft.randomOperators == checked)
                                       return;
                                   state.draft.randomOperators = checked;
                                   state.outputChanged();
                               });
                const float seedWidth = c - operatorsWidth - 12;
                const bool seedDisabled = disabled || state.draft.runMode == 0;
                u.column("runorder.seed.control")
                    .width(seedWidth)
                    .disabled(seedDisabled)
                    .content([&] {
                        field(u, "runorder.seed", "运行顺序种子", state.draft.runSeed, seedWidth,
                              seedDisabled ? "随机化时可用" : "选填正整数，留空自动选取", true);
                    })
                    .build();
            })
            .build();
        if (!disabled)
            paragraph(u, "runorder.availability",
                      state.draft.runMode == 0   ? "选择随机化后可填写种子，复现相同的运行顺序。"
                      : state.draft.runMode == 1 ? "选择「操作员内随机化」后可单独打乱操作员顺序。"
                                                 : "勾选后随机确定一次操作员顺序，各轮次沿用。",
                      c, muted, 17.5f);
    });
}
void pathSettings(Ui &u, float w) {
    section(u, "configpath", "配置管理", w, [&](float c) {
        const auto configPath = platform::compatibilityConfig();
        std::error_code directoryError;
        const bool directoryExists = std::filesystem::is_directory(configPath.parent_path(),
                                                                    directoryError);
        const auto location = [&](float width) {
            u.column("configpath.location")
                .width(width)
                .gap(6)
                .content([&] {
                    paragraph(u, "configpathhint", "下次启动时读取此配置文件。", width,
                              muted, 17.5f);
                    u.stack("configpath.file")
                        .width(width)
                        .content([&] {
                            u.rect("configpath.file.bg")
                                .width(eui::SizeValue::fill())
                                .height(eui::SizeValue::fill())
                                .ignoreLayout()
                                .color(surfaceSoft)
                                .border(1, line)
                                .radius(controlRadius)
                                .build();
                            u.column("configpath.file.body")
                                .width(width)
                                .padding(10, 8)
                                .content([&] {
                                    paragraph(u, "path", configPath.u8string(), width - 20, ink,
                                              17.5f);
                                })
                                .build();
                        })
                        .build();
                })
                .build();
        };
        const auto actions = [&](float width) {
            u.row("pathbuttons")
                .width(width)
                .gap(10)
                .content([&] {
                    const float bw = std::min(140.f, (width - 10) / 2);
                    button(u, "copypath", "复制路径", bw, [configPath] {
                        safe([&] {
                            platform::clipboard(configPath.wstring());
                            state.feedback("配置文件路径已复制");
                        });
                    });
                    button(
                        u, "opendir", "打开目录", bw,
                        [configPath] {
                            safe([&] { platform::openDirectory(configPath.parent_path()); });
                        },
                        false, !directoryExists);
                })
                .build();
        };
        if (c >= 760) {
            u.row("configpath.row")
                .width(c)
                .gap(24)
                .alignItems(eui::Align::CENTER)
                .content([&] {
                    location(c - 314);
                    actions(290);
                })
                .build();
        } else {
            location(c);
            actions(c);
        }
        if (!directoryExists)
            paragraph(u, "configpath.unavailable", "首次保存配置时会创建目录。", c,
                      muted, 17.5f);
    });
}
constexpr float optionsGridMinWidth = 1100;

void pairedCards(Ui &u, const std::string &id, float w, const char *leftId,
                 const std::function<void(Ui &, float)> &left, const char *rightId,
                 const std::function<void(Ui &, float)> &right,
                 float splitWidth = optionsGridMinWidth) {
    if (w < splitWidth) {
        left(u, w);
        right(u, w);
        return;
    }
    constexpr float gap = 20;
    const float column = (w - gap) / 2;
    Ui measure;
    measure.begin(id + ".measure");
    left(measure, column);
    right(measure, column);
    measure.end();
    measure.layout(column, 0);
    const float leftHeight = measure.find(leftId)->frame.height;
    const float rightHeight = measure.find(rightId)->frame.height;
    const float rowHeight = std::max(leftHeight, rightHeight);
    const auto alignCard = [&](const char *bodyId, float naturalHeight) {
        auto &body = *u.find(bodyId);
        body.minLayoutHeight = rowHeight;
        // Keep the title and first group aligned across the row.
        if (body.children.size() <= 2)
            return;
        const float gaps = static_cast<float>(body.children.size() - 2);
        const float extra = std::min(16.f, (rowHeight - naturalHeight) / gaps);
        for (size_t i = 2; i < body.children.size(); ++i)
            body.children[i]->margin.top += extra;
    };
    u.row(id)
        .width(w)
        .gap(gap)
        .content([&] {
            left(u, column);
            right(u, column);
            alignCard(leftId, leftHeight);
            alignCard(rightId, rightHeight);
        })
        .build();
}
void chartStyleSettings(Ui &u, float w);
void chartExportSettings(Ui &u, float w);
void openChartOptions() {
    state.optionsTab = chartOptionsTab;
    state.configOffset = 0;
    state.navigate(Page::Options);
}
void config(Ui &u, float w, float disclosureProgress = reveal(state.advancedChanged)) {
    if (state.optionsTab == 0) {
        pairedCards(u, "options.study.definition", w, "design", designSettings, "generation.rules",
                    generationRules);
        pairedCards(
            u, "options.study.generation", w, "generation", generationSettings, "advanced",
            [disclosureProgress](Ui &v, float c) { searchSettings(v, c, disclosureProgress); });
    } else if (state.optionsTab == 1) {
        if (w >= optionsGridMinWidth) {
            pairedCards(u, "options.analysis.method", w, "analysis", methodSettings,
                        "analysis.process", processSettings);
            pairedCards(u, "options.analysis.evaluation", w, "analysis.evaluation",
                        evaluationSettings, "analysis.statistics", statisticsSettings);
        } else {
            methodSettings(u, w);
            evaluationSettings(u, w);
            processSettings(u, w);
            statisticsSettings(u, w);
        }
    } else if (state.optionsTab == chartOptionsTab) {
        chartStyleSettings(u, w);
        chartExportSettings(u, w);
    } else if (state.optionsTab == fileOptionsTab) {
        fileSettings(u, w);
        exportPreferences(u, w);
        pairedCards(u, "options.output", w, "output", exportSettings, "runorder", runOrderSettings);
    }
    pathSettings(u, w);
}
void optionsPage(Ui &u, float w, float h) {
    const float navigationWidth = w - tokens().metrics.control.scrollbar - scrollGutter;
    constexpr float titleWidth = 66;
    const float disclosureProgress = reveal(state.advancedChanged);
    u.column("options.page")
        .size(w, h)
        .gap(sectionGap)
        .content([&] {
            navigationCard(u, "options.navigation", "options.intro", navigationWidth, [&] {
                text(u, "options.title", "选项", titleWidth, 38, 16);
                const float tabsWidth =
                    navigationWidth - titleWidth - secondaryNavGap - 2 * secondaryNavInset;
                tabs(
                    u, "options.tabs", {"研究", "分析", "图表", "文件"}, state.optionsTab,
                    std::min(tabsWidth, 480.f),
                    [](int i) {
                        state.optionsTab = i;
                        state.configOffset = 0;
                        menu.id.clear();
                    },
                    38);
            });
            components::scrollView(u, "configscroll")
                .size(w, h - secondaryNavHeight - sectionGap)
                .offset(state.configOffset)
                .theme(tokens())
                .scrollbarGap(scrollGutter)
                .reserveScrollbar()
                .transition(transition(.12f))
                .onChange([](float offset) {
                    state.configOffset = offset;
                    menu.id.clear();
                })
                .content([disclosureProgress](Ui &v, float cw, float) {
                    v.column("configcontent")
                        .width(cw)
                        .gap(16)
                        .padding(0, 0, 0, 12)
                        .disabled(state.busy())
                        .content([&] {
                            notice(v, cw);
                            config(v, cw, disclosureProgress);
                        })
                        .build();
                })
                .build();
        })
        .build();
}
void menuOverlay(Ui &u, const eui::Screen &screen) {
    if (menu.id.empty())
        return;
    if (menu.items.empty() || screen.width <= 24 || screen.height <= 24) {
        menu.id.clear();
        return;
    }
    constexpr float rowHeight = 38, inset = 4, maxHeight = 388;
    const float contentHeight = float(menu.items.size()) * rowHeight;
    float h = std::min({contentHeight + inset * 2, maxHeight, screen.height - 16});
    const float w = std::min(std::max(138.0f, menu.anchor.width), screen.width - 16);
    const float below = std::max(0.f, screen.height - 8 - menu.anchor.y - menu.anchor.height - 4);
    const float above = std::max(0.f, menu.anchor.y - 12);
    const bool upward = h > below && above > below;
    const float available = upward ? above : below;
    if (available >= rowHeight + inset * 2)
        h = std::min(h, available);
    float x = std::clamp(menu.anchor.x, 8.0f, std::max(8.0f, screen.width - w - 8)),
          y = upward ? menu.anchor.y - h - 4 : menu.anchor.y + menu.anchor.height + 4;
    y = std::clamp(y, 8.f, std::max(8.f, screen.height - h - 8));
    const float viewport = h - inset * 2;
    if (std::fabs(menu.viewportHeight - viewport) > .01f)
        menu.revealHighlighted = true;
    menu.viewportHeight = viewport;
    menu.highlighted = std::clamp(menu.highlighted, 0, int(menu.items.size()) - 1);
    const float maximum = std::max(0.f, contentHeight - viewport);
    menu.offset = std::clamp(menu.offset, 0.f, maximum);
    if (menu.revealHighlighted) {
        const float top = menu.highlighted * rowHeight;
        if (top < menu.offset)
            menu.offset = top;
        else if (top + rowHeight > menu.offset + viewport)
            menu.offset = top + rowHeight - viewport;
        menu.offset = std::clamp(menu.offset, 0.f, maximum);
        menu.revealHighlighted = false;
    }
    const auto owner = menu.id;
    u.rect("menubackdrop")
        .size(screen.width, screen.height)
        .color("#00000000")
        .zIndex(1000)
        .preserveFocusOnPress()
        .onClick([] { menu.id.clear(); })
        .build();
    u.stack("menubox")
        .position(x, y)
        .size(w, h)
        .zIndex(1001)
        .content([&] {
            u.rect("menubg")
                .size(w, h)
                .color(surface)
                .radius(controlRadius)
                .border(1, line)
                .shadow(20, 0, 6, "#182C4526")
                .build();
            components::scrollView(u, "menuoverlay")
                .position(inset, inset)
                .size(w - inset * 2, viewport)
                .offset(menu.offset)
                .step(rowHeight)
                .scrollbarWidth(8)
                .scrollbarGap(4)
                .theme(tokens())
                .zIndex(1002)
                .onChange([owner](float next) {
                    if (menu.id == owner) {
                        menu.offset = next;
                        menu.revealHighlighted = false;
                    }
                })
                .content([owner](Ui &v, float width, float) {
                    for (int i = 0; i < int(menu.items.size()); ++i) {
                        std::string id = "menuitem" + std::to_string(i);
                        v.stack(id + ".wrap")
                            .size(width, 38)
                            .content([&] {
                                v.rect(id)
                                    .size(width, 38)
                                    .states(i == menu.highlighted ? blueWash : surface,
                                            i == menu.highlighted ? blueWashActive : surfaceHover,
                                            i == menu.highlighted ? blueWashActive : surfacePressed)
                                    .radius(6)
                                    .transition(transition(.12f))
                                    .preserveFocusOnPress()
                                    .onClick([i, owner] {
                                        if (menu.id != owner || i >= int(menu.items.size()))
                                            return;
                                        auto fn = menu.select;
                                        menu.id.clear();
                                        if (fn)
                                            fn(i);
                                    })
                                    .build();
                                v.text(id + ".text")
                                    .position(12, 0)
                                    .size(std::max(1.f, width - 24), 38)
                                    .text(menu.items[i])
                                    .fontSize(controlFont)
                                    .color(i == menu.highlighted ? blue : ink)
                                    .verticalAlign(eui::VerticalAlign::Center)
                                    .clip()
                                    .build();
                            })
                            .build();
                    }
                })
                .build();
            // 拖动菜单滚动条后仍让方向键作用于原下拉框。
            for (const auto *id : {"menuoverlay", "menuoverlay.scroll.track",
                                   "menuoverlay.scroll.thumb"})
                if (auto *node = u.find(id))
                    node->preserveFocusOnPress = true;
        })
        .build();
}
void helpText(Ui &u, const std::string &id, const std::string &copy, float w,
              const char *color = ink) {
    u.text(id)
        .width(w)
        .text(copy)
        .wrap()
        .maxWidth(w)
        .fontSize(19)
        .lineHeight(27)
        .color(color)
        .build();
}
void copyHelpContent(const std::string &content, const std::string &confirmation) {
    try {
        platform::clipboard(platform::wide(content));
        state.hint(confirmation);
    } catch (const std::exception &error) {
        state.feedback("复制失败，请重试", error.what(), Level::Error);
    }
}
void copyFilenameVariable(const std::string &token) {
    copyHelpContent(token, "已复制 " + token);
}
void filenameVariableHelp(Ui &u, const msa::FilenameVariable &variable, float w) {
    const auto id = std::string("help.variable.") + variable.name;
    const auto tokenWidth = [](const std::string &token) {
        float width = 24;
        for (const unsigned char c : token)
            if ((c & 0xC0) != 0x80)
                width += c < 0x80 ? 10.f : 18.f;
        return width;
    };
    const auto name = std::string("[") + variable.name + "]";
    const auto alias = *variable.alias ? "[" + std::string(variable.alias) + "]" : std::string{};
    const float nameWidth = tokenWidth(name), aliasWidth = alias.empty() ? 0 : tokenWidth(alias);
    const bool wide = w >= 760;
    const float tokenColumn = wide ? 312.f : 160.f;
    const float descriptionWidth = w - tokenColumn - 12;
    constexpr float exampleWidth = 206;
    const auto description = [&](float width) {
        helpText(u, id + ".description", variable.description, width);
    };
    const auto example = [&](float width) {
        helpText(u, id + ".example", std::string("示例 ") + variable.example, width, muted);
    };
    u.row(id)
        .width(w)
        .gap(12)
        .alignItems(eui::Align::CENTER)
        .content([&] {
            (wide ? u.row(id + ".tokens") : u.column(id + ".tokens"))
                .width(tokenColumn)
                .gap(wide ? 8.f : 5.f)
                .content([&] {
                    button(
                        u, id + ".copy", name, nameWidth, [name] { copyFilenameVariable(name); },
                        false, false, 32);
                    if (!alias.empty())
                        button(
                            u, id + ".alias", alias, aliasWidth,
                            [alias] { copyFilenameVariable(alias); }, false, false, 32);
                })
                .build();
            if (wide) {
                description(descriptionWidth - exampleWidth - 12);
                example(exampleWidth);
            } else {
                u.column(id + ".detail")
                    .width(descriptionWidth)
                    .content([&] {
                        description(descriptionWidth);
                        example(descriptionWidth);
                    })
                    .build();
            }
        })
        .build();
}
std::string levelName(Level level) {
    return level == Level::Error ? "错误" : level == Level::Warning ? "警告" : "信息";
}
constexpr float dataCardSpacing = sectionGap;

float dataResultContentHeight(float left, float right) {
    // Use the input cards' measured height at this width as the desktop baseline.
    Ui measure;
    measure.begin("data.column.measure");
    measure.row("columns")
        .gap(16)
        .content([&] {
            measure.column("form")
                .width(left)
                .gap(dataCardSpacing)
                .content([&] { inputs(measure, left); })
                .build();
            studySummary(measure, right);
        })
        .build();
    measure.end();
    measure.layout(left + right + 16, 0);
    return measure.find("form")->frame.height - measure.find("summary.card")->frame.height -
           dataCardSpacing - resultFixedHeight;
}

void dataPage(Ui &u, float w, float h) {
    // ScrollView measures and builds its content at the same width with the reserved scrollbar.
    std::optional<float> resultHeight;
    components::scrollView(u, "generatescroll")
        .size(w, h)
        .offset(state.generateOffset)
        .theme(tokens())
        .scrollbarGap(scrollGutter)
        .reserveScrollbar()
        .onChange([](float offset) {
            state.generateOffset = offset;
            menu.id.clear();
        })
        .content([&resultHeight](Ui &v, float cw, float) {
            v.column("generationcontent")
                .width(cw)
                .gap(dataCardSpacing)
                .padding(0, 0, 0, 10)
                .content([&] {
                    notice(v, cw);
                    if (cw >= 1080) {
                        const float left = std::clamp(cw * .38f, 460.f, 500.f),
                                    right = cw - left - 16;
                        if (!resultHeight)
                            resultHeight = dataResultContentHeight(left, right);
                        v.row("split")
                            .width(cw)
                            .gap(16)
                            .content([&] {
                                v.column("inputcolumn")
                                    .width(left)
                                    .gap(dataCardSpacing)
                                    .disabled(state.busy())
                                    .content([&] { inputs(v, left); })
                                    .build();
                                v.column("resultcolumn")
                                    .width(right)
                                    .gap(dataCardSpacing)
                                    .content([&] { results(v, right, true, *resultHeight); })
                                    .build();
                            })
                            .build();
                    } else {
                        studySummary(v, cw);
                        v.column("inputcolumn")
                            .width(cw)
                            .gap(dataCardSpacing)
                            .disabled(state.busy())
                            .content([&] { inputs(v, cw); })
                            .build();
                        results(v, cw, false);
                    }
                })
                .build();
        })
        .build();
}
constexpr float compactLogsWidth = 880, narrowLogsWidth = 520, logVerticalInset = 12,
                logTitleHeight = 46, logToolbarGap = 8, logTableInset = 12,
                logMinimumRowsHeight = 72;

float logToolsHeight(float w) {
    return w < narrowLogsWidth ? 2 * controlHeight + logToolbarGap : controlHeight;
}

float logHeaderHeight(float w) {
    return logVerticalInset * 2 + logTitleHeight +
           (w < compactLogsWidth ? logToolbarGap + logToolsHeight(w) : 0.f);
}

float minimumLogPanelHeight(float w) {
    return logHeaderHeight(w) + sectionGap + logTableInset * 2 + resultTableHeaderHeight +
           logMinimumRowsHeight;
}

void logsPanel(Ui &u, float w, float h) {
    const auto count = [](Level level) {
        return std::count_if(state.logs.begin(), state.logs.end(),
                             [level](const LogEntry &entry) { return entry.level == level; });
    };
    const auto matches = [](const LogEntry &entry) {
        return state.logFilter == 0 ||
               (state.logFilter == 1 ? entry.level == Level::Warning : entry.level == Level::Error);
    };
    const auto shown = std::count_if(state.logs.begin(), state.logs.end(), matches);
    const std::vector<std::string> filters{"全部 " + std::to_string(state.logs.size()),
                                           "警告 " + std::to_string(count(Level::Warning)),
                                           "错误 " + std::to_string(count(Level::Error))};
    const auto copy = [matches] {
        safe([matches] {
            std::string content;
            for (const auto &entry : state.logs)
                if (matches(entry))
                    content +=
                        entry.time + " [" + levelName(entry.level) + "] " + entry.message + "\r\n";
            if (content.empty())
                return;
            platform::clipboard(platform::wide(content));
            state.logsCopied = true;
            state.hint("已复制当前显示的日志");
        });
    };

    const bool compact = w < compactLogsWidth;
    constexpr float inset = 16;
    constexpr float panelGap = sectionGap;
    const float toolsHeight = logToolsHeight(w), headerHeight = logHeaderHeight(w);
    const float controlsWidth = w - inset * 2;
    constexpr float captionWidth = 192;
    const float toolsWidth = compact ? controlsWidth : controlsWidth - captionWidth - inset;
    const float followWidth = compact ? 124.f : 138.f;
    const float copyWidth = compact ? 126.f : 138.f;
    const float filtersWidth =
        std::min(300.f, toolsWidth - followWidth - copyWidth - 3 * logToolbarGap);
    const auto heading = [&] {
        u.row("logs.heading")
            .size(compact ? controlsWidth : captionWidth, logTitleHeight)
            .alignItems(eui::Align::CENTER)
            .gap(9)
            .content([&] {
                u.rect("logs.heading.accent").size(3, 18).color(blue).radius(1.5f).build();
                u.column("logs.heading.copy")
                    .width(captionWidth - 12)
                    .gap(2)
                    .content([&] {
                        text(u, "logs.title", "运行日志", captionWidth - 12, 26, 17);
                        text(u, "logs.retention",
                             "本次运行最多保留 " + std::to_string(logLimit) + " 条",
                             captionWidth - 12, 18, 11, muted);
                    })
                    .build();
            })
            .build();
    };
    const auto filterControl = [&](float width) {
        segments(
            u, "logfilter", filters, state.logFilter, width,
            [](int i) {
                state.logFilter = i;
                state.logsCopied = false;
                state.logOffset = 0;
                state.followLogs = false;
            },
            38);
    };
    const auto actions = [&] {
        toggle(u, "logfollow", "跟随最新", state.followLogs, followWidth, [](bool follow) {
            state.followLogs = follow;
            if (follow)
                state.logOffset = 1000000;
        });
        button(u, "copylogs", state.logsCopied ? "已复制日志" : "复制当前日志", copyWidth, copy,
               false, shown == 0, 38);
    };
    const auto toolbar = [&] {
        if (w < narrowLogsWidth) {
            u.column("logs.toolbar")
                .size(toolsWidth, toolsHeight)
                .gap(logToolbarGap)
                .content([&] {
                    filterControl(toolsWidth);
                    u.row("logs.toolbar.actions")
                        .size(toolsWidth, controlHeight)
                        .gap(logToolbarGap)
                        .justifyContent(eui::Align::END)
                        .alignItems(eui::Align::CENTER)
                        .content(actions)
                        .build();
                })
                .build();
            return;
        }
        u.row("logs.toolbar")
            .size(toolsWidth, toolsHeight)
            .gap(logToolbarGap)
            .alignItems(eui::Align::CENTER)
            .content([&] {
                filterControl(filtersWidth);
                u.rect("logs.toolbar.space")
                    .width(eui::SizeValue::fill())
                    .height(1)
                    .color("#00000000")
                    .build();
                actions();
            })
            .build();
    };

    constexpr float tableHeaderHeight = resultTableHeaderHeight, rowMinimum = 46;
    constexpr float cellInset = 14, timeWidth = 84, levelWidth = 58, cellGap = 12;
    constexpr float barWidth = 6, barGap = 4;
    const float tableWidth = w - logTableInset * 2;
    const float messageWidth = tableWidth - cellInset * 2 - timeWidth - levelWidth - cellGap * 2;
    const auto rows = [&](Ui &v, float, float) {
        if (!shown) {
            v.row("logs.empty.state")
                .width(tableWidth)
                .minHeight(108)
                .padding(cellInset, 16)
                .gap(12)
                .alignItems(eui::Align::CENTER)
                .content([&] {
                    emptyDocumentIcon(v, "logs.empty.icon");
                    const float width = tableWidth - cellInset * 2 - 52;
                    v.column("logs.empty.copy")
                        .width(width)
                        .gap(4)
                        .content([&] {
                            text(v, "logs.empty.title",
                                 state.logFilter == 0   ? "暂无运行记录"
                                 : state.logFilter == 1 ? "暂无警告"
                                                        : "暂无错误",
                                 width, 24, 15);
                            paragraph(v, "logs.empty",
                                      state.logFilter == 0
                                          ? "生成、保存和导出后的操作记录会显示在这里。"
                                          : "可切换到「全部」查看本次运行的记录。",
                                      width, muted, 16);
                        })
                        .build();
                })
                .build();
            return;
        }
        bool alternate = false;
        for (const auto &entry : state.logs) {
            if (!matches(entry))
                continue;
            const auto id = "log." + std::to_string(entry.sequence);
            v.stack(id + ".wrap")
                .width(tableWidth)
                .content([&] {
                    v.rect(id + ".bg")
                        .width(eui::SizeValue::fill())
                        .height(eui::SizeValue::fill())
                        .ignoreLayout()
                        .color(alternate ? tableStripe : surface)
                        .build();
                    v.row(id)
                        .width(tableWidth)
                        .minHeight(rowMinimum)
                        .padding(cellInset, 9)
                        .gap(cellGap)
                        .content([&] {
                            text(v, id + ".time", entry.time, timeWidth, 28, 12, muted);
                            badge(v, id + ".level", levelName(entry.level), levelWidth,
                                  levelColor(entry.level), levelSurface(entry.level));
                            v.text(id + ".message")
                                .width(messageWidth)
                                .minHeight(28)
                                .text(entry.message)
                                .fontSize(tableValueFont)
                                .lineHeight(24)
                                .color(ink)
                                .verticalAlign(eui::VerticalAlign::Center)
                                .wrap()
                                .maxWidth(messageWidth)
                                .build();
                        })
                        .build();
                })
                .build();
            alternate = !alternate;
        }
    };

    const float available = std::max(
        logMinimumRowsHeight, h - headerHeight - panelGap - logTableInset * 2 - tableHeaderHeight);
    float bodyHeight = available;
    // A short log should stop after its last row. Measure only when rows might fit.
    if (static_cast<float>(shown) * rowMinimum < available) {
        Ui measure;
        measure.begin("logs.measure");
        measure.column("rows")
            .width(tableWidth)
            .content([&] { rows(measure, tableWidth, available); })
            .build();
        measure.end();
        measure.layout(tableWidth, 0);
        bodyHeight = std::min(available, measure.find("rows")->frame.height);
    }
    const float tableHeight = logTableInset * 2 + tableHeaderHeight + bodyHeight;
    u.column("logs.page")
        .size(w, h)
        .gap(panelGap)
        .content([&] {
            components::card(u, "logs.controls.card")
                .size(w, headerHeight)
                .theme(tokens())
                .padding(0)
                .color(surface)
                .radius(cardRadius)
                .border(1, line)
                .shadow(cardShadow())
                .content([&] {
                    if (compact)
                        u.column("logs.controls")
                            .size(w, headerHeight)
                            .padding(inset, logVerticalInset)
                            .gap(logToolbarGap)
                            .content([&] {
                                heading();
                                toolbar();
                            })
                            .build();
                    else
                        u.row("logs.controls")
                            .size(w, headerHeight)
                            .padding(inset, logVerticalInset)
                            .gap(inset)
                            .alignItems(eui::Align::CENTER)
                            .content([&] {
                                heading();
                                toolbar();
                            })
                            .build();
                })
                .build();
            components::card(u, "logs.card")
                .size(w, tableHeight)
                .theme(tokens())
                .padding(0)
                .color(surface)
                .radius(cardRadius)
                .border(1, line)
                .shadow(cardShadow())
                .content([&] {
                    u.column("logs.table")
                        .size(w, tableHeight)
                        .padding(logTableInset)
                        .content([&] {
                            u.stack("logs.table.heading")
                                .size(tableWidth, tableHeaderHeight)
                                .content([&] {
                                    u.rect("logs.table.surface")
                                        .size(tableWidth, tableHeaderHeight)
                                        .color(tableHeader)
                                        .radius(controlRadius)
                                        .build();
                                    u.row("logs.table.labels")
                                        .size(tableWidth, tableHeaderHeight)
                                        .padding(cellInset, 0)
                                        .gap(cellGap)
                                        .alignItems(eui::Align::CENTER)
                                        .content([&] {
                                            text(u, "logs.table.time", "时间", timeWidth, 28, 12,
                                                 muted);
                                            u.text("logs.table.level")
                                                .size(levelWidth, 28)
                                                .text("级别")
                                                .fontSize(tableLabelFont)
                                                .color(muted)
                                                .horizontalAlign(eui::HorizontalAlign::Center)
                                                .verticalAlign(eui::VerticalAlign::Center)
                                                .build();
                                            text(u, "logs.table.message", "操作记录", messageWidth,
                                                 28, 12, muted);
                                        })
                                        .build();
                                })
                                .build();
                            components::scrollView(u, "logscroll")
                                .size(tableWidth + barWidth + barGap, bodyHeight)
                                .offset(state.logOffset)
                                .theme(tokens())
                                .scrollbarWidth(barWidth)
                                .scrollbarGap(barGap)
                                .reserveScrollbar()
                                .onChange([&u](float offset) {
                                    state.logOffset = offset;
                                    const auto *scroll = u.find("logscroll");
                                    state.followLogs =
                                        scroll && offset >= scroll->scrollMaxOffset - 1;
                                    menu.id.clear();
                                })
                                .content(rows)
                                .build();
                        })
                        .build();
                })
                .build();
        })
        .build();
}

void logsPage(Ui &u, float w, float h) {
    if (h >= minimumLogPanelHeight(w)) {
        state.logPageOffset = 0;
        logsPanel(u, w, h);
        return;
    }
    components::scrollView(u, "logs.page.scroll")
        .size(w, h)
        .offset(state.logPageOffset)
        .theme(tokens())
        .scrollbarWidth(6)
        .scrollbarGap(4)
        .reserveScrollbar()
        .onChange([](float offset) {
            state.logPageOffset = offset;
            menu.id.clear();
        })
        .content(
            [](Ui &v, float width, float) { logsPanel(v, width, minimumLogPanelHeight(width)); })
        .build();
}

void helpSubheading(Ui &u, const std::string &id, const std::string &title, float w) {
    u.column(id + ".rule")
        .width(w)
        .gap(7)
        .content([&] {
            u.rect(id + ".divider").size(w, 1).color(line).build();
            text(u, id, title, w, 25, 15, blue);
        })
        .build();
}
void helpExample(Ui &u, const std::string &id, const std::string &copy, float w) {
    components::card(u, id + ".card")
        .width(w)
        .wrapContentHeight()
        .theme(tokens())
        .padding(0)
        .radius(controlRadius)
        .color(surfaceSoft)
        .border(1, line)
        .shadow({})
        .content([&] {
            u.column(id + ".inset")
                .width(w)
                .padding(12, 8)
                .content([&] { helpText(u, id, copy, w - 24, ink); })
                .build();
        })
        .build();
}
void helpItem(Ui &u, const std::string &id, const std::string &title, const std::string &copy,
             float w) {
    helpSubheading(u, id + ".title", title, w);
    helpText(u, id, copy, w);
}
void helpRow(Ui &u, const std::string &row, const char *label, const char *copy, float w) {
    constexpr float gap = 14.f;
    const float labelWidth = std::min(150.f, w * .29f);
    u.row(row)
        .width(w)
        .gap(gap)
        .content([&] {
            helpText(u, row + ".label", label, labelWidth, blue);
            helpText(u, row + ".copy", copy, w - labelWidth - gap);
        })
        .build();
}
template <std::size_t N>
void helpRows(Ui &u, const std::string &id,
              const std::array<std::pair<const char *, const char *>, N> &rows, float w) {
    for (std::size_t i = 0; i < rows.size(); ++i)
        helpRow(u, id + "." + std::to_string(i), rows[i].first, rows[i].second, w);
}
void helpCommand(Ui &u, const std::string &id, const std::string &command, float w) {
    helpExample(u, id, command, w);
    u.row(id + ".actions")
        .width(w)
        .justifyContent(eui::Align::END)
        .content([&] {
            button(u, id + ".copy", "复制命令", 112,
                   [command] { copyHelpContent(command, "已复制命令"); }, false, false, 32);
        })
        .build();
}
void helpQuickStart(Ui &v, float cw) {
    section(v, "help.workflow", "从示例开始", cw, [&](float c) {
        helpText(v, "help.workflow.intro",
                 "「载入示例配置」会填入一组可生成的参数并返回「数据」页。点击「生成数据」即可查看结果。",
                 c);
        button(
            v, "sample", "载入示例配置", 156,
            [] {
                if (state.busy())
                    return;
                state.sample();
                state.navigate(Page::Data);
            },
            false, state.busy());

        const auto step = [&](const std::string &id, const std::string &number,
                              const std::string &title, const std::string &detail) {
            v.rect(id + ".divider").size(c, 1).color(line).build();
            v.row(id + ".row")
                .width(c)
                .gap(12)
                .content([&] {
                    badge(v, id + ".number", number, 40);
                    v.column(id + ".content")
                        .width(c - 52)
                        .gap(4)
                        .content([&] {
                            text(v, id + ".title", title, c - 52, 26, 15);
                            helpText(v, id, detail, c - 52);
                        })
                        .build();
                })
                .build();
        };
        step("help.step1", "1", "设置研究设计",
             "在「选项 → 研究 → 研究设计」设置操作员数、重复测量次数和部件数。默认操作员名称"
             "留空时依次使用 A、B、C；选择 2 人时，第三个名称保留，切回 3 人后可继续编辑。"
             "在「选项 → 分析」选择分析方法。");
        step("help.step2", "2", "填写生成条件",
             "在「数据」页填写规格限和指标约束，数据中心可留空。「规格内生成」需要至少一侧规格限，"
             "「随机取样」和「最低部件跨度」都需要上下规格限。");
        step("help.step3", "3", "生成并核对结果",
             "点击「生成数据」。在「指标复核」核对实际值，在「数据预览」检查测量记录，"
             "再到「分析详情」或「ANOVA 详情」查看计算结果。搜索期间可在底栏取消。");
        step("help.step4", "4", "导出数据",
             "在「选项 → 文件」选择格式、运行顺序和保存位置，然后回到「数据」页点击「导出数据」。");
    });

    section(v, "help.rules", "部件取样与分布", cw, [&](float c) {
        helpText(v, "help.part.sampling",
                 "在「选项 → 研究 → 生成规则」选择「部件取样」。每件部件的基础尺寸供所有操作员和"
                 "重复测量使用。",
                 c);
        button(v, "help.rules.open", "前往研究选项", 160, [] {
            state.optionsTab = 0;
            state.configOffset = 0;
            state.navigate(Page::Options);
        });
        for (const auto &method : msa::part_sampling_methods)
            helpRow(v, std::string("help.part.sampling.") + method.key, method.label,
                    method.description, c);
        helpItem(v, "help.part.random", "随机取样的中心与尺度",
                 "请在「数据」页填写上下规格限。每件部件的基础尺寸各自独立抽取，两端可能都没有部件。"
                 "填写数据中心后，搜索会按生成优先级挑选候选结果。选择参考总变异时，参考标准差控制"
                 "测量扰动的尺度，部件基础尺寸仍从整个规格区间抽取。",
                 c);
        helpItem(v, "help.part.span", "最低部件跨度",
                 "在「生成参数」选填百分比，留空时沿用生成尺度。程序先按最终精度舍入读数，"
                 "再按部件求均值。规格为 9.5～10.5 时，填写 80 表示最大与最小部件均值至少相差 0.8。",
                 c);
        helpItem(v, "help.normality", "要求近似正态",
                 "开启后，程序用 Anderson–Darling 的 5% 参考阈值筛选舍入后的全部读数。"
                 "「正态分位」安排的是部件基础尺寸，最终读数仍需通过筛选。选择「两端加密」或"
                 "「双批取样」时，满足这项要求可能需要更多尝试。",
                 c);
    });

    section(v, "help.scale", "生成尺度与种子", cw, [&](float c) {
        constexpr std::array<std::pair<const char *, const char *>, 4> settings{{
            {"生成优先级",
             "「中心优先」先找更接近所填中心的结果。「指标优先」先找更接近指标目标的结果。"
             "候选数据均须满足已启用的约束。"},
            {"规格内生成", "开启后，每个最终测量值都须落在已填写的规格限内。关闭后允许超限。"},
            {"生成尺度",
             "选择「参考总变异」时填写参考总变异标准差（TV_SD），留空则用历史标准差。"
             "只填一侧规格限或未填规格时，也需要其中一项。"},
            {"数据生成种子",
             "相同版本、配置和固定种子可复现生成结果。搜索超时可能提前结束，"
             "小数位数同时决定生成时的舍入和 导出测量值的精度。"},
        }};
        helpRows(v, "help.scale.settings", settings, c);
    });

    section(v, "help.settings", "保存配置", cw, [&](float c) {
        helpText(v, "help.settings.save",
                 "在「选项」底栏点击「保存配置」，下次启动会自动载入。点击「恢复默认」后，"
                 "再保存即可在下次启动使用默认设置。配置路径可在「配置管理」复制或打开所在目录。",
                 c);
        helpExample(v, "help.path", "%USERPROFILE%\\MSA数据生成器\\msa_generator.json", c);
        helpText(v, "help.settings.regenerate",
                 "修改研究、生成、规格、指标或分析设置后，需要重新生成。仅调整导出设置时，"
                 "可以直接用同一批数据再次导出。",
                 c);
    });

    section(v, "help.troubleshooting", "生成失败时怎么排查", cw, [&](float c) {
        constexpr std::array<std::pair<const char *, const char *>, 3> issues{{
            {"参数校验失败", "按提示补齐规格限或参考尺度，并检查数字格式、范围和小数位。"},
            {"约束冲突", "检查指标范围能否同时满足。规格内可用的精度数值过少时，可提高小数位数。"},
            {"搜索未找到结果",
             "先检查指标误差、最低部件跨度和近似正态要求，再按需增加「约束与搜索」里的尝试次数或时限。"},
        }};
        helpRows(v, "help.troubleshooting.cases", issues, c);
        helpItem(v, "help.logs", "查看日志和保存路径",
                 "「日志」显示本次运行最近的 100 条记录，可筛选「全部」「警告」或「错误」。"
                 "导出失败时，从日志查看完整原因。点击「复制当前日志」可带走筛选后的记录，"
                 "开启「跟随最新」可回到列表末尾。",
                 c);
        button(v, "help.logs.open", "打开日志页", 144, [] { state.navigate(Page::Logs); });
    });
}

void helpAnalysis(Ui &v, float cw) {
    section(v, "help.analysis", "选择分析方法", cw, [&](float c) {
        helpText(v, "help.analysis.select",
                 "两种方法均用于平衡交叉量具 R&R。每名操作员测量全部部件，每个组合重复测量相同次数。"
                 "支持 2 或 3 名操作员、2 或 3 次重复测量及 5～10 个部件。",
                 c);
        button(v, "help.analysis.open", "前往分析选项", 160, [] {
            state.optionsTab = 1;
            state.configOffset = 0;
            state.navigate(Page::Options);
        });
        helpItem(v, "help.xbarr", "Xbar-R（均值极差法）",
                 "按每个操作员与部件组合的极差估计重复性（EV），用操作员均值的极差估计再现性（AV）并扣除重复性影响，"
                 "用部件均值的极差估计部件间变异（PV）。在「分析详情」查看分量。",
                 c);
        helpItem(v, "help.anova", "ANOVA（方差分析法）",
                 "把部件和操作员作为随机因子，以平方和、自由度和均方估计方差分量。"
                 "在「ANOVA 详情」查看最终模型、完整模型的交互项 P 值、方差分析表和方差分量。",
                 c);
        helpItem(v, "help.anova.interaction", "部件 × 操作员交互项",
                 "选择「按 P 值判断」时可设置显著性水平 α，默认 0.05。P < α 时保留交互项并计入再现性。"
                 "P ≥ α 或 P 值不可计算时，将交互项并入重复性误差。也可直接选择「保留交互项」或"
                 "「并入重复性误差」。方差分量为负时按 0 计算。修改后请重新生成。",
                 c);
    });
    section(v, "help.metrics", "读懂复核指标", cw, [&](float c) {
        constexpr std::array<std::pair<const char *, const char *>, 5> metrics{{
            {"EV / TV %", "重复性占总变异的比例，反映同一操作员反复测量同一部件的波动。"},
            {"AV / TV %", "再现性占总变异的比例，反映操作员间的差异。ANOVA 保留的交互分量也计入再现性。"},
            {"GRR / TV %", "量具 R&R 占总变异的比例。量具 R&R 合并重复性与再现性。"},
            {"P/T %", "量具 R&R 的研究变异相对有效容差的比例。有效容差由分析容差或规格限确定。"},
            {"ndc", "可区分类别数。将 1.41 × PV_SD ÷ GRR_SD 向下取整，最小为 1。GRR_SD 为 0 时不可计算。"},
        }};
        helpRows(v, "help.metrics.values", metrics, c);
        helpText(v, "help.sd",
                 "EV、AV、GRR、PV 和 TV 均以标准差 SD（1σ）表示。研究变异等于 k × SD，k 默认取 6。"
                 "「指标复核」中的百分比显示两位小数，ndc 显示整数。判定使用完整精度。",
                 c);
    });
    section(v, "help.constraints", "选择指标约束", cw, [&](float c) {
        constexpr std::array<std::pair<const char *, const char *>, 5> constraintModes{{
            {"不约束", "仅查看该指标的实际值。"},
            {"目标值", "接受目标值加减允许误差形成的范围。误差填 0 时要求等于目标值。"},
            {"区间", "接受下限到上限之间的数值，包含两端。"},
            {"不高于", "接受小于或等于阈值的数值。"},
            {"不低于", "接受大于或等于阈值的数值。"},
        }};
        helpRows(v, "help.constraints.modes", constraintModes, c);
        helpItem(v, "help.units", "允许误差",
                 "百分比指标的误差以百分点填写。ndc 的目标值和误差都用整数。"
                 "误差留空时采用「选项 → 研究 → 约束与搜索」里的默认允许误差。",
                 c);
        helpExample(v, "help.constraints.example",
                    "将 GRR / TV % 设为「目标值」，填写 10 和 0.1，接受范围为 9.90%～10.10%。", c);
        helpText(v, "help.constraints.result",
                 "「满足」表示生成结果达到所设约束。评价实际测量系统时，应结合真实测量数据与验收要求。", c);
    });
    section(v, "help.process", "P/T 与历史过程", cw, [&](float c) {
        helpItem(v, "help.process.tolerance", "如何确定有效容差",
                 "优先使用「选项 → 分析 → 评价口径」里的分析容差。留空时，双侧规格取 USL − LSL。"
                 "单侧规格用规格限与均值之间距离的两倍。历史均值留空时用实际测量均值，距离为零时 P/T 不可计算。",
                 c);
        helpExample(v, "help.process.tolerance.formula",
                    "P/T% = 100 × k × GRR_SD ÷ 有效容差\n"
                    "例如 GRR_SD = 0.02、k = 6，有效容差为 1，则 P/T 为 12%。",
                    c);
        helpItem(v, "help.process.history", "选择过程变异估计来源",
                 "选择「优先使用历史标准差」时，历史 SD ≥ GRR_SD 便以历史 SD 作为 TV_SD，并由 TV_SD 和 "
                 "GRR_SD 推算 PV_SD。历史 SD 留空或小于 GRR_SD 时，使用本次研究估计。"
                 "选择「按本次研究估计」则始终以本次研究计算 TV 和 PV。填写历史标准差后，"
                 "可查看各分量 SD 相对历史标准差的百分比（%Process）。",
                 c);
    });
    section(v, "help.inference", "ANOVA 统计推断", cw, [&](float c) {
        helpItem(v, "help.process.classification", "计算误分类概率",
                 "在「选项 → 分析 → 统计推断」开启，需至少一侧规格限。结果可查看真实不合格却被判合格、"
                 "真实合格却被判不合格的概率，并分别给出联合概率和条件概率。"
                 "模型假定真实部件尺寸与测量误差独立且服从正态分布。",
                 c);
        helpText(v, "help.process.classification.source",
                 "当历史 SD ≥ GRR_SD 时，误分类使用历史过程波动，否则使用研究部件间变异。"
                 "「过程变异估计来源」可单独设置。条件事件概率为 0 时，相应条件概率不可计算。",
                 c);
        helpItem(v, "help.process.confidence", "计算置信区间",
                 "在「统计推断」选择置信水平与置信限类型。可查看方差分量、标准差、研究变异、P/T 和 "
                 "%Process 的双侧区间或单侧置信限。重复性采用卡方区间，其他分量可选「MLS（Minitab）」"
                 "或「Satterthwaite（JMP）」。",
                 c);
        helpText(v, "help.process.confidence.range",
                 "MLS 的最低适用水平随操作员数和置信限类型变化。界面提示下界时，可提高置信水平或改选 "
                 "Satterthwaite。部分边界情形以「—」标记不可计算的限值。"
                 "%Contribution、%Study Var 和 ndc 在报告中提供点估计。",
                 c);
    });
    section(v, "help.software", "比较不同软件的结果", cw, [&](float c) {
        helpItem(v, "help.software.setup", "先统一分析设置",
                 "使用相同的完整记录和分析方法，并核对规格限、分析容差、研究变异倍数、历史均值和标准差、"
                 "过程变异来源及区间算法。ANOVA 还应核对随机效应、交互项、EMS 均方估计和负方差分量的处理。"
                 "REML、Bayesian 或固定效应模型应按各自口径解释。",
                 c);
        helpItem(v, "help.software.columns", "再对照报表列",
                 "EV / TV %、AV / TV %、GRR / TV % 对应 Minitab 的 %Study Var（标准差之比）。"
                 "%Contribution 使用方差之比。P/T % 对照总量具 R&R 行的 %Tolerance。"
                 "Trial 和 StdOrder 用于追溯测量次数与标准顺序，模型使用操作员和部件作为因子。",
                 c);
        helpText(v, "help.software.single",
                 "仅有一侧规格时，应核对软件如何计算有效容差。例如 MATLAB 原生应用会将缺失一侧视为无穷规格。",
                 c);
    });
}

void helpCsv(Ui &v, float cw) {
    section(v, "help.files", "导出当前结果", cw, [&](float c) {
        helpText(v, "help.csv.start",
                 "在「选项 → 文件」选择测量数据文件类型、数据布局、运行顺序和保存位置。回到「数据」页，"
                 "点击底部「导出数据」。在「导出方式」关闭测量数据静默导出，即可每次选择位置。",
                 c);
        button(v, "help.files.open", "前往文件选项", 160, [] {
            state.optionsTab = fileOptionsTab;
            state.configOffset = 0;
            state.navigate(Page::Options);
        });
        helpItem(v, "help.csv.location", "保存位置",
                 "在「目录与文件名」填写「测量数据导出目录」和「测量数据导出名称」。目录留空时使用 Windows"
                 " 系统桌面，相对路径也从桌面开始。完整保存路径和失败原因可在「日志」查看。",
                 c);
    });
    section(v, "help.runorder", "数据布局与运行顺序", cw, [&](float c) {
        helpItem(v, "help.csv", "默认（宽表）",
                 "列按部件编号排列，每行记录一名操作员在一轮中测得的全部数值。CSV 宽表采用 "
                 "UTF-8 BOM 编码。选择任一长表格式即可设置逐条测量的运行顺序。",
                 c);
        helpItem(v, "help.csv.encoding", "长表",
                 "Minitab、SPSS、JMP 19、Origin 和 MATLAB 共用长表。每行一次测量，"
                 "依次记录操作员（Operator）、部件（Part）、重复测量编号（Trial）和"
                 "测量值（Measurement）。CSV 长表采用 UTF-8 无 BOM，CSV 宽表和长表均使用"
                 "英文逗号、小数点及 CRLF 换行。",
                 c);
        helpItem(v, "help.runorder.modes", "三种运行顺序",
                 "标准顺序 · 依次按重复测量轮次、部件、操作员排列。\n"
                 "全部随机化 · 打乱所有测量记录，可跨轮次和操作员排列。\n"
                 "操作员内随机化 · 每轮按操作员分组，各组独立打乱部件顺序。",
                 c);
        helpText(v, "help.runorder.operators",
                 "选用「操作员内随机化」后，可勾选「随机化操作员顺序」。"
                 "每项研究确定一次操作员顺序，各轮测量沿用该顺序。",
                 c);
        helpItem(v, "help.runorder.seed", "复现运行顺序",
                 "运行顺序种子只控制测量记录的排列，数据生成种子控制测量值。"
                 "随机化时可填正整数，留空时为本批数据自动选取。比较多种运行顺序时，"
                 "对同一批数据重复导出即可。重新生成时可固定两个种子和研究设置，也可用 "
                 "--replay-report 从含导出记录的生成报告还原测量数据文件。",
                 c);
        helpItem(v, "help.runorder.standard", "追溯标准顺序",
                 "在「数据布局」勾选「保留标准运行序号（StdOrder）」，首列会记录"
                 "随机化前的序号。数据行序是实际运行顺序，Trial 始终保留原重复测量编号。"
                 "排序会移动完整记录，测量值和字段对应关系随记录一起移动。",
                 c);
        helpItem(v, "help.runorder.identity", "手工核对序号",
                 "StdOrder 等于 ((Trial − 1) × 部件数 + Part − 1) × 操作员数 + "
                 "操作员编号。编号 1、2、3 对应「研究设计」中从左到右的操作员，默认名称为 A、B、C。"
                 "总记录数为操作员数 × 重复测量次数 × 部件数。",
                 c);
    });
    section(v, "help.minitab", "导入 Minitab 并核对结果", cw, [&](float c) {
        helpItem(v, "help.minitab.import", "导入并指定列",
                 "选择 Minitab 格式导出，在 Minitab 中以首行为列名打开 CSV。进入「统计 → "
                 "质量工具 → 量具研究 → 交叉量具 R&R」，将 Part 指定为部件，Operator "
                 "指定为操作员，Measurement 指定为测量数据。Trial 和 StdOrder 用于追溯。",
                 c);
        helpItem(v, "help.minitab.method", "统一分析设置",
                 "对照同一批数据，使用相同的分析方法、研究变异倍数和规格设置。"
                 "选择 ANOVA 时还需对齐交互项设置。比较 P/T 时统一分析容差与均值口径。",
                 c);
        helpItem(v, "help.minitab.columns", "核对指标",
                 "EV/TV%、AV/TV% 和 GRR/TV% 对应各分量的 %Study Var，P/T% 对应总量具 "
                 "R&R 的 %Tolerance。对比运行顺序时核对记录数、测量值及字段对应关系。",
                 c);
    });
}

void helpFilename(Ui &v, float cw) {
    section(v, "help.filename", "导入与导出名称变量", cw, [&](float c) {
        helpText(v, "help.filename.syntax",
                 "点击变量名复制带方括号的写法，可用于「选项 → 文件 → 目录与文件名」中的"
                 "「测量数据导入名称」「测量数据导出名称」「图表导出名称」和「方法对比报告导出名称」。"
                 "只填写基本名称，导出时按所选文件类型添加扩展名。",
                 c);
        button(v, "help.filename.open", "编辑导入与导出名称", 176, [] {
            state.optionsTab = fileOptionsTab;
            state.configOffset = 0;
            state.navigate(Page::Options);
        });
        helpExample(v, "help.filename.example",
                    "研究_[OPERATORS]人_[PARTS]件_[DATETIME]\n"
                    "→ 研究_3人_10件_2026-10-24_22-08-12.csv",
                    c);
        for (const auto &variable : msa::filename_variables()) {
            const std::string name = variable.name;
            if (name == "YYYY")
                helpSubheading(v, "help.filename.date", "日期与时间", c);
            else if (name == "OPERATORS")
                helpSubheading(v, "help.filename.study", "研究参数", c);
            else if (name == "EV_PCT")
                helpSubheading(v, "help.filename.metrics", "实际指标", c);
            else if (name == "VERSION")
                helpSubheading(v, "help.filename.version", "程序信息", c);
            filenameVariableHelp(v, variable, c);
        }
        helpItem(v, "help.filename.values", "拼写和取值",
                 "变量区分大小写，可使用表中的英文名或中文别名。[MM] 表示月份，[M] 表示分钟。"
                 "输入 [[ 或 ]] 可输出字面方括号。日期取本次操作的本地时间。"
                 "测量数据导出使用已生成结果，图表和报告使用所选数据源的研究设置与指标。"
                 "百分比指标保留两位小数并省略百分号。缺少的值写为 NA，导入的数据没有生成种子。",
                 c);
        helpItem(v, "help.filename.import", "预填导入名称",
                 "导入时使用当前研究设置和所选数据源的可用指标。尚无有效结果时，指标写为 NA。"
                 "展开后的名称仅用于预填文件选择窗口，仍可另选 CSV 或 XLSX。留空时直接选择文件。",
                 c);
        helpItem(v, "help.filename.collision", "同名文件与保存失败",
                 "测量数据静默导出时，「追加序号」会在扩展名前加 _001、_002 等；「覆盖文件」会替换同名文件。"
                 "图表和报告静默保存时，重名会追加 _1、_2 等。出现保存或导出错误时，先检查变量拼写、"
                 "括号、Windows 保留名称和文件名长度，再到「日志」查看具体原因。",
                 c);
    });
}

void helpCli(Ui &v, float cw) {
    section(v, "help.cli", "在 PowerShell 中使用命令行", cw, [&](float c) {
        helpText(v, "help.cli.start",
                 "在包含 msa_cli.exe 的解压目录打开 PowerShell，可先查看完整参数。",
                 c);
        helpCommand(v, "help.cli.help", ".\\msa_cli.exe --help", c);
        helpItem(v, "help.cli.config", "选择配置来源",
                 "命令行参数优先于 JSON 配置，配置缺项用内置默认值。省略 --config 时读取"
                 "用户配置，文件不存在时使用内置默认值。--no-config 可跳过用户配置，"
                 "与 --config 二选一。普通运行只读取配置。--init 可创建缺失的默认模板，"
                 "生成前按需补齐规格和尺度。",
                 c);
        helpItem(v, "help.cli.syntax", "常用参数写法",
                 "含空格的路径用引号括起。用 null 清空可选数值，布尔选项填写 true 或 "
                 "false。同一参数写多次时，以最后一次为准。生成数据时可用 --operator-a、--operator-b、"
                 "--operator-c 分别指定三位操作员的名称，例如 --operator-a 张工。",
                 c);
    });
    section(v, "help.cli.generate", "生成并导出数据", cw, [&](float c) {
        helpText(v, "help.cli.example.intro",
                 "这条命令使用分层随机、80% 最低部件跨度，导出 Minitab 长表和 JSON 报告。",
                 c);
        helpCommand(v, "help.cli.example",
                    ".\\msa_cli.exe --no-config --lsl 9 --usl 11 --seed 123 "
                    "--part-sampling stratified --minimum-part-span-pct 80 "
                    "--csv-format minitab --output data.csv --report result.json",
                    c);
        helpText(v, "help.cli.normality",
                 "将 --part-sampling 的值换为 uniform_random，可在 --lsl 和 --usl 之间随机抽取部件基础尺寸。"
                 "加上 --require-normality true 可筛选舍入后的整批测量值。"
                 "启用的规格、指标、跨度和近似正态要求须同时满足。",
                 c);
        helpItem(v, "help.cli.targets", "给指标设置约束",
                 "例如 --ev target:10:0.1 表示 EV/TV% 的目标值为 10%，允许误差为 0.1 个百分点。"
                 "也可用 between:下限:上限、at_most:阈值、at_least:阈值或 none，整项替换原约束。",
                 c);
    });
    section(v, "help.cli.analyze.section", "分析已有测量数据", cw, [&](float c) {
        helpText(v, "help.cli.analyze",
                 "用 --input 读取 CSV 或 XLSX。带标签的宽表和长表自动识别操作员名称、人数、"
                 "部件数和重复次数。纯数值宽表按位置使用 A/B/C。六行纯数值宽表须显式指定设计："
                 "--replicates 3 表示 2 人各测 3 次，--replicates 2 表示 3 人各测 2 次。"
                 "省略时命令行会提示补充参数。分析 XLSX 时可加 --sheet \"复测 数据\" 指定工作表，"
                 "省略时读取第一张数据工作表。用 --report 保存分析报告。下例分析上一步的 Minitab 长表。",
                 c);
        helpCommand(v, "help.cli.analyze.example",
                    ".\\msa_cli.exe --no-config --input data.csv "
                    "--method crossed_anova --lsl 9 --usl 11 --report analysis.json",
                    c);
    });
    section(v, "help.cli.outputs", "文件路径与退出状态", cw, [&](float c) {
        helpItem(v, "help.cli.files", "选择输出位置",
                 "省略 --output 时按配置的目录、文件名模板和同名处理方式保存。显式 "
                 "--output 直接指定 .csv 或 .xlsx 路径。命令中的相对 --config、--input、--output "
                 "和 --report 路径从当前工作目录开始。配置中的相对输出目录从 Windows 系统桌面开始。",
                 c);
        helpText(v, "help.cli.report",
                 "--report 保存生效配置、测量矩阵和统计结果。生成模式同时记录实际种子。", c);
        helpItem(v, "help.cli.status", "覆盖文件与查看退出码",
                 "确认要替换显式指定的测量数据或报告时加 --overwrite。"
                 "生成过程中可按 Ctrl+C 取消。\n"
                 "0 · 成功。1 · 配置、生成或文件操作失败，或生成已取消。"
                 "2 · 参数错误或显式文件路径检查失败。",
                 c);
    });
}

void helpCharts(Ui &v, float cw) {
    section(v, "help.charts", "选择数据源", cw, [&](float c) {
        helpItem(v, "help.charts.source.current", "当前结果",
                 "先在「数据」页生成一组数据，再到「图表」选择「当前结果」。图表沿用这批数据的"
                 "研究配置与统计结果。",
                 c);
        helpItem(v, "help.charts.source.import", "导入数据",
                 "在「图表」点击「导入数据」，选择 CSV 文件或 XLSX 工作簿。带标签的宽表按首列识别"
                 "操作员，长表按 Operator 列识别。程序从所选数据识别操作员名称、人数、部件数和"
                 "重复次数。选项页保留原有的默认研究设计。"
                 "XLSX 只有一张数据工作表时直接读取该表。若有多张，弹窗按工作簿顺序列出数据工作表，"
                 "也可选择隐藏的数据工作表。选中一张后点击「导入」或按回车。取消会保留原数据和图表。"
                 "「选项 → 文件 → 目录与文件名」可设置导入目录和用于预填的「测量数据导入名称」。",
                 c);
        helpItem(v, "help.charts.source.order", "人员顺序与无标签宽表",
                 "宽表按操作员分组顺序显示。长表有 StdOrder 时按标准序号确定人员顺序，省略时"
                 "按名称首次出现的顺序排列。纯数值宽表按位置使用 A/B/C。遇到六行纯数值宽表，"
                 "弹窗会请你选择「2 人各测 3 次」或「3 人各测 2 次」，然后点击「导入」。"
                 "选择仅用于本次导入。取消或关闭弹窗会保留当前数据和图表。",
                 c);
        helpText(v, "help.charts.source.switch",
                 "导入成功后可在「当前结果」和「已导入数据」之间切换。修改分析选项后，点击"
                 "「按当前设置重算」重新分析原始记录。重算、图表和方法对比报告均沿用导入的操作员名称和人数。"
                 "「导出方法对比报告」可将当前数据源的"
                 " Xbar-R 与 ANOVA 结果保存为 PDF 或 HTML，默认选中 PDF。「选项 → 文件」中可设置"
                 "报告目录、文件名、默认格式和静默保存开关。",
                 c);
        button(v, "help.charts.open", "打开图表页", 152, [] { state.navigate(Page::Charts); });
    });
    section(v, "help.charts.view", "按目的选图", cw, [&](float c) {
        helpItem(v, "help.charts.view.parts", "查看部件间距和整批分布",
                 "用「测量值与部件」查看各部件的读数及均值。菱形表示部件均值，可用来比较"
                 "取样方案产生的跨度和间距。「测量值直方图」显示整批读数的频数。",
                 c);
        helpItem(v, "help.charts.view.variation", "查看变异来源和操作员差异",
                 "在「变异分量」查看量具和部件间变异，在「操作员分布」比较各操作员的箱线图"
                 "和测量点。「部件 × 操作员交互图」比较各操作员对同一部件的平均读数。",
                 c);
        helpItem(v, "help.charts.view.control", "查看均值和极差",
                 "「均值控制图」显示各重复测量子组的均值，「极差控制图」显示各子组的极差。"
                 "控制限的计算口径可在对应图表的说明中查看。",
                 c);
    });
    section(v, "help.charts.edit", "编辑并应用外观", cw, [&](float c) {
        helpItem(v, "help.charts.edit.fields", "编辑图表",
                 "选择图表后点击「编辑图表」。「坐标轴」可修改标题、轴名称、纵轴范围和主刻度目标间隔数"
                 "（2 至 10），数值留空时自动设定。宽窗可在预览旁编辑，窄窗可在图下编辑。",
                 c);
        helpItem(v, "help.charts.edit.symbols", "调整图形、图例与参考线",
                 "「图形」可更换主色。有数据点的图可调线宽、点大小和点形，「按系列区分」以点形和颜色"
                 "区分系列。直方图可指定 3 至 40 个等宽分箱，留空自动分箱。「图例与线」可将图例放在"
                 "绘图区上方或下方，也可切换网格、规格线或控制线。交互图和直方图默认聚焦实测范围，"
                 "可开启规格线以比较测量值与规格界限。控制图默认显示中心线和控制限。",
                 c);
        helpItem(v, "help.charts.edit.draft", "应用、撤销或重置",
                 "编辑时直接预览草稿，输入无效时显示已应用的图形并提示原因。每张图的草稿独立保存，"
                 "收起编辑器可查看已应用的图形。点击「应用编辑」即可按该样式导出。「撤销草稿」返回"
                 "上次应用的样式，「重置当前图表」恢复默认外观。",
                 c);
        helpText(v, "help.charts.edit.session",
                 "在「选项 → 图表」可选用与界面配色一致的默认风格，或沿用 Origin 风格。"
                 "每种风格各自保留七张图的已应用样式和草稿，重置当前或全部图表仅作用于选中的风格。"
                 "编辑内容在本次运行期间保留，方法对比报告使用导出时所选风格的控制图配色。",
                 c);
    });
    section(v, "help.charts.export", "导出单图或组合", cw, [&](float c) {
        helpItem(v, "help.charts.export.scope", "选图和版式",
                 "在「选项 → 图表 → 图表导出」选择「当前单图」或「多图组合」。组合可勾选至少一张图，按列表"
                 "顺序排列，支持单列或双列。返回图表页，应用或撤销待导出图表的草稿，点击底部「导出图表」。",
                 c);
        helpItem(v, "help.charts.export.files", "设置格式和保存位置",
                 "选择 PNG、JPEG、PDF 或 SVG 和单图宽度。「目录与文件名」可打开选项页修改图表名称和目录。"
                 "默认打开「另存为」。开启图表静默导出后直接保存，同名时追加序号。"
                 "保存后可点击「上次导出目录」查看文件。",
                 c);
        button(v, "help.charts.options", "前往图表选项", 160, openChartOptions);
        helpText(v, "help.charts.export.persist",
                 "点击选项页底部「保存配置」，下次启动会恢复风格、格式、宽度、组合范围、排版和勾选项。", c);
        helpText(v, "help.charts.export.pdf",
                 "PDF 将全部所选图表保存为一页，图形与中文文字一起绘制成高分辨率图像。SVG 可导出"
                 "单图或组合图，缩放后线条、数据点和文字仍清晰，适合排版或在矢量编辑器中继续编辑。", c);
    });
}

void helpPage(Ui &u, float w, float h) {
    static const std::vector<std::string> topics{
        "快速入门", "分析指标", "数据导出", "名称变量", "命令操作", "结果图表"};
    using Article = void (*)(Ui &, float);
    constexpr std::array<Article, 6> articles{
        helpQuickStart, helpAnalysis, helpCsv, helpFilename, helpCli, helpCharts};
    const int topic = std::clamp(state.helpTopic, 0, int(articles.size()) - 1);
    const auto changeTopic = [](int next) {
        if (state.helpTopic == next)
            return;
        state.helpTopic = next;
        state.helpOffset = 0;
        menu.id.clear();
    };
    constexpr float columnLimit = 940.f;
    const float readingWidth = std::min(w, columnLimit);
    const float cardWidth = readingWidth - tokens().metrics.control.scrollbar - scrollGutter;
    const bool compact = w < 700;
    u.column("help.layout")
        .size(w, h)
        .gap(sectionGap)
        .alignItems(eui::Align::CENTER)
        .content([&] {
            u.row("help.navigation.row")
                .width(readingWidth)
                .content([&] {
                    navigationCard(u, "help.navigation", "help.heading", cardWidth, [&] {
                        const float labelWidth = compact ? 60.f : 121.f;
                        const float topicsWidth =
                            cardWidth - labelWidth - secondaryNavGap - 2 * secondaryNavInset;
                        text(u, "help.title", compact ? "主题" : "帮助主题", labelWidth, 38, 16);
                        if (compact)
                            combo(u, "help.topics", topics, topic, topicsWidth, changeTopic);
                        else
                            tabs(u, "help.topics", topics, topic, topicsWidth, changeTopic, 38);
                    });
                })
                .build();
            components::scrollView(u, "helpscroll")
                .size(readingWidth, std::max(74.f, h - secondaryNavHeight - sectionGap))
                .offset(state.helpOffset)
                .theme(tokens())
                .scrollbarGap(scrollGutter)
                .reserveScrollbar()
                .onChange([](float offset) {
                    state.helpOffset = offset;
                    menu.id.clear();
                })
                .content([&](Ui &v, float cw, float) {
                    v.column("help.page")
                        .width(cw)
                        .gap(sectionGap)
                        .padding(0, 0, 0, 10)
                        .content([&] { articles[topic](v, cw); })
                        .build();
                })
                .build();
        })
        .build();
}
void appMark(Ui &u, const std::string &id, float size) {
    const float factor = size / 40;
    const auto snap = [factor](float value) {
        return std::round(value * factor * state.pixelScale) / state.pixelScale;
    };
    u.stack(id)
        .size(size, size)
        .content([&] {
            u.rect(id + ".bg").size(size, size).color(blue).radius(8.75f * factor).build();
            constexpr std::array<float, 3> barTop{22.5f, 15.f, 17.5f};
            for (int i = 0; i < 3; ++i) {
                const float x = 7.5f + i * 10, y = barTop[i];
                u.rect(id + ".bar." + std::to_string(i))
                    .position(snap(x), snap(y))
                    .size(snap(x + 5) - snap(x), snap(32.5f) - snap(y))
                    .radius(1.25f * factor)
                    .color(surface)
                    .build();
            }
            u.rect(id + ".sample")
                .position(snap(27.5f), snap(7.5f))
                .size(snap(32.5f) - snap(27.5f), snap(12.5f) - snap(7.5f))
                .radius(1.25f * factor)
                .color("#71F0D2")
                .build();
        })
        .build();
}
struct AboutAccessory {
    std::string label;
    std::function<void()> action{};
    bool disabled = false;
};
void aboutRow(Ui &u, const std::string &id, const std::string &symbol, const std::string &title,
              const std::string &value, const std::string &valueId, float w, bool divider,
              const AboutAccessory &accessory) {
    if (divider)
        u.rect(id + ".divider").size(w, 1).color(line).build();
    constexpr float iconSize = 36, gap = 12, accessoryWidth = 104;
    const float detailWidth = w - iconSize - accessoryWidth - 2 * gap;
    u.row(id)
        .width(w)
        .minHeight(78)
        .padding(0, 8)
        .gap(gap)
        .alignItems(eui::Align::CENTER)
        .content([&] {
            u.stack(id + ".icon")
                .size(iconSize, iconSize)
                .content([&] {
                    u.rect(id + ".icon.bg")
                        .size(iconSize, iconSize)
                        .radius(9)
                        .color(blueWash)
                        .border(1, brandBorder)
                        .build();
                    u.text(id + ".icon.text")
                        .size(iconSize, iconSize)
                        .text(symbol)
                        .fontSize(17)
                        .color(blue)
                        .horizontalAlign(eui::HorizontalAlign::Center)
                        .verticalAlign(eui::VerticalAlign::Center)
                        .build();
                })
                .build();
            u.column(id + ".details")
                .width(detailWidth)
                .gap(4)
                .content([&] {
                    paragraph(u, id + ".title", title, detailWidth, ink, 19);
                    paragraph(u, valueId, value, detailWidth, muted, 16);
                })
                .build();
            if (accessory.action)
                button(u, id + ".open", accessory.label, accessoryWidth, accessory.action, false,
                       accessory.disabled, 36);
            else
                badge(u, id + ".license", accessory.label, accessoryWidth, blue, blueWash);
        })
        .build();
}
void aboutGroup(Ui &u, const std::string &id, const std::string &title, float w,
                const std::function<void(float)> &content) {
    section(u, id, title, w,
            [&](float c) { u.column(id + ".rows").width(c).content([&] { content(c); }).build(); });
}
void aboutIdentity(Ui &u, float w) {
    section(u, "about.hero", "", w, [&](float c) {
        const bool inlineAction = c >= 460;
        const float mark = inlineAction ? 60.f : 48.f;
        constexpr float gap = 16, actionWidth = 140;
        const float titleWidth = c - mark - gap - (inlineAction ? actionWidth + gap : 0.f);
        const auto copyButton = [&] {
            button(u, "copyversion", state.versionCopied ? "已复制版本信息" : "复制版本信息",
                   actionWidth, [] {
                       safe([] {
                           platform::clipboard(platform::wide(
                               std::string("MSA 数据生成器 v") + MSA_UI_VERSION +
                               "\r\nWindows x64 / EUI-NEO 0.6.2"
                               "\r\n交叉量具 R&R：Xbar-R / ANOVA"
                               "\r\n作者：" +
                               msa::project_author + "\r\n项目仓库：" + msa::project_repository +
                               "\r\n项目许可：" + msa::project_license));
                           state.versionCopied = true;
                           state.hint("已复制版本信息");
                       });
                   });
        };
        u.row("about.identity")
            .width(c)
            .gap(gap)
            .alignItems(eui::Align::CENTER)
            .content([&] {
                appMark(u, "about.mark", mark);
                u.column("about.hero.copy")
                    .width(titleWidth)
                    .gap(5)
                    .content([&] {
                        text(u, "about.title", "MSA 数据生成器", titleWidth, 32, 22);
                        paragraph(u, "about.purpose", "交叉量具 R&R 模拟数据生成与统计复核",
                                  titleWidth, muted, 17);
                        u.flow("about.hero.meta")
                            .width(titleWidth)
                            .gap(8)
                            .lineGap(6)
                            .margin(0, 3, 0, 0)
                            .content([&] {
                                badge(u, "about.version", std::string("v") + MSA_UI_VERSION, 82,
                                      blue, blueWash);
                                badge(u, "about.platform", "Windows x64", 112, muted,
                                      surfaceDisabled);
                            })
                            .build();
                    })
                    .build();
                if (inlineAction)
                    copyButton();
            })
            .build();
        if (!inlineAction)
            u.row("about.hero.action")
                .width(c)
                .justifyContent(eui::Align::END)
                .content(copyButton)
                .build();
    });
}
void aboutEnvironment(Ui &u, float w) {
    section(u, "about.environment", "运行环境", w, [&](float c) {
        const std::array<const char *, 4> ids{"about.build", "about.graphics", "about.dpi",
                                              "about.scale"};
        const std::array<const char *, 4> labels{"构建标准", "图形接口", "窗口 DPI", "界面缩放"};
        const std::array<std::string, 4> values{"C++17", "OpenGL 3.3",
                                                std::to_string(state.systemDpi),
                                                number(state.pixelScale * 100, 0) + "%"};
        const int columns = c >= 456 ? 4 : 2;
        constexpr float gap = 12;
        const float cell = (c - (columns - 1) * gap) / columns;
        for (int start = 0; start < int(ids.size()); start += columns)
            u.row("about.environment.row." + std::to_string(start))
                .width(c)
                .gap(gap)
                .content([&] {
                    for (int i = start; i < start + columns; ++i)
                        u.stack(std::string(ids[i]) + ".tile")
                            .size(cell, 64)
                            .content([&] {
                                u.rect(std::string(ids[i]) + ".surface")
                                    .size(cell, 64)
                                    .color(surfaceSoft)
                                    .border(1, line)
                                    .radius(controlRadius)
                                    .build();
                                u.column(std::string(ids[i]) + ".caption")
                                    .width(cell)
                                    .padding(12, 8)
                                    .gap(2)
                                    .content([&] {
                                        text(u, std::string(ids[i]) + ".label", labels[i],
                                             cell - 24, 19, 11, muted);
                                        text(u, ids[i], values[i], cell - 24, 27, 15);
                                    })
                                    .build();
                            })
                            .build();
                })
                .build();
    });
}
void aboutPage(Ui &u, float w, float h) {
    const auto licenseDir = platform::executableDirectory() / "licenses";
    std::error_code licenseError;
    const bool licenseAvailable = std::filesystem::is_directory(licenseDir, licenseError);
    const float viewportWidth =
        std::min(w, 1100.f + tokens().metrics.control.scrollbar + scrollGutter);
    u.row("about.center")
        .size(w, h)
        .justifyContent(eui::Align::CENTER)
        .content([&] {
            components::scrollView(u, "aboutscroll")
                .size(viewportWidth, h)
                .offset(state.aboutOffset)
                .theme(tokens())
                .scrollbarGap(scrollGutter)
                .reserveScrollbar()
                .onChange([](float offset) {
                    state.aboutOffset = offset;
                    menu.id.clear();
                })
                .content([licenseDir, licenseAvailable](Ui &v, float cw, float) {
                    const auto project = [&](Ui &v, float width) {
                        aboutGroup(v, "about.project", "项目信息", width, [&](float c) {
                            aboutRow(v, "about.author.row", "@", "作者", msa::project_author,
                                     "about.author", c, false,
                                     {"访问主页", [] {
                                          safe([] {
                                              platform::openWebsite(
                                                  L"https://github.com/print-yuhuan");
                                              state.hint("已打开作者主页");
                                          });
                                      }});
                            aboutRow(v, "about.repository.row", "↗", "项目仓库",
                                     msa::project_repository, "about.repository", c, true,
                                     {"访问仓库", [] {
                                          safe([] {
                                              platform::openWebsite(
                                                  platform::wide(msa::project_repository));
                                              state.hint("已打开项目仓库");
                                          });
                                      }});
                            std::string description =
                                "允许商业使用、修改和分发。\n"
                                "分发副本或实质部分时须保留版权声明与许可声明。";
                            if (!licenseAvailable)
                                description += "\n许可全文随便携运行包提供。";
                            aboutRow(v, "about.license.row", "§", msa::project_license, description,
                                     "about.projectlicense", c, true,
                                     {"许可目录",
                                      [licenseDir] {
                                          safe([licenseDir] {
                                              platform::openDirectory(licenseDir);
                                              state.hint("已打开许可目录");
                                          });
                                      },
                                      !licenseAvailable});
                        });
                    };
                    const auto dependencies = [](Ui &v, float width) {
                        aboutGroup(v, "about.dependencies", "第三方组件", width, [&](float c) {
                            aboutRow(v, "about.framework", "UI", "EUI-NEO", "界面框架 · 0.6.2",
                                     "about.euiversion", c, false, {"Apache-2.0"});
                            aboutRow(v, "about.font", "Aa", "Noto Sans CJK SC", "中文字体 · 2.004",
                                     "about.fontlicense", c, true, {"OFL-1.1"});
                            aboutRow(v, "about.json", "{}", "nlohmann/json", "配置与报告 · 3.12.0",
                                     "about.jsonlicense", c, true, {"MIT"});
                        });
                    };
                    v.column("about.page")
                        .width(cw)
                        .gap(16)
                        .padding(0, 0, 0, 12)
                        .content([&] {
                            aboutIdentity(v, cw);
                            pairedCards(v, "about.details", cw, "about.project", project,
                                        "about.dependencies", dependencies, 860);
                            aboutEnvironment(v, cw);
                        })
                        .build();
                })
                .build();
        })
        .build();
}
void footerStatus(Ui &u, float w, const std::string &fallback) {
    // 固定提示槽仅更新文字和颜色，不改变底栏尺寸或按钮位置。
    const bool showHint = state.visibleHint();
    auto status = u.text("footer.status")
                      .size(w, controlHeight)
                      .text(showHint ? state.footerMessage : fallback)
                      .fontSize(16.5f)
                      .color(!showHint                          ? muted
                             : state.footerLevel == Level::Info ? green
                                                                : levelColor(state.footerLevel))
                      .verticalAlign(eui::VerticalAlign::Center)
                      .clip();
    if (showHint)
        status.onFrame([](float) {});
    status.build();
}
// 新页面共享现有主题、状态栏与本地统计结果。
#include "gui_visualization.hpp"
#include "gui_status.hpp"
void footer(Ui &u, float w) {
    const bool compact = w < 760;
    if (state.page == Page::Charts && !state.busy()) {
        const float applyWidth = compact ? 110.f : 132.f, exportWidth = compact ? 124.f : 152.f;
        const float settingsWidth = compact ? 96.f : 112.f;
        footerStatus(u, w - settingsWidth - applyWidth - exportWidth - 36, chartFooterSummary());
        button(u, "charts.options", "图表选项", settingsWidth, openChartOptions);
        button(u, "charts.apply", "应用编辑", applyWidth, [] { applyChartEdits(visual.selected); },
               false, visual.busy() || !insightResult() || !chartEdits().editDirty[visual.selected]);
        button(u, "charts.export", visual.busy() && !visual.exportingReport ? "导出中…" : "导出图表", exportWidth,
               exportVisuals, true, visual.busy() || !insightResult() || selectedChartCount() == 0);
        return;
    }
    if (state.page == Page::Data) {
        const std::string fallback =
            state.busy()
                ? (w >= 760 ? "已搜索候选 " + std::to_string(state.ticks.load()) + " / " +
                                  state.draft.attempts + " · " + number(state.elapsed) + " 秒"
                            : std::to_string(state.ticks.load()) + " 次 · " +
                                  number(state.elapsed) + " 秒")
            : !state.dataExportError.empty() ? "测量数据导出失败，请查看页面提示"
            : state.retainedResult() ? "保留上次成功结果，可复核或导出"
            : !state.message.empty() ? "操作失败，请查看页面提示"
            : state.validResult()    ? "数据已生成，可复核并导出数据"
            : state.result && state.resultRevision != state.revision ? "参数已修改，请重新生成"
            : "准备就绪";
        const float generateWidth = compact ? 112.f : 146.f, exportWidth = compact ? 124.f : 158.f;
        footerStatus(u, w - generateWidth - exportWidth - 24, fallback);
        button(
            u, "generate",
            state.status == Status::Cancelling ? "取消中…"
            : state.busy()                     ? "取消生成"
            : state.validResult()              ? "重新生成"
                                               : "生成数据",
            generateWidth,
            [cancelAction = state.busy()] {
                if (cancelAction)
                    state.requestCancel();
                else if (!state.busy())
                    state.start();
            },
            state.busy() || !state.validResult(), state.status == Status::Cancelling);
        button(
            u, "export", "导出数据", exportWidth, [] { exportData(); }, state.validResult(),
            !state.validResult());
    } else if (state.busy()) {
        footerStatus(u, w - 284, "生成中 · " + number(state.elapsed) + " 秒");
        button(u, "showtask", "返回数据", 140, [] { state.navigate(Page::Data); });
        button(
            u, "cancel", state.status == Status::Cancelling ? "取消中…" : "取消", 120,
            [] { state.requestCancel(); }, true, state.status == Status::Cancelling);
    } else if (state.page == Page::Options) {
        const float resetWidth = compact ? 108.f : 124.f, saveWidth = compact ? 122.f : 148.f;
        footerStatus(u, w - resetWidth - saveWidth - 24,
                     state.configDirty ? "配置有未保存的更改" : "配置已保存");
        button(u, "defaults", "恢复默认", resetWidth, [] {
            state.draft = Draft{};
            state.changed();
            state.status = Status::Ready;
            state.message.clear();
            state.feedback("已恢复默认 · 尚未保存", "已恢复默认设置。点击「保存配置」可供下次启动使用。");
            state.configOffset = 0;
        });
        button(u, "saveconfig", "保存配置", saveWidth, [] { saveSettings(); }, true);
    } else {
        footerStatus(u, w - 132, std::string("MSA 数据生成器 · v") + MSA_UI_VERSION);
        button(u, "returntask", "返回数据", 120, [] { state.navigate(Page::Data); });
    }
}
void compose(Ui &u, const eui::Screen &screen) {
    state.poll();
    pollVisuals();
    state.systemDpi = GetDpiForWindow(platform::owner());
    RECT physical{};
    GetClientRect(platform::owner(), &physical);
    if (physical.right > 0 && screen.width > 0)
        state.pixelScale = physical.right / screen.width;
    static Page previousPage = Page::Data;
    if (state.previousWidth != screen.width || state.previousHeight != screen.height ||
        previousPage != state.page) {
        menu.id.clear();
        state.previousWidth = screen.width;
        state.previousHeight = screen.height;
        previousPage = state.page;
    }
    if (std::fabs(state.limitScale - state.pixelScale) > .01f) {
        state.limitScale = state.pixelScale;
        MONITORINFO monitor{sizeof(MONITORINFO)};
        RECT outer{};
        int minWidth = int(620 * state.pixelScale), minHeight = int(500 * state.pixelScale);
        if (GetMonitorInfoW(MonitorFromWindow(platform::owner(), MONITOR_DEFAULTTONEAREST),
                            &monitor) &&
            GetWindowRect(platform::owner(), &outer)) {
            minWidth =
                std::min(minWidth,
                         int(std::max(160L, monitor.rcWork.right - monitor.rcWork.left -
                                                (outer.right - outer.left - physical.right) - 32)));
            minHeight = std::min(
                minHeight,
                int(std::max(160L, monitor.rcWork.bottom - monitor.rcWork.top -
                                       (outer.bottom - outer.top - physical.bottom) - 32)));
        }
        glfwSetWindowSizeLimits(glfwGetCurrentContext(), minWidth, minHeight, GLFW_DONT_CARE,
                                GLFW_DONT_CARE);
    }
    const bool compact = screen.width < 760;
    const float pad = screen.width < 1120 ? 16.0f : 24.0f, c = screen.width - pad * 2;
    const float headerHeight = compact ? 56.f : 64.f, navHeight = 48.f;
    const float footerHeight = compact ? 56.f : 60.f, bodyInset = compact ? 8.f : 12.f;
    const float h = std::max(120.0f, screen.height - headerHeight - navHeight - footerHeight -
                                       bodyInset);
    u.rect("header.surface")
        .size(screen.width, headerHeight + navHeight)
        .color(headerSurface)
        .build();
    u.rect("header.divider")
        .position(0, headerHeight + navHeight - 1)
        .size(screen.width, 1)
        .color(line)
        .build();
    u.rect("footer.surface")
        .position(0, screen.height - footerHeight)
        .size(screen.width, footerHeight)
        .color(surface)
        .build();
    u.rect("footer.divider")
        .position(0, screen.height - footerHeight)
        .size(screen.width, 1)
        .color(line)
        .build();
    u.column("shell")
        .size(screen.width, screen.height)
        .content([&] {
            u.row("header")
                .size(screen.width, headerHeight)
                .padding(pad, 0)
                .gap(12)
                .alignItems(eui::Align::CENTER)
                .content([&] {
                    u.row("brandgroup")
                        .size(compact ? 236.f : 282.f, 42)
                        .gap(11)
                        .alignItems(eui::Align::CENTER)
                        .content([&] {
                            appMark(u, "brand.mark", compact ? 34.f : 38.f);
                            u.text("brand")
                                .height(42)
                                .text("MSA")
                                .fontSize(compact ? 24.f : 27.f)
                                .color(blue)
                                .verticalAlign(eui::VerticalAlign::Center)
                                .build();
                            u.text("title")
                                .height(42)
                                .text("数据生成器")
                                .fontSize(compact ? 21.f : 24.f)
                                .color(ink)
                                .verticalAlign(eui::VerticalAlign::Center)
                                .build();
                        })
                        .build();
                    u.rect("header.spacer")
                        .width(eui::SizeValue::fill())
                        .height(1)
                        .color("#00000000")
                        .build();
                    headerStatusIndicator(u, headerStatusPresentation());
                })
                .build();
            u.row("navigation")
                .size(screen.width, navHeight)
                .padding(pad, 3)
                .gap(8)
                .content([&] {
                    tabs(
                        u, "pages",
                        {"数据", "图表", "日志", "选项", "帮助", "关于"},
                        static_cast<int>(state.page), std::min(c, 684.0f),
                        [](int i) {
                            state.navigate(static_cast<Page>(i));
                            menu.id.clear();
                        },
                        38);
                })
                .build();
            u.rect("body.inset").size(screen.width, bodyInset).color(bg).build();
            auto body = u.column("body").size(screen.width, h).padding(pad, 0);
            // 正文始终清晰；过渡仅作用于导航和控件状态。
            if (visual.busy())
                body.onFrame([](float) {});
            body.content([&] {
                    switch (state.page) {
                    case Page::Data:
                        dataPage(u, c, h);
                        break;
                    case Page::Charts:
                        chartsPage(u, c, h);
                        break;
                    case Page::Logs:
                        logsPage(u, c, h);
                        break;
                    case Page::Options:
                        optionsPage(u, c, h);
                        break;
                    case Page::Help:
                        helpPage(u, c, h);
                        break;
                    case Page::About:
                        aboutPage(u, c, h);
                        break;
                    }
                })
                .build();
            u.row("footer")
                .size(screen.width, footerHeight)
                .padding(pad, 0)
                .gap(12)
                .alignItems(eui::Align::CENTER)
                .justifyContent(state.page == Page::Data ? eui::Align::END : eui::Align::START)
                .content([&] { footer(u, c); })
                .build();
        })
        .build();
    menuOverlay(u, screen);
}
} // namespace gui
namespace app {
const DslAppConfig &dslAppConfig() {
    static const DslAppConfig config = [] {
        // 无状态解码与图形初始化并行，FreeType操作仅在UI线程执行。
        (void)gui::fontLoad();
        float scale = 1;
        wchar_t env[32]{};
        if (GetEnvironmentVariableW(L"MSA_UI_SCALE", env, 32))
            scale = std::clamp(float(_wtof(env)), 1.0f, 2.0f);
        RECT a{0, 0, 1280, 960};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &a, 0);
        const UINT dpi = GetDpiForSystem();
        const float effectiveScale = dpi / 96.0f * scale;
        const int frameWidth = 2 * (GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) +
                                    GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi));
        const int frameHeight = 2 * (GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) +
                                     GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi)) +
                                GetSystemMetricsForDpi(SM_CYCAPTION, dpi);
        auto [w, h] = native_ui::initial_window_size(a.right - a.left, a.bottom - a.top,
                                                     effectiveScale, frameWidth, frameHeight);
        wchar_t ew[32]{}, eh[32]{};
        if (GetEnvironmentVariableW(L"MSA_UI_WIDTH", ew, 32))
            w = _wtoi(ew);
        if (GetEnvironmentVariableW(L"MSA_UI_HEIGHT", eh, 32))
            h = _wtoi(eh);
        w = std::clamp(w, 160, std::max(160, int(a.right - a.left) - frameWidth - 32));
        h = std::clamp(h, 160, std::max(160, int(a.bottom - a.top) - frameHeight - 32));
        return DslAppConfig{}
            .title(std::string("MSA 数据生成器 · v") + MSA_UI_VERSION)
            // Use the embedded, multi-size Windows icon for both small and large
            // shell surfaces; a single PNG would override it with scaled pixels.
            .iconPath("")
            .windowSize(w, h)
            .minWindowSize(std::min(w, int(620 * effectiveScale)),
                           std::min(h, int(500 * effectiveScale)))
            .highDpi()
            .uiScale(scale)
            .fonts(gui::font(), gui::font())
            .clearColor(eui::Color(gui::bg))
            .fps(60)
            .showDebugOverlay(false)
            .showDebugStatsInTitle(false)
            .onStart([] {
                // 首次显示前按实际外框居中，工作区不包含任务栏。
                RECT workArea{}, windowRect{};
                const HWND window = platform::owner();
                // Windows 10 不支持标题栏颜色属性，调用失败时使用系统配色。
                if (SUCCEEDED(DwmSetWindowAttribute(window, DWMWA_CAPTION_COLOR,
                                                    &gui::headerBackground,
                                                    sizeof(gui::headerBackground)))) {
                    constexpr COLORREF captionText = RGB(24, 44, 69);
                    DwmSetWindowAttribute(window, DWMWA_TEXT_COLOR, &captionText,
                                          sizeof(captionText));
                }
                if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0) &&
                    GetWindowRect(window, &windowRect)) {
                    const int x =
                        workArea.left + std::max(0L, (workArea.right - workArea.left -
                                                      (windowRect.right - windowRect.left)) /
                                                         2);
                    const int y =
                        workArea.top + std::max(0L, (workArea.bottom - workArea.top -
                                                     (windowRect.bottom - windowRect.top)) /
                                                        2);
                    SetWindowPos(window, nullptr, x, y, 0, 0,
                                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                }
                gui::initializeFont();
                CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                BOOL animations = TRUE;
                if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0))
                    gui::state.motion = animations != FALSE;
                gui::state.systemDpi = GetDpiForWindow(platform::owner());
                gui::state.loadInitial();
            })
            .onShutdown([] {
                gui::stopVisuals();
                gui::state.stop();
                CoUninitialize();
            })
            .onKeyEvent([](const eui::KeyEvent &k) {
                if (k.isDown() && k.key == eui::InputKey::Tab)
                    gui::menu.id.clear();
                if (k.isDown() && k.key == eui::InputKey::Escape) {
                    gui::menu.id.clear();
                }
            });
    }();
    return config;
}
void compose(eui::Ui &u, const eui::Screen &s) { gui::compose(u, s); }
} // namespace app
