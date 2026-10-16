// Log horodaté `Script_Multiplayer.log`, à côté de la DLL, tronqué à chaque lancement.
// Si le fichier ne peut pas être ouvert (dossier en lecture seule…), le log est
// silencieusement désactivé et le mod continue.
#pragma once

#include <string>

namespace mp::log {

// Ouvre (et tronque) le fichier `<dossier de la DLL>\<file_name>`.
void open(const std::wstring& dll_dir, const wchar_t* file_name);

// Active ou coupe les lignes `debug` (config `log_level`). Coupé par défaut.
void set_debug(bool enabled);
bool debug_enabled();

// Écrit une ligne `[AAAA-MM-JJ HH:MM:SS.mmm] <niveau> <message>`. Message en UTF-8.
// `debug` n'écrit rien tant que set_debug(true) n'a pas été appelé.
void debug(const std::string& message);
void info(const std::string& message);
void warn(const std::string& message);
void error(const std::string& message);

}  // namespace mp::log
