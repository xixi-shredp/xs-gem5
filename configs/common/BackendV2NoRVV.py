"""Explicit modelable parameters of the frozen BackendV2 scalar experiment.

This module has no m5 imports so the launch audit can inspect the same data
used by the simulator, before a new executable has been built.
"""
TICK_FREQUENCY_HZ = 3_000_000_000_000
CORE_FREQUENCY_HZ = 3_000_000_000
RESPONSE_PADDING_CYCLES = 66

CPU_PARAMS = {
    'fetchWidth': 32, 'fetchQueueSize': 48, 'fetchBufferSize': 66,
    'enableTwoFetch': True, 'twoFetchMaxBytes': 64,
    'iewToFetchDelay': 4, 'commitToFetchDelay': 4, 'fetchToDecodeDelay': 3,
    'decodeWidth': 6, 'renameWidth': 6, 'commitWidth': 8, 'squashWidth': 8,
    'phyregReleaseWidth': 6, 'numROBEntries': 208,
    'RobCompressPolicy': 'none', 'CROB_instPerGroup': 1,
    'numMaxRatSnapshot': 4, 'numPhysIntRegs': 224, 'numPhysFloatRegs': 192,
    # Fixed checkpoint/difftest ABI state, with zero vector rename capacity.
    'numPhysVecRegs': 40, 'numPhysVecPredRegs': 1,
    'enableDispatchStage': False, 'numDQEntries': [8, 8, 8],
    'dispWidth': [6, 6, 6], 'enable_loadFusion': False,
    'enableConstantFolding': False, 'enable_storeSet_train': True,
    'StoreWbStage': 4, 'EnableLdMissReplay': True,
    'EnablePipeNukeCheck': True, 'BankConflictCheck': True,
    'sbufferBankWriteAccurately': True,
    'LQEntries': 120, 'SQEntries': 64, 'StoreQueueMultiple': 2,
    'RARQEntries': 96, 'RAWQEntries': 56,
    'LoadCompletionWidth': 3, 'StoreCompletionWidth': 2,
    'RARDequeuePerCycle': 4, 'RAWDequeuePerCycle': 4,
    'SbufferEntries': 16, 'SbufferEvictThreshold': 9,
    'store_prefetch_train': False,
}

BPU_PARAMS = {
    '': {'ftq_size': 64, 'fsq_size': 64, 'predictWidth': 64,
         'idealTwoTaken': False, 'resolveBlockThreshold': 8},
    'ubtb': {'enabled': True, 'numEntries': 32, 'tagBits': 22,
             'usingS3Pred': True},
    'abtb': {'enabled': True, 'numEntries': 1024, 'numWays': 4,
             'tagBits': 24, 'usingS3Pred': True},
    'mbtb': {'enabled': True, 'numEntries': 8192, 'numWays': 4,
             'tagBits': 16, 'resolvedUpdate': True},
    'tage': {'enabled': True, 'resolvedUpdate': True,
             'tableSizes': [2048] * 8, 'numWays': [2] * 8,
             'TTagBitSizes': [13] * 8,
             'histLengths': [4, 9, 17, 29, 56, 109, 211, 397],
             'numBanks': 4, 'useAltOnNaSize': 16, 'useAltOnNaWidth': 5},
    'ittage': {'enabled': True, 'resolvedUpdate': True,
               'tableSizes': [256, 256, 512, 512, 512],
               'TTagBitSizes': [9] * 5, 'histLengths': [4, 8, 13, 16, 32]},
    'microtage': {'enabled': True, 'usingS3Pred': True,
                  'tableSizes': [512, 512], 'TTagBitSizes': [8, 8],
                  'histLengths': [5, 9], 'baseTableSize': 512},
    'ras': {'enabled': True, 'numEntries': 16,
            'numInflightEntries': 32, 'ctrWidth': 3},
    'mgsc': {'enabled': True, 'enableLTable': False,
             'bwTableNum': 2, 'bwHistLen': [4, 8], 'bwTableIdxWidth': 10,
             'gTableNum': 2, 'gHistLen': [8, 16], 'gTableIdxWidth': 10,
             'pTableNum': 2, 'pHistLen': [8, 16], 'pTableIdxWidth': 10,
             'iTableNum': 1, 'iHistLen': [8], 'iTableIdxWidth': 11,
             'biasTableNum': 1, 'biasTableIdxWidth': 12,
             'numCtrsPerLine': 8, 'scCountersWidth': 6},
}

RESIDUAL_DIFFERENCES = [
    'gem5 wakeup is per IQ; RTL wakeup is per execution unit. Aggregation '
    'also exposes the co-located BJU source to its IQ wakeup channel.',
    'BKU instructions share IntAlu opclass: RTL restricts BKU to ALU2/3; '
    'gem5 cannot separate that placement without changing its decoder.',
    'FloatMv includes both I-to-F and F-to-I FMV directions and FLI. All '
    'remain legal scalar FP operations, but existing shared opclass routing '
    'cannot independently match their RTL integer/FP execution endpoints.',
    'gem5 IQ lacks the RTL simple/complex entry split and physical bypass '
    'topology; retained FU execution latencies are model abstractions.',
    'RAB256 has no separate gem5 resource; commit remains 8, register release '
    'and rename are 6. Register snapshots use existing model recovery.',
    'RTL SC IMLI has 128x8=1024 counters; existing ImliFoldedHist requires '
    'folded index bits >= history8, so gem5 keeps 256x8=2048 counters.',
    'SC bias counter capacity4096 matches RTL128x32; gem5 layout is512x8. '
    'SC/MicroTAGE hashing and RAS physical mechanisms remain model algorithms.',
    'ITTAGE bank2 ports, RTL resolve/commit queue timing and IBuffer banking '
    'have no exact existing gem5 parameter.',
    'L2 aligned wrapper -> membus approximates the cacheless RTL CHI-AXI '
    'bridge. Arbitration and protocol pipeline latency are not cycle calibrated.',
    'DRAM callbacks use exact tCK1890ticks in gem5; RTL steps fractional '
    'DRAM clocks at core1000tick boundaries, leaving <1 corecycle quantization.',
    'Fixed 40 vector and 1 predicate states preserve checkpoint ABI; they '
    'provide no free speculative rename entries and no executable RVV FU.',
]
