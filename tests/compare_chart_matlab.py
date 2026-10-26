"""核对 MATLAB 原生 gagerr 表与本程序实际绘图结果；不启动外部软件。"""

import argparse
import csv
import json
import math
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--metrics", type=Path, required=True)
    parser.add_argument("--matlab", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    local = json.loads(args.metrics.read_text(encoding="utf-8-sig"))
    with args.matlab.open(encoding="utf-8-sig", newline="") as stream:
        reference = {row["Row"]: row for row in csv.DictReader(stream)}
    mapping = {"GRR": "GageRR", "EV": "Gage_Repeatability",
               "AV": "Gage_Reproducibility", "PV": "Part"}
    fields = {"variance": "Variance", "contribution_pct": "PercentVariance",
              "sd": "Sigma", "study_variation": "StudyVariation",
              "study_variation_pct": "PercentStudyVariation"}
    results = []
    for key, source in mapping.items():
        component = local["metrics"]["study"]["components"][key]
        for field, column in fields.items():
            actual, expected = float(component[field]), float(reference[source][column])
            error = abs(actual - expected)
            tolerance = 1e-11 + 1e-9 * abs(expected)
            results.append({"component": key, "field": field, "actual": actual,
                            "reference": expected, "absolute_error": error,
                            "passed": math.isfinite(actual) and error <= tolerance})
    report = {"source": "MATLAB 原生 gagerr，Model=interaction，SigmaMultiplier=6",
              "count": len(results), "passed": all(r["passed"] for r in results),
              "max_absolute_error": max(r["absolute_error"] for r in results),
              "checks": results}
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"MATLAB 绘图指标：{report['count']} 项，"
          f"失败 {sum(not r['passed'] for r in results)} 项；"
          f"最大绝对误差 {report['max_absolute_error']:.3g}")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
