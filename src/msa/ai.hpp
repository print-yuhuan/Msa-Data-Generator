#pragma once
#include "core.hpp"
#include <functional>
#include <memory>

namespace msa::ai {
struct Settings {
    std::string provider = "deepseek";
    std::string base_url = "https://api.deepseek.com";
    std::string model = "deepseek-flash";
    std::string api_key;
    std::string generation_system_prompt;
    std::string interpretation_system_prompt;
    int timeout_seconds = 120;
};

Settings defaults();
std::filesystem::path settings_path();
Settings load_settings(const std::filesystem::path &path = settings_path());
void save_settings(const Settings &settings, const std::filesystem::path &path = settings_path());
// 保存配置时允许密钥留空；远程请求开始前才要求填写密钥。
void validate_settings(const Settings &settings);
// 仅接受业务参数；输出目录、文件名、接口设置和密钥不属于 AI 可修改范围。
Json apply_config_patch(const Json &base, const Json &patch);

struct Message {
    std::string role;
    std::string content;
};
struct Reply {
    std::string text;
    std::optional<Json> config_patch;
    std::string action;
    Json structured;
};
// 错误只包含应用生成的分类与恢复建议，不包含服务端原文或请求凭据。
struct Error : std::runtime_error {
    std::string code, recovery;
    int http_status = 0;
    bool retryable = false;
    Error(std::string code, std::string message, std::string recovery, int http_status = 0,
          bool retryable = false);
};
// 纯函数：严格验证结构化正文，不剥离代码围栏、不截取 JSON、不自动纠正回复。
Reply parse_reply(const std::string &content, bool generation, const Json &context);
Reply decode_response(const std::string &body, bool generation, const Json &context);
Json build_request(const Settings &settings, const std::vector<Message> &messages, bool generation,
                   const Json &context);
enum class ServiceAction { Models, TestConnection, Balance };
struct ServiceReply {
    std::vector<std::string> models;
    std::optional<Json> balance;
};
// 仅允许 DeepSeek 官方 HTTPS 地址或本机回环测试地址查询余额。
bool supports_balance(const Settings &settings) noexcept;
enum class Status { Idle, Running, Cancelling, Success, Cancelled, Failed };
struct View {
    std::uint64_t id = 0;
    Status status = Status::Idle;
    std::string error;
    std::string error_code, recovery;
    int http_status = 0;
    bool retryable = false;
    std::optional<Reply> reply;
    std::optional<ServiceReply> service_reply;
    double elapsed_seconds = 0;
};

// start 先完成有界请求构建与前置校验；HTTP 与响应解析在工作线程完成，通知只用于唤醒界面。
class AsyncClient {
  public:
    AsyncClient();
    ~AsyncClient();
    AsyncClient(const AsyncClient &) = delete;
    AsyncClient &operator=(const AsyncClient &) = delete;
    void start(const Settings &settings, const std::vector<Message> &messages, bool generation,
               const Json &context, std::function<void()> notify = {});
    void start_service(const Settings &settings, ServiceAction action,
                       std::function<void()> notify = {});
    void cancel();
    void stop();
    View view() const;
    std::optional<Reply> take();
    std::optional<ServiceReply> take_service();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    void start_work(const Settings &settings, std::function<void(View &)> work,
                    std::function<void()> notify);
};
} // namespace msa::ai
