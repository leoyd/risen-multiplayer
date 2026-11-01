#include "sync/anim_filter.h"

#include <algorithm>

namespace mp::sync {

namespace {

thread_local int t_replay_depth = 0;

}  // namespace

AnimFilter::ReplayGuard::ReplayGuard() { ++t_replay_depth; }
AnimFilter::ReplayGuard::~ReplayGuard() { --t_replay_depth; }

bool AnimFilter::replaying() { return t_replay_depth > 0; }

void AnimFilter::set_local(void* animation_ps) {
    std::lock_guard<std::mutex> lock(mutex_);
    local_ = animation_ps;
}

void AnimFilter::track(const TrackedEntity& entry) {
    if (entry.entity == nullptr) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (entry.role == EntityRole::NpcCapture) npc_last_.erase(entry.net_id);  // filtre à neuf
    for (TrackedEntity& e : entities_) {
        if (e.entity == entry.entity) {
            if (e.role == EntityRole::NpcCapture) npc_last_.erase(e.net_id);
            e = entry;
            return;
        }
    }
    entities_.push_back(entry);
}

void AnimFilter::untrack(const void* entity) {
    if (entity == nullptr) return;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const TrackedEntity& e : entities_) {
        if (e.entity == entity && e.role == EntityRole::NpcCapture) npc_last_.erase(e.net_id);
    }
    entities_.erase(std::remove_if(entities_.begin(), entities_.end(),
                                   [entity](const TrackedEntity& e) { return e.entity == entity; }),
                    entities_.end());
}

void AnimFilter::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    entities_.clear();
    npc_last_.clear();
    held_routines_.clear();
}

bool AnimFilter::set_freed(const void* entity, bool freed) {
    if (entity == nullptr) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    for (TrackedEntity& e : entities_) {
        if (e.entity == entity && e.role == EntityRole::NpcReplica) {
            e.freed = freed;
            npc_last_.erase(e.net_id);  // filtre des réglages capturés à neuf
            return true;
        }
    }
    return false;
}

void AnimFilter::hold_routine(const void* routine_ps) {
    if (routine_ps == nullptr) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (std::find(held_routines_.begin(), held_routines_.end(), routine_ps) == held_routines_.end()) {
        held_routines_.push_back(routine_ps);
    }
}

void AnimFilter::release_routine(const void* routine_ps) {
    std::lock_guard<std::mutex> lock(mutex_);
    held_routines_.erase(std::remove(held_routines_.begin(), held_routines_.end(), routine_ps), held_routines_.end());
}

std::size_t AnimFilter::held_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return held_routines_.size();
}

std::size_t AnimFilter::tracked_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entities_.size();
}

bool AnimFilter::find_by_animation_ps(const void* ptr, TrackedEntity& out) const {
    if (ptr == nullptr) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const TrackedEntity& e : entities_) {
        if (e.animation_ps == ptr) {
            out = e;
            return true;
        }
    }
    return false;
}

bool AnimFilter::find_by_wrapper(const void* ptr, TrackedEntity& out) const {
    if (ptr == nullptr) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const TrackedEntity& e : entities_) {
        if (e.wrapper == ptr) {
            out = e;
            return true;
        }
    }
    return false;
}

bool AnimFilter::find_by_entity(const void* entity, TrackedEntity& out) const {
    if (entity == nullptr) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const TrackedEntity& e : entities_) {
        if (e.entity == entity) {
            out = e;
            return true;
        }
    }
    return false;
}

bool AnimFilter::blocks_locked(const void* ptr) const {
    for (const TrackedEntity& e : entities_) {
        if (e.role == EntityRole::LocalHero || e.role == EntityRole::NpcCapture || e.freed) continue;
        if (e.animation_ps == ptr || e.wrapper == ptr) return true;
    }
    return false;
}

std::size_t AnimFilter::drain(Capture* out, std::size_t cap, unsigned& dropped) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::size_t n = std::min(cap, count_);
    const std::size_t start = (head_ + kCaptureRing - count_) % kCaptureRing;
    for (std::size_t i = 0; i < n; ++i) out[i] = ring_[(start + i) % kCaptureRing];
    count_ -= n;
    dropped = dropped_;
    dropped_ = 0;
    return n;
}

unsigned AnimFilter::take_blocked() {
    std::lock_guard<std::mutex> lock(mutex_);
    const unsigned n = blocked_;
    blocked_ = 0;
    return n;
}

AnimFilter::Verdict AnimFilter::classify(void* ptr, std::uint32_t thread_id) {
    std::uint32_t net_id = 0;
    return classify(ptr, thread_id, net_id);
}

AnimFilter::Verdict AnimFilter::classify(void* ptr, std::uint32_t thread_id, std::uint32_t& net_id) {
    net_id = 0;
    std::uint32_t expected = 0;
    if (!hook_thread_.compare_exchange_strong(expected, thread_id) && expected != thread_id) {
        hook_thread_varied_.store(true);
    }
    if (ptr == nullptr) return Verdict::Pass;
    std::lock_guard<std::mutex> lock(mutex_);
    // Le PS local passe avant tout : jamais bloqué, même suivi comme avatar par erreur.
    if (ptr == local_) return Verdict::Capture;
    // PNJ animé de l'hôte (story 3.4) : capturé sous son NetId, jamais bloqué. Story 4.2 : réplique libérée
    // de l'invité, idem (gestes du partenaire vers l'hôte). Le rejeu passe sans être capturé (chez l'hôte,
    // gestes de l'invité rejoués sur le PNJ tenu : pas d'écho).
    for (const TrackedEntity& e : entities_) {
        const bool captured = e.role == EntityRole::NpcCapture || (e.role == EntityRole::NpcReplica && e.freed);
        if (captured && e.net_id != 0 && (e.animation_ps == ptr || e.wrapper == ptr)) {
            if (replaying()) return Verdict::Pass;
            net_id = e.net_id;
            return Verdict::Capture;
        }
    }
    if (!blocks_locked(ptr)) return Verdict::Pass;
    if (replaying()) return Verdict::Pass;
    ++blocked_;
    return Verdict::Block;
}

void AnimFilter::push(const Capture& capture) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (count_ == kCaptureRing) {
        ++dropped_;
        return;
    }
    ring_[head_] = capture;
    head_ = (head_ + 1) % kCaptureRing;
    ++count_;
}

bool AnimFilter::npc_param_changed(std::uint32_t net_id, int type, int kind, float value, float time) {
    if (type < 0 || type >= 8 || kind < 0 || kind >= 4) return true;
    std::lock_guard<std::mutex> lock(mutex_);
    LastCapture& last = npc_last_[net_id].last[type][kind];
    if (last.valid && last.value == value && last.time == time) return false;
    last = LastCapture{true, value, time};
    return true;
}

void AnimFilter::npc_motion_started(std::uint32_t net_id, int type) {
    if (type < 0 || type >= 8) return;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = npc_last_.find(net_id);
    if (it == npc_last_.end()) return;
    for (LastCapture& lc : it->second.last[type]) lc.valid = false;
}

bool AnimFilter::is_local(const void* animation_ps) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return animation_ps != nullptr && animation_ps == local_;
}

bool AnimFilter::blocks_routine(const void* routine_ps) {
    if (routine_ps == nullptr) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const TrackedEntity& e : entities_) {
        if (e.role == EntityRole::NpcReplica && !e.freed && e.routine_ps == routine_ps) {
            ++routine_blocked_;
            return true;
        }
    }
    // Story 4.2, hôte : PNJ tenu pendant le dialogue de l'invité.
    if (std::find(held_routines_.begin(), held_routines_.end(), routine_ps) != held_routines_.end()) {
        ++routine_blocked_;
        return true;
    }
    return false;
}

unsigned AnimFilter::take_routine_blocked() {
    std::lock_guard<std::mutex> lock(mutex_);
    const unsigned n = routine_blocked_;
    routine_blocked_ = 0;
    return n;
}

AnimFilter::Verdict AnimFilter::classify_ragdoll(void* animation_ps, bool on, std::uint32_t thread_id) {
    if (!on && !is_local(animation_ps)) return Verdict::Pass;
    return classify(animation_ps, thread_id);
}

AnimFilter& anim_filter() {
    static AnimFilter instance;
    return instance;
}

}  // namespace mp::sync
