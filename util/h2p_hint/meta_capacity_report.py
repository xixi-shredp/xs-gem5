"""Audit and report the completed metadata-capacity experiment on its host."""
import json
from pathlib import Path
import spec_smoke as smoke

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / 'results/model/smoke-test/meta32-20261022'

def main():
    smoke.require_experiment_host()
    comparison = smoke.read(OUT / 'comparison.json')
    old = smoke.read(OUT / 'previous-model-report.json')
    new = smoke.read(OUT / 'pipeline32/summary.json')
    same_metrics = []
    same_profiles = []
    same_static = []
    for cpt, revised in zip(old['cpts'], new['slices']):
        tag = revised['tag']
        smoke.need(tag == cpt['bench'] + '/' + cpt['cpt_simpoint_id'], 'Tag mismatch')
        r16 = smoke.read(OUT / 'verify16' / tag / 'report.json')
        r32 = smoke.read(OUT / 'fixed32' / tag / 'report.json')
        smoke.need(r16['window'] == r32['window'] and r16['prediction_metrics'] == r32['prediction_metrics'], 'Fixed-image capacity result changed')
        same_metrics.append(tag)
        for stage, name in [('discovery', 'hint-discovery.bin'), ('detail', 'hint-detail.bin'), ('select-cam', 'cam.bin')]:
            old_file = ROOT / 'results/model/smoke-test/spec-pipeline' / tag / stage / name
            new_file = OUT / 'pipeline32' / tag / stage / name
            same_profiles.append({'tag': tag, 'stage': stage, 'old': smoke.identity(old_file),
                'new': smoke.identity(new_file), 'same_sha256': smoke.sha(old_file) == smoke.sha(new_file)})
        initial = [c for c in cpt['all_candidate_results'] if c['label'] in ('static-best', 'uniform9', 'uniform15')]
        initial_best = min(initial, key=lambda c: (-c['ipc'], c['label']))
        same_static.append({'tag': tag, 'ipc16_initial': initial_best['ipc'],
            'ipc32_reprofile': revised['best_hint_candidate']['ipc'],
            'same_ipc': initial_best['ipc'] == revised['best_hint_candidate']['ipc'],
            'same_selected_image_sha256': smoke.sha(initial_best['image']) == smoke.sha(revised['best_hint_candidate']['image'])})
    audit = {'status': 'PASS', 'host': smoke.execution_host(),
        'fixed_images_all_windows_and_prediction_metrics_equal': same_metrics,
        'profiles': same_profiles, 'static_round_comparison': same_static,
        'new_binary': smoke.read(OUT / 'new-binary.json'),
        'experiment_resume_script': smoke.identity(ROOT / 'xs-gem5/util/h2p_hint/meta_capacity_experiment.py'),
        'report_script': smoke.identity(__file__)}
    smoke.need(all(x['same_sha256'] for x in same_profiles), 'Profiling output changed; investigate before interpreting results')
    smoke.need(all(x['same_ipc'] and x['same_selected_image_sha256'] for x in same_static), 'Initial candidate round differs')
    smoke.write(OUT / 'final-audit.json', audit)
    a = comparison['aggregate']
    off_cycles = sum(c['baseline']['window']['measure_cycles'] for c in new['slices'])
    off_ipc = 10_000_000 / off_cycles
    lines = ['# metaQueueHint 16 / 32 项容量实验', '',
        '结论：固定镜像仅扩大容量没有性能或 miss 收益。全部 10 片的严格退休窗口、周期和公开预测计数完全一致；metadata 容量回退均为 0。', '',
        'ps1；BackendV2NoRVVNoLLC；每片 1M warmup + 1M measure，NEMU difftest。32 项重新完成 Discovery / Detail / 构表 / 30 次静态候选评测；另用同一新 ELF 执行旧最终镜像 16/32 项配对复跑，共 20 次。Hint Off baseline 同一新 ELF 重跑。', '',
        '| 场景 | 汇总 IPC | 相对上一轮最终 16 项 | 非 return miss |',
        '|---|---:|---:|---:|',
        f"| 上一轮最终 16 项 | {a['ipc16']:.6f} | — | {a['miss16']:,} |",
        f"| 固定旧镜像，32 项 | {a['fixed32_ipc']:.6f} | {(a['fixed32_ipc']/a['ipc16']-1)*100:+.4f}% | {a['fixed32_miss']:,} |",
        f"| 32 项重新构表，静态候选择优 | {a['ipc32']:.6f} | {(a['ipc32']/a['ipc16']-1)*100:+.4f}% | {a['miss32']:,} |", '',
        '| 切片 | 上一轮最终 16 项 IPC | 32 项重新构表 IPC | 变化 | miss：16 → 32 |',
        '|---|---:|---:|---:|---:|']
    for r in comparison['slices']:
        lines.append(f"| {r['tag']} | {r['ipc16']:.4f} | {r['ipc32']:.4f} | {(r['ipc32']/r['ipc16']-1)*100:+.2f}% | {r['miss16']:,} → {r['miss32']:,} |")
    lines += ['',
        '两片差异来自候选范围：上一轮最终 16 项结果包含四次额外串行候选，其中 gc_collect/25 和 pyflate/43 被选中；本轮重新构表采用每片三个静态候选，未重新进行该串行优化。审计确认：32 项 Discovery/Detail/CAM 与旧版原始文件 SHA256 全部相同；同范围静态候选选择的镜像与 IPC 也全部相同。因此不能把 -0.23% 解释为容量扩大导致的退化。', '',
        f"Hint Off 汇总 IPC={off_ipc:.6f}；32 项重新构表相对 Off 提升 {(a['ipc32']/off_ipc-1)*100:.2f}%；沿用旧调优镜像的 32 项提升 {(a['fixed32_ipc']/off_ipc-1)*100:.2f}%。", '',
        'miss 口径为已提交非 return 间接分支最终目标错误，包含缺失目标。IPC 按总指令/总周期汇总，未使用 SimPoint 权重。', '',
        '只增加 Hint metadata 总深度：16→32；每 CAM entry 在飞上限仍为 8，LHR 环及 conf FIFO 深度保持原设定。默认仍为 16，使用 --h2p-hint-meta-entries=32 开启探索配置。架构文档及 RTL 未修改；没有综合、STA 或全量 workload 结论。', '',
        '构建沿用既有 deps/build-env/bin/g++ 包装器。首次 GCC15 告警升错和附加对照读取串行候选 SHA 字段的错误均有单独日志保留；最终 build-exit=0、pipeline 完整通过，附加对照与最终审计通过。大型 ELF、私有 replay 和 CSV 留在 ps1，其余结果回传工作区。', '',
        '证据：comparison.json、final-audit.json、pipeline32/summary.json、verify16-summary.json、build-exit.json、binary-check.log。']
    (OUT / 'report.md').write_text('\n'.join(lines) + '\n')
    print(json.dumps({'status': 'PASS', 'same_fixed_metric_slices': len(same_metrics),
        'same_profile_files': len(same_profiles), 'same_initial_selections': len(same_static),
        'off_ipc': off_ipc, 'comparison': a}))

if __name__ == '__main__':
    main()
