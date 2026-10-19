// Pose d'un héros : position et lacet (AD-7). Partagée par core/session et core/interpolation.
#pragma once

namespace mp::core {

struct Pose {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float yaw = 0.0f;  // radians, atan2(m[8], m[10])
};

}  // namespace mp::core
