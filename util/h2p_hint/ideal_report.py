"""Derive Chinese capacity-control reports from independently audited runs."""

import csv
import json

from .audit import legacy
from .ideal_runner import QUALIFICATION, image_path, read_status, run_path
from .model import address, contract, sha256, write_json
from .runner import now, tag


def final_report(args, tasks, complete):
    if (
        complete["result"] != "pass"
        or complete["full_runs"] != 185
        or complete["window_count"] != 9250
    ):
        raise ValueError(
            "report requires all 185 runs and 9250 windows to independently pass"
        )
    summaries, per_pc = [], []
    for task in tasks:
        statuses = {
            stage: read_status(args, task, stage)
            for stage in ("baseline", "finite", "fixed", "joint")
        }
        metadata_path = image_path(args, task, "joint") / "metadata.txt"
        metadata = json.loads(metadata_path.read_text())
        improved = (
            statuses["joint"]["final_misses"]
            < statuses["baseline"]["final_misses"]
        )
        recommended = (
            metadata
            if improved
            else {
                "format": "h2p-hint",
                **contract(ideal=True),
                "cam": [],
                "entries": [],
                "slots": [],
                "source": {
                    "reason": "actual joint final target misses did not improve off",
                    "evaluated_image_sha256": sha256(metadata_path),
                },
            }
        )
        write_json(
            metadata_path.with_name("recommended-metadata.txt"), recommended
        )
        row = {
            "task": tag(task),
            "recommended": "joint" if improved else "disabled",
            "metadata_sha256": sha256(metadata_path),
            "stages": {},
        }
        for stage, status in statuses.items():
            entry = {
                "final_misses": status["final_misses"],
                "ipc": status["ipc"],
                "sim_ticks": status["sim_ticks"],
                "phases": status["phases"],
            }
            if stage != "baseline":
                hint = status["hint"]
                entry.update(
                    global_counters=hint["global"],
                    query=hint["query"],
                    queue=hint["queue"],
                    occupancy=hint["occupancy"],
                )
            row["stages"][stage] = entry
        row["comparisons"] = []
        for left, right in (
            ("baseline", "finite"),
            ("finite", "fixed"),
            ("fixed", "joint"),
        ):
            before, after = statuses[left], statuses[right]
            row["comparisons"].append(
                {
                    "from": left,
                    "to": right,
                    "miss_reduction": before["final_misses"]
                    - after["final_misses"],
                    "ipc_change": after["ipc"] - before["ipc"],
                    "ipc_change_percent": (after["ipc"] / before["ipc"] - 1)
                    * 100,
                }
            )
        summaries.append(row)
        best = json.loads(
            (
                args.output_root / "inputs" / tag(task) / "best-masks.json"
            ).read_text()
        )
        loaded = {
            address(entry["pc"]): entry["mask"] for entry in metadata["cam"]
        }
        joint_rows = {
            address(entry["pc"]): entry
            for entry in statuses["joint"]["hint"]["pcs"]
        }
        for choice in best:
            pc = address(choice["pc"])
            actual = joint_rows.get(pc, {})
            per_pc.append(
                {
                    "task": tag(task),
                    **choice,
                    "final_loaded": pc in loaded,
                    "actual_joint": actual,
                    "actual_joint_net_correction": actual.get("corrected", 0)
                    - actual.get("harmed", 0),
                }
            )
        # Residual classifications are observations. They do not infer inherent
        # unpredictability from a capacity or query-coverage failure.
        hard = image_path(args, task, "joint") / "hard-pred.txt"
        retained = [
            line
            for line in hard.read_text().splitlines()
            if line.strip()
            and json.loads(line).get("reason") != "actual_joint_residual"
        ]
        with hard.open("w") as output:
            for line in retained:
                output.write(line + "\n")
            for pc, counts in sorted(joint_rows.items()):
                if not counts["final_misses"]:
                    continue
                output.write(
                    json.dumps(
                        {
                            "pc": hex(pc),
                            "reason": "actual_joint_residual",
                            "final_misses": counts["final_misses"],
                            "loaded": pc in loaded,
                            "not_eligible_instances": counts["executions"]
                            - counts["eligible"],
                            "eligible_no_tag_instances": counts["eligible"]
                            - counts["tag_hits"],
                            "tag_hit_conf_disabled_instances": counts[
                                "tag_hits"
                            ]
                            - counts["conf_enabled"],
                            "harmful_overrides": counts["harmed"],
                            "qualification": "coverage counts are all executions,"
                                "not disjoint residual miss attribution",
                        },
                        sort_keys=True,
                    )
                    + "\n"
                )
    write_json(args.output_root / "best-masks.json", per_pc)
    summary = {
        "format": "h2p-hint-ideal-experiment-summary",
        "version": 3,
        "qualification": QUALIFICATION,
        "full_runs": 185,
        "window_count": 9250,
        "complete_audit_sha256": sha256(
            args.output_root / "complete-audit.json"
        ),
        "finished_utc": now(),
        "results": summaries,
    }
    write_json(args.output_root / "summary.json", summary)
    sweep_fields = [
        "task",
        "mask",
        "pc",
        "executions",
        "baseline_misses",
        "final_misses",
        "corrected",
        "harmed",
        "net_correction",
        "eligible",
        "tag_hits",
        "conf_enabled",
        "overrides",
        "reference_net_correction",
        "ipc",
        "queue_dropped",
    ]
    with (args.output_root / "sweep.csv").open("w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=sweep_fields)
        writer.writeheader()
        sweep_keys = set()
        for task in tasks:
            candidate_pcs = {
                address(row["pc"])
                for row in per_pc
                if row["task"] == tag(task)
            }
            for mask in range(32):
                status = read_status(args, task, "masks", mask)
                reference = json.loads(
                    (
                        image_path(args, task, "masks", mask)
                        / "reference.json"
                    ).read_text()
                )
                ref_pc = {address(row["pc"]): row for row in reference["pcs"]}
                for counts in status["hint"]["pcs"]:
                    pc = address(counts["pc"])
                    if pc not in candidate_pcs:
                        continue
                    ref = ref_pc.get(pc, {})
                    key = (tag(task), mask, pc)
                    if key in sweep_keys:
                        raise ValueError(
                            "duplicate task/mask/PC in sweep report"
                        )
                    sweep_keys.add(key)
                    writer.writerow(
                        {
                            "task": tag(task),
                            "mask": mask,
                            **{
                                key: value
                                for key, value in counts.items()
                                if key in sweep_fields
                            },
                            "net_correction": counts["corrected"]
                            - counts["harmed"],
                            "reference_net_correction": ref.get("corrected", 0)
                            - ref.get("harmed", 0),
                            "ipc": status["ipc"],
                            "queue_dropped": status["hint"]["queue"][
                                "dropped"
                            ],
                        }
                    )
    if len(sweep_keys) != complete["expected_sweep_rows"]:
        raise ValueError(
            "sweep report does not cover every baseline candidate and all 32 masks"
        )
    summary["sweep_rows"] = len(sweep_keys)
    write_json(args.output_root / "summary.json", summary)
    checker = legacy()
    with (args.output_root / "window-trends.csv").open(
        "w", newline=""
    ) as output:
        writer = csv.writer(output)
        writer.writerow(
            [
                "task",
                "measure_start_inst",
                "pc",
                "off_miss",
                "finite_miss",
                "fixed_ideal_miss",
                "joint_ideal_miss",
            ]
        )
        for task in tasks:
            for offset in range(0, 50_000_000, 1_000_000):
                parsed = [
                    checker.parse_h2p_output(
                        run_path(args, task, stage)
                        / "h2p-indirec"
                        / f"{offset}.txt"
                    )
                    for stage in ("baseline", "finite", "fixed", "joint")
                ]
                for pc in sorted(set().union(*(set(rows) for rows in parsed))):
                    writer.writerow(
                        [
                            tag(task),
                            offset,
                            hex(pc),
                            *(rows.get(pc, (0,))[0] for rows in parsed),
                        ]
                    )
    lines = [
        "# 无限容量 Hint IBP 实验",
        "",
        "结果属于固定历史、28-bit hash 及别名、端口、时序和构表规则下的容量放开收益；"
        "同一切片的 measure 数据用于离线构表和重放，不代表完美 target oracle，也不保证联合 mask 的数学最优。",
        "",
        "五片均在 restore 完成后执行 100M warm-up，reset 后执行 50M measure，H2P interval 为 1M；"
        "difftest 关闭，DDR4_2400_8x8。全部 185 次运行和 9250 个窗口独立审计通过。",
        "",
        "| 切片 | off miss | 有限 miss | 固定 PC/mask 无限 miss | 重新搜索无限 miss | off / 有限 / 固定无限 / 联合无限 IPC | 推荐 |",
        "|---|---:|---:|---:|---:|---|---|",
    ]
    for row in summaries:
        stages = row["stages"]
        lines.append(
            f"| {row['task']} | "
            + " | ".join(
                str(stages[stage]["final_misses"])
                for stage in ("baseline", "finite", "fixed", "joint")
            )
            + " | "
            + " / ".join(
                f"{stages[stage]['ipc']:.6f}"
                for stage in ("baseline", "finite", "fixed", "joint")
            )
            + f" | {row['recommended']} |"
        )
    lines += [
        "",
        "| 切片 / 运行 | 表 | 常驻有效项 | 平均项 | 峰值项 | 平均 / 峰值相对有限容量倍数 | 采样周期 |",
        "|---|---|---:|---:|---:|---|---:|",
    ]
    for row in summaries:
        for stage in ("finite", "fixed", "joint"):
            for table, value in row["stages"][stage]["occupancy"].items():
                lines.append(
                    f"| {row['task']} / {stage} | {table} | {value['resident_entries']} | "
                    f"{value['avg_entries']} | {value['peak_entries']} | "
                    f"{value['avg_multiple']:.6f} / {value['peak_multiple']:.6f} | {value['sample_cycles']} |"
                )
    lines += [
        "",
        "三表冻结，measure 内不动态增删；conf=0 的有效项仍占空间，所以非空 measure 的平均和峰值"
        "等于常驻条目数。上述倍数以 CAM16、Context4096、Slot32 为分母，不是无限表的利用率百分比。",
        "collect 不参与三表常驻容量比较。",
        "",
        "固定选择无限组严格采用旧最终 CAM 的 PC/mask 和旧已审计 collect；重新搜索组使用新 baseline 的全部 H2P PC。"
        "因此有限→固定无限反映固定选择的容量收益，固定无限→联合无限还包含候选和 mask 重新选择的变化。",
        "",
        "逐 PC 的全部 mask 实测净纠错和固定 collect 周期参考结果见 `sweep.csv`；最终选择和联合实测见 `best-masks.json`。"
        "参考模型不预测 Hint 引起的新投机历史和运行周期，因此离线估计与真实 replay 分开报告。",
        "",
        "每个 1M 窗口的四组最终 miss 见 `window-trends.csv`。`summary.json` 保留三个阶段差分、IPC、"
        "纠错/错误覆盖、查询取消原因、队列入队/写回/丢弃/pending 以及三表占用。每项实际 1→0 保留在 `conf0.txt`。",
        "",
        "`hard-pred.txt` 区分无正收益映射、查询覆盖及实际残余；覆盖计数包含全部执行，不能直接相加作为残余 miss 的互斥分解。"
        "有限模式回归和新旧共同 PC 的采集一致性证据见完整审计。",
        "",
        "容量效率建议应优先依据固定选择对照：先定位增加的有效 Context/target 数量带来的实际 miss 改善，再研究压缩和共享。"
        "若容量放开收益很小，应进一步检查查询资格、hash 别名和 confidence，而不能仅扩大表。推荐只按实际最终 miss 是否优于 off 判定；"
        "IPC 单独报告，退化镜像及运行完整保留。",
    ]
    (args.output_root / "report.md").write_text("\n".join(lines) + "\n")
    return summary
