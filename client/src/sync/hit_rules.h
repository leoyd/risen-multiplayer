// Règles du détour des dégâts (stories 3.5 et 3.6, AD-9, AD-11, AD-12), sans Windows ni appel au
// jeu : testées sous Linux (client/tests/test_hit_rules.cpp). sync/hits lit l'état (garde, session,
// rôle, héros, agresseur, registre) et applique la décision rendue ici, dans l'ordre :
//   1. garde « application d'un HitRequest » active → laissé ;
//   2. hors session, ou rôle inconnu → laissé ;
//   3. invité, cible = réplique de PNJ : bloqué ; NetId mis en file seulement si l'agresseur est son
//      héros et la réplique vivante ;
//   3b. invité, cible = son héros : bloqué quel que soit l'agresseur, rien en file (seuls les
//      HitOnGuest blessent). Story 3.8 : agresseur = réplique et attaques natives actives → coup
//      **cosmétique** : l'original s'applique (effet d'impact, script de dégâts : recul, flash), puis
//      les PV d'avant sont rendus, mise à terre ou mort annulée (comme la mesure de l'hôte). Héros à
//      terre (story 3.7) : bloqué avec effet d'impact, comme en 3.6 ;
//   3c. invité, attaques natives actives, cible = avatar de l'hôte (story 3.8 : une réplique peut
//      désormais l'attaquer) : bloqué, rien en file ; effet d'impact, sauf tir ami de son héros ;
//   4. hôte, agresseur = avatar de l'invité : bloqué, rien en file ;
//   4b. hôte, cible = avatar de l'invité. Héros de l'hôte → bloqué (pas de tir ami) ; agresseur nul
//      → bloqué, rien ; sinon coup **mesuré** : l'original s'applique sur l'avatar, la perte de PV
//      part en HitOnGuest (source = NetId du PNJ capturé, sinon 0) et les PV sont rendus ;
//   5. sinon → laissé.
// Un coup bloqué rend en principe true sans appeler l'original (`impact`) : le jeu joue l'effet
// d'impact (sang, son) sans dégâts. Seuls le tir ami et l'agresseur nul (4b) rendent false.
// S'y ajoutent des calculs purs : dégâts d'un coup mesuré (hôte), PV du héros invité après un
// HitOnGuest (invité, maintien à 1 PV), et pour la story 3.8 la détection d'un début d'attaque
// (hôte), la traduction de sa cible en NetId et le recul de repli (invité).
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
    GuestHeroCosmetic,   // 3b (story 3.8). cosmétique : une réplique frappe le héros invité, PV rendus
    HostAvatarOnGuest,   // 3c (story 3.8). bloqué : coup natif sur l'avatar de l'hôte chez l'invité
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
    bool native_attacks = false;  // story 3.8 : attaques natives actives (coups des répliques cosmétiques)
    bool hero_down = false;       // story 3.8 : héros invité à terre (sync/guest_health) : pas de cosmétique
};

struct Decision {
    bool block = false;              // original non appelé
    bool impact = false;             // bloqué : rendre true (effet d'impact sans dégâts), sinon false
    bool measure = false;            // 4b : original appelé, perte de PV de l'avatar mesurée puis rendue
    bool cosmetic = false;           // 3b (story 3.8) : original appelé, PV du héros invité rendus
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

// Story 3.8 (D1), hôte : dernier état lu du système de combat d'un PNJ capturé.
struct AttackState {
    bool known = false;   // première lecture faite
    int current = 0;      // PSCombatSystem::GetCurrentMove
    int last_attack = 0;  // PSCombatSystem::GetLastAttackMove
    // LastAttackMove a changé avant que CurrentMove l'atteigne : l'entrée de CurrentMove dans ce
    // mouvement est la même attaque, déjà annoncée (pas de second début).
    bool announced = false;
};
// Met `state` à jour avec la lecture de la frame. Vrai si une attaque commence : déjà lu, dernier
// mouvement d'attaque non nul, et soit il a changé, soit le mouvement en cours redevient égal au
// dernier mouvement d'attaque après avoir valu autre chose, sauf si cette entrée suit le changement
// de LastAttackMove qui l'a déjà annoncée. La première lecture n'émet rien.
bool attack_started(AttackState& state, int current, int last_attack);
// Story 3.8 (D1), hôte : NetId de la cible d'une attaque. Nulle → 0 ; héros de l'hôte → 1 ; avatar
// de l'invité → 2 ; PNJ capturé du registre → son NetId ; sinon 0.
std::uint32_t attack_target_net_id(const void* target, const void* hero, const void* avatar, bool found,
                                   const TrackedEntity& entry);
// Story 3.8b (D2), invité : NetId de la cible d'une attaque de son héros. Nulle → 0 ; avatar de l'hôte
// (`avatar`, ou rôle Avatar au registre) → 1 ; réplique de PNJ du registre → son NetId (≥ 1000) ;
// sinon 0 (son héros compris).
std::uint32_t guest_attack_target_net_id(const void* target, const void* avatar, bool found,
                                         const TrackedEntity& entry);
// Story 3.8 (D4, différé), invité : un HitOnGuest non mortel met en attente un recul générique
// (StartStumble) ; il est annulé si un coup cosmétique touche le héros dans les kCosmeticWindowMs
// avant ou après le HitOnGuest, sinon joué kCosmeticWindowMs après lui. Un HitOnGuest plus récent
// remplace l'attente.
constexpr std::uint64_t kCosmeticWindowMs = 500;
struct PendingStumble {
    bool active = false;
    std::uint32_t source = 0;   // NetId du PNJ agresseur
    std::uint64_t hit_ms = 0;   // application du HitOnGuest (horloge QPC, ms)
};
enum class StumbleStep { Idle, Wait, Play, Cancel };
// `any_cosmetic` : un coup cosmétique a touché le héros dans la session, le dernier à `last_cosmetic_ms`.
StumbleStep stumble_step(const PendingStumble& p, std::uint64_t now_ms, bool any_cosmetic,
                         std::uint64_t last_cosmetic_ms);

}  // namespace mp::sync::hit_rules
