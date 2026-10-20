// Adaptateur ABI des entités (Script.dll, Engine.dll) : sonde du spike J1 et avatars.
//
// Faits établis par désassemblage (Steam x64, janvier 2023) :
// - `Entity` (Script.dll) = { eCDynamicEntity* } (8 octets), constructeur et destructeur vides.
// - Les statiques qui retournent un objet reçoivent le sret dans RCX ; les méthodes,
//   dans RDX (après `this`).
// - `Entity::Spawn(Template, bCMatrix const&, bool)` détruit lui-même le Template reçu
//   par valeur (??1eCTemplateEntityProxy) ; le bool appelle eCEntity::SetTemporary.
// - `Entity::Kill()` saute vers `eCEntity::Kill` (Engine.dll, 0x1806a5dd0) : sans effet si le
//   bit 0x8 de `+0x28` est posé ; sinon l'entité rejoint une liste du moteur (destruction
//   différée), le bit est posé et `KillIntern` est appliqué aux enfants.
// - `eCAnimation_PS::PlayMotion(this, type 0..7, eSMotionPlayBackDesc*, bool)` lit la
//   ressource de motion dans le PS lui-même, à `this + 0x120 + type * 0x10`.
//
// - Le choix de l'animation se fait avant PlayMotion, par
//   `eCAnimation_PS::SetMotion(type, eCMotionResource2*)`, qui range la ressource dans
//   cet emplacement (compteur de références à `res + 0x24`, comme eCResource2 : même
//   pointeur, sans décalage). `eCResource2::GetFileTitleA()` donne son nom.
//
// Toutes les fonctions sont protégées par SEH et retournent false en cas d'échec.
// À n'appeler que depuis le thread principal (hook OnProcess, AD-2), sauf
// `copy_bytes` et `read_motion_resource`, qui ne font que lire la mémoire.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

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
// Réglages de l'enveloppe EMotionFX (eCWrapper_emfx3ActorInstance, Engine.dll), appelés à chaque
// frame par le contrôleur de déplacement (gCCharacterMovement_PS, Game.dll) pour la locomotion
// (story 2.4b). x64 : this RCX, type EDX, flottants en XMM2/XMM3, bool en R8B/R9B selon la position.
using SetPlaySpeedFn = void (*)(void* wrapper, int type, float speed);
using SetTargetWeightFn = void (*)(void* wrapper, int type, float weight, float time);
using SwitchFadeOutFn = void (*)(void* wrapper, int type, bool on, float time);
using WrapperStopMotionFn = void (*)(void* wrapper, int type, float time, bool flag);
// eCAnimation_PS::SetRagDollEnabled(bool actif, bool, bCVector*) ; bCVector = 3 f32.
using SetRagdollFn = void (*)(void* animation_ps, bool on, bool flag, const float* vector);

// Résout en bloc les symboles des entités (Script.dll, Engine.dll, Game.dll). Script.dll est
// chargée si besoin. Retourne false et renseigne `missing` au premier échec.
bool resolve_entities(const char*& missing);

// Adresses des fonctions à hooker (valides après resolve_entities).
void* on_process_address();
void* play_motion_address();
void* set_ragdoll_address();
void* set_play_speed_address();
void* set_target_weight_address();
void* switch_fade_out_address();
void* wrapper_stop_motion_address();
// eCAnimation_PS::GetActorWrapper : enveloppe EMotionFX du PS (nul possible).
bool get_actor_wrapper(void* animation_ps, void*& out);
// Rejeu des réglages de locomotion (story 2.4b) ; passent par le hook (ReplayGuard requis sur
// une réplique). Sous SEH.
bool set_target_weight(void* wrapper, int type, float weight, float time);
bool set_play_speed(void* wrapper, int type, float speed);
bool switch_fade_out(void* wrapper, int type, bool on, float time);
bool wrapper_stop_motion(void* wrapper, int type, float time, bool flag);

bool get_player(Entity& out);  // out.ptr == nullptr : pas de joueur (menu, chargement)
bool get_pose(const Entity& e, Matrix& out);
bool set_pose(const Entity& e, const Matrix& pose);  // Entity::SetPose (SetToWorldMatrix seul)
// Entity::MoveTo(bCMatrix) : SetToWorldMatrix + recalage des property sets 0xd et 5
// (contrôleur de personnage / physique), pour que tout l'entité suive.
bool move_to(const Entity& e, const Matrix& pose);
bool get_template(const Entity& e, Template& out);
void destroy_template(Template& t);
// `tmpl` est consommé (détruit par le jeu) que l'appel réussisse ou non.
bool spawn(Template& tmpl, const Matrix& pose, bool temporary, Entity& out);
bool get_animation_ps(const Entity& e, void*& out);
// eCEntity::GetPropertySet(type) (variante const) : nul si l'entité n'a pas ce PS.
bool get_property_set(const Entity& e, int type, void*& out);
bool play_motion(void* animation_ps, int type, void* desc, bool flag);
// Range `resource` (eCMotionResource2*) dans l'emplacement `type` du PS.
bool set_motion(void* animation_ps, int type, void* resource);
// eCAnimation_PS::SetRagDollEnabled : `vector` = 3 f32 (bCVector) ou nul.
bool set_ragdoll(void* animation_ps, bool on, bool flag, const float* vector);
// eCAnimation_PS::GetRagDollEnabled (bit 2 de +0x22C).
bool get_ragdoll(const void* animation_ps, bool& out);
// Nom de la ressource de motion (eCResource2::GetFileTitleA), 255 caractères au plus.
bool get_resource_name(const void* resource, std::string& out);
// eCResource2::GetMotion(nom) (Engine.dll, statique) : ressource du cache, créée au besoin.
// Le pointeur rendu est emprunté : ne jamais le libérer (SetMotion prend sa référence).
// out == nullptr : nom inconnu. Retourne false sur SEH ou nom vide/trop long.
bool get_motion(const std::string& name, void*& out);

// Entity::Kill (Script.dll) : destruction différée par le moteur. Ne plus toucher à l'entité
// ensuite. Jamais sur une entité déjà détruite (fin de monde) : la mémoire n'est plus valide.
bool kill(const Entity& e);
// Entity::IsDead (Script.dll) : le PS 0x24 de l'entité, à l'offset +0xC0, vaut 12.
bool is_dead(const Entity& e, bool& out);
// gCSession::GetInstance() (pointeur global, nul possible) puis gCSession::IsGameRunning
// (octet à +0x110), Game.dll. Instance nulle : out = false.
bool game_running(bool& out);

// Entity::IsProcessingRangeEntered : l'entité est-elle dans la zone que le moteur met à jour ?
bool is_processing_range_entered(const Entity& e, bool& out);
// Entity::GetNPCs (copie de Entity::ms_arrNPCs). Le tampon du tableau renvoyé est
// alloué par le jeu et son destructeur n'est pas exporté : il fuit (quelques Ko par
// appel). Réservé à la sonde, sur action de l'utilisateur.
bool get_npcs(std::vector<Entity>& out);

// Lectures mémoire protégées, utilisables depuis le hook PlayMotion.
bool copy_bytes(const void* src, void* dst, std::size_t n);
bool read_motion_resource(const void* animation_ps, int type, void*& out);

}  // namespace mp::game
