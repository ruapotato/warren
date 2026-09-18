#include "net/socket.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "core/log.h"

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
#  define MF_INVALID_SOCKET INVALID_SOCKET
#  define MF_CLOSE closesocket
#  define MF_WOULD_BLOCK (WSAGetLastError() == WSAEWOULDBLOCK)
#else
#  include <arpa/inet.h>
#  include <errno.h>
#  include <fcntl.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
using socket_t = int;
#  define MF_INVALID_SOCKET (-1)
#  define MF_CLOSE ::close
#  define MF_WOULD_BLOCK (errno == EAGAIN || errno == EWOULDBLOCK)
#endif

namespace mf::net {
namespace {

// Winsock has to be started, once, before any socket call, and the
// count is per process. A static does it at first use rather than
// making every program that links the engine remember.
struct SocketStartup {
    SocketStartup() {
#if defined(_WIN32)
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
            MF_ERROR("net: WSAStartup failed");
#endif
    }
    ~SocketStartup() {
#if defined(_WIN32)
        WSACleanup();
#endif
    }
};
void ensure_started() { static SocketStartup s; }

}  // namespace

// ------------------------------------------------------------ Address

std::string Address::to_string() const {
    const uint8_t *b = (const uint8_t *)&host;
    char out[32];
    std::snprintf(out, sizeof(out), "%u.%u.%u.%u:%u", b[0], b[1], b[2], b[3],
                  port);
    return out;
}

Address Address::loopback(uint16_t port) {
    Address a;
    a.host = htonl(0x7F000001u);
    a.port = port;
    return a;
}

Address Address::any(uint16_t port) {
    Address a;
    a.host = htonl(0x00000000u);
    a.port = port;
    return a;
}

bool Address::parse(const std::string &text, Address *out) {
    if (!out) return false;
    const size_t colon = text.rfind(':');
    if (colon == std::string::npos) return false;
    const std::string host = text.substr(0, colon);
    const std::string port = text.substr(colon + 1);
    if (host.empty() || port.empty()) return false;
    ensure_started();

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo *res = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res)
        return false;
    const sockaddr_in *sa = (const sockaddr_in *)res->ai_addr;
    out->host = sa->sin_addr.s_addr;
    out->port = ntohs(sa->sin_port);
    freeaddrinfo(res);
    return true;
}

// ------------------------------------------------------- UdpTransport

UdpTransport::~UdpTransport() { close(); }

bool UdpTransport::open(uint16_t port) {
    ensure_started();
    close();
    socket_t s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == MF_INVALID_SOCKET) {
        MF_ERROR("net: could not create a socket");
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(s, (sockaddr *)&addr, sizeof(addr)) != 0) {
        MF_ERROR("net: could not bind port %u", port);
        MF_CLOSE(s);
        return false;
    }

    // NON-BLOCKING, ALWAYS. A game loop that can block in recvfrom
    // is a game loop that stutters when the network does.
#if defined(_WIN32)
    u_long nonblocking = 1;
    ioctlsocket(s, FIONBIO, &nonblocking);
#else
    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif

    sockaddr_in bound{};
    socklen_t len = sizeof(bound);
    getsockname(s, (sockaddr *)&bound, &len);
    local_.host = bound.sin_addr.s_addr;
    local_.port = ntohs(bound.sin_port);
    handle_ = (long long)s;
    return true;
}

void UdpTransport::close() {
    if (handle_ == -1) return;
    MF_CLOSE((socket_t)handle_);
    handle_ = -1;
    local_ = Address{};
}

bool UdpTransport::send(const Address &to, const void *data, size_t size) {
    if (handle_ == -1 || !data || !size || size > kMaxDatagram) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = to.host;
    addr.sin_port = htons(to.port);
    const auto n = ::sendto((socket_t)handle_, (const char *)data, int(size), 0,
                            (const sockaddr *)&addr, sizeof(addr));
    if (n < 0) return false;
    sent_packets++;
    sent_bytes += size;
    return true;
}

bool UdpTransport::receive(Datagram *out) {
    if (handle_ == -1 || !out) return false;
    uint8_t buffer[kMaxDatagram];
    sockaddr_in from{};
    socklen_t len = sizeof(from);
    const auto n = ::recvfrom((socket_t)handle_, (char *)buffer, sizeof(buffer),
                              0, (sockaddr *)&from, &len);
    if (n <= 0) return false;
    out->from.host = from.sin_addr.s_addr;
    out->from.port = ntohs(from.sin_port);
    out->bytes.assign(buffer, buffer + n);
    received_packets++;
    received_bytes += size_t(n);
    return true;
}

// -------------------------------------------------- SimulatedNetwork

class SimulatedTransport final : public Transport {
public:
    SimulatedTransport(SimulatedNetwork *net, uint16_t port) : net_(net) {
        local_ = Address::loopback(port);
    }
    bool send(const Address &to, const void *data, size_t size) override {
        if (!data || !size || size > kMaxDatagram) return false;
        net_->submit(local_, to, data, size, now_);
        return true;
    }
    bool receive(Datagram *out) override {
        if (inbox_.empty()) return false;
        *out = std::move(inbox_.front());
        inbox_.erase(inbox_.begin());
        return true;
    }
    Address local() const override { return local_; }
    void poll(double now) override { now_ = now; }

    void deliver(Datagram d) { inbox_.push_back(std::move(d)); }
    void set_now(double n) { now_ = n; }

private:
    SimulatedNetwork *net_;
    Address local_;
    double now_ = 0.0;
    std::vector<Datagram> inbox_;
};

SimulatedNetwork::SimulatedNetwork() = default;
SimulatedNetwork::~SimulatedNetwork() = default;

Transport *SimulatedNetwork::endpoint(uint16_t port) {
    for (auto &e : endpoints_)
        if (e->local().port == port) return e.get();
    endpoints_.push_back(std::make_unique<SimulatedTransport>(this, port));
    return endpoints_.back().get();
}

float SimulatedNetwork::random() {
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return float(rng_ & 0xFFFFFF) / 16777216.0f;
}

void SimulatedNetwork::submit(const Address &from, const Address &to,
                              const void *data, size_t size, double now) {
    sent++;
    if (conditions.loss > 0.0f && random() < conditions.loss) {
        dropped++;
        return;
    }
    int copies = 1;
    if (conditions.duplicate > 0.0f && random() < conditions.duplicate) {
        copies = 2;
        duplicated++;
    }
    for (int i = 0; i < copies; i++) {
        InFlight f;
        f.from = from;
        f.to = to;
        f.bytes.assign((const uint8_t *)data, (const uint8_t *)data + size);
        double delay = conditions.latency;
        if (conditions.jitter > 0.0)
            delay += double(random()) * conditions.jitter;
        f.due = now + delay;
        // WITHOUT THIS, A ZERO-JITTER NETWORK STILL REORDERS. The
        // delivery queue is scanned in order of `due`, and equal
        // times would be settled by whatever sort happens to do --
        // so a monotonically increasing tiebreak is what makes
        // "no jitter, no reordering" actually mean it.
        f.order = order_++;
        in_flight_.push_back(std::move(f));
    }
}

void SimulatedNetwork::poll(double now) {
    for (auto &e : endpoints_) e->set_now(now);

    std::vector<InFlight> still;
    std::vector<InFlight> ready;
    still.reserve(in_flight_.size());
    for (InFlight &f : in_flight_) {
        if (f.due <= now)
            ready.push_back(std::move(f));
        else
            still.push_back(std::move(f));
    }
    in_flight_ = std::move(still);

    std::sort(ready.begin(), ready.end(),
              [&](const InFlight &a, const InFlight &b) {
                  if (conditions.reorder && a.due != b.due) return a.due < b.due;
                  return a.order < b.order;
              });

    for (InFlight &f : ready) {
        for (auto &e : endpoints_) {
            if (e->local().port != f.to.port) continue;
            Datagram d;
            d.from = f.from;
            d.bytes = std::move(f.bytes);
            e->deliver(std::move(d));
            delivered++;
            break;
        }
    }
}

void SimulatedNetwork::reset() {
    in_flight_.clear();
    rng_ = conditions.seed;
    order_ = 0;
    sent = delivered = dropped = duplicated = 0;
}

}  // namespace mf::net
