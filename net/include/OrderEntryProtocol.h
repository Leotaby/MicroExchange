#pragma once

// ─────────────────────────────────────────────────────────────────────────
// OrderEntryProtocol — a compact binary wire protocol for order entry.
//
// Framing: every message is an 8-byte header followed by a fixed-size payload.
//
//     ┌────────────────── WireHeader (8 bytes) ──────────────────┐
//     │ type : u8 │ pad : u8[3] │ len : u32  (payload byte count) │
//     └───────────────────────────────────────────────────────────┘
//     ┌─────────────────────── payload (`len` bytes) ────────────┐
//
// Inbound (client → gateway):  NewOrder, Cancel
// Outbound (gateway → client): Exec (one per fill), Ack (one per request)
//
// A NewOrder elicits zero or more Exec messages (in match order) followed by
// exactly one Ack, so the client always knows when a request is complete.
//
// Endianness: this demo assumes a homogeneous little-endian deployment (x86-64
// / ARM64 on both ends) and copies POD structs on the wire. A production
// protocol would byte-order-normalise the header length and numeric fields
// (e.g. htole64); that is deliberately out of scope here and noted in the README.
// ─────────────────────────────────────────────────────────────────────────

#include "Order.h"          // micro_exchange::core types (Price, Quantity, ...)

#include <cstdint>
#include <cstring>
#include <unistd.h>

namespace micro_exchange::net {

using core::Price;
using core::Quantity;
using core::OrderId;

enum class MsgType : uint8_t {
    NewOrder = 1,
    Cancel   = 2,
    Ack      = 10,
    Exec     = 11,
    Reject   = 12,
};

enum class AckStatus : uint8_t {
    Accepted = 0,
    Rejected = 1,
    Cancelled = 2,
    Unknown  = 3,
};

// These structs are deliberately NOT bit-packed. Fields are ordered
// largest-first so every member is naturally aligned — no misaligned reads
// (clean under UBSan) — and both ends share one compiler/ABI in this project,
// so the on-wire layout matches without packing tricks. `pad` arrays make the
// trailing bytes explicit rather than compiler-implicit.
struct WireHeader {
    uint32_t len;       // payload size in bytes
    uint8_t  type;
    uint8_t  pad[3];
};

struct WireNewOrder {
    uint64_t id;
    int64_t  price;     // ticks; 0 = market
    uint64_t quantity;
    uint8_t  side;      // 0 = buy, 1 = sell
    uint8_t  type;      // core::OrderType
    uint8_t  tif;       // core::TimeInForce
    uint8_t  pad[5];
    char     symbol[16];
};

struct WireCancel {
    uint64_t id;
    char     symbol[16];
};

struct WireAck {
    uint64_t id;
    uint64_t filled_qty;    // cumulative filled at ack time
    uint8_t  status;        // AckStatus
    uint8_t  pad[7];
};

struct WireExec {
    uint64_t buy_order_id;
    uint64_t sell_order_id;
    int64_t  price;
    uint64_t quantity;
    uint8_t  aggressor;     // 0 = buy, 1 = sell
    uint8_t  pad[7];
};

// ── Blocking, partial-aware socket I/O ──

// Read exactly `n` bytes (loops over short recv()s). False on EOF/error.
inline bool read_full(int fd, void* buf, size_t n) {
    auto* p = static_cast<char*>(buf);
    size_t got = 0;
    while (got < n) {
        ssize_t r = ::read(fd, p + got, n - got);
        if (r <= 0) return false;   // 0 = peer closed, <0 = error
        got += static_cast<size_t>(r);
    }
    return true;
}

// Write exactly `n` bytes (loops over short write()s). False on error.
inline bool write_full(int fd, const void* buf, size_t n) {
    const auto* p = static_cast<const char*>(buf);
    size_t put = 0;
    while (put < n) {
        ssize_t w = ::write(fd, p + put, n - put);
        if (w <= 0) return false;
        put += static_cast<size_t>(w);
    }
    return true;
}

// Send one framed message (header + payload).
inline bool send_msg(int fd, MsgType type, const void* payload, uint32_t len) {
    WireHeader h{};
    h.type = static_cast<uint8_t>(type);
    h.len  = len;
    if (!write_full(fd, &h, sizeof(h))) return false;
    if (len && !write_full(fd, payload, len)) return false;
    return true;
}

// Receive one framed message header; caller then reads `out_hdr.len` bytes.
inline bool recv_header(int fd, WireHeader& out_hdr) {
    return read_full(fd, &out_hdr, sizeof(out_hdr));
}

} // namespace micro_exchange::net
