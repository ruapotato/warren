// Manifold -- the net stack, over a network that misbehaves.
//
// A reliability layer works perfectly on localhost. Loss, reordering
// and duplication are the entire reason it exists and none of them
// happen between two processes on one machine, so every interesting
// path in it is one that a loopback test never runs.
//
// So the transport here is a simulator with a seeded generator: one
// packet in three is dropped, one in ten arrives twice, and the
// delivery order is whatever the jitter makes it. The same seed gives
// the same abuse every run, so a failure is reproducible.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/serialize.h"
#include "net/connection.h"
#include "net/replication.h"
#include "scene/nodes.h"
#include "scene/scene_tree.h"
#include "net/socket.h"

using namespace mf;
using namespace mf::net;

namespace {

int g_fail = 0, g_checks = 0;
void check(bool ok, const char *what) {
    g_checks++;
    if (!ok) {
        std::printf("  FAIL  %s\n", what);
        g_fail++;
    }
}

struct Peer {
    Connection conn;
    std::vector<std::string> reliable;
    std::vector<std::string> unreliable;
    std::vector<std::string> sequenced;

    void wire() {
        conn.on_message = [this](Channel c, const uint8_t *d, size_t n) {
            std::string s((const char *)d, n);
            if (c == Channel::Reliable) reliable.push_back(std::move(s));
            else if (c == Channel::Sequenced) sequenced.push_back(std::move(s));
            else unreliable.push_back(std::move(s));
        };
    }
};

}  // namespace

int main() {
    log_set_level(LogLevel::Warn);
    std::printf("networking\n");

    // ------------------------------------------------ sequence numbers
    {
        check(sequence_newer(2, 1), "2 is newer than 1");
        check(!sequence_newer(1, 2), "and 1 is not newer than 2");
        // THE WRAP, which is the whole reason this is a function.
        check(sequence_newer(0, 65535), "0 is newer than 65535 (it wrapped)");
        check(!sequence_newer(65535, 0), "and 65535 is not newer than 0");
        check(sequence_newer(100, 65000), "100 is newer than 65000");
        check(!sequence_newer(65000, 100), "and not the other way round");
    }

    // ------------------------------------------------------- addresses
    {
        Address a = Address::loopback(7777);
        check(a.port == 7777 && a.valid(), "a loopback address has its port");
        check(a.to_string() == "127.0.0.1:7777", "and prints as dotted quad");
        Address p;
        check(Address::parse("127.0.0.1:1234", &p) && p.port == 1234,
              "an address parses back");
        check(!Address::parse("nonsense", &p), "and rubbish does not");
    }

    // ------------------------------------------- a real socket, briefly
    {
        UdpTransport a, b;
        if (a.open(0) && b.open(0)) {
            const char *msg = "hello";
            check(a.send(Address::loopback(b.local().port), msg, 5),
                  "a real udp socket sends");
            Datagram d;
            // Loopback is fast but not instant.
            bool got = false;
            for (int i = 0; i < 1000 && !got; i++) got = b.receive(&d);
            check(got && d.bytes.size() == 5 &&
                      std::memcmp(d.bytes.data(), msg, 5) == 0,
                  "and the other end receives it");
        } else {
            std::printf("  (no udp available; skipping the socket check)\n");
        }
    }

    // --------------------------------- reliability over a bad network
    {
        SimulatedNetwork net;
        net.conditions.loss = 0.33f;        // one in three, gone
        net.conditions.duplicate = 0.1f;
        net.conditions.latency = 0.03;
        net.conditions.jitter = 0.02;
        net.conditions.reorder = true;
        net.conditions.seed = 987654321u;
        net.reset();

        Peer server, client;
        server.wire();
        client.wire();
        ConnectionConfig cfg;
        cfg.resend_min = 0.02;
        server.conn.open(net.endpoint(7000), Address::loopback(7001), cfg);
        client.conn.open(net.endpoint(7001), Address::loopback(7000), cfg);

        // A hundred reliable messages, sent while the link eats a
        // third of everything.
        const int kMessages = 100;
        int sent = 0;
        double now = 0.0;
        for (int step = 0; step < 4000; step++) {
            now += 1.0 / 240.0;
            if (sent < kMessages && step % 3 == 0) {
                char body[32];
                std::snprintf(body, sizeof(body), "msg%d", sent);
                if (server.conn.send(Channel::Reliable, body,
                                     std::strlen(body)))
                    sent++;
            }
            net.poll(now);
            server.conn.update(now);
            client.conn.update(now);
            if (int(client.reliable.size()) >= kMessages && sent == kMessages)
                break;
        }

        std::printf("  simulated link: %llu sent, %llu delivered, %llu "
                    "dropped, %llu duplicated\n",
                    (unsigned long long)net.sent,
                    (unsigned long long)net.delivered,
                    (unsigned long long)net.dropped,
                    (unsigned long long)net.duplicated);
        std::printf("  %s\n", server.conn.report().c_str());

        char what[200];
        std::snprintf(what, sizeof(what),
                      "every reliable message arrived (%zu of %d)",
                      client.reliable.size(), kMessages);
        check(int(client.reliable.size()) == kMessages, what);

        // IN ORDER, AND EXACTLY ONCE. Duplicates are the half of
        // this that a loss-only simulator never tests: the link
        // sends one packet in ten twice, and a retransmission of an
        // already-delivered message is itself a duplicate.
        bool ordered = true, exact = true;
        for (size_t i = 0; i < client.reliable.size(); i++) {
            char expect[32];
            std::snprintf(expect, sizeof(expect), "msg%zu", i);
            if (client.reliable[i] != expect) ordered = false;
        }
        std::sort(client.reliable.begin(), client.reliable.end());
        for (size_t i = 1; i < client.reliable.size(); i++)
            if (client.reliable[i] == client.reliable[i - 1]) exact = false;
        check(ordered, "in the order they were sent");
        check(exact, "and none of them twice");

        std::snprintf(what, sizeof(what),
                      "and the link really was lossy (%.0f%% dropped)",
                      100.0 * double(net.dropped) / double(net.sent ? net.sent : 1));
        check(net.dropped > net.sent / 5, what);

        // AND IT DID NOT FLAIL DOING IT. A stack that delivers
        // everything by retransmitting everything ten times is
        // correct and unusable, and nothing else in this test would
        // notice: measured at 844 retransmissions for these 100
        // messages before the acknowledgement scheme was fixed.
        std::snprintf(what, sizeof(what),
                      "without flooding the link (%llu retransmissions for %d "
                      "messages over a third-lossy link)",
                      (unsigned long long)server.conn.resent_packets(),
                      kMessages);
        check(server.conn.resent_packets() < uint64_t(kMessages) * 2, what);

        std::snprintf(what, sizeof(what), "the round trip was measured (%.1f ms)",
                      server.conn.round_trip() * 1000.0);
        check(server.conn.round_trip() > 0.01 && server.conn.round_trip() < 0.5,
              what);
    }

    // ----------------------------------- sequenced drops the stale ones
    {
        SimulatedNetwork net;
        net.conditions.loss = 0.0f;
        net.conditions.latency = 0.01;
        net.conditions.jitter = 0.05;     // enough to reorder heavily
        net.conditions.reorder = true;
        net.conditions.seed = 424242u;
        net.reset();

        Peer a, b;
        a.wire();
        b.wire();
        a.conn.open(net.endpoint(7100), Address::loopback(7101));
        b.conn.open(net.endpoint(7101), Address::loopback(7100));

        double now = 0.0;
        const int kSnapshots = 200;
        for (int i = 0; i < kSnapshots; i++) {
            now += 1.0 / 60.0;
            char body[32];
            std::snprintf(body, sizeof(body), "%d", i);
            a.conn.send(Channel::Sequenced, body, std::strlen(body));
            net.poll(now);
            a.conn.update(now);
            b.conn.update(now);
        }
        for (int i = 0; i < 200; i++) {
            now += 1.0 / 60.0;
            net.poll(now);
            a.conn.update(now);
            b.conn.update(now);
        }

        // Some will have been thrown away for being stale. What must
        // never happen is one arriving that is older than one
        // already delivered -- a snapshot that winds the world
        // backwards for a frame and then jumps forward again.
        bool monotonic = true;
        int last = -1;
        for (const std::string &s : b.sequenced) {
            const int v = std::atoi(s.c_str());
            if (v <= last) monotonic = false;
            last = v;
        }
        char what[200];
        std::snprintf(what, sizeof(what),
                      "sequenced snapshots never go backwards (%zu of %d "
                      "delivered, newest %d)",
                      b.sequenced.size(), kSnapshots, last);
        check(monotonic && !b.sequenced.empty(), what);
        std::snprintf(what, sizeof(what),
                      "and the stale ones were dropped, not queued (%zu "
                      "dropped)",
                      size_t(kSnapshots) - b.sequenced.size());
        check(b.sequenced.size() < size_t(kSnapshots), what);
    }

    // --------------------------------------- a peer that goes away
    {
        SimulatedNetwork net;
        net.conditions.seed = 1u;
        net.reset();
        Peer a, b;
        a.wire();
        b.wire();
        ConnectionConfig cfg;
        cfg.timeout = 0.5;
        a.conn.open(net.endpoint(7200), Address::loopback(7201), cfg);
        b.conn.open(net.endpoint(7201), Address::loopback(7200), cfg);

        double now = 0.0;
        bool timed_out = false;
        a.conn.on_timeout = [&]() { timed_out = true; };
        for (int i = 0; i < 60; i++) {
            now += 1.0 / 60.0;
            net.poll(now);
            a.conn.update(now);
            b.conn.update(now);
        }
        check(a.conn.connected() && !timed_out, "a live peer stays connected");

        // b stops answering.
        for (int i = 0; i < 120; i++) {
            now += 1.0 / 60.0;
            net.poll(now);
            a.conn.update(now);
        }
        check(timed_out && !a.conn.connected(),
              "and a silent one times out");
    }

    // ----------------------------------------------- serialisation
    {
        ClassDB::register_all();
        ByteWriter w;
        w.variant(Variant(int64_t(-42)));
        w.variant(Variant(Vec3(1, 2, 3)));
        w.variant(Variant(std::string("hello")));
        w.variant(Variant(Quat::from_axis_angle(Vec3(0, 1, 0), 1.2f)));
        w.variant(Variant(Transform3D(Basis::identity(), Vec3(4, 5, 6))));
        ByteReader r(w.bytes);
        check(r.variant().to_int() == -42, "an int survives the wire");
        check(r.variant().to_vec3() == Vec3(1, 2, 3), "and a vector");
        check(r.variant().to_string() == "hello", "and a string");
        const Quat q = r.variant().to_quat();
        check(std::fabs(q.y) > 0.1f, "and a rotation");
        check(r.variant().to_transform().origin == Vec3(4, 5, 6),
              "and a transform");
        check(r.ok() && r.left() == 0, "with nothing left over");

        // A TRUNCATED PACKET MUST NOT INVENT DATA. Half a message is
        // what a hostile peer sends and what a bug produces, and a
        // reader that runs off the end quietly is a reader that
        // hands the game numbers nobody wrote.
        ByteReader cut(w.bytes.data(), 3);
        cut.variant();
        check(!cut.ok(), "a truncated stream reports failure");

        // And a length field is an allocation request from a
        // stranger, so it is bounded by what is actually there.
        ByteWriter evil;
        evil.u8(uint8_t(VType::Array));
        evil.u32(0xFFFFFFFFu);
        ByteReader er(evil.bytes);
        er.variant();
        check(!er.ok(), "and an absurd length is refused, not allocated");
    }

    // ------------------------------- a replicated world over a bad link
    {
        SimulatedNetwork net;
        net.conditions.loss = 0.2f;
        net.conditions.latency = 0.04;
        net.conditions.jitter = 0.03;
        net.conditions.seed = 5150u;
        net.reset();

        // The server's world: three cubes on the move.
        SceneTree server_tree;
        Node3D *server_scene = new Node3D();
        server_scene->set_name("Scene");
        server_tree.set_scene(server_scene);
        std::vector<Node3D *> movers;
        for (int i = 0; i < 3; i++) {
            Node3D *n = new Node3D();
            char name[16];
            std::snprintf(name, sizeof(name), "mover%d", i);
            n->set_name(name);
            server_scene->add_child(n);
            NetSync *s = new NetSync();
            s->set_name("NetSync");
            s->spawn_class = "Node3D";
            s->properties = {"position", "rotation"};
            n->add_child(s);
            movers.push_back(n);
        }

        SceneTree client_tree;
        Node3D *client_scene = new Node3D();
        client_scene->set_name("Scene");
        client_tree.set_scene(client_scene);

        Replicator server, client;
        server.init(&server_tree, Replicator::Role::Server);
        client.init(&client_tree, Replicator::Role::Client);

        Peer sp, cp;
        sp.wire();
        cp.wire();
        Connection sc, cc;
        std::vector<std::vector<uint8_t>> client_snapshots;
        std::vector<std::vector<uint8_t>> client_spawns;
        cc.on_message = [&](Channel ch, const uint8_t *d, size_t n) {
            if (ch == Channel::Reliable)
                client_spawns.emplace_back(d, d + n);
            else
                client_snapshots.emplace_back(d, d + n);
        };
        sc.open(net.endpoint(7300), Address::loopback(7301));
        cc.open(net.endpoint(7301), Address::loopback(7300));

        // The spawn list goes reliably, once.
        const std::vector<uint8_t> spawns = server.build_spawns();
        sc.send(Channel::Reliable, spawns);

        double now = 0.0;
        for (int tick = 0; tick < 400; tick++) {
            now += 1.0 / 60.0;
            // The server moves its world and publishes it.
            for (size_t i = 0; i < movers.size(); i++) {
                const float t = float(tick) * 0.02f + float(i);
                movers[i]->set_position(
                    Vec3(std::cos(t) * 5.0f, float(i), std::sin(t) * 5.0f));
                movers[i]->set_rotation(
                    Quat::from_axis_angle(Vec3(0, 1, 0), t));
            }
            if (tick % 3 == 0) {
                const std::vector<uint8_t> snap =
                    server.build_snapshot(uint32_t(tick));
                sc.send(Channel::Sequenced, snap);
            }

            net.poll(now);
            sc.update(now);
            cc.update(now);

            for (auto &s : client_spawns) client.apply_spawns(s.data(), s.size());
            client_spawns.clear();
            for (auto &s : client_snapshots)
                client.apply_snapshot(s.data(), s.size());
            client_snapshots.clear();
            client.interpolate(1.0f / 60.0f);
        }

        char what[220];
        std::snprintf(what, sizeof(what),
                      "the client spawned what the server has (%zu of %zu)",
                      client.tracked(), movers.size());
        check(client.tracked() == movers.size(), what);

        // AND THE WORLDS AGREE -- but "agree" needs saying carefully.
        //
        // The client is deliberately behind: it interpolates towards
        // the newest snapshot rather than snapping to it, so at 6 m/s
        // with a 50 ms snapshot interval and 40 ms of latency it sits
        // about half a metre back along the path. That is the feature
        // working, not error, and a test that called it error would
        // be measuring the latency of the simulated link.
        //
        // What is error is being off the path. These movers run on a
        // circle of radius 5, so the distance from the origin says
        // how far the client's idea of the world has drifted from the
        // server's, with the lag divided out.
        double worst = 0.0;      // raw separation, which is mostly lag
        double worst_radius = 0.0;  // and this, which is not
        int compared = 0;
        for (Node3D *m : movers) {
            NetSync *s = nullptr;
            for (const auto &c : m->children())
                if (c && (s = c->cast_to<NetSync>())) break;
            if (!s || !s->net_id) continue;
            Node *mirror = nullptr;
            char name[48];
            std::snprintf(name, sizeof(name), "Node3D_%u", s->net_id);
            mirror = client_tree.root()->find_child(name);
            if (!mirror) continue;
            Node3D *m3 = mirror->cast_to<Node3D>();
            if (!m3) continue;
            worst = std::max(worst,
                             double((m3->position() - m->position()).length()));
            const Vec3 p = m3->position();
            const double radius = std::sqrt(double(p.x) * p.x + double(p.z) * p.z);
            worst_radius = std::max(worst_radius, std::fabs(radius - 5.0));
            compared++;
        }
        std::printf("  replication: %d nodes mirrored over a fifth-lossy "
                    "link\n    %.3f m behind the server (interpolation lag "
                    "at 6 m/s), %.3f m off its path\n",
                    compared, worst, worst_radius);
        check(compared == int(movers.size()),
              "every replicated node has a counterpart");
        std::snprintf(what, sizeof(what),
                      "the client follows within one snapshot of lag (%.3f m "
                      "at 6 m/s)",
                      worst);
        check(compared > 0 && worst < 0.9, what);
        std::snprintf(what, sizeof(what),
                      "and stays ON the server's path, which lag does not "
                      "explain away (%.3f m off a 5 m circle)",
                      worst_radius);
        check(compared > 0 && worst_radius < 0.15, what);
    }

    std::printf("  %d checks\n%s\n", g_checks, g_fail ? "FAILED" : "ok");
    return g_fail ? 1 : 0;
}
