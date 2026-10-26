// Native MSA application: EUI-NEO presentation; shared, GUI-free business kernel.
#include "eui_neo.h"
#include "core/render/text.h"
#include "nlohmann/json.hpp"
#include "platform_win.hpp"
#include "platform/embedded_resources_win.hpp"
#include "ui_model.hpp"
#include "msa/task.hpp"
#include "msa/project.hpp"
#include "msa/ai.hpp"
#include "msa/charts.hpp"
#include "platform/chart_export.hpp"
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
constexpr const char *ink = "#20334B", *muted = "#64748B", *blue = "#3B65E8", *line = "#DFE6F0",
                     *bg = "#F3F5FA", *green = "#16866B";
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
    SearchFailed,
    ExportError
};
enum class Page { Data, Charts, Assistant, Logs, Options, Help, About };
enum class Level { Info, Warning, Error };
struct LogEntry {
    unsigned sequence;
    std::string time;
    Level level;
    std::string message;
};
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
    Clock::time_point pageEntered = Clock::now(), resultEntered = Clock::now(),
                      advancedChanged = Clock::now();
    float advancedFrom = 0;
    float generateOffset = 0, configOffset = 0, pixelScale = 1;
    float previousWidth = 0, previousHeight = 0, limitScale = 0;
    unsigned systemDpi = 96;
    std::string message, footerMessage;
    float logOffset = 0, helpOffset = 0, aboutOffset = 0;
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
    State() { log("程序已就绪。支持交叉量具 R&R：均值极差法与方差分析法。"); }
    ~State() { stop(); }
    void stop() {
        cancel = true;
        task.cancel();
        if (worker.joinable())
            worker.join();
        task.stop();
    }
    bool busy() const { return status == Status::Running || status == Status::Cancelling; }
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
        pageEntered = Clock::now();
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
        if (logs.size() >= 100)
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
        configDirty = true;
        if (!busy())
            status = Status::Ready;
        ++revision;
        resultTab = 0;
        previewPage = 0;
        if (invalidate)
            feedback("输入已修改，请重新生成", "输入已修改，请重新生成；旧结果已失效。");
    }
    void outputChanged() {
        footerMessage.clear();
        configDirty = true;
        if (status == Status::ExportError) {
            message.clear();
            status = result && resultRevision == revision ? Status::Success : Status::Ready;
        }
    }
    void sample() {
        const int method = draft.analysisMethod, interaction = draft.interactionMode;
        const std::string alpha = draft.interactionAlpha;
        const auto previous = draft;
        draft = Draft{};
        // 示例只替换研究参数；用户的导出路径、格式和运行顺序保持不变。
        draft.directory = previous.directory;
        draft.filename = previous.filename;
        draft.existingFile = previous.existingFile;
        draft.csvFormat = previous.csvFormat;
        draft.runMode = previous.runMode;
        draft.randomOperators = previous.randomOperators;
        draft.storeStandardOrder = previous.storeStandardOrder;
        draft.runSeed = previous.runSeed;
        draft.analysisMethod = method;
        draft.interactionMode = interaction;
        draft.interactionAlpha = alpha;
        draft.lsl = "9.5";
        draft.usl = "10.5";
        draft.targets[0] = {1, "10", "0.1"};
        draft.targets[1] = {3, "10", ""};
        draft.targets[2] = {2, "10", "20"};
        draft.targets[3] = {3, "20", ""};
        draft.targets[4] = {4, "5", ""};
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
                log("已载入用户配置（未写入）：" + path.u8string());
            } else
                log("用户配置尚不存在，使用内存默认值；保存配置时可创建。");
        } catch (const std::exception &e) {
            message = std::string("配置载入失败，原文件保持不变：") + e.what();
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
        footerMessage.clear();
        try {
            const auto snapshot = msa::validate_request(native_ui::to_config(draft));
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
        log(draft.analysisMethod ? "开始生成 ANOVA 模拟测量数据，使用启动任务时的配置。"
                                 : "开始生成 Xbar-R 模拟测量数据，使用启动任务时的配置。");
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
                     displayedResult() ? "本次生成已取消；上次同配置结果仍可复核、导出。"
                                       : "已取消，可以再次生成。",
                     Level::Warning);
        } else if (taskRevision != revision) {
            status = Status::Ready;
            feedback("输入已修改，请重新生成", "输入已变更，已丢弃过期任务。");
        } else if (v.status == msa::TaskStatus::Success && v.result) {
            result = std::move(v.result);
            status = Status::Success;
            resultRevision = revision;
            feedback("数据已生成", "已找到满足生成约束的数据；尝试 " +
                                       std::to_string(result->attempts) + " 次，种子 " +
                                       std::to_string(result->seed) + "。可导出 CSV 复验。");
        } else {
            status = v.status == msa::TaskStatus::Infeasible     ? Status::Infeasible
                     : v.status == msa::TaskStatus::SearchFailed ? Status::SearchFailed
                                                                 : Status::Error;
            message = v.error;
            feedback("生成失败，请查看日志", message, Level::Error);
        }
    }

} state;
void aiSettings(Ui &, float);
void saveAiSettings();
void resetAiSettings();
bool aiSettingsDirty();
bool aiNetworkBusy();
void loadExtensions();
void pollExtensions();
void stopExtensions();
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
    t.primary = eui::Color(blue);
    t.background = eui::Color(bg);
    t.surface = eui::Color("#F6F8FD");
    t.surfaceHover = eui::Color("#EDF2FC");
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
        if (u.isFocused(id))
            hit->border = {2, eui::Color(blue)};
    }
}
bool activate(const eui::KeyEvent &key) {
    return key.isDown() && (key.key == eui::InputKey::Enter || key.key == eui::InputKey::Space);
}
void segments(Ui &u, const std::string &id, const std::vector<std::string> &items, int selected,
              float w, std::function<void(int)> select, float h = 40, bool soft = false) {
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
        style.selected = eui::Color("#FFFFFF");
        style.selectedText = eui::Color(blue);
    }
    components::segmented(u, id)
        .size(w, h)
        .items(items)
        .selected(selected)
        .theme(tokens())
        .style(style)
        .fontSize(18)
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
          float w, std::function<void(int)> select, float h = 40) {
    auto &choice = u.state<int>(id + ".choice");
    choice = selected;
    select = [&u, id, &choice, callback = std::move(select)](int next) {
        if (next != choice) {
            choice = next;
            callback(next);
        }
        u.requestFocus(id + ".hit." + std::to_string(next));
    };
    components::tabs(u, id)
        .size(w, h)
        .items(items)
        .selected(selected)
        .fontSize(19)
        .theme(tokens())
        .transition(transition())
        .onChange(select)
        .build();
    for (int i = 0; i < int(items.size()); ++i) {
        const auto hitId = id + ".hit." + std::to_string(i);
        if (auto *hit = u.find(hitId)) {
            hit->hoverColor = eui::Color("#EAF0FC");
            hit->pressedColor = eui::Color("#DFE8FB");
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
        style.text = eui::Color("#7B899D");
        style.rowHover = style.rowPressed = eui::Color("#00000000");
    }
    components::toggleSwitch(u, id)
        .size(w, 40)
        .checked(value)
        .text(label)
        .fontSize(18)
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
void badge(Ui &u, const std::string &id, const std::string &label, float w,
           const char *color = blue, const char *surface = "#EDF2FF") {
    u.stack(id)
        .size(w, 28)
        .content([&] {
            u.rect(id + ".bg").size(w, 28).color(surface).radius(14).build();
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
            bool primary = false, bool disabled = false, float h = 40) {
    const bool focus = u.isFocused(id + ".bg");
    components::button(u, id)
        .size(w, h)
        .text(s)
        .fontSize(18)
        .theme(tokens(), primary)
        .colors(disabled  ? "#EEF1F6"
                : primary ? blue
                          : "#FFFFFF",
                primary ? "#3056D0" : "#EFF3FD", primary ? "#2848B5" : "#E4EBFC")
        .textColor(disabled  ? "#9AA6B8"
                   : primary ? "#FFFFFF"
                             : ink)
        .radius(9)
        .border(focus ? 2.0f : 1.0f, focus     ? blue
                                     : primary ? blue
                                               : line)
        .shadow(primary && !disabled ? 10.0f : 0.0f, 0, 3, "#3B65E81A")
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
        minimum = std::max(minimum, core::TextPrimitive::measureTextWidth(item, {}, 18, 400) + 36);
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
                            const bool focus = u.isFocused(itemId + ".bg");
                            components::button(u, itemId)
                                .size(cell, 36)
                                .text(items[i])
                                .fontSize(18)
                                .theme(tokens())
                                .colors(disabled ? "#F1F3F7"
                                        : active ? "#EDF2FF"
                                                 : "#FFFFFF",
                                        "#E5EDFE", "#DCE6FC")
                                .textColor(disabled ? "#7B899D"
                                           : active ? blue
                                                    : ink)
                                .radius(8)
                                .border(focus ? 2.f : 1.f, focus || active ? blue : line)
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
        style.text = eui::Color("#7B899D");
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
        .fontSize(18)
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
        .size(w, 40)
        .content([&] {
            u.rect(id)
                .size(w, 40)
                .states("#FFFFFF", "#F5F8FF", "#EAF1FD")
                .border(f ? 2.0f : 1.0f, f ? blue : line)
                .radius(9)
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
                    if (k.key == eui::InputKey::Enter || k.key == eui::InputKey::Space) {
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
                .size(w, 40)
                .padding(10)
                .alignItems(eui::Align::CENTER)
                .content([&] {
                    u.text(id + ".value")
                        .size(w - 40, 20)
                        .text(items.empty() ? "暂无可选项" : items.at(selected))
                        .fontSize(14 * 1.35f)
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
void input(Ui &u, const std::string &id, std::string &value, float w, const std::string &hint = "",
           bool exportOnly = false) {
    components::input(u, id)
        .size(w, 40)
        .value(value)
        .placeholder(hint)
        .fontSize(20)
        .inset(10)
        .theme(tokens())
        .transition(transition(.14f))
        .onChange([&value, exportOnly](const std::string &s) {
            if (value == s)
                return;
            value = s;
            if (exportOnly)
                state.outputChanged();
            else
                state.changed();
        })
        .build();
}
void field(Ui &u, const std::string &id, const std::string &label, std::string &v, float w,
           const std::string &hint = "", bool exportOnly = false) {
    u.column(id + ".field")
        .width(w)
        .gap(7)
        .content([&] {
            text(u, id + ".label", label, w, 22, 14);
            input(u, id, v, w, hint, exportOnly);
        })
        .build();
}
eui::Shadow cardShadow() {
    eui::Shadow shadow;
    shadow.enabled = true;
    shadow.offset = {0, 3};
    shadow.blur = 16;
    shadow.color = eui::Color("#233B6409");
    return shadow;
}
void section(Ui &u, const std::string &id, const std::string &title, float w,
             std::function<void(float)> fn) {
    const bool compact = id == "rules" || id == "targets" || id == "results";
    const float inset = compact ? 16.0f : 20.0f;
    components::card(u, id + ".card")
        .width(w)
        .wrapContentHeight()
        .theme(tokens())
        .padding(0)
        .color("#FFFFFF")
        .radius(14)
        .border(1, line)
        .shadow(cardShadow())
        .content([&] {
            u.column(id)
                .width(w)
                .padding(inset, id == "summary" ? 12.0f : inset)
                .gap(id == "summary" ? 4.0f
                     : compact       ? 10.0f
                                     : 15.0f)
                .content([&] {
                    if (id != "summary" && !title.empty())
                        u.row(id + ".heading")
                            .width(w - inset * 2)
                            .gap(10)
                            .alignItems(eui::Align::CENTER)
                            .content([&] {
                                u.rect(id + ".accent").size(3, 18).color(blue).radius(1.5f).build();
                                text(u, id + ".title", title, w - inset * 2 - 13, 25, 17);
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
    if (state.message.empty())
        return;
    u.stack("notice.card")
        .width(w)
        .content([&] {
            u.rect("notice.bg")
                .width(eui::SizeValue::fill())
                .height(eui::SizeValue::fill())
                .ignoreLayout()
                .color("#FFF0ED")
                .radius(7)
                .build();
            u.row("notice.row")
                .width(w)
                .padding(12, 8)
                .gap(12)
                .alignItems(eui::Align::CENTER)
                .content([&] {
                    const bool offerOptions = state.page != Page::Options;
                    const float actionWidth = offerOptions ? 168.0f : 84.0f;
                    u.text("notice.text")
                        .width(w - 24 - actionWidth)
                        .text(state.message)
                        .fontSize(18)
                        .color("#AF3525")
                        .wrap()
                        .maxWidth(w - 24 - actionWidth)
                        .build();
                    if (offerOptions)
                        button(
                            u, "fixoptions", "选项", 72,
                            [] {
                                state.navigate(Page::Options);
                                state.configOffset = 0;
                            },
                            false, state.busy(), 32);
                    button(
                        u, "showlogs", "日志", 72, [] { state.navigate(Page::Logs); }, false, false,
                        32);
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
        const float centerWidth = c * .40f, choiceWidth = c - centerWidth - 12;
        u.row("rulerow")
            .width(c)
            .gap(12)
            .content([&] {
                field(u, "center", "数据中心（可留空）", state.draft.center, centerWidth,
                      "自动确定中心");
                u.column("prioritycol")
                    .width(choiceWidth)
                    .gap(7)
                    .content([&] {
                        text(u, "prioritylabel", "生成优先级", choiceWidth, 22, 14);
                        segments(u, "priority", {"中心优先", "指标优先"}, state.draft.priority,
                                 choiceWidth, [](int i) {
                                     state.draft.priority = i;
                                     state.changed();
                                 });
                    })
                    .build();
            })
            .build();
        u.row("strictrow")
            .width(c)
            .gap(8)
            .alignItems(eui::Align::CENTER)
            .content([&] {
                toggle(u, "compliance", "规格内生成", state.draft.compliance == 0, 160,
                       [](bool strict) {
                           state.draft.compliance = strict ? 0 : 1;
                           state.changed();
                       });
                text(u, "stricthint",
                     state.draft.compliance == 0 ? "所有测量值均满足规格限" : "允许数据超出规格限",
                     c - 168, 28, 12, muted);
            })
            .build();
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
                        .size(c, 64)
                        .gap(12)
                        .alignItems(eui::Align::CENTER)
                        .content([&] {
                            text(u, id + ".name", metricNames[i], labelWidth, 40, 14);
                            combo(u, id + ".mode", modes, t.mode, modeWidth, [i](int mode) {
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
                                        const std::string unit = i == 4 ? "" : " (%)";
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
                                                t.second, f,
                                                t.mode == 1 ? (i == 4 ? state.draft.ndcError
                                                                      : state.draft.percentError)
                                                            : "");
                                    })
                                    .build();
                            } else
                                text(u, id + ".unconstrained", "不限制此指标", valueWidth, 56, 13,
                                     muted);
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
               display(t.second.empty() ? (i == 4 ? state.draft.ndcError : state.draft.percentError)
                                        : t.second);
    if (t.mode == 2)
        return first + " ～ " + display(t.second);
    return std::string(t.mode == 3 ? "≤ " : "≥ ") + first;
}
std::string boundText(double v, int dp) { return std::isfinite(v) ? msa::fixed(v, dp) : "∞"; }
void review(Ui &u, float w) {
    const bool hasResult = state.displayedResult();
    Json cfg;
    try {
        cfg = hasResult ? state.result->config : native_ui::to_config(state.draft);
    } catch (const std::exception &) {
        cfg = nullptr;
    }
    const Json m = hasResult ? state.result->metrics : Json::object();
    const Json status = hasResult ? msa::checks(m, cfg) : Json::object();
    bool detail = w >= 610;
    std::array<float, 6> ratios{.15f, .22f, .20f, .14f, .12f, .17f};
    auto row = [&](std::string id, std::array<std::string, 6> cells, bool header) {
        u.stack(id + ".wrap")
            .size(w, 44)
            .content([&] {
                u.rect(id + ".bg")
                    .size(w, 44)
                    .color(header ? "#F2F5FB" : (id.back() % 2 ? "#FAFBFE" : "#FFFFFF"))
                    .radius(header ? 8.f : 0.f)
                    .build();
                u.row(id)
                    .size(w, 44)
                    .padding(8)
                    .alignItems(eui::Align::CENTER)
                    .content([&] {
                        for (int j = 0; j < 6; ++j) {
                            const float width = (w - 16) * ratios[j];
                            if (j == 5 && !header && cells[j] != "—") {
                                const bool ok = cells[j] == "满足", neutral = cells[j] == "未约束";
                                badge(u, id + ".verdict", cells[j], width,
                                      ok        ? green
                                      : neutral ? muted
                                                : "#BB4B38",
                                      ok        ? "#E8F6F0"
                                      : neutral ? "#F0F3F8"
                                                : "#FFF0EB");
                            } else
                                text(u, id + "c" + std::to_string(j), cells[j], width, 26,
                                     header ? 12.f : 13.f, header || j == 5 ? muted : ink);
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
        else
            u.column("compact" + std::to_string(i))
                .width(w)
                .padding(12)
                .gap(5)
                .content([&] {
                    text(u, "metric" + std::to_string(i),
                         metricNames[i] + "    实际值 " + actual + "    " + verdict, w - 24, 25,
                         15);
                    u.text("condition" + std::to_string(i))
                        .width(w - 24)
                        .text("目标 " + condition(i) + "   ·   范围 " + range + "   ·   偏差 " +
                              delta)
                        .wrap()
                        .maxWidth(w - 24)
                        .fontSize(13 * 1.35f)
                        .color(muted)
                        .build();
                })
                .build();
    }
}
void preview(Ui &u, float w) {
    if (!state.displayedResult()) {
        text(u, "previewempty", "生成后可在这里查看完整精度的测量数据。", w, 64, 14, muted);
        return;
    }
    const auto &res = *state.result;
    int parts = res.config["design"]["parts"], reps = res.config["design"]["replicates"];
    std::size_t maxDigits = 1;
    for (const auto &row : res.matrix)
        for (double v : row)
            maxDigits = std::max(maxDigits,
                                 msa::fixed(v, res.config["generation"]["decimal_places"]).size());
    int cols = std::clamp(int((w - 88) / std::max(62.f, float(maxDigits) * 8.f + 14.f)), 1, parts),
        pages = (parts + cols - 1) / cols;
    state.previewPage = std::min(state.previewPage, pages - 1);
    int first = state.previewPage * cols, count = std::min(cols, parts - first);
    float cell = (w - 88) / count;
    u.row("previewtoolbar")
        .width(w)
        .gap(8)
        .content([&] {
            text(u, "previewinfo",
                 "部件 " + std::to_string(first + 1) + "–" + std::to_string(first + count) + " / " +
                     std::to_string(parts) + " · " +
                     std::to_string(res.config["generation"]["decimal_places"].get<int>()) +
                     " 位小数",
                 w - 192, 36, 13, muted);
            if (pages > 1) {
                button(
                    u, "prevcols", "上一组", 88, [] { --state.previewPage; }, false,
                    state.previewPage == 0, 36);
                button(
                    u, "nextcols", "下一组", 88, [] { ++state.previewPage; }, false,
                    state.previewPage == pages - 1, 36);
            }
        })
        .build();
    for (int r = -1; r < int(res.matrix.size()); ++r)
        u.row("previewrow" + std::to_string(r))
            .size(w, 34)
            .content([&] {
                text(u, "rowlabel" + std::to_string(r),
                     r < 0 ? "操作员/次"
                           : std::string(1, char('A' + r / reps)) + " / " +
                                 std::to_string(r % reps + 1),
                     88, 34, 13, muted);
                for (int c = 0; c < count; ++c)
                    text(u, "cell" + std::to_string(r) + "_" + std::to_string(c),
                         r < 0 ? std::to_string(first + c + 1)
                               : msa::fixed(res.matrix[r][first + c],
                                            res.config["generation"]["decimal_places"]),
                         cell, 34, 13);
            })
            .build();
    u.text("csvhint")
        .width(w)
        .text(state.draft.csvFormat == 0
                  ? "默认 CSV 保留部件编号和操作员标记；预览中的重复次数不单独写入列。"
                  : "此处按研究设计预览；CSV 按导出设置排列，Trial 保留原始重复测量编号。")
        .fontSize(16)
        .color(muted)
        .wrap()
        .maxWidth(w)
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
                             state.draft.analysisMethod ? "交叉量具 R&R · ANOVA"
                                                        : "交叉量具 R&R · Xbar–R",
                             c - 242, 22, 12, muted);
                        text(u, "study.design",
                             state.draft.operators + " 人 × " + state.draft.replicates + " 次 × " +
                                 state.draft.parts + " 件",
                             c - 242, 32, 20);
                    })
                    .build();
                button(
                    u, "study.sample", "载入示例", 106, [] { state.sample(); }, false, state.busy(),
                    36);
                button(
                    u, "study.options", "研究设置  →", 116,
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
                 const std::vector<std::vector<std::string>> &rows) {
    auto theme = tokens();
    theme.metrics.typography.label = 17;
    theme.metrics.spacing.section = 10;
    auto style = components::DataTableStyle(theme);
    style.row = eui::Color("#FFFFFF");
    style.rowAlt = eui::Color("#F7F9FD");
    style.mutedText = eui::Color(ink);
    components::dataTable(u, id)
        .size(w, 40 + float(rows.size()) * 42)
        .columns(columns)
        .rows(rows)
        .theme(theme)
        .style(style)
        .transition(transition())
        .build();
    for (size_t row = 0; row < rows.size(); ++row)
        for (size_t column = 0; column < columns.size(); ++column)
            if (auto *cell =
                    u.find(id + ".cell." + std::to_string(row) + "." + std::to_string(column)))
                cell->wrap = true;
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
void results(Ui &u, float w, bool summary = true) {
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
            .size(c, 72)
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
                                .size(width, 72)
                                .content([&] {
                                    u.rect(id + ".bg")
                                        .size(width, 72)
                                        .color(i == 2 ? "#EDF2FF" : "#F6F8FC")
                                        .radius(10)
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
                    .position(0, 69)
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
                state.resultEntered = Clock::now();
            },
            40);
        const float progress = reveal(state.resultEntered);
        auto content = u.column("result.content").width(c).opacity(.85f + .15f * progress);
        if (progress < 1)
            content.onFrame([](float) {});
        content
            .content([&] {
                if (state.resultTab == 0)
                    review(u, c);
                else if (state.resultTab == 1)
                    preview(u, c);
                else {
                    if (!hasResult) {
                        text(u, "anova.pending", "生成数据后显示分析详情。", c, 36, 14, muted);
                    } else {
                        const auto &metrics = state.result->metrics;
                        if (metrics.contains("study")) {
                            const auto &study = metrics["study"];
                            text(u, "study.basis",
                                 study["historical_applied"].get<bool>() ? "过程变异：历史标准差"
                                                                         : "过程变异：研究部件",
                                 c, 30, 14);
                            std::vector<std::vector<std::string>> rows;
                            for (const auto *key : {"EV", "AV", "GRR", "PV", "TV"}) {
                                const auto &row = study["components"][key];
                                rows.push_back(
                                    {componentName(key), native_ui::review_number(row["sd"], 6),
                                     native_ui::review_number(row["study_variation_pct"], 2),
                                     row["process_pct"].is_null()
                                         ? "—"
                                         : native_ui::review_number(row["process_pct"], 2)});
                            }
                            detailTable(u, "study.table", c,
                                        {"变异来源", "标准差 SD", "%研究变异", "%过程"}, rows);
                            if (study["components"]["GRR"]["process_pct"].is_null())
                                text(u, "study.process.note", "未提供历史标准差时，%过程不适用。",
                                     c, 24, 12, muted);
                        }
                        if (metrics.contains("misclassification")) {
                            const auto &probability = metrics["misclassification"];
                            text(u, "misclassification.title", "误分类概率（正态模型）", c, 30, 14);
                            const std::array<const char *, 4> keys{"bad_accepted", "good_rejected",
                                                                   "accept_given_bad",
                                                                   "reject_given_good"};
                            const std::array<const char *, 4> labels{
                                "不合格且被接受", "合格且被拒绝", "不合格品被接受（条件）",
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
                                 "标准差置信限（" + native_ui::review_number(ci["level"], 2) +
                                     "% · " +
                                     (ci["method"] == "Satterthwaite_variance_components"
                                          ? "Satterthwaite"
                                          : "MLS") +
                                     "）",
                                 c, 30, 14);
                            const auto limit = [](const msa::Json &v) {
                                return v.is_null() ? std::string("—")
                                                   : native_ui::review_number(v, 6);
                            };
                            std::vector<std::vector<std::string>> rows;
                            for (const auto *key : {"EV", "AV", "GRR", "PV", "TV"}) {
                                const auto &bounds = ci["stddev"][key];
                                rows.push_back({componentName(key), limit(bounds["lower"]),
                                                limit(bounds["upper"])});
                            }
                            detailTable(u, "confidence.table", c,
                                        {"变异来源", "置信下限", "置信上限"}, rows);
                        }
                        if (metrics.contains("anova")) {
                            const auto &a = state.result->metrics["anova"];
                            text(u, "anova.model",
                                 a["interaction_included"].get<bool>()
                                     ? "最终模型：保留部件 × 操作员交互项"
                                     : "最终模型：移除交互项，合并到重复性误差",
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
                                detailTable(u, "anova.table", c,
                                            {"变异来源", "自由度 DF", "平方和 SS", "均方 MS",
                                             "F 值", "P 值"},
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
                                            text(u, id + ".source",
                                                 cells[0] + " · DF = " + cells[1], c, 24, 14);
                                            const std::array<const char *, 4> names{
                                                "平方和 SS", "均方 MS", "F 值", "P 值"};
                                            for (int pair = 0; pair < 2; ++pair)
                                                u.row(id + ".pair" + std::to_string(pair))
                                                    .width(c)
                                                    .gap(12)
                                                    .content([&] {
                                                        for (int j = pair * 2; j < pair * 2 + 2;
                                                             ++j)
                                                            u.text(id + ".value" +
                                                                   std::to_string(j))
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
                    }
                }
            })
            .build();
        text(u, "result.caution", "满足生成约束 ≠ 测量系统合格", c, 26, 12, muted);
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
        state.configDirty = false;
        state.message.clear();
        state.feedback("配置已保存", "配置已保存：" + path.u8string());
    });
}
void exportData() {
    if (!state.validResult())
        return;
    try {
        const auto snapshot = *state.result;
        const auto output =
            msa::export_configured_csv(snapshot, native_ui::output_config(state.draft));
        state.status = Status::Success;
        state.message.clear();
        state.feedback("CSV 已导出", "CSV 已导出并复验：" + output.u8string());
    } catch (const std::exception &e) {
        state.status = Status::ExportError;
        state.message = std::string("导出失败：") + e.what();
        state.feedback("CSV 导出失败", state.message, Level::Error);
    }
}
void paragraph(Ui &, const std::string &, const std::string &, float, const char *, float);
void methodSettings(Ui &u, float w) {
    section(u, "analysis", "分析方法", w, [&](float c) {
        segments(u, "analysis.method", {"均值极差法 · Xbar-R", "方差分析法 · ANOVA"},
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
                "以均值和极差估计重复性、再现性及部件间变异；不单独估计部件与操作员的交互作用。", c,
                muted, 17.5f);
        if (state.draft.analysisMethod) {
            text(u, "analysis.interaction.label", "部件 × 操作员交互项", c, 24, 14);
            combo(u, "analysis.interaction",
                  {"自动判断（按 P 值）", "始终保留交互项", "移除交互项并合并误差"},
                  state.draft.interactionMode, c, [](int i) {
                      if (state.busy() || !state.draft.analysisMethod ||
                          state.draft.interactionMode == i)
                          return;
                      state.draft.interactionMode = i;
                      state.changed();
                  });
            u.column("analysis.alpha.control")
                .width(c)
                .disabled(state.draft.interactionMode != 0)
                .content([&] {
                    field(u, "analysis.alpha", "交互项判定的显著性水平 α",
                          state.draft.interactionAlpha, c, "默认 0.05，0 < α < 1");
                })
                .build();
            paragraph(
                u, "analysis.note",
                state.draft.interactionMode == 0
                    ? "交互项 P 值小于 α 时保留，否则合并到重复性误差；AV 包含保留的交互变异。"
                    : "显著性水平仅在自动判断时使用；当前按所选方式处理交互项。",
                c, muted, 17.5f);
        }
    });
}
void designSettings(Ui &u, float w) {
    section(u, "design", "研究设计", w, [&](float c) {
        float f = (c - 24) / 3;
        u.row("designrow")
            .width(c)
            .gap(12)
            .content([&] {
                u.column("ocol")
                    .width(f)
                    .gap(7)
                    .content([&] {
                        text(u, "olabel", "操作员数", f, 22, 14);
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
                    .gap(7)
                    .content([&] {
                        text(u, "rlabel", "重复测量次数", f, 22, 14);
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
                    .gap(7)
                    .content([&] {
                        text(u, "plabel", "部件数", f, 22, 14);
                        stepper(u, "parts", state.draft.parts, f, 5, 10);
                    })
                    .build();
            })
            .build();
        paragraph(u, "design.note",
                  "平衡交叉研究：每位操作员测量全部部件，每个部件的重复测量次数相同。", c, muted,
                  17.5f);
        const int records = std::atoi(state.draft.operators.c_str()) *
                            std::atoi(state.draft.replicates.c_str()) *
                            std::atoi(state.draft.parts.c_str());
        badge(u, "design.records", "共 " + std::to_string(records) + " 次测量", 144);
    });
}
void specSettings(Ui &u, float w) {
    section(u, "spec", "规格与尺度", w, [&](float c) {
        float f = (c - 12) / 2;
        u.row("limits")
            .width(c)
            .gap(12)
            .content([&] {
                field(u, "lsl", "规格下限 LSL", state.draft.lsl, f, "可留空");
                field(u, "usl", "规格上限 USL", state.draft.usl, f, "可留空");
            })
            .build();
        u.row("scalerow")
            .width(c)
            .gap(12)
            .content([&] {
                field(u, "multiplier", "研究变异倍数", state.draft.multiplier, f, "默认 6");
                u.column("scalecol")
                    .width(f)
                    .gap(7)
                    .content([&] {
                        text(u, "scalelabel", "生成尺度", f, 22, 14);
                        combo(u, "scale", {"自动", "参考 TV_SD"}, state.draft.scale, f, [](int i) {
                            if (state.busy() || state.draft.scale == i)
                                return;
                            state.draft.scale = i;
                            state.changed();
                        });
                    })
                    .build();
            })
            .build();
        if (state.draft.scale == 1 || native_ui::trim(state.draft.lsl).empty() ||
            native_ui::trim(state.draft.usl).empty())
            field(u, "reference", "参考总变异标准差 TV_SD", state.draft.reference, c,
                  "正数，1σ；可由历史标准差补足");
        paragraph(u, "spechelp", "开启“规格内生成”时至少填写一个规格限；单侧规格也可计算 P/T。", c,
                  muted, 17.5f);
    });
}
void processSettings(Ui &u, float w) {
    section(u, "analysis.process", "分析口径与过程参数", w, [&](float c) {
        subheading(u, "analysis.process.tolerance", "公差评价", "仅影响分析，不改变生成规格限。",
                   c);
        field(u, "analysis.tolerance", "分析容差（可留空）", state.draft.analysisTolerance, c,
              "留空按规格限计算 P/T");
        subheading(u, "analysis.process.history", "历史过程参考",
                   "可选；未填写时使用本次研究估计。", c);
        u.row("analysis.historical.values")
            .width(c)
            .gap(12)
            .content([&] {
                field(u, "analysis.historical.sd", "历史标准差（1σ）", state.draft.historicalSd,
                      (c - 12) / 2, "正数，可留空");
                field(u, "analysis.historical.mean", "历史均值", state.draft.historicalMean,
                      (c - 12) / 2, "留空使用测量均值");
            })
            .build();
        text(u, "analysis.process.label", "过程变异估计来源", c, 24, 14);
        combo(u, "analysis.process.source", {"历史标准差（填写后使用）", "研究中的部件"},
              state.draft.processVariation, c, [](int i) {
                  if (state.busy() || state.draft.processVariation == i)
                      return;
                  state.draft.processVariation = i;
                  state.changed();
              });
        paragraph(u, "analysis.process.note",
                  "分析容差仅用于计算，不替代生成时的规格限。未填写历史标准差时使用研究估计；单侧 "
                  "P/T 使用半研究变异。",
                  c, muted, 17.5f);
    });
}
void precisionSettings(Ui &u, float w) {
    section(u, "precision", "数值精度与允许误差", w, [&](float c) {
        float f = (c - 24) / 3;
        u.row("precisionrow")
            .width(c)
            .gap(12)
            .content([&] {
                u.column("decimals.field")
                    .width(f)
                    .gap(7)
                    .content([&] {
                        text(u, "decimals.label", "小数位数", f, 22, 14);
                        stepper(u, "decimals", state.draft.decimals, f, 0, 10);
                    })
                    .build();
                field(u, "percenterror", "误差（百分点）", state.draft.percentError, f);
                field(u, "ndcerror", "ndc 误差", state.draft.ndcError, f);
            })
            .build();
        paragraph(
            u, "precision.note",
            "小数位数支持 0～10 "
            "位。未单独设置目标误差时使用这里的默认误差；百分比指标以百分点计，ndc 误差为整数。",
            c, muted, 17.5f);
    });
}
void statisticsSettings(Ui &u, float w) {
    section(u, "analysis.statistics", "统计选项", w, [&](float c) {
        const bool anova = state.draft.analysisMethod == 1;
        const bool hasSpec =
            !native_ui::trim(state.draft.lsl).empty() || !native_ui::trim(state.draft.usl).empty();
        u.column("analysis.misclassification.control")
            .width(c)
            .disabled(!anova || !hasSpec)
            .content([&] {
                toggle(
                    u, "analysis.misclassification", "计算误分类概率",
                    state.draft.misclassification, c,
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
        paragraph(u, "statistics.note",
                  !anova     ? "误分类概率和置信区间仅适用于 ANOVA；切换方法后可设置。"
                  : !hasSpec ? "计算误分类概率需要至少一个规格限；请在“研究”中填写。"
                             : "误分类概率评估真实合格状态与测量判定不一致的可能性。",
                  c, muted, 17.5f);
        u.column("analysis.confidence.control")
            .width(c)
            .disabled(!anova)
            .content([&] {
                toggle(
                    u, "analysis.confidence", "计算置信区间", state.draft.confidenceEnabled, c,
                    [](bool enabled) {
                        if (state.busy() || state.draft.analysisMethod != 1 ||
                            state.draft.confidenceEnabled == enabled)
                            return;
                        state.draft.confidenceEnabled = enabled;
                        state.changed();
                    },
                    !anova);
            })
            .build();
        if (state.draft.confidenceEnabled) {
            u.column("analysis.confidence.options")
                .width(c)
                .gap(12)
                .disabled(!anova)
                .content([&] {
                    text(u, "analysis.confidence.method.label", "置信区间估计方法", c, 22, 14);
                    combo(
                        u, "analysis.confidence.method", {"MLS（Minitab）", "Satterthwaite（JMP）"},
                        state.draft.confidenceMethod, c, [](int i) {
                            if (state.busy() || state.draft.analysisMethod != 1 ||
                                !state.draft.confidenceEnabled || state.draft.confidenceMethod == i)
                                return;
                            state.draft.confidenceMethod = i;
                            state.changed();
                        });
                    field(u, "analysis.confidence.level", "置信水平（%）",
                          state.draft.confidenceLevel, c, "默认 95，范围 0.01～99.99");
                    text(u, "analysis.confidence.bound.label", "置信限类型", c, 22, 14);
                    segments(u, "analysis.confidence.bound", {"双侧区间", "单侧下限", "单侧上限"},
                             state.draft.confidenceBound, c, [](int i) {
                                 if (state.busy() || state.draft.analysisMethod != 1 ||
                                     !state.draft.confidenceEnabled ||
                                     state.draft.confidenceBound == i)
                                     return;
                                 state.draft.confidenceBound = i;
                                 state.changed();
                             });
                })
                .build();
        } else if (anova) {
            paragraph(u, "analysis.confidence.note", "开启后可设置估计方法、置信水平和置信限类型。",
                      c, muted, 17.5f);
        }
    });
}
void runOrderChoice(Ui &u, const std::string &id, const std::string &label, bool value, bool radio,
                    bool disabled, float w, std::function<void(bool)> change) {
    auto choiceTheme = tokens();
    if (disabled) {
        choiceTheme.text = components::theme::color(0.39f, 0.45f, 0.54f);
        choiceTheme.primary = components::theme::color(0.59f, 0.66f, 0.79f);
        choiceTheme.surface = components::theme::color(0.95f, 0.96f, 0.98f);
    }
    auto guarded = [disabled, change](bool next) {
        if (!disabled)
            change(next);
    };
    u.column(id + ".control")
        .width(w)
        .disabled(disabled)
        .content([&] {
            if (radio)
                components::radio(u, id)
                    .size(w, 36)
                    .selected(value)
                    .text(label)
                    .fontSize(18)
                    .theme(choiceTheme)
                    .transition(transition())
                    .onChange(guarded)
                    .build();
            else
                components::checkbox(u, id)
                    .size(w, 36)
                    .checked(value)
                    .text(label)
                    .fontSize(18)
                    .theme(choiceTheme)
                    .transition(transition())
                    .onChange(guarded)
                    .build();
        })
        .build();
    keyboard(u, id + ".hit", [guarded, value, radio, disabled](const eui::KeyEvent &key) {
        if (disabled || !activate(key))
            return false;
        guarded(radio ? true : !value);
        return true;
    });
    if (auto *hit = u.find(id + ".hit"))
        hit->focusable = !disabled;
    if (!state.motion)
        for (const auto *suffix : {".inner", ".mark.short", ".mark.long"})
            if (auto *part = u.find(id + suffix))
                part->transition.durationSeconds = 0;
}
void searchSettings(Ui &u, float w) {
    section(u, "advanced", "数据随机性与搜索", w, [&](float c) {
        field(u, "seed", "数据生成随机种子（可留空）", state.draft.seed, c,
              "留空自动生成；固定种子可复现数据");
        const float elapsed = reveal(state.advancedChanged);
        const float expanded =
            state.advancedFrom + ((state.advanced ? 1.0f : 0.0f) - state.advancedFrom) * elapsed;
        button(
            u, "advancedtoggle", state.advanced ? "收起搜索预算  ▴" : "展开搜索预算  ▾", c,
            [expanded] {
                if (state.busy())
                    return;
                state.advancedFrom = expanded;
                state.advanced = !state.advanced;
                state.advancedChanged = Clock::now();
            },
            false, state.busy(), 36);
        if (expanded > 0 || state.advanced) {
            float f = (c - 12) / 2;
            auto panel = u.column("advanced.reveal")
                             .size(c, 69 * expanded)
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
        paragraph(
            u, "advanced.note",
            "数据生成种子与导出的运行顺序种子相互独立。增加搜索预算可提高找到满足约束数据的机会。",
            c, muted, 17.5f);
    });
}
void exportSettings(Ui &u, float w) {
    section(u, "output", "CSV 文件导出", w, [&](float c) {
        text(u, "csvformat.label", "CSV 导出格式", c, 22, 14);
        combo(u, "csvformat", {"默认", "Minitab", "SPSS", "JMP 19", "Origin", "MATLAB"},
              state.draft.csvFormat, c, [](int i) {
                  if (state.busy() || state.draft.csvFormat == i)
                      return;
                  state.draft.csvFormat = i;
                  state.outputChanged();
              });
        u.text("outputformat")
            .width(c)
            .wrap()
            .maxWidth(c)
            .fontSize(17.5f)
            .color(muted)
            .text(state.draft.csvFormat == 0
                      ? "默认宽表：首行是部件编号，首列是操作员标记；每行是一名操作员的一轮测量。"
                      : std::string(state.draft.storeStandardOrder
                                        ? "五列长表：StdOrder,Operator,Part,Trial,Measurement。"
                                        : "四列长表：Operator,Part,Trial,Measurement。") +
                            "每行一次测量，行序即实际运行顺序；五种软件使用相同布局。")
            .build();
        text(u, "dirlabel", "输出目录（留空使用默认配置目录）", c, 22, 14);
        u.row("dirrow")
            .width(c)
            .gap(10)
            .content([&] {
                input(u, "directory", state.draft.directory, c - 106,
                      msa::config_path().parent_path().u8string(), true);
                button(u, "choosedirectory", "选择目录", 96, [] {
                    if (state.busy())
                        return;
                    safe([] {
                        if (auto p = platform::choose(false, true)) {
                            if (state.draft.directory == p->u8string())
                                return;
                            state.draft.directory = p->u8string();
                            state.outputChanged();
                        }
                    });
                });
            })
            .build();
        field(u, "filename", "CSV 文件名模板", state.draft.filename, c, "msa_data.csv", true);
        text(u, "collision.label", "同名文件处理", c, 22, 14);
        segments(u, "collision", {"追加序号", "覆盖文件"}, state.draft.existingFile, c, [](int i) {
            if (state.busy() || state.draft.existingFile == i)
                return;
            state.draft.existingFile = i;
            state.outputChanged();
        });
        paragraph(
            u, "silentoutput",
            state.draft.existingFile == 0
                ? "同名时自动追加序号，保留原文件。相对目录以配置目录为基准；导出结果在底部状态栏显"
                  "示。"
                : "同名时直接替换原文件。相对目录以配置目录为基准；导出结果在底部状态栏显示。",
            c, muted, 17.5f);
    });
}
void runOrderSettings(Ui &u, float w) {
    section(u, "runorder", "运行顺序", w, [&](float c) {
        const bool disabled = state.draft.csvFormat == 0;
        const std::array<std::string, 3> names{"不随机化", "随机化所有运行顺序",
                                               "在操作员内部随机化运行顺序"};
        u.text("runorder.explanation")
            .width(c)
            .wrap()
            .maxWidth(c)
            .fontSize(17.5f)
            .color(muted)
            .text(disabled ? "默认宽表无法完整表达逐条测量的随机运行顺序；选择任一长表格式后可用。"
                  : state.draft.runMode == 0
                      ? "标准顺序：重复测量轮次 → 部件 → 操作员（Trial → Part → Operator）。"
                  : state.draft.runMode == 1
                      ? "打乱完整记录，允许跨操作员、部件及重复测量轮次排列。"
                      : "逐 Trial、逐操作员安排；每个 Trial＋Operator 内独立打乱部件顺序。")
            .build();
        for (int i = 0; i < 3; ++i)
            runOrderChoice(u, "runorder.mode." + std::to_string(i), names[i],
                           state.draft.runMode == i, true, disabled, c, [i](bool selected) {
                               if (selected && !state.busy() && state.draft.csvFormat != 0 &&
                                   state.draft.runMode != i) {
                                   state.draft.runMode = i;
                                   state.outputChanged();
                               }
                           });
        subheading(u, "runorder.randomness", "随机性控制", "", c);
        runOrderChoice(u, "runorder.operators", "随机化操作员顺序", state.draft.randomOperators,
                       false, disabled || state.draft.runMode != 2, c, [](bool checked) {
                           if (state.busy() || state.draft.csvFormat == 0 ||
                               state.draft.runMode != 2 || state.draft.randomOperators == checked)
                               return;
                           state.draft.randomOperators = checked;
                           state.outputChanged();
                       });
        u.column("runorder.seed.control")
            .width(c)
            .disabled(disabled || state.draft.runMode == 0)
            .content([&] {
                field(u, "runorder.seed", "运行顺序随机种子（可留空）", state.draft.runSeed, c,
                      "正整数；留空时为本研究自动选择", true);
            })
            .build();
        if (!disabled)
            paragraph(u, "runorder.availability",
                      state.draft.runMode == 0
                          ? "不随机化时无需排序种子；操作员顺序选项仅在操作员内部随机化时可用。"
                      : state.draft.runMode == 1
                          ? "全部随机化时不单独指定操作员顺序；固定种子可复现相同排列。"
                          : "勾选后，每项研究随机确定一次操作员顺序，各轮次沿用；固定种子可复现相同"
                            "排列。",
                      c, muted, 17.5f);
        subheading(u, "runorder.columns", "附加导出列", "", c);
        runOrderChoice(u, "runorder.standard", "在导出文件中存储标准运行顺序",
                       state.draft.storeStandardOrder, false, disabled, c, [](bool checked) {
                           if (state.busy() || state.draft.csvFormat == 0 ||
                               state.draft.storeStandardOrder == checked)
                               return;
                           state.draft.storeStandardOrder = checked;
                           state.outputChanged();
                       });
        if (!disabled)
            paragraph(u, "runorder.standard.note",
                      "StdOrder 保存随机化前的标准序号；Trial 始终保留真实重复测量编号。", c, muted,
                      17.5f);
    });
}
void pathSettings(Ui &u, float w) {
    section(u, "configpath", "配置文件", w, [&](float c) {
        paragraph(u, "configpathhint", "保存配置后写入以下位置，下次启动自动读取。", c, muted,
                  17.5f);
        u.text("path")
            .width(c)
            .text(platform::compatibilityConfig().u8string())
            .wrap()
            .maxWidth(c)
            .fontSize(17.5f)
            .color(muted)
            .build();
        std::error_code directoryError;
        const bool directoryExists = std::filesystem::is_directory(
            platform::compatibilityConfig().parent_path(), directoryError);
        u.row("pathbuttons")
            .width(c)
            .gap(10)
            .content([&] {
                button(u, "copypath", "复制完整路径", 140, [] {
                    safe([] {
                        platform::clipboard(platform::compatibilityConfig().wstring());
                        state.feedback("配置文件路径已复制");
                    });
                });
                button(
                    u, "opendir", "打开配置目录", 140,
                    [] {
                        safe([] {
                            platform::openDirectory(platform::compatibilityConfig().parent_path());
                        });
                    },
                    false, !directoryExists);
            })
            .build();
        if (!directoryExists)
            paragraph(u, "configpath.unavailable", "配置目录尚未创建；保存配置后即可打开。", c,
                      muted, 17.5f);
    });
}
void outputConfig(Ui &u, float w) {
    exportSettings(u, w);
    runOrderSettings(u, w);
    pathSettings(u, w);
}
void settingsColumns(Ui &u, const std::string &id, float w, std::function<void(float)> left,
                     std::function<void(float)> right) {
    if (w >= 1100) {
        const float column = (w - 20) / 2;
        u.row(id)
            .width(w)
            .gap(20)
            .content([&] {
                u.column(id + ".left").width(column).gap(16).content([&] { left(column); }).build();
                u.column(id + ".right")
                    .width(column)
                    .gap(16)
                    .content([&] { right(column); })
                    .build();
            })
            .build();
    } else {
        left(w);
        right(w);
    }
}
void config(Ui &u, float w) {
    if (state.optionsTab == 0)
        settingsColumns(
            u, "options.study", w,
            [&](float c) {
                designSettings(u, c);
                specSettings(u, c);
            },
            [&](float c) {
                precisionSettings(u, c);
                searchSettings(u, c);
            });
    else if (state.optionsTab == 1)
        settingsColumns(
            u, "options.analysis", w,
            [&](float c) {
                methodSettings(u, c);
                statisticsSettings(u, c);
            },
            [&](float c) { processSettings(u, c); });
    else if (state.optionsTab == 2)
        settingsColumns(
            u, "options.output", w,
            [&](float c) {
                exportSettings(u, c);
                pathSettings(u, c);
            },
            [&](float c) { runOrderSettings(u, c); });
    else
        aiSettings(u, w);
}
void optionsPage(Ui &u, float w, float h) {
    u.column("options.page")
        .size(w, h)
        .gap(12)
        .content([&] {
            u.row("options.intro")
                .size(w, 32)
                .gap(16)
                .alignItems(eui::Align::CENTER)
                .content([&] {
                    text(u, "options.title", "选项", 76, 32, 22);
                    const std::array<std::string, 4> descriptions{
                        "定义研究设计、规格与生成精度", "选择分析方法及统计评价口径",
                        "设置 CSV 布局、文件位置和运行顺序",
                        "配置 AI 服务连接与系统提示词；独立加密保存"};
                    text(u, "options.subtitle", descriptions[state.optionsTab], w - 92, 32, 12,
                         muted);
                })
                .build();
            tabs(u, "options.tabs", {"研究", "分析", "导出", "助手"},
                 state.optionsTab, std::min(w, 760.f), [](int i) {
                     state.optionsTab = i;
                     state.configOffset = 0;
                     menu.id.clear();
                 });
            components::scrollView(u, "configscroll")
                .size(w, h - 96)
                .offset(state.configOffset)
                .theme(tokens())
                .scrollbarGap(8)
                .reserveScrollbar()
                .onChange([](float offset) {
                    state.configOffset = offset;
                    menu.id.clear();
                })
                .content([](Ui &v, float cw, float) {
                    v.column("configcontent")
                        .width(cw)
                        .gap(16)
                        .padding(0, 0, 0, 12)
                        .disabled(state.busy() || (aiNetworkBusy() && state.optionsTab != 3))
                        .content([&] {
                            notice(v, cw);
                            config(v, cw);
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
                .color("#FFFFFF")
                .radius(10)
                .border(1, line)
                .shadow(18, 0, 5, "#20334B18")
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
                                    .states(i == menu.highlighted ? "#E9F1FF" : "#FFFFFF",
                                            "#EDF3FD", "#DCE9FF")
                                    .radius(4)
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
                                    .position(8, 0)
                                    .size(std::max(1.f, width - 16), 38)
                                    .text(menu.items[i])
                                    .fontSize(14 * 1.35f)
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
void paragraph(Ui &u, const std::string &id, const std::string &content, float w,
               const char *color = muted, float fontSize = 19) {
    u.text(id).width(w).text(content).wrap().maxWidth(w).fontSize(fontSize).color(color).build();
}
void copyFilenameVariable(const std::string &token) {
    try {
        platform::clipboard(platform::wide(token));
        state.hint("已复制 " + token);
    } catch (const std::exception &error) {
        state.feedback("复制失败，请重试", error.what(), Level::Error);
    }
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
    if (w < 720) {
        u.column(id)
            .width(w)
            .gap(6)
            .content([&] {
                u.row(id + ".tokens")
                    .width(w)
                    .gap(10)
                    .content([&] {
                        button(
                            u, id + ".copy", name, nameWidth,
                            [name] { copyFilenameVariable(name); }, false, false, 32);
                        if (!alias.empty())
                            button(
                                u, id + ".alias", alias, aliasWidth,
                                [alias] { copyFilenameVariable(alias); }, false, false, 32);
                    })
                    .build();
                paragraph(u, id + ".description",
                          std::string(variable.description) + "   例：" + variable.example, w, ink,
                          16);
            })
            .build();
        return;
    }
    const float descriptionWidth = w - nameWidth - aliasWidth - (alias.empty() ? 10 : 20);
    u.row(id)
        .width(w)
        .gap(10)
        .alignItems(eui::Align::CENTER)
        .content([&] {
            button(
                u, id + ".copy", name, nameWidth, [name] { copyFilenameVariable(name); }, false,
                false, 32);
            if (!alias.empty())
                button(
                    u, id + ".alias", alias, aliasWidth, [alias] { copyFilenameVariable(alias); },
                    false, false, 32);
            paragraph(u, id + ".description",
                      std::string(variable.description) + "   例：" + variable.example,
                      descriptionWidth, ink, 16);
        })
        .build();
}
std::string levelName(Level level) {
    return level == Level::Error ? "错误" : level == Level::Warning ? "警告" : "信息";
}
const char *levelColor(Level level) {
    return level == Level::Error ? "#AD3626" : level == Level::Warning ? "#946117" : muted;
}
void dataPage(Ui &u, float w, float h) {
    components::scrollView(u, "generatescroll")
        .size(w, h)
        .offset(state.generateOffset)
        .theme(tokens())
        .scrollbarGap(8)
        .reserveScrollbar()
        .onChange([](float offset) {
            state.generateOffset = offset;
            menu.id.clear();
        })
        .content([](Ui &v, float cw, float) {
            v.column("generationcontent")
                .width(cw)
                .gap(12)
                .content([&] {
                    notice(v, cw);
                    if (cw >= 1080) {
                        const float left = std::clamp(cw * .38f, 460.f, 500.f),
                                    right = cw - left - 16;
                        v.row("split")
                            .width(cw)
                            .gap(16)
                            .content([&] {
                                v.column("inputcolumn")
                                    .width(left)
                                    .gap(12)
                                    .disabled(state.busy())
                                    .content([&] { inputs(v, left); })
                                    .build();
                                v.column("resultcolumn")
                                    .width(right)
                                    .gap(12)
                                    .content([&] { results(v, right); })
                                    .build();
                            })
                            .build();
                    } else {
                        studySummary(v, cw);
                        v.column("inputcolumn")
                            .width(cw)
                            .gap(12)
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
void logsPage(Ui &u, float w, float h) {
    const auto countLevel = [](Level level) {
        return std::count_if(state.logs.begin(), state.logs.end(),
                             [level](const LogEntry &entry) { return entry.level == level; });
    };
    const auto matches = [](const LogEntry &entry) {
        return state.logFilter == 0 ||
               (state.logFilter == 1 ? entry.level == Level::Warning : entry.level == Level::Error);
    };
    const auto shown = std::count_if(state.logs.begin(), state.logs.end(), matches);
    u.column("logs.page")
        .size(w, h)
        .gap(12)
        .content([&] {
            u.row("logs.heading")
                .size(w, 32)
                .gap(12)
                .alignItems(eui::Align::CENTER)
                .content([&] {
                    text(u, "logs.title", "运行日志", 136, 32, 22);
                    text(u, "logs.retention", "仅本次运行 · 保留最近 100 条记录", w - 148, 32, 12,
                         muted);
                })
                .build();
            u.row("logs.overview")
                .size(w, 28)
                .gap(8)
                .alignItems(eui::Align::CENTER)
                .content([&] {
                    badge(u, "logs.information", "信息 " + std::to_string(countLevel(Level::Info)),
                          96, muted, "#EAF0F7");
                    badge(u, "logs.warnings", "警告 " + std::to_string(countLevel(Level::Warning)),
                          96, "#91600C", "#FFF4DB");
                    badge(u, "logs.errors", "错误 " + std::to_string(countLevel(Level::Error)), 96,
                          "#AD3626", "#FCEDE9");
                    text(u, "logs.count", "显示 " + std::to_string(shown) + " 条", w - 312, 28, 12,
                         muted);
                })
                .build();
            u.row("logs.toolbar")
                .size(w, 40)
                .gap(8)
                .alignItems(eui::Align::CENTER)
                .content([&] {
                    segments(u, "logfilter", {"全部", "警告", "错误"}, state.logFilter, 240,
                             [](int i) {
                                 state.logFilter = i;
                                 state.logsCopied = false;
                                 state.logOffset = 0;
                                 state.followLogs = false;
                             });
                    toggle(u, "logfollow", "跟随最新", state.followLogs, 148, [](bool follow) {
                        state.followLogs = follow;
                        if (follow)
                            state.logOffset = 1000000;
                    });
                    button(
                        u, "copylogs", state.logsCopied ? "已复制日志" : "复制所示日志", 136,
                        [] {
                            safe([] {
                                std::string content;
                                for (const auto &entry : state.logs)
                                    if (state.logFilter == 0 ||
                                        (state.logFilter == 1 ? entry.level == Level::Warning
                                                              : entry.level == Level::Error))
                                        content += entry.time + " [" + levelName(entry.level) +
                                                   "] " + entry.message + "\r\n";
                                if (content.empty())
                                    return;
                                platform::clipboard(platform::wide(content));
                                state.logsCopied = true;
                                state.hint("当前显示的日志已复制");
                            });
                        },
                        false, shown == 0);
                })
                .build();
            components::scrollView(u, "logscroll")
                .size(w, std::max(80.0f, h - 136))
                .offset(state.logOffset)
                .theme(tokens())
                .scrollbarGap(8)
                .reserveScrollbar()
                .onChange([&u](float offset) {
                    state.logOffset = offset;
                    const auto *scroll = u.find("logscroll");
                    state.followLogs = scroll && offset >= scroll->scrollMaxOffset - 1;
                    menu.id.clear();
                })
                .content([matches](Ui &v, float cw, float) {
                    components::card(v, "logs.card")
                        .width(cw)
                        .wrapContentHeight()
                        .theme(tokens())
                        .shadow(cardShadow())
                        .padding(16)
                        .radius(14)
                        .color("#FFFFFF")
                        .border(1, line)
                        .content([&] {
                            v.column("logs.entries")
                                .width(cw - 32)
                                .gap(0)
                                .content([&] {
                                    bool hasEntries = false;
                                    for (const auto &entry : state.logs) {
                                        if (!matches(entry))
                                            continue;
                                        const std::string id =
                                            "log." + std::to_string(entry.sequence);
                                        if (hasEntries)
                                            v.rect(id + ".divider")
                                                .size(cw - 32, 1)
                                                .color(line)
                                                .build();
                                        hasEntries = true;
                                        v.row(id)
                                            .width(cw - 32)
                                            .padding(0, 12)
                                            .gap(12)
                                            .alignItems(eui::Align::START)
                                            .content([&] {
                                                text(v, id + ".time", entry.time, 76, 28, 13,
                                                     muted);
                                                badge(v, id + ".level", levelName(entry.level), 56,
                                                      levelColor(entry.level),
                                                      entry.level == Level::Error     ? "#FCEDE9"
                                                      : entry.level == Level::Warning ? "#FFF4DB"
                                                                                      : "#EAF0F7");
                                                paragraph(v, id + ".message", entry.message,
                                                          cw - 188, ink, 18);
                                            })
                                            .build();
                                    }
                                    if (!hasEntries) {
                                        v.column("logs.empty.state")
                                            .width(cw - 32)
                                            .padding(12, 28)
                                            .gap(10)
                                            .alignItems(eui::Align::CENTER)
                                            .content([&] {
                                                badge(v, "logs.empty.badge", "暂无记录", 100, green,
                                                      "#E8F6F0");
                                                paragraph(v, "logs.empty",
                                                          state.logFilter == 0
                                                              ? "当前没有运行记录。生成、保存或导出"
                                                                "后的操作信息会显示在这里。"
                                                          : state.logFilter == 1
                                                              ? "当前没有警告记录。选择“全部”可查看"
                                                                "正常操作记录。"
                                                              : "当前没有错误记录。选择“全部”可查看"
                                                                "正常操作记录。",
                                                          cw - 56, muted, 18);
                                            })
                                            .build();
                                    }
                                })
                                .build();
                        })
                        .build();
                })
                .build();
        })
        .build();
}
void helpPage(Ui &u, float w, float h) {
    const std::vector<std::string> topics{"快速入门", "分析指标", "导出顺序",
                                          "文件变量", "命令说明", "图表助手"};
    const std::array<std::string, 6> descriptions{
        "从研究设计到生成与复核，按步骤完成第一项研究。",
        "理解分析方法、指标单位与统计假设，按相同口径核对结果。",
        "选择 CSV 布局和运行顺序，在外部统计软件中复核同一组数据。",
        "点击变量名称即可复制，再粘贴到“选项 → 导出”的文件名模板中。",
        "GUI 与 CLI 共用研究、生成、分析和 CSV 配置；图表与 AI 功能由桌面页面提供。",
        "编辑并导出研究图表，使用自然语言生成数据或解释当前数据集。"};
    const auto changeTopic = [](int topic) {
        if (state.helpTopic == topic)
            return;
        state.helpTopic = topic;
        state.helpOffset = 0;
        menu.id.clear();
    };
    u.column("help.layout")
        .size(w, h)
        .gap(12)
        .content([&] {
            text(u, "help.title", "帮助与参考", w, 30, 20);
            if (w < 700)
                combo(u, "help.topics", topics, state.helpTopic, w, changeTopic);
            else
                tabs(u, "help.topics", topics, state.helpTopic, w, changeTopic);
            components::scrollView(u, "helpscroll")
                .size(w, std::max(80.f, h - 94))
                .offset(state.helpOffset)
                .theme(tokens())
                .scrollbarGap(8)
                .reserveScrollbar()
                .onChange([](float offset) {
                    state.helpOffset = offset;
                    menu.id.clear();
                })
                .content([&](Ui &v, float cw, float) {
                    v.column("help.page")
                        .width(cw)
                        .gap(16)
                        .content([&] {
                            paragraph(v, "help.intro", descriptions[state.helpTopic], cw);
                            if (state.helpTopic == 5) {
                                section(v, "help.charts", "结果图表与导出", cw, [&](float c) {
                                    paragraph(
                                        v, "help.charts.text",
                                        "图表页提供变异分量、按部件测量、按操作员分布、极差控制"
                                        "图、"
                                        "均值控制图、交互作用图和直方图。可编辑标题、坐标轴名称、颜"
                                        "色、"
                                        "线宽、点大小、纵轴范围、网格与图例。修改先保留为草稿，点击"
                                        "“应用编辑”后更新预览和导出；撤销草稿可保留上次有效设置。"
                                        "编辑不改变原始测量值和统计指标。",
                                        c);
                                    paragraph(
                                        v, "help.charts.export",
                                        "支持当前单图或选中多图组合导出 PNG、JPEG 和 "
                                        "PDF，同名自动追加序号。"
                                        "PDF "
                                        "为保留中文的高分辨率图像页面，文字不可选择或搜索；页面随组"
                                        "合尺寸"
                                        "和分辨率生成，不是固定 "
                                        "A4。导出前请应用或撤销所选图表的草稿。"
                                        "绘图失败时可按页内提示重试或恢复默认外观，文件错误可修正目"
                                        "录后重试。"
                                        "图形控制限口径见每图说明。",
                                        c);
                                    button(v, "help.charts.open", "打开结果图表", 160,
                                           [] { state.navigate(Page::Charts); });
                                });
                                section(v, "help.ai", "AI 辅助生成与解读", cw, [&](float c) {
                                    paragraph(v, "help.ai.flow",
                                              "先在选项 → 助手选择 DeepSeek 或自定义 OpenAI "
                                              "兼容服务，填写接口和密钥，拉取模型列表后选择模型并测试连接。"
                                              "DeepSeek 官方接口还支持余额查询。支持分别修改生成与解读系统提示词，密钥按"
                                              "当前 Windows 用户加密保存。"
                                              "修改或保存配置不会发送请求；拉取、测试和查询需手动点击。"
                                              "连接测试仅发送简短 JSON 消息，可能产生少量费用。生成失败会继续请求原因分析。"
                                              "解读会发送所选数据集与本机统计结果。",
                                              c);
                                    paragraph(v, "help.ai.generate",
                                              "在 AI 辅助生成中描述设计、规格与指标要求，AI "
                                              "给出参数后由本机执行生成和"
                                              "指标复核。失败时反馈真实原因并请求建议，不自动无限重"
                                              "试；可继续对话调整，"
                                              "或按建议重试。对话仅保留在本次程序会话。",
                                              c);
                                    paragraph(v, "help.ai.interpret",
                                              "AI 解读使用当前生成结果或在图表页导入的 "
                                              "CSV。导入前先设置对应研究设计和分析方法；"
                                              "更换数据集后需新建解读会话。AI "
                                              "评价仅供参考，数值以本机结果为准，模拟数据不能"
                                              "替代现场测量证据。",
                                              c);
                                    paragraph(v, "help.ai.protocol",
                                              "生成和评价均要求完整 JSON，程序校验后显示可读内容；"
                                              "代码块、额外文字、未知字段或非法参数会被拒绝。错误卡"
                                              "片提供原因和恢复步骤。"
                                              "“重试本轮”重新请求 "
                                              "AI；“按建议重试”在确认诊断建议后启动本机生成，"
                                              "两者均由用户触发。",
                                              c);
                                    button(v, "help.ai.open", "打开 AI 助手", 160,
                                           [] { state.navigate(Page::Assistant); });
                                });
                            } else if (state.helpTopic == 0) {
                                section(v, "help.workflow", "开始使用", cw, [&](float c) {
                                    const auto step =
                                        [&](const std::string &id, const std::string &number,
                                            const std::string &title, const std::string &detail) {
                                            v.row(id + ".row")
                                                .width(c)
                                                .gap(12)
                                                .content([&] {
                                                    badge(v, id + ".number", number, 40);
                                                    v.column(id + ".content")
                                                        .width(c - 52)
                                                        .gap(4)
                                                        .content([&] {
                                                            text(v, id + ".title", title, c - 52,
                                                                 26, 15);
                                                            paragraph(v, id, detail, c - 52);
                                                        })
                                                        .build();
                                                })
                                                .build();
                                        };
                                    step("help.step1", "01", "定义研究",
                                         "在“选项 → "
                                         "研究”填写操作员数、每件重复测量次数、部件数和规格限，"
                                         "在“分析”选择分析方法。开启“规格内生成”时至少需要一个"
                                         "规格限；"
                                         "默认设置尚未填写规格或参考尺度，也可载入下方示例快速开始"
                                         "。");
                                    step("help.step2", "02", "设置生成条件",
                                         "在“数据”设置数据中心、生成优先级和指标约束。数据中心"
                                         "可留空；"
                                         "选择“不约束”的指标无需填写数值。测量值的小数位数在“选项 "
                                         "→ 研究”"
                                         "中设置。");
                                    step("help.step3", "03", "生成并复核",
                                         "点击“生成数据”，在“指标复核”检查实际值与接受范围，在“数据"
                                         "预览”检查"
                                         "测量记录。生成期间可查看日志或从底栏取消。满足生成约束不"
                                         "代表测量系统合格。");
                                    step("help.step4", "04", "导出与保存",
                                         "在“选项 → 导出”设置格式、目录及文件名，再点击“导出 "
                                         "CSV”。"
                                         "仅修改导出设置无需重新生成；修改研究、分析或生成参数后需"
                                         "重新生成。"
                                         "“保存配置”保存当前设置，不保存已生成的测量数据。");
                                    button(
                                        v, "sample", "载入示例设置", 156,
                                        [] {
                                            if (state.busy())
                                                return;
                                            state.sample();
                                            state.navigate(Page::Data);
                                        },
                                        false, state.busy());
                                });
                            }
                            if (state.helpTopic == 1) {
                                section(v, "help.analysis", "分析方法", cw, [&](float c) {
                                    paragraph(v, "help.analysis.select",
                                              "在“选项 → 分析”选择 Xbar-R（均值极差法）或 "
                                              "ANOVA（方差分析法）。"
                                              "两者均适用于平衡交叉量具 R&R "
                                              "研究：每名操作员测量全部部件，"
                                              "每个“操作员＋部件”组合的重复测量次数相同。估计方法不"
                                              "同，同组数据的结果可能不同。",
                                              c);
                                    text(v, "help.xbarr.title", "Xbar-R（均值极差法）", c, 28, 16);
                                    paragraph(
                                        v, "help.xbarr.components",
                                        "使用各“操作员＋部件”组合的极差估计重复性 EV；使用操作员"
                                        "均值的极差并扣除重复性影响估计再现性 "
                                        "AV；使用部件均值的极差估计"
                                        "部件间变异 PV。GRR 合并 EV 与 AV，TV 合并 GRR 与 PV；AV "
                                        "校正后的方差"
                                        "小于零时取零。",
                                        c);
                                    paragraph(v, "help.xbarr.scope",
                                              "不单独估计部件×操作员交互作用，不输出 ANOVA "
                                              "表或交互项 P 值。"
                                              "交互项设置、误分类概率和方差分量置信区间仅在 ANOVA "
                                              "方法下生效。",
                                              c);
                                    text(v, "help.anova.title", "ANOVA（方差分析法）", c, 28, 16);
                                    paragraph(v, "help.anova.model",
                                              "采用平衡交叉双随机效应模型，部件和操作员均为随机因子"
                                              "。通过平方和、"
                                              "自由度及均方估计重复性、操作员、部件和部件×操作员交"
                                              "互的方差分量。",
                                              c);
                                    paragraph(v, "help.anova.interaction",
                                              "交互项默认自动判断，显著性水平 "
                                              "α=0.05。完整模型的交互项 P<α 时保留；"
                                              "P≥α "
                                              "时，将交互项的平方和与自由度合并到重复性误差。也可选"
                                              "择始终保留或"
                                              "始终移除。P 值不可计算时，自动模式采用合并模型。",
                                              c);
                                    paragraph(
                                        v, "help.anova.components",
                                        "负方差分量逐项置零。再现性 AV "
                                        "包括操作员分量和保留的交互分量；"
                                        "量具 R&R（GRR）包括重复性与再现性。%Study Var "
                                        "为标准差之比，"
                                        "%Contribution（百分比贡献）为方差之比，两者不能混用。",
                                        c);
                                    paragraph(v, "help.anova.report",
                                              "两种方法均可在结果区复核指标，并在“分析详情”查看分析"
                                              "口径。ANOVA 还显示"
                                              "最终模型、完整模型交互项 P 值、ANOVA "
                                              "表及方差分量。CLI 的 --report 保存分析结果，"
                                              "--input CSV 仅分析已有数据。GUI "
                                              "改变分析方法或交互项设置后需重新生成。",
                                              c);
                                    paragraph(
                                        v, "help.anova.software",
                                        "跨软件对标须使用相同的完整记录和分析方法，并统一研究变异倍"
                                        "数、规格限及容差。"
                                        "ANOVA 还须统一随机效应模型、交互项、EMS "
                                        "均方估计和负分量处理规则。"
                                        "REML、Bayesian 或固定效应 ANOVA "
                                        "的结果不能直接视为同一统计口径。"
                                        "Trial 与 StdOrder 用于追溯记录，均不作为额外分析因子。",
                                        c);
                                    button(v, "help.analysis.open", "前往分析选项", 160, [] {
                                        state.optionsTab = 1;
                                        state.configOffset = 0;
                                        state.navigate(Page::Options);
                                    });
                                });
                                section(
                                    v, "help.process", "分析口径、历史参数与置信区间", cw,
                                    [&](float c) {
                                        text(v, "help.process.tolerance.title",
                                             "P/T：量具变异占容差的比例", c, 26, 15);
                                        paragraph(
                                            v, "help.process.tolerance",
                                            "填写独立分析容差时：P/T%=100×k×GRR_SD/"
                                            "容差；留空且有双侧规格时，"
                                            "容差取 "
                                            "USL−LSL。只有单侧规格时，P/T%=100×(k/2)×GRR_SD/"
                                            "|规格限−均值|。"
                                            "k "
                                            "为研究变异倍数；距离为零时不可计算。历史均值留空"
                                            "时使用实际测量均值。"
                                            "独立分析容差只影响分析，不替代“规格内生成”所使用的规"
                                            "格限。",
                                            c);
                                        text(v, "help.process.history.title",
                                             "历史标准差与过程变异来源", c, 26, 15);
                                        paragraph(
                                            v, "help.process.history",
                                            "填写历史标准差并将过程变异估计来源设为“历史标准差”后，"
                                            "历史 SD≥GRR_SD "
                                            "时，TV_SD 取历史 SD，"
                                            "PV_SD=√(TV_SD²−GRR_SD²)；历史 SD 小于 GRR_SD "
                                            "时保留研究估计。"
                                            "选择“研究中的部件”时始终保留研究的 "
                                            "TV/PV。填写历史标准差后，另输出"
                                            "%Process，表示各分量标准差相对历史标准差的百分比。",
                                            c);
                                        text(v, "help.process.classification.title",
                                             "误分类概率（仅 ANOVA）", c, 26, 15);
                                        paragraph(v, "help.process.classification",
                                                  "在“统计选项”开启“计算误分类概率”；至少需要一个规"
                                                  "格限，并假设真实部件与测量误差相互"
                                                  "独立且服从正态分布。"
                                                  "联合概率与条件概率分别报告；条件事件的概率为零时"
                                                  "，条件概率不可计算。"
                                                  "历史 SD≥GRR_SD "
                                                  "时使用历史过程波动，否则使用研究部件变异；"
                                                  "这一规则独立于 %Study Var 的过程变异来源设置。",
                                                  c);
                                        text(v, "help.process.confidence.title",
                                             "置信区间（仅 ANOVA）", c, 26, 15);
                                        paragraph(v, "help.process.confidence",
                                                  "在“统计选项”开启“计算置信区间”，选择估计方法、置"
                                                  "信水平及置信限类型。支持方差分量、标准差、研究变"
                                                  "异、P/T 和 %Process "
                                                  "的双侧或单侧置信限。"
                                                  "重复性采用卡方精确区间；其他分量默认采用 MLS "
                                                  "近似，也可选择 JMP EMS 使用的"
                                                  "Satterthwaite "
                                                  "近似。MLS 的最低适用水平取决于操作员人数和单/双侧"
                                                  "类型，超出范围时会提示下界；需要更低水平时请显式"
                                                  "选择 Satterthwaite，程序不会自动更换方法。"
                                                  "零方差或边界情形的部分置信限可能不可计算，"
                                                  "显示为“—”。"
                                                  "当前不提供 %Contribution、%Study Var 和 ndc "
                                                  "的比值置信区间，不能将分量置信限直接相除替代。",
                                                  c);
                                        paragraph(
                                            v, "help.process.compatibility",
                                            "对标时还需统一均值、历史标准差、过程变异来源、置信水平"
                                            "和区间算法。"
                                            "MATLAB 原生应用将缺失侧视为无穷规格，与本程序的单侧 "
                                            "P/T 定义不同。"
                                            "各软件的默认模型、估计方法和 ndc 取整规则也可能不同。",
                                            c);
                                    });
                            }
                            if (state.helpTopic == 0 || state.helpTopic == 1) {
                                section(v, "help.constraints", "约束模式与单位", cw, [&](float c) {
                                    const std::array<std::pair<std::string, std::string>, 5> rows{
                                        {{"不约束", "不以该指标筛选数据，仍显示其实际计算结果。"},
                                         {"目标值", "目标值 ± 允许误差；误差留空时采用默认误差。"},
                                         {"区间",
                                          "接受下限至上限之间的值，含两端；不额外叠加误差。"},
                                         {"不高于", "实际值不超过填写的阈值。"},
                                         {"不低于", "实际值不低于填写的阈值。"}}};
                                    for (size_t i = 0; i < rows.size(); ++i)
                                        v.row("help.mode" + std::to_string(i))
                                            .width(c)
                                            .gap(12)
                                            .content([&] {
                                                badge(v, "help.mode.name" + std::to_string(i),
                                                      rows[i].first, 84);
                                                paragraph(v, "help.mode.desc" + std::to_string(i),
                                                          rows[i].second, c - 96);
                                            })
                                            .build();
                                    paragraph(
                                        v, "help.units",
                                        "EV/TV%、AV/TV%、GRR/TV% 以总变异的标准差 TV_SD "
                                        "为分母，对应 Minitab 的"
                                        " %Study Var。百分比指标的允许误差以“百分点”为单位，例如 "
                                        "10±0.1 表示"
                                        "接受 "
                                        "9.90%～10.10%"
                                        "。ndc（可区分类别数）的目标值与允许误差均使用整数。"
                                        "误差填 0 表示零误差，留空则使用默认允许误差。",
                                        c);
                                    paragraph(v, "help.sd",
                                              "EV（重复性）、AV（再现性）、GRR（量具 "
                                              "R&R）、PV（部件间变异）和"
                                              " TV（总变异）以标准差 SD（1σ）表示。研究变异为 "
                                              "k×SD，k 默认取 6。"
                                              "研究变异倍数影响研究变异及 P/T，不改变标准差本身。",
                                              c);
                                    paragraph(v, "help.display",
                                              "指标复核中的百分比、目标范围与偏差显示两位小数，ndc "
                                              "显示整数；约束判定仍使用原始精度。数据预览和 CSV "
                                              "保持配置指定的小数位。",
                                              c);
                                });
                            }
                            if (state.helpTopic == 0) {
                                section(v, "help.rules", "生成规则与尺度", cw, [&](float c) {
                                    paragraph(v, "help.priority",
                                              "中心优先和指标优先决定候选数据的优化侧重，最终结果都"
                                              "必须满足已设置的指标约束。"
                                              "开启“规格内生成”时，全部测量值必须位于已设置的规格限"
                                              "内；关闭后允许超出规格限。",
                                              c);
                                    paragraph(
                                        v, "help.reference",
                                        "缺少双侧规格或显式选择参考尺度时，需要提供参考总变异标准"
                                        "差（TV_SD）或历史标准差。"
                                        "数据生成种子位于“选项 → 研究 → "
                                        "数据随机性与搜索”：固定种子在相同程序版本"
                                        "、相同参数且搜索未被"
                                        "超时截断时可复现；留空时每次生成自动选择种子。搜索超"
                                        "时或次数耗尽不代表数学上无解。",
                                        c);
                                });
                            }
                            if (state.helpTopic == 2) {
                                section(v, "help.files", "配置与数据文件", cw, [&](float c) {
                                    paragraph(v, "help.config", "用户配置文件：", c);
                                    paragraph(v, "help.path",
                                              platform::compatibilityConfig().u8string(), c, ink);
                                    paragraph(v, "help.silent",
                                              "“保存配置”写入以上位置，启动时自动读取。CSV "
                                              "使用“选项 → 导出”的目录和文件名；"
                                              "目录留空或使用相对路径时，以用户配置文件所在目录为基"
                                              "准。缺失的输出目录自动创建。"
                                              "同名文件默认追加序号，也可选择覆盖。保存、导出和复制"
                                              "的结果在固定底部状态栏提示，"
                                              "详细信息及错误原因见日志。",
                                              c);
                                    text(v, "help.csv.wide.title", "默认格式 · 带标记的宽表", c, 26,
                                         15);
                                    paragraph(v, "help.csv",
                                              "左上角留空，首行为从 1 开始的部件编号，首列为 "
                                              "A、B、C 等操作员标记。"
                                              "每行包含一名操作员在一轮重复测量中对全部部件的测量值"
                                              "，按操作员 → 重复轮次排列。"
                                              "使用 UTF-8 BOM；运行顺序设置不适用于该布局。",
                                              c);
                                    text(v, "help.csv.long.title", "五种软件格式 · 每行一次测量", c,
                                         26, 15);
                                    paragraph(v, "help.csv.encoding",
                                              "Minitab、SPSS、JMP 19、Origin、MATLAB "
                                              "共用同一种长表布局，使用 UTF-8 无 BOM。"
                                              "所有 CSV 格式均使用英文逗号、CRLF "
                                              "换行和英文小数点，测量值保留配置指定的小数位数。",
                                              c);
                                    button(v, "help.files.open", "前往导出设置", 160, [] {
                                        state.optionsTab = 2;
                                        state.configOffset = 0;
                                        state.navigate(Page::Options);
                                    });
                                });
                                section(v, "help.runorder", "CSV 格式与运行顺序", cw, [&](float c) {
                                    paragraph(v, "help.runorder.format",
                                              "在“选项 → 导出”选择 CSV "
                                              "导出格式；所有导出入口均使用该设置。"
                                              "五种长表的列名统一为 "
                                              "Operator,Part,Trial,Measurement，分别表示操作员、"
                                              "部件、重复测量编号与测量值。操作员标记为 "
                                              "A、B、C，Part 和 Trial 从 1 开始。"
                                              "默认宽表无法表达逐条测量的随机运行顺序，因此相关控件"
                                              "禁用。",
                                              c);
                                    paragraph(
                                        v, "help.runorder.modes",
                                        "不随机化：按 Trial → Part → Operator 的标准序排列。\n"
                                        "随机化所有运行顺序：打乱全部完整记录，允许操作员、部件和 "
                                        "Trial 混排。\n"
                                        "在操作员内部随机化：逐 Trial、逐操作员安排测量，在每个 "
                                        "Trial＋Operator "
                                        "组合内独立打乱部件顺序。",
                                        c);
                                    paragraph(v, "help.runorder.operators",
                                              "“随机化操作员顺序”仅第三种模式可用：每项研究随机确定"
                                              "一次操作员顺序，"
                                              "所有 Trial "
                                              "沿用这一顺序。随机结果也可能与原操作员顺序相同。",
                                              c);
                                    paragraph(v, "help.runorder.seed",
                                              "运行顺序随机种子与数据生成种子相互独立，仅在随机化模"
                                              "式下可设置。允许留空，填"
                                              "写时须为 "
                                              "1～9223372036854775807 的整数。"
                                              "相同数据、运行顺序设置和排序种子可复现同一排列，但不"
                                              "保证与 Minitab "
                                              "的相同种子逐行一致。留空时为本项研究自动选择种子；同"
                                              "一研究以相同运行顺序"
                                              "设置导出到五种软件时，复用同一排列。",
                                              c);
                                    paragraph(v, "help.runorder.standard",
                                              "勾选“在导出文件中存储标准运行顺序”后，在首列添加 "
                                              "StdOrder，记录随机化前的 1～N 标准序号。"
                                              "CSV 行序就是实际运行顺序；Trial "
                                              "始终是真实重复测量编号，不随随机排序重编号。"
                                              "随机化仅移动完整记录，不改变测量值、字段对应关系或数"
                                              "据精度。"
                                              "对同一种分析方法，随机化前后的统计结果保持一致，允许"
                                              "浮点末位差异。"
                                              "修改导出格式或运行顺序无需重新生成数据；保存配置、启"
                                              "动加载和恢复"
                                              "默认均包含这些设置。",
                                              c);
                                    paragraph(v, "help.runorder.identity",
                                              "标准序号：((Trial − 1) × 部件数 + Part − 1) × "
                                              "操作员数 + 操作员编号；"
                                              "A=1、B=2、C=3。总记录数 "
                                              "N=操作员数×每件重复测量次数×部件数。"
                                              "同一组数据比较不同运行顺序时，直接更改导出设置后再次"
                                              "导出；不要重新"
                                              "生成。若要多次生成相同数据，请固定“研究 → "
                                              "数据随机性与搜索”中的数据生成种子。",
                                              c);
                                });
                                section(
                                    v, "help.minitab", "导入 Minitab 与结果对标", cw, [&](float c) {
                                        paragraph(v, "help.minitab.import",
                                                  "1. 选择“Minitab”长表并导出，在 Minitab 打开 "
                                                  "CSV，首行为列名。"
                                                  "选择“统计 → 质量工具 → 量具研究 → 交叉量具 "
                                                  "R&R”。部件选 Part，"
                                                  "操作员选 Operator，测量数据选 Measurement。",
                                                  c);
                                        paragraph(
                                            v, "help.minitab.method",
                                            "2. 分析方法与本程序一致：Xbar 和 R 或 "
                                            "ANOVA；ANOVA 还须统一交互项设置。研究变异倍数默认 6。"
                                            "比较 P/T "
                                            "时，规格、独立容差及均值等设置须一致。EV/TV%、AV/TV%、"
                                            "GRR/TV% 对应 %Study Var，P/T% 对应总量具 R&R 的 "
                                            "%Tolerance。",
                                            c);
                                        paragraph(v, "help.minitab.columns",
                                                  "3. Trial 保留原重复测量编号；StdOrder "
                                                  "供追溯标准序。两列均不作为"
                                                  "测量响应或额外因子。Minitab "
                                                  "按操作员和部件识别重复测量。"
                                                  "三种运行顺序只重排完整记录，统计结果允许浮点末位"
                                                  "差异，ndc 应一致。",
                                                  c);
                                        paragraph(v, "help.minitab.evidence",
                                                  "已用 Minitab 22.6 实测：同一份 "
                                                  "3人×3次×10件数据只生成一次，三种"
                                                  "顺序各导出 90 条，Trial、StdOrder、测量值及 21 "
                                                  "项统计核对均通过。"
                                                  "相同随机种子不保证与 Minitab 产生相同排列。",
                                                  c);
                                    });
                            }
                            if (state.helpTopic == 4) {
                                section(v, "help.cli", "CLI 命令行使用", cw, [&](float c) {
                                    text(v, "help.cli.start.title", "查看参数与读取配置", c, 26,
                                         15);
                                    paragraph(
                                        v, "help.cli.config",
                                        "msa_cli.exe 支持 GUI 的全部业务配置。优先级：内置默认值 → "
                                        "JSON "
                                        "配置 → 命令行覆盖；重复参数最后一次生效。GUI "
                                        "保存的配置可用 "
                                        "--config 指定；未指定配置文件时，自动读取 GUI 用户配置。"
                                        "普通 CLI 运行不会把命令行覆盖值写回配置文件。",
                                        c);
                                    paragraph(v, "help.cli.syntax",
                                              "运行 msa_cli.exe --help "
                                              "查看完整参数及示例。--no-config 跳过用户配置；"
                                              "可选数值用 null 清空，布尔选项用 "
                                              "true/false；指标约束可用 none、"
                                              "target:值[:误差]、between:下限:上限、at_most:值、at_"
                                              "least:值。"
                                              "指标参数会整项替换旧约束。",
                                              c);
                                    text(v, "help.cli.example.title",
                                         "示例：生成随机运行顺序的长表", c, 26, 15);
                                    paragraph(v, "help.cli.example",
                                              "msa_cli.exe --no-config --lsl 9 --usl 11 --seed 123 "
                                              "--csv-format minitab --run-order all --run-seed 321 "
                                              "--store-standard-order true --output data.csv "
                                              "--report result.json",
                                              c);
                                    text(v, "help.cli.analyze.title", "仅分析已有数据", c, 26, 15);
                                    paragraph(v, "help.cli.analyze",
                                              "使用 --input CSV "
                                              "读取已有宽表或长表，按操作员与部件标识恢复测量记录。"
                                              "配合 --method crossed_anova 或 crossed_xbarr "
                                              "选择方法，并使用 --report 保存分析报告。"
                                              "此模式不生成数据、不导出 CSV，不能与 --output "
                                              "同用；研究设计须与输入数据一致。",
                                              c);
                                    text(v, "help.cli.files.title", "输出路径与退出状态", c, 26,
                                         15);
                                    paragraph(
                                        v, "help.cli.files",
                                        "生成模式省略 --output "
                                        "时使用配置中的输出目录、文件名模板和同名处理方式。显式 "
                                        "--output "
                                        "是完整文件路径，不展开变量；覆盖显式 CSV 或报告须加 "
                                        "--overwrite。"
                                        "--overwrite 始终优先于 "
                                        "--existing-file，与参数书写顺序无关。--report "
                                        "保存最终配置、"
                                        "测量矩阵、指标及实际生成种子。Ctrl+C 可取消；退出码 0 "
                                        "表示成功，1 表示配置、生成、"
                                        "文件错误或取消，2 表示参数错误或显式同名文件预检查失败。",
                                        c);
                                });
                            }
                            if (state.helpTopic == 3) {
                                section(v, "help.filename", "导出文件名变量", cw, [&](float c) {
                                    paragraph(v, "help.filename.syntax",
                                              "方括号表示变量，区分大小写；括号外的中文、空格和符号"
                                              "按原文保留。"
                                              "月份使用 [MM]；时、分、秒使用 [H]、[M]、[S]；[MS] "
                                              "为毫秒，[TIMESTAMP] 为 Unix 秒数。"
                                              "[[ 和 ]] 分别输出字面量方括号。变量只用于 CSV "
                                              "文件名，目录不展开。"
                                              "点击下方变量名称即可复制（含方括号）；英文和中文名称"
                                              "可分别复制。",
                                              c);
                                    for (const auto &variable : msa::filename_variables()) {
                                        const std::string name = variable.name;
                                        if (name == "YYYY")
                                            text(v, "help.filename.date", "日期与时间", c, 26, 15);
                                        else if (name == "OPERATORS")
                                            text(v, "help.filename.study", "研究参数", c, 26, 15);
                                        else if (name == "EV_PCT")
                                            text(v, "help.filename.metrics", "实际指标", c, 26, 15);
                                        else if (name == "VERSION")
                                            text(v, "help.filename.version", "程序信息", c, 26, 15);
                                        filenameVariableHelp(v, variable, c);
                                    }
                                    paragraph(
                                        v, "help.filename.example",
                                        "MSA_Data_[YYYY]-[MM]-[DD]_[H]-[M]-[S].csv → "
                                        "MSA_Data_2026-10-24_22-08-12.csv\n"
                                        "研究_[人员数量]人_[测量次数]次_[产品件数]件_GRR[GRR_PCT]_["
                                        "DATETIME].csv",
                                        c);
                                    paragraph(
                                        v, "help.filename.values",
                                        "日期和时钟变量采用导出开始时的电脑本地时间，同次导出共用同"
                                        "一时刻；"
                                        "Unix 时间戳从 1970-01-01 UTC 起算，不随本地时区改变。"
                                        "研究参数和指标取已生成数据的配置及实际复核值，随机种子取本"
                                        "次实际种子。"
                                        "百分比指标保留两位、不含百分号；规格未设置、P/T 或 ndc "
                                        "不可计算时写 "
                                        "NA。"
                                        "中心留空时取实际均值；修改文件名不改变数据或计算精度。",
                                        c);
                                    paragraph(
                                        v, "help.filename.collision",
                                        "追加序号为默认策略：先使用原文件名，重名时在扩展名前追加 "
                                        "_001、_002 等。"
                                        "选择覆盖文件时才替换原文件；写入过程中使用原子保存。"
                                        "配置保存的是模板原文。未知变量、括号错误、非法字符、Window"
                                        "s "
                                        "保留名或文件名过长时停止保存或导出，"
                                        "底部显示简短失败提示，详细原因见日志。",
                                        c);
                                    button(v, "help.filename.open", "编辑文件名模板", 176, [] {
                                        state.optionsTab = 2;
                                        state.configOffset = 0;
                                        state.navigate(Page::Options);
                                    });
                                });
                            }
                            if (state.helpTopic == 1) {
                                section(v, "help.native", "适用范围与结果校验", cw, [&](float c) {
                                    paragraph(v, "help.boundary",
                                              "支持 2 或 3 名操作员、每件重复测量 2 或 3 次、5～10 "
                                              "个部件，共 24 种平衡交叉设计。"
                                              "当前不支持嵌套设计、非平衡数据、固定或混合效应模型，"
                                              "以及 REML、ML、MINQUE 或 Bayesian 估计。",
                                              c);
                                    paragraph(
                                        v, "help.compatibility",
                                        "配置采用 schema_version=1，缺失的可选字段按默认值补齐。"
                                        "固定种子在相同程序版本和参数下可复现；搜索时限可能使搜索提"
                                        "前结束。"
                                        "找到满足约束的结果后，最多继续优化 120 次。",
                                        c);
                                    paragraph(v, "help.verify",
                                              "候选测量值先按小数位数舍入，再进行指标复核。导出时校"
                                              "验序列化结果，"
                                              "保存后回读实际 "
                                              "CSV，再次检查测量值精度、记录数量和字段对应关系。"
                                              "长表保留所选运行顺序，不在写出时重新排序。",
                                              c);
                                    paragraph(v, "help.failures",
                                              "程序分别报告参数错误、已证明的约束冲突和有限搜索失败"
                                              "。超时或次数耗尽"
                                              "不代表数学上无解。修改研究或生成参数会使旧结果失效；"
                                              "导出设置独立。",
                                              c);
                                });
                            }
                        })
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
                    .color("#FFFFFF")
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
void aboutRow(Ui &u, const std::string &id, const std::string &symbol, const std::string &title,
              const std::string &value, const std::string &valueId, float w, bool divider,
              std::function<void()> action = {}, bool disabled = false,
              const std::string &actionLabel = "打开 ↗") {
    if (divider)
        u.rect(id + ".divider").size(w, 1).color(line).build();
    const float detailWidth = w - 52 - (action ? 108 : 0);
    u.row(id)
        .width(w)
        .padding(0, 8)
        .gap(12)
        .alignItems(eui::Align::CENTER)
        .content([&] {
            u.stack(id + ".icon")
                .size(40, 40)
                .content([&] {
                    u.rect(id + ".icon.bg").size(40, 40).radius(20).color("#EDF2FF").build();
                    u.text(id + ".icon.text")
                        .size(40, 40)
                        .text(symbol)
                        .fontSize(20)
                        .color(blue)
                        .horizontalAlign(eui::HorizontalAlign::Center)
                        .verticalAlign(eui::VerticalAlign::Center)
                        .build();
                })
                .build();
            u.column(id + ".details")
                .width(detailWidth)
                .gap(3)
                .content([&] {
                    paragraph(u, id + ".title", title, detailWidth, ink, 20);
                    paragraph(u, valueId, value, detailWidth, muted, 17);
                })
                .build();
            if (action)
                button(u, id + ".open", actionLabel, 96, action, false, disabled, 36);
        })
        .build();
}
void aboutGroup(Ui &u, const std::string &id, const std::string &title, float w,
                const std::function<void(float)> &content) {
    u.column(id + ".group")
        .width(w)
        .gap(10)
        .content([&] {
            text(u, id + ".title", title, w, 28, 17);
            components::card(u, id + ".card")
                .width(w)
                .wrapContentHeight()
                .theme(tokens())
                .shadow(cardShadow())
                .padding(16)
                .radius(14)
                .color("#FFFFFF")
                .border(1, line)
                .content([&] {
                    u.column(id + ".rows")
                        .width(w - 32)
                        .gap(8)
                        .content([&] { content(w - 32); })
                        .build();
                })
                .build();
        })
        .build();
}
void aboutPage(Ui &u, float w, float h) {
    components::scrollView(u, "aboutscroll")
        .size(w, h)
        .offset(state.aboutOffset)
        .theme(tokens())
        .scrollbarGap(8)
        .reserveScrollbar()
        .onChange([](float offset) {
            state.aboutOffset = offset;
            menu.id.clear();
        })
        .content([](Ui &v, float cw, float) {
            const float contentWidth = std::min(cw, 960.f);
            v.row("about.center")
                .width(cw)
                .justifyContent(eui::Align::CENTER)
                .content([&] {
                    v.column("about.page")
                        .width(contentWidth)
                        .gap(18)
                        .content([&] {
                            v.column("about.hero")
                                .width(contentWidth)
                                .padding(0, 18)
                                .gap(12)
                                .alignItems(eui::Align::CENTER)
                                .content([&] {
                                    appMark(v, "about.mark", 64);
                                    v.text("about.title")
                                        .size(contentWidth, 40)
                                        .text("MSA 数据生成器")
                                        .fontSize(28)
                                        .color(ink)
                                        .horizontalAlign(eui::HorizontalAlign::Center)
                                        .verticalAlign(eui::VerticalAlign::Center)
                                        .build();
                                    v.row("about.badges")
                                        .width(contentWidth)
                                        .gap(10)
                                        .justifyContent(eui::Align::CENTER)
                                        .content([&] {
                                            badge(v, "about.version",
                                                  std::string("v") + MSA_UI_VERSION, 86, "#FFFFFF",
                                                  blue);
                                            badge(v, "about.license.badge", msa::project_license_id,
                                                  64, muted, "#E9EDF5");
                                        })
                                        .build();
                                    v.text("about.purpose")
                                        .width(std::min(contentWidth, 680.f))
                                        .text("交叉量具 R&R 模拟测量数据生成与复核")
                                        .wrap()
                                        .fontSize(18)
                                        .color(muted)
                                        .horizontalAlign(eui::HorizontalAlign::Center)
                                        .build();
                                    button(v, "copyversion",
                                           state.versionCopied ? "已复制版本信息" : "复制版本信息",
                                           164, [] {
                                               safe([] {
                                                   platform::clipboard(platform::wide(
                                                       std::string("MSA 数据生成器 v") +
                                                       MSA_UI_VERSION +
                                                       "\r\nWindows x64 / EUI-NEO "
                                                       "0.6.1\r\n交叉量具 R&R：Xbar-R / ANOVA"
                                                       "\r\n作者：" +
                                                       msa::project_author + "\r\n项目仓库：" +
                                                       msa::project_repository + "\r\n项目许可：" +
                                                       msa::project_license));
                                                   state.versionCopied = true;
                                                   state.hint("版本信息已复制");
                                               });
                                           });
                                })
                                .build();
                            aboutGroup(
                                v, "about.project", "项目与许可", contentWidth, [&](float c) {
                                    aboutRow(
                                        v, "about.author.row", "@", "作者", msa::project_author,
                                        "about.author", c, false,
                                        [] {
                                            safe([] {
                                                platform::openWebsite(
                                                    L"https://github.com/print-yuhuan");
                                                state.hint("已打开作者主页");
                                            });
                                        },
                                        false, "作者主页 ↗");
                                    aboutRow(
                                        v, "about.repository.row", "</>", "项目仓库",
                                        msa::project_repository, "about.repository", c, true,
                                        [] {
                                            safe([] {
                                                platform::openWebsite(
                                                    platform::wide(msa::project_repository));
                                                state.hint("已打开项目仓库");
                                            });
                                        },
                                        false, "项目仓库 ↗");
                                    const auto licenseDir =
                                        platform::executableDirectory() / "licenses";
                                    std::error_code licenseError;
                                    const bool licenseAvailable =
                                        std::filesystem::is_directory(licenseDir, licenseError);
                                    aboutRow(
                                        v, "about.license.row", "§", msa::project_license,
                                        "允许商业使用、修改和分发；分发副本或实质部分时保留版权声明"
                                        "与许可声明。",
                                        "about.projectlicense", c, true,
                                        [licenseDir] {
                                            safe([licenseDir] {
                                                platform::openDirectory(licenseDir);
                                                state.hint("已打开许可目录");
                                            });
                                        },
                                        !licenseAvailable, "许可目录");
                                    if (!licenseAvailable)
                                        paragraph(
                                            v, "about.license.location",
                                            "当前程序目录未找到 licenses "
                                            "文件夹。许可全文随完整发布包提供；分发时请一并保留。",
                                            c, muted, 17);
                                });
                            aboutGroup(
                                v, "about.dependencies", "组件与许可", contentWidth, [&](float c) {
                                    aboutRow(v, "about.framework", "UI", "EUI-NEO 0.6.1",
                                             "界面框架 · Apache-2.0", "about.euiversion", c, false);
                                    aboutRow(v, "about.font", "Aa", "Noto Sans CJK SC 2.004",
                                             "中文字体 · OFL-1.1", "about.fontlicense", c, true);
                                    aboutRow(v, "about.json", "{}", "nlohmann/json 3.12.0",
                                             "配置与报告 · MIT", "about.jsonlicense", c, true);
                                });
                            v.row("about.platforms")
                                .width(contentWidth)
                                .gap(8)
                                .justifyContent(eui::Align::CENTER)
                                .content([&] {
                                    badge(v, "about.platform", "Windows x64", 128, muted,
                                          "#E9EDF5");
                                    badge(v, "about.language", "C++17", 76, muted, "#E9EDF5");
                                    badge(v, "about.renderer", "OpenGL 3.3", 124, muted, "#E9EDF5");
                                })
                                .build();
                            v.text("about.dpi")
                                .width(contentWidth)
                                .text("窗口 DPI " + std::to_string(state.systemDpi) +
                                      " · 界面缩放 " + number(state.pixelScale * 100, 0) +
                                      "% · 内嵌中文字体")
                                .fontSize(16)
                                .wrap()
                                .maxWidth(contentWidth)
                                .color(muted)
                                .horizontalAlign(eui::HorizontalAlign::Center)
                                .build();
                            paragraph(v, "about.freetype",
                                      "Portions of this software are copyright © 2024 The FreeType "
                                      "Project "
                                      "(www.freetype.org). All rights reserved.",
                                      contentWidth, muted, 14);
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
                      .size(w, 40)
                      .text(showHint ? state.footerMessage : fallback)
                      .fontSize(17.55f)
                      .color(!showHint                             ? muted
                             : state.footerLevel == Level::Error   ? "#AF3525"
                             : state.footerLevel == Level::Warning ? "#926014"
                                                                   : green)
                      .verticalAlign(eui::VerticalAlign::Center)
                      .clip();
    if (showHint)
        status.onFrame([](float) {});
    status.build();
}
// 新页面共享现有主题、状态栏与本地统计结果。
#include "gui_visualization.hpp"
#include "gui_ai.hpp"
void footer(Ui &u, float w) {
    if (state.page == Page::Assistant) {
        assistantFooter(u, w);
        return;
    }
    if (state.page == Page::Charts && !state.busy()) {
        footerStatus(u, w - 320, chartFooterSummary());
        if (visual.editDirty[visual.selected])
            button(u, "charts.apply", "应用编辑", 132, [] { applyChartEdits(visual.selected); });
        else
            button(u, "charts.askai", "AI 解读", 132, [] {
                aiUi.mode = 1;
                state.navigate(Page::Assistant);
            });
        button(u, "charts.export", visual.busy() ? "导出中…" : "导出图表", 164, exportVisuals, true,
               visual.busy() || !insightResult() || selectedChartCount() == 0);
        return;
    }
    if (state.page == Page::Data) {
        const std::string fallback =
            state.busy()
                ? (w >= 760 ? "已搜索候选 " + std::to_string(state.ticks.load()) + " / " +
                                  state.draft.attempts + " · " + number(state.elapsed) + " 秒"
                            : std::to_string(state.ticks.load()) + " 次 · " +
                                  number(state.elapsed) + " 秒")
            : state.retainedResult() ? "保留上次成功结果，可复核或导出"
            : !state.message.empty() ? "操作未完成，请查看页面提示或运行日志"
            : state.validResult()    ? (w >= 760 ? "数据已生成，可复核并导出 CSV" : "数据已生成")
            : state.result && state.resultRevision != state.revision ? "输入已修改，请重新生成"
            : w >= 760                                               ? "准备好后，开始生成测量数据"
                                                                     : "准备就绪";
        footerStatus(u, w - 360, fallback);
        button(
            u, "generate",
            state.status == Status::Cancelling ? "取消中…"
            : state.busy()                     ? "取消生成"
            : state.validResult()              ? "重新生成"
                                               : "生成数据",
            160,
            [] {
                if (state.busy()) {
                    state.status = Status::Cancelling;
                    state.cancel = true;
                } else
                    state.start();
            },
            state.busy() || !state.validResult(),
            state.status == Status::Cancelling || aiNetworkBusy());
        button(
            u, "export", "导出 CSV", 176, [] { exportData(); }, state.validResult(),
            !state.validResult());
    } else if (state.busy()) {
        footerStatus(u, w - 284, "生成中 · " + number(state.elapsed) + " 秒");
        button(u, "showtask", state.page == Page::Data ? "查看日志" : "返回数据", 140,
               [] { state.navigate(state.page == Page::Data ? Page::Logs : Page::Data); });
        button(
            u, "cancel", state.status == Status::Cancelling ? "取消中…" : "取消", 120,
            [] {
                state.status = Status::Cancelling;
                state.cancel = true;
            },
            true, state.status == Status::Cancelling);
    } else if (state.page == Page::Options) {
        if (state.optionsTab == 3) {
            footerStatus(u, w - 308,
                         aiSettingsDirty() ? "AI 配置尚未保存" : "AI 配置已保存 / 使用默认值");
            button(u, "ai.defaults", "恢复 AI 默认", 124, resetAiSettings, false, aiNetworkBusy());
            button(u, "ai.save", "保存 AI 配置", 160, saveAiSettings, true, aiNetworkBusy());
            return;
        }
        footerStatus(u, w - 308, state.configDirty ? "有未保存的配置更改" : "当前配置已保存");
        button(u, "defaults", "恢复默认", 124, [] {
            state.draft = Draft{};
            state.changed();
            state.status = Status::Ready;
            state.message.clear();
            state.feedback("已恢复默认，尚未保存", "已恢复默认；尚未写入任何配置文件。");
            state.configOffset = 0;
        });
        button(u, "saveconfig", "保存配置", 160, [] { saveSettings(); }, true);
    } else {
        footerStatus(u, w - 132, std::string("MSA 数据生成器 · v") + MSA_UI_VERSION);
        button(u, "returntask", "返回数据", 120, [] { state.navigate(Page::Data); });
    }
}
void compose(Ui &u, const eui::Screen &screen) {
    state.poll();
    pollExtensions();
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
        if (previousPage != state.page) {
            state.pageEntered = Clock::now();
            previousPage = state.page;
        }
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
    const float pad = screen.width < 1120 ? 16.0f : 24.0f, c = screen.width - pad * 2,
                h = std::max(120.0f, screen.height - 176);
    u.rect("header.surface").size(screen.width, 64).color("#FFFFFF").build();
    u.rect("footer.surface")
        .position(0, screen.height - 64)
        .size(screen.width, 64)
        .color("#FFFFFF")
        .border(1, line)
        .build();
    u.column("shell")
        .size(screen.width, screen.height)
        .content([&] {
            u.row("header")
                .size(screen.width, 64)
                .padding(pad, 10)
                .gap(14)
                .alignItems(eui::Align::CENTER)
                .content([&] {
                    u.row("brandgroup")
                        .size(312, 44)
                        .gap(14)
                        .alignItems(eui::Align::CENTER)
                        .content([&] {
                            appMark(u, "brand.mark", 40);
                            u.text("brand")
                                .height(42)
                                .text("MSA")
                                .fontSize(29)
                                .color(blue)
                                .verticalAlign(eui::VerticalAlign::Center)
                                .build();
                            u.text("title")
                                .height(42)
                                .text("数据生成器")
                                .fontSize(26)
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
                    badge(u, "header.status",
                          state.busy()
                              ? (state.status == Status::Cancelling ? "● 取消中" : "● 正在生成")
                          : !state.message.empty()            ? "● 需要处理"
                          : state.retainedResult()            ? "● 保留上次结果"
                          : state.validResult()               ? "● 已生成"
                          : state.status == Status::Cancelled ? "● 已取消"
                                                              : "● 就绪",
                          140,
                          !state.message.empty() ? "#AF3525"
                          : state.validResult()  ? green
                                                 : blue,
                          !state.message.empty() ? "#FFF0ED"
                          : state.validResult()  ? "#E8F6F0"
                                                 : "#EDF2FF");
                })
                .build();
            u.row("navigation")
                .size(screen.width, 48)
                .padding(pad, 4)
                .gap(8)
                .content([&] {
                    tabs(
                        u, "pages",
                        {"数据", "图表", "助手", "日志", "选项", "帮助", "关于"},
                        static_cast<int>(state.page), std::min(c, 900.0f),
                        [](int i) {
                            state.navigate(static_cast<Page>(i));
                            menu.id.clear();
                        },
                        40);
                })
                .build();
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
                    case Page::Assistant:
                        assistantPage(u, c, h);
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
                .size(screen.width, 64)
                .padding(pad, 12)
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
                gui::loadExtensions();
            })
            .onShutdown([] {
                gui::stopExtensions();
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
