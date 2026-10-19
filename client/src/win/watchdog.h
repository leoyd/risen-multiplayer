// Sentinelle de diagnostic (story 2.2, gel après reconnexion) : si la frame (hook OnProcess)
// ne revient plus pendant kStallMs alors qu'elle est armée, un thread à part suspend le thread
// principal, déroule sa pile (RtlVirtualUnwind) et l'écrit au log, trois fois à 1 s
// d'intervalle. Elle ne touche jamais au jeu : lecture de contexte et de pile seulement.
#pragma once

namespace mp::win::watchdog {

// Démarre le thread de la sentinelle (une fois). Appelé depuis ScriptInit.
void start();
// Frame (thread principal) : la frame tourne ; `armed` = surveiller (un avatar existe).
void beat(bool armed);

}  // namespace mp::win::watchdog
