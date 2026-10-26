// 由 gui.cpp 在通用控件及图表页面之后包含；所有界面状态只在主线程读写。
struct AiConversation {
    // 展示消息可标记为 local；发给模型的历史独立保存在 wireMessages 中。
    std::vector<msa::ai::Message> messages;
    std::vector<msa::ai::Message> wireMessages;
    std::string input, datasetSignature;
    std::string errorCode, errorText, recovery;
    bool canRetry = false, retryDiagnosis = false;
    unsigned retryRevision = 0;
    float offset = 0;
};
struct AiState {
    msa::ai::Settings settings = msa::ai::defaults();
    std::string timeout = "120";
    bool dirty = false, showKey = false, requesting = false, generating = false, diagnosis = false;
    int mode = 0, requestMode = 0;
    int promptTab = 0;
    bool focusComposer = false;
    unsigned requestRevision = 0;
    unsigned suggestionRevision = 0;
    unsigned generationRevision = 0;
    Json requestConfig;
    Json generationConfig;
    Json localGeneration;
    std::optional<Json> suggestion;
    std::array<AiConversation, 2> chats;
    msa::ai::AsyncClient client;
    msa::ai::AsyncClient serviceClient;
    std::vector<std::string> models;
    bool modelsLoaded = false, serviceBusy = false, serviceFailed = false;
    msa::ai::ServiceAction serviceAction = msa::ai::ServiceAction::Models;
    unsigned serviceRevision = 0, activeServiceRevision = 0;
    std::string serviceSummary = "尚未验证连接";
    std::string serviceDetail = "填写接口和密钥，拉取模型后选择并测试。";
    float serviceOffset = 0;
} aiUi;
bool aiNetworkBusy() { return aiUi.requesting || aiUi.serviceBusy; }
bool aiSettingsDirty() { return aiUi.dirty; }
bool assistantBusy() { return aiNetworkBusy() || aiUi.generating || state.busy(); }
void invalidateAiService(bool clearModels = true) {
    ++aiUi.serviceRevision;
    if (clearModels) {
        aiUi.models.clear();
        aiUi.modelsLoaded = false;
    }
    aiUi.serviceFailed = false;
    aiUi.serviceSummary = "连接配置已改变，需重新验证";
    aiUi.serviceDetail = clearModels ? "请拉取此接口的模型列表，选择模型后测试连接。"
                                     : "已选择模型；点击测试连接确认该模型可用。";
    aiUi.serviceOffset = 0;
}
// 保留已保存但暂未发现的模型，失败或空列表不能悄悄替换用户的选择。
std::vector<std::string> aiModelChoices() {
    auto values = aiUi.models;
    if (!aiUi.settings.model.empty() &&
        std::find(values.begin(), values.end(), aiUi.settings.model) == values.end())
        values.insert(values.begin(), aiUi.settings.model);
    return values;
}
void startAiService(msa::ai::ServiceAction action) {
    if (assistantBusy())
        return;
    try {
        const auto timeout = native_ui::numeric(aiUi.timeout, "AI 超时", true);
        if (timeout < 10 || timeout > 600)
            throw std::runtime_error("AI 请求超时须为 10～600 秒的整数");
        aiUi.settings.timeout_seconds = timeout.get<int>();
        aiUi.serviceClient.start_service(aiUi.settings, action, [] { app::requestUpdate(); });
        aiUi.serviceAction = action;
        aiUi.activeServiceRevision = aiUi.serviceRevision;
        aiUi.serviceBusy = true;
        aiUi.serviceFailed = false;
        aiUi.serviceOffset = 0;
        aiUi.serviceSummary = action == msa::ai::ServiceAction::Models    ? "正在拉取模型列表…"
                              : action == msa::ai::ServiceAction::Balance ? "正在查询账户余额…"
                                                                          : "正在测试所选模型…";
        aiUi.serviceDetail = "请求在后台执行，可随时取消；不会自动重试或保存配置。";
        menu.id.clear();
        state.hint(aiUi.serviceSummary);
    } catch (const msa::ai::Error &e) {
        aiUi.serviceFailed = true;
        aiUi.serviceSummary = e.what();
        aiUi.serviceDetail = e.code + " · " + e.recovery;
        state.hint("AI 连接操作未完成，请查看服务连接说明", Level::Warning);
    } catch (const std::exception &e) {
        aiUi.serviceFailed = true;
        aiUi.serviceSummary = "连接配置需修正";
        aiUi.serviceDetail = e.what();
        state.hint("AI 连接操作未开始，请核对配置", Level::Warning);
    }
}
void pollAiService() {
    if (!aiUi.serviceBusy)
        return;
    const auto view = aiUi.serviceClient.view();
    if (view.status == msa::ai::Status::Running || view.status == msa::ai::Status::Cancelling)
        return;
    aiUi.serviceBusy = false;
    aiUi.serviceOffset = 0;
    if (aiUi.activeServiceRevision != aiUi.serviceRevision) {
        (void)aiUi.serviceClient.take_service();
        state.hint("连接配置已改变，已忽略旧请求结果");
        return;
    }
    if (view.status == msa::ai::Status::Cancelled) {
        aiUi.serviceSummary = "请求已取消";
        aiUi.serviceDetail = "原配置和模型选择已保留；需要时可再次手动操作。";
        state.hint("AI 连接操作已取消");
        return;
    }
    if (view.status != msa::ai::Status::Success) {
        aiUi.serviceFailed = true;
        aiUi.serviceSummary = view.error;
        aiUi.serviceDetail = view.error_code + " · " + view.recovery;
        state.hint("AI 连接操作未完成，请查看服务连接说明", Level::Warning);
        return;
    }
    const auto reply = aiUi.serviceClient.take_service();
    if (!reply)
        return;
    if (aiUi.serviceAction == msa::ai::ServiceAction::Models) {
        aiUi.models = reply->models;
        aiUi.modelsLoaded = true;
        if (aiUi.settings.model.empty() && !aiUi.models.empty()) {
            aiUi.settings.model = aiUi.models.front();
            aiUi.dirty = true;
        }
        aiUi.serviceSummary = "已获取 " + std::to_string(aiUi.models.size()) + " 个模型";
        aiUi.serviceDetail =
            "请从模型列表选择，再测试连接。列表可见不代表该模型具备调用权限或 JSON 模式能力。";
        if (aiUi.models.empty())
            aiUi.serviceDetail =
                "接口未返回可选模型。请检查账户权限或联系服务商；已有模型配置保留。";
    } else if (aiUi.serviceAction == msa::ai::ServiceAction::TestConnection) {
        aiUi.serviceSummary = "连接测试通过";
        aiUi.serviceDetail =
            "所选模型已完成测试请求并返回有效 JSON 对象。业务参数生成及结果解读仍会独立校验。";
    } else if (reply->balance) {
        const auto &balance = *reply->balance;
        const bool available = balance.at("is_available").get<bool>();
        aiUi.serviceSummary = available ? "账户有可用余额" : "账户暂无可用余额";
        aiUi.serviceDetail.clear();
        for (const auto &entry : balance.at("balance_infos")) {
            if (!aiUi.serviceDetail.empty())
                aiUi.serviceDetail += "\n";
            aiUi.serviceDetail += entry.at("currency").get<std::string>() + "  可用 " +
                                  entry.at("total_balance").get<std::string>() + "，赠金 " +
                                  entry.at("granted_balance").get<std::string>() + "，充值 " +
                                  entry.at("topped_up_balance").get<std::string>();
        }
        aiUi.serviceDetail += available ? "\n余额为本次查询快照，不代表模型调用测试通过。"
                                        : "\n请充值或检查账户状态后重新查询。";
    }
    state.hint(aiUi.serviceSummary);
}
void appendAi(int mode, const std::string &role, const std::string &content) {
    aiUi.chats[mode].messages.push_back({role, content});
    aiUi.chats[mode].offset = 1000000;
}
void clearAiError(int mode) {
    auto &chat = aiUi.chats[mode];
    chat.errorCode.clear();
    chat.errorText.clear();
    chat.recovery.clear();
    chat.canRetry = false;
}
void showAiError(int mode, const std::string &code, const std::string &message,
                 const std::string &recovery, bool canRetry, bool diagnosis = false) {
    auto &chat = aiUi.chats[mode];
    chat.errorCode = code;
    chat.errorText = message;
    chat.recovery = recovery;
    chat.canRetry = canRetry;
    chat.retryDiagnosis = diagnosis;
    chat.retryRevision = aiUi.requestRevision;
    chat.offset = 1000000;
    state.feedback("AI 操作未完成", message + "；" + recovery, Level::Warning);
}
Json aiConfig() {
    auto c = native_ui::to_config(state.draft);
    c.erase("output");
    return c;
}
Json interpretationData() {
    const auto *result = insightResult();
    if (!result)
        throw std::runtime_error("请先生成有效数据，或在结果可视化中导入 CSV");
    auto data = result->as_json();
    data["config"].erase("output");
    data["checks"] = msa::checks(result->metrics, result->config);
    data["analysis_only"] = visual.importedSelected;
    if (visual.importedSelected)
        data["seed"] = nullptr;
    return data;
}
void updateAiSettingsDraft() {
    const auto timeout = native_ui::numeric(aiUi.timeout, "AI 超时", true);
    if (timeout < 10 || timeout > 600)
        throw std::runtime_error("AI 请求超时须为 10～600 秒的整数");
    aiUi.settings.timeout_seconds = timeout.get<int>();
    msa::ai::validate_settings(aiUi.settings);
}
void saveAiSettings() {
    safe([] {
        updateAiSettingsDraft();
        msa::ai::save_settings(aiUi.settings);
        aiUi.dirty = false;
        state.feedback("AI 配置已保存", "AI 配置已保存；密钥使用 Windows 当前用户加密存储。");
    });
}
void resetAiSettings() {
    if (aiNetworkBusy())
        return;
    aiUi.settings = msa::ai::defaults();
    aiUi.timeout = "120";
    aiUi.dirty = true;
    aiUi.showKey = false;
    invalidateAiService();
    state.feedback("AI 默认设置已恢复，尚未保存");
}
void aiSettings(Ui &u, float w) {
    auto changed = [] { aiUi.dirty = true; };
    const auto connection = [&](float cardWidth) {
        section(u, "ai.connection", "服务连接", cardWidth, [&](float c) {
            u.column("ai.connection.fields")
                .width(c)
                .gap(12)
                .disabled(aiNetworkBusy() || state.busy())
                .content([&] {
                    subheading(u, "ai.provider.heading", "选择服务",
                               "填写服务地址与密钥，再拉取可选模型。", c);
                    choiceChips(u, "ai.provider", {"DeepSeek 预设", "自定义 OpenAI 兼容接口"},
                                aiUi.settings.provider == "deepseek" ? 0 : 1, c, [](int i) {
                                    aiUi.settings.provider =
                                        i == 0 ? "deepseek" : "openai_compatible";
                                    aiUi.settings.api_key.clear();
                                    if (i == 0) {
                                        aiUi.settings.base_url = "https://api.deepseek.com";
                                        aiUi.settings.model = "deepseek-flash";
                                    } else
                                        aiUi.settings.model.clear();
                                    invalidateAiService();
                                    aiUi.dirty = true;
                                });
                    const auto endpointField = [&](float width) {
                        localField(u, "ai.url", "接口地址", aiUi.settings.base_url, width,
                                   "https://your-provider.example/v1", [] {
                                       aiUi.settings.api_key.clear();
                                       aiUi.settings.model.clear();
                                       invalidateAiService();
                                       aiUi.dirty = true;
                                   });
                    };
                    endpointField(c);
                    localField(
                        u, "ai.key", "API Key", aiUi.settings.api_key, c,
                        "本机服务可留空；远程服务需要密钥",
                        [] {
                            invalidateAiService();
                            aiUi.dirty = true;
                        },
                        !aiUi.showKey);
                    checkOption(u, "ai.showkey", "显示 API Key", aiUi.showKey, c,
                                [](bool b) { aiUi.showKey = b; });
                    text(u, "ai.model.label", "选择模型", c, 22, 13, muted);
                    const auto choices = aiModelChoices();
                    auto labels = choices;
                    for (auto &label : labels)
                        if (std::find(aiUi.models.begin(), aiUi.models.end(), label) ==
                            aiUi.models.end())
                            label += "（当前配置，未验证）";
                    if (labels.empty())
                        labels.emplace_back("请先拉取模型列表");
                    auto selected = std::find(choices.begin(), choices.end(), aiUi.settings.model);
                    const int index =
                        selected == choices.end() ? 0 : int(selected - choices.begin());
                    u.column("ai.model.control")
                        .width(c)
                        .disabled(choices.empty())
                        .content([&] {
                            combo(u, "ai.model", labels, index, c, [choices](int i) {
                                if (i < 0 || i >= int(choices.size()))
                                    return;
                                aiUi.settings.model = choices[i];
                                aiUi.dirty = true;
                                invalidateAiService(false);
                            });
                        })
                        .build();
                })
                .build();
            const bool busy = assistantBusy();
            u.row("ai.connection.actions")
                .width(c)
                .gap(8)
                .content([&] {
                    button(
                        u, "ai.models.fetch", aiUi.modelsLoaded ? "刷新模型" : "拉取模型列表",
                        (c - 8) / 2, [] { startAiService(msa::ai::ServiceAction::Models); }, false,
                        busy);
                    button(
                        u, "ai.connection.test", "测试连接", (c - 8) / 2,
                        [] { startAiService(msa::ai::ServiceAction::TestConnection); }, true,
                        busy || aiUi.settings.model.empty());
                })
                .build();
            u.row("ai.account.actions")
                .width(c)
                .gap(8)
                .content([&] {
                    button(
                        u, "ai.balance.query", "查询 DeepSeek 余额", (c - 8) / 2,
                        [] { startAiService(msa::ai::ServiceAction::Balance); }, false,
                        busy || !msa::ai::supports_balance(aiUi.settings));
                    button(
                        u, "ai.service.cancel", "取消请求", (c - 8) / 2,
                        [] {
                            aiUi.serviceClient.cancel();
                            aiUi.serviceSummary = "正在取消请求…";
                        },
                        false, !aiUi.serviceBusy);
                })
                .build();
            // 为请求状态保留固定空间，错误和余额详情变化不挤动其他控件。
            components::scrollView(u, "ai.service.status")
                .size(c, 120)
                .offset(aiUi.serviceOffset)
                .theme(tokens())
                .reserveScrollbar()
                .onChange([](float offset) { aiUi.serviceOffset = offset; })
                .content([&](Ui &v, float width, float) {
                    v.column("ai.service.feedback")
                        .width(width)
                        .gap(6)
                        .content([&] {
                            paragraph(v, "ai.service.summary", aiUi.serviceSummary, width,
                                      aiUi.serviceFailed ? "#B54735" : blue, 17);
                            paragraph(v, "ai.service.detail", aiUi.serviceDetail, width, muted, 17);
                        })
                        .build();
                })
                .build();
            paragraph(u, "ai.connection.note",
                      "测试连接会发送简短 JSON 请求，可能产生少量费用，不附带研究数据或会话。"
                      "余额查询仅适用于 DeepSeek 官方接口。切换服务或地址会清空密钥；密钥按当前 "
                      "Windows 用户加密保存。",
                      c, muted, 17);
        });
    };
    const auto advanced = [&](float cardWidth) {
        section(u, "ai.advanced", "高级参数", cardWidth, [&](float c) {
            subheading(u, "ai.timeout.heading", "等待时限",
                       "每轮请求独立计时，超时后由你决定是否重试。", c);
            const std::vector<std::string> values{"30", "60", "120", "300"};
            int selected = -1;
            for (int i = 0; i < int(values.size()); ++i)
                if (aiUi.timeout == values[i])
                    selected = i;
            choiceChips(u, "ai.timeout.presets", {"30 秒", "60 秒", "120 秒", "300 秒"}, selected,
                        c, [values](int i) {
                            aiUi.timeout = values[i];
                            aiUi.dirty = true;
                        });
            localField(u, "ai.timeout", "自定义超时（10～600 秒）", aiUi.timeout,
                       std::min(c, 320.f), "120", changed);
        });
    };
    const auto prompts = [&](float cardWidth) {
        section(u, "ai.prompts", "系统提示词", cardWidth, [&](float c) {
            const auto promptInput = [&](const std::string &id, std::string &value,
                                         const std::string &hint) {
                // 初次载入或恢复长提示词时从开头阅读，实际输入后仍正常跟随光标。
                auto &editing = u.state<components::input_detail::InputModel::InputState>(id);
                if (editing.text != value && !u.isFocused(id + ".hit"))
                    editing.followCaret = false;
                localInput(u, id, value, c, hint, 240, changed);
            };
            choiceChips(u, "ai.prompts.task", {"辅助生成", "结果解读"}, aiUi.promptTab, c,
                        [](int i) { aiUi.promptTab = i; });
            if (aiUi.promptTab == 0) {
                subheading(u, "ai.generation.label", "生成助手的工作要求",
                           "调整需求澄清、参数解释和失败诊断的表达方式。", c);
                promptInput("ai.generation.prompt", aiUi.settings.generation_system_prompt,
                            "描述生成角色、参数解释与失败处理要求");
            } else {
                subheading(u, "ai.interpretation.label", "解读助手的评价要求",
                           "调整评价口径、适用范围和改进建议的表达方式。", c);
                promptInput("ai.interpretation.prompt", aiUi.settings.interpretation_system_prompt,
                            "描述评价口径、输出结构与局限说明");
            }
            paragraph(u, "ai.prompt.note",
                      "两类提示词独立保留。程序统一校验结构化 JSON、业务参数与本机指标；"
                      "请保留协议字段要求，不要求代码块或 JSON 外说明。",
                      c, muted, 17);
            button(u, "ai.prompts.reset", "仅恢复默认提示词", std::min(c, 220.f), [] {
                const auto defaults = msa::ai::defaults();
                aiUi.settings.generation_system_prompt = defaults.generation_system_prompt;
                aiUi.settings.interpretation_system_prompt = defaults.interpretation_system_prompt;
                aiUi.dirty = true;
            });
        });
    };
    settingsColumns(
        u, "ai.settings.layout", w,
        [&](float width) {
            connection(width);
            u.column("ai.advanced.lock")
                .width(width)
                .disabled(aiNetworkBusy() || state.busy())
                .content([&] { advanced(width); })
                .build();
        },
        [&](float width) {
            u.column("ai.prompts.lock")
                .width(width)
                .disabled(aiNetworkBusy() || state.busy())
                .content([&] { prompts(width); })
                .build();
        });
}
void requestAi(int mode, bool diagnosis = false) {
    aiUi.requestRevision = state.revision;
    updateAiSettingsDraft();
    Json context;
    if (mode == 0) {
        aiUi.requestConfig = aiConfig();
        context["current_config"] = aiUi.requestConfig;
        if (!aiUi.localGeneration.is_null())
            context["local_generation"] = aiUi.localGeneration;
        if (diagnosis) {
            context["last_generation"] = {
                {"status", "failed"},
                {"failure_kind", aiUi.localGeneration.value("failure_kind", "generation_failure")},
                {"reason", state.message},
                {"attempts", state.ticks.load()},
                {"elapsed_seconds", state.elapsed}};
        }
    } else {
        context = interpretationData();
        const auto signature = context.dump();
        auto &chat = aiUi.chats[1];
        if (!chat.datasetSignature.empty() && chat.datasetSignature != signature)
            throw std::runtime_error("数据集或分析配置已改变，请先新建解读会话，避免混用结果");
        chat.datasetSignature = signature;
    }
    if (!aiUi.chats[mode].errorCode.empty())
        context["last_response_error"] = {{"code", aiUi.chats[mode].errorCode},
                                          {"message", aiUi.chats[mode].errorText},
                                          {"recovery", aiUi.chats[mode].recovery}};
    aiUi.client.start(aiUi.settings, aiUi.chats[mode].wireMessages, mode == 0, context,
                      [] { app::requestUpdate(); });
    aiUi.requestMode = mode;
    aiUi.requesting = true;
    aiUi.diagnosis = diagnosis;
    clearAiError(mode);
    aiUi.chats[mode].offset = 1000000;
}
void sendAi() {
    if (assistantBusy())
        return;
    const int mode = aiUi.mode;
    const auto content = native_ui::trim(aiUi.chats[mode].input);
    if (content.empty())
        return;
    if (content.size() > 16000) {
        state.hint("本条消息过长，请精简后发送", Level::Warning);
        return;
    }
    // 启动失败时回滚本轮消息，保留编辑框，便于用户修正接口或研究设置。
    appendAi(mode, "user", content);
    aiUi.chats[mode].wireMessages.push_back({"user", content});
    try {
        requestAi(mode);
        aiUi.chats[mode].input.clear();
        if (mode == 0)
            aiUi.suggestion.reset();
    } catch (const msa::ai::Error &e) {
        aiUi.chats[mode].messages.pop_back();
        aiUi.chats[mode].wireMessages.pop_back();
        showAiError(mode, e.code, e.what(), e.recovery, false);
    } catch (const std::exception &e) {
        aiUi.chats[mode].messages.pop_back();
        aiUi.chats[mode].wireMessages.pop_back();
        showAiError(mode, "request_not_sent", e.what(),
                    "请修正 AI 配置或研究设置，再点击发送；输入内容已保留。", false);
    }
}
void startAiProposal(const Json &patch, const Json &base) {
    auto cfg = msa::ai::apply_config_patch(base, patch);
    cfg["output"] = native_ui::output_config(state.draft);
    // 参数必须完整、可执行后才更新界面；失败时保留用户原有研究及结果。
    cfg = msa::validate_request(cfg);
    // 保持真实校验错误：不自动放宽规格、误差或目标来宣称成功。
    auto draft = native_ui::from_config(cfg);
    state.draft = std::move(draft);
    state.changed();
    visual.importedSelected = false;
    appendAi(0, "local", "参数已交给本机生成器。调整内容：\n" + patch.dump(2));
    aiUi.generating = true;
    aiUi.generationRevision = state.revision;
    aiUi.generationConfig = cfg;
    aiUi.generationConfig.erase("output");
    aiUi.localGeneration = {{"status", "running"}, {"config", cfg}};
    aiUi.localGeneration["config"].erase("output");
    state.ticks = 0;
    state.elapsed = 0;
    state.start();
}
void diagnoseAiFailure() {
    aiUi.localGeneration = {
        {"status", "failed"},
        {"failure_kind", state.status == Status::Infeasible     ? "constraint_conflict"
                         : state.status == Status::SearchFailed ? "search_exhausted"
                                                                : "generation_failure"},
        {"reason", state.message},
        {"config", aiUi.generationConfig},
        {"attempts", state.ticks.load()},
        {"elapsed_seconds", state.elapsed}};
    appendAi(0, "local", "本机未获得满足约束的数据：" + state.message);
    appendAi(0, "user",
             "[程序诊断请求] "
             "请结合本机失败原因解释问题并建议下一步参数调整。不要自动重试；等待我确认建议或提出新"
             "要求。");
    aiUi.chats[0].wireMessages.push_back(aiUi.chats[0].messages.back());
    try {
        requestAi(0, true);
    } catch (const msa::ai::Error &e) {
        showAiError(0, e.code, e.what(), e.recovery, true, true);
    } catch (const std::exception &e) {
        showAiError(0, "diagnosis_unavailable", e.what(),
                    "本机失败原因已保留。修正服务配置后可重试原因分析，或输入新的参数要求。", true,
                    true);
    }
}
void retryAiRequest() {
    if (assistantBusy())
        return;
    const int mode = aiUi.mode;
    auto &chat = aiUi.chats[mode];
    if (!chat.canRetry)
        return;
    if (mode == 0 && chat.retryRevision != state.revision) {
        showAiError(mode, "stale_request", "研究参数已变化，不能重发旧请求。",
                    "请重新描述需求后发送，新的请求将使用当前研究参数。", false);
        return;
    }
    const bool diagnosis = chat.retryDiagnosis;
    try {
        requestAi(mode, diagnosis);
    } catch (const msa::ai::Error &e) {
        showAiError(mode, e.code, e.what(), e.recovery, true, diagnosis);
    } catch (const std::exception &e) {
        showAiError(mode, "retry_not_sent", e.what(),
                    "请检查 AI 配置；如已切换数据集，请新建解读会话。", true, diagnosis);
    }
}
void acceptAiReply(int mode, const msa::ai::Reply &reply) {
    if (mode == 0 && (state.busy() || state.revision != aiUi.requestRevision)) {
        aiUi.suggestion.reset();
        appendAi(0, "assistant",
                 "研究参数在请求期间发生变化，本次回复已作废，请重新发送需求。\n\n" + reply.text);
        state.hint("已丢弃过期 AI 回复", Level::Warning);
        return;
    }
    if (mode == 1) {
        bool stale = true;
        try {
            stale = interpretationData().dump() != aiUi.chats[1].datasetSignature;
        } catch (...) {
        }
        if (stale) {
            appendAi(1, "assistant",
                     "数据源在请求期间发生变化。以下回复仅针对发送时的数据快照；请新建会话后"
                     "解读当前数据。\n\n" +
                         reply.text);
            state.hint("解读基于旧数据快照，请新建会话", Level::Warning);
            return;
        }
    }
    // 模型历史只保存校验后的协议 JSON；本机状态与错误不会冒充模型回复。
    aiUi.chats[mode].wireMessages.push_back({"assistant", reply.structured.dump()});
    appendAi(mode, "assistant", reply.text);
    if (mode == 0 && reply.config_patch) {
        if (state.busy() || state.revision != aiUi.requestRevision) {
            aiUi.suggestion.reset();
            appendAi(0, "local", "研究参数在请求期间发生变化，本次建议已作废。请重新发送需求。");
        } else if (aiUi.diagnosis || reply.action == "diagnose") {
            aiUi.suggestion = reply.config_patch;
            aiUi.suggestionRevision = state.revision;
            appendAi(0, "local",
                     "建议参数：\n" + reply.config_patch->dump(2) +
                         "\n尚未应用。可继续提出要求，或点击“按建议重试”。");
        } else if (reply.action == "generate") {
            try {
                startAiProposal(*reply.config_patch, aiUi.requestConfig);
            } catch (const std::exception &e) {
                aiUi.generating = false;
                showAiError(0, "invalid_proposal", e.what(),
                            "建议未应用，原配置和数据已保留。请补充缺失条件或要求 AI 修正参数。",
                            false);
                aiUi.localGeneration = {{"status", "invalid_proposal"},
                                        {"reason", e.what()},
                                        {"proposed_patch", *reply.config_patch}};
            }
        }
    }
    if (aiUi.chats[mode].errorText.empty())
        state.hint("AI 结构化回复已校验");
}
void pollAi() {
    if (aiUi.suggestion && aiUi.suggestionRevision != state.revision) {
        aiUi.suggestion.reset();
        appendAi(0, "local", "研究参数已改变，之前的重试建议已作废。请重新描述需求后继续。");
    }
    if (aiUi.requesting) {
        const auto view = aiUi.client.view();
        if (view.status != msa::ai::Status::Running && view.status != msa::ai::Status::Cancelling) {
            aiUi.requesting = false;
            const int mode = aiUi.requestMode;
            if (view.status == msa::ai::Status::Success) {
                auto reply = aiUi.client.take();
                if (reply)
                    acceptAiReply(mode, *reply);
            } else {
                const bool cancelled = view.status == msa::ai::Status::Cancelled;
                showAiError(mode, cancelled ? "cancelled" : view.error_code,
                            cancelled ? "本轮请求已取消，未应用任何返回参数。" : view.error,
                            cancelled ? "可重试本轮，或输入新的要求；不会自动重试。"
                                      : view.recovery,
                            true, aiUi.diagnosis);
            }
        }
    }
    if (aiUi.generating && !state.busy()) {
        aiUi.generating = false;
        if (state.revision != aiUi.generationRevision) {
            aiUi.localGeneration = {{"status", "stale"}, {"config", aiUi.generationConfig}};
            appendAi(
                0, "local",
                "研究配置在生成期间发生变化，本次结果不适用于当前配置；请按当前要求重新发送。");
            return;
        }
        if (state.status == Status::Success && state.validResult()) {
            const auto &result = *state.result;
            aiUi.localGeneration = {{"status", "success"},
                                    {"config", aiUi.generationConfig},
                                    {"metrics", result.metrics},
                                    {"checks", msa::checks(result.metrics, result.config)}};
            std::string summary = "本机生成成功，全部生成约束已通过。";
            for (int i = 0; i < 5; ++i)
                summary += "\n" + metricNames[i] + "：" +
                           native_ui::review_number(result.metrics.at(msa::metric_keys[i]),
                                                    i == 4 ? 0 : 2);
            summary += "\n可在数据页复核并导出 "
                       "CSV，在图表页查看和导出图形。满足约束不等于测量系统合格。";
            appendAi(0, "local", summary);
        } else if (state.status == Status::Cancelled) {
            aiUi.localGeneration = {{"status", "cancelled"}, {"config", aiUi.generationConfig}};
            appendAi(0, "local", "本机生成已取消；未自动重试。可以继续描述新的要求。");
        } else
            diagnoseAiFailure();
    }
}
std::string aiMessageRole(const msa::ai::Message &message) {
    if (message.role == "local")
        return "本机结果";
    if (message.role == "user")
        return message.content.rfind("[程序诊断请求]", 0) == 0 ? "诊断请求" : "你的需求";
    return "AI 回复";
}
void fillAiExample(int mode, int index) {
    static const std::array<std::array<const char *, 3>, 2> examples{
        {{{"请用3名操作员、10个部件、每件测3次，规格9～11，GRR/TV不高于20%，P/"
           "T不高于20%，ndc至少5，生成学习用数据。未提及的参数保持当前设置。",
           "基于当前研究设置，将GRR/"
           "TV调整为不高于20%，ndc至少5，保留其余条件。缺少必要条件时，请先向我确认。",
           "请检查当前生成条件是否完整，解释各项指标目标的含义，以及还需要补充什么。暂不生成数据"
           "。"}},
         {{"请结合本机结果，评价重复性、再现性、量具R&"
           "R、部件区分能力与规格适用性，并说明适用范围。",
           "请解释重复性和再现性各自反映什么，结合当前结果指出应优先核查的测量环节。",
           "请说明本次研究的主要限制、仍需核实的现场条件，以及可执行的改进建议。"}}}};
    auto &input = aiUi.chats.at(mode).input;
    const bool append = !native_ui::trim(input).empty();
    if (append)
        input += "\n\n";
    input += examples.at(mode).at(index);
    aiUi.focusComposer = true;
    state.hint(append ? "示例已追加到输入框，可修改后发送" : "示例已填入，可修改后发送");
}
void aiWelcome(Ui &u, int mode, float w) {
    section(u, "ai.welcome", mode == 0 ? "从需求开始生成" : "读懂当前测量系统", w, [&](float c) {
        paragraph(u, "ai.welcome.text",
                  mode == 0 ? "描述研究设计、规格与目标；条件不完整时，助手会先与你确认。"
                            : "基于当前数据和本机统计结果解释指标，结论仍需结合实际测量用途。",
                  c, muted, 18);
        const std::array<const char *, 3> labels =
            mode == 0 ? std::array<const char *, 3>{"生成演示数据", "调整指标要求", "检查必要条件"}
                      : std::array<const char *, 3>{"整体评价", "解释变异来源", "查看风险与建议"};
        const auto example = [&](int index, float width) {
            button(
                u, "ai.example." + std::to_string(mode) + "." + std::to_string(index),
                labels[index], width, [mode, index] { fillAiExample(mode, index); }, false, false,
                36);
        };
        if (c >= 640)
            u.row("ai.welcome.examples")
                .width(c)
                .gap(10)
                .content([&] {
                    for (int i = 0; i < 3; ++i)
                        example(i, (c - 20) / 3);
                })
                .build();
        else
            for (int i = 0; i < 3; ++i)
                example(i, c);
        paragraph(u, "ai.welcome.example.note", "点击示例只填入编辑框，确认后再发送。", c, muted,
                  17);
    });
}
void aiMessageCard(Ui &u, const std::string &id, const msa::ai::Message &message, float w) {
    const bool diagnostic =
        message.role == "user" && message.content.rfind("[程序诊断请求]", 0) == 0;
    const bool local = message.role == "local" || diagnostic;
    const bool user = message.role == "user" && !diagnostic;
    const auto surface = user ? "#EEF3FF" : local ? "#F5F7FA" : "#FFFFFF";
    components::card(u, id + ".card")
        .width(w)
        .wrapContentHeight()
        .theme(tokens())
        .padding(0)
        .color(surface)
        .radius(12)
        .border(1, user ? "#D8E3FE" : line)
        .content([&] {
            u.column(id)
                .width(w)
                .padding(16)
                .gap(12)
                .content([&] {
                    u.row(id + ".role.row")
                        .width(w - 32)
                        .gap(10)
                        .alignItems(eui::Align::CENTER)
                        .content([&] {
                            badge(u, id + ".role", aiMessageRole(message), 88,
                                  user    ? blue
                                  : local ? muted
                                          : green,
                                  user    ? "#DDE7FF"
                                  : local ? "#E8EDF3"
                                          : "#E3F4EE");
                            text(u, id + ".source",
                                 user    ? "你提供的研究要求"
                                 : local ? "本机执行与校验"
                                         : "基于本轮对话的回复",
                                 w - 130, 26, 12, muted);
                        })
                        .build();
                    paragraph(u, id + ".body", message.content, w - 32, ink, 18);
                })
                .build();
        })
        .build();
}
void assistantPage(Ui &u, float w, float h) {
    auto &chat = aiUi.chats[aiUi.mode];
    u.column("ai.page")
        .size(w, h)
        .gap(10)
        .content([&] {
            u.row("ai.toolbar")
                .width(w)
                .gap(12)
                .content([&] {
                    segments(u, "ai.mode", {"AI 辅助生成", "AI 结果解读"}, aiUi.mode,
                             std::max(220.f, w - 276), [](int i) { aiUi.mode = i; });
                    button(u, "ai.settings", "AI 配置", 120, [] {
                        state.optionsTab = 3;
                        state.navigate(Page::Options);
                    });
                    button(
                        u, "ai.clear", "新建会话", 132,
                        [] {
                            const int mode = aiUi.mode;
                            aiUi.chats[mode] = AiConversation{};
                            if (mode == 0) {
                                aiUi.suggestion.reset();
                                aiUi.localGeneration = nullptr;
                            }
                            state.hint("已新建会话");
                        },
                        false, assistantBusy());
                })
                .build();
            const auto *data = insightResult();
            const auto description =
                aiUi.mode == 0
                    ? "描述研究设计、规格和指标要求；AI "
                      "提议参数，本机生成器校验并生成。失败后可继续对话调整。"
                    : (data ? std::string("解读数据源：") +
                                  (visual.importedSelected ? visual.importedName : "当前生成结果") +
                                  "。发送时将测量数据与统计结果提交至所选 AI 服务。"
                            : "先生成数据，或在图表页导入 "
                              "CSV，再发送解读问题。数据源改变后请新建会话。");
            u.text("ai.context")
                .size(w, 42)
                .text(description)
                .fontSize(17)
                .color(muted)
                .wrap()
                .maxWidth(w)
                .clip()
                .build();
            const float listHeight = std::max(80.f, h - 244);
            components::scrollView(u, "ai.messages." + std::to_string(aiUi.mode))
                .size(w, listHeight)
                .theme(tokens())
                .reserveScrollbar()
                .offset(chat.offset)
                .onChange([mode = aiUi.mode](float y) { aiUi.chats[mode].offset = y; })
                .content([mode = aiUi.mode](Ui &v, float cw, float) {
                    v.column("ai.transcript." + std::to_string(mode))
                        .width(cw)
                        .gap(12)
                        .content([&] {
                            const auto &messages = aiUi.chats[mode].messages;
                            if (messages.empty())
                                aiWelcome(v, mode, cw);
                            for (std::size_t i = 0; i < messages.size(); ++i) {
                                const auto &m = messages[i];
                                const auto id =
                                    "ai.message." + std::to_string(mode) + "." + std::to_string(i);
                                aiMessageCard(v, id, m, cw);
                            }
                            const auto &conversation = aiUi.chats[mode];
                            if (!conversation.errorText.empty())
                                section(v, "ai.error", "本轮未完成 · " + conversation.errorCode, cw,
                                        [&](float c) {
                                            paragraph(v, "ai.error.detail", conversation.errorText,
                                                      c, "#AF3525", 18);
                                            paragraph(v, "ai.error.recovery", conversation.recovery,
                                                      c, muted, 17);
                                            v.row("ai.error.actions")
                                                .width(c)
                                                .gap(12)
                                                .content([&] {
                                                    if (conversation.canRetry)
                                                        button(v, "ai.error.retry", "重试本轮", 132,
                                                               retryAiRequest, true,
                                                               assistantBusy());
                                                    button(v, "ai.error.settings", "检查 AI 配置",
                                                           148, [] {
                                                               state.optionsTab = 3;
                                                               state.navigate(Page::Options);
                                                           });
                                                })
                                                .build();
                                        });
                            if ((aiUi.requesting && mode == aiUi.requestMode) ||
                                (aiUi.generating && mode == 0))
                                text(v, "ai.waiting",
                                     aiUi.generating ? "本机正在搜索符合约束的数据…"
                                                     : "AI 正在处理，请稍候…",
                                     cw, 36, 14, blue);
                        })
                        .build();
                })
                .build();
            localInput(u, "ai.composer." + std::to_string(aiUi.mode), chat.input, w,
                       aiUi.mode == 0 ? "输入生成需求，或继续调整参数…"
                                      : "输入关于当前数据的分析问题…",
                       88);
            const auto characters =
                std::count_if(chat.input.begin(), chat.input.end(),
                              [](unsigned char ch) { return (ch & 0xc0) != 0x80; });
            const bool tooLong = chat.input.size() > 16000;
            const std::string inputHint =
                tooLong                     ? "内容超过长度限制，请精简后发送"
                : assistantBusy()           ? "可继续编辑下一轮问题，当前任务结束后再发送"
                : chat.input.size() > 14000 ? "内容接近长度上限，建议精简需求"
                                            : "确认内容后点击发送；换行不会发送消息";
            u.row("ai.composer.feedback")
                .size(w, 24)
                .gap(12)
                .content([&] {
                    text(u, "ai.composer.hint", inputHint, w - 142, 24, 12,
                         tooLong ? "#AF3525" : muted);
                    text(u, "ai.composer.count", std::to_string(characters) + " 字符", 130, 24, 12,
                         muted);
                })
                .build();
            if (aiUi.focusComposer) {
                u.requestFocus("ai.composer." + std::to_string(aiUi.mode) + ".hit");
                aiUi.focusComposer = false;
            }
        })
        .build();
}
void assistantFooter(Ui &u, float w) {
    footerStatus(u, std::max(90.f, w - 430),
                 aiUi.serviceBusy  ? "AI 连接检查中，可取消或返回配置页查看"
                 : aiUi.requesting ? "AI 请求处理中"
                 : aiUi.generating ? "本机数据生成中"
                                   : "对话保留在本次会话，关闭程序后清除");
    button(
        u, "ai.copy", "复制对话", 108,
        [] {
            safe([] {
                std::string content;
                for (const auto &m : aiUi.chats[aiUi.mode].messages)
                    content += aiMessageRole(m) + "：\n" + m.content + "\n\n";
                platform::clipboard(platform::wide(content));
                state.hint("对话已复制");
            });
        },
        false, aiUi.chats[aiUi.mode].messages.empty());
    if (aiUi.mode == 0 && aiUi.suggestion && !assistantBusy())
        button(u, "ai.retry", "按建议重试", 138, [] {
            safe([] {
                const auto suggestion = *aiUi.suggestion;
                startAiProposal(suggestion, aiConfig());
                aiUi.suggestion.reset();
            });
        });
    else
        button(u, "ai.charts", "结果图表", 138, [] { state.navigate(Page::Charts); });
    button(
        u, "ai.send", assistantBusy() ? "取消" : "发送", 148,
        [] {
            if (aiUi.serviceBusy)
                aiUi.serviceClient.cancel();
            else if (aiUi.requesting)
                aiUi.client.cancel();
            else if (state.busy()) {
                state.status = Status::Cancelling;
                state.cancel = true;
            } else
                sendAi();
        },
        true, !assistantBusy() && native_ui::trim(aiUi.chats[aiUi.mode].input).empty());
}
void loadExtensions() {
    try {
        aiUi.settings = msa::ai::load_settings();
        aiUi.timeout = std::to_string(aiUi.settings.timeout_seconds);
    } catch (const std::exception &e) {
        state.feedback("AI 配置载入失败", e.what(), Level::Warning);
    }
}
void pollExtensions() {
    pollVisuals();
    pollAiService();
    pollAi();
}
void stopExtensions() {
    aiUi.serviceClient.stop();
    aiUi.client.stop();
    if (visual.exporting.valid())
        visual.exporting.wait();
}
