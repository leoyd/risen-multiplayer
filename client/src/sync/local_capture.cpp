#include "sync/local_capture.h"

#include "game/motion_desc.h"

namespace mp::sync {

bool usable_motion_name(const std::string& name) {
    return !name.empty() && name.size() <= protocol::kMaxAniName && name[0] != '(';
}

std::optional<core::session::Input> local_input(const Capture& capture, const std::string& name) {
    if (capture.kind == CaptureKind::Ragdoll) return std::nullopt;
    if (capture.type < 0 || capture.type >= protocol::kMotionTypeCount || !usable_motion_name(name)) {
        return std::nullopt;
    }
    static_assert(sizeof capture.desc == game::kPlayDescSize, "Capture::desc = eSMotionPlayBackDesc");
    return core::session::LocalMotion{
        name, game::motion_from_call(capture.type, capture.flag, capture.desc_present ? &capture.desc : nullptr, 0)};
}

}  // namespace mp::sync
