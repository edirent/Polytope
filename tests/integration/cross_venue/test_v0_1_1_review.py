"""Real captured predicates and approval/readiness boundaries; no network."""
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT))
from research.cross_venue.review_v0_1_1 import rank_candidates, date


class ReviewTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.sample=json.loads((ROOT/'tests/fixtures/cross_venue/real_candidate_review_sample.json').read_text())

    def ranked(self):
        return rank_candidates(self.sample['candidates'],self.sample['markets'],date('2026-11-05T07:00:00Z'))

    def test_real_provider_conflict_hidden_by_same_event_title(self):
        rows=self.ranked()
        ai=next(r for r in rows if r['kalshi_market'].startswith('KXLLM'))
        self.assertIn('DIFFERENT_AI_COMPANY',ai['blockers'])
        self.assertIn('CONFLICTING_TIEBREAK_PAYOFF',ai['blockers'])
        self.assertEqual(ai['rule_compatibility_heuristic'],0)

    def test_real_short_bitcoin_quote_has_different_oracle(self):
        btc=next(r for r in self.ranked() if r['kalshi_market'].startswith('KXBTCD'))
        self.assertIn('DIFFERENT_PRICE_ORACLE',btc['blockers'])
        self.assertEqual(btc['review_disposition'],'CONTRADICTORY_OR_UNRELATED')

    def test_real_esports_single_game_is_not_season_champion(self):
        row=next(r for r in self.ranked() if r['kalshi_market'].startswith('KXESOCCER'))
        self.assertIn('DIFFERENT_EVENT_PREDICATE',row['blockers'])

    def test_triage_is_deterministic_and_never_approval(self):
        a=self.ranked();b=rank_candidates(list(reversed(self.sample['candidates'])),self.sample['markets'],date('2026-11-05T07:00:00Z'))
        self.assertEqual([r['candidate_pair_id'] for r in a],[r['candidate_pair_id'] for r in b])
        self.assertEqual(len(a),len(self.sample['candidates']))
        self.assertTrue(all(r['approval_status']=='PENDING_HUMAN_REVIEW' for r in a))
        self.assertTrue(a[0]['kalshi_market'].startswith('KXALIENS'))

    def test_24h_preflight_never_launches_empty_unapproved_campaign(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);(root/'approvals.json').write_text(json.dumps({'pairs':[]}))
            (root/'config.json').write_text(json.dumps({'approved_pairs':str(root/'approvals.json'),'require_explicit_revision_review':True}))
            env=dict(os.environ)
            for key in ['KALSHI_API_KEY_ID','KALSHI_PRIVATE_KEY_PATH','KALSHI_PRIVATE_KEY']:env.pop(key,None)
            process=subprocess.run([sys.executable,str(ROOT/'research/cross_venue/run_bounded_campaign.py'),
                '--config',str(root/'config.json'),'--hours','24','--require-research-ready',
                '--binary',str(root/'this_binary_does_not_exist'),'--out',str(root/'campaign')],env=env,capture_output=True,text=True)
            self.assertEqual(process.returncode,2,process.stderr)
            status=json.loads((root/'campaign/preflight.json').read_text())
            self.assertFalse(status['collection_started'])
            self.assertIn('NO_EXPLICITLY_APPROVED_PAIRS',status['reasons'])


if __name__=='__main__':unittest.main()
