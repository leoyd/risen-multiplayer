// Hooks MinHook. Sonde J1 (probe = 1) : eCApplication::OnProcess et
// eCAnimation_PS::PlayMotion. Session (probe = 0, story 1.4) : OnProcess seul.
// Les deux installations sont exclusives.
#pragma once

namespace mp::win::hooks {

// Résout l'ABI des entités puis installe les deux hooks, en tout ou rien. En cas
// d'échec, journalise la cause, retire ce qui a été posé et retourne false : la sonde
// reste inactive et le jeu intact. À appeler depuis ScriptInit (hors loader lock).
bool install_probe();

// Session : résout l'ABI des entités puis installe le seul hook OnProcess, dont le détour
// appelle sync::session::on_frame(). Même contrat d'échec que install_probe.
bool install_session();

// Retire les hooks posés (sans effet s'il n'y en a aucun).
void remove();

}  // namespace mp::win::hooks
