#!/usr/bin/env python3
"""Read-only crypto price-target market selector for Polymarket.

The selector fetches public Gamma market metadata and ranks markets that can be
anchored to external crypto oracles. An optional OpenRouter LLM pass can
classify borderline market text, but the deterministic gates remain the source
of truth for runtime eligibility.
"""

from __future__ import annotations

import argparse
import csv
import dataclasses
import datetime as dt
import json
import math
import os
import re
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any


GAMMA_MARKETS_ENDPOINT = "https://gamma-api.polymarket.com/markets"
OPENROUTER_ENDPOINT = "https://openrouter.ai/api/v1/chat/completions"
DEFAULT_MODEL = "nvidia/llama-3.3-nemotron-super-49b-v1.5"


ASSET_PATTERNS = {
    "BTC": re.compile(r"\b(bitcoin|btc)\b", re.IGNORECASE),
    "ETH": re.compile(r"\b(ethereum|ether|eth)\b", re.IGNORECASE),
    "SOL": re.compile(r"\b(solana|sol)\b", re.IGNORECASE),
}

PRICE_TARGET_RE = re.compile(
    r"(\$[0-9]|reach|hit|dip to|drop below|below|above|all time high|ath|"
    r"up or down)",
    re.IGNORECASE,
)
NON_PRICE_RE = re.compile(
    r"(satoshi|reserve|tax|unban|country buy|outperform|dominance|"
    r"bip-|sha-256|etf approved|ipo|airdrop|fdv|floor price|nyse choose|"
    r"gta|nothing ever happens)",
    re.IGNORECASE,
)
UPDOWN_5M_RE = re.compile(r"\bup or down\b.*\b[0-9]+:[0-9]{2}", re.IGNORECASE)


def now_utc() -> dt.datetime:
    return dt.datetime.now(dt.timezone.utc)


def http_json(
    url: str,
    *,
    method: str = "GET",
    headers: dict[str, str] | None = None,
    body: dict[str, Any] | None = None,
    timeout_s: int = 30,
) -> Any:
    encoded_body = None
    request_headers = {
        "Accept": "application/json",
        "User-Agent": "Mozilla/5.0 PolytopeCryptoSelector/0.1",
    }
    if headers:
        request_headers.update(headers)
    if body is not None:
        encoded_body = json.dumps(body, separators=(",", ":")).encode("utf-8")
        request_headers["Content-Type"] = "application/json"
    request = urllib.request.Request(
        url,
        data=encoded_body,
        headers=request_headers,
        method=method,
    )
    with urllib.request.urlopen(request, timeout=timeout_s) as response:
        return json.loads(response.read().decode("utf-8"))


def parse_json_array(value: Any) -> list[Any]:
    if isinstance(value, list):
        return value
    if isinstance(value, str) and value:
        try:
            parsed = json.loads(value)
            return parsed if isinstance(parsed, list) else []
        except json.JSONDecodeError:
            return []
    return []


def number(value: Any, default: float = 0.0) -> float:
    if value is None or value == "":
        return default
    try:
        parsed = float(value)
        if math.isnan(parsed) or math.isinf(parsed):
            return default
        return parsed
    except (TypeError, ValueError):
        return default


def end_datetime(market: dict[str, Any]) -> dt.datetime | None:
    value = market.get("endDateIso") or market.get("endDate")
    if not value:
        return None
    try:
        parsed = dt.datetime.fromisoformat(str(value).replace("Z", "+00:00"))
    except ValueError:
        return None
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=dt.timezone.utc)
    return parsed.astimezone(dt.timezone.utc)


def fetch_gamma_markets(limit: int, page_size: int, pause_s: float) -> list[dict[str, Any]]:
    markets: list[dict[str, Any]] = []
    offset = 0
    while len(markets) < limit:
        batch_size = min(page_size, limit - len(markets))
        query = urllib.parse.urlencode(
            {
                "active": "true",
                "closed": "false",
                "archived": "false",
                "limit": batch_size,
                "offset": offset,
            }
        )
        data = http_json(f"{GAMMA_MARKETS_ENDPOINT}?{query}")
        if not isinstance(data, list) or not data:
            break
        markets.extend([item for item in data if isinstance(item, dict)])
        offset += len(data)
        if len(data) < batch_size:
            break
        if pause_s > 0:
            time.sleep(pause_s)
    return markets[:limit]


def market_text(market: dict[str, Any]) -> str:
    return " ".join(
        str(market.get(key) or "")
        for key in ("question", "slug", "description")
    )


def detect_asset(text: str) -> str:
    for asset, pattern in ASSET_PATTERNS.items():
        if pattern.search(text):
            return asset
    return ""


def detect_category(text: str, asset: str) -> str:
    lowered = text.lower()
    if UPDOWN_5M_RE.search(text):
        return "intraday_5m"
    if "etf" in lowered:
        return "etf_related"
    if "all time high" in lowered or "ath" in lowered:
        return f"{asset.lower()}_ath"
    if any(word in lowered for word in ("dip to", "drop below", "below")):
        return f"{asset.lower()}_downside_target"
    if any(word in lowered for word in ("reach", "hit", "above", "$")):
        return f"{asset.lower()}_upside_target"
    return "other_crypto"


def horizon_bucket(days: float | None) -> str:
    if days is None:
        return "unknown"
    if days <= 1:
        return "intraday"
    if days <= 10:
        return "daily_weekly"
    if days <= 45:
        return "monthly"
    if days <= 120:
        return "quarterly"
    return "long_dated"


def is_orderbook_ready(market: dict[str, Any]) -> bool:
    outcomes = parse_json_array(market.get("outcomes"))
    tokens = parse_json_array(market.get("clobTokenIds"))
    return (
        bool(market.get("active"))
        and not bool(market.get("closed"))
        and not bool(market.get("archived"))
        and market.get("enableOrderBook", True) is not False
        and market.get("acceptingOrders") is not False
        and len(outcomes) >= 2
        and len(tokens) >= 2
        and number(market.get("bestBid")) > 0
        and number(market.get("bestAsk")) > 0
        and number(market.get("spread")) > 0
    )


@dataclasses.dataclass
class Candidate:
    rank: int
    score: float
    asset: str
    category: str
    horizon: str
    question: str
    slug: str
    end: str
    days_to_end: float | None
    liquidity: float
    volume24h: float
    spread: float
    best_bid: float
    best_ask: float
    yes_token_id: str
    no_token_id: str
    oracle: str
    llm_class: str
    notes: list[str]


def score_market(market: dict[str, Any], as_of: dt.datetime) -> Candidate | None:
    text = market_text(market)
    asset = detect_asset(text)
    if not asset:
        return None
    if not PRICE_TARGET_RE.search(text):
        return None
    if NON_PRICE_RE.search(text):
        return None
    if not is_orderbook_ready(market):
        return None

    question = str(market.get("question") or "")
    slug = str(market.get("slug") or "")
    end = end_datetime(market)
    days = None if end is None else (end - as_of).total_seconds() / 86400.0
    tokens = [str(x) for x in parse_json_array(market.get("clobTokenIds"))]
    spread = number(market.get("spread"))
    liquidity = number(market.get("liquidityNum") or market.get("liquidity"))
    volume24h = number(market.get("volume24hr"))
    bid = number(market.get("bestBid"))
    ask = number(market.get("bestAsk"))
    mid = (bid + ask) / 2.0
    category = detect_category(text, asset)
    horizon = horizon_bucket(days)

    notes: list[str] = []
    score = 0.0
    score += min(35.0, math.log10(max(liquidity, 1.0)) * 7.0)
    score += min(30.0, math.log10(max(volume24h, 1.0)) * 6.0)
    score += max(0.0, 20.0 - spread * 1000.0)
    if 0.03 <= mid <= 0.97:
        score += 8.0
    else:
        score -= 20.0
        notes.append("near-terminal price")
    if horizon in {"daily_weekly", "monthly", "quarterly"}:
        score += 10.0
    elif horizon == "long_dated":
        score -= 4.0
    elif horizon == "intraday":
        score -= 15.0
        notes.append("intraday market")
    if UPDOWN_5M_RE.search(text):
        score -= 45.0
        notes.append("5m option excluded from primary pool")
    if spread <= 0.02:
        notes.append("spread <= 2c")
    if volume24h >= 10_000:
        notes.append("active 24h volume")
    if liquidity >= 50_000:
        notes.append("deep liquidity")

    oracle = {
        "BTC": "Binance BTCUSDT + Deribit BTC IV",
        "ETH": "Binance ETHUSDT + Deribit ETH IV",
        "SOL": "Binance SOLUSDT + realized/Deribit proxy vol",
    }[asset]
    return Candidate(
        rank=0,
        score=score,
        asset=asset,
        category=category,
        horizon=horizon,
        question=question,
        slug=slug,
        end=(end.isoformat() if end else ""),
        days_to_end=days,
        liquidity=liquidity,
        volume24h=volume24h,
        spread=spread,
        best_bid=bid,
        best_ask=ask,
        yes_token_id=tokens[0],
        no_token_id=tokens[1],
        oracle=oracle,
        llm_class="not_run",
        notes=notes,
    )


def build_llm_prompt(candidates: list[Candidate]) -> str:
    payload = [
        {
            "slug": item.slug,
            "question": item.question,
            "asset": item.asset,
            "category": item.category,
            "horizon": item.horizon,
            "spread": item.spread,
            "liquidity": item.liquidity,
            "volume24h": item.volume24h,
        }
        for item in candidates
    ]
    return (
        "Classify these Polymarket crypto markets for whether their fair value "
        "can be driven by public external crypto oracles. Return JSON only: "
        "{\"markets\":[{\"slug\":\"...\",\"class\":\"pure_price_target|"
        "oracle_plus_headline|exclude\",\"reason\":\"short\"}]}. "
        "pure_price_target means BTC/ETH/SOL price threshold, ATH, or dip/reach "
        "market resolvable from public price data. Exclude ETF decisions, policy, "
        "company, floor-price, airdrop, FDV, 5-minute up/down, or insider/headline "
        "markets.\n\n"
        + json.dumps({"markets": payload}, ensure_ascii=False)
    )


def apply_llm(candidates: list[Candidate], model: str, max_tokens: int) -> str:
    api_key = os.getenv("OPENROUTER_API_KEY")
    if not api_key:
        return "missing_api_key"
    prompt = build_llm_prompt(candidates[:80])
    body = {
        "model": model,
        "max_tokens": max_tokens,
        "temperature": 0,
        "messages": [
            {"role": "system", "content": "Return strict JSON only."},
            {"role": "user", "content": prompt},
        ],
    }
    try:
        response = http_json(
            OPENROUTER_ENDPOINT,
            method="POST",
            headers={"Authorization": f"Bearer {api_key}"},
            body=body,
            timeout_s=60,
        )
    except urllib.error.HTTPError as exc:
        return f"http_{exc.code}"
    except Exception as exc:  # noqa: BLE001
        return f"error:{exc}"

    try:
        content = response["choices"][0]["message"]["content"]
        if isinstance(content, list):
            content = "".join(
                part.get("text", "") if isinstance(part, dict) else str(part)
                for part in content
            )
        text = str(content).strip()
        text = text[text.find("{") : text.rfind("}") + 1]
        parsed = json.loads(text)
        by_slug = {item.slug: item for item in candidates}
        for row in parsed.get("markets", []):
            if not isinstance(row, dict):
                continue
            item = by_slug.get(str(row.get("slug") or ""))
            if item:
                item.llm_class = str(row.get("class") or "unknown")
                reason = str(row.get("reason") or "")
                if reason:
                    item.notes.append("llm: " + reason)
        return "ok"
    except Exception as exc:  # noqa: BLE001
        return f"parse_error:{exc}"


def dedupe_candidates(candidates: list[Candidate]) -> list[Candidate]:
    by_key: dict[str, Candidate] = {}
    for item in candidates:
        key = item.slug or item.question.lower().strip()
        existing = by_key.get(key)
        if existing is None or item.score > existing.score:
            by_key[key] = item
    return list(by_key.values())


def write_outputs(out_dir: Path, candidates: list[Candidate], metadata: dict[str, Any]) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    data = {
        "metadata": metadata,
        "candidates": [dataclasses.asdict(item) for item in candidates],
    }
    (out_dir / "crypto_market_candidates.json").write_text(
        json.dumps(data, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    with (out_dir / "crypto_market_candidates.csv").open(
        "w",
        newline="",
        encoding="utf-8",
    ) as handle:
        writer = csv.DictWriter(
            handle,
            fieldnames=list(dataclasses.asdict(candidates[0]).keys())
            if candidates
            else [field.name for field in dataclasses.fields(Candidate)],
        )
        writer.writeheader()
        for item in candidates:
            row = dataclasses.asdict(item)
            row["notes"] = "; ".join(item.notes)
            writer.writerow(row)
    lines = [
        "# Crypto Market Selector",
        "",
        f"- fetched_markets: {metadata['fetched_markets']}",
        f"- candidate_count: {len(candidates)}",
        f"- llm_status: {metadata['llm_status']}",
        "",
        "| rank | asset | question | spread | liq | vol24 | bid/ask | horizon |",
        "| ---: | --- | --- | ---: | ---: | ---: | --- | --- |",
    ]
    for item in candidates[:25]:
        lines.append(
            f"| {item.rank} | {item.asset} | {item.question} | "
            f"{item.spread:.3f} | {item.liquidity:,.0f} | "
            f"{item.volume24h:,.0f} | {item.best_bid:.3f}/{item.best_ask:.3f} | "
            f"{item.horizon} |"
        )
    (out_dir / "report.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--limit", type=int, default=5000)
    parser.add_argument("--page-size", type=int, default=100)
    parser.add_argument("--pause-s", type=float, default=0.05)
    parser.add_argument("--out-dir", default="")
    parser.add_argument("--top", type=int, default=40)
    parser.add_argument("--use-llm", action="store_true")
    parser.add_argument("--model", default=DEFAULT_MODEL)
    parser.add_argument("--max-tokens", type=int, default=3000)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    as_of = now_utc()
    out_dir = Path(args.out_dir) if args.out_dir else Path(
        "runs/crypto_market_selector_" + as_of.strftime("%Y%m%d_%H%M%S")
    )
    markets = fetch_gamma_markets(args.limit, args.page_size, args.pause_s)
    candidates = [
        item
        for item in (score_market(market, as_of) for market in markets)
        if item is not None
    ]
    candidates = dedupe_candidates(candidates)
    candidates.sort(key=lambda item: item.score, reverse=True)
    candidates = candidates[: args.top]
    llm_status = "not_requested"
    if args.use_llm:
        llm_status = apply_llm(candidates, args.model, args.max_tokens)
        candidates.sort(
            key=lambda item: (
                item.llm_class == "pure_price_target",
                item.score,
            ),
            reverse=True,
        )
    for rank, item in enumerate(candidates, start=1):
        item.rank = rank
    metadata = {
        "as_of": as_of.isoformat(),
        "fetched_markets": len(markets),
        "llm_status": llm_status,
        "model": args.model if args.use_llm else "",
    }
    write_outputs(out_dir, candidates, metadata)
    print("crypto_market_selector:")
    print(f"  out_dir: {out_dir}")
    print(f"  fetched_markets: {len(markets)}")
    print(f"  candidate_count: {len(candidates)}")
    print(f"  llm_status: {llm_status}")
    for item in candidates[:10]:
        print(
            f"  #{item.rank} {item.asset} {item.spread:.3f} "
            f"liq={item.liquidity:.0f} vol24={item.volume24h:.0f} "
            f"{item.question}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
