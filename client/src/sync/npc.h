// Répliques des PNJ (story 3.3) : seul module qui lit les PNJ pour la session et qui inscrit des
// répliques (rôle NpcReplica) au registre des entités (AD-12).
//
// - Hôte : à chaque snapshot (Session::wants_npcs), lecture de Entity::ms_arrNPCs (après
//   UpdateEntityLists, jamais GetNPCs), sans le héros ni l'avatar ; pour chaque PNJ assez proche
//   d'un des deux joueurs (marge au-delà de kNpcRadius, la sélection exacte est celle de
//   core/session) : GUID, pose, PV, vivant. Aucun pointeur gardé après la lecture.
// - Invité : SpawnNpc retrouve le PNJ natif par gCRuntimeInfo::GetEntity(GUID + 4 octets nuls),
//   l'inscrit au registre avec son PS de routine (le détour de gCScriptRoutine_PS::OnProcess fige
//   alors son IA) et le place ; MoveNpc et SetNpcHp écrivent sa pose (Entity::MoveTo) et ses PV
//   (SetHitPoints(pv, fixer)) ; ReleaseNpc le sort du registre (IA rendue) et l'oublie. Jamais
//   de Kill ni de destruction. Une fois par frame (validate), GetEntity(GUID) doit redonner la
//   même entité, sinon la réplique est perdue ; un MoveTo en SEH la libère aussi (IA rendue).
//   Sans gCScriptRoutine_PS, le PNJ est traité comme un GUID introuvable.
// - Le PS d'animation n'est pas inscrit : le moteur anime encore la réplique (story 3.4).
//
// Tout est appelé depuis la frame (hook OnProcess, AD-2). Les erreurs sont journalisées ;
// aucune ne remonte.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/session.h"

namespace mp::sync::npc {

// Hôte : remplit world.npcs et world.npcs_ok (faux si la liste est illisible). `world` doit
// porter le joueur et sa pose ; sans pose lisible, rien n'est lu.
void read_host(core::session::World& world);

// Invité.
void spawn(std::uint32_t net_id, const protocol::Guid& guid, const core::session::Pose& pose);
void move(std::uint32_t net_id, const core::session::Pose& pose);
void set_hp(std::uint32_t net_id, int hp);
void release(std::uint32_t net_id);
// Une fois par frame, avant les commandes : chaque réplique dont GetEntity(GUID) ne rend plus
// la même entité sort du registre et de la table (journalisée une fois par NetId).
void validate();
// Hors session (après Ended, ReleaseNpc déjà exécutés) : filet de sécurité, toutes les
// répliques restantes sortent du registre, sans appel au jeu ; les NetId introuvables déjà
// signalés sont oubliés (une ligne par NetId et par session). Sans effet si tout est vide.
void release_all();
std::size_t count();

}  // namespace mp::sync::npc
