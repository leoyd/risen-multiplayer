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

void AnimFilter::add_replica(void* animation_ps) {
    if (animation_ps == nullptr) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (std::find(replicas_.begin(), replicas_.end(), animation_ps) == replicas_.end()) {
        replicas_.push_back(animation_ps);
    }
}

void AnimFilter::remove_replica(void* animation_ps) {
    std::lock_guard<std::mutex> lock(mutex_);
    replicas_.erase(std::remove(replicas_.begin(), replicas_.end(), animation_ps), replicas_.end());
}

void AnimFilter::clear_replicas() {
    std::lock_guard<std::mutex> lock(mutex_);
    replicas_.clear();
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

AnimFilter::Verdict AnimFilter::classify(void* animation_ps, std::uint32_t thread_id) {
    std::uint32_t expected = 0;
    if (!hook_thread_.compare_exchange_strong(expected, thread_id) && expected != thread_id) {
        hook_thread_varied_.store(true);
    }
    if (animation_ps == nullptr) return Verdict::Pass;
    std::lock_guard<std::mutex> lock(mutex_);
    // Le PS local passe avant tout : jamais bloqué, même déclaré réplique par erreur.
    if (animation_ps == local_) return Verdict::Capture;
    if (std::find(replicas_.begin(), replicas_.end(), animation_ps) == replicas_.end()) return Verdict::Pass;
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

AnimFilter& anim_filter() {
    static AnimFilter instance;
    return instance;
}

}  // namespace mp::sync
