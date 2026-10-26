"""通过回环模拟服务验证真实 GUI 发送路径，不使用个人配置或真实凭据。"""

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    parser.add_argument("--font", required=True)
    parser.add_argument("--config", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    run = Path(tempfile.mkdtemp(prefix="session-", dir=args.output))
    ready, stats = run / "ready.json", run / "stats.json"
    flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    env = dict(os.environ, PYTHONIOENCODING="utf-8")
    with (run / "server.log").open("wb") as log:
        server = subprocess.Popen(
            [sys.executable, "-u", str(Path(__file__).with_name("mock_ai_server.py")),
             "--ready", str(ready), "--stats", str(stats)],
            stdout=log, stderr=subprocess.STDOUT, env=env, creationflags=flags,
        )
        try:
            deadline = time.monotonic() + 10
            while not ready.exists():
                if server.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError("本机会话模拟服务未正常启动")
                time.sleep(0.05)
            port = json.loads(ready.read_text(encoding="utf-8"))["port"]
            result = subprocess.run(
                [args.binary, args.font, args.config, "--ai-sessions",
                 f"http://127.0.0.1:{port}"],
                capture_output=True, timeout=60, creationflags=flags,
            )
            (run / "gui.stdout.txt").write_bytes(result.stdout)
            (run / "gui.stderr.txt").write_bytes(result.stderr)
            print(result.stdout.decode("utf-8", errors="replace"))
            if result.returncode:
                raise RuntimeError(result.stderr.decode("utf-8", errors="replace"))
            observed = json.loads(stats.read_text(encoding="utf-8"))
            expected = {"success-interpret": 1, "status-500": 1, "gui-clarify": 1}
            if observed["counts"] != expected or any(
                observed[k] for k in ("invalid_requests", "non_json_requests", "non_loopback")
            ):
                raise RuntimeError("会话请求数量、协议或回环限制不符合预期")
            (run / "result.json").write_text(
                json.dumps({"passed": True, "requests": expected}, indent=2), encoding="utf-8"
            )
        finally:
            server.terminate()
            try:
                server.wait(timeout=5)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait(timeout=5)


if __name__ == "__main__":
    main()
