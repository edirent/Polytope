#!/usr/bin/env python3
"""Fair-value research pipeline for Polymarket comparisons.

Read-only by construction:
- no order placement
- no wallet access
- no authenticated trading endpoints

The script supports two research families:
- Crypto binary markets using cash-or-nothing Black-Scholes N(d2).
- Sports/event markets using multiplicative de-vig from decimal odds.

Polymarket prices can come from:
- explicit config values,
- Gamma market outcome prices,
- CLOB midpoint endpoint when accessible from the runtime environment.
"""

from __future__ import annotations

import argparse
import csv
import dataclasses
import datetime as dt
import json
import math
import os
import statistics
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any


BINANCE_SPOT_ENDPOINT = "https://api.binance.com/api/v3/ticker/price"
BINANCE_FUTURES_ENDPOINT = "https://fapi.binance.com/fapi/v1/ticker/price"
DERIBIT_ENDPOINT = "https://www.deribit.com/api/v2"
POLYMARKET_CLOB_ENDPOINT = "https://clob.polymarket.com"
POLYMARKET_GAMMA_ENDPOINT = "https://gamma-api.polymarket.com"


def now_utc() -> dt.datetime:
    return dt.datetime.now(dt.timezone.utc)


def parse_time(value: str) -> dt.datetime:
    parsed = dt.datetime.fromisoformat(value.replace("Z", "+00:00"))
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=dt.timezone.utc)
    return parsed.astimezone(dt.timezone.utc)


def year_fraction(start: dt.datetime, end: dt.datetime) -> float:
    return max(0.0, (end - start).total_seconds() / (365.25 * 86400.0))


def norm_cdf(x: float) -> float:
    return 0.5 * (1.0 + math.erf(x / math.sqrt(2.0)))


def binary_cash_or_nothing_call(
    *,
    spot: float,
    strike: float,
    years_to_expiry: float,
    volatility: float,
    rate: float = 0.0,
    discount: bool = False,
) -> float:
    if spot <= 0 or strike <= 0 or volatility <= 0 or years_to_expiry <= 0:
        raise ValueError("spot, strike, volatility, and maturity must be positive")
    sigma_sqrt_t = volatility * math.sqrt(years_to_expiry)
    d2 = (
        math.log(spot / strike)
        + (rate - 0.5 * volatility * volatility) * years_to_expiry
    ) / sigma_sqrt_t
    probability = norm_cdf(d2)
    if discount:
        return math.exp(-rate * years_to_expiry) * probability
    return probability


def multiplicative_devig(decimal_odds: dict[str, float]) -> dict[str, float]:
    raw = {name: 1.0 / odds for name, odds in decimal_odds.items() if odds > 1.0}
    total = sum(raw.values())
    if total <= 0:
        raise ValueError("no valid decimal odds")
    return {name: implied / total for name, implied in raw.items()}


def expected_value(fair_value: float, polymarket_price: float) -> float:
    # Algebraically equal to fair_value - price for a $1 binary, but kept in
    # expanded form to match the research spec.
    return (
        fair_value * (1.0 - polymarket_price)
        - (1.0 - fair_value) * polymarket_price
    )


def z_score(value: float, history: list[float]) -> float | None:
    if len(history) < 2:
        return None
    mean = statistics.mean(history)
    std = statistics.pstdev(history)
    if std <= 0:
        return None
    return (value - mean) / std


def http_json(url: str, timeout_s: int = 20) -> Any:
    request = urllib.request.Request(
        url,
        headers={"Accept": "application/json", "User-Agent": "PolytopeResearch/0.1"},
    )
    with urllib.request.urlopen(request, timeout=timeout_s) as response:
        return json.loads(response.read().decode("utf-8"))


def number(value: Any, default: float | None = None) -> float | None:
    if value is None or value == "":
        return default
    try:
        parsed = float(value)
    except (TypeError, ValueError):
        return default
    if math.isnan(parsed) or math.isinf(parsed):
        return default
    return parsed


def fetch_binance_price(symbol: str, *, futures: bool = False) -> tuple[float | None, str]:
    endpoint = BINANCE_FUTURES_ENDPOINT if futures else BINANCE_SPOT_ENDPOINT
    url = endpoint + "?" + urllib.parse.urlencode({"symbol": symbol})
    try:
        data = http_json(url)
        price = number(data.get("price"))
        return price, "ok" if price is not None else "missing_price"
    except urllib.error.HTTPError as ex:
        return None, f"http_{ex.code}"
    except Exception as ex:  # noqa: BLE001
        return None, f"error:{ex}"


def fetch_deribit_order_book(instrument_name: str) -> tuple[dict[str, Any] | None, str]:
    query = urllib.parse.urlencode({"instrument_name": instrument_name, "depth": 1})
    url = f"{DERIBIT_ENDPOINT}/public/get_order_book?{query}"
    try:
        data = http_json(url)
        result = data.get("result")
        if not isinstance(result, dict):
            return None, "missing_result"
        return result, "ok"
    except Exception as ex:  # noqa: BLE001
        return None, f"error:{ex}"


def fetch_deribit_iv(instrument_name: str) -> tuple[float | None, str]:
    book, status = fetch_deribit_order_book(instrument_name)
    if status != "ok" or not book:
        return None, status
    mark_iv = number(book.get("mark_iv"))
    if mark_iv is None:
        return None, "missing_mark_iv"
    # Deribit IV is usually percent, e.g. 55 means 55%.
    return mark_iv / 100.0 if mark_iv > 3.0 else mark_iv, "ok"


def fetch_deribit_underlying(instrument_name: str) -> tuple[float | None, str]:
    book, status = fetch_deribit_order_book(instrument_name)
    if status != "ok" or not book:
        return None, status
    for field in ("underlying_price", "index_price", "mark_price", "last_price"):
        value = number(book.get(field))
        if value is not None and value > 0:
            return value, "ok"
    return None, "missing_underlying"


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


def fetch_gamma_market_price(
    *,
    market_id: str | None = None,
    slug: str | None = None,
    outcome: str | None = None,
    token_id: str | None = None,
) -> tuple[float | None, str]:
    if not market_id and not slug:
        return None, "missing_gamma_identifier"
    if market_id:
        url = f"{POLYMARKET_GAMMA_ENDPOINT}/markets/{urllib.parse.quote(market_id)}"
    else:
        query = urllib.parse.urlencode({"slug": slug})
        url = f"{POLYMARKET_GAMMA_ENDPOINT}/markets?{query}"
    try:
        data = http_json(url)
    except Exception as ex:  # noqa: BLE001
        return None, f"error:{ex}"
    market = data[0] if isinstance(data, list) and data else data
    if not isinstance(market, dict):
        return None, "missing_market"
    outcomes = [str(x) for x in parse_json_array(market.get("outcomes"))]
    prices = [number(x) for x in parse_json_array(market.get("outcomePrices"))]
    tokens = [str(x) for x in parse_json_array(market.get("clobTokenIds"))]
    for idx, price in enumerate(prices):
        if price is None:
            continue
        if outcome and idx < len(outcomes) and outcomes[idx].lower() == outcome.lower():
            return price, "ok"
        if token_id and idx < len(tokens) and tokens[idx] == token_id:
            return price, "ok"
    return None, "outcome_not_found"


def fetch_polymarket_midpoint(token_id: str) -> tuple[float | None, str]:
    if not token_id:
        return None, "missing_token_id"
    query = urllib.parse.urlencode({"token_id": token_id})
    url = f"{POLYMARKET_CLOB_ENDPOINT}/midpoint?{query}"
    try:
        data = http_json(url)
        for field in ("mid", "midpoint"):
            value = number(data.get(field))
            if value is not None:
                return value, "ok"
        return None, "missing_mid"
    except urllib.error.HTTPError as ex:
        return None, f"http_{ex.code}"
    except Exception as ex:  # noqa: BLE001
        return None, f"error:{ex}"


def load_spread_history(path: str) -> dict[str, list[float]]:
    if not path:
        return {}
    out: dict[str, list[float]] = {}
    with open(path, newline="", encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            key = row.get("id") or row.get("market_id") or row.get("event_id")
            spread = number(row.get("spread"))
            if key and spread is not None:
                out.setdefault(key, []).append(spread)
    return out


@dataclasses.dataclass
class SignalRow:
    id: str
    kind: str
    fair_value: float
    polymarket_price: float
    absolute_spread: float
    z_score: float | None
    expected_value: float
    side: str
    action: str
    source_status: str
    notes: str


def choose_side(fair_value: float, price: float) -> str:
    if fair_value > price:
        return "buy_yes"
    if fair_value < price:
        return "sell_yes_or_buy_no"
    return "none"


def action_for(spread: float, z: float | None, ev: float, min_spread: float, min_z: float) -> str:
    if ev <= 0:
        return "ignore_negative_ev"
    if spread < min_spread:
        return "watch_small_spread"
    if z is not None and z < min_z:
        return "watch_not_statistically_extreme"
    if z is None:
        return "candidate_no_history"
    return "candidate"


def resolve_polymarket_price(config: dict[str, Any]) -> tuple[float | None, str]:
    direct = number(config.get("polymarket_price"))
    if direct is not None:
        return direct, "config"
    token_id = str(config.get("polymarket_token_id") or "")
    midpoint, mid_status = fetch_polymarket_midpoint(token_id)
    if midpoint is not None:
        return midpoint, "clob_midpoint"
    gamma_price, gamma_status = fetch_gamma_market_price(
        market_id=str(config.get("gamma_market_id") or "") or None,
        slug=str(config.get("gamma_slug") or "") or None,
        outcome=str(config.get("outcome") or "") or None,
        token_id=token_id or None,
    )
    if gamma_price is not None:
        return gamma_price, "gamma_outcome_price"
    return None, f"no_polymarket_price(clob={mid_status},gamma={gamma_status})"


def evaluate_crypto(
    item: dict[str, Any],
    *,
    as_of: dt.datetime,
    history: dict[str, list[float]],
    min_spread: float,
    min_z: float,
) -> SignalRow:
    item_id = str(item.get("id") or item.get("name") or "crypto")
    strike = number(item.get("strike"))
    expiry_text = str(item.get("expiry") or "")
    if strike is None or not expiry_text:
        raise ValueError(f"{item_id}: crypto item requires strike and expiry")
    expiry = parse_time(expiry_text)
    t = year_fraction(as_of, expiry)

    spot = number(item.get("spot"))
    source_bits = []
    if spot is None and item.get("binance_symbol"):
        spot, status = fetch_binance_price(
            str(item["binance_symbol"]),
            futures=bool(item.get("binance_futures")),
        )
        source_bits.append(f"binance={status}")
    if spot is None and item.get("deribit_underlying_instrument"):
        spot, status = fetch_deribit_underlying(str(item["deribit_underlying_instrument"]))
        source_bits.append(f"deribit_underlying={status}")

    iv = number(item.get("iv"))
    if iv is None and item.get("deribit_option_instrument"):
        iv, status = fetch_deribit_iv(str(item["deribit_option_instrument"]))
        source_bits.append(f"deribit_iv={status}")
    rate = number(item.get("rate"), 0.0) or 0.0
    if spot is None or iv is None:
        return SignalRow(
            id=item_id,
            kind="crypto_binary",
            fair_value=0.0,
            polymarket_price=0.0,
            absolute_spread=0.0,
            z_score=None,
            expected_value=0.0,
            side="none",
            action="missing_external_price_or_iv",
            source_status=";".join(source_bits),
            notes=f"spot={spot}, iv={iv}",
        )

    fair = binary_cash_or_nothing_call(
        spot=spot,
        strike=strike,
        years_to_expiry=t,
        volatility=iv,
        rate=rate,
        discount=bool(item.get("discount")),
    )
    price, price_status = resolve_polymarket_price(item)
    if price is None:
        price = 0.0
    spread = abs(fair - price)
    z = z_score(spread, history.get(item_id, []))
    ev = expected_value(fair, price)
    return SignalRow(
        id=item_id,
        kind="crypto_binary",
        fair_value=fair,
        polymarket_price=price,
        absolute_spread=spread,
        z_score=z,
        expected_value=ev,
        side=choose_side(fair, price),
        action=action_for(spread, z, ev, min_spread, min_z),
        source_status=";".join(source_bits + [f"polymarket={price_status}"]),
        notes=f"spot={spot:.4f}, strike={strike:.4f}, t={t:.6f}, iv={iv:.4f}, r={rate:.4f}",
    )


def evaluate_sports_item(
    item: dict[str, Any],
    *,
    history: dict[str, list[float]],
    min_spread: float,
    min_z: float,
) -> SignalRow:
    item_id = str(item.get("id") or item.get("event_id") or "sports")
    outcome = str(item.get("outcome") or "")
    odds_rows = item.get("odds") or []
    odds = {
        str(row["outcome"]): float(row["decimal_odds"])
        for row in odds_rows
        if row.get("outcome") and number(row.get("decimal_odds")) is not None
    }
    fair_map = multiplicative_devig(odds)
    if outcome not in fair_map:
        raise ValueError(f"{item_id}: outcome {outcome!r} not present in odds")
    fair = fair_map[outcome]
    price, price_status = resolve_polymarket_price(item)
    if price is None:
        price = 0.0
    spread = abs(fair - price)
    z = z_score(spread, history.get(item_id, []))
    ev = expected_value(fair, price)
    overround = sum(1.0 / x for x in odds.values())
    return SignalRow(
        id=item_id,
        kind="sports_devig",
        fair_value=fair,
        polymarket_price=price,
        absolute_spread=spread,
        z_score=z,
        expected_value=ev,
        side=choose_side(fair, price),
        action=action_for(spread, z, ev, min_spread, min_z),
        source_status=f"polymarket={price_status}",
        notes=f"outcome={outcome}, overround={overround:.6f}, odds={json.dumps(odds, sort_keys=True)}",
    )


def load_sports_csv(path: str) -> list[dict[str, Any]]:
    if not path:
        return []
    grouped: dict[str, dict[str, Any]] = {}
    with open(path, newline="", encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            event_id = row.get("event_id") or row.get("id")
            if not event_id:
                continue
            target = grouped.setdefault(
                event_id,
                {
                    "id": event_id,
                    "outcome": row.get("target_outcome") or row.get("outcome"),
                    "polymarket_price": row.get("polymarket_price"),
                    "polymarket_token_id": row.get("polymarket_token_id"),
                    "odds": [],
                },
            )
            target["odds"].append(
                {
                    "outcome": row.get("outcome"),
                    "decimal_odds": number(row.get("decimal_odds")),
                }
            )
    return list(grouped.values())


def write_outputs(output_dir: Path, rows: list[SignalRow], metadata: dict[str, Any]) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    (output_dir / "signals.json").write_text(
        json.dumps(
            {
                "metadata": metadata,
                "signals": [dataclasses.asdict(row) for row in rows],
            },
            indent=2,
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    with (output_dir / "signals.csv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(dataclasses.asdict(rows[0]).keys()) if rows else [])
        if rows:
            writer.writeheader()
            for row in rows:
                writer.writerow(dataclasses.asdict(row))

    lines = [
        "# Fair Value Spread Research",
        "",
        f"Generated: {metadata['generated_at']}",
        "",
        "## Method",
        "",
        "- Crypto binary fair value uses cash-or-nothing Black-Scholes `N(d2)`.",
        "- Sports fair value uses multiplicative de-vig on decimal odds.",
        "- Signal spread is `abs(fair_value - polymarket_price)`.",
        "- EV is `fair_value * (1 - price) - (1 - fair_value) * price`.",
        "- Z-score is computed only when a spread-history CSV is supplied.",
        "",
        "## Signals",
        "",
        "| ID | Kind | Fair | Polymarket | Spread | Z | EV | Side | Action | Notes |",
        "|---|---|---:|---:|---:|---:|---:|---|---|---|",
    ]
    for row in rows:
        z_text = "" if row.z_score is None else f"{row.z_score:.3f}"
        lines.append(
            "| "
            + " | ".join(
                [
                    row.id,
                    row.kind,
                    f"{row.fair_value:.6f}",
                    f"{row.polymarket_price:.6f}",
                    f"{row.absolute_spread:.6f}",
                    z_text,
                    f"{row.expected_value:.6f}",
                    row.side,
                    row.action,
                    row.notes.replace("|", "/"),
                ]
            )
            + " |"
        )
    lines += [
        "",
        "## Caveats",
        "",
        "- Binance may be region-blocked from this host; the script records source status instead of hiding it.",
        "- Deribit option `mark_iv` is used only when a matching option instrument is provided.",
        "- Polymarket CLOB public midpoint may be blocked by geography or CDN policy; config/Gamma prices are supported fallbacks.",
        "- These outputs are research signals, not executable orders.",
    ]
    (output_dir / "report.md").write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", default="")
    parser.add_argument("--sports-odds-csv", default="")
    parser.add_argument("--spread-history-csv", default="")
    parser.add_argument("--out-dir", default="")
    parser.add_argument("--min-spread", type=float, default=0.03)
    parser.add_argument("--min-z", type=float, default=2.0)
    args = parser.parse_args()

    as_of = now_utc()
    output_dir = Path(
        args.out_dir
        or f"research/runs/fair_value_pipeline_{as_of.strftime('%Y%m%d_%H%M%S')}"
    )
    config: dict[str, Any] = {"crypto": [], "sports": []}
    if args.config:
        config = json.loads(Path(args.config).read_text(encoding="utf-8"))
    config.setdefault("crypto", [])
    config.setdefault("sports", [])
    config["sports"].extend(load_sports_csv(args.sports_odds_csv))
    history = load_spread_history(args.spread_history_csv)

    rows: list[SignalRow] = []
    for item in config.get("crypto", []):
        rows.append(
            evaluate_crypto(
                item,
                as_of=as_of,
                history=history,
                min_spread=args.min_spread,
                min_z=args.min_z,
            )
        )
    for item in config.get("sports", []):
        rows.append(
            evaluate_sports_item(
                item,
                history=history,
                min_spread=args.min_spread,
                min_z=args.min_z,
            )
        )

    metadata = {
        "generated_at": as_of.isoformat(),
        "config": args.config,
        "sports_odds_csv": args.sports_odds_csv,
        "spread_history_csv": args.spread_history_csv,
        "min_spread": args.min_spread,
        "min_z": args.min_z,
    }
    write_outputs(output_dir, rows, metadata)

    print("fair_value_pipeline:")
    print(f"  out_dir: {output_dir}")
    print(f"  signals: {len(rows)}")
    print(f"  report: {output_dir / 'report.md'}")
    for row in rows[:10]:
        print(
            f"  {row.id}: fair={row.fair_value:.4f} "
            f"pm={row.polymarket_price:.4f} spread={row.absolute_spread:.4f} "
            f"ev={row.expected_value:.4f} action={row.action}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

