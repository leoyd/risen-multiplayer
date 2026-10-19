// Horloge distante et interpolation des snapshots (story 2.3, AD-7). Logique pure : ni horloge
// système ni socket. Le temps local est en ms (horloge monotone de l'appelant), le temps distant
// en ms dans la base de l'émetteur (tick × 50 ms).
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/pose.h"

namespace mp::core {

inline constexpr double kInterpolationDelayMs = 150.0;  // retard d'affichage
inline constexpr double kMaxExtrapolationMs = 250.0;    // au-delà du dernier snapshot
inline constexpr double kTickMs = 50.0;                 // 20 Hz
inline constexpr double kClockRiseRate = 0.01;          // remontée du décalage, par échantillon
inline constexpr double kClockMaxFallMs = 2.0;          // baisse du décalage, par échantillon

// Estime le décalage entre l'horloge locale et celle de l'émetteur. Échantillon :
// s = local − tick × 50. Le décalage suit le plus petit `s` vu (le moins de latence) : il
// descend vers lui de kClockMaxFallMs au plus par échantillon (un nouveau minimum ne fait pas
// bondir le temps affiché), et ne remonte que lentement (kClockRiseRate de l'écart par
// échantillon), pour suivre une latence qui augmente sans réagir à la gigue. Le premier
// échantillon fixe le décalage.
class RemoteClock {
public:
    void reset() { ready_ = false; offset_ = 0.0; }
    void observe(std::uint64_t local_ms, std::uint32_t tick);
    bool ready() const { return ready_; }
    // Temps distant correspondant à `local_ms` (sans retard d'affichage).
    double remote_ms(std::uint64_t local_ms) const { return static_cast<double>(local_ms) - offset_; }
    double offset_ms() const { return offset_; }

private:
    bool ready_ = false;
    double offset_ = 0.0;
};

enum class SampleMode {
    Empty,         // aucun échantillon
    Interpolated,  // entre deux snapshots
    Extrapolated,  // après le dernier, dans la limite de kMaxExtrapolationMs
    Held,          // avant le premier, un seul échantillon, ou extrapolation épuisée
};

struct Sample {
    Pose pose;
    SampleMode mode = SampleMode::Empty;
};

// Poses reçues d'une entité, triées par tick. Hermite (tangentes de Catmull-Rom) pour la
// position, chemin le plus court pour le lacet, extrapolation linéaire de la position au plus
// kMaxExtrapolationMs (le lacet reste celui du dernier snapshot).
class Interpolator {
public:
    static constexpr std::size_t kCapacity = 16;

    void reset() { points_.clear(); }
    // false : doublon (même tick) ou plus ancien que tout le tampon plein.
    bool push(std::uint32_t tick, const Pose& pose);
    bool empty() const { return points_.empty(); }
    std::size_t size() const { return points_.size(); }
    Sample sample(double remote_ms) const;

private:
    struct Point {
        std::uint32_t tick = 0;
        double t = 0.0;  // tick × 50 ms
        Pose pose;
    };
    std::vector<Point> points_;
};

// Angle ramené dans ]−π, π].
float wrap_angle(float a);

}  // namespace mp::core
