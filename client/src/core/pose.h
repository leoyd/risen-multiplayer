// Pose d'un héros : position et lacet (AD-7). Partagée par core/session, core/interpolation et
// sync/avatar (pose_matrix).
#pragma once

#include <cmath>

namespace mp::core {

struct Pose {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float yaw = 0.0f;  // radians, atan2(m[8], m[10])
};

// bCMatrix (4×4 f32, en lignes) d'une pose : ligne 0 = droite (cos ψ, 0, −sin ψ), ligne 1 =
// haut (Y), ligne 2 = avant (sin ψ, 0, cos ψ), translation m[12..14]. atan2(m[8], m[10]) = ψ.
inline void pose_matrix(const Pose& p, float (&m)[16]) {
    const float c = std::cos(p.yaw), s = std::sin(p.yaw);
    for (float& v : m) v = 0.0f;
    m[0] = c;
    m[2] = -s;
    m[5] = 1.0f;
    m[8] = s;
    m[10] = c;
    m[12] = p.x;
    m[13] = p.y;
    m[14] = p.z;
    m[15] = 1.0f;
}

}  // namespace mp::core
