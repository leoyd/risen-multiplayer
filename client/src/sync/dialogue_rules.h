// Règles des dialogues (story 4.1, AD-13), sans Windows ni appel au jeu : testées sous Linux
// (client/tests/test_dialogue_rules.cpp). sync/dialogue lit l'état du jeu et applique la décision.
//
// Hôte, pour chaque StartOutput relevé (état d'InfoSystem lu dans le détour, au moment de la réplique) :
//   1. pas de dialogue en cours → ignoré, non compté (commentaire ambiant) ;
//   2. locuteur illisible → ignoré, compté ;
//   3. locuteur ni héros ni partenaire de dialogue → ignoré, compté ;
//   4. échantillon ou type invalides → ignoré, compté ;
//   5. sinon : envoyé comme héros (NetId 1) ou comme PNJ partenaire (NetId par son GUID).
// Invité, pour chaque DialogLine à rejouer (entité trouvée) : mort → ignoré ; à plus de 20 m (2000 cm
// inclus) → ignoré ; invité lui-même en dialogue → ignoré ; sinon rejoué.
#pragma once

#include <string>

namespace mp::sync::dialogue_rules {

inline constexpr float kReplayRadius = 2000.0f;  // 20 m, unités du monde (cm), inclus

enum class HostAction { SendHero, SendNpc, Ignore };
struct HostDecision {
    HostAction action = HostAction::Ignore;
    const char* reason = nullptr;  // Ignore : raison lisible
    bool counted = false;          // Ignore : compté au bilan (faux pour un commentaire ambiant)
};
// `speaker`, `hero`, `partner` : pointeurs opaques, jamais déréférencés (nul : absent ou illisible).
HostDecision host_decide(bool running, const void* speaker, const void* hero, const void* partner, int type,
                         const std::string& sample);

enum class GuestAction { Replay, Ignore };
struct GuestDecision {
    GuestAction action = GuestAction::Ignore;
    const char* reason = nullptr;
};
GuestDecision guest_decide(float distance_cm, bool guest_running, bool alive);

}  // namespace mp::sync::dialogue_rules
