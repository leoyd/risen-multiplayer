#include "sync/probe_j2.h"

#include "common/log.h"
#include "game/combat.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace mp::sync::probe_j2 {

namespace {

constexpr float kMaxNpcDistance = 1500.0f;       // Num3 : 15 m
constexpr float kGoalPoseMaxGap = 150.0f;        // Num2 mode 3 : MoveTo au-delà de 150 cm
constexpr std::uint64_t kHoldReviveMs = 1000;    // Num6 tenu 1 s : relève
constexpr std::uint64_t kFreezeLogMs = 3000;     // Num4 : pose sur 3 s
constexpr std::uint64_t kFreezeLogStepMs = 500;
constexpr std::uint64_t kFollowUpMs = 1000;      // Num1 : relecture 1 s plus tard
constexpr std::uint64_t kSummaryPeriodMs = 1000;
constexpr long kProbeDamage = 10;
constexpr const char* kStandUpState = "ZS_StandUp";

// --- Partagé avec les détours --------------------------------------------------------------
std::atomic<void*> g_frozen_routine_ps{nullptr};  // Num4 : PS 0x24 dont l'IA est figée
std::atomic<void*> g_hero{nullptr};               // eCEntity* du héros, publié par la frame
std::atomic<bool> g_intercept{false};             // Num6
std::atomic<unsigned> g_kills_seen{0};            // Kill reçus par le détour
std::atomic<unsigned> g_kills_null_other{0};      // Kill sans victime explicite (non identifiés)
std::atomic<unsigned> g_kills_intercepted{0};
std::atomic<unsigned> g_defeat_failures{0};       // Defeat a levé une exception (SEH)
std::atomic<long long> g_last_defeat_result{0};
std::atomic<std::uint32_t> g_kill_thread{0};
std::atomic<unsigned> g_routine_blocked{0};       // OnProcess bloqués sur le PS figé

// --- Thread principal uniquement -----------------------------------------------------------
bool g_combat = false;
bool g_routine_hook = false;
bool g_kill_hook = false;
bool g_down[6] = {};
std::uint64_t g_num6_since = 0;
bool g_num6_revived = false;
std::uint64_t g_last_summary = 0;
unsigned g_logged_seen = 0, g_logged_intercepted = 0, g_logged_failures = 0, g_logged_null = 0;
// Q5
int g_copy_stack = -1;        // pile créée dans l'inventaire de la copie
bool g_copy_holding = false;  // dernière action : en main (true) ou rangée (false)
std::uint64_t g_q5_followup = 0;
game::Entity g_q5_copy;
// Q6
int g_mode = 0;
bool g_group_saved = false;
int g_saved_group = 0;
bool g_grav_saved = false;
bool g_saved_grav = true;
unsigned g_goal_moves = 0;    // mode 3 : MoveTo de recalage depuis le dernier résumé
float g_goal_max_gap = 0.0f;
unsigned g_placements = 0;
unsigned g_placement_failures = 0;
// Q3
game::Entity g_npc;
std::uint64_t g_freeze_log_start = 0;
std::uint64_t g_freeze_log_next = 0;

std::string hex_ptr(const void* p) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(p)));
    return buf;
}

std::string position(const game::Matrix& m) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "(%.1f, %.1f, %.1f)", static_cast<double>(m.m[12]), static_cast<double>(m.m[13]),
                  static_cast<double>(m.m[14]));
    return buf;
}

std::string int_or(bool ok, int v) { return ok ? std::to_string(v) : std::string("SEH"); }
std::string bool_or(bool ok, bool v) { return ok ? (v ? std::string("oui") : std::string("non")) : std::string("SEH"); }
std::string call(bool ok) { return ok ? "OK" : "SEH"; }

std::string ps_presence(const game::Entity& e, int type) {
    void* ps = nullptr;
    const bool ok = game::get_property_set(e, type, ps);
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%x", type);
    return std::string("PS ") + buf + "=" + (ok ? (ps != nullptr ? "oui" : "NON") : "SEH");
}

std::string entity_name(const game::Entity& e) {
    if (e.ptr == nullptr) return "(aucune)";
    std::string name;
    if (!game::get_entity_name(e, name)) return "(nom illisible)";
    return name.empty() ? std::string("(nom vide)") : name;
}

void log_line(const std::string& text) { log::info("[probe] J2 " + text); }

bool pressed(int vk, bool& was_down, bool focus) {
    const bool down = focus && (GetAsyncKeyState(vk) & 0x8000) != 0;
    const bool edge = down && !was_down;
    was_down = down;
    return edge;
}

// --- Q5 : arme de la copie (Num1) ----------------------------------------------------------

std::string combat_mode(const game::Entity& e) {
    int mode = 0;
    return int_or(game::get_combat_mode(e, mode), mode);
}

void num1_weapon(const game::Entity& player, const game::Entity& copy, std::uint64_t now) {
    int hero_stack = -1;
    const bool stack_ok = game::get_right_hold_stack(player, hero_stack);
    game::Entity hero_item;
    game::get_right_hold_item(player, hero_item);
    log_line("Num1 : héros pile en main=" + int_or(stack_ok, hero_stack) + " arme=« " + entity_name(hero_item) +
             " » (" + hex_ptr(hero_item.ptr) + ") mode combat=" + combat_mode(player));
    if (copy.ptr == nullptr) {
        log_line("Num1 : pas de copie (F6 d'abord)");
        return;
    }
    void* inventory = nullptr;
    game::get_property_set(copy, game::kInventoryPropertySet, inventory);
    log_line("Num1 : copie " + ps_presence(copy, game::kInventoryPropertySet) + " " +
             ps_presence(copy, game::kCombatPropertySet));
    if (inventory == nullptr) {
        log_line("Num1 : copie sans inventaire (PS 0x1f) : rien à faire");
        return;
    }
    const std::string before = combat_mode(copy);
    if (g_copy_stack < 0) {
        if (!stack_ok || hero_stack < 0) {
            log_line("Num1 : main du héros vide : sortir une arme puis réessayer");
            return;
        }
        game::Template tmpl;
        if (!game::get_inventory_template(player, hero_stack, tmpl) || tmpl.proxy == nullptr) {
            log_line("Num1 : GetTemplate(pile " + std::to_string(hero_stack) + ") du héros a échoué");
            if (tmpl.proxy != nullptr) game::destroy_template(tmpl);
            return;
        }
        int index = -1;
        const bool created = game::create_items(copy, tmpl, 1, 0, index);
        game::destroy_template(tmpl);
        log_line("Num1 : copie CreateItems(template de l'arme, 1, pile 0) " + call(created) + " -> index " +
                 std::to_string(index));
        if (!created || index < 0) return;
        g_copy_stack = index;
        g_copy_holding = false;
    }
    bool result = false;
    bool ok = false;
    const char* action = nullptr;
    if (!g_copy_holding) {
        ok = game::hold_right_stack(copy, g_copy_stack, result);
        action = "HoldRightStack";
    } else {
        ok = game::equip_stack(copy, g_copy_stack, result);
        action = "EquipStack (arme rangée)";
    }
    if (ok && result) g_copy_holding = !g_copy_holding;  // échec : même action au prochain appui
    int copy_stack = -1;
    const bool copy_stack_ok = game::get_right_hold_stack(copy, copy_stack);
    log_line(std::string("Num1 : copie ") + action + "(" + std::to_string(g_copy_stack) + ") " + call(ok) + " -> " +
             (ok ? (result ? "true" : "false") : "?") + ", mode combat avant=" + before +
             " après=" + combat_mode(copy) + ", pile en main=" + int_or(copy_stack_ok, copy_stack));
    g_q5_followup = now + kFollowUpMs;
    g_q5_copy = copy;
}

void q5_followup(const game::Entity& copy, std::uint64_t now) {
    if (g_q5_followup == 0 || now < g_q5_followup) return;
    g_q5_followup = 0;
    if (copy.ptr == nullptr || copy.ptr != g_q5_copy.ptr) return;
    int stack = -1;
    const bool ok = game::get_right_hold_stack(copy, stack);
    game::Entity item;
    game::get_right_hold_item(copy, item);
    log_line("Num1 : 1 s plus tard, copie mode combat=" + combat_mode(copy) + " pile en main=" + int_or(ok, stack) +
             " objet en main=« " + entity_name(item) + " »");
}

// --- Q6 : collisions (Num2) ----------------------------------------------------------------

void log_collision_state(const game::Entity& copy, const char* when) {
    int group = 0;
    const bool group_ok = game::get_collision_group(copy, group);
    bool grav = false;
    const bool grav_ok = game::is_gravitation_enabled(copy, grav);
    float radius = 0.0f;
    const bool radius_ok = game::get_dcc_radius(copy, radius);
    char r[32];
    std::snprintf(r, sizeof r, "%.1f", static_cast<double>(radius));
    log_line(std::string("Num2 ") + when + " : mode " + std::to_string(g_mode) + ", groupe de collision=" +
             int_or(group_ok, group) + ", " + ps_presence(copy, game::kRigidBodyPropertySet) + " " + ps_presence(copy, 0xe) + " " +
             ps_presence(copy, game::kCharacterMovementPropertySet) + " " + ps_presence(copy, game::kNavigationPropertySet) +
             ", gravité=" + bool_or(grav_ok, grav) + ", rayon DCC=" + (radius_ok ? std::string(r) : "SEH"));
}

bool has_ps(const game::Entity& e, int type) {
    void* ps = nullptr;
    return game::get_property_set(e, type, ps) && ps != nullptr;
}

void enter_solid(const game::Entity& copy) {
    if (!g_group_saved) {
        g_group_saved = game::get_collision_group(copy, g_saved_group);
    }
    const bool group = game::set_collision_group(copy, game::kCollisionGroupNpc);
    const bool coll = game::enable_collision(copy, true);
    std::string dcc = "sauté (pas de PS 0x5)";
    if (has_ps(copy, game::kNavigationPropertySet)) dcc = call(game::set_dcc_enabled(copy, true));
    log_line("Num2 : SetCollisionGroup(" + std::to_string(game::kCollisionGroupNpc) + ") " + call(group) +
             ", EnableCollision(true) " + call(coll) + ", SetDCCEnabled(true) " + dcc);
}

void set_kinematic(const game::Entity& copy, bool on) {
    std::string flags = "sauté (pas de PS 0xd)";
    if (has_ps(copy, game::kRigidBodyPropertySet)) {
        flags = call(on ? game::add_body_flag(copy, game::kBodyFlagsKinematic)
                        : game::remove_body_flag(copy, game::kBodyFlagsKinematic));
    }
    std::string grav = "sauté (pas de PS 0x15)";
    if (has_ps(copy, game::kCharacterMovementPropertySet)) {
        if (on && !g_grav_saved) g_grav_saved = game::is_gravitation_enabled(copy, g_saved_grav);
        const bool target = on ? false : (g_grav_saved ? g_saved_grav : true);
        grav = call(game::enable_gravitation(copy, target)) + std::string(target ? " (true)" : " (false)");
    }
    log_line(std::string("Num2 : ") + (on ? "AddBodyFlag(0x81) " : "RemoveBodyFlag(0x81) ") + flags +
             ", EnableGravitation " + grav);
}

void num2_collision(const game::Entity& copy) {
    if (copy.ptr == nullptr) {
        log_line("Num2 : pas de copie (F6 d'abord)");
        return;
    }
    log_collision_state(copy, "avant");
    g_mode = (g_mode + 1) % 4;
    g_goal_moves = 0;
    g_goal_max_gap = 0.0f;
    switch (g_mode) {
        case 1: enter_solid(copy); break;
        case 2: set_kinematic(copy, true); break;
        case 3: set_kinematic(copy, false); break;  // 3 = 1 + SetGoalPose : corps rendu au moteur
        default: {
            if (g_group_saved) {
                log_line("Num2 : SetCollisionGroup(" + std::to_string(g_saved_group) + ") (groupe d'origine) " +
                         call(game::set_collision_group(copy, g_saved_group)) +
                         " ; collision et DCC laissés actifs");
            }
            break;
        }
    }
    static const char* const kPlacement[] = {"MoveTo", "MoveTo (copie solide)",
                                             "SetPose + PSRigidBody::SetPose, sans StopMovement",
                                             "SetGoalPose chaque frame, MoveTo si écart > 150 cm"};
    log_line(std::string("Num2 : mode ") + std::to_string(g_mode) + " — placement en miroir : " + kPlacement[g_mode]);
    log_collision_state(copy, "après");
}

// --- Q3 : PNJ natifs (Num3, Num4, Num5) ----------------------------------------------------

std::string hex_bytes(const std::uint8_t* p, std::size_t n) {
    std::string s;
    char buf[4];
    for (std::size_t i = 0; i < n; ++i) {
        std::snprintf(buf, sizeof buf, "%02x", p[i]);
        s += buf;
    }
    return s;
}

std::string hit_points(const game::Entity& e) {
    int hp = 0, max = 0;
    const bool a = game::get_hit_points(e, hp);
    const bool b = game::get_hit_points_max(e, max);
    return int_or(a, hp) + "/" + int_or(b, max);
}

void num3_npc(const game::Entity& player, const game::Entity& copy) {
    game::Entity best;
    float distance = 0.0f;
    std::size_t total = 0;
    bool list_ok = false;
    find_nearest_npc(player, copy, kMaxNpcDistance, best, distance, total, list_ok);
    if (!list_ok) {
        log_line("Num3 : GetNPCs a échoué (SEH)");
        return;
    }
    if (best.ptr == nullptr) {
        log_line("Num3 : aucun PNJ à moins de 15 m (" + std::to_string(total) + " PNJ au total)");
        return;
    }
    if (g_npc.ptr != nullptr && g_npc.ptr != best.ptr && g_frozen_routine_ps.load() != nullptr) {
        g_frozen_routine_ps.store(nullptr);
        log_line("Num3 : IA de l'ancien PNJ " + hex_ptr(g_npc.ptr) + " libérée");
    }
    g_npc = best;
    std::uint8_t id[game::kPropertyIdSize];
    const bool id_ok = game::get_entity_id(best, id);
    void* back = nullptr;
    const bool back_ok = id_ok && game::runtime_entity_by_id(id, back);
    char dist[32];
    std::snprintf(dist, sizeof dist, "%.0f cm", static_cast<double>(distance));
    log_line("Num3 : PNJ " + hex_ptr(best.ptr) + " à " + dist + " nom=« " + entity_name(best) + " » GUID=" +
             (id_ok ? hex_bytes(id, 16) + " (bCPropertyID complet " + hex_bytes(id, sizeof id) + ")" : "SEH") +
             " PV=" + hit_points(best) + " groupe de collision=" + [&] {
                 int g = 0;
                 return int_or(game::get_collision_group(best, g), g);
             }() +
             ", gCRuntimeInfo::GetEntity(GUID)=" + (back_ok ? hex_ptr(back) : std::string("SEH")) +
             (back_ok ? (back == best.ptr ? " (== entité)" : " (DIFFÉRENT)") : std::string()));
}

// Le PNJ suivi existe-t-il encore (mort, retiré du monde) ? Sinon, il est oublié et son IA
// libérée, pour ne jamais toucher un pointeur périmé ni figer le PS qui réutiliserait l'adresse.
bool tracked_npc_alive(const char* key) {
    if (g_npc.ptr == nullptr) return false;
    std::vector<game::Entity> npcs;
    bool found = false;
    if (game::get_npcs(npcs)) {
        for (const auto& npc : npcs) found = found || npc.ptr == g_npc.ptr;
    }
    if (found) return true;
    log_line(std::string(key) + " : PNJ suivi " + hex_ptr(g_npc.ptr) + " introuvable (retiré du monde ?) : oublié, IA libérée");
    g_npc = game::Entity{};
    g_frozen_routine_ps.store(nullptr);
    g_freeze_log_start = 0;
    return false;
}

void log_npc_pose(const char* when) {
    game::Matrix pose;
    const bool ok = game::get_pose(g_npc, pose);
    int mode = 0;
    const bool mode_ok = game::get_ai_mode(g_npc, mode);
    log_line(std::string("Num4 ") + when + " : PNJ " + hex_ptr(g_npc.ptr) + " pos=" +
             (ok ? position(pose) : std::string("illisible")) + " AIMode=" + int_or(mode_ok, mode) +
             ", OnProcess bloqués=" + std::to_string(g_routine_blocked.exchange(0)));
}

void num4_freeze(std::uint64_t now) {
    if (!g_routine_hook) {
        log_line("Num4 : hook gCScriptRoutine_PS::OnProcess absent : expérience inactive");
        return;
    }
    if (g_npc.ptr == nullptr) {
        log_line("Num4 : aucun PNJ suivi (Num3 d'abord)");
        return;
    }
    if (!tracked_npc_alive("Num4")) return;
    void* ps = nullptr;
    if (!game::get_property_set(g_npc, game::kRoutinePropertySet, ps) || ps == nullptr) {
        log_line("Num4 : PNJ " + hex_ptr(g_npc.ptr) + " sans PS 0x24 (gCScriptRoutine_PS)");
        return;
    }
    const bool freeze = g_frozen_routine_ps.load() != ps;
    g_routine_blocked.store(0);
    g_frozen_routine_ps.store(freeze ? ps : nullptr);
    log_line(std::string("Num4 : IA du PNJ ") + hex_ptr(g_npc.ptr) + (freeze ? " FIGÉE" : " libérée") + " (PS 0x24 " +
             hex_ptr(ps) + ")");
    g_freeze_log_start = now;
    g_freeze_log_next = now;
}

void freeze_followup(std::uint64_t now) {
    if (g_freeze_log_start == 0 || now < g_freeze_log_next) return;
    if (g_npc.ptr == nullptr) {
        g_freeze_log_start = 0;
        return;
    }
    char when[32];
    std::snprintf(when, sizeof when, "t+%.1f s", static_cast<double>(now - g_freeze_log_start) / 1000.0);
    log_npc_pose(when);
    g_freeze_log_next += kFreezeLogStepMs;
    if (g_freeze_log_next > g_freeze_log_start + kFreezeLogMs) g_freeze_log_start = 0;
}

void num5_damage(const game::Entity& player, const game::Entity& copy) {
    if (g_npc.ptr == nullptr) {
        log_line("Num5 : aucun PNJ suivi (Num3 d'abord)");
        return;
    }
    if (!tracked_npc_alive("Num5")) return;
    if (g_frozen_routine_ps.load() != nullptr) {
        log_line("Num5 : attention, IA du PNJ figée (Num4) : sa réaction au coup et sa mort ne se déroulent pas");
    }
    game::Entity attacker = player;
    game::Entity weapon;
    bool damaged = false;  // DoDamage appelé sans exception et a rendu true
    if (copy.ptr != nullptr) {
        game::get_right_hold_item(copy, weapon);
        if (weapon.ptr != nullptr) attacker = copy;
    }
    if (weapon.ptr == nullptr) game::get_right_hold_item(player, weapon);
    if (weapon.ptr == nullptr) {
        log_line("Num5 : pas d'arme en main (copie ni héros) : DoDamage sauté");
    } else {
        long old_amount = 0;
        const bool amount_ok = game::get_damage_amount(weapon, old_amount);
        const std::string before = hit_points(g_npc);
        const bool set_ok = amount_ok && game::set_damage_amount(weapon, kProbeDamage);
        bool result = false;
        const bool ok = game::do_damage(weapon, g_npc, attacker, result);
        damaged = ok && result;
        const std::string after = hit_points(g_npc);
        if (set_ok) game::set_damage_amount(weapon, old_amount);
        log_line("Num5 : DoDamage(arme « " + entity_name(weapon) + " », PNJ, " +
                 (attacker.ptr == copy.ptr ? "copie" : "héros") + ") " + call(ok) + " -> " +
                 (ok ? (result ? "true" : "false") : "?") + ", DamageAmount " +
                 (amount_ok ? std::to_string(old_amount) : std::string("illisible (pas de gCDamage_PS ?)")) +
                 (set_ok ? " -> 10 (rétabli après)" : " (inchangé)") + ", PV " + before + " -> " + after);
    }
    if (damaged) return;  // repli seulement si DoDamage n'a pas porté : une seule source de dégâts par appui
    const std::string before = hit_points(g_npc);
    const bool ok = game::set_hit_points(g_npc, -kProbeDamage, game::kSkillModifierAdd);
    log_line("Num5 : repli SetHitPoints(-10, ajout) " + call(ok) + ", PV " + before + " -> " + hit_points(g_npc));
}

// --- Q7 : mort et relève (Num6) ------------------------------------------------------------

void toggle_intercept() {
    if (!g_kill_hook) {
        log_line("Num6 : expérience Q7 inactive (hook Kill absent) ; Num6 tenu 1 s relève le héros");
        return;
    }
    const bool on = !g_intercept.load();
    g_intercept.store(on);
    log_line(std::string("Num6 : interception de la mort du héros ") +
             (on ? "ACTIVÉE (Kill du héros → Defeat)" : "désactivée (mort native)"));
}

void revive(const game::Entity& player) {
    if (player.ptr == nullptr) {
        log_line("Num6 relève : pas de joueur");
        return;
    }
    bool dead = false;
    const bool dead_ok = game::is_dead(player, dead);
    int mode = 0, last = 0;
    const bool mode_ok = game::get_ai_mode(player, mode);
    const bool last_ok = game::get_last_ai_mode(player, last);
    log_line("Num6 relève : avant, mort=" + bool_or(dead_ok, dead) + " PV=" + hit_points(player) +
             " AIMode=" + int_or(mode_ok, mode) + " LastAIMode=" + int_or(last_ok, last));
    int max = 0;
    const bool max_ok = game::get_hit_points_max(player, max);
    log_line("Num6 relève 1/6 : SetHitPoints(max, fixer) " +
             (max_ok ? call(game::set_hit_points(player, max, game::kSkillModifierSet)) : std::string("sauté (max SEH)")) +
             ", PV=" + hit_points(player));
    const bool last_usable = last_ok && last != game::kAIModeDead && last != game::kAIModeDefeated;
    log_line("Num6 relève 2/6 : AIMode = LastAIMode (" + int_or(last_ok, last) + ") " +
             (last_usable ? call(game::set_ai_mode(player, last))
                          : std::string("sauté (LastAIMode illisible, mort ou à terre)")) +
             ", AIMode=" + [&] {
                 int m = 0;
                 return int_or(game::get_ai_mode(player, m), m);
             }());
    log_line("Num6 relève 3/6 : EnableCollision(true) " + call(game::enable_collision(player, true)));
    log_line("Num6 relève 4/6 : SetDCCEnabled(true) " +
             (has_ps(player, game::kNavigationPropertySet) ? call(game::set_dcc_enabled(player, true))
                                                           : std::string("sauté (pas de PS 0x5)")));
    void* anim = nullptr;
    std::string ragdoll = "sauté (pas d'eCAnimation_PS)";
    if (game::get_animation_ps(player, anim) && anim != nullptr) {
        ragdoll = call(game::set_ragdoll(anim, false, true, nullptr));
    }
    log_line("Num6 relève 5/6 : SetRagDollEnabled(false) " + ragdoll);
    log_line(std::string("Num6 relève 6/6 : SetState(« ") + kStandUpState + " ») " +
             call(game::set_routine_state(player, kStandUpState)));
    const bool after_ok = game::is_dead(player, dead);
    log_line("Num6 relève : après, mort=" + bool_or(after_ok, dead) + " PV=" + hit_points(player));
}

void num6(const game::Entity& player, bool focus, std::uint64_t now) {
    const bool down = focus && (GetAsyncKeyState(VK_NUMPAD6) & 0x8000) != 0;
    bool& was = g_down[5];
    if (down && !was) {
        g_num6_since = now;
        g_num6_revived = false;
    } else if (down && was && !g_num6_revived && now - g_num6_since >= kHoldReviveMs) {
        g_num6_revived = true;
        revive(player);
    } else if (!down && was && !g_num6_revived && focus) {
        toggle_intercept();  // appui court (relâché avant 1 s)
    }
    was = down;
}

void kill_summary() {
    const unsigned seen = g_kills_seen.load();
    const unsigned intercepted = g_kills_intercepted.load();
    const unsigned failures = g_defeat_failures.load();
    const unsigned null_other = g_kills_null_other.load();
    if (seen == g_logged_seen && intercepted == g_logged_intercepted && failures == g_logged_failures &&
        null_other == g_logged_null) {
        return;
    }
    const std::uint32_t tid = g_kill_thread.load();
    log_line("Num6 : Kill vus=" + std::to_string(seen) + " (sans victime explicite=" + std::to_string(null_other) +
             "), morts du héros interceptées=" + std::to_string(intercepted) + " (+" +
             std::to_string(intercepted - g_logged_intercepted) + "), Defeat en échec (SEH)=" +
             std::to_string(failures) + ", dernier résultat de Defeat=" + std::to_string(g_last_defeat_result.load()) +
             ", thread du détour=" + std::to_string(tid) +
             (tid == 0 ? "" : (tid == GetCurrentThreadId() ? " (celui d'OnProcess)" : " (DIFFÉRENT d'OnProcess)")));
    g_logged_seen = seen;
    g_logged_intercepted = intercepted;
    g_logged_failures = failures;
    g_logged_null = null_other;
}

}  // namespace

void set_status(bool combat_resolved, bool routine_hook, bool kill_hook) {
    g_combat = combat_resolved;
    g_routine_hook = combat_resolved && routine_hook;
    g_kill_hook = combat_resolved && kill_hook;
}

void find_nearest_npc(const game::Entity& player, const game::Entity& exclude, float max_distance,
                      game::Entity& best, float& distance, std::size_t& total, bool& list_ok) {
    best = game::Entity{};
    distance = 0.0f;
    total = 0;
    list_ok = false;
    game::Matrix pose;
    if (player.ptr == nullptr || !game::get_pose(player, pose)) return;
    std::vector<game::Entity> npcs;
    if (!game::get_npcs(npcs)) return;
    list_ok = true;
    total = npcs.size();
    float best_d2 = max_distance * max_distance;
    for (const auto& npc : npcs) {
        if (npc.ptr == nullptr || npc.ptr == player.ptr || npc.ptr == exclude.ptr) continue;
        game::Matrix p;
        if (!game::get_pose(npc, p)) continue;
        const float dx = p.m[12] - pose.m[12], dy = p.m[13] - pose.m[13], dz = p.m[14] - pose.m[14];
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < best_d2) {
            best_d2 = d2;
            best = npc;
        }
    }
    if (best.ptr != nullptr) distance = std::sqrt(best_d2);
}

void reset() {
    g_frozen_routine_ps.store(nullptr);
    g_npc = game::Entity{};
    g_freeze_log_start = 0;
    g_copy_stack = -1;
    g_copy_holding = false;
    g_q5_followup = 0;
    g_q5_copy = game::Entity{};
    g_mode = 0;
    g_group_saved = g_grav_saved = false;
    g_goal_moves = g_placements = g_placement_failures = 0;
    g_goal_max_gap = 0.0f;
}

void place_copy(const game::Entity& copy, const game::Matrix& target) {
    bool ok = true;
    switch (g_mode) {
        case 2:
            // Corps cinématique : la pose de l'entité et celle du corps, sans ResetAll ni StopMovement.
            ok = game::set_pose(copy, target) && game::set_body_pose(copy, target);
            break;
        case 3: {
            ok = game::set_goal_pose(copy, target);
            game::Matrix current;
            if (game::get_pose(copy, current)) {
                const float dx = current.m[12] - target.m[12], dy = current.m[13] - target.m[13],
                            dz = current.m[14] - target.m[14];
                const float gap = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (gap > g_goal_max_gap) g_goal_max_gap = gap;
                if (gap > kGoalPoseMaxGap) {
                    ++g_goal_moves;
                    ok = game::move_to(copy, target) && ok;
                }
            }
            break;
        }
        default: ok = game::move_to(copy, target); break;  // modes 0 et 1
    }
    ++g_placements;
    if (!ok) ++g_placement_failures;
}

void frame(const game::Entity& player, const game::Entity& copy, bool focus, std::uint64_t now_ms) {
    g_hero.store(player.ptr);
    if (!g_combat) {
        // ABI du combat introuvable : seules les touches sont suivies (aucun appel).
        for (int i = 0; i < 6; ++i) pressed(VK_NUMPAD1 + i, g_down[i], false);
        return;
    }
    if (player.ptr != nullptr) {
        if (pressed(VK_NUMPAD1, g_down[0], focus)) num1_weapon(player, copy, now_ms);
        if (pressed(VK_NUMPAD2, g_down[1], focus)) num2_collision(copy);
        if (pressed(VK_NUMPAD3, g_down[2], focus)) num3_npc(player, copy);
        if (pressed(VK_NUMPAD4, g_down[3], focus)) num4_freeze(now_ms);
        if (pressed(VK_NUMPAD5, g_down[4], focus)) num5_damage(player, copy);
        num6(player, focus, now_ms);
    } else {
        for (int i = 0; i < 6; ++i) pressed(VK_NUMPAD1 + i, g_down[i], false);
    }
    q5_followup(copy, now_ms);
    freeze_followup(now_ms);
    if (now_ms - g_last_summary >= kSummaryPeriodMs) {
        g_last_summary = now_ms;
        kill_summary();
        if (g_mode != 0 && g_placements != 0) {
            char gap[32];
            std::snprintf(gap, sizeof gap, "%.0f cm", static_cast<double>(g_goal_max_gap));
            log_line("Num2 : mode " + std::to_string(g_mode) + ", " + std::to_string(g_placements) +
                     " placements/s, " + std::to_string(g_placement_failures) + " en échec" +
                     (g_mode == 3 ? ", " + std::to_string(g_goal_moves) + " recalages MoveTo, écart max " + gap
                                  : std::string()));
        }
        g_placements = g_placement_failures = g_goal_moves = 0;
        g_goal_max_gap = 0.0f;
    }
}

bool on_routine_process(void* routine_ps) noexcept {
    const void* frozen = g_frozen_routine_ps.load(std::memory_order_relaxed);
    if (frozen != nullptr && routine_ps == frozen) {
        g_routine_blocked.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

bool intercept_kill(void* other) noexcept {
    g_kills_seen.fetch_add(1);
    g_kill_thread.store(GetCurrentThreadId());
    if (other == nullptr) {
        g_kills_null_other.fetch_add(1);  // victime = GetOtherEntity de l'unité : non identifiée
        return false;
    }
    if (!g_intercept.load()) return false;
    void* victim = nullptr;
    if (!game::read_entity_ptr(other, victim)) return false;  // lecture mémoire seule
    const void* hero = g_hero.load();
    return hero != nullptr && victim == hero;
}

void note_defeat(bool call_ok, long long result) noexcept {
    if (!call_ok) {
        g_defeat_failures.fetch_add(1);  // le détour retombe sur le Kill d'origine
        return;
    }
    g_kills_intercepted.fetch_add(1);
    g_last_defeat_result.store(result);
}

}  // namespace mp::sync::probe_j2
