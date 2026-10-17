#include "sync/anim_filter.h"
#include "test.h"

#include <thread>

using mp::sync::AnimFilter;
using mp::sync::Capture;
using mp::sync::kCaptureRing;
using Verdict = AnimFilter::Verdict;

namespace {

int g_ps_local, g_ps_replica, g_ps_other;  // adresses servant de PS opaques
void* const kLocal = &g_ps_local;
void* const kReplica = &g_ps_replica;
void* const kOther = &g_ps_other;

}  // namespace

TEST("filtre : PS local capturé, autres PS libres") {
    AnimFilter f;
    f.set_local(kLocal);
    CHECK(f.classify(kLocal, 1) == Verdict::Capture);
    CHECK(f.classify(kOther, 1) == Verdict::Pass);
    CHECK(f.classify(nullptr, 1) == Verdict::Pass);
    CHECK_EQ(f.take_blocked(), 0u);
}

TEST("filtre : le PS local n'est jamais bloqué, même déclaré réplique") {
    AnimFilter f;
    f.set_local(kLocal);
    f.add_replica(kLocal);
    CHECK(f.classify(kLocal, 1) == Verdict::Capture);
    CHECK_EQ(f.take_blocked(), 0u);
}

TEST("filtre : réplique bloquée sans garde, libre sous garde du même thread") {
    AnimFilter f;
    f.add_replica(kReplica);
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
    f.add_replica(kReplica);
    AnimFilter::ReplayGuard guard;  // garde du thread de test
    Verdict seen = Verdict::Pass;
    std::thread other([&] { seen = f.classify(kReplica, 2); });
    other.join();
    CHECK(seen == Verdict::Block);
    CHECK(f.classify(kReplica, 1) == Verdict::Pass);
}

TEST("filtre : retrait d'une réplique") {
    AnimFilter f;
    f.add_replica(kReplica);
    f.add_replica(kReplica);  // pas de doublon
    f.remove_replica(kReplica);
    CHECK(f.classify(kReplica, 1) == Verdict::Pass);
    f.add_replica(kReplica);
    f.clear_replicas();
    CHECK(f.classify(kReplica, 1) == Verdict::Pass);
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
