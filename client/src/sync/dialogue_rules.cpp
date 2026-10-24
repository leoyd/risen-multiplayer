#include "sync/dialogue_rules.h"

#include "core/protocol.h"

namespace mp::sync::dialogue_rules {

HostDecision host_decide(bool running, const void* speaker, const void* hero, const void* partner, int type,
                         const std::string& sample) {
    if (!running) return {HostAction::Ignore, "pas de dialogue en cours", false};
    if (speaker == nullptr) return {HostAction::Ignore, "locuteur illisible", true};
    const bool is_hero = hero != nullptr && speaker == hero;
    const bool is_partner = partner != nullptr && speaker == partner;
    if (!is_hero && !is_partner) return {HostAction::Ignore, "autre locuteur", true};
    if (type < 0 || type > protocol::kMaxDialogOutputType || !protocol::valid_dialog_sample(sample)) {
        return {HostAction::Ignore, "échantillon ou type invalide", true};
    }
    return {is_hero ? HostAction::SendHero : HostAction::SendNpc, nullptr, false};
}

GuestDecision guest_decide(float distance_cm, bool guest_running, bool alive) {
    if (!alive) return {GuestAction::Ignore, "locuteur mort"};
    if (!(distance_cm <= kReplayRadius)) return {GuestAction::Ignore, "trop loin"};
    if (guest_running) return {GuestAction::Ignore, "joueur local en dialogue"};
    return {GuestAction::Replay, nullptr};
}

GuestTransition guest_transition(std::uint32_t prev, std::uint32_t now, bool readable) {
    if (!readable || now == prev) return {0, 0, prev};
    return {now, prev, now};
}

}  // namespace mp::sync::dialogue_rules
