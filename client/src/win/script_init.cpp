// Point d'entrée du mod : export `ScriptInit`, appelé par gCScriptAdmin::LoadScriptDLL
// (Game.dll) pour chaque DLL de `bin/scripts/`.
//
// Contrat du chargeur (rétro-ingénierie, Game.dll Steam x64) :
// - `ScriptInit()` sans argument retourne un pointeur vers 4 tables de 16 octets
//   {T* data; int count; int cap} : AIState, AIFunction, AICallback, Script.
//   Le pointeur doit rester valide toute la durée du processus.
// - Si les 4 comptes valent 0, le chargeur rejette le module silencieusement
//   (Game.dll 0x1809632dc, sans boîte de dialogue). « Could not load script module »
//   ne s'affiche que si la DLL elle-même ne se charge pas.
// - Entrée (pas de 0x18) : +0x00 bCString nom, +0x08 bCString (copiée par operator=),
//   +0x10 pointeur de fonction. Un nom déjà enregistré est ignoré.
//
// En J0, la seule entrée est le script `MP_Probe`, que personne n'appelle. Avec
// `probe = 1` (spike J1), ScriptInit installe les hooks de la sonde (win/hooks,
// sync/probe) ; sinon (story 1.4) il installe les hooks de la session (sync/session,
// sync/avatar). Sonde et session sont exclusives. Si la version du jeu
// n'est pas reconnue ou si l'ABI est introuvable, les tables restent vides (rejet
// silencieux) et aucun code du jeu n'est appelé. Toute la logique s'exécute dans ScriptInit (hors loader lock).

#include "core/config.h"
#include "core/version.h"
#include "game/abi.h"
#include "common/log.h"
#include "sync/session.h"
#include "win/hooks.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#ifndef MP_MOD_VERSION
#define MP_MOD_VERSION "0.0.0-dev"
#endif

namespace {

struct Table {
    void* data;
    std::int32_t count;
    std::int32_t capacity;
};
static_assert(sizeof(Table) == 0x10, "table du descripteur : 16 octets");

struct ScriptDescriptor {
    Table ai_states;
    Table ai_functions;
    Table ai_callbacks;
    Table scripts;
};
static_assert(offsetof(ScriptDescriptor, ai_functions) == 0x10, "offset AIFunction");
static_assert(offsetof(ScriptDescriptor, ai_callbacks) == 0x20, "offset AICallback");
static_assert(offsetof(ScriptDescriptor, scripts) == 0x30, "offset Script");

struct ScriptEntry {
    mp::game::bCString name;
    mp::game::bCString description;
    void* function;
};
static_assert(sizeof(ScriptEntry) == 0x18, "entrée de table : pas de 0x18");
static_assert(offsetof(ScriptEntry, function) == 0x10, "offset du pointeur de fonction");

constexpr const char* kProbeName = "MP_Probe";
constexpr const char* kProbeDescription = "Risen Multiplayer : sonde de chargement (J0)";

// Durée de vie = processus : jamais détruits (Risen ne décharge pas les scripts).
ScriptDescriptor g_descriptor{};
ScriptEntry g_script_entries[1]{};
bool g_initialized = false;

// Vrai si version reconnue + config valide. Consommé à partir de J1 (réseau, hooks).
bool g_mod_active = false;
mp::Config g_config;

// Script inoffensif : quels que soient ses arguments (convention MSVC x64, l'appelé
// n'a rien à nettoyer), il retourne 0 dans RAX.
extern "C" std::intptr_t MP_Probe(void*, void*, void*, void*) { return 0; }

// Appel protégé du constructeur du jeu. Fonction séparée : pas d'objet C++ à
// détruire dans un cadre __try.
bool construct_bcstring(mp::game::BCStringCtorFromCStr ctor, mp::game::bCString* self, const char* text) {
    __try {
        ctor(self, text);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        self->data = nullptr;
        return false;
    }
}

void destroy_bcstring(mp::game::BCStringDtor dtor, mp::game::bCString* self) {
    __try {
        dtor(self);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    self->data = nullptr;
}

std::wstring dll_directory() {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&g_descriptor), &self)) {
        return L".";
    }
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(self, path.data(), static_cast<DWORD>(path.size()));
        if (n == 0) return L".";
        if (n < path.size()) {
            path.resize(n);
            break;
        }
        path.resize(path.size() * 2);
    }
    const std::size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring(L".") : path.substr(0, slash);
}

std::string to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

// Remplit le descripteur. Retourne false (tables vides) si l'ABI est introuvable.
bool build_descriptor() {
    mp::game::GameAbi abi;
    const char* missing = nullptr;
    if (!mp::game::resolve(abi, missing)) {
        mp::log::error(std::string("ABI du jeu : symbole introuvable : ") + missing);
        return false;
    }
    ScriptEntry& e = g_script_entries[0];
    if (!construct_bcstring(abi.bcstring_ctor, &e.name, kProbeName)) {
        mp::log::error("ABI du jeu : exception dans bCString::bCString (nom)");
        return false;
    }
    if (!construct_bcstring(abi.bcstring_ctor, &e.description, kProbeDescription)) {
        mp::log::error("ABI du jeu : exception dans bCString::bCString (description)");
        destroy_bcstring(abi.bcstring_dtor, &e.name);
        return false;
    }
    e.function = reinterpret_cast<void*>(&MP_Probe);
    g_descriptor.scripts = Table{g_script_entries, 1, 1};
    mp::log::info(std::string("script « ") + kProbeName + " » exposé au chargeur");
    return true;
}

enum class FileRead { Ok, NotFound, Error };

FileRead read_file(const std::wstring& path, std::string& out, DWORD& error) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        return (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) ? FileRead::NotFound
                                                                                 : FileRead::Error;
    }
    constexpr DWORD kMaxSize = 64 * 1024;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart > kMaxSize) {
        error = size.QuadPart > kMaxSize ? ERROR_FILE_TOO_LARGE : GetLastError();
        CloseHandle(h);
        return FileRead::Error;
    }
    out.assign(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    const BOOL ok = out.empty() || ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &read, nullptr);
    error = ok ? 0 : GetLastError();
    CloseHandle(h);
    if (!ok) return FileRead::Error;
    out.resize(read);
    return FileRead::Ok;
}

bool fingerprint_of(const wchar_t* module_name, mp::ModuleFingerprint& out) {
    HMODULE m = GetModuleHandleW(module_name);
    if (m == nullptr) return false;
    // L'en-tête PE occupe la première page de l'image, toujours mappée.
    return mp::read_pe_fingerprint(reinterpret_cast<const std::uint8_t*>(m), 0x1000, out);
}

bool check_game_version() {
    mp::ModuleFingerprint game, engine;
    const bool have_game = fingerprint_of(L"Game.dll", game);
    const bool have_engine = fingerprint_of(L"Engine.dll", engine);
    mp::log::info(std::string("empreinte Game.dll : ") +
                  (have_game ? mp::format_fingerprint(game) : std::string("illisible")));
    mp::log::info(std::string("empreinte Engine.dll : ") +
                  (have_engine ? mp::format_fingerprint(engine) : std::string("illisible")));
    if (!have_game || !have_engine) {
        mp::log::error("version non supportée : modules du jeu introuvables ou illisibles");
        return false;
    }
    const mp::SupportedVersion* v = mp::find_supported_version(game, engine);
    if (v == nullptr) {
        mp::log::error("version non supportée : empreinte absente de la liste blanche");
        return false;
    }
    mp::log::info(std::string("version du jeu : ") + v->label);
    return true;
}

bool load_config(const std::wstring& dir) {
    const std::wstring path = dir + L"\\multiplayer.ini";
    std::string text;
    DWORD error = 0;
    switch (read_file(path, text, error)) {
        case FileRead::NotFound:
            mp::log::error("config absente : " + to_utf8(path));
            return false;
        case FileRead::Error:
            mp::log::error("config illisible : " + to_utf8(path) + " (erreur Windows " + std::to_string(error) + ")");
            return false;
        case FileRead::Ok:
            break;
    }
    const mp::ConfigResult r = mp::parse_config(text);
    for (const auto& w : r.warnings) mp::log::warn("config : " + w);
    if (!r.ok) {
        mp::log::error("config invalide : champ « " + r.error_field + " », valeur « " + r.error_value + " » (" +
                       r.error_message + ")");
        return false;
    }
    g_config = r.config;
    mp::log::set_debug(g_config.log_level == mp::LogLevel::Debug);
    mp::log::info(std::string("config OK (rôle=") + mp::role_name(g_config.role) + ", port=" +
                  std::to_string(g_config.port) + ", pseudo=" + g_config.nickname +
                  ", log_level=" + mp::log_level_name(g_config.log_level) + ")");
    return true;
}

// Session (story 1.4, probe = 0) : hooks de session (OnProcess, et depuis la story 2.2
// PlayMotion et SetRagDollEnabled pour les avatars), puis empreintes des 4 modules
// (Script.dll est chargée par la résolution des entités). Un échec laisse le jeu en solo.
void start_session() {
    if (!mp::win::hooks::install_session()) {
        mp::log::error("[sync] session impossible : le jeu reste en solo");
        return;
    }
    static const wchar_t* const kModules[] = {L"Game.dll", L"Engine.dll", L"Script.dll", L"SharedBase.dll"};
    std::array<mp::ModuleFingerprint, 4> fingerprint{};
    for (std::size_t i = 0; i < fingerprint.size(); ++i) {
        if (!fingerprint_of(kModules[i], fingerprint[i])) {
            mp::log::error("[sync] session impossible : empreinte illisible : " + to_utf8(kModules[i]));
            mp::win::hooks::remove();
            return;
        }
    }
    mp::sync::session::configure(g_config, fingerprint, MP_MOD_VERSION);
}

void initialize() {
    const std::wstring dir = dll_directory();
    mp::log::open(dir, L"Script_Multiplayer.log");
    mp::log::info("Risen Multiplayer " MP_MOD_VERSION " chargé depuis " + to_utf8(dir));

    const bool version_ok = check_game_version();
    const bool config_ok = load_config(dir);  // lue même si la version est refusée
    // Aucun appel au code du jeu sur une version non reconnue : tables vides.
    const bool descriptor_ok = version_ok && build_descriptor();
    g_mod_active = version_ok && config_ok && descriptor_ok;
    mp::log::info(g_mod_active ? "mod actif" : "mod inactif : le jeu continue en solo");
    if (!g_mod_active) {
        if (config_ok && g_config.probe) mp::log::info("sonde ignorée : mod inactif");
        return;
    }
    // Spike J1 : la sonde n'installe ses hooks que sur demande explicite (probe = 1).
    // Un échec la laisse inactive sans toucher au mod ni au jeu.
    if (g_config.probe) {
        mp::log::info("sonde activée (probe = 1)");
        mp::win::hooks::install_probe();
    } else {
        mp::log::info("sonde désactivée");
        start_session();
    }
}

}  // namespace

extern "C" __declspec(dllexport) void* ScriptInit() {
    if (!g_initialized) {
        g_initialized = true;
        try {
            initialize();
        } catch (...) {
            // std::bad_alloc ou autre : ne jamais propager dans le jeu.
            g_mod_active = false;
            try {
                mp::log::error("exception pendant l'initialisation : mod inactif");
            } catch (...) {
            }
        }
    }
    return &g_descriptor;
}
