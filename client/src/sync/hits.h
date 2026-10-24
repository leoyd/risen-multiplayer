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
//      HitOnGuest blessent, par sync/guest_health). Story 3.8 : agresseur = réplique et attaques
//      natives actives → coup cosmétique : original appelé (sang, recul, flash), PV d'avant rendus
//      aussitôt, mise à terre ou mort annulée (measured_target) ;
//   3c. invité, cible = avatar de l'hôte : bloqué, rien d'émis (story 3.8 : une réplique peut
//      l'attaquer) ; effet d'impact sauf tir ami de son héros ;
//   4. hôte, agresseur = avatar de l'invité (registre, rôle Avatar) : bloqué, rien d'émis (seuls
//      les HitRequest blessent) ;
//   4b. hôte, cible = avatar de l'invité. Héros de l'hôte ou agresseur nul : bloqué, rien d'émis ;
//      sinon coup mesuré : avatar remis à ses PV max, original appelé (scripts de dégâts du jeu,
//      armure de l'avatar comprise), perte de PV mise en file {source, dégâts} (LocalHitOnGuest,
//      HitOnGuest de la frame), PV d'avant rendus. Mise à terre ou mort de l'avatar annulée par
//      sync/guest_health (measured_target) ;
//   5. sinon → laissé.
// Un coup bloqué rend true sans appeler l'original (effet d'impact du jeu, sans dégâts), sauf le
// tir ami et l'agresseur nul (4b), et l'avatar qui frappe le héros de l'hôte (4), qui rendent false.
// En cas de doute (agresseur illisible, exception), le coup est laissé, sauf s'il vise le héros
// invité (3b) ou l'avatar invité chez l'hôte (4b) : l'agresseur est alors tenu pour nul et le coup
// reste bloqué (sans HitOnGuest), ces règles ne dépendant que de la cible. Des dégâts bruts
// Des PV d'avatar illisibles laissent le coup sur l'avatar bloqué, sans HitOnGuest. Le détour ne
// fait que lire le registre, des files sous verrou court et des atomiques : ni attente, ni réseau.
// Appels au jeu : l'accesseur game::damage_aggressor, et pour un coup mesuré (4b, même thread
// qu'OnProcess) les PV de l'avatar (lecture, SetHitPoints fixé avant et après).
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

// Détour : rend le résultat de gCDamage_PS::DoDamage pour ce coup, en appelant `original` s'il est
// laissé ou mesuré. Ne lève jamais (hors exception de l'original lui-même).
using DamageFn = bool (*)(void* damage_ps, void* target);
bool on_damage(void* damage_ps, void* target, DamageFn original) noexcept;
// Avatar dont un coup est en cours de mesure (hôte), ou héros invité sous un coup cosmétique (invité,
// story 3.8), sur ce thread ; sinon nul (détours DownOrDeadBy*).
void* measured_target();
// Story 3.8 : coups des répliques sur le héros invité rendus nativement (cosmétiques). Réglé une fois
// par sync/session (native_attacks et bloc résolu) ; faux : règle 3b de la story 3.6.
void set_native_attacks(bool on);
bool native_attacks();
// Story 3.8 (D4) : horloge QPC (ms) ; dernier coup cosmétique de la session sur le héros invité
// (`ms`), false s'il n'y en a eu aucun.
std::uint64_t now_ms();
// Un coup natif d'`aggressor` (avatar ou réplique) est passé au détour depuis `since_ms` (horloge de
// now_ms). Frame : arrêt du mouvement local d'une attaque native une fois le coup porté.
bool native_hit_since(const void* aggressor, std::uint64_t since_ms);
bool last_cosmetic(std::uint64_t& ms);

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
