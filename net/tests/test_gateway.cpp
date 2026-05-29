/**
 * test_gateway.cpp — end-to-end loopback test for the TCP order-entry gateway.
 *
 * Spins up an OrderGateway on an ephemeral port in a server thread, connects a
 * client over 127.0.0.1, and streams a deterministic order flow through the
 * wire protocol. It then runs the SAME orders through an in-process
 * MatchingEngine reference and asserts the networked path produced an identical
 * number of executions and identical traded volume — i.e. serialising orders
 * over TCP and parsing them back changes nothing about the matching outcome.
 *
 * Doubles as a usage demo for the protocol and as a CTest gate.
 */

#include "OrderGateway.h"
#include "OrderEntryProtocol.h"
#include "MatchingEngine.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <thread>
#include <vector>
#include <random>
#include <iostream>
#include <cstring>

using namespace micro_exchange;
using namespace micro_exchange::core;
using namespace micro_exchange::net;

static std::vector<NewOrderRequest> make_flow(size_t n, const char* sym) {
    std::mt19937_64 rng(7);
    std::uniform_int_distribution<Price>    price(95, 105);
    std::uniform_int_distribution<Quantity> qty(1, 5);
    std::uniform_int_distribution<int>      side(0, 1);
    std::uniform_real_distribution<double>  type(0.0, 1.0);

    std::vector<NewOrderRequest> v;
    v.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        NewOrderRequest r{};
        r.id   = i + 1;
        r.side = side(rng) ? Side::Buy : Side::Sell;
        if (type(rng) < 0.65) {
            r.type  = OrderType::Limit;
            r.tif   = TimeInForce::GTC;
            r.price = price(rng);
        } else {
            r.type  = OrderType::Market;
            r.tif   = TimeInForce::IOC;
            r.price = PRICE_MARKET;
        }
        r.quantity = qty(rng) * 100;
        std::strncpy(r.symbol, sym, sizeof(r.symbol) - 1);
        v.push_back(r);
    }
    return v;
}

int main() {
    const char* SYM = "TEST";
    auto orders = make_flow(3000, SYM);

    // ── Reference: same flow, in-process (no network) ──
    uint64_t ref_trades = 0, ref_volume = 0;
    {
        MatchingEngine ref;
        ref.add_symbol(SYM);
        ref.set_trade_callback([&](const Trade& t) { ++ref_trades; ref_volume += t.quantity; });
        for (const auto& r : orders) ref.submit_order(r);
    }

    // ── Gateway on an ephemeral port, served on its own thread ──
    OrderGateway gateway(0, SYM);
    uint16_t port = gateway.port();
    std::thread server([&] { gateway.serve_one_client(); });

    // ── Client: connect and stream orders lock-step (send → read execs → ack) ──
    int cfd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);

    // Disable Nagle on the client too: a lock-step small-message protocol must
    // not buffer — Nagle + delayed-ACK otherwise adds ~40ms per round trip.
    int nodelay = 1;
    ::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    // Defensive: a 5s receive timeout turns any stall into a visible error
    // instead of an indefinite hang.
    timeval tv{}; tv.tv_sec = 5; tv.tv_usec = 0;
    ::setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    bool connected = false;
    for (int i = 0; i < 200; ++i) {
        if (::connect(cfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) { connected = true; break; }
        usleep(2000);
    }
    if (!connected) { std::cerr << "client could not connect to gateway\n"; return 1; }

    uint64_t wire_execs = 0, wire_exec_volume = 0, acks = 0;

    for (const auto& r : orders) {
        WireNewOrder w{};
        w.id       = r.id;
        w.side     = static_cast<uint8_t>(r.side);
        w.type     = static_cast<uint8_t>(r.type);
        w.tif      = static_cast<uint8_t>(r.tif);
        w.price    = r.price;
        w.quantity = r.quantity;
        std::memcpy(w.symbol, r.symbol, sizeof(w.symbol));
        if (!send_msg(cfd, MsgType::NewOrder, &w, sizeof(w))) { std::cerr << "send failed\n"; return 1; }

        // Read execs until this order's Ack arrives.
        WireHeader h{};
        bool acked = false;
        while (!acked && recv_header(cfd, h)) {
            if (static_cast<MsgType>(h.type) == MsgType::Exec) {
                WireExec e{};
                if (!read_full(cfd, &e, sizeof(e))) { std::cerr << "exec read failed\n"; return 1; }
                ++wire_execs;
                wire_exec_volume += e.quantity;
            } else if (static_cast<MsgType>(h.type) == MsgType::Ack) {
                WireAck a{};
                if (!read_full(cfd, &a, sizeof(a))) { std::cerr << "ack read failed\n"; return 1; }
                ++acks;
                acked = true;
            } else {
                std::vector<char> tmp(h.len);
                read_full(cfd, tmp.data(), h.len);
            }
        }
        if (!acked) { std::cerr << "no ack for order " << r.id << "\n"; return 1; }
    }

    ::close(cfd);          // client done → server's read loop ends
    server.join();

    auto gs = gateway.stats();

    std::cout << "\n──────────── Gateway end-to-end test ────────────\n";
    std::cout << "  orders sent over TCP : " << orders.size() << "\n";
    std::cout << "  acks received        : " << acks << "\n";
    std::cout << "  execs over the wire  : " << wire_execs << " (vol " << wire_exec_volume << ")\n";
    std::cout << "  in-process reference : " << ref_trades << " (vol " << ref_volume << ")\n";
    std::cout << "  gateway engine trades: " << gs.total_trades << "\n";

    bool ok = (acks == orders.size())
           && (wire_execs == ref_trades)
           && (wire_exec_volume == ref_volume)
           && (gs.total_trades == ref_trades);

    std::cout << (ok ? "  GATEWAY TEST PASSED ✓\n" : "  GATEWAY TEST FAILED ✗\n");
    std::cout << "─────────────────────────────────────────────────\n";
    return ok ? 0 : 1;
}
