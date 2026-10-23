#include "sync/hit_rules.h"

#include <cmath>

namespace mp::sync::hit_rules {

bool considered(bool guard, bool in_session, HitRole role) {
    return !guard && in_session && role != HitRole::Unknown;
}

Decision decide(const Inputs& in) {
    Decision d;
    if (!considered(in.guard, in.in_session, in.role)) return d;  // 1, 2
    if (in.role == HitRole::Guest && in.target_found && in.target_entry.role == EntityRole::NpcReplica) {
        d.block = true;  // 3. la cible n'est pas sous autorité locale (AD-9)
        if (in.aggressor != nullptr && in.aggressor == in.hero) {
            if (in.replica_dead) {
                d.reason = Reason::DeadReplica;
            } else {
                d.reason = Reason::HitRequest;
                d.queue_net_id = in.target_entry.net_id;
            }
        } else {
            d.reason = Reason::OtherAggressor;
        }
        return d;
    }
    if (in.role == HitRole::Guest && in.target != nullptr &&
        (in.target == in.hero || (in.target_found && in.target_entry.role == EntityRole::LocalHero))) {
        d.block = true;  // 3b. seuls les HitOnGuest blessent le héros invité
        d.reason = in.aggressor != nullptr && in.aggressor_found && in.aggressor_entry.role == EntityRole::NpcReplica
                       ? Reason::GuestHeroByReplica
                       : Reason::GuestHeroOther;
        return d;
    }
    if (in.role == HitRole::Host && in.aggressor_found && in.aggressor_entry.role == EntityRole::Avatar) {
        d.block = true;  // 4. seuls les HitRequest blessent
        d.reason = Reason::GuestAvatar;
        return d;
    }
    if (in.role == HitRole::Host && in.target_found && in.target_entry.role == EntityRole::Avatar) {
        d.block = true;  // 4b. avatar invulnérable chez l'hôte
        if (in.aggressor == nullptr) {
            d.reason = Reason::AvatarNoAggressor;
        } else if (in.aggressor == in.hero ||
                   (in.aggressor_found && in.aggressor_entry.role == EntityRole::LocalHero)) {
            d.reason = Reason::AvatarFriendly;  // pas de tir ami
        } else {
            d.reason = Reason::AvatarHit;
            d.hit_on_guest = true;
            d.source_net_id = in.aggressor_found && in.aggressor_entry.role == EntityRole::NpcCapture
                                  ? in.aggressor_entry.net_id
                                  : 0;
        }
        return d;
    }
    return d;  // 5.
}

std::uint32_t raw_damage(long amount, long bonus, float multiplier) {
    const double v = (static_cast<double>(amount) + static_cast<double>(bonus)) * static_cast<double>(multiplier);
    if (!std::isfinite(v) || v <= 0.0) return 0;
    if (v >= static_cast<double>(kMaxRawDamage)) return kMaxRawDamage;
    return static_cast<std::uint32_t>(std::llround(v));
}

GuestHp guest_hp_after(int hp, std::uint32_t damage) {
    GuestHp r;
    r.new_hp = hp;
    if (damage == 0) return r;
    if (hp <= 1) {
        r.held = true;
        return r;
    }
    const long long left = static_cast<long long>(hp) - static_cast<long long>(damage);
    r.write = true;
    if (left <= 0) {
        r.new_hp = 1;
        r.held = true;
    } else {
        r.new_hp = static_cast<int>(left);
    }
    return r;
}

}  // namespace mp::sync::hit_rules
