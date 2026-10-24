#include "sync/probe_j3.h"

#include "common/log.h"
#include "game/combat.h"
#include "game/dialogue.h"
#include "sync/probe_j2.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace mp::sync::probe_j3 {

namespace {

constexpr float kMaxNpcDistance = 1500.0f;     // Num7 : 15 m, comme Num3 du spike J2
constexpr std::uint64_t kSecondPressMs = 2000; // 2ᵉ appui de Num7, Num8 ou Num9
constexpr std::uint64_t kWatchMaxMs = 30000;   // suivi de sIsOutputPlaying : 30 s au plus
constexpr std::size_t kRing = 32;              // StartOutput en attente de la frame
constexpr std::size_t kSampleCap = 200;

// --- Partagé avec le détour ----------------------------------------------------------------
struct Captured {
    void* spu = nullptr;
    void* self = nullptr;
    bool self_ok = false;
    char sample[kSampleCap] = {};
    std::size_t sample_len = 0;
    bool sample_ok = false;
    int type = 0;
    bool result = false;
    bool replay = false;  // lancé par la sonde elle-même (Num7, Num8)
    std::uint32_t tid = 0;
};

std::mutex g_mutex;
Captured g_ring[kRing];   // sous g_mutex
std::size_t g_pending = 0;
unsigned g_dropped = 0;
std::atomic<bool> g_replaying{false};  // la frame appelle sStartOutput ou StartSay

// --- Thread principal uniquement -----------------------------------------------------------
bool g_active = false;
bool g_down[3] = {};      // Num7, Num8, Num9
bool g_hello = false;
bool g_inactive_logged = false;
bool g_thread_logged = false;
bool g_info_failed_logged = false;
int g_last_key = 0;
std::uint64_t g_last_press = 0;
bool g_talk_open = false; // GUI2::StartTalk ouvert par Num9

// Dernière réplique capturée (toutes) et dernière réplique du héros.
struct Line {
    bool valid = false;
    game::Entity speaker;
    std::string speaker_name;
    std::string sample;
    int type = 0;
    std::string text_id;  // commande Say d'InfoSystem au moment de la réplique (vide : inconnue)
};
Line g_last;
Line g_last_hero;

// État d'InfoSystem, journalisé une fois par changement.
struct InfoState {
    bool running = false;
    void* partner = nullptr;
    int command = game::kInfoCommandNone;
    std::string text_id;
    void* say_speaker = nullptr;
    bool operator==(const InfoState& o) const {
        return running == o.running && partner == o.partner && command == o.command && text_id == o.text_id &&
               say_speaker == o.say_speaker;
    }
};
InfoState g_info;
bool g_info_logged = false;

// Suivi de sIsOutputPlaying : fin de la réplique (passif) ou du rejeu (Num7, Num8).
struct Watch {
    bool active = false;
    game::Entity entity;
    std::string label;
    std::uint64_t start = 0;
    bool first_done = false;
    bool was_playing = false;
};
Watch g_watch_line;
Watch g_watch_replay;

std::string hex_ptr(const void* p) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(p)));
    return buf;
}

std::string hex_int(int v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%x", static_cast<unsigned>(v));
    return buf;
}

std::string call(bool ok) { return ok ? "OK" : "SEH"; }
std::string yes_no(bool v) { return v ? "oui" : "non"; }

std::string ascii(const std::string& s) {
    std::string out;
    for (unsigned char c : s) out += (c < 0x80) ? static_cast<char>(c) : '?';
    return out;
}

std::string utf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string("(conversion impossible)");
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring widen(const std::string& s) {
    std::wstring out;
    for (unsigned char c : s) out += static_cast<wchar_t>(c);  // échantillon : ASCII attendu
    return out;
}

std::string entity_name(const game::Entity& e) {
    if (e.ptr == nullptr) return "(aucune)";
    std::string name;
    if (!game::get_entity_name(e, name)) return "(nom illisible)";
    return name.empty() ? std::string("(nom vide)") : name;
}

// gEOutputType : valeurs vues au désassemblage de StartSayEx (Script.dll).
const char* type_hint(int type) {
    switch (type) {
        case 0: return "0 (défaut)";
        case 1: return "1 (auditeur = joueur)";
        case 2: return "2 (locuteur = joueur)";
        case 3: return "3 (2e bool de StartSay)";
        default: return nullptr;
    }
}

std::string type_text(int type) {
    const char* hint = type_hint(type);
    return hint != nullptr ? std::string(hint) : std::to_string(type);
}

std::string command_text(int command) {
    if (command == game::kInfoCommandNone) return "aucune (0x3d)";
    if (command == game::kInfoCommandSay) return "Say (0x1b)";
    return hex_int(command);
}

void log_line(const std::string& text) { log::info("[probe] J3 " + text); }

// Journal à l'écran du jeu (ASCII, via la sonde J2 : GUI2::PrintGameLogF). Copié dans le log.
void hud(const std::string& text) {
    log::info("[probe] J3 écran : " + text);
    game::hud_print(("[MP] " + text).c_str());
}

bool pressed(int vk, bool& was_down, bool focus) {
    const bool down = focus && (GetAsyncKeyState(vk) & 0x8000) != 0;
    const bool edge = down && !was_down;
    was_down = down;
    return edge;
}

// --- H1 : InfoSystem ------------------------------------------------------------------------

void poll_info() {
    InfoState s;
    const bool a = game::info_is_running(s.running);
    game::Entity partner;
    const bool b = game::info_dialog_npc(partner);
    s.partner = partner.ptr;
    const bool c = game::info_command_type(s.command);
    if (!(a && b && c)) {
        if (!g_info_failed_logged) {
            g_info_failed_logged = true;
            log_line("InfoSystem : lecture en échec (SEH) : IsRunning " + call(a) + ", GetNPC " + call(b) +
                     ", GetCurrentCommandType " + call(c));
        }
        return;
    }
    if (s.command == game::kInfoCommandSay) {
        game::info_say_text_id(s.text_id);
        game::Entity speaker;
        game::info_say_speaker(speaker);
        s.say_speaker = speaker.ptr;
    }
    if (g_info_logged && s == g_info) return;
    const bool started = s.running && (!g_info_logged || !g_info.running);
    const bool ended = !s.running && g_info_logged && g_info.running;
    g_info = s;
    g_info_logged = true;
    std::string line = std::string("dialogue : en cours=") + yes_no(s.running) + " partenaire PNJ=" + hex_ptr(s.partner);
    if (s.partner != nullptr) line += " « " + entity_name(game::Entity{s.partner}) + " »";
    line += ", commande=" + command_text(s.command);
    if (s.command == game::kInfoCommandSay) {
        line += ", texte=« " + s.text_id + " », locuteur de la commande=" + hex_ptr(s.say_speaker);
        if (s.say_speaker != nullptr) line += " « " + entity_name(game::Entity{s.say_speaker}) + " »";
    }
    if (started) line += " — DÉBUT";
    if (ended) line += " — FIN";
    log_line(line);
}

// --- H1 : répliques capturées par le détour ------------------------------------------------

void start_watch(Watch& w, const game::Entity& e, const std::string& label, std::uint64_t now) {
    w = Watch{};
    w.active = e.ptr != nullptr;
    w.entity = e;
    w.label = label;
    w.start = now;
}

void poll_watch(Watch& w, std::uint64_t now) {
    if (!w.active) return;
    bool playing = false;
    const bool ok = game::output_playing(w.entity, playing);
    const std::uint64_t dt = now - w.start;
    if (!w.first_done) {
        w.first_done = true;
        w.was_playing = ok && playing;
        log_line(w.label + " : sIsOutputPlaying à la 1re frame=" + (ok ? yes_no(playing) : std::string("SEH")));
        if (!ok || !playing) w.active = false;
        return;
    }
    if (!ok) {
        log_line(w.label + " : sIsOutputPlaying en échec (SEH) après " + std::to_string(dt) + " ms, suivi arrêté");
        w.active = false;
        return;
    }
    if (!playing) {
        log_line(w.label + " : fin de la sortie (sIsOutputPlaying=non) après " + std::to_string(dt) + " ms");
        w.active = false;
        return;
    }
    if (dt >= kWatchMaxMs) {
        log_line(w.label + " : sortie toujours en attente après " + std::to_string(dt) +
                 " ms (IA figée ? jamais traitée), suivi arrêté");
        w.active = false;
    }
}

void drain(const game::Entity& player, const game::Entity& copy, std::uint64_t now) {
    Captured batch[kRing];
    std::size_t n = 0;
    unsigned dropped = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        n = g_pending;
        for (std::size_t i = 0; i < n; ++i) batch[i] = g_ring[i];
        g_pending = 0;
        dropped = g_dropped;
        g_dropped = 0;
    }
    if (dropped != 0) log_line("StartOutput : " + std::to_string(dropped) + " appels perdus (plus de 32 par frame)");
    for (std::size_t i = 0; i < n; ++i) {
        const Captured& c = batch[i];
        if (!g_thread_logged) {
            g_thread_logged = true;
            log_line("StartOutput : thread du détour=" + std::to_string(c.tid) +
                     (c.tid == GetCurrentThreadId() ? " (celui d'OnProcess)" : " (DIFFÉRENT d'OnProcess)"));
        }
        const game::Entity speaker{c.self};
        const bool hero = c.self != nullptr && c.self == player.ptr;
        const bool is_copy = c.self != nullptr && c.self == copy.ptr;
        const std::string sample = c.sample_ok ? std::string(c.sample, c.sample_len) : std::string();
        // Identifiant de texte : commande Say d'InfoSystem lue à cette frame (poll_info avant drain).
        const std::string text_id =
            (g_info.running && g_info.command == game::kInfoCommandSay) ? g_info.text_id : std::string();
        const std::string name = entity_name(speaker);
        std::string line = std::string(c.replay ? "StartOutput (rejeu de la sonde)" : "StartOutput") +
                           " : locuteur=" + (c.self_ok ? hex_ptr(c.self) : std::string("SEH")) + " « " + name + " » (" +
                           (hero ? "HÉROS" : (is_copy ? "copie F6" : "autre")) + "), échantillon=« " +
                           (c.sample_ok ? sample : std::string("illisible")) + " », type=" + type_text(c.type) +
                           ", retour=" + (c.result ? "true" : "false") + ", dialogue en cours=" + yes_no(g_info.running) +
                           ", texte Say=« " + text_id + " », spu=" + hex_ptr(c.spu);
        log_line(line);
        if (c.replay || !c.sample_ok || sample.empty()) continue;
        Line l;
        l.valid = true;
        l.speaker = speaker;
        l.speaker_name = name;
        l.sample = sample;
        l.type = c.type;
        l.text_id = text_id;
        g_last = l;
        if (hero) g_last_hero = l;
        start_watch(g_watch_line, speaker, "réplique de « " + name + " »", now);
    }
}

// --- H2, H3, H4 : rejeu (Num7, Num8) -------------------------------------------------------

// PNJ suivi par Num3 (spike J2) s'il est à moins de 15 m, sinon le plus proche ; jamais le
// partenaire du dialogue en cours, le joueur ni la copie.
bool choose_npc(const game::Entity& player, const game::Entity& copy, game::Entity& out, float& distance,
                bool& tracked) {
    out = game::Entity{};
    distance = 0.0f;
    tracked = false;
    game::Matrix pose;
    if (player.ptr == nullptr || !game::get_pose(player, pose)) return false;
    std::vector<game::Entity> npcs;
    if (!game::get_npcs(npcs)) return false;
    const game::Entity followed = probe_j2::tracked_npc();
    const float max2 = kMaxNpcDistance * kMaxNpcDistance;
    float best_d2 = max2;
    for (const auto& npc : npcs) {
        if (npc.ptr == nullptr || npc.ptr == player.ptr || npc.ptr == copy.ptr || npc.ptr == g_info.partner) continue;
        game::Matrix p;
        if (!game::get_pose(npc, p)) continue;
        const float dx = p.m[12] - pose.m[12], dy = p.m[13] - pose.m[13], dz = p.m[14] - pose.m[14];
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 >= max2) continue;
        if (npc.ptr == followed.ptr) {
            out = npc;
            best_d2 = d2;
            tracked = true;
            break;
        }
        if (d2 < best_d2) {
            best_d2 = d2;
            out = npc;
        }
    }
    if (out.ptr != nullptr) distance = std::sqrt(best_d2);
    return true;
}

std::string routine_ps(const game::Entity& e) {
    void* ps = nullptr;
    const bool ok = game::get_property_set(e, game::kRoutinePropertySet, ps);
    return std::string("PS 0x24=") + (ok ? (ps != nullptr ? "oui" : "NON (sStartOutput rendra false)") : "SEH");
}

void replay(const char* key, const Line& line, const game::Entity& target, const game::Entity& player, bool second,
            std::uint64_t now) {
    const std::string target_name = entity_name(target);
    if (!second) {
        bool result = false;
        g_replaying.store(true);
        const bool ok = game::start_output(target, line.sample.c_str(), line.type, result);
        g_replaying.store(false);
        log_line(std::string(key) + " : sStartOutput(cible, « " + line.sample + " », type " + type_text(line.type) +
                 ") " + call(ok) + " -> " + (ok ? (result ? "true" : "false") : "?"));
        const bool face = game::start_facial_ani(target, line.sample.c_str());
        log_line(std::string(key) + " : StartFacialAni(« " + line.sample + " ») " + call(face));
        start_watch(g_watch_replay, target, std::string(key) + " sur « " + target_name + " »", now);
        hud(std::string(key) + " : replique rejouee sur " + ascii(target_name) + " (voix+levres). " + key +
            " encore (<2 s) = StartSay");
        return;
    }
    if (line.text_id.empty()) {
        log_line(std::string(key) + " bis : identifiant de texte inconnu (réplique capturée hors commande Say "
                 "d'InfoSystem) : StartSay sauté");
        hud(std::string(key) + " bis : identifiant de texte inconnu, StartSay saute");
        return;
    }
    g_replaying.store(true);
    const bool ok = game::start_say(target, player, line.text_id.c_str(), /*show_text=*/true, /*flag=*/false);
    g_replaying.store(false);
    log_line(std::string(key) + " bis : StartSay(cible « " + target_name + " », auditeur = héros, texte « " +
             line.text_id + " », afficher le texte=true, 2e bool=false) " + call(ok) +
             " (échantillon calculé par le jeu : voir la ligne StartOutput (rejeu) ci-dessous)");
    start_watch(g_watch_replay, target, std::string(key) + " bis sur « " + target_name + " »", now);
    hud(std::string(key) + " bis : StartSay sur " + ascii(target_name) + " " + call(ok));
}

bool second_press(int key, std::uint64_t now) {
    const bool second = g_last_key == key && now - g_last_press < kSecondPressMs;
    g_last_key = second ? 0 : key;  // 3ᵉ appui : de nouveau un 1ᵉʳ appui
    g_last_press = now;
    return second;
}

void num7(const game::Entity& player, const game::Entity& copy, std::uint64_t now) {
    const bool second = second_press(7, now);
    if (!g_last.valid) {
        log_line("Num7 : aucune réplique capturée (parler d'abord à un PNJ)");
        hud("Num7 : aucune replique capturee, parle d'abord a un PNJ");
        return;
    }
    game::Entity target;
    float distance = 0.0f;
    bool tracked = false;
    if (!choose_npc(player, copy, target, distance, tracked)) {
        log_line("Num7 : GetNPCs ou pose du joueur en échec (SEH)");
        return;
    }
    if (target.ptr == nullptr) {
        log_line("Num7 : aucun PNJ à moins de 15 m hors partenaire de dialogue (" + hex_ptr(g_info.partner) + ")");
        hud("Num7 : aucun PNJ a moins de 15 m (hors partenaire de dialogue)");
        return;
    }
    char dist[32];
    std::snprintf(dist, sizeof dist, "%.0f cm", static_cast<double>(distance));
    const bool frozen = tracked && probe_j2::tracked_npc_frozen();
    log_line(std::string("Num7") + (second ? " bis" : "") + " : réplique « " + g_last.sample + " » de « " +
             g_last.speaker_name + " » -> PNJ " + hex_ptr(target.ptr) + " « " + entity_name(target) + " » à " + dist +
             (tracked ? " (PNJ suivi par Num3)" : " (le plus proche)") + ", IA figée (Num4)=" + yes_no(frozen) + ", " +
             routine_ps(target) + ", partenaire de dialogue exclu=" + hex_ptr(g_info.partner) +
             ", dialogue en cours=" + yes_no(g_info.running));
    replay("Num7", g_last, target, player, second, now);
}

void num8(const game::Entity& player, const game::Entity& copy, std::uint64_t now) {
    const bool second = second_press(8, now);
    if (!g_last_hero.valid) {
        log_line("Num8 : aucune réplique du héros capturée (choisir une réplique du héros dans un dialogue)");
        hud("Num8 : aucune replique du heros capturee");
        return;
    }
    if (copy.ptr == nullptr) {
        log_line("Num8 : pas de copie (F6 d'abord)");
        hud("Num8 : pas de copie, appuie d'abord sur F6");
        return;
    }
    log_line(std::string("Num8") + (second ? " bis" : "") + " : réplique du héros « " + g_last_hero.sample +
             " » -> copie " + hex_ptr(copy.ptr) + ", " + routine_ps(copy));
    replay("Num8", g_last_hero, copy, player, second, now);
}

// --- H5 : sous-titre (Num9) ----------------------------------------------------------------

void num9(const game::Entity& player, std::uint64_t now) {
    if (g_talk_open) {
        const bool ok = game::stop_talk();
        g_talk_open = false;
        g_last_key = 0;
        log_line("Num9 : GUI2::StopTalk " + call(ok));
        hud("Num9 : page de dialogue fermee (StopTalk)");
        return;
    }
    const bool second = second_press(9, now);
    if (!g_last.valid) {
        log_line("Num9 : aucune réplique capturée (parler d'abord à un PNJ)");
        hud("Num9 : aucune replique capturee");
        return;
    }
    if (!second) {
        std::wstring text;
        std::string source = "échantillon brut (identifiant de texte inconnu)";
        if (!g_last.text_id.empty()) {
            const bool ok = game::localized_string(g_last.text_id.c_str(), text);
            log_line("Num9 : eCLocAdmin2::GetString(« " + g_last.text_id + " ») " + call(ok) + " -> « " + utf8(text) +
                     " » (" + std::to_string(text.size()) + " car.)");
            if (!text.empty()) source = "texte localisé";
        }
        if (text.empty()) text = widen(g_last.sample);
        void* system = nullptr;
        const bool ok = game::show_subtitle(text.c_str(), system);
        log_line("Num9 : gCHudSystem2::ShowSubtitle(" + source + " « " + utf8(text) + " ») " + call(ok) +
                 ", gCHudSystem2=" + hex_ptr(system) + (system == nullptr ? " (nul : rien affiché)" : "") +
                 " ; GUI2::ShowSubtitle (Script.dll) n'est qu'un ret, non appelé");
        hud("Num9 : sous-titre demande (" + ascii(source) + "). Num9 encore (<2 s) = StartTalk");
        return;
    }
    if (g_last.text_id.empty()) {
        log_line("Num9 bis : identifiant de texte inconnu : GUI2::StartTalk sauté");
        hud("Num9 bis : identifiant de texte inconnu, StartTalk saute");
        return;
    }
    // Locuteur de la réplique s'il est encore là (sinon le héros) : StartTalk affiche son nom.
    game::Entity speaker = g_last.speaker.ptr != nullptr ? g_last.speaker : player;
    bool result = false;
    const bool ok = game::start_talk(speaker, g_last.text_id.c_str(), result);
    if (ok && result) g_talk_open = true;
    log_line("Num9 bis : GUI2::StartTalk(« " + entity_name(speaker) + " », « " + g_last.text_id + " ») " + call(ok) +
             " -> " + (ok ? (result ? "true" : "false") : "?") + (g_talk_open ? " ; Num9 pour la fermer" : ""));
    hud(g_talk_open ? "Num9 bis : page de dialogue ouverte (StartTalk), Num9 pour la fermer"
                    : "Num9 bis : StartTalk a echoue (voir le log)");
}

}  // namespace

void set_status(bool start_output_hook) { g_active = start_output_hook; }

const char* help_line() {
    return g_active ? "Num7 replique->PNJ (x2: StartSay) | Num8 replique heros->copie | Num9 sous-titre (x2: StartTalk)"
                    : "Num7 a Num9 (dialogues J3) inactifs : ABI introuvable";
}

void reset() {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_pending = 0;
        g_dropped = 0;
    }
    g_last = Line{};
    g_last_hero = Line{};
    g_watch_line = Watch{};
    g_watch_replay = Watch{};
    g_info = InfoState{};
    g_info_logged = false;
    g_talk_open = false;  // la page a été fermée avec le monde
    g_last_key = 0;
    g_hello = false;
}

void frame(const game::Entity& player, const game::Entity& copy, bool focus, std::uint64_t now_ms) {
    if (!g_active) {
        // ABI des dialogues introuvable : seules les touches sont suivies (aucun appel au jeu).
        bool any = false;
        for (int i = 0; i < 3; ++i) any = pressed(VK_NUMPAD7 + i, g_down[i], focus) || any;
        if (any && !g_inactive_logged) {
            g_inactive_logged = true;
            log_line("Num7 à Num9 : expériences J3 inactives (voir la ligne [hooks] J3)");
        }
        return;
    }
    if (player.ptr == nullptr) {
        for (int i = 0; i < 3; ++i) pressed(VK_NUMPAD7 + i, g_down[i], false);
        drain(player, copy, now_ms);  // menu : vider sans retenir de réplique du héros
        return;
    }
    if (!g_hello) {
        g_hello = true;
        hud("Sonde J3 active : parle a un PNJ, puis Num7/Num8/Num9 (Num0 = aide)");
    }
    poll_info();
    drain(player, copy, now_ms);
    poll_watch(g_watch_line, now_ms);
    poll_watch(g_watch_replay, now_ms);
    if (pressed(VK_NUMPAD7, g_down[0], focus)) num7(player, copy, now_ms);
    if (pressed(VK_NUMPAD8, g_down[1], focus)) num8(player, copy, now_ms);
    if (pressed(VK_NUMPAD9, g_down[2], focus)) num9(player, now_ms);
}

void on_start_output(void* spu, const void* sample, int type, bool result) noexcept {
    Captured c;
    c.spu = spu;
    c.type = type;
    c.result = result;
    c.replay = g_replaying.load();
    c.tid = GetCurrentThreadId();
    c.self_ok = game::spu_self_entity(spu, c.self);
    c.sample_ok = game::read_bcstring(sample, c.sample, sizeof c.sample, c.sample_len);
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_pending < kRing) {
            g_ring[g_pending++] = c;
        } else {
            ++g_dropped;
        }
    } catch (...) {
        // std::system_error du verrou : la réplique est perdue, jamais d'exception vers le jeu.
    }
}

}  // namespace mp::sync::probe_j3
