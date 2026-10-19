#include "win/watchdog.h"

#include "common/log.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <string>

namespace mp::win::watchdog {

namespace {

constexpr ULONGLONG kStallMs = 4000;
constexpr int kMaxFrames = 48;
constexpr int kTraces = 3;

std::atomic<ULONGLONG> g_last_beat{0};
std::atomic<bool> g_armed{false};
std::atomic<DWORD> g_main_thread{0};
bool g_started = false;

// Déroulé x64 par les tables d'exceptions (.pdata) : ni allocation ni verrou du tas, le
// thread visé étant suspendu (il peut tenir le verrou du tas).
int unwind(CONTEXT ctx, DWORD64* frames) {
    int n = 0;
    while (n < kMaxFrames && ctx.Rip != 0) {
        frames[n++] = ctx.Rip;
        DWORD64 image_base = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &image_base, nullptr);
        if (fn == nullptr) {  // fonction feuille : l'adresse de retour est au sommet de la pile
            if (ctx.Rsp == 0 || IsBadReadPtr(reinterpret_cast<void*>(ctx.Rsp), 8)) break;
            ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
            ctx.Rsp += 8;
            continue;
        }
        void* handler_data = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, fn, &ctx, &handler_data, &establisher, nullptr);
    }
    return n;
}

std::string describe(DWORD64 address) {
    HMODULE module = nullptr;
    char text[400];
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(address), &module) &&
        module != nullptr) {
        char path[MAX_PATH] = {};
        GetModuleFileNameA(module, path, MAX_PATH);
        const char* name = path;
        for (const char* p = path; *p != '\0'; ++p) {
            if (*p == '\\' || *p == '/') name = p + 1;
        }
        std::snprintf(text, sizeof text, "%s+0x%llx", name,
                      static_cast<unsigned long long>(address - reinterpret_cast<DWORD64>(module)));
    } else {
        std::snprintf(text, sizeof text, "0x%llx (hors module)", static_cast<unsigned long long>(address));
    }
    return text;
}

void trace(HANDLE thread, int index) {
    DWORD64 frames[kMaxFrames];
    int n = 0;
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_FULL;
    if (SuspendThread(thread) == static_cast<DWORD>(-1)) {
        log::error("[watchdog] SuspendThread a échoué");
        return;
    }
    const bool ok = GetThreadContext(thread, &ctx) != 0;
    if (ok) n = unwind(ctx, frames);
    ResumeThread(thread);
    if (!ok) {
        log::error("[watchdog] GetThreadContext a échoué");
        return;
    }
    std::string line = "[watchdog] pile " + std::to_string(index) + " du thread principal :";
    for (int i = 0; i < n; ++i) line += "\n    #" + std::to_string(i) + " " + describe(frames[i]);
    log::error(line);
}

DWORD WINAPI run(void*) {
    ULONGLONG reported_beat = 0;  // gel déjà signalé (même dernier battement)
    for (;;) {
        Sleep(500);
        const ULONGLONG beat = g_last_beat.load();
        if (!g_armed.load() || beat == 0 || beat == reported_beat) continue;
        const ULONGLONG stalled = GetTickCount64() - beat;
        if (stalled < kStallMs) continue;
        reported_beat = beat;
        log::error("[watchdog] la frame ne revient plus depuis " + std::to_string(stalled) +
                   " ms (avatar présent) : piles du thread principal");
        HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                                   g_main_thread.load());
        if (thread == nullptr) {
            log::error("[watchdog] OpenThread a échoué");
            continue;
        }
        for (int i = 1; i <= kTraces; ++i) {
            trace(thread, i);
            Sleep(1000);
        }
        CloseHandle(thread);
    }
}

}  // namespace

void start() {
    if (g_started) return;
    g_started = true;
    HANDLE t = CreateThread(nullptr, 0, &run, nullptr, 0, nullptr);
    if (t == nullptr) {
        log::warn("[watchdog] thread non créé : pas de diagnostic de gel");
        return;
    }
    CloseHandle(t);
}

void beat(bool armed) {
    if (g_main_thread.load() == 0) g_main_thread.store(GetCurrentThreadId());
    g_armed.store(armed);
    g_last_beat.store(GetTickCount64());
}

}  // namespace mp::win::watchdog
