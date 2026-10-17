#include "sync/probe.h"

#include "common/log.h"
#include "core/protocol.h"
#include "game/entity.h"
#include "game/motion_desc.h"
#include "sync/anim_filter.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

namespace mp::sync::probe {

namespace {

constexpr int kMaxAnimLinesPerSecond = 20;
constexpr float kSpawnDistance = 200.0f;  // 2 m, si les unités sont des centimètres
constexpr ULONGLONG kTickPeriodMs = 1000;
constexpr float kMaxNpcDistance = 1500.0f;  // F4 : PNJ à 15 m au plus

using sync::Capture;
namespace proto = mp::protocol;

// L'état partagé avec les hooks PlayMotion et SetRagDollEnabled est dans sync::anim_filter()
// (AD-2) : PS du joueur (capturé), PS de la copie en mode miroir (réplique bloquée), anneau
// des captures.

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
bool g_mirror = false;                 // mode miroir (F7)
float g_mirror_offset[3] = {0, 0, 0};  // position copie - joueur à l'activation du miroir
unsigned g_mirror_replays = 0;         // PlayMotion rejoués par nom sur la copie (par seconde)
unsigned g_mirror_failures = 0;        // rejeux échoués (nom, Motion, GetMotion, SEH)
unsigned g_name_mismatches = 0;        // GetMotion(nom) ≠ ressource capturée (depuis F7)
bool g_hook_thread_logged = false;
bool g_hook_varied_logged = false;
ULONGLONG g_mirror_start = 0;          // base du champ `ms` des Motion du miroir
game::Entity g_copy;          // oubliée dès que le joueur change (chargement, menu)
void* g_copy_anim_ps = nullptr;
void* g_last_player = nullptr;
Capture g_last{};             // dernière capture PlayMotion, quelle qu'elle soit (repli de F4)
int g_last_ragdoll = -1;      // dernier état ragdoll capturé du joueur (-1 : aucun) : dédoublonnage
bool g_copy_dead = false;      // copie mise en ragdoll (mort) : sa pose n'est plus imposée
bool g_copy_settle = false;    // fin de ragdoll du joueur à reproduire : CheckRagdollDeathPose sur la copie
ULONGLONG g_copy_settle_until = 0;
constexpr ULONGLONG kSettleTimeoutMs = 10000;
bool g_have_last = false;
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

bool usable_name(const std::string& name) { return !name.empty() && name[0] != '('; }

// Ressource par nom (eCResource2::GetMotion), mise en cache comme le fera l'avatar. Nul :
// introuvable. À la première résolution d'un nom, on compare avec la ressource capturée.
std::unordered_map<std::string, void*> g_by_name;

bool motion_by_name(const std::string& name, const void* captured, void*& out) {
    auto it = g_by_name.find(name);
    if (it != g_by_name.end()) {
        out = it->second;
        return out != nullptr;
    }
    out = nullptr;
    if (!game::get_motion(name, out)) {
        // Exception (SEH) : pas mise en cache, le rejeu suivant réessaie ; comptée en échec.
        log::warn("[probe] GetMotion(« " + name + " ») a levé une exception (SEH)");
        return false;
    }
    g_by_name.emplace(name, out);
    const bool same = out == captured;
    if (!same) ++g_name_mismatches;
    log::info("[probe] GetMotion(« " + name + " ») = " + hex_ptr(out) +
              (same ? " (identique à la ressource capturée)" : " (DIFFÉRENT de " + hex_ptr(captured) + ")"));
    return out != nullptr;
}

// Capture → Motion (7 scalaires lus par le moteur ; ani_id et net_id inutilisés ici).
proto::Motion to_motion(const Capture& c, std::uint32_t ms) {
    static_assert(sizeof c.desc == game::kPlayDescSize, "Capture::desc = eSMotionPlayBackDesc");
    return game::motion_from_call(c.type, c.flag, c.desc_present ? &c.desc : nullptr, ms);
}

std::string describe(const Capture& c) {
    if (c.kind == sync::CaptureKind::Ragdoll) {
        char buf[160];
        if (c.has_vector) {
            std::snprintf(buf, sizeof buf, "[probe] SetRagDollEnabled actif=%d b=%d vecteur=(%g, %g, %g)",
                          c.ragdoll_on ? 1 : 0, c.ragdoll_flag ? 1 : 0, static_cast<double>(c.vector[0]),
                          static_cast<double>(c.vector[1]), static_cast<double>(c.vector[2]));
        } else {
            std::snprintf(buf, sizeof buf, "[probe] SetRagDollEnabled actif=%d b=%d vecteur=nul", c.ragdoll_on ? 1 : 0,
                          c.ragdoll_flag ? 1 : 0);
        }
        return buf;
    }
    std::string line = "[probe] PlayMotion type=" + std::to_string(c.type) + " nom=« " + motion_name(c.resource) +
                       " » flag=" + (c.flag ? "1" : "0");
    if (!c.desc_present) return line + " (descripteur nul ou illisible)";
    proto::Motion m = to_motion(c, 0);
    char buf[160];
    std::snprintf(buf, sizeof buf, " d00=%g d04=%g d08=%u d0C=%g d10=%u d14=%g d18=%u", static_cast<double>(m.d00),
                  static_cast<double>(m.d04), m.d08, static_cast<double>(m.d0c), m.d10, static_cast<double>(m.d14),
                  m.d18);
    return line + buf;
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

bool replay_by_name(void* anim_ps, const Capture& c, bool& set_ok);

// Ragdoll capturé → même appel sur la copie, sous le garde de rejeu (sinon bloqué).
// Fin de ragdoll (actif=0) : chez le joueur, c'est CheckRagdollDeathPose qui fige d'abord la
// pose du squelette puis coupe le ragdoll. Rejouer le seul SetRagDollEnabled(0) laisse la copie
// en T-pose (test en jeu 2.1b) : on demande donc à la copie sa propre CheckRagdollDeathPose,
// réessayée à chaque frame tant que son corps bouge encore (settle_copy).
bool replay_ragdoll(void* anim_ps, const Capture& c) {
    if (!c.ragdoll_on) {
        g_copy_settle = true;
        g_copy_settle_until = GetTickCount64() + kSettleTimeoutMs;
        log::info("[probe] SetRagDollEnabled actif=0 du joueur : pose de mort à figer sur la copie");
        return true;
    }
    g_copy_dead = true;
    sync::AnimFilter::ReplayGuard guard;
    const bool ok = game::set_ragdoll(anim_ps, c.ragdoll_on, c.ragdoll_flag, c.has_vector ? c.vector : nullptr);
    log::info(std::string("[probe] SetRagDollEnabled actif=") + (c.ragdoll_on ? "1" : "0") + " rejoué sur la copie : " +
              (ok ? "OK" : "SEH"));
    return ok;
}

void settle_copy() {
    if (!g_copy_settle || g_copy_anim_ps == nullptr) return;
    bool ragdoll = false;
    if (game::get_ragdoll(g_copy_anim_ps, ragdoll) && !ragdoll) {
        g_copy_settle = false;
        log::info("[probe] copie : plus de ragdoll (le moteur l'a déjà figée), rien à faire");
        return;
    }
    bool done = false;
    {
        sync::AnimFilter::ReplayGuard guard;  // laisse passer son SetRagDollEnabled(0) interne
        game::check_ragdoll_death_pose(g_copy_anim_ps, done);
    }
    if (done) {
        g_copy_settle = false;
        log::info("[probe] copie : CheckRagdollDeathPose OK (pose de mort figée)");
    } else if (GetTickCount64() > g_copy_settle_until) {
        g_copy_settle = false;
        log::warn("[probe] copie : corps toujours en mouvement après 10 s, laissée en ragdoll");
    }
}

void flush_captures() {
    static Capture batch[sync::kCaptureRing];
    unsigned dropped = 0;
    const std::size_t n = sync::anim_filter().drain(batch, sync::kCaptureRing, dropped);

    g_ring_overflow += dropped;
    for (std::size_t i = n; i-- > 0;) {
        if (batch[i].kind == sync::CaptureKind::Motion) {
            g_last = batch[i];
            g_have_last = true;
            break;
        }
    }
    // Ragdoll : seuls les changements d'état comptent (le moteur sort aussitôt si l'état ne
    // change pas) ; les appels répétés ne sont ni rejoués ni journalisés.
    bool repeat[sync::kCaptureRing] = {};
    for (std::size_t i = 0; i < n; ++i) {
        if (batch[i].kind != sync::CaptureKind::Ragdoll) continue;
        const int state = batch[i].ragdoll_on ? 1 : 0;
        repeat[i] = state == g_last_ragdoll;
        g_last_ragdoll = state;
    }
    void* copy_ps = g_mirror ? g_copy_anim_ps : nullptr;
    for (std::size_t i = 0; i < n && copy_ps != nullptr; ++i) {
        if (repeat[i]) continue;
        bool set_ok = false;
        const bool ok = batch[i].kind == sync::CaptureKind::Ragdoll ? replay_ragdoll(copy_ps, batch[i])
                                                                     : replay_by_name(copy_ps, batch[i], set_ok);
        if (ok) {
            ++g_mirror_replays;
        } else {
            ++g_mirror_failures;
        }
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (batch[i].kind != sync::CaptureKind::Motion) continue;
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
        if (batch[i].kind == sync::CaptureKind::Ragdoll) {
            if (!repeat[i]) log::info(describe(batch[i]));  // changement d'état, rare : jamais limité
            continue;
        }
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
        void* anim_ps = nullptr;
        game::get_animation_ps(player, anim_ps);
        line += " anim_ps=" + hex_ptr(anim_ps);
    }
    log::info(line);
    if (g_copy.ptr != nullptr) {
        // Diagnostic « copie figée » : le moteur la met-il à jour et l'anime-t-il ?
        bool in_range = false;
        const bool range_ok = game::is_processing_range_entered(g_copy, in_range);
        void* slot0 = nullptr;
        game::read_motion_resource(g_copy_anim_ps, 0, slot0);
        game::Matrix copy_pose;
        const bool pose_ok = game::get_pose(g_copy, copy_pose);
        bool ragdoll = false;
        const bool ragdoll_ok = game::get_ragdoll(g_copy_anim_ps, ragdoll);
        log::info("[probe] copie " + hex_ptr(g_copy.ptr) + " : zone de traitement=" +
                  (range_ok ? (in_range ? "oui" : "NON") : "illisible") +
                  ", ragdoll=" + (ragdoll_ok ? (ragdoll ? "oui" : "non") : "illisible") +
                  (g_mirror ? ", miroir par nom : " + std::to_string(g_mirror_replays) + " rejoués, " +
                                  std::to_string(g_mirror_failures) + " échecs, " +
                                  std::to_string(sync::anim_filter().take_blocked()) + " appels moteur bloqués/s, " +
                                  std::to_string(g_name_mismatches) + " écarts de pointeur depuis F7"
                            : std::string()) +
                  ", emplacement 0 « " + motion_name(slot0) +
                  " », pos=" + (pose_ok ? format_position(copy_pose) : std::string("illisible")));
    }
    if (g_anim_skipped != 0) {
        log::info("[probe] " + std::to_string(g_anim_skipped) + " PlayMotion non journalisés (limite de débit)");
    }
    if (g_ring_overflow != 0) {
        log::info("[probe] " + std::to_string(g_ring_overflow) + " PlayMotion perdus (anneau de " +
                  std::to_string(sync::kCaptureRing) + " plein en une frame)");
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
    game::get_animation_ps(copy, g_copy_anim_ps);

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

// Rejoue `c` sur `anim_ps` par le chemin exact de l'avatar (AD-8), sans le pointeur de
// ressource ni les octets bruts du descripteur : Motion encodé puis décodé (core/), nom →
// eCResource2::GetMotion → SetMotion → PlayMotion avec le descripteur reconstruit. Le garde
// de rejeu laisse passer cet appel dans le hook si `anim_ps` est une réplique.
bool replay_by_name(void* anim_ps, const Capture& c, bool& set_ok) {
    set_ok = false;
    const std::string& name = motion_name(c.resource);
    if (!usable_name(name)) return false;

    std::vector<std::uint8_t> wire;
    const auto ms = static_cast<std::uint32_t>(GetTickCount64() - g_mirror_start);
    if (!proto::encode_reliable(to_motion(c, ms), wire)) return false;
    const auto decoded = proto::decode_reliable(wire.data(), wire.size());
    const proto::Motion* m = decoded.ok() ? std::get_if<proto::Motion>(&decoded.message) : nullptr;
    if (m == nullptr) return false;

    void* resource = nullptr;
    if (!motion_by_name(name, c.resource, resource)) return false;
    static std::uint8_t desc[game::kPlayDescSize];
    void* desc_arg = const_cast<void*>(game::replay_desc(*m, desc));
    set_ok = game::set_motion(anim_ps, m->type, resource);
    if (!set_ok) return false;
    sync::AnimFilter::ReplayGuard guard;
    return game::play_motion(anim_ps, m->type, desc_arg, game::replay_flag(*m));
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
    } else if (g_have_last) {
        last = g_last;
        kind = "dernière animation";
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
    const bool ok = replay_by_name(anim_ps, last, set_ok);
    void* after = nullptr;
    game::read_motion_resource(anim_ps, last.type, after);
    log::info(std::string("[probe] ") + key + " : " + kind + " « " + motion_name(last.resource) +
              " » (type=" + std::to_string(last.type) + ") rejouée sur " + label + " : SetMotion " +
              (set_ok ? "OK" : "SEH") + ", PlayMotion " + (ok ? "appelé" : (set_ok ? "SEH" : "non appelé")) +
              ", emplacement " + hex_ptr(before) + " « " + motion_name(before) + " » -> " + hex_ptr(after) +
              (after == last.resource ? " (même ressource que le joueur)" : " (INATTENDU)"));
}

void toggle_mirror() {
    if (g_copy.ptr == nullptr) {
        log::info("[probe] F7 : pas de copie (F6 d'abord)");
        return;
    }
    if (g_mirror) {
        sync::anim_filter().remove_replica(g_copy_anim_ps);
        g_mirror = false;
        log::info("[probe] F7 : miroir désactivé (la copie reprend ses propres animations)");
        return;
    }
    if (g_copy_anim_ps == nullptr) {
        log::info("[probe] F7 : pas d'eCAnimation_PS sur la copie");
        return;
    }
    bool copy_ragdoll = false;
    if (game::get_ragdoll(g_copy_anim_ps, copy_ragdoll) && copy_ragdoll) {
        log::info("[probe] F7 : la copie est restée en ragdoll (morte) ; recharger puis F6 pour une nouvelle copie");
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
    g_mirror_replays = g_mirror_failures = g_name_mismatches = 0;
    g_by_name.clear();  // chaque nom est revérifié contre sa ressource capturée
    g_mirror_start = GetTickCount64();
    sync::anim_filter().take_blocked();
    sync::anim_filter().add_replica(g_copy_anim_ps);
    g_mirror = true;
    char off[96];
    std::snprintf(off, sizeof off, "(%.0f, %.0f, %.0f)", static_cast<double>(g_mirror_offset[0]),
                  static_cast<double>(g_mirror_offset[1]), static_cast<double>(g_mirror_offset[2]));
    log::info(std::string("[probe] F7 : miroir par nom activé, décalage copie-joueur=") + off +
              " ; animations du moteur sur la copie bloquées");
}

// Miroir, chaque frame : la copie prend la pose du joueur (orientation comprise),
// décalée du vecteur mémorisé à l'activation. En ragdoll (mort), la physique pilote la
// copie : on ne lui impose plus rien.
void mirror_pose(const game::Entity& player) {
    if (g_copy_dead) return;  // morte : elle reste où le ragdoll l'a laissée
    bool ragdoll = false;
    if (game::get_ragdoll(g_copy_anim_ps, ragdoll) && ragdoll) return;  // lecture impossible : pose imposée
    game::Matrix p;
    if (!game::get_pose(player, p)) return;
    for (int i = 0; i < 3; ++i) p.m[12 + i] += g_mirror_offset[i];
    game::move_to(g_copy, p);  // v4 : SetPose laissait contrôleur et équipement au sol
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

    // Mise à jour du filtre des hooks.
    sync::AnimFilter& filter = sync::anim_filter();
    game::Entity player;
    void* anim_ps = nullptr;
    if (game::get_player(player) && player.ptr != nullptr) game::get_animation_ps(player, anim_ps);
    filter.set_local(anim_ps);
    if (player.ptr != g_last_player) {
        // Chargement, retour au menu ou prise de contrôle : la copie temporaire a pu
        // être détruite avec le monde. Ne jamais réutiliser un pointeur peut-être libéré.
        // Elle sort du filtre avant d'être oubliée (AD-2).
        filter.remove_replica(g_copy_anim_ps);
        if (g_copy.ptr != nullptr) log::info("[probe] joueur changé : copie " + hex_ptr(g_copy.ptr) + " oubliée");
        g_copy = game::Entity{};
        g_copy_anim_ps = nullptr;
        g_mirror = false;
        g_last_player = player.ptr;
        g_names.clear();  // ressources peut-être libérées avec le monde
        g_by_name.clear();
        g_have_last_attack = g_have_last_other = g_have_last = false;
        g_last_ragdoll = -1;
        g_copy_dead = g_copy_settle = false;
        // Captures de l'ancien monde : leurs ressources sont peut-être libérées, on les jette.
        static Capture stale[sync::kCaptureRing];
        unsigned dropped = 0;
        filter.drain(stale, sync::kCaptureRing, dropped);
    }

    // AD-2 : les hooks (PlayMotion, SetRagDollEnabled) tournent-ils sur le thread d'OnProcess ?
    const std::uint32_t hook_tid = filter.hook_thread();
    if (!g_hook_thread_logged && hook_tid != 0) {
        g_hook_thread_logged = true;
        log::info("[probe] hooks PlayMotion/SetRagDollEnabled : thread=" + std::to_string(hook_tid) +
                  (hook_tid == g_first_thread ? " (celui d'OnProcess)" : " (DIFFÉRENT du thread d'OnProcess " +
                                                                              std::to_string(g_first_thread) + ")"));
    }
    if (!g_hook_varied_logged && filter.hook_thread_varied()) {
        g_hook_varied_logged = true;
        log::warn("[probe] hooks PlayMotion/SetRagDollEnabled : appelés depuis plusieurs threads");
    }

    if (g_mirror && g_copy.ptr != nullptr && player.ptr != nullptr) mirror_pose(player);
    if (g_mirror && g_copy.ptr != nullptr) settle_copy();
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
    sync::AnimFilter& filter = sync::anim_filter();
    switch (filter.classify(animation_ps, GetCurrentThreadId())) {
        case sync::AnimFilter::Verdict::Pass: return true;
        case sync::AnimFilter::Verdict::Block: return false;  // miroir : seul le rejeu anime la copie
        case sync::AnimFilter::Verdict::Capture: break;
    }
    Capture c;
    c.type = type;
    c.flag = flag;
    c.desc_present = desc != nullptr && game::copy_bytes(desc, c.desc, sizeof c.desc);
    game::read_motion_resource(animation_ps, type, c.resource);
    filter.push(c);
    return true;
}

bool on_set_ragdoll(void* animation_ps, bool on, bool flag, const float* vector) noexcept {
    sync::AnimFilter& filter = sync::anim_filter();
    switch (filter.classify(animation_ps, GetCurrentThreadId())) {
        case sync::AnimFilter::Verdict::Pass: return true;
        // Miroir : seul le rejeu met la copie en ragdoll. Une désactivation (nettoyage par le
        // moteur) passe toujours : bloquée, elle laisserait un ragdoll orphelin (décision du
        // 2026-10-17). Elle reste comptée dans les appels bloqués.
        case sync::AnimFilter::Verdict::Block: return !on;
        case sync::AnimFilter::Verdict::Capture: break;
    }
    Capture c;
    c.kind = sync::CaptureKind::Ragdoll;
    c.ragdoll_on = on;
    c.ragdoll_flag = flag;
    c.has_vector = vector != nullptr && game::copy_bytes(vector, c.vector, sizeof c.vector);
    filter.push(c);
    return true;
}

}  // namespace mp::sync::probe
