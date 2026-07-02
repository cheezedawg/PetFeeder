#!/usr/bin/env python3
"""
Mine Kalshi KXMLBGAME (MLB game winner) markets for activity in the $0.01–$0.02 band.

Public market data only — no API key required.
"""

from __future__ import annotations

import argparse
import csv
import json
import sys
import time
from collections import defaultdict
from dataclasses import asdict, dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable

import requests

BASE_URL = "https://api.elections.kalshi.com/trade-api/v2"
SERIES_TICKER = "KXMLBGAME"
PENNY_LOW = 0.01
PENNY_HIGH = 0.02
PRICE_EPS = 1e-6


@dataclass
class MarketSummary:
    ticker: str
    event_ticker: str
    title: str
    team: str
    result: str
    open_time: str
    close_time: str
    last_price: float | None
    total_volume: float
    trade_count: int
    vol_at_01: float
    vol_at_02: float
    vol_in_band: float
    trades_at_01: int
    trades_at_02: int
    touched_band_trades: bool
    transitions_01_to_02: int
    transitions_02_to_01: int
    candle_minutes_in_band: int
    candle_minutes_total: int
    bid_touched_band: bool
    ask_touched_band: bool
    max_yes_price_seen: float | None
    min_yes_price_seen: float | None


@dataclass
class AggregateStats:
    markets_analyzed: int = 0
    markets_with_trade_band_activity: int = 0
    markets_with_candle_band_activity: int = 0
    total_trade_volume: float = 0.0
    total_vol_at_01: float = 0.0
    total_vol_at_02: float = 0.0
    total_vol_in_band: float = 0.0
    total_transitions_01_to_02: int = 0
    total_transitions_02_to_01: int = 0
    touched_band_and_won: int = 0
    touched_band_and_lost: int = 0
    events_analyzed: int = 0
    events_with_any_penny_activity: int = 0


class KalshiClient:
    def __init__(self, base_url: str = BASE_URL, sleep_s: float = 0.12):
        self.base_url = base_url.rstrip("/")
        self.sleep_s = sleep_s
        self.session = requests.Session()

    def _get(self, path: str, params: dict[str, Any] | None = None) -> dict[str, Any]:
        url = f"{self.base_url}{path}"
        for attempt in range(4):
            resp = self.session.get(url, params=params, timeout=60)
            if resp.status_code == 429:
                time.sleep(2 ** attempt)
                continue
            resp.raise_for_status()
            time.sleep(self.sleep_s)
            return resp.json()
        resp.raise_for_status()
        raise RuntimeError("unreachable")

    def paginate(self, path: str, params: dict[str, Any], key: str) -> list[dict[str, Any]]:
        items: list[dict[str, Any]] = []
        cursor: str | None = None
        while True:
            page_params = dict(params)
            if cursor:
                page_params["cursor"] = cursor
            data = self._get(path, page_params)
            items.extend(data.get(key, []))
            cursor = data.get("cursor") or ""
            if not cursor:
                break
        return items

    def get_all_mlb_markets(self) -> list[dict[str, Any]]:
        live = self.paginate(
            "/markets",
            {"series_ticker": SERIES_TICKER, "status": "settled", "limit": 1000},
            "markets",
        )
        historical = self.paginate(
            "/historical/markets",
            {"series_ticker": SERIES_TICKER, "limit": 1000},
            "markets",
        )
        by_ticker = {m["ticker"]: m for m in live}
        for m in historical:
            by_ticker.setdefault(m["ticker"], m)
        return sorted(by_ticker.values(), key=lambda m: m.get("close_time", ""))

    def get_trades(self, ticker: str) -> list[dict[str, Any]]:
        return self.paginate(
            "/markets/trades",
            {"ticker": ticker, "limit": 1000},
            "trades",
        )

    def get_candlesticks(
        self, market_ticker: str, start_ts: int, end_ts: int, period_interval: int = 1
    ) -> list[dict[str, Any]]:
        path = f"/series/{SERIES_TICKER}/markets/{market_ticker}/candlesticks"
        data = self._get(
            path,
            {
                "start_ts": start_ts,
                "end_ts": end_ts,
                "period_interval": period_interval,
            },
        )
        return data.get("candlesticks", [])


def parse_price(value: str | float | None) -> float | None:
    if value is None or value == "":
        return None
    return float(value)


def parse_ts_iso(value: str) -> int:
    return int(datetime.fromisoformat(value.replace("Z", "+00:00")).timestamp())


def price_in_band(price: float | None) -> bool:
    if price is None:
        return False
    return PENNY_LOW - PRICE_EPS <= price <= PENNY_HIGH + PRICE_EPS


def is_exact_cent(price: float | None, cent: float) -> bool:
    if price is None:
        return False
    return abs(price - cent) <= PRICE_EPS


def candle_ohlc_values(candle: dict[str, Any]) -> list[float]:
    values: list[float] = []
    for side in ("yes_bid", "yes_ask"):
        block = candle.get(side) or {}
        for key in ("open_dollars", "high_dollars", "low_dollars", "close_dollars"):
            p = parse_price(block.get(key))
            if p is not None:
                values.append(p)
    trade_price = (candle.get("price") or {}).get("close_dollars")
    p = parse_price(trade_price)
    if p is not None:
        values.append(p)
    return values


def analyze_trades(trades: list[dict[str, Any]]) -> dict[str, Any]:
    if not trades:
        return {
            "trade_count": 0,
            "vol_at_01": 0.0,
            "vol_at_02": 0.0,
            "vol_in_band": 0.0,
            "trades_at_01": 0,
            "trades_at_02": 0,
            "touched_band": False,
            "transitions_01_to_02": 0,
            "transitions_02_to_01": 0,
            "max_yes_price": None,
            "min_yes_price": None,
        }

    ordered = sorted(trades, key=lambda t: t["created_time"])
    vol_at_01 = vol_at_02 = vol_in_band = 0.0
    trades_at_01 = trades_at_02 = 0
    transitions_01_to_02 = transitions_02_to_01 = 0
    touched_band = False
    prices: list[float] = []
    prev_price: float | None = None

    for trade in ordered:
        price = parse_price(trade.get("yes_price_dollars"))
        if price is None:
            continue
        qty = float(trade.get("count_fp") or 0)
        prices.append(price)

        if price_in_band(price):
            touched_band = True
            vol_in_band += qty
        if is_exact_cent(price, 0.01):
            vol_at_01 += qty
            trades_at_01 += 1
        if is_exact_cent(price, 0.02):
            vol_at_02 += qty
            trades_at_02 += 1

        if prev_price is not None:
            if is_exact_cent(prev_price, 0.01) and is_exact_cent(price, 0.02):
                transitions_01_to_02 += 1
            elif is_exact_cent(prev_price, 0.02) and is_exact_cent(price, 0.01):
                transitions_02_to_01 += 1
        prev_price = price

    return {
        "trade_count": len(ordered),
        "vol_at_01": vol_at_01,
        "vol_at_02": vol_at_02,
        "vol_in_band": vol_in_band,
        "trades_at_01": trades_at_01,
        "trades_at_02": trades_at_02,
        "touched_band": touched_band,
        "transitions_01_to_02": transitions_01_to_02,
        "transitions_02_to_01": transitions_02_to_01,
        "max_yes_price": max(prices) if prices else None,
        "min_yes_price": min(prices) if prices else None,
    }


def analyze_candles(candles: list[dict[str, Any]]) -> dict[str, Any]:
    minutes_in_band = 0
    bid_touched = ask_touched = False

    for candle in candles:
        values = candle_ohlc_values(candle)
        if any(price_in_band(v) for v in values):
            minutes_in_band += 1

        bid = candle.get("yes_bid") or {}
        ask = candle.get("yes_ask") or {}
        bid_vals = [parse_price(bid.get(k)) for k in ("open_dollars", "high_dollars", "low_dollars", "close_dollars")]
        ask_vals = [parse_price(ask.get(k)) for k in ("open_dollars", "high_dollars", "low_dollars", "close_dollars")]
        if any(price_in_band(v) for v in bid_vals if v is not None):
            bid_touched = True
        if any(price_in_band(v) for v in ask_vals if v is not None):
            ask_touched = True

    return {
        "candle_minutes_in_band": minutes_in_band,
        "candle_minutes_total": len(candles),
        "bid_touched_band": bid_touched,
        "ask_touched_band": ask_touched,
        "candle_touched_band": minutes_in_band > 0 or bid_touched or ask_touched,
    }


def summarize_market(client: KalshiClient, market: dict[str, Any], fetch_candles: bool) -> MarketSummary:
    ticker = market["ticker"]
    trades = client.get_trades(ticker)
    trade_stats = analyze_trades(trades)

    candle_stats = {
        "candle_minutes_in_band": 0,
        "candle_minutes_total": 0,
        "bid_touched_band": False,
        "ask_touched_band": False,
        "candle_touched_band": False,
    }
    if fetch_candles and market.get("open_time") and market.get("close_time"):
        start_ts = parse_ts_iso(market["open_time"])
        end_ts = parse_ts_iso(market["close_time"])
        candles = client.get_candlesticks(ticker, start_ts, end_ts)
        candle_stats = analyze_candles(candles)

    return MarketSummary(
        ticker=ticker,
        event_ticker=market.get("event_ticker", ""),
        title=market.get("title", ""),
        team=market.get("yes_sub_title", ""),
        result=market.get("result", ""),
        open_time=market.get("open_time", ""),
        close_time=market.get("close_time", ""),
        last_price=parse_price(market.get("last_price_dollars")),
        total_volume=float(market.get("volume_fp") or 0),
        trade_count=trade_stats["trade_count"],
        vol_at_01=trade_stats["vol_at_01"],
        vol_at_02=trade_stats["vol_at_02"],
        vol_in_band=trade_stats["vol_in_band"],
        trades_at_01=trade_stats["trades_at_01"],
        trades_at_02=trade_stats["trades_at_02"],
        touched_band_trades=trade_stats["touched_band"],
        transitions_01_to_02=trade_stats["transitions_01_to_02"],
        transitions_02_to_01=trade_stats["transitions_02_to_01"],
        candle_minutes_in_band=candle_stats["candle_minutes_in_band"],
        candle_minutes_total=candle_stats["candle_minutes_total"],
        bid_touched_band=candle_stats["bid_touched_band"],
        ask_touched_band=candle_stats["ask_touched_band"],
        max_yes_price_seen=trade_stats["max_yes_price"],
        min_yes_price_seen=trade_stats["min_yes_price"],
    )


def aggregate(summaries: list[MarketSummary]) -> AggregateStats:
    stats = AggregateStats(markets_analyzed=len(summaries))
    events: dict[str, list[MarketSummary]] = defaultdict(list)

    for s in summaries:
        events[s.event_ticker].append(s)
        stats.total_trade_volume += s.total_volume
        stats.total_vol_at_01 += s.vol_at_01
        stats.total_vol_at_02 += s.vol_at_02
        stats.total_vol_in_band += s.vol_in_band
        stats.total_transitions_01_to_02 += s.transitions_01_to_02
        stats.total_transitions_02_to_01 += s.transitions_02_to_01

        trade_or_candle = s.touched_band_trades or s.candle_minutes_in_band > 0 or s.bid_touched_band
        if s.touched_band_trades:
            stats.markets_with_trade_band_activity += 1
        if s.candle_minutes_in_band > 0 or s.bid_touched_band or s.ask_touched_band:
            stats.markets_with_candle_band_activity += 1
        if trade_or_candle:
            if s.result == "yes":
                stats.touched_band_and_won += 1
            elif s.result == "no":
                stats.touched_band_and_lost += 1

    stats.events_analyzed = len(events)
    for event_summaries in events.values():
        if any(
            s.touched_band_trades or s.candle_minutes_in_band > 0 or s.bid_touched_band
            for s in event_summaries
        ):
            stats.events_with_any_penny_activity += 1

    return stats


def write_csv(path: Path, summaries: Iterable[MarketSummary]) -> None:
    rows = [asdict(s) for s in summaries]
    if not rows:
        return
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=rows[0].keys())
        writer.writeheader()
        writer.writerows(rows)


def print_report(stats: AggregateStats, summaries: list[MarketSummary]) -> None:
    n = stats.markets_analyzed or 1
    print("\n=== Kalshi MLB $0.01–$0.02 Penny Band Report ===\n")
    print(f"Markets analyzed:              {stats.markets_analyzed}")
    print(f"Games (events):                {stats.events_analyzed}")
    print(f"Markets with trade activity:   {stats.markets_with_trade_band_activity} ({100*stats.markets_with_trade_band_activity/n:.1f}%)")
    print(f"Markets with quote activity:   {stats.markets_with_candle_band_activity} ({100*stats.markets_with_candle_band_activity/n:.1f}%)")
    print(f"Games with penny activity:     {stats.events_with_any_penny_activity} ({100*stats.events_with_any_penny_activity/max(stats.events_analyzed,1):.1f}%)")
    print()
    print(f"Total reported volume:         {stats.total_trade_volume:,.2f} contracts")
    print(f"Volume traded at $0.01:        {stats.total_vol_at_01:,.2f} ({100*stats.total_vol_at_01/max(stats.total_trade_volume,1):.2f}%)")
    print(f"Volume traded at $0.02:        {stats.total_vol_at_02:,.2f} ({100*stats.total_vol_at_02/max(stats.total_trade_volume,1):.2f}%)")
    print(f"Volume in $0.01–$0.02 band:    {stats.total_vol_in_band:,.2f} ({100*stats.total_vol_in_band/max(stats.total_trade_volume,1):.2f}%)")
    print()
    print(f"Trade transitions $0.01→$0.02: {stats.total_transitions_01_to_02}")
    print(f"Trade transitions $0.02→$0.01: {stats.total_transitions_02_to_01}")
    touched = stats.touched_band_and_won + stats.touched_band_and_lost
    if touched:
        print()
        print(f"Markets touching band — settled YES: {stats.touched_band_and_won} ({100*stats.touched_band_and_won/touched:.1f}%)")
        print(f"Markets touching band — settled NO:  {stats.touched_band_and_lost} ({100*stats.touched_band_and_lost/touched:.1f}%)")

    # Top movers in the band
    movers = sorted(
        [s for s in summaries if s.transitions_01_to_02 + s.transitions_02_to_01 > 0],
        key=lambda s: s.transitions_01_to_02 + s.transitions_02_to_01,
        reverse=True,
    )[:10]
    if movers:
        print("\nTop markets by 1¢↔2¢ trade transitions:")
        for s in movers:
            total = s.transitions_01_to_02 + s.transitions_02_to_01
            print(
                f"  {s.ticker}: {total} transitions "
                f"(↑{s.transitions_01_to_02}/↓{s.transitions_02_to_01}), "
                f"vol@1¢={s.vol_at_01:,.0f}, result={s.result}"
            )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sample", type=int, default=0, help="Only analyze first N markets (0 = all)")
    parser.add_argument("--recent", type=int, default=0, help="Analyze N most recently settled markets")
    parser.add_argument("--no-candles", action="store_true", help="Skip candlestick fetches (faster)")
    parser.add_argument("--output-dir", type=Path, default=Path("output"), help="Directory for CSV/JSON output")
    parser.add_argument("--sleep", type=float, default=0.12, help="Seconds between API calls")
    args = parser.parse_args()

    client = KalshiClient(sleep_s=args.sleep)
    print("Fetching settled KXMLBGAME markets (live + historical)...", flush=True)
    markets = client.get_all_mlb_markets()
    print(f"Found {len(markets)} unique settled markets.", flush=True)

    if args.recent > 0:
        markets = list(reversed(markets))[: args.recent]
        print(f"Analyzing {len(markets)} most recent markets.", flush=True)
    elif args.sample > 0:
        markets = markets[: args.sample]
        print(f"Sampling first {len(markets)} markets.", flush=True)

    summaries: list[MarketSummary] = []
    for i, market in enumerate(markets, 1):
        if i % 25 == 0 or i == 1:
            print(f"  [{i}/{len(markets)}] {market['ticker']}", flush=True)
        try:
            summaries.append(
                summarize_market(client, market, fetch_candles=not args.no_candles)
            )
        except requests.HTTPError as exc:
            print(f"  WARN: failed {market['ticker']}: {exc}", file=sys.stderr)

    stats = aggregate(summaries)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now(timezone.utc).strftime("%Y%m%d_%H%M%S")
    csv_path = args.output_dir / f"penny_band_markets_{stamp}.csv"
    json_path = args.output_dir / f"penny_band_summary_{stamp}.json"
    write_csv(csv_path, summaries)
    json_path.write_text(json.dumps({"aggregate": asdict(stats), "generated_at": stamp}, indent=2))
    print(f"\nWrote {csv_path}")
    print(f"Wrote {json_path}")
    print_report(stats, summaries)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
