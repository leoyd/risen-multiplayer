// Machine à états de session (story 1.4) : matrice I/O de la spec, sans socket ni jeu.
#include "core/session.h"
#include "core/protocol.h"
#include "test.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace mp::core::session;
namespace proto = mp::protocol;

namespace {

constexpr std::uint32_t kLoopback = 0x7f000001;
const Endpoint kGuestA{0xc0a8010a, 50000};  // 192.168.1.10:50000
const Endpoint kGuestB{0xc0a8010b, 50001};
const Endpoint kHost{0xc0a80114, 27960};    // 192.168.1.20:27960

std::array<mp::ModuleFingerprint, 4> steam_fingerprint() {
    return {{{0x63c9aff6, 0x01417000}, {0x63ca945d, 0x00d3a000}, {0x63c9b014, 0x004d1000}, {0x63c569b6, 0x0025a000}}};
}

Settings host_settings() {
    Settings s;
    s.role = mp::Role::Host;
    s.port = 27960;
    s.nickname = "Hote";
    s.mod_version = "0.1.0";
    s.fingerprint = steam_fingerprint();
    s.seed = 42;
    return s;
}

Settings guest_settings(const std::string& ip = "192.168.1.20") {
    Settings s = host_settings();
    s.role = mp::Role::Guest;
    s.host_ip = ip;
    s.nickname = "Invite";
    return s;
}

World loaded_world() {
    World w;
    w.loaded = true;
    w.pose_ok = true;
    w.x = 100.0f;
    w.y = -50.5f;
    w.z = 2000.0f;
    w.yaw = 1.5f;
    return w;
}

proto::Hello make_hello() {
    proto::Hello h;
    h.mod_version = "0.1.0";
    h.fingerprint = steam_fingerprint();
    h.nickname = "Invite";
    return h;
}

Bytes encode(const proto::ControlMessage& m) {
    Bytes b;
    CHECK(proto::encode_control(m, b));
    return b;
}

Bytes encode_r(const proto::ReliableMessage& m) {
    Bytes b;
    CHECK(proto::encode_reliable(m, b));
    return b;
}

std::vector<Input> control_from(const Endpoint& from, Bytes bytes) { return {ControlIn{from, std::move(bytes)}}; }
std::vector<Input> reliable(Bytes bytes) { return {ReliableIn{std::move(bytes)}}; }

template <typename T>
std::vector<const T*> commands_of(const Output& out) {
    std::vector<const T*> v;
    for (const auto& c : out.commands) {
        if (const auto* p = std::get_if<T>(&c)) v.push_back(p);
    }
    return v;
}

bool has_log(const Output& out, const std::string& needle) {
    for (const auto& l : out.logs) {
        if (l.text.find(needle) != std::string::npos) return true;
    }
    return false;
}

// Reject{reason} envoyé à `to` dans `out`.
bool sent_reject(const Output& out, const Endpoint& to, std::uint8_t reason) {
    for (const auto* s : commands_of<Send>(out)) {
        if (s->destination != to) continue;
        const auto d = proto::decode_control(s->bytes.data(), s->bytes.size());
        if (!d.ok()) continue;
        if (const auto* r = std::get_if<proto::Reject>(&d.message)) {
            if (r->reason == reason) return true;
        }
    }
    return false;
}

// Welcome envoyé dans `out` (premier trouvé), décodé.
bool sent_welcome(const Output& out, proto::Welcome& w, Bytes* raw = nullptr) {
    for (const auto* s : commands_of<Send>(out)) {
        const auto d = proto::decode_control(s->bytes.data(), s->bytes.size());
        if (!d.ok()) continue;
        if (const auto* p = std::get_if<proto::Welcome>(&d.message)) {
            w = *p;
            if (raw) *raw = s->bytes;
            return true;
        }
    }
    return false;
}

std::size_t hellos_sent(const Output& out) {
    std::size_t n = 0;
    for (const auto* s : commands_of<Send>(out)) {
        std::uint16_t v = 0;
        if (proto::peek_hello_version(s->bytes.data(), s->bytes.size(), v)) ++n;
    }
    return n;
}

std::size_t heartbeats_sent(const Output& out) {
    std::size_t n = 0;
    for (const auto* s : commands_of<SendReliable>(out)) {
        const auto d = proto::decode_reliable(s->bytes.data(), s->bytes.size());
        if (d.ok() && std::holds_alternative<proto::Heartbeat>(d.message)) ++n;
    }
    return n;
}

// Hôte armé puis invité A accepté à t = 1000.
Session hosting_with_a(std::uint32_t& conv) {
    Session s(host_settings());
    s.tick(0, loaded_world(), {});
    const Output out = s.tick(1000, loaded_world(), control_from(kGuestA, encode(make_hello())));
    proto::Welcome w;
    CHECK(sent_welcome(out, w));
    conv = w.conv;
    return s;
}

}  // namespace

// --- Hôte ----------------------------------------------------------------------------

TEST("session hôte : pas armé tant que le monde n'est pas chargé") {
    Session s(host_settings());
    const Output out = s.tick(0, World{}, {});
    CHECK(s.state() == State::Inactive);
    CHECK(out.commands.empty());
}

TEST("session hôte : armé au monde chargé, écoute sur le port") {
    Session s(host_settings());
    const Output out = s.tick(0, loaded_world(), {});
    CHECK(s.state() == State::Hosting);
    const auto starts = commands_of<Start>(out);
    CHECK_EQ(starts.size(), 1u);
    if (!starts.empty()) CHECK_EQ(starts[0]->port, 27960);
    CHECK(has_log(out, "en attente d'un invité sur le port 27960"));
    CHECK(has_log(out, "[sync] Inactive → Hosting"));
}

TEST("session hôte : Hello compatible → Welcome{conv≠0, pose}, OpenReliable, InSession") {
    Session s(host_settings());
    s.tick(0, loaded_world(), {});
    const Output out = s.tick(10, loaded_world(), control_from(kGuestA, encode(make_hello())));
    CHECK(s.state() == State::InSession);
    proto::Welcome w;
    CHECK(sent_welcome(out, w));
    CHECK(w.conv != 0);
    CHECK_EQ(w.x, 100.0f);
    CHECK_EQ(w.y, -50.5f);
    CHECK_EQ(w.z, 2000.0f);
    CHECK_EQ(w.yaw, 1.5f);
    const auto opens = commands_of<OpenReliable>(out);
    CHECK_EQ(opens.size(), 1u);
    if (!opens.empty()) {
        CHECK_EQ(opens[0]->conv, w.conv);
        CHECK(opens[0]->peer == kGuestA);
    }
    CHECK(has_log(out, "Hello reçu de 192.168.1.10:50000"));
    CHECK(has_log(out, "Hosting → InSession"));
    // Heartbeat dès l'entrée en session.
    CHECK_EQ(heartbeats_sent(out), 1u);
}

TEST("session hôte : retransmission depuis A → même Welcome, pas de nouvel OpenReliable") {
    Session s(host_settings());
    s.tick(0, loaded_world(), {});
    Bytes first, second;
    proto::Welcome w1, w2;
    Output out = s.tick(10, loaded_world(), control_from(kGuestA, encode(make_hello())));
    CHECK(sent_welcome(out, w1, &first));
    out = s.tick(510, loaded_world(), control_from(kGuestA, encode(make_hello())));
    CHECK(sent_welcome(out, w2, &second));
    CHECK(first == second);
    CHECK_EQ(w1.conv, w2.conv);
    CHECK(commands_of<OpenReliable>(out).empty());
    CHECK(s.state() == State::InSession);
}

TEST("session hôte : Hello de A après un message fiable de A → ignoré") {
    std::uint32_t conv = 0;
    Session s = hosting_with_a(conv);
    s.tick(1100, loaded_world(), reliable(encode_r(proto::Heartbeat{})));
    const Output out = s.tick(1200, loaded_world(), control_from(kGuestA, encode(make_hello())));
    CHECK(commands_of<Send>(out).empty());
    CHECK(commands_of<OpenReliable>(out).empty());
    CHECK(s.state() == State::InSession);
    CHECK(has_log(out, "ignoré (KCP déjà établi)"));
}

TEST("session hôte : Hello invalide venant du pair → ignoré") {
    std::uint32_t conv = 0;
    Session s = hosting_with_a(conv);
    proto::Hello h = make_hello();
    h.mod_version = "9.9.9";
    const Output out = s.tick(1200, loaded_world(), control_from(kGuestA, encode(h)));
    CHECK(commands_of<Send>(out).empty());
    CHECK(s.state() == State::InSession);
}

TEST("session hôte : Hello d'une autre adresse en session → Reject{4}, session intacte") {
    std::uint32_t conv = 0;
    Session s = hosting_with_a(conv);
    const Output out = s.tick(1500, loaded_world(), control_from(kGuestB, encode(make_hello())));
    CHECK(sent_reject(out, kGuestB, 4));
    CHECK(has_log(out, "session pleine (4)"));
    CHECK(s.state() == State::InSession);
    CHECK(s.peer() == kGuestA);
    CHECK_EQ(s.conv(), conv);
    CHECK(commands_of<OpenReliable>(out).empty());
}

TEST("session hôte : en session, l'ordre du §6 s'applique encore à un autre invité") {
    std::uint32_t conv = 0;
    Session s = hosting_with_a(conv);
    const Bytes v2 = {0x02, 0x01, 0x02, 0x00, 0xff, 0xff, 0xff, 0xff};
    const Output out = s.tick(1500, loaded_world(), control_from(kGuestB, v2));
    CHECK(sent_reject(out, kGuestB, 1));
}

TEST("session hôte : refus v2 / mod / empreinte / illisible → Reject{1,2,3,5}") {
    struct Case {
        Bytes hello;
        std::uint8_t reason;
        const char* log;
    };
    proto::Hello mod = make_hello();
    mod.mod_version = "0.1.0 ";  // comparaison octet par octet
    proto::Hello fp = make_hello();
    fp.fingerprint[3].size_of_image ^= 1;  // SharedBase seulement
    Bytes truncated = encode(make_hello());
    truncated.resize(22);
    const Case cases[] = {
        {{0x02, 0x01, 0x02, 0x00, 0xff, 0xff, 0xff, 0xff}, 1, "version du protocole différente (1)"},
        {encode(mod), 2, "version du mod différente (2)"},
        {encode(fp), 3, "empreinte du jeu différente (3)"},
        {truncated, 5, "Hello invalide (5)"},
        {{0x02, 0x01, 0x01}, 5, "Hello invalide (5)"},
    };
    for (const auto& c : cases) {
        Session s(host_settings());
        s.tick(0, loaded_world(), {});
        const Output out = s.tick(10, loaded_world(), control_from(kGuestA, c.hello));
        CHECK(sent_reject(out, kGuestA, c.reason));
        CHECK(has_log(out, c.log));
        CHECK(s.state() == State::Hosting);
        CHECK(commands_of<OpenReliable>(out).empty());
    }
}

TEST("session hôte : Welcome ou Reject reçus par l'hôte → ignorés") {
    Session s(host_settings());
    s.tick(0, loaded_world(), {});
    proto::Welcome w;
    w.conv = 7;
    Output out = s.tick(10, loaded_world(), control_from(kGuestA, encode(w)));
    CHECK(out.commands.empty());
    out = s.tick(20, loaded_world(), control_from(kGuestA, encode(proto::Reject{4})));
    CHECK(out.commands.empty());
    CHECK(s.state() == State::Hosting);
}

TEST("session hôte : heartbeat toutes les 1000 ms en session") {
    std::uint32_t conv = 0;
    Session s = hosting_with_a(conv);  // heartbeat à 1000
    std::size_t sent = 0;
    for (std::uint64_t t = 1010; t <= 4000; t += 10) {
        // le pair reste actif
        const Output out = s.tick(t, loaded_world(), t % 500 == 0 ? reliable(encode_r(proto::Heartbeat{})) : std::vector<Input>{});
        sent += heartbeats_sent(out);
    }
    CHECK_EQ(sent, 3u);  // 2000, 3000, 4000
    CHECK(s.state() == State::InSession);
}

TEST("session hôte : 5 s sans événement du pair → Ended « timeout », sans envoi") {
    std::uint32_t conv = 0;
    Session s = hosting_with_a(conv);  // dernière activité : 1000
    Output out = s.tick(5999, loaded_world(), {});
    CHECK(s.state() == State::InSession);
    out = s.tick(6000, loaded_world(), {});
    CHECK(s.state() == State::Ended);
    CHECK(has_log(out, "timeout"));
    CHECK(commands_of<Send>(out).empty());
    CHECK(commands_of<SendReliable>(out).empty());
    CHECK_EQ(commands_of<Stop>(out).size(), 1u);
    // Ended est final.
    out = s.tick(7000, loaded_world(), control_from(kGuestB, encode(make_hello())));
    CHECK(out.commands.empty());
    CHECK(s.state() == State::Ended);
}

TEST("session hôte : un Hello retransmis par le pair maintient l'activité") {
    std::uint32_t conv = 0;
    Session s = hosting_with_a(conv);
    s.tick(5000, loaded_world(), control_from(kGuestA, encode(make_hello())));
    s.tick(9999, loaded_world(), {});
    CHECK(s.state() == State::InSession);
    s.tick(10000, loaded_world(), {});
    CHECK(s.state() == State::Ended);
}

TEST("session hôte : SessionEnd reçu → Ended avec la raison") {
    std::uint32_t conv = 0;
    Session s = hosting_with_a(conv);
    const Output out = s.tick(2000, loaded_world(), reliable(encode_r(proto::SessionEnd{1})));
    CHECK(s.state() == State::Ended);
    CHECK(has_log(out, "départ du joueur (1)"));
    CHECK_EQ(commands_of<Stop>(out).size(), 1u);
}

TEST("session hôte : SessionEnd de raison inconnue → code brut au log") {
    std::uint32_t conv = 0;
    Session s = hosting_with_a(conv);
    const Output out = s.tick(2000, loaded_world(), reliable(encode_r(proto::SessionEnd{200})));
    CHECK(s.state() == State::Ended);
    CHECK(has_log(out, "(200)"));
}

TEST("session hôte : conv non nul et différent d'une graine à l'autre") {
    std::uint32_t a = 0, b = 0;
    {
        Settings st = host_settings();
        st.seed = 1;
        Session s(st);
        s.tick(0, loaded_world(), {});
        proto::Welcome w;
        CHECK(sent_welcome(s.tick(1, loaded_world(), control_from(kGuestA, encode(make_hello()))), w));
        a = w.conv;
    }
    {
        Settings st = host_settings();
        st.seed = 2;
        Session s(st);
        s.tick(0, loaded_world(), {});
        proto::Welcome w;
        CHECK(sent_welcome(s.tick(1, loaded_world(), control_from(kGuestA, encode(make_hello()))), w));
        b = w.conv;
    }
    CHECK(a != 0);
    CHECK(b != 0);
    CHECK(a != b);
}

TEST("session hôte : échec de démarrage → Ended, Stop") {
    Session s(host_settings());
    s.tick(0, loaded_world(), {});
    const Output out = s.fail(0, "port 27960 occupé");
    CHECK(s.state() == State::Ended);
    CHECK(has_log(out, "port 27960 occupé"));
    CHECK_EQ(commands_of<Stop>(out).size(), 1u);
}

// --- Invité --------------------------------------------------------------------------

TEST("session invité : armé → Connecting, Hello à 0, 500, 1000 ms") {
    Session s(guest_settings());
    Output out = s.tick(0, loaded_world(), {});
    CHECK(s.state() == State::Connecting);
    const auto starts = commands_of<Start>(out);
    CHECK_EQ(starts.size(), 1u);
    if (!starts.empty()) CHECK_EQ(starts[0]->port, 0);
    CHECK_EQ(hellos_sent(out), 1u);
    for (const auto* send : commands_of<Send>(out)) CHECK(send->destination == kHost);
    CHECK(has_log(out, "Inactive → Connecting"));
    std::size_t hellos = 0;
    for (std::uint64_t t = 1; t <= 1000; ++t) hellos += hellos_sent(s.tick(t, loaded_world(), {}));
    CHECK_EQ(hellos, 2u);  // 500, 1000
    // Le Hello porte la config et les empreintes.
    out = s.tick(1500, loaded_world(), {});
    const auto sends = commands_of<Send>(out);
    CHECK_EQ(sends.size(), 1u);
    if (!sends.empty()) {
        const auto d = proto::decode_control(sends[0]->bytes.data(), sends[0]->bytes.size());
        CHECK(d.ok());
        if (const auto* h = std::get_if<proto::Hello>(&d.message)) {
            CHECK_EQ(h->nickname, std::string("Invite"));
            CHECK(h->fingerprint == steam_fingerprint());
            CHECK_EQ(h->mod_version, std::string("0.1.0"));
        }
    }
}

TEST("session invité : Welcome{conv≠0} → OpenReliable, InSession, pose au log") {
    Session s(guest_settings());
    s.tick(0, loaded_world(), {});
    proto::Welcome w;
    w.conv = 0xfe24b1fd;
    w.x = 1.0f;
    w.y = 2.0f;
    w.z = 3.0f;
    w.yaw = 0.5f;
    const Output out = s.tick(100, loaded_world(), control_from(kHost, encode(w)));
    CHECK(s.state() == State::InSession);
    const auto opens = commands_of<OpenReliable>(out);
    CHECK_EQ(opens.size(), 1u);
    if (!opens.empty()) {
        CHECK_EQ(opens[0]->conv, 0xfe24b1fdu);
        CHECK(opens[0]->peer == kHost);
    }
    CHECK(has_log(out, "(1.0, 2.0, 3.0) lacet=0.500"));
    CHECK(has_log(out, "Connecting → InSession"));
    CHECK_EQ(hellos_sent(out), 0u);
}

TEST("session invité : Welcome conv nul → ignoré, Hello continue") {
    Session s(guest_settings());
    s.tick(0, loaded_world(), {});
    proto::Welcome w;
    w.conv = 0;
    Output out = s.tick(100, loaded_world(), control_from(kHost, encode(w)));
    CHECK(s.state() == State::Connecting);
    CHECK(commands_of<OpenReliable>(out).empty());
    out = s.tick(500, loaded_world(), {});
    CHECK_EQ(hellos_sent(out), 1u);
}

TEST("session invité : Welcome d'une autre adresse → ignoré") {
    Session s(guest_settings());
    s.tick(0, loaded_world(), {});
    proto::Welcome w;
    w.conv = 5;
    s.tick(100, loaded_world(), control_from(kGuestB, encode(w)));
    CHECK(s.state() == State::Connecting);
}

TEST("session invité : Reject{r} → Ended, raison lisible, Stop") {
    Session s(guest_settings());
    s.tick(0, loaded_world(), {});
    const Output out = s.tick(100, loaded_world(), control_from(kHost, encode(proto::Reject{3})));
    CHECK(s.state() == State::Ended);
    CHECK(has_log(out, "empreinte du jeu différente (3)"));
    CHECK_EQ(commands_of<Stop>(out).size(), 1u);
}

TEST("session invité : Reject de raison inconnue → code brut") {
    Session s(guest_settings());
    s.tick(0, loaded_world(), {});
    const Output out = s.tick(100, loaded_world(), control_from(kHost, encode(proto::Reject{200})));
    CHECK(s.state() == State::Ended);
    CHECK(has_log(out, "(200)"));
}

TEST("session invité : 10 s sans réponse → Ended « hôte injoignable », Stop") {
    Session s(guest_settings());
    s.tick(0, loaded_world(), {});
    std::size_t hellos = 1;
    for (std::uint64_t t = 100; t < 10000; t += 100) hellos += hellos_sent(s.tick(t, loaded_world(), {}));
    CHECK(s.state() == State::Connecting);
    CHECK_EQ(hellos, 20u);
    const Output out = s.tick(10000, loaded_world(), {});
    CHECK(s.state() == State::Ended);
    CHECK(has_log(out, "hôte injoignable"));
    CHECK_EQ(commands_of<Stop>(out).size(), 1u);
    CHECK_EQ(hellos_sent(out), 0u);
}

TEST("session invité : en session, Welcome ou Reject → ignorés ; heartbeat ; timeout") {
    Session s(guest_settings());
    s.tick(0, loaded_world(), {});
    proto::Welcome w;
    w.conv = 9;
    Output out = s.tick(100, loaded_world(), control_from(kHost, encode(w)));
    CHECK_EQ(heartbeats_sent(out), 1u);
    out = s.tick(200, loaded_world(), control_from(kHost, encode(proto::Reject{4})));
    CHECK(s.state() == State::InSession);
    CHECK(out.commands.empty());
    out = s.tick(1100, loaded_world(), {});
    CHECK_EQ(heartbeats_sent(out), 1u);
    s.tick(5199, loaded_world(), {});
    CHECK(s.state() == State::InSession);  // Reject à 200 compte comme activité
    out = s.tick(5200, loaded_world(), {});
    CHECK(s.state() == State::Ended);
    CHECK(has_log(out, "timeout"));
    CHECK(commands_of<SendReliable>(out).empty());
}

TEST("session invité : SessionEnd reçu → Ended avec la raison") {
    Session s(guest_settings());
    s.tick(0, loaded_world(), {});
    proto::Welcome w;
    w.conv = 9;
    s.tick(100, loaded_world(), control_from(kHost, encode(w)));
    const Output out = s.tick(500, loaded_world(), reliable(encode_r(proto::SessionEnd{2})));
    CHECK(s.state() == State::Ended);
    CHECK(has_log(out, "fin de monde"));
}

TEST("session invité : nom d'hôte ou IPv6 → « IPv4 attendue », reste Inactive") {
    for (const char* ip : {"mon.serveur", "::1", "192.168.1"}) {
        Session s(guest_settings(ip));
        Output out = s.tick(0, loaded_world(), {});
        CHECK(s.state() == State::Inactive);
        CHECK(has_log(out, "IPv4 attendue"));
        CHECK(out.commands.empty());
        out = s.tick(1000, loaded_world(), {});
        CHECK(s.state() == State::Inactive);
        CHECK(out.logs.empty());  // erreur journalisée une seule fois
    }
}

TEST("session invité : équipement hors bornes → erreur, reste Inactive") {
    Settings st = guest_settings();
    st.equipment.assign(300, 'x');
    Session s(st);
    const Output out = s.tick(0, loaded_world(), {});
    CHECK(s.state() == State::Inactive);
    CHECK(out.commands.empty());
    CHECK(has_log(out, "Hello non encodable"));
}

TEST("session : bouclage hôte ⇄ invité en mémoire") {
    Settings hs = host_settings();
    Session host(hs);
    Settings gs = guest_settings("127.0.0.1");
    Session guest(gs);
    const Endpoint guest_addr{kLoopback, 40000};
    host.tick(0, loaded_world(), {});
    const Output g0 = guest.tick(0, loaded_world(), {});
    const auto hello = commands_of<Send>(g0);
    CHECK_EQ(hello.size(), 1u);
    if (hello.empty()) return;
    const Output h1 = host.tick(5, loaded_world(), control_from(guest_addr, hello[0]->bytes));
    Bytes welcome;
    proto::Welcome w;
    CHECK(sent_welcome(h1, w, &welcome));
    guest.tick(10, loaded_world(), control_from(Endpoint{kLoopback, 27960}, welcome));
    CHECK(host.state() == State::InSession);
    CHECK(guest.state() == State::InSession);
}

// --- Frames en retard, échecs, entrées invalides ---------------------------------------

TEST("session invité : frame très en retard → un seul Hello de rattrapage") {
    Session s(guest_settings());
    s.tick(0, loaded_world(), {});
    CHECK_EQ(hellos_sent(s.tick(3000, loaded_world(), {})), 1u);
    CHECK_EQ(hellos_sent(s.tick(3001, loaded_world(), {})), 0u);
}

TEST("session hôte : frame très en retard → un seul heartbeat de rattrapage") {
    std::uint32_t conv = 0;
    Session s = hosting_with_a(conv);  // heartbeat à 1000
    CHECK_EQ(heartbeats_sent(s.tick(4500, loaded_world(), reliable(encode_r(proto::Heartbeat{})))), 1u);
    CHECK_EQ(heartbeats_sent(s.tick(4501, loaded_world(), {})), 0u);
    CHECK(s.state() == State::InSession);
}

TEST("session invité : fail() pendant Connecting → Ended, Stop") {
    Session s(guest_settings());
    s.tick(0, loaded_world(), {});
    const Output out = s.fail(10, "port occupé");
    CHECK(s.state() == State::Ended);
    CHECK(has_log(out, "port occupé"));
    CHECK_EQ(commands_of<Stop>(out).size(), 1u);
}

TEST("session hôte : message fiable illisible → ignoré, session intacte") {
    std::uint32_t conv = 0;
    Session s = hosting_with_a(conv);
    const Output out = s.tick(1500, loaded_world(), reliable(Bytes{0x63}));
    CHECK(s.state() == State::InSession);
    CHECK(has_log(out, "message fiable illisible"));
    CHECK(commands_of<Stop>(out).empty());
}

TEST("session hôte : en session, autre adresse avec mod différent → Reject{2} avant Reject{4}") {
    std::uint32_t conv = 0;
    Session s = hosting_with_a(conv);
    proto::Hello h = make_hello();
    h.mod_version = "0.2.0";
    const Output out = s.tick(1500, loaded_world(), control_from(kGuestB, encode(h)));
    CHECK(sent_reject(out, kGuestB, 2));
    CHECK(!sent_reject(out, kGuestB, 4));
    CHECK(s.state() == State::InSession);
}

TEST("session hôte : Hello trop court venant du pair → ignoré, session intacte") {
    std::uint32_t conv = 0;
    Session s = hosting_with_a(conv);
    const Output out = s.tick(1500, loaded_world(), control_from(kGuestA, Bytes{0x02, 0x01, 0x01}));
    CHECK(commands_of<Send>(out).empty());
    CHECK(s.state() == State::InSession);
    CHECK(s.peer() == kGuestA);
}

TEST("session invité : réponse d'une autre adresse → signalée une fois en Warn") {
    Session s(guest_settings());
    s.tick(0, loaded_world(), {});
    proto::Welcome w;
    w.conv = 5;
    Output out = s.tick(100, loaded_world(), control_from(kGuestB, encode(w)));
    bool warned = false;
    for (const auto& l : out.logs) {
        if (l.level == Level::Warn && l.text.find("192.168.1.11:50001") != std::string::npos) warned = true;
    }
    CHECK(warned);
    out = s.tick(200, loaded_world(), control_from(kGuestB, encode(w)));
    for (const auto& l : out.logs) CHECK(l.level != Level::Warn);
    CHECK(s.state() == State::Connecting);
}
