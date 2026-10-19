// Horloge distante et interpolateur (story 2.3) : matrice I/O de la spec, fluidité sous
// latence, gigue et pertes.
#include "core/interpolation.h"
#include "test.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace mp::core;

namespace {

constexpr double kPi = 3.14159265358979323846;

bool near(double a, double b, double eps = 1e-3) { return std::fabs(a - b) <= eps; }

Pose at_x(double x, float yaw = 0.0f) { return Pose{static_cast<float>(x), 0.0f, 0.0f, yaw}; }

// Générateur reproductible (xorshift) : pertes et gigue des tests de fluidité.
struct Rng {
    std::uint64_t s = 0x9e3779b97f4a7c15ull;
    double next() {  // [0, 1)
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return static_cast<double>(s >> 11) / 9007199254740992.0;
    }
};

using Trajectory = Pose (*)(double t_ms);

Pose circle(double t_ms) {  // rayon 300 cm, 300 cm/s
    const double a = t_ms / 1000.0;
    return Pose{static_cast<float>(300.0 * std::sin(a)), 0.0f, static_cast<float>(300.0 - 300.0 * std::cos(a)),
                static_cast<float>(std::remainder(a, 2 * kPi))};
}

Pose back_and_forth(double t_ms) {  // 0 → 600 cm → 0 à 300 cm/s, demi-tour sec
    const double d = std::fmod(t_ms / 1000.0 * 300.0, 1200.0);
    const bool going = d < 600.0;
    return Pose{static_cast<float>(going ? d : 1200.0 - d), 0.0f, 0.0f, going ? 0.0f : static_cast<float>(kPi)};
}

struct Smoothness {
    double max_step = 0.0;   // plus grand déplacement entre deux frames (cm)
    double max_error = 0.0;  // plus grand écart à la trajectoire au temps affiché (cm)
    double min_x = 1e9, max_x = -1e9;
    int extrapolated = 0, frames = 0;
};

// L'émetteur tourne à 5 ms par frame et envoie à la première frame de chaque tick ; le réseau
// ajoute latence ± gigue et perd `loss` des datagrammes ; le récepteur affiche toutes les 6 ms.
Smoothness simulate(Trajectory traj, double latency, double jitter, double loss, double duration_ms) {
    struct Packet {
        double arrival;
        std::uint32_t tick;
        Pose pose;
    };
    constexpr double kClockSkew = 12345.0;  // horloge du récepteur en avance sur celle de l'émetteur
    Rng rng;
    std::vector<Packet> packets;
    std::int64_t last_tick = -1;
    for (double t = 0; t < duration_ms; t += 5.0) {
        const auto tick = static_cast<std::int64_t>(t / 50.0);
        if (tick == last_tick) continue;
        last_tick = tick;
        if (rng.next() < loss) continue;
        const double delay = latency + (rng.next() * 2.0 - 1.0) * jitter;
        packets.push_back(Packet{t + delay + kClockSkew, static_cast<std::uint32_t>(tick), traj(t)});
    }
    RemoteClock clock;
    Interpolator interp;
    Smoothness r;
    bool have = false;
    Pose prev;
    for (double now = kClockSkew; now < duration_ms + kClockSkew; now += 6.0) {
        for (const Packet& p : packets) {
            if (p.arrival <= now && p.arrival > now - 6.0) {
                clock.observe(static_cast<std::uint64_t>(p.arrival), p.tick);
                interp.push(p.tick, p.pose);
            }
        }
        if (!clock.ready() || interp.empty()) continue;
        const double shown = clock.remote_ms(static_cast<std::uint64_t>(now)) - kInterpolationDelayMs;
        const Sample s = interp.sample(shown);
        if (now - kClockSkew < 1000.0) {  // mise en route
            prev = s.pose;
            have = true;
            continue;
        }
        ++r.frames;
        if (s.mode == SampleMode::Extrapolated) ++r.extrapolated;
        if (have) {
            const double step = std::hypot(s.pose.x - prev.x, s.pose.z - prev.z);
            if (step > r.max_step) r.max_step = step;
        }
        const Pose truth = traj(shown);
        const double err = std::hypot(s.pose.x - truth.x, s.pose.z - truth.z);
        if (err > r.max_error) r.max_error = err;
        if (s.pose.x < r.min_x) r.min_x = s.pose.x;
        if (s.pose.x > r.max_x) r.max_x = s.pose.x;
        prev = s.pose;
        have = true;
    }
    return r;
}

}  // namespace

// --- Horloge distante --------------------------------------------------------------------

TEST("horloge : latence constante → décalage = avance + latence") {
    RemoteClock c;
    CHECK(!c.ready());
    for (std::uint32_t tick = 0; tick < 40; ++tick) c.observe(10000 + tick * 50 + 80, tick);
    CHECK(c.ready());
    CHECK(near(c.offset_ms(), 10080.0));
    CHECK(near(c.remote_ms(10080 + 500), 500.0));
}

TEST("horloge : un nouveau minimum fait descendre le décalage de 2 ms au plus par échantillon") {
    RemoteClock c;
    c.observe(1000 + 130, 0);  // premier échantillon : 30 ms de gigue
    c.observe(1000 + 50 + 100, 1);  // minimum (1100) : 1130 → 1128 seulement
    CHECK(near(c.offset_ms(), 1128.0));
    for (std::uint32_t tick = 2; tick < 30; ++tick) c.observe(1000 + tick * 50 + 100, tick);
    CHECK(near(c.offset_ms(), 1100.0));  // atteint en 15 échantillons, sans le dépasser
}

TEST("horloge : 20 000 échantillons de gigue ± 20 ms → décalage borné, pas de bond") {
    RemoteClock c;
    std::uint64_t r = 0x9e3779b97f4a7c15ull;
    double prev = 0.0, max_step = 0.0, lo = 1e18, hi = -1e18;
    for (std::uint32_t tick = 0; tick < 20000; ++tick) {
        r ^= r << 13;
        r ^= r >> 7;
        r ^= r << 17;
        const auto jitter = static_cast<std::uint64_t>(r % 41);  // 0 à 40 ms au-dessus du minimum
        c.observe(5000 + static_cast<std::uint64_t>(tick) * 50 + 100 + jitter, tick);
        if (tick > 100) {
            max_step = std::max(max_step, std::fabs(c.offset_ms() - prev));
            lo = std::min(lo, c.offset_ms());
            hi = std::max(hi, c.offset_ms());
        }
        prev = c.offset_ms();
    }
    CHECK(max_step <= 2.0 + 1e-9);            // ni bond en avant, ni bond en arrière
    CHECK(lo >= 5100.0 && hi < 5100.0 + 12.0);  // reste près de la latence minimale
}

TEST("horloge : une latence qui augmente est suivie lentement, puis reset") {
    RemoteClock c;
    c.observe(1000, 0);  // latence 0
    for (std::uint32_t tick = 1; tick <= 100; ++tick) c.observe(1000 + tick * 50 + 200, tick);  // +200 ms
    const double after_100 = c.offset_ms() - 1000.0;
    CHECK(after_100 > 100.0 && after_100 < 200.0);  // 1 % par échantillon : ≈ 127 ms
    c.reset();
    CHECK(!c.ready());
}

// --- Interpolateur : matrice de la spec --------------------------------------------------

TEST("interpolation : vide, un seul échantillon, avant le premier") {
    Interpolator in;
    CHECK(in.sample(0).mode == SampleMode::Empty);
    in.push(10, at_x(5));
    Sample s = in.sample(100);
    CHECK(s.mode == SampleMode::Held && s.pose.x == 5.0f);
    in.push(11, at_x(10));
    s = in.sample(0);  // avant le premier (tick 10 = 500 ms)
    CHECK(s.mode == SampleMode::Held && s.pose.x == 5.0f);
}

TEST("interpolation : mouvement linéaire → pose exacte, perte isolée sans effet") {
    Interpolator in;
    for (std::uint32_t tick = 0; tick < 10; ++tick) {
        if (tick == 5) continue;  // perte isolée
        in.push(tick, at_x(tick * 15.0));  // 300 cm/s
    }
    for (double t = 60.0; t <= 400.0; t += 7.0) {
        const Sample s = in.sample(t);
        CHECK(s.mode == SampleMode::Interpolated);
        CHECK(near(s.pose.x, t * 0.3, 1e-2));
    }
}

TEST("interpolation : gigue → ordre rétabli par tick, doublon refusé") {
    Interpolator in;
    const std::uint32_t order[] = {0, 2, 1, 4, 3, 5};
    for (std::uint32_t tick : order) CHECK(in.push(tick, at_x(tick * 15.0)));
    CHECK(!in.push(3, at_x(999)));  // doublon
    CHECK_EQ(in.size(), 6u);
    CHECK(near(in.sample(125.0).pose.x, 37.5, 1e-2));
}

TEST("interpolation : flux coupé → extrapolation de 250 ms au plus, puis figée") {
    Interpolator in;
    for (std::uint32_t tick = 0; tick < 4; ++tick) in.push(tick, at_x(tick * 15.0, 0.5f));  // dernier : 150 ms
    Sample s = in.sample(150.0 + 100.0);
    CHECK(s.mode == SampleMode::Extrapolated);
    CHECK(near(s.pose.x, 45.0 + 30.0, 1e-2));
    CHECK(s.pose.yaw == 0.5f);  // le lacet n'est pas extrapolé
    s = in.sample(150.0 + 1000.0);
    CHECK(s.mode == SampleMode::Held);
    CHECK(near(s.pose.x, 45.0 + 75.0, 1e-2));  // 250 ms à 0,3 cm/ms
}

TEST("interpolation : lacet de +3,1 à −3,1 → passe par π, pas par 0") {
    Interpolator in;
    in.push(0, at_x(0, 3.1f));
    in.push(1, at_x(0, -3.1f));
    const float mid = in.sample(25.0).pose.yaw;
    CHECK(std::fabs(std::fabs(mid) - 3.14159f) < 1e-3f);
    const float quarter = in.sample(12.5).pose.yaw;
    CHECK(quarter > 3.1f && quarter <= 3.1416f);
}

TEST("interpolation : tampon de 16, le plus ancien est jeté ; reset") {
    Interpolator in;
    for (std::uint32_t tick = 0; tick < 20; ++tick) in.push(tick, at_x(tick));
    CHECK_EQ(in.size(), Interpolator::kCapacity);
    CHECK(!in.push(1, at_x(0)));  // plus ancien que tout le tampon plein
    CHECK(in.sample(0).pose.x == 4.0f);
    in.reset();
    CHECK(in.empty());
}

TEST("wrap_angle : ]−π, π]") {
    CHECK(near(wrap_angle(static_cast<float>(3 * kPi)), kPi, 1e-5));
    CHECK(near(wrap_angle(static_cast<float>(-kPi)), kPi, 1e-5));
    CHECK(near(wrap_angle(7.0f), 7.0 - 2 * kPi, 1e-5));
}

// --- Fluidité ----------------------------------------------------------------------------

TEST("fluidité : cercle à 300 cm/s, latence 100 ms ± 20 ms, 10 % de pertes") {
    const Smoothness r = simulate(&circle, 100.0, 20.0, 0.10, 20000.0);
    const double nominal = 300.0 * 0.006;  // pas nominal : 1,8 cm par frame de 6 ms
    CHECK(r.frames > 2500);
    CHECK(r.max_step < 3 * nominal + 2.0);
    CHECK(r.max_error < 15.0);
    CHECK(r.extrapolated < r.frames / 10);  // le retard de 150 ms absorbe la gigue et les pertes isolées
}

TEST("fluidité : sans perte ni gigue, l'écart reste sous 2 cm") {
    const Smoothness r = simulate(&circle, 100.0, 0.0, 0.0, 10000.0);
    CHECK(r.max_error < 2.0);
    CHECK_EQ(r.extrapolated, 0);
}

TEST("fluidité : allers-retours avec demi-tours secs → ni saut ni dépassement") {
    const Smoothness r = simulate(&back_and_forth, 100.0, 20.0, 0.10, 20000.0);
    CHECK(r.max_step < 3 * 300.0 * 0.006 + 2.0);
    CHECK(r.min_x > -8.0 && r.max_x < 608.0);  // Catmull-Rom : léger dépassement au plus
}
