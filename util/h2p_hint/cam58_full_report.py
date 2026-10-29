"""Audit and summarize the completed 100M/200M experiment on its host."""
from pathlib import Path
import json
import os
import spec_smoke as smoke

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / 'results/model/full-test' / os.environ.get('CAM58_FULL_RUN_ID', 'cam58-100m200m-20261023')

def main():
    smoke.require_experiment_host()
    status = smoke.read(OUT / 'status.json')
    smoke.need(status['status'] == 'PASS', 'Full pipeline did not complete')
    inputs = smoke.read(OUT / 'inputs.json')
    for item in inputs['sources'] + inputs['docs'] + [inputs['binary']]:
        smoke.need(smoke.identity(item['path']) == item, 'Frozen input drift')
    stages = runs = artifacts = 0
    for path in (OUT / 'pipeline').glob('*/*/*/stage.json'):
        stage = smoke.read(path)
        stages += 1
        for item in stage['artifacts']:
            smoke.need(smoke.identity(item['path']) == item, f'Artifact drift {item["path"]}')
            artifacts += 1
        reports = ([stage['report']] if stage['stage'] in ('baseline', 'discovery', 'detail')
                   else stage['report']['candidates'] if stage['stage'] == 'replay' else [])
        for report in reports:
            smoke.need(report['difftest_enabled'] and report['binary'] == inputs['binary'], 'Run identity differs')
            smoke.need(report['window']['warmup_instructions'] == 100000000 and
                       report['window']['measure_instructions'] == 200000000, 'Instruction window differs')
            runs += 1
    smoke.need(stages == 70 and runs == 60, 'Missing stages/runs')
    rows = smoke.read(OUT / 'pipeline/summary.json')['slices']
    aggregate = smoke.read(OUT / 'aggregate.json')
    b, h = aggregate['baseline'], aggregate['best_hint_candidate']
    lines = ['# CAM58 100M warmup / 200M measure 模拟器测试', '',
        'ps1；10 个切片；NEMU difftest；冻结 CAM58 v2 ELF；默认 metaQueueHint=16。每片重新 Discovery、选择 CAM、Detail、固定 CFI 过滤、构表并实测三个静态候选。', '',
        '| 模式 | 汇总 IPC | 非 return 间接目标 miss |', '|---|---:|---:|',
        f"| Hint Off | {b['ipc']:.6f} | {b['nonreturn_misses']:,} |",
        f"| Hint On | {h['ipc']:.6f} | {h['nonreturn_misses']:,} |", '',
        f"IPC 提升 {(h['ipc']/b['ipc']-1)*100:.2f}%；非 return miss 减少 {(1-h['nonreturn_misses']/b['nonreturn_misses'])*100:.2f}%。IPC 按20亿总测量指令/总测量周期汇总，不使用SimPoint权重。", '',
        '| 切片 | Off IPC | On IPC | IPC变化 | 非 return miss：Off → On | 候选 |',
        '|---|---:|---:|---:|---:|---|']
    for row in rows:
        base, hint = row['baseline'], row['best_hint_candidate']
        bm = base['prediction_metrics']['nonreturn_ittage_plus_hint']['misses']
        hm = hint['prediction_metrics']['nonreturn_ittage_plus_hint']['misses']
        lines.append(f"| {row['tag']} | {base['ipc']:.4f} | {hint['ipc']:.4f} | {(hint['ipc']/base['ipc']-1)*100:+.2f}% | {bm:,} → {hm:,} | {hint['label']} |")
    lines += ['', f'70 个阶段、60 次模拟运行通过；{artifacts} 个阶段产物的 SHA256、源码/文档/ELF 身份、100M/200M 退休窗口和 difftest 已核对。', '',
              '三个候选在相同测量窗口内选择，为配置探索结果；未运行额外串行调优。架构文档、RTL 和模型二进制未修改。未进行模型/RTL周期校准、综合或STA。先前1M/1M smoke结果单独保留。', '',
              '启动时首个用户服务随SSH会话退出被终止，证据保留在相邻 interrupted-session 目录；最终批次通过保持SSH等待服务完成运行。该中断批次未计入性能汇总。', '',
              '旧NEMU参考库未启用RV_CBO，长窗口baseline在内核cbo.zero处触发difftest失败；该失败批次独立保留。最终批次保持GEM5 ABI，隔离启用RV_CBO，并补CBO异常枚举所需intr.h声明。参考库配置差异、构建日志及新旧SHA均保留；所有最终阶段使用同一新参考库，未关闭difftest。', '',
              '大型ELF和原始trace保留在ps1；完整运行证据见pipeline/、inputs.json、status.json、aggregate.json与final-audit.json。']
    (OUT / 'report.md').write_text('\n'.join(lines) + '\n')
    audit = {'status': 'PASS', 'stages': stages, 'difftest_runs': runs,
             'checked_artifacts': artifacts, 'binary': inputs['binary'], 'inputs_unchanged': True}
    smoke.write(OUT / 'final-audit.json', audit)
    smoke.write(ROOT / 'results/model-full-test.json', {
        'status': 'FULL_TEST_COMPLETE_HUMAN_REVIEW_PENDING', 'difftest_status': True,
        'warmup_insts': 100000000, 'measure_insts': 200000000,
        'report': str(OUT / 'report.md'), 'aggregate': aggregate, 'audit': audit,
        'cpts': rows})
    print(json.dumps({'audit': audit, 'aggregate': aggregate}))

if __name__ == '__main__':
    main()
