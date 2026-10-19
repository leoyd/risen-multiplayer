// Hooks MinHook : eCApplication::OnProcess, eCAnimation_PS::PlayMotion et SetRagDollEnabled.
// Sonde J1 (probe = 1) : détours de sync/probe. Session (probe = 0) : OnProcess vers
// sync/session, PlayMotion et SetRagDollEnabled vers sync/avatar (story 2.2). Les deux
// installations sont exclusives : un seul détour PlayMotion actif.
#pragma once

namespace mp::win::hooks {

// Résout l'ABI des entités puis installe les trois hooks, en tout ou rien. En cas
// d'échec, journalise la cause, retire ce qui a été posé et retourne false : la sonde
// reste inactive et le jeu intact. À appeler depuis ScriptInit (hors loader lock).
bool install_probe();

// Session : résout l'ABI des entités puis installe les trois hooks ; OnProcess appelle
// sync::session::on_frame(), les deux autres bloquent le moteur sur les avatars. Même
// contrat d'échec que install_probe.
bool install_session();

// Retire les hooks posés (sans effet s'il n'y en a aucun).
void remove();

}  // namespace mp::win::hooks
