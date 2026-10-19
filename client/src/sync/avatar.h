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
#include <string>

#include "core/session.h"

namespace mp::sync::avatar {

// Crée l'avatar `net_id` à `pose`. Un avatar déjà présent sous ce NetId est gardé tel quel.
void spawn(std::uint32_t net_id, const core::session::Pose& pose);
// Retire puis détruit l'avatar `net_id` (sans effet s'il n'existe pas).
void despawn(std::uint32_t net_id);
// Place l'avatar `net_id` (Entity::MoveTo), à chaque frame, en silence. Après une première
// erreur (SEH, journalisée), l'avatar n'est plus déplacé.
void move(std::uint32_t net_id, const core::session::Pose& pose);
// Rejoue un Motion du pair (AD-8) : GetMotion(nom) → SetMotion → PlayMotion avec le
// descripteur reconstruit, sous ReplayGuard. Un échec est journalisé une fois par nom.
void play(std::uint32_t net_id, const std::string& name, const protocol::Motion& motion);
// Met l'avatar en ragdoll (Death du pair) : il n'est plus déplacé ni animé jusqu'au retrait.
void die(std::uint32_t net_id);
// Un avatar existe-t-il ? (sentinelle de diagnostic)
bool present();
// Déplace le héros local (invité, à l'entrée en session) par Entity::MoveTo.
void teleport_player(const core::session::Pose& pose);

// Hooks eCAnimation_PS::PlayMotion et SetRagDollEnabled en session : capturent les appels sur
// le PS du héros local (AnimFilter), retournent false pour bloquer ceux du moteur sur un avatar.
// N'appellent jamais le jeu, hormis les lectures mémoire protégées de game/.
bool on_play_motion(void* animation_ps, int type, void* desc, bool flag) noexcept;
bool on_set_ragdoll(void* animation_ps, bool on, bool flag, const float* vector) noexcept;

}  // namespace mp::sync::avatar
