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
//
// Arme (story 3.2) : le nom de l'objet en main du héros local (LocalWeapon, vide si l'arme est
// rangée) part en Weapon à chaque changement, et une fois à l'entrée en session, après le Spawn.
// Celui du pair devient AvatarWeapon au même instant d'affichage qu'un Death.
//
// Armure (story 3.2c) : même logique que l'arme. Le nom de l'armure portée par le héros local
// (LocalArmor, emplacement d'équipement 8, vide s'il n'en porte pas) part en Armor à chaque
// changement, et une fois à l'entrée en session, après le Spawn et le Weapon. Celui du pair
// devient AvatarArmor au même instant d'affichage ; Weapon et Armor du pair partagent une même
// file datée, qui conserve leur ordre d'arrivée.
//
// Équipement (story 3.2b) : `Settings.equipment` (forme de core/equipment.h) part dans le Hello
// de l'invité ; invalide, il est traité comme vide (Warn au premier tick). L'invité équipe son
// héros à chaque entrée en session, après la Teleport (EquipHero). L'hôte garde les noms du Hello
// accepté jusqu'à Ended et équipe l'avatar de l'invité juste après chaque SpawnAvatar
// (EquipAvatar) ; un Hello.equipment invalide donne un Warn et un avatar non équipé (pas de Reject).
//
// Répliques des PNJ (story 3.3, PROTOCOL.md §8) : l'hôte fait autorité. À chaque snapshot, il
// retient les PNJ vivants de World::npcs à kNpcRadius au plus de son héros ou de l'avatar de
// l'invité, du plus proche au plus loin, au plus kMaxSnapshotNpcs ; il leur attribue un NetId
// ≥ 1000 lié au GUID pour la session, annonce chaque entrée par Spawn{1, guid} et chaque sortie
// par Despawn, puis met leur pose et leurs PV dans le snapshot. L'invité tient une table NetId →
// {interpolation, PV} : Spawn{1} donne SpawnNpc, chaque tick MoveNpc (même horloge distante et
// même retard que l'avatar) et SetNpcHp quand les PV affichés changent, Despawn et Ended donnent
// ReleaseNpc. L'appariement par GUID et le gel de l'IA sont l'affaire de sync/npc.
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <set>
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

using core::Pose;

// Un PNJ lu par l'appelant côté hôte (story 3.3, sync/npc) : GUID (16 premiers octets de
// eCDynamicEntity::GetID), pose, PV (PSSkills::GetHitPoints), vivant (Entity::IsDead = 0).
struct NpcState {
    protocol::Guid guid{};
    Pose pose;
    int hp = 0;
    bool alive = true;
};

// État du monde lu par l'appelant à chaque tick.
struct World {
    std::uintptr_t player = 0;  // Entity::GetPlayer(), entier opaque (jamais déréférencé ici)
    bool running = false;       // gCSession::IsGameRunning (instance nulle : false)
    bool dead = false;          // Entity::IsDead du joueur
    bool pose_ok = false;       // pose du héros lue (Welcome, Spawn, snapshots)
    float x = 0.0f, y = 0.0f, z = 0.0f, yaw = 0.0f;
    int hp = 0;                 // PV du héros (PSSkills::GetHitPoints), dans le snapshot (v9)
    // Hôte (story 3.3) : PNJ du monde, hors héros et avatar, lus seulement quand
    // Session::wants_npcs() le demande. `npcs_ok` faux : lecture impossible (les PNJ annoncés
    // restent tels quels).
    bool npcs_ok = false;
    std::vector<NpcState> npcs;
    // Invité (story 3.3) : gCScriptRoutine_PS::OnProcess bloqués depuis le tick précédent
    // (répliques à l'IA figée), cumulés dans le bilan debug.
    unsigned routine_blocked = 0;

    bool loaded() const { return player != 0 && running; }  // monde chargé
};

inline constexpr float kNpcRadius = 5000.0f;  // 50 m (unités du monde = cm) autour d'un joueur
// Test en jeu du 2026-10-22 : Entity::ms_arrNPCs ne garde que les PNJ traités par le moteur
// (environ 40 m), qui y entrent et en sortent sans cesse à cette limite. Un PNJ annoncé absent
// de la liste n'est retiré qu'après ce nombre de ticks de snapshot (2 s) ; il n'est plus dans les
// snapshots pendant ce temps, et revenu, il garde son NetId sans nouveau Spawn.
inline constexpr std::uint32_t kNpcAbsentGraceTicks = 40;
// Le snapshot porte au plus 54 entités, héros de l'hôte compris (AD-7).
inline constexpr std::size_t kMaxSnapshotNpcs = protocol::kMaxSnapshotEntities - 1;
inline constexpr std::size_t kMaxNpcHpChanges = 16;  // PV d'une réplique en attente d'affichage

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
// Un réglage de locomotion du héros local (SetTargetWeight / SetPlaySpeed, story 2.4b).
struct LocalParam {
    std::uint8_t type = 0;
    std::uint8_t kind = 0;  // protocol::MotionParamKind
    float value = 0.0f;
    float time = 0.0f;
};
// Arme en main du héros local (story 3.2), poussée par l'appelant quand elle change (et à la
// première lecture dans un monde) : nom de l'objet tenu, vide si l'arme est rangée. Retenue dans
// tous les états ; envoyée en session seulement.
struct LocalWeapon {
    std::string name;
};
// Armure portée par le héros local (story 3.2c), mêmes règles que LocalWeapon : nom de l'objet de
// la pile équipée à l'emplacement 8, vide s'il n'en porte pas.
struct LocalArmor {
    std::string name;
};
using Input = std::variant<ControlIn, ReliableIn, SnapshotIn, LocalMotion, LocalParam, LocalWeapon, LocalArmor>;

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
struct PlayAvatarParam {  // rejouer un réglage de locomotion du pair sur l'avatar (story 2.4b)
    std::uint32_t net_id = 0;
    protocol::MotionParam param;
};
struct EquipHero {  // invité : équiper son propre héros (story 3.2b), nom par nom
    std::vector<std::string> names;
};
struct EquipAvatar {  // hôte : équiper l'avatar de l'invité, juste après sa création (story 3.2b)
    std::uint32_t net_id = 0;
    std::vector<std::string> names;
};
struct AvatarWeapon {  // faire tenir (nom non vide) ou ranger (nom vide) l'arme de l'avatar (story 3.2)
    std::uint32_t net_id = 0;
    std::string name;
};
struct AvatarArmor {  // équiper (nom non vide) ou retirer (nom vide) l'armure de l'avatar (story 3.2c)
    std::uint32_t net_id = 0;
    std::string name;
};
// Répliques des PNJ, côté invité (story 3.3) : exécutées par sync/npc.
struct SpawnNpc {  // apparier le PNJ natif de ce GUID, l'inscrire comme réplique (IA figée), le placer
    std::uint32_t net_id = 0;
    protocol::Guid guid{};
    Pose pose;
};
struct MoveNpc {  // placer la réplique (Entity::MoveTo), à chaque tick
    std::uint32_t net_id = 0;
    Pose pose;
};
struct SetNpcHp {  // écrire ses PV (SetHitPoints(pv, fixer)), quand la valeur affichée change
    std::uint32_t net_id = 0;
    int hp = 0;
};
struct ReleaseNpc {  // la sortir du registre (IA rendue) et l'oublier ; jamais détruite
    std::uint32_t net_id = 0;
};
using Command = std::variant<Start, Send, OpenReliable, SendReliable, CloseReliable, Stop, Teleport, SpawnAvatar,
                             DespawnAvatar, SendSnapshot, MoveAvatar, PlayAvatarMotion, AvatarDeath, PlayAvatarParam,
                             AvatarWeapon, EquipHero, EquipAvatar, AvatarArmor, SpawnNpc, MoveNpc, SetNpcHp,
                             ReleaseNpc>;

inline constexpr float kParamEpsilon = 0.01f;       // écart minimal pour renvoyer un poids
inline constexpr float kParamSpeedEpsilon = 0.02f;  // écart minimal pour renvoyer une vitesse de lecture
inline constexpr float kParamTimeEpsilon = 0.005f;  // écart minimal de durée de fondu

inline constexpr std::size_t kMaxPendingMotions = 256;  // Motion du pair en attente d'affichage
inline constexpr std::size_t kMaxPendingCarried = 16;   // Weapon et Armor du pair en attente d'affichage
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
    // Hôte en session dont le prochain tick à `now_ms` enverra un snapshot : l'appelant lit
    // alors les PNJ (World::npcs), et seulement alors (story 3.3).
    bool wants_npcs(std::uint64_t now_ms) const;
    // Hôte : PNJ annoncés à l'invité. Invité : répliques suivies.
    std::size_t npc_count() const { return settings_.role == Role::Host ? npc_active_.size() : replicas_.size(); }
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
    void on_local_param(std::uint64_t now, const LocalParam& in, Output& out);
    void on_peer_param(const protocol::MotionParam& p);
    // Objet tenu ou porté (arme, story 3.2 ; armure, story 3.2c) : mêmes règles d'émission et
    // de réception, un état par type.
    enum class Carried : std::uint8_t { Weapon = 0, Armor = 1 };
    static constexpr std::size_t kCarriedCount = 2;
    void on_local_carried(Carried kind, Output& out);
    void send_carried(Carried kind, Output& out);
    void on_peer_carried(Carried kind, std::uint32_t net_id, const std::string& name, Output& out);
    void apply_due_carried(std::uint64_t now, Output& out);
    void reset_carried_sent();
    bool accepts_peer_animation(std::uint32_t net_id) const;
    void play_due_motions(std::uint64_t now, Output& out);
    void clear_peer_avatar_state();
    void send_snapshot(std::uint64_t now, const World& world, Output& out);
    // Story 3.3. Hôte : sélection, NetId, Spawn{1}/Despawn, entrées du snapshot.
    void select_npcs(const World& world, std::vector<protocol::SnapshotEntity>& entities, Output& out);
    // Invité : Spawn{1}, Despawn d'un PNJ, entité de snapshot, placement et PV à chaque tick.
    void on_npc_spawn(const protocol::Spawn& sp, Output& out);
    void on_npc_despawn(std::uint32_t net_id, Output& out);
    bool on_npc_entity(std::uint32_t tick, const protocol::SnapshotEntity& e);
    void place_npcs(std::uint64_t now, Output& out);
    void release_npcs(const std::string& why, Output& out);  // invité : toutes ; hôte : oubli des NetId
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
    // Événement d'animation du pair en attente : Motion (avec son nom) ou MotionParam.
    struct PendingMotion {
        bool is_param = false;
        protocol::Motion motion;
        std::string name;
        protocol::MotionParam param;
        std::uint32_t ms() const { return is_param ? param.ms : motion.ms; }
    };
    // Émetteur : dernier réglage envoyé par (type, nature) ; invalidé par un Motion du type.
    struct LastParam {
        bool valid = false;
        float value = 0.0f;
        float time = 0.0f;
    };
    LastParam last_params_[protocol::kMotionTypeCount][protocol::kMotionParamKindCount];
    std::deque<PendingMotion> pending_;
    bool avatar_dead_ = false;   // Death appliqué (AvatarDeath émis)
    bool death_pending_ = false; // Death reçu, appliqué quand le temps affiché atteint death_at_
    double death_at_ = 0.0;      // dernier temps distant connu à la réception (dernier snapshot)
    struct MotionCounters {
        unsigned sent = 0, played = 0, ignored = 0, superseded = 0, dropped = 0;
        unsigned params_sent = 0, params_played = 0, params_ignored = 0;
    } anim_;

    // Arme (story 3.2) et armure (story 3.2c), indexées par Carried. Héros local : dernier nom
    // connu (gardé d'une session à l'autre : c'est l'état du héros) ; dernier nom envoyé au pair,
    // remis à zéro à chaque entrée en session.
    struct CarriedState {
        bool known = false;
        std::string local;
        bool sent_valid = false;
        std::string sent;
    };
    CarriedState carried_[kCarriedCount];
    struct PendingCarried {
        double at = 0.0;  // temps distant à atteindre (dernier snapshot connu à la réception)
        Carried kind = Carried::Weapon;
        std::string name;
    };
    std::deque<PendingCarried> pending_carried_;  // pair, ordre d'arrivée : vidée au retrait de l'avatar

    // Équipement (story 3.2b). Invité : noms de sa config (vides si invalides), Warn en attente
    // jusqu'au premier tick. Hôte : noms du Hello accepté, gardés jusqu'à Ended.
    std::vector<std::string> own_equipment_;
    std::string equipment_warning_;
    std::vector<std::string> peer_equipment_;

    // Répliques des PNJ (story 3.3), remises à zéro à chaque entrée en session et à Ended.
    // Hôte : table GUID → NetId de la session (jamais réutilisé), NetId → GUID, NetId annoncés.
    std::map<protocol::Guid, std::uint32_t> npc_ids_;
    std::unordered_map<std::uint32_t, protocol::Guid> npc_guids_;
    std::set<std::uint32_t> npc_active_;
    std::unordered_map<std::uint32_t, std::uint32_t> npc_absent_ticks_;  // NetId annoncé absent de la liste
    std::uint32_t next_npc_id_ = protocol::kFirstNpcNetId;
    bool avatar_pose_ok_ = false;  // hôte : dernière pose affichée de l'avatar (rayon)
    Pose avatar_pose_;
    // Invité : NetId → réplique (ordre stable des commandes).
    struct Replica {
        core::Interpolator interp;
        std::deque<std::pair<double, std::uint16_t>> hp_changes;  // (temps distant, PV), au changement
        bool hp_received = false;
        std::uint16_t last_hp = 0;  // derniers PV reçus
        int applied_hp = -1;        // derniers PV écrits (SetNpcHp)
    };
    std::map<std::uint32_t, Replica> replicas_;
    struct NpcCounters {
        unsigned spawned = 0, despawned = 0, unreadable = 0, unknown = 0, capped = 0, routine_blocked = 0;
    } npc_;

    std::uintptr_t armed_player_ = 0;  // joueur à l'armement (fin de monde s'il change)
    std::uint64_t linger_until_ = 0;   // Ended : fin du maintien du KCP
    bool wait_new_world_ = false;      // Inactive : attendre que le monde change
};

}  // namespace mp::core::session
