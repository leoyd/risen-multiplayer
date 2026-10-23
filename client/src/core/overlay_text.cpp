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

}  // namespace mp::core
