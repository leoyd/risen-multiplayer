#include "game_abi.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace mp::abi {

namespace {

constexpr const char* kCtorName = "??0bCString@@QEAA@PEBD@Z";
constexpr const char* kDtorName = "??1bCString@@QEAA@XZ";

template <typename Fn>
Fn lookup(HMODULE module, const char* name) {
    return reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(module, name)));
}

}  // namespace

bool resolve(GameAbi& out, const char*& missing) {
    HMODULE shared_base = GetModuleHandleW(L"SharedBase.dll");
    if (shared_base == nullptr) {
        missing = "SharedBase.dll";
        return false;
    }
    out.bcstring_ctor = lookup<BCStringCtorFromCStr>(shared_base, kCtorName);
    if (out.bcstring_ctor == nullptr) {
        missing = kCtorName;
        return false;
    }
    out.bcstring_dtor = lookup<BCStringDtor>(shared_base, kDtorName);
    if (out.bcstring_dtor == nullptr) {
        missing = kDtorName;
        return false;
    }
    return true;
}

}  // namespace mp::abi
