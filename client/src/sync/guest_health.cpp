#include "sync/guest_health.h"

#include "common/log.h"
#include "game/combat.h"
#include "game/entity.h"
#include "sync/hit_rules.h"
#include "sync/hits.h"

#include <string>

namespace mp::sync::guest_health {

namespace {

std::string source_text(std::uint32_t source) {
    return source == 0 ? std::string("inconnue") : "PNJ " + std::to_string(source);
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
    if (r.held) {
        log::warn("[sync] HitOnGuest mortel " + src + " : héros maintenu à 1 PV (à terre : story 3.7)");
    }
}

}  // namespace mp::sync::guest_health
