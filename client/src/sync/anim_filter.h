// Filtre des hooks et registre des entités (AD-2, AD-8, AD-12) : seul état partagé entre les
// hooks et la frame (OnProcess), sous un seul verrou. Réutilisé par la sonde (miroir) et par
// la session (avatar ; répliques de PNJ à partir de la story 3.3).
//
// - PS local (héros du joueur, `set_local`) : ses appels sont capturés, jamais bloqués.
// - Registre : une entrée par entité suivie {entité, PS d'animation, PS de routine, enveloppe
//   EMotionFX, rôle}. Il n'est écrit que par le module propriétaire de l'entité (sync/avatar,
//   la sonde, sync/npc), et une entité en sort AVANT d'être oubliée ou détruite. Les détours ne
//   font que le lire.
// - Avatar et réplique de PNJ : les appels que le moteur lance de lui-même sur leur PS
//   d'animation ou leur enveloppe sont bloqués ; seul le rejeu, sous ReplayGuard sur le même
//   thread, passe.
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

constexpr std::size_t kCaptureRing = 256;  // PlayMotion et réglages de locomotion (2.4b)
constexpr std::size_t kCaptureDescSize = 0x60;  // eSMotionPlayBackDesc

enum class CaptureKind : std::uint8_t {
    Motion,   // eCAnimation_PS::PlayMotion
    Ragdoll,  // eCAnimation_PS::SetRagDollEnabled (mort : story 2.1b)
    Param,    // eCWrapper_emfx3ActorInstance::SetTargetWeight / SetPlaySpeed (story 2.4b)
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
    // SetTargetWeight (param_kind 0) / SetPlaySpeed (param_kind 1) : `type` porte le type de motion
    std::uint8_t param_kind = 0;
    float value = 0.0f;
    float time = 0.0f;
};

// Rôle d'une entité suivie (AD-12). Le héros local est déclaré par `set_local` ; une entrée de
// rôle LocalHero n'est jamais bloquée.
// Le héros local reste déclaré par set_local (capturé) : ce rôle décrit une entrée du registre,
// il ne déclenche pas la capture.
enum class EntityRole : std::uint8_t { LocalHero, Avatar, NpcReplica };

// Une entité suivie. Les pointeurs sont opaques ; nuls si absents (sauf `entity`, la clé).
struct TrackedEntity {
    void* entity = nullptr;        // eCDynamicEntity*
    void* animation_ps = nullptr;  // eCAnimation_PS (0x52)
    void* routine_ps = nullptr;    // gCScriptRoutine_PS (0x24)
    void* wrapper = nullptr;       // eCWrapper_emfx3ActorInstance (story 2.4b)
    EntityRole role = EntityRole::Avatar;
};

class AnimFilter {
public:
    enum class Verdict { Pass, Capture, Block };

    // --- Frame (thread principal) ---
    void set_local(void* animation_ps);
    // Ajoute l'entité au registre, ou remplace son entrée (pas de doublon). `entity` nul : ignoré.
    void track(const TrackedEntity& entry);
    // Retire l'entité (PS et enveloppe compris). À appeler AVANT de l'oublier ou de la détruire.
    void untrack(const void* entity);
    // Vide le registre (le PS local et l'anneau sont gardés).
    void clear();
    std::size_t tracked_count() const;
    // Recherches (frame ou hook) : entrée dont le PS d'animation, ou l'enveloppe, vaut `ptr`.
    bool find_by_animation_ps(const void* ptr, TrackedEntity& out) const;
    bool find_by_wrapper(const void* ptr, TrackedEntity& out) const;
    // Retire au plus `cap` captures (les plus anciennes d'abord). `dropped` reçoit le nombre
    // de captures perdues (anneau plein) depuis le dernier appel.
    std::size_t drain(Capture* out, std::size_t cap, unsigned& dropped);
    // Appels bloqués (PlayMotion et SetRagDollEnabled) depuis le dernier appel.
    unsigned take_blocked();
    // Premier thread vu par le hook (0 : aucun appel encore), et s'il en a vu un autre.
    std::uint32_t hook_thread() const { return hook_thread_.load(); }
    bool hook_thread_varied() const { return hook_thread_varied_.load(); }

    // --- Hook ---
    // `ptr` : PS d'animation (PlayMotion, SetRagDollEnabled) ou enveloppe EMotionFX (réglages de
    // locomotion). Capture pour le PS local ; Block pour un avatar ou une réplique de PNJ hors
    // rejeu ; Pass sinon.
    Verdict classify(void* ptr, std::uint32_t thread_id);
    // PS du héros local ? (sans effet sur les compteurs)
    bool is_local(const void* animation_ps) const;
    // Hook gCScriptRoutine_PS::OnProcess en session (story 3.3) : `routine_ps` est-il le PS de
    // routine d'une réplique de PNJ (rôle NpcReplica) ? Vrai : son IA est figée, l'original
    // n'est pas appelé. Simple comparaison sous le verrou ; compte les appels bloqués.
    bool blocks_routine(const void* routine_ps);
    // Appels de routine bloqués depuis le dernier appel.
    unsigned take_routine_blocked();
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
    // Sous mutex_. Une entrée bloque-t-elle `ptr` (PS d'animation ou enveloppe) ?
    bool blocks_locked(const void* ptr) const;

    void* local_ = nullptr;
    std::vector<TrackedEntity> entities_;
    Capture ring_[kCaptureRing];
    std::size_t head_ = 0;   // prochaine écriture
    std::size_t count_ = 0;
    unsigned dropped_ = 0;
    unsigned blocked_ = 0;
    unsigned routine_blocked_ = 0;
    std::atomic<std::uint32_t> hook_thread_{0};
    std::atomic<bool> hook_thread_varied_{false};
};

// L'instance unique (AD-2).
AnimFilter& anim_filter();

}  // namespace mp::sync
