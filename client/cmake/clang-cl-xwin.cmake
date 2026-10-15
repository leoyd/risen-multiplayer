# Toolchain croisée Fedora -> Windows x64 : clang-cl + lld-link + llvm-lib + CRT/SDK xwin.
#
# Prérequis : sudo dnf install clang lld llvm cmake ninja-build
#             xwin --accept-license --arch x86_64 splat --output ~/.xwin
# Usage     : cmake -S client -B client/build -G Ninja \
#               -DCMAKE_TOOLCHAIN_FILE=cmake/clang-cl-xwin.cmake [-DXWIN_DIR=/chemin/xwin]

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 10.0)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

if(NOT XWIN_DIR)
    if(DEFINED ENV{XWIN_DIR})
        set(XWIN_DIR "$ENV{XWIN_DIR}")
    else()
        set(XWIN_DIR "$ENV{HOME}/.xwin")
    endif()
endif()
set(XWIN_DIR "${XWIN_DIR}" CACHE PATH "Racine du CRT/SDK Microsoft extrait par xwin")
# Propagé aux projets de test de try_compile.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES XWIN_DIR)

if(NOT EXISTS "${XWIN_DIR}/crt/include" OR NOT EXISTS "${XWIN_DIR}/sdk/include/um")
    message(FATAL_ERROR "xwin introuvable dans '${XWIN_DIR}'. Lancer : "
                        "xwin --accept-license --arch x86_64 splat --output ~/.xwin")
endif()

find_program(CLANG_CL clang-cl REQUIRED)
find_program(LLD_LINK lld-link REQUIRED)
find_program(LLVM_LIB llvm-lib REQUIRED)
find_program(LLVM_RC llvm-rc)
find_program(LLVM_MT llvm-mt)

set(CMAKE_C_COMPILER "${CLANG_CL}")
set(CMAKE_CXX_COMPILER "${CLANG_CL}")
set(CMAKE_LINKER "${LLD_LINK}")
set(CMAKE_AR "${LLVM_LIB}")
if(LLVM_RC)
    set(CMAKE_RC_COMPILER "${LLVM_RC}")
endif()
if(LLVM_MT)
    set(CMAKE_MT "${LLVM_MT}")
endif()
# xwin ne fournit que le CRT « release » : CRT statique non debug partout,
# y compris pour les try_compile de CMake.
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
set(CMAKE_C_COMPILER_TARGET x86_64-pc-windows-msvc)
set(CMAKE_CXX_COMPILER_TARGET x86_64-pc-windows-msvc)

set(_xwin_includes
    "/imsvc${XWIN_DIR}/crt/include"
    "/imsvc${XWIN_DIR}/sdk/include/ucrt"
    "/imsvc${XWIN_DIR}/sdk/include/um"
    "/imsvc${XWIN_DIR}/sdk/include/shared")
list(JOIN _xwin_includes " " _xwin_includes)
set(CMAKE_C_FLAGS_INIT "${_xwin_includes}")
set(CMAKE_CXX_FLAGS_INIT "${_xwin_includes}")

set(_xwin_libpaths
    "/libpath:${XWIN_DIR}/crt/lib/x86_64"
    "/libpath:${XWIN_DIR}/sdk/lib/ucrt/x86_64"
    "/libpath:${XWIN_DIR}/sdk/lib/um/x86_64"
    "/manifest:no")
list(JOIN _xwin_libpaths " " _xwin_libpaths)
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_xwin_libpaths}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_xwin_libpaths}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_xwin_libpaths}")

set(CMAKE_FIND_ROOT_PATH "${XWIN_DIR}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
