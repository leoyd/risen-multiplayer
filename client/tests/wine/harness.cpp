// Banc d'essai Wine : charge les vraies DLL du jeu (SharedBase, Engine, Game) puis le
// mod, et appelle ScriptInit comme le fait gCScriptAdmin::LoadScriptDLL.
// Usage : mp_harness.exe <dossier bin de Risen> <chemin de Script_Multiplayer.dll> [expect-empty]
// Code de sortie : 0 si le descripteur est conforme au contrat du chargeur :
// - par défaut : une seule entrée `MP_Probe` dans la table Script ;
// - avec `expect-empty` : les 4 tables vides (rejet silencieux, version non reconnue).
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <initializer_list>

namespace {

struct Table {
    void* data;
    int count;
    int capacity;
};

struct Entry {
    const char* name;  // bCString : un seul pointeur vers le texte
    const char* description;
    long long (*function)(void*, void*, void*, void*);
};

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 3 && argc != 4) return 2;
    const bool expect_empty = argc == 4 && std::wcscmp(argv[3], L"expect-empty") == 0;
    if (argc == 4 && !expect_empty) return 2;
    SetDllDirectoryW(argv[1]);
    for (const wchar_t* dll : {L"SharedBase.dll", L"Engine.dll", L"Game.dll"}) {
        if (LoadLibraryW(dll) == nullptr) {
            std::wprintf(L"échec du chargement de %ls (erreur %lu)\n", dll, GetLastError());
            return 3;
        }
    }
    HMODULE mod = LoadLibraryW(argv[2]);
    if (mod == nullptr) return 4;
    auto script_init = reinterpret_cast<void* (*)()>(GetProcAddress(mod, "ScriptInit"));
    if (script_init == nullptr) return 5;

    auto* tables = static_cast<Table*>(script_init());
    if (tables == nullptr || script_init() != tables) return 6;
    if (expect_empty) {
        for (int i = 0; i < 4; ++i) {
            if (tables[i].count != 0) return 10;
        }
        std::printf("tables vides (rejet silencieux)\n");
        return 0;
    }
    if (tables[0].count != 0 || tables[1].count != 0 || tables[2].count != 0 || tables[3].count != 1) return 7;

    const auto* entry = static_cast<const Entry*>(tables[3].data);
    if (entry->name == nullptr || std::strcmp(entry->name, "MP_Probe") != 0) return 8;
    if (entry->function(nullptr, nullptr, nullptr, nullptr) != 0) return 9;
    std::printf("descripteur conforme\n");
    return 0;
}
