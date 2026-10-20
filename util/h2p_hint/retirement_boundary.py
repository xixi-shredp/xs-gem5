"""Strict fusion-boundary retirement audit.

Ported without semantic changes from the independently validated recovery audit:
results/model/smoke-test/20261016-uart16550-100m50m/recovery/tools/boundary_audit.py.
Raw simulator counters are never altered; corrections assign already executed
logical instructions to their requested warmup and measurement intervals.
"""
SCHEMA='hint-retirement-boundary/v2-fusion-effective'

def retirement_window(phases,warmup,measure,stats):
    def need(ok,msg):
        if not ok:raise ValueError(msg)
    restore,reset,end=(phases[n] for n in ('restore_complete','warmup_complete','measure_complete'))
    for p in (restore,reset,end):
        for k in ('retired_post_restore','core_cycle','tick','warmup','measure','reset_retired','warmup_fusion_split','terminal_fusion_excess'):
            need(type(p.get(k)) is int and p[k]>=0,'nonnegative integer phase field required: '+k)
    split=reset['warmup_fusion_split'];excess=end['terminal_fusion_excess']
    need(split in (0,1) and excess in (0,1),'only documented two-instruction fusion boundary corrections are legal')
    need(restore['warmup_fusion_split']==0 and restore['terminal_fusion_excess']==0,'restore boundary must be clean')
    need(reset['terminal_fusion_excess']==0 and end['warmup_fusion_split']==split,'phase fusion markers disagree')
    need(reset['reset_retired']==reset['retired_post_restore'] and end['reset_retired']==reset['reset_retired'],'raw reset counter not stable')
    reset_effective=reset['retired_post_restore']-split
    end_effective=end['retired_post_restore']-excess
    actual_warmup=reset_effective-restore['retired_post_restore']
    actual_measure=end_effective-reset_effective
    cycles=end['core_cycle']-reset['core_cycle']
    need(actual_warmup==warmup and actual_measure==measure and cycles>0,'effective retired windows must exactly match requested counts')
    need(reset['warmup']==end['warmup']==actual_warmup and reset['measure']==0 and end['measure']==actual_measure,'phase warmupCommitted/measureCommitted disagree with corrected retire counters')
    need(end['tick']-reset['tick']==1000*cycles,'raw ticks and 3GHz core cycles disagree')
    for name,expected in [('simInsts',actual_measure),('system.cpu.committedInsts',actual_measure),('system.cpu.numCycles',cycles),('simTicks',1000*cycles)]:
        need(stats.get(name)==[float(expected)],'strict measured statistic disagrees: '+name)
    window={'warmup_instructions':actual_warmup,'measure_instructions':actual_measure,'measure_cycles':cycles,
        'measure_ipc':actual_measure/cycles,'reset_retired':reset_effective,'end_retired':end_effective,
        'reset_core_cycle':reset['core_cycle'],'end_core_cycle':end['core_cycle']}
    boundary={'schema':SCHEMA,'raw_restore_retired':restore['retired_post_restore'],
        'raw_reset_retired':reset['retired_post_restore'],'raw_end_retired':end['retired_post_restore'],
        'warmup_fusion_split':split,'terminal_fusion_excess':excess,
        'reset_effective':reset_effective,'end_effective':end_effective,
        'strict_measure_crosschecks':['phase.measureCommitted','stats.simInsts','stats.system.cpu.committedInsts'],
        'raw_logs_or_stats_modified':False,'bounds_relaxed':False,
        'normalization_semantics':'logical retirement ownership at fused per-unit phase transitions, not relaxed execution window'}
    return window,boundary
