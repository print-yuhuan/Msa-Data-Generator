// 由 gui.cpp 在 gui 命名空间、通用控件之后包含。

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
                 "点击「载入示例」填入研究参数并前往数据页，再点击「生成数据」。"
                 "已有测量记录可直接从「图表 → 导入数据」打开 CSV 或 XLSX。",
                 c);
        button(
            v, "sample", "载入示例", 156,
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
             "回到「数据」页，点击「导出数据」保存测量记录。要比较两种分析方法，请在「图表」页"
             "选择数据源，再点击「导出方法对比报告」。");
        helpItem(v, "help.motion", "界面动画",
                 "如需让页签指示器、开关和搜索预算立即切换，可在 Windows 设置中关闭「动画效果」。"
                 "程序运行时即可生效，重新开启后，下次操作恢复过渡。", c);
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
             "在同一版本中使用相同配置和种子，可复现测量值。达到搜索时限时可能提前结束。"
             "小数位数同时决定生成时的舍入精度和导出时的显示精度。"},
        }};
        helpRows(v, "help.scale.settings", settings, c);
    });

    section(v, "help.settings", "保存配置", cw, [&](float c) {
        helpText(v, "help.settings.save",
                 "在「选项」底部点击「保存配置」，下次启动会自动载入。要恢复初始设置，依次点击"
                 "「恢复默认」和「保存配置」。「配置管理」可复制配置路径或打开目录。导入、导出目录单独设置。",
                 c);
        helpExample(v, "help.path", "%USERPROFILE%\\MSA数据生成器\\msa_generator.json", c);
        helpText(v, "help.settings.regenerate",
                 "修改研究、生成、规格、指标或分析设置后，需要重新生成。仅调整导出设置时，"
                 "可以直接用同一批数据再次导出。",
                 c);
    });

    section(v, "help.troubleshooting", "生成失败时怎么排查", cw, [&](float c) {
        constexpr std::array<std::pair<const char *, const char *>, 4> issues{{
            {"配置载入失败", "到「日志」查看配置字段或格式错误。可按「配置管理」中的路径修正文件，或在「选项」恢复默认后保存配置。"},
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
                 "在数据页的「ANOVA 详情」查看最终模型、完整模型的交互项 P 值、方差分析表和方差分量。",
                 c);
        helpItem(v, "help.anova.interaction", "部件 × 操作员交互项",
                 "选择「按 P 值判断」时可设置显著性水平 α，默认 0.05。P < α 时保留交互项并计入再现性。"
                 "P ≥ α 或 P 值不可计算时，将交互项并入重复性误差。也可直接选择「保留交互项」或"
                 "「并入重复性误差」。方差分量为负时按 0 计算。调整交互项后，可重新生成数据，"
                 "或在图表页对已导入数据点击「按当前设置重算」。",
                 c);
    });
    section(v, "help.metrics", "读懂复核指标", cw, [&](float c) {
        constexpr std::array<std::pair<const char *, const char *>, 5> metrics{{
            {metricNames[0], "重复性占总变异的比例，反映同一操作员反复测量同一部件的波动。"},
            {metricNames[1], "再现性占总变异的比例，反映操作员间的差异。ANOVA 保留的交互分量也计入再现性。"},
            {metricNames[2], "量具 R&R 占总变异的比例。量具 R&R 合并重复性与再现性。"},
            {metricNames[3], "量具 R&R 的研究变异相对有效容差的比例。有效容差由分析容差或规格限确定。"},
            {metricNames[4], "可区分类别数。将 1.41 × PV_SD ÷ GRR_SD 向下取整，最小为 1。GRR_SD 为 0 时不可计算。"},
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
                    "将 GRR/TV (%) 设为「目标值」，填写 10 和 0.1，接受范围为 9.90%～10.10%。", c);
        helpText(v, "help.constraints.result",
                 "「满足」表示生成结果达到所设约束。评价实际测量系统时，应结合真实测量数据与验收要求。", c);
    });
    section(v, "help.process", "P/T 与历史过程", cw, [&](float c) {
        helpItem(v, "help.process.tolerance", "如何确定有效容差",
                 "优先使用「选项 → 分析 → 评价口径」里的分析容差。留空时，双侧规格取 USL − LSL。"
                 "单侧规格用规格限与均值之间距离的两倍。历史均值留空时用实际测量均值，距离为零时 P/T 不可计算。",
                 c);
        helpExample(v, "help.process.tolerance.formula",
                    "P/T (%) = 100 × k × GRR_SD ÷ 有效容差\n"
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
                 "EV/TV (%)、AV/TV (%)、GRR/TV (%) 对应 Minitab 的 %Study Var（标准差之比）。"
                 "%Contribution 使用方差之比。P/T (%) 对照总量具 R&R 行的 %Tolerance。"
                 "Trial 标记重复测量轮次，StdOrder 标记标准运行序号。模型使用操作员和部件作为因子。",
                 c);
        helpText(v, "help.software.single",
                 "仅有一侧规格时，应核对软件如何计算有效容差。例如 MATLAB 原生应用会将缺失一侧视为无穷规格。",
                 c);
    });
}

void helpDataExport(Ui &v, float cw) {
    section(v, "help.files", "导出当前结果", cw, [&](float c) {
        helpText(v, "help.csv.start",
                 "在数据页生成并核对结果后，点击底部「导出数据」。程序默认打开「另存为」，"
                 "可选 CSV 或 XLSX。在「选项 → 文件 → 导出方式」设置「测量数据默认文件类型」，"
                 "或开启「静默导出测量数据」直接保存。",
                 c);
        button(v, "help.files.open", "前往文件选项", 160, [] {
            state.optionsTab = fileOptionsTab;
            state.configOffset = 0;
            state.navigate(Page::Options);
        });
        helpItem(v, "help.csv.location", "保存位置",
                 "在「选项 → 文件 → 目录与文件名」设置「测量数据导出目录」和「测量数据导出名称」。"
                 "目录留空时使用当前用户的 Windows 系统桌面，相对目录也从桌面开始。「日志」会记录实际保存路径。",
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
                 "EV/TV (%)、AV/TV (%) 和 GRR/TV (%) 对应各分量的 %Study Var，P/T (%) 对应总量具 "
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
                 "静默导出图表或方法对比报告时，同名文件会追加 _1、_2 等。名称校验失败时，"
                 "从「日志」查看原因，再检查扩展名、变量拼写、方括号或 Windows 保留名称。",
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
                 "使用 --config study.json 指定配置，或用 --no-config 直接采用内置默认值。省略两者时，"
                 "程序读取已保存的用户配置。可用 --init --config study.json 创建当前格式的配置文件。"
                 "配置文件须包含 schema_version，值为 2。命令行参数覆盖同名配置项。",
                 c);
        helpItem(v, "help.cli.syntax", "常用参数写法",
                 "含空格的路径用引号括起。用 null 清空可选数值，布尔选项填写 true 或 "
                 "false。同一参数写多次时，以最后一次为准。生成数据时可用 --operator-a、--operator-b、"
                 "--operator-c 分别指定三位操作员的名称，例如 --operator-a 张工。",
                 c);
    });
    section(v, "help.cli.generate", "生成并导出数据", cw, [&](float c) {
        helpText(v, "help.cli.example.intro",
                 "这条命令使用分层随机取样和 80% 最低部件跨度，将测量记录保存为 Minitab 长表，"
                 "另存一份 JSON 生成报告。把 --output 后的文件名改为 data.xlsx，即可导出相同布局的工作簿。",
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
                 "例如 --ev target:10:0.1 表示 EV/TV (%) 的目标值为 10%，允许误差为 0.1 个百分点。"
                 "也可用 between:下限:上限、at_most:阈值、at_least:阈值或 none，整项替换原约束。",
                 c);
    });
    section(v, "help.cli.analyze.section", "分析已有测量数据", cw, [&](float c) {
        helpText(v, "help.cli.analyze",
                 "用 --input 读取 CSV 或 XLSX。带操作员名称的数据会自动识别研究设计。"
                 "六行纯数值宽表须指定 --replicates 3（2 人各测 3 次）或 --replicates 2（3 人各测 2 次）。"
                 "XLSX 默认读取第一张数据工作表，使用 --sheet \"复测 数据\" 可指定工作表。"
                 "下例分析上一步生成的 data.csv，保存 JSON 分析结果和 HTML 方法对比报告。",
                 c);
        helpCommand(v, "help.cli.analyze.example",
                    ".\\msa_cli.exe --no-config --input data.csv "
                    "--method crossed_anova --lsl 9 --usl 11 --report analysis.json "
                    "--comparison-report comparison.html",
                    c);
    });
    section(v, "help.cli.outputs", "文件路径与退出状态", cw, [&](float c) {
        helpItem(v, "help.cli.files", "选择导出位置",
                 "省略 --output 时，程序使用配置的测量数据导出目录、导出名称和同名文件处理方式。"
                 "显式 --output 直接指定 .csv 或 .xlsx 文件。命令行中指定的相对路径以当前工作目录为起点，"
                 "配置中的相对导出目录以 Windows 系统桌面为起点。",
                 c);
        helpText(v, "help.cli.report",
                 "--report 写入 JSON，包含生效配置、测量矩阵和统计结果，生成数据时还记录实际种子。"
                 "--comparison-report 接受 .pdf 或 .html，便于阅读和打印两种分析方法的对比结果。", c);
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
                 "在图表页点击「导入数据」，选择 CSV 或 XLSX。程序从宽表首列或长表的 Operator 列识别"
                 "操作员名称，并从记录中确定操作员数、部件数和重复测量次数。图表和方法对比报告沿用"
                 "文件中的名称。选项页保存的默认操作员仍用于生成数据。",
                 c);
        helpItem(v, "help.charts.source.sheet", "选择 XLSX 工作表",
                 "只有一张数据工作表时直接读取。多张时，「选择工作表」按工作簿顺序列出可读工作表，"
                 "包括隐藏工作表。点选后点击「继续」，也可用上下键选择后按回车，或双击表名。",
                 c);
        helpItem(v, "help.charts.source.order", "操作员顺序",
                 "宽表按操作员分组顺序显示。长表有 StdOrder 时按标准运行序号确定操作员顺序，省略时"
                 "按名称首次出现的顺序排列。纯数值宽表按位置使用 A/B/C。",
                 c);
        helpItem(v, "help.charts.source.sixrows", "六行纯数值宽表如何分组",
                 "文件中只有六行读数且没有操作员名称时，程序会打开「选择研究设计」。"
                 "「2 人各测 3 次」将第 1～3 行归给 A、第 4～6 行归给 B。"
                 "「3 人各测 2 次」将第 1～2、3～4、5～6 行分别归给 A、B、C。选中后点击「导入」。"
                 "选择仅对本次导入生效。来自多表工作簿时，可点「上一步」改选工作表。点击「取消」"
                 "或按 Esc 会保留当前数据、图表和编辑草稿。再次导入时可重新选择。",
                 c);
        helpText(v, "help.charts.source.switch",
                 "导入后可在「当前结果」和「已导入数据」之间切换。要用新的分析选项复核导入记录，"
                 "先修改「选项 → 分析」，再回到「已导入数据」点击「按当前设置重算」。",
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
                 "上次应用的外观，「重置当前图表」恢复所选风格的预设外观。",
                 c);
        helpText(v, "help.charts.edit.session",
                 "在「选项 → 图表 → 图表风格」选择「默认风格」或「Origin 风格」。"
                 "两种风格各自保留七张图的已应用外观和编辑草稿。「重置所有图表」仅重置所选风格。"
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
    section(v, "help.charts.report", "导出方法对比报告", cw, [&](float c) {
        helpItem(v, "help.charts.report.source", "比较同一批数据的两种分析结果",
                 "在图表页选择「当前结果」或「已导入数据」，点击「导出方法对比报告」。"
                 "报告列出同一组测量记录的 Xbar-R 和 ANOVA 结果，并附均值控制图和极差控制图。", c);
        helpItem(v, "help.charts.report.format", "选择 PDF 或 HTML",
                 "默认导出 PDF，需要本机已安装 Microsoft Edge 或 Google Chrome。HTML 可用浏览器打开。"
                 "在「选项 → 文件 → 导出方式」设置「方法对比报告默认格式」及「静默导出方法对比报告」。"
                 "导出失败时可到「日志」查看原因，回到图表页点击「重新导出报告」继续使用当前数据。", c);
    });
}

void helpPage(Ui &u, float w, float h) {
    static const std::vector<std::string> topics{
        "快速入门", "分析指标", "数据导出", "名称变量", "命令操作", "结果图表"};
    using Article = void (*)(Ui &, float);
    constexpr std::array<Article, 6> articles{
        helpQuickStart, helpAnalysis, helpDataExport, helpFilename, helpCli, helpCharts};
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
                .contentKey(std::to_string(topic))
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
