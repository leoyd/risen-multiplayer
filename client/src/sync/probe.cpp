#include "sync/probe.h"

#include "common/log.h"
#include "game/entity.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace mp::sync::probe {

namespace {

constexpr std::size_t kDescDumpSize = 0x60;
constexpr std::size_t kCaptureRing = 64;
constexpr int kMaxAnimLinesPerSecond = 20;
constexpr float kSpawnDistance = 200.0f;  // 2 m, si les unités sont des centimètres
constexpr ULONGLONG kTickPeriodMs = 1000;

struct Capture {
    int type;
    bool flag;
    bool desc_null;
    bool desc_readable;
    void* desc;
    void* resource;
    std::uint8_t bytes[kDescDumpSize];
};

// --- État partagé avec le hook PlayMotion (protégé par g_lock) ----------------------
SRWLOCK g_lock = SRWLOCK_INIT;
Capture g_ring[kCaptureRing];
std::size_t g_ring_head = 0;  // prochaine écriture
std::size_t g_ring_count = 0;
unsigned g_ring_dropped = 0;  // captures perdues (anneau plein)
Capture g_last{};             // dernière capture, pour F7
bool g_have_last = false;
std::atomic<void*> g_player_anim_ps{nullptr};

// --- État du thread principal uniquement ------------------------------------------
DWORD g_first_thread = 0;
bool g_started = false;
ULONGLONG g_window_start = 0;
unsigned g_frames = 0;
int g_anim_lines = 0;
unsigned g_anim_skipped = 0;   // non journalisées (limite de débit)
unsigned g_ring_overflow = 0;  // perdues : anneau plein (> 64 PlayMotion en une frame)
bool g_f6_down = false;
bool g_f7_down = false;
game::Entity g_copy;          // oubliée dès que le joueur change (chargement, menu)
void* g_last_player = nullptr;

std::string hex_ptr(const void* p) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(p)));
    return buf;
}

std::string format_matrix(const game::Matrix& m) {
    std::string s;
    char buf[32];
    for (int i = 0; i < 16; ++i) {
        std::snprintf(buf, sizeof buf, i == 0 ? "%.2f" : (i % 4 == 0 ? " | %.2f" : " %.2f"),
                      static_cast<double>(m.m[i]));
        s += buf;
    }
    return s;
}

std::string format_position(const game::Matrix& m) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "(%.1f, %.1f, %.1f)", static_cast<double>(m.m[12]),
                  static_cast<double>(m.m[13]), static_cast<double>(m.m[14]));
    return buf;
}

std::string hex_dump(const std::uint8_t* b, std::size_t n) {
    static const char kHex[] = "0123456789abcdef";
    std::string s;
    s.reserve(n * 3);
    for (std::size_t i = 0; i < n; ++i) {
        if (i != 0) s += (i % 16 == 0) ? '|' : ' ';
        s += kHex[b[i] >> 4];
        s += kHex[b[i] & 0xf];
    }
    return s;
}

std::string describe(const Capture& c) {
    std::string line = "[probe] PlayMotion type=" + std::to_string(c.type) + " flag=" + (c.flag ? "1" : "0") +
                       " ressource=" + hex_ptr(c.resource) + " desc=" + hex_ptr(c.desc);
    if (c.desc_null) return line + " (descripteur nul)";
    if (!c.desc_readable) return line + " (descripteur illisible)";
    return line + " : " + hex_dump(c.bytes, kDescDumpSize);
}

bool game_has_focus() {
    DWORD pid = 0;
    const HWND fg = GetForegroundWindow();
    return fg != nullptr && GetWindowThreadProcessId(fg, &pid) != 0 && pid == GetCurrentProcessId();
}

// Front montant d'une touche (état courant via le bit de poids fort).
bool pressed(int vk, bool& was_down) {
    const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    const bool edge = down && !was_down;
    was_down = down;
    return edge;
}

void flush_captures() {
    Capture batch[kCaptureRing];
    std::size_t n = 0;
    unsigned dropped = 0;
    AcquireSRWLockExclusive(&g_lock);
    const std::size_t start = (g_ring_head + kCaptureRing - g_ring_count) % kCaptureRing;
    for (; n < g_ring_count; ++n) batch[n] = g_ring[(start + n) % kCaptureRing];
    g_ring_count = 0;
    dropped = g_ring_dropped;
    g_ring_dropped = 0;
    ReleaseSRWLockExclusive(&g_lock);

    g_ring_overflow += dropped;
    for (std::size_t i = 0; i < n; ++i) {
        if (g_anim_lines >= kMaxAnimLinesPerSecond) {
            ++g_anim_skipped;
            continue;
        }
        ++g_anim_lines;
        log::info(describe(batch[i]));
    }
}

void log_tick(ULONGLONG now) {
    const DWORD tid = GetCurrentThreadId();
    const double seconds = static_cast<double>(now - g_window_start) / 1000.0;
    char fps[32];
    std::snprintf(fps, sizeof fps, "%.1f", seconds > 0 ? g_frames / seconds : 0.0);

    game::Entity player;
    const bool call_ok = game::get_player(player);
    std::string line = "[probe] tick thread=" + std::to_string(tid) +
                       (tid == g_first_thread ? " (identique au 1er appel)" : " (DIFFÉRENT du 1er appel)") +
                       " images/s=" + fps + " joueur=";
    if (!call_ok) {
        line += "erreur (SEH dans GetPlayer)";
    } else if (player.ptr == nullptr) {
        line += "non";
    } else {
        line += "oui " + hex_ptr(player.ptr);
        game::Matrix pose;
        if (game::get_pose(player, pose)) {
            line += " pos=" + format_position(pose) + " pose=[" + format_matrix(pose) + "]";
        } else {
            line += " pose illisible";
        }
        line += " anim_ps=" + hex_ptr(g_player_anim_ps.load());
    }
    log::info(line);
    if (g_anim_skipped != 0) {
        log::info("[probe] " + std::to_string(g_anim_skipped) + " PlayMotion non journalisés (limite de débit)");
    }
    if (g_ring_overflow != 0) {
        log::info("[probe] " + std::to_string(g_ring_overflow) + " PlayMotion perdus (anneau de " +
                  std::to_string(kCaptureRing) + " plein en une frame)");
    }
    g_anim_skipped = 0;
    g_ring_overflow = 0;
    g_anim_lines = 0;
}

void spawn_copy() {
    if (g_copy.ptr != nullptr) {
        log::info("[probe] F6 : une copie existe déjà (" + hex_ptr(g_copy.ptr) + "), une seule par partie chargée");
        return;
    }
    game::Entity player;
    if (!game::get_player(player) || player.ptr == nullptr) {
        log::info("[probe] F6 : pas de joueur");
        return;
    }
    game::Matrix pose;
    if (!game::get_pose(player, pose)) {
        log::info("[probe] F6 : pose du joueur illisible");
        return;
    }
    game::Template tmpl;
    if (!game::get_template(player, tmpl) || tmpl.proxy == nullptr) {
        log::info("[probe] F6 : GetTemplate a échoué");
        if (tmpl.proxy != nullptr) game::destroy_template(tmpl);
        return;
    }
    game::Matrix target = pose;
    for (int i = 0; i < 3; ++i) target.m[12 + i] += kSpawnDistance * pose.m[8 + i];  // [ASSUMPTION] axe avant

    game::Entity copy;
    const bool ok = game::spawn(tmpl, target, /*temporary=*/true, copy);  // tmpl consommé
    log::info("[probe] F6 : Spawn " + std::string(ok ? "appelé" : "a levé une exception (SEH)") +
              ", copie=" + hex_ptr(copy.ptr) + ", pose demandée=[" + format_matrix(target) + "]");
    if (!ok || copy.ptr == nullptr) return;
    g_copy = copy;

    game::Matrix copy_pose;
    log::info("[probe] F6 : pose de la copie=" +
              (game::get_pose(copy, copy_pose) ? "[" + format_matrix(copy_pose) + "]" : std::string("illisible")));
    game::Entity after;
    if (!game::get_player(after)) {
        log::info("[probe] F6 : GetPlayer après Spawn a levé une exception (SEH)");
        return;
    }
    log::info("[probe] F6 : joueur après Spawn=" + hex_ptr(after.ptr) +
              (after.ptr == player.ptr ? " (inchangé)" : " (CHANGÉ : la copie a peut-être pris le contrôle)"));
}

void replay_on_copy() {
    if (g_copy.ptr == nullptr) {
        log::info("[probe] F7 : pas de copie (F6 d'abord)");
        return;
    }
    Capture last;
    bool have = false;
    AcquireSRWLockShared(&g_lock);
    have = g_have_last;
    last = g_last;
    ReleaseSRWLockShared(&g_lock);
    if (!have) {
        log::info("[probe] F7 : aucune animation capturée");
        return;
    }
    void* anim_ps = nullptr;
    if (!game::get_animation_ps(g_copy, anim_ps) || anim_ps == nullptr) {
        log::info("[probe] F7 : pas d'eCAnimation_PS sur la copie");
        return;
    }
    // Le descripteur rejoué est notre copie de 0x60 octets (ou nul si l'original l'était).
    static std::uint8_t desc[kDescDumpSize];
    void* desc_arg = nullptr;
    if (!last.desc_null && last.desc_readable) {
        std::memcpy(desc, last.bytes, kDescDumpSize);
        desc_arg = desc;
    }
    void* resource = nullptr;
    game::read_motion_resource(anim_ps, last.type, resource);
    const bool ok = game::play_motion(anim_ps, last.type, desc_arg, last.flag);
    log::info("[probe] F7 : PlayMotion(type=" + std::to_string(last.type) + ") sur la copie " +
              (ok ? "appelé" : "a levé une exception (SEH)") + ", anim_ps=" + hex_ptr(anim_ps) +
              ", ressource de la copie=" + hex_ptr(resource) + " (joueur : " + hex_ptr(last.resource) + ")");
}

void frame() {
    const ULONGLONG now = GetTickCount64();
    if (!g_started) {
        g_started = true;
        g_first_thread = GetCurrentThreadId();
        g_window_start = now;
        log::info("[probe] premier OnProcess, thread=" + std::to_string(g_first_thread));
    }
    ++g_frames;

    // Mise à jour du filtre du hook PlayMotion.
    game::Entity player;
    void* anim_ps = nullptr;
    if (game::get_player(player) && player.ptr != nullptr) game::get_animation_ps(player, anim_ps);
    g_player_anim_ps.store(anim_ps);
    if (player.ptr != g_last_player) {
        // Chargement, retour au menu ou prise de contrôle : la copie temporaire a pu
        // être détruite avec le monde. Ne jamais réutiliser un pointeur peut-être libéré.
        if (g_copy.ptr != nullptr) log::info("[probe] joueur changé : copie " + hex_ptr(g_copy.ptr) + " oubliée");
        g_copy = game::Entity{};
        g_last_player = player.ptr;
    }

    flush_captures();

    if (game_has_focus()) {
        if (pressed(VK_F6, g_f6_down)) spawn_copy();
        if (pressed(VK_F7, g_f7_down)) replay_on_copy();
    } else {
        // Hors focus : suivre l'état réel, pour qu'une touche tenue au retour ne déclenche rien.
        pressed(VK_F6, g_f6_down);
        pressed(VK_F7, g_f7_down);
    }

    if (now - g_window_start >= kTickPeriodMs) {
        log_tick(now);
        g_window_start = now;
        g_frames = 0;
    }
}

}  // namespace

void on_frame() noexcept {
    try {
        frame();
    } catch (...) {
        // std::bad_alloc… : ne jamais propager dans le jeu.
    }
}

void on_play_motion(void* animation_ps, int type, void* desc, bool flag) noexcept {
    if (animation_ps == nullptr || animation_ps != g_player_anim_ps.load()) return;
    Capture c{};
    c.type = type;
    c.flag = flag;
    c.desc = desc;
    c.desc_null = desc == nullptr;
    c.desc_readable = !c.desc_null && game::copy_bytes(desc, c.bytes, kDescDumpSize);
    game::read_motion_resource(animation_ps, type, c.resource);

    AcquireSRWLockExclusive(&g_lock);
    if (g_ring_count == kCaptureRing) {
        ++g_ring_dropped;
    } else {
        g_ring[g_ring_head] = c;
        g_ring_head = (g_ring_head + 1) % kCaptureRing;
        ++g_ring_count;
    }
    g_last = c;
    g_have_last = true;
    ReleaseSRWLockExclusive(&g_lock);
}

}  // namespace mp::sync::probe
