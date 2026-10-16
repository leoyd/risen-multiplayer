// Sonde du spike J1 (`probe = 1`) : répond aux questions de docs/re/spike-j1.md.
// - Tick par seconde : thread, images/s, joueur valide, pose brute du joueur.
// - Capture des PlayMotion du joueur (type, ressource, 0x60 octets du descripteur),
//   journalisées depuis le tick, 20 lignes/s au plus.
// - F9 : copie du joueur ~2 m devant lui. F10 : la copie rejoue la dernière animation.
#pragma once

namespace mp::sync::probe {

// Hook OnProcess, thread principal, une fois par appel. Seul endroit où la sonde
// appelle le jeu (AD-2). Ne lève jamais d'exception.
void on_frame() noexcept;

// Hook PlayMotion, appelé avant l'original : copie des octets si `animation_ps` est
// celui du joueur (mis à jour à chaque tick). N'appelle jamais le jeu.
void on_play_motion(void* animation_ps, int type, void* desc, bool flag) noexcept;

}  // namespace mp::sync::probe
