#include "core/interpolation.h"

#include <algorithm>
#include <cmath>

#include "core/protocol.h"

static_assert(mp::core::kTickMs == static_cast<double>(mp::protocol::kSnapshotTickMs),
              "émetteur (protocol) et récepteur (interpolation) doivent partager la période du tick");

namespace mp::core {

namespace {

constexpr float kPi = 3.14159265358979323846f;

struct Vec {
    double x = 0.0, y = 0.0, z = 0.0;
};

Vec position(const Pose& p) { return {p.x, p.y, p.z}; }

}  // namespace

float wrap_angle(float a) {
    a = std::fmod(a, 2.0f * kPi);
    if (a > kPi) a -= 2.0f * kPi;
    if (a <= -kPi) a += 2.0f * kPi;
    return a;
}

void RemoteClock::observe(std::uint64_t local_ms, std::uint32_t tick) {
    const double s = static_cast<double>(local_ms) - static_cast<double>(tick) * kTickMs;
    if (!ready_) {
        offset_ = s;
        ready_ = true;
        return;
    }
    if (s < offset_) {
        offset_ = std::max(s, offset_ - kClockMaxFallMs);
        return;
    }
    offset_ += (s - offset_) * kClockRiseRate;
}

bool Interpolator::push(std::uint32_t tick, const Pose& pose) {
    auto it = std::lower_bound(points_.begin(), points_.end(), tick,
                               [](const Point& p, std::uint32_t t) { return p.tick < t; });
    if (it != points_.end() && it->tick == tick) return false;
    if (it == points_.begin() && points_.size() >= kCapacity) return false;
    points_.insert(it, Point{tick, static_cast<double>(tick) * kTickMs, pose});
    if (points_.size() > kCapacity) points_.erase(points_.begin());
    return true;
}

Sample Interpolator::sample(double t) const {
    Sample out;
    const std::size_t n = points_.size();
    if (n == 0) return out;
    if (n == 1 || t <= points_.front().t) {
        out.pose = (t <= points_.front().t ? points_.front() : points_.back()).pose;
        out.mode = SampleMode::Held;
        return out;
    }
    const Point& last = points_[n - 1];
    if (t >= last.t) {
        const Point& prev = points_[n - 2];
        const double dt = last.t - prev.t;
        const double e = std::min(t - last.t, kMaxExtrapolationMs);
        out.pose = last.pose;
        out.pose.x = static_cast<float>(last.pose.x + (last.pose.x - prev.pose.x) / dt * e);
        out.pose.y = static_cast<float>(last.pose.y + (last.pose.y - prev.pose.y) / dt * e);
        out.pose.z = static_cast<float>(last.pose.z + (last.pose.z - prev.pose.z) / dt * e);
        out.mode = t - last.t <= kMaxExtrapolationMs ? SampleMode::Extrapolated : SampleMode::Held;
        return out;
    }
    // Segment [i, i + 1] contenant t.
    std::size_t i = 0;
    while (i + 2 < n && points_[i + 1].t <= t) ++i;
    const Point& a = points_[i];
    const Point& b = points_[i + 1];
    const double h = b.t - a.t;
    const double u = (t - a.t) / h;
    // Tangentes de Catmull-Rom (différences centrées ; décentrées aux bouts du tampon).
    auto tangent = [&](std::size_t k) {
        const Point& lo = points_[k == 0 ? 0 : k - 1];
        const Point& hi = points_[k + 1 < n ? k + 1 : k];
        const double span = hi.t - lo.t;
        const Vec pl = position(lo.pose), ph = position(hi.pose);
        return Vec{(ph.x - pl.x) / span, (ph.y - pl.y) / span, (ph.z - pl.z) / span};
    };
    const Vec pa = position(a.pose), pb = position(b.pose);
    const Vec ma = tangent(i), mb = tangent(i + 1);
    const double u2 = u * u, u3 = u2 * u;
    const double h00 = 2 * u3 - 3 * u2 + 1, h10 = u3 - 2 * u2 + u, h01 = -2 * u3 + 3 * u2, h11 = u3 - u2;
    out.pose.x = static_cast<float>(h00 * pa.x + h10 * h * ma.x + h01 * pb.x + h11 * h * mb.x);
    out.pose.y = static_cast<float>(h00 * pa.y + h10 * h * ma.y + h01 * pb.y + h11 * h * mb.y);
    out.pose.z = static_cast<float>(h00 * pa.z + h10 * h * ma.z + h01 * pb.z + h11 * h * mb.z);
    out.pose.yaw = wrap_angle(a.pose.yaw + wrap_angle(b.pose.yaw - a.pose.yaw) * static_cast<float>(u));
    out.mode = SampleMode::Interpolated;
    return out;
}

}  // namespace mp::core
