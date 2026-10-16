// Transport UDP + KCP en boucle locale (story 1.2) : matrice I/O de la spec.
// Compilé dans client_tests (Linux) et dans net_tests.exe (Windows, lancé sous Wine).
// Chaque conclusion repose sur une attente active bornée par l'horloge, jamais sur un
// sleep fixe ; « rien n'est arrivé » se prouve par un datagramme témoin envoyé ensuite.
#include "net/transport.h"
#include "test.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>
#include <vector>

using namespace mp::net;

namespace {

#ifndef MP_VECTORS_DIR
#error "MP_VECTORS_DIR doit pointer vers protocol/vectors"
#endif

constexpr std::uint32_t kLoopback = 0x7f000001;
constexpr std::uint32_t kConv = 0x12345678;

using Clock = std::chrono::steady_clock;

Endpoint loopback(std::uint16_t port) { return Endpoint{kLoopback, port}; }

bool read_file(const std::string& path, Bytes& out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return false;
    std::uint8_t buf[512];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.insert(out.end(), buf, buf + n);
    std::fclose(f);
    return true;
}

Bytes load_vector(const char* name) {
    const std::string path = std::string(MP_VECTORS_DIR) + "/" + name;
    Bytes out;
    if (read_file(path, out)) return out;
#if defined(_WIN32)
    // Sous Wine, un chemin Unix absolu se lit via le lecteur Z:.
    out.clear();
    if (read_file("Z:" + path, out)) return out;
#endif
    std::printf("  vecteur introuvable : %s\n", path.c_str());
    ++::mptest::failures();
    return {};
}

// Attente active bornée : appelle `step` jusqu'à ce qu'il rende true ou que le délai expire.
bool wait_until(std::chrono::milliseconds timeout, const std::function<bool()>& step) {
    const auto deadline = Clock::now() + timeout;
    for (;;) {
        if (step()) return true;
        if (Clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

bool start_or_report(Transport& t, std::uint16_t port, const Options& options = {}) {
    std::string error;
    if (t.start(port, options, error)) return true;
    std::printf("  start(%u) a échoué : %s\n", port, error.c_str());
    ++::mptest::failures();
    return false;
}

// Datagramme de contrôle témoin : [canal 2][marqueur].
Bytes marker(std::uint8_t tag) { return Bytes{2, 0xEE, tag}; }

// Lit les événements de `t` jusqu'au témoin `tag` (borné à 2 s). Retourne le nombre
// d'événements reçus avant lui, ou -1 s'il n'est jamais arrivé.
int events_before_marker(Transport& t, std::uint8_t tag) {
    int before = -1;
    int seen = 0;
    const Bytes expected = marker(tag);
    wait_until(std::chrono::milliseconds(2000), [&] {
        Event e;
        while (t.poll(e)) {
            const auto* c = std::get_if<ControlReceived>(&e);
            if (c != nullptr && c->bytes == expected) {
                before = seen;
                return true;
            }
            ++seen;
        }
        return false;
    });
    return before;
}

// Segment KCP minimal (24 octets d'en-tête) portant `conv`, précédé du canal 0.
Bytes kcp_segment(std::uint32_t conv) {
    Bytes d(1 + 24, 0);
    for (int i = 0; i < 4; ++i) d[1 + i] = static_cast<std::uint8_t>(conv >> (8 * i));
    return d;
}

// Message fiable déterministe n°i : taille de 1 à 3000 octets, contenu dépendant de i et du sens.
Bytes make_message(int i, std::uint8_t direction) {
    const std::size_t size = 1 + static_cast<std::size_t>(i) * 2999 / 199;
    Bytes b(size);
    for (std::size_t k = 0; k < size; ++k) b[k] = static_cast<std::uint8_t>(k * 31 + i * 7 + direction);
    return b;
}

// Ouvre KCP des deux côtés, échange 200 messages dans chaque sens et vérifie l'ordre et les octets.
void exchange_reliable(const Options& options, std::chrono::milliseconds budget) {
    Transport a, b;
    if (!start_or_report(a, 0, options) || !start_or_report(b, 0, options)) return;
    a.post(OpenReliable{kConv, loopback(b.local_port())});
    b.post(OpenReliable{kConv, loopback(a.local_port())});

    constexpr int kCount = 200;
    for (int i = 0; i < kCount; ++i) {
        CHECK(a.post(SendReliable{make_message(i, 0xA0)}));
        CHECK(b.post(SendReliable{make_message(i, 0xB0)}));
    }

    int got_a = 0, got_b = 0;  // messages reçus par a (venant de b) et par b
    bool order_ok = true;
    auto drain = [&](Transport& t, int& got, std::uint8_t direction) {
        Event e;
        while (t.poll(e)) {
            const auto* r = std::get_if<ReliableReceived>(&e);
            if (r == nullptr) {
                order_ok = false;
                continue;
            }
            if (got >= kCount || r->bytes != make_message(got, direction)) order_ok = false;
            ++got;
        }
    };
    const auto t0 = Clock::now();
    const bool done = wait_until(budget, [&] {
        drain(a, got_a, 0xB0);
        drain(b, got_b, 0xA0);
        return got_a >= kCount && got_b >= kCount;
    });
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
    if (!done) std::printf("  reçus : a=%d b=%d en %lld ms\n", got_a, got_b, static_cast<long long>(elapsed));
    CHECK(done);
    CHECK(order_ok);
    CHECK_EQ(got_a, kCount);
    CHECK_EQ(got_b, kCount);

    for (Transport* t : {&a, &b}) {
        const Stats s = t->stats();
        CHECK(s.max_datagram_sent <= 1200);
        CHECK(s.max_datagram_sent >= 1000);  // les messages de 3000 octets sont fragmentés au MTU
        CHECK_EQ(s.too_big, 0u);
        CHECK_EQ(s.ignored_wrong_conv, 0u);
        CHECK_EQ(s.ignored_wrong_peer, 0u);
        CHECK_EQ(s.kcp_input_errors, 0u);
        CHECK_EQ(s.reliable_send_errors, 0u);
        if (options.drop_rate > 0.0) {
            CHECK(s.dropped_simulated > 0);
        } else {
            CHECK_EQ(s.dropped_simulated, 0u);
        }
    }
}

}  // namespace

TEST("net : parse_ipv4") {
    std::uint32_t ip = 0;
    CHECK(parse_ipv4("127.0.0.1", ip));
    CHECK_EQ(ip, 0x7f000001u);
    CHECK(parse_ipv4("255.255.255.255", ip));
    CHECK_EQ(ip, 0xffffffffu);
    CHECK(parse_ipv4("0.0.0.0", ip));
    CHECK_EQ(ip, 0u);
    ip = 42;
    for (const char* bad : {"", "1.2.3", "1.2.3.4.5", "256.0.0.1", "1.2.3.", "a.b.c.d", " 1.2.3.4",
                            "1.2.3.4 ", "01.2.3.4", "1..2.3", "1234.1.1.1", "localhost", "::1"}) {
        CHECK(!parse_ipv4(bad, ip));
    }
    CHECK_EQ(ip, 42u);
    CHECK_EQ(to_string(Endpoint{0xc0a80114, 27960}), std::string("192.168.1.20:27960"));
}

TEST("net : contrôle — hello.bin arrive intact avec la source") {
    const Bytes hello = load_vector("hello.bin");
    if (hello.empty()) return;
    Transport a, b;
    if (!start_or_report(a, 0) || !start_or_report(b, 0)) return;
    CHECK(a.local_port() != 0);
    CHECK(b.local_port() != 0);
    CHECK(a.local_port() != b.local_port());

    CHECK(a.post(SendControl{loopback(b.local_port()), hello}));
    ControlReceived got;
    bool received = false;
    CHECK(wait_until(std::chrono::milliseconds(2000), [&] {
        Event e;
        if (!b.poll(e)) return false;
        if (auto* c = std::get_if<ControlReceived>(&e)) {
            got = *c;
            received = true;
        }
        return true;
    }));
    CHECK(received);
    CHECK(got.bytes == hello);
    CHECK(got.source == loopback(a.local_port()));
    CHECK_EQ(a.stats().datagrams_sent, 1u);
}

TEST("net : fiable sans perte — 200 messages dans chaque sens, dans l'ordre") {
    exchange_reliable(Options{}, std::chrono::milliseconds(15000));
}

TEST("net : fiable avec 20 % de pertes — tout arrive en moins de 15 s") {
    Options lossy;
    lossy.drop_rate = 0.2;
    exchange_reliable(lossy, std::chrono::milliseconds(15000));
}

TEST("net : datagramme de 1201 octets refusé et compté") {
    Transport a, b;
    if (!start_or_report(a, 0) || !start_or_report(b, 0)) return;
    Bytes big(1201, 0x55);
    big[0] = 2;
    CHECK(a.post(SendControl{loopback(b.local_port()), big}));
    CHECK(a.post(SendControl{loopback(b.local_port()), marker(1)}));
    CHECK_EQ(events_before_marker(b, 1), 0);  // le témoin arrive, seul
    CHECK_EQ(a.stats().too_big, 1u);
    CHECK_EQ(a.stats().datagrams_sent, 1u);
    CHECK(a.stats().max_datagram_sent <= 1200);
}

TEST("net : canaux ignorés et comptés (1, 9, 0 sans KCP, autre adresse, autre conv)") {
    Transport b;
    if (!start_or_report(b, 0)) return;
    UdpSocket peer, stranger;  // pair légitime et tiers, en sockets bruts
    std::string error;
    CHECK(peer.open(0, error));
    CHECK(stranger.open(0, error));
    const Endpoint to_b = loopback(b.local_port());

    auto send = [&](UdpSocket& s, const Bytes& d) { CHECK(s.send_to(to_b, d.data(), d.size())); };
    auto segment = kcp_segment;

    // Sans KCP : canal 1, canal 9, canal 0.
    send(peer, Bytes{1, 0x10, 0x20});
    send(peer, Bytes{9, 0x10});
    send(peer, segment(kConv));
    // Trop gros à la réception : 1201 octets (tient dans le tampon, refusé par la taille)
    // et 2000 octets (tronqué : MSG_TRUNC / WSAEMSGSIZE). Puis un datagramme vide.
    Bytes big(1201, 0x55);
    big[0] = 2;
    send(peer, big);
    Bytes huge(2000, 0x66);
    huge[0] = 2;
    send(peer, huge);
    send(peer, Bytes{});
    send(peer, marker(1));
    CHECK_EQ(events_before_marker(b, 1), 0);

    // KCP ouvert vers `peer` : attendre que la commande soit exécutée (file vue vide).
    CHECK(b.post(OpenReliable{kConv, loopback(peer.local_port())}));
    CHECK(wait_until(std::chrono::milliseconds(2000), [&] { return b.pending_commands() == 0; }));
    send(stranger, segment(kConv));     // autre adresse
    send(peer, segment(kConv ^ 0xFF));  // autre conv
    send(peer, Bytes{0, 0x78, 0x56});   // trop court pour porter un conv
    send(peer, marker(2));
    CHECK_EQ(events_before_marker(b, 2), 0);

    const Stats s = b.stats();
    CHECK_EQ(s.ignored_snapshot, 1u);
    CHECK_EQ(s.ignored_unknown_channel, 1u);
    CHECK_EQ(s.ignored_no_reliable, 1u);
    CHECK_EQ(s.ignored_wrong_peer, 1u);
    CHECK_EQ(s.ignored_wrong_conv, 2u);
    CHECK_EQ(s.kcp_input_errors, 0u);
    CHECK_EQ(s.rx_too_big, 2u);
    CHECK_EQ(s.ignored_empty, 1u);
    CHECK_EQ(s.datagrams_received, 11u);
}

TEST("net : port occupé — false, message, pas de thread") {
    Transport a, b;
    if (!start_or_report(a, 0)) return;
    std::string error;
    CHECK(!b.start(a.local_port(), error));
    CHECK(!error.empty());
    CHECK(!b.running());
    CHECK_EQ(b.local_port(), 0u);
    b.stop();  // sans effet
    CHECK(a.running());
}

TEST("net : start puis stop 20 fois sur le même port") {
    Transport t;
    if (!start_or_report(t, 0)) return;
    const std::uint16_t port = t.local_port();
    t.stop();
    CHECK(!t.running());
    for (int i = 0; i < 20; ++i) {
        std::string error;
        const bool ok = t.start(port, error);
        if (!ok) std::printf("  itération %d : %s\n", i, error.c_str());
        CHECK(ok);
        CHECK_EQ(t.local_port(), port);
        const auto t0 = Clock::now();
        t.stop();  // revient après join
        CHECK(Clock::now() - t0 < std::chrono::seconds(1));
        CHECK(!t.running());
        CHECK_EQ(t.local_port(), 0u);
    }
    // Le port est bien libéré : un socket brut peut le prendre.
    UdpSocket s;
    std::string error;
    CHECK(s.open(port, error));
}

TEST("net : file pleine — 5000 commandes, 4096 gardées, 904 comptées") {
    Transport t;  // thread arrêté : rien ne vide la file
    int accepted = 0;
    for (int i = 0; i < 5000; ++i) {
        if (t.post(SendControl{loopback(9), Bytes{2, 3, 1}})) ++accepted;
    }
    CHECK_EQ(accepted, 4096);
    CHECK_EQ(t.pending_commands(), 4096u);
    CHECK_EQ(t.stats().command_queue_full, 904u);
}

TEST("net : file entrante pleine — contrôle jeté et compté, fiable retenu dans KCP") {
    Transport a, b;  // b n'est pas lu tant que sa file n'est pas pleine
    if (!start_or_report(a, 0) || !start_or_report(b, 0)) return;
    UdpSocket sender;
    std::string error;
    CHECK(sender.open(0, error));
    const Endpoint to_b = loopback(b.local_port());

    // 4200 datagrammes de contrôle, cadencés : chaque lot de 64 est lu par b avant le suivant,
    // pour que le tampon du socket n'en perde aucun.
    constexpr int kControl = 4200;
    for (int i = 0; i < kControl; ++i) {
        const Bytes d{2, 0xC0, static_cast<std::uint8_t>(i), static_cast<std::uint8_t>(i >> 8)};
        CHECK(sender.send_to(to_b, d.data(), d.size()));
        if ((i + 1) % 64 == 0 || i + 1 == kControl) {
            const auto target = static_cast<std::uint64_t>(i + 1);
            CHECK(wait_until(std::chrono::milliseconds(2000),
                             [&] { return b.stats().datagrams_received >= target; }));
        }
    }
    CHECK_EQ(b.stats().datagrams_received, static_cast<std::uint64_t>(kControl));
    CHECK_EQ(b.stats().event_queue_full, static_cast<std::uint64_t>(kControl - 4096));

    // KCP ouvert alors que la file de b est pleine : 200 messages (plus que la fenêtre de
    // réception de 128 fragments) attendent dans KCP, ou chez l'émetteur, sans être perdus.
    a.post(OpenReliable{kConv, loopback(b.local_port())});
    b.post(OpenReliable{kConv, loopback(a.local_port())});
    constexpr int kCount = 200;
    for (int i = 0; i < kCount; ++i) CHECK(a.post(SendReliable{make_message(i, 0xA0)}));
    const std::uint64_t before = b.stats().datagrams_received;
    // Une bonne partie de la fenêtre arrive et reste dans KCP (environ 100 segments avant que
    // la fenêtre annoncée tombe à zéro) : 64 suffisent à prouver l'arriéré.
    CHECK(wait_until(std::chrono::milliseconds(5000),
                     [&] { return b.stats().datagrams_received >= before + 64; }));

    int control = 0, reliable = 0;
    bool order_ok = true;
    CHECK(wait_until(std::chrono::milliseconds(15000), [&] {
        Event e;
        while (b.poll(e)) {
            if (std::get_if<ControlReceived>(&e) != nullptr) {
                if (reliable > 0) order_ok = false;  // contrôles d'abord : ils étaient en file
                ++control;
            } else if (const auto* r = std::get_if<ReliableReceived>(&e)) {
                if (reliable >= kCount || r->bytes != make_message(reliable, 0xA0)) order_ok = false;
                ++reliable;
            }
        }
        return reliable >= kCount;
    }));
    CHECK_EQ(control, 4096);
    CHECK_EQ(reliable, kCount);
    CHECK(order_ok);
    CHECK_EQ(b.stats().event_queue_full, static_cast<std::uint64_t>(kControl - 4096));
}

TEST("net : CloseReliable, envoi sans KCP, message vide, réouverture avec un autre conv") {
    Transport a, b;
    if (!start_or_report(a, 0) || !start_or_report(b, 0)) return;
    const Endpoint to_a = loopback(a.local_port()), to_b = loopback(b.local_port());

    // Lit le prochain message fiable de `t` (borné à 5 s).
    auto next_reliable = [](Transport& t, Bytes& out) {
        return wait_until(std::chrono::milliseconds(5000), [&] {
            Event e;
            while (t.poll(e)) {
                if (const auto* r = std::get_if<ReliableReceived>(&e)) {
                    out = r->bytes;
                    return true;
                }
            }
            return false;
        });
    };

    a.post(OpenReliable{kConv, to_b});
    b.post(OpenReliable{kConv, to_a});
    a.post(SendReliable{Bytes{0x10}});
    Bytes got;
    CHECK(next_reliable(b, got));
    CHECK(got == Bytes{0x10});

    // Fermeture des deux côtés, puis attente que les commandes soient exécutées.
    a.post(CloseReliable{});
    b.post(CloseReliable{});
    b.post(SendReliable{Bytes{0x10}});
    CHECK(wait_until(std::chrono::milliseconds(2000), [&] { return b.stats().reliable_without_kcp == 1; }));
    CHECK(wait_until(std::chrono::milliseconds(2000), [&] { return a.pending_commands() == 0; }));

    UdpSocket raw;
    std::string error;
    CHECK(raw.open(0, error));
    const std::uint64_t no_reliable = b.stats().ignored_no_reliable;
    const Bytes seg = kcp_segment(kConv);
    CHECK(raw.send_to(to_b, seg.data(), seg.size()));
    const Bytes m = marker(3);
    CHECK(raw.send_to(to_b, m.data(), m.size()));
    CHECK_EQ(events_before_marker(b, 3), 0);
    CHECK_EQ(b.stats().ignored_no_reliable, no_reliable + 1);

    // Réouverture avec un nouveau conv : un message vide est refusé, un vrai arrive.
    constexpr std::uint32_t kConv2 = 0xCAFE0002;
    a.post(OpenReliable{kConv2, to_b});
    b.post(OpenReliable{kConv2, to_a});
    a.post(SendReliable{Bytes{}});
    CHECK(wait_until(std::chrono::milliseconds(2000), [&] { return a.stats().reliable_send_errors == 1; }));
    a.post(SendReliable{Bytes{0x11, 0x02}});
    got.clear();
    CHECK(next_reliable(b, got));
    CHECK(got == (Bytes{0x11, 0x02}));
    CHECK_EQ(b.stats().ignored_wrong_conv, 0u);
}
