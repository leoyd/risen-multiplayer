// Machine à états de session (stories 1.4 et 1.5) : matrices I/O des specs, sans socket ni jeu.
#include "core/session.h"
#include "core/protocol.h"
#include "test.h"

#include <cmath>
#include <cstdint>
#include <limits>
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

constexpr std::uintptr_t kPlayer = 0x1000;
constexpr std::uintptr_t kPlayer2 = 0x2000;

World loaded_world(std::uintptr_t player = kPlayer) {
    World w;
    w.player = player;
    w.running = true;
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

// Fin du maintien : CloseReliable puis Stop, Ended → Inactive, dans le tick de `now`.
bool linger_closed(const Output& out) {
    std::size_t close_at = out.commands.size(), stop_at = out.commands.size();
    for (std::size_t i = 0; i < out.commands.size(); ++i) {
        if (std::holds_alternative<CloseReliable>(out.commands[i]) && close_at == out.commands.size()) close_at = i;
        if (std::holds_alternative<Stop>(out.commands[i]) && stop_at == out.commands.size()) stop_at = i;
    }
    return close_at < stop_at && stop_at < out.commands.size() && has_log(out, "Ended → Inactive");
}

// SessionEnd{reason} envoyé dans `out`.
bool sent_session_end(const Output& out, std::uint8_t reason) {
    for (const auto* s : commands_of<SendReliable>(out)) {
        const auto d = proto::decode_reliable(s->bytes.data(), s->bytes.size());
        if (!d.ok()) continue;
        if (const auto* e = std::get_if<proto::SessionEnd>(&d.message)) {
            if (e->reason == reason) return true;
        }
    }
    return false;
}

std::size_t session_ends_sent(const Output& out) {
    std::size_t n = 0;
    for (const auto* s : commands_of<SendReliable>(out)) {
        const auto d = proto::decode_reliable(s->bytes.data(), s->bytes.size());
        if (d.ok() && std::holds_alternative<proto::SessionEnd>(d.message)) ++n;
    }
    return n;
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
    const Bytes future = {0x02, 0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    const Output out = s.tick(1500, loaded_world(), control_from(kGuestB, future));
    CHECK(sent_reject(out, kGuestB, 1));
}

TEST("session hôte : refus version future / mod / empreinte / illisible → Reject{1,2,3,5}") {
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
        {{0x02, 0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}, 1, "version du protocole différente (1)"},
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
    CHECK(commands_of<Stop>(out).empty());  // maintien de 1 s
    // Pendant le maintien, les entrées sont sans objet.
    out = s.tick(6999, loaded_world(), control_from(kGuestB, encode(make_hello())));
    CHECK(out.commands.empty());
    CHECK(s.state() == State::Ended);
    // Fin du maintien : Stop, Inactive, puis réarmement immédiat (l'invité est parti, même
    // monde). Les entrées de ce tick viennent de l'ancien transport : ignorées.
    out = s.tick(7000, loaded_world(), control_from(kGuestB, encode(make_hello())));
    CHECK(linger_closed(out));
    CHECK(s.state() == State::Hosting);
    CHECK(has_log(out, "réarmement immédiat"));
    CHECK_EQ(commands_of<Start>(out).size(), 1u);
    CHECK(commands_of<Send>(out).empty());
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
    Output out = s.tick(2000, loaded_world(), reliable(encode_r(proto::SessionEnd{1})));
    CHECK(s.state() == State::Ended);
    CHECK(has_log(out, "départ du joueur (1)"));
    CHECK_EQ(session_ends_sent(out), 0u);  // pas d'envoi : la cause vient du pair
    out = s.tick(3000, loaded_world(), {});
    CHECK(linger_closed(out));
    CHECK(s.state() == State::Hosting);  // l'hôte se réarme aussitôt
}

TEST("session hôte : Motion reçu sans avatar → ignoré, la session continue") {
    std::uint32_t conv = 0;
    Session s = hosting_with_a(conv);
    proto::Motion m;
    m.net_id = 2;
    const Output out = s.tick(2000, loaded_world(), reliable(encode_r(m)));
    CHECK(s.state() == State::InSession);
    CHECK(commands_of<PlayAvatarMotion>(out).empty());
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
    CHECK_EQ(session_ends_sent(out), 0u);  // pas en session
    const Output later = s.tick(1000, loaded_world(), {});
    CHECK(linger_closed(later));
    CHECK(s.state() == State::Inactive);  // pas de nouvel essai dans le même monde
    CHECK(commands_of<Start>(s.tick(5000, loaded_world(), {})).empty());
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
    CHECK(linger_closed(s.tick(1100, loaded_world(), {})));
    CHECK(s.state() == State::Inactive);
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
    CHECK_EQ(hellos_sent(out), 0u);
    CHECK(linger_closed(s.tick(11000, loaded_world(), {})));
    CHECK(s.state() == State::Inactive);
    // Même monde : pas de nouvel essai.
    for (std::uint64_t t = 11000; t <= 30000; t += 500) CHECK(s.tick(t, loaded_world(), {}).commands.empty());
    CHECK(s.state() == State::Inactive);
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
    CHECK_EQ(out.commands.size(), 1u);  // le snapshot du tick, rien d'autre
    CHECK_EQ(commands_of<SendSnapshot>(out).size(), 1u);
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
    CHECK(linger_closed(s.tick(1010, loaded_world(), {})));
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

// --- Story 1.5 : fin de session propre -------------------------------------------------

TEST("fin 1.5 : rechargement en session → SessionEnd{2}, maintien 1 s, Stop, Inactive, réarmement") {
    std::uint32_t conv = 0;
    Session s = hosting_with_a(conv);
    Output out = s.tick(2000, loaded_world(kPlayer2), control_from(kGuestB, encode(make_hello())));
    CHECK(s.state() == State::Ended);
    CHECK(sent_session_end(out, 2));
    CHECK(has_log(out, "InSession → Ended : fin de monde"));
    CHECK(commands_of<Stop>(out).empty());
    CHECK(commands_of<CloseReliable>(out).empty());
    CHECK(commands_of<Send>(out).empty());  // entrées du tick de fin sans objet
    // Le SessionEnd part avant toute fermeture.
    out = s.tick(2999, loaded_world(kPlayer2), {});
    CHECK(out.commands.empty());
    out = s.tick(3000, loaded_world(kPlayer2), {});
    CHECK(linger_closed(out));
    CHECK(s.state() == State::Hosting);  // nouveau monde : réarmement
    CHECK(has_log(out, "Inactive → Hosting"));
    // Nouvelle session, nouveau conv.
    out = s.tick(3100, loaded_world(kPlayer2), control_from(kGuestA, encode(make_hello())));
    proto::Welcome w;
    CHECK(sent_welcome(out, w));
    CHECK(w.conv != conv);
    CHECK(s.state() == State::InSession);
}

TEST("fin 1.5 : retour au menu (joueur nul) → fin de monde, pas de réarmement hors monde") {
    std::uint32_t conv = 0;
    Session s = hosting_with_a(conv);
    Output out = s.tick(2000, World{}, {});
    CHECK(sent_session_end(out, 2));
    CHECK(has_log(out, "fin de monde"));
    out = s.tick(3000, World{}, {});
    CHECK(linger_closed(out));
    CHECK(s.state() == State::Inactive);
    CHECK(has_log(out, "nouvelle session au prochain monde chargé"));
    for (std::uint64_t t = 3000; t < 8000; t += 100) CHECK(s.tick(t, World{}, {}).commands.empty());
    // Le même pointeur revient (le jeu réutilise l'adresse) : c'est quand même un nouveau monde.
    out = s.tick(8000, loaded_world(kPlayer), {});
    CHECK(s.state() == State::Hosting);
}

TEST("fin 1.5 : partie arrêtée (running faux, joueur présent) → fin de monde") {
    std::uint32_t conv = 0;
    Session s = hosting_with_a(conv);
    World w = loaded_world();
    w.running = false;
    const Output out = s.tick(2000, w, {});
    CHECK(s.state() == State::Ended);
    CHECK(sent_session_end(out, 2));
}

TEST("fin 1.5 : même joueur après un chargement rapide pendant le maintien → réarmement") {
    std::uint32_t conv = 0;
    Session s = hosting_with_a(conv);
    World menu = loaded_world();
    menu.running = false;
    s.tick(2000, menu, {});
    CHECK(s.state() == State::Ended);
    s.tick(2500, loaded_world(), {});  // revenu pendant le maintien, même adresse
    const Output out = s.tick(3000, loaded_world(), {});
    CHECK(linger_closed(out));
    CHECK(s.state() == State::Hosting);
}

TEST("fin 1.5 : fin de monde hors session (Hosting, Connecting) → sans envoi, Inactive, réarmement") {
    {
        Session s(host_settings());
        s.tick(0, loaded_world(), {});
        CHECK(s.state() == State::Hosting);
        Output out = s.tick(100, loaded_world(kPlayer2), {});
        CHECK(s.state() == State::Ended);
        CHECK(has_log(out, "Hosting → Ended : fin de monde"));
        CHECK_EQ(session_ends_sent(out), 0u);
        out = s.tick(1100, loaded_world(kPlayer2), {});
        CHECK(linger_closed(out));
        CHECK(s.state() == State::Hosting);
    }
    {
        Session s(guest_settings());
        s.tick(0, loaded_world(), {});
        CHECK(s.state() == State::Connecting);
        Output out = s.tick(100, World{}, {});
        CHECK(s.state() == State::Ended);
        CHECK(has_log(out, "Connecting → Ended : fin de monde"));
        CHECK_EQ(session_ends_sent(out), 0u);
        CHECK_EQ(hellos_sent(out), 0u);
        out = s.tick(1100, World{}, {});
        CHECK(linger_closed(out));
        CHECK(s.state() == State::Inactive);
        out = s.tick(1200, loaded_world(kPlayer2), {});
        CHECK(s.state() == State::Connecting);
        CHECK_EQ(hellos_sent(out), 1u);  // nouvel essai au nouveau monde
    }
}

TEST("fin 1.5 : mort de l'hôte en session → SessionEnd{3}, Inactive, réarmement au monde suivant") {
    std::uint32_t conv = 0;
    Session s = hosting_with_a(conv);
    World dead = loaded_world();
    dead.dead = true;
    Output out = s.tick(2000, dead, {});
    CHECK(s.state() == State::Ended);
    CHECK(sent_session_end(out, 3));
    CHECK(has_log(out, "InSession → Ended : mort de l'hôte"));
    out = s.tick(3000, dead, {});
    CHECK(linger_closed(out));
    CHECK(s.state() == State::Inactive);
    CHECK(s.tick(4000, dead, {}).commands.empty());
    CHECK(s.state() == State::Inactive);
    s.tick(5000, loaded_world(kPlayer2), {});  // le jeu recharge
    CHECK(s.state() == State::Hosting);
}

TEST("fin 1.5 : mort de l'hôte sans invité (Hosting) → sans envoi, Inactive") {
    Session s(host_settings());
    s.tick(0, loaded_world(), {});
    World dead = loaded_world();
    dead.dead = true;
    Output out = s.tick(100, dead, {});
    CHECK(s.state() == State::Ended);
    CHECK(has_log(out, "mort de l'hôte"));
    CHECK_EQ(session_ends_sent(out), 0u);
    out = s.tick(1100, dead, {});
    CHECK(linger_closed(out));
    CHECK(s.state() == State::Inactive);
}

TEST("fin 1.5 : mort du joueur côté invité → pas de fin (J2)") {
    Session s(guest_settings());
    s.tick(0, loaded_world(), {});
    proto::Welcome w;
    w.conv = 9;
    s.tick(100, loaded_world(), control_from(kHost, encode(w)));
    World dead = loaded_world();
    dead.dead = true;
    s.tick(200, dead, reliable(encode_r(proto::Heartbeat{})));
    CHECK(s.state() == State::InSession);
}

TEST("fin 1.5 : SessionEnd reçu par l'invité → sans envoi, Inactive, attend le monde suivant") {
    Session s(guest_settings());
    s.tick(0, loaded_world(), {});
    proto::Welcome w;
    w.conv = 9;
    s.tick(100, loaded_world(), control_from(kHost, encode(w)));
    Output out = s.tick(500, loaded_world(), reliable(encode_r(proto::SessionEnd{2})));
    CHECK(s.state() == State::Ended);
    CHECK_EQ(session_ends_sent(out), 0u);
    out = s.tick(1500, loaded_world(), {});
    CHECK(linger_closed(out));
    CHECK(s.state() == State::Inactive);
    CHECK(s.tick(3000, loaded_world(), {}).commands.empty());
    s.tick(4000, loaded_world(kPlayer2), {});
    CHECK(s.state() == State::Connecting);
}

TEST("fin 1.5 : timeout de l'invité → attend le monde suivant") {
    Session s(guest_settings());
    s.tick(0, loaded_world(), {});
    proto::Welcome w;
    w.conv = 9;
    s.tick(100, loaded_world(), control_from(kHost, encode(w)));
    s.tick(5100, loaded_world(), {});
    CHECK(s.state() == State::Ended);
    CHECK(linger_closed(s.tick(6100, loaded_world(), {})));
    CHECK(s.state() == State::Inactive);
    CHECK(s.tick(7000, loaded_world(), {}).commands.empty());
}

TEST("fin 1.5 : Reject reçu → pas de nouvel essai dans le même monde, nouvel essai au suivant") {
    Session s(guest_settings());
    s.tick(0, loaded_world(), {});
    s.tick(100, loaded_world(), control_from(kHost, encode(proto::Reject{4})));
    CHECK(s.state() == State::Ended);
    CHECK(linger_closed(s.tick(1100, loaded_world(), {})));
    for (std::uint64_t t = 1200; t < 20000; t += 500) CHECK(s.tick(t, loaded_world(), {}).commands.empty());
    CHECK(s.state() == State::Inactive);
    s.tick(20000, loaded_world(kPlayer2), {});
    CHECK(s.state() == State::Connecting);
}

TEST("fin 1.5 : trois rechargements → trois conv distincts, trois couples InSession/Ended") {
    Session s(host_settings());
    std::vector<std::uint32_t> convs;
    std::size_t in_session = 0, ended = 0;
    std::uint64_t t = 0;
    const std::uintptr_t players[] = {0x1000, 0x2000, 0x3000, 0x4000};
    for (int i = 0; i < 3; ++i) {
        const World here = loaded_world(players[i]);
        s.tick(t, here, {});
        CHECK(s.state() == State::Hosting);
        Output out = s.tick(t + 10, here, control_from(kGuestA, encode(make_hello())));
        proto::Welcome w;
        if (sent_welcome(out, w)) convs.push_back(w.conv);
        if (has_log(out, "→ InSession")) ++in_session;
        out = s.tick(t + 500, loaded_world(players[i + 1]), {});  // rechargement
        CHECK(sent_session_end(out, 2));
        if (has_log(out, "InSession → Ended : fin de monde")) ++ended;
        CHECK(linger_closed(s.tick(t + 1500, World{}, {})));  // écran de chargement
        t += 2000;
    }
    CHECK_EQ(convs.size(), 3u);
    CHECK_EQ(in_session, 3u);
    CHECK_EQ(ended, 3u);
    if (convs.size() == 3) {
        CHECK(convs[0] != convs[1]);
        CHECK(convs[1] != convs[2]);
        CHECK(convs[0] != convs[2]);
    }
}

TEST("fin 1.5 : fail() en session → SessionEnd{4}, maintien puis Stop") {
    std::uint32_t conv = 0;
    Session s = hosting_with_a(conv);
    const Output out = s.fail(2000, "erreur");
    CHECK(sent_session_end(out, 4));
    CHECK(s.state() == State::Ended);
    CHECK(linger_closed(s.tick(3000, loaded_world(), {})));
    CHECK(s.state() == State::Inactive);
}

// --- Story 1.5 : revue ----------------------------------------------------------------

TEST("fin 1.5 : hôte au héros mort → pas d'armement (ni au départ ni après le départ de l'invité)") {
    World dead = loaded_world();
    dead.dead = true;
    {
        Session s(host_settings());
        CHECK(s.tick(0, dead, {}).commands.empty());
        CHECK(s.state() == State::Inactive);
        s.tick(100, loaded_world(), {});  // le héros revit
        CHECK(s.state() == State::Hosting);
    }
    {
        std::uint32_t conv = 0;
        Session s = hosting_with_a(conv);
        s.tick(2000, loaded_world(), reliable(encode_r(proto::SessionEnd{1})));  // réarmement immédiat prévu
        CHECK(s.state() == State::Ended);
        // Le héros meurt pendant le maintien : la fin du maintien ne réarme pas.
        const Output out = s.tick(3000, dead, {});
        CHECK(linger_closed(out));
        CHECK(s.state() == State::Inactive);
        CHECK(commands_of<Start>(out).empty());
        CHECK(has_log(out, "nouvelle session au prochain monde chargé"));
    }
}

TEST("fin 1.5 : après la mort de l'hôte, même adresse et running constant → réarmement quand le héros revit") {
    std::uint32_t conv = 0;
    Session s = hosting_with_a(conv);
    World dead = loaded_world();
    dead.dead = true;
    s.tick(2000, dead, {});
    CHECK(s.state() == State::Ended);
    s.tick(3000, dead, {});
    CHECK(s.state() == State::Inactive);
    s.tick(4000, dead, {});
    CHECK(s.state() == State::Inactive);
    s.tick(5000, loaded_world(), {});  // rechargement : même adresse, running jamais retombé
    CHECK(s.state() == State::Hosting);
}

TEST("fin 1.5 : niveaux de log de Ended (Info en routine, Warn sinon)") {
    auto ended_level = [](const Output& out, Level& level) {
        for (const auto& l : out.logs) {
            if (l.text.find("→ Ended") != std::string::npos) {
                level = l.level;
                return true;
            }
        }
        return false;
    };
    Level level = Level::Debug;
    {
        std::uint32_t conv = 0;
        Session s = hosting_with_a(conv);
        CHECK(ended_level(s.tick(2000, loaded_world(kPlayer2), {}), level));
        CHECK(level == Level::Info);  // fin de monde
    }
    {
        std::uint32_t conv = 0;
        Session s = hosting_with_a(conv);
        World dead = loaded_world();
        dead.dead = true;
        CHECK(ended_level(s.tick(2000, dead, {}), level));
        CHECK(level == Level::Info);  // mort de l'hôte
    }
    for (std::uint8_t r : {std::uint8_t{1}, std::uint8_t{2}}) {
        std::uint32_t conv = 0;
        Session s = hosting_with_a(conv);
        CHECK(ended_level(s.tick(2000, loaded_world(), reliable(encode_r(proto::SessionEnd{r}))), level));
        CHECK(level == Level::Info);
    }
    {
        std::uint32_t conv = 0;
        Session s = hosting_with_a(conv);
        CHECK(ended_level(s.tick(2000, loaded_world(), reliable(encode_r(proto::SessionEnd{3}))), level));
        CHECK(level == Level::Warn);
    }
    {
        std::uint32_t conv = 0;
        Session s = hosting_with_a(conv);
        CHECK(ended_level(s.tick(6000, loaded_world(), {}), level));
        CHECK(level == Level::Warn);  // timeout
    }
    {
        Session s(guest_settings());
        s.tick(0, loaded_world(), {});
        CHECK(ended_level(s.tick(100, loaded_world(), control_from(kHost, encode(proto::Reject{4}))), level));
        CHECK(level == Level::Warn);
    }
    {
        Session s(guest_settings());
        s.tick(0, loaded_world(), {});
        CHECK(ended_level(s.tick(10000, loaded_world(), {}), level));
        CHECK(level == Level::Warn);  // injoignable
    }
    {
        Session s(host_settings());
        s.tick(0, loaded_world(), {});
        CHECK(ended_level(s.fail(10, "erreur"), level));
        CHECK(level == Level::Warn);
    }
}

TEST("fin 1.5 : fail() de l'invité en session → SessionEnd{4}") {
    Session s(guest_settings());
    s.tick(0, loaded_world(), {});
    proto::Welcome w;
    w.conv = 9;
    s.tick(100, loaded_world(), control_from(kHost, encode(w)));
    CHECK(s.state() == State::InSession);
    const Output out = s.fail(200, "erreur");
    CHECK(sent_session_end(out, 4));
    CHECK(s.state() == State::Ended);
}

TEST("fin 1.5 : SessionEnd du pair et fin de monde dans le même tick → la fin de monde l'emporte") {
    std::uint32_t conv = 0;
    Session s = hosting_with_a(conv);
    const Output out = s.tick(2000, loaded_world(kPlayer2), reliable(encode_r(proto::SessionEnd{1})));
    CHECK(s.state() == State::Ended);
    CHECK(has_log(out, "InSession → Ended : fin de monde"));
    CHECK(!has_log(out, "SessionEnd reçu"));
    CHECK(sent_session_end(out, 2));
}

TEST("fin 1.5 : hôte à réarmement immédiat dont le monde se décharge pendant le maintien → attend un monde") {
    std::uint32_t conv = 0;
    Session s = hosting_with_a(conv);
    s.tick(2000, loaded_world(), reliable(encode_r(proto::SessionEnd{1})));
    s.tick(2500, World{}, {});  // retour au menu pendant le maintien
    Output out = s.tick(3000, World{}, {});
    CHECK(linger_closed(out));
    CHECK(s.state() == State::Inactive);
    CHECK(commands_of<Start>(out).empty());
    CHECK(has_log(out, "nouvelle session au prochain monde chargé"));
    CHECK(!has_log(out, "réarmement immédiat"));
    for (std::uint64_t t = 3100; t < 6000; t += 100) CHECK(s.tick(t, World{}, {}).commands.empty());
    s.tick(6000, loaded_world(kPlayer2), {});
    CHECK(s.state() == State::Hosting);
}

// --- Avatars (story 2.2) ---------------------------------------------------------------

namespace {

// Messages fiables envoyés dans `out`, décodés, dans l'ordre.
std::vector<proto::ReliableMessage> reliables_sent(const Output& out) {
    std::vector<proto::ReliableMessage> v;
    for (const auto* s : commands_of<SendReliable>(out)) {
        const auto d = proto::decode_reliable(s->bytes.data(), s->bytes.size());
        CHECK(d.ok());
        if (d.ok()) v.push_back(d.message);
    }
    return v;
}

template <typename T>
std::vector<T> sent_of(const Output& out) {
    std::vector<T> v;
    for (const auto& m : reliables_sent(out)) {
        if (const auto* p = std::get_if<T>(&m)) v.push_back(*p);
    }
    return v;
}

// Indice de la première commande de type T dans `out` (out.commands.size() si absente).
template <typename T>
std::size_t index_of(const Output& out) {
    for (std::size_t i = 0; i < out.commands.size(); ++i) {
        if (std::holds_alternative<T>(out.commands[i])) return i;
    }
    return out.commands.size();
}

// Indice de la commande SendReliable portant un message de type T.
template <typename T>
std::size_t index_of_sent(const Output& out) {
    for (std::size_t i = 0; i < out.commands.size(); ++i) {
        const auto* s = std::get_if<SendReliable>(&out.commands[i]);
        if (s == nullptr) continue;
        const auto d = proto::decode_reliable(s->bytes.data(), s->bytes.size());
        if (d.ok() && std::holds_alternative<T>(d.message)) return i;
    }
    return out.commands.size();
}

proto::Spawn spawn_of(std::uint32_t net_id, std::uint8_t type = proto::kSpawnTypeHero) {
    proto::Spawn m;
    m.net_id = net_id;
    m.type = type;
    m.x = 10.0f;
    m.y = 20.0f;
    m.z = 30.0f;
    m.yaw = -0.25f;
    return m;
}

// Hôte en session avec A, avatar de l'invité créé à t = 1100.
Session host_with_avatar() {
    std::uint32_t conv = 0;
    Session s = hosting_with_a(conv);
    const Output out = s.tick(1100, loaded_world(), reliable(encode_r(spawn_of(proto::kGuestNetId))));
    CHECK_EQ(commands_of<SpawnAvatar>(out).size(), 1u);
    CHECK(s.avatar_present());
    return s;
}

proto::Welcome welcome_at(float x, float y, float z, float yaw) {
    proto::Welcome w;
    w.conv = 0xfe24b1fd;
    w.x = x;
    w.y = y;
    w.z = z;
    w.yaw = yaw;
    return w;
}

bool near(float a, float b) { return a - b < 1e-3f && b - a < 1e-3f; }

}  // namespace

TEST("avatars : guest_pose_near → 2 m devant l'hôte, tourné vers lui") {
    Pose p = guest_pose_near(Pose{1.0f, 2.0f, 3.0f, 0.0f});
    CHECK(near(p.x, 1.0f) && near(p.y, 2.0f) && near(p.z, 203.0f));
    CHECK(near(p.yaw, 3.14159265f));
    p = guest_pose_near(Pose{0.0f, 0.0f, 0.0f, 1.5707963f});  // avant = +x
    CHECK(near(p.x, 200.0f) && near(p.z, 0.0f));
    CHECK(near(p.yaw, 1.5707963f - 3.14159265f));  // ramené dans ]−π, π]
    p = guest_pose_near(Pose{0.0f, 0.0f, 0.0f, -3.0f});
    CHECK(near(p.yaw, -3.0f + 3.14159265f));
}

TEST("avatars : hôte accepte → Welcome, OpenReliable, puis Spawn{1, pose du Welcome}") {
    Session s(host_settings());
    s.tick(0, loaded_world(), {});
    const Output out = s.tick(1000, loaded_world(), control_from(kGuestA, encode(make_hello())));
    const auto spawns = sent_of<proto::Spawn>(out);
    CHECK_EQ(spawns.size(), 1u);
    if (!spawns.empty()) {
        CHECK_EQ(spawns[0].net_id, proto::kHostNetId);
        CHECK_EQ(static_cast<int>(spawns[0].type), 0);
        CHECK(spawns[0].x == 100.0f && spawns[0].y == -50.5f && spawns[0].z == 2000.0f && spawns[0].yaw == 1.5f);
    }
    CHECK(index_of<OpenReliable>(out) < index_of_sent<proto::Spawn>(out));
    CHECK(commands_of<Teleport>(out).empty());
    CHECK(has_log(out, "Spawn envoyé : NetId 1 (hôte)"));
}

TEST("avatars : invité reçoit Welcome → Teleport devant l'hôte, puis Spawn{2, même pose}") {
    Session s(guest_settings());
    s.tick(0, loaded_world(), {});
    const Output out = s.tick(100, loaded_world(), control_from(kHost, encode(welcome_at(1.0f, 2.0f, 3.0f, 0.0f))));
    CHECK(s.state() == State::InSession);
    const auto tps = commands_of<Teleport>(out);
    const auto spawns = sent_of<proto::Spawn>(out);
    CHECK_EQ(tps.size(), 1u);
    CHECK_EQ(spawns.size(), 1u);
    if (!tps.empty() && !spawns.empty()) {
        const Pose want = guest_pose_near(Pose{1.0f, 2.0f, 3.0f, 0.0f});
        CHECK(tps[0]->pose.x == want.x && tps[0]->pose.z == want.z && tps[0]->pose.yaw == want.yaw);
        CHECK_EQ(spawns[0].net_id, proto::kGuestNetId);
        CHECK(spawns[0].x == want.x && spawns[0].y == want.y && spawns[0].z == want.z && spawns[0].yaw == want.yaw);
    }
    CHECK(index_of<OpenReliable>(out) < index_of<Teleport>(out));
    CHECK(index_of<Teleport>(out) < index_of_sent<proto::Spawn>(out));
}

TEST("avatars : Welcome à pose nulle → pas de Teleport, Spawn à la pose du héros") {
    Session s(guest_settings());
    s.tick(0, loaded_world(), {});
    const Output out = s.tick(100, loaded_world(), control_from(kHost, encode(welcome_at(0, 0, 0, 0))));
    CHECK(commands_of<Teleport>(out).empty());
    const auto spawns = sent_of<proto::Spawn>(out);
    CHECK_EQ(spawns.size(), 1u);
    if (!spawns.empty()) CHECK(spawns[0].x == 100.0f && spawns[0].z == 2000.0f && spawns[0].yaw == 1.5f);
    CHECK(has_log(out, "pose de l'hôte nulle"));
}

TEST("avatars : Spawn du pair → SpawnAvatar ; en double → ignoré") {
    Session s = host_with_avatar();
    const Output out = s.tick(1200, loaded_world(), reliable(encode_r(spawn_of(proto::kGuestNetId))));
    CHECK(commands_of<SpawnAvatar>(out).empty());
    CHECK(has_log(out, "ignoré (avatar déjà présent)"));
    CHECK(s.state() == State::InSession);
}

TEST("avatars : SpawnAvatar porte le NetId et la pose reçus") {
    std::uint32_t conv = 0;
    Session s = hosting_with_a(conv);
    const Output out = s.tick(1100, loaded_world(), reliable(encode_r(spawn_of(proto::kGuestNetId))));
    const auto sp = commands_of<SpawnAvatar>(out);
    CHECK_EQ(sp.size(), 1u);
    if (!sp.empty()) {
        CHECK_EQ(sp[0]->net_id, proto::kGuestNetId);
        CHECK(sp[0]->pose.x == 10.0f && sp[0]->pose.y == 20.0f && sp[0]->pose.z == 30.0f &&
              sp[0]->pose.yaw == -0.25f);
    }
}

TEST("avatars : Spawn d'un mauvais NetId ou d'un autre type → ignoré, avec log") {
    std::uint32_t conv = 0;
    Session s = hosting_with_a(conv);
    Output out = s.tick(1100, loaded_world(), reliable(encode_r(spawn_of(proto::kHostNetId))));
    CHECK(commands_of<SpawnAvatar>(out).empty());
    CHECK(has_log(out, "ignoré (attendu NetId 2 (invité), type 0)"));
    out = s.tick(1200, loaded_world(), reliable(encode_r(spawn_of(proto::kGuestNetId, 7))));
    CHECK(commands_of<SpawnAvatar>(out).empty());
    CHECK(has_log(out, "type 7"));
    CHECK(!s.avatar_present());
    // Côté invité : seul NetId 1 est accepté.
    Session g(guest_settings());
    g.tick(0, loaded_world(), {});
    g.tick(100, loaded_world(), control_from(kHost, encode(welcome_at(1, 2, 3, 0))));
    out = g.tick(200, loaded_world(), reliable(encode_r(spawn_of(proto::kGuestNetId))));
    CHECK(commands_of<SpawnAvatar>(out).empty());
    out = g.tick(300, loaded_world(), reliable(encode_r(spawn_of(proto::kHostNetId))));
    CHECK_EQ(commands_of<SpawnAvatar>(out).size(), 1u);
}

TEST("avatars : fin locale en session → Despawn, SessionEnd, puis DespawnAvatar") {
    Session s = host_with_avatar();
    const Output out = s.tick(2000, loaded_world(kPlayer2), {});  // fin de monde
    const auto despawns = sent_of<proto::Despawn>(out);
    CHECK_EQ(despawns.size(), 1u);
    if (!despawns.empty()) CHECK_EQ(despawns[0].net_id, proto::kHostNetId);
    const auto da = commands_of<DespawnAvatar>(out);
    CHECK_EQ(da.size(), 1u);
    if (!da.empty()) CHECK_EQ(da[0]->net_id, proto::kGuestNetId);
    CHECK(index_of_sent<proto::Despawn>(out) < index_of_sent<proto::SessionEnd>(out));
    CHECK(index_of_sent<proto::SessionEnd>(out) < index_of<DespawnAvatar>(out));
    CHECK(!s.avatar_present());
    // Le maintien et le réarmement n'en émettent pas d'autre.
    CHECK(commands_of<DespawnAvatar>(s.tick(3000, loaded_world(kPlayer2), {})).empty());
}

TEST("avatars : fin locale sans avatar → Despawn de son héros quand même, pas de DespawnAvatar") {
    std::uint32_t conv = 0;
    Session s = hosting_with_a(conv);
    const Output out = s.fail(2000, "test");
    CHECK_EQ(sent_of<proto::Despawn>(out).size(), 1u);
    CHECK(sent_session_end(out, 4));
    CHECK(commands_of<DespawnAvatar>(out).empty());
}

TEST("avatars : timeout avec avatar → DespawnAvatar, sans Despawn envoyé") {
    Session s = host_with_avatar();
    const Output out = s.tick(1100 + kPeerTimeoutMs, loaded_world(), {});
    CHECK(s.state() == State::Ended);
    CHECK(reliables_sent(out).empty());
    CHECK_EQ(commands_of<DespawnAvatar>(out).size(), 1u);
}

TEST("avatars : SessionEnd reçu avec avatar → DespawnAvatar, sans Despawn envoyé") {
    Session s = host_with_avatar();
    const Output out = s.tick(1200, loaded_world(), reliable(encode_r(proto::SessionEnd{1})));
    CHECK(s.state() == State::Ended);
    CHECK(sent_of<proto::Despawn>(out).empty());
    CHECK_EQ(commands_of<DespawnAvatar>(out).size(), 1u);
}

TEST("avatars : Despawn reçu → DespawnAvatar, la session continue ; nouveau Spawn accepté") {
    Session s = host_with_avatar();
    Output out = s.tick(1200, loaded_world(), reliable(encode_r(proto::Despawn{proto::kGuestNetId})));
    CHECK(s.state() == State::InSession);
    const auto da = commands_of<DespawnAvatar>(out);
    CHECK_EQ(da.size(), 1u);
    if (!da.empty()) CHECK_EQ(da[0]->net_id, proto::kGuestNetId);
    CHECK(!s.avatar_present());
    out = s.tick(1300, loaded_world(), reliable(encode_r(spawn_of(proto::kGuestNetId))));
    CHECK_EQ(commands_of<SpawnAvatar>(out).size(), 1u);
    // Fin ensuite : un seul DespawnAvatar, pour le nouvel avatar.
    out = s.tick(1400, loaded_world(), reliable(encode_r(proto::SessionEnd{1})));
    CHECK_EQ(commands_of<DespawnAvatar>(out).size(), 1u);
}

TEST("avatars : Despawn sans avatar ou d'un autre NetId → ignoré, avec log") {
    std::uint32_t conv = 0;
    Session s = hosting_with_a(conv);
    Output out = s.tick(1100, loaded_world(), reliable(encode_r(proto::Despawn{proto::kGuestNetId})));
    CHECK(commands_of<DespawnAvatar>(out).empty());
    CHECK(has_log(out, "ignoré (pas d'avatar)"));
    s = host_with_avatar();
    out = s.tick(1200, loaded_world(), reliable(encode_r(proto::Despawn{proto::kHostNetId})));
    CHECK(commands_of<DespawnAvatar>(out).empty());
    CHECK(has_log(out, "ignoré (pas l'avatar du pair)"));
    CHECK(s.avatar_present());
}

TEST("avatars : rejoindre, timeout, rejoindre, quitter → jamais deux avatars") {
    Session s(host_settings());
    int present = 0, max_present = 0;
    auto run = [&](const Output& out) {
        for (const auto& c : out.commands) {
            if (std::holds_alternative<SpawnAvatar>(c)) ++present;
            if (std::holds_alternative<DespawnAvatar>(c)) --present;
            if (present > max_present) max_present = present;
            CHECK(present >= 0);
        }
    };
    std::uint64_t t = 0;
    run(s.tick(t, loaded_world(), {}));
    for (int round = 0; round < 3; ++round) {
        t += 100;
        run(s.tick(t, loaded_world(), control_from(kGuestA, encode(make_hello()))));
        CHECK(s.state() == State::InSession);
        t += 100;
        run(s.tick(t, loaded_world(), reliable(encode_r(spawn_of(proto::kGuestNetId)))));
        run(s.tick(t + 1, loaded_world(), reliable(encode_r(spawn_of(proto::kGuestNetId)))));  // en double
        CHECK_EQ(present, 1);
        if (round == 1) {
            t += kPeerTimeoutMs + 10;  // --kill : timeout
            run(s.tick(t, loaded_world(), {}));
        } else {
            t += 100;
            run(s.tick(t, loaded_world(), reliable(encode_r(proto::Despawn{proto::kGuestNetId}))));
            run(s.tick(t + 1, loaded_world(), reliable(encode_r(proto::SessionEnd{1}))));
        }
        CHECK_EQ(present, 0);
        t += kLingerMs + 10;
        run(s.tick(t, loaded_world(), {}));
        CHECK(s.state() == State::Hosting);
    }
    CHECK_EQ(max_present, 1);
}

TEST("avatars : Spawn à pose non finie → ignoré, avec log") {
    std::uint32_t conv = 0;
    Session s = hosting_with_a(conv);
    proto::Spawn m = spawn_of(proto::kGuestNetId);
    m.y = std::numeric_limits<float>::quiet_NaN();
    Output out = s.tick(1100, loaded_world(), reliable(encode_r(m)));
    CHECK(commands_of<SpawnAvatar>(out).empty());
    CHECK(has_log(out, "ignoré (pose non finie)"));
    m = spawn_of(proto::kGuestNetId);
    m.yaw = std::numeric_limits<float>::infinity();
    out = s.tick(1200, loaded_world(), reliable(encode_r(m)));
    CHECK(commands_of<SpawnAvatar>(out).empty());
    CHECK(!s.avatar_present());
}

TEST("avatars : côté invité, Despawn{1} reçu, fin locale et SessionEnd reçu") {
    auto guest_with_avatar = [] {
        Session g(guest_settings());
        g.tick(0, loaded_world(), {});
        g.tick(100, loaded_world(), control_from(kHost, encode(welcome_at(1, 2, 3, 0))));
        const Output out = g.tick(200, loaded_world(), reliable(encode_r(spawn_of(proto::kHostNetId))));
        CHECK_EQ(commands_of<SpawnAvatar>(out).size(), 1u);
        return g;
    };
    // Despawn{1} : avatar de l'hôte retiré, la session continue.
    Session g = guest_with_avatar();
    Output out = g.tick(300, loaded_world(), reliable(encode_r(proto::Despawn{proto::kHostNetId})));
    auto da = commands_of<DespawnAvatar>(out);
    CHECK_EQ(da.size(), 1u);
    if (!da.empty()) CHECK_EQ(da[0]->net_id, proto::kHostNetId);
    CHECK(g.state() == State::InSession);
    // Fin locale (fin de monde) : Despawn{2} avant SessionEnd{2}, puis DespawnAvatar{1}.
    g = guest_with_avatar();
    out = g.tick(300, loaded_world(kPlayer2), {});
    const auto despawns = sent_of<proto::Despawn>(out);
    CHECK_EQ(despawns.size(), 1u);
    if (!despawns.empty()) CHECK_EQ(despawns[0].net_id, proto::kGuestNetId);
    CHECK(index_of_sent<proto::Despawn>(out) < index_of_sent<proto::SessionEnd>(out));
    da = commands_of<DespawnAvatar>(out);
    CHECK_EQ(da.size(), 1u);
    if (!da.empty()) CHECK_EQ(da[0]->net_id, proto::kHostNetId);
    // SessionEnd reçu : DespawnAvatar{1}, rien d'envoyé.
    g = guest_with_avatar();
    out = g.tick(300, loaded_world(), reliable(encode_r(proto::SessionEnd{2})));
    CHECK(reliables_sent(out).empty());
    CHECK_EQ(commands_of<DespawnAvatar>(out).size(), 1u);
}

// --- Snapshots (story 2.3) -------------------------------------------------------------

namespace {

std::vector<proto::Snapshot> snapshots_sent(const Output& out) {
    std::vector<proto::Snapshot> v;
    for (const auto* s : commands_of<SendSnapshot>(out)) {
        const auto d = proto::decode_snapshot(s->bytes.data(), s->bytes.size());
        CHECK(d.ok());
        if (d.ok()) v.push_back(d.message);
    }
    return v;
}

Bytes snapshot_bytes(std::uint32_t tick, std::uint32_t net_id, float x, float yaw = 0.0f) {
    proto::Snapshot s;
    s.tick = tick;
    proto::SnapshotEntity e;
    e.net_id = net_id;
    e.x = x;
    e.yaw = yaw;
    s.entities.push_back(e);
    Bytes b;
    CHECK(proto::encode_snapshot(s, b));
    return b;
}

std::vector<Input> snapshot_in(Bytes b) { return {SnapshotIn{std::move(b)}}; }

}  // namespace

TEST("snapshots : 20 Hz à l'horloge, tick depuis l'entrée en session, pose et NetId propres") {
    Session s(host_settings());
    s.tick(0, loaded_world(), {});
    s.tick(1000, loaded_world(), control_from(kGuestA, encode(make_hello())));  // t0 = 1000
    std::vector<proto::Snapshot> all;
    for (std::uint64_t t = 1001; t < 2000; t += 7) {  // frames de 7 ms pendant 1 s
        for (const auto& sn : snapshots_sent(s.tick(t, loaded_world(), {}))) all.push_back(sn);
    }
    CHECK(all.size() >= 19 && all.size() <= 20);
    for (std::size_t i = 0; i < all.size(); ++i) {
        CHECK_EQ(all[i].tick, static_cast<std::uint32_t>(i + 1));  // pas de tick 0 (voir enter_session)
        CHECK_EQ(all[i].entities.size(), 1u);
        if (all[i].entities.size() == 1) {
            CHECK_EQ(all[i].entities[0].net_id, proto::kHostNetId);
            CHECK(all[i].entities[0].x == 100.0f && all[i].entities[0].yaw == 1.5f);
            CHECK_EQ(all[i].entities[0].hp, 0);
        }
    }
}

TEST("snapshots : pose illisible → rien n'est envoyé ; hors session → rien") {
    Session s(host_settings());
    World w = loaded_world();
    s.tick(0, w, {});
    CHECK(commands_of<SendSnapshot>(s.tick(500, w, {})).empty());  // Hosting
    s.tick(1000, w, control_from(kGuestA, encode(make_hello())));
    w.pose_ok = false;
    CHECK(commands_of<SendSnapshot>(s.tick(1100, w, {})).empty());
}

TEST("snapshots : reçus du pair → MoveAvatar interpolé avec 150 ms de retard") {
    Session s = host_with_avatar();  // avatar créé à 1100
    // Snapshots du pair : x = 3 × (tick × 50), soit 0,3 cm/ms ; reçus 20 ms après leur tick.
    Output out;
    for (std::uint32_t tick = 0; tick <= 10; ++tick) {
        out = s.tick(1200 + tick * 50, loaded_world(), snapshot_in(snapshot_bytes(tick, proto::kGuestNetId, tick * 15.0f)));
    }
    // Horloge : décalage = 1200 ; temps distant à 1700 = 500 ; affiché = 350 → x = 105.
    const auto moves = commands_of<MoveAvatar>(out);
    CHECK_EQ(moves.size(), 1u);
    if (!moves.empty()) {
        CHECK_EQ(moves[0]->net_id, proto::kGuestNetId);
        CHECK(std::fabs(moves[0]->pose.x - 105.0f) < 0.05f);
    }
}

TEST("snapshots : pas d'avatar → pas de MoveAvatar ; nouveau Spawn → interpolation remise à zéro") {
    std::uint32_t conv = 0;
    Session s = hosting_with_a(conv);
    Output out = s.tick(1100, loaded_world(), snapshot_in(snapshot_bytes(0, proto::kGuestNetId, 50.0f)));
    CHECK(commands_of<MoveAvatar>(out).empty());
    out = s.tick(1150, loaded_world(), reliable(encode_r(spawn_of(proto::kGuestNetId))));
    CHECK(commands_of<MoveAvatar>(out).empty());  // l'échantillon d'avant le Spawn est oublié
    out = s.tick(1200, loaded_world(), snapshot_in(snapshot_bytes(2, proto::kGuestNetId, 70.0f)));
    const auto moves = commands_of<MoveAvatar>(out);
    CHECK_EQ(moves.size(), 1u);
    if (!moves.empty()) CHECK(moves[0]->pose.x == 70.0f);  // un seul échantillon : figé dessus
}

TEST("snapshots : illisible, autre NetId, NaN → ignorés, comptés au bilan debug") {
    Session s = host_with_avatar();
    s.tick(1200, loaded_world(), snapshot_in(Bytes{1, 0, 0}));
    s.tick(1250, loaded_world(), snapshot_in(snapshot_bytes(3, proto::kHostNetId, 10.0f)));
    s.tick(1300, loaded_world(), snapshot_in(snapshot_bytes(4, proto::kGuestNetId, std::nanf(""))));
    s.tick(1320, loaded_world(),
           snapshot_in(snapshot_bytes(5, proto::kGuestNetId, 1.0f, std::numeric_limits<float>::infinity())));
    Output out = s.tick(1350, loaded_world(), {});
    CHECK(commands_of<MoveAvatar>(out).empty());  // aucun échantillon valable
    CHECK(s.state() == State::InSession);
    // Bilan à t0 + 5 s (t0 = 1000).
    for (std::uint64_t t = 1400; t < 6000; t += 500) s.tick(t, loaded_world(), {});
    out = s.tick(6000, loaded_world(), {});
    CHECK(has_log(out, "reçus 3 (illisibles 1, entités ignorées 3)"));
}

TEST("snapshots : un snapshot du pair compte comme activité (pas de timeout)") {
    Session s = host_with_avatar();
    for (std::uint32_t i = 1; i <= 12; ++i) {
        s.tick(1100 + i * 1000, loaded_world(), snapshot_in(snapshot_bytes(i, proto::kGuestNetId, 1.0f)));
    }
    CHECK(s.state() == State::InSession);
}

TEST("snapshots côté invité : NetId 2, pas de tick 0, rien avant la Teleport ; avatar de l'hôte déplacé") {
    Session g(guest_settings());
    g.tick(0, loaded_world(), {});
    Output out = g.tick(100, loaded_world(), control_from(kHost, encode(welcome_at(1, 2, 3, 0))));  // t0 = 100
    CHECK_EQ(commands_of<Teleport>(out).size(), 1u);
    CHECK(commands_of<SendSnapshot>(out).empty());  // la pose lue est celle d'avant la Teleport
    World moved = loaded_world();
    moved.x = 1.0f;  // pose après la Teleport
    out = g.tick(160, moved, {});
    const auto sent = snapshots_sent(out);
    CHECK_EQ(sent.size(), 1u);
    if (!sent.empty()) {
        CHECK_EQ(sent[0].tick, 1u);
        CHECK(!sent[0].entities.empty() && sent[0].entities[0].net_id == proto::kGuestNetId &&
              sent[0].entities[0].x == 1.0f);
    }
    // Spawn de l'hôte, puis ses snapshots : MoveAvatar{1} ; ceux de NetId 2 sont ignorés.
    g.tick(170, moved, reliable(encode_r(spawn_of(proto::kHostNetId))));
    out = g.tick(180, moved, snapshot_in(snapshot_bytes(1, proto::kGuestNetId, 99.0f)));
    CHECK(commands_of<MoveAvatar>(out).empty());
    out = g.tick(190, moved, snapshot_in(snapshot_bytes(2, proto::kHostNetId, 42.0f)));
    const auto moves = commands_of<MoveAvatar>(out);
    CHECK_EQ(moves.size(), 1u);
    if (!moves.empty()) CHECK(moves[0]->net_id == proto::kHostNetId && moves[0]->pose.x == 42.0f);
}

// --- Animations et mort (story 2.4) -------------------------------------------------------

namespace {

proto::Motion local_motion_of(std::uint8_t type = 0, float d0c = 1.0f) {
    proto::Motion m;
    m.type = type;
    m.flags = proto::kMotionFlagPlay | proto::kMotionFlagDesc;
    m.d00 = 0.2f;
    m.d0c = d0c;
    m.d10 = 1;
    m.d14 = 1.0f;
    m.d18 = 1;
    return m;
}

std::vector<Input> local(const std::string& name, std::uint8_t type = 0) {
    return {LocalMotion{name, local_motion_of(type)}};
}

proto::Motion peer_motion(std::uint16_t ani_id, std::uint32_t ms, std::uint8_t type = 0, float d0c = 1.0f) {
    proto::Motion m = local_motion_of(type, d0c);
    m.net_id = proto::kGuestNetId;
    m.ani_id = ani_id;
    m.ms = ms;
    return m;
}

// Hôte avec avatar dont l'horloge est calée : snapshots du pair reçus 20 ms après leur tick,
// décalage 1200 ms (tick 0 à 1200). Temps affiché à `now` = now − 1200 − 150.
Session host_with_clock() {
    Session s = host_with_avatar();
    for (std::uint32_t tick = 0; tick < 4; ++tick) {
        s.tick(1200 + tick * 50, loaded_world(), snapshot_in(snapshot_bytes(tick, proto::kGuestNetId, 0.0f)));
    }
    return s;
}

std::vector<Input> reliable2(const proto::ReliableMessage& a, const proto::ReliableMessage& b) {
    return {ReliableIn{encode_r(a)}, ReliableIn{encode_r(b)}};
}

}  // namespace

TEST("animations : premier geste → AniName{0} puis Motion{ani 0, NetId, ms} ; répété → Motion seul") {
    Session s = host_with_avatar();  // t0 = 1000
    Output out = s.tick(1300, loaded_world(), local("Hero_Run"));
    auto sent = reliables_sent(out);
    CHECK_EQ(sent.size(), 2u);
    if (sent.size() == 2) {
        const auto* an = std::get_if<proto::AniName>(&sent[0]);
        const auto* m = std::get_if<proto::Motion>(&sent[1]);
        CHECK(an != nullptr && an->ani_id == 0 && an->name == "Hero_Run");
        CHECK(m != nullptr && m->ani_id == 0 && m->net_id == proto::kHostNetId && m->ms == 300 && m->d00 == 0.2f);
    }
    out = s.tick(1400, loaded_world(), local("Hero_Jump"));
    CHECK(!sent_of<proto::AniName>(out).empty() && sent_of<proto::AniName>(out)[0].ani_id == 1);
    out = s.tick(1500, loaded_world(), local("Hero_Run"));
    CHECK(sent_of<proto::AniName>(out).empty());
    CHECK(!sent_of<proto::Motion>(out).empty() && sent_of<proto::Motion>(out)[0].ani_id == 0);
}

TEST("animations : hors session rien ne part ; nouvelle session → AniId repart de 0") {
    Session s(host_settings());
    s.tick(0, loaded_world(), {});
    CHECK(reliables_sent(s.tick(100, loaded_world(), local("Hero_Run"))).empty());  // Hosting
    s.tick(1000, loaded_world(), control_from(kGuestA, encode(make_hello())));
    s.tick(1100, loaded_world(), local("A"));
    s.tick(1200, loaded_world(), local("B"));
    s.tick(1300, loaded_world(), reliable(encode_r(proto::SessionEnd{1})));  // fin, réarmement immédiat
    s.tick(2400, loaded_world(), {});
    s.tick(2500, loaded_world(), control_from(kGuestA, encode(make_hello())));
    const Output out = s.tick(2600, loaded_world(), local("B"));
    const auto an = sent_of<proto::AniName>(out);
    CHECK(an.size() == 1 && an[0].ani_id == 0 && an[0].name == "B");
}

TEST("animations : Motion à l'heure attend l'instant d'affichage ; en retard → aussitôt") {
    Session s = host_with_clock();
    s.tick(1360, loaded_world(), reliable(encode_r(proto::AniName{3, "Hero_Attack"})));
    // ms = 400 : affiché à now − 1350 ≥ 400, soit now ≥ 1750.
    Output out = s.tick(1370, loaded_world(), reliable(encode_r(peer_motion(3, 400, 2))));
    CHECK(commands_of<PlayAvatarMotion>(out).empty());
    CHECK(commands_of<PlayAvatarMotion>(s.tick(1749, loaded_world(), {})).empty());
    out = s.tick(1750, loaded_world(), {});
    const auto p = commands_of<PlayAvatarMotion>(out);
    CHECK_EQ(p.size(), 1u);
    if (!p.empty()) CHECK(p[0]->net_id == proto::kGuestNetId && p[0]->name == "Hero_Attack" && p[0]->motion.type == 2);
    // En retard (ms = 100, déjà affiché) : rejoué dans le tick de réception.
    out = s.tick(1800, loaded_world(), reliable(encode_r(peer_motion(3, 100, 2))));
    CHECK_EQ(commands_of<PlayAvatarMotion>(out).size(), 1u);
}

TEST("animations : types 0/1 dus au même tick → seul le plus récent de chacun ; type 2 tous") {
    Session s = host_with_clock();
    s.tick(1360, loaded_world(), reliable2(proto::AniName{0, "Run"}, proto::AniName{1, "Side"}));
    std::vector<Input> in;
    in.push_back(ReliableIn{encode_r(peer_motion(0, 10, 0, 0.1f))});
    in.push_back(ReliableIn{encode_r(peer_motion(1, 20, 1, 0.2f))});
    in.push_back(ReliableIn{encode_r(peer_motion(0, 30, 0, 0.3f))});
    in.push_back(ReliableIn{encode_r(peer_motion(0, 40, 2, 0.4f))});
    in.push_back(ReliableIn{encode_r(peer_motion(0, 50, 0, 0.5f))});
    in.push_back(ReliableIn{encode_r(peer_motion(0, 60, 2, 0.6f))});
    const Output out = s.tick(1500, loaded_world(), in);  // affiché : 150 ms, tout est dû
    const auto p = commands_of<PlayAvatarMotion>(out);
    CHECK_EQ(p.size(), 4u);  // type 1 (0.2), type 2 (0.4), type 0 (0.5), type 2 (0.6), dans l'ordre
    if (p.size() == 4) {
        CHECK(p[0]->motion.d0c == 0.2f && p[1]->motion.d0c == 0.4f && p[2]->motion.d0c == 0.5f &&
              p[3]->motion.d0c == 0.6f);
    }
}

TEST("animations : AniId inconnu, NaN, autre NetId, sans avatar → ignorés ; une ligne par AniId") {
    Session s = host_with_clock();
    Output out = s.tick(1400, loaded_world(), reliable(encode_r(peer_motion(9, 0))));
    CHECK(commands_of<PlayAvatarMotion>(out).empty());
    CHECK(has_log(out, "AniId 9 ignoré : AniId inconnu"));
    out = s.tick(1410, loaded_world(), reliable(encode_r(peer_motion(9, 0))));
    CHECK(!has_log(out, "AniId 9"));  // déjà signalé
    s.tick(1420, loaded_world(), reliable(encode_r(proto::AniName{4, "X"})));
    proto::Motion nan = peer_motion(4, 0);
    nan.d0c = std::nanf("");
    out = s.tick(1430, loaded_world(), reliable(encode_r(nan)));
    CHECK(commands_of<PlayAvatarMotion>(out).empty());
    CHECK(has_log(out, "scalaire non fini"));
    proto::Motion other = peer_motion(4, 0);
    other.net_id = proto::kHostNetId;
    CHECK(commands_of<PlayAvatarMotion>(s.tick(1440, loaded_world(), reliable(encode_r(other)))).empty());
    CHECK_EQ(commands_of<PlayAvatarMotion>(s.tick(1450, loaded_world(), reliable(encode_r(peer_motion(4, 0))))).size(), 1u);
}

TEST("animations : avant tout snapshot, un Motion part aussitôt") {
    Session s = host_with_avatar();
    const Output out = s.tick(1200, loaded_world(),
                              reliable2(proto::AniName{0, "Run"}, peer_motion(0, 999999)));
    CHECK_EQ(commands_of<PlayAvatarMotion>(out).size(), 1u);
}

TEST("animations : file de 256 au plus, le plus ancien est jeté") {
    Session s = host_with_clock();
    s.tick(1360, loaded_world(), reliable(encode_r(proto::AniName{0, "A"})));
    // À 1370, affiché 20 : ms 500 à 799 attendent (moins d'1 s d'avance).
    for (std::uint32_t i = 0; i < 300; ++i) s.tick(1370, loaded_world(), reliable(encode_r(peer_motion(0, 500 + i, 2))));
    // À 3000, affiché 1650 : tout est dû ; 256 rejoués, les 44 plus anciens jetés.
    const auto p = commands_of<PlayAvatarMotion>(s.tick(3000, loaded_world(), {}));
    CHECK_EQ(p.size(), kMaxPendingMotions);
    if (!p.empty()) CHECK_EQ(p[0]->motion.ms, 544u);
}

TEST("animations : un Motion à plus d'1 s d'avance part aussitôt, sans bloquer les suivants") {
    Session s = host_with_clock();  // affiché à 1400 : 50 ms
    s.tick(1360, loaded_world(), reliable(encode_r(proto::AniName{0, "A"})));
    Output out = s.tick(1400, loaded_world(), reliable2(peer_motion(0, 999999, 2), peer_motion(0, 40, 2)));
    CHECK_EQ(commands_of<PlayAvatarMotion>(out).size(), 2u);
}

TEST("mort : IsDead passe à 1 en session → Death une fois par mort ; hors session → rien") {
    Session g(guest_settings());
    g.tick(0, loaded_world(), {});
    g.tick(100, loaded_world(), control_from(kHost, encode(welcome_at(1, 2, 3, 0))));
    World dead = loaded_world();
    dead.dead = true;
    Output out = g.tick(200, dead, {});
    const auto d = sent_of<proto::Death>(out);
    CHECK(d.size() == 1 && d[0].net_id == proto::kGuestNetId);
    CHECK(sent_of<proto::Death>(g.tick(300, dead, {})).empty());  // même mort
    CHECK(g.state() == State::InSession);                         // l'invité mort reste en session
    CHECK(sent_of<proto::Death>(g.tick(400, loaded_world(), {})).empty());  // retour à la vie
    CHECK_EQ(sent_of<proto::Death>(g.tick(500, dead, {})).size(), 1u);      // nouvelle mort
    Session idle(guest_settings());
    idle.tick(0, loaded_world(), {});
    CHECK(sent_of<proto::Death>(idle.tick(100, dead, {})).empty());  // Connecting
}

TEST("mort : un ragdoll sans mort n'envoie rien (aucune entrée ragdoll) ; mort de l'hôte → SessionEnd{3}") {
    Session s = host_with_avatar();
    for (std::uint64_t t = 1200; t < 2000; t += 100) CHECK(sent_of<proto::Death>(s.tick(t, loaded_world(), {})).empty());
    World dead = loaded_world();
    dead.dead = true;
    const Output out = s.tick(2000, dead, {});
    CHECK(sent_of<proto::Death>(out).empty());
    CHECK(sent_session_end(out, 3));
}

TEST("mort : Death du pair → AvatarDeath à l'instant d'affichage, puis plus de MoveAvatar ni d'animation") {
    Session s = host_with_clock();  // dernier snapshot : tick 3 (150 ms) ; affiché = now − 1350
    s.tick(1360, loaded_world(), reliable2(proto::AniName{0, "A"}, peer_motion(0, 400, 2)));  // en attente
    Output out = s.tick(1400, loaded_world(), reliable(encode_r(proto::Death{proto::kGuestNetId})));
    CHECK(commands_of<AvatarDeath>(out).empty());  // affiché 50 < 150 : pas encore
    CHECK(!commands_of<MoveAvatar>(out).empty());
    CHECK(!s.avatar_dead());
    CHECK(commands_of<AvatarDeath>(s.tick(1499, loaded_world(), {})).empty());
    out = s.tick(1500, loaded_world(), {});
    CHECK_EQ(commands_of<AvatarDeath>(out).size(), 1u);
    CHECK(commands_of<MoveAvatar>(out).empty());
    CHECK(s.avatar_dead());
    out = s.tick(1510, loaded_world(), reliable2(peer_motion(0, 0, 2), proto::Death{proto::kGuestNetId}));
    CHECK(commands_of<PlayAvatarMotion>(out).empty() && commands_of<AvatarDeath>(out).empty());
    CHECK(has_log(out, "avatar déjà mort"));
    for (std::uint64_t t = 1500; t < 4000; t += 250) CHECK(commands_of<PlayAvatarMotion>(s.tick(t, loaded_world(), {})).empty());
    // Despawn puis Spawn : vivant à nouveau.
    s = host_with_clock();
    s.tick(1400, loaded_world(), reliable(encode_r(proto::Death{proto::kGuestNetId})));
    s.tick(1410, loaded_world(), reliable2(proto::Despawn{proto::kGuestNetId}, spawn_of(proto::kGuestNetId)));
    CHECK(!s.avatar_dead());
    CHECK(has_log(s.tick(1420, loaded_world(), reliable(encode_r(proto::Death{proto::kHostNetId}))), "pas l'avatar du pair"));
}

TEST("animations côté invité : Motion envoyé sous NetId 2, Motion de l'hôte rejoué sur l'avatar 1") {
    Session g(guest_settings());
    g.tick(0, loaded_world(), {});
    g.tick(100, loaded_world(), control_from(kHost, encode(welcome_at(1, 2, 3, 0))));
    Output out = g.tick(150, loaded_world(), local("G"));
    const auto m = sent_of<proto::Motion>(out);
    CHECK(m.size() == 1 && m[0].net_id == proto::kGuestNetId && m[0].ms == 50);
    g.tick(160, loaded_world(), reliable(encode_r(spawn_of(proto::kHostNetId))));
    proto::Motion hm = peer_motion(5, 0, 2);
    hm.net_id = proto::kHostNetId;
    out = g.tick(170, loaded_world(), reliable2(proto::AniName{5, "H"}, hm));
    const auto p = commands_of<PlayAvatarMotion>(out);
    CHECK(p.size() == 1 && p[0]->net_id == proto::kHostNetId && p[0]->name == "H");
}

TEST("animations : nouvelle session → table des noms du pair vidée (AniId d'avant inconnu)") {
    Session s = host_with_avatar();
    s.tick(1200, loaded_world(), reliable2(proto::AniName{0, "Vieux"}, peer_motion(0, 0, 2)));
    s.tick(1300, loaded_world(), reliable(encode_r(proto::SessionEnd{1})));  // fin, réarmement immédiat
    s.tick(2400, loaded_world(), {});
    s.tick(2500, loaded_world(), control_from(kGuestA, encode(make_hello())));
    s.tick(2600, loaded_world(), reliable(encode_r(spawn_of(proto::kGuestNetId))));
    const Output out = s.tick(2700, loaded_world(), reliable(encode_r(peer_motion(0, 0, 2))));
    CHECK(commands_of<PlayAvatarMotion>(out).empty());
    CHECK(has_log(out, "AniId 0 ignoré : AniId inconnu"));
}

TEST("animations : AniName redéfini → Warn et nouveau nom ; retrait de l'avatar → file vidée") {
    Session s = host_with_clock();
    s.tick(1360, loaded_world(), reliable(encode_r(proto::AniName{0, "A"})));
    Output out = s.tick(1370, loaded_world(), reliable(encode_r(proto::AniName{0, "B"})));
    CHECK(has_log(out, "AniName 0 redéfini : « A » → « B »"));
    s.tick(1380, loaded_world(), reliable(encode_r(peer_motion(0, 900, 2))));  // en attente
    s.tick(1390, loaded_world(), reliable2(proto::Despawn{proto::kGuestNetId}, spawn_of(proto::kGuestNetId)));
    for (std::uint64_t t = 1400; t < 3000; t += 100) CHECK(commands_of<PlayAvatarMotion>(s.tick(t, loaded_world(), {})).empty());
}

TEST("animations : 65 536 noms internés au plus, puis une seule ligne et plus rien") {
    Session s = host_with_avatar();
    std::vector<Input> in;
    for (std::uint32_t i = 0; i < kAniIdCount; ++i) in.push_back(LocalMotion{"n" + std::to_string(i), local_motion_of()});
    Output out = s.tick(1200, loaded_world(), in);
    CHECK_EQ(sent_of<proto::AniName>(out).size(), static_cast<std::size_t>(kAniIdCount));
    out = s.tick(1300, loaded_world(), {LocalMotion{"encore", local_motion_of()}, LocalMotion{"et_encore", local_motion_of()}});
    CHECK(reliables_sent(out).empty() || sent_of<proto::Heartbeat>(out).size() == reliables_sent(out).size());
    CHECK(has_log(out, "65536 noms d'animation"));
    out = s.tick(1400, loaded_world(), {LocalMotion{"n5", local_motion_of()}});  // nom déjà interné : part
    CHECK_EQ(sent_of<proto::Motion>(out).size(), 1u);
}

TEST("animations : Motion local à scalaire non fini ou hors domaine → rien, pas même l'AniName") {
    Session s = host_with_avatar();
    proto::Motion bad = local_motion_of();
    bad.d14 = std::numeric_limits<float>::infinity();
    Output out = s.tick(1200, loaded_world(), {LocalMotion{"X", bad}});
    CHECK(sent_of<proto::AniName>(out).empty() && sent_of<proto::Motion>(out).empty());
    bad = local_motion_of(9);
    out = s.tick(1300, loaded_world(), {LocalMotion{"Y", bad}});
    CHECK(sent_of<proto::AniName>(out).empty() && sent_of<proto::Motion>(out).empty());
    out = s.tick(1400, loaded_world(), {LocalMotion{"X", local_motion_of()}});
    const auto an = sent_of<proto::AniName>(out);
    CHECK(an.size() == 1 && an[0].ani_id == 0);  // X n'avait pas été interné
}

// --- Réglages de locomotion (story 2.4b) --------------------------------------------------

namespace {

std::vector<Input> local_param(std::uint8_t type, proto::MotionParamKind kind, float value, float time = 0.0f) {
    return {LocalParam{type, static_cast<std::uint8_t>(kind), value, time}};
}

proto::MotionParam peer_param(std::uint32_t ms, std::uint8_t type, float value, float time = 0.25f) {
    proto::MotionParam p;
    p.net_id = proto::kGuestNetId;
    p.ms = ms;
    p.type = type;
    p.kind = static_cast<std::uint8_t>(proto::MotionParamKind::TargetWeight);
    p.value = value;
    p.time = time;
    return p;
}

}  // namespace

TEST("locomotion : démarrage → Motion puis MotionParam ; 300 réglages identiques → un seul") {
    Session s = host_with_avatar();  // t0 = 1000
    Output out = s.tick(1200, loaded_world(), local("Move_Begin"));
    std::vector<Input> in;
    for (int i = 0; i < 300; ++i) in.push_back(LocalParam{0, 0, 1.0f, 0.25f});
    out = s.tick(1210, loaded_world(), in);
    const auto p = sent_of<proto::MotionParam>(out);
    CHECK_EQ(p.size(), 1u);
    if (!p.empty()) {
        CHECK(p[0].net_id == proto::kHostNetId && p[0].ms == 210 && p[0].type == 0 && p[0].kind == 0 &&
              p[0].value == 1.0f && p[0].time == 0.25f);
    }
    CHECK(sent_of<proto::MotionParam>(s.tick(1220, loaded_world(), local_param(0, proto::MotionParamKind::TargetWeight, 1.0f, 0.25f))).empty());
    // Même valeur, autre durée → renvoyé.
    CHECK_EQ(sent_of<proto::MotionParam>(s.tick(1230, loaded_world(), local_param(0, proto::MotionParamKind::TargetWeight, 1.0f, 0.5f))).size(), 1u);
}

TEST("locomotion : accélération 0,03 → 1,0 → un MotionParam par pas de plus de 0,02 (vitesse)") {
    Session s = host_with_avatar();
    std::vector<Input> in;
    for (int i = 0; i < 970; ++i) in.push_back(LocalParam{0, 1, 0.03f + 0.001f * static_cast<float>(i), 0.0f});
    in.push_back(LocalParam{0, 1, 1.0f, 0.0f});  // fin de la rampe, exacte
    const auto p = sent_of<proto::MotionParam>(s.tick(1200, loaded_world(), in));
    CHECK(p.size() >= 40 && p.size() <= 55);  // ≈ 0,97 / 0,021
    for (std::size_t i = 1; i + 1 < p.size(); ++i) CHECK(p[i].value - p[i - 1].value > kParamSpeedEpsilon);
    if (!p.empty()) CHECK(p[0].kind == 1 && p[0].value == 0.03f && p.back().value == 1.0f);
}

TEST("locomotion : nouveau Motion du même type → le réglage identique repart ; autre type non") {
    Session s = host_with_avatar();
    s.tick(1200, loaded_world(), local_param(0, proto::MotionParamKind::TargetWeight, 1.0f, 0.25f));
    s.tick(1210, loaded_world(), local_param(1, proto::MotionParamKind::TargetWeight, 0.0f, 0.25f));
    s.tick(1220, loaded_world(), local("Run", 0));  // invalide le type 0 seulement
    Output out = s.tick(1230, loaded_world(), local_param(0, proto::MotionParamKind::TargetWeight, 1.0f, 0.25f));
    CHECK_EQ(sent_of<proto::MotionParam>(out).size(), 1u);
    out = s.tick(1240, loaded_world(), local_param(1, proto::MotionParamKind::TargetWeight, 0.0f, 0.25f));
    CHECK(sent_of<proto::MotionParam>(out).empty());
}

TEST("locomotion : valeur non finie, type 8, nature 4, hors session → rien") {
    Session s = host_with_avatar();
    CHECK(sent_of<proto::MotionParam>(s.tick(1200, loaded_world(), local_param(0, proto::MotionParamKind::PlaySpeed, std::nanf("")))).empty());
    CHECK(sent_of<proto::MotionParam>(s.tick(1210, loaded_world(), {LocalParam{8, 0, 1.0f, 0.0f}})).empty());
    CHECK(sent_of<proto::MotionParam>(s.tick(1220, loaded_world(), {LocalParam{0, 4, 1.0f, 0.0f}})).empty());
    Session idle(host_settings());
    idle.tick(0, loaded_world(), {});
    CHECK(reliables_sent(idle.tick(100, loaded_world(), local_param(0, proto::MotionParamKind::PlaySpeed, 1.0f))).empty());
}

TEST("locomotion : réception → même instant que les Motion, dans l'ordre ; après un Motion écrasé, appliqué") {
    Session s = host_with_clock();  // affiché = now − 1350
    s.tick(1360, loaded_world(), reliable(encode_r(proto::AniName{0, "Move_Begin"})));
    std::vector<Input> in;
    in.push_back(ReliableIn{encode_r(peer_motion(0, 400, 0, 0.004f))});
    in.push_back(ReliableIn{encode_r(peer_param(400, 0, 1.0f))});
    in.push_back(ReliableIn{encode_r(peer_motion(0, 410, 0, 0.5f))});  // écrase le premier au même tick
    in.push_back(ReliableIn{encode_r(peer_param(420, 0, 0.8f))});
    Output out = s.tick(1370, loaded_world(), in);
    CHECK(commands_of<PlayAvatarParam>(out).empty());  // pas encore l'heure
    out = s.tick(1780, loaded_world(), {});            // affiché 430 : tout est dû
    // Le Motion 0.004 est écrasé par le 0.5 ; le réglage 1.0 qui le visait aussi. Reste : M0.5, P0.8.
    std::vector<std::string> order;
    for (const auto& c : out.commands) {
        if (const auto* m = std::get_if<PlayAvatarMotion>(&c)) order.push_back("M" + std::to_string(m->motion.d0c));
        if (const auto* p = std::get_if<PlayAvatarParam>(&c)) order.push_back("P" + std::to_string(p->param.value));
    }
    CHECK_EQ(order.size(), 2u);
    if (order.size() == 2) {
        CHECK(order[0] == "M" + std::to_string(0.5f));
        CHECK(order[1] == "P" + std::to_string(0.8f));
    }
    const auto params = commands_of<PlayAvatarParam>(out);
    if (!params.empty()) CHECK(params[0]->net_id == proto::kGuestNetId && params[0]->param.time == 0.25f);
    // Sans Motion écrasé, un réglage qui précède le Motion gardé de son type s'applique.
    std::vector<Input> in2;
    in2.push_back(ReliableIn{encode_r(peer_param(500, 1, 0.3f))});
    in2.push_back(ReliableIn{encode_r(peer_motion(0, 510, 0, 0.7f))});
    out = s.tick(1880, loaded_world(), in2);
    CHECK_EQ(commands_of<PlayAvatarParam>(out).size(), 1u);  // type 1 : pas visé par le Motion de type 0
}

TEST("locomotion : MotionParam sans avatar, d'un autre NetId ou avatar mort → ignoré") {
    std::uint32_t conv = 0;
    Session none = hosting_with_a(conv);
    CHECK(commands_of<PlayAvatarParam>(none.tick(1100, loaded_world(), reliable(encode_r(peer_param(0, 0, 1.0f))))).empty());
    Session s = host_with_clock();
    proto::MotionParam other = peer_param(0, 0, 1.0f);
    other.net_id = proto::kHostNetId;
    CHECK(commands_of<PlayAvatarParam>(s.tick(1400, loaded_world(), reliable(encode_r(other)))).empty());
    s.tick(1410, loaded_world(), reliable(encode_r(proto::Death{proto::kGuestNetId})));
    s.tick(1500, loaded_world(), {});  // mort appliquée
    CHECK(s.avatar_dead());
    CHECK(commands_of<PlayAvatarParam>(s.tick(1510, loaded_world(), reliable(encode_r(peer_param(0, 0, 1.0f))))).empty());
    // Despawn puis Spawn : le réglage reçu pendant la mort n'a pas été gardé en attente.
    s.tick(1520, loaded_world(), reliable2(proto::Despawn{proto::kGuestNetId}, spawn_of(proto::kGuestNetId)));
    CHECK(commands_of<PlayAvatarParam>(s.tick(1530, loaded_world(), {})).empty());
}

TEST("locomotion : valeur finale exacte (0 ou 1) toujours envoyée ; durée comparée avec tolérance") {
    Session s = host_with_avatar();
    s.tick(1200, loaded_world(), local_param(0, proto::MotionParamKind::TargetWeight, 0.995f, 0.25f));
    auto p = sent_of<proto::MotionParam>(s.tick(1210, loaded_world(), local_param(0, proto::MotionParamKind::TargetWeight, 1.0f, 0.25f)));
    CHECK(p.size() == 1 && p[0].value == 1.0f);
    s.tick(1220, loaded_world(), local_param(1, proto::MotionParamKind::TargetWeight, 0.008f, 0.25f));
    p = sent_of<proto::MotionParam>(s.tick(1230, loaded_world(), local_param(1, proto::MotionParamKind::TargetWeight, 0.0f, 0.25f)));
    CHECK(p.size() == 1 && p[0].value == 0.0f);
    // Durée qui varie de moins de 0,005 s : pas renvoyée.
    CHECK(sent_of<proto::MotionParam>(s.tick(1240, loaded_world(), local_param(1, proto::MotionParamKind::TargetWeight, 0.0f, 0.252f))).empty());
}

TEST("locomotion : nouvelle session → le premier réglage repart, même identique") {
    Session s = host_with_avatar();
    CHECK_EQ(sent_of<proto::MotionParam>(s.tick(1200, loaded_world(), local_param(0, proto::MotionParamKind::TargetWeight, 1.0f, 0.25f))).size(), 1u);
    s.tick(1300, loaded_world(), reliable(encode_r(proto::SessionEnd{1})));  // fin, réarmement immédiat
    s.tick(2400, loaded_world(), {});
    s.tick(2500, loaded_world(), control_from(kGuestA, encode(make_hello())));
    CHECK(s.state() == State::InSession);
    CHECK_EQ(sent_of<proto::MotionParam>(s.tick(2600, loaded_world(), local_param(0, proto::MotionParamKind::TargetWeight, 1.0f, 0.25f))).size(), 1u);
}

TEST("locomotion : fondu et arrêt (natures 2 et 3) — envoyés si différents, rejoués, valeur 0/1 exigée") {
    Session s = host_with_avatar();
    auto fade = local_param(0, proto::MotionParamKind::FadeOut, 1.0f, 0.3f);
    CHECK_EQ(sent_of<proto::MotionParam>(s.tick(1200, loaded_world(), fade)).size(), 1u);
    CHECK(sent_of<proto::MotionParam>(s.tick(1210, loaded_world(), fade)).empty());  // identique
    auto stop = local_param(0, proto::MotionParamKind::StopMotion, 0.0f, 0.2f);
    auto sent = sent_of<proto::MotionParam>(s.tick(1220, loaded_world(), stop));
    CHECK(sent.size() == 1 && sent[0].kind == 3 && sent[0].value == 0.0f && sent[0].time == 0.2f);
    CHECK_EQ(sent_of<proto::MotionParam>(s.tick(1230, loaded_world(), local_param(0, proto::MotionParamKind::StopMotion, 0.0f, 0.4f))).size(), 1u);
    CHECK(sent_of<proto::MotionParam>(s.tick(1240, loaded_world(), local_param(0, proto::MotionParamKind::FadeOut, 0.5f, 0.3f))).empty());
    // Réception : rejoué tel quel.
    Session r = host_with_clock();
    proto::MotionParam p = peer_param(0, 1, 1.0f, 0.3f);
    p.kind = static_cast<std::uint8_t>(proto::MotionParamKind::FadeOut);
    const auto played = commands_of<PlayAvatarParam>(r.tick(1400, loaded_world(), reliable(encode_r(p))));
    CHECK(played.size() == 1 && played[0]->param.kind == 2 && played[0]->param.time == 0.3f);
}
