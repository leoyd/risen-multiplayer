#include "game/entity.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstring>

namespace mp::game {

namespace {

// Script.dll
using GetPlayerFn = Entity* (*)(Entity* sret);
using GetPoseFn = Matrix* (*)(const Entity* self, Matrix* sret);
using SetPoseFn = void (*)(const Entity* self, const Matrix* pose);
using MoveToFn = void (*)(const Entity* self, const Matrix* pose);
using GetTemplateFn = Template* (*)(const Entity* self, Template* sret);
using SpawnFn = Entity* (*)(Entity* sret, Template* by_value, const Matrix* pose, bool temporary);
using TemplateDtorFn = void (*)(Template* self);
using IsProcessingRangeEnteredFn = bool (*)(const Entity* self);
using IsDeadFn = bool (*)(const Entity* self);
// Saute vers eCEntity::Kill (Engine.dll) : destruction différée (liste du moteur, bit 0x8 de +0x28).
using KillFn = void (*)(const Entity* self);
// Game.dll
using SessionGetInstanceFn = void* (*)();             // gCSession& (pointeur global, nul possible)
using SessionIsGameRunningFn = bool (*)(const void* session);  // octet à +0x110
// bTObjArray<Entity> : { Entity* data; u32 count; … } (Entity::GetNPCs, Script.dll x64).
struct EntityArray {
    Entity* data;
    std::uint32_t count;
    std::uint32_t capacity;
};
using GetNPCsFn = EntityArray* (*)(EntityArray* sret);
// Engine.dll
using SetMotionFn = void (*)(void* animation_ps, int type, void* resource);
using GetRagdollFn = bool (*)(const void* animation_ps);
using GetFileTitleFn = void* (*)(const void* resource, void* bcstring_sret);
// Statique, sans sret : rend un pointeur emprunté (cache des ressources).
using GetMotionFn = void* (*)(const void* bcstring_name);
// SharedBase.dll
using BCStringCtorFn = void* (*)(void* self, const char* text);
using BCStringDtorFn = void (*)(void* self);
// Engine.dll — variante const non virtuelle : elle fait elle-même le dispatch virtuel.
using GetPropertySetFn = void* (*)(const void* entity, int type);

constexpr const char* kGetPlayer = "?GetPlayer@Entity@@SA?AV1@XZ";
constexpr const char* kGetPose = "?GetPose@Entity@@QEBA?AVbCMatrix@@XZ";
constexpr const char* kSetPose = "?SetPose@Entity@@QEAAXAEBVbCMatrix@@@Z";
constexpr const char* kMoveTo = "?MoveTo@Entity@@QEAAXAEBVbCMatrix@@@Z";
constexpr const char* kGetTemplate = "?GetTemplate@Entity@@QEBA?AVTemplate@@XZ";
constexpr const char* kSpawn = "?Spawn@Entity@@SA?AV1@VTemplate@@AEBVbCMatrix@@_N@Z";
constexpr const char* kTemplateDtor = "??1Template@@QEAA@XZ";
constexpr const char* kIsProcessingRangeEntered = "?IsProcessingRangeEntered@Entity@@QEBA_NXZ";
constexpr const char* kIsDead = "?IsDead@Entity@@QEBA_NXZ";
constexpr const char* kKill = "?Kill@Entity@@QEAAXXZ";
constexpr const char* kSessionGetInstance = "?GetInstance@gCSession@@SAAEAV1@XZ";
constexpr const char* kSessionIsGameRunning = "?IsGameRunning@gCSession@@QEBA_NXZ";
constexpr const char* kGetNPCs = "?GetNPCs@Entity@@SA?AV?$bTObjArray@VEntity@@@@XZ";
constexpr const char* kGetPropertySet =
    "?GetPropertySet@eCEntity@@QEBAPEBVeCEntityPropertySet@@W4eEPropertySetType@@@Z";
constexpr const char* kSetMotion =
    "?SetMotion@eCAnimation_PS@@QEAAXW4eEMotionType@eAnimShared@@PEAVeCMotionResource2@@@Z";
constexpr const char* kSetRagdoll = "?SetRagDollEnabled@eCAnimation_PS@@QEAAX_N0PEAVbCVector@@@Z";
constexpr const char* kGetRagdoll = "?GetRagDollEnabled@eCAnimation_PS@@QEBA_NXZ";
constexpr const char* kGetFileTitle = "?GetFileTitleA@eCResource2@@QEBA?AVbCString@@XZ";
constexpr const char* kGetMotion = "?GetMotion@eCResource2@@SAPEAVeCMotionResource2@@AEBVbCString@@@Z";
constexpr const char* kBCStringCtor = "??0bCString@@QEAA@PEBD@Z";
constexpr const char* kBCStringDtor = "??1bCString@@QEAA@XZ";
constexpr const char* kOnProcess = "?OnProcess@eCApplication@@UEAAXXZ";
constexpr const char* kSetPlaySpeed = "?SetPlaySpeed@eCWrapper_emfx3ActorInstance@@QEAAXW4eEMotionType@eAnimShared@@M@Z";
constexpr const char* kSetTargetWeight =
    "?SetTargetWeight@eCWrapper_emfx3ActorInstance@@QEAAXW4eEMotionType@eAnimShared@@MM@Z";
constexpr const char* kSwitchFadeOut = "?SwitchFadeOut@eCWrapper_emfx3ActorInstance@@QEAAXW4eEMotionType@eAnimShared@@_NM@Z";
constexpr const char* kWrapperStopMotion =
    "?StopMotion@eCWrapper_emfx3ActorInstance@@QEAAXW4eEMotionType@eAnimShared@@M_N@Z";
constexpr const char* kGetActorWrapper = "?GetActorWrapper@eCAnimation_PS@@QEAAPEAVeCWrapper_emfx3ActorInstance@@XZ";
using GetActorWrapperFn = void* (*)(void* animation_ps);
constexpr const char* kPlayMotion =
    "?PlayMotion@eCAnimation_PS@@QEAAXW4eEMotionType@eAnimShared@@PEAUeSMotionPlayBackDesc@3@_N@Z";

constexpr std::size_t kMotionResourceOffset = 0x120;
constexpr std::size_t kMotionResourceStride = 0x10;

struct EntityAbi {
    GetPlayerFn get_player = nullptr;
    GetPoseFn get_pose = nullptr;
    SetPoseFn set_pose = nullptr;
    MoveToFn move_to = nullptr;
    GetTemplateFn get_template = nullptr;
    SpawnFn spawn = nullptr;
    TemplateDtorFn template_dtor = nullptr;
    IsProcessingRangeEnteredFn is_processing_range_entered = nullptr;
    IsDeadFn is_dead = nullptr;
    KillFn kill = nullptr;
    SessionGetInstanceFn session_get_instance = nullptr;
    SessionIsGameRunningFn session_is_game_running = nullptr;
    GetNPCsFn get_npcs = nullptr;
    GetPropertySetFn get_property_set = nullptr;
    void* on_process = nullptr;
    PlayMotionFn play_motion = nullptr;
    SetPlaySpeedFn set_play_speed = nullptr;
    SetTargetWeightFn set_target_weight = nullptr;
    SwitchFadeOutFn switch_fade_out = nullptr;
    WrapperStopMotionFn wrapper_stop_motion = nullptr;
    GetActorWrapperFn get_actor_wrapper = nullptr;
    SetMotionFn set_motion = nullptr;
    SetRagdollFn set_ragdoll = nullptr;
    GetRagdollFn get_ragdoll = nullptr;
    GetFileTitleFn get_file_title = nullptr;
    GetMotionFn get_motion = nullptr;
    BCStringCtorFn bcstring_ctor = nullptr;
    BCStringDtorFn bcstring_dtor = nullptr;
};

EntityAbi g_abi;
bool g_resolved = false;

template <typename Fn>
bool lookup(HMODULE module, const char* name, Fn& out, const char*& missing) {
    out = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(module, name)));
    if (out == nullptr) missing = name;
    return out != nullptr;
}

}  // namespace

bool resolve_entities(const char*& missing) {
    HMODULE engine = GetModuleHandleW(L"Engine.dll");
    if (engine == nullptr) {
        missing = "Engine.dll";
        return false;
    }
    HMODULE game = GetModuleHandleW(L"Game.dll");
    if (game == nullptr) {
        missing = "Game.dll";
        return false;
    }
    HMODULE shared_base = GetModuleHandleW(L"SharedBase.dll");
    if (shared_base == nullptr) {
        missing = "SharedBase.dll";
        return false;
    }
    // Script.dll est normalement déjà chargée (Script_Game.dll en dépend) ; sinon on la
    // charge depuis le dossier du jeu (hors loader lock : appelé depuis ScriptInit).
    HMODULE script = GetModuleHandleW(L"Script.dll");
    if (script == nullptr) script = LoadLibraryW(L"Script.dll");
    if (script == nullptr) {
        missing = "Script.dll";
        return false;
    }
    EntityAbi a;
    void (*on_process)(void*) = nullptr;
    const bool ok = lookup(script, kGetPlayer, a.get_player, missing) &&
                    lookup(script, kGetPose, a.get_pose, missing) &&
                    lookup(script, kSetPose, a.set_pose, missing) &&
                    lookup(script, kMoveTo, a.move_to, missing) &&
                    lookup(script, kGetTemplate, a.get_template, missing) &&
                    lookup(script, kSpawn, a.spawn, missing) &&
                    lookup(script, kTemplateDtor, a.template_dtor, missing) &&
                    lookup(script, kIsProcessingRangeEntered, a.is_processing_range_entered, missing) &&
                    lookup(script, kGetNPCs, a.get_npcs, missing) &&
                    lookup(script, kIsDead, a.is_dead, missing) &&
                    lookup(script, kKill, a.kill, missing) &&
                    lookup(game, kSessionGetInstance, a.session_get_instance, missing) &&
                    lookup(game, kSessionIsGameRunning, a.session_is_game_running, missing) &&
                    lookup(engine, kGetPropertySet, a.get_property_set, missing) &&
                    lookup(engine, kOnProcess, on_process, missing) &&
                    lookup(engine, kPlayMotion, a.play_motion, missing) &&
                    lookup(engine, kSetPlaySpeed, a.set_play_speed, missing) &&
                    lookup(engine, kSetTargetWeight, a.set_target_weight, missing) &&
                    lookup(engine, kSwitchFadeOut, a.switch_fade_out, missing) &&
                    lookup(engine, kWrapperStopMotion, a.wrapper_stop_motion, missing) &&
                    lookup(engine, kGetActorWrapper, a.get_actor_wrapper, missing) &&
                    lookup(engine, kSetMotion, a.set_motion, missing) &&
                    lookup(engine, kSetRagdoll, a.set_ragdoll, missing) &&
                    lookup(engine, kGetRagdoll, a.get_ragdoll, missing) &&
                    lookup(engine, kGetFileTitle, a.get_file_title, missing) &&
                    lookup(engine, kGetMotion, a.get_motion, missing) &&
                    lookup(shared_base, kBCStringCtor, a.bcstring_ctor, missing) &&
                    lookup(shared_base, kBCStringDtor, a.bcstring_dtor, missing);
    if (!ok) return false;
    a.on_process = reinterpret_cast<void*>(on_process);
    g_abi = a;
    g_resolved = true;
    return true;
}

void* on_process_address() { return g_abi.on_process; }
void* play_motion_address() { return reinterpret_cast<void*>(g_abi.play_motion); }
void* set_ragdoll_address() { return reinterpret_cast<void*>(g_abi.set_ragdoll); }
void* set_play_speed_address() { return reinterpret_cast<void*>(g_abi.set_play_speed); }
void* set_target_weight_address() { return reinterpret_cast<void*>(g_abi.set_target_weight); }
void* switch_fade_out_address() { return reinterpret_cast<void*>(g_abi.switch_fade_out); }
void* wrapper_stop_motion_address() { return reinterpret_cast<void*>(g_abi.wrapper_stop_motion); }

bool set_target_weight(void* wrapper, int type, float weight, float time) {
    if (!g_resolved || wrapper == nullptr) return false;
    __try {
        g_abi.set_target_weight(wrapper, type, weight, time);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool set_play_speed(void* wrapper, int type, float speed) {
    if (!g_resolved || wrapper == nullptr) return false;
    __try {
        g_abi.set_play_speed(wrapper, type, speed);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool get_actor_wrapper(void* animation_ps, void*& out) {
    out = nullptr;
    if (!g_resolved || animation_ps == nullptr) return false;
    __try {
        out = g_abi.get_actor_wrapper(animation_ps);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out = nullptr;
        return false;
    }
}

// Chaque enveloppe : pas d'objet C++ à détruire dans le cadre __try.

bool get_player(Entity& out) {
    out.ptr = nullptr;
    if (!g_resolved) return false;
    __try {
        g_abi.get_player(&out);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out.ptr = nullptr;
        return false;
    }
}

bool get_pose(const Entity& e, Matrix& out) {
    if (!g_resolved || e.ptr == nullptr) return false;
    __try {
        g_abi.get_pose(&e, &out);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool set_pose(const Entity& e, const Matrix& pose) {
    if (!g_resolved || e.ptr == nullptr) return false;
    __try {
        g_abi.set_pose(&e, &pose);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool move_to(const Entity& e, const Matrix& pose) {
    if (!g_resolved || e.ptr == nullptr) return false;
    __try {
        g_abi.move_to(&e, &pose);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool get_template(const Entity& e, Template& out) {
    out.proxy = nullptr;
    if (!g_resolved || e.ptr == nullptr) return false;
    __try {
        g_abi.get_template(&e, &out);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out.proxy = nullptr;
        return false;
    }
}

void destroy_template(Template& t) {
    if (!g_resolved) return;
    __try {
        g_abi.template_dtor(&t);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    t.proxy = nullptr;
}

bool spawn(Template& tmpl, const Matrix& pose, bool temporary, Entity& out) {
    out.ptr = nullptr;
    if (!g_resolved) return false;
    bool ok = false;
    __try {
        // Passage par valeur (AD-3) : `tmpl` est la copie, détruite par l'appelé.
        g_abi.spawn(&out, &tmpl, &pose, temporary);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out.ptr = nullptr;
    }
    tmpl.proxy = nullptr;  // consommé dans tous les cas : ne jamais le détruire ici
    return ok;
}

bool get_animation_ps(const Entity& e, void*& out) {
    out = nullptr;
    if (!g_resolved || e.ptr == nullptr) return false;
    __try {
        out = g_abi.get_property_set(e.ptr, kAnimationPropertySet);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out = nullptr;
        return false;
    }
}

bool play_motion(void* animation_ps, int type, void* desc, bool flag) {
    if (!g_resolved || animation_ps == nullptr) return false;
    __try {
        g_abi.play_motion(animation_ps, type, desc, flag);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool is_processing_range_entered(const Entity& e, bool& out) {
    out = false;
    if (!g_resolved || e.ptr == nullptr) return false;
    __try {
        out = g_abi.is_processing_range_entered(&e);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool is_dead(const Entity& e, bool& out) {
    out = false;
    if (!g_resolved || e.ptr == nullptr) return false;
    __try {
        out = g_abi.is_dead(&e);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out = false;
        return false;
    }
}

bool kill(const Entity& e) {
    if (!g_resolved || e.ptr == nullptr) return false;
    __try {
        g_abi.kill(&e);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool game_running(bool& out) {
    out = false;
    if (!g_resolved) return false;
    __try {
        const void* session = g_abi.session_get_instance();
        out = session != nullptr && g_abi.session_is_game_running(session);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out = false;
        return false;
    }
}

namespace {

constexpr std::uint32_t kMaxNpcs = 4096;

bool call_get_npcs(EntityArray& arr) {
    __try {
        g_abi.get_npcs(&arr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

bool get_npcs(std::vector<Entity>& out) {
    out.clear();
    if (!g_resolved) return false;
    EntityArray arr{nullptr, 0, 0};
    if (!call_get_npcs(arr)) return false;
    if (arr.data == nullptr || arr.count == 0) return true;
    const std::uint32_t n = arr.count < kMaxNpcs ? arr.count : kMaxNpcs;
    out.resize(n);
    if (!copy_bytes(arr.data, out.data(), n * sizeof(Entity))) {
        out.clear();
        return false;
    }
    return true;  // arr.data fuit volontairement (voir entity.h)
}

bool set_motion(void* animation_ps, int type, void* resource) {
    if (!g_resolved || animation_ps == nullptr || type < 0 || type >= kMotionTypeCount) return false;
    __try {
        g_abi.set_motion(animation_ps, type, resource);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool set_ragdoll(void* animation_ps, bool on, bool flag, const float* vector) {
    if (!g_resolved || animation_ps == nullptr) return false;
    // bCVector durable : on ignore si la création du ragdoll garde le pointeur (non vérifié).
    static float v[3];
    if (vector != nullptr) {
        v[0] = vector[0];
        v[1] = vector[1];
        v[2] = vector[2];
    }
    __try {
        g_abi.set_ragdoll(animation_ps, on, flag, vector != nullptr ? v : nullptr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool get_ragdoll(const void* animation_ps, bool& out) {
    out = false;
    if (!g_resolved || animation_ps == nullptr) return false;
    __try {
        out = g_abi.get_ragdoll(animation_ps);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out = false;
        return false;
    }
}

// clang-cl sans /EHa ne protège sous __try que les appels : une lecture fautive inlinée
// dans le bloc ne serait pas interceptée. La copie se fait donc dans un appel opaque.
__declspec(noinline) static void raw_copy(void* dst, const void* src, std::size_t n) {
    std::memcpy(dst, src, n);
}

bool copy_bytes(const void* src, void* dst, std::size_t n) {
    if (src == nullptr) return false;
    __try {
        raw_copy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool read_motion_resource(const void* animation_ps, int type, void*& out) {
    out = nullptr;
    if (animation_ps == nullptr || type < 0 || type >= kMotionTypeCount) return false;
    const auto* slot = static_cast<const std::uint8_t*>(animation_ps) + kMotionResourceOffset +
                       static_cast<std::size_t>(type) * kMotionResourceStride;
    return copy_bytes(slot, &out, sizeof out);
}

}  // namespace mp::game

namespace mp::game {

namespace {

constexpr std::size_t kMaxNameLength = 255;

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

bool title_into(const void* resource, char* buf, std::size_t cap, std::size_t& len) {
    void* text = nullptr;  // bCString retourné par valeur (sret), détruit ici
    bool ok = false;
    __try {
        g_abi.get_file_title(resource, &text);
        len = raw_copy_cstr(static_cast<const char*>(text), buf, cap);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    __try {
        if (text != nullptr) g_abi.bcstring_dtor(&text);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return ok;
}

}  // namespace

bool get_resource_name(const void* resource, std::string& out) {
    out.clear();
    if (!g_resolved || resource == nullptr) return false;
    char buf[kMaxNameLength];
    std::size_t len = 0;
    if (!title_into(resource, buf, sizeof buf, len)) return false;
    out.assign(buf, len);
    return true;
}

namespace {

// bCString construit ici puis détruit ici : passé par référence (AEBV), il reste au mod.
bool call_get_motion(const char* name, void*& out) {
    void* text = nullptr;  // bCString : un seul pointeur
    bool built = false;
    bool ok = false;
    __try {
        g_abi.bcstring_ctor(&text, name);
        built = true;
        out = g_abi.get_motion(&text);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out = nullptr;
        ok = false;
    }
    if (built) {
        __try {
            g_abi.bcstring_dtor(&text);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    return ok;
}

}  // namespace

bool get_motion(const std::string& name, void*& out) {
    out = nullptr;
    if (!g_resolved || name.empty() || name.size() > kMaxNameLength) return false;
    return call_get_motion(name.c_str(), out);
}

}  // namespace mp::game
