#include "net/connection.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "core/log.h"

namespace wr::net {

void Connection::open(Transport *transport, const Address &peer,
                      const ConnectionConfig &cfg) {
    transport_ = transport;
    peer_ = peer;
    cfg_ = cfg;
    connected_ = transport != nullptr;
    next_sequence_ = 1;
    remote_sequence_ = 0;
    received_bits_ = 0;
    had_remote_ = false;
    reliable_out_ = 0;
    reliable_next_in_ = 0;
    reliable_held_.clear();
    pending_.clear();
    had_sequenced_ = false;
    unacked_ = 0;
    now_ = 0.0;
    last_received_ = 0.0;
    last_sent_ = 0.0;
    sent_ = received_ = resent_ = lost_ = 0;
}

void Connection::close() {
    connected_ = false;
    pending_.clear();
    reliable_held_.clear();
}

void Connection::note_received(uint16_t sequence) {
    if (!had_remote_) {
        remote_sequence_ = sequence;
        received_bits_ = 0;
        had_remote_ = true;
        return;
    }
    if (sequence_newer(sequence, remote_sequence_)) {
        const uint16_t shift = uint16_t(sequence - remote_sequence_);
        received_bits_ = shift >= 32 ? 0u : ((received_bits_ << shift) | (1u << (shift - 1)));
        remote_sequence_ = sequence;
    } else {
        const uint16_t back = uint16_t(remote_sequence_ - sequence);
        if (back >= 1 && back <= 32) received_bits_ |= 1u << (back - 1);
    }
}

void Connection::send_raw(Channel channel, const void *payload, size_t size,
                          uint8_t flags, uint16_t *out_sequence) {
    if (!transport_) return;
    std::vector<uint8_t> packet(sizeof(PacketHeader) + size);
    PacketHeader h{};
    h.protocol = cfg_.protocol;
    h.sequence = next_sequence_++;
    if (next_sequence_ == 0) next_sequence_ = 1;
    h.ack = remote_sequence_;
    h.ack_bits = received_bits_;
    h.reliable_ack = reliable_next_in_;
    h.reliable_ack_bits = held_bits();
    h.channel = uint8_t(channel);
    h.flags = flags;
    std::memcpy(packet.data(), &h, sizeof(h));
    if (size) std::memcpy(packet.data() + sizeof(h), payload, size);
    transport_->send(peer_, packet.data(), packet.size());
    sent_++;
    // EVERY packet carries the ack state, so any send at all is an
    // ack and resets both the keep-alive timer and the backlog.
    last_sent_ = now_;
    unacked_ = 0;
    if (out_sequence) *out_sequence = h.sequence;

    if (channel == Channel::Reliable) {
        if (pending_.size() >= cfg_.max_pending) {
            WR_WARN("net: %zu reliable messages unacked; dropping the oldest",
                    pending_.size());
            pending_.pop_front();
        }
        Pending p;
        p.sequence = h.sequence;
        p.bytes = std::move(packet);
        p.attempts = 1;
        p.sent_at = now_;
        // The id is the first four bytes of the body, which send()
        // wrote; keeping a copy is what lets a cumulative ack find
        // it without re-parsing the packet.
        if (size >= 4) std::memcpy(&p.id, payload, 4);
        pending_.push_back(std::move(p));
    }
}

bool Connection::send(Channel channel, const void *data, size_t size) {
    if (!connected_ || !transport_) return false;
    const size_t room = kMaxDatagram - sizeof(PacketHeader);

    if (channel == Channel::Reliable) {
        // CHECKED BEFORE ANYTHING IS ALLOCATED OR COPIED. Rejecting
        // a message after building it wastes the allocation, and it
        // also leaves the compiler unable to see that the memcpy
        // below is bounded -- which it says so about.
        if (size + 4 > room) {
            WR_ERROR("net: a reliable message of %zu bytes exceeds the %zu a "
                     "datagram holds; fragmentation is not implemented",
                     size, room - 4);
            return false;
        }
        // A reliable message carries its own ordinal, because packet
        // sequence numbers are per packet and a retransmission gets a
        // new one -- so the receiver cannot order by them.
        std::vector<uint8_t> body(4 + size);
        const uint32_t id = reliable_out_++;
        std::memcpy(body.data(), &id, 4);
        if (size) std::memcpy(body.data() + 4, data, size);
        send_raw(channel, body.data(), body.size(), 0, nullptr);
        return true;
    }

    if (size > room) {
        WR_ERROR("net: a %zu byte message exceeds the %zu a datagram holds",
                 size, room);
        return false;
    }
    send_raw(channel, data, size, 0, nullptr);
    return true;
}

void Connection::handle_acks(uint16_t ack, uint32_t ack_bits, double now) {
    auto acked = [&](uint16_t seq) {
        if (seq == ack) return true;
        const uint16_t back = uint16_t(ack - seq);
        return back >= 1 && back <= 32 && (ack_bits & (1u << (back - 1)));
    };
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (!acked(it->sequence)) {
            ++it;
            continue;
        }
        // ROUND TRIP FROM THE FIRST SEND, not the last. Measuring
        // from a retransmission makes a lossy link look fast, which
        // then shortens the resend timer and makes it lossier.
        if (it->attempts == 1) {
            const double sample = now - it->sent_at;
            if (sample >= 0.0 && sample < 5.0)
                rtt_ = rtt_ * 0.9 + sample * 0.1;
        }
        it = pending_.erase(it);
    }
}

uint32_t Connection::held_bits() const {
    uint32_t bits = 0;
    for (const auto &held : reliable_held_) {
        const uint32_t offset = held.first - reliable_next_in_;
        if (offset < 32) bits |= 1u << offset;
    }
    return bits;
}

void Connection::handle_reliable_ack(uint32_t up_to, uint32_t bits,
                                     double now) {
    while (!pending_.empty() && pending_.front().id < up_to) {
        const Pending &p = pending_.front();
        if (p.attempts == 1) {
            const double sample = now - p.sent_at;
            if (sample >= 0.0 && sample < 5.0)
                rtt_ = rtt_ * 0.9 + sample * 0.1;
        }
        pending_.pop_front();
    }
    // Everything below the cumulative point, plus anything the
    // receiver said it is already holding.
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                  [&](const Pending &p) {
                                      if (p.id < up_to) return true;
                                      const uint32_t offset = p.id - up_to;
                                      return offset < 32 &&
                                             (bits & (1u << offset)) != 0;
                                  }),
                   pending_.end());
}

void Connection::deliver(const Datagram &d, double now) {
    if (d.bytes.size() < sizeof(PacketHeader)) return;
    PacketHeader h{};
    std::memcpy(&h, d.bytes.data(), sizeof(h));
    if (h.protocol != cfg_.protocol) return;   // not ours

    received_++;
    unacked_++;
    last_received_ = now;
    connected_ = true;
    note_received(h.sequence);
    handle_acks(h.ack, h.ack_bits, now);
    handle_reliable_ack(h.reliable_ack, h.reliable_ack_bits, now);

    const uint8_t *body = d.bytes.data() + sizeof(h);
    const size_t size = d.bytes.size() - sizeof(h);
    if (h.flags & kFlagKeepAlive) return;
    if (!size) return;

    const Channel channel = Channel(h.channel);
    switch (channel) {
        case Channel::Unreliable:
            if (on_message) on_message(channel, body, size);
            break;

        case Channel::Sequenced:
            // An older snapshot arriving after a newer one is worse
            // than no snapshot at all: it winds the world backwards
            // for one frame and then jumps forward again.
            if (had_sequenced_ && !sequence_newer(h.sequence, sequenced_in_))
                return;
            sequenced_in_ = h.sequence;
            had_sequenced_ = true;
            if (on_message) on_message(channel, body, size);
            break;

        case Channel::Reliable: {
            if (size < 4) return;
            uint32_t id = 0;
            std::memcpy(&id, body, 4);
            // A retransmission of something already delivered.
            if (id < reliable_next_in_) return;
            if (id == reliable_next_in_) {
                if (on_message) on_message(channel, body + 4, size - 4);
                reliable_next_in_++;
                // And anything that arrived early and has been
                // waiting for this one.
                bool progressed = true;
                while (progressed) {
                    progressed = false;
                    for (auto it = reliable_held_.begin();
                         it != reliable_held_.end(); ++it) {
                        if (it->first != reliable_next_in_) continue;
                        if (on_message)
                            on_message(channel, it->second.data(),
                                       it->second.size());
                        reliable_next_in_++;
                        reliable_held_.erase(it);
                        progressed = true;
                        break;
                    }
                }
            } else {
                for (const auto &held : reliable_held_)
                    if (held.first == id) return;   // already waiting
                reliable_held_.emplace_back(
                    id, std::vector<uint8_t>(body + 4, body + size));
            }
            break;
        }
        default:
            break;
    }
}

void Connection::update(double now) {
    if (!transport_) return;
    now_ = now;
    if (last_received_ == 0.0) last_received_ = now;

    // Anything the transport has for us that is from our peer.
    Datagram d;
    while (transport_->receive(&d)) {
        if (d.from != peer_) continue;
        deliver(d, now);
    }

    // Retransmit what has not been acked. The timer is the measured
    // round trip with a floor, so a fast link retries quickly and a
    // slow one does not flood itself with duplicates.
    const double wait = std::max(cfg_.resend_min, rtt_ * 1.5);
    int budget = cfg_.max_resends_per_update;
    for (Pending &p : pending_) {
        if (budget <= 0) break;
        if (now - p.sent_at < wait) continue;
        if (p.attempts >= cfg_.max_resends) continue;
        budget--;
        // A NEW SEQUENCE NUMBER FOR THE RETRANSMISSION. The ack
        // scheme acks packets, not messages; reusing the number
        // would make one ack cover two different sends and the
        // round-trip estimate meaningless.
        PacketHeader h{};
        std::memcpy(&h, p.bytes.data(), sizeof(h));
        h.sequence = next_sequence_++;
        if (next_sequence_ == 0) next_sequence_ = 1;
        h.ack = remote_sequence_;
        h.ack_bits = received_bits_;
        std::memcpy(p.bytes.data(), &h, sizeof(h));
        transport_->send(peer_, p.bytes.data(), p.bytes.size());
        p.sequence = h.sequence;
        p.sent_at = now;
        p.attempts++;
        sent_++;
        resent_++;
    }
    // A packet now and then even with nothing to say: the ack
    // stream is what tells the other end we are still here, and
    // what confirms its reliable messages. And immediately once
    // enough have piled up unacknowledged -- see ack_after.
    if (unacked_ >= cfg_.ack_after || now - last_sent_ >= cfg_.keep_alive)
        send_raw(Channel::Unreliable, nullptr, 0, kFlagKeepAlive, nullptr);

    if (connected_ && now - last_received_ > cfg_.timeout) {
        connected_ = false;
        if (on_timeout) on_timeout();
    }
}

std::string Connection::report() const {
    char b[224];
    std::snprintf(b, sizeof(b),
                  "net: %s, rtt %.1f ms, %llu sent (%llu resent), %llu "
                  "received, %zu pending",
                  connected_ ? "connected" : "disconnected", rtt_ * 1000.0,
                  (unsigned long long)sent_, (unsigned long long)resent_,
                  (unsigned long long)received_, pending_.size());
    return b;
}

}  // namespace wr::net
