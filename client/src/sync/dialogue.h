// Dialogues en session (story 4.1, AD-13, protocole v16 ; story 4.2, v17 : les deux sens) : seul module
// qui appelle game/dialogue pour la session. Spike et résultats : docs/re/spike-j3.md.
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
// - Invité, à chaque frame (guest_frame, story 4.2) : InfoSystem::IsRunning et GetNPC ; un dialogue
//   avec une réplique (NetId par sync/npc) donne LocalDialogState{NetId, 1} avant les répliques de la
//   frame, sa fin (ou un autre partenaire) LocalDialogState{NetId, 0} après. Ses répliques suivent la
//   règle de l'hôte (son héros, ou le partenaire traduit en NetId de réplique, 0 sinon). Transitions
//   de PSDialog::IsBeingTalkedTo des répliques en debug (H1, sync/npc).
//   H1 réfuté (test du 2026-10-25) : la demande de dialogue (script AssessTalk) lance ZS_Talk dans la
//   routine du PNJ, figée pour une réplique : soft lock. Détour d'AssessTalk (on_assess_talk) : une
//   demande du héros invité à une réplique la compte comme partenaire dès la frame suivante (libérée,
//   DialogState{NetId, 1}), jusqu'au début du dialogue ou 5 s sans dialogue (re-figée) ; après
//   la fin, libre encore 2 s puis tant que la caméra de dialogue est active (10 s au plus).
// - Hôte, à chaque frame (read_host_state, story 4.2) : son propre dialogue (IsRunning, GUID du
//   partenaire) dans World, pour refuser de tenir un PNJ qui lui parle.
// - Rejeu (replay, ApplyDialogLine), chez l'un ou l'autre : entité du locuteur résolue par l'appelant
//   (invité : avatar de l'hôte ou réplique ; hôte : avatar de l'invité ou PNJ tenu). Ignoré et compté
//   si l'entité manque ou est morte, est à plus de 20 m du héros local, ou si le joueur local est
//   lui-même en dialogue. Sinon sStartOutput(entité, échantillon, type) puis StartFacialAni(échantillon),
//   sous ReplayGuard (le registre bloque les animations natives de l'avatar et des répliques). Jamais
//   StartSay, caméra, menu ni ZS_Talk. SEH : Warn une fois.
//
// Tout sauf on_start_output et on_assess_talk est appelé depuis la frame (hook OnProcess, AD-2).
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

// Détour d'AssessTalk (story 4.2), avant l'original : copie de la demande (PNJ self, demandeur other :
// `Entity*` lus en eCEntity*, ceux de l'unité si nuls) pour la frame. Lecture mémoire et accesseurs
// GetSelfEntity / GetOtherEntity seuls (sous SEH).
// Ne lève jamais d'exception.
void on_assess_talk(void* spu, void* self, void* other) noexcept;

// Hôte, frame : file vidée ; en session (`in_session`), répliques du dialogue en cours → `out`
// (LocalDialogLine, et LocalDialogReplayed pour les ignorées). Hors session : file vidée seulement.
void host_frame(void* hero, bool in_session, std::vector<core::session::Input>& out);

// Invité, frame (story 4.2) : en session (`in_session`, `hero` non nul), début et fin de son dialogue
// avec une réplique (LocalDialogState) et ses répliques (LocalDialogLine) → `out` ; bilan des rejeux
// depuis la frame précédente → LocalDialogReplayed. Hors session : file vidée, dialogue oublié.
void guest_frame(void* hero, bool in_session, std::vector<core::session::Input>& out);

// Hôte, frame (story 4.2) : World::dialog_running, dialog_partner_ok et dialog_partner (sans effet si
// inactif ou hors monde chargé).
void read_host_state(core::session::World& world);

// ApplyDialogLine : rejoue la réplique sur `speaker` (entité du locuteur `net_id`, nulle : absente ou
// morte ; `speaker_name` pour le journal). `hero` = héros local (distance, nul : ignoré).
void replay(void* hero, std::uint32_t net_id, std::uint8_t output_type, const std::string& sample, void* speaker,
            const char* speaker_name);

// Fin de session : Warn déjà émis oubliés (une ligne par session), dialogue de l'invité oublié.
void reset();

}  // namespace mp::sync::dialogue
