// Adaptateur ABI du combat (spike J2, story 3.0) : arme en main (Q5), avatar solide (Q6),
// PNJ natifs (Q3), mort interceptée (Q7). Détails et offsets : docs/re/spike-j2.md.
//
// Faits établis par désassemblage (Steam x64, janvier 2023) :
// - Les « vues » PS de Script.dll (PSInventory, PSSkills, PSRoutine, PSRigidBody…) ne sont que
//   l'`Entity` (8 octets) : `this` = Entity*. Chaque méthode refait GetPropertySet(type) et ne fait
//   rien (ou rend -1 / 0) si l'entité n'a pas ce PS. Types : inventaire 0x1f, combat 0x60,
//   routine 0x24, dégâts 0x29, corps rigide 0xd, déplacement 0x15, navigation 0x5.
// - Les propriétés imbriquées (PSRoutine::PropertyAIMode…) prennent elles aussi `this` = Entity*.
// - Les retours d'objets (Entity, Template, bCString) passent par un sret explicite (RDX) ;
//   un bCString ou un bCMatrix passé par valeur est un pointeur vers une copie, détruite par
//   l'appelé (PSRoutine::SetState appelle ~bCString).
//
// Toutes les fonctions sont protégées par SEH et retournent false en cas d'échec. Thread
// principal uniquement (hook OnProcess, AD-2), sauf `read_entity_ptr` (lecture mémoire seule)
// et `defeat` (exception documentée : appelée depuis le détour de Kill, même thread).
#pragma once

#include "game/entity.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mp::game {

constexpr int kInventoryPropertySet = 0x1f;
constexpr int kCombatPropertySet = 0x60;
constexpr int kRoutinePropertySet = 0x24;    // gCScriptRoutine_PS
constexpr int kDamagePropertySet = 0x29;     // gCDamage_PS (sur l'arme)
constexpr int kRigidBodyPropertySet = 0xd;
constexpr int kCharacterMovementPropertySet = 0x15;
constexpr int kNavigationPropertySet = 0x5;

constexpr int kCollisionGroupNpc = 4;        // eECollisionGroup (hypothèse de la spec, à vérifier)
constexpr int kBodyFlagsKinematic = 0x80 | 0x1;  // eERigidbody_Flag, ajoutés par OU (+0x90 du PS)
constexpr int kSkillModifierAdd = 0;         // gESkillModifier
constexpr int kSkillModifierMax = 1;
constexpr int kSkillModifierSet = 2;
constexpr int kAIModeDead = 12;              // Kill : AIMode = 12 (Entity::IsDead)
constexpr int kAIModeDefeated = 11;          // Defeat : AIMode = 11
constexpr int kAIModeRoutine = 2;            // héros vivant (1ᵉʳ test en jeu, 2026-10-20)

constexpr std::size_t kPropertyIdSize = 0x14;  // bCPropertyID (16 o de GUID en tête)

// Signature des fonctions de script de Script_Game.dll (Kill, Defeat) :
// (gCScriptProcessingUnit*, Entity* self, Entity* other, __int64). Nul : entité de l'unité.
using ScriptFn = long long (*)(void* spu, void* self, void* other, long long arg);
using RoutineOnProcessFn = void (*)(void* routine_ps);

// Résout en bloc les symboles du combat (Script.dll, Engine.dll, Game.dll, SharedBase.dll).
// Indépendante de resolve_entities (à appeler après). False et `missing` au premier échec.
bool resolve_combat(const char*& missing);
// Symboles d'inventaire de la session (story 3.2, AD-3) : arme en main du héros et de l'avatar.
// Script.dll et SharedBase.dll, en tout ou rien, indépendants de resolve_combat (sonde). Les
// enveloppes d'inventaire ci-dessous marchent après l'une ou l'autre. False et `missing` au
// premier échec. Après resolve_entities (destroy_template).
bool resolve_inventory(const char*& missing);
// Kill et Defeat (Script_Game.dll), facultatifs : module déjà chargé par le jeu, jamais par
// nous. False et `missing` (module ou symbole) en cas d'échec.
bool resolve_script_game(const char*& missing);

void* routine_on_process_address();  // gCScriptRoutine_PS::OnProcess (Game.dll)
void* kill_address();                // ::Kill (Script_Game.dll)
// ::DownOrDeadBy{MeleeWeapon,Projectile,Inferno} (Script_Game.dll), which = 0, 1, 2.
void* down_or_dead_address(int which);

// --- Q5 : arme et mode de combat ------------------------------------------------------
bool get_right_hold_stack(const Entity& e, int& out);      // -1 : rien ou pas d'inventaire
bool get_right_hold_item(const Entity& e, Entity& out);
bool get_inventory_template(const Entity& e, int stack, Template& out);  // à détruire (destroy_template)
bool create_items(const Entity& e, const Template& t, int count, int stack_type, int& out);
bool hold_right_stack(const Entity& e, int stack, bool& out);
bool equip_stack(const Entity& e, int stack, bool& out);
// PSInventory::IsStackEquiped(int) const (story 3.2b) : pile déjà équipée ? resolve_inventory seulement.
bool is_stack_equipped(const Entity& e, int stack, bool& out);
// Diagnostic (story 3.2b), en debug : chaque pile de l'inventaire (au plus 64), nom de son objet
// (GetTemplateItem + GetName), équipée ou non, emplacement (gEEquipSlot brut). False si les
// symboles facultatifs manquent ou sur SEH.
struct StackInfo {
    int index = 0;
    std::string name;
    bool equipped = false;
    int slot = 0;
};
bool inventory_stacks(const Entity& e, std::vector<StackInfo>& out);
bool get_combat_mode(const Entity& e, int& out);
bool get_entity_name(const Entity& e, std::string& out);   // Entity::GetName
// Template(const char* nom) (Script.dll) puis Template::IsValid. `out` est construit dès que
// l'appel rend true, même invalide : à détruire (destroy_template). resolve_inventory seulement.
bool template_from_name(const char* name, Template& out, bool& valid);
// PSInventory::FindStackIndex(Template const&, int après, bool hors emplacement) : première pile de
// ce template d'index > `après` (-1 : depuis 0) ; `bool` vrai saute les piles posées dans un
// emplacement (gCInventorySlot::GetSlot ≠ 0). Désassemblage (Steam x64, janvier 2023). Appelé
// avec (-1, false) : n'importe quelle pile. -1 : aucune. resolve_inventory seulement.
bool find_stack_index(const Entity& e, const Template& t, int& out);

// --- Q6 : collisions --------------------------------------------------------------------
bool get_collision_group(const Entity& e, int& out);
bool set_collision_group(const Entity& e, int group);
bool enable_collision(const Entity& e, bool on);
bool add_body_flag(const Entity& e, int flags);
bool remove_body_flag(const Entity& e, int flags);
bool set_body_pose(const Entity& e, const Matrix& pose);   // PSRigidBody::SetPose
bool enable_gravitation(const Entity& e, bool on);
bool is_gravitation_enabled(const Entity& e, bool& out);
bool set_goal_pose(const Entity& e, const Matrix& pose);   // PSCharacterMovement::SetGoalPose
bool set_dcc_enabled(const Entity& e, bool on);
bool get_dcc_radius(const Entity& e, float& out);

// --- Q3 : PNJ natifs -------------------------------------------------------------------
// eCDynamicEntity::GetID : copie des 0x14 octets du bCPropertyID.
bool get_entity_id(const Entity& e, std::uint8_t (&out)[kPropertyIdSize]);
// gCRuntimeInfo::GetEntity(bCPropertyID const&) (this inutilisé : nul).
bool runtime_entity_by_id(const std::uint8_t (&id)[kPropertyIdSize], void*& out);
bool get_hit_points(const Entity& e, int& out);
bool get_hit_points_max(const Entity& e, int& out);
bool set_hit_points(const Entity& e, int value, int modifier);
bool get_damage_amount(const Entity& weapon, long& out);     // gCDamage_PS de l'arme
bool set_damage_amount(const Entity& weapon, long amount);
bool do_damage(const Entity& weapon, const Entity& target, const Entity& attacker, bool& out);

// --- Q7 : mort et relève ---------------------------------------------------------------
bool get_ai_mode(const Entity& e, int& out);
bool get_last_ai_mode(const Entity& e, int& out);
bool set_ai_mode(const Entity& e, int mode);
bool set_routine_state(const Entity& e, const char* state);  // PSRoutine::SetState(bCString)
// Appelle ::Defeat(spu, self, other, arg). Depuis le détour de Kill uniquement.
bool defeat(void* spu, void* self, void* other, long long arg, long long& out);
// GUI2::PrintGameLogF (Script.dll) : une ligne dans le journal à l'écran du jeu (ASCII, 255 car.).
// False si le journal est désactivé dans le jeu ou sur SEH. Thread principal.
bool hud_print(const char* text);
// gCScriptProcessingUnit::GetOtherEntity (Game.dll) : victime d'un Kill dont `other` est nul.
// Accesseur sans effet de bord, appelé depuis le détour de Kill (même thread). Sous SEH.
bool spu_other_entity(void* spu, void*& out);
// *(void**)entity_object : eCEntity* d'un `Entity*` reçu par un script. Lecture protégée.
bool read_entity_ptr(const void* entity_object, void*& out);

}  // namespace mp::game
