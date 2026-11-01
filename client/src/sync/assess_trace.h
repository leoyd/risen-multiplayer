// Trace de diagnostic des scripts de perception (clé `trace_assess = 1`, test du 2026-11-01 : les
// gardes ne réagissent pas quand l'avatar de l'invité frappe un PNJ chez l'hôte).
//
// Les détours de win/hooks posent un détour sur chaque script de Script_Game.dll de signature
// `long long (gCScriptProcessingUnit*, void* self, void* other, long long)` dont le nom commence par
// `Assess` ou `Do…Damage` (hors AssessTalk, déjà détourné en session). Le détour ne fait que copier
// (script, self, other, argument, retour) dans un anneau (accesseurs GetSelfEntity / GetOtherEntity
// sous SEH si self ou other est nul, AD-2) ; la frame garde les appels où le héros local ou l'avatar
// est self ou other, les dédoublonne (même script, self, other et retour : une ligne par 3 s, avec le
// nombre d'appels) et les journalise en Info avec les noms des entités.
#pragma once

#include <cstddef>
#include <cstdint>

namespace mp::sync::assess_trace {

constexpr std::size_t kMaxScripts = 96;

// Nom (décoré) du script `index`, enregistré par win/hooks à la pose.
void set_script_name(std::size_t index, const char* name);

// Depuis le détour, avant l'original : rend un jeton pour `after`. Ne lève jamais d'exception.
std::uint32_t before(std::size_t index, void* spu, void* self, void* other, long long arg) noexcept;
// Depuis le détour, après l'original.
void after(std::uint32_t token, long long result) noexcept;

// Frame : héros local et avatar (nul : aucun), puis journal des appels retenus.
void frame(void* hero, void* avatar);

}  // namespace mp::sync::assess_trace
