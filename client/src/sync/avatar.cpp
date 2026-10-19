#include "sync/avatar.h"

#include "common/log.h"
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
    void* animation_ps = nullptr;  // réplique dans l'AnimFilter (nul : pas de PS trouvé)
    void* wrapper = nullptr;       // enveloppe EMotionFX, réplique aussi (story 2.4b)
    void* player = nullptr;        // héros local à la création : le monde de l'avatar
    bool move_error_logged = false;
    bool dead = false;  // en ragdoll (Death) : plus de pose ni d'animation imposées
    bool param_error_logged = false;
};
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
    } else {
        anim_filter().add_replica(e.animation_ps);
        // Story 2.4b : l'enveloppe aussi, pour bloquer les réglages de locomotion du moteur.
        if (game::get_actor_wrapper(e.animation_ps, e.wrapper) && e.wrapper != nullptr) {
            anim_filter().add_replica(e.wrapper);
        } else {
            e.wrapper = nullptr;
            log::warn("[sync] avatar " + std::to_string(net_id) + " : pas d'enveloppe EMotionFX, locomotion non protégée");
        }
    }
    anim_filter().take_blocked();  // compteur remis à zéro : bilan au retrait
    g_table.push_back(e);
    log::info("[sync] avatar " + std::to_string(net_id) + " créé : entité " + hex_ptr(e.entity.ptr) + ", PS " +
              hex_ptr(e.animation_ps) + ", " + pose_text(pose));
}

void despawn(std::uint32_t net_id) {
    for (auto it = g_table.begin(); it != g_table.end(); ++it) {
        if (it->net_id != net_id) continue;
        const Entry e = *it;
        // AD-11 : d'abord l'AnimFilter et la table, ensuite seulement la destruction.
        if (e.animation_ps != nullptr) anim_filter().remove_replica(e.animation_ps);
        if (e.wrapper != nullptr) anim_filter().remove_replica(e.wrapper);
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

void move(std::uint32_t net_id, const Pose& pose) {
    for (Entry& e : g_table) {
        if (e.net_id != net_id) continue;
        if (e.move_error_logged || e.dead) return;  // après une SEH, ou mort : plus de pose imposée
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

bool present() { return !g_table.empty(); }

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
