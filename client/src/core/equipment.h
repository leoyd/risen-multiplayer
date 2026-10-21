// Équipement de l'invité (story 3.2b) : format de `guest_equipment` et du champ `équipement` du
// Hello (protocol/PROTOCOL.md §4 et §8). Logique pure, testée sous Linux.
//
// Format : noms de templates séparés par des virgules, espaces tolérés autour de chaque nom.
// Un nom compte 1 à 64 octets [A-Za-z0-9_] ; 16 noms au plus, sans doublon ; la forme canonique
// (`A,B`, sans espaces) tient en 255 octets au plus. Une valeur vide (ou faite d'espaces) = aucun
// équipement.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace mp::core {

inline constexpr std::size_t kMaxEquipmentNames = 16;
inline constexpr std::size_t kMaxEquipmentNameLength = 64;   // octets
inline constexpr std::size_t kMaxEquipmentCanonical = 255;   // octets (= Hello.equipment)

// Analyse `text`. True : `names` reçoit les noms dans l'ordre (vide si aucun équipement), `error`
// est vidé. False : `names` est vidé et `error` explique la raison (UTF-8).
bool parse_equipment(std::string_view text, std::vector<std::string>& names, std::string& error);

// Forme canonique : noms joints par des virgules, sans espaces.
std::string join_equipment(const std::vector<std::string>& names);

}  // namespace mp::core
