// Règles du détour des dégâts (stories 3.5 et 3.6, AD-9, AD-11, AD-12), sans Windows ni appel au
// jeu : testées sous Linux (client/tests/test_hit_rules.cpp). sync/hits lit l'état (garde, session,
// rôle, héros, agresseur, registre) et applique la décision rendue ici, dans l'ordre :
//   1. garde « application d'un HitRequest » active → laissé ;
//   2. hors session, ou rôle inconnu → laissé ;
//   3. invité, cible = réplique de PNJ : bloqué ; NetId mis en file seulement si l'agresseur est son
//      héros et la réplique vivante ;
//   3b. invité, cible = son héros : bloqué quel que soit l'agresseur, rien en file (seuls les
//      HitOnGuest blessent) ;
//   4. hôte, agresseur = avatar de l'invité : bloqué, rien en file ;
//   4b. hôte, cible = avatar de l'invité. Héros de l'hôte → bloqué (pas de tir ami) ; agresseur nul
//      → bloqué, rien ; sinon coup **mesuré** : l'original s'applique sur l'avatar, la perte de PV
//      part en HitOnGuest (source = NetId du PNJ capturé, sinon 0) et les PV sont rendus ;
//   5. sinon → laissé.
// Un coup bloqué rend en principe true sans appeler l'original (`impact`) : le jeu joue l'effet
// d'impact (sang, son) sans dégâts. Seuls le tir ami et l'agresseur nul (4b) rendent false.
// S'y ajoutent deux calculs purs : dégâts d'un coup mesuré (hôte) et PV du héros invité après un
// HitOnGuest (invité, maintien à 1 PV jusqu'à la story 3.7).
#pragma once

#include <cstdint>

#include "sync/anim_filter.h"

namespace mp::sync::hit_rules {

enum class HitRole : int { Unknown = 0, Host = 1, Guest = 2 };

enum class Reason : std::uint8_t {
    Pass,            // laissé
    HitRequest,      // 3. bloqué, NetId en file
    DeadReplica,     // 3. bloqué, réplique morte (agresseur = héros)
    OtherAggressor,  // 3. bloqué, autre agresseur
    GuestAvatar,     // 4. bloqué, agresseur = avatar invité chez l'hôte
    GuestHeroByReplica,  // 3b. bloqué, une réplique frappe le héros invité
    GuestHeroOther,      // 3b. bloqué, autre agresseur sur le héros invité
    AvatarHit,           // 4b. mesuré, HitOnGuest à émettre
    AvatarFriendly,      // 4b. bloqué, héros de l'hôte sur l'avatar (pas de tir ami)
    AvatarNoAggressor,   // 4b. bloqué, agresseur nul, sans HitOnGuest
};

struct Inputs {
    bool guard = false;       // garde de réentrance active sur ce thread
    bool in_session = false;
    HitRole role = HitRole::Unknown;
    const void* hero = nullptr;
    const void* aggressor = nullptr;
    const void* target = nullptr;
    bool target_found = false;  // find_by_entity(target)
    TrackedEntity target_entry;
    bool aggressor_found = false;  // find_by_entity(aggressor)
    TrackedEntity aggressor_entry;
    bool replica_dead = false;  // la réplique cible est morte (sync/npc::die)
};

struct Decision {
    bool block = false;              // original non appelé
    bool impact = false;             // bloqué : rendre true (effet d'impact sans dégâts), sinon false
    bool measure = false;            // 4b : original appelé, perte de PV de l'avatar mesurée puis rendue
    std::uint32_t queue_net_id = 0;  // ≠ 0 : NetId à mettre en file (HitRequest)
    bool hit_on_guest = false;       // 4b : HitOnGuest à émettre (dégâts mesurés par l'appelant)
    std::uint32_t source_net_id = 0; // 4b : NetId du PNJ agresseur, 0 = source inconnue
    Reason reason = Reason::Pass;
};

constexpr std::uint32_t kMaxRawDamage = 1000000;

// PV du héros invité après un HitOnGuest (sync/guest_health, seul écrivain, AD-11).
struct GuestHp {
    int new_hp = 0;
    bool write = false;  // écrire new_hp (SetHitPoints fixé)
    bool held = false;   // coup mortel : héros maintenu à 1 PV (à terre : story 3.7)
};

// Règles 1 et 2 seules : le détour doit-il aller plus loin (lire l'agresseur) ?
bool considered(bool guard, bool in_session, HitRole role);
// Règles 1 à 5.
Decision decide(const Inputs& in);
// Dégâts d'un coup mesuré sur l'avatar (hôte) : PV avant − PV après ; ≤ 0 → 0 ; plafonné à
// kMaxRawDamage. Les PV avant sont les PV max (fixés le temps du coup) : la perte n'est pas bornée
// par les PV affichés de l'avatar. Armure de l'avatar (celle de l'invité) comprise.
// `player_party` : l'avatar est du groupe « Player » ; le script de dégâts du jeu divise alors par
// deux les dégâts d'un PNJ (IsInPlayerParty), que le joueur subirait entiers : la perte est doublée.
std::uint32_t measured_damage(int hp_before, int hp_after, bool player_party);
// damage = 0 → pas d'écriture ; hp ≤ 1 → pas d'écriture, held ; hp − damage ≤ 0 → 1, held ;
// sinon hp − damage.
GuestHp guest_hp_after(int hp, std::uint32_t damage);

}  // namespace mp::sync::hit_rules
