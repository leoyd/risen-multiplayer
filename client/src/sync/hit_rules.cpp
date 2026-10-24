#include "sync/hit_rules.h"

#include <cmath>

#include "core/protocol.h"

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
        const bool by_replica =
            in.aggressor != nullptr && in.aggressor_found && in.aggressor_entry.role == EntityRole::NpcReplica;
        if (by_replica && in.native_attacks && !in.hero_down) {
            d.cosmetic = true;  // 3b (story 3.8) : rendu natif du coup, PV rendus ensuite
            d.reason = Reason::GuestHeroCosmetic;
            return d;
        }
        d.block = true;  // 3b. seuls les HitOnGuest blessent le héros invité
        d.impact = true;
        d.reason = by_replica ? Reason::GuestHeroByReplica : Reason::GuestHeroOther;
        return d;
    }
    if (in.role == HitRole::Guest && in.native_attacks && in.target_found &&
        in.target_entry.role == EntityRole::Avatar) {
        d.block = true;  // 3c. l'avatar de l'hôte n'est jamais blessé chez l'invité
        const bool friendly = (in.aggressor != nullptr && in.aggressor == in.hero) ||
                              (in.aggressor_found && in.aggressor_entry.role == EntityRole::LocalHero);
        d.impact = !friendly;  // pas d'effet sur un tir ami du héros invité
        d.reason = Reason::HostAvatarOnGuest;
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

bool attack_started(AttackState& state, int current, int last_attack) {
    bool started = false;
    if (state.known && last_attack != 0) {
        if (last_attack != state.last_attack) {
            started = true;
            state.announced = current != last_attack;  // entrée de CurrentMove à venir : même attaque
        } else if (current == last_attack && state.current != current) {
            started = !state.announced;
            state.announced = false;  // entrée consommée
        }
    } else if (last_attack == 0) {
        state.announced = false;
    }
    state.known = true;
    state.current = current;
    state.last_attack = last_attack;
    return started;
}

std::uint32_t attack_target_net_id(const void* target, const void* hero, const void* avatar, bool found,
                                   const TrackedEntity& entry) {
    if (target == nullptr) return 0;
    if (target == hero) return 1;    // protocol::kHostNetId
    if (target == avatar) return 2;  // protocol::kGuestNetId
    if (found && entry.role == EntityRole::NpcCapture) return entry.net_id;
    return 0;
}

std::uint32_t guest_attack_target_net_id(const void* target, const void* avatar, bool found,
                                         const TrackedEntity& entry) {
    if (target == nullptr) return 0;
    if (target == avatar || (found && entry.role == EntityRole::Avatar)) return protocol::kHostNetId;
    if (found && entry.role == EntityRole::NpcReplica && entry.net_id >= protocol::kFirstNpcNetId) return entry.net_id;
    return 0;
}

StumbleStep stumble_step(const PendingStumble& p, std::uint64_t now_ms, bool any_cosmetic,
                         std::uint64_t last_cosmetic_ms) {
    if (!p.active) return StumbleStep::Idle;
    if (any_cosmetic) {
        const std::uint64_t gap =
            last_cosmetic_ms >= p.hit_ms ? last_cosmetic_ms - p.hit_ms : p.hit_ms - last_cosmetic_ms;
        if (gap < kCosmeticWindowMs) return StumbleStep::Cancel;  // recul adapté déjà joué (ou à venir)
    }
    if (now_ms >= p.hit_ms && now_ms - p.hit_ms >= kCosmeticWindowMs) return StumbleStep::Play;
    return StumbleStep::Wait;
}

bool native_move_over(std::uint64_t started_ms, std::uint64_t now_ms, bool hit_landed) {
    if (started_ms == 0) return false;
    return hit_landed || (now_ms >= started_ms && now_ms - started_ms >= kNativeMoveMaxMs);
}

float yaw_towards(float x, float z, float tx, float tz) {
    const float dx = tx - x, dz = tz - z;
    if (dx == 0.0f && dz == 0.0f) return 0.0f;
    return std::atan2(dx, dz);
}

}  // namespace mp::sync::hit_rules
