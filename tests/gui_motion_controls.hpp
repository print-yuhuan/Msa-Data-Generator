#pragma once

void gui_motion_controls() {
    const auto savedMotion = gui::state.motion;
    gui::state.motion = true;
    core::dsl::Runtime runtime;
    runtime.initialize();
    eui::Ui *ui = nullptr;
    float width = 300, top = 10;
    int selected = 1, flips = 0;
    bool checked = true;
    const auto compose = [&] {
        runtime.compose("motion-controls", 500, 360, [&](eui::Ui &view, const core::dsl::Screen &) {
            ui = &view;
            view.column("controls")
                .x(20)
                .y(top)
                .width(width)
                .content([&] {
                    const std::vector<std::string> choices{"研究", "分析", "图表"};
                    const auto choose = [&](int next) { selected = next; };
                    gui::tabs(view, "tabs", choices, selected, width, choose);
                    gui::segments(view, "segments", choices, selected, width, choose);
                    gui::toggle(view, "toggle", "静默导出", checked, width, [&](bool next) {
                        checked = next;
                        ++flips;
                    });
                })
                .build();
        });
    };
    const auto tick = [&](float dt) { runtime.update(nullptr, dt, 1, 1); };
    const auto require = [](bool satisfied, const char *what) {
        if (!satisfied)
            throw std::runtime_error(what);
    };
    const auto markersAligned = [&] {
        for (const std::string group : {"tabs", "segments"}) {
            const auto &marker = *ui->find(group + ".indicator");
            const auto &hit = *ui->find(group + ".hit." + std::to_string(selected));
            const auto &label = *ui->find(group + ".label." + std::to_string(selected));
            const float center = marker.frame.x + marker.transform.translate.x + marker.frame.width / 2;
            require(std::abs(center - (hit.frame.x + hit.frame.width / 2)) < .01f &&
                        hit.frame.x == label.frame.x && label.opacity == 1 && hit.accessibleChecked,
                    "选择指示器、文字与可访问命中区域未对齐");
            require(!marker.explicitFrameAnimation && !hit.smoothStateColors &&
                        !core::hasAnimProperty(hit.transition.properties, eui::AnimProperty::Border) &&
                        hit.onFrame == nullptr && marker.onFrame == nullptr,
                    "选择器启用了屏幕坐标动画、滞后的焦点边框或持续刷新");
        }
        const auto &knob = *ui->find("toggle.knob");
        const auto &track = ui->find("toggle.track")->frame;
        const auto center = knob.frame.x + knob.transform.translate.x + knob.frame.width / 2;
        require(!knob.explicitFrameAnimation && center > track.x && center < track.x + track.width &&
                    (center > track.x + track.width / 2) == checked,
                "开关圆点偏离轨道或与当前值不一致");
    };
    core::queuePointerMotion(nullptr, 490, 350, {}, {});
    compose();
    tick(.016f);
    markersAligned();
    require(!runtime.isAnimating(), "首次打开控件启动了多余动效");

    ui->requestFocus("tabs.hit.1");
    tick(.016f);
    core::queueKeyInput(nullptr, {core::InputKey::Right});
    tick(.016f);
    compose();
    tick(.016f);
    markersAligned();
    require(selected == 2 && ui->isFocused("tabs.hit.2") && runtime.isAnimating(),
            "切换页签未同步更新键盘焦点或未启动指示器动画");
    core::queueKeyInput(nullptr, {core::InputKey::Home});
    tick(.016f);
    compose();
    tick(.016f);
    markersAligned();
    require(selected == 0 && ui->isFocused("tabs.hit.0"), "动画期间反向切换失去焦点");
    for (int frame = 0; frame < 16; ++frame)
        tick(1.f / 60);
    require(!runtime.isAnimating(), "控件过渡结束后仍持续刷新");

    ui->requestFocus("toggle.hit");
    tick(.016f);
    core::queueKeyInput(nullptr, {core::InputKey::Enter});
    core::queueKeyInput(nullptr, {core::InputKey::Space});
    tick(.016f);
    require(checked && flips == 2, "同帧连续切换开关复用了旧的选中值");
    const auto switchHit = ui->find("toggle.hit")->frame;
    const float x = switchHit.x + switchHit.width / 2, y = switchHit.y + switchHit.height / 2;
    for (int click = 0; click < 2; ++click) {
        core::queuePointerButton(nullptr, x, y, core::PointerButton::Left, core::PointerAction::Press, {});
        core::queuePointerButton(nullptr, x, y, core::PointerButton::Left, core::PointerAction::Release, {});
    }
    core::queueKeyInput(nullptr, {core::InputKey::Space});
    tick(.016f);
    require(!checked && flips == 5, "同帧两次点击和一次按键未分别切换开关");
    core::queuePointerMotion(nullptr, 490, 350, {}, {});

    selected = 2;
    compose();
    tick(.2f);
    top += 50;
    width += 60;
    compose();
    tick(.016f);
    markersAligned();
    require(!runtime.isAnimating(), "父级重排或宽度变化拖动了静止的选中指示器");
    const auto hit = ui->find("segments.hit.0")->frame;
    core::queuePointerMotion(nullptr, hit.x + hit.width / 2, hit.y + hit.height / 2, {}, {});
    tick(.016f);
    require(runtime.isAnimating(), "分段选项悬停缺少反馈");
    gui::state.motion = false;
    checked = true;
    selected = 1;
    compose();
    tick(.016f);
    markersAligned();
    require(!runtime.isAnimating(), "系统关闭动画后切换控件仍持续刷新");
    core::queuePointerMotion(nullptr, 490, 350, {}, {});
    tick(.016f);
    require(!runtime.isAnimating(), "系统关闭动画后离开悬停仍持续刷新");
    core::detail::inputQueues().erase(nullptr);
    core::detail::pointerStates().erase(nullptr);
    runtime.shutdown(false);
    gui::state.motion = savedMotion;
    std::cout << "页签、分段控件与开关的位置、连续输入、动效停帧及减少动效通过\n";
}
