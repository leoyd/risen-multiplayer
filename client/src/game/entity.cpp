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
using GetTemplateFn = Template* (*)(const Entity* self, Template* sret);
using SpawnFn = Entity* (*)(Entity* sret, Template* by_value, const Matrix* pose, bool temporary);
using TemplateDtorFn = void (*)(Template* self);
// Engine.dll — variante const non virtuelle : elle fait elle-même le dispatch virtuel.
using GetPropertySetFn = void* (*)(const void* entity, int type);

constexpr const char* kGetPlayer = "?GetPlayer@Entity@@SA?AV1@XZ";
constexpr const char* kGetPose = "?GetPose@Entity@@QEBA?AVbCMatrix@@XZ";
constexpr const char* kGetTemplate = "?GetTemplate@Entity@@QEBA?AVTemplate@@XZ";
constexpr const char* kSpawn = "?Spawn@Entity@@SA?AV1@VTemplate@@AEBVbCMatrix@@_N@Z";
constexpr const char* kTemplateDtor = "??1Template@@QEAA@XZ";
constexpr const char* kGetPropertySet =
    "?GetPropertySet@eCEntity@@QEBAPEBVeCEntityPropertySet@@W4eEPropertySetType@@@Z";
constexpr const char* kOnProcess = "?OnProcess@eCApplication@@UEAAXXZ";
constexpr const char* kPlayMotion =
    "?PlayMotion@eCAnimation_PS@@QEAAXW4eEMotionType@eAnimShared@@PEAUeSMotionPlayBackDesc@3@_N@Z";

constexpr std::size_t kMotionResourceOffset = 0x120;
constexpr std::size_t kMotionResourceStride = 0x10;

struct EntityAbi {
    GetPlayerFn get_player = nullptr;
    GetPoseFn get_pose = nullptr;
    GetTemplateFn get_template = nullptr;
    SpawnFn spawn = nullptr;
    TemplateDtorFn template_dtor = nullptr;
    GetPropertySetFn get_property_set = nullptr;
    void* on_process = nullptr;
    PlayMotionFn play_motion = nullptr;
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
                    lookup(script, kGetTemplate, a.get_template, missing) &&
                    lookup(script, kSpawn, a.spawn, missing) &&
                    lookup(script, kTemplateDtor, a.template_dtor, missing) &&
                    lookup(engine, kGetPropertySet, a.get_property_set, missing) &&
                    lookup(engine, kOnProcess, on_process, missing) &&
                    lookup(engine, kPlayMotion, a.play_motion, missing);
    if (!ok) return false;
    a.on_process = reinterpret_cast<void*>(on_process);
    g_abi = a;
    g_resolved = true;
    return true;
}

void* on_process_address() { return g_abi.on_process; }
void* play_motion_address() { return reinterpret_cast<void*>(g_abi.play_motion); }

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
