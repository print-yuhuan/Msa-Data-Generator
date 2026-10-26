"""显式运行真实 AI 专项探测；凭据仅从应用的 DPAPI 配置读取，不属于默认 CTest。"""
import argparse
import copy
import json
from pathlib import Path
import subprocess
import time


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--settings", type=Path, required=True, help="本机加密 AI 配置文件")
    parser.add_argument("--binary", type=Path, default=Path("build/Release/msa_ai_tests.exe"))
    parser.add_argument("--cli", type=Path, default=Path("build/Release/msa_cli.exe"))
    parser.add_argument("--output", type=Path, default=Path("build/ai-test-output/live"))
    parser.add_argument("--second-model", default="deepseek-v4-pro")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    checks = []
    last_end = None

    def check(name, passed, detail=""):
        checks.append({"name": name, "passed": bool(passed), "detail": detail})
        write_json(args.output / "live-summary.json", {"passed": all(x["passed"] for x in checks),
                   "real_network": True, "checks": checks,
                   "scope": "DeepSeek Flash/Pro；不代表所有参数规模或所有兼容服务已实测"})

    def probe(name, data, expected):
        nonlocal last_end
        # 真实联网统一串行，任意两次完成/开始之间显式等待至少十秒。
        if last_end is not None:
            time.sleep(max(0, 10 - (time.monotonic() - last_end)))
        folder = args.output / name
        write_json(folder / "case.json", data)
        completed = subprocess.run([str(args.binary.resolve()), "--request", str(args.settings.resolve()),
                                    str(folder / "case.json"), str(folder)], capture_output=True)
        last_end = time.monotonic()
        report_path = folder / "request-report.json"
        if not report_path.exists():
            check(name, False, "探测进程未生成安全报告")
            raise RuntimeError("测试入口失败，未输出可能包含敏感内容的原始进程信息")
        report = read_json(report_path)
        action = report.get("reply", {}).get("action")
        passed = completed.returncode == 0 and report.get("passed") and action in expected
        check(name, passed, f"action={action}; http_status={report.get('http_status')}; error_code={report.get('error_code')}")
        print(f"{name}: {'PASS' if passed else 'FAIL'}", flush=True)
        if not passed:
            raise RuntimeError("真实探测未通过，已停止后续联网；请查看对应安全报告")
        return report, folder

    base = read_json(root / "examples/示例配置.json")
    base.pop("output", None)
    base["specification"].update(lsl=None, usl=None)
    base["targets"] = {name: None for name in base["targets"]}
    history = [{"role": "user", "content": "请按当前研究设计帮我生成数据，用于学习量具 R&R。"}]
    first, _ = probe("01_missing_conditions", {"generation": True, "messages": history,
                     "context": {"current_config": base}}, {"clarify"})
    history += [{"role": "assistant", "content": json.dumps(first["reply"]["structured"], ensure_ascii=False)},
                {"role": "user", "content": "规格下限9、上限11，3名操作员、10件部件，每件重复3次，采用均值极差法，保留4位小数，种子123。GRR/TV不高于20%，P/T不高于20%，ndc不低于5，其它指标不约束。请生成。"}]
    second, folder = probe("02_generate_xbarr", {"generation": True, "messages": history,
                           "context": {"current_config": base}, "local_generate": True}, {"generate"})
    result = read_json(folder / "local-result.json")
    check("Xbar-R 本机生成与复核", second["local_generation"]["qualified"])
    cfg = result["config"]
    check("自然语言研究参数与约束准确映射",
          cfg["design"] == {"operators": 3, "parts": 10, "replicates": 3}
          and cfg["specification"] == {"lsl": 9, "usl": 11, "compliance": "strict"}
          and cfg["analysis"]["method"] == "crossed_xbarr"
          and cfg["generation"]["seed"] == 123 and cfg["generation"]["decimal_places"] == 4
          and cfg["targets"] == {"EV_pct": None, "AV_pct": None,
               "GRR_pct": {"mode": "at_most", "value": 20},
               "PT_pct": {"mode": "at_most", "value": 20},
               "ndc": {"mode": "at_least", "value": 5}}
          and sum(map(len, result["matrix"])) == 90)
    history += [{"role": "assistant", "content": json.dumps(second["reply"]["structured"], ensure_ascii=False)},
                {"role": "user", "content": "改用ANOVA方差分析法，交互项自动判断。其它研究参数、规格、精度、种子及指标约束全部保持不变，请重新生成。"}]
    third, folder = probe("03_followup_anova", {"generation": True, "messages": history,
                          "context": {"current_config": result["config"]}, "local_generate": True}, {"generate"})
    anova = read_json(folder / "local-result.json")
    check("ANOVA 本机生成与复核", third["local_generation"]["qualified"])
    before = copy.deepcopy(result["config"])
    after = copy.deepcopy(anova["config"])
    before["analysis"]["method"] = after["analysis"]["method"]
    before["analysis"]["interaction"] = after["analysis"]["interaction"]
    check("多轮修改保留其它业务参数", before == after)

    impossible = copy.deepcopy(anova["config"])
    impossible["targets"]["EV_pct"] = {"mode": "at_least", "value": 80}
    impossible["targets"]["GRR_pct"] = {"mode": "at_most", "value": 10}
    write_json(args.output / "infeasible.json", impossible)
    failure = subprocess.run([str(args.cli.resolve()), "--config", str(args.output / "infeasible.json")],
                             capture_output=True)
    check("冲突约束由本机拒绝", failure.returncode != 0)
    diagnostic = {"current_config": impossible,
                  "last_generation": {"status": "failed", "failure_kind": "constraint_conflict",
                                      "reason": "EV%、AV%、GRR%、P/T 或 ndc 的允许范围没有共同解；请调整目标或允许误差。"}}
    probe("04_failure_diagnosis", {"generation": True,
          "messages": [{"role": "user", "content": "请解释本机失败原因，提出修改建议，但先不要执行，不要擅自放宽我要求的指标。"}],
          "context": diagnostic}, {"diagnose", "clarify"})
    probe("05_unsupported_design", {"generation": True,
          "messages": [{"role": "user", "content": "我需要4名操作员、10件部件、每件3次的研究，不能改成人员数3。请判断能否生成。"}],
          "context": {"current_config": result["config"]}}, {"clarify", "diagnose"})
    evaluation_context = copy.deepcopy(anova)
    evaluation_context["config"].pop("output", None)
    question = [{"role": "user", "content": "用于学习验证统计方法。请依据本机指标评价重复性、再现性、GRR、P/T、ndc，解释ANOVA口径和模拟数据限制，给出改善建议。"}]
    flash, _ = probe("06_flash_evaluation", {"generation": False, "messages": question,
                    "context": evaluation_context}, {"evaluate"})
    probe("07_pro_evaluation", {"generation": False, "model": args.second_model,
          "messages": question, "context": evaluation_context}, {"evaluate"})
    followup = question + [{"role": "assistant", "content": json.dumps(flash["reply"]["structured"], ensure_ascii=False)},
                           {"role": "user", "content": "继续解释为什么GRR/TV与P/T不相等，只解释这两个指标的口径，不修改数据也不重新生成。"}]
    probe("08_evaluation_followup", {"generation": False, "messages": followup,
          "context": evaluation_context}, {"evaluate"})
    print("真实 AI 专项探测完成；仅安全业务报告落盘。", flush=True)
    return 0 if all(x["passed"] for x in checks) else 1


if __name__ == "__main__":
    raise SystemExit(main())
