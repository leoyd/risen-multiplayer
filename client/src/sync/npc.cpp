#include "sync/npc.h"

#include "common/log.h"
#include "game/combat.h"
#include "game/entity.h"
#include "sync/anim_filter.h"
#include "sync/avatar.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace mp::sync::npc {

namespace {

using core::session::Pose;
namespace cs = mp::core::session;

constexpr std::size_t kMaxNpcList = 4096;            // entités lues au plus dans ms_arrNPCs
constexpr float kReadRadius = 2.0f * cs::kNpcRadius;  // marge : « hors du rayon » au journal

// Invité : table NetId → réplique. Seul endroit où un pointeur de PNJ est gardé.
struct Replica {
    protocol::Guid guid{};
    game::Entity entity;
    void* routine_ps = nullptr;
};
std::map<std::uint32_t, Replica> g_replicas;
// NetId dont le GUID est introuvable (ou la réplique perdue) : une ligne par NetId et par session.
std::set<std::uint32_t> g_missing;

// Hôte : erreurs déjà signalées (une ligne chacune, jusqu'au redémarrage).
bool g_list_error_logged = false;
bool g_list_capped_logged = false;
bool g_hp_error_logged = false;

std::string guid_text(const protocol::Guid& g) {
    char buf[2 * protocol::kGuidSize + 1];
    for (std::size_t i = 0; i < g.size(); ++i) std::snprintf(buf + 2 * i, 3, "%02x", static_cast<unsigned>(g[i]));
    return buf;
}

std::string hex_ptr(const void* p) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(p)));
    return buf;
}

std::string who(std::uint32_t net_id) { return "[sync] PNJ " + std::to_string(net_id); }

void matrix_of(const Pose& pose, game::Matrix& m) { core::pose_matrix(pose, m.m); }

Pose pose_of(const game::Matrix& m) {
    return Pose{m.m[12], m.m[13], m.m[14], std::atan2(m.m[8], m.m[10])};  // AD-7
}

float distance(const Pose& a, const Pose& b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// GetEntity(GUID + 4 octets nuls) : entité native, nul si introuvable. False sur SEH.
bool find_entity(const protocol::Guid& guid, void*& out) {
    std::uint8_t id[game::kPropertyIdSize] = {};
    std::memcpy(id, guid.data(), guid.size());
    return game::runtime_entity_by_id(id, out);
}

// Sort la réplique du registre (IA rendue) et de la table, journalisé une fois par NetId et par
// session ; ses commandes suivantes sont ignorées. Aucun appel au jeu.
std::map<std::uint32_t, Replica>::iterator drop(std::map<std::uint32_t, Replica>::iterator it, const std::string& why) {
    anim_filter().untrack(it->second.entity.ptr);
    const std::uint32_t net_id = it->first;
    if (g_missing.insert(net_id).second) log::warn(who(net_id) + " : " + why + " : oubliée, snapshots ignorés");
    return g_replicas.erase(it);
}

}  // namespace

void read_host(cs::World& world) {
    world.npcs.clear();
    world.npcs_ok = false;
    if (!world.loaded() || !world.pose_ok) return;
    std::vector<game::Entity> list;
    std::size_t total = 0;
    if (!game::read_npc_list(list, kMaxNpcList, total)) {
        if (!g_list_error_logged) {
            g_list_error_logged = true;
            log::error("[sync] PNJ : lecture de Entity::ms_arrNPCs impossible (SEH ou symboles absents) : "
                       "aucun PNJ répliqué");
        }
        return;
    }
    world.npcs_ok = true;
    if (total > kMaxNpcList && !g_list_capped_logged) {
        g_list_capped_logged = true;
        log::warn("[sync] PNJ : Entity::ms_arrNPCs compte " + std::to_string(total) + " entités : seules les " +
                  std::to_string(kMaxNpcList) + " premières (ordre du tableau) sont lues");
    }
    const Pose hero{world.x, world.y, world.z, world.yaw};
    void* const player = reinterpret_cast<void*>(world.player);
    void* const avatar_entity = avatar::entity();
    bool avatar_ok = false;
    Pose avatar_pose;
    if (avatar_entity != nullptr) {
        game::Matrix m{};
        if (game::get_pose(game::Entity{avatar_entity}, m)) {
            avatar_pose = pose_of(m);
            avatar_ok = true;
        }
    }
    for (const game::Entity& e : list) {
        if (e.ptr == nullptr || e.ptr == player || e.ptr == avatar_entity) continue;
        bool is_player = false;
        if (!game::is_player(e, is_player) || is_player) continue;
        game::Matrix m{};
        if (!game::get_pose(e, m)) continue;
        cs::NpcState n;
        n.pose = pose_of(m);
        float d = distance(n.pose, hero);
        if (avatar_ok) d = std::fmin(d, distance(n.pose, avatar_pose));
        if (!(d <= kReadRadius)) continue;
        bool dead = false;
        if (!game::is_dead(e, dead)) continue;
        n.alive = !dead;
        std::uint8_t id[game::kPropertyIdSize];
        if (!game::get_entity_id(e, id)) continue;
        std::memcpy(n.guid.data(), id, n.guid.size());
        if (!game::get_hit_points(e, n.hp)) {  // PV illisibles : pas d'entrée à ce tick (jamais 0 répliqué)
            if (!g_hp_error_logged) {
                g_hp_error_logged = true;
                log::warn("[sync] PNJ : PSSkills::GetHitPoints a échoué (SEH) : PNJ ignoré à ce tick");
            }
            continue;
        }
        world.npcs.push_back(n);
    }
}

void spawn(std::uint32_t net_id, const protocol::Guid& guid, const Pose& pose) {
    if (g_replicas.count(net_id) != 0) {
        log::warn(who(net_id) + " : déjà répliqué : Spawn ignoré");
        return;
    }
    void* found = nullptr;
    if (!find_entity(guid, found) || found == nullptr) {
        // Q10 : PNJ absent chez l'invité (créé en cours de partie chez l'hôte ?). Le PNJ éventuel
        // de l'invité garde son IA ; une ligne par NetId.
        if (g_missing.insert(net_id).second) {
            log::warn(who(net_id) + " : GUID " + guid_text(guid) + " introuvable chez l'invité : ignoré");
        }
        return;
    }
    Replica r;
    r.guid = guid;
    r.entity.ptr = found;
    for (const auto& [id, other] : g_replicas) {
        if (other.entity.ptr == found) {
            log::warn(who(net_id) + " : même entité que le PNJ " + std::to_string(id) + " : ignoré");
            return;
        }
    }
    if (!game::get_property_set(r.entity, game::kRoutinePropertySet, r.routine_ps) || r.routine_ps == nullptr) {
        // IA impossible à figer : comme un GUID introuvable (ni registre ni placement).
        if (g_missing.insert(net_id).second) {
            log::warn(who(net_id) + " : pas de gCScriptRoutine_PS (IA impossible à figer) : ignoré");
        }
        return;
    }
    g_missing.erase(net_id);
    // Registre (AD-12) : seul sync/npc y inscrit des répliques. Sans PS d'animation (story 3.4).
    TrackedEntity tracked;
    tracked.entity = r.entity.ptr;
    tracked.routine_ps = r.routine_ps;
    tracked.role = EntityRole::NpcReplica;
    anim_filter().track(tracked);
    auto it = g_replicas.emplace(net_id, r).first;
    game::Matrix m{};
    matrix_of(pose, m);
    if (!game::move_to(r.entity, m)) {
        drop(it, "Entity::MoveTo a levé une exception (SEH), IA rendue");
        return;
    }
    std::string name;
    game::get_entity_name(r.entity, name);  // diagnostic seulement (vide si indisponible)
    log::info(who(net_id) + " répliqué : " + (name.empty() ? std::string() : "« " + name + " », ") + "entité " +
              hex_ptr(found) + ", GUID " + guid_text(guid) + ", IA figée");
}

void move(std::uint32_t net_id, const Pose& pose) {
    auto it = g_replicas.find(net_id);
    if (it == g_replicas.end()) return;  // inconnu, introuvable ou perdu : ignoré
    game::Matrix m{};
    matrix_of(pose, m);
    if (!game::move_to(it->second.entity, m)) drop(it, "Entity::MoveTo a levé une exception (SEH), IA rendue");
}

void set_hp(std::uint32_t net_id, int hp) {
    auto it = g_replicas.find(net_id);
    if (it == g_replicas.end()) return;
    if (!game::set_hit_points(it->second.entity, hp, game::kSkillModifierSet)) {
        log::warn(who(net_id) + " : SetHitPoints a levé une exception (SEH)");
        return;
    }
    log::debug(who(net_id) + " : PV = " + std::to_string(hp));
}

void validate() {
    for (auto it = g_replicas.begin(); it != g_replicas.end();) {
        void* now = nullptr;
        if (find_entity(it->second.guid, now) && now == it->second.entity.ptr) {
            ++it;
            continue;
        }
        it = drop(it, "réplique perdue (GetEntity ne la retrouve plus)");
    }
}

void release(std::uint32_t net_id) {
    auto it = g_replicas.find(net_id);
    if (it == g_replicas.end()) return;
    // AD-12 : le registre d'abord (IA rendue), puis la table. Aucun appel au jeu.
    anim_filter().untrack(it->second.entity.ptr);
    g_replicas.erase(it);
    log::debug(who(net_id) + " libéré : IA rendue");
}

void release_all() {
    if (g_replicas.empty() && g_missing.empty()) return;
    for (const auto& entry : g_replicas) anim_filter().untrack(entry.second.entity.ptr);
    if (!g_replicas.empty()) {
        log::info("[sync] PNJ : " + std::to_string(g_replicas.size()) + " réplique(s) oubliée(s) hors session");
    }
    g_replicas.clear();
    g_missing.clear();
}

std::size_t count() { return g_replicas.size(); }

}  // namespace mp::sync::npc
