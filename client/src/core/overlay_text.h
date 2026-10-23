// Texte de l'overlay à l'écran (rôle, pseudo, état de session). Logique pure, testable sous
// Linux ; le dessin est l'affaire de win/overlay.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "core/config.h"
#include "core/session.h"

namespace mp::core {

// Couleurs ARGB (D3DCOLOR) : hôte or, invité cyan.
inline constexpr std::uint32_t kOverlayHostColor = 0xFFFFC040;
inline constexpr std::uint32_t kOverlayGuestColor = 0xFF40D0FF;

std::uint32_t overlay_color(Role role);

// `HÔTE · <pseudo> · <état>` ou `INVITÉ · <pseudo> · <état>` (UTF-8). `host_ip` et `port` ne
// servent qu'à l'état Connecting (`connexion à <ip>:<port>…`).
std::string overlay_text(Role role, std::string_view nickname, session::State state, std::string_view host_ip,
                         std::uint16_t port);

}  // namespace mp::core
