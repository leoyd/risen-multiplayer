// Règles du détour des dégâts (story 3.5) : un cas par règle (sync/hit_rules).
#include "sync/hit_rules.h"
#include "test.h"

#include <cmath>
#include <limits>

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
    CHECK(d.block);
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
    CHECK(d.block && d.queue_net_id == 0 && d.reason == Reason::GuestAvatar);
    Inputs in = host_avatar_hits();
    in.target = &g_hero;  // héros de l'hôte : pas de tir ami
    d = decide(in);
    CHECK(d.block && d.reason == Reason::GuestAvatar);
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

TEST("règles des dégâts 4b : hôte, PNJ capturé frappe l'avatar → bloqué, HitOnGuest source 1000") {
    const Decision d = decide(host_npc_hits_avatar());
    CHECK(d.block);
    CHECK(d.hit_on_guest);
    CHECK_EQ(d.source_net_id, 1000u);
    CHECK_EQ(d.queue_net_id, 0u);
    CHECK(d.reason == Reason::AvatarHit);
}

TEST("règles des dégâts 4b : hôte, agresseur hors registre frappe l'avatar → bloqué, HitOnGuest source 0") {
    Inputs in = host_npc_hits_avatar();
    in.aggressor = &g_other;
    in.aggressor_found = false;
    in.aggressor_entry = TrackedEntity{};
    const Decision d = decide(in);
    CHECK(d.block && d.hit_on_guest && d.reason == Reason::AvatarHit);
    CHECK_EQ(d.source_net_id, 0u);
}

TEST("règles des dégâts 4b : hôte, son héros frappe l'avatar → bloqué, rien (pas de tir ami)") {
    Inputs in = host_npc_hits_avatar();
    in.aggressor = &g_hero;
    in.aggressor_found = false;
    in.aggressor_entry = TrackedEntity{};
    CHECK(blocked_silent(decide(in), Reason::AvatarFriendly));
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

TEST("dégâts bruts : (10 + 2) × 1,5 = 18 ; négatif, NaN → 0 ; énorme → plafond") {
    CHECK_EQ(raw_damage(10, 2, 1.5f), 18u);
    CHECK_EQ(raw_damage(-10, 2, 1.0f), 0u);
    CHECK_EQ(raw_damage(10, 2, -1.0f), 0u);
    CHECK_EQ(raw_damage(0, 0, 1.0f), 0u);
    CHECK_EQ(raw_damage(10, 2, std::nanf("")), 0u);
    CHECK_EQ(raw_damage(10, 2, 1e12f), kMaxRawDamage);
    CHECK_EQ(raw_damage(10, 2, std::numeric_limits<float>::infinity()), 0u);
    CHECK_EQ(raw_damage(10, 0, 1.25f), 13u);  // 12,5 → arrondi au plus proche (13)
    CHECK_EQ(raw_damage(10, 0, 1.24f), 12u);
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
