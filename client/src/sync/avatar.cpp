#include "sync/avatar.h"

#include "common/log.h"
#include "game/combat.h"
#include "game/entity.h"
#include "game/motion_desc.h"
#include "sync/anim_filter.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mp::sync::avatar {

namespace {

using core::session::Pose;

// Table NetId ↔ entité (AD-6) : au plus un avatar en v3 (celui du pair).
struct Entry {
    std::uint32_t net_id = 0;
    game::Entity entity;
    void* animation_ps = nullptr;  // suivi dans le registre (nul : pas de PS trouvé)
    void* wrapper = nullptr;       // enveloppe EMotionFX, suivie aussi (story 2.4b)
    void* routine_ps = nullptr;    // gCScriptRoutine_PS (0x24), au registre (AD-12)
    void* player = nullptr;        // héros local à la création : le monde de l'avatar
    bool move_error_logged = false;
    bool dead = false;  // en ragdoll (Death) : plus de pose ni d'animation imposées
    bool param_error_logged = false;
    // Stories 3.2 à 3.2c : pile de chaque objet (arme, équipement, armure) trouvé ou créé dans son
    // inventaire (une par nom), noms en échec déjà signalés. Vidés avec l'entrée, au retrait.
    std::unordered_map<std::string, int> item_stacks;
    std::unordered_set<std::string> item_failed;
    bool stack_fallback_logged = false;  // repli sur une pile mémorisée (FindStackIndex en SEH)
    // Story 3.2b (test en jeu du 2026-10-21) : équipement et arme différés de kInventoryReadyFrames
    // frames après la création. Équipée avant le premier gCInventory_PS::OnCacheIn de l'entité,
    // l'armure est marquée liée, et OnCacheIn ne retire plus l'objet par défaut de l'emplacement
    // (tunique du héros) : les deux s'affichent (désassemblage de Game.dll).
    unsigned frames = 0;
    bool inventory_ready = false;
    std::vector<std::string> pending_equipment;
    bool pending_weapon_set = false;
    std::string pending_weapon;
    bool pending_armor_set = false;  // story 3.2c : dernier Armor reçu avant que l'inventaire soit prêt
    std::string pending_armor;
};
constexpr unsigned kInventoryReadyFrames = 15;  // environ 250 ms à 60 images/s
std::vector<Entry> g_table;

// Rejeu par nom : ressource de chaque nom (nul : introuvable), vidée au retrait d'un avatar ;
// noms déjà signalés en échec (une ligne chacun).
std::unordered_map<std::string, void*> g_resources;
std::unordered_set<std::string> g_failed_names;

// Diagnostic 2.4b : réglages de locomotion du héros local (comptés, dernières valeurs des types
// 0 et 1) et réglages bloqués sur l'avatar, journalisés chaque seconde en debug.
std::atomic<void*> g_local_wrapper{nullptr};
void* g_local_ps = nullptr;  // frame seulement
struct WrapperStats {
    unsigned hero[4] = {};
    unsigned blocked[4] = {};
    float hero_speed[2] = {};
    float hero_weight[2] = {};
};
std::mutex g_stats_mutex;
WrapperStats g_stats;
// Dernier réglage capturé par (type, nature), sous g_stats_mutex : un réglage strictement
// identique n'entre pas dans l'anneau (le moteur en lance des centaines par seconde). Invalidé
// par un PlayMotion du héros sur ce type, et au changement d'enveloppe.
struct LastCapture {
    bool valid = false;
    float value = 0.0f;
    float time = 0.0f;
};
LastCapture g_last_capture[8][4];
ULONGLONG g_next_stats_log = 0;

void log_wrapper_stats() {
    if (!log::debug_enabled()) return;
    const ULONGLONG t = GetTickCount64();
    if (t < g_next_stats_log) return;
    g_next_stats_log = t + 1000;
    WrapperStats s;
    {
        std::lock_guard<std::mutex> lock(g_stats_mutex);
        s = g_stats;
        g_stats = WrapperStats{};
        // Dernières valeurs conservées d'une seconde à l'autre.
        for (int i = 0; i < 2; ++i) {
            g_stats.hero_speed[i] = s.hero_speed[i];
            g_stats.hero_weight[i] = s.hero_weight[i];
        }
    }
    char buf[320];
    std::snprintf(buf, sizeof buf,
                  "[sync] locomotion (1 s) — héros : SetPlaySpeed %u (type0 %.2f, type1 %.2f), SetTargetWeight %u "
                  "(type0 %.2f, type1 %.2f), SwitchFadeOut %u, StopMotion %u ; bloqués sur l'avatar : %u, %u, %u, %u",
                  s.hero[0], static_cast<double>(s.hero_speed[0]), static_cast<double>(s.hero_speed[1]), s.hero[1],
                  static_cast<double>(s.hero_weight[0]), static_cast<double>(s.hero_weight[1]), s.hero[2], s.hero[3],
                  s.blocked[0], s.blocked[1], s.blocked[2], s.blocked[3]);
    log::debug(buf);
}

std::string hex_ptr(const void* p) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(p)));
    return buf;
}

std::string pose_text(const Pose& p) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "(%.1f, %.1f, %.1f) lacet=%.3f", static_cast<double>(p.x),
                  static_cast<double>(p.y), static_cast<double>(p.z), static_cast<double>(p.yaw));
    return buf;
}

game::Matrix matrix_of(const Pose& p) {
    game::Matrix m{};
    core::pose_matrix(p, m.m);  // AD-7, testé sous Linux
    return m;
}

// Le monde de la création est-il toujours là ? Sinon l'avatar est détruit avec lui.
bool same_world(const Entry& e) {
    game::Entity player;
    bool running = false;
    return game::get_player(player) && player.ptr != nullptr && player.ptr == e.player &&
           game::game_running(running) && running;
}

// Diagnostic (story 3.2b), en debug : inventaire de `entity`, pile par pile.
void log_inventory(const game::Entity& entity, const std::string& who) {
    if (!log::debug_enabled()) return;
    std::vector<game::StackInfo> stacks;
    if (!game::inventory_stacks(entity, stacks)) {
        log::debug(who + " : inventaire illisible (symboles de diagnostic absents ou SEH)");
        return;
    }
    std::string text = who + " : inventaire, " + std::to_string(stacks.size()) + " pile(s)";
    for (const game::StackInfo& st : stacks) {
        text += " | " + std::to_string(st.index) + " « " + st.name + " »" + (st.equipped ? " équipée" : "") +
                " emplacement " + std::to_string(st.slot);
    }
    log::debug(text);
}

// Pile de `tmpl` (nom `name`) dans l'inventaire de `entity`, trouvée (FindStackIndex(-1, false))
// ou, à défaut, créée (CreateItems(…, 1, 0)). Avec `memory` (avatar), la pile est mémorisée par nom
// (partagée entre l'arme de la 3.2 et l'équipement de la 3.2b) et sert de repli si FindStackIndex
// lève une exception (SEH, signalée une fois). True : `stack` ≥ 0 et `how` = « trouvée »,
// « mémorisée » ou « créée ». False : FindStackIndex a levé une exception sans pile mémorisée
// (`how` = « FindStackIndex », rien n'est créé : pas de doublon) ou CreateItems a échoué (`how` =
// « CreateItems », `stack` = sa valeur de retour) ; rien n'est journalisé ici. Le template reste à
// l'appelant.
bool ensure_stack(const game::Entity& entity, const game::Template& tmpl, const std::string& name, Entry* memory,
                  const std::string& who, int& stack, const char*& how) {
    stack = -1;
    how = "trouvée";
    const bool found = game::find_stack_index(entity, tmpl, stack);
    auto known = memory != nullptr ? memory->item_stacks.find(name) : std::unordered_map<std::string, int>::iterator{};
    const bool has_known = memory != nullptr && known != memory->item_stacks.end();
    if (found && stack < 0 && has_known) {
        memory->item_stacks.erase(known);  // objet sorti de l'inventaire : pile mémorisée périmée
    }
    if (!found || stack < 0) {
        if (!found && has_known) {
            stack = known->second;  // repli : FindStackIndex a levé une exception (SEH)
            how = "mémorisée";
            if (!memory->stack_fallback_logged) {
                memory->stack_fallback_logged = true;
                log::warn(who + " : FindStackIndex a levé une exception (SEH) : pile mémorisée reprise");
            }
        } else if (!found) {
            how = "FindStackIndex";  // SEH : on ne sait pas si l'objet est là, on ne crée rien
            return false;
        } else {
            stack = -1;
            how = "CreateItems";
            if (!game::create_items(entity, tmpl, 1, 0, stack) || stack < 0) return false;
            how = "créée";
        }
    }
    if (memory != nullptr) memory->item_stacks[name] = stack;
    return true;
}

// Raison d'un échec d'ensure_stack, d'après `how`.
std::string stack_failure(const char* how, const std::string& name, int stack) {
    if (how != nullptr && std::string(how) == "FindStackIndex") {
        return "FindStackIndex(« " + name + " ») a levé une exception (SEH), rien de créé";
    }
    return "CreateItems(« " + name + " ») a échoué (pile " + std::to_string(stack) + ")";
}

// Équipe `entity`, nom par nom (story 3.2b) : Template(nom) puis IsValid, pile trouvée ou créée
// (ensure_stack), puis EquipStack sauf si IsStackEquiped. Un nom en échec est journalisé et
// n'empêche pas les suivants. `memory` : entrée de l'avatar (piles mémorisées), nul pour le héros.
void equip_entity(const game::Entity& entity, const std::vector<std::string>& names, Entry* memory,
                  const std::string& who) {
    unsigned equipped = 0, failed = 0;
    for (const std::string& name : names) {
        game::Template tmpl;
        bool valid = false;
        if (!game::template_from_name(name.c_str(), tmpl, valid)) {
            log::error(who + " : Template(« " + name + " ») a levé une exception (SEH)");
            ++failed;
            continue;
        }
        if (!valid) {
            game::destroy_template(tmpl);
            log::warn(who + " : template « " + name + " » inconnu du jeu, ignoré");
            ++failed;
            continue;
        }
        int stack = -1;
        const char* how = nullptr;
        const bool ok_stack = ensure_stack(entity, tmpl, name, memory, who, stack, how);
        game::destroy_template(tmpl);
        if (!ok_stack) {
            log::error(who + " : " + stack_failure(how, name, stack));
            ++failed;
            continue;
        }
        bool already = false;
        if (!game::is_stack_equipped(entity, stack, already)) {
            log::error(who + " : IsStackEquiped(" + std::to_string(stack) + ") a levé une exception (SEH)");
            ++failed;
            continue;
        }
        if (already) {
            log::info(who + " : « " + name + " » déjà équipé (pile " + std::to_string(stack) + " " + how + ")");
            ++equipped;
            continue;
        }
        bool ok = false;
        if (!game::equip_stack(entity, stack, ok)) {
            log::error(who + " : EquipStack(" + std::to_string(stack) + ") a levé une exception (SEH)");
            ++failed;
            continue;
        }
        if (ok) {
            log::info(who + " : « " + name + " » équipé (pile " + std::to_string(stack) + " " + how +
                      ", EquipStack → true)");
            ++equipped;
        } else {
            log::warn(who + " : « " + name + " » non équipé (pile " + std::to_string(stack) + " " + how +
                      ", EquipStack → false)");
            ++failed;
        }
    }
    log::info(who + " : " + std::to_string(equipped) + " objet(s) équipé(s), " + std::to_string(failed) +
              " en échec, sur " + std::to_string(names.size()));
}

// Pile de l'objet `name` dans l'inventaire de l'avatar (arme, story 3.2 ; armure, story 3.2c) :
// Template(nom) puis IsValid, puis ensure_stack. Un échec est journalisé une fois par nom
// (Entry::item_failed) et laisse l'avatar inchangé.
bool avatar_stack(Entry& e, const std::string& name, const std::string& who, int& stack, const char*& how) {
    stack = -1;
    how = nullptr;
    game::Template tmpl;
    bool valid = false;
    if (!game::template_from_name(name.c_str(), tmpl, valid)) {
        if (e.item_failed.insert(name).second) {
            log::error(who + " : Template(« " + name + " ») a levé une exception (SEH)");
        }
        return false;
    }
    if (!valid) {
        game::destroy_template(tmpl);
        if (e.item_failed.insert(name).second) {
            log::warn(who + " : template « " + name + " » inconnu du jeu, avatar inchangé");
        }
        return false;
    }
    const bool ok = ensure_stack(e.entity, tmpl, name, &e, who, stack, how);
    game::destroy_template(tmpl);
    if (!ok && e.item_failed.insert(name).second) log::error(who + " : " + stack_failure(how, name, stack));
    return ok;
}
}  // namespace

void spawn(std::uint32_t net_id, const Pose& pose) {
    for (const Entry& e : g_table) {
        if (e.net_id == net_id) {
            log::warn("[sync] avatar " + std::to_string(net_id) + " déjà présent : Spawn ignoré");
            return;
        }
    }
    game::Entity player;
    if (!game::get_player(player) || player.ptr == nullptr) {
        log::error("[sync] avatar " + std::to_string(net_id) + " : pas de héros local, avatar non créé");
        return;
    }
    game::Template tmpl;
    if (!game::get_template(player, tmpl) || tmpl.proxy == nullptr) {
        if (tmpl.proxy != nullptr) game::destroy_template(tmpl);
        log::error("[sync] avatar " + std::to_string(net_id) + " : Entity::GetTemplate a échoué, avatar non créé");
        return;
    }
    Entry e;
    e.net_id = net_id;
    e.player = player.ptr;
    if (!game::spawn(tmpl, matrix_of(pose), /*temporary=*/true, e.entity) || e.entity.ptr == nullptr) {  // tmpl consommé
        log::error("[sync] avatar " + std::to_string(net_id) + " : Entity::Spawn a échoué, avatar non créé");
        return;
    }
    if (!game::get_animation_ps(e.entity, e.animation_ps) || e.animation_ps == nullptr) {
        e.animation_ps = nullptr;
        log::warn("[sync] avatar " + std::to_string(net_id) + " : pas d'eCAnimation_PS, le moteur peut l'animer");
    } else if (!game::get_actor_wrapper(e.animation_ps, e.wrapper) || e.wrapper == nullptr) {
        // Story 2.4b : l'enveloppe sert à bloquer les réglages de locomotion du moteur.
        e.wrapper = nullptr;
        log::warn("[sync] avatar " + std::to_string(net_id) + " : pas d'enveloppe EMotionFX, locomotion non protégée");
    }
    if (!game::get_property_set(e.entity, game::kRoutinePropertySet, e.routine_ps)) e.routine_ps = nullptr;
    // Registre (AD-12) : seul sync/avatar y inscrit ses avatars.
    TrackedEntity tracked;
    tracked.entity = e.entity.ptr;
    tracked.animation_ps = e.animation_ps;
    tracked.routine_ps = e.routine_ps;
    tracked.wrapper = e.wrapper;
    tracked.role = EntityRole::Avatar;
    anim_filter().track(tracked);
    anim_filter().take_blocked();  // compteur remis à zéro : bilan au retrait
    g_table.push_back(e);
    log::info("[sync] avatar " + std::to_string(net_id) + " créé : entité " + hex_ptr(e.entity.ptr) + ", PS " +
              hex_ptr(e.animation_ps) + ", " + pose_text(pose));
}

void despawn(std::uint32_t net_id) {
    for (auto it = g_table.begin(); it != g_table.end(); ++it) {
        if (it->net_id != net_id) continue;
        const Entry e = *it;
        // AD-11, AD-12 : d'abord le registre et la table, ensuite seulement la destruction.
        anim_filter().untrack(e.entity.ptr);
        g_table.erase(it);
        g_resources.clear();
        g_failed_names.clear();
        const std::string blocked = std::to_string(anim_filter().take_blocked());
        if (!same_world(e)) {
            log::info("[sync] avatar " + std::to_string(net_id) + " oublié : monde détruit, avatar avec lui (" +
                      blocked + " appels moteur bloqués)");
            return;
        }
        if (game::kill(e.entity)) {
            log::info("[sync] avatar " + std::to_string(net_id) + " détruit (Entity::Kill, " + blocked +
                      " appels moteur bloqués)");
        } else {
            log::error("[sync] avatar " + std::to_string(net_id) + " : Entity::Kill a levé une exception (SEH)");
        }
        return;
    }
    log::debug("[sync] avatar " + std::to_string(net_id) + " absent : rien à retirer");
}

// Équipement, armure puis arme reçus avant que l'inventaire de l'avatar soit prêt (voir
// Entry::frames) : l'armure (Armor, état courant du pair) passe après l'équipement du Hello.
void apply_pending_inventory(Entry& e) {
    e.inventory_ready = true;
    log::debug("[sync] avatar " + std::to_string(e.net_id) + " : inventaire prêt après " + std::to_string(e.frames) +
               " frames");
    if (!e.pending_equipment.empty()) {
        const std::vector<std::string> names = std::move(e.pending_equipment);
        e.pending_equipment.clear();
        equip(e.net_id, names);
    }
    if (e.pending_armor_set) {
        e.pending_armor_set = false;
        set_armor(e.net_id, e.pending_armor);
    }
    if (e.pending_weapon_set) {
        e.pending_weapon_set = false;
        set_weapon(e.net_id, e.pending_weapon);
    }
}

void move(std::uint32_t net_id, const Pose& pose) {
    for (Entry& e : g_table) {
        if (e.net_id != net_id) continue;
        if (e.move_error_logged || e.dead) return;  // après une SEH, ou mort : plus de pose imposée
        if (!e.inventory_ready && ++e.frames >= kInventoryReadyFrames) apply_pending_inventory(e);
        // Corps cinématique (AD-8, story 3.1) : pose de l'entité puis du corps, sans MoveTo
        // (StopMovement) ni SetGoalPose.
        // Story 3.1 (tests en jeu du 2026-10-20) : les réglages d'avatar solide du spike J2
        // décrochaient le modèle animé de l'avatar (seule l'arme suivait). Avatar traversable :
        // placement par MoveTo, comme en J1.
        if (!game::move_to(e.entity, matrix_of(pose))) {
            e.move_error_logged = true;
            log::error("[sync] avatar " + std::to_string(net_id) +
                       " : Entity::MoveTo a levé une exception (SEH) : avatar plus déplacé");
        }
        log_wrapper_stats();
        return;
    }
}

void play(std::uint32_t net_id, const std::string& name, const protocol::Motion& motion) {
    for (Entry& e : g_table) {
        if (e.net_id != net_id) continue;
        if (e.dead || e.animation_ps == nullptr) return;
        auto failed = [&](const char* why) {
            if (g_failed_names.insert(name).second) {
                log::warn("[sync] avatar " + std::to_string(net_id) + " : « " + name + " » non rejoué (" + why + ")");
            }
        };
        auto it = g_resources.find(name);
        if (it == g_resources.end()) {
            void* resource = nullptr;
            if (!game::get_motion(name, resource)) {
                failed("GetMotion a levé une exception (SEH)");
                return;  // pas mis en cache : on réessaiera
            }
            it = g_resources.emplace(name, resource).first;
        }
        if (it->second == nullptr) {
            failed("ressource introuvable");
            return;
        }
        static std::uint8_t desc[game::kPlayDescSize];
        const void* desc_arg = game::replay_desc(motion, desc);
        if (!game::set_motion(e.animation_ps, motion.type, it->second)) {
            failed("SetMotion a levé une exception (SEH)");
            return;
        }
        AnimFilter::ReplayGuard guard;  // le hook laisse passer ce PlayMotion
        if (!game::play_motion(e.animation_ps, motion.type, const_cast<void*>(desc_arg), game::replay_flag(motion))) {
            failed("PlayMotion a levé une exception (SEH)");
        }
        return;
    }
}

void play_param(std::uint32_t net_id, const protocol::MotionParam& param) {
    for (Entry& e : g_table) {
        if (e.net_id != net_id) continue;
        if (e.dead || e.wrapper == nullptr) return;
        bool ok = false;
        {
            AnimFilter::ReplayGuard guard;  // le hook laisse passer ce réglage sur la réplique
            switch (static_cast<protocol::MotionParamKind>(param.kind)) {
                case protocol::MotionParamKind::TargetWeight:
                    ok = game::set_target_weight(e.wrapper, param.type, param.value, param.time);
                    break;
                case protocol::MotionParamKind::PlaySpeed:
                    ok = game::set_play_speed(e.wrapper, param.type, param.value);
                    break;
                case protocol::MotionParamKind::FadeOut:
                    ok = game::switch_fade_out(e.wrapper, param.type, param.value != 0.0f, param.time);
                    break;
                case protocol::MotionParamKind::StopMotion:
                    ok = game::wrapper_stop_motion(e.wrapper, param.type, param.time, param.value != 0.0f);
                    break;
            }
        }
        if (!ok && !e.param_error_logged) {
            e.param_error_logged = true;
            log::error("[sync] avatar " + std::to_string(net_id) + " : réglage de locomotion a levé une exception (SEH)");
        }
        return;
    }
}

void die(std::uint32_t net_id) {
    for (Entry& e : g_table) {
        if (e.net_id != net_id || e.dead) continue;
        if (e.animation_ps == nullptr) {
            log::warn("[sync] avatar " + std::to_string(net_id) + " mort, mais sans eCAnimation_PS : pas de ragdoll");
            return;
        }
        bool ok = false;
        {
            AnimFilter::ReplayGuard guard;
            ok = game::set_ragdoll(e.animation_ps, true, false, nullptr);
        }
        // En cas d'échec, l'avatar garde pose et animations plutôt que de rester figé debout.
        e.dead = ok;
        if (ok) {
            log::info("[sync] avatar " + std::to_string(net_id) + " en ragdoll (mort)");
        } else {
            log::error("[sync] avatar " + std::to_string(net_id) + " : SetRagDollEnabled a levé une exception (SEH)");
        }
        return;
    }
}

void set_weapon(std::uint32_t net_id, const std::string& name) {
    for (Entry& e : g_table) {
        if (e.net_id != net_id) continue;
        const std::string who = "[sync] arme : avatar " + std::to_string(net_id);
        if (e.dead) {
            log::debug(who + " mort : Weapon ignoré");
            return;
        }
        if (!e.inventory_ready) {  // appliqué par move(), une fois l'inventaire prêt
            e.pending_weapon_set = true;
            e.pending_weapon = name;
            log::debug(who + " : Weapon « " + name + " » différé (inventaire pas encore prêt)");
            return;
        }
        if (name.empty()) {
            int held = -1;
            if (!game::get_right_hold_stack(e.entity, held)) {
                log::error(who + " : GetRightHoldStack a levé une exception (SEH)");
                return;
            }
            if (held < 0) {
                log::debug(who + " : main déjà vide, rien à ranger");
                return;
            }
            bool ok = false;
            if (!game::equip_stack(e.entity, held, ok)) {
                log::error(who + " : EquipStack a levé une exception (SEH)");
            } else {
                log::info(who + " range son arme (pile " + std::to_string(held) + ", EquipStack → " +
                          (ok ? "true" : "false") + ")");
            }
            return;
        }
        int stack = -1;
        const char* how = nullptr;
        if (!avatar_stack(e, name, who, stack, how)) return;
        bool ok = false;
        if (!game::hold_right_stack(e.entity, stack, ok)) {
            log::error(who + " : HoldRightStack a levé une exception (SEH)");
            return;
        }
        log::info(who + " tient « " + name + " » (pile " + std::to_string(stack) + " " + how + ", HoldRightStack → " +
                  (ok ? "true" : "false") + ")");
        return;
    }
    log::debug("[sync] arme : avatar " + std::to_string(net_id) + " absent : Weapon ignoré");
}

void set_armor(std::uint32_t net_id, const std::string& name) {
    for (Entry& e : g_table) {
        if (e.net_id != net_id) continue;
        const std::string who = "[sync] armure : avatar " + std::to_string(net_id);
        if (e.dead) {
            log::debug(who + " mort : Armor ignoré");
            return;
        }
        if (!e.inventory_ready) {  // appliqué par move(), une fois l'inventaire prêt (dernier reçu)
            e.pending_armor_set = true;
            e.pending_armor = name;
            log::debug(who + " : Armor « " + name + " » différé (inventaire pas encore prêt)");
            return;
        }
        if (name.empty()) {
            int worn = -1;
            if (!game::find_stack_by_equip_slot(e.entity, game::kEquipSlotArmor, worn)) {
                log::error(who + " : FindStackIndex(emplacement " + std::to_string(game::kEquipSlotArmor) +
                           ") a levé une exception (SEH)");
                return;
            }
            if (worn < 0) {
                log::debug(who + " : aucune armure portée, rien à retirer");
                return;
            }
            bool ok = false;
            if (!game::unequip_stack(e.entity, worn, ok)) {
                log::error(who + " : UnEquipStack(" + std::to_string(worn) + ") a levé une exception (SEH)");
                return;
            }
            log::info(who + " retire son armure (pile " + std::to_string(worn) + ", UnEquipStack → " +
                      (ok ? "true" : "false") + ")");
            log_inventory(e.entity, "[sync] diagnostic : avatar " + std::to_string(net_id) + " sans armure");
            return;
        }
        int stack = -1;
        const char* how = nullptr;
        if (!avatar_stack(e, name, who, stack, how)) return;
        bool already = false;
        if (!game::is_stack_equipped(e.entity, stack, already)) {
            log::error(who + " : IsStackEquiped(" + std::to_string(stack) + ") a levé une exception (SEH)");
            return;
        }
        if (already) {
            log::info(who + " porte déjà « " + name + " » (pile " + std::to_string(stack) + " " + how + ")");
            log_inventory(e.entity, "[sync] diagnostic : avatar " + std::to_string(net_id) + " en armure");
            return;
        }
        bool ok = false;
        if (!game::equip_stack(e.entity, stack, ok)) {
            log::error(who + " : EquipStack(" + std::to_string(stack) + ") a levé une exception (SEH)");
            return;
        }
        log::info(who + " porte « " + name + " » (pile " + std::to_string(stack) + " " + how + ", EquipStack → " +
                  (ok ? "true" : "false") + ")");
        log_inventory(e.entity, "[sync] diagnostic : avatar " + std::to_string(net_id) + " en armure");
        return;
    }
    log::debug("[sync] armure : avatar " + std::to_string(net_id) + " absent : Armor ignoré");
}

void equip(std::uint32_t net_id, const std::vector<std::string>& names) {
    for (Entry& e : g_table) {
        if (e.net_id != net_id) continue;
        const std::string who = "[sync] équipement : avatar " + std::to_string(net_id);
        if (e.dead) {
            log::warn(who + " mort : non équipé");
            return;
        }
        if (!e.inventory_ready) {  // appliqué par move(), une fois l'inventaire prêt
            e.pending_equipment = names;
            log::debug(who + " : équipement différé (inventaire pas encore prêt)");
            return;
        }
        equip_entity(e.entity, names, &e, who);
        log_inventory(e.entity, "[sync] diagnostic : avatar " + std::to_string(net_id) + " équipé");
        return;
    }
    log::warn("[sync] équipement : avatar " + std::to_string(net_id) + " absent : non équipé");
}

void equip_player(const std::vector<std::string>& names) {
    game::Entity player;
    if (!game::get_player(player) || player.ptr == nullptr) {
        log::error("[sync] équipement : pas de héros local, non équipé");
        return;
    }
    equip_entity(player, names, nullptr, "[sync] équipement : héros");
}

bool local_weapon(std::string& name) {
    name.clear();
    game::Entity player;
    if (!game::get_player(player) || player.ptr == nullptr) return false;
    int held = -1;
    if (!game::get_right_hold_stack(player, held)) return false;
    if (held < 0) return true;  // arme rangée (ou pas d'inventaire)
    game::Entity item;
    if (!game::get_right_hold_item(player, item)) return false;
    if (item.ptr == nullptr) return true;
    return game::get_entity_name(item, name);
}

bool local_armor(std::string& name) {
    name.clear();
    game::Entity player;
    if (!game::get_player(player) || player.ptr == nullptr) return false;
    int stack = -1;
    if (!game::find_stack_by_equip_slot(player, game::kEquipSlotArmor, stack)) return false;
    if (stack < 0) return true;  // aucune armure (ou pas d'inventaire)
    game::Entity item;
    if (!game::get_template_item(player, stack, item)) return false;
    if (item.ptr == nullptr) return true;  // objet nul : traité comme sans armure, comme local_weapon
    return game::get_entity_name(item, name);
}

bool present() { return !g_table.empty(); }

void* entity() { return g_table.empty() ? nullptr : g_table.front().entity.ptr; }

void teleport_player(const Pose& pose) {
    game::Entity player;
    if (!game::get_player(player) || player.ptr == nullptr) {
        log::error("[sync] téléportation impossible : pas de héros local");
        return;
    }
    if (!game::move_to(player, matrix_of(pose))) {
        log::error("[sync] téléportation : Entity::MoveTo a levé une exception (SEH)");
        return;
    }
    log::info("[sync] héros local placé devant l'hôte : " + pose_text(pose));
}

bool on_play_motion(void* animation_ps, int type, void* desc, bool flag) noexcept {
    AnimFilter& filter = anim_filter();
    switch (filter.classify(animation_ps, GetCurrentThreadId())) {
        case AnimFilter::Verdict::Pass: return true;
        case AnimFilter::Verdict::Block: return false;  // le moteur n'anime jamais un avatar (AD-8)
        case AnimFilter::Verdict::Capture: break;
    }
    if (type >= 0 && type < 8) {  // nouvelle animation : ses réglages repartiront, même identiques
        std::lock_guard<std::mutex> lock(g_stats_mutex);
        for (LastCapture& lc : g_last_capture[type]) lc.valid = false;
    }
    Capture c;
    c.type = type;
    c.flag = flag;
    c.desc_present = desc != nullptr && game::copy_bytes(desc, c.desc, sizeof c.desc);
    game::read_motion_resource(animation_ps, type, c.resource);
    filter.push(c);
    return true;
}

void track_local(void* animation_ps) {
    if (animation_ps == g_local_ps) return;
    g_local_ps = animation_ps;
    void* wrapper = nullptr;
    if (animation_ps != nullptr && !game::get_actor_wrapper(animation_ps, wrapper)) wrapper = nullptr;
    g_local_wrapper.store(wrapper);
    std::lock_guard<std::mutex> lock(g_stats_mutex);
    for (auto& per_type : g_last_capture) {
        for (LastCapture& lc : per_type) lc = LastCapture{};
    }
}

bool on_wrapper_call(void* wrapper, WrapperCall call, int type, float value, float time) noexcept {
    const int k = static_cast<int>(call);
    if (wrapper != nullptr && wrapper == g_local_wrapper.load()) {
        {
            std::lock_guard<std::mutex> lock(g_stats_mutex);
            ++g_stats.hero[k];
            if (type == 0 || type == 1) {
                if (call == WrapperCall::PlaySpeed) g_stats.hero_speed[type] = value;
                if (call == WrapperCall::TargetWeight) g_stats.hero_weight[type] = value;
            }
        }
        // Story 2.4b : poids cible et vitesse de lecture partent en MotionParam (même anneau que
        // les PlayMotion : l'ordre est conservé).
        if (type >= 0 && type < 8) {
            // Natures de MotionParam : 0 poids, 1 vitesse, 2 fondu, 3 arrêt.
            const int kind = call == WrapperCall::TargetWeight ? 0
                             : call == WrapperCall::PlaySpeed  ? 1
                             : call == WrapperCall::FadeOut    ? 2
                                                               : 3;
            const float t = kind == 1 ? 0.0f : time;
            {
                std::lock_guard<std::mutex> lock(g_stats_mutex);
                LastCapture& last = g_last_capture[type][kind];
                if (last.valid && last.value == value && last.time == t) return true;
                last = LastCapture{true, value, t};
            }
            Capture c;
            c.kind = CaptureKind::Param;
            c.type = type;
            c.param_kind = static_cast<std::uint8_t>(kind);
            c.value = value;
            c.time = t;
            anim_filter().push(c);
        }
        return true;
    }
    if (anim_filter().classify(wrapper, GetCurrentThreadId()) != AnimFilter::Verdict::Block) return true;
    std::lock_guard<std::mutex> lock(g_stats_mutex);
    ++g_stats.blocked[k];
    return false;  // le contrôleur de l'avatar ne règle plus nos animations rejouées
}

bool on_set_ragdoll(void* animation_ps, bool on, bool flag, const float* vector) noexcept {
    AnimFilter& filter = anim_filter();
    // Le ragdoll du héros local n'est pas capturé : la mort part sur Entity::IsDead.
    (void)flag;
    (void)vector;
    return filter.classify_ragdoll(animation_ps, on, GetCurrentThreadId()) != AnimFilter::Verdict::Block;
}

}  // namespace mp::sync::avatar
