// Manifold -- one peer talking to another over datagrams.
//
// UDP gives you "sometimes, in any order, possibly twice". A game
// needs three different things out of that and a stack that offers
// only one of them makes the other two somebody else's problem:
//
//   Unreliable        a snapshot. If it is lost, the next one is
//                     along in 50 ms and is better anyway, so
//                     retransmitting it would waste the bandwidth
//                     that carries the newer one.
//   Sequenced         also a snapshot, but never delivered out of
//                     order: an older one arriving late is dropped
//                     rather than winding the world backwards.
//   Reliable ordered   a chat line, a spawn, "you died". Must
//                     arrive, must arrive in order, and the cost of
//                     that is a queue and a retransmit timer.
//
// Acknowledgement is Glenn Fiedler's: every packet carries the
// newest sequence the sender has seen from the other side and a
// bitfield of the 32 before it, so one ack packet confirms up to 33
// and a lost ack costs nothing as long as the next one arrives.
// There is no separate ack packet and no ack timer.
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "net/socket.h"

namespace mf::net {

enum class Channel : uint8_t {
    Unreliable = 0,
    Sequenced = 1,
    Reliable = 2,
    Count
};

// On the wire, little endian, 12 bytes.
#pragma pack(push, 1)
struct PacketHeader {
    uint16_t protocol;      // rejects stray traffic and old builds
    uint16_t sequence;      // of this packet
    uint16_t ack;           // newest sequence seen from the peer
    uint32_t ack_bits;      // and the 32 before it
    // CUMULATIVE ACK FOR THE RELIABLE STREAM: "I have every reliable
    // message with an id below this one."
    //
    // The packet ack above is Fiedler's and is the right shape for
    // measuring a link -- round trip, loss -- but it is the wrong
    // shape for confirming delivery, because it reaches only 32
    // packets back. A peer sending eighty packets a second while the
    // other answers ten times a second has moved past the window
    // before any ack arrives, so its oldest messages are never
    // confirmed and are retransmitted until they hit the attempt
    // limit: measured at 694 retransmissions to deliver 100
    // messages. One number that says "everything below here" cannot
    // fall out of a window, and it collapses to 4.
    uint32_t reliable_ack;
    // AND WHICH OF THE NEXT 32 HAVE ALSO ARRIVED. A cumulative ack
    // alone leaves head-of-line waste: messages 2..40 are sitting in
    // the receiver's hold queue, complete, while message 1 is still
    // lost -- and the sender, told only "I have everything below 1",
    // retransmits all thirty-nine of them on every timer. This says
    // which ones not to bother with.
    uint32_t reliable_ack_bits;
    uint8_t channel;
    uint8_t flags;
};
#pragma pack(pop)
static_assert(sizeof(PacketHeader) == 20, "the wire format is 20 bytes");

constexpr uint8_t kFlagFragment = 1 << 0;
constexpr uint8_t kFlagKeepAlive = 1 << 1;

struct ConnectionConfig {
    uint16_t protocol = 0x4D46;     // 'MF'
    // How long without a packet before the peer is declared gone.
    double timeout = 5.0;
    // A packet every this often even with nothing to say, so the
    // ack stream keeps flowing and the timeout means something.
    double keep_alive = 0.1;
    // AND ONE IMMEDIATELY ONCE THIS MANY ARE WAITING TO BE ACKED.
    //
    // The ack bitfield reaches 32 packets back. A peer that sends 80
    // a second while the other answers 10 times a second has, by the
    // time an ack arrives, moved more than 32 packets on -- so the
    // oldest fall outside the window, are never acknowledged, and
    // are retransmitted until they hit the attempt limit. Measured:
    // 844 retransmissions to deliver 100 messages. Acking promptly
    // when traffic is one-sided costs an empty datagram and fixes
    // it; the keep-alive timer then only matters when nothing is
    // happening at all.
    int ack_after = 8;
    // How long to wait for an ack before sending a reliable message
    // again. Multiplied by the measured round trip, floored here.
    double resend_min = 0.05;
    int max_resends = 64;
    // AND HOW MANY MAY GO OUT IN ONE UPDATE. Without a cap, a
    // hundred unacked messages all time out together and the burst
    // pushes the peer's ack window forward faster than its acks can
    // come back -- the stack making its own problem worse.
    int max_resends_per_update = 8;
    // Reliable messages waiting for an ack. Past this, sending
    // fails rather than growing without bound -- a peer that cannot
    // keep up should be disconnected, not buffered for ever.
    size_t max_pending = 512;
};

class Connection {
public:
    void open(Transport *transport, const Address &peer,
              const ConnectionConfig &cfg = {});
    void close();
    bool connected() const { return connected_; }
    const Address &peer() const { return peer_; }

    // Queue a message. Reliable messages are kept until acked;
    // everything else is sent once, now.
    bool send(Channel channel, const void *data, size_t size);
    bool send(Channel channel, const std::vector<uint8_t> &bytes) {
        return send(channel, bytes.data(), bytes.size());
    }

    // Pump: read what arrived, hand each complete message to
    // `on_message`, retransmit what has not been acked, and send a
    // keep-alive if there is nothing else to say.
    void update(double now);

    std::function<void(Channel, const uint8_t *, size_t)> on_message;
    std::function<void()> on_timeout;

    // --- what the connection knows about itself ---------------------
    double round_trip() const { return rtt_; }
    float packet_loss() const { return loss_; }
    uint64_t sent_packets() const { return sent_; }
    uint64_t received_packets() const { return received_; }
    uint64_t resent_packets() const { return resent_; }
    size_t pending_reliable() const { return pending_.size(); }
    std::string report() const;

    // Feed a datagram that something else already read off the
    // transport -- a server demultiplexing one socket across many
    // connections.
    void deliver(const Datagram &d, double now);

private:
    struct Pending {
        uint32_t id = 0;            // the reliable message's own ordinal
        uint16_t sequence = 0;
        std::vector<uint8_t> bytes;   // header included, ready to resend
        double sent_at = 0.0;
        int attempts = 0;
    };

    void send_raw(Channel channel, const void *payload, size_t size,
                  uint8_t flags, uint16_t *out_sequence);
    void handle_acks(uint16_t ack, uint32_t ack_bits, double now);
    void handle_reliable_ack(uint32_t up_to, uint32_t bits, double now);
    uint32_t held_bits() const;
    void note_received(uint16_t sequence);

    Transport *transport_ = nullptr;
    Address peer_;
    ConnectionConfig cfg_;
    bool connected_ = false;

    uint16_t next_sequence_ = 1;
    uint16_t remote_sequence_ = 0;
    uint32_t received_bits_ = 0;
    bool had_remote_ = false;

    // Reliable delivery. `reliable_out_` numbers messages so the
    // receiver can order them; `reliable_in_` holds the ones that
    // arrived early.
    uint32_t reliable_out_ = 0;
    uint32_t reliable_next_in_ = 0;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> reliable_held_;
    std::deque<Pending> pending_;

    // The newest sequenced message accepted, so an older one that
    // arrives late can be dropped.
    uint16_t sequenced_in_ = 0;
    bool had_sequenced_ = false;

    double last_received_ = 0.0;
    double last_sent_ = 0.0;
    double now_ = 0.0;
    int unacked_ = 0;
    double rtt_ = 0.05;
    float loss_ = 0.0f;
    uint64_t sent_ = 0, received_ = 0, resent_ = 0, lost_ = 0;
};

// True when `a` is newer than `b` in a 16-bit sequence space that
// wraps. Half the space is "newer", which is the standard answer and
// is correct as long as the two ends never drift more than 32767
// packets apart -- at 60 packets a second that is nine minutes of
// total silence, by which time the timeout has fired.
inline bool sequence_newer(uint16_t a, uint16_t b) {
    return (a > b && a - b <= 0x8000) || (b > a && b - a > 0x8000);
}

}  // namespace mf::net
