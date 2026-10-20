"""V7 regressions preserving all v6 start-PC, validity, and strict-window cases.

The historical module filename is retained so existing validation entry points
continue to execute the complete suite; profiles come from current v7 fixtures.
"""
import copy
import gzip
import json
from pathlib import Path
import tempfile
import unittest

from . import finite_v3_test as fixtures
from .finite_v3 import build_image, read_profile, score_image
from .finite_runner import ranked_candidates
from .retirement_boundary import retirement_window


class SpecV7Test(unittest.TestCase):
    def test_same_actual_branch_multiple_query_windows(self):
        helper = fixtures.FiniteV3Test()
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)/'profile.gz'
            rows = [helper.row(0x1000,1), helper.row(0x1010,2)]
            for row in rows:
                row.update(pc='0x1012', baseline_pc='0x1012')
            helper.profile(path,[0x1000,0x1010],rows)
            _, decoded = read_profile(path)
            metadata,image,_ = build_image(decoded,{0x1000:0,0x1010:0},helper.source())
            self.assertEqual([r['pc'] for r in metadata['cam']],['0x1000','0x1010'])
            self.assertEqual(sum(r['corrected'] for r in score_image(decoded,metadata)['pcs']),2)
            self.assertEqual(len(image),16784)

    def test_invalid_snapshot_not_used_or_reconstructed(self):
        helper = fixtures.FiniteV3Test()
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'profile.gz'
            row=helper.row(0x1000)
            row.update(query_valid=False,eligible=False,contexts=None,history=None,query_cycle=None)
            helper.profile(path,[0x1000],[row])
            _,decoded=read_profile(path)
            metadata,_,_=build_image(decoded,{0x1000:15},helper.source())
            self.assertEqual(metadata['entries'],[])
            row.update(query_valid=True)
            helper.profile(path,[0x1000],[row])
            with self.assertRaises(ValueError): read_profile(path)

    def test_discovery_top16_start_pc_and_strict_duplicates(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); path=root/'hint-discovery.json'
            branches=[dict(pc=hex(0x1000+2*i),executions=100,target_misses=i) for i in range(20)]
            document=dict(schema='hint-discovery/v1',key='query_start_pc',branches=branches)
            path.write_text(json.dumps(document))
            pcs,rank=ranked_candidates(root)
            self.assertEqual(len(pcs),16)
            self.assertEqual(pcs[0],0x1026)
            self.assertEqual(rank[-1][1],4)
            branches.append(branches[0]);path.write_text(json.dumps(document))
            with self.assertRaises(ValueError): ranked_candidates(root)

    def sample(self, split=0, excess=0):
        common=dict(warmup_fusion_split=0,terminal_fusion_excess=0)
        restore=dict(common,retired_post_restore=0,reset_retired=0,core_cycle=10,tick=10000,warmup=0,measure=0)
        reset=dict(common,retired_post_restore=1000000+split,reset_retired=1000000+split,core_cycle=1000010,tick=1000010000,warmup=1000000,measure=0)
        reset['warmup_fusion_split']=split
        end=dict(common,retired_post_restore=2000000+excess,reset_retired=1000000+split,core_cycle=2000010,tick=2000010000,warmup=1000000,measure=1000000)
        end.update(warmup_fusion_split=split,terminal_fusion_excess=excess)
        stats={'simInsts':[1000000.0],'system.cpu.committedInsts':[1000000.0],
               'system.cpu.numCycles':[1000000.0],'simTicks':[1000000000.0]}
        return dict(restore_complete=restore,warmup_complete=reset,measure_complete=end),stats

    def test_all_fusion_boundaries_preserve_exact_1m_windows(self):
        for split,excess in ((0,0),(1,0),(0,1),(1,1)):
            phases,stats=self.sample(split,excess)
            window,evidence=retirement_window(phases,1000000,1000000,stats)
            self.assertEqual(window['measure_instructions'],1000000)
            self.assertEqual(window['warmup_instructions'],1000000)
            self.assertFalse(evidence['bounds_relaxed'])

    def test_reject_unjustified_boundary_and_short_measurement(self):
        for kind in ('split','phase','stats','tick','excess'):
            phases,stats=self.sample(1,0)
            if kind=='split': phases['warmup_complete']['warmup_fusion_split']=2
            if kind=='phase': phases['measure_complete']['measure']-=1
            if kind=='stats': stats['simInsts']=[999999.0]
            if kind=='tick': phases['measure_complete']['tick']+=1
            if kind=='excess': phases['measure_complete']['terminal_fusion_excess']=1
            with self.assertRaises(ValueError): retirement_window(phases,1000000,1000000,stats)


if __name__=='__main__': unittest.main()
