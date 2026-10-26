#include "msa/ai.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <regex>
#include <thread>

namespace {
using msa::Json;
using namespace msa::ai;
using Clock = std::chrono::steady_clock;
const std::string fakeKey = "msa-test-only-not-a-real-api-key";
Json checks = Json::array();

void require(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}
std::string redact(std::string text, const std::string &key = fakeKey) {
    if (!key.empty()) {
        std::size_t offset = 0;
        while ((offset = text.find(key, offset)) != std::string::npos) {
            text.replace(offset, key.size(), "[已隐藏]");
            offset += std::string("[已隐藏]").size();
        }
    }
    return text;
}
template <class Work> void check(const std::string &name, Work work) {
    try {
        work();
        checks.push_back({{"name", name}, {"passed", true}});
    } catch (const std::exception &e) {
        checks.push_back({{"name", name}, {"passed", false}, {"error", redact(e.what())}});
    }
}
template <class Work> void rejects(Work work) {
    bool rejected = false;
    try {
        work();
    } catch (const std::exception &) {
        rejected = true;
    }
    require(rejected, "无效输入未被拒绝");
}
template <class Work> void rejects_with_code(const std::string &code, Work work) {
    try {
        work();
    } catch (const Error &error) {
        require(error.code == code, "错误分类与预期不一致：" + error.code);
        require(!error.recovery.empty(), "错误缺少恢复建议");
        return;
    }
    throw std::runtime_error("无效输入未返回预期的受控错误");
}
Json configuration() {
    auto config = msa::default_config();
    config["specification"]["lsl"] = 9;
    config["specification"]["usl"] = 11;
    config["generation"]["seed"] = 123;
    return msa::normalize_config(config);
}
Json context() {
    return {
        {"current_config", configuration()},
        {"metrics",
         {{"EV_pct", 10.0}, {"AV_pct", 5.0}, {"GRR_pct", 11.18}, {"PT_pct", 12.0}, {"ndc", 12}}}};
}
void redact_values(Json &value, const std::string &key) {
    if (value.is_string())
        value = redact(value.get<std::string>(), key);
    else if (value.is_array() || value.is_object())
        for (auto &child : value)
            redact_values(child, key);
}
Json generation_reply(const std::string &action = "generate") {
    Json value{{"schema_version", 1},
               {"task", "generate"},
               {"action", action},
               {"message", "按指定研究设计生成模拟数据，由本机内核核对指标。"},
               {"questions", Json::array()},
               {"warnings", Json::array()},
               {"config_patch", {{"generation", {{"seed", 456}}}}}};
    if (action == "clarify") {
        value["questions"] = {"请提供规格上限和下限。"};
        value["config_patch"] = nullptr;
    }
    if (action == "diagnose")
        value["message"] = "搜索预算耗尽并不证明数学无解，请先确认是否放宽目标允许误差。";
    return value;
}
Json interpretation_reply() {
    return {{"schema_version", 1},
            {"task", "interpret"},
            {"action", "evaluate"},
            {"message", "根据给定数据和本机结果解释测量系统。"},
            {"questions", Json::array()},
            {"warnings", {"模拟数据不代表现场证据。"}},
            {"evaluation",
             {{"conclusion", "应结合用途与规格判断适用性。"},
              {"findings",
               {{{"metric", "GRR_pct"}, {"assessment", "应按研究变异口径解释。"}},
                {{"metric", "ndc"}, {"assessment", "应核对实际区分类别数。"}}}},
              {"recommendations", {"核对样本代表性与测量流程。"}}}}};
}
Json envelope(const Json &reply, const std::string &finish = "stop") {
    return {{"choices",
             {{{"finish_reason", finish},
               {"message", {{"role", "assistant"}, {"content", reply.dump()}}}}}}};
}

void protocol_checks(const std::filesystem::path &output) {
    check("生成回复保留结构、动作和参数补丁", [] {
        const auto reply = parse_reply(generation_reply().dump(), true, context());
        require(reply.action == "generate" && reply.structured == generation_reply() &&
                    reply.config_patch && (*reply.config_patch)["generation"]["seed"] == 456,
                "生成回复解析不完整");
    });
    check("澄清不提供可执行补丁", [] {
        const auto reply = parse_reply(generation_reply("clarify").dump(), true, context());
        require(reply.action == "clarify" && !reply.config_patch, "澄清回复不应含可执行补丁");
    });
    check("失败诊断保留建议补丁供人工重试", [] {
        const auto reply = parse_reply(generation_reply("diagnose").dump(), true, context());
        require(reply.action == "diagnose" && reply.config_patch, "诊断建议未保留或动作被篡改");
    });
    check("解读回复使用严格评价对象", [] {
        const auto reply = parse_reply(interpretation_reply().dump(), false, context());
        require(reply.action == "evaluate" && !reply.config_patch &&
                    reply.structured["evaluation"]["findings"].size() == 2,
                "解读结构不完整");
    });
    check("解读澄清必须携带问题且没有评价结论", [] {
        auto value = interpretation_reply();
        value["action"] = "clarify";
        value["evaluation"] = nullptr;
        value["questions"] = {"请补充规格依据。"};
        require(parse_reply(value.dump(), false, context()).action == "clarify", "解读澄清失败");
    });

    const auto valid = generation_reply().dump();
    const std::vector<std::pair<std::string, std::string>> malformed{
        {"拒绝 Markdown 围栏", "```json\n" + valid + "\n```"},
        {"拒绝 JSON 前解释文字", "以下是参数：\n" + valid},
        {"拒绝 JSON 后解释文字", valid + "\n已经完成"},
        {"拒绝 JSON 数组", "[" + valid + "]"},
        {"拒绝 JSON 标量", "null"},
        {"拒绝空回复", " \n\t"},
        {"拒绝重复顶层键", "{\"schema_version\":1," + valid.substr(1)},
        {"拒绝重复嵌套键", "{\"schema_version\":1,\"task\":\"generate\",\"action\":\"generate\","
                           "\"message\":\"参数\",\"questions\":[],\"warnings\":[],\"config_patch\":"
                           "{\"generation\":{\"seed\":1,\"seed\":2}}}"},
        {"拒绝重复评价字段", "{\"schema_version\":1,\"task\":\"interpret\",\"action\":\"evaluate\","
                             "\"message\":\"评价\",\"questions\":[],\"warnings\":[],\"evaluation\":"
                             "{\"conclusion\":\"a\",\"conclusion\":\"b\",\"findings\":[{\"metric\":"
                             "\"ndc\",\"assessment\":\"说明\"}],\"recommendations\":[]}}"},
        {"拒绝非有限数值",
         "{\"schema_version\":1,\"task\":\"generate\",\"action\":\"generate\",\"message\":\"参数\","
         "\"questions\":[],\"warnings\":[],\"config_patch\":{\"generation\":{\"center\":1e999}}}"}};
    for (const auto &item : malformed)
        check(item.first, [&] {
            rejects([&] { parse_reply(item.second, item.first != "拒绝重复评价字段", context()); });
        });

    const auto invalid_generation = [&](const std::string &name,
                                        const std::function<void(Json &)> &mutate) {
        check(name, [&] {
            auto value = generation_reply();
            mutate(value);
            rejects([&] { parse_reply(value.dump(), true, context()); });
        });
    };
    invalid_generation("拒绝协议之外的顶层字段", [](Json &j) { j["command"] = "ignored"; });
    invalid_generation("拒绝错误协议版本", [](Json &j) { j["schema_version"] = 2; });
    invalid_generation("拒绝小数协议版本", [](Json &j) { j["schema_version"] = 1.0; });
    invalid_generation("拒绝生成任务混用解读模式", [](Json &j) { j["task"] = "interpret"; });
    invalid_generation("拒绝不存在的动作", [](Json &j) { j["action"] = "execute"; });
    invalid_generation("拒绝空说明", [](Json &j) { j["message"] = " \n"; });
    invalid_generation("拒绝数字说明", [](Json &j) { j["message"] = 12; });
    invalid_generation("拒绝遗漏必需字段", [](Json &j) { j.erase("warnings"); });
    invalid_generation("拒绝非字符串问题", [](Json &j) { j["questions"] = {123}; });
    invalid_generation("拒绝非数组警告", [](Json &j) { j["warnings"] = "warning"; });
    invalid_generation("生成动作必须有对象补丁", [](Json &j) { j["config_patch"] = nullptr; });
    invalid_generation("澄清动作不能携带补丁", [](Json &j) {
        j["action"] = "clarify";
        j["questions"] = {"请确认"};
    });
    invalid_generation("澄清动作必须有问题", [](Json &j) {
        j["action"] = "clarify";
        j["config_patch"] = nullptr;
    });
    invalid_generation("拒绝输出路径越权补丁", [](Json &j) {
        j["config_patch"] = {{"output", {{"directory", "forbidden-output"}}}};
    });
    invalid_generation("拒绝接口密钥越权补丁",
                       [](Json &j) { j["config_patch"] = {{"api_key", "not-a-key"}}; });
    invalid_generation("拒绝补丁中的未知配置键", [](Json &j) {
        j["config_patch"] = {{"generation", {{"unknown_parameter", true}}}};
    });
    invalid_generation("拒绝不支持的研究设计",
                       [](Json &j) { j["config_patch"] = {{"design", {{"operators", 99}}}}; });
    invalid_generation("拒绝无效小数精度", [](Json &j) {
        j["config_patch"] = {{"generation", {{"decimal_places", -1}}}};
    });
    invalid_generation("拒绝反向规格限", [](Json &j) {
        j["config_patch"] = {{"specification", {{"lsl", 11}, {"usl", 9}}}};
    });
    invalid_generation("拒绝错误布尔类型", [](Json &j) {
        j["config_patch"] = {{"analysis", {{"misclassification", "true"}}}};
    });
    const auto invalid_interpretation = [&](const std::string &name,
                                            const std::function<void(Json &)> &mutate) {
        check(name, [&] {
            auto value = interpretation_reply();
            mutate(value);
            rejects([&] { parse_reply(value.dump(), false, context()); });
        });
    };
    invalid_interpretation("解读不可混入配置补丁",
                           [](Json &j) { j["config_patch"] = Json::object(); });
    invalid_interpretation("评价动作必须有评价对象", [](Json &j) { j["evaluation"] = nullptr; });
    invalid_interpretation("评价对象拒绝未知字段", [](Json &j) { j["evaluation"]["score"] = 100; });
    invalid_interpretation("评价指标不得重复", [](Json &j) {
        j["evaluation"]["findings"].push_back(j["evaluation"]["findings"][0]);
    });
    invalid_interpretation("评价指标须为支持的指标键", [](Json &j) {
        j["evaluation"]["findings"][0]["metric"] = "imaginary_metric";
    });
    invalid_interpretation("评价须有至少一项指标依据",
                           [](Json &j) { j["evaluation"]["findings"] = Json::array(); });
    check("补丁保留无关业务配置与输出设置", [] {
        const auto base = configuration();
        const auto changed = apply_config_patch(base, {{"generation", {{"seed", 456}}}});
        auto expected = base;
        expected["generation"]["seed"] = 456;
        require(changed == expected, "补丁修改了未声明的业务配置");
    });
    check("指标模式切换不残留旧模式字段", [] {
        auto base = configuration();
        base["targets"]["EV_pct"] = {{"mode", "between"}, {"min", 5}, {"max", 15}};
        const auto patched = apply_config_patch(
            base, {{"targets", {{"EV_pct", {{"mode", "at_most"}, {"value", 10}}}}}});
        require(patched["targets"]["EV_pct"] == Json({{"mode", "at_most"}, {"value", 10}}),
                "旧指标字段未清理");
    });
    check("失败诊断上下文禁止自动再次生成", [] {
        auto data = context();
        data["last_generation"] = {{"qualified", false}, {"reason", "搜索预算耗尽"}};
        rejects([&] { parse_reply(generation_reply().dump(), true, data); });
        require(parse_reply(generation_reply("diagnose").dump(), true, data).action == "diagnose",
                "有效失败诊断未被接受");
    });
    const auto not_ready = [&](const std::string &name, const std::function<void(Json &)> &mutate) {
        check(name, [&] {
            auto data = context();
            mutate(data["current_config"]);
            // 配置可以保存，但未满足生成前提时不能接受立即生成动作。
            msa::normalize_config(data["current_config"]);
            rejects([&] { msa::validate_request(data["current_config"]); });
            auto proposal = generation_reply();
            proposal["config_patch"] = Json::object();
            rejects_with_code("invalid_config", [&] { parse_reply(proposal.dump(), true, data); });
            proposal["action"] = "diagnose";
            const auto diagnosis = parse_reply(proposal.dump(), true, data);
            require(diagnosis.action == "diagnose" && diagnosis.config_patch &&
                        diagnosis.config_patch->empty(),
                    "未完成的诊断建议不应被当成生成请求或完全丢弃");
        });
    };
    not_ready("严格模式缺少所有规格与尺度时拒绝生成并允许诊断建议", [](Json &config) {
        config["specification"]["lsl"] = nullptr;
        config["specification"]["usl"] = nullptr;
    });
    not_ready("有参考尺度但严格模式缺少规格时仍拒绝生成", [](Json &config) {
        config["specification"]["lsl"] = nullptr;
        config["specification"]["usl"] = nullptr;
        config["generation"]["reference_tv_sd"] = 0.2;
    });
    not_ready("单侧规格缺少尺度时拒绝生成并允许诊断建议",
              [](Json &config) { config["specification"]["usl"] = nullptr; });
    not_ready("宽松模式缺少规格和尺度时拒绝生成", [](Json &config) {
        config["specification"]["lsl"] = nullptr;
        config["specification"]["usl"] = nullptr;
        config["specification"]["compliance"] = "relaxed";
    });
    not_ready("显式参考尺度模式未填写尺度时拒绝生成",
              [](Json &config) { config["generation"]["scale_mode"] = "reference_tv_sd"; });
    not_ready("有尺度但 P/T 缺少规格或容差时拒绝生成", [](Json &config) {
        config["specification"]["lsl"] = nullptr;
        config["specification"]["usl"] = nullptr;
        config["specification"]["compliance"] = "relaxed";
        config["generation"]["reference_tv_sd"] = 0.2;
        config["targets"]["PT_pct"] = {{"mode", "at_most"}, {"value", 20}};
    });
    not_ready("指标约束无共同解时拒绝生成并允许诊断建议", [](Json &config) {
        config["targets"]["EV_pct"] = {{"mode", "at_least"}, {"value", 50}};
        config["targets"]["GRR_pct"] = {{"mode", "at_most"}, {"value", 10}};
    });
    not_ready("规格范围不足两个输出精度数值时拒绝生成", [](Json &config) {
        config["specification"]["lsl"] = 9.01;
        config["specification"]["usl"] = 9.02;
        config["generation"]["decimal_places"] = 0;
    });
    check("参数补丁补齐双侧规格后允许生成", [] {
        auto data = context();
        data["current_config"] = msa::default_config();
        auto proposal = generation_reply();
        proposal["config_patch"] = {{"specification", {{"lsl", 9}, {"usl", 11}}}};
        const auto reply = parse_reply(proposal.dump(), true, data);
        require(reply.action == "generate" && reply.config_patch, "满足生成前提的完整补丁未被接受");
        msa::validate_request(apply_config_patch(data["current_config"], *reply.config_patch));
    });
    check("单侧规格补齐参考尺度或历史标准差后允许生成", [] {
        auto data = context();
        data["current_config"]["specification"]["usl"] = nullptr;
        for (const auto &patch : std::vector<Json>{{{"generation", {{"reference_tv_sd", 0.2}}}},
                                                   {{"analysis", {{"historical_sd", 0.2}}}}}) {
            auto proposal = generation_reply();
            proposal["config_patch"] = patch;
            const auto reply = parse_reply(proposal.dump(), true, data);
            require(reply.action == "generate" && reply.config_patch,
                    "已补齐尺度的单侧规格配置未被接受");
            msa::validate_request(apply_config_patch(data["current_config"], *reply.config_patch));
        }
    });
    check("补丁自身引入不可行约束时拒绝生成但保留诊断动作", [] {
        auto proposal = generation_reply();
        proposal["config_patch"] = {{"targets",
                                     {{"EV_pct", {{"mode", "at_least"}, {"value", 50}}},
                                      {"GRR_pct", {{"mode", "at_most"}, {"value", 10}}}}}};
        rejects_with_code("invalid_config", [&] { parse_reply(proposal.dump(), true, context()); });
        proposal["action"] = "diagnose";
        require(parse_reply(proposal.dump(), true, context()).action == "diagnose",
                "结构有效的诊断建议应保留供用户确认修改");
    });
    check("解读必须引用当前本机指标", [] {
        auto data = context();
        data["metrics"].erase("ndc");
        rejects([&] { parse_reply(interpretation_reply().dump(), false, data); });
        data.erase("metrics");
        rejects([&] { parse_reply(interpretation_reply().dump(), false, data); });
    });
    check("响应 envelope 与严格协议联合校验", [] {
        require(decode_response(envelope(generation_reply()).dump(), true, context()).action ==
                    "generate",
                "有效 envelope 未解析");
        rejects([] {
            decode_response(envelope(generation_reply(), "length").dump(), true, context());
        });
        rejects([] { decode_response("{\"choices\":[]}", true, context()); });
        rejects([] { decode_response("not-json", true, context()); });
    });

    check("请求采用严格 JSON 模式且不流式输出", [] {
        const auto request =
            build_request(defaults(), {{"user", "生成符合要求的数据"}}, true, context());
        require(request["response_format"]["type"] == "json_object" && request["stream"] == false,
                "未声明严格 JSON 响应模式");
        require(request["thinking"]["type"] == "disabled", "DeepSeek 未关闭思考输出");
    });
    check("请求上下文明示本机生成前提检查结果", [] {
        for (const bool ready : {false, true}) {
            auto data = context();
            if (!ready)
                data["current_config"] = msa::default_config();
            // 即使传入内容已有同名字段，也必须以本机重新计算结果为准。
            data["generation_readiness"] = {{"can_generate", !ready}, {"reason", "过时结果"}};
            const auto request = build_request(defaults(), {{"user", "生成"}}, true, data);
            const auto content = request["messages"].back()["content"].get<std::string>();
            const std::string startMarker = "<msa_context>\n", endMarker = "\n</msa_context>";
            const auto start = content.find(startMarker), end = content.rfind(endMarker);
            require(start != std::string::npos && end != std::string::npos && end > start,
                    "请求缺少独立只读上下文区块");
            const auto sent = Json::parse(
                content.substr(start + startMarker.size(), end - start - startMarker.size()));
            const auto readiness = sent.at("generation_readiness");
            require(readiness.at("can_generate") == ready && readiness.at("reason").is_string(),
                    "生成前提检查结果未重新计算或格式不正确");
            if (!ready)
                require(!readiness.at("reason").get<std::string>().empty(),
                        "无法生成时缺少明确原因");
        }
    });
    check("生成和解读系统提示词相互隔离", [] {
        auto settings = defaults();
        settings.generation_system_prompt = "GENERATION_ONLY_SENTINEL";
        settings.interpretation_system_prompt = "INTERPRETATION_ONLY_SENTINEL";
        const auto generation = build_request(settings, {{"user", "生成"}}, true, context()).dump();
        const auto interpretation =
            build_request(settings, {{"user", "解读"}}, false, context()).dump();
        require(generation.find("GENERATION_ONLY_SENTINEL") != std::string::npos &&
                    generation.find("INTERPRETATION_ONLY_SENTINEL") == std::string::npos &&
                    interpretation.find("INTERPRETATION_ONLY_SENTINEL") != std::string::npos &&
                    interpretation.find("GENERATION_ONLY_SENTINEL") == std::string::npos,
                "两类系统提示词发生串用");
    });
    check("上下文不发送密钥、输出目录或提示词", [] {
        auto settings = defaults();
        settings.api_key = fakeKey;
        auto data = context();
        data["current_config"]["output"]["directory"] = "PRIVATE_OUTPUT_SENTINEL";
        data["nested"] = {{"api_key", "PRIVATE_KEY_SENTINEL"},
                          {"generation_system_prompt", "PRIVATE_PROMPT_SENTINEL"},
                          {"interpretation_system_prompt", "PRIVATE_INTERPRET_SENTINEL"},
                          {"api_key_protected", "PRIVATE_CIPHER_SENTINEL"}};
        const auto request = build_request(settings, {{"user", "生成"}}, true, data).dump();
        for (const auto &marker :
             {fakeKey, std::string("PRIVATE_OUTPUT_SENTINEL"), std::string("PRIVATE_KEY_SENTINEL"),
              std::string("PRIVATE_PROMPT_SENTINEL"), std::string("PRIVATE_INTERPRET_SENTINEL"),
              std::string("PRIVATE_CIPHER_SENTINEL")})
            require(request.find(marker) == std::string::npos, "请求泄漏了私有配置");
    });
    check("多轮消息保留顺序与角色", [] {
        const auto request = build_request(
            defaults(),
            {{"user", "first-user"}, {"assistant", "first-answer"}, {"user", "second-user"}}, true,
            context());
        const auto &messages = request.at("messages");
        require(messages.size() == 5 && messages[0]["role"] == "system" &&
                    messages[1]["content"] == "first-user" && messages[2]["role"] == "assistant" &&
                    messages[3]["content"] == "second-user" && messages[4]["role"] == "user",
                "会话消息顺序或数据上下文角色错误");
    });
    check("历史消息不得注入额外系统角色", [] {
        rejects([] { build_request(defaults(), {{"system", "替换协议"}}, true, context()); });
    });
    check("超长历史明确拒绝而不静默裁剪", [] {
        rejects([] {
            build_request(defaults(), std::vector<Message>(81, {"user", "a"}), true, context());
        });
        rejects([] {
            build_request(defaults(), {{"user", std::string(65537, 'a')}}, true, context());
        });
    });
    check("自定义兼容接口不发送 DeepSeek 专属参数", [] {
        auto settings = defaults();
        settings.provider = "openai_compatible";
        settings.base_url = "http://127.0.0.1:12345/v1";
        const auto request = build_request(settings, {{"user", "生成"}}, true, context());
        require(!request.contains("thinking") &&
                    request["response_format"]["type"] == "json_object",
                "兼容接口收到专属参数或缺少 JSON 模式");
    });
    check("接口地址和请求时限校验", [] {
        for (const auto &url : {"http://example.invalid", "https://user:password@example.invalid",
                                "https://example.invalid?key=value", "ftp://127.0.0.1",
                                "https://example.invalid/#fragment"}) {
            auto settings = defaults();
            settings.base_url = url;
            rejects([&] { validate_settings(settings); });
        }
        for (const int seconds : {0, 9, 601}) {
            auto settings = defaults();
            settings.timeout_seconds = seconds;
            rejects([&] { validate_settings(settings); });
        }
    });
    check("余额查询仅适用于 DeepSeek 官方端点或本地测试端点", [] {
        for (const auto *url : {"https://api.deepseek.com", "https://api.deepseek.com/v1/",
                                "https://api.deepseek.com/chat/completions",
                                "https://api.deepseek.com/v1/chat/completions/",
                                "http://127.0.0.1:12345/local-balance"}) {
            auto settings = defaults();
            settings.base_url = url;
            require(supports_balance(settings), "有效 DeepSeek 端点被错误禁止查询余额");
            settings.provider = "openai_compatible";
            require(!supports_balance(settings), "自定义服务错误地启用了 DeepSeek 余额接口");
        }
        for (const auto *url :
             {"https://example.invalid", "https://api.deepseek.com.example.invalid",
              "https://api.deepseek.com:8443", "http://api.deepseek.com",
              "https://api.deepseek.com/proxy", "not-a-url"}) {
            auto settings = defaults();
            settings.base_url = url;
            require(!supports_balance(settings), "非官方或错误端点启用了余额查询");
        }
    });
    check("DPAPI 保存密钥无明文且配置可往返", [&] {
        auto settings = defaults();
        settings.api_key = fakeKey;
        settings.model = "unit-test-model";
        settings.generation_system_prompt = "仅用于生成的自定义提示";
        settings.interpretation_system_prompt = "仅用于解读的自定义提示";
        const auto path = output / "protected-settings.json";
        save_settings(settings, path);
        const auto bytes = msa::read_text(path);
        const auto stored = Json::parse(bytes);
        require(bytes.find(fakeKey) == std::string::npos && !stored.contains("api_key") &&
                    !stored["api_key_protected"].get<std::string>().empty(),
                "配置包含明文密钥");
        const auto loaded = load_settings(path);
        require(loaded.api_key == settings.api_key && loaded.model == settings.model &&
                    loaded.generation_system_prompt == settings.generation_system_prompt &&
                    loaded.interpretation_system_prompt == settings.interpretation_system_prompt,
                "加密配置往返不一致");
    });
    check("明文密钥配置与损坏密文均拒绝", [&] {
        const auto path = output / "invalid-settings.json";
        msa::atomic_text(path, Json({{"schema_version", 1}, {"api_key", fakeKey}}).dump());
        rejects([&] { load_settings(path); });
        msa::atomic_text(path, Json({{"schema_version", 1}, {"api_key_protected", "00"}}).dump());
        rejects([&] { load_settings(path); });
    });
}

View await_request(AsyncClient &client, int cancelAfterMs = -1, int hardLimitSeconds = 25) {
    const auto started = Clock::now();
    bool cancelled = false;
    for (;;) {
        auto view = client.view();
        if (view.status != Status::Running && view.status != Status::Cancelling)
            return view;
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
        if (cancelAfterMs >= 0 && elapsed >= cancelAfterMs && !cancelled) {
            client.cancel();
            cancelled = true;
        }
        if (elapsed > hardLimitSeconds * 1000) {
            client.cancel();
            throw std::runtime_error("AI 请求测试超过硬时限");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

void service_checks(const std::string &base, const std::filesystem::path &output) {
    Json exchanges = Json::array();
    const auto settingsFor = [&](const std::string &suffix, ServiceAction action) {
        auto settings = defaults();
        settings.provider = action == ServiceAction::Balance ? "deepseek" : "openai_compatible";
        settings.base_url = base + suffix;
        settings.model = "local-mock-model";
        settings.api_key = fakeKey;
        settings.timeout_seconds = 10;
        settings.generation_system_prompt = "PRIVATE_GENERATION_PROMPT_SENTINEL";
        settings.interpretation_system_prompt = "PRIVATE_INTERPRETATION_PROMPT_SENTINEL";
        return settings;
    };
    const auto send = [&](const std::string &name, const std::string &suffix, ServiceAction action,
                          int cancelMs = -1) {
        auto settings = settingsFor(suffix, action);
        if (name == "connection-deepseek")
            settings.provider = "deepseek";
        if (name == "models-unconfigured") {
            settings.model.clear();
            settings.generation_system_prompt.clear();
            settings.interpretation_system_prompt.clear();
        }
        AsyncClient client;
        std::atomic<unsigned> notices{0};
        client.start_service(settings, action, [&] { ++notices; });
        const auto view = await_request(client, cancelMs);
        require(!view.reply && !client.take(), "服务查询错误地生成或消耗了研究回复");
        const auto reply = client.take_service();
        require(bool(reply) == bool(view.service_reply), "服务查询结果被其他消费接口提前取走");
        require(!client.take_service(), "同一服务查询被重复消费");
        client.stop();
        require(notices.load() >= 1, "服务查询完成未通知调用方");
        for (const auto &message : {view.error, view.error_code, view.recovery})
            for (const auto &marker :
                 {fakeKey, std::string("PRIVATE_SERVER_ERROR_SENTINEL"),
                  settings.generation_system_prompt, settings.interpretation_system_prompt})
                require(marker.empty() || message.find(marker) == std::string::npos,
                        "服务错误消息包含凭据、系统提示词或原始服务端内容");
        exchanges.push_back({{"case", name},
                             {"status", static_cast<int>(view.status)},
                             {"http_status", view.http_status},
                             {"error_code", view.error_code},
                             {"elapsed_seconds", view.elapsed_seconds},
                             {"has_service_reply", bool(reply)}});
        return view;
    };
    const auto expectedModels = std::vector<std::string>{"local-mock-model", "second-mock-model"};
    check("服务操作前置校验失败不发送请求", [&] {
        AsyncClient client;
        auto settings = settingsFor("/service-never-send", ServiceAction::Models);
        rejects_with_code("invalid_action",
                          [&] { client.start_service(settings, static_cast<ServiceAction>(99)); });
        settings.model.clear();
        rejects_with_code("invalid_settings",
                          [&] { client.start_service(settings, ServiceAction::TestConnection); });
        settings.model = "local-mock-model";
        rejects_with_code("balance_unsupported",
                          [&] { client.start_service(settings, ServiceAction::Balance); });
        require(client.view().status != Status::Running && !client.take_service(),
                "前置校验失败后仍在后台请求服务");
    });
    for (const auto &entry : std::vector<std::pair<std::string, std::string>>{
             {"models-root", ""},
             {"models-v1", "/v1/"},
             {"models-full", "/models-full/chat/completions/"},
             {"models-duplicate", "/models-duplicate"},
             {"models-unconfigured", "/models-unconfigured"}})
        check("模型列表路径、内容与去重：" + entry.first, [&] {
            const auto view = send(entry.first, entry.second, ServiceAction::Models);
            require(view.status == Status::Success && view.service_reply &&
                        view.service_reply->models == expectedModels &&
                        !view.service_reply->balance,
                    "模型列表未保留有效 ID、顺序或重复项未去重");
        });
    check("空模型列表保留为空供用户手动填写", [&] {
        const auto view = send("models-empty", "/models-empty", ServiceAction::Models);
        require(view.status == Status::Success && view.service_reply &&
                    view.service_reply->models.empty() && !view.service_reply->balance,
                "空列表被替换为伪造模型或混为查询失败");
    });
    for (const auto *route :
         {"models-missing-data", "models-not-array", "models-numeric-id", "models-blank-id",
          "models-control-id", "models-long-id", "models-missing-id", "models-non-object",
          "models-too-many", "models-malformed", "models-truncated", "models-oversized"})
        check(std::string("模型列表拒绝无效响应：") + route, [&] {
            const auto view = send(route, "/" + std::string(route), ServiceAction::Models);
            require(view.status == Status::Failed && !view.service_reply &&
                        !view.error_code.empty() && !view.recovery.empty(),
                    "无效模型列表被接受或错误缺少恢复说明");
        });
    check("连接测试使用所选模型并验证最小 JSON 响应", [&] {
        const auto view = send("connection-success", "/connection-success/chat/completions/",
                               ServiceAction::TestConnection);
        require(view.status == Status::Success && view.service_reply &&
                    view.service_reply->models.empty() && !view.service_reply->balance,
                "有效连接探测没有成功或混入模型列表/余额结果");
    });
    check("连接测试接受合法 JSON 对象而不套用业务响应协议", [&] {
        const auto view =
            send("connection-object", "/connection-object", ServiceAction::TestConnection);
        require(view.status == Status::Success && view.service_reply,
                "连接探测错误要求业务参数或固定对象字段");
    });
    check("DeepSeek 连接测试使用最小请求并关闭思考输出", [&] {
        const auto view =
            send("connection-deepseek", "/connection-deepseek", ServiceAction::TestConnection);
        require(view.status == Status::Success && view.service_reply,
                "DeepSeek 专属连接请求格式不正确");
    });
    for (const auto *route :
         {"connection-empty-choices", "connection-non-json", "connection-fenced",
          "connection-length", "connection-array", "connection-role", "connection-multi",
          "connection-malformed", "connection-unsupported-json"})
        check(std::string("连接测试拒绝非预期成功结构：") + route, [&] {
            const auto view = send(route, "/" + std::string(route), ServiceAction::TestConnection);
            require(view.status == Status::Failed && !view.service_reply &&
                        !view.error_code.empty() && !view.recovery.empty(),
                    "连接探测将异常或不支持 JSON 的响应显示为成功");
        });
    check("余额查询保留币种和原始金额精度", [&] {
        const auto view = send("balance-success", "/balance-success", ServiceAction::Balance);
        require(view.status == Status::Success && view.service_reply &&
                    view.service_reply->models.empty() && view.service_reply->balance,
                "有效余额查询未成功");
        const auto &balance = *view.service_reply->balance;
        require(balance.at("is_available") == true && balance.at("balance_infos").size() == 1 &&
                    balance.at("balance_infos")[0].at("currency") == "CNY" &&
                    balance.at("balance_infos")[0].at("total_balance") == "12.34" &&
                    balance.at("balance_infos")[0].at("granted_balance") == "2.34" &&
                    balance.at("balance_infos")[0].at("topped_up_balance") == "10.00",
                "余额、币种或可用状态发生错误转换");
    });
    check("余额不足是成功查询结果而不是传输失败", [&] {
        const auto view =
            send("balance-unavailable", "/balance-unavailable", ServiceAction::Balance);
        require(
            view.status == Status::Success && view.service_reply && view.service_reply->balance &&
                view.service_reply->balance->at("is_available") == false &&
                view.service_reply->balance->at("balance_infos")[0].at("total_balance") == "0.00",
            "余额不可用被当成查询失败或零余额被改变");
    });
    check("HTTP 402 与有效零余额响应区分", [&] {
        const auto view = send("balance-status-402", "/balance-status-402", ServiceAction::Balance);
        require(view.status == Status::Failed && view.http_status == 402 && !view.service_reply &&
                    !view.error_code.empty() && !view.recovery.empty(),
                "账户 HTTP 错误伪装成有效余额");
    });
    for (const auto *route :
         {"balance-non-bool", "balance-missing-infos", "balance-non-array", "balance-bad-amount",
          "balance-missing-currency", "balance-malformed"})
        check(std::string("余额查询拒绝非法结构：") + route, [&] {
            const auto view = send(route, "/" + std::string(route), ServiceAction::Balance);
            require(view.status == Status::Failed && !view.service_reply &&
                        !view.error_code.empty() && !view.recovery.empty(),
                    "无效余额结构被接受或缺少可读错误");
        });
    const std::vector<std::pair<std::string, ServiceAction>> actions{
        {"models", ServiceAction::Models},
        {"connection", ServiceAction::TestConnection},
        {"balance", ServiceAction::Balance}};
    for (const auto &[prefix, action] : actions) {
        for (const int status : {401, 403, 429, 500})
            check(prefix + " 服务 HTTP " + std::to_string(status) + " 错误不泄密", [&] {
                const auto route = prefix + "-status-" + std::to_string(status);
                const auto view = send(route, "/" + route, action);
                require(view.status == Status::Failed && view.http_status == status &&
                            !view.service_reply && !view.error_code.empty() &&
                            !view.recovery.empty(),
                        "服务查询 HTTP 错误类型或状态不完整");
            });
        check(prefix + " 服务取消不应用结果", [&] {
            const auto route = prefix + "-cancel";
            const auto view = send(route, "/" + route, action, 200);
            require(view.status == Status::Cancelled && !view.service_reply &&
                        view.elapsed_seconds < 6,
                    "服务取消未及时结束或保留了已取消结果");
        });
    }
    check("模型列表请求全程超时终止", [&] {
        const auto view = send("models-timeout", "/models-timeout", ServiceAction::Models);
        require(view.status == Status::Failed && !view.service_reply &&
                    view.error_code == "timeout" && view.elapsed_seconds >= 9 &&
                    view.elapsed_seconds < 16,
                "服务超时没有按配置生效");
    });
    check("服务查询不跟随重定向", [&] {
        const auto view = send("models-redirect", "/models-redirect", ServiceAction::Models);
        require(view.status == Status::Failed && view.http_status == 302 && !view.service_reply,
                "服务查询跟随重定向或接受了重定向响应");
    });
    check("服务成功后取消清除未消费结果且可再次请求", [&] {
        AsyncClient client;
        const auto settings = settingsFor("/models-reuse", ServiceAction::Models);
        client.start_service(settings, ServiceAction::Models);
        const auto first = await_request(client);
        require(first.status == Status::Success && first.service_reply, "首次服务查询未成功");
        client.cancel();
        require(client.view().status == Status::Cancelled && !client.view().service_reply &&
                    !client.take_service() && !client.take(),
                "取消后仍可消费服务结果");
        client.start_service(settings, ServiceAction::Models);
        const auto second = await_request(client);
        const auto reply = client.take_service();
        require(second.status == Status::Success && second.id > first.id && reply &&
                    reply->models == expectedModels,
                "取消后服务客户端不可复用");
    });
    msa::atomic_text(output / "service-cases.json", exchanges.dump(2) + '\n');
}

void transport_checks(const std::string &base, const std::filesystem::path &output) {
    require(std::regex_match(base, std::regex(R"(^http://127\.0\.0\.1:[0-9]+$)")),
            "mock 测试仅允许明确的本机回环地址");
    Json exchanges = Json::array();
    const auto send = [&](const std::string &route, bool generation = true, int cancelMs = -1) {
        auto settings = defaults();
        settings.provider = "openai_compatible";
        settings.base_url = base + "/" + route;
        settings.model = "local-mock-model";
        settings.api_key = fakeKey;
        settings.timeout_seconds = 10;
        AsyncClient client;
        std::atomic<unsigned> notices{0};
        client.start(settings, {{"user", generation ? "生成一组模拟数据" : "解读当前结果"}},
                     generation, context(), [&] { ++notices; });
        const auto view = await_request(client, cancelMs);
        require(!view.service_reply && !client.take_service(),
                "聊天请求混入服务查询结果或错误消费了聊天回复");
        const auto reply = client.take();
        require(view.error.find(fakeKey) == std::string::npos, "错误消息泄漏密钥");
        require(!client.take(), "同一响应被重复消费");
        client.stop();
        require(notices.load() >= 1, "请求完成未通知调用方");
        exchanges.push_back({{"case", route},
                             {"status", static_cast<int>(view.status)},
                             {"http_status", view.http_status},
                             {"error_code", view.error_code},
                             {"elapsed_seconds", view.elapsed_seconds},
                             {"has_reply", bool(reply)}});
        return view;
    };
    check("HTTP 成功生成和严格协议", [&] {
        const auto view = send("success-generate");
        require(view.status == Status::Success && view.reply && view.reply->action == "generate",
                "成功生成响应未被接受");
    });
    check("HTTP 成功解读和评价对象", [&] {
        const auto view = send("success-interpret", false);
        require(view.status == Status::Success && view.reply && view.reply->action == "evaluate",
                "成功解读响应未被接受");
    });
    for (const int code : {401, 402, 429, 500})
        check("HTTP " + std::to_string(code) + " 错误分类与恢复建议", [&] {
            const auto view = send("status-" + std::to_string(code));
            require(view.status == Status::Failed && view.http_status == code &&
                        !view.error_code.empty() && !view.recovery.empty() && !view.reply,
                    "HTTP 错误缺少类型或恢复建议");
        });
    check("不支持 JSON 模式时明确失败且不自动降级", [&] {
        const auto view = send("unsupported-json");
        require(view.status == Status::Failed && view.http_status == 400 && !view.recovery.empty(),
                "接口不支持 JSON 时未明确失败");
    });
    check("拒绝重定向且不转发凭据", [&] {
        const auto view = send("redirect");
        require(view.status == Status::Failed && view.http_status == 302, "未阻止重定向");
    });
    for (const auto *route :
         {"truncated", "empty-choices", "malformed-envelope", "blank-content", "missing-content",
          "finish-length", "fenced-content", "wrong-task", "oversized"})
        check(std::string("拒绝异常响应：") + route, [&] {
            const auto view = send(route);
            require(view.status == Status::Failed && !view.reply && !view.error.empty() &&
                        !view.error_code.empty(),
                    "异常响应被误认为成功");
        });
    check("全程超时可终止阻塞请求", [&] {
        const auto view = send("timeout");
        require(view.status == Status::Failed && !view.reply && view.elapsed_seconds >= 9 &&
                    view.elapsed_seconds < 16,
                "请求超时没有按配置生效");
    });
    check("取消可终止阻塞请求且不会应用回复", [&] {
        const auto view = send("cancel", true, 200);
        require(view.status == Status::Cancelled && !view.reply && view.elapsed_seconds < 6,
                "取消后仍返回结果或响应过慢");
    });
    check("成功但未消费时取消会丢弃回复并可重新请求", [&] {
        auto settings = defaults();
        settings.provider = "openai_compatible";
        settings.base_url = base + "/reuse";
        settings.model = "local-mock-model";
        settings.api_key = fakeKey;
        settings.timeout_seconds = 10;
        AsyncClient client;
        client.start(settings, {{"user", "生成"}}, true, context());
        const auto first = await_request(client);
        require(first.status == Status::Success, "第一次请求未成功");
        client.cancel();
        require(client.view().status == Status::Cancelled && !client.take(), "未丢弃已取消回复");
        client.start(settings, {{"user", "重新生成"}}, true, context());
        const auto second = await_request(client);
        require(second.status == Status::Success && second.id > first.id && bool(client.take()),
                "取消后不能重新请求");
    });
    msa::atomic_text(output / "transport-cases.json", exchanges.dump(2) + '\n');
    service_checks(base, output);
}

int request_once(const std::filesystem::path &settingsPath, const std::filesystem::path &casePath,
                 const std::filesystem::path &output) {
    std::filesystem::create_directories(output);
    auto settings = load_settings(settingsPath);
    const auto input = Json::parse(msa::read_text(casePath));
    if (input.contains("model"))
        settings.model = input.at("model").get<std::string>();
    const bool generation = input.at("generation").get<bool>();
    const auto data = input.at("context");
    std::vector<Message> messages;
    for (const auto &value : input.at("messages"))
        messages.push_back(
            {value.at("role").get<std::string>(), value.at("content").get<std::string>()});
    Json report{{"passed", false}, {"generation", generation}};
    int code = 1;
    try {
        AsyncClient client;
        client.start(settings, messages, generation, data);
        const auto view = await_request(client, -1, settings.timeout_seconds + 10);
        report["elapsed_seconds"] = view.elapsed_seconds;
        report["http_status"] = view.http_status;
        report["error_code"] = view.error_code;
        report["error"] = redact(view.error, settings.api_key);
        report["recovery"] = redact(view.recovery, settings.api_key);
        if (auto reply = client.take()) {
            report["reply"] = {{"action", reply->action},
                               {"text", reply->text},
                               {"structured", reply->structured}};
            report["passed"] = true;
            code = 0;
            if (generation && reply->action == "generate" && reply->config_patch &&
                input.value("local_generate", false)) {
                try {
                    const auto config =
                        apply_config_patch(data.at("current_config"), *reply->config_patch);
                    const auto result = msa::generate(config);
                    msa::atomic_text(output / "local-result.json", result.as_json().dump(2) + '\n');
                    report["local_generation"] = {
                        {"passed", true},
                        {"qualified", msa::qualified(result.metrics, result.config)}};
                } catch (const std::exception &e) {
                    report["local_generation"] = {{"passed", false},
                                                  {"error", redact(e.what(), settings.api_key)}};
                    report["passed"] = false;
                    code = 2;
                }
            }
        }
    } catch (const std::exception &e) {
        report["error"] = redact(e.what(), settings.api_key);
    }
    // 即便服务将密钥反射到文本，也不把该字符串写入探测报告。
    redact_values(report, settings.api_key);
    msa::atomic_text(output / "request-report.json", report.dump(2) + '\n');
    std::cout << "AI 单次探测已结束；安全报告已保存。\n";
    return code;
}

int service_live(const std::filesystem::path &settingsPath, const std::filesystem::path &output) {
    std::filesystem::create_directories(output);
    Json report{{"passed", false}, {"real_network", true}, {"checks", Json::array()}};
    std::string key;
    int result = 1;
    try {
        const auto settings = load_settings(settingsPath);
        key = settings.api_key;
        // 此入口只由明确命令调用，不加入默认回归；白名单失败时禁止发送任何请求。
        const std::regex official(
            R"(^https://api\.deepseek\.com(?::443)?(/v1)?(/chat/completions)?/*$)");
        require(settings.provider == "deepseek" && std::regex_match(settings.base_url, official) &&
                    supports_balance(settings) && !settings.api_key.empty(),
                "只允许已有密钥的官方 DeepSeek 配置");
        report["selected_model"] = settings.model;
        const std::vector<std::pair<std::string, ServiceAction>> actions{
            {"models", ServiceAction::Models},
            {"connection", ServiceAction::TestConnection},
            {"balance", ServiceAction::Balance}};
        bool passed = true;
        for (std::size_t i = 0; i < actions.size(); ++i) {
            if (i) {
                // 每项真实服务请求之间显式等待，避免短时间内连续发送。
                std::this_thread::sleep_for(std::chrono::seconds(10));
            }
            AsyncClient client;
            client.start_service(settings, actions[i].second);
            const auto view = await_request(client, -1, settings.timeout_seconds + 10);
            const auto reply = client.take_service();
            Json item{{"name", actions[i].first},
                      {"passed", view.status == Status::Success && bool(reply)},
                      {"error_code", view.error_code},
                      {"http_status", view.http_status}};
            if (!item["passed"].get<bool>()) {
                report["checks"].push_back(item);
                passed = false;
                break;
            }
            if (actions[i].second == ServiceAction::Models) {
                report["models"] = reply->models;
                if (std::find(reply->models.begin(), reply->models.end(), settings.model) ==
                    reply->models.end()) {
                    item["passed"] = false;
                    item["error_code"] = "configured_model_not_listed";
                    report["checks"].push_back(item);
                    passed = false;
                    break;
                }
            } else if (actions[i].second == ServiceAction::Balance) {
                const bool valid = reply->balance && reply->balance->is_object() &&
                                   reply->balance->contains("is_available") &&
                                   reply->balance->at("is_available").is_boolean() &&
                                   reply->balance->contains("balance_infos") &&
                                   reply->balance->at("balance_infos").is_array();
                report["balance_format_valid"] = valid;
                if (valid)
                    report["balance_available"] = reply->balance->at("is_available");
                else {
                    item["passed"] = false;
                    item["error_code"] = "balance_response_invalid";
                    passed = false;
                }
            }
            report["checks"].push_back(item);
        }
        report["passed"] = passed && report["checks"].size() == 3;
        if (report["passed"].get<bool>())
            result = 0;
    } catch (const Error &error) {
        report["error_code"] = error.code;
        report["http_status"] = error.http_status;
    } catch (const std::exception &) {
        report["error_code"] = "invalid_settings_or_local_error";
    }
    // 报告不包含账户金额、接口配置正文、密钥、请求正文或服务端原始错误内容。
    redact_values(report, key);
    msa::atomic_text(output / "ai-service-live-report.json", report.dump(2) + '\n');
    std::cout << "AI 官方服务显式探测已结束；安全报告已保存。\n";
    return result;
}
} // namespace

int wmain(int argc, wchar_t **argv) {
    try {
        if (argc == 5 && std::wstring(argv[1]) == L"--request")
            return request_once(argv[2], argv[3], argv[4]);
        if (argc == 4 && std::wstring(argv[1]) == L"--service-live")
            return service_live(argv[2], argv[3]);
        const bool transport = argc == 4 && std::wstring(argv[1]) == L"--transport";
        require(transport || argc <= 2, "参数格式错误");
        require(transport || argc < 2 || std::wstring(argv[1]).find(L"--") != 0,
                "测试参数不支持或缺少所需路径");
        const auto output = std::filesystem::path(transport  ? argv[3]
                                                  : argc > 1 ? argv[1]
                                                             : L"ai-test-output");
        std::filesystem::create_directories(output);
        if (transport) {
            const std::wstring address(argv[2]);
            require(std::all_of(address.begin(), address.end(), [](wchar_t c) { return c < 128; }),
                    "mock 地址必须为 ASCII 回环地址");
            std::string asciiAddress;
            asciiAddress.reserve(address.size());
            for (const wchar_t character : address)
                asciiAddress.push_back(static_cast<char>(character));
            transport_checks(asciiAddress, output);
        } else
            protocol_checks(output);
        const auto failures = std::count_if(checks.begin(), checks.end(), [](const Json &v) {
            return !v.at("passed").get<bool>();
        });
        const Json report{{"passed", failures == 0}, {"version", msa::version},
                          {"checks", checks},        {"failed", failures},
                          {"count", checks.size()},  {"real_network", false}};
        msa::atomic_text(output /
                             (transport ? "ai-transport-regression.json" : "ai-regression.json"),
                         report.dump(2) + '\n');
        std::cout << "AI " << (transport ? "本地传输" : "协议与配置") << "检查：" << checks.size()
                  << " 项，失败 " << failures << " 项。\n";
        for (const auto &item : checks)
            if (!item.at("passed").get<bool>())
                std::cerr << item.at("name").get<std::string>() << "："
                          << item.at("error").get<std::string>() << '\n';
        return failures ? 1 : 0;
    } catch (const std::exception &e) {
        std::cerr << "AI 测试入口失败：" << redact(e.what()) << '\n';
        return 1;
    }
}
