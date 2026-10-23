// Règles du détour des dégâts (story 3.5) : un cas par règle (sync/hit_rules).
#include "sync/hit_rules.h"
#include "test.h"

using mp::sync::EntityRole;
using mp::sync::TrackedEntity;
using namespace mp::sync::hit_rules;

namespace {

int g_hero, g_avatar, g_replica, g_other;

TrackedEntity entry(void* entity, EntityRole role, std::uint32_t net_id = 0) {
    TrackedEntity e;
    e.entity = entity;
    e.role = role;
    e.net_id = net_id;
    return e;
}

// Invité en session : son héros frappe la réplique 1000.
Inputs guest_hits_replica() {
    Inputs in;
    in.in_session = true;
    in.role = HitRole::Guest;
    in.hero = &g_hero;
    in.aggressor = &g_hero;
    in.target = &g_replica;
    in.target_found = true;
    in.target_entry = entry(&g_replica, EntityRole::NpcReplica, 1000);
    return in;
}

// Hôte en session : l'avatar de l'invité frappe un PNJ hors registre.
Inputs host_avatar_hits() {
    Inputs in;
    in.in_session = true;
    in.role = HitRole::Host;
    in.hero = &g_hero;
    in.aggressor = &g_avatar;
    in.target = &g_other;
    in.aggressor_found = true;
    in.aggressor_entry = entry(&g_avatar, EntityRole::Avatar, 2);
    return in;
}

bool passes(const Decision& d) { return !d.block && d.queue_net_id == 0 && d.reason == Reason::Pass; }

}  // namespace

TEST("règles des dégâts 1 : garde active avec agresseur avatar chez l'hôte → laissé") {
    Inputs in = host_avatar_hits();
    in.guard = true;
    CHECK(passes(decide(in)));
    CHECK(!considered(true, true, HitRole::Host));
}

TEST("règles des dégâts 1 : garde active, héros invité sur une réplique → laissé, rien en file") {
    Inputs in = guest_hits_replica();
    in.guard = true;
    CHECK(passes(decide(in)));
}

TEST("règles des dégâts 2 : hors session ou rôle inconnu → laissé") {
    Inputs in = guest_hits_replica();
    in.in_session = false;
    CHECK(passes(decide(in)));
    CHECK(!considered(false, false, HitRole::Guest));
    in = host_avatar_hits();
    in.role = HitRole::Unknown;
    CHECK(passes(decide(in)));
    CHECK(!considered(false, true, HitRole::Unknown));
    CHECK(considered(false, true, HitRole::Guest));
}

TEST("règles des dégâts 3 : invité, son héros sur une réplique vivante → bloqué, NetId en file") {
    const Decision d = decide(guest_hits_replica());
    CHECK(d.block && d.impact && !d.measure);
    CHECK_EQ(d.queue_net_id, 1000u);
    CHECK(d.reason == Reason::HitRequest);
}

TEST("règles des dégâts 3 : invité, réplique morte → bloqué sans file") {
    Inputs in = guest_hits_replica();
    in.replica_dead = true;
    const Decision d = decide(in);
    CHECK(d.block);
    CHECK_EQ(d.queue_net_id, 0u);
    CHECK(d.reason == Reason::DeadReplica);
}

TEST("règles des dégâts 3 : invité, autre agresseur (avatar de l'hôte, nul) → bloqué sans file") {
    Inputs in = guest_hits_replica();
    in.aggressor = &g_avatar;
    in.aggressor_found = true;
    in.aggressor_entry = entry(&g_avatar, EntityRole::Avatar, 1);
    Decision d = decide(in);
    CHECK(d.block && d.queue_net_id == 0 && d.reason == Reason::OtherAggressor);
    in.aggressor = nullptr;
    in.aggressor_found = false;
    d = decide(in);
    CHECK(d.block && d.queue_net_id == 0 && d.reason == Reason::OtherAggressor);
}

TEST("règles des dégâts 4 : hôte, agresseur avatar invité → bloqué, rien en file (cible quelconque)") {
    Decision d = decide(host_avatar_hits());
    CHECK(d.block && d.impact && d.queue_net_id == 0 && d.reason == Reason::GuestAvatar);
    Inputs in = host_avatar_hits();
    in.target = &g_hero;  // héros de l'hôte : pas de tir ami, pas d'effet d'impact
    d = decide(in);
    CHECK(d.block && !d.impact && d.reason == Reason::GuestAvatar);
}

TEST("règles des dégâts 5 : sinon laissé (héros de l'hôte sur un PNJ, PNJ sur le héros de l'hôte)") {
    // Hôte : son héros frappe un PNJ capturé.
    Inputs in;
    in.in_session = true;
    in.role = HitRole::Host;
    in.hero = &g_hero;
    in.aggressor = &g_hero;
    in.target = &g_replica;
    in.target_found = true;
    in.target_entry = entry(&g_replica, EntityRole::NpcCapture, 1000);
    CHECK(passes(decide(in)));
    // Hôte : un PNJ capturé frappe le héros de l'hôte (dégâts natifs normaux).
    in = Inputs{};
    in.in_session = true;
    in.role = HitRole::Host;
    in.hero = &g_hero;
    in.aggressor = &g_replica;
    in.aggressor_found = true;
    in.aggressor_entry = entry(&g_replica, EntityRole::NpcCapture, 1000);
    in.target = &g_hero;
    CHECK(passes(decide(in)));
    // Invité : un PNJ natif hors registre frappé par un autre (ni héros ni réplique en cible).
    in = Inputs{};
    in.in_session = true;
    in.role = HitRole::Guest;
    in.hero = &g_hero;
    in.aggressor = &g_other;
    in.target = &g_replica;
    CHECK(passes(decide(in)));
}

// --- Story 3.6 : coups subis par l'invité -------------------------------------------------------

namespace {

// Hôte en session : le PNJ capturé 1000 frappe l'avatar de l'invité.
Inputs host_npc_hits_avatar() {
    Inputs in;
    in.in_session = true;
    in.role = HitRole::Host;
    in.hero = &g_hero;
    in.aggressor = &g_replica;
    in.aggressor_found = true;
    in.aggressor_entry = entry(&g_replica, EntityRole::NpcCapture, 1000);
    in.target = &g_avatar;
    in.target_found = true;
    in.target_entry = entry(&g_avatar, EntityRole::Avatar, 2);
    return in;
}

// Invité en session : la réplique 1000 frappe son héros.
Inputs guest_replica_hits_hero() {
    Inputs in;
    in.in_session = true;
    in.role = HitRole::Guest;
    in.hero = &g_hero;
    in.aggressor = &g_replica;
    in.aggressor_found = true;
    in.aggressor_entry = entry(&g_replica, EntityRole::NpcReplica, 1000);
    in.target = &g_hero;
    return in;
}

bool blocked_silent(const Decision& d, Reason r) {
    return d.block && d.queue_net_id == 0 && !d.hit_on_guest && d.source_net_id == 0 && d.reason == r;
}

}  // namespace

TEST("règles des dégâts 4b : hôte, PNJ capturé frappe l'avatar → mesuré, HitOnGuest source 1000") {
    const Decision d = decide(host_npc_hits_avatar());
    CHECK(!d.block && d.measure);
    CHECK(d.hit_on_guest);
    CHECK_EQ(d.source_net_id, 1000u);
    CHECK_EQ(d.queue_net_id, 0u);
    CHECK(d.reason == Reason::AvatarHit);
}

TEST("règles des dégâts 4b : hôte, agresseur hors registre frappe l'avatar → mesuré, HitOnGuest source 0") {
    Inputs in = host_npc_hits_avatar();
    in.aggressor = &g_other;
    in.aggressor_found = false;
    in.aggressor_entry = TrackedEntity{};
    const Decision d = decide(in);
    CHECK(!d.block && d.measure && d.hit_on_guest && d.reason == Reason::AvatarHit);
    CHECK_EQ(d.source_net_id, 0u);
}

TEST("règles des dégâts 4b : hôte, son héros frappe l'avatar → bloqué, rien (pas de tir ami)") {
    Inputs in = host_npc_hits_avatar();
    in.aggressor = &g_hero;
    in.aggressor_found = false;
    in.aggressor_entry = TrackedEntity{};
    CHECK(blocked_silent(decide(in), Reason::AvatarFriendly));
    CHECK(!decide(in).impact && !decide(in).measure);
    // Héros connu du registre seulement (rôle LocalHero).
    in.hero = nullptr;
    in.aggressor_found = true;
    in.aggressor_entry = entry(&g_hero, EntityRole::LocalHero);
    CHECK(blocked_silent(decide(in), Reason::AvatarFriendly));
}

TEST("règles des dégâts 4b : hôte, agresseur nul sur l'avatar → bloqué, rien") {
    Inputs in = host_npc_hits_avatar();
    in.aggressor = nullptr;
    in.aggressor_found = false;
    in.aggressor_entry = TrackedEntity{};
    CHECK(blocked_silent(decide(in), Reason::AvatarNoAggressor));
    CHECK(!decide(in).impact && !decide(in).measure);
    in.hero = nullptr;  // héros pas encore publié : agresseur nul reste « nul », pas « tir ami »
    CHECK(blocked_silent(decide(in), Reason::AvatarNoAggressor));
}

TEST("règles des dégâts 4b : hôte, garde active, cible avatar → laissé (règle 1)") {
    Inputs in = host_npc_hits_avatar();
    in.guard = true;
    const Decision d = decide(in);
    CHECK(passes(d));
    CHECK(!d.hit_on_guest);
}

TEST("règles des dégâts 4 avant 4b : hôte, avatar qui se frappe lui-même → règle 4") {
    Inputs in = host_npc_hits_avatar();
    in.aggressor = &g_avatar;
    in.aggressor_entry = entry(&g_avatar, EntityRole::Avatar, 2);
    CHECK(blocked_silent(decide(in), Reason::GuestAvatar));
}

TEST("règles des dégâts 3b : invité, réplique frappe son héros → bloqué, rien en file") {
    CHECK(blocked_silent(decide(guest_replica_hits_hero()), Reason::GuestHeroByReplica));
    CHECK(decide(guest_replica_hits_hero()).impact);
}

TEST("règles des dégâts 3b : invité, agresseur inconnu ou nul sur son héros → bloqué, rien") {
    Inputs in = guest_replica_hits_hero();
    in.aggressor = &g_other;
    in.aggressor_found = false;
    in.aggressor_entry = TrackedEntity{};
    CHECK(blocked_silent(decide(in), Reason::GuestHeroOther));
    in.aggressor = nullptr;
    CHECK(blocked_silent(decide(in), Reason::GuestHeroOther));
    // Avatar de l'hôte (rôle Avatar chez l'invité) : autre agresseur.
    in.aggressor = &g_avatar;
    in.aggressor_found = true;
    in.aggressor_entry = entry(&g_avatar, EntityRole::Avatar, 1);
    CHECK(blocked_silent(decide(in), Reason::GuestHeroOther));
}

TEST("règles des dégâts 3b : invité, héros reconnu par le registre (LocalHero) → bloqué") {
    Inputs in = guest_replica_hits_hero();
    in.hero = nullptr;
    in.target_found = true;
    in.target_entry = entry(&g_hero, EntityRole::LocalHero);
    CHECK(blocked_silent(decide(in), Reason::GuestHeroByReplica));
}

TEST("règles des dégâts 3b : invité, garde active ou hors session → laissé") {
    Inputs in = guest_replica_hits_hero();
    in.guard = true;
    CHECK(passes(decide(in)));
    in.guard = false;
    in.in_session = false;
    CHECK(passes(decide(in)));
}

TEST("dégâts mesurés : 100 → 82 = 18 ; 100 → -30 = 130 ; gain ou égalité → 0 ; groupe Player ×2 ; plafond") {
    CHECK_EQ(measured_damage(100, 82, false), 18u);
    CHECK_EQ(measured_damage(100, -30, false), 130u);  // PV sous 0 : la perte entière compte
    CHECK_EQ(measured_damage(100, 100, false), 0u);
    CHECK_EQ(measured_damage(100, 120, false), 0u);
    CHECK_EQ(measured_damage(80, 75, true), 10u);  // divisé par deux par le jeu (IsInPlayerParty)
    CHECK_EQ(measured_damage(80, 80, true), 0u);
    CHECK_EQ(measured_damage(2000000, -2000000, false), kMaxRawDamage);
    CHECK_EQ(measured_damage(1000000, 0, true), kMaxRawDamage);
}

TEST("PV invité : (100, 18) → 82 ; (10, 18) → 1 held ; (1, 5) → rien, held ; (50, 0) → rien") {
    GuestHp r = guest_hp_after(100, 18);
    CHECK(r.write && !r.held);
    CHECK_EQ(r.new_hp, 82);
    r = guest_hp_after(10, 18);
    CHECK(r.write && r.held);
    CHECK_EQ(r.new_hp, 1);
    r = guest_hp_after(18, 18);  // exactement 0 → maintenu à 1
    CHECK(r.write && r.held);
    CHECK_EQ(r.new_hp, 1);
    r = guest_hp_after(1, 5);
    CHECK(!r.write && r.held);
    r = guest_hp_after(0, 5);
    CHECK(!r.write && r.held);
    r = guest_hp_after(50, 0);
    CHECK(!r.write && !r.held);
    r = guest_hp_after(100, kMaxRawDamage);
    CHECK(r.write && r.held);
    CHECK_EQ(r.new_hp, 1);
}

// --- Story 3.8 : attaques natives des répliques ------------------------------------------------

TEST("règles des dégâts 3b (3.8) : invité, réplique frappe son héros, attaques natives → cosmétique") {
    Inputs in = guest_replica_hits_hero();
    in.native_attacks = true;
    const Decision d = decide(in);
    CHECK(d.cosmetic && !d.block && !d.impact && !d.measure && !d.hit_on_guest);
    CHECK_EQ(d.queue_net_id, 0u);
    CHECK(d.reason == Reason::GuestHeroCosmetic);
    // Héros reconnu par le registre seulement : cosmétique aussi.
    in.hero = nullptr;
    in.target_found = true;
    in.target_entry = entry(&g_hero, EntityRole::LocalHero);
    CHECK(decide(in).cosmetic);
}

TEST("règles des dégâts 3b (3.8) : autre agresseur, attaques natives coupées, garde → inchangé (3.6)") {
    Inputs in = guest_replica_hits_hero();
    in.native_attacks = true;
    in.aggressor = &g_avatar;  // avatar de l'hôte : toujours bloqué avec effet
    in.aggressor_entry = entry(&g_avatar, EntityRole::Avatar, 1);
    Decision d = decide(in);
    CHECK(!d.cosmetic && blocked_silent(d, Reason::GuestHeroOther) && d.impact);
    in.aggressor = nullptr;
    in.aggressor_found = false;
    CHECK(!decide(in).cosmetic);
    in = guest_replica_hits_hero();  // native_attacks = 0 : comportement 3.6
    d = decide(in);
    CHECK(!d.cosmetic && blocked_silent(d, Reason::GuestHeroByReplica));
    in.native_attacks = true;
    in.guard = true;
    CHECK(passes(decide(in)) && !decide(in).cosmetic);
    // Hôte : sans effet (la règle 4b reste une mesure).
    in = host_npc_hits_avatar();
    in.native_attacks = true;
    d = decide(in);
    CHECK(d.measure && !d.cosmetic);
}

TEST("attaque 3.8 (D1) : première lecture muette, LastAttackMove 0 → M émis une fois, inchangé → rien") {
    AttackState s;
    CHECK(!attack_started(s, 0, 0));  // première lecture
    CHECK(s.known);
    CHECK(!attack_started(s, 0, 0));
    CHECK(attack_started(s, 7, 7));   // 0 → 7 : début d'attaque
    CHECK(!attack_started(s, 7, 7));  // même attaque
    CHECK(!attack_started(s, 3, 7));  // autre mouvement (parade…)
    CHECK(attack_started(s, 7, 7));   // le même coup recommence après autre chose
    CHECK(attack_started(s, 9, 9));   // nouveau mouvement d'attaque
    CHECK(attack_started(s, 2, 5));   // LastAttackMove change seul
    CHECK(!attack_started(s, 0, 0));  // mouvement 0 : jamais émis
    AttackState first;
    CHECK(!attack_started(first, 7, 7));  // déjà en attaque à la première lecture : rien
}

TEST("attaque 3.8 (D1) : LastAttackMove change une frame avant CurrentMove → un seul début") {
    AttackState s;
    CHECK(!attack_started(s, 0, 0));
    CHECK(attack_started(s, 0, 7));   // LastAttackMove annonce l'attaque
    CHECK(!attack_started(s, 7, 7));  // CurrentMove l'atteint : même attaque, pas de second début
    CHECK(!attack_started(s, 7, 7));
    CHECK(!attack_started(s, 0, 7));  // fin du mouvement
    CHECK(attack_started(s, 7, 7));   // nouveau coup du même mouvement
    CHECK(attack_started(s, 3, 9));   // autre attaque annoncée avant d'être en cours…
    CHECK(attack_started(s, 9, 9) == false);  // …puis atteinte : rien
}

TEST("attaque 3.8 (D1) : cible → héros 1, avatar 2, PNJ capturé NetId, hors registre ou nulle 0") {
    const TrackedEntity none{};
    CHECK_EQ(attack_target_net_id(&g_hero, &g_hero, &g_avatar, false, none), 1u);
    CHECK_EQ(attack_target_net_id(&g_avatar, &g_hero, &g_avatar, true, entry(&g_avatar, EntityRole::Avatar, 2)), 2u);
    CHECK_EQ(attack_target_net_id(&g_replica, &g_hero, &g_avatar, true, entry(&g_replica, EntityRole::NpcCapture, 1005)),
             1005u);
    CHECK_EQ(attack_target_net_id(&g_other, &g_hero, &g_avatar, false, none), 0u);
    CHECK_EQ(attack_target_net_id(nullptr, &g_hero, nullptr, false, none), 0u);  // pas d'avatar : nul ≠ cible
    CHECK_EQ(attack_target_net_id(&g_other, &g_hero, nullptr, false, none), 0u);
}

TEST("attaque 3.8b (D2) : cible d'une attaque du héros invité traduite en NetId") {
    const TrackedEntity none{};
    CHECK_EQ(guest_attack_target_net_id(nullptr, &g_avatar, false, none), 0u);
    CHECK_EQ(guest_attack_target_net_id(&g_avatar, &g_avatar, false, none), 1u);  // avatar de l'hôte
    CHECK_EQ(guest_attack_target_net_id(&g_avatar, nullptr, true, entry(&g_avatar, EntityRole::Avatar, 1)), 1u);
    CHECK_EQ(guest_attack_target_net_id(&g_replica, &g_avatar, true, entry(&g_replica, EntityRole::NpcReplica, 1004)),
             1004u);
    // Son propre héros, un PNJ capturé (rôle d'hôte), une entité inconnue : aucune cible.
    CHECK_EQ(guest_attack_target_net_id(&g_hero, &g_avatar, true, entry(&g_hero, EntityRole::LocalHero, 2)), 0u);
    CHECK_EQ(guest_attack_target_net_id(&g_replica, &g_avatar, true, entry(&g_replica, EntityRole::NpcCapture, 1004)),
             0u);
    CHECK_EQ(guest_attack_target_net_id(&g_other, &g_avatar, false, none), 0u);
    CHECK_EQ(guest_attack_target_net_id(&g_other, nullptr, false, none), 0u);  // pas d'avatar : nul ≠ cible
}

TEST("attaque 3.8 (D4 différé) : recul en attente 500 ms, annulé par un coup cosmétique ±500 ms") {
    const PendingStumble none{};
    CHECK(stumble_step(none, 5000, false, 0) == StumbleStep::Idle);
    const PendingStumble p{true, 1000, 1000};  // HitOnGuest à 1000
    CHECK(stumble_step(p, 1000, false, 0) == StumbleStep::Wait);
    CHECK(stumble_step(p, 1499, false, 0) == StumbleStep::Wait);
    CHECK(stumble_step(p, 1500, false, 0) == StumbleStep::Play);   // échéance, aucun coup cosmétique
    CHECK(stumble_step(p, 1200, true, 1150) == StumbleStep::Cancel);  // coup local 150 ms après
    CHECK(stumble_step(p, 1200, true, 600) == StumbleStep::Cancel);   // 400 ms avant
    CHECK(stumble_step(p, 1500, true, 500) == StumbleStep::Play);     // 500 ms avant : hors fenêtre
    CHECK(stumble_step(p, 1200, true, 100) == StumbleStep::Wait);     // ancien coup cosmétique
}

TEST("règles des dégâts 3c (3.8) : invité, coup sur l'avatar de l'hôte → bloqué ; effet sauf tir ami") {
    Inputs in = guest_replica_hits_hero();
    in.native_attacks = true;
    in.target = &g_avatar;
    in.target_found = true;
    in.target_entry = entry(&g_avatar, EntityRole::Avatar, 1);
    Decision d = decide(in);
    CHECK(blocked_silent(d, Reason::HostAvatarOnGuest) && d.impact && !d.cosmetic);
    in.aggressor = &g_hero;  // son héros : pas de tir ami, pas d'effet
    in.aggressor_found = false;
    d = decide(in);
    CHECK(blocked_silent(d, Reason::HostAvatarOnGuest) && !d.impact);
    in.hero = nullptr;  // héros connu du registre seulement
    in.aggressor_found = true;
    in.aggressor_entry = entry(&g_hero, EntityRole::LocalHero);
    d = decide(in);
    CHECK(blocked_silent(d, Reason::HostAvatarOnGuest) && !d.impact);
    in.guard = true;
    CHECK(passes(decide(in)));
}

TEST("règles des dégâts 3c (3.8) : native_attacks = 0 → règle absente (comportement 3.6 : laissé)") {
    Inputs in = guest_replica_hits_hero();
    in.target = &g_avatar;
    in.target_found = true;
    in.target_entry = entry(&g_avatar, EntityRole::Avatar, 1);
    CHECK(passes(decide(in)));
}

TEST("règles des dégâts 3b (3.8) : héros à terre → coup de réplique bloqué avec effet, pas cosmétique") {
    Inputs in = guest_replica_hits_hero();
    in.native_attacks = true;
    in.hero_down = true;
    const Decision d = decide(in);
    CHECK(!d.cosmetic && blocked_silent(d, Reason::GuestHeroByReplica) && d.impact);
}
