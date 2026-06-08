#!/usr/bin/env python3
"""Build a clean BTC/ETH/SOL "above strike by expiry" Polymarket universe.

This is a read-only discovery script. It only calls public Gamma metadata,
never places orders, and writes a CSV universe for later fair-value research.
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import json
import math
import re
import sys
import time
import urllib.parse
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Any


GAMMA_MARKETS_ENDPOINT = "https://gamma-api.polymarket.com/markets"
COIN_ALIASES: dict[str, tuple[str, ...]] = {
    "BTC": ("btc", "bitcoin"),
    "ETH": ("eth", "ethereum", "ether"),
    "SOL": ("sol", "solana"),
}
EXCLUDE_MARKET_RE = re.compile(
    r"\b("
    r"between|range|highest|lowest|high price|low price|"
    r"up or down|up/down|5m|5 min|5-minute|15m|hourly|"
    r"hit|reach|touch|ath|all[- ]time high|"
    r"market cap|etf|reserve|news|approve|approved|"
    r"outperform|dominance|flip|floor price"
    r")\b",
    re.IGNORECASE,
)
PRICE_RE = re.compile(
    r"(?:above|over|greater than|higher than)\s+\$?\s*"
    r"([0-9]{1,3}(?:,[0-9]{3})+(?:\.[0-9]+)?|[0-9]+(?:\.[0-9]+)?)",
    re.IGNORECASE,
)
FALLBACK_PRICE_RE = re.compile(
    r"\$?\s*([0-9]{1,3}(?:,[0-9]{3})+(?:\.[0-9]+)?|[0-9]+(?:\.[0-9]+)?)"
)
MONTH_NAME_RE = re.compile(
    r"\b("
    r"jan(?:uary)?|feb(?:ruary)?|mar(?:ch)?|apr(?:il)?|may|jun(?:e)?|"
    r"jul(?:y)?|aug(?:ust)?|sep(?:tember)?|oct(?:ober)?|nov(?:ember)?|dec(?:ember)?"
    r")\b",
    re.IGNORECASE,
)


@dataclass(frozen=True)
class UniverseMarket:
    market_id: str
    slug: str
    question: str
    coin: str
    strike: float
    expiry_ts: int
    yes_token_id: str
    no_token_id: str
    active: bool
    closed: bool
    volume: float
    liquidity: float


def now_utc() -> dt.datetime:
    return dt.datetime.now(dt.timezone.utc)


def http_json(url: str, timeout_s: int = 30) -> Any:
    request = urllib.request.Request(
        url,
        headers={
            "Accept": "application/json",
            "User-Agent": "Mozilla/5.0 PolytopeEdgeDiscovery/0.1",
        },
    )
    with urllib.request.urlopen(request, timeout=timeout_s) as response:
        return json.loads(response.read().decode("utf-8"))


def parse_json_array(value: Any) -> list[Any]:
    if isinstance(value, list):
        return value
    if isinstance(value, str) and value:
        try:
            parsed = json.loads(value)
        except json.JSONDecodeError:
            return []
        return parsed if isinstance(parsed, list) else []
    return []


def parse_bool(value: Any) -> bool:
    if isinstance(value, bool):
        return value
    if isinstance(value, str):
        return value.strip().lower() in {"1", "true", "yes"}
    return bool(value)


def number(value: Any, default: float = 0.0) -> float:
    if value in (None, ""):
        return default
    try:
        parsed = float(value)
    except (TypeError, ValueError):
        return default
    return parsed if math.isfinite(parsed) else default


def parse_datetime(value: Any) -> dt.datetime | None:
    if value in (None, ""):
        return None
    text = str(value).strip()
    if not text:
        return None
    if text.endswith("Z"):
        text = text[:-1] + "+00:00"
    try:
        parsed = dt.datetime.fromisoformat(text)
    except ValueError:
        return None
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=dt.timezone.utc)
    return parsed.astimezone(dt.timezone.utc)


def market_text(market: dict[str, Any]) -> str:
    return " ".join(
        str(market.get(key) or "")
        for key in ("question", "slug", "title", "description")
    )


def parse_coin(text: str) -> str | None:
    lowered = text.lower()
    for coin, aliases in COIN_ALIASES.items():
        if any(re.search(rf"\b{re.escape(alias)}\b", lowered) for alias in aliases):
            return coin
    return None


def contains_coin_and_above(text: str, coin: str) -> bool:
    lowered = text.lower()
    aliases = COIN_ALIASES[coin]
    return "above" in lowered and any(re.search(rf"\b{re.escape(alias)}\b", lowered) for alias in aliases)


def parse_strike(text: str, coin: str) -> float | None:
    """Parse the price threshold from the local "above <price>" phrase first."""
    match = PRICE_RE.search(text)
    if not match:
        match = FALLBACK_PRICE_RE.search(text)
    if not match:
        return None

    strike = number(match.group(1).replace(",", ""), default=math.nan)
    if not math.isfinite(strike):
        return None

    # Loose sanity bounds catch parsed dates such as "June 6" and irrelevant IDs.
    bounds = {
        "BTC": (1_000.0, 1_000_000.0),
        "ETH": (100.0, 100_000.0),
        "SOL": (1.0, 10_000.0),
    }[coin]
    return strike if bounds[0] <= strike <= bounds[1] else None


def expiry_datetime(market: dict[str, Any]) -> dt.datetime | None:
    for key in ("endDate", "endDateIso", "closedTime"):
        parsed = parse_datetime(market.get(key))
        if parsed is not None:
            return parsed
    return None


def is_recent_or_nearby(
    expiry: dt.datetime,
    as_of: dt.datetime,
    lookback_days: int,
    forward_days: int,
) -> bool:
    start = as_of - dt.timedelta(days=lookback_days)
    end = as_of + dt.timedelta(days=forward_days)
    return start <= expiry <= end


def outcome_token_ids(market: dict[str, Any]) -> tuple[str, str] | None:
    outcomes = [str(x).strip().lower() for x in parse_json_array(market.get("outcomes"))]
    tokens = [str(x) for x in parse_json_array(market.get("clobTokenIds"))]
    if len(outcomes) < 2 or len(tokens) < 2:
        return None

    yes_index = next((idx for idx, value in enumerate(outcomes) if value == "yes"), None)
    no_index = next((idx for idx, value in enumerate(outcomes) if value == "no"), None)
    if yes_index is None or no_index is None:
        return None
    if yes_index >= len(tokens) or no_index >= len(tokens) or yes_index == no_index:
        return None
    if not tokens[yes_index] or not tokens[no_index]:
        return None
    return tokens[yes_index], tokens[no_index]


def is_clean_above_market(market: dict[str, Any], coin: str) -> bool:
    q_slug = " ".join(str(market.get(key) or "") for key in ("question", "slug"))
    full_text = market_text(market)
    lowered = full_text.lower()
    if not contains_coin_and_above(q_slug, coin):
        return False
    if EXCLUDE_MARKET_RE.search(full_text):
        return False
    if "will " in lowered and (" hit " in lowered or " reach " in lowered):
        return False
    # Clean daily/weekly price-target markets almost always name the deadline.
    return bool(MONTH_NAME_RE.search(full_text) or expiry_datetime(market) is not None)


def universe_market_from_gamma(
    market: dict[str, Any],
    *,
    as_of: dt.datetime,
    lookback_days: int,
    forward_days: int,
) -> UniverseMarket | None:
    full_text = market_text(market)
    coin = parse_coin(full_text)
    if coin is None or not is_clean_above_market(market, coin):
        return None

    strike = parse_strike(full_text, coin)
    expiry = expiry_datetime(market)
    tokens = outcome_token_ids(market)
    if strike is None or expiry is None or tokens is None:
        return None
    if not is_recent_or_nearby(expiry, as_of, lookback_days, forward_days):
        return None

    active = parse_bool(market.get("active"))
    closed = parse_bool(market.get("closed"))
    if not (active or closed):
        return None

    yes_token_id, no_token_id = tokens
    question = str(market.get("question") or market.get("title") or "")
    slug = str(market.get("slug") or "")
    return UniverseMarket(
        market_id=str(market.get("conditionId") or market.get("id") or slug or question),
        slug=slug,
        question=question,
        coin=coin,
        strike=strike,
        expiry_ts=int(expiry.timestamp()),
        yes_token_id=yes_token_id,
        no_token_id=no_token_id,
        active=active,
        closed=closed,
        volume=number(market.get("volumeNum") or market.get("volume")),
        liquidity=number(market.get("liquidityNum") or market.get("liquidity")),
    )


def fetch_gamma_markets(
    *,
    limit: int,
    page_size: int,
    pause_s: float,
    active: bool | None,
    closed: bool | None,
    end_date_min: dt.datetime,
    end_date_max: dt.datetime,
    label: str,
    verbose: bool,
) -> list[dict[str, Any]]:
    markets: list[dict[str, Any]] = []
    offset = 0
    while len(markets) < limit:
        batch_size = min(page_size, limit - len(markets))
        query_params: dict[str, str | int] = {
            "end_date_min": end_date_min.isoformat().replace("+00:00", "Z"),
            "end_date_max": end_date_max.isoformat().replace("+00:00", "Z"),
            "limit": batch_size,
            "offset": offset,
        }
        if active is not None:
            query_params["active"] = str(active).lower()
        if closed is not None:
            query_params["closed"] = str(closed).lower()
        query = urllib.parse.urlencode(query_params)
        data = http_json(f"{GAMMA_MARKETS_ENDPOINT}?{query}")
        if not isinstance(data, list) or not data:
            break
        rows = [item for item in data if isinstance(item, dict)]
        markets.extend(rows)
        if verbose:
            print(f"fetched {label}: offset={offset} total={len(markets)}", file=sys.stderr)
        offset += len(data)
        if len(data) < batch_size:
            break
        if pause_s > 0:
            time.sleep(pause_s)
    return markets[:limit]


def build_universe(args: argparse.Namespace) -> list[UniverseMarket]:
    as_of = now_utc()
    end_date_min = as_of - dt.timedelta(days=args.lookback_days)
    end_date_max = as_of + dt.timedelta(days=args.forward_days)
    fetched = fetch_gamma_markets(
        limit=args.limit,
        page_size=args.page_size,
        pause_s=args.pause_s,
        active=None,
        closed=None,
        end_date_min=end_date_min,
        end_date_max=end_date_max,
        label="window",
        verbose=args.verbose,
    )

    by_id: dict[str, UniverseMarket] = {}
    for market in fetched:
        parsed = universe_market_from_gamma(
            market,
            as_of=as_of,
            lookback_days=args.lookback_days,
            forward_days=args.forward_days,
        )
        if parsed is not None:
            by_id[parsed.market_id] = parsed

    return sorted(by_id.values(), key=lambda row: (row.expiry_ts, row.coin, row.strike, row.slug))


def write_csv(path: Path, rows: list[UniverseMarket]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fieldnames = [
        "market_id",
        "slug",
        "question",
        "coin",
        "strike",
        "expiry_ts",
        "yes_token_id",
        "no_token_id",
        "active",
        "closed",
        "volume",
        "liquidity",
    ]
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow(
                {
                    "market_id": row.market_id,
                    "slug": row.slug,
                    "question": row.question,
                    "coin": row.coin,
                    "strike": f"{row.strike:.8g}",
                    "expiry_ts": row.expiry_ts,
                    "yes_token_id": row.yes_token_id,
                    "no_token_id": row.no_token_id,
                    "active": row.active,
                    "closed": row.closed,
                    "volume": f"{row.volume:.8g}",
                    "liquidity": f"{row.liquidity:.8g}",
                }
            )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", default="runs/edge_discovery/latest_14d/markets_universe.csv")
    parser.add_argument("--limit", type=int, default=10_000, help="Gamma rows to fetch inside the expiry window.")
    parser.add_argument("--page-size", type=int, default=100)
    parser.add_argument("--pause-s", type=float, default=0.0)
    parser.add_argument("--lookback-days", type=int, default=14)
    parser.add_argument("--forward-days", type=int, default=14)
    parser.add_argument("--verbose", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    rows = build_universe(args)
    out_path = Path(args.out)
    write_csv(out_path, rows)
    print(f"wrote {len(rows)} markets to {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
