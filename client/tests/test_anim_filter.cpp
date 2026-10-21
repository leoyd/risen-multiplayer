#include "sync/anim_filter.h"
#include "test.h"

#include <thread>

using mp::sync::AnimFilter;
using mp::sync::Capture;
using mp::sync::EntityRole;
using mp::sync::kCaptureRing;
using mp::sync::TrackedEntity;
using Verdict = AnimFilter::Verdict;

namespace {

int g_ps_local, g_ps_replica, g_ps_other;  // adresses servant de PS opaques
int g_entity_a, g_entity_b, g_wrapper_a, g_routine_a;
void* const kLocal = &g_ps_local;
void* const kReplica = &g_ps_replica;
void* const kOther = &g_ps_other;

// Suit `entity` comme avatar, avec ce PS d'animation (et éventuellement cette enveloppe).
void track_avatar(AnimFilter& f, void* animation_ps, void* entity = &g_entity_a, void* wrapper = nullptr,
                  EntityRole role = EntityRole::Avatar) {
    TrackedEntity e;
    e.entity = entity;
    e.animation_ps = animation_ps;
    e.wrapper = wrapper;
    e.role = role;
    f.track(e);
}

}  // namespace

TEST("filtre : PS local capturé, autres PS libres") {
    AnimFilter f;
    f.set_local(kLocal);
    CHECK(f.classify(kLocal, 1) == Verdict::Capture);
    CHECK(f.classify(kOther, 1) == Verdict::Pass);
    CHECK(f.classify(nullptr, 1) == Verdict::Pass);
    CHECK_EQ(f.take_blocked(), 0u);
}

TEST("filtre : le PS local n'est jamais bloqué, même suivi comme avatar") {
    AnimFilter f;
    f.set_local(kLocal);
    track_avatar(f, kLocal);
    CHECK(f.classify(kLocal, 1) == Verdict::Capture);
    CHECK_EQ(f.take_blocked(), 0u);
}

TEST("filtre : réplique bloquée sans garde, libre sous garde du même thread") {
    AnimFilter f;
    track_avatar(f, kReplica);
    CHECK(f.classify(kReplica, 1) == Verdict::Block);
    {
        AnimFilter::ReplayGuard guard;
        CHECK(AnimFilter::replaying());
        CHECK(f.classify(kReplica, 1) == Verdict::Pass);
    }
    CHECK(!AnimFilter::replaying());
    CHECK(f.classify(kReplica, 1) == Verdict::Block);
    CHECK_EQ(f.take_blocked(), 2u);
    CHECK_EQ(f.take_blocked(), 0u);  // compteur remis à zéro
}

TEST("filtre : la garde d'un autre thread ne laisse pas passer") {
    AnimFilter f;
    track_avatar(f, kReplica);
    AnimFilter::ReplayGuard guard;  // garde du thread de test
    Verdict seen = Verdict::Pass;
    std::thread other([&] { seen = f.classify(kReplica, 2); });
    other.join();
    CHECK(seen == Verdict::Block);
    CHECK(f.classify(kReplica, 1) == Verdict::Pass);
}

TEST("registre : retrait d'une entité, puis vidage") {
    AnimFilter f;
    track_avatar(f, kReplica);
    track_avatar(f, kReplica);  // pas de doublon
    CHECK_EQ(f.tracked_count(), static_cast<std::size_t>(1));
    f.untrack(&g_entity_a);
    CHECK(f.classify(kReplica, 1) == Verdict::Pass);
    CHECK_EQ(f.tracked_count(), static_cast<std::size_t>(0));
    track_avatar(f, kReplica);
    f.clear();
    CHECK(f.classify(kReplica, 1) == Verdict::Pass);
    CHECK_EQ(f.take_blocked(), 0u);
}

TEST("registre : une entité suivie deux fois garde une seule entrée, la dernière") {
    AnimFilter f;
    track_avatar(f, kReplica, &g_entity_a, &g_wrapper_a);
    track_avatar(f, kOther, &g_entity_a);  // même entité, autre PS : remplacement
    CHECK_EQ(f.tracked_count(), static_cast<std::size_t>(1));
    CHECK(f.classify(kReplica, 1) == Verdict::Pass);
    CHECK(f.classify(&g_wrapper_a, 1) == Verdict::Pass);
    CHECK(f.classify(kOther, 1) == Verdict::Block);
    f.track(TrackedEntity{});  // entité nulle : ignorée
    CHECK_EQ(f.tracked_count(), static_cast<std::size_t>(1));
}

TEST("registre : untrack retire le PS et l'enveloppe, sans toucher aux autres entités") {
    AnimFilter f;
    track_avatar(f, kReplica, &g_entity_a, &g_wrapper_a);
    track_avatar(f, kOther, &g_entity_b, nullptr, EntityRole::NpcReplica);
    CHECK(f.classify(kReplica, 1) == Verdict::Block);
    CHECK(f.classify(&g_wrapper_a, 1) == Verdict::Block);
    f.untrack(&g_entity_a);
    CHECK(f.classify(kReplica, 1) == Verdict::Pass);
    CHECK(f.classify(&g_wrapper_a, 1) == Verdict::Pass);
    CHECK(f.classify(kOther, 1) == Verdict::Block);  // réplique de PNJ toujours suivie
    f.untrack(nullptr);
    f.untrack(&g_entity_a);  // déjà retirée : sans effet
    CHECK_EQ(f.tracked_count(), static_cast<std::size_t>(1));
    CHECK_EQ(f.take_blocked(), 3u);
}

TEST("registre : recherches par PS d'animation et par enveloppe, avec le rôle") {
    AnimFilter f;
    TrackedEntity a;
    a.entity = &g_entity_a;
    a.animation_ps = kReplica;
    a.routine_ps = &g_routine_a;
    a.wrapper = &g_wrapper_a;
    a.role = EntityRole::Avatar;
    f.track(a);
    track_avatar(f, kOther, &g_entity_b, nullptr, EntityRole::NpcReplica);
    TrackedEntity out;
    CHECK(f.find_by_animation_ps(kReplica, out));
    CHECK(out.entity == &g_entity_a && out.role == EntityRole::Avatar);
    CHECK(out.routine_ps == &g_routine_a && out.wrapper == &g_wrapper_a);
    CHECK(f.find_by_wrapper(&g_wrapper_a, out));
    CHECK(out.entity == &g_entity_a);
    CHECK(f.find_by_animation_ps(kOther, out));
    CHECK(out.entity == &g_entity_b && out.role == EntityRole::NpcReplica);
    CHECK(!f.find_by_animation_ps(&g_wrapper_a, out));  // une enveloppe n'est pas un PS
    CHECK(!f.find_by_wrapper(kReplica, out));
    CHECK(!f.find_by_animation_ps(nullptr, out));
    CHECK(!f.find_by_wrapper(nullptr, out));  // l'enveloppe nulle de b ne correspond pas
    CHECK_EQ(f.take_blocked(), 0u);  // les recherches ne comptent rien
}

TEST("registre : une entrée de rôle héros local n'est jamais bloquée") {
    AnimFilter f;
    track_avatar(f, kReplica, &g_entity_a, &g_wrapper_a, EntityRole::LocalHero);
    CHECK(f.classify(kReplica, 1) == Verdict::Pass);
    CHECK(f.classify(&g_wrapper_a, 1) == Verdict::Pass);
    CHECK(f.classify_ragdoll(kReplica, true, 1) == Verdict::Pass);
    CHECK_EQ(f.take_blocked(), 0u);
}

TEST("filtre : anneau plein, 64 captures restituées dans l'ordre et 1 perte") {
    AnimFilter f;
    for (int i = 0; i < static_cast<int>(kCaptureRing) + 1; ++i) {
        Capture c;
        c.type = i % 8;
        c.desc[0] = static_cast<std::uint8_t>(i);
        f.push(c);
    }
    Capture out[kCaptureRing + 8];
    unsigned dropped = 0;
    const std::size_t n = f.drain(out, kCaptureRing + 8, dropped);
    CHECK_EQ(n, kCaptureRing);
    CHECK_EQ(dropped, 1u);
    for (std::size_t i = 0; i < n; ++i) CHECK_EQ(out[i].desc[0], static_cast<std::uint8_t>(i));
    CHECK_EQ(f.drain(out, kCaptureRing, dropped), static_cast<std::size_t>(0));
    CHECK_EQ(dropped, 0u);
}

TEST("filtre : vidage partiel puis reprise") {
    AnimFilter f;
    for (int i = 0; i < 10; ++i) {
        Capture c;
        c.desc[0] = static_cast<std::uint8_t>(i);
        f.push(c);
    }
    Capture out[4];
    unsigned dropped = 0;
    CHECK_EQ(f.drain(out, 4, dropped), static_cast<std::size_t>(4));
    CHECK_EQ(out[3].desc[0], 3);
    Capture c;
    c.desc[0] = 10;
    f.push(c);
    Capture rest[16];
    CHECK_EQ(f.drain(rest, 16, dropped), static_cast<std::size_t>(7));
    CHECK_EQ(rest[0].desc[0], 4);
    CHECK_EQ(rest[6].desc[0], 10);
}

TEST("filtre : PlayMotion et ragdoll restitués dans l'ordre") {
    AnimFilter f;
    Capture a;
    a.type = 0;
    f.push(a);
    Capture r;
    r.kind = mp::sync::CaptureKind::Ragdoll;
    r.ragdoll_on = true;
    r.has_vector = false;  // vecteur nul
    f.push(r);
    Capture v;
    v.kind = mp::sync::CaptureKind::Ragdoll;
    v.ragdoll_flag = true;
    v.has_vector = true;
    v.vector[0] = 1.5f;
    v.vector[1] = -2.0f;
    v.vector[2] = 300.0f;
    f.push(v);
    Capture b;
    b.type = 1;
    f.push(b);
    Capture out[5];
    unsigned dropped = 0;
    CHECK_EQ(f.drain(out, 5, dropped), static_cast<std::size_t>(4));
    CHECK(out[0].kind == mp::sync::CaptureKind::Motion && out[0].type == 0);
    CHECK(out[1].kind == mp::sync::CaptureKind::Ragdoll && out[1].ragdoll_on && !out[1].has_vector);
    CHECK(out[1].type == 0 && !out[1].desc_present && out[1].resource == nullptr);  // champs PlayMotion neutres
    CHECK(out[2].kind == mp::sync::CaptureKind::Ragdoll && !out[2].ragdoll_on && out[2].ragdoll_flag);
    CHECK(out[2].has_vector && out[2].vector[0] == 1.5f && out[2].vector[1] == -2.0f && out[2].vector[2] == 300.0f);
    CHECK(out[3].kind == mp::sync::CaptureKind::Motion && out[3].type == 1);
}

TEST("filtre : thread du hook retenu au premier appel, changement signalé") {
    AnimFilter f;
    CHECK_EQ(f.hook_thread(), 0u);
    f.classify(kOther, 42);
    f.classify(nullptr, 42);
    CHECK_EQ(f.hook_thread(), 42u);
    CHECK(!f.hook_thread_varied());
    f.classify(kOther, 43);
    CHECK_EQ(f.hook_thread(), 42u);
    CHECK(f.hook_thread_varied());
}

TEST("anim_filter : is_local ne compte rien") {
    AnimFilter f;
    int local = 0, replica = 0;
    f.set_local(&local);
    track_avatar(f, &replica);
    CHECK(f.is_local(&local));
    CHECK(!f.is_local(&replica));
    CHECK(!f.is_local(nullptr));
    CHECK_EQ(f.take_blocked(), 0u);
}

TEST("anim_filter : classify_ragdoll — désactivation d'un avatar passe sans compter, activation bloquée sauf rejeu") {
    AnimFilter f;
    int local = 0, replica = 0, other = 0;
    f.set_local(&local);
    track_avatar(f, &replica);
    CHECK(f.classify_ragdoll(&replica, false, 1) == Verdict::Pass);  // nettoyage par le moteur
    CHECK_EQ(f.take_blocked(), 0u);
    CHECK(f.classify_ragdoll(&replica, true, 1) == Verdict::Block);
    CHECK_EQ(f.take_blocked(), 1u);
    {
        AnimFilter::ReplayGuard guard;  // avatar::die
        CHECK(f.classify_ragdoll(&replica, true, 1) == Verdict::Pass);
    }
    CHECK(f.classify_ragdoll(&local, true, 1) == Verdict::Capture);
    CHECK(f.classify_ragdoll(&local, false, 1) == Verdict::Capture);  // capturé aussi (fin du ragdoll)
    CHECK(f.classify_ragdoll(&other, true, 1) == Verdict::Pass);
    CHECK(f.classify_ragdoll(&other, false, 1) == Verdict::Pass);
    CHECK_EQ(f.take_blocked(), 0u);
}

TEST("registre : blocks_routine — seul le PS de routine d'une réplique de PNJ est figé (story 3.3)") {
    AnimFilter f;
    int routine_b = 0;
    CHECK(!f.blocks_routine(&g_routine_a));  // registre vide
    CHECK(!f.blocks_routine(nullptr));
    // Un avatar qui aurait un PS de routine n'est pas figé par ce hook.
    TrackedEntity avatar;
    avatar.entity = &g_entity_b;
    avatar.routine_ps = &routine_b;
    avatar.role = EntityRole::Avatar;
    f.track(avatar);
    CHECK(!f.blocks_routine(&routine_b));
    TrackedEntity npc;
    npc.entity = &g_entity_a;
    npc.routine_ps = &g_routine_a;
    npc.role = EntityRole::NpcReplica;
    f.track(npc);
    CHECK(f.blocks_routine(&g_routine_a));
    CHECK(f.blocks_routine(&g_routine_a));
    CHECK(!f.blocks_routine(&g_ps_other));
    CHECK_EQ(f.take_routine_blocked(), 2u);
    CHECK_EQ(f.take_routine_blocked(), 0u);
    // Sans PS d'animation, la réplique ne bloque aucun PlayMotion (animations : story 3.4).
    CHECK(f.classify(kOther, 1) == Verdict::Pass);
    CHECK_EQ(f.take_blocked(), 0u);
    // Libérée : IA rendue.
    f.untrack(&g_entity_a);
    CHECK(!f.blocks_routine(&g_routine_a));
    f.track(npc);
    f.clear();
    CHECK(!f.blocks_routine(&g_routine_a));
}
