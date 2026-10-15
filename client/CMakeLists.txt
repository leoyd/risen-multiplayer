cmake_minimum_required(VERSION 3.21)

project(RisenMultiplayerClient VERSION 0.1.0 LANGUAGES CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Type de build" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Logique pure (config, version) : aucune dépendance Windows, compilée partout.
add_library(mp_core STATIC
    src/core/config.cpp
    src/core/version.cpp)
target_include_directories(mp_core PUBLIC src)

if(WIN32)
    # --- DLL chargée par Risen depuis bin/scripts/ ---------------------------------
    # CRT statique : aucune dépendance aux redistribuables MSVC.
    set_property(TARGET mp_core PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")

    add_library(Script_Multiplayer SHARED
        src/win/script_init.cpp
        src/win/log.cpp
        src/win/game_abi.cpp)
    target_link_libraries(Script_Multiplayer PRIVATE mp_core kernel32)
    target_compile_definitions(Script_Multiplayer PRIVATE
        MP_MOD_VERSION="${PROJECT_VERSION}"
        WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    set_target_properties(Script_Multiplayer PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded"
        PREFIX "")
    # Banc d'essai Wine pour les scénarios d'intégration (tests/wine/run_scenarios.sh).
    add_executable(mp_harness tests/wine/harness.cpp)
    target_compile_definitions(mp_harness PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    set_target_properties(mp_harness PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded")

    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|MSVC")
        target_compile_options(mp_harness PRIVATE /EHsc /utf-8)
        # /EHsc : exceptions C++ ; __try/__except (SEH) est géré nativement par clang-cl.
        target_compile_options(Script_Multiplayer PRIVATE /EHsc /W4 /utf-8)
        target_compile_options(mp_core PRIVATE /EHsc /W4 /utf-8)
    endif()
else()
    # --- Tests unitaires natifs (Linux), sans le jeu --------------------------------
    enable_testing()
    add_executable(client_tests
        tests/test_main.cpp
        tests/test_config.cpp
        tests/test_version.cpp)
    target_link_libraries(client_tests PRIVATE mp_core)
    target_include_directories(client_tests PRIVATE tests)
    target_compile_options(client_tests PRIVATE -Wall -Wextra)
    target_compile_options(mp_core PRIVATE -Wall -Wextra)

    # Test d'intégration optionnel sur les vraies DLL (risen_ori/, non versionné).
    set(RISEN_BIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../risen_ori/bin" CACHE PATH
        "Dossier bin/ d'une installation de Risen (facultatif)")
    if(EXISTS "${RISEN_BIN_DIR}/Game.dll")
        target_compile_definitions(client_tests PRIVATE RISEN_BIN_DIR="${RISEN_BIN_DIR}")
    endif()

    add_test(NAME client_tests COMMAND client_tests)

    # Scénarios d'intégration sous Wine : DLL Windows + vraies DLL du jeu + wine.
    # Enregistrés seulement si les trois sont disponibles.
    set(MP_WIN_BUILD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/build" CACHE PATH
        "Dossier du build Windows (Script_Multiplayer.dll, mp_harness.exe)")
    find_program(WINE_EXECUTABLE wine)
    if(WINE_EXECUTABLE AND EXISTS "${RISEN_BIN_DIR}/Game.dll"
       AND EXISTS "${MP_WIN_BUILD_DIR}/mp_harness.exe"
       AND EXISTS "${MP_WIN_BUILD_DIR}/Script_Multiplayer.dll")
        add_test(NAME wine_scenarios
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/wine/run_scenarios.sh
                         ${MP_WIN_BUILD_DIR} ${RISEN_BIN_DIR})
        set_tests_properties(wine_scenarios PROPERTIES TIMEOUT 300)
    else()
        message(STATUS "Scénarios Wine non enregistrés (wine, build Windows ou DLL du jeu absents)")
    endif()
endif()
