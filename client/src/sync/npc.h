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
//   de Kill ni de destruction ; NpcDeath (story 3.4b) la tue « visuellement » (die). Une fois par frame (validate), GetEntity(GUID) doit redonner la
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
// - Attaques natives (story 3.8, bloc facultatif game::resolve_native_attacks) :
//   - hôte : detect_attacks lit à chaque frame, pour chaque PNJ capturé, GetLastAttackMove et
//     GetCurrentMove (PSCombatSystem) ; un début d'attaque (règle pure sync::hit_rules::attack_started)
//     devient LocalNpcAttack{NetId, mouvement, cible}, cible traduite en NetId (héros 1, avatar 2,
//     PNJ capturé, sinon 0) ;
//   - invité : attack fixe la cible de la réplique (SetCurrentTarget) puis lance le mouvement
//     (StartMove), hors garde de rejeu : l'animation locale reste bloquée par le registre.
//
// - Dialogues de l'invité (story 4.2) :
//   - invité : set_freed libère la réplique partenaire de son dialogue (registre : ni routine ni
//     animations bloquées, AnimFilter::set_freed) puis la re-fige ; libérée, move, set_hp, play,
//     play_param et attack sont sans effet ; die la re-fige d'abord. Journal debug des transitions de
//     PSDialog::IsBeingTalkedTo des répliques (H1 : la demande de dialogue passe-t-elle avec l'IA figée ?) ;
//   - hôte : hold fige la routine du PNJ de ce GUID (AnimFilter::hold_routine, sans rôle de réplique),
//     unhold la rend ; validate rend un PNJ tenu perdu, release_all les rend tous. play, play_param et
//     move rejouent sur lui les gestes et la pose de la réplique libérée de l'invité (sous ReplayGuard :
//     un PNJ tenu aussi capturé n'en renvoie pas l'écho) ;
//   - invité : libérée, la réplique est capturée sous son NetId (AnimFilter) et freed_pose lit sa pose.
//
// Tout est appelé depuis la frame (hook OnProcess, AD-2). Les erreurs sont journalisées ;
// aucune ne remonte.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

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
// Story 3.4b, invité : mort « visuelle » de la réplique, sans Kill natif : SetHitPoints(0, fixer),
// AIMode = 12 (PSRoutine::PropertyAIMode), puis SetRagDollEnabled(1, 0, nul) sous ReplayGuard. Un
// échec est journalisé sans arrêter les étapes suivantes ; la réplique est marquée morte quoi qu'il
// arrive (move, set_hp, play et play_param sans effet ensuite). Elle reste au registre (rôle
// NpcReplica : IA, animations et ragdoll natifs bloqués) jusqu'à release ou release_all, qui la
// laissent morte (aucun appel au jeu).
void die(std::uint32_t net_id);
// Story 3.4, invité : rejoue un Motion (GetMotion(nom) → SetMotion → PlayMotion, descripteur
// reconstruit) ou un réglage de locomotion de l'hôte sur la réplique, sous ReplayGuard. Un échec
// est journalisé une fois par nom (Motion) ou par réplique (réglage).
void play(std::uint32_t net_id, const std::string& name, const protocol::Motion& motion);
void play_param(std::uint32_t net_id, const protocol::MotionParam& param);
// Story 3.8, invité : la réplique `net_id` vise `target` (1 avatar de l'hôte, 2 héros local, ≥ 1000
// réplique de ce NetId, 0 ou introuvable : cible effacée, pour qu'une ancienne cible ne serve pas)
// puis lance le mouvement `move`
// (PSCombatSystem::SetCurrentTarget, StartMove). Réplique inconnue ou morte : ignoré. Un SEH est
// journalisé une fois par session (Warn), le NpcAttack est alors sans effet.
void attack(std::uint32_t net_id, std::uint32_t move, std::uint32_t target);
// Story 3.8, hôte : débuts d'attaque des PNJ capturés depuis la frame précédente → LocalNpcAttack.
// `hero` = héros de l'hôte (cible 1). Sans effet si le bloc des attaques natives n'est pas résolu.
void detect_attacks(void* hero, std::vector<core::session::Input>& out);
// Story 3.8b, invité : début d'attaque de son héros `hero` depuis la frame précédente (même règle,
// hit_rules::attack_started) → LocalNpcAttack{2, mouvement, cible}, cible traduite par
// hit_rules::guest_attack_target_net_id (avatar de l'hôte 1, réplique son NetId, sinon 0). Sans effet
// si le bloc des attaques natives n'est pas résolu. État oublié par release_all.
void detect_hero_attack(void* hero, std::vector<core::session::Input>& out);
// Story 3.8b, hôte : entité du PNJ capturé `net_id` (nul s'il ne l'est pas). Le temps de la frame.
void* captured_entity(std::uint32_t net_id);
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
// GetEntity(GUID + 4 octets nuls) : entité native (eCEntity*), nul si introuvable. False sur SEH.
// Frame seulement (story 3.5 : cible d'un HitRequest chez l'hôte).
bool find_entity(const protocol::Guid& guid, void*& out);
// Invité : entité de la réplique `net_id` (nul si inconnue ou morte). Frame seulement, jamais gardée.
void* replica_entity(std::uint32_t net_id);
// Story 4.2, invité : NetId de la réplique vivante d'entité `entity` (0 si ce n'en est pas une).
std::uint32_t replica_net_id(const void* entity);
// Story 4.2, invité : libère (`freed`) ou re-fige la réplique `net_id` (FreeReplica). Inconnue ou
// morte : sans effet (journalisé en debug).
void set_freed(std::uint32_t net_id, bool freed);
// Story 4.2, invité : World::freed_* = NetId et pose de la réplique libérée (une au plus), à chaque frame.
void freed_pose(core::session::World& world);
// Story 4.2, invité : transitions de PSDialog::IsBeingTalkedTo de chaque réplique, en debug seulement
// (`running` : InfoSystem::IsRunning de l'invité à cette frame, pour le journal).
void log_talk_states(bool running);
// Story 4.2, hôte : tenir le PNJ de ce GUID (routine figée) pendant le dialogue de l'invité, le rendre.
void hold(std::uint32_t net_id, const protocol::Guid& guid);
void unhold(std::uint32_t net_id);
// Story 4.2 : PNJ que hold (GUID introuvable, pas de PS de routine), validate (PNJ tenu perdu) ou
// set_freed(…, true) (réplique absente ou hors registre) n'ont pas pu tenir ou libérer depuis le dernier
// appel → LocalDialogNpcLost, pour que la session oublie la tenue ou clôe le dialogue de l'invité.
void drain_lost(std::vector<core::session::Input>& out);
std::size_t count();  // répliques (invité), PNJ capturés, à capturer ou tenus (hôte)

}  // namespace mp::sync::npc
