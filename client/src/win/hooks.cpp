#include "win/hooks.h"

#include "common/log.h"
#include "game/combat.h"
#include "game/dialogue.h"
#include "game/entity.h"
#include "sync/anim_filter.h"
#include "sync/avatar.h"
#include "sync/dialogue.h"
#include "sync/guest_health.h"
#include "sync/hits.h"
#include "sync/probe.h"
#include "sync/probe_j2.h"
#include "sync/probe_j3.h"
#include "sync/session.h"
#include "win/watchdog.h"

#include <MinHook.h>

#include <string>

namespace mp::win::hooks {

namespace {

game::OnProcessFn g_original_on_process = nullptr;
game::PlayMotionFn g_original_play_motion = nullptr;
game::SetRagdollFn g_original_set_ragdoll = nullptr;
game::SetPlaySpeedFn g_original_set_play_speed = nullptr;
game::SetTargetWeightFn g_original_set_target_weight = nullptr;
game::SwitchFadeOutFn g_original_switch_fade_out = nullptr;
game::WrapperStopMotionFn g_original_wrapper_stop_motion = nullptr;
game::RoutineOnProcessFn g_original_routine_on_process = nullptr;  // spike J2 (Num4)
game::ScriptFn g_original_kill = nullptr;                           // spike J2 (Num6)
game::ScriptFn g_original_down_or_dead[3] = {};  // spike J2 (Num6, 5ᵉ test) ; session (story 3.7) : sonde et session sont exclusives
game::StartOutputFn g_original_start_output = nullptr;            // spike J3 (journal des répliques) ; session (story 4.1) : sonde et session sont exclusives
using DamageDoDamageFn = bool (*)(void* damage_ps, void* target);
DamageDoDamageFn g_original_damage = nullptr;                       // story 3.5 (Q8)
bool g_initialized = false;

void detour_on_process(void* application) {
    g_original_on_process(application);
    sync::probe::on_frame();
}

void detour_on_process_session(void* application) {
    g_original_on_process(application);
    sync::session::on_frame();  // noexcept
    watchdog::beat(sync::avatar::present());
}

void detour_play_motion(void* animation_ps, int type, void* desc, bool flag) {
    if (sync::probe::on_play_motion(animation_ps, type, desc, flag)) {
        g_original_play_motion(animation_ps, type, desc, flag);
    }
}

void detour_set_ragdoll(void* animation_ps, bool on, bool flag, const float* vector) {
    if (sync::probe::on_set_ragdoll(animation_ps, on, flag, vector)) {
        g_original_set_ragdoll(animation_ps, on, flag, vector);
    }
}

bool create(void* target, void* detour, void** original, const char* name);

// Spike J2, Num4 : IA d'un PNJ figée. Le détour ne fait que comparer un pointeur (AD-2).
void detour_routine_on_process(void* routine_ps) {
    if (sync::probe_j2::on_routine_process(routine_ps)) g_original_routine_on_process(routine_ps);
}

// Session (story 3.3) : IA figée des répliques de PNJ. Le détour ne fait que lire le registre
// (comparaison sous le verrou de l'AnimFilter, AD-2) ; sync/npc seul y inscrit les répliques.
void detour_routine_on_process_session(void* routine_ps) {
    if (!sync::anim_filter().blocks_routine(routine_ps)) g_original_routine_on_process(routine_ps);
}

// Story 3.5 (Q8) : point d'accroche des dégâts, gCDamage_PS::DoDamage(eCEntity*). sync/hits décide :
// laisser (original), bloquer en rendant true (HitTargets joue l'effet d'impact, sans dégâts) ou
// false (pas d'effet), ou mesurer un coup sur l'avatar (original appelé, PV rendus). En cas de
// doute, sync/hits laisse passer.
bool detour_damage(void* damage_ps, void* target) {
    return sync::hits::on_damage(damage_ps, target, g_original_damage);
}

// Spike J2, Num6 : un Kill du héros devient Defeat (même thread, mêmes arguments). Seule
// exception documentée à AD-2 : appel de Defeat depuis le détour.
// Garde de réentrance : si Defeat repasse par Kill, l'appel imbriqué va à l'original.
thread_local bool t_in_kill_detour = false;

long long detour_kill(void* spu, void* self, void* other, long long arg) {
    if (t_in_kill_detour) {
        // Kill lancé pendant Defeat (3ᵉ test : il tuait le héros juste après l'interception).
        if (sync::probe_j2::swallow_nested_kill(spu, other)) return 0;
        return g_original_kill(spu, self, other, arg);
    }
    if (sync::probe_j2::intercept_kill(spu, other)) {
        t_in_kill_detour = true;
        long long result = 0;
        const bool ok = game::defeat(spu, self, other, arg, result);
        t_in_kill_detour = false;
        sync::probe_j2::note_defeat(ok, result);
        if (ok) return result;
    }
    return g_original_kill(spu, self, other, arg);
}

// Spike J2, Num6 (5ᵉ test) : coup mortel sur le héros annulé avant toute décision de mort.
template <int Which>
long long detour_down_or_dead(void* spu, void* self, void* other, long long arg) {
    if (sync::probe_j2::intercept_down_or_dead(Which, spu, other)) return 0;
    return g_original_down_or_dead[Which](spu, self, other, arg);
}

// Story 3.7 (AD-11, AD-12) : filet de la mise à terre de l'invité. Un coup natif mortel resté sur
// son héros (point d'accroche des dégâts absent, damage_hook = 0) est annulé par sync/guest_health
// (héros à 1 PV, mise à terre), qui rend true : le détour rend alors 0 sans appeler l'original.
// Même thread qu'OnProcess (exception AD-2 documentée).
template <int Which>
long long detour_down_or_dead_session(void* spu, void* self, void* other, long long arg) {
    if (sync::guest_health::on_down_or_dead(Which, spu, other)) return 0;
    return g_original_down_or_dead[Which](spu, self, other, arg);
}

// Spike J2 : hooks facultatifs. Un échec rend l'expérience inactive, la sonde J1 continue.
void install_probe_j2() {
    const char* missing = nullptr;
    if (!game::resolve_combat(missing)) {
        sync::probe_j2::set_status(false, false, false);
        log::warn(std::string("[hooks] J2 : expériences Num1 à Num6 inactives : symbole introuvable : ") + missing);
        return;
    }
    const bool routine = create(game::routine_on_process_address(), reinterpret_cast<void*>(&detour_routine_on_process),
                                reinterpret_cast<void**>(&g_original_routine_on_process), "gCScriptRoutine_PS::OnProcess");
    bool kill = false;
    std::string kill_reason;
    const char* kill_missing = nullptr;
    if (game::resolve_script_game(kill_missing)) {
        kill = create(game::kill_address(), reinterpret_cast<void*>(&detour_kill),
                      reinterpret_cast<void**>(&g_original_kill), "Kill") &&
               create(game::down_or_dead_address(0), reinterpret_cast<void*>(&detour_down_or_dead<0>),
                      reinterpret_cast<void**>(&g_original_down_or_dead[0]), "DownOrDeadByMeleeWeapon") &&
               create(game::down_or_dead_address(1), reinterpret_cast<void*>(&detour_down_or_dead<1>),
                      reinterpret_cast<void**>(&g_original_down_or_dead[1]), "DownOrDeadByProjectile") &&
               create(game::down_or_dead_address(2), reinterpret_cast<void*>(&detour_down_or_dead<2>),
                      reinterpret_cast<void**>(&g_original_down_or_dead[2]), "DownOrDeadByInferno");
        if (!kill) kill_reason = "MH_CreateHook(Kill ou DownOrDeadBy*) a échoué";
    } else {
        kill_reason = std::string(kill_missing != nullptr ? kill_missing : "?") + " introuvable";
    }
    sync::probe_j2::set_status(true, routine, kill);
    if (routine && kill) {
        log::info("[hooks] J2 : hooks Kill/Routine posés");
        return;
    }
    if (routine) {
        log::info("[hooks] J2 : hook Routine posé (Num4)");
    } else {
        log::warn("[hooks] J2 : expérience Q3 Num4 (IA figée) inactive : hook gCScriptRoutine_PS::OnProcess impossible");
    }
    if (kill) {
        log::info("[hooks] J2 : hook Kill posé (Num6)");
    } else {
        log::warn("[hooks] J2 : expérience Q7 (Num6, mort interceptée) inactive : " + kill_reason +
                  " ; Num6 tenu 1 s relève encore le héros");
    }
}

// Spike J3 : chaque réplique passe par gCScriptProcessingUnit::StartOutput. Le détour appelle
// l'original, puis copie locuteur, échantillon, type et retour pour la frame (accesseurs seuls, AD-2).
bool detour_start_output(void* spu, const void* sample, int type) {
    const bool result = g_original_start_output(spu, sample, type);
    sync::probe_j3::on_start_output(spu, sample, type, result);  // noexcept
    return result;
}

// Story 4.1 (AD-13) : même détour en session. L'original est toujours appelé ; sync/dialogue ne fait
// que copier locuteur, échantillon et type dans sa file (accesseurs seuls, AD-2), la frame décide.
bool detour_start_output_session(void* spu, const void* sample, int type) {
    const bool result = g_original_start_output(spu, sample, type);
    sync::dialogue::on_start_output(spu, sample, type);  // noexcept
    return result;
}

// Spike J3 : bloc facultatif. Un échec rend le journal des répliques et Num7 à Num9 inactifs ;
// les sondes J1 et J2 continuent.
void install_probe_j3() {
    const char* missing = nullptr;
    if (!game::resolve_dialogue(missing)) {
        sync::probe_j3::set_status(false);
        log::warn(std::string("[hooks] J3 : journal des répliques et expériences Num7 à Num9 inactifs : symbole "
                              "introuvable : ") +
                  missing);
        return;
    }
    const bool hook = create(game::start_output_address(), reinterpret_cast<void*>(&detour_start_output),
                             reinterpret_cast<void**>(&g_original_start_output), "gCScriptProcessingUnit::StartOutput");
    sync::probe_j3::set_status(hook);
    if (hook) {
        log::info("[hooks] J3 : hook StartOutput posé (journal des répliques, Num7 à Num9)");
    } else {
        log::warn("[hooks] J3 : journal des répliques et expériences Num7 à Num9 inactifs : hook "
                  "gCScriptProcessingUnit::StartOutput impossible");
    }
}

// Session : capture du héros local (story 2.4), blocage du moteur sur les avatars (2.2).
void detour_play_motion_session(void* animation_ps, int type, void* desc, bool flag) {
    if (sync::avatar::on_play_motion(animation_ps, type, desc, flag)) {
        g_original_play_motion(animation_ps, type, desc, flag);
    }
}

void detour_set_ragdoll_session(void* animation_ps, bool on, bool flag, const float* vector) {
    if (sync::avatar::on_set_ragdoll(animation_ps, on, flag, vector)) g_original_set_ragdoll(animation_ps, on, flag, vector);
}

// Story 2.4b : réglages de locomotion de l'enveloppe EMotionFX (bloqués sur l'avatar).
void detour_set_play_speed(void* wrapper, int type, float speed) {
    if (sync::avatar::on_wrapper_call(wrapper, sync::avatar::WrapperCall::PlaySpeed, type, speed, 0.0f)) {
        g_original_set_play_speed(wrapper, type, speed);
    }
}

void detour_set_target_weight(void* wrapper, int type, float weight, float time) {
    if (sync::avatar::on_wrapper_call(wrapper, sync::avatar::WrapperCall::TargetWeight, type, weight, time)) {
        g_original_set_target_weight(wrapper, type, weight, time);
    }
}

void detour_switch_fade_out(void* wrapper, int type, bool on, float time) {
    if (sync::avatar::on_wrapper_call(wrapper, sync::avatar::WrapperCall::FadeOut, type, on ? 1.0f : 0.0f, time)) {
        g_original_switch_fade_out(wrapper, type, on, time);
    }
}

void detour_wrapper_stop_motion(void* wrapper, int type, float time, bool flag) {
    if (sync::avatar::on_wrapper_call(wrapper, sync::avatar::WrapperCall::StopMotion, type, flag ? 1.0f : 0.0f, time)) {
        g_original_wrapper_stop_motion(wrapper, type, time, flag);
    }
}

bool create(void* target, void* detour, void** original, const char* name) {
    const MH_STATUS s = MH_CreateHook(target, detour, original);
    if (s != MH_OK) {
        log::error(std::string("[hooks] MH_CreateHook(") + name + ") : " + MH_StatusToString(s));
        return false;
    }
    return true;
}

}  // namespace

bool install_probe() {
    const char* missing = nullptr;
    if (!game::resolve_entities(missing)) {
        log::error(std::string("[hooks] sonde inactive : symbole introuvable : ") + missing);
        return false;
    }
    MH_STATUS s = MH_Initialize();
    if (s != MH_OK) {
        log::error(std::string("[hooks] sonde inactive : MH_Initialize : ") + MH_StatusToString(s));
        return false;
    }
    g_initialized = true;

    if (!create(game::on_process_address(), reinterpret_cast<void*>(&detour_on_process),
                reinterpret_cast<void**>(&g_original_on_process), "OnProcess") ||
        !create(game::play_motion_address(), reinterpret_cast<void*>(&detour_play_motion),
                reinterpret_cast<void**>(&g_original_play_motion), "PlayMotion") ||
        !create(game::set_ragdoll_address(), reinterpret_cast<void*>(&detour_set_ragdoll),
                reinterpret_cast<void**>(&g_original_set_ragdoll), "SetRagDollEnabled")) {
        remove();
        log::error("[hooks] sonde inactive");
        return false;
    }
    install_probe_j2();  // facultatif : créés ici, activés avec les autres
    install_probe_j3();  // idem (spike J3)
    // Activation groupée : MinHook suspend les autres threads pendant l'écriture.
    s = MH_EnableHook(MH_ALL_HOOKS);
    if (s != MH_OK) {
        log::error(std::string("[hooks] sonde inactive : MH_EnableHook : ") + MH_StatusToString(s));
        remove();
        return false;
    }
    log::info("[hooks] hooks installés (OnProcess, PlayMotion, SetRagDollEnabled)");
    return true;
}

bool install_session(bool damage_hook, bool native_attacks, bool dialogues) {
    const char* missing = nullptr;
    if (!game::resolve_entities(missing)) {
        log::error(std::string("[hooks] session inactive : symbole introuvable : ") + missing);
        return false;
    }
    // Story 3.2 (AD-3) : arme en main du héros et de l'avatar, en tout ou rien.
    if (!game::resolve_inventory(missing)) {
        log::error(std::string("[hooks] session inactive : symbole d'inventaire introuvable : ") + missing);
        return false;
    }
    log::info("[hooks] inventaire de session résolu (arme de l'avatar)");
    // Story 3.3 (AD-3) : répliques des PNJ, en tout ou rien.
    if (!game::resolve_npc(missing)) {
        log::error(std::string("[hooks] session inactive : symbole des PNJ introuvable : ") + missing);
        return false;
    }
    log::info("[hooks] PNJ de session résolus (répliques)");
    MH_STATUS s = MH_Initialize();
    if (s != MH_OK) {
        log::error(std::string("[hooks] session inactive : MH_Initialize : ") + MH_StatusToString(s));
        return false;
    }
    g_initialized = true;
    if (!create(game::on_process_address(), reinterpret_cast<void*>(&detour_on_process_session),
                reinterpret_cast<void**>(&g_original_on_process), "OnProcess") ||
        !create(game::play_motion_address(), reinterpret_cast<void*>(&detour_play_motion_session),
                reinterpret_cast<void**>(&g_original_play_motion), "PlayMotion") ||
        !create(game::set_ragdoll_address(), reinterpret_cast<void*>(&detour_set_ragdoll_session),
                reinterpret_cast<void**>(&g_original_set_ragdoll), "SetRagDollEnabled") ||
        !create(game::set_play_speed_address(), reinterpret_cast<void*>(&detour_set_play_speed),
                reinterpret_cast<void**>(&g_original_set_play_speed), "SetPlaySpeed") ||
        !create(game::set_target_weight_address(), reinterpret_cast<void*>(&detour_set_target_weight),
                reinterpret_cast<void**>(&g_original_set_target_weight), "SetTargetWeight") ||
        !create(game::switch_fade_out_address(), reinterpret_cast<void*>(&detour_switch_fade_out),
                reinterpret_cast<void**>(&g_original_switch_fade_out), "SwitchFadeOut") ||
        !create(game::wrapper_stop_motion_address(), reinterpret_cast<void*>(&detour_wrapper_stop_motion),
                reinterpret_cast<void**>(&g_original_wrapper_stop_motion), "StopMotion") ||
        !create(game::routine_on_process_address(), reinterpret_cast<void*>(&detour_routine_on_process_session),
                reinterpret_cast<void**>(&g_original_routine_on_process), "gCScriptRoutine_PS::OnProcess")) {
        remove();
        log::error("[hooks] session inactive");
        return false;
    }
    // Story 3.5 (Q8) : point d'accroche des dégâts, facultatif (D3) : un échec laisse la session
    // sans dégâts synchronisés. Résolu même avec damage_hook = 0 : l'hôte applique les HitRequest.
    bool damage = false;
    if (!game::resolve_damage(missing)) {
        log::warn(std::string("[hooks] dégâts inactifs : symbole introuvable : ") + missing +
                  " (pas de point d'accroche, HitRequest ignorés)");
    } else if (!damage_hook) {
        log::info("[hooks] point d'accroche des dégâts désactivé (damage_hook = 0)");
    } else if (create(game::damage_hook_address(), reinterpret_cast<void*>(&detour_damage),
                      reinterpret_cast<void**>(&g_original_damage), "gCDamage_PS::DoDamage")) {
        damage = true;
    } else {
        log::warn("[hooks] dégâts inactifs : point d'accroche impossible (gCDamage_PS::DoDamage)");
    }
    // Story 3.7 : détours DownOrDeadBy* de session, facultatifs (Script_Game.dll déjà chargée par le
    // jeu) : sans eux, la session continue, sans filet si le point d'accroche des dégâts manque.
    bool down_or_dead = false;
    std::string down_or_dead_reason;
    const char* script_game_missing = nullptr;
    if (game::resolve_script_game(script_game_missing)) {
        down_or_dead =
            create(game::down_or_dead_address(0), reinterpret_cast<void*>(&detour_down_or_dead_session<0>),
                   reinterpret_cast<void**>(&g_original_down_or_dead[0]), "DownOrDeadByMeleeWeapon") &&
            create(game::down_or_dead_address(1), reinterpret_cast<void*>(&detour_down_or_dead_session<1>),
                   reinterpret_cast<void**>(&g_original_down_or_dead[1]), "DownOrDeadByProjectile") &&
            create(game::down_or_dead_address(2), reinterpret_cast<void*>(&detour_down_or_dead_session<2>),
                   reinterpret_cast<void**>(&g_original_down_or_dead[2]), "DownOrDeadByInferno");
        if (!down_or_dead) {
            down_or_dead_reason = "MH_CreateHook a échoué";
            // Un détour créé seul resterait désactivé avec les autres : on retire ceux qui ont réussi.
            for (int i = 0; i < 3; ++i) MH_RemoveHook(game::down_or_dead_address(i));
        }
    } else {
        down_or_dead_reason = std::string(script_game_missing != nullptr ? script_game_missing : "?") + " introuvable";
    }
    if (!down_or_dead) {
        log::warn("[hooks] DownOrDeadBy* inactifs : " + down_or_dead_reason +
                  " (coup natif mortel non intercepté si le point d'accroche des dégâts manque)");
    }
    // Story 4.1 (AD-13) : dialogues, bloc facultatif en tout ou rien : un échec laisse la session sans
    // dialogues (Warn). Statut publié à sync/dialogue avant le premier OnProcess.
    bool dialogue = false;
    if (!dialogues) {
        log::info("[hooks] dialogues désactivés (dialogues = 0)");
    } else if (!game::resolve_dialogue(missing)) {
        log::warn(std::string("[hooks] dialogues inactifs : symbole introuvable : ") + missing +
                  " (session sans dialogues)");
    } else if (create(game::start_output_address(), reinterpret_cast<void*>(&detour_start_output_session),
                      reinterpret_cast<void**>(&g_original_start_output), "gCScriptProcessingUnit::StartOutput")) {
        dialogue = true;
    } else {
        log::warn("[hooks] dialogues inactifs : hook gCScriptProcessingUnit::StartOutput impossible (session sans "
                  "dialogues)");
    }
    sync::dialogue::set_active(dialogue);
    s = MH_EnableHook(MH_ALL_HOOKS);
    if (s != MH_OK) {
        sync::dialogue::set_active(false);
        log::error(std::string("[hooks] session inactive : MH_EnableHook : ") + MH_StatusToString(s));
        remove();
        return false;
    }
    log::info("[hooks] hooks de session installés (OnProcess, PlayMotion, SetRagDollEnabled, locomotion EMotionFX, "
              "gCScriptRoutine_PS::OnProcess)");
    if (damage) log::info("[hooks] point d'accroche des dégâts posé (gCDamage_PS::DoDamage)");
    // Story 3.8 : attaques natives des répliques, facultatives. Sans le détour des dégâts, le coup
    // natif d'une réplique blesserait vraiment le héros invité, et sans les détours DownOrDeadBy* un
    // coup cosmétique mortel le mettrait vraiment à terre : elles restent alors inactives.
    if (!native_attacks) {
        log::info("[hooks] attaques natives des répliques désactivées (native_attacks = 0)");
    } else if (!damage) {
        log::warn("[hooks] attaques natives des répliques inactives : pas de point d'accroche des dégâts "
                  "(recul générique seul)");
    } else if (!down_or_dead) {
        log::warn("[hooks] attaques natives des répliques inactives : détours DownOrDeadBy* absents "
                  "(recul générique seul)");
    } else if (!game::resolve_native_attacks(missing)) {
        log::warn(std::string("[hooks] attaques natives des répliques inactives : symbole introuvable : ") + missing +
                  " (recul générique seul)");
    } else {
        log::info("[hooks] attaques natives des répliques résolues (PSCombatSystem::StartMove)");
    }
    if (down_or_dead) log::info("[hooks] détours DownOrDeadBy* de session posés (mise à terre de l'invité)");
    if (dialogue) log::info("[hooks] dialogues : hook StartOutput posé (répliques de l'hôte vues par l'invité)");
    watchdog::start();
    return true;
}

void remove() {
    if (!g_initialized) return;
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();  // supprime aussi les hooks créés
    g_initialized = false;
    sync::dialogue::set_active(false);  // story 4.1 : plus de détour de StartOutput
    // Les pointeurs vers les trampolines sont conservés : si le retrait échouait en
    // partie, un détour encore actif n'appellerait jamais un original nul.
}

}  // namespace mp::win::hooks
