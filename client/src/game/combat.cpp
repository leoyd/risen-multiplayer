#include "game/combat.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstring>
#include <string>

namespace mp::game {

namespace {

// Script.dll — vues PS : this = Entity*.
using IntGetterFn = int (*)(const Entity* self);
using BoolGetterFn = bool (*)(const Entity* self);
using FloatGetterFn = float (*)(const Entity* self);
using VoidIntFn = void (*)(const Entity* self, int value);
using VoidBoolFn = void (*)(const Entity* self, bool value);
using BoolIntFn = bool (*)(const Entity* self, int value);
using GetEntityRetFn = Entity* (*)(const Entity* self, Entity* sret);
using GetTemplateAtFn = Template* (*)(const Entity* self, Template* sret, int stack);
using CreateItemsFn = int (*)(const Entity* self, const Template* tmpl, int count, int stack_type);
using MatrixByValueFn = void (*)(const Entity* self, Matrix* by_value);
using MatrixByRefFn = void (*)(const Entity* self, const Matrix* pose);
using SetHitPointsFn = void (*)(const Entity* self, int value, int modifier);
using DoDamageFn = bool (*)(const Entity* weapon, const Entity* target, const Entity* attacker);
using AssignAIModeFn = void* (*)(const Entity* self, const int* mode);
using SetStateFn = void (*)(const Entity* self, void* bcstring_by_value);
using GetNameFn = void* (*)(const Entity* self, void* bcstring_sret);
// Engine.dll
using GetIdFn = const void* (*)(const void* dynamic_entity);
// Game.dll
using RuntimeGetEntityFn = void* (*)(void* runtime_info, const void* property_id);
using GetDamageAmountFn = const long* (*)(const void* damage_ps);
using SetDamageAmountFn = void (*)(void* damage_ps, const long* amount);
// Script.dll : GUI2::PrintGameLogF(type, format, ...), `this` = la donnée exportée `gui2`, dont
// l'octet 0 active le journal à l'écran (sinon l'appel ne fait rien).
using PrintGameLogFFn = void (*)(void* gui, int type, const char* format, ...);
// Script.dll : Template (8 octets, eCTemplateEntityProxy) et recherche de pile (story 3.2).
using TemplateCtorFn = void* (*)(Template* self, const char* name);
using TemplateIsValidFn = bool (*)(const Template* self);
using FindStackIndexFn = int (*)(const Entity* self, const Template* tmpl, int after, bool unslotted_only);
// SharedBase.dll
using BCStringCtorFn = void* (*)(void* self, const char* text);
using BCStringDtorFn = void (*)(void* self);

constexpr const char* kGetRightHoldStack = "?GetRightHoldStack@PSInventory@@QEBAHXZ";
constexpr const char* kGetRightHoldItem = "?GetRightHoldItem@PSInventory@@QEBA?AVEntity@@XZ";
constexpr const char* kInventoryGetTemplate = "?GetTemplate@PSInventory@@QEBA?AVTemplate@@H@Z";
constexpr const char* kCreateItems = "?CreateItems@PSInventory@@QEAAHAEBVTemplate@@HW4gEStackType@@@Z";
constexpr const char* kHoldRightStack = "?HoldRightStack@PSInventory@@QEAA_NH@Z";
constexpr const char* kEquipStack = "?EquipStack@PSInventory@@QEAA_NH@Z";
constexpr const char* kGetCombatMode = "?GetCombatMode@PSCombatSystem@@QEBA?AW4gECombatMode@@XZ";
constexpr const char* kGetName = "?GetName@Entity@@QEBA?AVbCString@@XZ";
constexpr const char* kGetCollisionGroup = "?GetCollisionGroup@Entity@@QEBA?AW4eECollisionGroup@@XZ";
constexpr const char* kSetCollisionGroup = "?SetCollisionGroup@Entity@@QEAAXW4eECollisionGroup@@@Z";
constexpr const char* kEnableCollision = "?EnableCollision@Entity@@QEAAX_N@Z";
constexpr const char* kAddBodyFlag = "?AddBodyFlag@PSRigidBody@@QEAAXW4eERigidbody_Flag@@@Z";
constexpr const char* kRemoveBodyFlag = "?RemoveBodyFlag@PSRigidBody@@QEAAXW4eERigidbody_Flag@@@Z";
constexpr const char* kBodySetPose = "?SetPose@PSRigidBody@@QEAAXVbCMatrix@@@Z";
constexpr const char* kEnableGravitation = "?EnableGravitation@PSCharacterMovement@@QEAAX_N@Z";
constexpr const char* kIsGravitationEnabled = "?IsGravitationEnabled@PSCharacterMovement@@QEBA_NXZ";
constexpr const char* kSetGoalPose = "?SetGoalPose@PSCharacterMovement@@QEAAXAEBVbCMatrix@@@Z";
constexpr const char* kSetDCCEnabled = "?SetDCCEnabled@PSNavigation@@QEAAX_N@Z";
constexpr const char* kGetDCCRadius = "?GetDCCRadius@PSNavigation@@QEBAMXZ";
constexpr const char* kGetHitPoints = "?GetHitPoints@PSSkills@@QEBAHXZ";
constexpr const char* kGetHitPointsMax = "?GetHitPointsMax@PSSkills@@QEBAHXZ";
constexpr const char* kSetHitPoints = "?SetHitPoints@PSSkills@@QEAAXHW4gESkillModifier@@@Z";
constexpr const char* kDoDamage = "?DoDamage@PSDamage@@QEAA_NAEAVEntity@@0@Z";
constexpr const char* kGetAIMode = "??BPropertyAIMode@PSRoutine@@QEBA?BW4gEAIMode@@XZ";
constexpr const char* kGetLastAIMode = "??BPropertyLastAIMode@PSRoutine@@QEBA?BW4gEAIMode@@XZ";
constexpr const char* kSetAIMode = "??4PropertyAIMode@PSRoutine@@QEAAAEAV01@AEBW4gEAIMode@@@Z";
constexpr const char* kSetState = "?SetState@PSRoutine@@QEAAXVbCString@@@Z";
constexpr const char* kGetId = "?GetID@eCDynamicEntity@@UEBAAEBVbCPropertyID@@XZ";
constexpr const char* kRuntimeGetEntity = "?GetEntity@gCRuntimeInfo@@QEAAPEAVeCEntity@@AEBVbCPropertyID@@@Z";
constexpr const char* kGetDamageAmount = "?GetDamageAmount@gCDamage_PS@@QEBAAEBJXZ";
constexpr const char* kSetDamageAmount = "?SetDamageAmount@gCDamage_PS@@QEAAXAEBJ@Z";
constexpr const char* kRoutineOnProcess = "?OnProcess@gCScriptRoutine_PS@@UEAAXXZ";
constexpr const char* kBCStringCtor = "??0bCString@@QEAA@PEBD@Z";
constexpr const char* kBCStringDtor = "??1bCString@@QEAA@XZ";
constexpr const char* kTemplateCtor = "??0Template@@QEAA@PEBD@Z";
constexpr const char* kTemplateIsValid = "?IsValid@Template@@QEBA_NXZ";
constexpr const char* kFindStackIndex = "?FindStackIndex@PSInventory@@QEBAHAEBVTemplate@@H_N@Z";
constexpr const char* kGui2 = "gui2";
constexpr const char* kPrintGameLogF = "?PrintGameLogF@GUI2@@QEAAXW4gELogMessageType@@PEBDZZ";
// Décision « à terre ou mort » sur un coup (self = ce qui frappe, other = la victime ; rend 0
// sans rien faire si la victime a encore des PV). 5ᵉ test : point d'interception de la mort.
constexpr const char* kDownOrDead[3] = {
    "?DownOrDeadByMeleeWeapon@@YA_JPEAVgCScriptProcessingUnit@@PEAX1_J@Z",
    "?DownOrDeadByProjectile@@YA_JPEAVgCScriptProcessingUnit@@PEAX1_J@Z",
    "?DownOrDeadByInferno@@YA_JPEAVgCScriptProcessingUnit@@PEAX1_J@Z"};
constexpr const char* kKill = "?Kill@@YA_JPEAVgCScriptProcessingUnit@@PEAX1_J@Z";
constexpr const char* kDefeat = "?Defeat@@YA_JPEAVgCScriptProcessingUnit@@PEAX1_J@Z";
// Game.dll : `this + 0x150` (proxy) → eCEntityProxy::GetEntity. Accesseur, sans effet de bord.
constexpr const char* kSpuGetOther = "?GetOtherEntity@gCScriptProcessingUnit@@QEAAPEAVeCGeometryEntity@@XZ";
using SpuGetEntityFn = void* (*)(void* spu);

struct CombatAbi {
    IntGetterFn get_right_hold_stack = nullptr;
    GetEntityRetFn get_right_hold_item = nullptr;
    GetTemplateAtFn inventory_get_template = nullptr;
    CreateItemsFn create_items = nullptr;
    BoolIntFn hold_right_stack = nullptr;
    BoolIntFn equip_stack = nullptr;
    IntGetterFn get_combat_mode = nullptr;
    GetNameFn get_name = nullptr;
    IntGetterFn get_collision_group = nullptr;
    VoidIntFn set_collision_group = nullptr;
    VoidBoolFn enable_collision = nullptr;
    VoidIntFn add_body_flag = nullptr;
    VoidIntFn remove_body_flag = nullptr;
    MatrixByValueFn body_set_pose = nullptr;
    VoidBoolFn enable_gravitation = nullptr;
    BoolGetterFn is_gravitation_enabled = nullptr;
    MatrixByRefFn set_goal_pose = nullptr;
    VoidBoolFn set_dcc_enabled = nullptr;
    FloatGetterFn get_dcc_radius = nullptr;
    IntGetterFn get_hit_points = nullptr;
    IntGetterFn get_hit_points_max = nullptr;
    SetHitPointsFn set_hit_points = nullptr;
    DoDamageFn do_damage = nullptr;
    IntGetterFn get_ai_mode = nullptr;
    IntGetterFn get_last_ai_mode = nullptr;
    AssignAIModeFn set_ai_mode = nullptr;
    SetStateFn set_state = nullptr;
    GetIdFn get_id = nullptr;
    RuntimeGetEntityFn runtime_get_entity = nullptr;
    GetDamageAmountFn get_damage_amount = nullptr;
    SetDamageAmountFn set_damage_amount = nullptr;
    void* routine_on_process = nullptr;
    BCStringCtorFn bcstring_ctor = nullptr;
    BCStringDtorFn bcstring_dtor = nullptr;
    void* gui2 = nullptr;
    PrintGameLogFFn print_game_log_f = nullptr;
};

CombatAbi g_abi;
bool g_resolved = false;

// Inventaire de la session (story 3.2) : sous-ensemble résolu à part, sans la sonde.
struct InventoryAbi {
    IntGetterFn get_right_hold_stack = nullptr;
    GetEntityRetFn get_right_hold_item = nullptr;
    CreateItemsFn create_items = nullptr;
    BoolIntFn hold_right_stack = nullptr;
    BoolIntFn equip_stack = nullptr;
    GetNameFn get_name = nullptr;
    BCStringDtorFn bcstring_dtor = nullptr;
    TemplateCtorFn template_ctor = nullptr;
    TemplateIsValidFn template_is_valid = nullptr;
    FindStackIndexFn find_stack_index = nullptr;
};
InventoryAbi g_inv;
bool g_inv_resolved = false;

// Pointeur d'inventaire : celui de resolve_inventory, sinon celui de resolve_combat, sinon nul.
template <typename Fn>
Fn pick(Fn inventory, Fn combat) {
    return g_inv_resolved ? inventory : (g_resolved ? combat : nullptr);
}
ScriptFn g_kill = nullptr;
ScriptFn g_defeat = nullptr;
SpuGetEntityFn g_spu_get_other = nullptr;
ScriptFn g_down_or_dead[3] = {};

template <typename Fn>
bool lookup(HMODULE module, const char* name, Fn& out, const char*& missing) {
    out = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(module, name)));
    if (out == nullptr) missing = name;
    return out != nullptr;
}

}  // namespace

bool resolve_combat(const char*& missing) {
    HMODULE script = GetModuleHandleW(L"Script.dll");
    HMODULE engine = GetModuleHandleW(L"Engine.dll");
    HMODULE game = GetModuleHandleW(L"Game.dll");
    HMODULE shared_base = GetModuleHandleW(L"SharedBase.dll");
    if (script == nullptr) {
        missing = "Script.dll";
        return false;
    }
    if (engine == nullptr || game == nullptr || shared_base == nullptr) {
        missing = engine == nullptr ? "Engine.dll" : (game == nullptr ? "Game.dll" : "SharedBase.dll");
        return false;
    }
    CombatAbi a;
    const bool ok = lookup(script, kGetRightHoldStack, a.get_right_hold_stack, missing) &&
                    lookup(script, kGetRightHoldItem, a.get_right_hold_item, missing) &&
                    lookup(script, kInventoryGetTemplate, a.inventory_get_template, missing) &&
                    lookup(script, kCreateItems, a.create_items, missing) &&
                    lookup(script, kHoldRightStack, a.hold_right_stack, missing) &&
                    lookup(script, kEquipStack, a.equip_stack, missing) &&
                    lookup(script, kGetCombatMode, a.get_combat_mode, missing) &&
                    lookup(script, kGetName, a.get_name, missing) &&
                    lookup(script, kGetCollisionGroup, a.get_collision_group, missing) &&
                    lookup(script, kSetCollisionGroup, a.set_collision_group, missing) &&
                    lookup(script, kEnableCollision, a.enable_collision, missing) &&
                    lookup(script, kAddBodyFlag, a.add_body_flag, missing) &&
                    lookup(script, kRemoveBodyFlag, a.remove_body_flag, missing) &&
                    lookup(script, kBodySetPose, a.body_set_pose, missing) &&
                    lookup(script, kEnableGravitation, a.enable_gravitation, missing) &&
                    lookup(script, kIsGravitationEnabled, a.is_gravitation_enabled, missing) &&
                    lookup(script, kSetGoalPose, a.set_goal_pose, missing) &&
                    lookup(script, kSetDCCEnabled, a.set_dcc_enabled, missing) &&
                    lookup(script, kGetDCCRadius, a.get_dcc_radius, missing) &&
                    lookup(script, kGetHitPoints, a.get_hit_points, missing) &&
                    lookup(script, kGetHitPointsMax, a.get_hit_points_max, missing) &&
                    lookup(script, kSetHitPoints, a.set_hit_points, missing) &&
                    lookup(script, kDoDamage, a.do_damage, missing) &&
                    lookup(script, kGetAIMode, a.get_ai_mode, missing) &&
                    lookup(script, kGetLastAIMode, a.get_last_ai_mode, missing) &&
                    lookup(script, kSetAIMode, a.set_ai_mode, missing) &&
                    lookup(script, kSetState, a.set_state, missing) &&
                    lookup(engine, kGetId, a.get_id, missing) &&
                    lookup(game, kRuntimeGetEntity, a.runtime_get_entity, missing) &&
                    lookup(game, kGetDamageAmount, a.get_damage_amount, missing) &&
                    lookup(game, kSetDamageAmount, a.set_damage_amount, missing) &&
                    lookup(game, kRoutineOnProcess, a.routine_on_process, missing) &&
                    lookup(shared_base, kBCStringCtor, a.bcstring_ctor, missing) &&
                    lookup(shared_base, kBCStringDtor, a.bcstring_dtor, missing) &&
                    lookup(script, kGui2, a.gui2, missing) &&
                    lookup(script, kPrintGameLogF, a.print_game_log_f, missing);
    if (!ok) return false;
    g_abi = a;
    g_resolved = true;
    return true;
}

bool resolve_inventory(const char*& missing) {
    HMODULE script = GetModuleHandleW(L"Script.dll");
    HMODULE shared_base = GetModuleHandleW(L"SharedBase.dll");
    if (script == nullptr || shared_base == nullptr) {
        missing = script == nullptr ? "Script.dll" : "SharedBase.dll";
        return false;
    }
    InventoryAbi a;
    const bool ok = lookup(script, kGetRightHoldStack, a.get_right_hold_stack, missing) &&
                    lookup(script, kGetRightHoldItem, a.get_right_hold_item, missing) &&
                    lookup(script, kCreateItems, a.create_items, missing) &&
                    lookup(script, kHoldRightStack, a.hold_right_stack, missing) &&
                    lookup(script, kEquipStack, a.equip_stack, missing) &&
                    lookup(script, kGetName, a.get_name, missing) &&
                    lookup(script, kTemplateCtor, a.template_ctor, missing) &&
                    lookup(script, kTemplateIsValid, a.template_is_valid, missing) &&
                    lookup(script, kFindStackIndex, a.find_stack_index, missing) &&
                    lookup(shared_base, kBCStringDtor, a.bcstring_dtor, missing);
    if (!ok) return false;
    g_inv = a;
    g_inv_resolved = true;
    return true;
}

bool resolve_script_game(const char*& missing) {
    // Jamais chargée par nous : son DllMain et son ScriptInit restent à l'initiative du jeu.
    // [ASSUMPTION] le jeu la charge avant Script_Multiplayer (ordre alphabétique ?) : le log
    // d'installation le dira (Q7 inactive sinon).
    HMODULE module = GetModuleHandleW(L"Script_Game.dll");
    if (module == nullptr) {
        missing = "Script_Game.dll";
        return false;
    }
    HMODULE game = GetModuleHandleW(L"Game.dll");
    if (game == nullptr) {
        missing = "Game.dll";
        return false;
    }
    ScriptFn kill = nullptr;
    ScriptFn defeat = nullptr;
    SpuGetEntityFn get_other = nullptr;
    if (!lookup(module, kKill, kill, missing) || !lookup(module, kDefeat, defeat, missing) ||
        !lookup(game, kSpuGetOther, get_other, missing)) {
        return false;
    }
    ScriptFn down_or_dead[3] = {};
    for (int i = 0; i < 3; ++i) {
        if (!lookup(module, kDownOrDead[i], down_or_dead[i], missing)) return false;
    }
    for (int i = 0; i < 3; ++i) g_down_or_dead[i] = down_or_dead[i];
    g_kill = kill;
    g_defeat = defeat;
    g_spu_get_other = get_other;
    return true;
}

void* routine_on_process_address() { return g_abi.routine_on_process; }
void* kill_address() { return reinterpret_cast<void*>(g_kill); }
void* down_or_dead_address(int which) {
    return which >= 0 && which < 3 ? reinterpret_cast<void*>(g_down_or_dead[which]) : nullptr;
}

// --- Appels génériques -------------------------------------------------------------------
// Aucun objet C++ à détruire dans un cadre __try.

namespace {

// Les pointeurs sont nuls tant que rien n'est résolu : le test de `fn` suffit.
bool call_int(IntGetterFn fn, const Entity& e, int& out) {
    out = 0;
    if (fn == nullptr || e.ptr == nullptr) return false;
    __try {
        out = fn(&e);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out = 0;
        return false;
    }
}

bool call_bool_getter(BoolGetterFn fn, const Entity& e, bool& out) {
    out = false;
    if (fn == nullptr || e.ptr == nullptr) return false;
    __try {
        out = fn(&e);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out = false;
        return false;
    }
}

bool call_void_int(VoidIntFn fn, const Entity& e, int value) {
    if (fn == nullptr || e.ptr == nullptr) return false;
    __try {
        fn(&e, value);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool call_void_bool(VoidBoolFn fn, const Entity& e, bool value) {
    if (fn == nullptr || e.ptr == nullptr) return false;
    __try {
        fn(&e, value);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool call_bool_int(BoolIntFn fn, const Entity& e, int value, bool& out) {
    out = false;
    if (fn == nullptr || e.ptr == nullptr) return false;
    __try {
        out = fn(&e, value);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out = false;
        return false;
    }
}

void* property_set(const Entity& e, int type) {
    void* ps = nullptr;
    return get_property_set(e, type, ps) ? ps : nullptr;  // game/entity
}

// bCString : un seul pointeur vers un texte terminé par zéro (ou nul).
__declspec(noinline) std::size_t raw_copy_cstr(const char* src, char* dst, std::size_t cap) {
    std::size_t n = 0;
    if (src != nullptr) {
        while (n < cap && src[n] != '\0') {
            dst[n] = src[n];
            ++n;
        }
    }
    return n;
}

__declspec(noinline) void raw_copy(void* dst, const void* src, std::size_t n) { std::memcpy(dst, src, n); }

bool name_into(GetNameFn get_name, BCStringDtorFn dtor, const Entity& e, char* buf, std::size_t cap,
               std::size_t& len) {
    void* text = nullptr;  // bCString retourné par valeur (sret), détruit ici
    bool ok = false;
    __try {
        get_name(&e, &text);
        len = raw_copy_cstr(static_cast<const char*>(text), buf, cap);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    __try {
        if (text != nullptr) dtor(&text);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return ok;
}

}  // namespace

// --- Q5 ------------------------------------------------------------------------------------

bool get_right_hold_stack(const Entity& e, int& out) {
    return call_int(pick(g_inv.get_right_hold_stack, g_abi.get_right_hold_stack), e, out);
}

bool get_right_hold_item(const Entity& e, Entity& out) {
    out.ptr = nullptr;
    const GetEntityRetFn fn = pick(g_inv.get_right_hold_item, g_abi.get_right_hold_item);
    if (fn == nullptr || e.ptr == nullptr) return false;
    __try {
        fn(&e, &out);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out.ptr = nullptr;
        return false;
    }
}

bool get_inventory_template(const Entity& e, int stack, Template& out) {
    out.proxy = nullptr;
    if (!g_resolved || e.ptr == nullptr) return false;
    __try {
        g_abi.inventory_get_template(&e, &out, stack);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out.proxy = nullptr;
        return false;
    }
}

bool create_items(const Entity& e, const Template& t, int count, int stack_type, int& out) {
    out = -1;
    const CreateItemsFn fn = pick(g_inv.create_items, g_abi.create_items);
    if (fn == nullptr || e.ptr == nullptr) return false;
    __try {
        out = fn(&e, &t, count, stack_type);  // Template par référence : reste à nous
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out = -1;
        return false;
    }
}

bool hold_right_stack(const Entity& e, int stack, bool& out) {
    return call_bool_int(pick(g_inv.hold_right_stack, g_abi.hold_right_stack), e, stack, out);
}

bool equip_stack(const Entity& e, int stack, bool& out) {
    return call_bool_int(pick(g_inv.equip_stack, g_abi.equip_stack), e, stack, out);
}

bool get_combat_mode(const Entity& e, int& out) { return call_int(g_abi.get_combat_mode, e, out); }

bool get_entity_name(const Entity& e, std::string& out) {
    out.clear();
    const GetNameFn get_name = pick(g_inv.get_name, g_abi.get_name);
    const BCStringDtorFn dtor = pick(g_inv.bcstring_dtor, g_abi.bcstring_dtor);
    if (get_name == nullptr || dtor == nullptr || e.ptr == nullptr) return false;
    char buf[255];
    std::size_t len = 0;
    if (!name_into(get_name, dtor, e, buf, sizeof buf, len)) return false;
    out.assign(buf, len);
    return true;
}

bool template_from_name(const char* name, Template& out, bool& valid) {
    out.proxy = nullptr;
    valid = false;
    if (!g_inv_resolved || name == nullptr) return false;
    __try {
        g_inv.template_ctor(&out, name);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out.proxy = nullptr;  // construction inachevée : rien à détruire de fiable
        return false;
    }
    __try {
        valid = g_inv.template_is_valid(&out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        valid = false;
    }
    return true;
}

bool find_stack_index(const Entity& e, const Template& t, int& out) {
    out = -1;
    if (!g_inv_resolved || e.ptr == nullptr) return false;
    __try {
        out = g_inv.find_stack_index(&e, &t, -1, false);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out = -1;
        return false;
    }
}

// --- Q6 ------------------------------------------------------------------------------------

bool get_collision_group(const Entity& e, int& out) { return call_int(g_abi.get_collision_group, e, out); }
bool set_collision_group(const Entity& e, int group) { return call_void_int(g_abi.set_collision_group, e, group); }
bool enable_collision(const Entity& e, bool on) { return call_void_bool(g_abi.enable_collision, e, on); }
bool add_body_flag(const Entity& e, int flags) { return call_void_int(g_abi.add_body_flag, e, flags); }
bool remove_body_flag(const Entity& e, int flags) { return call_void_int(g_abi.remove_body_flag, e, flags); }

bool set_body_pose(const Entity& e, const Matrix& pose) {
    if (!g_resolved || e.ptr == nullptr) return false;
    Matrix copy = pose;  // passage par valeur : copie de l'appelant (bCMatrix trivial)
    __try {
        g_abi.body_set_pose(&e, &copy);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool enable_gravitation(const Entity& e, bool on) { return call_void_bool(g_abi.enable_gravitation, e, on); }
bool is_gravitation_enabled(const Entity& e, bool& out) {
    return call_bool_getter(g_abi.is_gravitation_enabled, e, out);
}

bool set_goal_pose(const Entity& e, const Matrix& pose) {
    if (!g_resolved || e.ptr == nullptr) return false;
    __try {
        g_abi.set_goal_pose(&e, &pose);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool set_dcc_enabled(const Entity& e, bool on) { return call_void_bool(g_abi.set_dcc_enabled, e, on); }

bool get_dcc_radius(const Entity& e, float& out) {
    out = 0.0f;
    if (!g_resolved || e.ptr == nullptr) return false;
    __try {
        out = g_abi.get_dcc_radius(&e);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out = 0.0f;
        return false;
    }
}

// --- Q3 ------------------------------------------------------------------------------------

bool get_entity_id(const Entity& e, std::uint8_t (&out)[kPropertyIdSize]) {
    std::memset(out, 0, sizeof out);
    if (!g_resolved || e.ptr == nullptr) return false;
    __try {
        const void* id = g_abi.get_id(e.ptr);
        if (id == nullptr) return false;
        raw_copy(out, id, sizeof out);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool runtime_entity_by_id(const std::uint8_t (&id)[kPropertyIdSize], void*& out) {
    out = nullptr;
    if (!g_resolved) return false;
    __try {
        out = g_abi.runtime_get_entity(nullptr, id);  // this ignoré (désassemblage)
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out = nullptr;
        return false;
    }
}

bool get_hit_points(const Entity& e, int& out) { return call_int(g_abi.get_hit_points, e, out); }
bool get_hit_points_max(const Entity& e, int& out) { return call_int(g_abi.get_hit_points_max, e, out); }

bool set_hit_points(const Entity& e, int value, int modifier) {
    if (!g_resolved || e.ptr == nullptr) return false;
    __try {
        g_abi.set_hit_points(&e, value, modifier);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool get_damage_amount(const Entity& weapon, long& out) {
    out = 0;
    if (!g_resolved || weapon.ptr == nullptr) return false;
    void* ps = property_set(weapon, kDamagePropertySet);
    if (ps == nullptr) return false;
    __try {
        const long* p = g_abi.get_damage_amount(ps);
        if (p == nullptr) return false;
        raw_copy(&out, p, sizeof out);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out = 0;
        return false;
    }
}

bool set_damage_amount(const Entity& weapon, long amount) {
    if (!g_resolved || weapon.ptr == nullptr) return false;
    void* ps = property_set(weapon, kDamagePropertySet);
    if (ps == nullptr) return false;
    __try {
        g_abi.set_damage_amount(ps, &amount);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool do_damage(const Entity& weapon, const Entity& target, const Entity& attacker, bool& out) {
    out = false;
    if (!g_resolved || weapon.ptr == nullptr || target.ptr == nullptr) return false;
    __try {
        out = g_abi.do_damage(&weapon, &target, &attacker);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out = false;
        return false;
    }
}

// --- Q7 ------------------------------------------------------------------------------------

bool get_ai_mode(const Entity& e, int& out) { return call_int(g_abi.get_ai_mode, e, out); }
bool get_last_ai_mode(const Entity& e, int& out) { return call_int(g_abi.get_last_ai_mode, e, out); }

bool set_ai_mode(const Entity& e, int mode) {
    if (!g_resolved || e.ptr == nullptr) return false;
    __try {
        g_abi.set_ai_mode(&e, &mode);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool set_routine_state(const Entity& e, const char* state) {
    if (!g_resolved || e.ptr == nullptr || state == nullptr) return false;
    void* text = nullptr;  // bCString passé par valeur : l'appelé le détruit (~bCString en fin de SetState)
    __try {
        g_abi.bcstring_ctor(&text, state);
        g_abi.set_state(&e, &text);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Exception dans SetState : la chaîne fuit peut-être (rare, sonde seulement).
        return false;
    }
}

bool defeat(void* spu, void* self, void* other, long long arg, long long& out) {
    out = 0;
    if (g_defeat == nullptr) return false;
    __try {
        out = g_defeat(spu, self, other, arg);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out = 0;
        return false;
    }
}

bool hud_print(const char* text) {
    if (!g_resolved || g_abi.gui2 == nullptr || text == nullptr) return false;
    __try {
        if (*static_cast<const unsigned char*>(g_abi.gui2) == 0) return false;  // journal à l'écran désactivé
        g_abi.print_game_log_f(g_abi.gui2, 0, "%s", text);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool spu_other_entity(void* spu, void*& out) {
    out = nullptr;
    if (g_spu_get_other == nullptr || spu == nullptr) return false;
    __try {
        out = g_spu_get_other(spu);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out = nullptr;
        return false;
    }
}

bool read_entity_ptr(const void* entity_object, void*& out) {
    out = nullptr;
    if (entity_object == nullptr) return false;
    __try {
        raw_copy(&out, entity_object, sizeof out);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out = nullptr;
        return false;
    }
}

}  // namespace mp::game
