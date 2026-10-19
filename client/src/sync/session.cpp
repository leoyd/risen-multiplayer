#include "sync/session.h"

#include "common/log.h"
#include "core/session.h"
#include "game/entity.h"
#include "net/transport.h"
#include "sync/avatar.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace mp::sync::session {

namespace {

namespace cs = mp::core::session;

// Durée de vie = processus, jamais détruits (pas de join sous le verrou du chargeur).
cs::Session* g_session = nullptr;
net::Transport* g_transport = nullptr;
bool g_broken = false;            // exception : plus rien jusqu'au redémarrage
bool g_player_error_logged = false;
bool g_running_error_logged = false;
bool g_dead_error_logged = false;
LARGE_INTEGER g_qpc_frequency{};
std::vector<cs::Input> g_inputs;  // réutilisé d'une frame à l'autre

std::uint64_t now_ms() {
    LARGE_INTEGER t{};
    QueryPerformanceCounter(&t);
    return static_cast<std::uint64_t>(t.QuadPart / g_qpc_frequency.QuadPart * 1000 +
                                      t.QuadPart % g_qpc_frequency.QuadPart * 1000 / g_qpc_frequency.QuadPart);
}

void write(const cs::LogLine& line) {
    switch (line.level) {
        case cs::Level::Debug: log::debug(line.text); break;
        case cs::Level::Info: log::info(line.text); break;
        case cs::Level::Warn: log::warn(line.text); break;
        case cs::Level::Error: log::error(line.text); break;
    }
}

// Dernier état des signaux journalisé (diagnostic debug, une ligne par changement).
struct Signals {
    std::uintptr_t player = 0;
    bool running = false;
    bool dead = false;
    bool operator==(const Signals& o) const { return player == o.player && running == o.running && dead == o.dead; }
};
Signals g_signals;
bool g_signals_logged = false;

std::string hex_ptr(std::uintptr_t v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "0x%016llx", static_cast<unsigned long long>(v));
    return buf;
}

// Signaux du monde (story 1.5), lus à chaque frame : joueur, partie en cours, mort du joueur.
cs::World read_world(cs::State state) {
    cs::World w;
    game::Entity player;
    if (!game::get_player(player)) {
        if (!g_player_error_logged) {
            g_player_error_logged = true;
            log::error("[sync] Entity::GetPlayer a échoué (SEH) : monde considéré comme non chargé");
        }
        player.ptr = nullptr;
    }
    w.player = reinterpret_cast<std::uintptr_t>(player.ptr);  // entier opaque pour core/
    if (!game::game_running(w.running)) {
        if (!g_running_error_logged) {
            g_running_error_logged = true;
            log::error("[sync] gCSession::IsGameRunning a échoué (SEH) : partie considérée comme arrêtée");
        }
        w.running = false;
    }
    // IsDead seulement dans un monde chargé (pas pendant sa destruction : entité à moitié détruite).
    if (w.loaded() && !game::is_dead(player, w.dead)) {
        if (!g_dead_error_logged) {
            g_dead_error_logged = true;
            log::error("[sync] Entity::IsDead a échoué (SEH) : joueur considéré comme vivant");
        }
        w.dead = false;
    }

    const Signals now{w.player, w.running, w.dead};
    if (!g_signals_logged || !(now == g_signals)) {
        g_signals_logged = true;
        g_signals = now;
        log::debug("[sync] signaux : player=" + hex_ptr(now.player) + " running=" + (now.running ? "1" : "0") +
                   " dead=" + (now.dead ? "1" : "0") + " (état " + cs::state_name(state) + ")");
    }

    // La pose ne sert qu'au Welcome de l'hôte et au Spawn de l'invité (repli si le Welcome
    // porte une pose nulle) : lue seulement avant l'entrée en session.
    if (w.loaded() && (state == cs::State::Inactive || state == cs::State::Hosting ||
                       state == cs::State::Connecting)) {
        game::Matrix m{};
        if (game::get_pose(player, m)) {
            w.pose_ok = true;
            w.x = m.m[12];
            w.y = m.m[13];
            w.z = m.m[14];
            w.yaw = std::atan2(m.m[8], m.m[10]);  // AD-7
        }
    }
    return w;
}

// Exécute les commandes dans l'ordre. Un échec de démarrage rend la main à la session
// (fail), dont les commandes (Stop) et les logs sont traités à leur tour.
void execute(cs::Output out) {
    std::vector<cs::Output> pending;
    pending.push_back(std::move(out));
    for (std::size_t p = 0; p < pending.size(); ++p) {
        for (const auto& line : pending[p].logs) write(line);
        for (auto& cmd : pending[p].commands) {
            if (const auto* s = std::get_if<cs::Start>(&cmd)) {
                std::string error;
                if (g_transport == nullptr) g_transport = new (std::nothrow) net::Transport();
                if (g_transport == nullptr) {
                    error = "allocation du transport impossible";
                } else if (g_transport->start(s->port, error)) {
                    log::info("[sync] transport démarré sur le port UDP " + std::to_string(g_transport->local_port()));
                    continue;
                }
                pending.push_back(g_session->fail(now_ms(), "réseau : " + error));
                break;  // commandes suivantes sans objet
            }
            // Avatars (story 2.2) : appels au jeu, sur ce thread (AD-2).
            if (const auto* t = std::get_if<cs::Teleport>(&cmd)) {
                avatar::teleport_player(t->pose);
                continue;
            }
            if (const auto* sa = std::get_if<cs::SpawnAvatar>(&cmd)) {
                avatar::spawn(sa->net_id, sa->pose);
                continue;
            }
            if (const auto* da = std::get_if<cs::DespawnAvatar>(&cmd)) {
                avatar::despawn(da->net_id);
                continue;
            }
            if (g_transport == nullptr) continue;
            bool posted = true;
            if (auto* s = std::get_if<cs::Send>(&cmd)) {
                posted = g_transport->post(net::SendControl{s->destination, std::move(s->bytes)});
            } else if (const auto* o = std::get_if<cs::OpenReliable>(&cmd)) {
                posted = g_transport->post(net::OpenReliable{o->conv, o->peer});
            } else if (auto* r = std::get_if<cs::SendReliable>(&cmd)) {
                posted = g_transport->post(net::SendReliable{std::move(r->bytes)});
            } else if (std::holds_alternative<cs::CloseReliable>(cmd)) {
                posted = g_transport->post(net::CloseReliable{});
            } else if (std::holds_alternative<cs::Stop>(cmd)) {
                g_transport->stop();  // thread principal, hors verrou du chargeur
                log::info("[sync] transport arrêté");
            }
            if (!posted) log::warn("[sync] file du transport pleine : commande jetée");
        }
    }
}

void frame() {
    const cs::State state = g_session->state();
    const cs::World world = read_world(state);
    g_inputs.clear();
    if (g_transport != nullptr) {
        net::Event ev;
        while (g_transport->poll(ev)) {
            if (auto* c = std::get_if<net::ControlReceived>(&ev)) {
                g_inputs.push_back(cs::ControlIn{c->source, std::move(c->bytes)});
            } else {
                g_inputs.push_back(cs::ReliableIn{std::move(std::get<net::ReliableReceived>(ev).bytes)});
            }
        }
    }
    execute(g_session->tick(now_ms(), world, g_inputs));
}

}  // namespace

void configure(const Config& config, const std::array<ModuleFingerprint, 4>& fingerprint,
               const char* mod_version) {
    if (g_session != nullptr) return;
    QueryPerformanceFrequency(&g_qpc_frequency);
    if (g_qpc_frequency.QuadPart <= 0) {
        log::error("[sync] QueryPerformanceFrequency indisponible : pas de session");
        return;
    }
    cs::Settings s;
    s.role = config.role;
    s.host_ip = config.host_ip;
    s.port = config.port;
    s.nickname = config.nickname;
    s.equipment = config.guest_equipment;
    s.mod_version = mod_version;
    s.fingerprint = fingerprint;
    LARGE_INTEGER t{};
    QueryPerformanceCounter(&t);
    s.seed = static_cast<std::uint64_t>(t.QuadPart) ^ (static_cast<std::uint64_t>(GetCurrentProcessId()) << 32) ^
             GetTickCount64();
    g_session = new cs::Session(std::move(s));
    static const char* const kModules[] = {"Game", "Engine", "Script", "SharedBase"};
    for (std::size_t i = 0; i < fingerprint.size(); ++i) {
        log::debug(std::string("[sync] empreinte locale ") + kModules[i] + ".dll : " + format_fingerprint(fingerprint[i]));
    }
    log::info(std::string("[sync] session prête (rôle ") + role_name(config.role) +
              ") : démarrage au chargement d'une partie");
}

void on_frame() noexcept {
    if (g_session == nullptr || g_broken) return;
    try {
        frame();
    } catch (...) {
        g_broken = true;
        try {
            log::error(std::string("[sync] exception dans la session (état ") + cs::state_name(g_session->state()) +
                       ") : abandon, retour au solo");
            if (g_transport != nullptr) g_transport->stop();
        } catch (...) {
        }
    }
}

}  // namespace mp::sync::session
