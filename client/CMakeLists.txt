cmake_minimum_required(VERSION 3.21)

project(RisenMultiplayerClient VERSION 0.1.0 LANGUAGES C CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Type de build" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Logique pure (config, version, protocole) : aucune dépendance Windows, compilée partout.
add_library(mp_core STATIC
    src/core/config.cpp
    src/core/protocol.cpp
    src/core/session.cpp
    src/core/interpolation.cpp
    src/core/version.cpp)
target_include_directories(mp_core PUBLIC src)

# Transport UDP + KCP (story 1.2) : socket, démultiplexage des canaux, thread réseau.
# KCP 2.1.1 (third_party/kcp, commit figé : voir VERSION.txt), compilé sans avertissements.
if(WIN32)
    set(MP_NET_SOCKET_SOURCE src/net/udp_socket_win.cpp)
else()
    set(MP_NET_SOCKET_SOURCE src/net/udp_socket_posix.cpp)
endif()
add_library(mp_net STATIC
    src/net/transport.cpp
    ${MP_NET_SOCKET_SOURCE}
    third_party/kcp/ikcp.c)
target_include_directories(mp_net PUBLIC src PRIVATE third_party/kcp)
target_link_libraries(mp_net PUBLIC mp_core)
if(WIN32)
    target_link_libraries(mp_net PUBLIC ws2_32)
    target_compile_definitions(mp_net PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|MSVC")
        set_source_files_properties(third_party/kcp/ikcp.c PROPERTIES COMPILE_OPTIONS "/W0")
    endif()
else()
    find_package(Threads REQUIRED)
    target_link_libraries(mp_net PUBLIC Threads::Threads)
    set_source_files_properties(third_party/kcp/ikcp.c PROPERTIES COMPILE_OPTIONS "-w")
endif()

if(WIN32)
    # --- DLL chargée par Risen depuis bin/scripts/ ---------------------------------
    # CRT statique : aucune dépendance aux redistribuables MSVC.
    set_property(TARGET mp_core mp_net PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")

    # MinHook (third_party/minhook, commit figé : voir VERSION.txt), cible x64 uniquement.
    set(MINHOOK_SOURCES
        third_party/minhook/src/buffer.c
        third_party/minhook/src/hook.c
        third_party/minhook/src/trampoline.c
        third_party/minhook/src/hde/hde64.c)
    set_source_files_properties(${MINHOOK_SOURCES} PROPERTIES COMPILE_OPTIONS "/W0")

    add_library(Script_Multiplayer SHARED
        src/win/script_init.cpp
        src/win/hooks.cpp
        src/common/log.cpp
        src/game/abi.cpp
        src/game/combat.cpp
        src/game/entity.cpp
        src/game/motion_desc.cpp
        src/sync/anim_filter.cpp
        src/sync/avatar.cpp
        src/sync/local_capture.cpp
        src/sync/probe.cpp
        src/sync/probe_j2.cpp
        src/sync/session.cpp
        src/win/watchdog.cpp
        ${MINHOOK_SOURCES})
    target_include_directories(Script_Multiplayer PRIVATE third_party/minhook/include)
    # USER32 : GetAsyncKeyState / GetForegroundWindow (touches F9/F10 de la sonde).
    # WS2_32 : transport de la session (mp_net), appelé depuis la story 1.4.
    target_link_libraries(Script_Multiplayer PRIVATE mp_core mp_net kernel32 user32)
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

    # Tests du transport compilés pour Windows, lancés sous Wine (test CTest wine_net).
    add_executable(net_tests tests/test_main.cpp tests/test_net.cpp)
    target_link_libraries(net_tests PRIVATE mp_net)
    target_include_directories(net_tests PRIVATE tests)
    target_compile_definitions(net_tests PRIVATE
        MP_VECTORS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../protocol/vectors"
        WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    set_target_properties(net_tests PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded")

    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|MSVC")
        target_compile_options(mp_harness PRIVATE /EHsc /utf-8)
        # /EHsc : exceptions C++ ; __try/__except (SEH) est géré nativement par clang-cl.
        target_compile_options(Script_Multiplayer PRIVATE /utf-8
            $<$<COMPILE_LANGUAGE:CXX>:/EHsc /W4>)
        target_compile_options(mp_core PRIVATE /EHsc /W4 /utf-8)
        target_compile_options(mp_net PRIVATE /utf-8 $<$<COMPILE_LANGUAGE:CXX>:/EHsc /W4>)
        target_compile_options(net_tests PRIVATE /EHsc /W4 /utf-8)
    endif()
else()
    # --- Tests unitaires natifs (Linux), sans le jeu --------------------------------
    enable_testing()
    add_executable(client_tests
        tests/test_main.cpp
        tests/test_anim_filter.cpp
        tests/test_config.cpp
        tests/test_interpolation.cpp
        tests/test_local_capture.cpp
        tests/test_motion_desc.cpp
        tests/test_net.cpp
        tests/test_protocol.cpp
        tests/test_session.cpp
        tests/test_version.cpp
        # Code portable de game/ et sync/ (sans appel au jeu), compilé aussi dans la DLL.
        src/game/motion_desc.cpp
        src/sync/anim_filter.cpp
        src/sync/local_capture.cpp)
    target_link_libraries(client_tests PRIVATE mp_core mp_net)
    # Vecteurs binaires de référence partagés C++/Go (protocol/vectors/).
    target_compile_definitions(client_tests PRIVATE
        MP_VECTORS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../protocol/vectors")
    target_include_directories(client_tests PRIVATE tests)
    target_compile_options(client_tests PRIVATE -Wall -Wextra)
    target_compile_options(mp_core PRIVATE -Wall -Wextra)
    target_compile_options(mp_net PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra>)

    # Test d'intégration optionnel sur les vraies DLL (risen_ori/, non versionné).
    set(RISEN_BIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../risen_ori/bin" CACHE PATH
        "Dossier bin/ d'une installation de Risen (facultatif)")
    if(EXISTS "${RISEN_BIN_DIR}/Game.dll")
        target_compile_definitions(client_tests PRIVATE RISEN_BIN_DIR="${RISEN_BIN_DIR}")
    endif()

    add_test(NAME client_tests COMMAND client_tests)

    # Hôte de session sous Linux, sans le jeu : piloté par tests/session_fake_guest.sh.
    add_executable(session_host tests/session_host_main.cpp)
    target_link_libraries(session_host PRIVATE mp_core mp_net)
    target_compile_definitions(session_host PRIVATE MP_MOD_VERSION="${PROJECT_VERSION}")
    target_compile_options(session_host PRIVATE -Wall -Wextra)

    # Cohérence des hexdumps de protocol/PROTOCOL.md et des vecteurs .bin.
    find_program(XXD_EXECUTABLE xxd)
    if(XXD_EXECUTABLE)
        add_test(NAME protocol_vectors
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/../protocol/vectors/regen.sh --check)
    endif()

    # Faux invité de session (tools/fake-guest/, Go + cgo) : codec Go contre les vecteurs,
    # KCP vendorisé via cgo, matrice de session contre un hôte simulé.
    find_program(GO_EXECUTABLE go)
    if(GO_EXECUTABLE)
        add_test(NAME fake_guest
                 COMMAND ${GO_EXECUTABLE} test -count=1 ./...
                 WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../tools/fake-guest)
        # -count=1 : le cache de go test ne suit pas ikcp.c, inclus hors du paquet.
        set_tests_properties(fake_guest PROPERTIES
            TIMEOUT 300
            ENVIRONMENT "CGO_ENABLED=1;CC=${CMAKE_C_COMPILER}")
        # Hôte de session Linux (core/session + transport, monde simulé) face au faux invité :
        # nominal, --dup-hello, --bad-version, deux invités, --drop 20, --kill (story 1.4).
        add_test(NAME session_fake_guest
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/session_fake_guest.sh
                         $<TARGET_FILE:session_host> ${CMAKE_CURRENT_SOURCE_DIR}/../tools/fake-guest)
        set_tests_properties(session_fake_guest PROPERTIES
            TIMEOUT 300
            ENVIRONMENT "CGO_ENABLED=1;CC=${CMAKE_C_COMPILER}")
    else()
        message(STATUS "Tests fake_guest et session_fake_guest non enregistrés (go absent)")
    endif()

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

    # Mêmes tests du transport, compilés pour Windows (net_tests.exe) et lancés sous Wine.
    # Pas besoin des DLL du jeu.
    if(WINE_EXECUTABLE AND EXISTS "${MP_WIN_BUILD_DIR}/net_tests.exe")
        add_test(NAME wine_net COMMAND ${WINE_EXECUTABLE} ${MP_WIN_BUILD_DIR}/net_tests.exe)
        if(DEFINED ENV{MP_WINEPREFIX})
            set(_mp_wineprefix "$ENV{MP_WINEPREFIX}")
        else()
            set(_mp_wineprefix "${MP_WIN_BUILD_DIR}/wineprefix")
        endif()
        set_tests_properties(wine_net PROPERTIES
            TIMEOUT 300
            ENVIRONMENT "WINEPREFIX=${_mp_wineprefix};WINEDEBUG=-all")
    else()
        message(STATUS "Test wine_net non enregistré (wine ou net_tests.exe absents)")
    endif()
endif()
