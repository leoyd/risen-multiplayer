// PV du héros de l'invité (stories 3.6 et 3.7, AD-11) : seul écrivain des PV du héros invité en
// session. Un HitOnGuest de l'hôte (commande ApplyHitOnGuest) retire ses dégâts bruts par
// SetHitPoints fixé ; tous les coups natifs sur le héros sont bloqués par le détour des dégâts
// (sync/hits, règle 3b). Le héros ne descend jamais sous 1 PV : un coup mortel le maintient à 1 PV
// et le met à terre (story 3.7, LocalDown vers la session, qui envoie Death{2} une seule fois).
// Filet (D7) : les détours DownOrDeadBy* de session (win/hooks) appellent on_down_or_dead, qui
// annule un coup natif mortel resté sur le héros (point d'accroche des dégâts absent ou
// damage_hook = 0). La réapparition (RespawnHero) replace le héros et remet ses PV au maximum.
// Jamais 0 PV, jamais Kill, DoDamage ni ragdoll sur le héros.
//
// Thread principal (AD-2) : la frame, et les détours DownOrDeadBy* (même thread qu'OnProcess).
#pragma once

#include <cstdint>
#include <vector>

#include "core/pose.h"
#include "core/session.h"

namespace mp::sync::guest_health {

// Invité : applique un HitOnGuest (`source` = NetId du PNJ agresseur, 0 = inconnu). Un coup mortel
// laisse le héros à 1 PV et note une mise à terre (cause HitOnGuest).
void apply_hit(std::uint32_t source, std::uint32_t damage);

// Détour DownOrDeadBy{MeleeWeapon,Projectile,Inferno} (`which` = 0, 1, 2). Hôte : victime = avatar
// dont un coup est en cours de mesure (hits::measured_target) → true (mise à terre ou mort annulée).
// Sinon, en session chez l'invité seulement. Victime = *(void**)other, ou GetOtherEntity(spu) si `other` est nul. Victime
// = héros et PV ≤ 0 : PV remis à 1, mise à terre notée (cause 1 + which), true (le détour rend 0
// sans appeler l'original). Sinon, ou sur échec : false (original appelé). Ne lève jamais.
bool on_down_or_dead(int which, void* spu, void* other) noexcept;

// Frame : mise à terre notée depuis l'appel précédent (au plus une par frame) → LocalDown.
void drain(std::vector<core::session::Input>& out);

// Invité, commande RespawnHero : si `teleport`, héros placé à `pose` (Entity::MoveTo) ; puis PV =
// GetHitPointsMax (SetHitPoints fixé).
void respawn(const core::Pose& pose, bool teleport);

}  // namespace mp::sync::guest_health
