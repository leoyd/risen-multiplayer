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
// - Animations (story 3.4) :
//   - hôte : CaptureNpc inscrit le PS d'animation et l'enveloppe EMotionFX du PNJ de ce GUID
//     (GetEntity), rôle NpcCapture avec son NetId : ses PlayMotion et réglages sont capturés comme
//     ceux du héros. UncaptureNpc et release_all l'en retirent. Une capture en échec, ou un PNJ
//     perdu (validate), reste voulu : validate réessaie au plus une fois par seconde ;
//   - invité : la réplique est inscrite avec son PS d'animation et son enveloppe : son moteur ne
//     l'anime plus (seul le rejeu, sous ReplayGuard, passe). play / play_param rejouent les
//     animations de l'hôte ; libérée, elle sort du registre et son moteur l'anime de nouveau.
//
// Tout est appelé depuis la frame (hook OnProcess, AD-2). Les erreurs sont journalisées ;
// aucune ne remonte.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

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
// Story 3.4, invité : rejoue un Motion (GetMotion(nom) → SetMotion → PlayMotion, descripteur
// reconstruit) ou un réglage de locomotion de l'hôte sur la réplique, sous ReplayGuard. Un échec
// est journalisé une fois par nom (Motion) ou par réplique (réglage).
void play(std::uint32_t net_id, const std::string& name, const protocol::Motion& motion);
void play_param(std::uint32_t net_id, const protocol::MotionParam& param);
// Story 3.4, hôte : inscrit (capture) ou retire (uncapture) le PNJ animé `net_id`.
void capture(std::uint32_t net_id, const protocol::Guid& guid);
void uncapture(std::uint32_t net_id);
// Une fois par frame, avant les commandes : chaque réplique (ou PNJ capturé de l'hôte) dont
// GetEntity(GUID) ne rend plus la même entité sort du registre et de la table (journalisée une
// fois par NetId).
void validate();
// Hors session (après Ended, ReleaseNpc déjà exécutés) ou dès que la frame voit le monde changer
// (story 3.4a, avant validate) : toutes les répliques restantes sortent du registre, sans appel au
// jeu ; les NetId introuvables déjà signalés sont oubliés (une ligne par NetId et par session).
// Sans effet si tout est vide. Les PNJ capturés de l'hôte sortent aussi du registre. `cause`
// (« hors session », « fin de monde ») termine la ligne de log.
void release_all(const std::string& cause);
std::size_t count();  // répliques (invité), PNJ capturés ou à capturer (hôte)

}  // namespace mp::sync::npc
