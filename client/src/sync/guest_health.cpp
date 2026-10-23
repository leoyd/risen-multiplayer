#include "sync/guest_health.h"

#include "common/log.h"
#include "game/combat.h"
#include "game/entity.h"
#include "sync/avatar.h"
#include "sync/hit_rules.h"
#include "sync/hits.h"

#include <atomic>
#include <cstdio>
#include <string>

namespace mp::sync::guest_health {

namespace {

namespace cs = mp::core::session;

// Mise à terre notée (cause LocalDown), −1 = aucune. La première de la frame est gardée : la session
// ignore de toute façon les suivantes (verrou).
std::atomic<int> g_down{-1};

const char* const kDownOrDeadName[3] = {"DownOrDeadByMeleeWeapon", "DownOrDeadByProjectile", "DownOrDeadByInferno"};

std::string source_text(std::uint32_t source) {
    return source == 0 ? std::string("inconnue") : "PNJ " + std::to_string(source);
}

void note_down(std::uint8_t cause) {
    int none = -1;
    g_down.compare_exchange_strong(none, cause);
}

std::string pose_text(const core::Pose& p) {
    char buf[128];
    std::snprintf(buf, sizeof buf, "(%.1f, %.1f, %.1f) lacet=%.3f", static_cast<double>(p.x),
                  static_cast<double>(p.y), static_cast<double>(p.z), static_cast<double>(p.yaw));
    return buf;
}

}  // namespace

void apply_hit(std::uint32_t source, std::uint32_t damage) {
    const std::string src = "(source " + source_text(source) + ")";
    game::Entity hero;
    hero.ptr = hits::hero();
    if (hero.ptr == nullptr) {
        log::warn("[sync] HitOnGuest " + src + " ignoré : pas de héros");
        return;
    }
    int hp = 0;
    if (!game::get_hit_points(hero, hp)) {
        log::warn("[sync] HitOnGuest " + src + " ignoré : PV du héros illisibles");
        return;
    }
    const hit_rules::GuestHp r = hit_rules::guest_hp_after(hp, damage);
    if (r.write && !game::set_hit_points(hero, r.new_hp, game::kSkillModifierSet)) {
        log::error("[sync] HitOnGuest " + src + " : SetHitPoints a levé une exception (SEH) : PV inchangés");
        return;
    }
    log::info("[sync] HitOnGuest " + src + " appliqué : dégâts bruts " + std::to_string(damage) + ", PV " +
              std::to_string(hp) + " -> " + std::to_string(r.new_hp));
    if (r.held) note_down(cs::kDownCauseHitOnGuest);  // story 3.7 : à terre, Death{2} par la session
}

bool on_down_or_dead(int which, void* spu, void* other) noexcept {
    if (which < 0 || which > 2) return false;
    // Hôte : coup mesuré sur l'avatar invité (sync/hits, règle 4b) : sa mise à terre ou sa mort est
    // annulée, ses PV sont rendus après la mesure.
    if (void* measured = hits::measured_target(); measured != nullptr) {
        void* victim = nullptr;
        const bool read = other != nullptr ? game::read_entity_ptr(other, victim) : game::spu_other_entity(spu, victim);
        return read && victim == measured;
    }
    if (!hits::guest_in_session()) return false;
    void* hero = hits::hero();
    if (hero == nullptr) return false;
    void* victim = nullptr;
    const bool read = other != nullptr ? game::read_entity_ptr(other, victim) : game::spu_other_entity(spu, victim);
    if (!read || victim != hero) return false;
    // Appelées après chaque coup (elles vérifient elles-mêmes les PV) : seul un coup mortel compte.
    int hp = 1;
    if (!game::get_hit_points(game::Entity{victim}, hp) || hp > 0) return false;
    if (!game::set_hit_points(game::Entity{victim}, 1, game::kSkillModifierSet)) return false;
    note_down(static_cast<std::uint8_t>(cs::kDownCauseMelee + which));
    try {
        log::warn(std::string("[sync] coup natif mortel intercepté (") + kDownOrDeadName[which] +
                  ") : héros à 1 PV, à terre");
    } catch (...) {
        // Journal impossible (allocation) : l'interception reste valable.
    }
    return true;
}

void drain(std::vector<cs::Input>& out) {
    const int cause = g_down.exchange(-1);
    if (cause >= 0) out.push_back(cs::LocalDown{static_cast<std::uint8_t>(cause)});
}

void respawn(const core::Pose& pose, bool teleport) {
    game::Entity hero;
    hero.ptr = hits::hero();
    if (hero.ptr == nullptr) {
        log::warn("[sync] réapparition impossible : pas de héros");
        return;
    }
    // Téléportation en échec (Error au log) : les PV sont rendus quand même, la session tient la
    // réapparition pour faite.
    const bool moved = teleport && avatar::teleport_player(pose, "replacé (réapparition)");
    int max = 0;
    if (!game::get_hit_points_max(hero, max) || max <= 0) {
        log::warn("[sync] réapparition : PV max du héros illisibles : PV inchangés");
        return;
    }
    if (!game::set_hit_points(hero, max, game::kSkillModifierSet)) {
        log::error("[sync] réapparition : SetHitPoints a levé une exception (SEH) : PV inchangés");
        return;
    }
    log::info("[sync] réapparition : PV du héros remis à " + std::to_string(max) +
              (moved ? " ; placé en " + pose_text(pose) : std::string(" (sur place)")));
}

}  // namespace mp::sync::guest_health
