// Règles du détour des dégâts (story 3.5, AD-9, AD-12), sans Windows ni appel au jeu : testées sous
// Linux (client/tests/test_hit_rules.cpp). sync/hits lit l'état (garde, session, rôle, héros,
// agresseur, registre) et applique la décision rendue ici, dans l'ordre :
//   1. garde « application d'un HitRequest » active → laissé ;
//   2. hors session, ou rôle inconnu → laissé ;
//   3. invité, cible = réplique de PNJ : bloqué ; NetId mis en file seulement si l'agresseur est son
//      héros et la réplique vivante ;
//   4. hôte, agresseur = avatar de l'invité : bloqué, rien en file ;
//   5. sinon → laissé.
#pragma once

#include <cstdint>

#include "sync/anim_filter.h"

namespace mp::sync::hit_rules {

enum class HitRole : int { Unknown = 0, Host = 1, Guest = 2 };

enum class Reason : std::uint8_t {
    Pass,            // laissé
    HitRequest,      // 3. bloqué, NetId en file
    DeadReplica,     // 3. bloqué, réplique morte (agresseur = héros)
    OtherAggressor,  // 3. bloqué, autre agresseur
    GuestAvatar,     // 4. bloqué, agresseur = avatar invité chez l'hôte
};

struct Inputs {
    bool guard = false;       // garde de réentrance active sur ce thread
    bool in_session = false;
    HitRole role = HitRole::Unknown;
    const void* hero = nullptr;
    const void* aggressor = nullptr;
    const void* target = nullptr;
    bool target_found = false;  // find_by_entity(target)
    TrackedEntity target_entry;
    bool aggressor_found = false;  // find_by_entity(aggressor)
    TrackedEntity aggressor_entry;
    bool replica_dead = false;  // la réplique cible est morte (sync/npc::die)
};

struct Decision {
    bool block = false;
    std::uint32_t queue_net_id = 0;  // ≠ 0 : NetId à mettre en file (HitRequest)
    Reason reason = Reason::Pass;
};

// Règles 1 et 2 seules : le détour doit-il aller plus loin (lire l'agresseur) ?
bool considered(bool guard, bool in_session, HitRole role);
// Règles 1 à 5.
Decision decide(const Inputs& in);

}  // namespace mp::sync::hit_rules
