// Adaptateur ABI des entités (Script.dll, Engine.dll), pour la sonde du spike J1.
//
// Faits établis par désassemblage (Steam x64, janvier 2023) :
// - `Entity` (Script.dll) = { eCDynamicEntity* } (8 octets), constructeur et destructeur vides.
// - Les statiques qui retournent un objet reçoivent le sret dans RCX ; les méthodes,
//   dans RDX (après `this`).
// - `Entity::Spawn(Template, bCMatrix const&, bool)` détruit lui-même le Template reçu
//   par valeur (??1eCTemplateEntityProxy) ; le bool appelle eCEntity::SetTemporary.
// - `eCAnimation_PS::PlayMotion(this, type 0..7, eSMotionPlayBackDesc*, bool)` lit la
//   ressource de motion dans le PS lui-même, à `this + 0x120 + type * 0x10`.
//
// Toutes les fonctions sont protégées par SEH et retournent false en cas d'échec.
// À n'appeler que depuis le thread principal (hook OnProcess, AD-2), sauf
// `copy_bytes` et `read_motion_resource`, qui ne font que lire la mémoire.
#pragma once

#include <cstddef>
#include <cstdint>

namespace mp::game {

struct Entity {
    void* ptr = nullptr;  // eCDynamicEntity*
};
static_assert(sizeof(Entity) == 8, "Entity doit faire 8 octets (x64)");

struct Template {
    void* proxy = nullptr;  // enveloppe de eCTemplateEntityProxy (compteur de références)
};
static_assert(sizeof(Template) == 8, "Template doit faire 8 octets (x64)");

// bCMatrix : 4×4 float. [ASSUMPTION] translation en m[12..14], axe avant en m[8..10],
// unités en centimètres : à vérifier dans le log de la sonde.
struct Matrix {
    float m[16];
};
static_assert(sizeof(Matrix) == 64, "bCMatrix doit faire 64 octets");

constexpr int kAnimationPropertySet = 0x52;  // eCAnimation_PS::GetPropertySetType
constexpr int kMotionTypeCount = 8;          // PlayMotion ignore type > 7

using OnProcessFn = void (*)(void* application);
using PlayMotionFn = void (*)(void* animation_ps, int type, void* desc, bool flag);

// Résout en bloc les symboles des entités (Script.dll, Engine.dll). Script.dll est
// chargée si besoin. Retourne false et renseigne `missing` au premier échec.
bool resolve_entities(const char*& missing);

// Adresses des fonctions à hooker (valides après resolve_entities).
void* on_process_address();
void* play_motion_address();

bool get_player(Entity& out);  // out.ptr == nullptr : pas de joueur (menu, chargement)
bool get_pose(const Entity& e, Matrix& out);
bool get_template(const Entity& e, Template& out);
void destroy_template(Template& t);
// `tmpl` est consommé (détruit par le jeu) que l'appel réussisse ou non.
bool spawn(Template& tmpl, const Matrix& pose, bool temporary, Entity& out);
bool get_animation_ps(const Entity& e, void*& out);
bool play_motion(void* animation_ps, int type, void* desc, bool flag);

// Lectures mémoire protégées, utilisables depuis le hook PlayMotion.
bool copy_bytes(const void* src, void* dst, std::size_t n);
bool read_motion_resource(const void* animation_ps, int type, void*& out);

}  // namespace mp::game
