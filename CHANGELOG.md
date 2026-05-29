# Changelog

## v1.3.0 (2026-05-30)

### New features
- **`ArrayOrderBook` — a tick-indexed array order book** as a drop-in
  alternative to the `std::map`-based `OrderBook`. Levels live in one
  contiguous array indexed by `price - min_price`; an **occupied-level bitmap**
  with `__builtin_ctzll` / `__builtin_clzll` advances the best-bid/ask cursors
  by word-skipping instead of a linear scan. Reuses the same `Order`,
  `PriceLevel`, and `ArenaAllocator`, so it is a clean A/B against the tree.
- **`bench_orderbook_compare`** — runs an identical order stream through both
  books, asserts a **byte-identical trade stream**, then reports throughput and
  latency. Registered as the CTest gate `orderbook_equivalence`.

### Results (1M orders, ~9900–10100 band)
- Trade streams identical (176,732 trades) — CI-gated.
- Median per-order latency **84 ns (array) vs 125 ns (std::map)**, ~33% lower;
  P99 334 vs 375 ns. Throughput ~6.0M orders/sec for both — the per-order cost
  is dominated by the `OrderId→Order*` hash insert and the `now()` timestamp,
  not the level container.
- Characterised the band-width tradeoff: a *naive* linear best-bid/ask scan is
  ~25× slower than `std::map` on a sparse 200k-level band; the bitmap index
  removes that cliff and keeps the array ≥1.0× through realistic widths.

### Known limitations / next
- Extreme sparsity (≫50k active levels) still favours a two-level summary
  bitmap; tracked as future work.
- `ArrayOrderBook` covers Limit/Market/IOC/FOK + cancel; Stop/StopLimit/amend
  remain in the full `OrderBook`.

## v1.2.0 (2026-05-30)

### Fixes (analytics correctness)
- **Stylized facts were sampled per-event on the integer-tick mid**, so the
  return series was ~99% exact zeros — inflating excess kurtosis to a spurious
  78 and crushing the autocorrelation of |returns| to ~0.02. Returns are now
  sampled on fixed 1-second clock-time bars as log returns (Cont, 2001). With
  the artifact removed, volatility clustering emerges from the Hawkes arrivals:
  AC(|r|, lag 1) = 0.24 (empirical 0.15–0.40) and excess kurtosis = 1.16.
- **Spread decomposition violated `effective = realized + impact`** because the
  price-impact term was averaged in absolute value while realized was signed.
  All three components are now consistently signed; the identity holds and the
  adverse-selection ratio is meaningful (≈0 for uninformed ZI flow — as theory
  predicts, and consistent with the near-zero Kyle's λ).

### Docs
- README sample results, visualization captions, validation table, and CLI
  usage now match what the current code actually produces and supports.
- `output/report.txt` regenerated from the current build.
- `research/microstructure_paper.md` now reports the simulation's real numbers
  and contrasts them honestly with empirical equity benchmarks; an informed-
  trader agent population is identified as the principal extension.

### Notes
- Throughput/latency figures are hardware-dependent; the analytics are
  deterministic and identical across machines and runs.

## v1.1.0 (2026-04-06)

### New features
- **Stop and Stop-Limit orders.** A parked-order book per side, indexed by
  trigger price. The book tracks `last_trade_price` and on every aggressive
  cycle it walks the parked orders whose trigger has been crossed and
  re-submits them as Market (Stop) or Limit (StopLimit) orders. Cancellation
  works against parked stops too. Re-entry is guarded so a stop that
  triggers another stop doesn't blow the stack.
- **Multi-subscriber callback fan-out on `OrderBook`.** New
  `add_trade_listener` / `add_order_listener` APIs let the matching engine,
  the feed publisher, the analytics layer and any user code subscribe to
  the same book without clobbering each other. The legacy `set_*_callback`
  setters now alias to `add_*_listener` so existing code keeps working.
- **`FeedPublisher` re-enabled in the main simulation path.** Previously
  disabled because it would replace the engine's trade callback. Now wired
  through the listener fan-out and reports `Feed messages: ...` in the
  per-run report.
- **`bench_latency` benchmark binary.** Reports per-operation latency
  histogram (min/p50/p90/p95/p99/p999/max) plus end-to-end throughput.
- **CTest integration.** `enable_testing()` and two registered tests:
  the invariant suite and a tiny end-to-end smoke test of the simulator.
- **GitHub Actions CI.** Builds Debug and Release on Ubuntu and macOS,
  runs `ctest`, and on Release builds also runs the throughput and
  latency benches.

### Fixes
- **Undefined behaviour in `match_against`.** The function was computing
  `std::prev(contra_side.end())` *before* checking whether the side was
  empty, which is UB on an empty `std::map`. Caught by AddressSanitizer
  on a Debug build. The iterator is now computed inside the loop, after
  the empty check.
- Kyle's lambda regression now uses simulated wall-clock timestamps from
  the Hawkes event stream instead of an event-index proxy. Spread
  decomposition's mid-after lookup is also time-based now.
- Removed the `unused variable` warning in the cancel test.

### Tests
- Four new test cases for the changes above:
  `test_stop_market_triggers`, `test_stop_limit_triggers_and_rests`,
  `test_stop_cancel`, `test_multi_listener_fanout`.

### Still on the to-do list
- Iceberg / hidden-quantity orders.
- Per-agent order tracking in the simulator (cancel rates are still
  estimates rather than ground truth).
- Volatility clustering remains weak; the ZI agents need to condition on
  recent volatility before this will improve materially.

## v1.0.0 (2026-02-12)

First public release.

### Core Engine
- CLOB with price-time priority (FIFO)
- Limit, Market, IOC, FOK order types
- Amend and Cancel support
- Arena allocator for zero-malloc hot path
- All three invariants tested (no crossed book, FIFO, determinism)

### Market Data
- ITCH-style feed messages (Add, Execute, Delete, Snapshot, Quote)
- SPSC lock-free ring buffer for producer-consumer transport
- Binary dump/replay support

### Simulation
- Hawkes process for clustered order arrivals
- ZI agents with strategic cancellation
- Full pipeline: events → matching → analytics

### Analytics
- Spread decomposition (Huang-Stoll)
- Kyle's lambda estimation
- Order flow imbalance
- Stylized facts verification

### Known Issues
- FeedPublisher clobbers OrderBook's trade callback when attached.
  Needs a proper multi-subscriber dispatcher. See TODO in main.cpp.
- Kyle's lambda R² is lower than expected (~0.00 vs 0.20-0.40).
  Likely because midprice indexing uses event count rather than wall time.
  Need to switch to proper timestamp-based interval bucketing.
- Volatility clustering AC is weak (0.02 vs 0.15-0.40 benchmark).
  May need stronger Hawkes excitation or multi-regime switching.
- Arena allocator never actually frees memory (orders accumulate).
  Fine for simulation but would need periodic cleanup for long-running.

## v0.9.0 (internal)

- Got matching engine + tests passing
- Realized the `reduce_quantity` assert was firing on partial fills
  because fill() modifies leaves_qty before the level can update.
  Fixed by calling reduce_quantity before fill().
- First working Hawkes simulation (but no trades because market order
  price was 0 and the price check rejected it — forgot PRICE_MARKET
  special case initially)

## v0.5.0 (internal)

- Basic OrderBook with std::map
- Limit orders only
- No tests, lots of segfaults from dangling pointers in the linked list
  (forgot to null out prev/next on removal)

## v0.1.0 (internal)

- Prototype in Python to validate spread decomposition math
- Ported to C++ once the formulas checked out
