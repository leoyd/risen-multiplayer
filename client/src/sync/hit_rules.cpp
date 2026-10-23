#include "sync/hit_rules.h"

namespace mp::sync::hit_rules {

bool considered(bool guard, bool in_session, HitRole role) {
    return !guard && in_session && role != HitRole::Unknown;
}

Decision decide(const Inputs& in) {
    Decision d;
    if (!considered(in.guard, in.in_session, in.role)) return d;  // 1, 2
    if (in.role == HitRole::Guest && in.target_found && in.target_entry.role == EntityRole::NpcReplica) {
        d.block = true;  // 3. la cible n'est pas sous autorité locale (AD-9)
        d.impact = true;
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
        d.impact = true;
        d.reason = in.aggressor != nullptr && in.aggressor_found && in.aggressor_entry.role == EntityRole::NpcReplica
                       ? Reason::GuestHeroByReplica
                       : Reason::GuestHeroOther;
        return d;
    }
    if (in.role == HitRole::Host && in.aggressor_found && in.aggressor_entry.role == EntityRole::Avatar) {
        d.block = true;  // 4. seuls les HitRequest blessent
        d.impact = in.target != in.hero;  // pas d'effet d'impact sur le héros de l'hôte (tir ami)
        d.reason = Reason::GuestAvatar;
        return d;
    }
    if (in.role == HitRole::Host && in.target_found && in.target_entry.role == EntityRole::Avatar) {
        if (in.aggressor == nullptr) {
            d.block = true;
            d.reason = Reason::AvatarNoAggressor;
        } else if (in.aggressor == in.hero ||
                   (in.aggressor_found && in.aggressor_entry.role == EntityRole::LocalHero)) {
            d.block = true;
            d.reason = Reason::AvatarFriendly;  // pas de tir ami
        } else {
            d.reason = Reason::AvatarHit;  // 4b. mesuré : les PV de l'avatar sont rendus après le coup
            d.measure = true;
            d.hit_on_guest = true;
            d.source_net_id = in.aggressor_found && in.aggressor_entry.role == EntityRole::NpcCapture
                                  ? in.aggressor_entry.net_id
                                  : 0;
        }
        return d;
    }
    return d;  // 5.
}

std::uint32_t measured_damage(int hp_before, int hp_after, bool player_party) {
    long long v = static_cast<long long>(hp_before) - static_cast<long long>(hp_after);
    if (player_party) v *= 2;
    if (v <= 0) return 0;
    if (v >= static_cast<long long>(kMaxRawDamage)) return kMaxRawDamage;
    return static_cast<std::uint32_t>(v);
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
