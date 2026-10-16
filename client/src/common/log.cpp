#include "common/log.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>

namespace mp::log {

namespace {

HANDLE g_file = INVALID_HANDLE_VALUE;
SRWLOCK g_lock = SRWLOCK_INIT;
volatile LONG g_debug = 0;  // lu depuis n'importe quel thread

void write_line(const char* level, const std::string& message) {
    if (g_file == INVALID_HANDLE_VALUE) return;

    SYSTEMTIME t;
    GetLocalTime(&t);
    char prefix[64];
    const int n = std::snprintf(prefix, sizeof prefix, "[%04u-%02u-%02u %02u:%02u:%02u.%03u] %-5s ",
                                t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
                                t.wMilliseconds, level);
    std::string line;
    line.reserve(static_cast<std::size_t>(n > 0 ? n : 0) + message.size() + 2);
    if (n > 0) line.append(prefix, static_cast<std::size_t>(n));
    line += message;
    line += "\r\n";

    AcquireSRWLockExclusive(&g_lock);
    DWORD written = 0;
    WriteFile(g_file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    ReleaseSRWLockExclusive(&g_lock);
}

}  // namespace

void open(const std::wstring& dll_dir, const wchar_t* file_name) {
    if (g_file != INVALID_HANDLE_VALUE) return;
    std::wstring path = dll_dir;
    if (!path.empty() && path.back() != L'\\' && path.back() != L'/') path += L'\\';
    path += file_name;
    // FILE_SHARE_READ : le log reste lisible pendant que le jeu tourne.
    // Écritures non bufferisées côté processus : rien n'est perdu en cas de crash du jeu.
    g_file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_file != INVALID_HANDLE_VALUE) {
        static const char kBom[] = "\xEF\xBB\xBF";
        DWORD written = 0;
        WriteFile(g_file, kBom, 3, &written, nullptr);
    }
}

void set_debug(bool enabled) { InterlockedExchange(&g_debug, enabled ? 1 : 0); }
bool debug_enabled() { return InterlockedCompareExchange(&g_debug, 0, 0) != 0; }

void debug(const std::string& message) {
    if (debug_enabled()) write_line("DEBUG", message);
}
void info(const std::string& message) { write_line("INFO", message); }
void warn(const std::string& message) { write_line("WARN", message); }
void error(const std::string& message) { write_line("ERROR", message); }

}  // namespace mp::log
