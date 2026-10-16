// Hooks MinHook de la sonde J1 : eCApplication::OnProcess et eCAnimation_PS::PlayMotion.
#pragma once

namespace mp::win::hooks {

// Résout l'ABI des entités puis installe les deux hooks, en tout ou rien. En cas
// d'échec, journalise la cause, retire ce qui a été posé et retourne false : la sonde
// reste inactive et le jeu intact. À appeler depuis ScriptInit (hors loader lock).
bool install_probe();

// Retire les hooks posés (sans effet s'il n'y en a aucun).
void remove();

}  // namespace mp::win::hooks
