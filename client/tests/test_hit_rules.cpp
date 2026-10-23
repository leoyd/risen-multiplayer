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

TEST("règles des dégâts 5 : sinon laissé (héros de l'hôte, coups reçus par le héros invité ou l'avatar)") {
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
    // Hôte : un PNJ frappe l'avatar invité (3.6).
    in.aggressor = &g_other;
    in.target = &g_avatar;
    in.target_entry = entry(&g_avatar, EntityRole::Avatar, 2);
    CHECK(passes(decide(in)));
    // Invité : une réplique frappe son héros (3.6) ; rôle « avatar » chez l'invité = hôte, pas de règle 4.
    in = Inputs{};
    in.in_session = true;
    in.role = HitRole::Guest;
    in.hero = &g_hero;
    in.aggressor = &g_replica;
    in.aggressor_found = true;
    in.aggressor_entry = entry(&g_replica, EntityRole::NpcReplica, 1000);
    in.target = &g_hero;
    CHECK(passes(decide(in)));
    in.aggressor = &g_avatar;
    in.aggressor_entry = entry(&g_avatar, EntityRole::Avatar, 1);
    CHECK(passes(decide(in)));
}
