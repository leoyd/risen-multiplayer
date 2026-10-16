// Machine à états de session, hôte et invité (story 1.4, protocol/PROTOCOL.md §8).
//
// Logique pure : ni horloge système, ni socket, ni appel au jeu. L'appelant (le pilote
// Windows sync/session, ou l'hôte Linux de test) lui donne à chaque tick le temps en ms
// (horloge monotone), l'état du monde et les datagrammes reçus ; elle rend des commandes
// à exécuter dans l'ordre et des lignes de log. Les deux appelants exécutent ainsi
// exactement la même logique.
//
// États : Inactive → Hosting | Connecting → InSession → Ended. L'armement a lieu au premier
// tick où le monde est chargé. Ended est final (le réarmement vient en story 1.5).
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "core/config.h"
#include "core/version.h"
#include "net/udp_socket.h"  // Endpoint, parse_ipv4 (en-tête seul, aucun socket utilisé ici)

namespace mp::core::session {

using Bytes = std::vector<std::uint8_t>;
using net::Endpoint;

// Délais du §8 (ms, horloge monotone).
inline constexpr std::uint64_t kHelloIntervalMs = 500;
inline constexpr std::uint64_t kConnectTimeoutMs = 10000;
inline constexpr std::uint64_t kHeartbeatIntervalMs = 1000;
inline constexpr std::uint64_t kPeerTimeoutMs = 5000;

enum class State { Inactive, Hosting, Connecting, InSession, Ended };
const char* state_name(State state);  // "Inactive", "Hosting"…

struct Settings {
    Role role = Role::Host;
    std::string host_ip;   // invité : IPv4 littérale de l'hôte
    std::uint16_t port = kDefaultPort;
    std::string nickname;
    std::string equipment;
    std::string mod_version;                      // MP_MOD_VERSION
    std::array<ModuleFingerprint, 4> fingerprint{};  // Game, Engine, Script, SharedBase (les siennes)
    std::uint64_t seed = 0;                       // graine du tirage de `conv`
};

// État du monde lu par l'appelant à chaque tick.
struct World {
    bool loaded = false;  // Entity::GetPlayer() non nul
    bool pose_ok = false; // pose lue (utile seulement en Hosting, pour le Welcome)
    float x = 0.0f, y = 0.0f, z = 0.0f, yaw = 0.0f;
};

// Entrées : ce que le transport a rendu depuis le tick précédent.
struct ControlIn {
    Endpoint source;
    Bytes bytes;  // datagramme complet, octet de canal compris
};
struct ReliableIn {
    Bytes bytes;  // [MsgType][charge]
};
using Input = std::variant<ControlIn, ReliableIn>;

// Commandes, à exécuter dans l'ordre.
struct Start {
    std::uint16_t port = 0;  // 0 = port éphémère (invité)
};
struct Send {
    Endpoint destination;
    Bytes bytes;  // datagramme de contrôle complet
};
struct OpenReliable {
    std::uint32_t conv = 0;
    Endpoint peer;
};
struct SendReliable {
    Bytes bytes;
};
struct CloseReliable {};
struct Stop {};
using Command = std::variant<Start, Send, OpenReliable, SendReliable, CloseReliable, Stop>;

enum class Level { Debug, Info, Warn, Error };
struct LogLine {
    Level level = Level::Info;
    std::string text;  // UTF-8, préfixe [sync] compris
};

struct Output {
    std::vector<Command> commands;
    std::vector<LogLine> logs;
};

class Session {
public:
    explicit Session(Settings settings);

    // Un tick : entrées traitées dans l'ordre, puis délais (Hello, heartbeat, timeouts).
    Output tick(std::uint64_t now_ms, const World& world, const std::vector<Input>& inputs);

    // Échec d'exécution d'une commande (port occupé…) : la session passe en Ended avec
    // `cause` au log, et rend Stop. Sans effet si elle est déjà terminée ou inactive.
    Output fail(std::uint64_t now_ms, const std::string& cause);

    State state() const { return state_; }
    std::uint32_t conv() const { return conv_; }
    const Endpoint& peer() const { return peer_; }

private:
    void arm(std::uint64_t now, const World& world, Output& out);
    void on_control(std::uint64_t now, const ControlIn& in, Output& out);
    void on_reliable(std::uint64_t now, const ReliableIn& in, Output& out);
    void host_on_hello(std::uint64_t now, const ControlIn& in, Output& out);
    void guest_on_control(std::uint64_t now, const ControlIn& in, Output& out);
    void timers(std::uint64_t now, Output& out);
    void enter_session(std::uint64_t now, Output& out);
    void end(const std::string& reason, Output& out);
    void transition(State next, const std::string& detail, Output& out);
    void send_reject(const Endpoint& to, std::uint8_t reason, Output& out);
    void send_hello(Output& out);
    std::uint32_t draw_conv();

    Settings settings_;
    State state_ = State::Inactive;
    bool disabled_ = false;  // config inutilisable (ex. host_ip non IPv4) : reste Inactive

    Endpoint host_;           // invité : adresse de l'hôte
    Bytes hello_;             // invité : Hello encodé une fois
    std::uint64_t next_hello_ = 0;
    std::uint64_t connect_deadline_ = 0;

    Endpoint peer_;           // pair accepté (hôte) ou hôte (invité), en session
    std::uint32_t conv_ = 0;
    Bytes welcome_;           // hôte : Welcome envoyé, renvoyé tel quel aux retransmissions
    bool reliable_seen_ = false;  // hôte : un message fiable du pair est arrivé
    std::uint64_t last_peer_ = 0;
    std::uint64_t next_heartbeat_ = 0;
    std::uint64_t heartbeats_rx_ = 0;
    std::uint64_t rng_ = 0;
    bool foreign_warned_ = false;  // invité : réponse d'une autre adresse déjà signalée
    World world_;             // monde du tick en cours (pose du Welcome)
};

}  // namespace mp::core::session
