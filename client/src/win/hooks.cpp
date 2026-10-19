#include "win/hooks.h"

#include "common/log.h"
#include "game/entity.h"
#include "sync/avatar.h"
#include "sync/probe.h"
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
