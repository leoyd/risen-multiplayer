#include "win/overlay.h"

#include "common/log.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <d3d9.h>

#include <MinHook.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <string>

namespace mp::win::overlay {

namespace {

// --- ID3DXFont, déclarée à la main (pas de SDK DirectX) ----------------------------------------
// Seul l'ordre de la vtable compte ; les méthodes jamais appelées ont des signatures simplifiées.
struct ID3DXFontLike {
    virtual HRESULT __stdcall QueryInterface(REFIID, void**) = 0;
    virtual ULONG __stdcall AddRef() = 0;
    virtual ULONG __stdcall Release() = 0;
    virtual HRESULT __stdcall GetDevice(IDirect3DDevice9**) = 0;
    virtual HRESULT __stdcall GetDescA(void*) = 0;
    virtual HRESULT __stdcall GetDescW(void*) = 0;
    virtual BOOL __stdcall GetTextMetricsA(void*) = 0;
    virtual BOOL __stdcall GetTextMetricsW(void*) = 0;
    virtual HDC __stdcall GetDC() = 0;
    virtual HRESULT __stdcall GetGlyphData(UINT, void**, RECT*, POINT*) = 0;
    virtual HRESULT __stdcall PreloadCharacters(UINT, UINT) = 0;
    virtual HRESULT __stdcall PreloadGlyphs(UINT, UINT) = 0;
    virtual HRESULT __stdcall PreloadTextA(LPCSTR, INT) = 0;
    virtual HRESULT __stdcall PreloadTextW(LPCWSTR, INT) = 0;
    virtual INT __stdcall DrawTextA(void* sprite, LPCSTR, INT, RECT*, DWORD, D3DCOLOR) = 0;
    virtual INT __stdcall DrawTextW(void* sprite, LPCWSTR, INT, RECT*, DWORD, D3DCOLOR) = 0;
    virtual HRESULT __stdcall OnLostDevice() = 0;
    virtual HRESULT __stdcall OnResetDevice() = 0;
};

using D3DXCreateFontWFn = HRESULT(WINAPI*)(IDirect3DDevice9* device, INT height, UINT width, UINT weight,
                                            UINT mip_levels, BOOL italic, DWORD charset, DWORD output_precision,
                                            DWORD quality, DWORD pitch_and_family, LPCWSTR face_name,
                                            ID3DXFontLike** font);
using Direct3DCreate9Fn = IDirect3D9*(WINAPI*)(UINT sdk_version);
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using ResetFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);

constexpr std::size_t kPresentSlot = 17;
constexpr std::size_t kResetSlot = 16;
constexpr std::size_t kMaxText = 256;  // caractères UTF-16, terminateur compris

D3DXCreateFontWFn g_create_font = nullptr;
PresentFn g_original_present = nullptr;
ResetFn g_original_reset = nullptr;
void* g_present_target = nullptr;
void* g_reset_target = nullptr;
bool g_installed = false;

// Texte partagé frame → détour : seul état partagé, sous verrou court.
SRWLOCK g_text_lock = SRWLOCK_INIT;
wchar_t g_text[kMaxText] = {};
D3DCOLOR g_color = 0;

// État du thread de rendu (Present/Reset), jamais lu ailleurs.
ID3DXFontLike* g_font = nullptr;
IDirect3DDevice9* g_font_device = nullptr;   // device pour lequel g_font a été créée
IDirect3DDevice9* g_failed_device = nullptr; // création échouée sur ce device : pas de nouvel essai
bool g_first_draw_logged = false;
bool g_draw_fault_logged = false;

// --- Dessin (thread de rendu) ----------------------------------------------------------------

void release_font() {
    if (g_font != nullptr) g_font->Release();
    g_font = nullptr;
    g_font_device = nullptr;
}

// Corps du dessin, sans objet C++ à détruire (appelé sous __try). Retourne false si la police
// n'a pas pu être créée.
bool draw_raw(IDirect3DDevice9* device, const wchar_t* text, D3DCOLOR color) {
    if (g_font_device != device) {
        release_font();
        if (device == g_failed_device) return false;
        ID3DXFontLike* font = nullptr;
        const HRESULT hr = g_create_font(device, 18, 0, FW_BOLD, 1, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                         ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Arial", &font);
        if (FAILED(hr) || font == nullptr) {
            g_failed_device = device;
            return false;
        }
        g_font = font;
        g_font_device = device;
    }
    IDirect3DSurface9* old_target = nullptr;
    IDirect3DSurface9* back_buffer = nullptr;
    D3DVIEWPORT9 old_viewport{};
    const bool have_viewport = SUCCEEDED(device->GetViewport(&old_viewport));
    if (FAILED(device->GetRenderTarget(0, &old_target))) old_target = nullptr;
    if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back_buffer))) back_buffer = nullptr;
    if (back_buffer != nullptr) {
        device->SetRenderTarget(0, back_buffer);
        if (SUCCEEDED(device->BeginScene())) {
            RECT shadow = {11, 9, 4096, 64};
            RECT rect = {10, 8, 4096, 64};
            g_font->DrawTextW(nullptr, text, -1, &shadow, DT_NOCLIP | DT_LEFT | DT_TOP, 0xFF000000);
            g_font->DrawTextW(nullptr, text, -1, &rect, DT_NOCLIP | DT_LEFT | DT_TOP, color);
            device->EndScene();
        }
        back_buffer->Release();
    }
    if (old_target != nullptr) {
        device->SetRenderTarget(0, old_target);
        old_target->Release();
    }
    if (have_viewport) device->SetViewport(&old_viewport);  // SetRenderTarget remet le viewport
    return true;
}

enum class DrawResult { Ok, NoFont, Fault };

DrawResult draw_guarded(IDirect3DDevice9* device, const wchar_t* text, D3DCOLOR color) {
    __try {
        return draw_raw(device, text, color) ? DrawResult::Ok : DrawResult::NoFont;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_font = nullptr;  // état inconnu : abandonnée (fuite volontaire), recréée au prochain passage
        g_font_device = nullptr;
        return DrawResult::Fault;
    }
}

void log_noexcept(bool warn, const char* prefix, unsigned long value) {
    try {
        const std::string line = std::string(prefix) + std::to_string(value);
        if (warn) {
            log::warn(line);
        } else {
            log::info(line);
        }
    } catch (...) {
    }
}

void draw(IDirect3DDevice9* device) {
    wchar_t text[kMaxText];
    D3DCOLOR color = 0;
    AcquireSRWLockShared(&g_text_lock);
    std::wmemcpy(text, g_text, kMaxText);
    color = g_color;
    ReleaseSRWLockShared(&g_text_lock);
    if (text[0] == L'\0') return;
    const bool failed_before = g_failed_device == device;
    switch (draw_guarded(device, text, color)) {
        case DrawResult::Ok:
            if (!g_first_draw_logged) {
                g_first_draw_logged = true;
                log_noexcept(false, "[overlay] premier dessin, thread ", GetCurrentThreadId());
            }
            break;
        case DrawResult::NoFont:
            if (!failed_before) log_noexcept(true, "[overlay] inactif : D3DXCreateFontW a échoué, thread ",
                                             GetCurrentThreadId());
            break;
        case DrawResult::Fault:
            if (!g_draw_fault_logged) {
                g_draw_fault_logged = true;
                log_noexcept(true, "[overlay] exception pendant le dessin (ignorée), thread ", GetCurrentThreadId());
            }
            break;
    }
}

HRESULT STDMETHODCALLTYPE detour_present(IDirect3DDevice9* device, const RECT* source, const RECT* dest,
                                         HWND window, const RGNDATA* dirty) {
    if (device != nullptr) draw(device);  // ne lève jamais : SEH + try/catch du log
    return g_original_present(device, source, dest, window, dirty);
}

void lost_guarded(IDirect3DDevice9* device) {
    __try {
        if (g_font != nullptr && g_font_device == device) g_font->OnLostDevice();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_font = nullptr;
        g_font_device = nullptr;
    }
}

void reset_guarded(IDirect3DDevice9* device) {
    __try {
        if (g_font != nullptr && g_font_device == device) g_font->OnResetDevice();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_font = nullptr;
        g_font_device = nullptr;
    }
}

HRESULT STDMETHODCALLTYPE detour_reset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* params) {
    lost_guarded(device);  // ressources de la police relâchées avant l'original
    const HRESULT hr = g_original_reset(device, params);
    if (SUCCEEDED(hr)) reset_guarded(device);
    return hr;
}

// --- Installation (thread de ScriptInit) -------------------------------------------------------

LRESULT CALLBACK dummy_window_proc(HWND window, UINT message, WPARAM w, LPARAM l) {
    return DefWindowProcW(window, message, w, l);
}

// Lit Present et Reset dans la vtable d'un device factice (fenêtre cachée 1×1, fenêtré), puis
// libère tout. Retourne une cause d'échec, vide si les deux adresses sont lues.
std::string read_device_vtable(void*& present, void*& reset) {
    HMODULE d3d9 = LoadLibraryW(L"d3d9.dll");  // déjà chargée par Engine.dll : simple référence
    if (d3d9 == nullptr) return "d3d9.dll introuvable";
    auto create9 = reinterpret_cast<Direct3DCreate9Fn>(GetProcAddress(d3d9, "Direct3DCreate9"));
    if (create9 == nullptr) return "Direct3DCreate9 introuvable";

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &dummy_window_proc;
    wc.hInstance = instance;
    wc.lpszClassName = L"RisenMultiplayerOverlayDummy";
    const ATOM atom = RegisterClassExW(&wc);
    if (atom == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return "RegisterClassExW a échoué";
    HWND window = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 1, 1, nullptr, nullptr,
                                  instance, nullptr);
    std::string error;
    if (window == nullptr) {
        error = "fenêtre factice impossible";
    } else {
        IDirect3D9* d3d = create9(D3D_SDK_VERSION);
        if (d3d == nullptr) {
            error = "Direct3DCreate9 a échoué";
        } else {
            D3DPRESENT_PARAMETERS pp{};
            pp.Windowed = TRUE;
            pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
            pp.BackBufferFormat = D3DFMT_UNKNOWN;
            pp.BackBufferWidth = 1;
            pp.BackBufferHeight = 1;
            pp.hDeviceWindow = window;
            IDirect3DDevice9* device = nullptr;
            const HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
                                                 D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device);
            if (FAILED(hr) || device == nullptr) {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "device factice impossible (0x%08lx)", static_cast<unsigned long>(hr));
                error = buf;
            } else {
                void** vtable = *reinterpret_cast<void***>(device);
                present = vtable[kPresentSlot];
                reset = vtable[kResetSlot];
                device->Release();  // libéré avant la pose des détours
            }
            d3d->Release();
        }
        DestroyWindow(window);
    }
    UnregisterClassW(wc.lpszClassName, instance);
    return error;
}

}  // namespace

bool install() {
    if (g_installed) return true;
    HMODULE d3dx = GetModuleHandleW(L"d3dx9_39.dll");
    if (d3dx == nullptr) {
        log::warn("[overlay] inactif : d3dx9_39.dll non chargée");
        return false;
    }
    g_create_font = reinterpret_cast<D3DXCreateFontWFn>(GetProcAddress(d3dx, "D3DXCreateFontW"));
    if (g_create_font == nullptr) {
        log::warn("[overlay] inactif : D3DXCreateFontW introuvable dans d3dx9_39.dll");
        return false;
    }
    void* present = nullptr;
    void* reset = nullptr;
    const std::string error = read_device_vtable(present, reset);
    if (!error.empty()) {
        log::warn("[overlay] inactif : " + error);
        return false;
    }
    MH_STATUS s = MH_CreateHook(present, reinterpret_cast<void*>(&detour_present),
                                reinterpret_cast<void**>(&g_original_present));
    if (s != MH_OK) {
        log::warn(std::string("[overlay] inactif : MH_CreateHook(Present) : ") + MH_StatusToString(s));
        return false;
    }
    s = MH_CreateHook(reset, reinterpret_cast<void*>(&detour_reset), reinterpret_cast<void**>(&g_original_reset));
    if (s != MH_OK) {
        MH_RemoveHook(present);
        log::warn(std::string("[overlay] inactif : MH_CreateHook(Reset) : ") + MH_StatusToString(s));
        return false;
    }
    // Reset d'abord : Present ne dessine qu'avec une police déjà gérée par le détour de Reset.
    s = MH_EnableHook(reset);
    if (s == MH_OK) s = MH_EnableHook(present);
    if (s != MH_OK) {
        MH_DisableHook(present);
        MH_DisableHook(reset);
        MH_RemoveHook(present);
        MH_RemoveHook(reset);
        log::warn(std::string("[overlay] inactif : MH_EnableHook : ") + MH_StatusToString(s));
        return false;
    }
    g_present_target = present;
    g_reset_target = reset;
    g_installed = true;
    log::info("[overlay] détours posés (IDirect3DDevice9::Present, Reset)");
    return true;
}

void remove() {
    if (!g_installed) return;
    MH_DisableHook(g_present_target);
    MH_DisableHook(g_reset_target);
    MH_RemoveHook(g_present_target);
    MH_RemoveHook(g_reset_target);
    g_installed = false;
    // Trampolines conservés (même règle que win/hooks) ; la police reste allouée (durée du processus).
}

void set_text(const std::string& utf8, std::uint32_t argb) {
    wchar_t wide[kMaxText] = {};
    std::wstring converted;
    if (!utf8.empty()) {
        const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
        if (n > 0) {
            converted.resize(static_cast<std::size_t>(n));
            MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), converted.data(), n);
        }
    }
    const std::size_t len = std::min(converted.size(), kMaxText - 1);  // tronqué si trop long
    std::wmemcpy(wide, converted.data(), len);
    AcquireSRWLockExclusive(&g_text_lock);
    std::wmemcpy(g_text, wide, kMaxText);
    g_color = argb;
    ReleaseSRWLockExclusive(&g_text_lock);
}

}  // namespace mp::win::overlay
