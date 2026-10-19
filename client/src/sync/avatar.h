// Avatars du pair (story 2.2) : seul propriétaire des entités qui représentent l'autre joueur.
//
// - Création à réception d'un Spawn : Entity::Spawn(template du héros local, pose,
//   temporary = true), puis enregistrement dans la table NetId et dans l'AnimFilter
//   (réplique : le moteur ne l'anime jamais de lui-même, AD-8).
// - Retrait (AD-11) : sortie de l'AnimFilter, effacement de la table, puis Entity::Kill.
//   Kill n'est appelé que si le monde de la création est toujours là (même joueur, partie en
//   cours) ; sinon l'avatar a été détruit avec le monde et il est seulement oublié.
// - Aucun pointeur gardé hors de la table, ni après le retrait.
//
// Tout sauf les deux hooks est appelé depuis la frame (hook OnProcess, AD-2). Les erreurs
// sont journalisées ; aucune ne remonte (le jeu continue sans avatar).
#pragma once

#include <cstdint>

#include "core/session.h"

namespace mp::sync::avatar {

// Crée l'avatar `net_id` à `pose`. Un avatar déjà présent sous ce NetId est gardé tel quel.
void spawn(std::uint32_t net_id, const core::session::Pose& pose);
// Retire puis détruit l'avatar `net_id` (sans effet s'il n'existe pas).
void despawn(std::uint32_t net_id);
// Déplace le héros local (invité, à l'entrée en session) par Entity::MoveTo.
void teleport_player(const core::session::Pose& pose);

// Hooks eCAnimation_PS::PlayMotion et SetRagDollEnabled en session : retournent false pour
// bloquer l'appel du moteur sur un avatar. N'appellent jamais le jeu.
bool on_play_motion(void* animation_ps) noexcept;
bool on_set_ragdoll(void* animation_ps, bool on) noexcept;

}  // namespace mp::sync::avatar
