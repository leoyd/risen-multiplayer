#include "win/hooks.h"

#include "common/log.h"
#include "game/combat.h"
#include "game/entity.h"
#include "sync/avatar.h"
#include "sync/probe.h"
#include "sync/probe_j2.h"
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
game::ScriptFn g_original_down_or_dead[3] = {};                     // spike J2 (Num6, 5ᵉ test)
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

bool install_session() {
    const char* missing = nullptr;
    if (!game::resolve_entities(missing)) {
        log::error(std::string("[hooks] session inactive : symbole introuvable : ") + missing);
        return false;
    }
    // Story 3.1 (AD-3) : enveloppes du combat de l'avatar solide, en tout ou rien.
    if (!game::resolve_combat_session(missing)) {
        log::error(std::string("[hooks] session inactive : symbole du combat introuvable : ") + missing);
        return false;
    }
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
                reinterpret_cast<void**>(&g_original_wrapper_stop_motion), "StopMotion")) {
        remove();
        log::error("[hooks] session inactive");
        return false;
    }
    s = MH_EnableHook(MH_ALL_HOOKS);
    if (s != MH_OK) {
        log::error(std::string("[hooks] session inactive : MH_EnableHook : ") + MH_StatusToString(s));
        remove();
        return false;
    }
    log::info("[hooks] hooks de session installés (OnProcess, PlayMotion, SetRagDollEnabled, locomotion EMotionFX)");
    watchdog::start();
    return true;
}

void remove() {
    if (!g_initialized) return;
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();  // supprime aussi les hooks créés
    g_initialized = false;
    // Les pointeurs vers les trampolines sont conservés : si le retrait échouait en
    // partie, un détour encore actif n'appellerait jamais un original nul.
}

}  // namespace mp::win::hooks
