# Changelog

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
