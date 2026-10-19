#include "sync/avatar.h"

#include "common/log.h"
#include "game/entity.h"
#include "sync/anim_filter.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace mp::sync::avatar {

namespace {

using core::session::Pose;

// Table NetId ↔ entité (AD-6) : au plus un avatar en v3 (celui du pair).
struct Entry {
    std::uint32_t net_id = 0;
    game::Entity entity;
    void* animation_ps = nullptr;  // réplique dans l'AnimFilter (nul : pas de PS trouvé)
    void* player = nullptr;        // héros local à la création : le monde de l'avatar
};
std::vector<Entry> g_table;

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

// bCMatrix en lignes : 0 = droite, 1 = haut (Y), 2 = avant, translation m[12..14] (AD-7).
game::Matrix matrix_of(const Pose& p) {
    const float c = std::cos(p.yaw), s = std::sin(p.yaw);
    game::Matrix m{};
    m.m[0] = c;
    m.m[2] = -s;
    m.m[5] = 1.0f;
    m.m[8] = s;
    m.m[10] = c;
    m.m[12] = p.x;
    m.m[13] = p.y;
    m.m[14] = p.z;
    m.m[15] = 1.0f;
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
        g_table.erase(it);
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

bool on_play_motion(void* animation_ps) noexcept {
    return anim_filter().classify(animation_ps, GetCurrentThreadId()) != AnimFilter::Verdict::Block;
}

bool on_set_ragdoll(void* animation_ps, bool on) noexcept {
    // Une désactivation (nettoyage par le moteur) passe toujours (décision du 2026-10-17),
    // sans être comptée parmi les appels bloqués.
    if (!on) return true;
    return anim_filter().classify(animation_ps, GetCurrentThreadId()) != AnimFilter::Verdict::Block;
}

}  // namespace mp::sync::avatar
