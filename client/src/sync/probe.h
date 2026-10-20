// Sonde du spike J1 (`probe = 1`) : répond aux questions de docs/re/spike-j1.md.
// - Tick par seconde : thread, images/s, joueur valide, pose brute du joueur.
// - Capture des PlayMotion du joueur (type, nom, 7 scalaires du descripteur),
//   journalisées depuis le tick, 20 lignes/s au plus.
// - F6 : copie du joueur ~2 m devant lui. F7 : mode miroir (la copie rejoue chaque
//   animation du joueur par son nom, via un Motion encodé puis décodé, et suit ses
//   déplacements ; story 2.1). F4 : même rejeu du dernier coup sur le PNJ le plus proche.
// - Mort (story 2.1b) : SetRagDollEnabled du joueur capturé et rejoué sur la copie.
// L'état partagé avec les hooks passe par sync::anim_filter() (AD-2).
// - Spike J2 (story 3.0) : pavé numérique Num1 à Num6, voir sync/probe_j2.h.
#pragma once

namespace mp::sync::probe {

// Hook OnProcess, thread principal, une fois par appel. Seul endroit où la sonde
// appelle le jeu (AD-2). Ne lève jamais d'exception.
void on_frame() noexcept;

// Hook PlayMotion, appelé avant l'original : copie des octets si `animation_ps` est
// celui du joueur (mis à jour à chaque frame). N'appelle jamais le jeu, hormis les lectures
// mémoire protégées de game/.
// Retourne false pour bloquer l'original : en mode miroir, les PlayMotion que le
// moteur lance de lui-même sur la copie.
bool on_play_motion(void* animation_ps, int type, void* desc, bool flag) noexcept;

// Hook SetRagDollEnabled (story 2.1b), mêmes règles : capture sur le PS du joueur, blocage
// sur la copie en mode miroir hors rejeu. `vector` = bCVector (3 f32) ou nul.
bool on_set_ragdoll(void* animation_ps, bool on, bool flag, const float* vector) noexcept;

}  // namespace mp::sync::probe
