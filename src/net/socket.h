// Manifold -- datagrams, and a way to abuse them on purpose.
//
// THE SIMULATED TRANSPORT IS NOT A TEST FIXTURE, IT IS THE POINT.
//
// A reliability layer is the kind of code that works perfectly on
// localhost and falls apart on a train. Loss, reordering, duplication
// and jitter are the entire reason it exists, and none of them happen
// between two processes on one machine -- so a net stack tested only
// over loopback is a net stack whose interesting paths have never
// run. Transport is therefore an interface with two implementations:
// UDP, and one that drops one packet in five and delivers the rest
// backwards.
//
// Portability: Winsock and BSD sockets differ in about six places,
// and all six are in socket.cpp.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mf::net {

// An IPv4 endpoint. IPv6 is not here yet and the struct is the only
// thing that would have to change.
struct Address {
    uint32_t host = 0;      // network order
    uint16_t port = 0;

    bool operator==(const Address &o) const {
        return host == o.host && port == o.port;
    }
    bool operator!=(const Address &o) const { return !(*this == o); }
    bool valid() const { return port != 0; }
    std::string to_string() const;

    static Address loopback(uint16_t port);
    static Address any(uint16_t port);
    // "1.2.3.4:5678" or "host:port". Blocking DNS, so not for a frame.
    static bool parse(const std::string &text, Address *out);
};

// The largest datagram the stack will send. Chosen to stay inside a
// 1500-byte Ethernet MTU with room for IP, UDP and a tunnel or two:
// a fragmented UDP datagram is one lost fragment away from being
// entirely lost, so the stack fragments at this level instead, where
// it can retransmit a piece rather than the whole.
constexpr size_t kMaxDatagram = 1200;

struct Datagram {
    Address from;
    std::vector<uint8_t> bytes;
};

class Transport {
public:
    virtual ~Transport() = default;
    // Non-blocking. False means the datagram was dropped by the
    // sender, which callers must treat as normal.
    virtual bool send(const Address &to, const void *data, size_t size) = 0;
    // Non-blocking; false when nothing is waiting.
    virtual bool receive(Datagram *out) = 0;
    virtual Address local() const = 0;
    // Advances anything time-based -- the simulator's delay queue.
    virtual void poll(double now) { (void)now; }
};

// A real UDP socket.
class UdpTransport final : public Transport {
public:
    ~UdpTransport() override;
    // Port 0 asks the system for one.
    bool open(uint16_t port);
    void close();
    bool is_open() const { return handle_ != -1; }

    bool send(const Address &to, const void *data, size_t size) override;
    bool receive(Datagram *out) override;
    Address local() const override { return local_; }

    uint64_t sent_packets = 0, sent_bytes = 0;
    uint64_t received_packets = 0, received_bytes = 0;

private:
    long long handle_ = -1;
    Address local_;
};

// Two endpoints wired together in one process, through as bad a
// network as the test asks for.
class SimulatedNetwork {
public:
    // Declared here and defined in the .cpp: the endpoint type is
    // incomplete at this point, and a unique_ptr to an incomplete
    // type needs its destructor where the type is whole.
    SimulatedNetwork();
    ~SimulatedNetwork();

    struct Conditions {
        float loss = 0.0f;           // 0..1, chance a packet vanishes
        float duplicate = 0.0f;      // chance it arrives twice
        double latency = 0.0;        // seconds, each way
        double jitter = 0.0;         // seconds, added uniformly
        // Deliver out of order when jitter allows it. Off by default
        // because a test that wants reordering should say so.
        bool reorder = true;
        uint32_t seed = 12345;
    };

    Conditions conditions;

    // A transport plugged into this network at `port`.
    Transport *endpoint(uint16_t port);
    void poll(double now);
    void reset();

    uint64_t sent = 0, delivered = 0, dropped = 0, duplicated = 0;

private:
    friend class SimulatedTransport;
    struct InFlight {
        Address to;
        Address from;
        std::vector<uint8_t> bytes;
        double due = 0.0;
        uint64_t order = 0;
    };
    void submit(const Address &from, const Address &to, const void *data,
                size_t size, double now);
    float random();

    std::vector<std::unique_ptr<class SimulatedTransport>> endpoints_;
    std::vector<InFlight> in_flight_;
    uint32_t rng_ = 12345;
    uint64_t order_ = 0;
};

}  // namespace mf::net
