"""Bounded native workflow gates, with fake results and no network/credentials."""
import importlib.util
import json
import os
import tempfile
import types
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[3]
spec=importlib.util.spec_from_file_location('authenticated_workflow',ROOT/'research/cross_venue/authenticated_ws_workflow.py')
workflow=importlib.util.module_from_spec(spec);spec.loader.exec_module(workflow)


class WorkflowTests(unittest.TestCase):
    def exercise(self,results,retry_allowed):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);config=root/'config.json';config.write_text('{}')
            args=types.SimpleNamespace(out=root/'output',config=config,automatic=True,ready_prefix=None)
            durations=[];audits=iter(results)
            def native(command,**kwargs):
                if command[0]=='build/run_cross_venue_live_shadow':durations.append(int(command[command.index('--duration-sec')+1]))
                if command[0]=='build/cross_venue_feed_audit':
                    out=Path(command[command.index('--out')+1]);out.mkdir()
                    (out/'summary.json').write_text(json.dumps({'kalshi_snapshot_and_delta_observed':next(audits),'both_venues_any_asset_healthy_l2_sec':1}))
                return types.SimpleNamespace(returncode=0)
            def retry(path,seconds):
                self.assertEqual(path.name,'retry_smoke.request');self.assertEqual(seconds,600)
                if not retry_allowed:raise TimeoutError('synthetic bounded retry expiry')
            with patch.dict(os.environ,{'KALSHI_API_KEY_ID':'FAKE_TEST_VALUE'}),patch.object(workflow.subprocess,'run',side_effect=native),patch.object(workflow,'ready',side_effect=retry),patch('builtins.print'):
                try:workflow.run(args);error=None
                except (TimeoutError,ValueError) as caught:error=type(caught).__name__
            for path in args.out.rglob('*'):
                if path.is_file():self.assertNotIn('FAKE_TEST_VALUE',path.read_text())
            return durations,error

    def test_missing_real_delta_does_not_start_ten_minute_capture(self):
        self.assertEqual(self.exercise([False],False),([60],'TimeoutError'))

    def test_verified_smoke_starts_exactly_ten_minute_capture(self):
        self.assertEqual(self.exercise([True,True],False),([60,600],None))

    def test_one_bounded_operator_retry_reuses_environment_without_persistence(self):
        self.assertEqual(self.exercise([False,True,True],True),([60,60,600],None))


if __name__=='__main__':unittest.main()
