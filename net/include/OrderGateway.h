#pragma once

// ─────────────────────────────────────────────────────────────────────────
// OrderGateway — a single-threaded TCP order-entry gateway.
//
// This is the network front-end a real exchange puts in front of its matching
// engine: clients open a socket, stream binary order messages in, and receive
// execution reports + acknowledgements back. It deliberately mirrors the
// standard exchange model — one sequential gateway feeding a single-threaded
// matching core, which keeps the hot path lock-free and the event order
// deterministic.
//
// Design notes:
//   • Binds to 127.0.0.1 only (a demo gateway should never be world-reachable).
//   • Port 0 lets the OS pick a free port — handy for tests; read it via port().
//   • Trade prints are pushed to the connected client synchronously from the
//     matching callback, so Exec messages for a NewOrder arrive before its Ack.
//   • SIGPIPE is ignored so a client disconnect mid-write can't kill the server.
//
// Scope: one client at a time, processed sequentially (the common single-
// gateway model). Multiplexing many clients (epoll/kqueue) is a natural
// extension and is noted as future work.
// ─────────────────────────────────────────────────────────────────────────

#include "MatchingEngine.h"
#include "OrderEntryProtocol.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <csignal>
#include <cstring>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace micro_exchange::net {

using namespace micro_exchange::core;

class OrderGateway {
public:
    OrderGateway(uint16_t port, const std::string& symbol)
        : symbol_(symbol)
    {
        std::signal(SIGPIPE, SIG_IGN);   // a dead client must not kill us

        engine_.add_symbol(symbol_);

        // Stream every trade print to the currently-connected client.
        engine_.set_trade_callback([this](const Trade& t) {
            if (client_fd_ < 0) return;
            WireExec e{};
            e.buy_order_id  = t.buy_order_id;
            e.sell_order_id = t.sell_order_id;
            e.price         = t.price;
            e.quantity      = t.quantity;
            e.aggressor     = static_cast<uint8_t>(t.aggressor);
            send_msg(client_fd_, MsgType::Exec, &e, sizeof(e));
            ++execs_sent_;
        });

        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) throw std::runtime_error("socket() failed");

        int yes = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port);
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            ::close(listen_fd_);
            throw std::runtime_error("bind() failed");
        }
        if (::listen(listen_fd_, 16) < 0) {
            ::close(listen_fd_);
            throw std::runtime_error("listen() failed");
        }

        // Resolve the actual port (relevant when caller passed 0).
        sockaddr_in bound{};
        socklen_t blen = sizeof(bound);
        ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound), &blen);
        port_ = ntohs(bound.sin_port);
    }

    ~OrderGateway() {
        if (listen_fd_ >= 0) ::close(listen_fd_);
    }

    OrderGateway(const OrderGateway&) = delete;
    OrderGateway& operator=(const OrderGateway&) = delete;

    [[nodiscard]] uint16_t port() const { return port_; }

    // Accept ONE client and process its order stream until it disconnects.
    // Returns the number of inbound requests handled.
    uint64_t serve_one_client() {
        int fd = ::accept(listen_fd_, nullptr, nullptr);
        if (fd < 0) return 0;

        // Disable Nagle: order entry wants each message out immediately.
        int yes = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));

        client_fd_ = fd;
        uint64_t handled = 0;

        WireHeader h{};
        while (recv_header(fd, h)) {
            switch (static_cast<MsgType>(h.type)) {
                case MsgType::NewOrder: handled += handle_new_order(fd, h); break;
                case MsgType::Cancel:   handled += handle_cancel(fd, h);    break;
                default:                drain(fd, h.len);                   break;
            }
        }

        client_fd_ = -1;
        ::close(fd);
        return handled;
    }

    [[nodiscard]] MatchingEngine::EngineStats stats() const { return engine_.get_stats(); }
    [[nodiscard]] uint64_t execs_sent() const { return execs_sent_; }

private:
    uint64_t handle_new_order(int fd, const WireHeader& h) {
        WireNewOrder w{};
        if (h.len != sizeof(w) || !read_full(fd, &w, sizeof(w))) { drain(fd, h.len); return 0; }

        NewOrderRequest req{};
        req.id       = w.id;
        req.side     = static_cast<Side>(w.side);
        req.type     = static_cast<OrderType>(w.type);
        req.tif      = static_cast<TimeInForce>(w.tif);
        req.price    = w.price;
        req.quantity = w.quantity;
        std::memcpy(req.symbol, w.symbol, sizeof(req.symbol));

        // Exec messages for this order are emitted synchronously by the trade
        // callback during submit_order(); the Ack follows them.
        Order* o = engine_.submit_order(req);

        WireAck ack{};
        ack.id         = w.id;
        ack.status     = static_cast<uint8_t>(o ? AckStatus::Accepted : AckStatus::Rejected);
        ack.filled_qty = o ? o->filled_qty : 0;
        send_msg(fd, MsgType::Ack, &ack, sizeof(ack));
        return 1;
    }

    uint64_t handle_cancel(int fd, const WireHeader& h) {
        WireCancel w{};
        if (h.len != sizeof(w) || !read_full(fd, &w, sizeof(w))) { drain(fd, h.len); return 0; }

        CancelRequest req{};
        req.order_id = w.id;
        std::memcpy(req.symbol, w.symbol, sizeof(req.symbol));
        bool ok = engine_.cancel_order(req);

        WireAck ack{};
        ack.id     = w.id;
        ack.status = static_cast<uint8_t>(ok ? AckStatus::Cancelled : AckStatus::Unknown);
        send_msg(fd, MsgType::Ack, &ack, sizeof(ack));
        return 1;
    }

    // Discard an unrecognised / malformed payload so framing stays in sync.
    void drain(int fd, uint32_t len) {
        char buf[256];
        while (len > 0) {
            size_t chunk = len < sizeof(buf) ? len : sizeof(buf);
            if (!read_full(fd, buf, chunk)) return;
            len -= static_cast<uint32_t>(chunk);
        }
    }

    std::string     symbol_;
    MatchingEngine  engine_;
    int             listen_fd_ = -1;
    int             client_fd_ = -1;
    uint16_t        port_      = 0;
    uint64_t        execs_sent_ = 0;
};

} // namespace micro_exchange::net
