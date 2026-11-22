#include "game/scene.h"

#include <windows.h>

#include <cstring>

namespace mp::game::scene {

namespace {

constexpr const char* kModuleAdminInstance = "?GetInstance@eCModuleAdmin@@SAAEAV1@XZ";
constexpr const char* kFindModule = "?FindModule@eCModuleAdmin@@QEBAPEAVeCEngineComponentBase@@AEBVbCString@@@Z";
constexpr const char* kSceneAdminVtable = "??_7eCSceneAdmin@@6B@";
constexpr const char* kGetPrefetchAdmin = "?GetPrefetchAdmin@eCSceneAdmin@@QEAAAEAVeCPrefetchAdmin@@XZ";
constexpr const char* kGetROISphere = "?GetROISphere@eCPrefetchAdmin@@QEAAAEBVbCSphere@@XZ";
constexpr const char* kSetROISphere = "?SetROISphere@eCPrefetchAdmin@@QEAAXAEBVbCSphere@@@Z";
constexpr const char* kGetDefaultCacheInRange = "?GetDefaultCacheInRange@eCPrefetchAdmin@@QEBAMXZ";
constexpr const char* kGetPrefetchRange = "?GetPrefetchRange@eCPrefetchAdmin@@QEBAMXZ";
constexpr const char* kSetPrefetchRange = "?SetPrefetchRange@eCPrefetchAdmin@@QEAAXM@Z";
constexpr const char* kBCStringCtor = "??0bCString@@QEAA@PEBD@Z";
constexpr const char* kBCStringDtor = "??1bCString@@QEAA@XZ";

// bCSphere (SharedBase.dll) : rayon puis centre (ApplyConfiguration la construit ainsi sur la pile).
struct Sphere {
    float radius;
    float x, y, z;
};

struct Abi {
    void* (*module_admin)() = nullptr;
    void* (*find_module)(const void* admin, const void* name) = nullptr;
    const void* scene_vtable = nullptr;
    void* (*get_prefetch_admin)(void* scene) = nullptr;
    const Sphere* (*get_roi)(void* prefetch) = nullptr;
    void (*set_roi)(void* prefetch, const Sphere* sphere) = nullptr;
    float (*get_cache_in)(const void* prefetch) = nullptr;
    float (*get_prefetch)(const void* prefetch) = nullptr;
    void (*set_prefetch)(void* prefetch, float range) = nullptr;
    void* (*bcstring_ctor)(void* self, const char* text) = nullptr;
    void (*bcstring_dtor)(void* self) = nullptr;
};

Abi g_abi;
int g_state = 0;  // 0 : non résolu, 1 : résolu, -1 : échec
const char* g_missing = nullptr;

template <typename Fn>
bool lookup(HMODULE module, const char* name, Fn& out) {
    out = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(module, name)));
    if (out == nullptr && g_missing == nullptr) g_missing = name;
    return out != nullptr;
}

bool resolve(std::string& error) {
    if (g_state == 0) {
        HMODULE engine = GetModuleHandleW(L"Engine.dll");
        HMODULE shared = GetModuleHandleW(L"SharedBase.dll");
        Abi a;
        bool ok = engine != nullptr && shared != nullptr;
        if (!ok) g_missing = "Engine.dll / SharedBase.dll";
        ok = ok && lookup(engine, kModuleAdminInstance, a.module_admin) && lookup(engine, kFindModule, a.find_module) &&
             lookup(engine, kSceneAdminVtable, a.scene_vtable) &&
             lookup(engine, kGetPrefetchAdmin, a.get_prefetch_admin) && lookup(engine, kGetROISphere, a.get_roi) &&
             lookup(engine, kSetROISphere, a.set_roi) && lookup(engine, kGetDefaultCacheInRange, a.get_cache_in) &&
             lookup(engine, kGetPrefetchRange, a.get_prefetch) && lookup(engine, kSetPrefetchRange, a.set_prefetch) &&
             lookup(shared, kBCStringCtor, a.bcstring_ctor) && lookup(shared, kBCStringDtor, a.bcstring_dtor);
        g_abi = a;
        g_state = ok ? 1 : -1;
    }
    if (g_state < 0) error = std::string("symbole absent : ") + (g_missing != nullptr ? g_missing : "?");
    return g_state > 0;
}

// Aucun objet C++ à détruire dans un cadre __try.

// 1 : trouvé, 0 : module absent, -1 : vtable inattendue, -2 : SEH.
int find_prefetch_raw(void*& prefetch) {
    prefetch = nullptr;
    void* name = nullptr;  // bCString construit ici, passé par référence constante, détruit ici
    int result = -2;
    __try {
        g_abi.bcstring_ctor(&name, "eCSceneAdmin");
        void* scene = g_abi.find_module(g_abi.module_admin(), &name);
        if (scene == nullptr) {
            result = 0;
        } else if (*static_cast<const void* const*>(scene) != g_abi.scene_vtable) {
            result = -1;
        } else {
            prefetch = g_abi.get_prefetch_admin(scene);
            result = prefetch != nullptr ? 1 : 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result = -2;
    }
    __try {
        if (name != nullptr) g_abi.bcstring_dtor(&name);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return result;
}

bool find_prefetch(void*& prefetch, std::string& error) {
    if (!resolve(error)) return false;
    switch (find_prefetch_raw(prefetch)) {
        case 1: return true;
        case 0: error = "module eCSceneAdmin introuvable"; return false;
        case -1: error = "module eCSceneAdmin de vtable inattendue"; return false;
        default: error = "exception (SEH) en cherchant eCSceneAdmin"; return false;
    }
}

bool read_raw(void* prefetch, Ranges& out) {
    __try {
        const Sphere* roi = g_abi.get_roi(prefetch);
        out.roi = roi != nullptr ? roi->radius : 0.0f;
        out.cache_in = g_abi.get_cache_in(prefetch);
        out.prefetch = g_abi.get_prefetch(prefetch);
        return roi != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool set_raw(void* prefetch, float roi, float prefetch_range) {
    __try {
        const Sphere* cur = g_abi.get_roi(prefetch);
        if (cur == nullptr) return false;
        Sphere s = *cur;
        s.radius = roi;
        g_abi.set_roi(prefetch, &s);
        if (g_abi.get_prefetch(prefetch) < prefetch_range) g_abi.set_prefetch(prefetch, prefetch_range);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

bool read_ranges(Ranges& out, std::string& error) {
    void* prefetch = nullptr;
    if (!find_prefetch(prefetch, error)) return false;
    if (!read_raw(prefetch, out)) {
        error = "exception (SEH) en lisant les portées";
        return false;
    }
    return true;
}

bool set_ranges(float roi, float prefetch_range, std::string& error) {
    void* prefetch = nullptr;
    if (!find_prefetch(prefetch, error)) return false;
    if (!set_raw(prefetch, roi, prefetch_range)) {
        error = "exception (SEH) en réglant les portées";
        return false;
    }
    return true;
}

}  // namespace mp::game::scene
