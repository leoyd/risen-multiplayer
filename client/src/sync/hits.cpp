#include "sync/hits.h"

#include "common/log.h"
#include "game/combat.h"
#include "game/entity.h"
#include "sync/anim_filter.h"
#include "sync/avatar.h"
#include "sync/hit_rules.h"
#include "sync/npc.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>

namespace mp::sync::hits {

namespace {

namespace cs = mp::core::session;

constexpr std::size_t kMaxQueued = 64;     // HitRequest en attente de la frame (au-delà : jetés)
constexpr std::size_t kMaxDead = 256;      // répliques mortes retenues (au-delà : la session filtre)

using hit_rules::HitRole;

// État publié par la frame, lu par le détour.
std::atomic<void*> g_hero{nullptr};
std::atomic<int> g_role{static_cast<int>(HitRole::Unknown)};
std::atomic<bool> g_in_session{false};
std::atomic<std::uint32_t> g_frame_thread{0};
// Diagnostic, remis à zéro à chaque entrée en session.
std::atomic<bool> g_first_pass_logged{false};
std::atomic<bool> g_aggressor_warned{false};
std::atomic<bool> g_raw_warned{false};  // story 3.6 : dégâts bruts illisibles
std::atomic<unsigned> g_blocked{0};

// File des NetId frappés par le héros de l'invité, et répliques mortes : verrou court.
std::mutex g_mutex;
std::vector<std::uint32_t> g_queue;
struct GuestHit {
    std::uint32_t source = 0;
    std::uint32_t damage = 0;
};
std::vector<GuestHit> g_guest_hits;  // story 3.6 : HitOnGuest à émettre (hôte), 64 au plus
std::vector<std::uint32_t> g_dead;
bool g_was_in_session = false;  // frame seulement

// Garde « application d'un HitRequest » : le chemin du mod (PSDamage::DoDamage → version à 2
// arguments → jmp version à 1 argument) traverse le détour.
thread_local bool t_applying = false;

struct ApplyGuard {
    ApplyGuard() { t_applying = true; }
    ~ApplyGuard() { t_applying = false; }
    ApplyGuard(const ApplyGuard&) = delete;
    ApplyGuard& operator=(const ApplyGuard&) = delete;
};

std::string hex_ptr(const void* p) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(p)));
    return buf;
}

// Nom d'une entité pour le log : héros, avatar, PNJ N, sinon son adresse.
std::string name_of(const void* entity, bool found, const TrackedEntity& t, const void* hero) {
    if (entity == nullptr) return "nul";
    if (entity == hero) return "héros";
    if (found) {
        switch (t.role) {
            case EntityRole::LocalHero: return "héros";
            case EntityRole::Avatar: return "avatar invité";
            case EntityRole::NpcReplica:
            case EntityRole::NpcCapture: return "PNJ " + std::to_string(t.net_id);
        }
    }
    return "entité " + hex_ptr(entity);
}

const char* role_text(const void* entity, bool found, const TrackedEntity& t, const void* hero) {
    if (entity != nullptr && entity == hero) return "héros local";
    if (!found) return "hors registre";
    switch (t.role) {
        case EntityRole::LocalHero: return "héros local";
        case EntityRole::Avatar: return "avatar";
        case EntityRole::NpcReplica: return "réplique";
        case EntityRole::NpcCapture: return "PNJ capturé";
    }
    return "?";
}

bool is_dead(std::uint32_t net_id) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return std::find(g_dead.begin(), g_dead.end(), net_id) != g_dead.end();
}

void queue_hit(std::uint32_t net_id) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_queue.size() < kMaxQueued) g_queue.push_back(net_id);
}

void queue_guest_hit(std::uint32_t source, std::uint32_t damage) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_guest_hits.size() < kMaxQueued) g_guest_hits.push_back({source, damage});
}

bool decide(void* damage_ps, void* target) {
    const auto role = static_cast<HitRole>(g_role.load());
    const bool in_session = g_in_session.load();
    if (!hit_rules::considered(t_applying, in_session, role)) return false;  // règles 1 et 2
    const std::uint32_t thread = GetCurrentThreadId();
    if (!g_first_pass_logged.exchange(true)) {
        log::info("[sync] dégâts : 1er passage au point d'accroche (thread " + std::to_string(thread) +
                  ", thread OnProcess " + std::to_string(g_frame_thread.load()) + ")");
    }
    void* aggressor = nullptr;
    // Agresseur illisible : on décide quand même avec un agresseur nul ; seules les règles qui ne
    // dépendent que de la cible (3b, 4b « agresseur nul ») bloquent, le reste est laissé (3.5).
    const bool aggressor_unreadable = !game::damage_aggressor(damage_ps, aggressor);
    if (aggressor_unreadable) {
        aggressor = nullptr;
        if (!g_aggressor_warned.exchange(true)) {
            log::warn("[sync] dégâts : agresseur illisible (SEH) : coup laissé, sauf sur le héros invité ou "
                      "l'avatar invité (une fois par session)");
        }
    }
    void* hero = g_hero.load();
    AnimFilter& filter = anim_filter();
    TrackedEntity tt, ta;
    const bool target_found = filter.find_by_entity(target, tt);
    const bool aggressor_found = filter.find_by_entity(aggressor, ta);

    hit_rules::Inputs in;
    in.guard = t_applying;
    in.in_session = in_session;
    in.role = role;
    in.hero = hero;
    in.aggressor = aggressor;
    in.target = target;
    in.target_found = target_found;
    in.target_entry = tt;
    in.aggressor_found = aggressor_found;
    in.aggressor_entry = ta;
    // Réplique morte : lue seulement quand la règle 3 peut mettre son NetId en file.
    in.replica_dead = role == HitRole::Guest && target_found && tt.role == EntityRole::NpcReplica &&
                      aggressor != nullptr && aggressor == hero && is_dead(tt.net_id);
    const hit_rules::Decision d = hit_rules::decide(in);
    if (aggressor_unreadable && d.reason != hit_rules::Reason::GuestHeroByReplica &&
        d.reason != hit_rules::Reason::GuestHeroOther && d.reason != hit_rules::Reason::AvatarNoAggressor) {
        return false;  // en cas de doute, on laisse passer (3.5)
    }
    const bool block = d.block;
    if (d.queue_net_id != 0) queue_hit(d.queue_net_id);
    // Story 3.6, règle 4b : dégâts bruts du coup sur l'avatar, lus seulement s'il faut émettre.
    bool raw_ok = false;
    std::uint32_t raw = 0;
    if (d.hit_on_guest) {
        long amount = 0, bonus = 0;
        float multiplier = 0.0f;
        raw_ok = game::damage_raw(damage_ps, amount, bonus, multiplier);
        if (raw_ok) {
            raw = hit_rules::raw_damage(amount, bonus, multiplier);
            queue_guest_hit(d.source_net_id, raw);
        } else if (!g_raw_warned.exchange(true)) {
            log::warn("[sync] dégâts : dégâts bruts illisibles (SEH) : coup sur l'avatar bloqué sans HitOnGuest");
        }
    }
    std::string reason;  // coup bloqué : fin de la ligne Info
    switch (d.reason) {
        case hit_rules::Reason::HitRequest: reason = "(HitRequest)"; break;
        case hit_rules::Reason::DeadReplica: reason = "(réplique morte, sans HitRequest)"; break;
        case hit_rules::Reason::OtherAggressor: reason = "(autre agresseur, sans HitRequest)"; break;
        case hit_rules::Reason::GuestAvatar: reason = "(seuls les HitRequest blessent)"; break;
        case hit_rules::Reason::GuestHeroByReplica:
        case hit_rules::Reason::GuestHeroOther: reason = "(seuls les HitOnGuest blessent)"; break;
        case hit_rules::Reason::AvatarHit:
            reason = raw_ok ? "(HitOnGuest, dégâts bruts " + std::to_string(raw) + ")"
                            : std::string("(dégâts bruts illisibles, sans HitOnGuest)");
            break;
        case hit_rules::Reason::AvatarFriendly: reason = "(pas de tir ami)"; break;
        case hit_rules::Reason::AvatarNoAggressor: reason = "(agresseur nul, sans HitOnGuest)"; break;
        case hit_rules::Reason::Pass: break;
    }
    if (block) {
        g_blocked.fetch_add(1);
        log::info("[sync] coup natif bloqué : " + name_of(aggressor, aggressor_found, ta, hero) + " -> " +
                  name_of(target, target_found, tt, hero) + " " + reason);
    }
    if (log::debug_enabled() &&
        (target_found || aggressor_found || (hero != nullptr && (target == hero || aggressor == hero)))) {
        log::debug("[sync] dégâts : agresseur " + hex_ptr(aggressor) + " (" +
                   role_text(aggressor, aggressor_found, ta, hero) + ") cible " + hex_ptr(target) + " (" +
                   role_text(target, target_found, tt, hero) + ") -> " + (block ? "bloqué" : "laissé"));
    }
    return block;
}

}  // namespace

bool on_damage(void* damage_ps, void* target) noexcept {
    try {
        return decide(damage_ps, target);
    } catch (...) {
        return false;  // en cas de doute, on laisse passer
    }
}

void publish(void* hero, Role role, bool in_session, std::uint32_t frame_thread) {
    g_frame_thread.store(frame_thread);
    g_hero.store(hero);
    g_role.store(static_cast<int>(role == Role::Host ? HitRole::Host : HitRole::Guest));
    if (in_session && !g_was_in_session) {
        g_first_pass_logged.store(false);
        g_aggressor_warned.store(false);
        g_raw_warned.store(false);
        g_blocked.store(0);
        std::lock_guard<std::mutex> lock(g_mutex);
        g_queue.clear();
        g_guest_hits.clear();
        g_dead.clear();
    } else if (!in_session && g_was_in_session) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_queue.clear();
        g_guest_hits.clear();
        g_dead.clear();
    }
    g_was_in_session = in_session;
    g_in_session.store(in_session);
}

void* hero() { return g_hero.load(); }

bool guest_in_session() {
    return g_in_session.load() && g_role.load() == static_cast<int>(HitRole::Guest);
}

void drain(std::vector<cs::Input>& out) {
    std::vector<std::uint32_t> ids;
    std::vector<GuestHit> guest_hits;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        ids.swap(g_queue);
        guest_hits.swap(g_guest_hits);
    }
    for (std::uint32_t id : ids) out.push_back(cs::LocalHit{id});
    for (const GuestHit& h : guest_hits) out.push_back(cs::LocalHitOnGuest{h.source, h.damage});
    const unsigned blocked = g_blocked.exchange(0);
    if (blocked != 0) out.push_back(cs::LocalHitsBlocked{blocked});
}

void mark_dead(std::uint32_t net_id) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_dead.size() < kMaxDead && std::find(g_dead.begin(), g_dead.end(), net_id) == g_dead.end()) {
        g_dead.push_back(net_id);
    }
}

void apply(std::uint32_t net_id, const protocol::Guid& guid) {
    const std::string what = "[sync] HitRequest PNJ " + std::to_string(net_id);
    if (!game::damage_resolved()) {
        log::warn(what + " ignoré : dégâts inactifs (symboles introuvables)");
        return;
    }
    game::Entity target;
    if (!npc::find_entity(guid, target.ptr) || target.ptr == nullptr) {
        log::warn(what + " ignoré : PNJ introuvable");
        return;
    }
    game::Entity attacker;
    attacker.ptr = avatar::entity();
    if (attacker.ptr == nullptr) {
        log::warn(what + " ignoré : pas d'avatar");
        return;
    }
    int stack = -1;
    game::Entity weapon;
    if (!game::get_right_hold_stack(attacker, stack) || stack < 0 || !game::get_right_hold_item(attacker, weapon) ||
        weapon.ptr == nullptr) {
        log::warn(what + " ignoré : avatar sans arme en main");  // D8 : les poings ne sont pas des objets
        return;
    }
    int before = 0, after = 0;
    const bool before_ok = game::get_hit_points(target, before);
    // Le script CanDoMeleeDamage refuse le coup d'un PNJ (l'avatar) dont la cible actuelle n'est pas
    // l'entité frappée : l'avatar, à l'IA figée, n'en a pas. Cible fixée le temps du coup, puis rendue.
    game::Entity previous;
    const bool previous_ok = game::npc_current_target(attacker, previous);
    if (!game::set_npc_current_target(attacker, target)) {
        log::warn(what + " : PSNpc::SetCurrentTarget a levé une exception (SEH) : le coup sera sans doute refusé");
    }
    bool result = false;
    bool called = false;
    {
        ApplyGuard guard;  // le chemin du mod traverse le détour : il doit passer
        called = game::do_damage(weapon, target, attacker, result);
    }
    if (previous_ok) game::set_npc_current_target(attacker, previous);
    const bool after_ok = game::get_hit_points(target, after);
    if (!called) {
        log::error(what + " : PSDamage::DoDamage a levé une exception (SEH) : aucun dégât");
        return;
    }
    log::info(what + " appliqué : DoDamage -> " + (result ? "1" : "0") + ", PV " +
              (before_ok ? std::to_string(before) : std::string("?")) + " -> " +
              (after_ok ? std::to_string(after) : std::string("?")));
}

}  // namespace mp::sync::hits
