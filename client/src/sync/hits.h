// Coups de l'invité (story 3.5, AD-9, AD-12) : logique du détour de gCDamage_PS::DoDamage(eCEntity*)
// (Game.dll, Q8), seul passage commun de tout coup natif avant la baisse des PV, et application
// des HitRequest chez l'hôte.
//
// Détour (`on_damage`), dans l'ordre (règles pures dans sync/hit_rules, testées sous Linux) :
//   1. garde « application d'un HitRequest » active sur ce thread → laissé ;
//   2. hors session, ou rôle inconnu → laissé ;
//   3. invité, cible = réplique de PNJ (registre, rôle NpcReplica) : bloqué. Si l'agresseur est son
//      héros et la réplique vivante, son NetId est mis en file (LocalHit, HitRequest de la frame) ;
//   4. hôte, agresseur = avatar de l'invité (registre, rôle Avatar) : bloqué, rien d'émis (seuls
//      les HitRequest blessent) ;
//   5. sinon → laissé (les coups reçus par le héros invité ou l'avatar relèvent de la 3.6).
// En cas de doute (agresseur illisible, exception), le coup est laissé. Le détour ne fait que lire
// le registre, une file sous verrou court et des atomiques : ni attente, ni réseau, ni appel au
// jeu hors des accesseurs de l'agresseur (game::damage_aggressor).
//
// Frame (thread principal, AD-2) : `publish` (héros, rôle, état de session), `drain` (LocalHit et
// LocalHitsBlocked vers la session), `apply` (hôte : ApplyHit).
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
// Frame : coups mis en file par le détour (LocalHit), puis coups bloqués depuis l'appel précédent
// (LocalHitsBlocked, si > 0).
void drain(std::vector<core::session::Input>& out);
// sync/npc::die : réplique morte, plus aucun HitRequest pour elle (jusqu'à la fin de session).
void mark_dead(std::uint32_t net_id);
// Hôte, frame : applique un HitRequest : PSDamage::DoDamage(arme en main de l'avatar, PNJ de ce
// GUID, avatar), sous la garde de réentrance. Jamais de SetHitPoints direct.
void apply(std::uint32_t net_id, const protocol::Guid& guid);

}  // namespace mp::sync::hits
