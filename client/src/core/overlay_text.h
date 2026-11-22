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

// Story 4.5 : alerte du champ de force (invité). Niveau 0 sous 80 % du rayon (distance horizontale à
// l'hôte, unités du monde), puis linéaire jusqu'à 1 au rayon (et au-delà) ; 0 si distance < 0 (inconnue)
// ou rayon ≤ 0. Texte : « Distance de l’hôte : 32 m / 35 m » (mètres arrondis), vide au niveau 0.
inline constexpr float kLeashWarningStart = 0.8f;
float leash_warning_level(float distance_cm, float radius_cm);
std::string leash_warning_text(float distance_cm, float radius_cm);

}  // namespace mp::core
