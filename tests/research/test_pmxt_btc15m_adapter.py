"""Offline checks for timestamp, missing-depth and public-volume semantics."""
import importlib.util
import json
import hashlib
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq

ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('pmxt_adapter',ROOT/'research/pmxt_btc15m_backtest.py')
adapter=importlib.util.module_from_spec(spec);spec.loader.exec_module(adapter)


class AdapterTest(unittest.TestCase):
    def test_receive_clock_missing_depth_and_fractional_volume(self):
        market={'slug':'btc-updown-15m-test','start_ms':100000,'expiry_ms':1000000,
                'up_token':'up','down_token':'down','winner':'Up'}
        def row(token,kind,source,recv,**kw):
            return {'asset_id':token,'event_type':kind,'timestamp':source,'timestamp_received':recv,
                'bids':None,'asks':None,'price':None,'size':None,'side':None,
                'best_bid':None,'best_ask':None,'new_tick_size':None,'transaction_hash':None,**kw}
        rows=[row(t,'book',90000,90050,bids='[[0.49,17]]',asks='[[0.51,19]]') for t in ('up','down')]
        rows += [row('down','price_change',100000,100020,price=.49,size=17.,side='BUY',best_bid=.49,best_ask=.51),
                 row('up','price_change',100000,100001,price=.55,size=1.,side='SELL',best_bid=.49,best_ask=.53),
                 row('up','price_change',100060,100075,price=.53,size=7.,side='SELL',best_bid=.49,best_ask=.53)]
        trade=row('up','last_trade_price',100200,100260,price=.53,size=2.75,side='BUY',transaction_hash='tx')
        rows += [trade,trade.copy()]
        rows += [row('up','last_trade_price',1000013,1000107,price=.53,size=5.6,side='BUY',transaction_hash='post')]
        # The Parquet rows intentionally disagree with receive ordering.
        table=pa.Table.from_pylist(rows).set_column(2,'timestamp',pa.array([r['timestamp'] for r in rows],type=pa.timestamp('ms',tz='UTC')))
        table=table.set_column(3,'timestamp_received',pa.array([r['timestamp_received'] for r in rows],type=pa.timestamp('ms',tz='UTC')))
        bt=np.arange(35000,105000,100,dtype=np.int64)
        btc=(bt,bt+20,70000*np.exp(.0002*np.sin(bt/310)),np.ones(len(bt)),np.zeros(len(bt),dtype=bool))
        with tempfile.TemporaryDirectory() as tmp,patch.object(adapter,'_load_btc',return_value=btc):
            out=Path(tmp);dest=out/'market_rows'/market['slug'];dest.mkdir(parents=True)
            pq.write_table(table,dest/'fixture.parquet')
            audit=adapter.normalize_market((str(out),market));selected={};public=[]
            for line in (out/'normalized'/market['slug']/'events.jsonl').read_text().splitlines():
                e=json.loads(line)
                if e['type']=='books' and e['recv_ms'] in (100050,100100):selected[e['recv_ms']]=e
                if e['type']=='public_trade':public.append(e)
            self.assertFalse(selected[100050]['up']['usable'])
            self.assertTrue(selected[100100]['up']['usable'])
            self.assertEqual(selected[100100]['up']['recv_ms'],100075)
            self.assertEqual(selected[100100]['down']['recv_ms'],100020)
            self.assertEqual(selected[100100]['up']['asks'][0],[530000,7.])
            self.assertEqual(len(public),1)
            self.assertEqual(public[0]['quantity_lots'],2)
            self.assertEqual((public[0]['event_ms'],public[0]['recv_ms']),(100200,100260))
            self.assertEqual(audit['duplicate_public_events_dropped'],1)
            self.assertAlmostEqual(audit['fractional_volume_floor_lost'],.75)
            adapter.save(out/'markets.json',[market])
            features=out/'normalized'/market['slug']/'features.npz'
            frozen_hash=hashlib.sha256(features.read_bytes()).hexdigest()
            adapter.repair_expiry_tail(out,1);adapter.repair_expiry_tail(out,1)
            events=[json.loads(s) for s in (features.with_name('events.jsonl')).read_text().splitlines()]
            tail=[e for e in events if e['type']=='public_trade' and e['event_ms']>=market['expiry_ms']]
            self.assertEqual(len(tail),1)
            self.assertEqual((tail[0]['event_ms'],tail[0]['recv_ms']),(1000013,1000107))
            self.assertEqual(tail[0]['quantity_lots'],5)
            self.assertEqual(hashlib.sha256(features.read_bytes()).hexdigest(),frozen_hash)

    def test_independent_pm_only_features_with_missing_external_history(self):
        times=np.arange(100000,120001,50,dtype=np.int64)
        tops=np.tile(np.array([[490000,510000],[490000,510000]]),(len(times),1,1))
        valid=np.ones(len(times),dtype=bool)
        empty=(np.array([],dtype=np.int64),np.array([],dtype=np.int64),np.array([]),np.array([]),np.array([],dtype=bool))
        pm_x,pm_ready=adapter.build_features(times,tops,valid,empty,1000000,pm_only=True)
        _,external_ready=adapter.build_features(times,tops,valid,empty,1000000)
        self.assertTrue(np.any(pm_ready));self.assertFalse(np.any(external_ready))
        self.assertTrue(np.all(pm_x[:,:,:5]==0))


if __name__=='__main__':unittest.main()
