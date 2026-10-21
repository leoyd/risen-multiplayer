#include "core/session.h"

#include "core/equipment.h"
#include "core/protocol.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

namespace mp::core::session {

namespace {

namespace proto = mp::protocol;

std::string hex32(std::uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%08x", v);
    return buf;
}

std::string pose_text(float x, float y, float z, float yaw) {
    char buf[128];
    std::snprintf(buf, sizeof buf, "(%.1f, %.1f, %.1f) lacet=%.3f", static_cast<double>(x),
                  static_cast<double>(y), static_cast<double>(z), static_cast<double>(yaw));
    return buf;
}

// « session pleine (4) », « raison inconnue (200) ».
std::string reason_text(const char* name, std::uint8_t reason) {
    const std::string n(name);
    return (n == "inconnue" ? "raison inconnue" : n) + " (" + std::to_string(reason) + ")";
}

std::string reject_text(std::uint8_t reason) { return reason_text(proto::reject_reason_name(reason), reason); }
std::string end_text(std::uint8_t reason) { return reason_text(proto::end_reason_name(reason), reason); }

// Ligne de diagnostic d'un Motion (debug seulement).
std::string motion_text(const proto::Motion& m) {
    char buf[160];
    std::snprintf(buf, sizeof buf, "ani %u type %u ms %u flags %u d00 %.2f d0C %.3f d10 %d d14 %.3f",
                  static_cast<unsigned>(m.ani_id), static_cast<unsigned>(m.type), m.ms, static_cast<unsigned>(m.flags),
                  static_cast<double>(m.d00), static_cast<double>(m.d0c), static_cast<int>(m.d10),
                  static_cast<double>(m.d14));
    return buf;
}

std::string net_id_text(std::uint32_t id) {
    return id == proto::kHostNetId ? "1 (hôte)" : id == proto::kGuestNetId ? "2 (invité)" : std::to_string(id);
}

std::string guid_text(const proto::Guid& g) {
    char buf[2 * proto::kGuidSize + 1];
    for (std::size_t i = 0; i < g.size(); ++i) std::snprintf(buf + 2 * i, 3, "%02x", static_cast<unsigned>(g[i]));
    return buf;
}

bool finite_pose(float x, float y, float z, float yaw) {
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z) && std::isfinite(yaw);
}

float distance(const Pose& a, const Pose& b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

std::uint16_t clamp_hp(int hp) {
    return static_cast<std::uint16_t>(hp < 0 ? 0 : hp > 0xFFFF ? 0xFFFF : hp);
}

}  // namespace

Pose guest_pose_near(const Pose& host) {
    constexpr float kPi = 3.14159265358979323846f;
    Pose p;
    p.x = host.x + kGuestSpawnDistance * std::sin(host.yaw);
    p.y = host.y;
    p.z = host.z + kGuestSpawnDistance * std::cos(host.yaw);
    p.yaw = host.yaw + kPi;
    if (p.yaw > kPi) p.yaw -= 2.0f * kPi;
    return p;
}

const char* state_name(State state) {
    switch (state) {
        case State::Inactive: return "Inactive";
        case State::Hosting: return "Hosting";
        case State::Connecting: return "Connecting";
        case State::InSession: return "InSession";
        case State::Ended: return "Ended";
    }
    return "?";
}

Session::Session(Settings settings) : settings_(std::move(settings)) {
    // splitmix64 : la graine 0 reste utilisable.
    rng_ = settings_.seed;
    // Story 3.2b : l'hôte ignore sa propre guest_equipment ; l'invité envoie la forme canonique.
    if (settings_.role == Role::Guest) {
        std::string error;
        if (!parse_equipment(settings_.equipment, own_equipment_, error)) {
            equipment_warning_ = "[sync] équipement « " + settings_.equipment + " » invalide (" + error +
                                 ") : traité comme vide";
        }
        settings_.equipment = join_equipment(own_equipment_);
    } else {
        settings_.equipment.clear();
    }
}

std::uint32_t Session::draw_conv() {
    for (;;) {
        rng_ += 0x9e3779b97f4a7c15ull;
        std::uint64_t z = rng_;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        z ^= z >> 31;
        const auto conv = static_cast<std::uint32_t>(z ^ (z >> 32));
        if (conv != 0) return conv;  // §8 : conv aléatoire non nul
    }
}

void Session::transition(State next, const std::string& detail, Output& out, Level level) {
    std::string line = std::string("[sync] ") + state_name(state_) + " → " + state_name(next);
    if (!detail.empty()) line += " : " + detail;
    state_ = next;
    out.logs.push_back({level, std::move(line)});
}

void Session::end(std::uint64_t now, const std::string& reason, std::uint8_t local_reason, bool rearm_now,
                  Level level, Output& out) {
    if (state_ == State::Ended || state_ == State::Inactive) return;
    if (state_ == State::InSession && local_reason != 0) {
        Bytes bytes;
        if (proto::encode_reliable(proto::Despawn{own_net_id()}, bytes)) {
            out.commands.push_back(SendReliable{std::move(bytes)});
            out.logs.push_back({Level::Info, "[sync] Despawn envoyé : NetId " + net_id_text(own_net_id())});
        }
        if (proto::encode_reliable(proto::SessionEnd{local_reason}, bytes)) {
            out.commands.push_back(SendReliable{std::move(bytes)});
            out.logs.push_back({Level::Info, "[sync] SessionEnd envoyé : " + end_text(local_reason)});
        }
    }
    transition(State::Ended, reason + " ; retour au solo", out, level);
    remove_avatar("fin de session", out);  // AD-11 : la transition unique vers Ended retire l'avatar
    release_npcs("fin de session", out);   // story 3.3 : toutes les répliques rendent leur IA
    pending_carried_.clear();              // stories 3.2 et 3.2c : rien ne survit à la session
    peer_equipment_.clear();               // story 3.2b
    reset_carried_sent();
    linger_until_ = now + kLingerMs;  // le KCP reste ouvert pour laisser partir le SessionEnd
    wait_new_world_ = !rearm_now;
    track_world(world_, false);  // fin de monde : le monde a déjà changé
}

void Session::remove_avatar(const std::string& why, Output& out) {
    if (!avatar_present_) return;
    avatar_present_ = false;
    avatar_pose_ok_ = false;
    clear_peer_avatar_state();
    out.commands.push_back(DespawnAvatar{peer_net_id()});
    out.logs.push_back({Level::Info, "[sync] avatar " + net_id_text(peer_net_id()) + " retiré : " + why});
}

std::uint32_t Session::own_net_id() const {
    return settings_.role == Role::Host ? proto::kHostNetId : proto::kGuestNetId;
}

std::uint32_t Session::peer_net_id() const {
    return settings_.role == Role::Host ? proto::kGuestNetId : proto::kHostNetId;
}

void Session::finish_linger(const World& world, Output& out) {
    out.commands.push_back(CloseReliable{});
    out.commands.push_back(Stop{});
    transition(State::Inactive,
               can_arm(world) ? "transport arrêté ; réarmement immédiat"
                              : "transport arrêté ; nouvelle session au prochain monde chargé",
               out);
}

Output Session::fail(std::uint64_t now_ms, const std::string& cause) {
    Output out;
    if (state_ == State::Inactive || state_ == State::Ended) return out;
    out.logs.push_back({Level::Error, "[sync] " + cause});
    end(now_ms, "échec : " + cause, static_cast<std::uint8_t>(proto::EndReason::InternalError), false, Level::Warn,
        out);
    return out;
}

void Session::track_world(const World& world, bool revived) {
    // Le monde a changé depuis l'armement (menu, chargement, autre joueur) : le prochain
    // monde chargé est forcément un nouveau monde. Repli : le héros mort revit (rechargement
    // qui redonne la même adresse sans que `running` retombe).
    if (wait_new_world_ && (!world.loaded() || world.player != armed_player_ || revived)) wait_new_world_ = false;
}

bool Session::can_arm(const World& world) const {
    // Un hôte au héros mort n'arme pas : il finirait aussitôt en « mort de l'hôte ».
    return !disabled_ && !wait_new_world_ && world.loaded() && !(settings_.role == Role::Host && world.dead);
}

void Session::maybe_arm(std::uint64_t now, const World& world, Output& out) {
    if (can_arm(world)) arm(now, world, out);
}

Output Session::tick(std::uint64_t now, const World& world, const std::vector<Input>& inputs) {
    Output out;
    if (!equipment_warning_.empty()) {
        out.logs.push_back({Level::Warn, std::move(equipment_warning_)});
        equipment_warning_.clear();
    }
    const bool revived = world_.dead && !world.dead;  // world_ = monde du tick précédent
    npc_.routine_blocked += world.routine_blocked;     // story 3.3 : bilan debug
    world_ = world;
    // Arme et armure du héros local : retenues dans tous les états (envoyées à l'entrée en session).
    for (const Input& in : inputs) {
        if (const auto* lw = std::get_if<LocalWeapon>(&in)) {
            carried_[static_cast<std::size_t>(Carried::Weapon)].known = true;
            carried_[static_cast<std::size_t>(Carried::Weapon)].local = lw->name;
        } else if (const auto* la = std::get_if<LocalArmor>(&in)) {
            carried_[static_cast<std::size_t>(Carried::Armor)].known = true;
            carried_[static_cast<std::size_t>(Carried::Armor)].local = la->name;
        }
    }
    if (state_ == State::Ended) {
        track_world(world, revived);
        if (now < linger_until_) return out;  // maintien : entrées et monde sans objet
        finish_linger(world, out);
        // Les entrées de ce tick viennent du transport qu'on vient d'arrêter : ignorées.
        maybe_arm(now, world, out);
        timers(now, out);
        return out;
    }
    if (state_ == State::Inactive) {
        track_world(world, revived);
        maybe_arm(now, world, out);
        if (state_ == State::Inactive) return out;  // entrées éventuelles sans objet
    }
    // Signaux du monde, lus à chaque tick, avant les entrées : une fin de monde l'emporte sur
    // un SessionEnd arrivé dans le même tick (entrées alors ignorées).
    if (!world.loaded() || world.player != armed_player_) {
        end(now, "fin de monde", static_cast<std::uint8_t>(proto::EndReason::WorldEnded), false, Level::Info, out);
        return out;
    }
    if (settings_.role == Role::Host && world.dead &&
        (state_ == State::Hosting || state_ == State::InSession)) {
        end(now, "mort de l'hôte", static_cast<std::uint8_t>(proto::EndReason::HostDied), false, Level::Info, out);
        return out;
    }
    for (const Input& in : inputs) {
        if (state_ == State::Ended) break;
        if (const auto* c = std::get_if<ControlIn>(&in)) {
            on_control(now, *c, out);
        } else if (const auto* r = std::get_if<ReliableIn>(&in)) {
            on_reliable(now, *r, out);
        } else if (const auto* sn = std::get_if<SnapshotIn>(&in)) {
            on_snapshot(now, *sn, out);
        } else if (const auto* lm = std::get_if<LocalMotion>(&in)) {
            on_local_motion(now, *lm, out);
        } else if (std::holds_alternative<LocalWeapon>(in)) {
            on_local_carried(Carried::Weapon, out);
        } else if (std::holds_alternative<LocalArmor>(in)) {
            on_local_carried(Carried::Armor, out);
        } else {
            on_local_param(now, std::get<LocalParam>(in), out);
        }
    }
    if (state_ != State::Ended) timers(now, out);
    if (state_ == State::InSession) {
        send_death(world, out);
        send_snapshot(now, world, out);
        place_avatar(now, out);
        place_npcs(now, out);
        snapshot_stats(now, out);
    }
    return out;
}

void Session::arm(std::uint64_t now, const World& world, Output& out) {
    // Nouvelle session : rien ne survit de la précédente (le conv sera tiré à neuf).
    armed_player_ = world.player;
    peer_ = Endpoint{};
    conv_ = 0;
    welcome_.clear();
    reliable_seen_ = false;
    heartbeats_rx_ = 0;
    foreign_warned_ = false;
    avatar_present_ = false;  // déjà retiré par end() ; par sûreté
    if (settings_.role == Role::Host) {
        out.commands.push_back(Start{settings_.port});
        transition(State::Hosting,
                   "monde chargé, en attente d'un invité sur le port " + std::to_string(settings_.port), out);
        return;
    }
    // Invité : IPv4 littérale seulement, pas de résolution de nom.
    std::uint32_t ip = 0;
    if (!net::parse_ipv4(settings_.host_ip, ip)) {
        out.logs.push_back({Level::Error, "[sync] host_ip « " + settings_.host_ip +
                                              " » : IPv4 attendue (ex. 192.168.1.20 ; ni nom d'hôte ni IPv6) ; "
                                              "pas de session, le jeu reste en solo"});
        disabled_ = true;
        return;
    }
    proto::Hello hello;
    hello.mod_version = settings_.mod_version;
    hello.fingerprint = settings_.fingerprint;
    hello.nickname = settings_.nickname;
    hello.equipment = settings_.equipment;
    if (!proto::encode_control(hello, hello_)) {
        out.logs.push_back({Level::Error,
                            "[sync] Hello non encodable (pseudo, équipement ou version du mod hors bornes) ; "
                            "pas de session, le jeu reste en solo"});
        disabled_ = true;
        return;
    }
    host_ = Endpoint{ip, settings_.port};
    out.commands.push_back(Start{0});
    transition(State::Connecting, "monde chargé, connexion à " + net::to_string(host_), out);
    connect_deadline_ = now + kConnectTimeoutMs;
    next_hello_ = now;  // premier Hello dans ce tick (timers)
}

void Session::on_control(std::uint64_t now, const ControlIn& in, Output& out) {
    if (settings_.role == Role::Host) {
        host_on_hello(now, in, out);
    } else {
        guest_on_control(now, in, out);
    }
}

void Session::send_reject(const Endpoint& to, std::uint8_t reason, Output& out) {
    Bytes bytes;
    if (!proto::encode_control(proto::Reject{reason}, bytes)) return;
    out.commands.push_back(Send{to, std::move(bytes)});
    out.logs.push_back({Level::Info, "[sync] Reject envoyé à " + net::to_string(to) + " : " + reject_text(reason)});
}

void Session::host_on_hello(std::uint64_t now, const ControlIn& in, Output& out) {
    const std::uint8_t* data = in.bytes.data();
    const std::size_t size = in.bytes.size();
    const bool from_peer = state_ == State::InSession && in.source == peer_;
    if (from_peer) last_peer_ = now;  // activité du pair : tout événement venu de lui

    std::uint16_t version = 0;
    if (!proto::peek_hello_version(data, size, version)) {
        // Pas un Hello : Welcome ou Reject reçus par l'hôte, datagramme illisible… ignorés.
        const bool hello_type = size >= 2 && data[0] == static_cast<std::uint8_t>(proto::Channel::Control) &&
                                data[1] == static_cast<std::uint8_t>(proto::MsgType::Hello);
        if (hello_type && !from_peer) {
            out.logs.push_back({Level::Info, "[sync] Hello reçu de " + net::to_string(in.source) + " : trop court"});
            send_reject(in.source, static_cast<std::uint8_t>(proto::RejectReason::BadHello), out);
        } else {
            out.logs.push_back({Level::Debug, "[sync] contrôle ignoré de " + net::to_string(in.source)});
        }
        return;
    }

    // Vérifications dans l'ordre du §6 (version, décodage, mod, empreintes).
    const std::string who = net::to_string(in.source);
    std::vector<LogLine> details;
    std::uint8_t refusal = 0;
    proto::Hello hello;
    if (version != proto::kProtocolVersion) {
        details.push_back({Level::Info, "[sync] Hello reçu de " + who + " : version du protocole " +
                                            std::to_string(version) + " (attendue " +
                                            std::to_string(proto::kProtocolVersion) + ")"});
        refusal = static_cast<std::uint8_t>(proto::RejectReason::Protocol);
    } else {
        const auto decoded = proto::decode_control(data, size);
        const proto::Hello* h = decoded.ok() ? std::get_if<proto::Hello>(&decoded.message) : nullptr;
        if (h == nullptr) {
            details.push_back({Level::Info, "[sync] Hello reçu de " + who + " : illisible (" +
                                                proto::decode_error_name(decoded.error) + ")"});
            refusal = static_cast<std::uint8_t>(proto::RejectReason::BadHello);
        } else {
            hello = *h;
            details.push_back({Level::Info, "[sync] Hello reçu de " + who + " : pseudo « " + hello.nickname +
                                                " », mod « " + hello.mod_version + " »"});
            static const char* const kModules[] = {"Game", "Engine", "Script", "SharedBase"};
            if (hello.mod_version != settings_.mod_version) {
                details.push_back({Level::Info, "[sync] version du mod de l'invité « " + hello.mod_version +
                                                    " », la nôtre « " + settings_.mod_version + " »"});
                refusal = static_cast<std::uint8_t>(proto::RejectReason::ModVersion);
            } else {
                for (std::size_t i = 0; i < settings_.fingerprint.size(); ++i) {
                    if (!(hello.fingerprint[i] == settings_.fingerprint[i])) {
                        details.push_back({Level::Info, std::string("[sync] empreinte ") + kModules[i] +
                                                            ".dll de l'invité : " +
                                                            format_fingerprint(hello.fingerprint[i]) + ", la nôtre : " +
                                                            format_fingerprint(settings_.fingerprint[i])});
                        refusal = static_cast<std::uint8_t>(proto::RejectReason::GameFingerprint);
                        break;
                    }
                }
            }
        }
    }

    if (from_peer) {
        // Pair accepté : même Welcome tant qu'aucun message fiable n'est arrivé de lui,
        // ensuite ignoré. Un Hello invalide venu de lui est ignoré aussi (§8).
        if (reliable_seen_) {
            out.logs.push_back({Level::Info, "[sync] Hello reçu de " + who + " : ignoré (KCP déjà établi)"});
        } else if (refusal != 0) {
            out.logs.push_back({Level::Info, "[sync] Hello invalide du pair " + who + " : ignoré (" +
                                                 reject_text(refusal) + ")"});
        } else {
            out.commands.push_back(Send{in.source, welcome_});
            out.logs.push_back({Level::Info, "[sync] Hello retransmis par " + who +
                                                 " : même Welcome renvoyé (conv=" + hex32(conv_) + ")"});
        }
        return;
    }

    for (auto& line : details) out.logs.push_back(std::move(line));
    if (refusal == 0 && state_ != State::Hosting) refusal = static_cast<std::uint8_t>(proto::RejectReason::SessionFull);
    if (refusal != 0) {
        send_reject(in.source, refusal, out);
        return;
    }

    // Accepté : conv neuf, Welcome gardé pour les retransmissions, KCP ouvert.
    proto::Welcome welcome;
    welcome.conv = draw_conv();
    if (world_.pose_ok) {
        welcome.x = world_.x;
        welcome.y = world_.y;
        welcome.z = world_.z;
        welcome.yaw = world_.yaw;
    } else {
        out.logs.push_back({Level::Warn, "[sync] pose du joueur illisible : Welcome envoyé avec une pose nulle"});
    }
    Bytes bytes;
    if (!proto::encode_control(welcome, bytes)) {  // impossible en pratique (22 octets)
        out.logs.push_back({Level::Error, "[sync] Welcome non encodable"});
        return;
    }
    welcome_ = bytes;
    conv_ = welcome.conv;
    peer_ = in.source;
    // Équipement de l'invité (story 3.2b) : appliqué à son avatar à chaque création.
    std::string equipment_error;
    if (!parse_equipment(hello.equipment, peer_equipment_, equipment_error)) {
        out.logs.push_back({Level::Warn, "[sync] équipement de l'invité « " + hello.equipment + " » invalide (" +
                                             equipment_error + ") : avatar non équipé"});
    } else if (!peer_equipment_.empty()) {
        out.logs.push_back({Level::Info, "[sync] équipement de l'invité : " + join_equipment(peer_equipment_)});
    }
    reliable_seen_ = false;
    out.commands.push_back(Send{in.source, std::move(bytes)});
    out.commands.push_back(OpenReliable{conv_, peer_});
    transition(State::InSession, "invité " + who + " « " + hello.nickname + " » accepté, conv=" + hex32(conv_) +
                                     ", pose envoyée " + pose_text(welcome.x, welcome.y, welcome.z, welcome.yaw),
               out);
    enter_session(now, Pose{welcome.x, welcome.y, welcome.z, welcome.yaw}, out);
}

void Session::guest_on_control(std::uint64_t now, const ControlIn& in, Output& out) {
    if (in.source != host_) {
        if (!foreign_warned_) {
            foreign_warned_ = true;
            out.logs.push_back({Level::Warn, "[sync] datagramme de contrôle reçu de " + net::to_string(in.source) +
                                                 " au lieu de l'hôte " + net::to_string(host_) +
                                                 " : ignoré (NAT ou hôte à plusieurs adresses ?)"});
            return;
        }
        out.logs.push_back({Level::Debug, "[sync] contrôle ignoré de " + net::to_string(in.source) + " (pas l'hôte)"});
        return;
    }
    if (state_ == State::InSession) last_peer_ = now;
    const auto decoded = proto::decode_control(in.bytes.data(), in.bytes.size());
    if (!decoded.ok()) {
        out.logs.push_back({Level::Info, std::string("[sync] contrôle illisible de l'hôte : ") +
                                             proto::decode_error_name(decoded.error)});
        return;
    }
    if (const auto* w = std::get_if<proto::Welcome>(&decoded.message)) {
        if (state_ != State::Connecting) return;  // Welcome reçu en session : ignoré (§8)
        if (w->conv == 0) {
            out.logs.push_back({Level::Warn, "[sync] Welcome avec conv nul (interdit par le §8) : ignoré"});
            return;
        }
        conv_ = w->conv;
        peer_ = host_;
        out.commands.push_back(OpenReliable{conv_, peer_});
        transition(State::InSession, "Welcome de " + net::to_string(host_) + ", conv=" + hex32(conv_) +
                                         ", pose de l'hôte " + pose_text(w->x, w->y, w->z, w->yaw),
                   out);
        // Pose nulle : l'hôte n'a pas pu lire la sienne (§8). On ne téléporte pas à l'origine.
        Pose own{world_.x, world_.y, world_.z, world_.yaw};
        if (w->x == 0.0f && w->y == 0.0f && w->z == 0.0f && w->yaw == 0.0f) {
            out.logs.push_back({Level::Warn, "[sync] pose de l'hôte nulle : héros de l'invité laissé sur place"});
            if (!world_.pose_ok) {
                out.logs.push_back({Level::Warn, "[sync] pose du héros illisible : Spawn envoyé avec une pose nulle"});
            }
        } else {
            own = guest_pose_near(Pose{w->x, w->y, w->z, w->yaw});
            out.commands.push_back(Teleport{own});
            out.logs.push_back({Level::Info, "[sync] héros téléporté devant l'hôte : " +
                                                 pose_text(own.x, own.y, own.z, own.yaw)});
        }
        // Story 3.2b : à chaque entrée en session, après la téléportation.
        if (!own_equipment_.empty()) {
            out.commands.push_back(EquipHero{own_equipment_});
            out.logs.push_back({Level::Info, "[sync] équipement du héros : " + join_equipment(own_equipment_)});
        }
        enter_session(now, own, out);
        return;
    }
    if (const auto* r = std::get_if<proto::Reject>(&decoded.message)) {
        if (state_ != State::Connecting) return;  // Reject reçu en session : ignoré (§8)
        out.logs.push_back({Level::Info, "[sync] Reject reçu de " + net::to_string(host_) + " : " +
                                             reject_text(r->reason)});
        end(now, "rejeté par l'hôte : " + reject_text(r->reason), 0, false, Level::Warn, out);
        return;
    }
    // Hello reçu par l'invité : sans objet.
}

void Session::on_reliable(std::uint64_t now, const ReliableIn& in, Output& out) {
    if (state_ != State::InSession) return;  // KCP fermé : impossible en pratique
    last_peer_ = now;
    reliable_seen_ = true;
    const auto decoded = proto::decode_reliable(in.bytes.data(), in.bytes.size());
    if (!decoded.ok()) {
        out.logs.push_back({Level::Warn, std::string("[sync] message fiable illisible : ") +
                                             proto::decode_error_name(decoded.error)});
        return;
    }
    if (std::holds_alternative<proto::Heartbeat>(decoded.message)) {
        ++heartbeats_rx_;
        out.logs.push_back({Level::Debug, "[sync] Heartbeat reçu (" + std::to_string(heartbeats_rx_) + ")"});
        return;
    }
    if (const auto* m = std::get_if<proto::Motion>(&decoded.message)) {
        on_peer_motion(*m, out);
        return;
    }
    if (const auto* mp = std::get_if<proto::MotionParam>(&decoded.message)) {
        on_peer_param(*mp);
        return;
    }
    if (const auto* an = std::get_if<proto::AniName>(&decoded.message)) {
        auto it = peer_names_.find(an->ani_id);
        if (it != peer_names_.end() && it->second != an->name) {
            out.logs.push_back({Level::Warn, "[sync] AniName " + std::to_string(an->ani_id) + " redéfini : « " +
                                                 it->second + " » → « " + an->name + " »"});
        }
        peer_names_[an->ani_id] = an->name;
        out.logs.push_back({Level::Debug, "[sync] AniName reçu : " + std::to_string(an->ani_id) + " = « " +
                                              an->name + " »"});
        return;
    }
    if (const auto* wp = std::get_if<proto::Weapon>(&decoded.message)) {
        on_peer_carried(Carried::Weapon, wp->net_id, wp->name, out);
        return;
    }
    if (const auto* ar = std::get_if<proto::Armor>(&decoded.message)) {
        on_peer_carried(Carried::Armor, ar->net_id, ar->name, out);
        return;
    }
    if (const auto* de = std::get_if<proto::Death>(&decoded.message)) {
        if (de->net_id != peer_net_id() || !avatar_present_ || avatar_dead_ || death_pending_) {
            out.logs.push_back({Level::Warn, "[sync] Death reçu (NetId " + net_id_text(de->net_id) + ") : ignoré (" +
                                                 (de->net_id != peer_net_id() ? "pas l'avatar du pair"
                                                  : !avatar_present_          ? "pas d'avatar"
                                                                              : "avatar déjà mort") +
                                                 ")"});
            return;
        }
        // Même retard d'affichage que le reste : la mort tombe après les gestes et la pose
        // déjà reçus (dernier snapshot), le coup fatal compris.
        death_pending_ = true;
        death_at_ = interp_.empty() ? 0.0 : interp_.last_time();
        out.logs.push_back({Level::Info, "[sync] Death reçu : avatar " + net_id_text(de->net_id) +
                                             " en ragdoll à l'instant d'affichage"});
        return;
    }
    if (const auto* sp = std::get_if<proto::Spawn>(&decoded.message)) {
        if (sp->type == proto::kSpawnTypeNpc) {  // story 3.3
            on_npc_spawn(*sp, out);
            return;
        }
        const std::string what = "Spawn reçu (NetId " + net_id_text(sp->net_id) + ", type " +
                                 std::to_string(sp->type) + ", " + pose_text(sp->x, sp->y, sp->z, sp->yaw) + ")";
        if (sp->net_id != peer_net_id() || sp->type != proto::kSpawnTypeHero) {
            out.logs.push_back({Level::Warn, "[sync] " + what + " : ignoré (attendu NetId " +
                                                 net_id_text(peer_net_id()) + ", type 0)"});
            return;
        }
        if (!std::isfinite(sp->x) || !std::isfinite(sp->y) || !std::isfinite(sp->z) || !std::isfinite(sp->yaw)) {
            out.logs.push_back({Level::Warn, "[sync] " + what + " : ignoré (pose non finie)"});
            return;
        }
        if (avatar_present_) {
            out.logs.push_back({Level::Warn, "[sync] " + what + " : ignoré (avatar déjà présent)"});
            return;
        }
        avatar_present_ = true;
        interp_.reset();  // nouvel avatar : rien ne survit du précédent (AD-7)
        clear_peer_avatar_state();
        out.commands.push_back(SpawnAvatar{sp->net_id, Pose{sp->x, sp->y, sp->z, sp->yaw}});
        out.logs.push_back({Level::Info, "[sync] " + what + " : avatar créé"});
        if (settings_.role == Role::Host && !peer_equipment_.empty()) {  // story 3.2b
            out.commands.push_back(EquipAvatar{sp->net_id, peer_equipment_});
            out.logs.push_back({Level::Info, "[sync] EquipAvatar : avatar " + net_id_text(sp->net_id) + ", " +
                                                 join_equipment(peer_equipment_)});
        }
        return;
    }
    if (const auto* dp = std::get_if<proto::Despawn>(&decoded.message)) {
        if (dp->net_id >= proto::kFirstNpcNetId) {  // story 3.3
            on_npc_despawn(dp->net_id, out);
            return;
        }
        if (dp->net_id != peer_net_id() || !avatar_present_) {
            out.logs.push_back({Level::Warn, "[sync] Despawn reçu (NetId " + net_id_text(dp->net_id) +
                                                 ") : ignoré (" +
                                                 (dp->net_id != peer_net_id() ? "pas l'avatar du pair" : "pas d'avatar") +
                                                 ")"});
            return;
        }
        remove_avatar("Despawn reçu", out);
        return;
    }
    const auto& e = std::get<proto::SessionEnd>(decoded.message);
    out.logs.push_back({Level::Info, "[sync] SessionEnd reçu : " + end_text(e.reason)});
    // L'hôte dont l'invité part se réarme aussitôt ; l'invité attend le monde suivant.
    const bool routine = e.reason == static_cast<std::uint8_t>(proto::EndReason::PlayerLeft) ||
                         e.reason == static_cast<std::uint8_t>(proto::EndReason::WorldEnded);
    end(now, "fin de session par le pair : " + end_text(e.reason), 0, settings_.role == Role::Host,
        routine ? Level::Info : Level::Warn, out);
}

void Session::enter_session(std::uint64_t now, const Pose& own, Output& out) {
    last_peer_ = now;
    next_heartbeat_ = now;  // premier heartbeat dès l'entrée en session
    heartbeats_rx_ = 0;
    t0_ = now;
    // Pas de snapshot au tick 0 : chez l'invité, la Teleport de ce tick n'est pas encore
    // exécutée, et la pose lue serait celle d'avant (l'avatar sauterait chez l'hôte).
    last_tick_sent_ = 0;
    clock_.reset();
    interp_.reset();
    next_stats_ = now + kSnapshotStatsMs;
    snap_ = SnapshotCounters{};
    ani_ids_.clear();
    next_ani_id_ = 0;
    for (auto& per_type : last_params_) {
        for (LastParam& lp : per_type) lp = LastParam{};
    }
    ani_full_logged_ = false;
    death_sent_ = false;
    peer_names_.clear();
    warned_ani_ids_.clear();
    clear_peer_avatar_state();
    anim_ = MotionCounters{};
    reset_carried_sent();
    npc_ids_.clear();  // story 3.3 : NetId de PNJ propres à la session
    npc_guids_.clear();
    npc_active_.clear();
    npc_absent_ticks_.clear();
    next_npc_id_ = proto::kFirstNpcNetId;
    replicas_.clear();  // vide en pratique : libérées à Ended
    npc_ = NpcCounters{};
    avatar_pose_ok_ = false;
    proto::Spawn spawn;
    spawn.net_id = own_net_id();
    spawn.x = own.x;
    spawn.y = own.y;
    spawn.z = own.z;
    spawn.yaw = own.yaw;
    Bytes bytes;
    if (proto::encode_reliable(spawn, bytes)) {
        out.commands.push_back(SendReliable{std::move(bytes)});
        out.logs.push_back({Level::Info, "[sync] Spawn envoyé : NetId " + net_id_text(spawn.net_id) + ", " +
                                             pose_text(own.x, own.y, own.z, own.yaw)});
    }
    // Arme puis armure déjà connues (vides comprises) : après le Spawn, pour que l'avatar
    // apparaisse armé et vêtu.
    if (carried_[static_cast<std::size_t>(Carried::Weapon)].known) send_carried(Carried::Weapon, out);
    if (carried_[static_cast<std::size_t>(Carried::Armor)].known) send_carried(Carried::Armor, out);
}

void Session::on_snapshot(std::uint64_t now, const SnapshotIn& in, Output& /*out*/) {
    if (state_ != State::InSession) return;
    last_peer_ = now;
    const auto decoded = proto::decode_snapshot(in.bytes.data(), in.bytes.size());  // ≤ 54 entités
    if (!decoded.ok()) {
        ++snap_.unreadable;
        return;
    }
    ++snap_.received;
    clock_.observe(now, decoded.message.tick);
    for (const proto::SnapshotEntity& e : decoded.message.entities) {
        if (!finite_pose(e.x, e.y, e.z, e.yaw)) {
            ++snap_.ignored;
            continue;
        }
        if (e.net_id == peer_net_id()) {
            interp_.push(decoded.message.tick, Pose{e.x, e.y, e.z, e.yaw});
            continue;
        }
        if (!on_npc_entity(decoded.message.tick, e)) ++snap_.ignored;
    }
}

bool Session::wants_npcs(std::uint64_t now) const {
    if (settings_.role != Role::Host || state_ != State::InSession || now < t0_) return false;
    return static_cast<std::int64_t>((now - t0_) / proto::kSnapshotTickMs) != last_tick_sent_;
}

void Session::select_npcs(const World& world, std::vector<proto::SnapshotEntity>& entities, Output& out) {
    if (!world.npcs_ok) {  // liste illisible : rien ne change, le snapshot ne porte que le héros
        ++npc_.unreadable;
        return;
    }
    const Pose hero{world.x, world.y, world.z, world.yaw};
    struct Candidate {
        const NpcState* npc;
        float d;
    };
    std::vector<Candidate> candidates;
    // Pourquoi un PNJ annoncé sort (journal) : par GUID, l'état lu à ce tick.
    std::map<proto::Guid, const char*> outside;
    for (const NpcState& n : world.npcs) {
        if (!n.alive) {
            outside.emplace(n.guid, "mort");
            continue;
        }
        if (!finite_pose(n.pose.x, n.pose.y, n.pose.z, n.pose.yaw)) continue;
        float d = distance(n.pose, hero);
        if (avatar_present_ && avatar_pose_ok_) d = std::min(d, distance(n.pose, avatar_pose_));
        if (!(d <= kNpcRadius)) {
            outside.emplace(n.guid, "hors du rayon");
            continue;
        }
        candidates.push_back({&n, d});
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        return a.d < b.d || (a.d == b.d && a.npc->guid < b.npc->guid);
    });
    // Un GUID en double (copie d'un PNJ ?) : seul le plus proche compte.
    std::set<proto::Guid> seen;
    std::vector<Candidate> kept;
    for (const Candidate& c : candidates) {
        if (!seen.insert(c.npc->guid).second) continue;
        if (kept.size() >= kMaxSnapshotNpcs) {
            outside.emplace(c.npc->guid, "évincé par plus proche (54 entités au plus)");
            ++npc_.capped;
            continue;
        }
        kept.push_back(c);
    }
    std::set<std::uint32_t> retained;
    std::vector<std::uint32_t> ids;
    ids.reserve(kept.size());
    for (const Candidate& c : kept) {
        auto it = npc_ids_.find(c.npc->guid);
        std::uint32_t id = 0;
        if (it != npc_ids_.end()) {
            id = it->second;
        } else {
            id = next_npc_id_++;  // jamais réutilisé pendant la session
            npc_ids_.emplace(c.npc->guid, id);
            npc_guids_.emplace(id, c.npc->guid);
        }
        retained.insert(id);
        ids.push_back(id);
    }
    // Sorties d'abord, puis entrées : tout part avant le snapshot.
    for (auto it = npc_active_.begin(); it != npc_active_.end();) {
        if (retained.count(*it) != 0) {
            npc_absent_ticks_.erase(*it);
            ++it;
            continue;
        }
        const std::uint32_t id = *it;
        const auto why = outside.find(npc_guids_[id]);
        // Absent de la liste (limite de traitement du moteur) : délai de grâce avant le retrait.
        if (why == outside.end() && ++npc_absent_ticks_[id] < kNpcAbsentGraceTicks) {
            ++it;
            continue;
        }
        npc_absent_ticks_.erase(id);
        it = npc_active_.erase(it);
        Bytes bytes;
        if (proto::encode_reliable(proto::Despawn{id}, bytes)) out.commands.push_back(SendReliable{std::move(bytes)});
        ++npc_.despawned;
        out.logs.push_back({Level::Info, "[sync] PNJ " + std::to_string(id) + " retiré (Despawn) : " +
                                             (why != outside.end() ? why->second : "absent de la liste depuis 2 s")});
    }
    for (std::size_t i = 0; i < kept.size(); ++i) {
        const NpcState& n = *kept[i].npc;
        const std::uint32_t id = ids[i];
        if (npc_active_.insert(id).second) {
            proto::Spawn sp;
            sp.net_id = id;
            sp.type = proto::kSpawnTypeNpc;
            sp.x = n.pose.x;
            sp.y = n.pose.y;
            sp.z = n.pose.z;
            sp.yaw = n.pose.yaw;
            sp.guid = n.guid;
            Bytes bytes;
            if (proto::encode_reliable(sp, bytes)) out.commands.push_back(SendReliable{std::move(bytes)});
            ++npc_.spawned;
            char dist[32];
            std::snprintf(dist, sizeof dist, "%.1f m", static_cast<double>(kept[i].d) / 100.0);
            out.logs.push_back({Level::Info, "[sync] PNJ " + std::to_string(id) + " annoncé (Spawn) : GUID " +
                                                 guid_text(n.guid) + ", " + dist + ", PV " +
                                                 std::to_string(clamp_hp(n.hp)) + ", " +
                                                 pose_text(n.pose.x, n.pose.y, n.pose.z, n.pose.yaw)});
            // Porté à partir du snapshot suivant : le Spawn passe par KCP (jusqu'à 10 ms) et le
            // snapshot part aussitôt ; l'invité ignorerait l'entité, faute d'annonce.
            continue;
        }
        proto::SnapshotEntity e;
        e.net_id = id;
        e.x = n.pose.x;
        e.y = n.pose.y;
        e.z = n.pose.z;
        e.yaw = n.pose.yaw;
        e.hp = clamp_hp(n.hp);
        entities.push_back(e);
    }
}

void Session::on_npc_spawn(const proto::Spawn& sp, Output& out) {
    const std::string what = "[sync] PNJ " + std::to_string(sp.net_id) + " : Spawn reçu (GUID " + guid_text(sp.guid) +
                             ", " + pose_text(sp.x, sp.y, sp.z, sp.yaw) + ")";
    const char* refusal = settings_.role == Role::Host       ? "l'hôte fait autorité sur les PNJ"
                          : sp.net_id < proto::kFirstNpcNetId ? "NetId < 1000"
                          : !finite_pose(sp.x, sp.y, sp.z, sp.yaw) ? "pose non finie"
                          : replicas_.count(sp.net_id) != 0     ? "NetId déjà annoncé"
                                                                : nullptr;
    if (refusal != nullptr) {
        out.logs.push_back({Level::Warn, what + " : ignoré (" + refusal + ")"});
        return;
    }
    replicas_.emplace(sp.net_id, Replica{});  // interpolation neuve (AD-7)
    out.commands.push_back(SpawnNpc{sp.net_id, sp.guid, Pose{sp.x, sp.y, sp.z, sp.yaw}});
    out.logs.push_back({Level::Info, what + " : réplique"});
}

void Session::on_npc_despawn(std::uint32_t net_id, Output& out) {
    auto it = settings_.role == Role::Guest ? replicas_.find(net_id) : replicas_.end();
    if (it == replicas_.end()) {
        out.logs.push_back({Level::Warn, "[sync] PNJ " + std::to_string(net_id) + " : Despawn reçu : ignoré (" +
                                             (settings_.role == Role::Host ? "l'hôte fait autorité sur les PNJ"
                                                                           : "NetId non annoncé") +
                                             ")"});
        return;
    }
    replicas_.erase(it);
    out.commands.push_back(ReleaseNpc{net_id});
    out.logs.push_back({Level::Info, "[sync] PNJ " + std::to_string(net_id) + " : Despawn reçu : réplique libérée"});
}

bool Session::on_npc_entity(std::uint32_t tick, const proto::SnapshotEntity& e) {
    auto it = settings_.role == Role::Guest ? replicas_.find(e.net_id) : replicas_.end();
    if (it == replicas_.end()) {
        ++npc_.unknown;
        return false;
    }
    Replica& r = it->second;
    r.interp.push(tick, Pose{e.x, e.y, e.z, e.yaw});
    if (!r.hp_received || e.hp != r.last_hp) {
        r.hp_received = true;
        r.last_hp = e.hp;
        if (r.hp_changes.size() >= kMaxNpcHpChanges) r.hp_changes.pop_front();
        r.hp_changes.emplace_back(static_cast<double>(tick) * core::kTickMs, e.hp);
    }
    return true;
}

void Session::place_npcs(std::uint64_t now, Output& out) {
    if (replicas_.empty() || !clock_.ready()) return;
    const double shown = clock_.remote_ms(now) - core::kInterpolationDelayMs;
    for (auto& [id, r] : replicas_) {
        int hp = -1;
        while (!r.hp_changes.empty() && r.hp_changes.front().first <= shown) {
            hp = r.hp_changes.front().second;
            r.hp_changes.pop_front();
        }
        if (hp >= 0 && hp != r.applied_hp) {
            r.applied_hp = hp;
            out.commands.push_back(SetNpcHp{id, hp});
        }
        if (r.interp.empty()) continue;  // pas encore de snapshot : reste à la pose du Spawn
        out.commands.push_back(MoveNpc{id, r.interp.sample(shown).pose});
    }
}

void Session::release_npcs(const std::string& why, Output& out) {
    if (!replicas_.empty()) {
        for (const auto& entry : replicas_) out.commands.push_back(ReleaseNpc{entry.first});
        out.logs.push_back({Level::Info, "[sync] PNJ : " + std::to_string(replicas_.size()) +
                                             " réplique(s) libérée(s) : " + why});
        replicas_.clear();
    }
    npc_ids_.clear();
    npc_guids_.clear();
    npc_active_.clear();
    npc_absent_ticks_.clear();
    next_npc_id_ = proto::kFirstNpcNetId;
}

void Session::send_snapshot(std::uint64_t now, const World& world, Output& out) {
    const auto tick = static_cast<std::int64_t>((now - t0_) / proto::kSnapshotTickMs);
    if (tick == last_tick_sent_) return;
    last_tick_sent_ = tick;
    if (!world.pose_ok) return;
    proto::Snapshot snapshot;
    snapshot.tick = static_cast<std::uint32_t>(tick);
    proto::SnapshotEntity e;
    e.net_id = own_net_id();
    e.x = world.x;
    e.y = world.y;
    e.z = world.z;
    e.yaw = world.yaw;
    e.hp = clamp_hp(world.hp);  // v9
    snapshot.entities.push_back(e);
    if (settings_.role == Role::Host) select_npcs(world, snapshot.entities, out);  // story 3.3
    Bytes bytes;
    if (!proto::encode_snapshot(snapshot, bytes)) return;
    out.commands.push_back(SendSnapshot{std::move(bytes)});
    ++snap_.sent;
}

void Session::clear_peer_avatar_state() {
    pending_.clear();
    pending_carried_.clear();
    avatar_dead_ = false;
    death_pending_ = false;
    death_at_ = 0.0;
}

void Session::on_local_motion(std::uint64_t now, const LocalMotion& in, Output& out) {
    if (state_ != State::InSession) return;
    // Un Motion que le pair refuserait (champ hors domaine, scalaire non fini) ne part pas, et
    // son nom n'est pas interné : jamais d'AniName sans Motion.
    const proto::Motion& lm = in.motion;
    Bytes probe;
    if (!std::isfinite(lm.d00) || !std::isfinite(lm.d04) || !std::isfinite(lm.d0c) || !std::isfinite(lm.d14) ||
        !proto::encode_reliable(lm, probe)) {
        return;
    }
    auto it = ani_ids_.find(in.name);
    const bool fresh = it == ani_ids_.end();  // nom nouveau : AniName d'abord
    std::uint16_t id = 0;
    if (!fresh) {
        id = it->second;
    } else {
        if (next_ani_id_ >= kAniIdCount) {
            if (!ani_full_logged_) {
                ani_full_logged_ = true;
                out.logs.push_back({Level::Warn, "[sync] 65536 noms d'animation internés : les nouveaux ne partent plus"});
            }
            return;
        }
        proto::AniName an;
        an.ani_id = static_cast<std::uint16_t>(next_ani_id_);
        an.name = in.name;
        Bytes bytes;
        if (!proto::encode_reliable(an, bytes)) return;  // nom vide ou de plus de 255 octets
        ++next_ani_id_;
        id = an.ani_id;
        ani_ids_.emplace(in.name, id);
        out.commands.push_back(SendReliable{std::move(bytes)});
    }
    proto::Motion m = in.motion;
    m.net_id = own_net_id();
    m.ani_id = id;
    m.ms = static_cast<std::uint32_t>(now - t0_);
    Bytes bytes;
    if (!proto::encode_reliable(m, bytes)) return;  // validé avant l'internement : ne peut échouer
    out.commands.push_back(SendReliable{std::move(bytes)});
    ++anim_.sent;
    // Nouvelle animation sur ce type : son premier réglage repartira, même identique (2.4b).
    for (LastParam& lp : last_params_[m.type]) lp.valid = false;
    out.logs.push_back({Level::Debug, "[sync] Motion envoyé : " + motion_text(m) +
                                          (fresh ? " « " + in.name + " »" : std::string())});
}

void Session::send_death(const World& world, Output& out) {
    // Une fois par mort (Entity::IsDead). Chez l'hôte, la mort termine la session avant d'arriver
    // ici (raison 3) : seul l'invité envoie Death.
    if (!world.dead) {
        death_sent_ = false;
        return;
    }
    if (death_sent_) return;
    Bytes bytes;
    if (!proto::encode_reliable(proto::Death{own_net_id()}, bytes)) return;
    death_sent_ = true;
    out.commands.push_back(SendReliable{std::move(bytes)});
    out.logs.push_back({Level::Info, "[sync] Death envoyé : NetId " + net_id_text(own_net_id())});
}

bool Session::accepts_peer_animation(std::uint32_t net_id) const {
    return net_id == peer_net_id() && avatar_present_ && !avatar_dead_;
}

static const char* carried_message(bool armor) { return armor ? "Armor" : "Weapon"; }

static std::string carried_text(bool armor, const std::string& name) {
    if (name.empty()) return armor ? std::string("sans armure") : std::string("arme rangée");
    return "« " + name + " »";
}

void Session::reset_carried_sent() {
    for (CarriedState& c : carried_) {
        c.sent_valid = false;
        c.sent.clear();
    }
}

void Session::on_local_carried(Carried kind, Output& out) {
    if (state_ != State::InSession) return;  // retenu en tête de tick, envoyé à l'entrée en session
    // Déjà retenu dans carried_ (balayage en tête de tick) : c'est ce nom qui part.
    const CarriedState& c = carried_[static_cast<std::size_t>(kind)];
    if (c.sent_valid && c.sent == c.local) return;
    send_carried(kind, out);
}

void Session::send_carried(Carried kind, Output& out) {
    CarriedState& c = carried_[static_cast<std::size_t>(kind)];
    const bool armor = kind == Carried::Armor;
    const proto::ReliableMessage message = armor ? proto::ReliableMessage{proto::Armor{own_net_id(), c.local}}
                                                 : proto::ReliableMessage{proto::Weapon{own_net_id(), c.local}};
    c.sent_valid = true;  // encodable ou non : une ligne par nom
    c.sent = c.local;
    Bytes bytes;
    if (!proto::encode_reliable(message, bytes)) {
        // Nom de plus de 255 octets : impossible en pratique (Entity::GetName est borné à 255).
        out.logs.push_back({Level::Warn, std::string("[sync] ") + carried_message(armor) + " non encodable (nom de " +
                                             std::to_string(c.local.size()) + " octets) : non envoyé"});
        return;
    }
    out.commands.push_back(SendReliable{std::move(bytes)});
    out.logs.push_back({Level::Info, std::string("[sync] ") + carried_message(armor) + " envoyé : NetId " +
                                         net_id_text(own_net_id()) + ", " + carried_text(armor, c.local)});
}

void Session::on_peer_carried(Carried kind, std::uint32_t net_id, const std::string& name, Output& out) {
    const bool armor = kind == Carried::Armor;
    if (net_id != peer_net_id() || !avatar_present_ || avatar_dead_ || death_pending_) {
        out.logs.push_back({Level::Debug, std::string("[sync] ") + carried_message(armor) + " reçu (NetId " +
                                              net_id_text(net_id) + ", " + carried_text(armor, name) + ") : ignoré (" +
                                              (net_id != peer_net_id() ? "pas l'avatar du pair"
                                               : !avatar_present_     ? "pas d'avatar"
                                               : avatar_dead_         ? "avatar mort"
                                                                      : "mort de l'avatar en attente") +
                                              ")"});
        return;
    }
    if (pending_carried_.size() >= kMaxPendingCarried) {
        // File pleine : on retire le plus ancien du même type (un Armor, envoyé au changement
        // seulement, ne doit pas être évincé par une rafale de Weapon), sinon le plus ancien.
        auto oldest = pending_carried_.begin();
        for (auto it = pending_carried_.begin(); it != pending_carried_.end(); ++it) {
            if (it->kind == kind) {
                oldest = it;
                break;
            }
        }
        pending_carried_.erase(oldest);
    }
    // Comme Death : appliqué quand le temps affiché atteint le dernier snapshot reçu.
    pending_carried_.push_back({interp_.empty() ? 0.0 : interp_.last_time(), kind, name});
    out.logs.push_back({Level::Info, std::string("[sync] ") + carried_message(armor) + " reçu : avatar " +
                                         net_id_text(net_id) + ", " + carried_text(armor, name) +
                                         " à l'instant d'affichage"});
}

void Session::apply_due_carried(std::uint64_t now, Output& out) {
    const double shown = clock_.ready() ? clock_.remote_ms(now) - core::kInterpolationDelayMs : 1e300;
    while (!pending_carried_.empty() && (!clock_.ready() || shown >= pending_carried_.front().at)) {
        PendingCarried& p = pending_carried_.front();
        if (p.kind == Carried::Armor) {
            out.commands.push_back(AvatarArmor{peer_net_id(), std::move(p.name)});
        } else {
            out.commands.push_back(AvatarWeapon{peer_net_id(), std::move(p.name)});
        }
        pending_carried_.pop_front();
    }
}

void Session::on_local_param(std::uint64_t now, const LocalParam& in, Output& out) {
    if (state_ != State::InSession) return;
    proto::MotionParam p;
    p.net_id = own_net_id();
    p.ms = static_cast<std::uint32_t>(now - t0_);
    p.type = in.type;
    p.kind = in.kind;
    p.value = in.value;
    p.time = in.time;
    Bytes bytes;
    if (!proto::encode_reliable(p, bytes)) return;  // type, nature ou valeur hors domaine
    LastParam& last = last_params_[p.type][p.kind];
    // Seuls les changements partent ; une valeur finale exacte de 0 ou 1 (fin de fondu) part
    // toujours, même proche de la précédente, pour que l'avatar ne reste pas à 0,99 ou 0,008.
    // Fondu et arrêt (natures 2 et 3) : seulement s'ils diffèrent (valeur ou durée).
    const bool exact_end = (p.value == 0.0f || p.value == 1.0f) && p.value != last.value;
    const float eps = p.kind == static_cast<std::uint8_t>(proto::MotionParamKind::PlaySpeed) ? kParamSpeedEpsilon
                      : p.kind == static_cast<std::uint8_t>(proto::MotionParamKind::TargetWeight) ? kParamEpsilon
                                                                                                   : 0.0f;
    if (last.valid && std::fabs(p.value - last.value) <= eps && std::fabs(p.time - last.time) <= kParamTimeEpsilon &&
        !exact_end) {
        return;
    }
    last = LastParam{true, p.value, p.time};
    out.commands.push_back(SendReliable{std::move(bytes)});
    ++anim_.params_sent;
}

void Session::on_peer_param(const proto::MotionParam& p) {
    if (!accepts_peer_animation(p.net_id)) {
        ++anim_.params_ignored;
        return;
    }
    if (pending_.size() >= kMaxPendingMotions) {
        pending_.pop_front();
        ++anim_.dropped;
    }
    PendingMotion e;
    e.is_param = true;
    e.param = p;
    pending_.push_back(std::move(e));
}

void Session::on_peer_motion(const proto::Motion& m, Output& out) {
    if (!accepts_peer_animation(m.net_id)) {
        ++anim_.ignored;
        return;
    }
    auto name = peer_names_.find(m.ani_id);
    const bool finite = std::isfinite(m.d00) && std::isfinite(m.d04) && std::isfinite(m.d0c) && std::isfinite(m.d14);
    if (name == peer_names_.end() || !finite) {
        ++anim_.ignored;
        if (warned_ani_ids_.insert(m.ani_id).second) {
            out.logs.push_back({Level::Warn, "[sync] Motion d'AniId " + std::to_string(m.ani_id) + " ignoré : " +
                                                 (name == peer_names_.end() ? "AniId inconnu" : "scalaire non fini")});
        }
        return;
    }
    if (pending_.size() >= kMaxPendingMotions) {
        pending_.pop_front();
        ++anim_.dropped;
    }
    PendingMotion e;
    e.motion = m;
    e.name = name->second;
    pending_.push_back(std::move(e));
}

void Session::play_due_motions(std::uint64_t now, Output& out) {
    if (pending_.empty()) return;
    // Même instant d'affichage que les snapshots ; avant tout snapshot, aussitôt.
    const double shown = clock_.ready() ? clock_.remote_ms(now) - core::kInterpolationDelayMs : 1e300;
    // Un Motion daté de plus de kMaxMotionLeadMs dans le futur (horloge mal recalée, pair
    // fautif) part aussitôt : il ne bloque pas ceux qui le suivent.
    std::size_t due = 0;
    while (due < pending_.size() && (static_cast<double>(pending_[due].ms()) <= shown ||
                                     static_cast<double>(pending_[due].ms()) > shown + kMaxMotionLeadMs)) {
        ++due;
    }
    if (due == 0) return;
    // Types 0 et 1 (locomotion) : seul le plus récent de chacun part ; les réglages de ce type
    // placés avant lui visaient l'animation qu'il remplace : écartés aussi.
    std::size_t last_of_type[2] = {due, due};
    for (std::size_t i = 0; i < due; ++i) {
        if (pending_[i].is_param) continue;
        const std::uint8_t type = pending_[i].motion.type;
        if (type <= 1) last_of_type[type] = i;
    }
    for (std::size_t i = 0; i < due; ++i) {
        PendingMotion& p = pending_[i];
        if (p.is_param) {
            if (p.param.type <= 1 && last_of_type[p.param.type] != due && i < last_of_type[p.param.type]) {
                ++anim_.superseded;
                continue;
            }
            out.commands.push_back(PlayAvatarParam{peer_net_id(), p.param});
            ++anim_.params_played;
            continue;
        }
        if (p.motion.type <= 1 && last_of_type[p.motion.type] != i) {
            ++anim_.superseded;
            continue;
        }
        char lag[64];
        std::snprintf(lag, sizeof lag, " (affiché %+.0f ms)", shown >= 1e299 ? 0.0 : shown - p.motion.ms);
        out.logs.push_back({Level::Debug, "[sync] Motion rejoué : " + motion_text(p.motion) + lag + " « " + p.name + " »"});
        out.commands.push_back(PlayAvatarMotion{peer_net_id(), std::move(p.name), p.motion});
        ++anim_.played;
    }
    pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(due));
}

void Session::place_avatar(std::uint64_t now, Output& out) {
    if (!avatar_present_ || avatar_dead_) return;  // un avatar mort reste en ragdoll, sans pose imposée
    apply_due_carried(now, out);  // avant les gestes du même instant : attaques rejouées arme en main
    play_due_motions(now, out);
    if (death_pending_ && (!clock_.ready() || clock_.remote_ms(now) - core::kInterpolationDelayMs >= death_at_)) {
        death_pending_ = false;
        avatar_dead_ = true;
        pending_.clear();
        pending_carried_.clear();
        out.commands.push_back(AvatarDeath{peer_net_id()});
        return;
    }
    if (!clock_.ready() || interp_.empty()) return;
    const core::Sample s = interp_.sample(clock_.remote_ms(now) - core::kInterpolationDelayMs);
    switch (s.mode) {
        case core::SampleMode::Interpolated: ++snap_.interpolated; break;
        case core::SampleMode::Extrapolated: ++snap_.extrapolated; break;
        default: ++snap_.held; break;
    }
    avatar_pose_ = s.pose;  // hôte : rayon des PNJ autour de l'avatar (story 3.3)
    avatar_pose_ok_ = true;
    out.commands.push_back(MoveAvatar{peer_net_id(), s.pose});
}

void Session::snapshot_stats(std::uint64_t now, Output& out) {
    if (now < next_stats_) return;
    next_stats_ = now + kSnapshotStatsMs;
    char buf[900];
    std::snprintf(buf, sizeof buf,
                  "[sync] snapshots (5 s) : envoyés %u, reçus %u (illisibles %u, entités ignorées %u) ; "
                  "frames de l'avatar : interpolées %u, extrapolées %u, figées %u ; décalage horloge %.1f ms ; "
                  "Motion : envoyés %u, rejoués %u, écrasés %u, ignorés %u, file pleine %u ; "
                  "MotionParam : envoyés %u, rejoués %u, ignorés %u ; "
                  "PNJ : suivis %zu, annoncés %u, retirés %u, plafond %u, liste illisible %u, entités inconnues %u, "
                  "OnProcess de routine bloqués %u",
                  snap_.sent, snap_.received, snap_.unreadable, snap_.ignored, snap_.interpolated,
                  snap_.extrapolated, snap_.held, clock_.offset_ms(), anim_.sent, anim_.played, anim_.superseded,
                  anim_.ignored, anim_.dropped, anim_.params_sent, anim_.params_played, anim_.params_ignored,
                  npc_count(), npc_.spawned, npc_.despawned, npc_.capped, npc_.unreadable, npc_.unknown,
                  npc_.routine_blocked);
    out.logs.push_back({Level::Debug, buf});
    const SnapshotCounters zero{};
    snap_ = zero;
    anim_ = MotionCounters{};
    npc_ = NpcCounters{};
}

void Session::send_hello(Output& out) {
    out.commands.push_back(Send{host_, hello_});
    out.logs.push_back({Level::Debug, "[sync] Hello envoyé à " + net::to_string(host_)});
}

void Session::timers(std::uint64_t now, Output& out) {
    switch (state_) {
        case State::Connecting:
            if (now >= connect_deadline_) {
                end(now, "hôte injoignable (aucune réponse en " + std::to_string(kConnectTimeoutMs / 1000) + " s)", 0,
                    false, Level::Warn, out);
                return;
            }
            if (now >= next_hello_) {
                send_hello(out);
                next_hello_ += kHelloIntervalMs;
                if (next_hello_ <= now) next_hello_ = now + kHelloIntervalMs;  // frame très en retard
            }
            return;
        case State::InSession:
            if (now - last_peer_ >= kPeerTimeoutMs) {
                end(now, "timeout (" + std::to_string(kPeerTimeoutMs / 1000) + " s sans nouvelles du pair)", 0,
                    settings_.role == Role::Host, Level::Warn, out);
                return;
            }
            if (now >= next_heartbeat_) {
                Bytes bytes;
                if (proto::encode_reliable(proto::Heartbeat{}, bytes)) {
                    out.commands.push_back(SendReliable{std::move(bytes)});
                }
                next_heartbeat_ += kHeartbeatIntervalMs;
                if (next_heartbeat_ <= now) next_heartbeat_ = now + kHeartbeatIntervalMs;
            }
            return;
        default:
            return;
    }
}

}  // namespace mp::core::session
