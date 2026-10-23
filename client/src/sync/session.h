// Pilote Windows de la session (stories 1.4 et 1.5) : relie core/session au transport et au
// jeu. Une chose par frame, depuis le hook eCApplication::OnProcess (thread principal) : lire
// le monde (GetPlayer, IsGameRunning, IsDead, pose), vider poll(), session.tick(), exécuter les
// commandes, écrire les logs [sync]. En debug, une ligne « [sync] signaux » par changement.
//
// Le Transport est alloué une fois et volontairement jamais détruit : aucun join ne se fait
// au déchargement de la DLL (sous le verrou du chargeur). L'arrêt explicite (fin du maintien
// après Ended) fait stop() depuis le thread principal ; le réarmement le redémarre.
#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "core/config.h"
#include "core/version.h"

namespace mp::sync::session {

// Prépare la session à partir de la config et des empreintes locales (Game, Engine, Script,
// SharedBase). À appeler depuis ScriptInit, après l'installation du hook OnProcess.
void configure(const Config& config, const std::array<ModuleFingerprint, 4>& fingerprint,
               const char* mod_version);

// Hook OnProcess, thread principal, une fois par appel. Ne lève jamais d'exception : un échec
// journalise la cause et laisse le jeu en solo.
void on_frame() noexcept;

// Overlay à l'écran : après chaque tick, le texte (core/overlay_text) est passé à `sink` s'il a
// changé depuis le dernier appel. Branché par win/script_init sur win::overlay::set_text (sync/
// n'en dépend pas) ; nul = pas d'overlay. À appeler depuis ScriptInit, avant la première frame.
using OverlaySink = void (*)(const std::string& utf8, std::uint32_t argb);
void set_overlay_sink(OverlaySink sink);

}  // namespace mp::sync::session
