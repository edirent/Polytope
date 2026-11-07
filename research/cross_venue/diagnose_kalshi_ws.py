"""One unsigned production handshake probe; no secrets, orders or retries.

This tests network/auth rejection only. A 401 is NOT authenticated L2 success.
Use the native shadow executable for an authorized signed subscription.
"""
import argparse
import asyncio
import json
import os
from datetime import datetime, timezone
from pathlib import Path


async def probe():
    import websockets
    endpoint='wss://external-api-ws.kalshi.com/trade-api/ws/v2'
    result={'endpoint':endpoint,'started_utc':datetime.now(timezone.utc).isoformat(),
            'probe':'SINGLE_UNSIGNED_HANDSHAKE_NETWORK_DIAGNOSTIC',
            'credential_presence':{k:bool(os.getenv(k)) for k in
               ['KALSHI_API_KEY_ID','KALSHI_PRIVATE_KEY_PATH','KALSHI_PRIVATE_KEY']},
            'authenticated_subscription_verified':False,'verified_live_realized_pnl':None,'live_orders_sent':0}
    try:
        async with websockets.connect(endpoint,open_timeout=12,close_timeout=2):
            result.update(status='UNSIGNED_HANDSHAKE_ACCEPTED_NOT_L2_VERIFIED',http_status=101)
    except Exception as exc:
        response=getattr(exc,'response',None)
        code=getattr(response,'status_code',getattr(exc,'status_code',None))
        result.update(status='AUTHENTICATION_REQUIRED' if code in (401,403) else 'NETWORK_OR_HANDSHAKE_ERROR',
                      http_status=code,error_type=type(exc).__name__)
    result['finished_utc']=datetime.now(timezone.utc).isoformat()
    return result


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args()
    if args.out.exists():raise ValueError('probe output must be new')
    result=asyncio.run(probe());args.out.parent.mkdir(parents=True,exist_ok=True)
    args.out.write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
