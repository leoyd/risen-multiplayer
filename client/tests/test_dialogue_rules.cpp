// Règles des dialogues (story 4.1) : un cas par règle (sync/dialogue_rules).
#include "sync/dialogue_rules.h"
#include "test.h"

#include <string>

using namespace mp::sync::dialogue_rules;

namespace {
int g_hero, g_partner, g_other;
const void* const kHero = &g_hero;
const void* const kPartner = &g_partner;
const void* const kOther = &g_other;
const std::string kSample = "INFO_Sara_2_Sara_DE_%_Sara.wav";
}  // namespace

TEST("dialogue_rules hôte : commentaire ambiant (pas de dialogue) → ignoré, non compté") {
    const HostDecision d = host_decide(false, kPartner, kHero, kPartner, 0, kSample);
    CHECK(d.action == HostAction::Ignore && !d.counted);
    CHECK_EQ(std::string(d.reason), std::string("pas de dialogue en cours"));
    CHECK(host_decide(false, kHero, kHero, nullptr, 3, kSample).action == HostAction::Ignore);
}

TEST("dialogue_rules hôte : héros → envoyé comme héros") {
    CHECK(host_decide(true, kHero, kHero, kPartner, 3, kSample).action == HostAction::SendHero);
    CHECK(host_decide(true, kHero, kHero, nullptr, 3, kSample).action == HostAction::SendHero);
}

TEST("dialogue_rules hôte : partenaire → envoyé comme PNJ") {
    CHECK(host_decide(true, kPartner, kHero, kPartner, 0, kSample).action == HostAction::SendNpc);
}

TEST("dialogue_rules hôte : troisième locuteur, locuteur illisible → ignorés, comptés") {
    HostDecision d = host_decide(true, kOther, kHero, kPartner, 0, kSample);
    CHECK(d.action == HostAction::Ignore && d.counted);
    CHECK_EQ(std::string(d.reason), std::string("autre locuteur"));
    d = host_decide(true, kOther, kHero, nullptr, 0, kSample);  // pas de partenaire
    CHECK(d.action == HostAction::Ignore && d.counted);
    d = host_decide(true, nullptr, kHero, kPartner, 0, kSample);
    CHECK(d.action == HostAction::Ignore && d.counted);
    CHECK_EQ(std::string(d.reason), std::string("locuteur illisible"));
}

TEST("dialogue_rules hôte : échantillon ou type invalides → ignorés, comptés") {
    for (const std::string& s : {std::string(""), std::string(".wav"), std::string("../x.wav"), std::string("x")}) {
        const HostDecision d = host_decide(true, kHero, kHero, kPartner, 3, s);
        CHECK(d.action == HostAction::Ignore && d.counted);
    }
    CHECK(host_decide(true, kHero, kHero, kPartner, 4, kSample).action == HostAction::Ignore);
    CHECK(host_decide(true, kHero, kHero, kPartner, -1, kSample).action == HostAction::Ignore);
}

TEST("dialogue_rules invité : 2000 cm rejoué, 2001 cm trop loin") {
    CHECK(guest_decide(0.0f, false, true).action == GuestAction::Replay);
    CHECK(guest_decide(2000.0f, false, true).action == GuestAction::Replay);
    const GuestDecision far = guest_decide(2001.0f, false, true);
    CHECK(far.action == GuestAction::Ignore);
    CHECK_EQ(std::string(far.reason), std::string("trop loin"));
}

TEST("dialogue_rules rejeu : joueur local en dialogue, locuteur mort → ignorés") {
    GuestDecision d = guest_decide(800.0f, true, true);
    CHECK(d.action == GuestAction::Ignore);
    CHECK_EQ(std::string(d.reason), std::string("joueur local en dialogue"));
    d = guest_decide(800.0f, false, false);
    CHECK(d.action == GuestAction::Ignore);
    CHECK_EQ(std::string(d.reason), std::string("locuteur mort"));
}

TEST("dialogue_rules invité (4.2) : transitions du dialogue de l'invité") {
    GuestTransition t = guest_transition(0, 1033, true);  // début
    CHECK(t.start == 1033u && t.end == 0u && t.next == 1033u);
    t = guest_transition(1033, 1033, true);  // en cours
    CHECK(t.start == 0u && t.end == 0u && t.next == 1033u);
    t = guest_transition(1033, 0, true);  // fin
    CHECK(t.start == 0u && t.end == 1033u && t.next == 0u);
    t = guest_transition(1033, 1040, true);  // nouveau partenaire dans la même frame
    CHECK(t.start == 1040u && t.end == 1033u && t.next == 1040u);
    t = guest_transition(1033, 0, false);  // frame illisible : état gardé
    CHECK(t.start == 0u && t.end == 0u && t.next == 1033u);
    t = guest_transition(0, 1033, false);
    CHECK(t.start == 0u && t.end == 0u && t.next == 0u);
    t = guest_transition(0, 0, true);
    CHECK(t.start == 0u && t.end == 0u && t.next == 0u);
}
