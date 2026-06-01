#!/usr/bin/env python3
"""WS-only research data collector.

This script is for research data capture only. It does not trade, approve risk,
or write into the runtime state store. Every payload is wrapped in a normalized
envelope with a local receive timestamp so downstream research can measure
arrival order, source lag, and spread half-life.
"""

from __future__ import annotations

import argparse
import collections
import dataclasses
import json
import os
from pathlib import Path
import queue
import threading
import time
from typing import Any

import websocket


def now_ns() -> int:
    return time.time_ns()


def utc_stamp() -> str:
    return time.strftime("%Y%m%d_%H%M%S", time.gmtime())


def load_json(path: str) -> dict[str, Any]:
    return json.loads(Path(path).read_text(encoding="utf-8"))


def json_dumps(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"))


def maybe_json(text: str) -> Any:
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        return {"raw": text}


def infer_event_type(source: str, payload: Any) -> str:
    if isinstance(payload, dict):
        if source == "deribit":
            if payload.get("method") == "subscription":
                return str(payload.get("params", {}).get("channel", "subscription"))
            if "id" in payload and "result" in payload:
                return "rpc_result"
            return str(payload.get("method") or payload.get("error") or "message")
        if source.startswith("binance"):
            return str(payload.get("e") or payload.get("stream") or "message")
        if source == "polymarket":
            return str(payload.get("event_type") or payload.get("type") or "message")
        if source == "coinbase":
            return str(payload.get("channel") or payload.get("type") or "message")
        if source == "kraken":
            return str(payload.get("channel") or payload.get("method") or payload.get("type") or "message")
    return "message"


def extract_exchange_ts(source: str, payload: Any) -> int | None:
    if not isinstance(payload, dict):
        return None
    candidates: list[Any] = []
    if source == "deribit":
        candidates.append(payload.get("params", {}).get("data", {}).get("timestamp"))
        candidates.append(payload.get("result", {}).get("timestamp") if isinstance(payload.get("result"), dict) else None)
    elif source.startswith("binance"):
        candidates.extend([payload.get("E"), payload.get("T"), payload.get("data", {}).get("E") if isinstance(payload.get("data"), dict) else None])
    elif source == "polymarket":
        candidates.extend([payload.get("timestamp"), payload.get("ts")])
    elif source == "coinbase":
        candidates.extend([payload.get("timestamp"), payload.get("time")])
        if isinstance(payload.get("events"), list):
            for event in payload["events"]:
                if isinstance(event, dict):
                    candidates.append(event.get("timestamp"))
    elif source == "kraken":
        candidates.extend([payload.get("time_in"), payload.get("time_out")])
        if isinstance(payload.get("data"), list):
            for item in payload["data"]:
                if isinstance(item, dict):
                    candidates.append(item.get("timestamp"))
    for candidate in candidates:
        if candidate is None:
            continue
        try:
            if isinstance(candidate, str) and ("T" in candidate or "-" in candidate):
                import datetime as _dt

                text = candidate.replace("Z", "+00:00")
                parsed_dt = _dt.datetime.fromisoformat(text)
                if parsed_dt.tzinfo is None:
                    parsed_dt = parsed_dt.replace(tzinfo=_dt.timezone.utc)
                return int(parsed_dt.timestamp() * 1_000_000_000)
            parsed = int(float(candidate))
            # Most exchange timestamps are ms; Polymarket may send seconds or ms.
            if parsed < 10_000_000_000:
                return parsed * 1_000_000_000
            if parsed < 10_000_000_000_000:
                return parsed * 1_000_000
            return parsed
        except (TypeError, ValueError):
            continue
    return None


@dataclasses.dataclass
class WsHandle:
    name: str
    app: websocket.WebSocketApp
    thread: threading.Thread


class Collector:
    def __init__(self, output_dir: Path) -> None:
        self.output_dir = output_dir
        self.queue: queue.Queue[dict[str, Any]] = queue.Queue()
        self.stop = threading.Event()
        self.handles: list[WsHandle] = []
        self.counts: collections.Counter[str] = collections.Counter()
        self.errors: list[dict[str, Any]] = []

    def emit(self, envelope: dict[str, Any]) -> None:
        self.queue.put(envelope)

    def emit_payload(self, *, source: str, channel: str, payload: Any) -> None:
        recv = now_ns()
        exchange_ts = extract_exchange_ts(source, payload)
        envelope = {
            "recv_ts_ns": recv,
            "source": source,
            "channel": channel,
            "event_type": infer_event_type(source, payload),
            "exchange_ts_ns": exchange_ts,
            "lag_ns": recv - exchange_ts if exchange_ts is not None else None,
            "payload": payload,
        }
        self.emit(envelope)

    def add_private_stub(self, name: str, config: dict[str, Any]) -> None:
        if not config.get("enabled"):
            return
        env_name = str(config.get("credential_env") or "").strip()
        present = bool(env_name and os.getenv(env_name))
        self.emit_payload(
            source=name,
            channel="private_adapter",
            payload={
                "status": "credentials_present" if present else "missing_credentials",
                "credential_env": env_name,
                "note": config.get("note", "Private source adapter placeholder"),
            },
        )

    def add_ws(
        self,
        *,
        name: str,
        url: str,
        source: str,
        channel: str,
        on_open_send: list[dict[str, Any]] | None = None,
    ) -> None:
        sends = on_open_send or []

        def on_open(ws: websocket.WebSocketApp) -> None:
            self.emit_payload(source=source, channel=channel, payload={"status": "ws_open", "url": url})
            for msg in sends:
                ws.send(json_dumps(msg))
                self.emit_payload(source=source, channel=channel, payload={"status": "ws_send", "message": msg})

        def on_message(_: websocket.WebSocketApp, message: str) -> None:
            self.emit_payload(source=source, channel=channel, payload=maybe_json(message))

        def on_error(_: websocket.WebSocketApp, error: Exception) -> None:
            payload = {"status": "ws_error", "error": str(error), "url": url}
            self.errors.append({"source": source, "channel": channel, "error": str(error)})
            self.emit_payload(source=source, channel=channel, payload=payload)

        def on_close(_: websocket.WebSocketApp, status_code: Any, close_msg: Any) -> None:
            self.emit_payload(
                source=source,
                channel=channel,
                payload={"status": "ws_close", "code": status_code, "message": close_msg},
            )

        app = websocket.WebSocketApp(
            url,
            on_open=on_open,
            on_message=on_message,
            on_error=on_error,
            on_close=on_close,
        )
        thread = threading.Thread(
            target=lambda: app.run_forever(ping_interval=20, ping_timeout=10),
            name=f"ws-{name}",
            daemon=True,
        )
        self.handles.append(WsHandle(name=name, app=app, thread=thread))

    def configure_deribit(self, config: dict[str, Any]) -> None:
        if not config.get("enabled"):
            return
        url = str(config.get("url") or "wss://www.deribit.com/ws/api/v2")
        messages: list[dict[str, Any]] = []
        subscriptions = [str(x) for x in config.get("subscriptions", [])]
        if subscriptions:
            messages.append(
                {
                    "jsonrpc": "2.0",
                    "id": 1,
                    "method": "public/subscribe",
                    "params": {"channels": subscriptions},
                }
            )
        next_id = 100
        for req in config.get("requests", []):
            if not isinstance(req, dict):
                continue
            messages.append(
                {
                    "jsonrpc": "2.0",
                    "id": next_id,
                    "method": req.get("method"),
                    "params": req.get("params", {}),
                }
            )
            next_id += 1
        self.add_ws(
            name="deribit",
            url=url,
            source="deribit",
            channel=";".join(subscriptions) if subscriptions else "rpc",
            on_open_send=messages,
        )

    def configure_binance(self, config: dict[str, Any]) -> None:
        if not config.get("enabled"):
            return
        spot_streams = [str(x).lower() for x in config.get("spot_streams", [])]
        futures_streams = [str(x).lower() for x in config.get("futures_streams", [])]
        if spot_streams:
            base = str(config.get("spot_url") or "wss://stream.binance.com:9443").rstrip("/")
            path = "/stream?streams=" + "/".join(spot_streams) if len(spot_streams) > 1 else "/ws/" + spot_streams[0]
            self.add_ws(name="binance_spot", url=base + path, source="binance_spot", channel=";".join(spot_streams))
        if futures_streams:
            base = str(config.get("futures_url") or "wss://fstream.binance.com").rstrip("/")
            path = "/stream?streams=" + "/".join(futures_streams) if len(futures_streams) > 1 else "/ws/" + futures_streams[0]
            self.add_ws(name="binance_futures", url=base + path, source="binance_futures", channel=";".join(futures_streams))

    def configure_coinbase(self, config: dict[str, Any]) -> None:
        if not config.get("enabled"):
            return
        url = str(config.get("url") or "wss://advanced-trade-ws.coinbase.com")
        product_ids = [str(x) for x in config.get("product_ids", []) if str(x)]
        channels = [str(x) for x in config.get("channels", ["ticker"]) if str(x)]
        if not product_ids:
            self.emit_payload(source="coinbase", channel="market", payload={"status": "no_product_ids"})
            return
        messages = [
            {"type": "subscribe", "product_ids": product_ids, "channel": channel}
            for channel in channels
        ]
        self.add_ws(
            name="coinbase",
            url=url,
            source="coinbase",
            channel=";".join(channels),
            on_open_send=messages,
        )

    def configure_kraken(self, config: dict[str, Any]) -> None:
        if not config.get("enabled"):
            return
        url = str(config.get("url") or "wss://ws.kraken.com/v2")
        symbols = [str(x) for x in config.get("symbols", []) if str(x)]
        channels = [str(x) for x in config.get("channels", ["ticker"]) if str(x)]
        if not symbols:
            self.emit_payload(source="kraken", channel="market", payload={"status": "no_symbols"})
            return
        messages: list[dict[str, Any]] = []
        req_id = 1
        for channel in channels:
            params: dict[str, Any] = {"channel": channel, "symbol": symbols}
            if channel == "ticker":
                params["event_trigger"] = "bbo"
                params["snapshot"] = True
            if channel == "book":
                params["depth"] = int(config.get("book_depth") or 10)
                params["snapshot"] = True
            messages.append({"method": "subscribe", "params": params, "req_id": req_id})
            req_id += 1
        self.add_ws(
            name="kraken",
            url=url,
            source="kraken",
            channel=";".join(channels),
            on_open_send=messages,
        )

    def configure_polymarket(self, config: dict[str, Any]) -> None:
        if not config.get("enabled"):
            return
        asset_ids = [str(x) for x in config.get("asset_ids", []) if str(x)]
        if not asset_ids:
            self.emit_payload(source="polymarket", channel="market", payload={"status": "no_asset_ids"})
            return
        url = str(config.get("url") or "wss://ws-subscriptions-clob.polymarket.com/ws/market")
        subscribe = {
            "type": "market",
            "assets_ids": asset_ids,
            "custom_feature_enabled": bool(config.get("custom_feature_enabled", True)),
        }
        self.add_ws(
            name="polymarket",
            url=url,
            source="polymarket",
            channel="market",
            on_open_send=[subscribe],
        )

    def configure(self, config: dict[str, Any]) -> None:
        self.configure_deribit(config.get("deribit", {}))
        self.configure_binance(config.get("binance", {}))
        self.configure_coinbase(config.get("coinbase", {}))
        self.configure_kraken(config.get("kraken", {}))
        self.configure_polymarket(config.get("polymarket", {}))
        for name in ("pinnacle", "betfair", "sportsradar", "opta"):
            self.add_private_stub(name, config.get(name, {}))

    def run(self, duration_s: int) -> dict[str, Any]:
        self.output_dir.mkdir(parents=True, exist_ok=True)
        events_path = self.output_dir / "events.jsonl"
        diagnostics_path = self.output_dir / "diagnostics.json"
        for handle in self.handles:
            handle.thread.start()

        deadline = time.monotonic() + duration_s
        with events_path.open("w", encoding="utf-8") as out:
            while time.monotonic() < deadline or not self.queue.empty():
                timeout = max(0.05, min(0.5, deadline - time.monotonic()))
                try:
                    envelope = self.queue.get(timeout=timeout)
                except queue.Empty:
                    continue
                key = f"{envelope.get('source')}:{envelope.get('event_type')}"
                self.counts[key] += 1
                out.write(json_dumps(envelope) + "\n")

        for handle in self.handles:
            try:
                handle.app.close()
            except Exception:  # noqa: BLE001
                pass
        time.sleep(0.2)
        while not self.queue.empty():
            envelope = self.queue.get_nowait()
            key = f"{envelope.get('source')}:{envelope.get('event_type')}"
            self.counts[key] += 1
            with events_path.open("a", encoding="utf-8") as out:
                out.write(json_dumps(envelope) + "\n")

        summary = {
            "duration_s": duration_s,
            "event_count": sum(self.counts.values()),
            "counts": dict(sorted(self.counts.items())),
            "errors": self.errors,
            "events_path": str(events_path),
        }
        diagnostics_path.write_text(json_dumps(summary) + "\n", encoding="utf-8")
        self.write_report(summary)
        return summary

    def write_report(self, summary: dict[str, Any]) -> None:
        lines = [
            "# WS Research Data Pipeline Report",
            "",
            "This is a read-only capture report. No trading endpoints were used.",
            "",
            f"- Duration: {summary['duration_s']} seconds",
            f"- Events captured: {summary['event_count']}",
            f"- Event file: `{summary['events_path']}`",
            "",
            "## Counts",
            "",
            "| Source/Event | Count |",
            "|---|---:|",
        ]
        for key, count in summary["counts"].items():
            lines.append(f"| {key} | {count} |")
        lines += [
            "",
            "## Private Source Policy",
            "",
            "Pinnacle, Betfair, Sportsradar, and Opta require licensed credentials.",
            "This collector records missing/present credential status but does not fake data.",
            "",
            "## Timestamp Policy",
            "",
            "- `recv_ts_ns` is local capture time.",
            "- `exchange_ts_ns` is parsed from the source payload when present.",
            "- `lag_ns` is computed only when both timestamps exist.",
            "",
        ]
        if summary["errors"]:
            lines += ["## Errors", ""]
            for error in summary["errors"]:
                lines.append(f"- `{error['source']}` `{error['channel']}`: {error['error']}")
            lines.append("")
        (self.output_dir / "report.md").write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", default="research/ws_research_config.example.json")
    parser.add_argument("--duration-seconds", type=int, default=30)
    parser.add_argument("--out-dir", default="")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    config = load_json(args.config)
    output_dir = Path(args.out_dir or f"research/runs/ws_data_pipeline_{utc_stamp()}")
    if args.dry_run:
        print("ws_data_pipeline_dry_run:")
        print(f"  config: {args.config}")
        print(f"  out_dir: {output_dir}")
        for private in ("pinnacle", "betfair", "sportsradar", "opta"):
            c = config.get(private, {})
            env_name = c.get("credential_env")
            if c.get("enabled"):
                print(f"  {private}: enabled credential_present={bool(env_name and os.getenv(env_name))}")
        return 0

    collector = Collector(output_dir)
    collector.configure(config)
    summary = collector.run(int(config.get("duration_seconds") or args.duration_seconds))
    print("ws_data_pipeline:")
    print(f"  out_dir: {output_dir}")
    print(f"  event_count: {summary['event_count']}")
    print(f"  report: {output_dir / 'report.md'}")
    for key, count in summary["counts"].items():
        print(f"  {key}: {count}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
