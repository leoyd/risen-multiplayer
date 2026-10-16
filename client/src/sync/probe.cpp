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
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace mp::sync::probe {

namespace {

constexpr std::size_t kDescDumpSize = 0x60;
constexpr std::size_t kCaptureRing = 64;
constexpr int kMaxAnimLinesPerSecond = 20;
constexpr float kSpawnDistance = 200.0f;  // 2 m, si les unités sont des centimètres
constexpr ULONGLONG kTickPeriodMs = 1000;
constexpr float kMaxNpcDistance = 1500.0f;  // F4 : PNJ à 15 m au plus

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
Capture g_last{};             // dernière capture
bool g_have_last = false;
std::atomic<void*> g_player_anim_ps{nullptr};
// Diagnostic de la copie : PlayMotion lancés par le moteur sur son PS (hors rejeu F7).
std::atomic<void*> g_copy_anim_ps{nullptr};
std::atomic<unsigned> g_copy_engine_motions{0};
std::atomic<unsigned> g_copy_blocked_motions{0};  // bloqués en mode miroir
std::atomic<bool> g_mirror{false};                // mode miroir (F7), lu par le hook

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
bool g_f4_down = false;
bool g_replaying = false;
float g_mirror_offset[3] = {0, 0, 0};  // position copie - joueur à l'activation du miroir
unsigned g_mirror_replays = 0;         // PlayMotion rejoués sur la copie (par seconde)
unsigned g_mirror_failures = 0;     // vrai pendant notre propre PlayMotion (même thread que le hook)
game::Entity g_copy;          // oubliée dès que le joueur change (chargement, menu)
void* g_last_player = nullptr;
// Classées par nom depuis le tick : rejouées en priorité par F4.
Capture g_last_attack{};      // nom contenant « Attack_ » (coup), hors « Recover »/« Stumble »
bool g_have_last_attack = false;
Capture g_last_other{};       // ni « _Move_ » ni « _Ambient_ »
bool g_have_last_other = false;

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

// Noms des ressources de motion, par pointeur (les ressources sont partagées et
// stables). Thread principal uniquement : GetFileTitleA est un appel au jeu.
std::unordered_map<const void*, std::string> g_names;

const std::string& motion_name(const void* resource) {
    auto it = g_names.find(resource);
    if (it != g_names.end()) return it->second;
    std::string name;
    if (resource == nullptr) {
        name = "(aucune)";
    } else if (!game::get_resource_name(resource, name)) {
        name = "(nom illisible)";
    } else if (name.empty()) {
        name = "(nom vide)";
    }
    return g_names.emplace(resource, std::move(name)).first->second;
}

std::string describe(const Capture& c) {
    std::string line = "[probe] PlayMotion type=" + std::to_string(c.type) + " nom=« " + motion_name(c.resource) +
                       " » flag=" + (c.flag ? "1" : "0") + " ressource=" + hex_ptr(c.resource) +
                       " desc=" + hex_ptr(c.desc);
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

bool replay_capture(void* anim_ps, const Capture& c, bool& set_ok);

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
    void* copy_ps = g_mirror.load() ? g_copy_anim_ps.load() : nullptr;
    for (std::size_t i = 0; i < n && copy_ps != nullptr; ++i) {
        bool set_ok = false;
        if (replay_capture(copy_ps, batch[i], set_ok)) {
            ++g_mirror_replays;
        } else {
            ++g_mirror_failures;
        }
    }
    for (std::size_t i = 0; i < n; ++i) {
        const std::string& name = motion_name(batch[i].resource);
        if (name.find("Attack_") != std::string::npos && name.find("Recover") == std::string::npos &&
            name.find("Stumble") == std::string::npos) {
            g_last_attack = batch[i];
            g_have_last_attack = true;
        } else if (name.find("_Move_") == std::string::npos && name.find("_Ambient_") == std::string::npos) {
            g_last_other = batch[i];
            g_have_last_other = true;
        }
    }
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
    if (g_copy.ptr != nullptr) {
        // Diagnostic « copie figée » : le moteur la met-il à jour et l'anime-t-il ?
        bool in_range = false;
        const bool range_ok = game::is_processing_range_entered(g_copy, in_range);
        void* copy_ps = g_copy_anim_ps.load();
        void* slot0 = nullptr;
        game::read_motion_resource(copy_ps, 0, slot0);
        game::Matrix copy_pose;
        const bool pose_ok = game::get_pose(g_copy, copy_pose);
        log::info("[probe] copie " + hex_ptr(g_copy.ptr) + " : zone de traitement=" +
                  (range_ok ? (in_range ? "oui" : "NON") : "illisible") + ", PlayMotion du moteur=" +
                  std::to_string(g_copy_engine_motions.exchange(0)) + "/s" +
                  (g_mirror.load() ? ", miroir : " + std::to_string(g_mirror_replays) + " rejoués, " +
                                         std::to_string(g_mirror_failures) + " échecs, " +
                                         std::to_string(g_copy_blocked_motions.exchange(0)) + " bloqués/s"
                                   : std::string()) +
                  ", emplacement 0 « " + motion_name(slot0) +
                  " », pos=" + (pose_ok ? format_position(copy_pose) : std::string("illisible")));
    }
    if (g_anim_skipped != 0) {
        log::info("[probe] " + std::to_string(g_anim_skipped) + " PlayMotion non journalisés (limite de débit)");
    }
    if (g_ring_overflow != 0) {
        log::info("[probe] " + std::to_string(g_ring_overflow) + " PlayMotion perdus (anneau de " +
                  std::to_string(kCaptureRing) + " plein en une frame)");
    }
    g_anim_skipped = 0;
    g_ring_overflow = 0;
    g_mirror_replays = g_mirror_failures = 0;
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
    void* copy_ps = nullptr;
    game::get_animation_ps(copy, copy_ps);
    g_copy_anim_ps.store(copy_ps);
    g_copy_engine_motions.store(0);

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

// SetMotion (ressource du joueur) puis PlayMotion avec une copie du descripteur, sur
// `anim_ps`. `g_replaying` laisse passer cet appel dans le hook.
bool replay_capture(void* anim_ps, const Capture& c, bool& set_ok) {
    static std::uint8_t desc[kDescDumpSize];
    void* desc_arg = nullptr;
    if (!c.desc_null && c.desc_readable) {
        std::memcpy(desc, c.bytes, kDescDumpSize);
        desc_arg = desc;
    }
    set_ok = game::set_motion(anim_ps, c.type, c.resource);
    if (!set_ok) return false;
    g_replaying = true;
    const bool ok = game::play_motion(anim_ps, c.type, desc_arg, c.flag);
    g_replaying = false;
    return ok;
}

// Rejoue sur `target` la dernière attaque capturée, sinon la dernière animation hors
// locomotion, sinon la dernière tout court : SetMotion (ressource du joueur) puis PlayMotion.
void replay_on(const game::Entity& target, const char* key, const std::string& label) {
    Capture last;
    const char* kind = nullptr;
    if (g_have_last_attack) {
        last = g_last_attack;
        kind = "dernière attaque";
    } else if (g_have_last_other) {
        last = g_last_other;
        kind = "dernière action";
    } else {
        AcquireSRWLockShared(&g_lock);
        if (g_have_last) {
            last = g_last;
            kind = "dernière animation";
        }
        ReleaseSRWLockShared(&g_lock);
    }
    if (kind == nullptr) {
        log::info(std::string("[probe] ") + key + " : aucune animation capturée");
        return;
    }
    void* anim_ps = nullptr;
    if (!game::get_animation_ps(target, anim_ps) || anim_ps == nullptr) {
        log::info(std::string("[probe] ") + key + " : pas d'eCAnimation_PS sur " + label);
        return;
    }
    void* before = nullptr;
    game::read_motion_resource(anim_ps, last.type, before);
    bool set_ok = false;
    const bool ok = replay_capture(anim_ps, last, set_ok);
    void* after = nullptr;
    game::read_motion_resource(anim_ps, last.type, after);
    log::info(std::string("[probe] ") + key + " : " + kind + " « " + motion_name(last.resource) +
              " » (type=" + std::to_string(last.type) + ") rejouée sur " + label + " : SetMotion " +
              (set_ok ? "OK" : "SEH") + ", PlayMotion " + (ok ? "appelé" : (set_ok ? "SEH" : "non appelé")) +
              ", emplacement " + hex_ptr(before) + " « " + motion_name(before) + " » -> " + hex_ptr(after) +
              (after == last.resource ? " (ressource du joueur)" : " (INATTENDU)"));
}

void toggle_mirror() {
    if (g_copy.ptr == nullptr) {
        log::info("[probe] F7 : pas de copie (F6 d'abord)");
        return;
    }
    if (g_mirror.load()) {
        g_mirror.store(false);
        log::info("[probe] F7 : miroir désactivé (la copie reprend ses propres animations)");
        return;
    }
    game::Entity player;
    game::Matrix pp, cp;
    if (!game::get_player(player) || player.ptr == nullptr || !game::get_pose(player, pp) ||
        !game::get_pose(g_copy, cp)) {
        log::info("[probe] F7 : pose du joueur ou de la copie illisible");
        return;
    }
    for (int i = 0; i < 3; ++i) g_mirror_offset[i] = cp.m[12 + i] - pp.m[12 + i];
    g_mirror_replays = g_mirror_failures = 0;
    g_copy_blocked_motions.store(0);
    g_mirror.store(true);
    char off[96];
    std::snprintf(off, sizeof off, "(%.0f, %.0f, %.0f)", static_cast<double>(g_mirror_offset[0]),
                  static_cast<double>(g_mirror_offset[1]), static_cast<double>(g_mirror_offset[2]));
    log::info(std::string("[probe] F7 : miroir activé, décalage copie-joueur=") + off +
              " ; animations du moteur sur la copie bloquées");
}

// Miroir, chaque frame : la copie prend la pose du joueur (orientation comprise),
// décalée du vecteur mémorisé à l'activation.
void mirror_pose(const game::Entity& player) {
    game::Matrix p;
    if (!game::get_pose(player, p)) return;
    for (int i = 0; i < 3; ++i) p.m[12 + i] += g_mirror_offset[i];
    game::set_pose(g_copy, p);
}

// F4 : même rejeu sur le PNJ le plus proche (≤ 15 m), pour distinguer « la copie est
// inerte » de « le rejeu ne marche pas ».
void replay_on_nearest_npc() {
    game::Entity player;
    game::Matrix pose;
    if (!game::get_player(player) || player.ptr == nullptr || !game::get_pose(player, pose)) {
        log::info("[probe] F4 : pas de joueur");
        return;
    }
    std::vector<game::Entity> npcs;
    if (!game::get_npcs(npcs)) {
        log::info("[probe] F4 : GetNPCs a échoué (SEH)");
        return;
    }
    game::Entity best;
    float best_d2 = kMaxNpcDistance * kMaxNpcDistance;
    for (const auto& npc : npcs) {
        if (npc.ptr == nullptr || npc.ptr == player.ptr || npc.ptr == g_copy.ptr) continue;
        game::Matrix p;
        if (!game::get_pose(npc, p)) continue;
        const float dx = p.m[12] - pose.m[12], dy = p.m[13] - pose.m[13], dz = p.m[14] - pose.m[14];
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < best_d2) {
            best_d2 = d2;
            best = npc;
        }
    }
    if (best.ptr == nullptr) {
        log::info("[probe] F4 : aucun PNJ à moins de 15 m (" + std::to_string(npcs.size()) + " PNJ au total)");
        return;
    }
    char dist[32];
    std::snprintf(dist, sizeof dist, "%.0f cm", static_cast<double>(std::sqrt(best_d2)));
    replay_on(best, "F4", "le PNJ " + hex_ptr(best.ptr) + " (à " + dist + ")");
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
        g_copy_anim_ps.store(nullptr);
        g_mirror.store(false);
        g_last_player = player.ptr;
        g_names.clear();  // ressources peut-être libérées avec le monde
        g_have_last_attack = g_have_last_other = false;
        AcquireSRWLockExclusive(&g_lock);
        g_have_last = false;
        ReleaseSRWLockExclusive(&g_lock);
    }

    if (g_mirror.load() && g_copy.ptr != nullptr && player.ptr != nullptr) mirror_pose(player);
    flush_captures();

    if (game_has_focus()) {
        if (pressed(VK_F6, g_f6_down)) spawn_copy();
        if (pressed(VK_F7, g_f7_down)) toggle_mirror();
        if (pressed(VK_F4, g_f4_down)) replay_on_nearest_npc();
    } else {
        // Hors focus : suivre l'état réel, pour qu'une touche tenue au retour ne déclenche rien.
        pressed(VK_F6, g_f6_down);
        pressed(VK_F7, g_f7_down);
        pressed(VK_F4, g_f4_down);
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

bool on_play_motion(void* animation_ps, int type, void* desc, bool flag) noexcept {
    if (animation_ps == nullptr) return true;
    if (animation_ps == g_copy_anim_ps.load()) {
        if (g_replaying) return true;
        if (g_mirror.load()) {
            g_copy_blocked_motions.fetch_add(1);
            return false;  // miroir : seul le rejeu anime la copie
        }
        g_copy_engine_motions.fetch_add(1);
        return true;
    }
    if (animation_ps != g_player_anim_ps.load()) return true;
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
    return true;
}

}  // namespace mp::sync::probe
