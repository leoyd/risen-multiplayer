// Dialogues en session (story 4.1, AD-13, protocole v16) : seul module qui appelle game/dialogue
// pour la session. Spike et résultats : docs/re/spike-j3.md.
//
// - Détour de gCScriptProcessingUnit::StartOutput (win/hooks, posé si `dialogues = 1` et le bloc
//   game/dialogue résolu) : on_start_output ne fait que copier le locuteur (GetSelfEntity),
//   l'échantillon (bCString::GetText), le type et l'état d'InfoSystem à cet instant (IsRunning,
//   GetNPC : accesseurs sans effet de bord) dans une file bornée (32) sous verrou court ;
//   l'original est toujours appelé par le détour.
// - Hôte, à chaque frame (host_frame) : file vidée ; en session seulement (règles pures :
//   sync/dialogue_rules), une réplique dite pendant un dialogue par son héros ou par le partenaire de
//   dialogue (traduit par son GUID) devient LocalDialogLine. Commentaires ambiants : ni envoyés ni
//   comptés. Autre locuteur, locuteur illisible, échantillon invalide ou GUID illisible pendant un
//   dialogue : ignoré, compté (LocalDialogReplayed), ligne Debug.
// - Invité (replay, ApplyDialogLine) : locuteur 1 → avatar de l'hôte, ≥ 1000 → réplique de ce
//   NetId. Ignoré et compté si l'entité manque ou est morte, est à plus de 20 m du héros, ou si
//   l'invité est lui-même en dialogue. Sinon sStartOutput(entité, échantillon, type) puis
//   StartFacialAni(échantillon), sous ReplayGuard (le registre bloque les animations natives de
//   l'avatar et des répliques). Jamais StartSay, caméra, menu ni ZS_Talk. SEH : Warn une fois.
//
// Tout sauf on_start_output est appelé depuis la frame (hook OnProcess, AD-2).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/session.h"

namespace mp::sync::dialogue {

// Fixé une fois par win/hooks avant le premier OnProcess : bloc game/dialogue résolu et détour de
// StartOutput posé. False : ni émission ni rejeu (sync/session met alors Settings::dialogues à faux).
void set_active(bool active);
bool active();

// Détour de StartOutput, après l'original : copie pour la frame. Aucun appel au jeu hormis les
// accesseurs GetSelfEntity, bCString::GetText, InfoSystem::IsRunning et GetNPC (sous SEH).
// Ne lève jamais d'exception.
void on_start_output(void* spu, const void* sample, int type) noexcept;

// Hôte, frame : file vidée ; en session (`in_session`), répliques du dialogue en cours → `out`
// (LocalDialogLine, et LocalDialogReplayed pour les ignorées). Hors session : file vidée seulement.
void host_frame(void* hero, bool in_session, std::vector<core::session::Input>& out);

// Invité : file vidée sans rien émettre (pas de dialogue de l'invité dans la story 4.1) ; bilan
// des rejeux depuis la frame précédente → LocalDialogReplayed.
void guest_frame(std::vector<core::session::Input>& out);

// Invité, ApplyDialogLine : rejoue la réplique sur le locuteur `net_id` (règles ci-dessus).
// `hero` = héros de l'invité (distance, nul : ignoré).
void replay(void* hero, std::uint32_t net_id, std::uint8_t output_type, const std::string& sample);

// Fin de session : Warn déjà émis oubliés (une ligne par session).
void reset();

}  // namespace mp::sync::dialogue
