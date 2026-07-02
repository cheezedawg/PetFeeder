# Kalshi MLB Penny-Band Miner

Exploratory tool for a Kalshi MLB game-winner bot. Pulls historical **KXMLBGAME** markets and measures how much trading and quoting happens between **$0.01 and $0.02** — the penny band where heavy underdogs often trade.

No API key is required; this uses Kalshi's public market data endpoints.

## What it measures

For each settled MLB game-winner contract:

| Metric | Source | Meaning |
|--------|--------|---------|
| `vol_at_01` / `vol_at_02` | Trades | Contract volume at exactly 1¢ or 2¢ |
| `vol_in_band` | Trades | Volume with yes price in [1¢, 2¢] |
| `transitions_01_to_02` | Trades | Consecutive trade went 1¢ → 2¢ |
| `transitions_02_to_01` | Trades | Consecutive trade went 2¢ → 1¢ |
| `candle_minutes_in_band` | 1-min candles | Minutes where bid/ask touched the band |
| `bid_touched_band` | 1-min candles | Best bid ever quoted in the band |
| Settlement win rate | Market result | How often penny-band underdogs actually won |

## Setup

```bash
cd kalshi-mlb
pip install -r requirements.txt
```

## Usage

Quick sample (20 markets, ~1–2 min):

```bash
python mine_penny_band.py --recent 20 --no-candles
```

Recent games with candlesticks (~50 markets, ~10 min):

```bash
python mine_penny_band.py --recent 50
```

Full run (~7,000+ markets; allow several hours due to per-market trade/candle fetches):

```bash
python mine_penny_band.py
```

Trades-only full run (faster, skips candlesticks):

```bash
python mine_penny_band.py --no-candles
```

Outputs land in `output/`:

- `penny_band_markets_<timestamp>.csv` — per-market detail
- `penny_band_summary_<timestamp>.json` — aggregate stats

## API notes

- **Series**: `KXMLBGAME` (individual MLB game winners; two markets per game)
- **Live settled markets**: `GET /markets?series_ticker=KXMLBGAME&status=settled`
- **Older archived markets**: `GET /historical/markets?series_ticker=KXMLBGAME`
- **Trades**: `GET /markets/trades?ticker=...`
- **Candlesticks**: `GET /series/KXMLBGAME/markets/{ticker}/candlesticks?period_interval=1`

Kalshi partitions data older than ~3 months into historical endpoints. This script queries both.

## Interpreting results for a bot

A 1¢ → 2¢ move is a **100% return** on capital but only **1¢ absolute** per contract. Useful questions this miner answers:

1. **Liquidity** — Is there enough volume at 1¢/2¢ to enter/exit?
2. **Oscillation** — Do prices bounce between 1¢ and 2¢ during live games (scalp opportunity)?
3. **Win rate** — When a team trades at 1¢, how often do they still win?
4. **Quote vs trade** — Candle bid/ask may show 1¢ quotes without fills (use both columns).

## Next steps toward a live bot

1. Run the full historical mine and review CSV outliers.
2. Add inning/score context (MLB Stats API) to correlate penny-band moves with game state.
3. Use the Kalshi WebSocket feed for live order book + trade stream.
4. Paper-trade a strategy: e.g. buy at 1¢ when bid depth appears, sell at 2¢ on bounce.

Docs: https://docs.kalshi.com/
