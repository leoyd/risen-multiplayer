// Règles des dialogues (story 4.1, AD-13 ; story 4.2 : aussi chez l'invité qui parle et l'hôte qui
// rejoue), sans Windows ni appel au jeu : testées sous Linux
// (client/tests/test_dialogue_rules.cpp). sync/dialogue lit l'état du jeu et applique la décision.
//
// Celui qui parle (hôte, et invité depuis la story 4.2), pour chaque StartOutput relevé (état
// d'InfoSystem lu dans le détour, au moment de la réplique) :
//   1. pas de dialogue en cours → ignoré, non compté (commentaire ambiant) ;
//   2. locuteur illisible → ignoré, compté ;
//   3. locuteur ni héros ni partenaire de dialogue → ignoré, compté ;
//   4. échantillon ou type invalides → ignoré, compté ;
//   5. sinon : envoyé comme héros (NetId 1 ou 2) ou comme PNJ partenaire (hôte : NetId par son GUID ;
//      invité : NetId de sa réplique).
// Celui qui rejoue (invité, et hôte depuis la story 4.2), pour chaque DialogLine (entité trouvée) :
// mort → ignoré ; à plus de 20 m (2000 cm inclus) du héros local → ignoré ; joueur local lui-même en
// dialogue → ignoré ; sinon rejoué.
#pragma once

#include <cstdint>
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

// Story 4.2, invité, à chaque frame : partenaire de son dialogue à la frame précédente (`prev`) et à
// celle-ci (`now`, NetId de réplique, 0 : aucun dialogue ou pas une réplique), lu ou non (`readable`).
// Illisible : rien ne change. Sinon `start` = now s'il diffère de prev (0 : aucun début), `end` = prev
// s'il diffère de now (0 : aucune fin) ; un changement de partenaire donne les deux. `next` = état gardé.
struct GuestTransition {
    std::uint32_t start = 0;
    std::uint32_t end = 0;
    std::uint32_t next = 0;
};
GuestTransition guest_transition(std::uint32_t prev, std::uint32_t now, bool readable);

}  // namespace mp::sync::dialogue_rules
