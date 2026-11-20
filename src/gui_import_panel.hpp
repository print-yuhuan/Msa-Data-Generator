// Included in the GUI namespace after the import flow and the shared form controls.
struct ImportPanelView {
    unsigned generation = 0;
    float listWidth = -1, offset = 0;
    bool focusChoice = true;
    int lastClicked = -1;
    POINT clickPoint{};
    Clock::time_point clickTime{};
    std::vector<float> rowTops;
};

std::string importChoiceId(bool sheets, int i) {
    return std::string(sheets ? "import.panel.sheet." : "import.panel.design.") + std::to_string(i);
}

float importLabelHeight(const std::string &label, float width, float fontSize) {
    core::TextStyle style;
    style.text = label;
    style.fontFamily = font();
    style.fontSize = fontSize;
    style.lineHeight = 22;
    style.maxWidth = std::max(1.f, width);
    style.wrap = true;
    return std::max(22.f, core::TextPrimitive::measureTextSize(style).y);
}

void importPanelText(Ui &u, const std::string &id, const std::string &label, float x, float y,
                     float w, float h, float size, const char *color, bool wrap = false) {
    u.text(id).position(x, y).size(w, h).text(label).fontSize(size).color(color)
        .verticalAlign(eui::VerticalAlign::Center).wrap(wrap).lineHeight(22).maxWidth(w).clip().build();
}

// Every option has one hit area. The dot represents selection and the outline represents focus.
void importChoice(Ui &u, ImportPanelView &view, bool sheets, int i, float y, float w, float h,
                  float viewport) {
    const auto id = importChoiceId(sheets, i);
    const int count = sheets ? int(pendingImport->sheetNames.size()) : 2;
    const bool selected = sheets ? pendingImport->sheetIndex == i
                                 : pendingImport->replicates == 3 - i;
    const auto generation = pendingImport->generation;
    auto choose = [&view, generation, sheets](int next) {
        if (!pendingImport || pendingImport->generation != generation)
            return;
        if (sheets)
            pendingImport->sheetIndex = next;
        else
            pendingImport->replicates = 3 - next;
        view.focusChoice = true;
    };
    u.stack(id + ".row").position(0, y).size(w, h).content([&] {
        u.rect(id).size(w, h)
            .states(selected ? blueWash : surface, selected ? blueWashActive : surfaceHover,
                    selected ? blueWashActive : surfacePressed)
            .radius(controlRadius)
            .border(1, selected ? brandBorder : eui::Color(sheets ? surface : line))
            .transition(transition(.12f))
            .onClick([&view, choose, sheets, i, generation] {
                if (!pendingImport || pendingImport->generation != generation)
                    return;
                choose(i);
                if (sheets) {
                    const auto now = Clock::now();
                    POINT point{};
                    GetCursorPos(&point);
                    const bool twice = view.lastClicked == i &&
                        std::abs(point.x - view.clickPoint.x) <= GetSystemMetrics(SM_CXDOUBLECLK) / 2 &&
                        std::abs(point.y - view.clickPoint.y) <= GetSystemMetrics(SM_CYDOUBLECLK) / 2 &&
                        now - view.clickTime <= std::chrono::milliseconds(GetDoubleClickTime());
                    view.lastClicked = i;
                    view.clickPoint = point;
                    view.clickTime = now;
                    if (twice)
                        confirmStudyImport();
                }
            }).build();
        const float markerY = sheets ? (h - 16) / 2 : 15;
        u.rect(id + ".radio").position(12, markerY).size(16, 16).radius(8)
            .color(selected ? blue : surface).border(1, selected ? brandColor : eui::Color(inkDisabled)).build();
        if (selected)
            u.rect(id + ".dot").position(17, markerY + 5).size(6, 6).radius(3).color(surface).build();
        if (sheets) {
            importPanelText(u, id + ".label", pendingImport->sheetNames[i], 40, 0, w - 52, h,
                            controlFont, selected ? blue : ink, true);
        } else {
            importPanelText(u, id + ".label", i == 0 ? "2 人各测 3 次" : "3 人各测 2 次",
                            40, 5, w - 52, 36, 21, ink);
            const int operators = i + 2, repeats = 3 - i;
            const bool narrow = w < 360;
            const float left = narrow ? 12.f : 40.f, gap = 6;
            const float cell = (w - left - 12 - gap * (operators - 1)) / operators;
            for (int person = 0; person < operators; ++person) {
                const auto segment = id + ".operator." + std::to_string(person);
                const std::string label = std::string(1, char('A' + person)) +
                    (narrow ? " " : "  第 ") + std::to_string(1 + person * repeats) + "–" +
                    std::to_string((person + 1) * repeats) + (narrow ? "行" : " 行");
                const float x = left + person * (cell + gap);
                u.rect(segment + ".fill").position(x, 43).size(cell, 26).radius(5)
                    .color(selected ? surface : surfaceSoft).build();
                u.text(segment).position(x, 43).size(cell, 26).text(label)
                    .fontSize(narrow ? 15.f : 17.f).color(muted).horizontalAlign(eui::HorizontalAlign::Center)
                    .verticalAlign(eui::VerticalAlign::Center).clip().build();
            }
        }
        keyboard(u, id, [&view, choose, i, count, sheets, generation, viewport](const eui::KeyEvent &key) {
            if (!key.isDown() || !pendingImport || pendingImport->generation != generation)
                return false;
            view.lastClicked = -1;
            int next = sheets ? pendingImport->sheetIndex :
                                (pendingImport->replicates ? 3 - pendingImport->replicates : i);
            switch (key.key) {
            case eui::InputKey::Space:
                if (key.action == core::KeyAction::Press)
                    choose(i);
                return true;
            case eui::InputKey::Enter:
            case eui::InputKey::NumpadEnter:
                if (key.action == core::KeyAction::Press && (sheets || pendingImport->replicates))
                    confirmStudyImport();
                return true;
            case eui::InputKey::Down: case eui::InputKey::Right: ++next; break;
            case eui::InputKey::Up: case eui::InputKey::Left: --next; break;
            case eui::InputKey::Home: next = 0; break;
            case eui::InputKey::End: next = count - 1; break;
            case eui::InputKey::PageUp: case eui::InputKey::PageDown:
                if (!sheets)
                    return false;
                {
                    const float target = view.rowTops[next] +
                        (key.key == eui::InputKey::PageUp ? -viewport : viewport);
                    next = int(std::upper_bound(view.rowTops.begin(), view.rowTops.end(), target) -
                               view.rowTops.begin()) - 1;
                }
                break;
            default: return false;
            }
            choose(sheets ? std::clamp(next, 0, count - 1) : (next + count) % count);
            return true;
        });
        u.find(id)->focusable = selected || (!sheets && !pendingImport->replicates && i == 0);
    }).build();
}

void importPanel(Ui &u, const eui::Screen &screen) {
    if (!pendingImport)
        return;
    auto &view = u.state<ImportPanelView>("import.panel.view");
    if (view.generation != pendingImport->generation) {
        view = {};
        view.generation = pendingImport->generation;
    }
    const bool sheets = pendingImport->step == ImportStep::Worksheet;
    // The runtime caches scroll transforms by owner ID between input and paint.
    // A newly opened design must not inherit the previous sheet list's offset.
    const std::string scrollId = sheets ? "import.panel.scroll" : "import.panel.design.scroll";
    const float margin = screen.width < 400 || screen.height < 420 ? 8.f : 20.f;
    const float w = std::max(1.f, std::min(562.f, screen.width - 2 * margin));
    const float inset = w < 420 ? 13.f : 22.f, body = std::max(1.f, w - 2 * inset);
    const float bar = 6, gutter = 6;
    const int active = sheets ? pendingImport->sheetIndex : (pendingImport->replicates == 2 ? 1 : 0);
    const auto &names = pendingImport->sheetNames;
    if (sheets && view.listWidth != body) {
        view.listWidth = body;
        view.rowTops = {0};
        for (const auto &name : names) {
            const auto textHeight = importLabelHeight(name, body - bar - gutter - 52, controlFont);
            view.rowTops.push_back(view.rowTops.back() + std::max(42.f, textHeight + 12));
        }
    }
    const auto file = pendingImport->path.filename().u8string();
    const auto sheet = !sheets && !names.empty() ? names[pendingImport->sheetIndex] : std::string{};
    const bool shortWindow = screen.height < 365;
    const float sectionGapY = shortWindow ? 5.f : 12.f;
    const float titleY = shortWindow ? 5.f : 15.f, sourceY = titleY + 35;
    const float sheetH = sheet.empty() ? 0 : std::min(44.f, importLabelHeight(sheet, body - 80, 17));
    const float instructionY = sourceY + 24 + sheetH + sectionGapY;
    const float listY = instructionY + 24 + sectionGapY;
    const float footerH = shortWindow ? 47.f : 65.f;
    constexpr float cardH = 80, cardGap = 8;
    const float contentH = sheets ? view.rowTops.back() : 2 * cardH + cardGap;
    const float maxH = std::max(1.f, screen.height - 2 * margin);
    const float viewport = std::max(1.f, std::min({contentH, sheets ? 236.f : contentH,
                                                   maxH - listY - sectionGapY - footerH}));
    const float footerY = listY + viewport + sectionGapY, h = footerY + footerH;
    if (view.focusChoice) {
        const float top = sheets ? view.rowTops[active] : active * (cardH + cardGap);
        const float end = sheets ? view.rowTops[active + 1] : top + cardH;
        if (top < view.offset || end - top > viewport)
            view.offset = top;
        else if (end > view.offset + viewport)
            view.offset = end - viewport;
    }
    view.offset = std::clamp(view.offset, 0.f, std::max(0.f, contentH - viewport));
    u.rect("import.panel.backdrop").size(screen.width, screen.height).color("#182C454D")
        .zIndex(1010).preserveFocusOnPress().blockPointer().build();
    u.stack("import.panel.dialog").position((screen.width - w) / 2, (screen.height - h) / 2)
        .size(w, h).zIndex(1011).content([&] {
            u.rect("import.panel.surface").size(w, h).radius(cardRadius).border(1, line)
                .color(surface).shadow(26, 0, 7, "#182C4538").build();
            u.rect("import.panel.accent").position(inset, titleY + 9).size(3, 16).radius(1.5f).color(blue).build();
            importPanelText(u, "import.panel.title", sheets ? "选择工作表" : "选择研究设计",
                            inset + 12, titleY, body - 54, 34, 25, ink);
            importPanelText(u, "import.panel.file.caption", "来源文件", inset, sourceY, 72, 24, 16, muted);
            importPanelText(u, "import.panel.file", labelThatFits(file, body - 80, 17), inset + 80,
                            sourceY, body - 80, 24, 17, ink);
            if (!sheet.empty()) {
                importPanelText(u, "import.panel.sheet.caption", "工作表", inset, sourceY + 24, 72, sheetH, 16, muted);
                importPanelText(u, "import.panel.source.sheet", sheet, inset + 80, sourceY + 24,
                                body - 80, sheetH, 17, ink, true);
            }
            const auto prompt = sheets ? "工作表名称" :
                (body < 350 ? "按原始行序选择操作员分组" : "按原始测量顺序，选择六行数据的操作员分组");
            importPanelText(u, "import.panel.prompt", prompt, inset, instructionY,
                            sheets ? body - 90 : body, 24, 17, muted);
            if (sheets)
                u.text("import.panel.count").position(w - inset - 90, instructionY).size(90, 24)
                    .text("共 " + std::to_string(names.size()) + " 张").fontSize(16).color(muted)
                    .horizontalAlign(eui::HorizontalAlign::Right).verticalAlign(eui::VerticalAlign::Center).build();
            components::scrollView(u, scrollId).position(inset, listY).size(body, viewport)
                .theme(tokens()).scrollbarWidth(bar).scrollbarGap(gutter).reserveScrollbar(sheets)
                .offset(view.offset).step(42).onChange([&view](float value) { view.offset = value; })
                .content([&](Ui &v, float width, float) {
                    v.stack(scrollId + ".items").size(width, contentH).content([&] {
                        if (!sheets) {
                            for (int i = 0; i < 2; ++i)
                                importChoice(v, view, false, i, i * (cardH + cardGap), width, cardH, viewport);
                            return;
                        }
                        // A workbook can contain thousands of sheets. Only compose visible rows
                        // and the active Tab stop, keeping their original positions in the list.
                        const int first = std::max(0, int(std::upper_bound(view.rowTops.begin(), view.rowTops.end(),
                            view.offset) - view.rowTops.begin()) - 1);
                        const int after = std::min(int(names.size()), int(std::lower_bound(
                            view.rowTops.begin(), view.rowTops.end(), view.offset + viewport) - view.rowTops.begin()));
                        auto row = [&](int i) {
                            importChoice(v, view, true, i, view.rowTops[i], width,
                                         view.rowTops[i + 1] - view.rowTops[i], viewport);
                        };
                        if (active < first || active >= after)
                            row(active);
                        for (int i = first; i < after; ++i)
                            row(i);
                    }).build();
                }).build();
            if (auto *list = u.find(scrollId))
                list->composeOnScrollOffsetChange = sheets;
            for (const auto &id : {scrollId, scrollId + ".scroll.track", scrollId + ".scroll.thumb"})
                if (auto *part = u.find(id))
                    part->preserveFocusOnPress = true;
            u.rect("import.panel.divider").position(0, footerY).size(w, 1).color(line).build();
            const float buttonW = w < 365 ? 70.f : 86.f, pad = (footerH - controlHeight) / 2;
            if (!sheets && names.size() > 1) {
                u.stack("import.panel.back.wrap").position(inset, footerY + pad)
                    .size(buttonW, controlHeight).content([&] {
                        button(u, "import.panel.back", "上一步", buttonW, backStudyImport);
                    }).build();
            } else if (body > 350)
                importPanelText(u, "import.panel.hint", sheets ? "仅读取选中的工作表" : "仅用于本次导入",
                                inset, footerY + pad, body - 2 * buttonW - 24, controlHeight, 16, muted);
            u.row("import.panel.actions").position(w - inset - 2 * buttonW - 8, footerY + pad)
                .size(2 * buttonW + 8, controlHeight).gap(8).content([&] {
                    button(u, "import.panel.cancel", "取消", buttonW, cancelStudyImport);
                    button(u, "import.panel.confirm", sheets ? "继续" : "导入", buttonW,
                           [] { confirmStudyImport(); }, true, !sheets && !pendingImport->replicates);
                }).build();
            u.stack("import.panel.close.wrap").position(w - inset - 32, titleY)
                .size(32, 32).content([&] {
                    button(u, "import.panel.close", "×", 32, cancelStudyImport, false, false, 32);
                }).build();
        }).build();
    if (view.focusChoice) {
        u.requestFocus(importChoiceId(sheets, active));
        view.focusChoice = false;
    }
}
