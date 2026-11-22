// Portées du moteur (story 3.4f) : sphère de traitement des entités (« ROI ») et portées de cache.
//
// Engine.dll : le gestionnaire de scène est le module « eCSceneAdmin » (eCModuleAdmin::FindModule,
// comme Script.dll pour ses modules ; vtable ??_7eCSceneAdmin@@6B@ vérifiée), son eCPrefetchAdmin est
// rendu par eCSceneAdmin::GetPrefetchAdmin (this + 0xe8). Désassemblage (Engine.dll) :
// - eCSceneAdmin::ApplyConfiguration lit /Engine/Scene/EntityROI (4000 = 40 m dans ConfigDefault.xml)
//   dans la sphère ROI du prefetch admin (+0x8 : rayon, +0xc : centre) et en fait la portée de cache
//   par défaut (SetDefaultCacheInRange, +0x28) ;
// - UpdateProcessingRange recentre la sphère sur la caméra à chaque frame, OnTestProcessingRange
//   teste chaque entité contre elle : hors de la sphère, une entité n'est pas traitée (ni IA, ni
//   gCInteractionAdmin, donc absente d'Entity::ms_arrNPCs) ;
// - SetROISphere (rayon et centre) refait la portée de cache par défaut ; GetPrefetchRange /
//   SetPrefetchRange : portée de préchargement (+0x18).
// La console du jeu documente : la portée de traitement doit rester inférieure à la portée d'entrée.
// Tout accès est sous SEH (AD-2), sur le thread principal.
#pragma once

#include <string>

namespace mp::game::scene {

struct Ranges {
    float roi = 0.0f;       // rayon de la sphère de traitement (unités du monde = cm)
    float cache_in = 0.0f;  // portée de cache par défaut
    float prefetch = 0.0f;  // portée de préchargement
};

// Lit les portées. false : symboles absents, module introuvable, vtable inattendue ou SEH (`error`).
bool read_ranges(Ranges& out, std::string& error);

// Règle la sphère de traitement à `roi` (centre gardé ; la portée de cache par défaut suit, comme dans
// SetROISphere) et relève la portée de préchargement à `prefetch` si elle est plus petite.
bool set_ranges(float roi, float prefetch, std::string& error);

}  // namespace mp::game::scene
