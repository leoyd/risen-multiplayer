// Sonde du spike J3 (story 4.0, `probe = 1`) : dialogues. Hypothèses H1 à H5 et protocole :
// docs/re/spike-j3.md (§ 3 et 4).
// - Passif (H1) : chaque gCScriptProcessingUnit::StartOutput (détour) est journalisé à la frame
//   suivante : locuteur (GetSelfEntity, nom, héros ou non), échantillon, type, retour ; puis la fin
//   de la sortie (sIsOutputPlaying). Les transitions du système de dialogue (InfoSystem : en cours,
//   partenaire, commande courante, identifiant de texte d'une commande Say) sont journalisées une
//   fois par changement.
// - Num7 (H2, H3) : rejoue la dernière réplique capturée (sStartOutput + StartFacialAni) sur le PNJ
//   suivi par Num3 s'il est à moins de 15 m, sinon sur le PNJ le plus proche, hors partenaire du
//   dialogue en cours. Num4 (spike J2) fige son IA avant (H3). 2ᵉ appui en moins de 2 s :
//   Entity::StartSay(auditeur = héros, identifiant de texte capturé).
// - Num8 (H4) : même chose avec la dernière réplique du héros, sur la copie (F6).
// - Num9 (H5) : sous-titre de la dernière réplique (texte localisé par eCLocAdmin2, sinon
//   l'échantillon brut) par gCHudSystem2::ShowSubtitle ; 2ᵉ appui en moins de 2 s :
//   GUI2::StartTalk (page de dialogue du HUD) ; appui suivant : GUI2::StopTalk.
// Jamais actif en session (`probe = 0`). Seul ce module appelle game/dialogue pour la sonde,
// depuis OnProcess (AD-2) ; le détour ne fait que copier (accesseurs GetSelfEntity, GetText).
#pragma once

#include "game/entity.h"

#include <cstdint>

namespace mp::sync::probe_j3 {

// Statut du détour de StartOutput (ABI des dialogues résolue et hook créé), fixé une fois par
// install_probe avant le premier OnProcess. False : journal et touches Num7 à Num9 inactifs.
void set_status(bool start_output_hook);

// Frame (thread principal), une fois par OnProcess, après la sonde J2.
void frame(const game::Entity& player, const game::Entity& copy, bool focus, std::uint64_t now_ms);

// Joueur changé (chargement, menu) : répliques capturées, suivis et page de dialogue oubliés.
// Aucun appel au jeu.
void reset();

// Ligne d'aide de Num0 (affichée par la sonde J2), ASCII.
const char* help_line();

// Détour de gCScriptProcessingUnit::StartOutput, après l'original (`result` = son retour) :
// copie du locuteur, de l'échantillon et du type pour la frame. Aucun appel au jeu hormis les
// accesseurs GetSelfEntity et bCString::GetText (sous SEH).
void on_start_output(void* spu, const void* sample, int type, bool result) noexcept;

}  // namespace mp::sync::probe_j3
