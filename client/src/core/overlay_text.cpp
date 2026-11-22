#include "core/overlay_text.h"

namespace mp::core {

std::uint32_t overlay_color(Role role) { return role == Role::Host ? kOverlayHostColor : kOverlayGuestColor; }

std::string overlay_text(Role role, std::string_view nickname, session::State state, std::string_view host_ip,
                         std::uint16_t port) {
    std::string out = role == Role::Host ? "HÔTE" : "INVITÉ";
    out += " · ";
    out += nickname;
    out += " · ";
    switch (state) {
        case session::State::Inactive: out += "hors partie"; break;
        case session::State::Hosting: out += "en attente d’un invité"; break;
        case session::State::Connecting:
            out += "connexion à ";
            out += host_ip;
            out += ':';
            out += std::to_string(port);
            out += "…";
            break;
        case session::State::InSession: out += "en session"; break;
        case session::State::Ended: out += "session terminée"; break;
    }
    return out;
}

float leash_warning_level(float distance_cm, float radius_cm) {
    if (!(distance_cm >= 0.0f) || !(radius_cm > 0.0f)) return 0.0f;
    const float start = radius_cm * kLeashWarningStart;
    if (distance_cm <= start) return 0.0f;
    const float level = (distance_cm - start) / (radius_cm - start);
    return level >= 1.0f ? 1.0f : level;
}

std::string leash_warning_text(float distance_cm, float radius_cm) {
    if (leash_warning_level(distance_cm, radius_cm) <= 0.0f) return {};
    const auto meters = [](float cm) { return std::to_string(static_cast<unsigned>(cm / 100.0f + 0.5f)); };
    return "Distance de l’hôte : " + meters(distance_cm) + " m / " + meters(radius_cm) + " m";
}

}  // namespace mp::core
