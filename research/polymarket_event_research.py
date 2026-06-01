#!/usr/bin/env python3
"""Read-only Polymarket event research.

This script deliberately does not place orders. It fetches Gamma market data,
aggregates reward/spread metrics by event, and optionally uses the same
OpenRouter environment contract as the Oracle LLM path to classify information
risk.
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
import textwrap
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any


GAMMA_MARKETS_ENDPOINT = "https://gamma-api.polymarket.com/markets"
OPENROUTER_ENDPOINT = "https://openrouter.ai/api/v1/chat/completions"
DEFAULT_MODEL = "nvidia/llama-3.3-nemotron-super-49b-v1.5"


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
    request_headers = {"Accept": "application/json", "User-Agent": "PolytopeResearch/0.1"}
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


def first_event(market: dict[str, Any]) -> dict[str, Any]:
    events = market.get("events")
    if isinstance(events, list) and events and isinstance(events[0], dict):
        return events[0]
    return {}


def market_event_id(market: dict[str, Any]) -> str:
    event = first_event(market)
    for key in ("id", "event_id"):
        value = event.get(key) or market.get(key)
        if value not in (None, ""):
            return str(value)
    return "market:" + str(market.get("id") or market.get("conditionId") or "")


def market_event_title(market: dict[str, Any]) -> str:
    event = first_event(market)
    return str(event.get("title") or event.get("question") or market.get("question") or "")


def reward_daily_rate(market: dict[str, Any]) -> float:
    total = 0.0
    for reward in market.get("clobRewards") or []:
        if isinstance(reward, dict):
            total += number(reward.get("rewardsDailyRate"))
    return total


def reward_amount(market: dict[str, Any]) -> float:
    total = 0.0
    for reward in market.get("clobRewards") or []:
        if isinstance(reward, dict):
            total += number(reward.get("rewardsAmount"))
    return total


def reward_max_spread_decimal(market: dict[str, Any]) -> float:
    # Gamma exposes rewardsMaxSpread in cents. 3.5 means 3.5 cents = 0.035.
    value = number(market.get("rewardsMaxSpread"))
    return value / 100.0 if value > 1.0 else value


def end_datetime(market: dict[str, Any]) -> dt.datetime | None:
    value = market.get("endDateIso") or market.get("endDate")
    if not value:
        return None
    try:
        text = str(value).replace("Z", "+00:00")
        parsed = dt.datetime.fromisoformat(text)
        if parsed.tzinfo is None:
            parsed = parsed.replace(tzinfo=dt.timezone.utc)
        return parsed.astimezone(dt.timezone.utc)
    except ValueError:
        return None


def keyword_score(text: str) -> tuple[int, list[str]]:
    t = text.lower()
    flags: list[str] = []
    score = 0
    groups = [
        (45, "crypto/token/airdrop/price shock", ["crypto", "bitcoin", "btc", "ethereum", "eth", "solana", "airdrop", "token", "fdv", "$"]),
        (45, "war/geopolitical shock", ["war", "military", "offensive", "missile", "invasion", "invade", "ceasefire", "hostage", "iran", "russia", "putin", "zelenskyy", "israel", "gaza", "taiwan", "annex", "nuclear deal", "abraham accords", "regime fall", "nato", "article 5", "korea", "direct talks"]),
        (40, "court/legal inside timing", ["sentenced", "sentence", "prison", "court", "judge", "trial", "verdict", "custody", "jail", "arrest", "detained"]),
        (38, "company/product/private benchmark risk", ["ai model", "company", "largest company", "top ai", "best ai", "acquire", "tiktok", "earnings", "ipo", "market cap", "spacex", "starship", "openai", "leave "]),
        (35, "company/product/entertainment private info", ["album", "release", "gta", "grand theft auto", "bond actor"]),
        (35, "political insider/headline risk", ["president", "nomination", "election", "trump", "biden", "congress", "senate", "xi jinping", "macron", "starmer", "out by", "out as", "secession", "state legislature", "citizenship", "revoked", "tax", "dollarize", "tariff", "treasury", "blockchain", "impeach", "midterm", "balance of power", "deduction", "repealed"]),
        (34, "sovereign credit/headline risk", ["default", "debt", "downgrade"]),
        (45, "market crash/extreme-tail risk", ["circuit breaker", "marketwide", "stock market crash", "s&p", "nasdaq", "dow jones"]),
        (42, "celebrity/private-life risk", ["pregnant", "marriage", "divorce", "taylor swift"]),
        (36, "macro policy/headline risk", ["fed", "rate cut", "interest rate", "cpi", "inflation", "jobs report", "unemployment", "recession", "gdp", "economic"]),
        (24, "award/voting leak risk", ["ballon d'or", "oscar", "grammy", "emmy", "award"]),
        (30, "weather/disaster tail risk", ["hurricane", "earthquake", "wildfire", "temperature", "rain", "snow"]),
        (22, "single-game injury/lineup risk", [" vs. ", " v. ", "beat ", "win "]),
    ]
    for add, label, keywords in groups:
        if any(keyword in t for keyword in keywords):
            score += add
            flags.append(label)
    if "world cup" in t or "stanley cup" in t or "nba finals" in t:
        score = max(0, score - 18)
        flags.append("long-horizon broad sports tournament")
    return score, sorted(set(flags))


@dataclasses.dataclass
class EventScore:
    event_id: str
    title: str
    market_count: int
    reward_market_count: int
    avg_spread: float
    median_spread: float
    max_spread: float
    avg_reward_cap: float
    avg_spread_to_cap: float
    reward_daily_rate_sum: float
    reward_amount_sum: float
    median_min_size: float
    liquidity_sum: float
    volume24h_sum: float
    competitive_avg: float
    days_to_end_min: float | None
    one_day_abs_move_max: float
    heuristic_risk_score: int
    heuristic_flags: list[str]
    heuristic_class: str
    llm_class: str = "not_run"
    llm_rationale: str = ""
    combined_score: float = 0.0


def risk_class(score: int) -> str:
    if score <= 25:
        return "low"
    if score <= 55:
        return "medium"
    return "high"


def classify_event(markets: list[dict[str, Any]], as_of: dt.datetime) -> EventScore:
    event_id = market_event_id(markets[0])
    title = market_event_title(markets[0])
    spreads = [number(m.get("spread")) for m in markets if number(m.get("spread")) > 0]
    reward_caps = [reward_max_spread_decimal(m) for m in markets if reward_max_spread_decimal(m) > 0]
    min_sizes = [number(m.get("rewardsMinSize")) for m in markets if number(m.get("rewardsMinSize")) > 0]
    spread_to_cap = []
    for market in markets:
        cap = reward_max_spread_decimal(market)
        spread = number(market.get("spread"))
        if cap > 0 and spread > 0:
            spread_to_cap.append(spread / cap)
    end_dates = [end_datetime(m) for m in markets if end_datetime(m) is not None]
    days_to_end_min = None
    if end_dates:
        days_to_end_min = min((e - as_of).total_seconds() / 86400.0 for e in end_dates)

    text = " ".join([title] + [str(m.get("question") or "") for m in markets])
    score, flags = keyword_score(text)

    if days_to_end_min is not None:
        if days_to_end_min < 3:
            score += 35
            flags.append("near-expiry")
        elif days_to_end_min < 14:
            score += 20
            flags.append("short-dated")
        elif days_to_end_min < 45:
            score += 8
            flags.append("medium-dated")

    abs_moves = [abs(number(m.get("oneDayPriceChange"))) for m in markets]
    one_day_abs_move_max = max(abs_moves) if abs_moves else 0.0
    if one_day_abs_move_max >= 0.08:
        score += 30
        flags.append("large 1d price move")
    elif one_day_abs_move_max >= 0.03:
        score += 12
        flags.append("moderate 1d price move")

    if spread_to_cap and statistics.mean(spread_to_cap) > 0.75:
        score += 8
        flags.append("spread close to reward cap")

    market_count = len(markets)
    reward_market_count = sum(
        1
        for m in markets
        if number(m.get("rewardsMinSize")) > 0
        or reward_max_spread_decimal(m) > 0
        or reward_daily_rate(m) > 0
    )
    liquidity_sum = sum(number(m.get("liquidityNum") or m.get("liquidity")) for m in markets)
    volume24h_sum = sum(number(m.get("volume24hr")) for m in markets)
    if reward_market_count >= 8 and market_count >= 8:
        flags.append("diversified multi-market event")
    if liquidity_sum >= 500_000:
        flags.append("high liquidity")
    if volume24h_sum >= 25_000:
        flags.append("active recent volume")

    score = max(0, min(score, 100))
    event = EventScore(
        event_id=event_id,
        title=title,
        market_count=market_count,
        reward_market_count=reward_market_count,
        avg_spread=statistics.mean(spreads) if spreads else 0.0,
        median_spread=statistics.median(spreads) if spreads else 0.0,
        max_spread=max(spreads) if spreads else 0.0,
        avg_reward_cap=statistics.mean(reward_caps) if reward_caps else 0.0,
        avg_spread_to_cap=statistics.mean(spread_to_cap) if spread_to_cap else 0.0,
        reward_daily_rate_sum=sum(reward_daily_rate(m) for m in markets),
        reward_amount_sum=sum(reward_amount(m) for m in markets),
        median_min_size=statistics.median(min_sizes) if min_sizes else 0.0,
        liquidity_sum=liquidity_sum,
        volume24h_sum=volume24h_sum,
        competitive_avg=statistics.mean([number(m.get("competitive")) for m in markets]),
        days_to_end_min=days_to_end_min,
        one_day_abs_move_max=one_day_abs_move_max,
        heuristic_risk_score=score,
        heuristic_flags=sorted(set(flags)),
        heuristic_class=risk_class(score),
    )
    return event


def fetch_gamma_markets(limit: int, page_size: int) -> list[dict[str, Any]]:
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
        markets.extend([m for m in data if isinstance(m, dict)])
        offset += len(data)
        if len(data) < batch_size:
            break
    return markets[:limit]


def keep_market(market: dict[str, Any]) -> bool:
    if not market.get("active") or market.get("closed") or market.get("archived"):
        return False
    if not market.get("enableOrderBook", True):
        return False
    if market.get("acceptingOrders") is False:
        return False
    outcomes = parse_json_array(market.get("outcomes"))
    tokens = parse_json_array(market.get("clobTokenIds"))
    if len(outcomes) < 2 or len(tokens) < 2:
        return False
    if number(market.get("spread")) <= 0:
        return False
    if number(market.get("bestBid")) <= 0 or number(market.get("bestAsk")) <= 0:
        return False
    has_reward = (
        number(market.get("rewardsMinSize")) > 0
        or reward_max_spread_decimal(market) > 0
        or reward_daily_rate(market) > 0
    )
    return has_reward


def build_llm_prompt(events: list[EventScore], event_markets: dict[str, list[dict[str, Any]]]) -> str:
    compact = []
    for event in events:
        markets = event_markets[event.event_id][:12]
        compact.append(
            {
                "event_id": event.event_id,
                "title": event.title,
                "heuristic_class": event.heuristic_class,
                "heuristic_score": event.heuristic_risk_score,
                "heuristic_flags": event.heuristic_flags,
                "market_count": event.market_count,
                "reward_market_count": event.reward_market_count,
                "avg_spread": round(event.avg_spread, 5),
                "avg_reward_cap": round(event.avg_reward_cap, 5),
                "volume24h_sum": round(event.volume24h_sum, 2),
                "liquidity_sum": round(event.liquidity_sum, 2),
                "sample_markets": [
                    {
                        "question": m.get("question"),
                        "spread": m.get("spread"),
                        "bestBid": m.get("bestBid"),
                        "bestAsk": m.get("bestAsk"),
                        "rewardsMinSize": m.get("rewardsMinSize"),
                        "rewardsMaxSpread": m.get("rewardsMaxSpread"),
                    }
                    for m in markets
                ],
            }
        )
    return (
        "You are screening Polymarket events for passive market-making. "
        "Prefer events least exposed to private/inside information, sudden "
        "binary shocks, manipulation, or extreme price jumps. Penalize legal "
        "case timing, corporate/product release timing, crypto/token events, "
        "geopolitical/military events, near-expiry events, and markets where "
        "a small informed group can know the outcome first. Long-horizon, "
        "broad, liquid, public-information events are safer. "
        "Return JSON only: {\"events\":[{\"event_id\":\"...\","
        "\"llm_class\":\"low|medium|high\",\"confidence\":0.0,"
        "\"rationale\":\"short\",\"risk_flags\":[\"...\"]}]}\n\n"
        + json.dumps({"events": compact}, ensure_ascii=False)
    )


def call_openrouter(prompt: str, model: str, max_tokens: int) -> tuple[str, dict[str, Any] | None]:
    api_key = os.getenv("OPENROUTER_API_KEY")
    if not api_key:
        return "missing_api_key", None
    body = {
        "model": model,
        "max_tokens": max_tokens,
        "temperature": 0,
        "messages": [
            {
                "role": "system",
                "content": "Return strict JSON only. Do not include markdown.",
            },
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
    except urllib.error.HTTPError as ex:
        detail = ex.read().decode("utf-8", errors="replace")
        return f"http_{ex.code}: {detail[:300]}", None
    except Exception as ex:  # noqa: BLE001
        return f"error: {ex}", None

    try:
        content = response["choices"][0]["message"]["content"]
        if isinstance(content, list):
            content = "".join(
                part.get("text", "") if isinstance(part, dict) else str(part)
                for part in content
            )
        text = str(content).strip()
        start = text.find("{")
        end = text.rfind("}")
        if start >= 0 and end > start:
            text = text[start : end + 1]
        return "ok", json.loads(text)
    except Exception as ex:  # noqa: BLE001
        return f"parse_error: {ex}", {"raw_response": response}


def apply_llm(events: list[EventScore], llm_json: dict[str, Any] | None) -> None:
    if not llm_json:
        return
    rows = llm_json.get("events")
    if not isinstance(rows, list):
        return
    by_id = {event.event_id: event for event in events}
    for row in rows:
        if not isinstance(row, dict):
            continue
        event = by_id.get(str(row.get("event_id")))
        if not event:
            continue
        llm_class = str(row.get("llm_class") or "").lower()
        if llm_class not in {"low", "medium", "high"}:
            continue
        event.llm_class = llm_class
        event.llm_rationale = str(row.get("rationale") or "")


def final_score(event: EventScore) -> float:
    class_penalty = {"low": 0, "medium": 25, "high": 60}
    llm_penalty = class_penalty.get(event.llm_class, class_penalty[event.heuristic_class])
    reward_fit = 0.0
    if event.avg_reward_cap > 0:
        reward_fit += max(0.0, 1.0 - event.avg_spread_to_cap) * 20.0
    else:
        reward_fit -= 25.0
    reward_fit += min(20.0, math.log10(max(event.liquidity_sum, 1.0)) * 3.0)
    reward_fit += min(15.0, math.log10(max(event.volume24h_sum, 1.0)) * 2.5)
    reward_fit += min(10.0, event.reward_market_count)
    return reward_fit - llm_penalty - event.heuristic_risk_score * 0.35


def write_csv(path: Path, events: list[EventScore]) -> None:
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(
            handle,
            fieldnames=[
                "rank",
                "event_id",
                "title",
                "combined_score",
                "heuristic_class",
                "llm_class",
                "heuristic_risk_score",
                "market_count",
                "reward_market_count",
                "avg_spread",
                "median_spread",
                "max_spread",
                "avg_reward_cap",
                "avg_spread_to_cap",
                "reward_daily_rate_sum",
                "reward_amount_sum",
                "median_min_size",
                "liquidity_sum",
                "volume24h_sum",
                "competitive_avg",
                "days_to_end_min",
                "one_day_abs_move_max",
                "heuristic_flags",
                "llm_rationale",
            ],
        )
        writer.writeheader()
        for rank, event in enumerate(events, start=1):
            row = dataclasses.asdict(event)
            row["rank"] = rank
            row["heuristic_flags"] = "; ".join(event.heuristic_flags)
            writer.writerow(row)


def fmt_money(value: float) -> str:
    return f"${value:,.2f}"


def write_report(
    path: Path,
    *,
    as_of: dt.datetime,
    fetched_count: int,
    reward_market_count: int,
    events: list[EventScore],
    llm_status: str,
    model: str,
    output_dir: Path,
) -> None:
    top = events[:15]
    actionable = [
        event for event in events
        if (event.llm_class if event.llm_class != "not_run" else event.heuristic_class) == "low"
        and event.avg_reward_cap > 0
        and event.avg_spread_to_cap <= 0.5
        and event.liquidity_sum >= 25_000
    ]
    lines = [
        "# Polymarket Low-Adverse-Selection Event Research",
        "",
        f"Generated: {as_of.isoformat()}",
        "",
        "## Scope",
        "",
        "This is a read-only research pass. It does not place orders, approve risk, or connect to execution.",
        "",
        "## Method Changes",
        "",
        "- Do not rank by reward alone; reward-rich markets can be exactly where adverse selection is highest.",
        "- Group by event before ranking. Single markets hide cross-market common shocks and shared inside-information channels.",
        "- Require reward metadata and live order-book spread before considering a market for market making.",
        "- Compare current spread with the reward spread cap; wide markets near the cap are less attractive despite rewards.",
        "- Penalize event categories where a small informed group can know the result early: courts, corporate/product releases, token launches, crypto price triggers, war/geopolitics, and near-expiry binaries.",
        "- Prefer long-horizon, liquid, multi-market public-information events, but still require live depth monitoring before quoting.",
        "",
        "## Data",
        "",
        f"- Gamma markets fetched: {fetched_count}",
        f"- Reward/order-book markets retained: {reward_market_count}",
        f"- Event candidates ranked: {len(events)}",
        f"- LLM status: `{llm_status}`",
        f"- LLM model: `{model}`",
        "",
        "## Executive Finding",
        "",
    ]
    if actionable:
        lines += [
            f"- Actionable low-risk reward events found: {len(actionable)}.",
            "- These still require live CLOB depth, quote-lifetime, and fill-toxicity checks before any maker deployment.",
        ]
    else:
        lines += [
            "- No clean low-risk + reward-cap + tight-spread event passed the conservative screen.",
            "- The best-scoring events are watchlist candidates, not automatic maker targets.",
            "- Most reward-rich events in the sample still carry macro, political, company/private-information, or geopolitical headline risk.",
        ]
    lines += [
        "",
        "## Top Event Candidates",
        "",
        "| Rank | Event | Risk | Markets | Avg spread | Reward cap | Spread/cap | Min size | Liquidity | 24h vol | Notes |",
        "|---:|---|---|---:|---:|---:|---:|---:|---:|---:|---|",
    ]
    for rank, event in enumerate(top, start=1):
        risk = event.llm_class if event.llm_class != "not_run" else event.heuristic_class
        notes = ", ".join(event.heuristic_flags[:3])
        lines.append(
            "| "
            + " | ".join(
                [
                    str(rank),
                    f"{event.title} (`{event.event_id}`)",
                    risk,
                    f"{event.reward_market_count}/{event.market_count}",
                    f"{event.avg_spread:.4f}",
                    f"{event.avg_reward_cap:.4f}",
                    f"{event.avg_spread_to_cap:.2f}",
                    f"{event.median_min_size:.0f}",
                    fmt_money(event.liquidity_sum),
                    fmt_money(event.volume24h_sum),
                    notes,
                ]
            )
            + " |"
        )
    lines += [
        "",
        "## Rejected Pattern Examples",
        "",
        "- Court/sentencing markets may have strong reward metadata but are vulnerable to legal-process timing and informed leaks.",
        "- Corporate/product/album/release markets are exposed to issuer or media-side information asymmetry.",
        "- Crypto/token/airdrop markets combine high jump risk with concentrated insider information.",
        "- Near-expiry sports or weather markets may have stale public prices but high private/live information value.",
        "",
        "## Output Files",
        "",
        f"- `{output_dir / 'raw_markets.json'}`",
        f"- `{output_dir / 'event_candidates.csv'}`",
        f"- `{output_dir / 'event_candidates.json'}`",
        f"- `{output_dir / 'llm_input.json'}`",
        f"- `{output_dir / 'llm_output.json'}`",
        "",
        "## Caveats",
        "",
        "- `clobRewards[].rewardsDailyRate` and `rewardsMinSize/rewardsMaxSpread` are treated as platform metadata, not guaranteed realized revenue.",
        "- This pass uses current Gamma spread fields. A production maker strategy must confirm CLOB book depth, queue position, cancel rates, and fill toxicity.",
        "- LLM classification is advisory only. It must not override deterministic risk guards.",
        "",
    ]
    path.write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--limit", type=int, default=1000)
    parser.add_argument("--page-size", type=int, default=100)
    parser.add_argument("--top-events", type=int, default=30)
    parser.add_argument("--llm-events", type=int, default=25)
    parser.add_argument("--use-llm", action="store_true")
    parser.add_argument("--model", default=os.getenv("OPENROUTER_MODEL") or DEFAULT_MODEL)
    parser.add_argument("--max-tokens", type=int, default=int(os.getenv("OPENROUTER_MAX_TOKENS") or "1800"))
    parser.add_argument("--out-dir", default="")
    args = parser.parse_args()

    as_of = now_utc()
    stamp = as_of.strftime("%Y%m%d_%H%M%S")
    output_dir = Path(args.out_dir or f"research/runs/polymarket_event_research_{stamp}")
    output_dir.mkdir(parents=True, exist_ok=True)

    markets = fetch_gamma_markets(args.limit, args.page_size)
    kept = [m for m in markets if keep_market(m)]
    by_event: dict[str, list[dict[str, Any]]] = {}
    for market in kept:
        by_event.setdefault(market_event_id(market), []).append(market)

    events = [classify_event(group, as_of) for group in by_event.values()]
    events = [event for event in events if event.reward_market_count > 0]
    events.sort(
        key=lambda e: (
            e.heuristic_risk_score,
            -e.reward_market_count,
            e.avg_spread_to_cap,
            -e.liquidity_sum,
        )
    )

    llm_status = "not_requested"
    llm_payload = None
    llm_input_events = events[: args.llm_events]
    prompt = build_llm_prompt(llm_input_events, by_event)
    (output_dir / "llm_input.json").write_text(
        json.dumps({"prompt": prompt}, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    if args.use_llm:
        llm_status, llm_payload = call_openrouter(prompt, args.model, args.max_tokens)
        apply_llm(events, llm_payload)

    for event in events:
        event.combined_score = final_score(event)
    events.sort(key=lambda e: e.combined_score, reverse=True)
    events = events[: args.top_events]

    raw_payload = {
        "generated_at": as_of.isoformat(),
        "markets_fetched": len(markets),
        "markets_kept": len(kept),
        "source": GAMMA_MARKETS_ENDPOINT,
        "markets": markets,
    }
    (output_dir / "raw_markets.json").write_text(
        json.dumps(raw_payload, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    (output_dir / "event_candidates.json").write_text(
        json.dumps([dataclasses.asdict(e) for e in events], ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    (output_dir / "llm_output.json").write_text(
        json.dumps({"status": llm_status, "response": llm_payload}, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    write_csv(output_dir / "event_candidates.csv", events)
    write_report(
        output_dir / "report.md",
        as_of=as_of,
        fetched_count=len(markets),
        reward_market_count=len(kept),
        events=events,
        llm_status=llm_status,
        model=args.model,
        output_dir=output_dir,
    )

    print("polymarket_event_research:")
    print(f"  out_dir: {output_dir}")
    print(f"  markets_fetched: {len(markets)}")
    print(f"  reward_markets: {len(kept)}")
    print(f"  events_ranked: {len(events)}")
    print(f"  llm_status: {llm_status}")
    print(f"  report: {output_dir / 'report.md'}")
    if events:
        print("top_events:")
        for rank, event in enumerate(events[:5], start=1):
            risk = event.llm_class if event.llm_class != "not_run" else event.heuristic_class
            print(
                f"  {rank}. {event.title} | risk={risk} | "
                f"spread={event.avg_spread:.4f} | reward_cap={event.avg_reward_cap:.4f} | "
                f"score={event.combined_score:.2f}"
            )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
