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
    for (TrackedEntity& e : entities_) {
        if (e.entity == entry.entity) {
            e = entry;
            return;
        }
    }
    entities_.push_back(entry);
}

void AnimFilter::untrack(const void* entity) {
    if (entity == nullptr) return;
    std::lock_guard<std::mutex> lock(mutex_);
    entities_.erase(std::remove_if(entities_.begin(), entities_.end(),
                                   [entity](const TrackedEntity& e) { return e.entity == entity; }),
                    entities_.end());
}

void AnimFilter::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    entities_.clear();
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

bool AnimFilter::blocks_locked(const void* ptr) const {
    for (const TrackedEntity& e : entities_) {
        if (e.role == EntityRole::LocalHero) continue;
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
    std::uint32_t expected = 0;
    if (!hook_thread_.compare_exchange_strong(expected, thread_id) && expected != thread_id) {
        hook_thread_varied_.store(true);
    }
    if (ptr == nullptr) return Verdict::Pass;
    std::lock_guard<std::mutex> lock(mutex_);
    // Le PS local passe avant tout : jamais bloqué, même suivi comme avatar par erreur.
    if (ptr == local_) return Verdict::Capture;
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

bool AnimFilter::is_local(const void* animation_ps) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return animation_ps != nullptr && animation_ps == local_;
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
