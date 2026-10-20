// Sonde du spike J2 (story 3.0, `probe = 1`) : six expériences au pavé numérique (Verr. Num
// actif), qui répondent aux questions Q5, Q6, Q3 et Q7 de la spine. Protocole : docs/re/spike-j2.md.
// - Num1 (Q5) : arme du héros ; la copie (F6) reçoit la même arme en main, puis rangée.
// - Num2 (Q6) : mode de collision et de placement de la copie (0 → 1 → 2 → 3 → 0).
// - Num3 (Q3) : PNJ le plus proche (≤ 15 m) : nom, GUID, PV, aller-retour GUID → entité.
// - Num4 (Q3) : fige ou libère l'IA de ce PNJ (hook gCScriptRoutine_PS::OnProcess).
// - Num5 (Q3) : dégâts sur ce PNJ (DoDamage, puis repli SetHitPoints).
// - Num6 (Q7) : appui court = interception de la mort du héros (Kill → Defeat) ;
//   tenu 1 s = relève du héros.
// Seul ce module et probe.cpp appellent game/ pour la sonde, depuis OnProcess (AD-2). Le détour
// RoutineOnProcess ne lit qu'un atomique ; celui de Kill lit la mémoire et, seule exception
// documentée à AD-2, appelle Defeat (mêmes arguments, même thread).
#pragma once

#include "game/entity.h"

#include <cstddef>
#include <cstdint>

namespace mp::sync::probe_j2 {

// Statut des hooks facultatifs, fixé une fois par install_probe (avant le premier OnProcess).
void set_status(bool combat_resolved, bool routine_hook, bool kill_hook);

// Frame (thread principal), une fois par OnProcess : touches, journaux différés, héros publié
// pour le détour de Kill. `focus` : le jeu a le focus. `now_ms` : GetTickCount64.
void frame(const game::Entity& player, const game::Entity& copy, bool focus, std::uint64_t now_ms);

// Joueur changé (chargement, menu) : PNJ suivi, PS figé, mode de collision et arme de la copie
// oubliés. Aucun appel au jeu (les entités ont pu être détruites).
void reset();

// Miroir (F7) : place la copie sur `target` selon le mode de collision de Num2.
void place_copy(const game::Entity& copy, const game::Matrix& target);

// PNJ le plus proche du joueur à moins de `max_distance` (cm), hors joueur et `exclude`.
// `list_ok` = false si GetNPCs a échoué. `best.ptr` nul : aucun.
void find_nearest_npc(const game::Entity& player, const game::Entity& exclude, float max_distance,
                      game::Entity& best, float& distance, std::size_t& total, bool& list_ok);

// --- Détours (aucun appel au jeu) ---
// gCScriptRoutine_PS::OnProcess : false = ne pas appeler l'original (IA figée de ce PS).
bool on_routine_process(void* routine_ps) noexcept;
// ::Kill(spu, self, other, arg) : true = la victime (`other`, un Entity*) est le héros et
// l'interception est active ; le détour appelle alors Defeat à la place.
// `other` nul : victime lue par gCScriptProcessingUnit::GetOtherEntity(spu).
bool intercept_kill(void* spu, void* other) noexcept;
// DownOrDeadBy* (which 0 mêlée, 1 projectile, 2 inferno) : true = la victime (`other`) est le
// héros et l'interception est active ; le héros est remis à 1 PV, le détour rend 0 sans appeler
// l'original (comme le jeu quand la victime survit).
bool intercept_down_or_dead(int which, void* spu, void* other) noexcept;
// Kill imbriqué dans Defeat : true = sa victime est le héros, l'appel est avalé (compté).
bool swallow_nested_kill(void* spu, void* other) noexcept;
// Résultat de l'appel de Defeat par le détour (journalisé par la frame).
void note_defeat(bool call_ok, long long result) noexcept;

}  // namespace mp::sync::probe_j2
