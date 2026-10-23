// Overlay à l'écran : une ligne de texte en haut à gauche (rôle, pseudo, état de session),
// dessinée par un détour de IDirect3DDevice9::Present avec ID3DXFont (d3dx9_39.dll, déjà chargée
// par le jeu). Facultatif : tout échec journalise un Warn `[overlay] …` et laisse la session
// intacte. Aucun appel au jeu (Genome) ici ni dans les détours.
#pragma once

#include <cstdint>
#include <string>

namespace mp::win::overlay {

// Pose les détours de Present et Reset (vtable lue sur un device factice). MinHook doit déjà être
// initialisé (install_session). Retourne false (Warn journalisé) si l'overlay est inactif.
bool install();

// Retire les détours (sans effet s'ils ne sont pas posés).
void remove();

// Texte (UTF-8) et couleur (ARGB) dessinés à partir du prochain Present. Verrou court ; appelable
// depuis n'importe quel thread.
void set_text(const std::string& utf8, std::uint32_t argb);

}  // namespace mp::win::overlay
