// Vérification de la version du jeu (PRD FR-2). Logique pure, testable sous Linux.
//
// L'empreinte d'un module est lue dans son en-tête PE (TimeDateStamp + SizeOfImage).
// Une mise à jour Steam change le TimeDateStamp : il suffit alors d'ajouter une
// entrée à la liste blanche (version.cpp) après avoir vérifié les offsets utilisés.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace mp {

struct ModuleFingerprint {
    std::uint32_t time_date_stamp = 0;
    std::uint32_t size_of_image = 0;

    friend bool operator==(const ModuleFingerprint& a, const ModuleFingerprint& b) {
        return a.time_date_stamp == b.time_date_stamp && a.size_of_image == b.size_of_image;
    }
};

struct SupportedVersion {
    const char* label;
    ModuleFingerprint game;    // Game.dll
    ModuleFingerprint engine;  // Engine.dll
};

// Retourne l'entrée de la liste blanche correspondante, ou nullptr.
const SupportedVersion* find_supported_version(const ModuleFingerprint& game,
                                               const ModuleFingerprint& engine);

// Lit l'empreinte dans une image PE32+ (en mémoire ou sur disque) de `size` octets.
// Retourne false si l'en-tête est absent, tronqué ou n'est pas PE32+ x86-64.
bool read_pe_fingerprint(const std::uint8_t* image, std::size_t size, ModuleFingerprint& out);

// « TimeDateStamp=0x63c9aff6 SizeOfImage=0x1417000 »
std::string format_fingerprint(const ModuleFingerprint& fp);

}  // namespace mp
