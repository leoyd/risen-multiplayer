// Lecture de multiplayer.ini (PRD FR-3). Logique pure, sans dépendance Windows :
// testable sous Linux sans le jeu.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mp {

enum class Role { Host, Guest };

const char* role_name(Role role);  // "hôte" / "invité" (UTF-8)

inline constexpr std::uint16_t kDefaultPort = 27960;
inline constexpr std::size_t kMaxNicknameLength = 32;  // en octets (UTF-8)

struct Config {
    Role role = Role::Host;
    std::string host_ip;          // obligatoire en invité
    std::uint16_t port = kDefaultPort;
    std::string nickname = "Joueur";
    std::string guest_equipment;  // texte libre, interprété en J1
    bool probe = false;           // mode sonde du spike J1 (hooks + journaux de rétro-ingénierie)
};

struct ConfigResult {
    bool ok = false;
    Config config;
    // Renseignés si !ok : champ fautif (ou « ligne N »), valeur lue, explication.
    std::string error_field;
    std::string error_value;
    std::string error_message;
    // Avertissements non bloquants (champ inconnu, doublon…).
    std::vector<std::string> warnings;
};

// Format : lignes `cle = valeur`, commentaires `;` ou `#` sur leur propre ligne
// uniquement (pas de commentaire en fin de ligne), sections `[...]` ignorées,
// BOM UTF-8 et fins de ligne CRLF tolérés, clés insensibles à la casse.
// Champs : role (host|guest, obligatoire), host_ip (validé en invité seulement),
// port (vide = défaut), nickname, guest_equipment, probe (0|1, vide = défaut 0).
ConfigResult parse_config(std::string_view text);

}  // namespace mp
