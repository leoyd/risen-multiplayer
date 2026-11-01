#include "sync/session.h"

#include "common/log.h"
#include "core/overlay_text.h"
#include "core/session.h"
#include "game/combat.h"
#include "game/entity.h"
#include "net/transport.h"
#include "sync/anim_filter.h"
#include "sync/avatar.h"
#include "sync/dialogue.h"
#include "sync/guest_health.h"
#include "sync/hits.h"
#include "sync/local_capture.h"
#include "sync/npc.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
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
// Overlay à l'écran : pseudo et adresse de l'hôte (texte), dernier texte poussé.
std::atomic<OverlaySink> g_overlay_sink{nullptr};
std::string g_overlay_nickname;
std::string g_overlay_host_ip;
std::uint16_t g_overlay_port = kDefaultPort;
std::string g_overlay_text;
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

// Story 3.4a : héros de la frame précédente (fin de monde : PNJ oubliés avant tout appel au jeu),
// et compteurs du heartbeat de secours déjà journalisés.
std::uintptr_t g_last_world_player = 0;
std::uint64_t g_keepalives_seen = 0;
std::uint64_t g_keepalive_capped_seen = 0;
Role g_role = Role::Host;  // story 3.5 : rôle publié au détour des dégâts
bool g_native_attacks = false;  // story 3.8 : native_attacks = 1 et bloc des attaques natives résolu

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
                } else if (g_transport->start(s->port, s->options, error)) {
                    g_keepalives_seen = 0;  // story 3.4a : compteurs du nouveau transport
                    g_keepalive_capped_seen = 0;
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
            if (const auto* nd = std::get_if<cs::NpcDeath>(&cmd)) {  // story 3.4b : mort sans Kill natif
                npc::die(nd->net_id);
                continue;
            }
            // Animations des PNJ (story 3.4) : capture chez l'hôte, rejeu chez l'invité.
            if (const auto* cn = std::get_if<cs::CaptureNpc>(&cmd)) {
                npc::capture(cn->net_id, cn->guid);
                continue;
            }
            if (const auto* un = std::get_if<cs::UncaptureNpc>(&cmd)) {
                npc::uncapture(un->net_id);
                continue;
            }
            if (const auto* pn = std::get_if<cs::PlayNpcMotion>(&cmd)) {
                npc::play(pn->net_id, pn->name, pn->motion);
                continue;
            }
            if (const auto* pq = std::get_if<cs::PlayNpcParam>(&cmd)) {
                npc::play_param(pq->net_id, pq->param);
                continue;
            }
            if (const auto* ah = std::get_if<cs::ApplyHit>(&cmd)) {  // story 3.5 : coup de l'invité, chez l'hôte
                hits::apply(ah->net_id, ah->guid);
                continue;
            }
            if (const auto* ag = std::get_if<cs::ApplyHitOnGuest>(&cmd)) {  // story 3.6 : chez l'invité
                guest_health::apply_hit(ag->source, ag->damage);
                continue;
            }
            if (const auto* an = std::get_if<cs::ApplyNpcAttack>(&cmd)) {  // story 3.8 : chez l'invité
                npc::attack(an->net_id, an->move, an->target);
                continue;
            }
            if (const auto* aa = std::get_if<cs::ApplyAvatarAttack>(&cmd)) {  // story 3.8b : chez l'hôte
                // Cible ≥ 1000 : PNJ capturé de ce NetId (introuvable : cibles effacées) ; 1 ou 0 : effacées.
                void* target = aa->target >= protocol::kFirstNpcNetId ? npc::captured_entity(aa->target) : nullptr;
                if (target == nullptr && aa->target >= protocol::kFirstNpcNetId) {
                    log::debug("[sync] avatar : attaque native sur le PNJ " + std::to_string(aa->target) +
                               " non capturé : cibles effacées");
                }
                avatar::attack(aa->move, target, target != nullptr ? aa->target : 0);
                continue;
            }
            if (const auto* ad = std::get_if<cs::ApplyDialogLine>(&cmd)) {  // story 4.1 (4.2 : chez l'hôte aussi)
                game::Entity player;
                if (!game::get_player(player)) player.ptr = nullptr;
                // Locuteur : avatar du pair (1 chez l'invité, 2 chez l'hôte), sinon PNJ (réplique chez
                // l'invité ; chez l'hôte, PNJ tenu retrouvé par son GUID).
                void* speaker = nullptr;
                const char* name = "la réplique";
                if (ad->net_id < protocol::kFirstNpcNetId) {
                    speaker = avatar::entity();
                    name = g_role == Role::Host ? "l'avatar de l'invité" : "l'avatar de l'hôte";
                } else if (g_role == Role::Host) {
                    if (!npc::find_entity(ad->guid, speaker)) speaker = nullptr;
                    name = "le PNJ tenu";
                } else {
                    speaker = npc::replica_entity(ad->net_id);
                }
                dialogue::replay(player.ptr, ad->net_id, ad->output_type, ad->sample, speaker, name);
                continue;
            }
            if (const auto* hn = std::get_if<cs::HoldNpc>(&cmd)) {  // story 4.2 : chez l'hôte
                npc::hold(hn->net_id, hn->guid);
                continue;
            }
            if (const auto* un = std::get_if<cs::UnholdNpc>(&cmd)) {
                npc::unhold(un->net_id);
                continue;
            }
            if (const auto* fr = std::get_if<cs::FreeReplica>(&cmd)) {  // story 4.2 : chez l'invité
                npc::set_freed(fr->net_id, fr->freed);
                continue;
            }
            if (const auto* rh = std::get_if<cs::RespawnHero>(&cmd)) {  // story 3.7 : chez l'invité
                guest_health::respawn(rh->pose, rh->teleport);
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

// Story 3.4a : à la reprise de la frame, une ligne si le thread réseau a envoyé des heartbeats
// de secours pendant la pause, une autre s'il a dû se taire (borne atteinte).
void log_keepalives() {
    const net::Stats st = g_transport->stats();
    if (st.keepalives_sent > g_keepalives_seen) {
        log::info("[sync] jeu en pause : " + std::to_string(st.keepalives_sent - g_keepalives_seen) +
                  " heartbeat(s) envoyé(s) par le thread réseau");
        g_keepalives_seen = st.keepalives_sent;
    }
    if (st.keepalive_capped > g_keepalive_capped_seen) {
        log::warn("[sync] jeu en pause plus de " + std::to_string(cs::kKeepaliveMaxPauseMs / 1000) +
                  " s : le thread réseau a cessé ses heartbeats de secours (timeout probable chez le pair)");
        g_keepalive_capped_seen = st.keepalive_capped;
    }
}

// Overlay : texte recalculé après le tick, poussé seulement s'il change (aucun appel au jeu).
void push_overlay() {
    const OverlaySink sink = g_overlay_sink.load(std::memory_order_acquire);
    if (sink == nullptr) return;
    std::string text = core::overlay_text(g_role, g_overlay_nickname, g_session->state(), g_overlay_host_ip,
                                          g_overlay_port);
    if (text == g_overlay_text) return;
    g_overlay_text = std::move(text);
    sink(g_overlay_text, core::overlay_color(g_role));
}

void frame() {
    const cs::State state = g_session->state();
    cs::World world = read_world(state);
    const std::uint64_t now = now_ms();
    // Story 3.4a : monde changé (non chargé, ou autre héros) : répliques et PNJ capturés oubliés
    // avant tout appel au jeu (validate, lecture des PNJ, session), sans appel au jeu. Les
    // ReleaseNpc et UncaptureNpc émis ensuite par la fin de session sont alors sans effet.
    const std::uint32_t thread = GetCurrentThreadId();
    if (!world.loaded() || world.player != g_last_world_player) {
        // Story 3.5 : le détour des dégâts repasse « hors session » avant tout oubli.
        hits::publish(nullptr, g_role, false, thread);
        npc::release_all("fin de monde");
    }
    g_last_world_player = world.player;
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
        log_keepalives();
    }
    local_captures(world);
    hits::drain(g_inputs);  // stories 3.5 et 3.6 : coups du héros, coups sur l'avatar, coups natifs bloqués
    guest_health::drain(g_inputs);  // story 3.7 : mise à terre du héros invité (LocalDown)
    // Story 3.3 : répliques (et, story 3.4, PNJ animés de l'hôte) revérifiées une fois par frame,
    // avant leurs commandes ; appels de routine bloqués (IA figée), pour le bilan debug.
    if (npc::count() != 0) npc::validate();
    // Story 3.8, hôte : débuts d'attaque des PNJ capturés, entités tout juste revérifiées.
    if (g_role == Role::Host && g_native_attacks && world.loaded() && g_session->state() == cs::State::InSession) {
        npc::detect_attacks(reinterpret_cast<void*>(world.player), g_inputs);
    }
    // Story 3.8b, invité : début d'attaque de son héros (NpcAttack{2} vers l'hôte).
    if (g_role == Role::Guest && g_native_attacks && world.loaded() && g_session->state() == cs::State::InSession) {
        npc::detect_hero_attack(reinterpret_cast<void*>(world.player), g_inputs);
    }
    // Story 4.1 : répliques relevées par le détour de StartOutput (hôte, en session) ; bilan des rejeux
    // de la frame précédente. Story 4.2 : dialogue de l'invité (début, fin, répliques) ; dialogue de
    // l'hôte lu pour refuser de tenir son partenaire.
    const bool in_session = g_session->state() == cs::State::InSession;
    void* const hero = world.loaded() ? reinterpret_cast<void*>(world.player) : nullptr;
    if (g_role == Role::Host) {
        dialogue::host_frame(hero, in_session, g_inputs);
        if (in_session) dialogue::read_host_state(world);
    } else {
        dialogue::guest_frame(hero, in_session, g_inputs);
        if (in_session) npc::freed_pose(world);  // story 4.2 : pose du partenaire, vers l'hôte
    }
    npc::drain_lost(g_inputs);  // story 4.2 : tenues ou libérations impossibles (frame précédente)
    world.routine_blocked = sync::anim_filter().take_routine_blocked();
    execute(g_session->tick(now, world, g_inputs));
    // Story 4.3 : PNJ occupé par l'autre joueur (hôte : le PNJ tenu ; invité : celui de l'hôte), pour le
    // détour d'AssessTalk.
    if (const std::uint32_t busy = g_session->busy_npc(); busy != 0 && hero != nullptr) {
        dialogue::set_busy(hero, g_role == Role::Host ? npc::held_entity(busy) : npc::replica_entity(busy));
    } else {
        dialogue::set_busy(hero, nullptr);
    }
    // Story 3.5 : héros, rôle et état vus par le détour des dégâts (hors session dès Ended).
    hits::publish(world.loaded() ? reinterpret_cast<void*>(world.player) : nullptr, g_role,
                  g_session->state() == cs::State::InSession, thread);
    // Filet de sécurité : hors session, aucune réplique ne reste au registre (ReleaseNpc déjà
    // exécutés à Ended).
    if (g_session->state() != cs::State::InSession) npc::release_all("hors session");
    // Story 4.1 : avertissements des dialogues de nouveau possibles à la session suivante.
    if (state == cs::State::InSession && g_session->state() != cs::State::InSession) dialogue::reset();
    push_overlay();
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
    g_role = config.role;
    g_overlay_nickname = config.nickname;
    g_overlay_host_ip = config.host_ip;
    g_overlay_port = config.port;
    cs::Settings s;
    s.role = config.role;
    s.host_ip = config.host_ip;
    s.port = config.port;
    s.nickname = config.nickname;
    s.equipment = config.guest_equipment;
    s.mod_version = mod_version;
    s.fingerprint = fingerprint;
    // Story 3.8 : attaques natives seulement si la config les veut et que win/hooks a résolu leur bloc
    // (détour des dégâts posé) ; sinon comportement de la story 3.6.
    g_native_attacks = config.native_attacks && game::native_attacks_resolved();
    s.native_attacks = g_native_attacks;
    hits::set_native_attacks(g_native_attacks);
    // Story 4.1 : dialogues seulement si la config les veut et que win/hooks a posé le détour de StartOutput.
    s.dialogues = config.dialogues && dialogue::active();
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

void set_overlay_sink(OverlaySink sink) { g_overlay_sink.store(sink, std::memory_order_release); }

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
            // Overlay : dernier texte « session terminée », plus aucun push ensuite (g_broken).
            const OverlaySink sink = g_overlay_sink.load(std::memory_order_acquire);
            if (sink != nullptr) {
                sink(core::overlay_text(g_role, g_overlay_nickname, cs::State::Ended, g_overlay_host_ip,
                                        g_overlay_port),
                     core::overlay_color(g_role));
            }
        } catch (...) {
        }
    }
}

}  // namespace mp::sync::session
