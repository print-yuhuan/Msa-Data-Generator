"""只监听回环地址的 AI 传输测试服务器，不记录请求正文或凭据。"""

import argparse
import json
import os
from pathlib import Path
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def generation_reply():
    return {
        "schema_version": 1,
        "task": "generate",
        "action": "generate",
        "message": "使用本机测试数据验证结构化生成响应。",
        "questions": [],
        "warnings": [],
        "config_patch": {"generation": {"seed": 456}},
    }


def interpretation_reply():
    return {
        "schema_version": 1,
        "task": "interpret",
        "action": "evaluate",
        "message": "使用本机指标验证评价响应。",
        "questions": [],
        "warnings": ["模拟数据不代替真实测量研究。"],
        "evaluation": {
            "conclusion": "应结合使用场景判断测量系统适用性。",
            "findings": [{"metric": "ndc", "assessment": "核对本机计算的区分类别数。"}],
            "recommendations": ["检查样本是否覆盖实际过程变异。"],
        },
    }


def envelope(reply, finish="stop"):
    return {
        "choices": [
            {
                "finish_reason": finish,
                "message": {"role": "assistant", "content": json.dumps(reply, ensure_ascii=False)},
            }
        ]
    }


def write_json(path, value):
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding="utf-8")
    os.replace(temporary, path)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--ready", type=Path, required=True)
    parser.add_argument("--stats", type=Path, required=True)
    args = parser.parse_args()
    args.ready.parent.mkdir(parents=True, exist_ok=True)
    args.stats.parent.mkdir(parents=True, exist_ok=True)
    lock = threading.Lock()
    stats = {"counts": {}, "invalid_requests": 0, "non_json_requests": 0, "non_loopback": 0,
             "service_methods": {}, "service_paths": {}, "unexpected_get_bodies": 0,
             "service_prompt_leaks": 0}

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *_args):
            pass

        def send_bytes(self, status, body, extra_headers=None, length=None):
            try:
                self.send_response(status)
                self.send_header("Content-Type", "application/json; charset=utf-8")
                self.send_header("Content-Length", str(len(body) if length is None else length))
                self.send_header("Connection", "close")
                for key, value in (extra_headers or {}).items():
                    self.send_header(key, value)
                self.end_headers()
                self.wfile.write(body)
                self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError, OSError):
                pass
            finally:
                self.close_connection = True

        def send_json(self, status, value, headers=None):
            self.send_bytes(status, json.dumps(value, ensure_ascii=False).encode("utf-8"), headers)

        def record(self, route, valid, json_mode=True, service=False):
            with lock:
                stats["counts"][route] = stats["counts"].get(route, 0) + 1
                stats["invalid_requests"] += int(not valid)
                stats["non_json_requests"] += int(not json_mode)
                stats["non_loopback"] += int(self.client_address[0] != "127.0.0.1")
                if service:
                    stats["service_methods"][route] = self.command
                    stats["service_paths"][route] = self.path
                write_json(args.stats, stats)

        def service_common(self, route, response):
            if "-status-" in route:
                status = int(route.rsplit("-", 1)[1])
                self.send_json(status, {"error": {"message": "PRIVATE_SERVER_ERROR_SENTINEL",
                                                  "code": "mock_error"}})
            elif route.endswith("-redirect"):
                target = f"http://127.0.0.1:{self.server.server_port}/service-redirect-target/models"
                self.send_json(302, {}, {"Location": target})
            elif route.endswith("-malformed"):
                self.send_bytes(200, b'{"data":')
            elif route.endswith("-truncated"):
                self.send_bytes(200, b'{"data":', length=200)
            elif route.endswith("-oversized"):
                self.send_json(200, {"padding": "a" * (2 * 1024 * 1024)})
            elif route.endswith("-timeout") or route.endswith("-cancel"):
                time.sleep(12)
                self.send_json(200, response)
            else:
                return False
            return True

        def do_GET(self):
            if self.path == "/models":
                route = "models-root"
            elif self.path == "/v1/models":
                route = "models-v1"
            else:
                route = self.path.strip("/").split("/")[0]
            body_size = int(self.headers.get("Content-Length", "0"))
            valid = (self.client_address[0] == "127.0.0.1" and body_size == 0 and
                     self.headers.get("Authorization", "").startswith("Bearer "))
            models = self.path.endswith("/models")
            balance = self.path.endswith("/user/balance")
            valid = valid and (models or balance)
            with lock:
                stats["unexpected_get_bodies"] += int(body_size != 0)
            self.record(route, valid, service=True)
            if not valid:
                self.send_json(422, {"error": {"message": "服务查询请求不符合规范。"}})
                return
            if models:
                response = {"object": "list", "data": [
                    {"id": "local-mock-model", "object": "model", "owned_by": "local"},
                    {"id": "second-mock-model", "object": "model", "owned_by": "local"}]}
                if route == "models-duplicate":
                    response["data"].append(response["data"][0])
                elif route == "models-empty":
                    response["data"] = []
                elif route == "models-missing-data":
                    response = {"object": "list"}
                elif route == "models-not-array":
                    response["data"] = {"id": "model"}
                elif route == "models-numeric-id":
                    response["data"][1]["id"] = 123
                elif route == "models-blank-id":
                    response["data"][1]["id"] = "  "
                elif route == "models-control-id":
                    response["data"][1]["id"] = "model\ninvalid"
                elif route == "models-long-id":
                    response["data"][1]["id"] = "a" * 201
                elif route == "models-missing-id":
                    del response["data"][1]["id"]
                elif route == "models-non-object":
                    response["data"][1] = "model"
                elif route == "models-too-many":
                    response["data"] = [{"id": f"model-{i}"} for i in range(1025)]
            else:
                response = {"is_available": True, "balance_infos": [
                    {"currency": "CNY", "total_balance": "12.34", "granted_balance": "2.34",
                     "topped_up_balance": "10.00"}]}
                if route == "balance-unavailable":
                    response["is_available"] = False
                    response["balance_infos"][0].update(
                        total_balance="0.00", granted_balance="0.00", topped_up_balance="0.00")
                elif route == "balance-non-bool":
                    response["is_available"] = "true"
                elif route == "balance-missing-infos":
                    del response["balance_infos"]
                elif route == "balance-non-array":
                    response["balance_infos"] = {}
                elif route == "balance-bad-amount":
                    response["balance_infos"][0]["total_balance"] = "not-a-number"
                elif route == "balance-missing-currency":
                    del response["balance_infos"][0]["currency"]
            if not self.service_common(route, response):
                self.send_json(200, response)

        def do_POST(self):
            route = self.path.strip("/").split("/")[0]
            service = route.startswith("connection-")
            valid = self.client_address[0] == "127.0.0.1"
            json_mode = False
            try:
                size = int(self.headers.get("Content-Length", "0"))
                if not 0 < size <= 256 * 1024:
                    raise ValueError("请求长度超出测试范围")
                request = json.loads(self.rfile.read(size))
                json_mode = request.get("response_format", {}).get("type") == "json_object"
                valid = valid and self.path.endswith("/chat/completions")
                valid = valid and request.get("stream") is False and json_mode
                messages = request.get("messages", [])
                if service:
                    leak = any(marker in json.dumps(request) for marker in
                               ("PRIVATE_GENERATION_PROMPT_SENTINEL", "PRIVATE_INTERPRETATION_PROMPT_SENTINEL",
                                "current_config", "generation_readiness", "config_patch"))
                    with lock:
                        stats["service_prompt_leaks"] += int(leak)
                    valid = valid and not leak and size <= 4096 and 1 <= len(messages) <= 2
                    valid = valid and request.get("model") == "local-mock-model"
                    valid = valid and isinstance(request.get("max_tokens"), int) and 1 <= request["max_tokens"] <= 256
                    valid = valid and all(m.get("role") in ("system", "user") for m in messages)
                else:
                    valid = valid and len(messages) >= 3 and messages[0].get("role") == "system"
                valid = valid and bool(messages) and messages[-1].get("role") == "user"
                valid = valid and (request.get("thinking") == {"type": "disabled"}
                                   if route == "connection-deepseek" else "thinking" not in request)
                # 只检验凭据是否存在；绝不保存、输出或回显 Authorization 内容。
                valid = valid and self.headers.get("Authorization", "").startswith("Bearer ")
            except (ValueError, TypeError, AttributeError, KeyError):
                valid = False
            self.record(route, valid, json_mode, service)
            if not valid:
                self.send_json(422, {"error": {"message": "本地测试请求未通过规范检查。"}})
                return
            if service:
                response = envelope({"ok": True})
                if self.service_common(route, response):
                    return
                if route == "connection-empty-choices":
                    response["choices"] = []
                elif route == "connection-non-json":
                    response["choices"][0]["message"]["content"] = "连接成功"
                elif route == "connection-fenced":
                    response["choices"][0]["message"]["content"] = '```json\n{"ok":true}\n```'
                elif route == "connection-length":
                    response["choices"][0]["finish_reason"] = "length"
                elif route == "connection-object":
                    response = envelope({"connection": "accepted", "service": "local"})
                elif route == "connection-array":
                    response = envelope([{"ok": True}])
                elif route == "connection-role":
                    response["choices"][0]["message"]["role"] = "user"
                elif route == "connection-multi":
                    response["choices"].append(response["choices"][0])
                elif route == "connection-unsupported-json":
                    self.send_json(400, {"error": {"message": "response_format json_object is not supported"}})
                    return
                self.send_json(200, response)
                return
            if route.startswith("status-"):
                status = int(route[7:])
                self.send_json(status, {"error": {"message": "服务端测试错误原文，不应直接展示。",
                                                   "code": "mock_error"}})
            elif route == "unsupported-json":
                self.send_json(400, {"error": {"code": "invalid_parameter",
                                                "message": "response_format json_object is not supported"}})
            elif route == "redirect":
                # 跳转目标也为回环地址，避免异常客户端导致任何外部联网。
                target = f"http://127.0.0.1:{self.server.server_port}/redirect-target/chat/completions"
                self.send_json(302, {}, {"Location": target})
            elif route == "truncated":
                body = b'{"choices":[{"message":'
                self.send_bytes(200, body, length=len(body) + 200)
            elif route == "empty-choices":
                self.send_json(200, {"choices": []})
            elif route == "malformed-envelope":
                self.send_bytes(200, b'{"choices":')
            elif route == "blank-content":
                self.send_json(200, {"choices": [{"finish_reason": "stop", "message": {"content": " "}}]})
            elif route == "missing-content":
                self.send_json(200, {"choices": [{"finish_reason": "stop", "message": {"role": "assistant"}}]})
            elif route == "finish-length":
                self.send_json(200, envelope(generation_reply(), "length"))
            elif route == "fenced-content":
                response = envelope(generation_reply())
                response["choices"][0]["message"]["content"] = "```json\n" + response["choices"][0]["message"]["content"] + "\n```"
                self.send_json(200, response)
            elif route == "wrong-task":
                self.send_json(200, envelope(interpretation_reply()))
            elif route == "oversized":
                response = envelope(generation_reply())
                response["padding"] = "a" * (2 * 1024 * 1024)
                self.send_json(200, response)
            elif route in ("timeout", "cancel"):
                time.sleep(12)
                self.send_json(200, envelope(generation_reply()))
            elif route == "success-interpret":
                self.send_json(200, envelope(interpretation_reply()))
            elif route == "gui-clarify":
                self.send_json(200, envelope({
                    "schema_version": 1, "task": "generate", "action": "clarify",
                    "message": "请确认需要调整的研究条件。", "questions": ["保留当前规格吗？"],
                    "warnings": [], "config_patch": None,
                }))
            else:
                self.send_json(200, envelope(generation_reply()))

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    write_json(args.stats, stats)
    write_json(args.ready, {"port": server.server_port})
    server.serve_forever(poll_interval=0.1)


if __name__ == "__main__":
    main()
