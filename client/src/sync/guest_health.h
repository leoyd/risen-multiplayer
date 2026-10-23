// PV du héros de l'invité (story 3.6, AD-11) : seul écrivain des PV du héros invité en session.
// Un HitOnGuest de l'hôte (commande ApplyHitOnGuest) retire ses dégâts bruts par SetHitPoints
// fixé ; tous les coups natifs sur le héros sont bloqués par le détour des dégâts (sync/hits,
// règle 3b). Jusqu'à la story 3.7 (mise à terre), le héros ne descend jamais sous 1 PV : un coup
// mortel le maintient à 1 PV (Warn), sans Kill, sans DoDamage, sans Death.
//
// Frame seulement (thread principal, AD-2).
#pragma once

#include <cstdint>

namespace mp::sync::guest_health {

// Invité : applique un HitOnGuest (`source` = NetId du PNJ agresseur, 0 = inconnu).
void apply_hit(std::uint32_t source, std::uint32_t damage);

}  // namespace mp::sync::guest_health
