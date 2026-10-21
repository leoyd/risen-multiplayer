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

enum class LogLevel { Info, Debug };

const char* log_level_name(LogLevel level);  // "info" / "debug"

inline constexpr std::uint16_t kDefaultPort = 27960;
inline constexpr std::size_t kMaxNicknameLength = 32;  // en octets (UTF-8)

struct Config {
    Role role = Role::Host;
    std::string host_ip;          // obligatoire en invité
    std::uint16_t port = kDefaultPort;
    std::string nickname = "Joueur";
    // Templates équipés sur le héros de l'invité (story 3.2b), forme canonique `A,B` (validée au
    // chargement dans les deux rôles, core/equipment.h) ; vide = aucun. Appliquée en invité
    // seulement (envoyée dans Hello) ; l'hôte ne s'en sert pas.
    std::string guest_equipment;
    bool probe = false;           // mode sonde du spike J1 (hooks + journaux de rétro-ingénierie)
    LogLevel log_level = LogLevel::Info;  // debug : lignes supplémentaires (par frame comprises)
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
// port (vide = défaut), nickname, guest_equipment (liste de templates, core/equipment.h), probe (0|1, vide = défaut 0),
// log_level (debug|info, insensible à la casse, vide = défaut info).
ConfigResult parse_config(std::string_view text);

}  // namespace mp
