#include "sync/npc.h"

#include "common/log.h"
#include "game/combat.h"
#include "game/entity.h"
#include "game/motion_desc.h"
#include "sync/anim_filter.h"
#include "sync/avatar.h"
#include "sync/hit_rules.h"
#include "sync/hits.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mp::sync::npc {

bool find_entity(const protocol::Guid& guid, void*& out) {
    std::uint8_t id[game::kPropertyIdSize] = {};
    std::memcpy(id, guid.data(), guid.size());
    return game::runtime_entity_by_id(id, out);
}

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
    void* animation_ps = nullptr;  // story 3.4 : au registre (moteur bloqué), cible du rejeu
    void* wrapper = nullptr;
    bool param_error_logged = false;
    // Story 3.4b : morte (NpcDeath) : plus de pose, de PV ni de rejeu ; reste au registre (IA,
    // animations et ragdoll natifs bloqués) jusqu'à sa libération.
    bool dead = false;
};
std::map<std::uint32_t, Replica> g_replicas;
// Story 3.4, invité : ressource de chaque nom d'animation (nul : introuvable) et noms déjà signalés
// en échec, vidés par release_all, hors session (pointeurs d'un monde peut-être détruit).
constexpr std::size_t kMaxResources = 4096;
std::unordered_map<std::string, void*> g_resources;
std::unordered_set<std::string> g_failed_names;
// Story 3.4, hôte : PNJ animés (rôle NpcCapture), NetId → GUID et entité (revérifiée par validate).
struct Captured {
    protocol::Guid guid{};
    game::Entity entity;
};
std::map<std::uint32_t, Captured> g_captured;
// PNJ que la session veut animer (CaptureNpc) sans capture active (échec, ou PNJ perdu) : nouvel
// essai depuis validate(), au plus une fois par seconde, jusqu'au succès ou à uncapture/release_all.
struct Wanted {
    protocol::Guid guid{};
    std::chrono::steady_clock::time_point next_try{};
};
std::map<std::uint32_t, Wanted> g_wanted;
constexpr std::chrono::seconds kCaptureRetry{1};
std::set<std::uint32_t> g_capture_failed;  // « capture échouée » : une ligne par NetId et par session
std::set<std::uint32_t> g_capture_lost;    // « PNJ animé perdu » : une ligne par NetId et par session

// Story 3.8, hôte : dernier état du système de combat de chaque PNJ capturé (D1), oublié avec sa
// capture. Invité : SEH d'une attaque native déjà signalé (une ligne par session).
std::map<std::uint32_t, hit_rules::AttackState> g_attack_state;
bool g_attack_error_logged = false;
bool g_detect_error_logged = false;

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

// Sort la réplique du registre (IA rendue) et de la table, journalisé une fois par NetId et par
// session ; ses commandes suivantes sont ignorées. Aucun appel au jeu.
std::map<std::uint32_t, Replica>::iterator drop(std::map<std::uint32_t, Replica>::iterator it, const std::string& why) {
    anim_filter().untrack(it->second.entity.ptr);
    const std::uint32_t net_id = it->first;
    if (g_missing.insert(net_id).second) {
        // Story 3.4b : une réplique morte n'a pas d'IA à rendre, elle reste morte.
        std::string text = why;
        const std::string ia = "IA rendue";
        if (it->second.dead) {
            const auto at = text.find(ia);
            text = at == std::string::npos ? text + ", reste morte" : text.replace(at, ia.size(), "reste morte");
        }
        log::warn(who(net_id) + " : " + text + " : oubliée, snapshots ignorés");
    }
    return g_replicas.erase(it);
}

// Essai de capture : true si le PNJ est inscrit au registre (rôle NpcCapture).
bool try_capture(std::uint32_t net_id, const protocol::Guid& guid) {
    auto fail = [&](const std::string& why) {
        if (g_capture_failed.insert(net_id).second) {
            log::warn(who(net_id) + " : capture échouée (" + why + ") : animations non envoyées, nouvel essai chaque seconde");
        }
        return false;
    };
    void* found = nullptr;
    if (!find_entity(guid, found) || found == nullptr) return fail("GUID " + guid_text(guid) + " introuvable chez l'hôte");
    Captured c;
    c.guid = guid;
    c.entity.ptr = found;
    TrackedEntity tracked;
    tracked.entity = found;
    tracked.net_id = net_id;
    tracked.role = EntityRole::NpcCapture;
    if (!game::get_animation_ps(c.entity, tracked.animation_ps) || tracked.animation_ps == nullptr) {
        return fail("pas d'eCAnimation_PS");
    }
    if (!game::get_actor_wrapper(tracked.animation_ps, tracked.wrapper)) tracked.wrapper = nullptr;
    anim_filter().track(tracked);
    g_captured.emplace(net_id, c);
    log::debug(who(net_id) + " animé : PS " + hex_ptr(tracked.animation_ps) + ", enveloppe " + hex_ptr(tracked.wrapper));
    return true;
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
    // Story 3.4 : PS d'animation et enveloppe au registre : le moteur de l'invité n'anime plus la
    // réplique (AD-8). Absents : journalisé, la réplique reste animée par son moteur.
    if (!game::get_animation_ps(r.entity, r.animation_ps) || r.animation_ps == nullptr) {
        r.animation_ps = nullptr;
        log::warn(who(net_id) + " : pas d'eCAnimation_PS : animée par le moteur de l'invité");
    } else if (!game::get_actor_wrapper(r.animation_ps, r.wrapper) || r.wrapper == nullptr) {
        r.wrapper = nullptr;
        log::warn(who(net_id) + " : pas d'enveloppe EMotionFX : locomotion non protégée");
    }
    // Registre (AD-12) : seul sync/npc y inscrit des répliques.
    TrackedEntity tracked;
    tracked.entity = r.entity.ptr;
    tracked.routine_ps = r.routine_ps;
    tracked.animation_ps = r.animation_ps;
    tracked.wrapper = r.wrapper;
    tracked.net_id = net_id;
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
              hex_ptr(found) + ", PS " + hex_ptr(r.animation_ps) + ", GUID " + guid_text(guid) +
              ", IA et animations figées");
}

void play(std::uint32_t net_id, const std::string& name, const protocol::Motion& motion) {
    auto it = g_replicas.find(net_id);
    if (it == g_replicas.end() || it->second.dead || it->second.animation_ps == nullptr) return;
    Replica& r = it->second;
    auto failed = [&](const char* why) {
        if (g_failed_names.insert(name).second) {
            log::warn(who(net_id) + " : « " + name + " » non rejoué (" + why + ")");
        }
    };
    auto res = g_resources.find(name);
    if (res == g_resources.end()) {
        void* resource = nullptr;
        if (!game::get_motion(name, resource)) {
            failed("GetMotion a levé une exception (SEH)");
            return;  // pas mis en cache : on réessaiera
        }
        if (g_resources.size() >= kMaxResources) g_resources.clear();
        res = g_resources.emplace(name, resource).first;
    }
    if (res->second == nullptr) {
        failed("ressource introuvable");
        return;
    }
    static std::uint8_t desc[game::kPlayDescSize];
    const void* desc_arg = game::replay_desc(motion, desc);
    if (!game::set_motion(r.animation_ps, motion.type, res->second)) {
        failed("SetMotion a levé une exception (SEH)");
        return;
    }
    AnimFilter::ReplayGuard guard;  // le hook laisse passer ce PlayMotion sur la réplique
    if (!game::play_motion(r.animation_ps, motion.type, const_cast<void*>(desc_arg), game::replay_flag(motion))) {
        failed("PlayMotion a levé une exception (SEH)");
    }
}

void play_param(std::uint32_t net_id, const protocol::MotionParam& param) {
    auto it = g_replicas.find(net_id);
    if (it == g_replicas.end() || it->second.dead || it->second.wrapper == nullptr) return;
    Replica& r = it->second;
    bool ok = false;
    {
        AnimFilter::ReplayGuard guard;  // le hook laisse passer ce réglage sur la réplique
        switch (static_cast<protocol::MotionParamKind>(param.kind)) {
            case protocol::MotionParamKind::TargetWeight:
                ok = game::set_target_weight(r.wrapper, param.type, param.value, param.time);
                break;
            case protocol::MotionParamKind::PlaySpeed:
                ok = game::set_play_speed(r.wrapper, param.type, param.value);
                break;
            case protocol::MotionParamKind::FadeOut:
                ok = game::switch_fade_out(r.wrapper, param.type, param.value != 0.0f, param.time);
                break;
            case protocol::MotionParamKind::StopMotion:
                ok = game::wrapper_stop_motion(r.wrapper, param.type, param.time, param.value != 0.0f);
                break;
        }
    }
    if (!ok && !r.param_error_logged) {
        r.param_error_logged = true;
        log::error(who(net_id) + " : réglage de locomotion a levé une exception (SEH)");
    }
}

void capture(std::uint32_t net_id, const protocol::Guid& guid) {
    if (g_captured.count(net_id) != 0) return;
    g_wanted.erase(net_id);
    if (!try_capture(net_id, guid)) g_wanted[net_id] = Wanted{guid, std::chrono::steady_clock::now() + kCaptureRetry};
}

void uncapture(std::uint32_t net_id) {
    g_wanted.erase(net_id);
    g_attack_state.erase(net_id);  // story 3.8 : rentré dans le groupe, il repart de la première lecture
    auto it = g_captured.find(net_id);
    if (it == g_captured.end()) return;
    anim_filter().untrack(it->second.entity.ptr);  // AD-12 : le registre d'abord
    g_captured.erase(it);
    log::debug(who(net_id) + " : retiré du registre des captures");
}

void move(std::uint32_t net_id, const Pose& pose) {
    auto it = g_replicas.find(net_id);
    if (it == g_replicas.end() || it->second.dead) return;  // inconnu, introuvable, perdu ou mort : ignoré
    game::Matrix m{};
    matrix_of(pose, m);
    if (!game::move_to(it->second.entity, m)) drop(it, "Entity::MoveTo a levé une exception (SEH), IA rendue");
}

void set_hp(std::uint32_t net_id, int hp) {
    auto it = g_replicas.find(net_id);
    if (it == g_replicas.end() || it->second.dead) return;
    if (!game::set_hit_points(it->second.entity, hp, game::kSkillModifierSet)) {
        log::warn(who(net_id) + " : SetHitPoints a levé une exception (SEH)");
        return;
    }
    log::debug(who(net_id) + " : PV = " + std::to_string(hp));
}

void die(std::uint32_t net_id) {
    auto it = g_replicas.find(net_id);
    if (it == g_replicas.end()) {
        log::warn(who(net_id) + " : Death sans réplique chez l'invité (introuvable ou perdue) : rien à tuer");
        return;
    }
    if (it->second.dead) return;  // déjà morte
    Replica& r = it->second;
    // Morte quoi qu'il arrive (D6) : figée debout plutôt que reprendre vie. Pas de Kill natif (ni
    // XP, ni script, ni quête) : PV 0, AIMode 12 (Entity::IsDead), puis ragdoll.
    r.dead = true;
    hits::mark_dead(net_id);  // story 3.5 : plus aucun HitRequest pour elle
    std::string failed;
    if (!game::set_hit_points(r.entity, 0, game::kSkillModifierSet)) {
        log::warn(who(net_id) + " mort : SetHitPoints(0) a levé une exception (SEH)");
        failed += ", PV";
    }
    if (!game::set_ai_mode(r.entity, game::kAIModeDead)) {
        log::warn(who(net_id) + " mort : PropertyAIMode = 12 impossible (SEH ou symbole absent)");
        failed += ", AIMode";
    }
    bool ragdoll = false;
    if (r.animation_ps == nullptr) {
        log::warn(who(net_id) + " mort : pas d'eCAnimation_PS : pas de ragdoll");
        failed += ", ragdoll";
    } else {
        {
            AnimFilter::ReplayGuard guard;  // le hook laisse passer ce ragdoll sur la réplique
            ragdoll = game::set_ragdoll(r.animation_ps, true, false, nullptr);
        }
        if (!ragdoll) {
            log::error(who(net_id) + " mort : SetRagDollEnabled a levé une exception (SEH)");
            failed += ", ragdoll";
        }
    }
    log::info(who(net_id) + " mort : PV 0, AIMode 12, ragdoll" +
              (failed.empty() ? std::string() : " (en échec : " + failed.substr(2) + ")"));
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
    // Story 3.4, hôte : un PNJ capturé détruit ne doit pas laisser son PS au registre (adresse
    // réutilisée par une autre entité). Il reste voulu (g_wanted) : nouvel essai ci-dessous.
    const auto now_t = std::chrono::steady_clock::now();
    for (auto it = g_captured.begin(); it != g_captured.end();) {
        void* now = nullptr;
        if (find_entity(it->second.guid, now) && now == it->second.entity.ptr) {
            ++it;
            continue;
        }
        anim_filter().untrack(it->second.entity.ptr);
        g_attack_state.erase(it->first);  // story 3.8
        if (g_capture_lost.insert(it->first).second) {
            log::warn(who(it->first) + " : PNJ animé perdu (GetEntity ne le retrouve plus) : nouvel essai chaque seconde");
        }
        g_wanted[it->first] = Wanted{it->second.guid, now_t + kCaptureRetry};
        it = g_captured.erase(it);
    }
    // PNJ voulus sans capture : un essai par seconde au plus, jusqu'au succès ou à uncapture.
    for (auto it = g_wanted.begin(); it != g_wanted.end();) {
        if (now_t < it->second.next_try) {
            ++it;
            continue;
        }
        if (try_capture(it->first, it->second.guid)) {
            it = g_wanted.erase(it);
            continue;
        }
        it->second.next_try = now_t + kCaptureRetry;
        ++it;
    }
}

void release(std::uint32_t net_id) {
    auto it = g_replicas.find(net_id);
    if (it == g_replicas.end()) return;
    // AD-12 : le registre d'abord (IA rendue), puis la table. Aucun appel au jeu.
    const bool dead = it->second.dead;
    anim_filter().untrack(it->second.entity.ptr);
    g_replicas.erase(it);
    if (dead) {
        log::info(who(net_id) + " libéré : reste morte");  // story 3.4b : AIMode 12 et 0 PV gardés
    } else {
        log::debug(who(net_id) + " libéré : IA rendue");
    }
}

void release_all(const std::string& cause) {
    g_attack_state.clear();  // story 3.8 : sans appel au jeu
    g_attack_error_logged = false;
    g_detect_error_logged = false;
    if (g_replicas.empty() && g_missing.empty() && g_captured.empty() && g_capture_failed.empty() &&
        g_wanted.empty() && g_capture_lost.empty() &&
        g_resources.empty() && g_failed_names.empty()) {
        return;
    }
    std::size_t dead = 0;  // story 3.4b : répliques mortes, laissées mortes (aucun appel au jeu)
    for (const auto& entry : g_replicas) {
        anim_filter().untrack(entry.second.entity.ptr);
        dead += entry.second.dead ? 1 : 0;
    }
    for (const auto& entry : g_captured) anim_filter().untrack(entry.second.entity.ptr);
    if (!g_replicas.empty()) {
        log::info("[sync] PNJ : " + std::to_string(g_replicas.size()) + " réplique(s) oubliée(s)" +
                  (dead > 0 ? ", dont " + std::to_string(dead) + " morte(s), laissée(s) mortes" : std::string()) +
                  " : " + cause);
    }
    if (!g_captured.empty()) {
        log::info("[sync] PNJ : " + std::to_string(g_captured.size()) + " PNJ animé(s) retiré(s) : " + cause);
    }
    g_replicas.clear();
    g_missing.clear();
    g_captured.clear();
    g_wanted.clear();
    g_capture_failed.clear();
    g_capture_lost.clear();
    g_resources.clear();  // ressources d'un monde peut-être détruit
    g_failed_names.clear();
}

void attack(std::uint32_t net_id, std::uint32_t move, std::uint32_t target) {
    auto it = g_replicas.find(net_id);
    if (it == g_replicas.end() || it->second.dead) {
        log::debug(who(net_id) + " : NpcAttack sans réplique vivante chez l'invité : ignoré");
        return;
    }
    if (!game::native_attacks_resolved()) return;  // la session ne l'émet pas dans ce cas
    Replica& r = it->second;
    // Cible : 1 avatar de l'hôte, 2 héros local, ≥ 1000 réplique de ce NetId, sinon aucune.
    game::Entity aim;
    if (target == protocol::kHostNetId) {
        aim.ptr = avatar::entity();
    } else if (target == protocol::kGuestNetId) {
        aim.ptr = hits::hero();
    } else if (target >= protocol::kFirstNpcNetId) {
        aim.ptr = replica_entity(target);
    }
    const std::string what = who(net_id) + " : attaque native (mouvement " + std::to_string(move) + ", cible " +
                             std::to_string(target) + ")";
    // Cible nulle ou introuvable : effacée (Entity nulle), sinon StartMove viserait l'ancienne cible
    // de la réplique, peut-être le héros local.
    // Test du 2026-10-24 : coups locaux refusés (DoDamage -> 0, ni sang ni recul) : le script
    // CanDoMeleeDamage exige que la cible PNJ (PSNpc, distincte de celle de PSCombatSystem) soit
    // l'entité frappée (docs/re/degats-hote.md). Les deux sont fixées.
    const bool npc_target_ok = game::set_npc_current_target(r.entity, aim);
    bool target_set = false;
    if (!game::combat_set_current_target(r.entity, aim, target_set)) {
        if (!g_attack_error_logged) {
            g_attack_error_logged = true;
            log::warn(what + " : PSCombatSystem::SetCurrentTarget a levé une exception (SEH) : ignorée "
                             "(une fois par session)");
        }
        return;
    }
    // Hors garde de rejeu : l'animation locale du mouvement reste bloquée par le registre (AD-8),
    // l'animation visible est celle rejouée depuis l'hôte.
    bool started = false;
    if (!game::combat_start_move(r.entity, static_cast<int>(move), started)) {
        if (!g_attack_error_logged) {
            g_attack_error_logged = true;
            log::warn(what + " : PSCombatSystem::StartMove a levé une exception (SEH) : ignorée (une fois par session)");
        }
        return;
    }
    // Test du 2026-10-24 : StartMove -> 0 sur toutes les répliques. Le mouvement est cherché dans le
    // style de combat de l'arme tenue ; une créature à l'IA figée n'a jamais sorti son arme naturelle.
    // Main vide : arme naturelle (GetRightFistItem) cherchée dans l'inventaire et tenue, puis nouvel essai.
    std::string retry;
    if (!started) {
        int held = -1;
        game::get_right_hold_stack(r.entity, held);
        retry = ", main " + std::to_string(held);
        if (held < 0) {
            game::Template fist;
            int stack = -1;
            const bool fist_ok = game::combat_right_fist_item(r.entity, fist) && fist.proxy != nullptr;
            if (fist_ok) game::find_stack_index(r.entity, fist, stack);
            // Test du 2026-10-24 : l'arme naturelle (It_Fist_Monster) n'est pas dans l'inventaire de la
            // réplique (créée par l'IA à l'entrée en combat) : créée ici si elle manque.
            bool created = false;
            if (fist_ok && stack < 0) created = game::create_items(r.entity, fist, 1, 0, stack) && stack >= 0;
            if (fist.proxy != nullptr) game::destroy_template(fist);
            bool held_ok = false;
            if (stack >= 0) game::hold_right_stack(r.entity, stack, held_ok);
            if (held_ok) game::combat_start_move(r.entity, static_cast<int>(move), started);
            retry += std::string(", arme naturelle ") +
                     (fist_ok ? "pile " + std::to_string(stack) + (created ? " (créée)" : "") : "absente") +
                     ", HoldRightStack -> " + (held_ok ? "1" : "0") + ", StartMove -> " + (started ? "1" : "0");
        }
    }
    log::debug(what + " : PSNpc::SetCurrentTarget " + (npc_target_ok ? "fait" : "en échec") + ", SetCurrentTarget -> " +
               (aim.ptr == nullptr ? std::string("(effacée) ") : std::string()) + (target_set ? "1" : "0") +
               ", StartMove -> " + (started ? "1" : "0") + retry);
}

void detect_attacks(void* hero, std::vector<cs::Input>& out) {
    if (g_captured.empty() || !game::native_attacks_resolved()) return;
    void* const avatar_entity = avatar::entity();
    for (const auto& [net_id, c] : g_captured) {
        int current = 0, last = 0;
        if (!game::combat_current_move(c.entity, current) || !game::combat_last_attack_move(c.entity, last)) {
            if (!g_detect_error_logged) {
                g_detect_error_logged = true;
                log::warn(who(net_id) + " : PSCombatSystem illisible (SEH) : attaque non détectée à cette frame "
                                        "(signalé une fois par session, tous PNJ confondus)");
            }
            continue;
        }
        hit_rules::AttackState& state = g_attack_state[net_id];
        const hit_rules::AttackState before = state;
        const bool started = hit_rules::attack_started(state, current, last);
        // Hypothèses H1 à H3 (spec 3.8) : chaque transition (mouvement en cours, dernier mouvement
        // d'attaque) d'un PNJ capturé, en debug.
        if (log::debug_enabled() && (!before.known || before.current != current || before.last_attack != last)) {
            log::debug(who(net_id) + " : combat (CurrentMove, LastAttackMove) = (" + std::to_string(current) + ", " +
                       std::to_string(last) + ")" + (started ? " : début d'attaque" : ""));
        }
        if (!started) continue;
        game::Entity aim;
        std::uint32_t target = 0;
        if (game::combat_current_target(c.entity, aim)) {
            TrackedEntity entry;
            const bool found = aim.ptr != nullptr && anim_filter().find_by_entity(aim.ptr, entry);
            target = hit_rules::attack_target_net_id(aim.ptr, hero, avatar_entity, found, entry);
        }
        if (log::debug_enabled()) {  // test du 2026-10-24 : arme tenue par le PNJ au début de l'attaque
            int held = -1;
            game::Entity item;
            std::string name;
            if (game::get_right_hold_stack(c.entity, held) && held >= 0 && game::get_right_hold_item(c.entity, item) &&
                item.ptr != nullptr) {
                game::get_entity_name(item, name);
            }
            log::debug(who(net_id) + " : attaque (mouvement " + std::to_string(last) + "), main " + std::to_string(held) +
                       (name.empty() ? std::string() : " « " + name + " »"));
        }
        out.push_back(cs::LocalNpcAttack{net_id, static_cast<std::uint32_t>(last), target});
    }
}

std::size_t count() { return g_replicas.size() + g_captured.size() + g_wanted.size(); }

void* replica_entity(std::uint32_t net_id) {
    const auto it = g_replicas.find(net_id);
    return it == g_replicas.end() || it->second.dead ? nullptr : it->second.entity.ptr;
}

}  // namespace mp::sync::npc
