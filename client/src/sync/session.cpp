#include "sync/session.h"

#include "common/log.h"
#include "core/session.h"
#include "game/combat.h"
#include "game/entity.h"
#include "net/transport.h"
#include "sync/anim_filter.h"
#include "sync/avatar.h"
#include "sync/local_capture.h"
#include "sync/npc.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <new>
#include <string>
#include <unordered_map>
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

// Captures du héros local (story 2.4) : PS suivi par l'AnimFilter, noms des ressources (vidés
// au changement de joueur : les pointeurs d'un autre monde ne valent plus rien).
void* g_local_player = nullptr;
std::unordered_map<const void*, std::string> g_motion_names;
sync::Capture g_captures[sync::kCaptureRing];

// Arme du héros local (story 3.2) : dernier nom poussé à la session, oublié au changement de
// héros (nouveau monde : il repart à la première lecture).
bool g_weapon_known = false;
std::string g_weapon;
bool g_weapon_error_logged = false;
bool g_armor_known = false;  // story 3.2c : armure portée par le héros, comme l'arme
std::string g_armor;
bool g_armor_error_logged = false;

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

    // Pose du héros : Welcome, Spawn et snapshots (story 2.3), lue dans tout monde chargé.
    if (w.loaded()) {
        game::Matrix m{};
        if (game::get_pose(player, m)) {
            w.pose_ok = true;
            w.x = m.m[12];
            w.y = m.m[13];
            w.z = m.m[14];
            w.yaw = std::atan2(m.m[8], m.m[10]);  // AD-7
        }
        // PV du héros (v9, story 3.3), dans chaque snapshot ; illisibles : 0.
        if (!game::get_hit_points(player, w.hp)) w.hp = 0;
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
            if (const auto* ma = std::get_if<cs::MoveAvatar>(&cmd)) {
                avatar::move(ma->net_id, ma->pose);
                continue;
            }
            if (const auto* pm = std::get_if<cs::PlayAvatarMotion>(&cmd)) {
                avatar::play(pm->net_id, pm->name, pm->motion);
                continue;
            }
            if (const auto* pp = std::get_if<cs::PlayAvatarParam>(&cmd)) {
                avatar::play_param(pp->net_id, pp->param);
                continue;
            }
            if (const auto* ad = std::get_if<cs::AvatarDeath>(&cmd)) {
                avatar::die(ad->net_id);
                continue;
            }
            if (const auto* aw = std::get_if<cs::AvatarWeapon>(&cmd)) {
                avatar::set_weapon(aw->net_id, aw->name);
                continue;
            }
            if (const auto* aa = std::get_if<cs::AvatarArmor>(&cmd)) {  // story 3.2c
                avatar::set_armor(aa->net_id, aa->name);
                continue;
            }
            if (const auto* eh = std::get_if<cs::EquipHero>(&cmd)) {  // story 3.2b
                avatar::equip_player(eh->names);
                continue;
            }
            if (const auto* ea = std::get_if<cs::EquipAvatar>(&cmd)) {
                avatar::equip(ea->net_id, ea->names);
                continue;
            }
            // Répliques des PNJ (story 3.3), côté invité : sync/npc seul écrit leur état.
            if (const auto* sp = std::get_if<cs::SpawnNpc>(&cmd)) {
                npc::spawn(sp->net_id, sp->guid, sp->pose);
                continue;
            }
            if (const auto* mn = std::get_if<cs::MoveNpc>(&cmd)) {
                npc::move(mn->net_id, mn->pose);
                continue;
            }
            if (const auto* hp = std::get_if<cs::SetNpcHp>(&cmd)) {
                npc::set_hp(hp->net_id, hp->hp);
                continue;
            }
            if (const auto* rn = std::get_if<cs::ReleaseNpc>(&cmd)) {
                npc::release(rn->net_id);
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
            } else if (auto* sn = std::get_if<cs::SendSnapshot>(&cmd)) {
                posted = g_transport->post(net::SendSnapshot{std::move(sn->bytes)});
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

constexpr std::size_t kMaxMotionNames = 4096;  // borne du cache (vidé au-delà)

// Nom de la ressource, mis en cache s'il a pu être lu (un échec n'est pas retenu : la capture
// suivante réessaie). Vide : illisible.
std::string motion_name(const void* resource) {
    auto it = g_motion_names.find(resource);
    if (it != g_motion_names.end()) return it->second;
    std::string name;
    if (resource == nullptr || !game::get_resource_name(resource, name) || name.empty()) return {};
    if (g_motion_names.size() >= kMaxMotionNames) g_motion_names.clear();
    g_motion_names.emplace(resource, name);
    return name;
}

// PS du héros local dans l'AnimFilter (capturé par les hooks), puis captures de la frame →
// entrées de la session. Hors monde chargé, plus rien n'est capturé.
void local_captures(const cs::World& world) {
    sync::AnimFilter& filter = sync::anim_filter();
    void* player = reinterpret_cast<void*>(world.player);
    unsigned dropped = 0;
    if (player != g_local_player) {
        // Autre héros (autre monde) : les captures en attente et les noms en cache désignent
        // des ressources de l'ancien monde, peut-être libérées.
        g_local_player = player;
        g_motion_names.clear();
        g_weapon_known = false;
        g_weapon_error_logged = false;
        g_armor_known = false;
        g_armor_error_logged = false;
        filter.drain(g_captures, sync::kCaptureRing, dropped);
    }
    void* ps = nullptr;
    if (world.loaded()) {
        game::Entity e;
        e.ptr = player;
        if (!game::get_animation_ps(e, ps)) ps = nullptr;
    }
    filter.set_local(ps);
    avatar::track_local(ps);  // diagnostic de locomotion (story 2.4b)
    // Arme en main (story 3.2), lue à chaque frame dans un monde chargé ; poussée au changement.
    if (world.loaded()) {
        std::string weapon;
        if (!avatar::local_weapon(weapon)) {
            if (!g_weapon_error_logged) {
                g_weapon_error_logged = true;
                log::error("[sync] arme du héros illisible (SEH) : dernier état gardé, pas de Weapon");
            }
        } else if (!g_weapon_known || weapon != g_weapon) {
            g_weapon_known = true;
            g_weapon = weapon;
            log::debug("[sync] arme du héros : " + (weapon.empty() ? std::string("rangée") : "« " + weapon + " »"));
            g_inputs.push_back(cs::LocalWeapon{weapon});
        }
        // Armure portée (story 3.2c), mêmes règles.
        std::string armor;
        if (!avatar::local_armor(armor)) {
            if (!g_armor_error_logged) {
                g_armor_error_logged = true;
                log::error("[sync] armure du héros illisible (SEH) : dernier état gardé, pas d'Armor");
            }
        } else if (!g_armor_known || armor != g_armor) {
            g_armor_known = true;
            g_armor = armor;
            log::debug("[sync] armure du héros : " + (armor.empty() ? std::string("aucune") : "« " + armor + " »"));
            g_inputs.push_back(cs::LocalArmor{armor});
        }
    }
    const std::size_t n = filter.drain(g_captures, sync::kCaptureRing, dropped);
    if (dropped != 0) log::debug("[sync] captures perdues (anneau plein) : " + std::to_string(dropped));
    for (std::size_t i = 0; i < n; ++i) {
        const sync::Capture& c = g_captures[i];
        auto in = sync::local_input(c, c.kind == sync::CaptureKind::Motion ? motion_name(c.resource) : std::string());
        if (in) g_inputs.push_back(std::move(*in));
    }
}

void frame() {
    const cs::State state = g_session->state();
    cs::World world = read_world(state);
    const std::uint64_t now = now_ms();
    // Hôte : PNJ lus seulement au tick qui enverra un snapshot (story 3.3).
    if (g_session->wants_npcs(now)) npc::read_host(world);
    g_inputs.clear();
    if (g_transport != nullptr) {
        net::Event ev;
        while (g_transport->poll(ev)) {
            if (auto* c = std::get_if<net::ControlReceived>(&ev)) {
                g_inputs.push_back(cs::ControlIn{c->source, std::move(c->bytes)});
            } else if (auto* r = std::get_if<net::ReliableReceived>(&ev)) {
                g_inputs.push_back(cs::ReliableIn{std::move(r->bytes)});
            } else {
                g_inputs.push_back(cs::SnapshotIn{std::move(std::get<net::SnapshotReceived>(ev).bytes)});
            }
        }
    }
    local_captures(world);
    // Story 3.3 : répliques revérifiées une fois par frame, avant leurs commandes ; appels de
    // routine bloqués (IA figée), pour le bilan debug.
    if (npc::count() != 0) npc::validate();
    world.routine_blocked = sync::anim_filter().take_routine_blocked();
    execute(g_session->tick(now, world, g_inputs));
    // Filet de sécurité : hors session, aucune réplique ne reste au registre (ReleaseNpc déjà
    // exécutés à Ended).
    if (g_session->state() != cs::State::InSession) npc::release_all();
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
