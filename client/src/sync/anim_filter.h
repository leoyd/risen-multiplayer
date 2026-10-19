// Filtre des hooks eCAnimation_PS::PlayMotion et SetRagDollEnabled (AD-2, AD-8) : seul
// état partagé entre les hooks et la frame (OnProcess). Réutilisé par la sonde (miroir) et
// par l'avatar.
//
// - PS local (héros du joueur) : ses appels sont capturés, jamais bloqués.
// - PS des répliques (copie de la sonde, avatar) : les appels que le moteur lance de
//   lui-même sont bloqués ; seul le rejeu, sous ReplayGuard sur le même thread, passe.
// - Anneau des captures, vidé par la frame.
//
// Le filtre ne touche jamais au jeu : il ne manipule que des pointeurs opaques. Portable
// (std::mutex), testé sous Linux.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace mp::sync {

constexpr std::size_t kCaptureRing = 64;
constexpr std::size_t kCaptureDescSize = 0x60;  // eSMotionPlayBackDesc

enum class CaptureKind : std::uint8_t {
    Motion,   // eCAnimation_PS::PlayMotion
    Ragdoll,  // eCAnimation_PS::SetRagDollEnabled (mort : story 2.1b)
};

// Un appel du héros local, copié par le hook. Les deux natures partagent l'anneau : leur
// ordre est conservé.
struct Capture {
    CaptureKind kind = CaptureKind::Motion;
    // PlayMotion
    int type = 0;
    bool flag = false;
    bool desc_present = false;   // descripteur non nul et lisible
    void* resource = nullptr;    // emplacement `type` du PS : nom (frame) et contrôle seulement
    std::uint8_t desc[kCaptureDescSize] = {};
    // SetRagDollEnabled
    bool ragdoll_on = false;
    bool ragdoll_flag = false;   // second bool (non lu par le moteur sur le chemin observé)
    bool has_vector = false;     // bCVector* non nul et lisible
    float vector[3] = {};
};

class AnimFilter {
public:
    enum class Verdict { Pass, Capture, Block };

    // --- Frame (thread principal) ---
    void set_local(void* animation_ps);
    void add_replica(void* animation_ps);
    // À appeler AVANT d'oublier ou de détruire l'entité.
    void remove_replica(void* animation_ps);
    void clear_replicas();
    // Retire au plus `cap` captures (les plus anciennes d'abord). `dropped` reçoit le nombre
    // de captures perdues (anneau plein) depuis le dernier appel.
    std::size_t drain(Capture* out, std::size_t cap, unsigned& dropped);
    // Appels bloqués (PlayMotion et SetRagDollEnabled) depuis le dernier appel.
    unsigned take_blocked();
    // Premier thread vu par le hook (0 : aucun appel encore), et s'il en a vu un autre.
    std::uint32_t hook_thread() const { return hook_thread_.load(); }
    bool hook_thread_varied() const { return hook_thread_varied_.load(); }

    // --- Hook ---
    Verdict classify(void* animation_ps, std::uint32_t thread_id);
    // PS du héros local ? (sans effet sur les compteurs)
    bool is_local(const void* animation_ps) const;
    // Hook SetRagDollEnabled : une désactivation qui ne vient pas du héros local passe toujours,
    // sans être comptée (le moteur nettoie un avatar : décision du 2026-10-17) ; sinon classify.
    Verdict classify_ragdoll(void* animation_ps, bool on, std::uint32_t thread_id);
    void push(const Capture& capture);

    // Garde de rejeu, propre au thread courant : pendant sa durée, les appels de ce
    // thread sur une réplique passent. Les autres threads restent bloqués.
    class ReplayGuard {
    public:
        ReplayGuard();
        ~ReplayGuard();
        ReplayGuard(const ReplayGuard&) = delete;
        ReplayGuard& operator=(const ReplayGuard&) = delete;
    };
    static bool replaying();

private:
    mutable std::mutex mutex_;
    void* local_ = nullptr;
    std::vector<void*> replicas_;
    Capture ring_[kCaptureRing];
    std::size_t head_ = 0;   // prochaine écriture
    std::size_t count_ = 0;
    unsigned dropped_ = 0;
    unsigned blocked_ = 0;
    std::atomic<std::uint32_t> hook_thread_{0};
    std::atomic<bool> hook_thread_varied_{false};
};

// L'instance unique (AD-2).
AnimFilter& anim_filter();

}  // namespace mp::sync
