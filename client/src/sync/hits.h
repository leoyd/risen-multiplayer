// Coups de l'invité et coups subis (stories 3.5 et 3.6, AD-9, AD-11, AD-12) : logique du détour de
// gCDamage_PS::DoDamage(eCEntity*) (Game.dll, Q8), seul passage commun de tout coup natif avant la
// baisse des PV, et application des HitRequest chez l'hôte.
//
// Détour (`on_damage`), dans l'ordre (règles pures dans sync/hit_rules, testées sous Linux) :
//   1. garde « application d'un HitRequest » active sur ce thread → laissé ;
//   2. hors session, ou rôle inconnu → laissé ;
//   3. invité, cible = réplique de PNJ (registre, rôle NpcReplica) : bloqué. Si l'agresseur est son
//      héros et la réplique vivante, son NetId est mis en file (LocalHit, HitRequest de la frame) ;
//   3b. invité, cible = son héros : bloqué quel que soit l'agresseur, rien en file (seuls les
//      HitOnGuest blessent, par sync/guest_health) ;
//   4. hôte, agresseur = avatar de l'invité (registre, rôle Avatar) : bloqué, rien d'émis (seuls
//      les HitRequest blessent) ;
//   4b. hôte, cible = avatar de l'invité : bloqué (invulnérable). Héros de l'hôte ou agresseur nul :
//      rien d'émis ; sinon dégâts bruts lus sur le PS (game::damage_raw) et {source, dégâts} mis en
//      file (LocalHitOnGuest, HitOnGuest de la frame) ;
//   5. sinon → laissé.
// En cas de doute (agresseur illisible, exception), le coup est laissé, sauf s'il vise le héros
// invité (3b) ou l'avatar invité chez l'hôte (4b) : l'agresseur est alors tenu pour nul et le coup
// reste bloqué (sans HitOnGuest), ces règles ne dépendant que de la cible. Des dégâts bruts
// illisibles laissent le coup sur l'avatar bloqué, sans HitOnGuest. Le détour ne fait que lire le
// registre, des files sous verrou court et des atomiques : ni attente, ni réseau, ni appel au jeu
// hors des accesseurs (game::damage_aggressor, game::damage_raw).
//
// Frame (thread principal, AD-2) : `publish` (héros, rôle, état de session), `drain` (LocalHit,
// LocalHitOnGuest et LocalHitsBlocked vers la session), `apply` (hôte : ApplyHit).
#pragma once

#include <cstdint>
#include <vector>

#include "core/config.h"
#include "core/protocol.h"
#include "core/session.h"

namespace mp::sync::hits {

// Détour : true = bloquer (l'appelant rend false sans appeler l'original). Ne lève jamais.
bool on_damage(void* damage_ps, void* target) noexcept;

// Frame : héros local (Entity::GetPlayer, eCEntity*), rôle et état « en session » vus par le
// détour. Le passage à « en session » remet à zéro le diagnostic de session, la file et les
// répliques mortes ; la sortie vide la file.
void publish(void* hero, Role role, bool in_session, std::uint32_t frame_thread);
// Héros local publié par la frame (nul hors monde chargé). Frame : sync/guest_health.
void* hero();
// Story 3.7 : rôle invité et « en session » publiés par la frame (lecture atomique, sûre depuis les
// détours DownOrDeadBy* de sync/guest_health).
bool guest_in_session();
// Frame : coups mis en file par le détour (LocalHit, puis LocalHitOnGuest), puis coups bloqués
// depuis l'appel précédent (LocalHitsBlocked, si > 0).
void drain(std::vector<core::session::Input>& out);
// sync/npc::die : réplique morte, plus aucun HitRequest pour elle (jusqu'à la fin de session).
void mark_dead(std::uint32_t net_id);
// Hôte, frame : applique un HitRequest : PSDamage::DoDamage(arme en main de l'avatar, PNJ de ce
// GUID, avatar), sous la garde de réentrance. Jamais de SetHitPoints direct.
void apply(std::uint32_t net_id, const protocol::Guid& guid);

}  // namespace mp::sync::hits
