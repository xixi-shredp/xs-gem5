"""Audit the final remote CAM58 experiment and publish its model report."""
import json
from pathlib import Path
import spec_smoke as smoke

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / 'results/model/smoke-test/cam58-20261023-v2'

def main():
    smoke.require_experiment_host()
    summary = smoke.read(OUT / 'pipeline/summary.json')
    smoke.need(summary['complete'] and len(summary['slices']) == 10, 'Incomplete smoke')
    for item in smoke.read(OUT / 'sources.json') + smoke.read(OUT / 'docs.json'):
        smoke.need(smoke.identity(item['path']) == item, 'Source/doc drift')
    smoke.need(smoke.read(OUT / 'build-exit.json')['returncode'] == 0, 'Build failed')
    directed = smoke.read(OUT / 'directed/validation.json')
    smoke.need(directed['passed'] and len(directed['cases']) == 21 and
               all(case['passed'] for case in directed['cases']), 'Directed checks failed')
    smoke.need('OK' in (OUT / 'adapter-test.log').read_text(), 'Adapter tests failed')
    binary = smoke.read(OUT / 'binary.json')
    smoke.need(smoke.identity(binary['path']) == binary, 'ELF drift')
    smoke.need(directed['gem5_sha256'] == binary['sha256'], 'Directed ELF differs')
    artifacts = 0
    stages = 0
    runs = 0
    for stage_file in (OUT / 'pipeline').glob('*/*/*/stage.json'):
        stage = smoke.read(stage_file)
        for item in stage['artifacts']:
            smoke.need(smoke.identity(item['path']) == item, f'Artifact drift {item["path"]}')
            artifacts += 1
        stages += 1
        if stage['stage'] in ('baseline', 'discovery', 'detail'):
            reports = [stage['report']]
        elif stage['stage'] == 'replay':
            reports = stage['report']['candidates']
        else:
            reports = []
        for report in reports:
            smoke.need(report['difftest_enabled'], 'Difftest disabled')
            smoke.need(report['window']['measure_instructions'] == 1000000 and
                       report['window']['warmup_instructions'] == 1000000, 'Window drift')
            smoke.need(report['binary'] == binary, 'Run ELF differs')
            runs += 1
    smoke.need(stages == 70 and runs == 60, 'Unexpected stage/run count')
    rows = summary['slices']
    def aggregate(key):
        reports = [row[key] for row in rows]
        cycles = sum(r['window']['measure_cycles'] for r in reports)
        return {'ipc': 10000000 / cycles, 'measure_cycles': cycles,
                'measure_instructions': 10000000,
                'nonreturn_misses': sum(r['prediction_metrics']['nonreturn_ittage_plus_hint']['misses'] for r in reports),
                'all_indirect_misses': sum(r['prediction_metrics']['all_indirect']['misses'] for r in reports)}
    off, on = aggregate('baseline'), aggregate('best_hint_candidate')
    audit = {'status': 'PASS', 'stages': stages, 'difftest_runs': runs,
             'checked_artifacts': artifacts, 'binary': binary,
             'source_and_docs_unchanged': True, 'host': smoke.execution_host()}
    smoke.write(OUT / 'final-audit.json', audit)
    lines = ['# CAM58 模拟器 SPEC 对齐结果', '',
        '最终版本：已接受的 conf FIFO 请求在 redirect 后保留，且 final value 不可修改、不得重复入队。首轮结果 cam58-20261023 单独保留；本报告只使用重建后的 v2。', '',
        'ps1；10 个切片；每片 1M warmup + 1M measure；NEMU difftest。每片重新执行 baseline、Discovery、CAM 选择、Detail、固定 CFI 过滤、构表、三个候选评测及按实测 IPC 选择。共 70 个阶段、60 次模拟运行。', '',
        '| 模式 | 汇总 IPC | 非 return 间接目标 miss | 全部间接 CFI miss |',
        '|---|---:|---:|---:|',
        f"| Hint Off | {off['ipc']:.6f} | {off['nonreturn_misses']:,} | {off['all_indirect_misses']:,} |",
        f"| Hint On，逐片最佳静态候选 | {on['ipc']:.6f} | {on['nonreturn_misses']:,} | {on['all_indirect_misses']:,} |", '',
        f"汇总 IPC 提升 {(on['ipc']/off['ipc']-1)*100:.2f}%；非 return miss 减少 {(1-on['nonreturn_misses']/off['nonreturn_misses'])*100:.2f}%。IPC 按总指令/总周期计算，不使用 SimPoint 权重。候选在同一测量窗口选择，属于 smoke 配置探索结果。", '',
        '| 切片 | Off IPC | On IPC | IPC 变化 | 非 return miss：Off → On | 候选 |',
        '|---|---:|---:|---:|---:|---|']
    cpts = []
    for row in rows:
        b, h = row['baseline'], row['best_hint_candidate']
        bm = b['prediction_metrics']['nonreturn_ittage_plus_hint']['misses']
        hm = h['prediction_metrics']['nonreturn_ittage_plus_hint']['misses']
        lines.append(f"| {row['tag']} | {b['ipc']:.4f} | {h['ipc']:.4f} | {(h['ipc']/b['ipc']-1)*100:+.2f}% | {bm:,} → {hm:,} | {h['label']} |")
        bench, point = row['tag'].split('/')
        base = OUT / 'pipeline' / row['tag']
        def paths(directory):
            return {name + '_path': str(directory / filename) for name, filename in
                    [('stats', 'stats.txt'), ('stdout', 'stdout.log'), ('stderr', 'stderr.log')]}
        cpts.append({'workload': 'cpython', 'bench': bench, 'cpt_simpoint_id': point,
                     'baseline_results': paths(base / 'baseline'),
                     'new_results': paths(base / 'replay' / h['label']),
                     'baseline': b, 'selected_hint': h})
    lines += ['', '实现包括 CAM58 严格 magic/位置/call、独立 ITTAGE 回退、最早 taken 仲裁、真实指令末半字身份、47b global/101b Hint metadata 的有限历史恢复、resolve 训练以及 all-control miss 排序和固定 CFI 构表适配。未修改架构文档或 RTL。', '',
        '4 项 Python adapter 测试与 21 项生产 gem5 helper/loader 正负控通过；helper 包含 pending/applied 请求跨 redirect 存活以及 retained-CFI target 改变不重写请求的回归。最终源码、文档、ELF 和阶段产物 SHA 已复核。', '',
        '有限 RTL 静态映射：function PASS；timing、area 和 overall NEEDS_INPUT。声明存储 payload 为 111536 bits（不含 live heads），不代表完整面积；仍需控制状态布局、SRAM 宏与综合/STA 证据。没有进行 RTL 周期校准或物理签核。', '',
        '详细证据：final-audit.json、sources.json、docs.json、binary.json、build.log、adapter-test.log、directed.log、pipeline/summary.json。大型 ELF、原始 trace 和 replay 文件保留在 ps1。人类审核待完成，尚未通知 RTL Writer。']
    (OUT / 'report.md').write_text('\n'.join(lines) + '\n')
    report = {'status': 'SMOKE_COMPLETE_HUMAN_REVIEW_PENDING', 'compile_status': True,
              'difftest_status': True, 'smoke_status': 'PASS',
              'report': str(OUT / 'report.md'), 'audit': audit, 'binary': binary,
              'aggregate': {'baseline': off, 'selected_hint': on,
                            'speedup': on['ipc']/off['ipc']},
              'rtl_mapping': {'function': 'PASS', 'timing': 'NEEDS_INPUT',
                              'area': 'NEEDS_INPUT', 'overall': 'NEEDS_INPUT'},
              'cpts': cpts}
    smoke.write(ROOT / 'results/model-smoke-test.json', report)
    print(json.dumps({'audit': audit, 'aggregate': report['aggregate']}))

if __name__ == '__main__':
    main()
