// Machine à états de session, hôte et invité (stories 1.4 et 1.5, protocol/PROTOCOL.md §8).
//
// Logique pure : ni horloge système, ni socket, ni appel au jeu. L'appelant (le pilote
// Windows sync/session, ou l'hôte Linux de test) lui donne à chaque tick le temps en ms
// (horloge monotone), l'état du monde et les datagrammes reçus ; elle rend des commandes
// à exécuter dans l'ordre et des lignes de log. Les deux appelants exécutent ainsi
// exactement la même logique.
//
// États : Inactive → Hosting | Connecting → InSession → Ended → Inactive. L'armement a lieu
// au premier tick où le monde est chargé (joueur non nul et partie en cours). Toutes les fins
// passent par la même transition : SessionEnd{raison} si la cause est locale et qu'on est en
// session, Ended (raison au log), maintien du KCP pendant kLingerMs, puis CloseReliable, Stop
// et Ended → Inactive. La session se réarme ensuite au monde chargé suivant ; l'hôte dont
// l'invité est parti (timeout, SessionEnd reçu) se réarme aussitôt, dans le même monde.
//
// Avatars (story 2.2, PROTOCOL.md §8) : à l'entrée en session, chaque pair envoie le Spawn de
// son héros ; l'invité est d'abord téléporté devant l'hôte (Teleport). Un Spawn reçu du pair
// donne SpawnAvatar, un Despawn reçu ou toute fin de session donne DespawnAvatar. La session
// sait seulement si l'avatar existe ; sa création et sa destruction sont l'affaire de sync/.
//
// Snapshots (story 2.3, AD-7) : en session, la pose du héros part à 20 Hz (à l'horloge) ; ceux
// du pair alimentent l'horloge distante et l'interpolateur, qui donnent à chaque tick la pose
// de l'avatar (MoveAvatar), affichée avec 150 ms de retard.
//
// Animations (story 2.4, AD-8) : chaque PlayMotion du héros local (LocalMotion) est interné
// (AniName la première fois) et part en Motion daté ; ceux du pair attendent le même instant
// d'affichage que les snapshots, puis deviennent PlayAvatarMotion. Le ragdoll du héros
// (IsDead passe à 1) envoie Death, une fois par mort ; celle du pair met l'avatar en ragdoll
// (AvatarDeath), au même instant d'affichage que la pose.
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include "core/config.h"
#include "core/interpolation.h"
#include "core/pose.h"
#include "core/protocol.h"
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
inline constexpr std::uint64_t kLingerMs = 1000;  // maintien du KCP après Ended (SessionEnd)
inline constexpr float kGuestSpawnDistance = 200.0f;  // invité placé à 2 m devant l'hôte (cm)

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
    std::uintptr_t player = 0;  // Entity::GetPlayer(), entier opaque (jamais déréférencé ici)
    bool running = false;       // gCSession::IsGameRunning (instance nulle : false)
    bool dead = false;          // Entity::IsDead du joueur
    bool pose_ok = false;       // pose du héros lue (Welcome, Spawn, snapshots)
    float x = 0.0f, y = 0.0f, z = 0.0f, yaw = 0.0f;

    bool loaded() const { return player != 0 && running; }  // monde chargé
};

// Entrées : ce que le transport a rendu depuis le tick précédent.
struct ControlIn {
    Endpoint source;
    Bytes bytes;  // datagramme complet, octet de canal compris
};
struct ReliableIn {
    Bytes bytes;  // [MsgType][charge]
};
struct SnapshotIn {
    Bytes bytes;  // datagramme de canal 1 complet, venu du pair
};
// Un PlayMotion du héros local (capturé par le hook) : nom lisible et Motion sans net_id,
// ani_id ni ms (renseignés par la session).
struct LocalMotion {
    std::string name;
    protocol::Motion motion;
};
using Input = std::variant<ControlIn, ReliableIn, SnapshotIn, LocalMotion>;

using core::Pose;

// Pose de l'invité devant l'hôte : kGuestSpawnDistance le long de l'axe avant de l'hôte
// (sin lacet, 0, cos lacet), tourné vers lui (lacet + π, ramené dans ]−π, π]).
Pose guest_pose_near(const Pose& host);

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
struct Teleport {  // invité : déplacer son propre héros (Entity::MoveTo)
    Pose pose;
};
struct SpawnAvatar {  // créer l'avatar du pair
    std::uint32_t net_id = 0;
    Pose pose;
};
struct DespawnAvatar {  // retirer puis détruire l'avatar du pair
    std::uint32_t net_id = 0;
};
struct SendSnapshot {  // datagramme de canal 1 complet, pour le pair
    Bytes bytes;
};
struct MoveAvatar {  // placer l'avatar (Entity::MoveTo), à chaque tick
    std::uint32_t net_id = 0;
    Pose pose;
};
struct PlayAvatarMotion {  // rejouer un Motion du pair sur l'avatar (nom → GetMotion → PlayMotion)
    std::uint32_t net_id = 0;
    std::string name;
    protocol::Motion motion;
};
struct AvatarDeath {  // mettre l'avatar en ragdoll
    std::uint32_t net_id = 0;
};
using Command = std::variant<Start, Send, OpenReliable, SendReliable, CloseReliable, Stop, Teleport, SpawnAvatar,
                             DespawnAvatar, SendSnapshot, MoveAvatar, PlayAvatarMotion, AvatarDeath>;

inline constexpr std::size_t kMaxPendingMotions = 256;  // Motion du pair en attente d'affichage
inline constexpr double kMaxMotionLeadMs = 1000.0;      // au-delà, un Motion « du futur » part aussitôt
inline constexpr std::uint32_t kAniIdCount = 0x10000;   // AniId u16

inline constexpr std::uint64_t kSnapshotStatsMs = 5000;  // bilan debug des snapshots

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

    // Échec d'exécution d'une commande (port occupé…) : la session se termine (transition
    // unique, SessionEnd{4} si en session) avec `cause` au log. Sans effet si elle est déjà
    // terminée ou inactive. Réarmement au monde suivant.
    Output fail(std::uint64_t now_ms, const std::string& cause);

    State state() const { return state_; }
    bool avatar_present() const { return avatar_present_; }
    bool avatar_dead() const { return avatar_dead_; }
    std::uint32_t conv() const { return conv_; }
    const Endpoint& peer() const { return peer_; }

private:
    void arm(std::uint64_t now, const World& world, Output& out);
    void on_control(std::uint64_t now, const ControlIn& in, Output& out);
    void on_reliable(std::uint64_t now, const ReliableIn& in, Output& out);
    void on_snapshot(std::uint64_t now, const SnapshotIn& in, Output& out);
    void on_local_motion(std::uint64_t now, const LocalMotion& in, Output& out);
    void send_death(const World& world, Output& out);
    void on_peer_motion(const protocol::Motion& m, Output& out);
    void play_due_motions(std::uint64_t now, Output& out);
    void clear_peer_avatar_state();
    void send_snapshot(std::uint64_t now, const World& world, Output& out);
    void place_avatar(std::uint64_t now, Output& out);
    void snapshot_stats(std::uint64_t now, Output& out);
    void host_on_hello(std::uint64_t now, const ControlIn& in, Output& out);
    void guest_on_control(std::uint64_t now, const ControlIn& in, Output& out);
    void timers(std::uint64_t now, Output& out);
    // Entrée en InSession : délais, puis Spawn de son héros à la pose `own`.
    void enter_session(std::uint64_t now, const Pose& own, Output& out);
    void remove_avatar(const std::string& why, Output& out);  // DespawnAvatar si l'avatar existe
    std::uint32_t own_net_id() const;
    std::uint32_t peer_net_id() const;
    // Transition de fin unique. `local_reason` ≠ 0 : cause locale, SessionEnd{local_reason}
    // envoyé si InSession. `rearm_now` : l'hôte se réarme dès la fin du maintien, dans le
    // même monde ; sinon, au monde suivant.
    // `level` : niveau du log de Ended (Info pour une fin de routine, Warn sinon).
    void end(std::uint64_t now, const std::string& reason, std::uint8_t local_reason, bool rearm_now, Level level,
             Output& out);
    void finish_linger(const World& world, Output& out);
    void track_world(const World& world, bool revived);
    bool can_arm(const World& world) const;
    void maybe_arm(std::uint64_t now, const World& world, Output& out);
    void transition(State next, const std::string& detail, Output& out, Level level = Level::Info);
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
    bool avatar_present_ = false;  // SpawnAvatar émis, pas encore de DespawnAvatar

    // Snapshots (story 2.3), remis à zéro à chaque entrée en session.
    std::uint64_t t0_ = 0;              // entrée en InSession (base du tick)
    std::int64_t last_tick_sent_ = -1;
    core::RemoteClock clock_;
    core::Interpolator interp_;         // avatar du pair
    std::uint64_t next_stats_ = 0;
    struct SnapshotCounters {
        unsigned sent = 0, received = 0, unreadable = 0, ignored = 0;
        unsigned interpolated = 0, extrapolated = 0, held = 0;
    } snap_;

    // Animations (story 2.4), remises à zéro à chaque entrée en session.
    std::unordered_map<std::string, std::uint16_t> ani_ids_;   // émetteur : nom → AniId
    std::uint32_t next_ani_id_ = 0;
    bool ani_full_logged_ = false;
    bool death_sent_ = false;  // Death envoyé pour la mort en cours (remis à faux au retour à la vie)
    std::unordered_map<std::uint16_t, std::string> peer_names_;  // récepteur : AniId → nom
    std::unordered_set<std::uint16_t> warned_ani_ids_;            // une ligne de log par AniId
    struct PendingMotion {
        protocol::Motion motion;
        std::string name;
    };
    std::deque<PendingMotion> pending_;
    bool avatar_dead_ = false;   // Death appliqué (AvatarDeath émis)
    bool death_pending_ = false; // Death reçu, appliqué quand le temps affiché atteint death_at_
    double death_at_ = 0.0;      // dernier temps distant connu à la réception (dernier snapshot)
    struct MotionCounters {
        unsigned sent = 0, played = 0, ignored = 0, superseded = 0, dropped = 0;
    } anim_;

    std::uintptr_t armed_player_ = 0;  // joueur à l'armement (fin de monde s'il change)
    std::uint64_t linger_until_ = 0;   // Ended : fin du maintien du KCP
    bool wait_new_world_ = false;      // Inactive : attendre que le monde change
};

}  // namespace mp::core::session
