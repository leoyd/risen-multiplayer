// ABI du jeu (MSVC x64), résolue par GetProcAddress — aucun .lib du jeu n'est lié.
//
// Convention MSVC x64 : une méthode non virtuelle reçoit `this` dans RCX, puis les
// arguments dans RDX, R8, R9. Un pointeur de fonction libre dont le premier
// paramètre est `this` est donc strictement équivalent.
#pragma once

#include <cstdint>

namespace mp::abi {

// bCString (SharedBase.dll) : un seul pointeur vers des données gérées par le jeu.
// Ne jamais copier/assigner ces objets côté mod : passer par les fonctions du jeu.
struct bCString {
    void* data;
};
static_assert(sizeof(bCString) == 8, "bCString doit faire 8 octets (x64)");

// ??0bCString@@QEAA@PEBD@Z — bCString::bCString(char const*)
using BCStringCtorFromCStr = bCString* (*)(bCString* self, const char* text);
// ??1bCString@@QEAA@XZ — bCString::~bCString()
using BCStringDtor = void (*)(bCString* self);

struct GameAbi {
    BCStringCtorFromCStr bcstring_ctor = nullptr;
    BCStringDtor bcstring_dtor = nullptr;
};

// Résout les symboles dans SharedBase.dll (déjà chargée par Risen.exe).
// Retourne false et renseigne `missing` (nom du premier symbole introuvable) en cas d'échec.
bool resolve(GameAbi& out, const char*& missing);

}  // namespace mp::abi
