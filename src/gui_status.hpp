// 由 gui.cpp 在 gui 命名空间、State 与 VisualState 定义之后包含。
// 顶栏描述持续的业务状态；保存、复制等短反馈仍只在底部状态栏显示。
enum class HeaderStatusTone { Neutral, Working, Success, Warning, Error };

struct HeaderStatusPresentation {
    std::string label;
    HeaderStatusTone tone;
    const char *foreground;
    const char *background;
    bool active = false;
    bool retained = false;
};

inline HeaderStatusPresentation headerStatusValue(std::string label, HeaderStatusTone tone,
                                                 bool active = false, bool retained = false) {
    switch (tone) {
    case HeaderStatusTone::Working:
        return {std::move(label), tone, "#3155B8", "#EEF3FF", active, retained};
    case HeaderStatusTone::Success:
        return {std::move(label), tone, "#11694F", "#E8F6EF", active, retained};
    case HeaderStatusTone::Warning:
        return {std::move(label), tone, "#8A570F", "#FFF4DE", active, retained};
    case HeaderStatusTone::Error:
        return {std::move(label), tone, "#A52D26", "#FFF0ED", active, retained};
    default:
        return {std::move(label), HeaderStatusTone::Neutral, "#536179", "#EEF1F6", active,
                retained};
    }
}

inline HeaderStatusPresentation headerStatusPresentation(const State &business = state,
                                                        const VisualState &charts = visual) {
    // 正在执行的任务优先于已有结果和上次错误，避免任务进行中显示为成功。
    if (business.status == Status::Cancelling)
        return headerStatusValue("正在取消", HeaderStatusTone::Warning, true);
    if (business.status == Status::Running)
        return headerStatusValue("正在生成", HeaderStatusTone::Working, true);
    if (charts.busy())
        return headerStatusValue("图表导出中", HeaderStatusTone::Working, true);

    // 图表错误及导入数据只描述当前图表页，离开后恢复数据生成状态。
    if (business.page == Page::Charts) {
        if (!charts.errorText.empty()) {
            if (charts.errorKind == "export")
                return headerStatusValue("图表导出失败", HeaderStatusTone::Error);
            if (charts.errorKind == "render")
                return headerStatusValue("图表渲染异常", HeaderStatusTone::Error);
            if (charts.errorKind == "import")
                return headerStatusValue("CSV 导入失败", HeaderStatusTone::Error);
            if (charts.errorKind == "edit")
                return headerStatusValue("图表设置待调整", HeaderStatusTone::Warning);
            if (charts.errorKind == "directory")
                return headerStatusValue("导出目录待检查", HeaderStatusTone::Warning);
            return headerStatusValue("图表操作异常", HeaderStatusTone::Error);
        }
        if (charts.importedSelected)
            return charts.imported
                       ? headerStatusValue("数据已载入", HeaderStatusTone::Success)
                       : headerStatusValue("待载入 CSV", HeaderStatusTone::Neutral);
    }

    const bool retained = business.retainedResult();
    switch (business.status) {
    case Status::Infeasible:
        return headerStatusValue(retained ? "冲突 · 结果保留" : "约束冲突",
                                 HeaderStatusTone::Warning, false, retained);
    case Status::SearchFailed:
        return headerStatusValue(retained ? "未找到 · 结果保留" : "未找到可行数据",
                                 HeaderStatusTone::Warning, false, retained);
    case Status::Error:
        return headerStatusValue(retained ? "异常 · 结果保留" : "操作异常",
                                 HeaderStatusTone::Error, false, retained);
    case Status::ExportError:
        return headerStatusValue("CSV 导出失败", HeaderStatusTone::Error);
    default:
        break;
    }
    // 通用文件/目录操作失败不一定改变任务枚举，但不能被有效结果覆盖为绿色。
    if (!business.message.empty())
        return headerStatusValue("操作异常", HeaderStatusTone::Error);
    if (business.status == Status::Cancelled)
        return headerStatusValue(retained ? "取消 · 结果保留" : "已取消生成",
                                 retained ? HeaderStatusTone::Warning : HeaderStatusTone::Neutral,
                                 false, retained);
    if (business.validResult())
        return headerStatusValue("数据已生成", HeaderStatusTone::Success);
    if (business.result)
        return headerStatusValue("待重新生成", HeaderStatusTone::Warning);
    return headerStatusValue("待生成数据", HeaderStatusTone::Neutral);
}

inline void headerStatusIndicator(Ui &u, const HeaderStatusPresentation &status) {
    constexpr float width = 176, height = 32;
    u.stack("header.status")
        .size(width, height)
        .content([&] {
            u.rect("header.status.bg")
                .size(width, height)
                .color(status.background)
                .radius(height / 2)
                .transition(transition(.14f))
                .animate(eui::AnimProperty::Color)
                .build();
            u.rect("header.status.dot")
                .position(14, 12)
                .size(8, 8)
                .radius(4)
                .color(status.foreground)
                .transition(transition(.14f))
                .animate(eui::AnimProperty::Color)
                .build();
            // 固定文字区域，不对字形位置、字号或透明度做补间。
            u.text("header.status.text")
                .position(30, 0)
                .size(width - 40, height)
                .text(status.label)
                .fontSize(17)
                .color(status.foreground)
                .verticalAlign(eui::VerticalAlign::Center)
                .clip()
                .build();
        })
        .build();
}
