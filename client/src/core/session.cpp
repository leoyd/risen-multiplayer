#include "core/session.h"

#include "core/protocol.h"

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

}  // namespace

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
        if (proto::encode_reliable(proto::SessionEnd{local_reason}, bytes)) {
            out.commands.push_back(SendReliable{std::move(bytes)});
            out.logs.push_back({Level::Info, "[sync] SessionEnd envoyé : " + end_text(local_reason)});
        }
    }
    transition(State::Ended, reason + " ; retour au solo", out, level);
    linger_until_ = now + kLingerMs;  // le KCP reste ouvert pour laisser partir le SessionEnd
    wait_new_world_ = !rearm_now;
    track_world(world_, false);  // fin de monde : le monde a déjà changé
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
    const bool revived = world_.dead && !world.dead;  // world_ = monde du tick précédent
    world_ = world;
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
        } else {
            on_reliable(now, std::get<ReliableIn>(in), out);
        }
    }
    if (state_ != State::Ended) timers(now, out);
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
    reliable_seen_ = false;
    out.commands.push_back(Send{in.source, std::move(bytes)});
    out.commands.push_back(OpenReliable{conv_, peer_});
    transition(State::InSession, "invité " + who + " « " + hello.nickname + " » accepté, conv=" + hex32(conv_) +
                                     ", pose envoyée " + pose_text(welcome.x, welcome.y, welcome.z, welcome.yaw),
               out);
    enter_session(now, out);
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
        enter_session(now, out);
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
    if (std::holds_alternative<proto::Motion>(decoded.message)) {
        // Avatar : story 2.4. En attendant, ignoré sans bruit hors debug.
        out.logs.push_back({Level::Debug, "[sync] Motion reçu, ignoré (pas encore d'avatar)"});
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

void Session::enter_session(std::uint64_t now, Output& /*out*/) {
    last_peer_ = now;
    next_heartbeat_ = now;  // premier heartbeat dès l'entrée en session
    heartbeats_rx_ = 0;
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
