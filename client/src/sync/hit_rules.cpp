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
    if (in.role == HitRole::Host && in.aggressor_found && in.aggressor_entry.role == EntityRole::Avatar) {
        d.block = true;  // 4. seuls les HitRequest blessent
        d.reason = Reason::GuestAvatar;
    }
    return d;  // 5.
}

}  // namespace mp::sync::hit_rules
