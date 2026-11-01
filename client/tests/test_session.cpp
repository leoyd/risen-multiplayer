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

bool has_log_at(const Output& out, Level level, const std::string& needle) {
    for (const auto& l : out.logs) {
        if (l.level == level && l.text.find(needle) != std::string::npos) return true;
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

TEST("session invité : équipement invalide (300 octets) → Warn, traité comme vide, Hello envoyé") {
    Settings st = guest_settings();
    st.equipment.assign(300, 'x');
    Session s(st);
    const Output out = s.tick(0, loaded_world(), {});
    CHECK(s.state() == State::Connecting);
    CHECK_EQ(hellos_sent(out), 1u);
    CHECK(!has_log(out, "Hello non encodable"));
    bool warned = false;
    for (const auto& l : out.logs) {
        if (l.level == Level::Warn && l.text.find("équipement") != std::string::npos) warned = true;
    }
    CHECK(warned);
    CHECK(!has_log(s.tick(500, loaded_world(), {}), "équipement"));  // une seule fois
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
    // v9 : un type inconnu n'est plus décodable ; un Spawn{1} (PNJ) venu de l'invité est ignoré.
    out = s.tick(1200, loaded_world(), reliable(encode_r(spawn_of(proto::kGuestNetId, proto::kSpawnTypeNpc))));
    CHECK(commands_of<SpawnAvatar>(out).empty());
    CHECK(commands_of<SpawnNpc>(out).empty());
    CHECK(has_log(out, "l'hôte fait autorité sur les PNJ"));
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

// Story 3.7 : chez l'hôte, Death{2} ne met plus l'avatar en ragdoll ; seul l'invité applique encore
// la mort v5 (Death{1}). Invité en session (t0 = 100), avatar de l'hôte créé à 1100, horloge calée
// comme host_with_clock : temps affiché à `now` = now − 1200 − 150.
Session guest_with_clock() {
    Session g(guest_settings());
    g.tick(0, loaded_world(), {});
    g.tick(100, loaded_world(), control_from(kHost, encode(welcome_at(1, 2, 3, 0))));
    const Output out = g.tick(1100, loaded_world(), reliable(encode_r(spawn_of(proto::kHostNetId))));
    CHECK_EQ(commands_of<SpawnAvatar>(out).size(), 1u);
    for (std::uint32_t tick = 0; tick < 4; ++tick) {
        g.tick(1200 + tick * 50, loaded_world(), snapshot_in(snapshot_bytes(tick, proto::kHostNetId, 0.0f)));
    }
    return g;
}

// Motion ou réglage venu de l'hôte (NetId 1), pour guest_with_clock.
proto::Motion host_motion(std::uint16_t ani_id, std::uint32_t ms, std::uint8_t type = 0) {
    proto::Motion m = peer_motion(ani_id, ms, type);
    m.net_id = proto::kHostNetId;
    return m;
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

TEST("mort : IsDead passe à 1 en session → Death une fois (verrou 3.7 jusqu'au Respawn) ; hors session → rien") {
    Session g(guest_settings());
    g.tick(0, loaded_world(), {});
    g.tick(100, loaded_world(), control_from(kHost, encode(welcome_at(1, 2, 3, 0))));
    World dead = loaded_world();
    dead.dead = true;
    Output out = g.tick(200, dead, {});
    const auto d = sent_of<proto::Death>(out);
    CHECK(d.size() == 1 && d[0].net_id == proto::kGuestNetId);
    CHECK(has_log_at(out, Level::Warn, "[sync] mort native du héros (chute, eau profonde ?) : Death envoyé ; "
                                       "réapparition impossible, la session finira au rechargement"));
    CHECK(g.guest_down());
    CHECK(sent_of<proto::Death>(g.tick(300, dead, {})).empty());  // même mort
    CHECK(g.state() == State::InSession);                         // l'invité mort reste en session
    CHECK(sent_of<proto::Death>(g.tick(400, loaded_world(), {})).empty());  // retour à la vie
    out = g.tick(500, dead, {});                                            // nouvelle mort, encore à terre
    CHECK(sent_of<proto::Death>(out).empty());
    CHECK(has_log_at(out, Level::Warn, "Death déjà envoyé"));
    g.tick(600, loaded_world(), {});
    // Respawn reçu (héros revenu à la vie) : verrou levé, une nouvelle mort renvoie Death.
    g.tick(700, loaded_world(), reliable(encode_r(proto::Respawn{proto::kGuestNetId, 1, 2, 3, 0, 0})));
    CHECK(!g.guest_down());
    CHECK_EQ(sent_of<proto::Death>(g.tick(800, dead, {})).size(), 1u);
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

TEST("mort : Death du pair (invité, Death{1}) → AvatarDeath à l'instant d'affichage, puis plus de MoveAvatar ni d'animation") {
    Session s = guest_with_clock();  // dernier snapshot : tick 3 (150 ms) ; affiché = now − 1350
    s.tick(1360, loaded_world(), reliable2(proto::AniName{0, "A"}, host_motion(0, 400, 2)));  // en attente
    Output out = s.tick(1400, loaded_world(), reliable(encode_r(proto::Death{proto::kHostNetId})));
    CHECK(commands_of<AvatarDeath>(out).empty());  // affiché 50 < 150 : pas encore
    CHECK(!commands_of<MoveAvatar>(out).empty());
    CHECK(!s.avatar_dead());
    CHECK(commands_of<AvatarDeath>(s.tick(1499, loaded_world(), {})).empty());
    out = s.tick(1500, loaded_world(), {});
    CHECK_EQ(commands_of<AvatarDeath>(out).size(), 1u);
    CHECK(commands_of<MoveAvatar>(out).empty());
    CHECK(s.avatar_dead());
    out = s.tick(1510, loaded_world(), reliable2(host_motion(0, 0, 2), proto::Death{proto::kHostNetId}));
    CHECK(commands_of<PlayAvatarMotion>(out).empty() && commands_of<AvatarDeath>(out).empty());
    CHECK(has_log(out, "avatar déjà mort"));
    for (std::uint64_t t = 1500; t < 4000; t += 250) CHECK(commands_of<PlayAvatarMotion>(s.tick(t, loaded_world(), {})).empty());
    // Despawn puis Spawn : vivant à nouveau.
    s = guest_with_clock();
    s.tick(1400, loaded_world(), reliable(encode_r(proto::Death{proto::kHostNetId})));
    s.tick(1410, loaded_world(), reliable2(proto::Despawn{proto::kHostNetId}, spawn_of(proto::kHostNetId)));
    CHECK(!s.avatar_dead());
    CHECK(has_log(s.tick(1420, loaded_world(), reliable(encode_r(proto::Death{proto::kGuestNetId}))), "pas l'avatar du pair"));
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
    // Avatar mort : chez l'invité seulement depuis la story 3.7 (Death{1}).
    s = guest_with_clock();
    proto::MotionParam from_host = peer_param(0, 0, 1.0f);
    from_host.net_id = proto::kHostNetId;
    s.tick(1410, loaded_world(), reliable(encode_r(proto::Death{proto::kHostNetId})));
    s.tick(1500, loaded_world(), {});  // mort appliquée
    CHECK(s.avatar_dead());
    CHECK(commands_of<PlayAvatarParam>(s.tick(1510, loaded_world(), reliable(encode_r(from_host)))).empty());
    // Despawn puis Spawn : le réglage reçu pendant la mort n'a pas été gardé en attente.
    s.tick(1520, loaded_world(), reliable2(proto::Despawn{proto::kHostNetId}, spawn_of(proto::kHostNetId)));
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

// --- Arme de l'avatar (story 3.2) -----------------------------------------------------------

namespace {

std::vector<Input> local_weapon(const std::string& name) { return {LocalWeapon{name}}; }

}  // namespace

TEST("arme : LocalWeapon en session → Weapon au changement seulement ; rangée → nom vide") {
    Session s = host_with_avatar();
    Output out = s.tick(1200, loaded_world(), local_weapon("It_1H_Sword"));
    auto w = sent_of<proto::Weapon>(out);
    CHECK(w.size() == 1 && w[0].net_id == proto::kHostNetId && w[0].name == "It_1H_Sword");
    CHECK(has_log(out, "Weapon envoyé : NetId 1 (hôte), « It_1H_Sword »"));
    CHECK(sent_of<proto::Weapon>(s.tick(1210, loaded_world(), local_weapon("It_1H_Sword"))).empty());
    w = sent_of<proto::Weapon>(s.tick(1220, loaded_world(), local_weapon("")));
    CHECK(w.size() == 1 && w[0].name.empty());
    w = sent_of<proto::Weapon>(s.tick(1230, loaded_world(), local_weapon("It_2H_Axe")));
    CHECK(w.size() == 1 && w[0].name == "It_2H_Axe");
}

TEST("arme : retenue hors session, envoyée à l'entrée en session juste après le Spawn") {
    Session s(host_settings());
    s.tick(0, loaded_world(), local_weapon("It_1H_Sword"));  // Inactive → Hosting
    CHECK(sent_of<proto::Weapon>(s.tick(100, loaded_world(), {})).empty());
    const Output out = s.tick(1000, loaded_world(), control_from(kGuestA, encode(make_hello())));
    const auto sent = reliables_sent(out);
    CHECK_EQ(sent.size(), 3u);  // Spawn, Weapon, Heartbeat
    if (sent.size() >= 2) {
        CHECK(std::holds_alternative<proto::Spawn>(sent[0]));
        const auto* w = std::get_if<proto::Weapon>(&sent[1]);
        CHECK(w != nullptr && w->net_id == proto::kHostNetId && w->name == "It_1H_Sword");
    }
    // Invité : idem, sous NetId 2, arme rangée comprise.
    Session g(guest_settings());
    g.tick(0, loaded_world(), local_weapon(""));
    const auto gw = sent_of<proto::Weapon>(g.tick(100, loaded_world(), control_from(kHost, encode(welcome_at(1, 2, 3, 0)))));
    CHECK(gw.size() == 1 && gw[0].net_id == proto::kGuestNetId && gw[0].name.empty());
}

TEST("arme : jamais lue → pas de Weapon à l'entrée ; nouvelle session → renvoyée") {
    std::uint32_t conv = 0;
    Session s = hosting_with_a(conv);
    // hosting_with_a n'a reçu aucun LocalWeapon : rien n'est parti.
    s.tick(1100, loaded_world(), local_weapon("It_1H_Sword"));
    s.tick(1300, loaded_world(), reliable(encode_r(proto::SessionEnd{1})));  // fin, réarmement immédiat
    s.tick(2400, loaded_world(), {});
    const auto w = sent_of<proto::Weapon>(s.tick(2500, loaded_world(), control_from(kGuestA, encode(make_hello()))));
    CHECK(w.size() == 1 && w[0].name == "It_1H_Sword");
    Session fresh(host_settings());
    fresh.tick(0, loaded_world(), {});
    CHECK(sent_of<proto::Weapon>(fresh.tick(1000, loaded_world(), control_from(kGuestA, encode(make_hello())))).empty());
}

TEST("arme : Weapon du pair → AvatarWeapon à l'instant d'affichage, avant les gestes du même tick") {
    Session s = host_with_clock();  // dernier snapshot : 150 ms ; affiché = now − 1350
    Output out = s.tick(1400, loaded_world(), reliable(encode_r(proto::Weapon{proto::kGuestNetId, "It_1H_Sword"})));
    CHECK(commands_of<AvatarWeapon>(out).empty());  // affiché 50 < 150
    CHECK(has_log(out, "Weapon reçu : avatar 2 (invité), « It_1H_Sword »"));
    s.tick(1410, loaded_world(), reliable2(proto::AniName{0, "Hero_Attack"}, peer_motion(0, 150, 2)));
    CHECK(commands_of<AvatarWeapon>(s.tick(1499, loaded_world(), {})).empty());
    out = s.tick(1500, loaded_world(), {});
    const auto aw = commands_of<AvatarWeapon>(out);
    CHECK(aw.size() == 1 && aw[0]->net_id == proto::kGuestNetId && aw[0]->name == "It_1H_Sword");
    CHECK(index_of<AvatarWeapon>(out) < index_of<PlayAvatarMotion>(out));
    // Arme rangée : nom vide transmis tel quel.
    out = s.tick(1510, loaded_world(), reliable(encode_r(proto::Weapon{proto::kGuestNetId, ""})));
    const auto sheathed = commands_of<AvatarWeapon>(out);
    CHECK(sheathed.size() == 1 && sheathed[0]->name.empty());
}

TEST("arme : avant tout snapshot (apparition), AvatarWeapon part aussitôt, après SpawnAvatar") {
    std::uint32_t conv = 0;
    Session s = hosting_with_a(conv);
    const Output out = s.tick(1100, loaded_world(),
                              reliable2(spawn_of(proto::kGuestNetId), proto::Weapon{proto::kGuestNetId, "It_1H_Sword"}));
    CHECK_EQ(commands_of<AvatarWeapon>(out).size(), 1u);
    CHECK(index_of<SpawnAvatar>(out) < index_of<AvatarWeapon>(out));
}

TEST("arme : Weapon sans avatar, d'un autre NetId ou avatar mort → ignoré (debug) ; retrait → file vidée") {
    std::uint32_t conv = 0;
    Session s = hosting_with_a(conv);
    Output out = s.tick(1100, loaded_world(), reliable(encode_r(proto::Weapon{proto::kGuestNetId, "It_1H_Sword"})));
    CHECK(commands_of<AvatarWeapon>(out).empty());
    bool debug = false;
    for (const auto& l : out.logs) {
        if (l.text.find("Weapon reçu") != std::string::npos && l.text.find("pas d'avatar") != std::string::npos) {
            debug = l.level == Level::Debug;
        }
    }
    CHECK(debug);
    Session c = host_with_clock();
    out = c.tick(1400, loaded_world(), reliable(encode_r(proto::Weapon{proto::kHostNetId, "X"})));
    CHECK(has_log(out, "pas l'avatar du pair"));
    c.tick(1410, loaded_world(), reliable(encode_r(proto::Weapon{proto::kGuestNetId, "It_1H_Sword"})));  // en attente
    c.tick(1420, loaded_world(), reliable2(proto::Despawn{proto::kGuestNetId}, spawn_of(proto::kGuestNetId)));
    for (std::uint64_t t = 1430; t < 3000; t += 100) CHECK(commands_of<AvatarWeapon>(c.tick(t, loaded_world(), {})).empty());
    Session d = guest_with_clock();  // story 3.7 : avatar mort chez l'invité seulement (Death{1})
    d.tick(1400, loaded_world(), reliable(encode_r(proto::Death{proto::kHostNetId})));
    d.tick(1500, loaded_world(), {});  // ragdoll appliqué
    CHECK(d.avatar_dead());
    out = d.tick(1510, loaded_world(), reliable(encode_r(proto::Weapon{proto::kHostNetId, "It_1H_Sword"})));
    CHECK(has_log(out, "avatar mort"));
    CHECK(commands_of<AvatarWeapon>(d.tick(3000, loaded_world(), {})).empty());
}

TEST("arme : Weapon tronqué → message fiable illisible, ignoré") {
    Session s = host_with_avatar();
    Bytes b = encode_r(proto::Weapon{proto::kGuestNetId, "It_1H_Sword"});
    b.pop_back();
    const Output out = s.tick(1200, loaded_world(), reliable(b));
    CHECK(commands_of<AvatarWeapon>(out).empty());
    CHECK(has_log(out, "message fiable illisible : message tronqué"));
}

TEST("arme : Weapon reçu pendant qu'un Death attend l'affichage → ignoré (debug), jamais appliqué") {
    Session s = guest_with_clock();  // affiché = now − 1350 ; Death{1} appliqué à 1500 (story 3.7 : invité)
    s.tick(1400, loaded_world(), reliable(encode_r(proto::Death{proto::kHostNetId})));
    const Output out = s.tick(1410, loaded_world(), reliable(encode_r(proto::Weapon{proto::kHostNetId, "It_1H_Sword"})));
    bool debug = false;
    for (const auto& l : out.logs) {
        if (l.text.find("Weapon reçu") != std::string::npos && l.text.find("mort de l'avatar en attente") != std::string::npos) {
            debug = l.level == Level::Debug;
        }
    }
    CHECK(debug);
    for (std::uint64_t t = 1420; t < 3000; t += 40) CHECK(commands_of<AvatarWeapon>(s.tick(t, loaded_world(), {})).empty());
    CHECK(s.avatar_dead());
}

// --- Équipement de l'invité (story 3.2b) ---------------------------------------------------

namespace {

// Équipement du premier Hello envoyé dans `out`.
bool hello_equipment(const Output& out, std::string& equipment) {
    for (const auto* snd : commands_of<Send>(out)) {
        const auto d = proto::decode_control(snd->bytes.data(), snd->bytes.size());
        if (!d.ok()) continue;
        if (const auto* h = std::get_if<proto::Hello>(&d.message)) {
            equipment = h->equipment;
            return true;
        }
    }
    return false;
}

const std::vector<std::string> kTwoItems{"It_Armor_Don_Fighter", "It_1H_Sword_Rusty"};

Settings equipped_guest() {
    Settings st = guest_settings();
    st.equipment = " It_Armor_Don_Fighter ,It_1H_Sword_Rusty ";
    return st;
}

proto::Hello hello_with(const std::string& equipment) {
    proto::Hello h = make_hello();
    h.equipment = equipment;
    return h;
}

}  // namespace

TEST("équipement invité : Hello porte la forme canonique") {
    Session s(equipped_guest());
    const Output out = s.tick(0, loaded_world(), {});
    std::string eq;
    CHECK(hello_equipment(out, eq));
    CHECK_EQ(eq, std::string("It_Armor_Don_Fighter,It_1H_Sword_Rusty"));
    CHECK(!has_log(out, "invalide"));
}

TEST("équipement invité : Welcome → Teleport, EquipHero, puis Spawn ; à chaque entrée en session") {
    Session s(equipped_guest());
    s.tick(0, loaded_world(), {});
    Output out = s.tick(100, loaded_world(), control_from(kHost, encode(welcome_at(1.0f, 2.0f, 3.0f, 0.0f))));
    auto eq = commands_of<EquipHero>(out);
    CHECK_EQ(eq.size(), 1u);
    if (!eq.empty()) CHECK(eq[0]->names == kTwoItems);
    CHECK(index_of<Teleport>(out) < index_of<EquipHero>(out));
    CHECK(index_of<EquipHero>(out) < index_of_sent<proto::Spawn>(out));
    CHECK(commands_of<EquipAvatar>(out).empty());
    // Fin, puis nouvelle session au monde suivant : équipé de nouveau.
    s.tick(500, loaded_world(), reliable(encode_r(proto::SessionEnd{2})));
    s.tick(1600, loaded_world(), {});
    CHECK(s.state() == State::Inactive);
    s.tick(4000, loaded_world(kPlayer2), {});
    CHECK(s.state() == State::Connecting);
    out = s.tick(4100, loaded_world(kPlayer2), control_from(kHost, encode(welcome_at(1.0f, 2.0f, 3.0f, 0.0f))));
    CHECK(s.state() == State::InSession);
    eq = commands_of<EquipHero>(out);
    CHECK_EQ(eq.size(), 1u);
    if (!eq.empty()) CHECK(eq[0]->names == kTwoItems);
}

TEST("équipement invité : Welcome à pose nulle → EquipHero quand même") {
    Session s(equipped_guest());
    s.tick(0, loaded_world(), {});
    const Output out = s.tick(100, loaded_world(), control_from(kHost, encode(welcome_at(0, 0, 0, 0))));
    CHECK(commands_of<Teleport>(out).empty());
    CHECK_EQ(commands_of<EquipHero>(out).size(), 1u);
}

TEST("équipement invité : vide ou invalide → pas d'EquipHero") {
    for (const char* v : {"", "It-Sword", "A,,B"}) {
        Settings st = guest_settings();
        st.equipment = v;
        Session s(st);
        const Output first = s.tick(0, loaded_world(), {});
        std::string eq = "?";
        CHECK(hello_equipment(first, eq));
        CHECK_EQ(eq, std::string());
        CHECK_EQ(has_log(first, "invalide"), std::string(v).size() > 0);
        const Output out = s.tick(100, loaded_world(), control_from(kHost, encode(welcome_at(1, 2, 3, 0))));
        CHECK(s.state() == State::InSession);
        CHECK(commands_of<EquipHero>(out).empty());
    }
}

TEST("équipement hôte : sa propre guest_equipment est ignorée") {
    Settings st = host_settings();
    st.equipment = "It-Sword";
    Session s(st);
    const Output out = s.tick(0, loaded_world(), {});
    CHECK(!has_log(out, "équipement"));
    const Output acc = s.tick(1000, loaded_world(), control_from(kGuestA, encode(make_hello())));
    CHECK(s.state() == State::InSession);
    const Output sp = s.tick(1100, loaded_world(), reliable(encode_r(spawn_of(proto::kGuestNetId))));
    CHECK(commands_of<EquipAvatar>(sp).empty());
    CHECK(commands_of<EquipHero>(acc).empty());
}

TEST("équipement hôte : SpawnAvatar puis EquipAvatar ; de nouveau après Despawn ; rien après Ended") {
    Session s(host_settings());
    s.tick(0, loaded_world(), {});
    Output out = s.tick(1000, loaded_world(),
                        control_from(kGuestA, encode(hello_with("It_Armor_Don_Fighter,It_1H_Sword_Rusty"))));
    CHECK(s.state() == State::InSession);
    CHECK(commands_of<EquipAvatar>(out).empty());  // pas d'avatar encore
    CHECK(has_log(out, "équipement de l'invité : It_Armor_Don_Fighter,It_1H_Sword_Rusty"));
    out = s.tick(1100, loaded_world(), reliable(encode_r(spawn_of(proto::kGuestNetId))));
    auto ea = commands_of<EquipAvatar>(out);
    CHECK_EQ(ea.size(), 1u);
    if (!ea.empty()) {
        CHECK_EQ(ea[0]->net_id, proto::kGuestNetId);
        CHECK(ea[0]->names == kTwoItems);
    }
    CHECK(index_of<SpawnAvatar>(out) < index_of<EquipAvatar>(out));
    CHECK(has_log(out, "EquipAvatar"));
    // Spawn en double : ni avatar ni équipement.
    out = s.tick(1150, loaded_world(), reliable(encode_r(spawn_of(proto::kGuestNetId))));
    CHECK(commands_of<EquipAvatar>(out).empty());
    // Despawn puis nouveau Spawn : nouvel avatar, équipé de nouveau.
    s.tick(1200, loaded_world(), reliable(encode_r(proto::Despawn{proto::kGuestNetId})));
    out = s.tick(1300, loaded_world(), reliable(encode_r(spawn_of(proto::kGuestNetId))));
    CHECK_EQ(commands_of<EquipAvatar>(out).size(), 1u);
    // Ended, réarmement, invité suivant sans équipement : rien ne survit.
    s.tick(1400, loaded_world(), reliable(encode_r(proto::SessionEnd{1})));
    s.tick(1400 + kLingerMs + 10, loaded_world(), {});
    CHECK(s.state() == State::Hosting);
    s.tick(3000, loaded_world(), control_from(kGuestB, encode(make_hello())));
    CHECK(s.state() == State::InSession);
    out = s.tick(3100, loaded_world(), reliable(encode_r(spawn_of(proto::kGuestNetId))));
    CHECK_EQ(commands_of<SpawnAvatar>(out).size(), 1u);
    CHECK(commands_of<EquipAvatar>(out).empty());
}

TEST("équipement hôte : Hello.equipment invalide → session acceptée, Warn une fois, avatar non équipé") {
    Session s(host_settings());
    s.tick(0, loaded_world(), {});
    Output out = s.tick(1000, loaded_world(), control_from(kGuestA, encode(hello_with("It-Sword"))));
    CHECK(s.state() == State::InSession);
    CHECK(!sent_reject(out, kGuestA, 5));
    unsigned warns = 0;
    for (const auto& l : out.logs) {
        if (l.level == Level::Warn && l.text.find("équipement de l'invité") != std::string::npos) ++warns;
    }
    CHECK_EQ(warns, 1u);
    // Hello retransmis : même Welcome, pas de nouveau Warn.
    out = s.tick(1050, loaded_world(), control_from(kGuestA, encode(hello_with("It-Sword"))));
    CHECK(!has_log(out, "équipement de l'invité"));
    out = s.tick(1100, loaded_world(), reliable(encode_r(spawn_of(proto::kGuestNetId))));
    CHECK_EQ(commands_of<SpawnAvatar>(out).size(), 1u);
    CHECK(commands_of<EquipAvatar>(out).empty());
}

// --- Armure portée (story 3.2c) ------------------------------------------------------------

namespace {

std::vector<Input> local_armor(const std::string& name) { return {LocalArmor{name}}; }

}  // namespace

TEST("armure : LocalArmor en session → Armor au changement seulement ; retirée → nom vide") {
    Session s = host_with_avatar();
    Output out = s.tick(1200, loaded_world(), local_armor("It_Armor_Inquisitor"));
    auto a = sent_of<proto::Armor>(out);
    CHECK(a.size() == 1 && a[0].net_id == proto::kHostNetId && a[0].name == "It_Armor_Inquisitor");
    CHECK(has_log(out, "Armor envoyé : NetId 1 (hôte), « It_Armor_Inquisitor »"));
    CHECK(sent_of<proto::Weapon>(out).empty());  // l'arme garde son propre état
    CHECK(sent_of<proto::Armor>(s.tick(1210, loaded_world(), local_armor("It_Armor_Inquisitor"))).empty());
    a = sent_of<proto::Armor>(s.tick(1220, loaded_world(), local_armor("It_Armor_Don_Fighter")));
    CHECK(a.size() == 1 && a[0].name == "It_Armor_Don_Fighter");
    out = s.tick(1230, loaded_world(), local_armor(""));
    a = sent_of<proto::Armor>(out);
    CHECK(a.size() == 1 && a[0].name.empty());
    CHECK(has_log(out, "Armor envoyé : NetId 1 (hôte), sans armure"));
    // Arme et armure dans le même tick : un message chacune.
    out = s.tick(1240, loaded_world(), {LocalWeapon{"It_1H_Sword"}, LocalArmor{"It_Armor_Inquisitor"}});
    CHECK_EQ(sent_of<proto::Weapon>(out).size(), 1u);
    CHECK_EQ(sent_of<proto::Armor>(out).size(), 1u);
}

TEST("armure : retenue hors session, envoyée à l'entrée en session après le Spawn et le Weapon") {
    Session s(host_settings());
    s.tick(0, loaded_world(), {LocalArmor{"It_Armor_Inquisitor"}, LocalWeapon{""}});  // Inactive → Hosting
    CHECK(sent_of<proto::Armor>(s.tick(100, loaded_world(), {})).empty());
    const Output out = s.tick(1000, loaded_world(), control_from(kGuestA, encode(make_hello())));
    const auto sent = reliables_sent(out);
    CHECK_EQ(sent.size(), 4u);  // Spawn, Weapon, Armor, Heartbeat
    if (sent.size() >= 3) {
        CHECK(std::holds_alternative<proto::Spawn>(sent[0]));
        CHECK(std::holds_alternative<proto::Weapon>(sent[1]));
        const auto* a = std::get_if<proto::Armor>(&sent[2]);
        CHECK(a != nullptr && a->net_id == proto::kHostNetId && a->name == "It_Armor_Inquisitor");
    }
    // Invité : idem, sous NetId 2, sans armure comprise.
    Session g(guest_settings());
    g.tick(0, loaded_world(), local_armor(""));
    const auto ga = sent_of<proto::Armor>(g.tick(100, loaded_world(), control_from(kHost, encode(welcome_at(1, 2, 3, 0)))));
    CHECK(ga.size() == 1 && ga[0].net_id == proto::kGuestNetId && ga[0].name.empty());
}

TEST("armure : jamais lue → pas d'Armor à l'entrée ; nouvelle session → renvoyée") {
    std::uint32_t conv = 0;
    Session s = hosting_with_a(conv);
    s.tick(1100, loaded_world(), local_armor("It_Armor_Inquisitor"));
    s.tick(1300, loaded_world(), reliable(encode_r(proto::SessionEnd{1})));
    s.tick(2400, loaded_world(), {});
    const auto a = sent_of<proto::Armor>(s.tick(2500, loaded_world(), control_from(kGuestA, encode(make_hello()))));
    CHECK(a.size() == 1 && a[0].name == "It_Armor_Inquisitor");
    Session fresh(host_settings());
    fresh.tick(0, loaded_world(), local_weapon("It_1H_Sword"));
    CHECK(sent_of<proto::Armor>(fresh.tick(1000, loaded_world(), control_from(kGuestA, encode(make_hello())))).empty());
}

TEST("armure : Armor du pair → AvatarArmor à l'instant d'affichage, dans l'ordre d'arrivée avec Weapon") {
    Session s = host_with_clock();  // dernier snapshot : 150 ms ; affiché = now − 1350
    Output out = s.tick(1400, loaded_world(), reliable(encode_r(proto::Armor{proto::kGuestNetId, "It_Armor_Inquisitor"})));
    CHECK(commands_of<AvatarArmor>(out).empty());  // affiché 50 < 150
    CHECK(has_log(out, "Armor reçu : avatar 2 (invité), « It_Armor_Inquisitor »"));
    s.tick(1410, loaded_world(), reliable(encode_r(proto::Weapon{proto::kGuestNetId, "It_1H_Sword"})));
    CHECK(commands_of<AvatarArmor>(s.tick(1499, loaded_world(), {})).empty());
    out = s.tick(1500, loaded_world(), {});
    const auto aa = commands_of<AvatarArmor>(out);
    CHECK(aa.size() == 1 && aa[0]->net_id == proto::kGuestNetId && aa[0]->name == "It_Armor_Inquisitor");
    CHECK_EQ(commands_of<AvatarWeapon>(out).size(), 1u);
    CHECK(index_of<AvatarArmor>(out) < index_of<AvatarWeapon>(out));
    // Armure retirée : nom vide transmis tel quel.
    out = s.tick(1510, loaded_world(), reliable(encode_r(proto::Armor{proto::kGuestNetId, ""})));
    const auto removed = commands_of<AvatarArmor>(out);
    CHECK(removed.size() == 1 && removed[0]->name.empty());
    CHECK(has_log(out, "Armor reçu : avatar 2 (invité), sans armure"));
}

TEST("armure : avant tout snapshot (apparition), AvatarArmor part aussitôt, après SpawnAvatar") {
    std::uint32_t conv = 0;
    Session s = hosting_with_a(conv);
    const Output out = s.tick(1100, loaded_world(),
                              reliable2(spawn_of(proto::kGuestNetId), proto::Armor{proto::kGuestNetId, "It_Armor_Inquisitor"}));
    CHECK_EQ(commands_of<AvatarArmor>(out).size(), 1u);
    CHECK(index_of<SpawnAvatar>(out) < index_of<AvatarArmor>(out));
}

TEST("armure : Armor sans avatar, d'un autre NetId, avatar mort ou mort en attente → ignoré (debug)") {
    std::uint32_t conv = 0;
    Session s = hosting_with_a(conv);
    Output out = s.tick(1100, loaded_world(), reliable(encode_r(proto::Armor{proto::kGuestNetId, "It_Armor_Inquisitor"})));
    CHECK(commands_of<AvatarArmor>(out).empty());
    bool debug = false;
    for (const auto& l : out.logs) {
        if (l.text.find("Armor reçu") != std::string::npos && l.text.find("pas d'avatar") != std::string::npos) {
            debug = l.level == Level::Debug;
        }
    }
    CHECK(debug);
    Session c = host_with_clock();
    out = c.tick(1400, loaded_world(), reliable(encode_r(proto::Armor{proto::kHostNetId, "X"})));
    CHECK(has_log(out, "pas l'avatar du pair"));
    c.tick(1410, loaded_world(), reliable(encode_r(proto::Armor{proto::kGuestNetId, "It_Armor_Inquisitor"})));  // en attente
    c.tick(1420, loaded_world(), reliable2(proto::Despawn{proto::kGuestNetId}, spawn_of(proto::kGuestNetId)));
    for (std::uint64_t t = 1430; t < 3000; t += 100) CHECK(commands_of<AvatarArmor>(c.tick(t, loaded_world(), {})).empty());
    Session d = guest_with_clock();  // story 3.7 : avatar mort chez l'invité seulement (Death{1})
    d.tick(1400, loaded_world(), reliable(encode_r(proto::Death{proto::kHostNetId})));
    out = d.tick(1410, loaded_world(), reliable(encode_r(proto::Armor{proto::kHostNetId, "It_Armor_Inquisitor"})));
    CHECK(has_log(out, "mort de l'avatar en attente"));
    d.tick(1500, loaded_world(), {});  // ragdoll appliqué
    CHECK(d.avatar_dead());
    out = d.tick(1510, loaded_world(), reliable(encode_r(proto::Armor{proto::kHostNetId, "It_Armor_Inquisitor"})));
    CHECK(has_log(out, "avatar mort"));
    CHECK(commands_of<AvatarArmor>(d.tick(3000, loaded_world(), {})).empty());
}

TEST("armure : Armor tronqué → message fiable illisible, ignoré") {
    Session s = host_with_avatar();
    Bytes b = encode_r(proto::Armor{proto::kGuestNetId, "It_Armor_Inquisitor"});
    b.pop_back();
    const Output out = s.tick(1200, loaded_world(), reliable(b));
    CHECK(commands_of<AvatarArmor>(out).empty());
    CHECK(has_log(out, "message fiable illisible : message tronqué"));
}

TEST("armure : file pleine de Weapon → l'Armor en attente n'est pas évincé") {
    Session s = host_with_clock();  // affiché = now − 1350 : rien n'est appliqué avant 1500
    s.tick(1400, loaded_world(), reliable(encode_r(proto::Armor{proto::kGuestNetId, "It_Armor_Inquisitor"})));
    for (std::size_t i = 0; i < kMaxPendingCarried + 4; ++i) {
        const Output out = s.tick(1401 + i, loaded_world(),
                                  reliable(encode_r(proto::Weapon{proto::kGuestNetId, "W" + std::to_string(i)})));
        CHECK(commands_of<AvatarArmor>(out).empty());
    }
    const Output out = s.tick(1500, loaded_world(), {});
    const auto aa = commands_of<AvatarArmor>(out);
    CHECK(aa.size() == 1 && aa[0]->name == "It_Armor_Inquisitor");
    const auto aw = commands_of<AvatarWeapon>(out);
    CHECK_EQ(aw.size(), kMaxPendingCarried - 1);
    if (!aw.empty()) CHECK_EQ(aw.back()->name, "W" + std::to_string(kMaxPendingCarried + 3));  // les plus récents gardés
}

// --- Répliques des PNJ (story 3.3) --------------------------------------------------------

namespace {

proto::Guid guid_of(std::uint8_t n) {
    proto::Guid g{};
    for (std::size_t i = 0; i < g.size(); ++i) g[i] = static_cast<std::uint8_t>(n + i);
    return g;
}

// PNJ à `dz` cm devant le héros de loaded_world() (100, -50.5, 2000).
NpcState npc_at(std::uint8_t n, float dz, int hp = 100, bool alive = true) {
    NpcState s;
    s.guid = guid_of(n);
    s.pose = Pose{100.0f, -50.5f, 2000.0f + dz, 0.25f};
    s.hp = hp;
    s.alive = alive;
    return s;
}

World host_world(std::vector<NpcState> npcs, bool ok = true) {
    World w = loaded_world();
    w.hp = 250;
    w.npcs_ok = ok;
    w.npcs = std::move(npcs);
    return w;
}

std::vector<proto::Spawn> npc_spawns_sent(const Output& out) {
    std::vector<proto::Spawn> v;
    for (const auto& sp : sent_of<proto::Spawn>(out)) {
        if (sp.type == proto::kSpawnTypeNpc) v.push_back(sp);
    }
    return v;
}

// Hôte en session avec A (t0 = 1000) ; le tick 1 tombe à 1050.
Session host_in_session() {
    std::uint32_t conv = 0;
    return hosting_with_a(conv);
}

// Invité en session (Welcome à t = 100 : t0 = 100).
Session guest_in_session() {
    Session g(guest_settings());
    g.tick(0, loaded_world(), {});
    g.tick(100, loaded_world(), control_from(kHost, encode(welcome_at(1, 2, 3, 0))));
    CHECK(g.state() == State::InSession);
    return g;
}

proto::Spawn npc_spawn(std::uint32_t net_id, std::uint8_t g = 7, float x = 10.0f) {
    proto::Spawn sp;
    sp.net_id = net_id;
    sp.type = proto::kSpawnTypeNpc;
    sp.x = x;
    sp.y = 20.0f;
    sp.z = 30.0f;
    sp.yaw = 0.5f;
    sp.guid = guid_of(g);
    return sp;
}

Bytes npc_snapshot(std::uint32_t tick, std::vector<proto::SnapshotEntity> entities) {
    proto::Snapshot s;
    s.tick = tick;
    s.entities = std::move(entities);
    Bytes b;
    CHECK(proto::encode_snapshot(s, b));
    return b;
}

proto::SnapshotEntity npc_entity(std::uint32_t net_id, float x, std::uint16_t hp) {
    proto::SnapshotEntity e;
    e.net_id = net_id;
    e.x = x;
    e.hp = hp;
    return e;
}

}  // namespace

TEST("PNJ hôte : wants_npcs seulement en session, au tick qui enverra un snapshot") {
    Session s(host_settings());
    s.tick(0, loaded_world(), {});
    CHECK(!s.wants_npcs(10));  // Hosting
    s.tick(1000, loaded_world(), control_from(kGuestA, encode(make_hello())));
    CHECK(!s.wants_npcs(1020));  // tick 0 : déjà compté à l'entrée en session
    CHECK(s.wants_npcs(1050));
    s.tick(1050, host_world({}), {});
    CHECK(!s.wants_npcs(1070));
    CHECK(s.wants_npcs(1100));
    Session g = guest_in_session();
    CHECK(!g.wants_npcs(200));  // l'invité ne lit pas de PNJ
}

TEST("PNJ hôte : entrée dans le rayon → NetId 1000, Spawn{1, pose, GUID} avant le snapshot, PV") {
    Session s = host_in_session();
    const Output out = s.tick(1050, host_world({npc_at(1, 4000.0f, 150)}), {});
    // Ordre : le Spawn part avant le snapshot.
    std::size_t spawn_at = out.commands.size(), snap_at = out.commands.size();
    for (std::size_t i = 0; i < out.commands.size(); ++i) {
        if (std::holds_alternative<SendReliable>(out.commands[i]) && spawn_at == out.commands.size()) spawn_at = i;
        if (std::holds_alternative<SendSnapshot>(out.commands[i])) snap_at = i;
    }
    CHECK(spawn_at < snap_at && snap_at < out.commands.size());
    const auto spawns = npc_spawns_sent(out);
    CHECK_EQ(spawns.size(), 1u);
    if (!spawns.empty()) {
        CHECK_EQ(spawns[0].net_id, proto::kFirstNpcNetId);
        CHECK(spawns[0].guid == guid_of(1));
        CHECK(spawns[0].z == 6000.0f && spawns[0].yaw == 0.25f);
    }
    CHECK(has_log(out, "PNJ 1000 annoncé (Spawn) : GUID 0102030405060708090a0b0c0d0e0f10, 40.0 m, PV 150"));
    // Le snapshot du tick de l'annonce ne porte que le héros (le Spawn passe par KCP).
    const auto snaps = snapshots_sent(out);
    CHECK(snaps.size() == 1 && snaps[0].entities.size() == 1);
    if (!snaps.empty() && !snaps[0].entities.empty()) {
        CHECK_EQ(snaps[0].entities[0].net_id, proto::kHostNetId);
        CHECK_EQ(snaps[0].entities[0].hp, 250);  // PV du héros (v9)
    }
    CHECK_EQ(s.npc_count(), 1u);
    // Tick suivant : plus de Spawn, l'entité est dans le snapshot, avec ses PV.
    const Output next = s.tick(1100, host_world({npc_at(1, 3900.0f, 140)}), {});
    CHECK(npc_spawns_sent(next).empty());
    const auto sn = snapshots_sent(next);
    CHECK(sn.size() == 1 && sn[0].entities.size() == 2);
    if (sn.size() == 1 && sn[0].entities.size() == 2) {
        CHECK_EQ(sn[0].entities[1].net_id, 1000u);
        CHECK_EQ(sn[0].entities[1].hp, 140);
        CHECK(sn[0].entities[1].z == 5900.0f);
    }
    // Entre deux snapshots, les PNJ ne sont pas relus : rien ne change.
    const Output idle = s.tick(1110, host_world({}, false), {});
    CHECK(reliables_sent(idle).empty());
    CHECK_EQ(s.npc_count(), 1u);
}

TEST("PNJ hôte : sortie du rayon → Despawn, mort → Death (3.4b) ; retour → même NetId, nouveau Spawn") {
    Session s = host_in_session();
    s.tick(1050, host_world({npc_at(1, 1000.0f), npc_at(2, 2000.0f)}), {});
    CHECK_EQ(s.npc_count(), 2u);
    // PNJ 1 à 100,01 m, PNJ 2 mort.
    Output out = s.tick(1100, host_world({npc_at(1, 10001.0f), npc_at(2, 2000.0f, 0, false)}), {});
    const auto despawns = sent_of<proto::Despawn>(out);
    CHECK(despawns.size() == 1 && despawns[0].net_id == 1000u);
    CHECK(has_log(out, "PNJ 1000 retiré (Despawn) : hors du rayon"));
    const auto deaths = sent_of<proto::Death>(out);
    CHECK(deaths.size() == 1 && deaths[0].net_id == 1001u);
    CHECK(has_log(out, "PNJ 1001 mort (Death)"));
    const auto sn = snapshots_sent(out);
    CHECK(sn.size() == 1 && sn[0].entities.size() == 1);  // héros seul
    CHECK_EQ(s.npc_count(), 0u);
    // Exactement 100 m : dans le rayon. Même GUID → même NetId.
    out = s.tick(1150, host_world({npc_at(1, 10000.0f)}), {});
    auto spawns = npc_spawns_sent(out);
    CHECK(spawns.size() == 1 && spawns[0].net_id == 1000u);
    // Absent de la liste : retiré seulement après kNpcAbsentGraceTicks ticks ; un nouveau GUID
    // reçoit 1002 (jamais de réutilisation).
    out = s.tick(1200, host_world({npc_at(3, 100.0f)}), {});
    CHECK(sent_of<proto::Despawn>(out).empty());
    spawns = npc_spawns_sent(out);
    CHECK(spawns.size() == 1 && spawns[0].net_id == 1002u);
    std::uint64_t t = 1200;
    for (std::uint32_t i = 1; i + 1 < kNpcAbsentGraceTicks; ++i) {
        t += 50;
        out = s.tick(t, host_world({npc_at(3, 100.0f)}), {});
        CHECK(sent_of<proto::Despawn>(out).empty());
    }
    t += 50;
    out = s.tick(t, host_world({npc_at(3, 100.0f)}), {});
    CHECK(has_log(out, "PNJ 1000 retiré (Despawn) : absent de la liste depuis 2 s"));
    CHECK_EQ(sent_of<proto::Despawn>(out).size(), 1u);
}

TEST("PNJ hôte : absent de la liste puis revenu pendant le délai de grâce → ni Despawn ni Spawn") {
    Session s = host_in_session();
    s.tick(1050, host_world({npc_at(1, 1000.0f)}), {});
    CHECK_EQ(s.npc_count(), 1u);
    std::uint64_t t = 1050;
    for (std::uint32_t i = 0; i + 1 < kNpcAbsentGraceTicks; ++i) {
        t += 50;
        const Output out = s.tick(t, host_world({}), {});
        CHECK(sent_of<proto::Despawn>(out).empty());
        const auto sn = snapshots_sent(out);
        CHECK(sn.size() == 1 && sn[0].entities.size() == 1);  // absent : hors du snapshot
    }
    t += 50;
    Output out = s.tick(t, host_world({npc_at(1, 1000.0f)}), {});
    CHECK(sent_of<proto::Despawn>(out).empty());
    CHECK(npc_spawns_sent(out).empty());
    const auto sn = snapshots_sent(out);
    CHECK(sn.size() == 1 && sn[0].entities.size() == 2 && sn[0].entities[1].net_id == 1000u);
    // Le compteur repart de zéro : une nouvelle absence a droit au délai entier.
    for (std::uint32_t i = 0; i + 1 < kNpcAbsentGraceTicks; ++i) {
        t += 50;
        out = s.tick(t, host_world({}), {});
        CHECK(sent_of<proto::Despawn>(out).empty());
    }
    CHECK_EQ(s.npc_count(), 1u);
}

TEST("PNJ hôte : plus de 200 PNJ → les 200 plus proches, en 4 datagrammes de même tick ; les autres attendent") {
    Session s = host_in_session();
    std::vector<NpcState> npcs;  // PNJ 1 (le plus loin, 90 m) … PNJ 210 (6,4 m)
    for (int i = 0; i < 210; ++i) npcs.push_back(npc_at(static_cast<std::uint8_t>(1 + i), 9000.0f - 40.0f * i));
    Output out = s.tick(1050, host_world(npcs), {});
    CHECK_EQ(npc_spawns_sent(out).size(), kMaxSnapshotNpcs);
    bool far_spawned = false;
    for (const auto& sp : npc_spawns_sent(out)) far_spawned |= sp.guid == guid_of(1);  // le plus loin
    CHECK(!far_spawned);
    out = s.tick(1100, host_world(npcs), {});
    const auto sn = snapshots_sent(out);
    CHECK_EQ(sn.size(), kMaxSnapshotParts);
    std::size_t entities = 0;
    bool same_tick = true;
    for (const auto& part : sn) {
        CHECK(part.entities.size() <= proto::kMaxSnapshotEntities);
        entities += part.entities.size();
        same_tick = same_tick && part.tick == sn[0].tick;
    }
    CHECK(same_tick);
    CHECK_EQ(entities, kMaxSnapshotNpcs + 1);
    // Le héros en tête du premier ; puis les plus proches : le PNJ 210 (à 6,4 m).
    if (!sn.empty() && sn[0].entities.size() > 1) {
        CHECK_EQ(sn[0].entities[0].net_id, proto::kHostNetId);
        CHECK(sn[0].entities[1].z == 2000.0f + 9000.0f - 40.0f * 209);
    }
    CHECK(npc_spawns_sent(out).empty());
    // Un des retenus meurt (Death, story 3.4b) : le suivant (PNJ 10) entre.
    npcs[209].alive = false;
    out = s.tick(1150, host_world(npcs), {});
    CHECK_EQ(sent_of<proto::Death>(out).size(), 1u);
    CHECK(sent_of<proto::Despawn>(out).empty());
    const auto spawns = npc_spawns_sent(out);
    CHECK(spawns.size() == 1 && spawns[0].guid == guid_of(10));
    // Un PNJ plus proche que tous évince le plus lointain des retenus.
    npcs.push_back(npc_at(250, 10.0f));
    out = s.tick(1200, host_world(npcs), {});
    CHECK(has_log(out, "évincé par plus proche"));
    CHECK_EQ(sent_of<proto::Despawn>(out).size(), 1u);
    CHECK_EQ(s.npc_count(), kMaxSnapshotNpcs);
}

TEST("PNJ hôte : rayon autour de l'avatar de l'invité aussi") {
    Session s = host_with_avatar();  // t0 = 1000, avatar créé à 1100
    // L'avatar est affiché à x = 100, z = 20000 (180 m du héros).
    proto::SnapshotEntity e;
    e.net_id = proto::kGuestNetId;
    e.x = 100.0f;
    e.y = -50.5f;
    e.z = 20000.0f;
    proto::Snapshot sn;
    sn.tick = 2;
    sn.entities = {e};
    Bytes b;
    CHECK(proto::encode_snapshot(sn, b));
    s.tick(1120, loaded_world(), snapshot_in(b));
    s.tick(1130, loaded_world(), {});  // MoveAvatar : pose de l'avatar connue
    NpcState near_avatar = npc_at(1, 18000.0f);  // z = 20000 : sur l'avatar, à 180 m du héros
    NpcState far_both = npc_at(2, -11000.0f);                        // à 110 m du héros, 290 m de l'avatar
    const Output out = s.tick(1150, host_world({near_avatar, far_both}), {});
    const auto spawns = npc_spawns_sent(out);
    CHECK(spawns.size() == 1 && spawns[0].guid == guid_of(1));
    // L'avatar est retiré : son ancienne pose ne compte plus, le PNJ (à 180 m du héros) sort.
    s.tick(1160, loaded_world(), reliable(encode_r(proto::Despawn{proto::kGuestNetId})));
    CHECK(!s.avatar_present());
    const Output gone = s.tick(1200, host_world({near_avatar, far_both}), {});
    const auto despawns = sent_of<proto::Despawn>(gone);
    CHECK(despawns.size() == 1 && despawns[0].net_id == 1000u);
    CHECK(has_log(gone, "PNJ 1000 retiré (Despawn) : hors du rayon"));
    CHECK(npc_spawns_sent(gone).empty());
    CHECK_EQ(s.npc_count(), 0u);
}

TEST("PNJ hôte : liste illisible → rien ne change, snapshot avec le héros seul") {
    Session s = host_in_session();
    s.tick(1050, host_world({npc_at(1, 100.0f)}), {});
    const Output out = s.tick(1100, host_world({}, false), {});
    CHECK(reliables_sent(out).empty());  // ni Despawn ni Spawn
    const auto sn = snapshots_sent(out);
    CHECK(sn.size() == 1 && sn[0].entities.size() == 1);
    CHECK_EQ(s.npc_count(), 1u);
}

TEST("PNJ hôte : PV bornés à [0, 65535], pose non finie ignorée, GUID en double compté une fois") {
    Session s = host_in_session();
    NpcState nan_pose = npc_at(3, 100.0f);
    nan_pose.pose.x = std::nanf("");
    const World w = host_world({npc_at(1, 100.0f, -5), npc_at(2, 200.0f, 70000), nan_pose, npc_at(1, 300.0f)});
    CHECK_EQ(npc_spawns_sent(s.tick(1050, w, {})).size(), 2u);
    const auto sn = snapshots_sent(s.tick(1100, w, {}));
    CHECK(sn.size() == 1 && sn[0].entities.size() == 3);
    if (sn.size() == 1 && sn[0].entities.size() == 3) {
        CHECK_EQ(sn[0].entities[1].hp, 0);
        CHECK_EQ(sn[0].entities[2].hp, 65535);
    }
}

TEST("PNJ hôte : NetId repartent de 1000 à chaque session ; Ended n'envoie rien aux PNJ") {
    Session s = host_in_session();
    s.tick(1050, host_world({npc_at(1, 100.0f), npc_at(2, 200.0f)}), {});
    // L'invité part : fin de session, réarmement immédiat de l'hôte.
    Output out = s.tick(1100, host_world({}), reliable(encode_r(proto::SessionEnd{1})));
    CHECK(sent_of<proto::Despawn>(out).empty());
    CHECK(commands_of<ReleaseNpc>(out).empty());  // l'hôte n'a pas de répliques
    CHECK_EQ(s.npc_count(), 0u);
    s.tick(2200, loaded_world(), {});  // fin du maintien → Hosting
    CHECK(s.state() == State::Hosting);
    s.tick(3000, loaded_world(), control_from(kGuestB, encode(make_hello())));
    CHECK(s.state() == State::InSession);
    out = s.tick(3050, host_world({npc_at(2, 200.0f)}), {});
    const auto spawns = npc_spawns_sent(out);
    CHECK(spawns.size() == 1 && spawns[0].net_id == 1000u);
}

TEST("PNJ invité : Spawn{1} → SpawnNpc (NetId, GUID, pose) ; en double, NetId < 1000 → ignorés") {
    Session g = guest_in_session();
    Output out = g.tick(200, loaded_world(), reliable(encode_r(npc_spawn(1000))));
    const auto sp = commands_of<SpawnNpc>(out);
    CHECK_EQ(sp.size(), 1u);
    if (!sp.empty()) {
        CHECK_EQ(sp[0]->net_id, 1000u);
        CHECK(sp[0]->guid == guid_of(7));
        CHECK(sp[0]->pose.x == 10.0f && sp[0]->pose.yaw == 0.5f);
    }
    CHECK(has_log(out, "PNJ 1000 : Spawn reçu (GUID 0708090a0b0c0d0e0f10111213141516"));
    CHECK_EQ(g.npc_count(), 1u);
    out = g.tick(210, loaded_world(), reliable(encode_r(npc_spawn(1000, 9))));
    CHECK(commands_of<SpawnNpc>(out).empty());
    CHECK(has_log(out, "ignoré (NetId déjà annoncé)"));
    out = g.tick(220, loaded_world(), reliable(encode_r(npc_spawn(999))));
    CHECK(commands_of<SpawnNpc>(out).empty());
    CHECK(has_log(out, "ignoré (NetId < 1000)"));
    out = g.tick(230, loaded_world(), reliable(encode_r(npc_spawn(1001, 7, std::nanf("")))));
    CHECK(commands_of<SpawnNpc>(out).empty());
    CHECK(has_log(out, "ignoré (pose non finie)"));
    // Tronqué → illisible.
    Bytes cut = encode_r(npc_spawn(1002));
    cut.pop_back();
    out = g.tick(240, loaded_world(), reliable(cut));
    CHECK(commands_of<SpawnNpc>(out).empty());
    CHECK(has_log(out, "message fiable illisible : message tronqué"));
    CHECK_EQ(g.npc_count(), 1u);
    CHECK(!g.avatar_present());  // un Spawn{1} ne crée jamais d'avatar
}

TEST("PNJ invité : snapshots → MoveNpc interpolé (150 ms, horloge commune), SetNpcHp au changement") {
    Session g = guest_in_session();  // t0 = 100
    g.tick(150, loaded_world(), reliable(encode_r(npc_spawn(1000))));
    // Avant tout snapshot : pas de MoveNpc (la réplique reste à la pose du Spawn).
    CHECK(commands_of<MoveNpc>(g.tick(160, loaded_world(), {})).empty());
    Output out;
    std::size_t hp_count = 0;
    std::vector<int> hps;
    for (std::uint32_t tick = 0; tick <= 10; ++tick) {
        const std::uint16_t hp = tick < 5 ? 150 : 140;
        out = g.tick(200 + tick * 50, loaded_world(),
                     snapshot_in(npc_snapshot(tick, {npc_entity(1000, tick * 15.0f, hp)})));
        for (const auto* c : commands_of<SetNpcHp>(out)) {
            ++hp_count;
            hps.push_back(c->hp);
        }
    }
    // Horloge : décalage = 200 ; à 700, distant = 500, affiché = 350 → x = 105.
    const auto moves = commands_of<MoveNpc>(out);
    CHECK_EQ(moves.size(), 1u);
    if (!moves.empty()) {
        CHECK_EQ(moves[0]->net_id, 1000u);
        CHECK(std::fabs(moves[0]->pose.x - 105.0f) < 0.05f);
    }
    // 150 écrit une fois ; 140 (tick 5 = 250 ms distants) affiché à 600 → écrit une fois.
    CHECK_EQ(hp_count, 2u);
    CHECK(hps.size() == 2 && hps[0] == 150 && hps[1] == 140);
}

TEST("PNJ invité : entité d'un NetId non annoncé → ignorée, comptée ; Despawn → ReleaseNpc") {
    Session g = guest_in_session();
    g.tick(150, loaded_world(), reliable(encode_r(npc_spawn(1000))));
    Output out = g.tick(200, loaded_world(), snapshot_in(npc_snapshot(2, {npc_entity(1005, 1.0f, 10)})));
    CHECK(commands_of<MoveNpc>(out).empty());
    World blocked = loaded_world();
    blocked.routine_blocked = 7;  // OnProcess de routine bloqués (IA figée), cumulés au bilan
    g.tick(250, blocked, {});
    g.tick(260, blocked, {});
    for (std::uint64_t t = 300; t < 5100; t += 500) g.tick(t, loaded_world(), {});
    out = g.tick(5100, loaded_world(), {});
    CHECK(has_log(out, "entités ignorées 1"));
    CHECK(has_log(out, "entités inconnues 1"));
    CHECK(has_log(out, "OnProcess de routine bloqués 14"));
    out = g.tick(5200, loaded_world(), reliable(encode_r(proto::Despawn{1000})));
    const auto rel = commands_of<ReleaseNpc>(out);
    CHECK(rel.size() == 1 && rel[0]->net_id == 1000u);
    CHECK(has_log(out, "PNJ 1000 : Despawn reçu : réplique libérée"));
    CHECK_EQ(g.npc_count(), 0u);
    CHECK(g.state() == State::InSession);
    out = g.tick(5300, loaded_world(), reliable(encode_r(proto::Despawn{1000})));
    CHECK(commands_of<ReleaseNpc>(out).empty());
    CHECK(has_log(out, "ignoré (NetId non annoncé)"));
    // Retour dans le rayon : même NetId, nouveau Spawn accepté.
    out = g.tick(5400, loaded_world(), reliable(encode_r(npc_spawn(1000))));
    CHECK_EQ(commands_of<SpawnNpc>(out).size(), 1u);
}

TEST("PNJ invité : fin de session ou fin de monde → toutes les répliques libérées") {
    {
        Session g = guest_in_session();
        g.tick(150, loaded_world(), reliable(encode_r(npc_spawn(1000))));
        g.tick(160, loaded_world(), reliable(encode_r(npc_spawn(1001, 8))));
        const Output out = g.tick(200, loaded_world(), reliable(encode_r(proto::SessionEnd{2})));
        const auto rel = commands_of<ReleaseNpc>(out);
        CHECK(rel.size() == 2 && rel[0]->net_id == 1000u && rel[1]->net_id == 1001u);
        CHECK(has_log(out, "2 réplique(s) libérée(s)"));
        CHECK_EQ(g.npc_count(), 0u);
    }
    {
        Session g = guest_in_session();
        g.tick(150, loaded_world(), reliable(encode_r(npc_spawn(1000))));
        const Output out = g.tick(200, World{}, {});  // fin de monde
        CHECK(g.state() == State::Ended);
        CHECK_EQ(commands_of<ReleaseNpc>(out).size(), 1u);
        CHECK(commands_of<MoveNpc>(g.tick(300, World{}, {})).empty());
    }
}

TEST("PNJ hôte : Spawn{1} ou Despawn de PNJ venus de l'invité → ignorés") {
    Session s = host_in_session();
    Output out = s.tick(1100, loaded_world(), reliable(encode_r(npc_spawn(1000))));
    CHECK(commands_of<SpawnNpc>(out).empty());
    CHECK(has_log(out, "l'hôte fait autorité sur les PNJ"));
    out = s.tick(1110, loaded_world(), reliable(encode_r(proto::Despawn{1000})));
    CHECK(commands_of<ReleaseNpc>(out).empty());
    CHECK(has_log(out, "PNJ 1000 : Despawn reçu : ignoré (l'hôte fait autorité sur les PNJ)"));
}

// --- Animations des PNJ (story 3.4) ------------------------------------------------------

namespace {

// PNJ n (1..n) à n × 100 cm du héros : le plus proche est le PNJ 1.
std::vector<NpcState> npcs_in_line(std::uint8_t count, std::uint8_t first = 1) {
    std::vector<NpcState> v;
    for (std::uint8_t n = first; n < first + count; ++n) v.push_back(npc_at(n, 100.0f * n));
    return v;
}

std::vector<std::uint32_t> captured_ids(const Output& out) {
    std::vector<std::uint32_t> v;
    for (const auto* c : commands_of<CaptureNpc>(out)) v.push_back(c->net_id);
    return v;
}

std::vector<std::uint32_t> uncaptured_ids(const Output& out) {
    std::vector<std::uint32_t> v;
    for (const auto* c : commands_of<UncaptureNpc>(out)) v.push_back(c->net_id);
    return v;
}

// Hôte en session (t0 = 1000) dont les PNJ 1 à `count` sont annoncés (tick 1050) et animés.
Session host_with_animated(std::uint8_t count) {
    Session s = host_in_session();
    s.tick(1050, host_world(npcs_in_line(count)), {});
    return s;
}

Input npc_motion_in(std::uint32_t net_id, const std::string& name, std::uint8_t type = 0) {
    return LocalMotion{name, local_motion_of(type), net_id};
}

Input npc_param_in(std::uint32_t net_id, std::uint8_t type, proto::MotionParamKind kind, float value, float time = 0.0f) {
    return LocalParam{type, static_cast<std::uint8_t>(kind), value, time, net_id};
}

// Invité en session (t0 = 100), PNJ 1000 et 1001 annoncés, AniName 3 = « Wolf_Run », 4 =
// « Wolf_Attack », horloge calée : snapshots du tick t reçus à 200 + 50 t (décalage 200). Temps
// affiché à `now` = now − 200 − 150.
Session guest_with_npcs() {
    Session g = guest_in_session();
    g.tick(150, loaded_world(), reliable2(npc_spawn(1000), npc_spawn(1001, 8)));
    g.tick(160, loaded_world(), reliable2(proto::AniName{3, "Wolf_Run"}, proto::AniName{4, "Wolf_Attack"}));
    for (std::uint32_t tick = 0; tick < 4; ++tick) {
        g.tick(200 + tick * 50, loaded_world(),
               snapshot_in(npc_snapshot(tick, {npc_entity(1000, 0.0f, 100), npc_entity(1001, 0.0f, 100)})));
    }
    return g;
}

proto::Motion host_npc_motion(std::uint32_t net_id, std::uint16_t ani_id, std::uint32_t ms, std::uint8_t type = 0) {
    proto::Motion m = local_motion_of(type);
    m.net_id = net_id;
    m.ani_id = ani_id;
    m.ms = ms;
    return m;
}

proto::MotionParam host_npc_param(std::uint32_t net_id, std::uint32_t ms, std::uint8_t type, float value) {
    proto::MotionParam p;
    p.net_id = net_id;
    p.ms = ms;
    p.type = type;
    p.kind = static_cast<std::uint8_t>(proto::MotionParamKind::TargetWeight);
    p.value = value;
    p.time = 0.25f;
    return p;
}

}  // namespace

TEST("animations PNJ hôte : les 24 plus proches sont capturés (CaptureNpc avec GUID), pas le 25e") {
    Session s = host_in_session();
    const Output out = s.tick(1050, host_world(npcs_in_line(30)), {});
    const auto caps = commands_of<CaptureNpc>(out);
    CHECK_EQ(caps.size(), kMaxAnimatedNpcs);
    // NetId attribués dans l'ordre des distances : 1000 (PNJ 1) … 1023 (PNJ 24).
    for (std::size_t i = 0; i < caps.size(); ++i) {
        CHECK_EQ(caps[i]->net_id, 1000u + static_cast<std::uint32_t>(i));
        CHECK(caps[i]->guid == guid_of(static_cast<std::uint8_t>(i + 1)));
    }
    CHECK_EQ(s.animated_npc_count(), kMaxAnimatedNpcs);
    CHECK_EQ(s.npc_count(), 30u);
    // Rien ne change : pas de nouvelle commande.
    const Output same = s.tick(1100, host_world(npcs_in_line(30)), {});
    CHECK(commands_of<CaptureNpc>(same).empty() && commands_of<UncaptureNpc>(same).empty());
}

TEST("animations PNJ hôte : entrée et sortie du groupe des 24 suivent la sélection, Despawn et Ended") {
    constexpr auto k = static_cast<std::uint32_t>(kMaxAnimatedNpcs);
    Session s = host_with_animated(k + 1);  // 1000…1023 animés, 1024 (PNJ 25) annoncé seulement
    // Le PNJ 25 passe devant tous les autres : il entre, le PNJ 24 (1023) sort.
    std::vector<NpcState> npcs = npcs_in_line(static_cast<std::uint8_t>(k));
    npcs.push_back(npc_at(static_cast<std::uint8_t>(k + 1), 10.0f));
    Output out = s.tick(1100, host_world(npcs), {});
    CHECK(captured_ids(out) == std::vector<std::uint32_t>{1000u + k});
    CHECK(uncaptured_ids(out) == std::vector<std::uint32_t>{1000u + k - 1});
    // Le PNJ 1 (1000) meurt : Death (story 3.4b), il sort du groupe ; le PNJ 24 (1023) y revient.
    npcs[0].alive = false;
    out = s.tick(1150, host_world(npcs), {});
    CHECK(uncaptured_ids(out) == std::vector<std::uint32_t>{1000u});
    CHECK(captured_ids(out) == std::vector<std::uint32_t>{1000u + k - 1});
    CHECK_EQ(sent_of<proto::Death>(out).size(), 1u);
    // Fin de session : tous retirés, sans rien envoyer aux PNJ.
    out = s.tick(1200, host_world(npcs), reliable(encode_r(proto::SessionEnd{1})));
    CHECK_EQ(commands_of<UncaptureNpc>(out).size(), kMaxAnimatedNpcs);
    CHECK_EQ(s.animated_npc_count(), 0u);
}

TEST("animations PNJ hôte : Motion d'un PNJ animé → AniName puis Motion sous son NetId, AniId partagés") {
    Session s = host_with_animated(2);
    Output out = s.tick(1060, host_world({}), {LocalMotion{"Hero_Run", local_motion_of()}, npc_motion_in(1001, "Wolf_Run"),
                                              npc_motion_in(1000, "Hero_Run")});
    const auto sent = reliables_sent(out);
    CHECK_EQ(sent.size(), 5u);  // AniName 0, Motion héros, AniName 1, Motion 1001, Motion 1000
    if (sent.size() == 5) {
        const auto* an1 = std::get_if<proto::AniName>(&sent[2]);
        const auto* m1 = std::get_if<proto::Motion>(&sent[3]);
        const auto* m0 = std::get_if<proto::Motion>(&sent[4]);
        CHECK(an1 != nullptr && an1->ani_id == 1 && an1->name == "Wolf_Run");
        CHECK(m1 != nullptr && m1->net_id == 1001u && m1->ani_id == 1 && m1->ms == 60);
        CHECK(m0 != nullptr && m0->net_id == 1000u && m0->ani_id == 0);  // nom déjà interné par le héros
    }
    const auto hero = sent_of<proto::Motion>(out);
    CHECK(!hero.empty() && hero[0].net_id == proto::kHostNetId);
}

TEST("animations PNJ hôte : PNJ hors des 24 ou inconnu → rien n'est envoyé, compté au bilan") {
    Session s = host_with_animated(25);  // 1024 annoncé, pas animé
    Output out = s.tick(1060, host_world({}), {npc_motion_in(1024, "Wolf_Run"), npc_motion_in(4242, "Wolf_Run"),
                                              npc_param_in(1024, 0, proto::MotionParamKind::PlaySpeed, 1.0f)});
    CHECK(reliables_sent(out).empty());
    for (std::uint64_t t = 1100; t < 6000; t += 500) s.tick(t, host_world({}, false), reliable(encode_r(proto::Heartbeat{})));
    out = s.tick(6000, host_world({}, false), {});
    CHECK(has_log(out, "animations des PNJ : animés 24, Motion envoyés 0, rejoués 0, écrasés 0, ignorés 2"));
    CHECK(has_log(out, "MotionParam envoyés 0, regroupés 0, rejoués 0, ignorés 1, 0 o/s"));
}

TEST("animations PNJ hôte : réglages regroupés (un par frame et par PNJ, type, nature), filtrés par PNJ") {
    Session s = host_with_animated(2);
    std::vector<Input> in;
    for (int i = 0; i < 5; ++i) in.push_back(npc_param_in(1000, 0, proto::MotionParamKind::TargetWeight, 0.1f * i, 0.25f));
    in.push_back(npc_param_in(1001, 0, proto::MotionParamKind::TargetWeight, 0.4f, 0.25f));
    in.push_back(npc_param_in(1000, 0, proto::MotionParamKind::PlaySpeed, 1.2f));
    Output out = s.tick(1060, host_world({}), in);
    auto p = sent_of<proto::MotionParam>(out);
    CHECK_EQ(p.size(), 3u);
    if (p.size() == 3) {
        // Ordre de la dernière mise à jour : 1000 poids (0,4), 1001 poids, 1000 vitesse.
        CHECK(p[0].net_id == 1000u && p[0].kind == 0 && std::fabs(p[0].value - 0.4f) < 1e-6f && p[0].ms == 60);
        CHECK(p[1].net_id == 1001u && p[1].kind == 0);
        CHECK(p[2].net_id == 1000u && p[2].kind == 1 && p[2].value == 1.2f);
    }
    // Frame suivante : identique au dernier envoyé pour ce PNJ → filtré ; l'autre PNJ a son filtre.
    out = s.tick(1070, host_world({}), {npc_param_in(1000, 0, proto::MotionParamKind::TargetWeight, 0.405f, 0.25f),
                                        npc_param_in(1001, 0, proto::MotionParamKind::TargetWeight, 0.8f, 0.25f)});
    p = sent_of<proto::MotionParam>(out);
    CHECK(p.size() == 1 && p[0].net_id == 1001u);
    // Valeur finale exacte : toujours envoyée.
    out = s.tick(1080, host_world({}), {npc_param_in(1001, 0, proto::MotionParamKind::TargetWeight, 0.0f, 0.25f)});
    CHECK_EQ(sent_of<proto::MotionParam>(out).size(), 1u);
    // Bilan : 5 réglages envoyés, 4 remplacés dans la frame.
    for (std::uint64_t t = 1100; t < 6000; t += 500) s.tick(t, host_world({}, false), reliable(encode_r(proto::Heartbeat{})));
    out = s.tick(6000, host_world({}, false), {});
    CHECK(has_log(out, "MotionParam envoyés 5, regroupés 4, rejoués 0, ignorés 0, 19 o/s"));
}

TEST("animations PNJ hôte : un Motion fait partir avant lui les réglages de son type, puis invalide le filtre") {
    Session s = host_with_animated(1);
    s.tick(1060, host_world({}), {npc_param_in(1000, 0, proto::MotionParamKind::TargetWeight, 1.0f, 0.25f)});
    Output out = s.tick(1070, host_world({}), {npc_param_in(1000, 0, proto::MotionParamKind::PlaySpeed, 1.5f),
                                               npc_param_in(1000, 2, proto::MotionParamKind::PlaySpeed, 0.5f),
                                               npc_motion_in(1000, "Wolf_Run", 0),
                                               npc_param_in(1000, 0, proto::MotionParamKind::TargetWeight, 1.0f, 0.25f)});
    const auto sent = reliables_sent(out);
    // Vitesse du type 0, AniName, Motion, puis en fin de frame, dans l'ordre de mise à jour : vitesse
    // du type 2 (en attente avant le Motion), poids du type 0 (identique, mais le filtre repart).
    CHECK_EQ(sent.size(), 5u);
    if (sent.size() == 5) {
        const auto* a = std::get_if<proto::MotionParam>(&sent[0]);
        CHECK(a != nullptr && a->type == 0 && a->kind == 1);
        CHECK(std::holds_alternative<proto::AniName>(sent[1]));
        CHECK(std::holds_alternative<proto::Motion>(sent[2]));
        const auto* c = std::get_if<proto::MotionParam>(&sent[3]);
        const auto* b = std::get_if<proto::MotionParam>(&sent[4]);
        CHECK(c != nullptr && c->type == 2 && c->kind == 1);
        CHECK(b != nullptr && b->type == 0 && b->kind == 0 && b->value == 1.0f);
    }
}

TEST("animations PNJ hôte : PNJ sorti du groupe → ses réglages en attente ne partent pas ; revenu, filtre à neuf") {
    Session s = host_with_animated(25);
    s.tick(1060, host_world({}), {npc_param_in(1023, 0, proto::MotionParamKind::TargetWeight, 1.0f, 0.25f)});
    // 1023 sort du groupe au tick 1100 (PNJ 25 devant) : un réglage capturé avant la sélection part encore.
    std::vector<NpcState> npcs = npcs_in_line(24);
    npcs.push_back(npc_at(25, 10.0f));
    s.tick(1100, host_world(npcs), {});
    Output out = s.tick(1110, host_world({}), {npc_param_in(1023, 0, proto::MotionParamKind::TargetWeight, 0.5f, 0.25f)});
    CHECK(sent_of<proto::MotionParam>(out).empty());
    // Il revient : le même réglage qu'avant repart (filtre à neuf).
    s.tick(1150, host_world(npcs_in_line(24)), {});
    out = s.tick(1160, host_world({}), {npc_param_in(1023, 0, proto::MotionParamKind::TargetWeight, 1.0f, 0.25f)});
    CHECK_EQ(sent_of<proto::MotionParam>(out).size(), 1u);
}

TEST("animations PNJ hôte : budget de 32 Ko/s dépassé → une ligne par bilan, octets par seconde au bilan") {
    Session s = host_with_animated(1);
    std::vector<Input> in;
    for (int i = 0; i < 4100; ++i) in.push_back(npc_motion_in(1000, "Wolf_Run"));
    s.tick(1060, host_world({}), in);
    for (std::uint64_t t = 1100; t < 6000; t += 500) s.tick(t, host_world({}, false), reliable(encode_r(proto::Heartbeat{})));
    Output out = s.tick(6000, host_world({}, false), {});
    // (12 + 4100 × 41) octets en 5 s = 33622 o/s.
    CHECK(has_log(out, "Motion envoyés 4100"));
    CHECK(has_log(out, "33622 o/s"));
    std::size_t warns = 0;
    for (const auto& l : out.logs) {
        if (l.level == Level::Warn && l.text.find("au-delà du budget de 32768 o/s") != std::string::npos) ++warns;
    }
    CHECK_EQ(warns, 1u);
    // Bilan suivant, sous le budget : pas de ligne.
    s.tick(6100, host_world({}), {npc_motion_in(1000, "Wolf_Run")});
    for (std::uint64_t t = 6500; t < 11000; t += 500) s.tick(t, host_world({}, false), reliable(encode_r(proto::Heartbeat{})));
    out = s.tick(11000, host_world({}, false), {});
    CHECK(!has_log(out, "au-delà du budget"));
}

TEST("animations PNJ invité : Motion et MotionParam d'un PNJ annoncé → rejoués 150 ms plus tard sur la réplique") {
    Session g = guest_with_npcs();
    // Motion daté de 300 ms distants : affiché quand now − 350 ≥ 300.
    Output out = g.tick(400, loaded_world(),
                        reliable2(host_npc_motion(1000, 3, 300), host_npc_param(1000, 300, 0, 1.0f)));
    CHECK(commands_of<PlayNpcMotion>(out).empty());
    out = g.tick(640, loaded_world(), {});
    CHECK(commands_of<PlayNpcMotion>(out).empty());
    out = g.tick(650, loaded_world(), {});
    const auto pm = commands_of<PlayNpcMotion>(out);
    const auto pp = commands_of<PlayNpcParam>(out);
    CHECK(pm.size() == 1 && pm[0]->net_id == 1000u && pm[0]->name == "Wolf_Run" && pm[0]->motion.type == 0);
    CHECK(pp.size() == 1 && pp[0]->net_id == 1000u && pp[0]->param.value == 1.0f);
    CHECK(commands_of<PlayAvatarMotion>(out).empty());  // l'avatar n'est pas concerné
}

TEST("animations PNJ invité : par réplique, un seul Motion des types 0/1 par frame, réglage antérieur écarté") {
    Session g = guest_with_npcs();
    std::vector<Input> in;
    in.push_back(ReliableIn{encode_r(host_npc_param(1000, 100, 0, 0.5f))});
    in.push_back(ReliableIn{encode_r(host_npc_motion(1000, 3, 110))});
    in.push_back(ReliableIn{encode_r(host_npc_motion(1001, 3, 115))});
    in.push_back(ReliableIn{encode_r(host_npc_motion(1000, 4, 120))});
    in.push_back(ReliableIn{encode_r(host_npc_motion(1000, 4, 125, 2))});  // type 2 : toujours rejoué
    Output out = g.tick(600, loaded_world(), in);  // affiché 250 : tout est échu
    const auto pm = commands_of<PlayNpcMotion>(out);
    CHECK_EQ(pm.size(), 3u);
    if (pm.size() == 3) {
        CHECK(pm[0]->net_id == 1000u && pm[0]->name == "Wolf_Attack" && pm[0]->motion.type == 0);
        CHECK(pm[1]->net_id == 1000u && pm[1]->motion.type == 2);
        CHECK(pm[2]->net_id == 1001u && pm[2]->name == "Wolf_Run");  // l'autre réplique a sa file
    }
    CHECK(commands_of<PlayNpcParam>(out).empty());
    for (std::uint64_t t = 700; t < 5100; t += 500) g.tick(t, loaded_world(), {});
    out = g.tick(5100, loaded_world(), {});
    CHECK(has_log(out, "animations des PNJ : animés 0, Motion envoyés 0, rejoués 3, écrasés 2, ignorés 0"));
}

TEST("animations PNJ invité : NetId inconnu ou retiré → ignoré, compté ; Despawn → file vidée, ReleaseNpc") {
    Session g = guest_with_npcs();
    g.tick(400, loaded_world(), reliable2(host_npc_motion(1005, 3, 300), host_npc_param(1005, 300, 0, 1.0f)));
    // Motion en attente puis Despawn : jamais rejoué.
    g.tick(410, loaded_world(), reliable(encode_r(host_npc_motion(1000, 3, 300))));
    Output out = g.tick(420, loaded_world(), reliable(encode_r(proto::Despawn{1000})));
    CHECK_EQ(commands_of<ReleaseNpc>(out).size(), 1u);
    out = g.tick(430, loaded_world(), reliable(encode_r(host_npc_motion(1000, 3, 310))));
    for (std::uint64_t t = 500; t < 5100; t += 100) {
        out = g.tick(t, loaded_world(), {});
        CHECK(commands_of<PlayNpcMotion>(out).empty());
    }
    out = g.tick(5100, loaded_world(), {});
    CHECK(has_log(out, "rejoués 0, écrasés 0, ignorés 2, file pleine 0, MotionParam envoyés 0, regroupés 0, rejoués 0, ignorés 1"));
    // AniId inconnu pour un PNJ annoncé : ignoré, journalisé une fois.
    out = g.tick(5200, loaded_world(), reliable(encode_r(host_npc_motion(1001, 99, 300))));
    CHECK(has_log(out, "Motion d'AniId 99 (PNJ 1001) ignoré : AniId inconnu"));
}

TEST("animations PNJ invité : fin de session → répliques libérées avec leurs animations en attente") {
    Session g = guest_with_npcs();
    g.tick(400, loaded_world(), reliable(encode_r(host_npc_motion(1000, 3, 900))));
    Output out = g.tick(410, loaded_world(), reliable(encode_r(proto::SessionEnd{2})));
    CHECK_EQ(commands_of<ReleaseNpc>(out).size(), 2u);
    CHECK(commands_of<PlayNpcMotion>(out).empty());
    CHECK(commands_of<PlayNpcMotion>(g.tick(2000, loaded_world(), {})).empty());
}

TEST("animations PNJ : l'hôte ignore un Motion de PNJ venu de l'invité ; l'avatar reste inchangé") {
    Session s = host_with_avatar();
    Output out = s.tick(1300, loaded_world(), reliable(encode_r(host_npc_motion(1000, 0, 100))));
    CHECK(commands_of<PlayNpcMotion>(out).empty());
    CHECK(commands_of<PlayAvatarMotion>(out).empty());
}

TEST("animations PNJ invité : Motion reçu avant tout snapshot → rejoué aussitôt, sans MoveNpc ni SetNpcHp") {
    Session g = guest_in_session();  // aucun snapshot : horloge distante pas encore calée
    g.tick(150, loaded_world(), reliable2(npc_spawn(1000), proto::AniName{3, "Wolf_Run"}));
    const Output out = g.tick(160, loaded_world(), reliable(encode_r(host_npc_motion(1000, 3, 5000))));
    const auto pm = commands_of<PlayNpcMotion>(out);
    CHECK(pm.size() == 1 && pm[0]->net_id == 1000u && pm[0]->name == "Wolf_Run");
    CHECK(commands_of<MoveNpc>(out).empty());
    CHECK(commands_of<SetNpcHp>(out).empty());
}

// --- Robustesse de session (story 3.4a) --------------------------------------------------

TEST("secours 3.4a : Start de l'hôte et de l'invité porte un Heartbeat décodable et les délais") {
    for (const Settings& settings : {host_settings(), guest_settings()}) {
        Session s(settings);
        const Output out = s.tick(0, loaded_world(), {});
        const auto starts = commands_of<Start>(out);
        CHECK_EQ(starts.size(), 1u);
        if (starts.empty()) continue;
        const mp::net::Options& o = starts[0]->options;
        const auto decoded = proto::decode_reliable(o.keepalive.data(), o.keepalive.size());
        CHECK(decoded.ok());
        if (decoded.ok()) CHECK(std::holds_alternative<proto::Heartbeat>(decoded.message));
        CHECK_EQ(o.keepalive_interval_ms, 1000u);
        CHECK_EQ(o.keepalive_after_ms, 1500u);
        CHECK_EQ(o.keepalive_max_pause_ms, 60000u);
    }
    // Les délais du timeout et du heartbeat ne changent pas.
    CHECK_EQ(kPeerTimeoutMs, 5000u);
    CHECK_EQ(kHeartbeatIntervalMs, 1000u);
}

namespace {

// Ligne du bilan debug de l'invité (t0 = 100 : bilan à 5100), après des ticks vides.
std::string guest_stats_line(Session& g, std::uint64_t from) {
    for (std::uint64_t t = from; t < 5100; t += 400) g.tick(t, loaded_world(), {});
    const Output out = g.tick(5100, loaded_world(), {});
    for (const auto& line : out.logs) {
        if (line.text.find("[sync] snapshots (5 s)") != std::string::npos) return line.text;
    }
    return {};
}

}  // namespace

TEST("PNJ en avance 3.4a : entrée avant le Spawn → appliquée à son arrivée, comptée en fin de bilan") {
    Session g = guest_in_session();  // t0 = 100
    Output out = g.tick(200, loaded_world(), snapshot_in(npc_snapshot(2, {npc_entity(1000, 42.0f, 77)})));
    CHECK(commands_of<MoveNpc>(out).empty());
    out = g.tick(300, loaded_world(), reliable(encode_r(npc_spawn(1000))));  // 100 ms plus tard
    CHECK_EQ(commands_of<SpawnNpc>(out).size(), 1u);
    out = g.tick(310, loaded_world(), {});
    const auto moves = commands_of<MoveNpc>(out);
    CHECK_EQ(moves.size(), 1u);
    if (!moves.empty()) CHECK(moves[0]->net_id == 1000u && moves[0]->pose.x == 42.0f);
    // PV du snapshot (tick 2 = 100 ms distants) écrits à leur instant d'affichage.
    std::vector<int> hps;
    for (std::uint64_t t = 320; t <= 400; t += 10) {
        const Output o = g.tick(t, loaded_world(), {});
        for (const auto* c : commands_of<SetNpcHp>(o)) hps.push_back(c->hp);
    }
    CHECK(hps == std::vector<int>{77});
    const std::string line = guest_stats_line(g, 410);
    CHECK(line.find("entités ignorées 0") != std::string::npos);
    CHECK(line.find("entités inconnues 0") != std::string::npos);
    // Nouveau compteur à la suite des précédents (story 3.4b : les morts de PNJ viennent après).
    CHECK(line.find(" o/s ; PNJ reçus avant leur Spawn 1 ; morts de PNJ : ") != std::string::npos);
}

TEST("PNJ en avance 3.4a : plusieurs entrées (ticks 10, 12, 11) → seule la plus récente est appliquée") {
    Session g = guest_in_session();
    g.tick(200, loaded_world(), snapshot_in(npc_snapshot(10, {npc_entity(1000, 10.0f, 50)})));
    g.tick(250, loaded_world(), snapshot_in(npc_snapshot(12, {npc_entity(1000, 12.0f, 50)})));
    g.tick(260, loaded_world(), snapshot_in(npc_snapshot(11, {npc_entity(1000, 11.0f, 50)})));
    g.tick(300, loaded_world(), reliable(encode_r(npc_spawn(1000))));
    const Output out = g.tick(310, loaded_world(), {});
    const auto moves = commands_of<MoveNpc>(out);
    CHECK_EQ(moves.size(), 1u);
    if (!moves.empty()) CHECK_EQ(moves[0]->pose.x, 12.0f);
    const std::string line = guest_stats_line(g, 400);
    CHECK(line.find("entités inconnues 0") != std::string::npos);
    CHECK(line.find("PNJ reçus avant leur Spawn 1") != std::string::npos);
}

TEST("PNJ en avance 3.4a : Spawn 600 ms après l'entrée → expirée, comptée inconnue, réplique à la pose du Spawn") {
    Session g = guest_in_session();
    g.tick(200, loaded_world(), snapshot_in(npc_snapshot(2, {npc_entity(1000, 42.0f, 77)})));
    Output out = g.tick(800, loaded_world(), reliable(encode_r(npc_spawn(1000))));
    CHECK_EQ(commands_of<SpawnNpc>(out).size(), 1u);
    out = g.tick(810, loaded_world(), {});
    CHECK(commands_of<MoveNpc>(out).empty());  // reste à la pose du Spawn
    CHECK(commands_of<SetNpcHp>(out).empty());
    const std::string line = guest_stats_line(g, 900);
    CHECK(line.find("entités ignorées 1") != std::string::npos);
    CHECK(line.find("entités inconnues 1") != std::string::npos);
    CHECK(line.find("PNJ reçus avant leur Spawn 0") != std::string::npos);
}

TEST("PNJ en avance 3.4a : 65e NetId inconnu → ignoré et compté tout de suite") {
    Session g = guest_in_session();
    std::vector<proto::SnapshotEntity> many;
    for (std::uint32_t i = 0; i < 65; ++i) many.push_back(npc_entity(1000 + i, static_cast<float>(i), 10));
    // 54 entités au plus par snapshot : deux snapshots.
    g.tick(200, loaded_world(), snapshot_in(npc_snapshot(2, std::vector<proto::SnapshotEntity>(many.begin(), many.begin() + 40))));
    g.tick(210, loaded_world(), snapshot_in(npc_snapshot(2, std::vector<proto::SnapshotEntity>(many.begin() + 40, many.end()))));
    g.tick(300, loaded_world(), reliable2(npc_spawn(1000), npc_spawn(1064, 8)));
    const Output out = g.tick(310, loaded_world(), {});
    const auto moves = commands_of<MoveNpc>(out);
    CHECK(moves.size() == 1 && moves[0]->net_id == 1000u);  // 1064 n'a pas été gardé
    const std::string line = guest_stats_line(g, 400);
    CHECK(line.find("PNJ reçus avant leur Spawn 1") != std::string::npos);
    // 1064 tout de suite, puis les 63 autres gardés sans Spawn, à leur expiration.
    CHECK(line.find("entités inconnues 64") != std::string::npos);
    CHECK(line.find("entités ignorées 64") != std::string::npos);
}

TEST("PNJ en avance 3.4a : entrées gardées vidées à Ended ; l'hôte ne garde rien") {
    {
        Session g = guest_in_session();
        g.tick(200, loaded_world(), snapshot_in(npc_snapshot(2, {npc_entity(1000, 42.0f, 77)})));
        const Output out = g.tick(250, loaded_world(), reliable(encode_r(proto::SessionEnd{1})));
        CHECK(g.state() == State::Ended);
        CHECK(commands_of<ReleaseNpc>(out).empty());
    }
    {
        Session s = host_in_session();  // t0 = 1000 : bilan à 6000
        s.tick(1100, loaded_world(), snapshot_in(npc_snapshot(2, {npc_entity(1000, 42.0f, 77)})));
        std::string line;
        for (std::uint64_t t = 1200; t <= 6000; t += 400) {
            for (const auto& l : s.tick(t, loaded_world(), {}).logs) {
                if (l.text.find("[sync] snapshots (5 s)") != std::string::npos) line = l.text;
            }
        }
        CHECK(line.find("entités inconnues 1") != std::string::npos);
        CHECK(line.find("PNJ reçus avant leur Spawn 0") != std::string::npos);
    }
}

TEST("fin de session hôte 3.4a : 3 PNJ animés, Ended → un UncaptureNpc et une ligne « plus animé » par PNJ") {
    Session s = host_with_animated(3);
    const Output out = s.tick(1100, host_world(npcs_in_line(3)), reliable(encode_r(proto::SessionEnd{1})));
    CHECK(s.state() == State::Ended);
    CHECK(uncaptured_ids(out) == (std::vector<std::uint32_t>{1000u, 1001u, 1002u}));
    for (std::uint32_t id = 1000; id <= 1002; ++id) {
        std::size_t lines = 0;
        for (const auto& l : out.logs) {
            if (l.text == "[sync] PNJ " + std::to_string(id) + " : plus animé") ++lines;
        }
        CHECK_EQ(lines, 1u);
    }
    std::size_t all = 0;
    for (const auto& l : out.logs) all += l.text.find("plus animé") != std::string::npos ? 1 : 0;
    CHECK_EQ(all, 3u);
    // Plus rien ensuite (maintien, retour à Inactive).
    CHECK(commands_of<UncaptureNpc>(s.tick(2200, host_world(npcs_in_line(3)), {})).empty());
}

// --- Mort des répliques de PNJ (story 3.4b, protocole v11) --------------------------------

namespace {

// Index de la première commande de type T dans `out` (taille si absente).
template <typename T>
std::size_t first_index(const Output& out) {
    for (std::size_t i = 0; i < out.commands.size(); ++i) {
        if (std::holds_alternative<T>(out.commands[i])) return i;
    }
    return out.commands.size();
}

// Index de la commande NpcDeath ou MoveNpc de `net_id` (taille si absente).
template <typename T>
std::size_t index_for(const Output& out, std::uint32_t net_id) {
    for (std::size_t i = 0; i < out.commands.size(); ++i) {
        if (const auto* c = std::get_if<T>(&out.commands[i]); c != nullptr && c->net_id == net_id) return i;
    }
    return out.commands.size();
}

template <typename T>
std::size_t count_for(const Output& out, std::uint32_t net_id) {
    std::size_t n = 0;
    for (const auto* c : commands_of<T>(out)) n += c->net_id == net_id ? 1 : 0;
    return n;
}

// Bilan de l'hôte (t0 = 1000) à 6000, heartbeats de l'invité entre-temps.
std::string host_stats_line(Session& s, std::uint64_t from) {
    for (std::uint64_t t = from; t < 6000; t += 500) s.tick(t, host_world({}, false), reliable(encode_r(proto::Heartbeat{})));
    for (const auto& l : s.tick(6000, host_world({}, false), {}).logs) {
        if (l.text.find("[sync] snapshots (5 s)") != std::string::npos) return l.text;
    }
    return {};
}

}  // namespace

TEST("mort PNJ 3.4b hôte : PNJ annoncé mort → Death (pas de Despawn) avant le snapshot, UncaptureNpc, compté") {
    Session s = host_with_animated(2);  // 1000 et 1001 annoncés et animés
    std::vector<NpcState> npcs = npcs_in_line(2);
    npcs[0].alive = false;
    const Output out = s.tick(1100, host_world(npcs), {});
    const auto deaths = sent_of<proto::Death>(out);
    CHECK(deaths.size() == 1 && deaths[0].net_id == 1000u);
    CHECK(sent_of<proto::Despawn>(out).empty());
    CHECK(has_log(out, "[sync] PNJ 1000 mort (Death)"));
    CHECK(!has_log(out, "retiré (Despawn)"));
    CHECK(first_index<SendReliable>(out) < first_index<SendSnapshot>(out));
    CHECK(uncaptured_ids(out) == std::vector<std::uint32_t>{1000u});
    const auto sn = snapshots_sent(out);
    CHECK(sn.size() == 1 && sn[0].entities.size() == 2 && sn[0].entities[1].net_id == 1001u);
    CHECK_EQ(s.npc_count(), 1u);
    // Toujours mort au tick suivant : une seule fois.
    const Output again = s.tick(1150, host_world(npcs), {});
    CHECK(sent_of<proto::Death>(again).empty());
    const std::string line = host_stats_line(s, 1200);
    const std::string tail = "morts de PNJ : envoyées 1, appliquées 0, ignorées 0";
    CHECK(line.find(tail + " ; coups : ") != std::string::npos);  // story 3.5 : coups en fin
    CHECK(line.find("retirés 0") != std::string::npos);  // pas compté comme un Despawn
}

TEST("mort PNJ 3.4b hôte : NetId mort jamais réannoncé, même revenu à la vie ou dans le rayon") {
    Session s = host_in_session();
    s.tick(1050, host_world({npc_at(1, 1000.0f)}), {});
    s.tick(1100, host_world({npc_at(1, 1000.0f, 0, false)}), {});
    std::uint64_t t = 1100;
    // Revenu à la vie, dans le rayon ; puis hors du rayon et de retour.
    for (float dz : {1000.0f, 1000.0f, 6000.0f, 500.0f}) {
        t += 50;
        const Output out = s.tick(t, host_world({npc_at(1, dz)}), {});
        CHECK(npc_spawns_sent(out).empty());
        CHECK(reliables_sent(out).empty());
        CHECK(commands_of<CaptureNpc>(out).empty());
        const auto sn = snapshots_sent(out);
        CHECK(sn.size() == 1 && sn[0].entities.size() == 1);  // héros seul
    }
    CHECK_EQ(s.npc_count(), 0u);
    // Un autre PNJ reçoit toujours un nouveau NetId.
    const auto spawns = npc_spawns_sent(s.tick(t + 50, host_world({npc_at(1, 500.0f), npc_at(2, 600.0f)}), {}));
    CHECK(spawns.size() == 1 && spawns[0].net_id == 1001u);
}

TEST("mort PNJ 3.4b hôte : nouvelle session → table des morts vidée") {
    Session s = host_in_session();
    s.tick(1050, host_world({npc_at(1, 100.0f)}), {});
    s.tick(1100, host_world({npc_at(1, 100.0f, 0, false)}), {});
    s.tick(1150, host_world({}), reliable(encode_r(proto::SessionEnd{1})));
    s.tick(2200, loaded_world(), {});
    s.tick(3000, loaded_world(), control_from(kGuestB, encode(make_hello())));
    CHECK(s.state() == State::InSession);
    const auto spawns = npc_spawns_sent(s.tick(3050, host_world({npc_at(1, 100.0f)}), {}));
    CHECK(spawns.size() == 1 && spawns[0].net_id == 1000u);
}

TEST("mort PNJ 3.4b hôte : Death{≥ 1000} reçu → ignoré (l'hôte fait autorité), avatar intact") {
    Session s = host_with_avatar();
    const Output out = s.tick(1300, loaded_world(), reliable(encode_r(proto::Death{1000})));
    CHECK(has_log(out, "PNJ 1000 : Death reçu : ignoré (l'hôte fait autorité sur les PNJ)"));
    CHECK(commands_of<NpcDeath>(out).empty());
    CHECK(commands_of<AvatarDeath>(s.tick(2000, loaded_world(), {})).empty());
    CHECK(!s.avatar_dead());
}

TEST("mort PNJ 3.4b invité : Death après le snapshot tick 3 → NpcDeath à l'instant d'affichage, après MoveNpc, une fois") {
    Session g = guest_with_npcs();  // dernier snapshot : tick 3 (150 ms distants), affiché à now − 350
    Output out = g.tick(400, loaded_world(), reliable(encode_r(proto::Death{1000})));
    CHECK(has_log(out, "[sync] PNJ 1000 : Death reçu : mort à l'instant d'affichage"));
    CHECK(commands_of<NpcDeath>(out).empty());
    CHECK_EQ(count_for<MoveNpc>(out, 1000), 1u);  // en attente : encore placée
    out = g.tick(499, loaded_world(), {});
    CHECK(commands_of<NpcDeath>(out).empty());
    out = g.tick(500, loaded_world(), {});
    const auto deaths = commands_of<NpcDeath>(out);
    CHECK(deaths.size() == 1 && deaths[0]->net_id == 1000u);
    CHECK(index_for<MoveNpc>(out, 1000) < index_for<NpcDeath>(out, 1000));
    CHECK_EQ(g.npc_count(), 2u);  // reste dans la table jusqu'à Ended
    for (std::uint64_t t = 510; t < 700; t += 50) {
        out = g.tick(t, loaded_world(), {});
        CHECK(commands_of<NpcDeath>(out).empty());
        CHECK_EQ(count_for<MoveNpc>(out, 1000), 0u);
        CHECK_EQ(count_for<SetNpcHp>(out, 1000), 0u);
        CHECK_EQ(count_for<MoveNpc>(out, 1001), 1u);  // l'autre réplique continue
    }
    const std::string line = guest_stats_line(g, 800);
    const std::string tail = "morts de PNJ : envoyées 0, appliquées 1, ignorées 0";
    CHECK(line.find(tail + " ; coups : ") != std::string::npos);  // story 3.5 : coups en fin
}

TEST("mort PNJ 3.4b invité : Death sans snapshot (horloge pas prête) → NpcDeath aussitôt") {
    Session g = guest_in_session();
    g.tick(150, loaded_world(), reliable(encode_r(npc_spawn(1000))));
    const Output out = g.tick(160, loaded_world(), reliable(encode_r(proto::Death{1000})));
    const auto deaths = commands_of<NpcDeath>(out);
    CHECK(deaths.size() == 1 && deaths[0]->net_id == 1000u);
}

TEST("mort PNJ 3.4b invité : gestes en file jetés, gestes après le Death ignorés et comptés, aucun PlayNpc*") {
    Session g = guest_with_npcs();
    g.tick(400, loaded_world(), reliable(encode_r(host_npc_motion(1000, 3, 300))));  // dû après la mort
    g.tick(410, loaded_world(), reliable(encode_r(proto::Death{1000})));
    g.tick(420, loaded_world(), reliable2(host_npc_motion(1000, 3, 160), host_npc_param(1000, 160, 0, 1.0f)));
    for (std::uint64_t t = 430; t < 900; t += 10) {
        const Output out = g.tick(t, loaded_world(), {});
        CHECK(commands_of<PlayNpcMotion>(out).empty());
        CHECK(commands_of<PlayNpcParam>(out).empty());
    }
    g.tick(900, loaded_world(), reliable(encode_r(host_npc_motion(1000, 3, 500))));
    const std::string line = guest_stats_line(g, 1000);
    CHECK(line.find("Motion envoyés 0, rejoués 0, écrasés 0, ignorés 3, file pleine 0, MotionParam envoyés 0, "
                    "regroupés 0, rejoués 0, ignorés 1") != std::string::npos);
    CHECK(line.find("appliquées 1") != std::string::npos);
}

TEST("mort PNJ 3.4b invité : entrée de snapshot après la mort → ni MoveNpc ni SetNpcHp, pas « inconnue »") {
    Session g = guest_with_npcs();
    g.tick(400, loaded_world(), reliable(encode_r(proto::Death{1000})));
    g.tick(500, loaded_world(), {});  // NpcDeath
    for (std::uint32_t tick = 6; tick < 10; ++tick) {
        const Output out = g.tick(200 + tick * 50, loaded_world(),
                                  snapshot_in(npc_snapshot(tick, {npc_entity(1000, 99.0f, 7), npc_entity(1001, 0.0f, 100)})));
        CHECK_EQ(count_for<MoveNpc>(out, 1000), 0u);
        CHECK_EQ(count_for<SetNpcHp>(out, 1000), 0u);
    }
    const std::string line = guest_stats_line(g, 800);
    CHECK(line.find("entités ignorées 0") != std::string::npos);
    CHECK(line.find("entités inconnues 0") != std::string::npos);
}

TEST("mort PNJ 3.4b invité : Death en double → ignoré (Debug) ; NetId non annoncé → ignoré (Warn) ; comptés") {
    Session g = guest_with_npcs();
    Output out = g.tick(400, loaded_world(), reliable2(proto::Death{1000}, proto::Death{1000}));  // en attente
    bool debug = false;
    for (const auto& l : out.logs) {
        debug |= l.level == Level::Debug && l.text == "[sync] PNJ 1000 : Death reçu : ignoré (réplique déjà morte)";
    }
    CHECK(debug);
    g.tick(500, loaded_world(), {});
    out = g.tick(510, loaded_world(), reliable(encode_r(proto::Death{1000})));  // déjà morte
    CHECK(has_log(out, "ignoré (réplique déjà morte)"));
    CHECK(commands_of<NpcDeath>(out).empty());
    out = g.tick(520, loaded_world(), reliable(encode_r(proto::Death{1500})));
    bool warn = false;
    for (const auto& l : out.logs) {
        warn |= l.level == Level::Warn && l.text == "[sync] PNJ 1500 : Death reçu : ignoré (NetId non annoncé)";
    }
    CHECK(warn);
    CHECK(commands_of<NpcDeath>(out).empty());
    CHECK(!g.avatar_dead());
    const std::string line = guest_stats_line(g, 600);
    CHECK(line.find("morts de PNJ : envoyées 0, appliquées 1, ignorées 3") != std::string::npos);
}

TEST("mort PNJ 3.4b invité : Spawn{1} d'une réplique morte refusé ; Despawn la libère ; Ended aussi") {
    {
        Session g = guest_with_npcs();
        g.tick(400, loaded_world(), reliable(encode_r(proto::Death{1000})));
        g.tick(500, loaded_world(), {});
        Output out = g.tick(510, loaded_world(), reliable(encode_r(npc_spawn(1000))));
        CHECK(commands_of<SpawnNpc>(out).empty());
        CHECK(has_log(out, "PNJ 1000 : Spawn reçu") && has_log(out, "ignoré (NetId déjà annoncé)"));
        out = g.tick(520, loaded_world(), reliable(encode_r(proto::Despawn{1000})));
        CHECK_EQ(count_for<ReleaseNpc>(out, 1000), 1u);
        CHECK_EQ(g.npc_count(), 1u);
    }
    {
        Session g = guest_with_npcs();
        g.tick(400, loaded_world(), reliable(encode_r(proto::Death{1000})));
        g.tick(500, loaded_world(), {});
        const Output out = g.tick(510, loaded_world(), reliable(encode_r(proto::SessionEnd{1})));
        const auto rel = commands_of<ReleaseNpc>(out);
        CHECK(rel.size() == 2 && rel[0]->net_id == 1000u && rel[1]->net_id == 1001u);
        CHECK(commands_of<NpcDeath>(out).empty());
    }
}

TEST("mort PNJ 3.4b invité : PNJ vivant à 0 PV (à terre chez l'hôte) → SetNpcHp 50 puis 1, jamais 0") {
    Session g = guest_in_session();  // t0 = 100
    g.tick(150, loaded_world(), reliable(encode_r(npc_spawn(1000))));
    std::vector<int> hps;
    for (std::uint32_t tick = 0; tick <= 12; ++tick) {
        const std::uint16_t hp = tick < 5 ? 50 : 0;
        const Output out = g.tick(200 + tick * 50, loaded_world(),
                                  snapshot_in(npc_snapshot(tick, {npc_entity(1000, 0.0f, hp)})));
        for (const auto* c : commands_of<SetNpcHp>(out)) hps.push_back(c->hp);
        CHECK(commands_of<NpcDeath>(out).empty());
    }
    CHECK(hps == (std::vector<int>{50, 1}));
}

TEST("mort PNJ 3.4b invité : Death en attente puis Ended ou Despawn → NpcDeath avant ReleaseNpc") {
    {
        Session g = guest_with_npcs();
        g.tick(400, loaded_world(), reliable(encode_r(proto::Death{1000})));  // affichée à 500
        const Output out = g.tick(410, loaded_world(), reliable(encode_r(proto::SessionEnd{1})));
        CHECK_EQ(count_for<NpcDeath>(out, 1000), 1u);
        CHECK_EQ(count_for<NpcDeath>(out, 1001), 0u);
        CHECK(index_for<NpcDeath>(out, 1000) < index_for<ReleaseNpc>(out, 1000));
        CHECK(index_for<ReleaseNpc>(out, 1000) < out.commands.size());
    }
    {
        Session g = guest_with_npcs();
        g.tick(400, loaded_world(), reliable(encode_r(proto::Death{1000})));
        const Output out = g.tick(410, loaded_world(), reliable(encode_r(proto::Despawn{1000})));
        CHECK_EQ(count_for<NpcDeath>(out, 1000), 1u);
        CHECK(index_for<NpcDeath>(out, 1000) < index_for<ReleaseNpc>(out, 1000));
        CHECK(index_for<ReleaseNpc>(out, 1000) < out.commands.size());
    }
}

TEST("mort PNJ 3.4b invité : snapshot plus récent reçu après le Death → mort reculée à sa pose") {
    Session g = guest_with_npcs();  // dernier snapshot : tick 3 (150 ms distants)
    g.tick(400, loaded_world(), reliable(encode_r(proto::Death{1000})));
    // Snapshot du tick 5 (250 ms) en retard sur le canal non fiable : la mort attend son affichage.
    g.tick(450, loaded_world(), snapshot_in(npc_snapshot(5, {npc_entity(1000, 50.0f, 100)})));  // horloge inchangée
    CHECK(commands_of<NpcDeath>(g.tick(500, loaded_world(), {})).empty());
    CHECK(commands_of<NpcDeath>(g.tick(599, loaded_world(), {})).empty());
    const Output out = g.tick(600, loaded_world(), {});
    CHECK_EQ(count_for<NpcDeath>(out, 1000), 1u);
    CHECK(index_for<MoveNpc>(out, 1000) < index_for<NpcDeath>(out, 1000));
}

// --- Coups de l'invité (story 3.5) --------------------------------------------------

namespace {

}  // namespace

TEST("coups 3.5 invité : LocalHit sur une réplique vivante → HitRequest fiable, Info, compté envoyé") {
    Session g = guest_with_npcs();
    const Output out = g.tick(400, loaded_world(), {LocalHit{1000}});
    const auto hits = sent_of<proto::HitRequest>(out);
    CHECK(hits.size() == 1 && hits[0].net_id == 1000u);
    CHECK(has_log_at(out, Level::Info, "[sync] coup sur PNJ 1000 : HitRequest envoyé"));
    const std::string line = guest_stats_line(g, 500);
    CHECK(line.find("coups : envoyés 1, appliqués 0, ignorés 0, bloqués 0 ; ") != std::string::npos);
}

TEST("coups 3.5 invité : réplique morte ou en attente de mort → rien envoyé, Debug, compté ignoré") {
    Session g = guest_with_npcs();
    g.tick(400, loaded_world(), reliable(encode_r(proto::Death{1000})));  // en attente
    Output out = g.tick(410, loaded_world(), {LocalHit{1000}});
    CHECK(sent_of<proto::HitRequest>(out).empty());
    CHECK(has_log_at(out, Level::Debug, "[sync] coup sur PNJ 1000 : ignoré (réplique morte)"));
    g.tick(500, loaded_world(), {});  // morte
    out = g.tick(510, loaded_world(), {LocalHit{1000}});
    CHECK(sent_of<proto::HitRequest>(out).empty());
    const std::string line = guest_stats_line(g, 600);
    CHECK(line.find("coups : envoyés 0, appliqués 0, ignorés 2, bloqués 0") != std::string::npos);
}

TEST("coups 3.5 invité : NetId non annoncé → rien envoyé, Debug, compté ignoré") {
    Session g = guest_with_npcs();
    const Output out = g.tick(400, loaded_world(), {LocalHit{1500}});
    CHECK(sent_of<proto::HitRequest>(out).empty());
    CHECK(has_log_at(out, Level::Debug, "[sync] coup sur PNJ 1500 : ignoré (réplique non annoncée)"));
    const std::string line = guest_stats_line(g, 500);
    CHECK(line.find("coups : envoyés 0, appliqués 0, ignorés 1, bloqués 0") != std::string::npos);
}

TEST("coups 3.5 invité : LocalHit hors session (Connecting) → ignoré, rien envoyé") {
    Session g(guest_settings());
    g.tick(0, loaded_world(), {});
    CHECK(g.state() == State::Connecting);
    const Output out = g.tick(10, loaded_world(), {LocalHit{1000}});
    CHECK(sent_of<proto::HitRequest>(out).empty());
    CHECK(commands_of<SendReliable>(out).empty());
}

TEST("coups 3.5 : coups bloqués par le détour → comptés en fin de bilan") {
    Session g = guest_with_npcs();
    g.tick(400, loaded_world(), {LocalHitsBlocked{3}, LocalHit{1000}});
    g.tick(450, loaded_world(), {LocalHitsBlocked{2}});
    const std::string line = guest_stats_line(g, 500);
    CHECK(line.find("coups : envoyés 1, appliqués 0, ignorés 0, bloqués 5") != std::string::npos);
}

TEST("coups 3.5 hôte : HitRequest d'un PNJ annoncé et vivant → ApplyHit{NetId, GUID}, compté appliqué") {
    Session s = host_in_session();
    s.tick(1050, host_world(npcs_in_line(2)), {});  // 1000 = GUID 1, 1001 = GUID 2
    const Output out = s.tick(1060, host_world({}, false), reliable(encode_r(proto::HitRequest{1001})));
    const auto apply = commands_of<ApplyHit>(out);
    CHECK(apply.size() == 1 && apply[0]->net_id == 1001u && apply[0]->guid == guid_of(2));
    const std::string line = host_stats_line(s, 1100);
    CHECK(line.find("coups : envoyés 0, appliqués 1, ignorés 0, bloqués 0 ; ") != std::string::npos);
}

TEST("coups 3.5 hôte : HitRequest d'un NetId inconnu → pas de commande, Warn « NetId inconnu », compté") {
    Session s = host_in_session();
    s.tick(1050, host_world(npcs_in_line(1)), {});
    const Output out = s.tick(1060, host_world({}, false), reliable(encode_r(proto::HitRequest{1500})));
    CHECK(commands_of<ApplyHit>(out).empty());
    CHECK(has_log_at(out, Level::Warn, "[sync] HitRequest PNJ 1500 ignoré : NetId inconnu"));
    const std::string line = host_stats_line(s, 1100);
    CHECK(line.find("coups : envoyés 0, appliqués 0, ignorés 1, bloqués 0") != std::string::npos);
}

TEST("coups 3.5 hôte : HitRequest d'un PNJ mort → pas de commande, Warn « PNJ mort »") {
    Session s = host_in_session();
    s.tick(1050, host_world({npc_at(1, 100.0f)}), {});
    s.tick(1100, host_world({npc_at(1, 100.0f, 0, false)}), {});  // Death{1000}
    const Output out = s.tick(1110, host_world({}, false), reliable(encode_r(proto::HitRequest{1000})));
    CHECK(commands_of<ApplyHit>(out).empty());
    CHECK(has_log_at(out, Level::Warn, "[sync] HitRequest PNJ 1000 ignoré : PNJ mort"));
}

TEST("coups 3.5 hôte : HitRequest d'un NetId < 1000 → pas de commande, Warn") {
    Session s = host_with_avatar();
    const Output out = s.tick(1300, loaded_world(), reliable(encode_r(proto::HitRequest{2})));
    CHECK(commands_of<ApplyHit>(out).empty());
    CHECK(has_log_at(out, Level::Warn, "[sync] HitRequest PNJ 2 ignoré : NetId < 1000"));
    CHECK(s.state() == State::InSession);
}

TEST("coups 3.5 invité : HitRequest reçu → ignoré (Warn), aucune commande") {
    Session g = guest_with_npcs();
    const Output out = g.tick(400, loaded_world(), reliable(encode_r(proto::HitRequest{1000})));
    CHECK(commands_of<ApplyHit>(out).empty());
    CHECK(has_log_at(out, Level::Warn, "[sync] HitRequest PNJ 1000 ignoré"));
    CHECK(g.state() == State::InSession);
}

TEST("coups 3.5 hôte : LocalHit chez l'hôte → ignoré, rien envoyé") {
    Session s = host_in_session();
    s.tick(1050, host_world(npcs_in_line(1)), {});
    const Output out = s.tick(1060, host_world({}, false), {LocalHit{1000}});
    CHECK(sent_of<proto::HitRequest>(out).empty());
    CHECK(commands_of<ApplyHit>(out).empty());
}

// --- Story 3.6 : coups subis par l'invité ---------------------------------------------------------

TEST("coups subis 3.6 hôte : LocalHitOnGuest en session → HitOnGuest fiable, Info, compté envoyé") {
    Session s = host_in_session();
    const Output out = s.tick(1060, host_world({}, false), {LocalHitOnGuest{1000, 18}});
    const auto sent = sent_of<proto::HitOnGuest>(out);
    CHECK(sent.size() == 1 && sent[0].source_net_id == 1000u && sent[0].damage == 18u);
    CHECK(has_log_at(out, Level::Info,
                     "[sync] coup sur l'avatar invité (source PNJ 1000) : HitOnGuest envoyé, dégâts bruts 18"));
    const std::string line = host_stats_line(s, 1100);
    const std::string tail = "coups subis : HitOnGuest envoyés 1, reçus 0, ignorés 0";
    CHECK(line.find(tail + " ; à terre : ") != std::string::npos);
}

TEST("coups subis 3.6 hôte : source inconnue (0) → HitOnGuest{0, D}, journalisé « inconnue »") {
    Session s = host_in_session();
    const Output out = s.tick(1060, host_world({}, false), {LocalHitOnGuest{0, 7}});
    const auto sent = sent_of<proto::HitOnGuest>(out);
    CHECK(sent.size() == 1 && sent[0].source_net_id == 0u && sent[0].damage == 7u);
    CHECK(has_log_at(out, Level::Info,
                     "[sync] coup sur l'avatar invité (source inconnue) : HitOnGuest envoyé, dégâts bruts 7"));
}

TEST("coups subis 3.6 hôte : LocalHitOnGuest hors session → rien envoyé, Debug, compté ignoré") {
    Session s(host_settings());
    s.tick(0, loaded_world(), {});
    CHECK(s.state() != State::InSession);
    const Output out = s.tick(10, loaded_world(), {LocalHitOnGuest{1000, 18}});
    CHECK(sent_of<proto::HitOnGuest>(out).empty());
    CHECK(commands_of<SendReliable>(out).empty());
    CHECK(has_log_at(out, Level::Debug, "[sync] coup sur l'avatar invité (source PNJ 1000) : ignoré (hors session)"));
}

TEST("coups subis 3.6 invité : LocalHitOnGuest chez l'invité → rien envoyé, Debug, compté ignoré") {
    Session g = guest_with_npcs();
    const Output out = g.tick(400, loaded_world(), {LocalHitOnGuest{1000, 18}});
    CHECK(sent_of<proto::HitOnGuest>(out).empty());
    CHECK(has_log_at(out, Level::Debug, "[sync] coup sur l'avatar invité (source PNJ 1000) : ignoré (pas hôte)"));
    const std::string line = guest_stats_line(g, 500);
    const std::string tail = "coups subis : HitOnGuest envoyés 0, reçus 0, ignorés 1";
    CHECK(line.find(tail + " ; à terre : ") != std::string::npos);
}

TEST("coups subis 3.6 invité : HitOnGuest reçu en session → ApplyHitOnGuest{1000, 18}, compté reçu") {
    Session g = guest_with_npcs();
    const Output out = g.tick(400, loaded_world(), reliable(encode_r(proto::HitOnGuest{1000, 18})));
    const auto apply = commands_of<ApplyHitOnGuest>(out);
    CHECK(apply.size() == 1 && apply[0]->source == 1000u && apply[0]->damage == 18u);
    CHECK(g.state() == State::InSession);
    const std::string line = guest_stats_line(g, 500);
    const std::string tail = "coups subis : HitOnGuest envoyés 0, reçus 1, ignorés 0";
    CHECK(line.find(tail + " ; à terre : ") != std::string::npos);
}

TEST("coups subis 3.6 hôte : HitOnGuest reçu → aucune commande, Warn, compté ignoré") {
    Session s = host_in_session();
    const Output out = s.tick(1060, host_world({}, false), reliable(encode_r(proto::HitOnGuest{1000, 18})));
    CHECK(commands_of<ApplyHitOnGuest>(out).empty());
    CHECK(has_log_at(out, Level::Warn, "[sync] HitOnGuest ignoré : l'hôte ne subit pas de coups"));
    CHECK(s.state() == State::InSession);
    const std::string line = host_stats_line(s, 1100);
    const std::string tail = "coups subis : HitOnGuest envoyés 0, reçus 0, ignorés 1";
    CHECK(line.find(tail + " ; à terre : ") != std::string::npos);
}

// --- Story 3.7 : à terre et réapparition de l'invité ----------------------------------------------

namespace {

std::vector<Input> heartbeat_in() { return reliable(encode_r(proto::Heartbeat{})); }

std::vector<Input> respawn_in(float x, float y, float z, float yaw, std::uint32_t tick = 0,
                              std::uint32_t net_id = proto::kGuestNetId) {
    return reliable(encode_r(proto::Respawn{net_id, x, y, z, yaw, tick}));
}

// Bilan des 5 s, à partir de « à terre : ».
std::string down_tail(const std::string& line) {
    const auto at = line.find("à terre : ");
    if (at == std::string::npos) return std::string();
    const auto end = line.find(" ; attaques natives : ", at);  // story 3.8 : dernier bloc du bilan
    return line.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

}  // namespace

TEST("à terre 3.7 invité : LocalDown en session → Death{2} une fois, Info ; deuxième → rien, Debug, ignoré") {
    Session g = guest_in_session();
    g.tick(300, loaded_world(), heartbeat_in());  // activité de l'hôte : pas de timeout avant le bilan
    Output out = g.tick(400, loaded_world(), {LocalDown{kDownCauseHitOnGuest}});
    const auto d = sent_of<proto::Death>(out);
    CHECK(d.size() == 1 && d[0].net_id == proto::kGuestNetId);
    CHECK(has_log_at(out, Level::Info, "[sync] héros à terre (HitOnGuest) : Death envoyé, attente du Respawn"));
    CHECK(g.guest_down());
    out = g.tick(450, loaded_world(), {LocalDown{kDownCauseMelee}});
    CHECK(sent_of<proto::Death>(out).empty());
    CHECK(has_log_at(out, Level::Debug, "[sync] héros à terre (DownOrDeadByMeleeWeapon) : déjà à terre, ignoré"));
    const std::string line = guest_stats_line(g, 500);
    CHECK_EQ(down_tail(line), std::string("à terre : Death 1, Respawn envoyés 0, reçus 0, accusés 0, ignorés 1, "
                                          "snapshots périmés 0"));
}

TEST("à terre 3.7 invité : LocalDown hors session ou chez l'hôte → rien envoyé") {
    Session g(guest_settings());
    g.tick(0, loaded_world(), {});
    CHECK(g.state() == State::Connecting);
    Output out = g.tick(10, loaded_world(), {LocalDown{kDownCauseInferno}});
    CHECK(commands_of<SendReliable>(out).empty());
    CHECK(!g.guest_down());
    Session s = host_with_avatar();
    out = s.tick(1200, loaded_world(), {LocalDown{kDownCauseHitOnGuest}});
    CHECK(sent_of<proto::Death>(out).empty());
    CHECK(has_log_at(out, Level::Debug, "ignoré (pas invité)"));
}

TEST("à terre 3.7 invité : HitOnGuest reçu à terre → pas d'ApplyHitOnGuest, Debug, coups subis ignorés") {
    Session g = guest_in_session();
    g.tick(400, loaded_world(), {LocalDown{kDownCauseHitOnGuest}});
    const Output out = g.tick(410, loaded_world(), reliable(encode_r(proto::HitOnGuest{1000, 18})));
    CHECK(commands_of<ApplyHitOnGuest>(out).empty());
    CHECK(has_log_at(out, Level::Debug, "ignoré (héros à terre)"));
    const std::string line = guest_stats_line(g, 500);
    CHECK(line.find("coups subis : HitOnGuest envoyés 0, reçus 0, ignorés 1 ; ") != std::string::npos);
}

TEST("à terre 3.7 invité : Respawn valide → RespawnHero{pose, téléport}, accusé tick courant + 1, plus à terre") {
    Session g = guest_in_session();  // t0 = 100
    g.tick(300, loaded_world(), heartbeat_in());
    g.tick(400, loaded_world(), {LocalDown{kDownCauseHitOnGuest}});
    const Output out = g.tick(420, loaded_world(), respawn_in(300.0f, -50.5f, 2200.0f, -1.5f));  // tick 6
    const auto r = commands_of<RespawnHero>(out);
    CHECK(r.size() == 1 && r[0]->teleport && r[0]->pose.x == 300.0f && r[0]->pose.z == 2200.0f &&
          r[0]->pose.yaw == -1.5f);
    const auto acks = sent_of<proto::Respawn>(out);
    CHECK(acks.size() == 1 && acks[0].net_id == proto::kGuestNetId && acks[0].x == 300.0f &&
          acks[0].y == -50.5f && acks[0].z == 2200.0f && acks[0].yaw == -1.5f && acks[0].tick == 7u);
    CHECK(has_log_at(out, Level::Info, "[sync] Respawn reçu : héros replacé (300.0, -50.5, 2200.0) lacet=-1.500, "
                                       "PV pleins ; accusé envoyé (tick 7)"));
    CHECK(!g.guest_down());
    // Plus à terre : un HitOnGuest blesse de nouveau, une mise à terre renvoie Death.
    CHECK_EQ(commands_of<ApplyHitOnGuest>(g.tick(430, loaded_world(), reliable(encode_r(proto::HitOnGuest{0, 5})))).size(), 1u);
    CHECK_EQ(sent_of<proto::Death>(g.tick(440, loaded_world(), {LocalDown{kDownCauseHitOnGuest}})).size(), 1u);
    const std::string line = guest_stats_line(g, 500);
    CHECK_EQ(down_tail(line), std::string("à terre : Death 2, Respawn envoyés 0, reçus 1, accusés 1, ignorés 0, "
                                          "snapshots périmés 0"));
}

TEST("à terre 3.7 invité : Respawn hors état (pas à terre, NetId ≠ 2, pose non finie, mort native) → rien, Warn") {
    Session g = guest_in_session();
    Output out = g.tick(400, loaded_world(), respawn_in(1, 2, 3, 0));
    CHECK(commands_of<RespawnHero>(out).empty() && sent_of<proto::Respawn>(out).empty());
    CHECK(has_log_at(out, Level::Warn, "ignoré (héros pas à terre)"));
    g.tick(410, loaded_world(), {LocalDown{kDownCauseHitOnGuest}});
    out = g.tick(420, loaded_world(), respawn_in(1, 2, 3, 0, 0, proto::kHostNetId));
    CHECK(commands_of<RespawnHero>(out).empty());
    CHECK(has_log_at(out, Level::Warn, "ignoré (NetId autre que 2)"));
    out = g.tick(430, loaded_world(), respawn_in(std::nanf(""), 2, 3, 0));
    CHECK(commands_of<RespawnHero>(out).empty());
    CHECK(has_log_at(out, Level::Warn, "ignoré (pose non finie)"));
    CHECK(g.guest_down());
    World dead = loaded_world();
    dead.dead = true;
    out = g.tick(440, dead, respawn_in(1, 2, 3, 0));
    CHECK(commands_of<RespawnHero>(out).empty() && sent_of<proto::Respawn>(out).empty());
    CHECK(has_log_at(out, Level::Warn, "pas de relève d'un mort"));
    CHECK(has_log_at(out, Level::Warn, "mort native du héros (chute, eau profonde ?) : Death déjà envoyé"));
    CHECK(sent_of<proto::Death>(out).empty());  // verrou : déjà envoyé par la mise à terre
    out = g.tick(450, dead, respawn_in(1, 2, 3, 0));
    CHECK(commands_of<RespawnHero>(out).empty());
    const std::string line = guest_stats_line(g, 500);
    CHECK_EQ(down_tail(line), std::string("à terre : Death 1, Respawn envoyés 0, reçus 0, accusés 0, ignorés 5, "
                                          "snapshots périmés 0"));
}

TEST("à terre 3.7 invité : repli 10 s sans Respawn → RespawnHero sur place, Warn ; Respawn tardif ignoré") {
    Session g = guest_in_session();
    g.tick(400, loaded_world(), {LocalDown{kDownCauseProjectile}});
    for (std::uint64_t t = 1000; t < 10400; t += 1000) {
        CHECK(commands_of<RespawnHero>(g.tick(t, loaded_world(), heartbeat_in())).empty());
    }
    CHECK(commands_of<RespawnHero>(g.tick(10399, loaded_world(), {})).empty());
    Output out = g.tick(10400, loaded_world(), {});
    const auto r = commands_of<RespawnHero>(out);
    CHECK(r.size() == 1 && !r[0]->teleport);
    CHECK(has_log_at(out, Level::Warn, "[sync] pas de Respawn de l'hôte depuis 10 s : PV rendus sur place"));
    CHECK(!g.guest_down());
    CHECK(g.state() == State::InSession);
    out = g.tick(10500, loaded_world(), respawn_in(1, 2, 3, 0));
    CHECK(commands_of<RespawnHero>(out).empty() && sent_of<proto::Respawn>(out).empty());
    CHECK(has_log_at(out, Level::Warn, "ignoré (héros pas à terre)"));
}

TEST("à terre 3.7 invité : mort native → Death une fois, pas de repli à 10 s ; Ended remet à zéro") {
    Session g = guest_in_session();
    World dead = loaded_world();
    dead.dead = true;
    Output out = g.tick(400, dead, {});
    CHECK_EQ(sent_of<proto::Death>(out).size(), 1u);
    CHECK(sent_of<proto::Death>(g.tick(450, dead, {LocalDown{kDownCauseMelee}})).empty());
    for (std::uint64_t t = 1000; t <= 12000; t += 1000) {
        CHECK(commands_of<RespawnHero>(g.tick(t, dead, heartbeat_in())).empty());
    }
    CHECK(g.guest_down());
    g.tick(12100, dead, reliable(encode_r(proto::SessionEnd{2})));
    CHECK(g.state() == State::Ended);
    CHECK(!g.guest_down());
}

TEST("à terre 3.7 hôte : Death{2} → pas d'AvatarDeath ; Respawn{2, devant l'hôte, 0} et MoveAvatar à cette pose") {
    Session s = host_with_clock();
    const Output out = s.tick(1400, loaded_world(), reliable(encode_r(proto::Death{proto::kGuestNetId})));
    CHECK(has_log_at(out, Level::Info, "[sync] Death reçu : invité à terre, Respawn à envoyer"));
    CHECK(commands_of<AvatarDeath>(out).empty());
    const World w = loaded_world();
    const Pose expected = guest_pose_near(Pose{w.x, w.y, w.z, w.yaw});
    const auto sent = sent_of<proto::Respawn>(out);
    CHECK(sent.size() == 1 && sent[0].net_id == proto::kGuestNetId && sent[0].tick == 0u && sent[0].x == expected.x &&
          sent[0].y == expected.y && sent[0].z == expected.z && sent[0].yaw == expected.yaw);
    const auto moves = commands_of<MoveAvatar>(out);
    CHECK(moves.size() == 1 && moves[0]->net_id == proto::kGuestNetId && moves[0]->pose.x == expected.x &&
          moves[0]->pose.z == expected.z && moves[0]->pose.yaw == expected.yaw);
    CHECK(has_log_at(out, Level::Info, "[sync] Respawn envoyé : invité replacé "));
    CHECK(s.respawn_waiting() && !s.respawn_pending());
    for (std::uint64_t t = 1450; t < 3000; t += 50) CHECK(commands_of<AvatarDeath>(s.tick(t, loaded_world(), {})).empty());
    CHECK(!s.avatar_dead());
}

TEST("à terre 3.7 hôte : pose de l'hôte illisible → Respawn différé au premier tick de pose lisible") {
    Session s = host_with_clock();
    World blind = loaded_world();
    blind.pose_ok = false;
    Output out = s.tick(1400, blind, reliable(encode_r(proto::Death{proto::kGuestNetId})));
    CHECK(sent_of<proto::Respawn>(out).empty());
    CHECK(s.respawn_pending());
    CHECK(sent_of<proto::Respawn>(s.tick(1450, blind, {})).empty());
    out = s.tick(1500, loaded_world(), {});
    CHECK_EQ(sent_of<proto::Respawn>(out).size(), 1u);
    CHECK(s.respawn_waiting());
}

TEST("à terre 3.7 hôte : Death{2} en attente d'accusé ou sans avatar → ignoré, Warn") {
    Session s = host_with_clock();
    s.tick(1400, loaded_world(), reliable(encode_r(proto::Death{proto::kGuestNetId})));
    Output out = s.tick(1410, loaded_world(), reliable(encode_r(proto::Death{proto::kGuestNetId})));
    CHECK(sent_of<proto::Respawn>(out).empty());
    CHECK(has_log_at(out, Level::Warn, "[sync] Death reçu (NetId 2 (invité)) : ignoré (réapparition déjà en cours)"));
    std::uint32_t conv = 0;
    Session none = hosting_with_a(conv);
    out = none.tick(1100, loaded_world(), reliable(encode_r(proto::Death{proto::kGuestNetId})));
    CHECK(sent_of<proto::Respawn>(out).empty());
    CHECK(has_log_at(out, Level::Warn, "ignoré (pas d'avatar)"));
    CHECK(!none.respawn_pending());
}

TEST("à terre 3.7 hôte : snapshots de l'invité ignorés en attente d'accusé, avatar laissé à la pose du Respawn") {
    Session s = host_with_clock();  // ticks 0 à 3 reçus
    s.tick(1400, loaded_world(), reliable(encode_r(proto::Death{proto::kGuestNetId})));
    for (std::uint32_t tick = 4; tick < 20; ++tick) {
        const Output out = s.tick(1200 + tick * 50, loaded_world(),
                                  snapshot_in(snapshot_bytes(tick, proto::kGuestNetId, 999.0f)));
        CHECK(commands_of<MoveAvatar>(out).empty());
    }
    const std::string line = host_stats_line(s, 2300);
    CHECK(down_tail(line).find("Death 1, Respawn envoyés 1, reçus 0, accusés 0, ignorés 0, snapshots périmés 16") !=
          std::string::npos);
}

TEST("à terre 3.7 hôte : accusé tick 42 → fin d'attente ; entrées de tick < 42 ignorées, ≥ 42 interpolées") {
    Session s = host_with_clock();
    s.tick(1400, loaded_world(), reliable(encode_r(proto::Death{proto::kGuestNetId})));
    Output out = s.tick(1500, loaded_world(), respawn_in(1, 2, 3, 0, 42));
    CHECK(has_log_at(out, Level::Info, "[sync] réapparition confirmée par l'invité (tick 42)"));
    CHECK(!s.respawn_waiting());
    // Snapshot parti avant l'accusé mais arrivé après : périmé.
    out = s.tick(3240, loaded_world(), snapshot_in(snapshot_bytes(41, proto::kGuestNetId, 999.0f)));
    CHECK(commands_of<MoveAvatar>(out).empty());
    out = s.tick(3270, loaded_world(), snapshot_in(snapshot_bytes(42, proto::kGuestNetId, 500.0f)));
    out = s.tick(3320, loaded_world(), snapshot_in(snapshot_bytes(43, proto::kGuestNetId, 510.0f)));
    bool moved = false;
    for (std::uint64_t t = 3330; t < 3600; t += 20) {
        const Output frame = s.tick(t, loaded_world(), {});
        for (const auto* m : commands_of<MoveAvatar>(frame)) {
            moved = true;
            CHECK(m->pose.x >= 500.0f && m->pose.x < 999.0f);  // jamais l'ancienne position
        }
    }
    CHECK(moved);
    const std::string line = host_stats_line(s, 3700);
    CHECK(down_tail(line).find("Death 1, Respawn envoyés 1, reçus 0, accusés 1, ignorés 0, snapshots périmés 1") !=
          std::string::npos);
}

TEST("à terre 3.7 hôte : accusé hors attente ou de NetId ≠ 2 → ignoré, Warn") {
    Session s = host_with_clock();
    Output out = s.tick(1400, loaded_world(), respawn_in(1, 2, 3, 0, 42));
    CHECK(has_log_at(out, Level::Warn, "ignoré (aucune réapparition en attente d'accusé)"));
    s.tick(1410, loaded_world(), reliable(encode_r(proto::Death{proto::kGuestNetId})));
    out = s.tick(1420, loaded_world(), respawn_in(1, 2, 3, 0, 42, 7));
    CHECK(has_log_at(out, Level::Warn, "ignoré (NetId autre que 2)"));
    CHECK(s.respawn_waiting());
}

TEST("à terre 3.7 hôte : repli 5 s sans accusé → snapshots de l'invité repris, Warn") {
    Session s = host_with_clock();
    s.tick(1400, loaded_world(), reliable(encode_r(proto::Death{proto::kGuestNetId})));
    for (std::uint64_t t = 1500; t < 6400; t += 500) s.tick(t, loaded_world(), heartbeat_in());
    CHECK(s.respawn_waiting());
    const Output out = s.tick(6400, loaded_world(), heartbeat_in());
    CHECK(has_log_at(out, Level::Warn, "[sync] pas d'accusé de Respawn depuis 5 s : snapshots de l'invité repris"));
    CHECK(!s.respawn_waiting());
    // Entrées acceptées de nouveau (tick minimal 0).
    s.tick(6450, loaded_world(), snapshot_in(snapshot_bytes(105, proto::kGuestNetId, 700.0f)));
    s.tick(6500, loaded_world(), snapshot_in(snapshot_bytes(106, proto::kGuestNetId, 710.0f)));
    bool moved = false;
    for (std::uint64_t t = 6510; t < 6800; t += 20) moved |= !commands_of<MoveAvatar>(s.tick(t, loaded_world(), {})).empty();
    CHECK(moved);
}

TEST("à terre 3.7 hôte : retrait de l'avatar ou fin de session → réapparition oubliée") {
    Session s = host_with_clock();
    s.tick(1400, loaded_world(), reliable(encode_r(proto::Death{proto::kGuestNetId})));
    CHECK(s.respawn_waiting());
    s.tick(1410, loaded_world(), reliable(encode_r(proto::Despawn{proto::kGuestNetId})));
    CHECK(!s.respawn_waiting() && !s.respawn_pending());
    Session p = host_with_clock();
    World blind = loaded_world();
    blind.pose_ok = false;
    p.tick(1400, blind, reliable(encode_r(proto::Death{proto::kGuestNetId})));
    CHECK(p.respawn_pending());
    p.tick(1410, blind, reliable(encode_r(proto::SessionEnd{1})));
    CHECK(p.state() == State::Ended && !p.respawn_pending());
}

// --- Story 3.8 : attaques natives des répliques ---------------------------------------------------

namespace {

std::string attack_tail(const std::string& line) {
    const auto at = line.find("attaques natives : ");
    if (at == std::string::npos) return std::string();
    const auto end = line.find(" ; dialogues : ", at);  // story 4.1 : dernier bloc du bilan
    return line.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

Session host_with_npcs() {
    Session s = host_in_session();
    s.tick(1050, host_world(npcs_in_line(2)), {});  // 1000 = GUID 1, 1001 = GUID 2 (annoncés)
    return s;
}

}  // namespace

TEST("attaques 3.8 hôte : LocalNpcAttack d'un PNJ annoncé → NpcAttack{1000, 11, 2} fiable une fois, Info, compté") {
    Session s = host_with_npcs();
    const Output out = s.tick(1060, host_world({}, false), {LocalNpcAttack{1000, 11, proto::kGuestNetId}});
    const auto sent = sent_of<proto::NpcAttack>(out);
    CHECK(sent.size() == 1 && sent[0].net_id == 1000u && sent[0].move == 11u &&
          sent[0].target_net_id == proto::kGuestNetId);
    CHECK(has_log_at(out, Level::Info, "[sync] attaque du PNJ 1000 (mouvement 11, cible 2) : NpcAttack envoyé"));
    const Output next = s.tick(1070, host_world({}, false), {});
    CHECK(sent_of<proto::NpcAttack>(next).empty());
    CHECK_EQ(attack_tail(host_stats_line(s, 1100)),
             std::string("attaques natives : NpcAttack envoyés 1, appliqués 0, ignorés 0"));
}

TEST("attaques 3.8 hôte : cible inconnue (0) transmise telle quelle") {
    Session s = host_with_npcs();
    const auto sent = sent_of<proto::NpcAttack>(s.tick(1060, host_world({}, false), {LocalNpcAttack{1001, 4, 0}}));
    CHECK(sent.size() == 1 && sent[0].net_id == 1001u && sent[0].target_net_id == 0u);
}

TEST("attaques 3.8 hôte : PNJ non annoncé, mort, hors session → rien envoyé, Debug, compté ignoré") {
    Session s = host_with_npcs();
    Output out = s.tick(1060, host_world({}, false), {LocalNpcAttack{1500, 11, 2}});
    CHECK(sent_of<proto::NpcAttack>(out).empty());
    CHECK(has_log_at(out, Level::Debug, "[sync] attaque du PNJ 1500 (mouvement 11, cible 2) : ignorée (PNJ non annoncé)"));
    Session d = host_in_session();
    d.tick(1050, host_world({npc_at(1, 100.0f)}), {});
    d.tick(1100, host_world({npc_at(1, 100.0f, 0, false)}), {});  // Death{1000}
    out = d.tick(1110, host_world({}, false), {LocalNpcAttack{1000, 11, 2}});
    CHECK(sent_of<proto::NpcAttack>(out).empty());
    CHECK(has_log(out, "ignorée (PNJ mort)"));
    Session h(host_settings());
    h.tick(0, loaded_world(), {});
    out = h.tick(10, loaded_world(), {LocalNpcAttack{1000, 11, 2}});
    CHECK(commands_of<SendReliable>(out).empty());
    CHECK(has_log(out, "ignorée (hors session)"));
    CHECK_EQ(attack_tail(host_stats_line(s, 1100)),
             std::string("attaques natives : NpcAttack envoyés 0, appliqués 0, ignorés 1"));
}

TEST("attaques 3.8 hôte : native_attacks = 0 → rien envoyé ; NpcAttack reçu → Warn, ignoré") {
    Settings off = host_settings();
    off.native_attacks = false;
    Session s(off);
    s.tick(0, loaded_world(), {});
    s.tick(10, loaded_world(), control_from(kGuestA, encode(make_hello())));
    CHECK(s.state() == State::InSession);
    s.tick(50, host_world(npcs_in_line(1)), {});
    Output out = s.tick(60, host_world({}, false), {LocalNpcAttack{1000, 11, 2}});
    CHECK(sent_of<proto::NpcAttack>(out).empty());
    CHECK(has_log(out, "ignorée (native_attacks = 0)"));
    Session h = host_with_npcs();
    out = h.tick(1060, host_world({}, false), reliable(encode_r(proto::NpcAttack{1000, 11, 2})));
    CHECK(commands_of<ApplyNpcAttack>(out).empty());
    CHECK(has_log_at(out, Level::Warn, "[sync] NpcAttack PNJ 1000 (mouvement 11, cible 2) ignoré : l'hôte fait autorité"));
    CHECK(h.state() == State::InSession);
}

TEST("attaques 3.8 invité : NpcAttack d'une réplique vivante → ApplyNpcAttack à l'instant d'affichage, après MoveNpc") {
    Session g = guest_with_npcs();  // dernier snapshot : tick 3, affiché à now − 350 (comme un Death)
    Output out = g.tick(400, loaded_world(), reliable(encode_r(proto::NpcAttack{1000, 11, proto::kGuestNetId})));
    CHECK(commands_of<ApplyNpcAttack>(out).empty());
    CHECK(commands_of<ApplyNpcAttack>(g.tick(499, loaded_world(), {})).empty());
    out = g.tick(500, loaded_world(), {});
    const auto apply = commands_of<ApplyNpcAttack>(out);
    CHECK(apply.size() == 1 && apply[0]->net_id == 1000u && apply[0]->move == 11u &&
          apply[0]->target == proto::kGuestNetId);
    CHECK(index_for<MoveNpc>(out, 1000) < index_for<ApplyNpcAttack>(out, 1000));
    CHECK(commands_of<ApplyNpcAttack>(g.tick(550, loaded_world(), {})).empty());  // une fois
    CHECK_EQ(attack_tail(guest_stats_line(g, 600)),
             std::string("attaques natives : NpcAttack envoyés 0, appliqués 1, ignorés 0"));
}

TEST("attaques 3.8 invité : horloge pas prête → ApplyNpcAttack aussitôt") {
    Session g = guest_in_session();
    g.tick(150, loaded_world(), reliable(encode_r(npc_spawn(1000))));
    const Output out = g.tick(160, loaded_world(), reliable(encode_r(proto::NpcAttack{1000, 5, 1})));
    const auto apply = commands_of<ApplyNpcAttack>(out);
    CHECK(apply.size() == 1 && apply[0]->net_id == 1000u && apply[0]->move == 5u && apply[0]->target == 1u);
}

TEST("attaques 3.8 invité : NetId inconnu, réplique morte ou en attente de mort → ignoré, compté") {
    Session g = guest_with_npcs();
    Output out = g.tick(400, loaded_world(), reliable(encode_r(proto::NpcAttack{1500, 11, 2})));
    CHECK(has_log_at(out, Level::Debug, "[sync] NpcAttack PNJ 1500 (mouvement 11, cible 2) ignoré (NetId non annoncé)"));
    g.tick(410, loaded_world(), reliable(encode_r(proto::Death{1001})));  // en attente
    out = g.tick(420, loaded_world(), reliable(encode_r(proto::NpcAttack{1001, 11, 2})));
    CHECK(has_log(out, "ignoré (réplique morte)"));
    for (std::uint64_t t = 430; t <= 600; t += 50) CHECK(commands_of<ApplyNpcAttack>(g.tick(t, loaded_world(), {})).empty());
    CHECK_EQ(attack_tail(guest_stats_line(g, 650)),
             std::string("attaques natives : NpcAttack envoyés 0, appliqués 0, ignorés 2"));
}

TEST("attaques 3.8 invité : attaque en attente puis Death → jetée, comptée ignorée, NpcDeath seul") {
    Session g = guest_with_npcs();
    g.tick(400, loaded_world(), reliable(encode_r(proto::NpcAttack{1000, 11, 2})));
    g.tick(410, loaded_world(), reliable(encode_r(proto::Death{1000})));
    bool died = false;
    for (std::uint64_t t = 420; t <= 600; t += 20) {
        const Output out = g.tick(t, loaded_world(), {});
        CHECK(count_for<ApplyNpcAttack>(out, 1000) == 0u);
        died |= count_for<NpcDeath>(out, 1000) == 1u;
    }
    CHECK(died);
    CHECK_EQ(attack_tail(guest_stats_line(g, 650)),
             std::string("attaques natives : NpcAttack envoyés 0, appliqués 0, ignorés 1"));
}

TEST("attaques 3.8 invité : native_attacks = 0 → NpcAttack ignoré, compté ; file de 8 au plus par réplique") {
    Settings off = guest_settings();
    off.native_attacks = false;
    Session g(off);
    g.tick(0, loaded_world(), {});
    g.tick(100, loaded_world(), control_from(kHost, encode(welcome_at(1, 2, 3, 0))));
    g.tick(150, loaded_world(), reliable(encode_r(npc_spawn(1000))));
    Output out = g.tick(160, loaded_world(), reliable(encode_r(proto::NpcAttack{1000, 11, 2})));
    CHECK(commands_of<ApplyNpcAttack>(out).empty());
    CHECK(has_log(out, "ignoré (native_attacks = 0)"));
    CHECK(has_log_at(out, Level::Info, "[sync] NpcAttack reçus de l'hôte mais attaques natives inactives ici"));
    out = g.tick(170, loaded_world(), reliable(encode_r(proto::NpcAttack{1000, 11, 2})));
    CHECK(!has_log(out, "attaques natives inactives ici"));  // une fois par session
    Session f = guest_with_npcs();
    for (int i = 0; i < 9; ++i) f.tick(400 + i, loaded_world(), reliable(encode_r(proto::NpcAttack{1000, 11, 2})));
    out = f.tick(500, loaded_world(), {});
    CHECK_EQ(count_for<ApplyNpcAttack>(out, 1000), kMaxPendingAttacks);
    CHECK_EQ(attack_tail(guest_stats_line(f, 600)),
             std::string("attaques natives : NpcAttack envoyés 0, appliqués 8, ignorés 1"));
}

TEST("attaques 3.8 invité : LocalNpcAttack d'un PNJ chez l'invité → rien envoyé") {
    Session g = guest_with_npcs();
    const Output out = g.tick(400, loaded_world(), {LocalNpcAttack{1000, 11, 2}});
    CHECK(sent_of<proto::NpcAttack>(out).empty());
    CHECK(has_log(out, "ignorée (pas hôte)"));
}

// --- Story 3.8b : attaques natives de l'avatar invité chez l'hôte --------------------------------

TEST("attaques 3.8b invité : LocalNpcAttack{2} de son héros → NpcAttack{2, 11, 1004} fiable une fois, Debug, compté") {
    Session g = guest_with_npcs();  // reste en session jusqu'au bilan
    const Output out = g.tick(400, loaded_world(), {LocalNpcAttack{proto::kGuestNetId, 11, 1004}});
    const auto sent = sent_of<proto::NpcAttack>(out);
    CHECK(sent.size() == 1 && sent[0].net_id == proto::kGuestNetId && sent[0].move == 11u &&
          sent[0].target_net_id == 1004u);
    CHECK(has_log_at(out, Level::Debug, "[sync] attaque du héros invité (mouvement 11, cible 1004) : NpcAttack envoyé"));
    CHECK(sent_of<proto::NpcAttack>(g.tick(410, loaded_world(), {})).empty());
    CHECK_EQ(attack_tail(guest_stats_line(g, 600)),
             std::string("attaques natives : NpcAttack envoyés 1, appliqués 0, ignorés 0"));
}

TEST("attaques 3.8b invité : native_attacks = 0, héros à terre, hors session → rien envoyé, compté ignoré") {
    Settings off = guest_settings();
    off.native_attacks = false;
    Session o(off);
    o.tick(0, loaded_world(), {});
    o.tick(100, loaded_world(), control_from(kHost, encode(welcome_at(1, 2, 3, 0))));
    Output out = o.tick(400, loaded_world(), {LocalNpcAttack{proto::kGuestNetId, 11, 1}});
    CHECK(sent_of<proto::NpcAttack>(out).empty());
    CHECK(has_log(out, "ignorée (native_attacks = 0)"));
    Session d = guest_in_session();
    d.tick(400, loaded_world(), {LocalDown{kDownCauseHitOnGuest}});
    out = d.tick(410, loaded_world(), {LocalNpcAttack{proto::kGuestNetId, 11, 1}});
    CHECK(sent_of<proto::NpcAttack>(out).empty());
    CHECK(has_log(out, "ignorée (héros à terre)"));
    Session h(guest_settings());
    h.tick(0, loaded_world(), {});
    out = h.tick(10, loaded_world(), {LocalNpcAttack{proto::kGuestNetId, 11, 1}});
    CHECK(commands_of<SendReliable>(out).empty());
    CHECK(has_log(out, "ignorée (hors session)"));
}

TEST("attaques 3.8b invité : NpcAttack{2} reçu → ignoré (NetId non annoncé), jamais appliqué") {
    Session g = guest_with_npcs();
    const Output out = g.tick(400, loaded_world(), reliable(encode_r(proto::NpcAttack{proto::kGuestNetId, 11, 1000})));
    CHECK(has_log(out, "ignoré (NetId non annoncé)"));
    for (std::uint64_t t = 410; t <= 600; t += 50) {
        const Output o = g.tick(t, loaded_world(), {});
        CHECK(commands_of<ApplyNpcAttack>(o).empty() && commands_of<ApplyAvatarAttack>(o).empty());
    }
}

TEST("attaques 3.8b hôte : NpcAttack{2} → ApplyAvatarAttack à l'instant d'affichage, après MoveAvatar, une fois") {
    Session s = host_with_clock();  // dernier snapshot : 150 ms ; affiché = now − 1350
    Output out = s.tick(1400, loaded_world(), reliable(encode_r(proto::NpcAttack{proto::kGuestNetId, 4, 1004})));
    CHECK(commands_of<ApplyAvatarAttack>(out).empty());  // affiché 50 < 150
    CHECK(commands_of<ApplyAvatarAttack>(s.tick(1499, loaded_world(), {})).empty());
    out = s.tick(1500, loaded_world(), {});
    const auto apply = commands_of<ApplyAvatarAttack>(out);
    CHECK(apply.size() == 1 && apply[0]->move == 4u && apply[0]->target == 1004u);
    CHECK(index_of<MoveAvatar>(out) < index_of<ApplyAvatarAttack>(out));
    CHECK(commands_of<ApplyAvatarAttack>(s.tick(1550, loaded_world(), {})).empty());
    CHECK_EQ(attack_tail(host_stats_line(s, 1600)),
             std::string("attaques natives : NpcAttack envoyés 0, appliqués 1, ignorés 0"));
}

TEST("attaques 3.8b hôte : horloge pas prête → ApplyAvatarAttack aussitôt, cible 1 transmise") {
    Session s = host_with_avatar();
    const Output out = s.tick(1110, loaded_world(), reliable(encode_r(proto::NpcAttack{proto::kGuestNetId, 5, 1})));
    const auto apply = commands_of<ApplyAvatarAttack>(out);
    CHECK(apply.size() == 1 && apply[0]->move == 5u && apply[0]->target == 1u);
}

TEST("attaques 3.8b hôte : NetId autre que 2 → Warn, ignoré ; sans avatar → ignoré, compté") {
    Session s = host_with_avatar();
    Output out = s.tick(1110, loaded_world(), reliable(encode_r(proto::NpcAttack{1004, 4, 1000})));
    CHECK(commands_of<ApplyAvatarAttack>(out).empty() && commands_of<ApplyNpcAttack>(out).empty());
    CHECK(has_log_at(out, Level::Warn, "[sync] NpcAttack PNJ 1004 (mouvement 4, cible 1000) ignoré : l'hôte fait autorité"));
    out = s.tick(1120, loaded_world(), reliable(encode_r(proto::NpcAttack{proto::kHostNetId, 4, 1000})));
    CHECK(has_log_at(out, Level::Warn, "ignoré : l'hôte fait autorité"));
    Session n = host_in_session();
    out = n.tick(1060, host_world({}, false), reliable(encode_r(proto::NpcAttack{proto::kGuestNetId, 4, 1000})));
    CHECK(commands_of<ApplyAvatarAttack>(out).empty());
    CHECK(has_log_at(out, Level::Debug, "[sync] NpcAttack de l'avatar invité (mouvement 4, cible 1000) ignoré (pas d'avatar)"));
    CHECK_EQ(attack_tail(host_stats_line(n, 1100)),
             std::string("attaques natives : NpcAttack envoyés 0, appliqués 0, ignorés 1"));
}

TEST("attaques 3.8b hôte : native_attacks = 0 → NpcAttack{2} ignoré, Info une fois par session") {
    Settings off = host_settings();
    off.native_attacks = false;
    Session s(off);
    s.tick(0, loaded_world(), {});
    s.tick(1000, loaded_world(), control_from(kGuestA, encode(make_hello())));
    s.tick(1100, loaded_world(), reliable(encode_r(spawn_of(proto::kGuestNetId))));
    CHECK(s.avatar_present());
    Output out = s.tick(1110, loaded_world(), reliable(encode_r(proto::NpcAttack{proto::kGuestNetId, 4, 1000})));
    CHECK(commands_of<ApplyAvatarAttack>(out).empty());
    CHECK(has_log(out, "ignoré (native_attacks = 0)"));
    CHECK(has_log_at(out, Level::Info, "[sync] NpcAttack reçus de l'invité mais attaques natives inactives ici"));
    out = s.tick(1120, loaded_world(), reliable(encode_r(proto::NpcAttack{proto::kGuestNetId, 4, 1000})));
    CHECK(!has_log(out, "attaques natives inactives ici"));
}

TEST("attaques 3.8b hôte : file de 8 au plus ; retrait de l'avatar → file vidée, comptée ignorée") {
    Session s = host_with_clock();
    for (int i = 0; i < 9; ++i) {
        s.tick(1400 + i, loaded_world(), reliable(encode_r(proto::NpcAttack{proto::kGuestNetId, 4, 1000})));
    }
    Output out = s.tick(1500, loaded_world(), {});
    CHECK_EQ(commands_of<ApplyAvatarAttack>(out).size(), kMaxPendingAttacks);
    Session r = host_with_clock();
    r.tick(1400, loaded_world(), reliable(encode_r(proto::NpcAttack{proto::kGuestNetId, 4, 1000})));
    r.tick(1410, loaded_world(), reliable(encode_r(proto::Despawn{proto::kGuestNetId})));
    CHECK(!r.avatar_present());
    for (std::uint64_t t = 1420; t <= 1600; t += 60) CHECK(commands_of<ApplyAvatarAttack>(r.tick(t, loaded_world(), {})).empty());
    CHECK_EQ(attack_tail(host_stats_line(r, 1700)),
             std::string("attaques natives : NpcAttack envoyés 0, appliqués 0, ignorés 1"));
}

TEST("attaques 3.8b hôte : invité à terre → file vidée, NpcAttack{2} refusé jusqu'à l'accusé, comptés ignorés") {
    Session s = host_with_clock();  // affiché = now − 1350
    s.tick(1400, loaded_world(), reliable(encode_r(proto::NpcAttack{proto::kGuestNetId, 4, 1000})));  // due à 1500
    s.tick(1410, loaded_world(), reliable(encode_r(proto::Death{proto::kGuestNetId})));
    CHECK(s.respawn_waiting());
    Output out = s.tick(1420, loaded_world(), reliable(encode_r(proto::NpcAttack{proto::kGuestNetId, 4, 1000})));
    CHECK(has_log_at(out, Level::Debug, "ignoré (invité à terre)"));
    for (std::uint64_t t = 1430; t <= 1700; t += 30) CHECK(commands_of<ApplyAvatarAttack>(s.tick(t, loaded_world(), {})).empty());
    CHECK_EQ(attack_tail(host_stats_line(s, 1800)),
             std::string("attaques natives : NpcAttack envoyés 0, appliqués 0, ignorés 2"));
}

TEST("attaques 3.8b hôte : après l'accusé de Respawn, NpcAttack{2} de nouveau accepté et appliqué") {
    Session s = host_with_clock();
    s.tick(1400, loaded_world(), reliable(encode_r(proto::Death{proto::kGuestNetId})));
    s.tick(1500, loaded_world(), respawn_in(1, 2, 3, 0, 42));
    CHECK(!s.respawn_waiting());
    // Interpolation remise à zéro par l'accusé : aucun snapshot depuis, attaque due aussitôt.
    const Output out = s.tick(1510, loaded_world(), reliable(encode_r(proto::NpcAttack{proto::kGuestNetId, 4, 1000})));
    const auto apply = commands_of<ApplyAvatarAttack>(out);
    CHECK(apply.size() == 1 && apply[0]->move == 4u && apply[0]->target == 1000u);
}

TEST("attaques 3.8b hôte : cible = PNJ déjà mort → ApplyAvatarAttack cible 0 (cibles effacées)") {
    Session s = host_in_session();
    s.tick(1050, host_world({npc_at(1, 100.0f)}), {});
    s.tick(1100, host_world({npc_at(1, 100.0f, 0, false)}), {});  // Death{1000}
    s.tick(1110, host_world({}, false), reliable(encode_r(spawn_of(proto::kGuestNetId))));
    CHECK(s.avatar_present());
    const Output out =
        s.tick(1120, host_world({}, false), reliable(encode_r(proto::NpcAttack{proto::kGuestNetId, 4, 1000})));
    const auto apply = commands_of<ApplyAvatarAttack>(out);
    CHECK(apply.size() == 1 && apply[0]->move == 4u && apply[0]->target == 0u);
}

TEST("attaques 3.8b hôte : Weapon{2} et NpcAttack{2} dus au même tick → AvatarWeapon avant ApplyAvatarAttack") {
    Session s = host_with_clock();
    s.tick(1400, loaded_world(),
           reliable2(proto::Weapon{proto::kGuestNetId, "It_1H_Sword"}, proto::NpcAttack{proto::kGuestNetId, 4, 1000}));
    const Output out = s.tick(1500, loaded_world(), {});
    CHECK_EQ(commands_of<AvatarWeapon>(out).size(), 1u);
    CHECK_EQ(commands_of<ApplyAvatarAttack>(out).size(), 1u);
    CHECK(index_of<AvatarWeapon>(out) < index_of<ApplyAvatarAttack>(out));
}

// --- Story 4.1 : dialogue de l'hôte vu par l'invité ----------------------------------------------

namespace {

std::string dialog_tail(const std::string& line) {
    const auto at = line.find("dialogues : ");
    return at == std::string::npos ? std::string() : line.substr(at);
}

const char* const kSample = "INFO_Sara_2_Sara_DE_%_Sara.wav";

LocalDialogLine npc_line(std::uint8_t g, std::uint8_t type = 0, const std::string& sample = kSample) {
    LocalDialogLine l;
    l.guid = guid_of(g);
    l.output_type = type;
    l.sample = sample;
    return l;
}

LocalDialogLine hero_line(const std::string& sample = kSample) {
    LocalDialogLine l;
    l.hero = true;
    l.output_type = 3;
    l.sample = sample;
    return l;
}

}  // namespace

TEST("dialogues 4.1 hôte : réplique du PNJ annoncé → DialogLine{1001, 0, échantillon} une fois, Debug, compté") {
    Session s = host_with_npcs();  // 1000 = GUID 1, 1001 = GUID 2
    const Output out = s.tick(1060, host_world({}, false), {npc_line(2)});
    const auto sent = sent_of<proto::DialogLine>(out);
    CHECK(sent.size() == 1 && sent[0].speaker_net_id == 1001u && sent[0].output_type == 0 && sent[0].sample == kSample);
    CHECK(has_log_at(out, Level::Debug,
                     "[sync] réplique de dialogue du PNJ 1001 (type 0, « INFO_Sara_2_Sara_DE_%_Sara.wav ») : DialogLine envoyé"));
    CHECK(sent_of<proto::DialogLine>(s.tick(1070, host_world({}, false), {})).empty());
    CHECK_EQ(dialog_tail(host_stats_line(s, 1100)),
             std::string("dialogues : DialogLine envoyés 1, reçus 0, rejoués 0, ignorés 0 ; DialogState envoyés 0, reçus 0"));
}

TEST("dialogues 4.1 hôte : réplique du héros → DialogLine{1, 3, …}") {
    Session s = host_in_session();
    const auto sent = sent_of<proto::DialogLine>(s.tick(1060, host_world({}, false), {hero_line()}));
    CHECK(sent.size() == 1 && sent[0].speaker_net_id == proto::kHostNetId && sent[0].output_type == 3);
}

TEST("dialogues 4.1 hôte : PNJ non annoncé, mort, échantillon invalide, hors session → rien, compté ignoré") {
    Session s = host_with_npcs();
    Output out = s.tick(1060, host_world({}, false), {npc_line(9)});
    CHECK(sent_of<proto::DialogLine>(out).empty());
    CHECK(has_log_at(out, Level::Debug, "ignorée (PNJ non annoncé)"));
    out = s.tick(1070, host_world({}, false), {npc_line(1, 0, "../x.wav"), npc_line(1, 4)});
    CHECK(sent_of<proto::DialogLine>(out).empty());
    CHECK(has_log(out, "ignorée (échantillon ou type invalide)"));
    CHECK_EQ(dialog_tail(host_stats_line(s, 1100)),
             std::string("dialogues : DialogLine envoyés 0, reçus 0, rejoués 0, ignorés 3 ; DialogState envoyés 0, reçus 0"));
    Session d = host_in_session();
    d.tick(1050, host_world({npc_at(1, 100.0f)}), {});
    d.tick(1100, host_world({npc_at(1, 100.0f, 0, false)}), {});  // Death{1000}
    out = d.tick(1110, host_world({}, false), {npc_line(1)});
    CHECK(sent_of<proto::DialogLine>(out).empty());
    CHECK(has_log_at(out, Level::Debug, "ignorée (PNJ mort)"));
    CHECK(!has_log(out, "PNJ non annoncé"));
    Session h(host_settings());
    h.tick(0, loaded_world(), {});
    out = h.tick(10, loaded_world(), {hero_line()});
    CHECK(commands_of<SendReliable>(out).empty());
    CHECK(has_log_at(out, Level::Debug, "ignorée (hors session)"));
}

TEST("dialogues 4.1 hôte : dialogues = 0 → rien envoyé ; DialogLine reçu → Warn, ignoré, session intacte") {
    Settings off = host_settings();
    off.dialogues = false;
    Session s(off);
    s.tick(0, loaded_world(), {});
    s.tick(10, loaded_world(), control_from(kGuestA, encode(make_hello())));
    CHECK(s.state() == State::InSession);
    Output out = s.tick(60, loaded_world(), {hero_line()});
    CHECK(sent_of<proto::DialogLine>(out).empty());
    CHECK(has_log(out, "ignorée (dialogues = 0)"));
    // Story 4.2 : l'hôte rejoue les répliques de l'invité, jamais le locuteur 1 (son propre héros).
    Session h = host_with_npcs();
    out = h.tick(1060, host_world({}, false), reliable(encode_r(proto::DialogLine{proto::kHostNetId, 3, kSample})));
    CHECK(commands_of<ApplyDialogLine>(out).empty());
    CHECK(has_log_at(out, Level::Warn, "ignoré : locuteur 1 = son propre héros"));
    out = h.tick(1070, host_world({}, false), reliable(encode_r(proto::DialogLine{proto::kHostNetId, 3, kSample})));
    CHECK(has_log_at(out, Level::Debug, "ignoré : locuteur 1 = son propre héros"));  // Warn une fois
    CHECK(!has_log_at(out, Level::Warn, "son propre héros"));
    CHECK(h.state() == State::InSession);
}

TEST("dialogues 4.1 invité : DialogLine d'une réplique vivante → ApplyDialogLine aussitôt, compté reçu") {
    Session g = guest_with_npcs();
    const Output out = g.tick(400, loaded_world(), reliable(encode_r(proto::DialogLine{1001, 2, kSample})));
    const auto apply = commands_of<ApplyDialogLine>(out);
    CHECK(apply.size() == 1 && apply[0]->net_id == 1001u && apply[0]->output_type == 2 && apply[0]->sample == kSample);
    // Bilan de sync/dialogue : un rejoué, un ignoré (trop loin, par exemple).
    g.tick(410, loaded_world(), {LocalDialogReplayed{1, 1}});
    CHECK_EQ(dialog_tail(guest_stats_line(g, 600)),
             std::string("dialogues : DialogLine envoyés 0, reçus 1, rejoués 1, ignorés 1 ; DialogState envoyés 0, reçus 0"));
}

TEST("dialogues 4.1 invité : locuteur 1 → avatar de l'hôte présent seulement") {
    Session g = guest_with_npcs();  // pas d'avatar de l'hôte
    Output out = g.tick(400, loaded_world(), reliable(encode_r(proto::DialogLine{proto::kHostNetId, 3, kSample})));
    CHECK(commands_of<ApplyDialogLine>(out).empty());
    CHECK(has_log_at(out, Level::Debug, "ignoré (pas d'avatar de l'hôte)"));
    Session a = guest_with_clock();
    out = a.tick(1400, loaded_world(), reliable(encode_r(proto::DialogLine{proto::kHostNetId, 3, kSample})));
    const auto apply = commands_of<ApplyDialogLine>(out);
    CHECK(apply.size() == 1 && apply[0]->net_id == proto::kHostNetId && apply[0]->output_type == 3);
}

TEST("dialogues 4.1 invité : avatar de l'hôte mort (Death{1} en attente puis appliqué) → pas d'ApplyDialogLine") {
    Session g = guest_with_clock();  // affiché = now − 1350
    g.tick(1400, loaded_world(), reliable(encode_r(proto::Death{proto::kHostNetId})));  // en attente
    Output out = g.tick(1410, loaded_world(), reliable(encode_r(proto::DialogLine{proto::kHostNetId, 3, kSample})));
    CHECK(commands_of<ApplyDialogLine>(out).empty());
    CHECK(has_log_at(out, Level::Debug, "ignoré (avatar de l'hôte mort)"));
    CHECK_EQ(commands_of<AvatarDeath>(g.tick(1500, loaded_world(), {})).size(), 1u);  // appliqué
    out = g.tick(1510, loaded_world(), reliable(encode_r(proto::DialogLine{proto::kHostNetId, 3, kSample})));
    CHECK(commands_of<ApplyDialogLine>(out).empty());
    CHECK(has_log_at(out, Level::Debug, "ignoré (avatar de l'hôte mort)"));
}

TEST("dialogues 4.1 invité : NetId non annoncé, réplique morte → ignoré, compté ; dialogues = 0 → Info une fois") {
    Session g = guest_with_npcs();
    Output out = g.tick(400, loaded_world(), reliable(encode_r(proto::DialogLine{1500, 0, kSample})));
    CHECK(commands_of<ApplyDialogLine>(out).empty());
    CHECK(has_log(out, "ignoré (NetId non annoncé)"));
    g.tick(410, loaded_world(), reliable(encode_r(proto::Death{1001})));
    out = g.tick(420, loaded_world(), reliable(encode_r(proto::DialogLine{1001, 0, kSample})));
    CHECK(commands_of<ApplyDialogLine>(out).empty());
    CHECK(has_log(out, "ignoré (réplique morte)"));
    CHECK_EQ(dialog_tail(guest_stats_line(g, 600)),
             std::string("dialogues : DialogLine envoyés 0, reçus 0, rejoués 0, ignorés 2 ; DialogState envoyés 0, reçus 0"));
    Settings off = guest_settings();
    off.dialogues = false;
    Session f(off);
    f.tick(0, loaded_world(), {});
    f.tick(100, loaded_world(), control_from(kHost, encode(welcome_at(1, 2, 3, 0))));
    f.tick(150, loaded_world(), reliable(encode_r(npc_spawn(1000))));
    out = f.tick(160, loaded_world(), reliable(encode_r(proto::DialogLine{1000, 0, kSample})));
    CHECK(commands_of<ApplyDialogLine>(out).empty());
    CHECK(has_log(out, "ignoré (dialogues = 0)"));
    CHECK(has_log_at(out, Level::Info, "[sync] DialogLine reçus de l'hôte mais dialogues inactifs ici"));
    out = f.tick(170, loaded_world(), reliable(encode_r(proto::DialogLine{1000, 0, kSample})));
    CHECK(!has_log(out, "dialogues inactifs ici"));
}

// --- Story 4.2 : dialogue de l'invité vu par l'hôte ----------------------------------------------

namespace {

LocalDialogLine guest_npc_line(std::uint32_t net_id, std::uint8_t type = 0) {
    LocalDialogLine l;
    l.net_id = net_id;
    l.output_type = type;
    l.sample = kSample;
    return l;
}

template <typename T>
std::size_t count_of(const Output& out) {
    return commands_of<T>(out).size();
}

}  // namespace

TEST("dialogues 4.2 invité : début avec une réplique → FreeReplica{1001, libérée} puis DialogState{1001, 1}, Info") {
    Session g = guest_with_npcs();
    const Output out = g.tick(400, loaded_world(), {LocalDialogState{1001, true}});
    const auto free = commands_of<FreeReplica>(out);
    CHECK(free.size() == 1 && free[0]->net_id == 1001u && free[0]->freed);
    const auto sent = sent_of<proto::DialogState>(out);
    CHECK(sent.size() == 1 && sent[0].npc_net_id == 1001u && sent[0].active == 1);
    CHECK(index_of<FreeReplica>(out) < index_of<SendReliable>(out));
    CHECK(has_log_at(out, Level::Info, "[sync] dialogue de l'invité avec le PNJ 1001 : début : réplique libérée"));
    CHECK_EQ(g.freed_replica(), 1001u);
    // Doublon : ignoré.
    CHECK(sent_of<proto::DialogState>(g.tick(410, loaded_world(), {LocalDialogState{1001, true}})).empty());
}

TEST("dialogues 4.2 invité : répliques du héros (2) et du partenaire (1001) envoyées ; autre PNJ ou hors dialogue : non") {
    Session g = guest_with_npcs();
    // Héros : toujours envoyé (locuteur 2), même sans dialogue avec une réplique.
    auto sent = sent_of<proto::DialogLine>(g.tick(400, loaded_world(), {hero_line()}));
    CHECK(sent.size() == 1 && sent[0].speaker_net_id == proto::kGuestNetId && sent[0].output_type == 3);
    // Partenaire avant le DialogState : ignoré.
    Output out = g.tick(410, loaded_world(), {guest_npc_line(1001)});
    CHECK(sent_of<proto::DialogLine>(out).empty());
    CHECK(has_log_at(out, Level::Debug, "ignorée (PNJ hors du dialogue en cours)"));
    out = g.tick(420, loaded_world(), {LocalDialogState{1001, true}, guest_npc_line(1001), guest_npc_line(1000),
                                       guest_npc_line(0)});
    sent = sent_of<proto::DialogLine>(out);
    CHECK(sent.size() == 1 && sent[0].speaker_net_id == 1001u && sent[0].output_type == 0 && sent[0].sample == kSample);
    CHECK(has_log(out, "ignorée (partenaire qui n'est pas une réplique)"));
    CHECK_EQ(dialog_tail(guest_stats_line(g, 600)),
             std::string("dialogues : DialogLine envoyés 2, reçus 0, rejoués 0, ignorés 3 ; DialogState envoyés 1, reçus 0"));
}

TEST("dialogues 4.2 invité : fin → FreeReplica{1001, figée}, DialogState{1001, 0}, PV de l'hôte réécrits") {
    Session g = guest_with_npcs();  // affiché = now − 350 ; snapshots 0 à 3 (PV 100)
    g.tick(400, loaded_world(), {LocalDialogState{1001, true}});
    // Pendant le dialogue : ni pose, ni PV, ni geste, ni attaque de l'hôte pour 1001 (1000 continue).
    g.tick(410, loaded_world(), snapshot_in(npc_snapshot(4, {npc_entity(1000, 5.0f, 90), npc_entity(1001, 5.0f, 60)})));
    g.tick(420, loaded_world(),
           reliable2(host_npc_motion(1001, 4, 200), proto::NpcAttack{1001, 11, proto::kGuestNetId}));
    bool other_moved = false, other_hp = false;
    for (std::uint64_t t = 430; t <= 700; t += 30) {
        const Output out = g.tick(t, loaded_world(), {});
        for (const auto* m : commands_of<MoveNpc>(out)) {
            CHECK(m->net_id != 1001u);
            other_moved = other_moved || m->net_id == 1000u;
        }
        for (const auto* h : commands_of<SetNpcHp>(out)) {
            CHECK(h->net_id != 1001u);
            other_hp = other_hp || (h->net_id == 1000u && h->hp == 90);
        }
        for (const auto* p : commands_of<PlayNpcMotion>(out)) CHECK(p->net_id != 1001u);
        CHECK(commands_of<ApplyNpcAttack>(out).empty());
    }
    CHECK(other_moved && other_hp);  // l'autre réplique suit toujours l'hôte
    Output out = g.tick(710, loaded_world(), {LocalDialogState{1001, false}});
    const auto free = commands_of<FreeReplica>(out);
    CHECK(free.size() == 1 && free[0]->net_id == 1001u && !free[0]->freed);
    const auto sent = sent_of<proto::DialogState>(out);
    CHECK(sent.size() == 1 && sent[0].npc_net_id == 1001u && sent[0].active == 0);
    CHECK_EQ(g.freed_replica(), 0u);
    // Re-figée : la pose et les PV de l'hôte reprennent.
    const auto hp = commands_of<SetNpcHp>(out);
    CHECK(hp.size() == 1 && hp[0]->net_id == 1001u && hp[0]->hp == 60);
    bool moved = false;
    for (const auto* m : commands_of<MoveNpc>(out)) moved = moved || m->net_id == 1001u;
    CHECK(moved);
    // Dernier geste de l'hôte gardé pendant le dialogue, rejoué au re-gel (après FreeReplica).
    const auto replayed = commands_of<PlayNpcMotion>(out);
    CHECK(replayed.size() == 1 && replayed[0]->net_id == 1001u && replayed[0]->name == "Wolf_Attack");
    CHECK(index_of<FreeReplica>(out) < index_of<PlayNpcMotion>(out));
    // Fin d'un dialogue qui n'est pas en cours : ignorée.
    CHECK(sent_of<proto::DialogState>(g.tick(720, loaded_world(), {LocalDialogState{1001, false}})).empty());
}

TEST("dialogues 4.2 invité : nouveau partenaire → l'ancien re-figé (DialogState{…, 0}) avant le nouveau") {
    Session g = guest_with_npcs();
    g.tick(400, loaded_world(), {LocalDialogState{1001, true}});
    const Output out = g.tick(410, loaded_world(), {LocalDialogState{1000, true}});
    const auto sent = sent_of<proto::DialogState>(out);
    CHECK(sent.size() == 2 && sent[0].npc_net_id == 1001u && sent[0].active == 0 && sent[1].npc_net_id == 1000u &&
          sent[1].active == 1);
    const auto free = commands_of<FreeReplica>(out);
    CHECK(free.size() == 2 && free[0]->net_id == 1001u && !free[0]->freed && free[1]->net_id == 1000u && free[1]->freed);
    CHECK_EQ(g.freed_replica(), 1000u);
}

TEST("dialogues 4.2 invité : réplique absente, morte, dialogues = 0, hors session → ignoré, rien libéré") {
    Session g = guest_with_npcs();
    Output out = g.tick(400, loaded_world(), {LocalDialogState{1500, true}});
    CHECK(count_of<FreeReplica>(out) == 0 && sent_of<proto::DialogState>(out).empty());
    CHECK(has_log(out, "ignoré (réplique absente)"));
    g.tick(410, loaded_world(), reliable(encode_r(proto::Death{1001})));
    // Death reçu, pas encore appliqué (death_pending : affiché 70 < 150) : refusé aussi.
    out = g.tick(420, loaded_world(), {LocalDialogState{1001, true}});
    CHECK(count_of<FreeReplica>(out) == 0 && count_of<NpcDeath>(out) == 0);
    CHECK(has_log(out, "ignoré (réplique morte)"));
    CHECK_EQ(count_of<NpcDeath>(g.tick(520, loaded_world(), {})), 1u);  // appliqué
    out = g.tick(530, loaded_world(), {LocalDialogState{1001, true}});
    CHECK(count_of<FreeReplica>(out) == 0);
    CHECK(has_log(out, "ignoré (réplique morte)"));
    Settings off = guest_settings();
    off.dialogues = false;
    Session f(off);
    f.tick(0, loaded_world(), {});
    f.tick(100, loaded_world(), control_from(kHost, encode(welcome_at(1, 2, 3, 0))));
    f.tick(150, loaded_world(), reliable(encode_r(npc_spawn(1000))));
    out = f.tick(160, loaded_world(), {LocalDialogState{1000, true}, hero_line()});
    CHECK(count_of<FreeReplica>(out) == 0 && commands_of<SendReliable>(out).empty());
    Session i(guest_settings());
    i.tick(0, loaded_world(), {});
    out = i.tick(10, loaded_world(), {LocalDialogState{1000, true}});
    CHECK(count_of<FreeReplica>(out) == 0);
    CHECK(has_log_at(out, Level::Debug, "ignoré (hors session)"));
}

TEST("dialogues 4.2 invité : Death de la réplique libérée → re-figée avant NpcDeath ; Despawn → ReleaseNpc seul") {
    Session g = guest_with_npcs();
    g.tick(400, loaded_world(), {LocalDialogState{1001, true}});
    g.tick(410, loaded_world(), reliable(encode_r(proto::Death{1001})));
    Output out;
    for (std::uint64_t t = 420; t <= 700 && count_of<NpcDeath>(out) == 0; t += 30) out = g.tick(t, loaded_world(), {});
    CHECK_EQ(count_of<NpcDeath>(out), 1u);
    const auto free = commands_of<FreeReplica>(out);
    CHECK(free.size() == 1 && free[0]->net_id == 1001u && !free[0]->freed);
    CHECK(index_of<FreeReplica>(out) < index_of<NpcDeath>(out));
    CHECK_EQ(g.freed_replica(), 0u);
    Session d = guest_with_npcs();
    d.tick(400, loaded_world(), {LocalDialogState{1000, true}});
    out = d.tick(410, loaded_world(), reliable(encode_r(proto::Despawn{1000})));
    CHECK(count_of<ReleaseNpc>(out) == 1 && count_of<FreeReplica>(out) == 0);
    CHECK_EQ(d.freed_replica(), 0u);
}

TEST("dialogues 4.2 invité : fin de session pendant le dialogue → ReleaseNpc (rendue au jeu), plus rien de libéré") {
    Session g = guest_with_npcs();
    g.tick(400, loaded_world(), {LocalDialogState{1001, true}});
    const Output out = g.tick(410, loaded_world(), reliable(encode_r(proto::SessionEnd{1})));
    CHECK(g.state() == State::Ended);
    bool released = false;
    for (const auto* r : commands_of<ReleaseNpc>(out)) released = released || r->net_id == 1001u;
    CHECK(released);
    CHECK_EQ(g.freed_replica(), 0u);
}

TEST("dialogues 4.2 invité : DialogLine{2} reçu → ignoré, Warn une fois") {
    Session g = guest_with_npcs();
    Output out = g.tick(410, loaded_world(), reliable(encode_r(proto::DialogLine{proto::kGuestNetId, 3, kSample})));
    CHECK(commands_of<ApplyDialogLine>(out).empty());
    CHECK(has_log_at(out, Level::Warn, "ignoré : locuteur 2 = son propre héros"));
    out = g.tick(420, loaded_world(), reliable(encode_r(proto::DialogLine{proto::kGuestNetId, 3, kSample})));
    CHECK(!has_log_at(out, Level::Warn, "ignoré"));
    CHECK(g.state() == State::InSession);
}

TEST("dialogues 4.3 invité : DialogState{1001, 1} de l'hôte → busy_npc 1001, Info ; {1001, 0} → 0") {
    Session g = guest_with_npcs();
    CHECK_EQ(g.busy_npc(), 0u);
    Output out = g.tick(400, loaded_world(), reliable(encode_r(proto::DialogState{1001, 1})));
    CHECK(count_of<FreeReplica>(out) == 0 && sent_of<proto::DialogState>(out).empty());
    CHECK(has_log_at(out, Level::Info, "[sync] DialogState de l'hôte (PNJ 1001, occupé) : dialogue refusé à l'invité"));
    CHECK_EQ(g.busy_npc(), 1001u);
    out = g.tick(410, loaded_world(), reliable(encode_r(proto::DialogState{1000, 0})));  // autre PNJ : sans effet
    CHECK_EQ(g.busy_npc(), 1001u);
    out = g.tick(420, loaded_world(), reliable(encode_r(proto::DialogState{1001, 0})));
    CHECK(has_log_at(out, Level::Info, "l'invité peut de nouveau lui parler"));
    CHECK_EQ(g.busy_npc(), 0u);
    // Despawn du PNJ occupé : plus rien à refuser.
    g.tick(440, loaded_world(), reliable(encode_r(proto::DialogState{1001, 1})));
    g.tick(450, loaded_world(), reliable(encode_r(proto::Despawn{1001})));
    CHECK_EQ(g.busy_npc(), 0u);
    // Fin de session : 0.
    g.tick(460, loaded_world(), reliable(encode_r(proto::DialogState{1000, 1})));
    CHECK_EQ(g.busy_npc(), 1000u);
    g.tick(470, loaded_world(), reliable(encode_r(proto::SessionEnd{1})));
    CHECK_EQ(g.busy_npc(), 0u);
}

TEST("dialogues 4.3 invité : l'hôte occupe le PNJ auquel l'invité parle déjà (course) → Warn") {
    Session g = guest_with_npcs();
    g.tick(400, loaded_world(), {LocalDialogState{1001, true}});
    const Output out = g.tick(410, loaded_world(), reliable(encode_r(proto::DialogState{1001, 1})));
    CHECK(has_log_at(out, Level::Warn, "l'hôte et l'invité lui parlent en même temps"));
    CHECK_EQ(g.busy_npc(), 1001u);
    CHECK_EQ(g.freed_replica(), 1001u);  // le dialogue local continue
}

TEST("dialogues 4.3 hôte : PNJ occupé par l'hôte → DialogState{1001, 1} envoyé, puis {1001, 0} ; busy_npc = PNJ tenu") {
    Session s = host_with_npcs();  // 1000 = GUID 1, 1001 = GUID 2
    World engaged = host_world({}, false);
    engaged.engaged_ok = true;
    engaged.engaged = guid_of(2);
    Output out = s.tick(1060, engaged, {});
    auto sent = sent_of<proto::DialogState>(out);
    CHECK(sent.size() == 1 && sent[0].npc_net_id == 1001u && sent[0].active == 1);
    CHECK(has_log_at(out, Level::Info, "[sync] PNJ 1001 occupé par l'hôte (dialogue)"));
    CHECK(sent_of<proto::DialogState>(s.tick(1070, engaged, {})).empty());  // une seule fois
    // Le dialogue en cours fait foi sur la demande : autre PNJ → fin de l'ancien, début du nouveau.
    World talking = engaged;
    talking.dialog_running = true;
    talking.dialog_partner_ok = true;
    talking.dialog_partner = guid_of(1);
    out = s.tick(1080, talking, {});
    sent = sent_of<proto::DialogState>(out);
    CHECK(sent.size() == 2 && sent[0].npc_net_id == 1001u && sent[0].active == 0 && sent[1].npc_net_id == 1000u &&
          sent[1].active == 1);
    out = s.tick(1090, host_world({}, false), {});
    sent = sent_of<proto::DialogState>(out);
    CHECK(sent.size() == 1 && sent[0].npc_net_id == 1000u && sent[0].active == 0);
    CHECK(has_log_at(out, Level::Info, "[sync] PNJ 1000 libéré par l'hôte"));
    // busy_npc de l'hôte : le PNJ tenu pour l'invité.
    CHECK_EQ(s.busy_npc(), 0u);
    s.tick(1100, host_world({}, false), reliable(encode_r(proto::DialogState{1001, 1})));
    CHECK_EQ(s.busy_npc(), 1001u);
}

TEST("dialogues 4.3 hôte : PNJ non annoncé occupé → rien envoyé ; DialogState de l'invité pour le PNJ occupé → ignoré") {
    Session s = host_with_npcs();
    World other = host_world({}, false);
    other.engaged_ok = true;
    other.engaged = guid_of(9);  // pas annoncé
    CHECK(sent_of<proto::DialogState>(s.tick(1060, other, {})).empty());
    World engaged = host_world({}, false);
    engaged.engaged_ok = true;
    engaged.engaged = guid_of(2);
    const Output out = s.tick(1070, engaged, reliable(encode_r(proto::DialogState{1001, 1})));
    CHECK(count_of<HoldNpc>(out) == 0);
    CHECK(has_log(out, "ignoré (PNJ occupé par l'hôte)"));
}

TEST("dialogues 4.3 hôte : l'hôte occupe le PNJ tenu (course) → UnholdNpc, Warn") {
    Session s = host_with_npcs();
    s.tick(1060, host_world({}, false), reliable(encode_r(proto::DialogState{1001, 1})));
    World engaged = host_world({}, false);
    engaged.engaged_ok = true;
    engaged.engaged = guid_of(2);
    const Output out = s.tick(1070, engaged, {});
    CHECK(count_of<UnholdNpc>(out) == 1);
    CHECK(has_log_at(out, Level::Warn, "[sync] PNJ 1001 rendu (routine reprise) : PNJ occupé par l'hôte"));
    CHECK_EQ(s.held_npc(), 0u);
}

TEST("dialogues 4.2 hôte : DialogState{1001, 1} → HoldNpc{1001, GUID 2}, Info ; {1001, 0} → UnholdNpc") {
    Session s = host_with_npcs();  // 1000 = GUID 1, 1001 = GUID 2
    Output out = s.tick(1060, host_world({}, false), reliable(encode_r(proto::DialogState{1001, 1})));
    const auto hold = commands_of<HoldNpc>(out);
    CHECK(hold.size() == 1 && hold[0]->net_id == 1001u && hold[0]->guid == guid_of(2));
    CHECK(has_log_at(out, Level::Info, "PNJ 1001 tenu (routine figée) pendant le dialogue de l'invité"));
    CHECK_EQ(s.held_npc(), 1001u);
    CHECK(count_of<HoldNpc>(s.tick(1070, host_world({}, false), reliable(encode_r(proto::DialogState{1001, 1})))) == 0);
    // Fin d'un autre PNJ : ignorée.
    out = s.tick(1080, host_world({}, false), reliable(encode_r(proto::DialogState{1000, 0})));
    CHECK(count_of<UnholdNpc>(out) == 0);
    out = s.tick(1090, host_world({}, false), reliable(encode_r(proto::DialogState{1001, 0})));
    const auto unhold = commands_of<UnholdNpc>(out);
    CHECK(unhold.size() == 1 && unhold[0]->net_id == 1001u);
    CHECK(has_log_at(out, Level::Info, "[sync] PNJ 1001 rendu (routine reprise) : fin du dialogue de l'invité"));
    CHECK_EQ(s.held_npc(), 0u);
    CHECK_EQ(dialog_tail(host_stats_line(s, 1100)),
             std::string("dialogues : DialogLine envoyés 0, reçus 0, rejoués 0, ignorés 1 ; DialogState envoyés 0, reçus 4"));
}

TEST("dialogues 4.2 hôte : un seul PNJ tenu : un nouveau DialogState{…, 1} rend d'abord le précédent") {
    Session s = host_with_npcs();
    s.tick(1060, host_world({}, false), reliable(encode_r(proto::DialogState{1001, 1})));
    const Output out = s.tick(1070, host_world({}, false), reliable(encode_r(proto::DialogState{1000, 1})));
    CHECK(count_of<UnholdNpc>(out) == 1 && count_of<HoldNpc>(out) == 1);
    CHECK(index_of<UnholdNpc>(out) < index_of<HoldNpc>(out));
    CHECK_EQ(s.held_npc(), 1000u);
}

TEST("dialogues 4.2 hôte : PNJ inconnu, mort, en dialogue avec l'hôte → ignoré, compté") {
    Session s = host_with_npcs();
    Output out = s.tick(1060, host_world({}, false), reliable(encode_r(proto::DialogState{1500, 1})));
    CHECK(count_of<HoldNpc>(out) == 0);
    CHECK(has_log(out, "ignoré (PNJ non annoncé)"));
    World talking = host_world({}, false);
    talking.dialog_running = true;
    talking.dialog_partner_ok = true;
    talking.dialog_partner = guid_of(2);
    out = s.tick(1070, talking, reliable(encode_r(proto::DialogState{1001, 1})));
    CHECK(count_of<HoldNpc>(out) == 0);
    CHECK(has_log(out, "ignoré (PNJ occupé par l'hôte)"));
    out = s.tick(1080, talking, reliable(encode_r(proto::DialogState{1000, 1})));  // autre PNJ : tenu
    CHECK(count_of<HoldNpc>(out) == 1);
    Session d = host_in_session();
    d.tick(1050, host_world({npc_at(1, 100.0f)}), {});
    d.tick(1100, host_world({npc_at(1, 100.0f, 0, false)}), {});  // Death{1000}
    out = d.tick(1110, host_world({}, false), reliable(encode_r(proto::DialogState{1000, 1})));
    CHECK(count_of<HoldNpc>(out) == 0);
    CHECK(has_log(out, "ignoré (PNJ mort)"));
}

TEST("dialogues 4.2 hôte : PNJ tenu mort ou retiré de la sélection → UnholdNpc") {
    Session s = host_in_session();
    s.tick(1050, host_world({npc_at(1, 100.0f), npc_at(2, 200.0f)}), {});  // 1000, 1001
    s.tick(1060, host_world({}, false), reliable(encode_r(proto::DialogState{1000, 1})));
    Output out = s.tick(1100, host_world({npc_at(1, 100.0f, 0, false), npc_at(2, 200.0f)}), {});  // Death{1000}
    auto unhold = commands_of<UnholdNpc>(out);
    CHECK(unhold.size() == 1 && unhold[0]->net_id == 1000u);
    CHECK(has_log(out, "[sync] PNJ 1000 rendu (routine reprise) : mort"));
    s.tick(1110, host_world({}, false), reliable(encode_r(proto::DialogState{1001, 1})));
    NpcState far = npc_at(2, 200.0f);
    far.pose.z += 10000.0f;  // hors du rayon
    out = s.tick(1150, host_world({far}), {});
    unhold = commands_of<UnholdNpc>(out);
    CHECK(unhold.size() == 1 && unhold[0]->net_id == 1001u);
    CHECK_EQ(s.held_npc(), 0u);
}

TEST("dialogues 4.2 hôte : 60 s sans fin → UnholdNpc, Warn ; fin de session → UnholdNpc") {
    Session s = host_with_npcs();
    s.tick(1060, host_world({}, false), reliable(encode_r(proto::DialogState{1001, 1})));
    Output out;
    std::uint64_t t = 1060;
    for (; t < 1060 + kDialogHoldTimeoutMs - 1; t += 3000) {
        out = s.tick(t, host_world({}, false), reliable(encode_r(proto::Heartbeat{})));
        CHECK(count_of<UnholdNpc>(out) == 0);
    }
    out = s.tick(1060 + kDialogHoldTimeoutMs, host_world({}, false), reliable(encode_r(proto::Heartbeat{})));
    CHECK(count_of<UnholdNpc>(out) == 1);
    CHECK(has_log_at(out, Level::Warn, "[sync] PNJ 1001 rendu (routine reprise) : 60 s sans activité du dialogue de l'invité"));
    CHECK_EQ(s.held_npc(), 0u);
    Session e = host_with_npcs();
    e.tick(1060, host_world({}, false), reliable(encode_r(proto::DialogState{1001, 1})));
    out = e.tick(1070, host_world({}, false), reliable(encode_r(proto::SessionEnd{1})));
    CHECK(e.state() == State::Ended);
    const auto unhold = commands_of<UnholdNpc>(out);
    CHECK(unhold.size() == 1 && unhold[0]->net_id == 1001u);
}

TEST("dialogues 4.2 hôte : DialogLine{2} → ApplyDialogLine sur l'avatar ; DialogLine{PNJ tenu} → avec son GUID") {
    Session s = host_with_npcs();
    // Sans avatar de l'invité : ignoré.
    Output out = s.tick(1060, host_world({}, false), reliable(encode_r(proto::DialogLine{proto::kGuestNetId, 3, kSample})));
    CHECK(commands_of<ApplyDialogLine>(out).empty());
    CHECK(has_log(out, "ignoré (pas d'avatar de l'invité)"));
    s.tick(1070, host_world({}, false), reliable(encode_r(spawn_of(proto::kGuestNetId))));
    out = s.tick(1080, host_world({}, false), reliable(encode_r(proto::DialogLine{proto::kGuestNetId, 3, kSample})));
    auto apply = commands_of<ApplyDialogLine>(out);
    CHECK(apply.size() == 1 && apply[0]->net_id == proto::kGuestNetId && apply[0]->output_type == 3);
    // PNJ non tenu : ignoré ; tenu : rejoué, retrouvé par son GUID.
    out = s.tick(1090, host_world({}, false), reliable(encode_r(proto::DialogLine{1001, 0, kSample})));
    CHECK(commands_of<ApplyDialogLine>(out).empty());
    CHECK(has_log(out, "ignoré (PNJ non tenu"));
    out = s.tick(1100, host_world({}, false),
                 reliable2(proto::DialogState{1001, 1}, proto::DialogLine{1001, 0, kSample}));
    apply = commands_of<ApplyDialogLine>(out);
    CHECK(apply.size() == 1 && apply[0]->net_id == 1001u && apply[0]->guid == guid_of(2) && apply[0]->sample == kSample);
}

TEST("dialogues 4.2 hôte : dialogues = 0 → DialogState et DialogLine de l'invité ignorés, Info une fois") {
    Settings off = host_settings();
    off.dialogues = false;
    Session s(off);
    s.tick(0, loaded_world(), {});
    s.tick(10, loaded_world(), control_from(kGuestA, encode(make_hello())));
    s.tick(50, host_world({npc_at(1, 100.0f)}), {});
    Output out = s.tick(60, host_world({}, false), reliable(encode_r(proto::DialogState{1000, 1})));
    CHECK(count_of<HoldNpc>(out) == 0);
    CHECK(has_log_at(out, Level::Info, "[sync] DialogState reçus de l'invité mais dialogues inactifs ici"));
    out = s.tick(70, host_world({}, false), reliable(encode_r(proto::DialogLine{proto::kGuestNetId, 3, kSample})));
    CHECK(commands_of<ApplyDialogLine>(out).empty());
    CHECK(!has_log(out, "dialogues inactifs ici"));  // une fois par session
}

TEST("dialogues 4.2 hôte : répliques toutes les 40 s → tenue gardée au-delà de 60 s, puis 60 s d'inactivité") {
    Session s = host_with_npcs();
    s.tick(1060, host_world({}, false), reliable(encode_r(proto::DialogState{1001, 1})));
    std::uint64_t last_line = 1060;
    for (std::uint64_t t = 4060; t <= 1060 + 3 * 40000; t += 4000) {
        std::vector<Input> in = reliable(encode_r(proto::Heartbeat{}));
        if (t - last_line >= 40000) {
            in = reliable(encode_r(proto::DialogLine{1001, 0, kSample}));
            last_line = t;
        }
        const Output out = s.tick(t, host_world({}, false), in);
        CHECK(count_of<UnholdNpc>(out) == 0);
    }
    CHECK_EQ(s.held_npc(), 1001u);  // 120 s après le début
    // DialogState{1001, 1} répété : délai relancé aussi.
    std::uint64_t t = last_line + 50000;
    for (std::uint64_t u = last_line + 4000; u < t; u += 4000) s.tick(u, host_world({}, false), reliable(encode_r(proto::Heartbeat{})));
    CHECK(count_of<UnholdNpc>(s.tick(t, host_world({}, false), reliable(encode_r(proto::DialogState{1001, 1})))) == 0);
    for (std::uint64_t u = t + 4000; u < t + kDialogHoldTimeoutMs; u += 4000) {
        CHECK(count_of<UnholdNpc>(s.tick(u, host_world({}, false), reliable(encode_r(proto::Heartbeat{})))) == 0);
    }
    CHECK_EQ(count_of<UnholdNpc>(s.tick(t + kDialogHoldTimeoutMs, host_world({}, false), reliable(encode_r(proto::Heartbeat{})))), 1u);
}

TEST("dialogues 4.2 hôte : hôte en dialogue au partenaire illisible → tenue refusée ; hôte qui parle au PNJ tenu → rendu") {
    Session s = host_with_npcs();
    World unreadable = host_world({}, false);
    unreadable.dialog_running = true;
    unreadable.dialog_partner_ok = false;
    Output out = s.tick(1060, unreadable, reliable(encode_r(proto::DialogState{1001, 1})));
    CHECK(count_of<HoldNpc>(out) == 0);
    CHECK(has_log(out, "ignoré (hôte en dialogue, partenaire illisible)"));
    s.tick(1070, host_world({}, false), reliable(encode_r(proto::DialogState{1001, 1})));
    CHECK_EQ(s.held_npc(), 1001u);
    World talking = host_world({}, false);
    talking.dialog_running = true;
    talking.dialog_partner_ok = true;
    talking.dialog_partner = guid_of(1);  // autre PNJ : tenue gardée
    CHECK(count_of<UnholdNpc>(s.tick(1080, talking, {})) == 0);
    talking.dialog_partner = guid_of(2);
    out = s.tick(1090, talking, {});
    const auto unhold = commands_of<UnholdNpc>(out);
    CHECK(unhold.size() == 1 && unhold[0]->net_id == 1001u);
    CHECK(has_log(out, "[sync] PNJ 1001 rendu (routine reprise) : PNJ occupé par l'hôte"));
    CHECK_EQ(s.held_npc(), 0u);
}

TEST("dialogues 4.2 : LocalDialogNpcLost → hôte : tenue oubliée (Warn) ; invité : dialogue clos (Warn, DialogState{…, 0})") {
    Session s = host_with_npcs();
    s.tick(1060, host_world({}, false), reliable(encode_r(proto::DialogState{1001, 1})));
    Output out = s.tick(1070, host_world({}, false), {LocalDialogNpcLost{1000}});  // pas le PNJ tenu
    CHECK_EQ(s.held_npc(), 1001u);
    out = s.tick(1080, host_world({}, false), {LocalDialogNpcLost{1001}});
    CHECK_EQ(s.held_npc(), 0u);
    CHECK(count_of<UnholdNpc>(out) == 0);
    CHECK(has_log_at(out, Level::Warn, "PNJ 1001 : tenue impossible ou perdue chez l'hôte"));
    Session g = guest_with_npcs();
    g.tick(400, loaded_world(), {LocalDialogState{1001, true}});
    out = g.tick(410, loaded_world(), {LocalDialogNpcLost{1001}});
    CHECK_EQ(g.freed_replica(), 0u);
    CHECK(has_log_at(out, Level::Warn, "PNJ 1001 : libération impossible chez l'invité"));
    const auto sent = sent_of<proto::DialogState>(out);
    CHECK(sent.size() == 1 && sent[0].npc_net_id == 1001u && sent[0].active == 0);
    // Fin tardive de ce dialogue déjà clos : Debug, non comptée.
    out = g.tick(420, loaded_world(), {LocalDialogState{1001, false}});
    CHECK(has_log_at(out, Level::Debug, "ignoré (pas le dialogue en cours)"));
    CHECK_EQ(dialog_tail(guest_stats_line(g, 600)),
             std::string("dialogues : DialogLine envoyés 0, reçus 0, rejoués 0, ignorés 0 ; DialogState envoyés 2, reçus 0"));
}

TEST("dialogues 4.2 invité : gestes et pose de la réplique libérée → Motion, MotionParam et snapshot{1001} ; fin → plus rien") {
    Session g = guest_with_npcs();
    // Avant le dialogue : capture d'une réplique jetée.
    Output out = g.tick(400, loaded_world(), {LocalMotion{"Talk_Gesture", local_motion_of(), 1001}});
    CHECK(sent_of<proto::Motion>(out).empty());
    World w = loaded_world();
    w.freed_npc = 1001;
    w.freed_pose_ok = true;
    w.freed_pose = Pose{5.0f, 6.0f, 7.0f, 0.25f};
    out = g.tick(460, w,
                 {LocalDialogState{1001, true}, LocalMotion{"Talk_Gesture", local_motion_of(), 1001},
                  LocalMotion{"Autre", local_motion_of(), 1000},
                  npc_param_in(1001, 0, proto::MotionParamKind::TargetWeight, 0.5f, 0.2f)});
    const auto motions = sent_of<proto::Motion>(out);
    CHECK(motions.size() == 1 && motions[0].net_id == 1001u);
    const auto names = sent_of<proto::AniName>(out);
    CHECK(names.size() == 1 && names[0].name == "Talk_Gesture");
    const auto params = sent_of<proto::MotionParam>(out);
    CHECK(params.size() == 1 && params[0].net_id == 1001u && params[0].value == 0.5f);
    auto snaps = snapshots_sent(out);
    CHECK(snaps.size() == 1 && snaps[0].entities.size() == 2);
    if (snaps.size() == 1 && snaps[0].entities.size() == 2) {
        const proto::SnapshotEntity& f = snaps[0].entities[1];
        CHECK(f.net_id == 1001u && f.x == 5.0f && f.y == 6.0f && f.z == 7.0f && f.yaw == 0.25f);
    }
    // Pose d'une autre réplique, ou illisible : pas d'entrée.
    World other = w;
    other.freed_npc = 1000;
    snaps = snapshots_sent(g.tick(510, other, {}));
    CHECK(snaps.size() == 1 && snaps[0].entities.size() == 1);
    World unread = w;
    unread.freed_pose_ok = false;
    snaps = snapshots_sent(g.tick(560, unread, {}));
    CHECK(snaps.size() == 1 && snaps[0].entities.size() == 1);
    // Fin : gestes jetés, pose plus envoyée.
    out = g.tick(610, w, {LocalDialogState{1001, false}, LocalMotion{"Talk_Gesture", local_motion_of(), 1001}});
    CHECK(sent_of<proto::Motion>(out).empty());
    snaps = snapshots_sent(out);
    CHECK(snaps.size() == 1 && snaps[0].entities.size() == 1);
}

TEST("dialogues 4.2 hôte : gestes et pose de l'invité pour le PNJ tenu → PlayNpcMotion, PlayNpcParam, MoveNpc ; rendu → plus rien") {
    Session s = host_with_npcs();  // 1000 = GUID 1, 1001 = GUID 2
    // Pas encore tenu : ignorés.
    Output out = s.tick(1055, host_world({}, false),
                        reliable2(proto::AniName{9, "Talk_Gesture"}, host_npc_motion(1001, 9, 0)));
    CHECK(count_of<PlayNpcMotion>(out) == 0);
    s.tick(1060, host_world({}, false), reliable(encode_r(proto::DialogState{1001, 1})));
    // Horloge pas prête : gestes aussitôt, pas de pose.
    out = s.tick(1070, host_world({}, false), reliable2(host_npc_motion(1001, 9, 10), host_npc_motion(1000, 9, 10)));
    auto played = commands_of<PlayNpcMotion>(out);
    CHECK(played.size() == 1 && played[0]->net_id == 1001u && played[0]->name == "Talk_Gesture");
    out = s.tick(1080, host_world({}, false), reliable(encode_r(host_npc_param(1001, 20, 0, 0.5f))));
    const auto params = commands_of<PlayNpcParam>(out);
    CHECK(params.size() == 1 && params[0]->net_id == 1001u && params[0]->param.value == 0.5f);
    CHECK(count_of<MoveNpc>(out) == 0);
    // Snapshots de l'invité avec la pose de sa réplique libérée : MoveNpc sur le PNJ tenu.
    bool moved = false;
    for (std::uint32_t tick = 0; tick < 6; ++tick) {
        out = s.tick(1100 + tick * 50, host_world({}, false),
                     snapshot_in(npc_snapshot(tick, {npc_entity(proto::kGuestNetId, 1.0f, 100),
                                                     npc_entity(1001, 40.0f, 0)})));
        for (const auto* m : commands_of<MoveNpc>(out)) {
            CHECK(m->net_id == 1001u);
            if (m->pose.x == 40.0f) moved = true;
        }
    }
    CHECK(moved);
    // Rendu : plus de pose ni de geste.
    s.tick(1400, host_world({}, false), reliable(encode_r(proto::DialogState{1001, 0})));
    out = s.tick(1410, host_world({}, false),
                 {SnapshotIn{npc_snapshot(6, {npc_entity(proto::kGuestNetId, 1.0f, 100), npc_entity(1001, 40.0f, 0)})},
                  ReliableIn{encode_r(host_npc_motion(1001, 9, 400))}});
    CHECK(count_of<MoveNpc>(out) == 0 && count_of<PlayNpcMotion>(out) == 0);
}

TEST("arme des PNJ 3.8c hôte : LocalNpcWeapon → Weapon{NetId} au changement seulement ; main vide d'abord : rien") {
    Session s = host_with_npcs();  // 1000, 1001 annoncés
    Output out = s.tick(1060, host_world({}, false), {LocalNpcWeapon{1000, ""}});
    CHECK(sent_of<proto::Weapon>(out).empty());
    out = s.tick(1070, host_world({}, false), {LocalNpcWeapon{1000, "It_1H_Club"}});
    auto sent = sent_of<proto::Weapon>(out);
    CHECK(sent.size() == 1 && sent[0].net_id == 1000u && sent[0].name == "It_1H_Club");
    CHECK(sent_of<proto::Weapon>(s.tick(1080, host_world({}, false), {LocalNpcWeapon{1000, "It_1H_Club"}})).empty());
    out = s.tick(1090, host_world({}, false), {LocalNpcWeapon{1000, ""}});
    sent = sent_of<proto::Weapon>(out);
    CHECK(sent.size() == 1 && sent[0].name.empty());
    // PNJ non annoncé : rien.
    CHECK(sent_of<proto::Weapon>(s.tick(1100, host_world({}, false), {LocalNpcWeapon{1500, "It_1H_Club"}})).empty());
}

TEST("arme des PNJ 3.8c invité : Weapon{1001} → NpcWeapon ; réplique absente → ignoré") {
    Session g = guest_with_npcs();
    Output out = g.tick(400, loaded_world(), reliable(encode_r(proto::Weapon{1001, "It_1H_Sword"})));
    const auto cmds = commands_of<NpcWeapon>(out);
    CHECK(cmds.size() == 1 && cmds[0]->net_id == 1001u && cmds[0]->name == "It_1H_Sword");
    CHECK(commands_of<AvatarWeapon>(out).empty());
    out = g.tick(410, loaded_world(), reliable(encode_r(proto::Weapon{1500, "It_1H_Sword"})));
    CHECK(commands_of<NpcWeapon>(out).empty());
    CHECK(has_log(out, "ignoré (réplique absente)"));
}
