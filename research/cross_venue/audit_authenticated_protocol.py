"""Independent stdlib integrity/sequence/clock audit of saved native L2 inputs.

No network, signing key, Key ID or trading access. This is not a second book
engine: reconstruction and execution remain native; this checks raw contracts.
"""
import argparse
import hashlib
import json
from collections import Counter
from decimal import Decimal
from pathlib import Path


def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()


def audit(run,native_audit):
    manifest=json.loads((run/'capture_manifest.json').read_text());records=[];hashes=0
    for entry in manifest['raw_inventory']:
        name=entry['file']
        if Path(name).name!=name:raise ValueError('unsafe raw path')
        path=run/name
        if sha(path)!=entry['sha256']:raise ValueError('raw file hash mismatch')
        hashes+=1;offset=0
        with path.open('rb') as stream:
            for line in stream:
                row=json.loads(line)
                if row['raw_file']!=name or row['raw_log_offset']!=offset:raise ValueError('raw provenance offset mismatch')
                payload=row['raw_payload']
                if hashlib.sha256(payload.encode()).hexdigest()!=row['raw_hash']:raise ValueError('raw payload hash mismatch')
                if any(x in payload for x in ['PRIVATE KEY-----','KALSHI-ACCESS-SIGNATURE','KALSHI-ACCESS-KEY']):raise ValueError('authentication material in raw input')
                records.append(row);offset+=len(line)
    records.sort(key=lambda row:row['record_sequence']);last=0;sequences={};clocks={};snapshots={};counts=Counter();clock_decreases=0;negative=0;book_ages=0
    for ordinal,row in enumerate(records,1):
        now=row['availability_monotonic_ns']
        if row['record_sequence']!=ordinal or now<last:raise ValueError('local arrival gap/order mismatch')
        if row.get('callback_recv_monotonic_ns',now)>now:raise ValueError('callback occurs after recorded availability')
        last=now
        if row['venue']!='kalshi' or row['transport']!='ws' or row['classification']!='market_data':continue
        packet=json.loads(row['raw_payload']);kind=packet['type']
        if kind not in ['orderbook_snapshot','orderbook_delta']:continue
        counts[kind]+=1;sid,seq=packet['sid'],packet['seq'];epoch=row['connection_epoch']
        if sid<=0 or seq<=0:raise ValueError('missing sid/seq')
        stream=(epoch,sid)
        if stream in sequences and seq!=sequences[stream]+1:raise ValueError('subscription sequence gap/duplicate')
        if stream not in sequences and kind!='orderbook_snapshot':raise ValueError('new subscription without snapshot')
        sequences[stream]=seq;body=packet['msg'];key=(epoch,body['market_ticker']);clock=body['clock']
        if not isinstance(clock,int) or clock<=0:raise ValueError('exchange clock absent or invalid')
        if key in clocks and clock<clocks[key]:clock_decreases+=1
        clocks[key]=clock
        if kind=='orderbook_snapshot':snapshots.setdefault(key,row['record_sequence'])
        elif key not in snapshots:raise ValueError('market delta before its own snapshot')
        else:
            price=Decimal(body['price_dollars']);delta=Decimal(body['delta_fp'])
            if not 0<price<1 or price*1000000%1 or delta*1000000%1:raise ValueError('unsupported fixed-point delta precision')
            counts['delta_after_own_market_snapshot']+=1
        source=packet.get('sending_ts_ms')
        if source is not None:
            age=Decimal(row.get('callback_recv_wall_ns',row['recv_wall_ns']))/1000000-Decimal(source)
            negative+=age<0;book_ages+=1
    native=json.loads((native_audit/'summary.json').read_text());wire=native['wire_protocol']['kalshi']
    if wire['snapshot_packets']!=counts['orderbook_snapshot'] or wire['delta_packets']!=counts['orderbook_delta']:raise ValueError('independent/native packet counts differ')
    if wire['clock_regressions_within_session_market']!=clock_decreases:raise ValueError('independent/native clock counts differ')
    if native['raw_records_verified']!=len(records):raise ValueError('native raw record count differs')
    return {'status':'INDEPENDENT_PROTOCOL_AUDIT_PASSED','source_run':str(run),'input_evidence_level':manifest['evidence_level'],'raw_records':len(records),
        'raw_files_sha256_verified':hashes,'kalshi_snapshots':counts['orderbook_snapshot'],'kalshi_deltas':counts['orderbook_delta'],
        'deltas_after_own_market_snapshot':counts['delta_after_own_market_snapshot'],'connection_subscription_streams':len(sequences),
        'market_epoch_snapshot_states':len(snapshots),'clock_decreases':clock_decreases,
        'timestamped_book_packets':book_ages,'negative_source_age_samples_retained':negative,
        'local_record_sequence_gaps':0,'subscription_sequence_gap_events':0,'authentication_marker_matches':0,
        'verified_live_realized_pnl':None,'scope':'Raw integrity and wire contracts; native book reconstruction, human rule approval and private execution receipts are separate.'}


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--run-dir',type=Path,required=True);p.add_argument('--native-audit-dir',type=Path,required=True)
    a=p.parse_args();print(json.dumps(audit(a.run_dir,a.native_audit_dir),indent=2))
