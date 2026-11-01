#include "sync/dialogue.h"

#include "common/log.h"
#include "core/protocol.h"
#include "game/combat.h"
#include "game/dialogue.h"
#include "game/entity.h"
#include "sync/anim_filter.h"
#include "sync/avatar.h"
#include "sync/dialogue_rules.h"
#include "sync/npc.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <cwchar>

namespace mp::sync::dialogue {

namespace {

namespace cs = mp::core::session;
namespace proto = mp::protocol;

constexpr std::size_t kRing = 32;                               // StartOutput en attente de la frame
constexpr std::size_t kSampleCap = proto::kMaxDialogSample + 1;  // un octet de plus : trop long détecté

// --- Partagé avec le détour ----------------------------------------------------------------
struct Captured {
    void* self = nullptr;
    bool self_ok = false;
    char sample[kSampleCap] = {};
    std::size_t sample_len = 0;
    bool sample_ok = false;
    int type = 0;
    // État d'InfoSystem lu dans le détour, au moment même de la réplique (pas à la frame).
    bool info_ok = false;
    bool running = false;
    void* partner = nullptr;
};

std::mutex g_mutex;
Captured g_ring[kRing];  // sous g_mutex
std::size_t g_pending = 0;
unsigned g_dropped = 0;
// Story 4.2 : demandes de dialogue (AssessTalk) en attente de la frame, sous g_mutex. Au-delà de
// kTalkRing par frame, les suivantes sont perdues (sans conséquence : le héros n'en fait qu'une).
struct TalkRequest {
    void* npc = nullptr;
    void* asker = nullptr;
};
constexpr std::size_t kTalkRing = 8;
TalkRequest g_talk_ring[kTalkRing];
std::size_t g_talk_pending = 0;
// Story 4.3 : PNJ occupé par l'autre joueur et héros local, publiés par la frame (set_busy) ; demandes
// du héros local à ce PNJ refusées par le détour, en attente du message de la frame.
void* g_busy_npc = nullptr;
void* g_local_hero = nullptr;
void* g_refused[kTalkRing] = {};
std::size_t g_refused_pending = 0;

// --- Thread principal uniquement -----------------------------------------------------------
bool g_active = false;
unsigned g_played = 0;   // invité : rejeux depuis la frame précédente
unsigned g_ignored = 0;  // ignorés depuis la frame précédente (les deux rôles)
bool g_info_warned = false;
bool g_overflow_warned = false;
bool g_sample_warned = false;
bool g_guid_warned = false;
bool g_output_warned = false;
bool g_facial_warned = false;
// Story 4.2, invité : dialogue en cours vu à la frame précédente (IsRunning, NetId du partenaire s'il
// est une réplique, 0 sinon), et InfoSystem illisible déjà signalé.
bool g_guest_running = false;
std::uint32_t g_guest_partner = 0;
bool g_running_warned = false;
bool g_npc_warned = false;
// Story 4.3, hôte : PNJ que son héros occupe hors dialogue (demande acceptée en attente du dialogue, ou
// clôture), comme g_talk_request et g_closing chez l'invité ; et état du dialogue à la frame précédente.
void* g_host_request = nullptr;
std::chrono::steady_clock::time_point g_host_request_deadline{};
void* g_host_closing = nullptr;
std::chrono::steady_clock::time_point g_host_closing_since{};
bool g_host_running = false;
void* g_host_partner = nullptr;
// Story 4.3 : dernier message « occupé » affiché (un par kRefusalCooldown au plus), message absent signalé.
std::chrono::steady_clock::time_point g_last_refusal_message{};
bool g_refusal_message_warned = false;
// Story 4.2 (correctif du test du 2026-10-25) : réplique à qui le héros invité a demandé à parler
// (AssessTalk), comptée comme partenaire jusqu'au début du dialogue ou jusqu'à l'échéance.
constexpr auto kTalkTimeout = std::chrono::seconds(5);
std::uint32_t g_talk_request = 0;
std::chrono::steady_clock::time_point g_talk_deadline{};
// Story 4.2 (même test) : re-figée dès la fin d'IsRunning, la réplique ne finissait pas son ZS_Talk et
// l'invité restait sous les bandes noires. Après la fin, elle reste libre au moins kCloseMin, puis tant
// que la caméra de dialogue est active, kCloseMax au plus.
constexpr auto kCloseMin = std::chrono::seconds(2);
constexpr auto kCloseMax = std::chrono::seconds(10);
std::uint32_t g_closing = 0;
std::chrono::steady_clock::time_point g_closing_since{};
// Story 4.2 : vrai pendant le sStartOutput d'un rejeu (même thread que le détour) : le détour ne relève
// pas ces répliques rejouées, qui ne sont pas celles du joueur local.
thread_local bool t_replaying = false;

std::size_t drain(Captured (&batch)[kRing], unsigned& dropped) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const std::size_t n = g_pending;
    for (std::size_t i = 0; i < n; ++i) batch[i] = g_ring[i];
    g_pending = 0;
    dropped = g_dropped;
    g_dropped = 0;
    return n;
}

std::size_t drain_talks(TalkRequest (&batch)[kTalkRing]) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const std::size_t n = g_talk_pending;
    for (std::size_t i = 0; i < n; ++i) batch[i] = g_talk_ring[i];
    g_talk_pending = 0;
    return n;
}

std::size_t drain_refused(void* (&batch)[kTalkRing]) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const std::size_t n = g_refused_pending;
    for (std::size_t i = 0; i < n; ++i) batch[i] = g_refused[i];
    g_refused_pending = 0;
    return n;
}

std::string utf8_of(const wchar_t* text, std::size_t len) {
    if (len == 0) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(len), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(len), out.data(), n, nullptr, nullptr);
    return out;
}

std::string ptr_text(const void* p) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(p)));
    return buf;
}

void push_counts(std::vector<cs::Input>& out) {
    if (g_played == 0 && g_ignored == 0) return;
    out.push_back(cs::LocalDialogReplayed{g_played, g_ignored});
    g_played = 0;
    g_ignored = 0;
}

std::string distance_text(float cm) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f m", static_cast<double>(cm) / 100.0);
    return buf;
}

}  // namespace

void set_active(bool active) { g_active = active; }

bool active() { return g_active; }

void on_start_output(void* spu, const void* sample, int type) noexcept {
    if (t_replaying) return;  // story 4.2 : rejeu d'une réplique du pair, pas une réplique locale
    Captured c;
    c.type = type;
    c.self_ok = game::spu_self_entity(spu, c.self);
    c.sample_ok = game::read_bcstring(sample, c.sample, sizeof c.sample, c.sample_len);
    // Accesseurs sans effet de bord (sous SEH), comme GetSelfEntity : la première réplique (IsRunning
    // pas encore vu par la frame) et la dernière (dialogue déjà fini à la frame) gardent leur état.
    game::Entity partner;
    c.info_ok = game::info_is_running(c.running) && game::info_dialog_npc(partner);
    c.partner = c.info_ok ? partner.ptr : nullptr;
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

bool on_assess_talk(void* spu, void* self, void* other) noexcept {
    // Arguments de script : `Entity*` (enveloppe d'un eCEntity*), nuls pour ceux de l'unité.
    TalkRequest t;
    if (self != nullptr ? !game::read_entity_ptr(self, t.npc) : !game::spu_self_entity(spu, t.npc)) t.npc = nullptr;
    if (other != nullptr ? !game::read_entity_ptr(other, t.asker) : !game::spu_other_entity(spu, t.asker)) {
        t.asker = nullptr;
    }
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        // Story 4.3 : le héros local demande à parler au PNJ que l'autre joueur occupe : refusée.
        if (t.npc != nullptr && t.npc == g_busy_npc && t.asker != nullptr && t.asker == g_local_hero) {
            if (g_refused_pending < kTalkRing) g_refused[g_refused_pending++] = t.npc;
            return true;
        }
        if (g_talk_pending < kTalkRing) g_talk_ring[g_talk_pending++] = t;
    } catch (...) {
        // std::system_error du verrou : demande perdue (laissée au jeu), jamais d'exception vers le jeu.
    }
    return false;
}

void set_busy(void* hero, void* npc) {
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_local_hero = hero;
        g_busy_npc = npc;
    } catch (...) {
        // std::system_error du verrou : état précédent gardé.
    }
}

namespace {

// Répliques relevées par le détour (hôte et, story 4.2, invité) : règles de sync/dialogue_rules, puis
// LocalDialogLine (hôte : partenaire par son GUID ; invité : par le NetId de sa réplique, 0 sinon).
void process_lines(const Captured* batch, std::size_t n, unsigned dropped, void* hero, bool guest,
                   std::vector<cs::Input>& out) {
    if (dropped != 0) {
        const std::string line =
            "[sync] dialogues : " + std::to_string(dropped) + " StartOutput perdus (plus de 32 par frame)";
        if (!g_overflow_warned) {
            g_overflow_warned = true;
            log::warn(line + " (une fois par session, puis en debug)");
        } else {
            log::debug(line);
        }
        g_ignored += dropped;
    }
    for (std::size_t i = 0; i < n; ++i) {
        const Captured& c = batch[i];
        if (!c.info_ok) {
            ++g_ignored;
            if (!g_info_warned) {
                g_info_warned = true;
                log::warn("[sync] dialogues : InfoSystem illisible (SEH) au moment d'une réplique : ignorée "
                          "(une fois par session)");
            }
            continue;
        }
        const std::string sample = c.sample_ok ? std::string(c.sample, c.sample_len) : std::string();
        const dialogue_rules::HostDecision d =
            dialogue_rules::host_decide(c.running, c.self_ok ? c.self : nullptr, hero, c.partner, c.type, sample);
        if (d.action == dialogue_rules::HostAction::Ignore) {
            if (!d.counted) continue;  // commentaire ambiant : ni compté ni journalisé
            ++g_ignored;
            log::debug("[sync] dialogues : réplique « " + (c.sample_ok ? sample : std::string("illisible")) +
                       " » (type " + std::to_string(c.type) + ") ignorée (" + d.reason + ")");
            if (std::strcmp(d.reason, "échantillon ou type invalide") == 0 && !g_sample_warned) {
                g_sample_warned = true;
                log::warn("[sync] dialogues : réplique ignorée, échantillon « " +
                          (c.sample_ok ? sample : std::string("illisible")) + " » ou type " +
                          std::to_string(c.type) + " invalide (une fois par session)");
            }
            continue;
        }
        cs::LocalDialogLine line;
        line.hero = d.action == dialogue_rules::HostAction::SendHero;
        line.output_type = static_cast<std::uint8_t>(c.type);
        line.sample = sample;
        if (!line.hero && guest) {
            // Story 4.2 : la session n'envoie que le partenaire du dialogue en cours (0 : pas une réplique).
            line.net_id = npc::replica_net_id(c.partner);
        } else if (!line.hero) {
            std::uint8_t id[game::kPropertyIdSize];
            if (!game::get_entity_id(game::Entity{c.partner}, id)) {
                ++g_ignored;
                log::debug("[sync] dialogues : réplique « " + sample + " » ignorée (GUID du partenaire illisible)");
                if (!g_guid_warned) {
                    g_guid_warned = true;
                    log::warn("[sync] dialogues : GUID du partenaire de dialogue illisible (SEH) : réplique "
                              "ignorée (une fois par session)");
                }
                continue;
            }
            std::memcpy(line.guid.data(), id, line.guid.size());
        }
        out.push_back(std::move(line));
    }
}

}  // namespace

namespace {

// Story 4.3 : demandes refusées par le détour : message du journal en jeu (un par kRefusalCooldown au
// plus, une touche tenue en redemande) et Info.
constexpr auto kRefusalCooldown = std::chrono::seconds(2);

void notify_refusals() {
    void* refused[kTalkRing];
    const std::size_t n = drain_refused(refused);
    if (n == 0) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - g_last_refusal_message < kRefusalCooldown) return;
    g_last_refusal_message = now;
    wchar_t name[128];
    std::size_t len = 0;
    if (!game::focus_name(game::Entity{refused[n - 1]}, name, sizeof name / sizeof name[0], len) || len == 0) {
        static constexpr wchar_t kGeneric[] = L"Ce personnage";
        len = sizeof kGeneric / sizeof kGeneric[0] - 1;
        std::wmemcpy(name, kGeneric, len);
    }
    const std::wstring message = std::wstring(name, len) + L" ne peut pas vous parler, il est occupé.";
    const bool shown = game::print_game_log(message.c_str());
    log::info("[sync] dialogues : demande de dialogue au PNJ « " + utf8_of(name, len) + " » (" +
              ptr_text(refused[n - 1]) + ") refusée : occupé par l'autre joueur" +
              (shown ? std::string() : std::string(" (message à l'écran impossible)")));
    if (!shown && !g_refusal_message_warned) {
        g_refusal_message_warned = true;
        log::warn("[sync] dialogues : GUI2::PrintGameLog absent ou en échec : refus de dialogue sans message à "
                  "l'écran (une fois par session)");
    }
}

}  // namespace

void host_frame(void* hero, bool in_session, std::vector<cs::Input>& out) {
    Captured batch[kRing];
    unsigned dropped = 0;
    const std::size_t n = drain(batch, dropped);
    TalkRequest talks[kTalkRing];
    const std::size_t nt = drain_talks(talks);  // hôte : PNJ natifs, rien à libérer
    notify_refusals();
    // Story 4.3 : demande acceptée du héros de l'hôte : PNJ occupé jusqu'au dialogue (ou 5 s sans).
    const auto now = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < nt && in_session && hero != nullptr; ++i) {
        if (talks[i].asker != hero || talks[i].npc == nullptr) continue;
        log::debug("[sync] dialogues : AssessTalk du héros de l'hôte au PNJ " + ptr_text(talks[i].npc));
        g_host_request = talks[i].npc;
        g_host_request_deadline = now + kTalkTimeout;
    }
    if (!g_active || !in_session || hero == nullptr) {
        g_ignored = 0;
        g_host_request = nullptr;
        g_host_closing = nullptr;
        g_host_running = false;
        g_host_partner = nullptr;
        return;
    }
    process_lines(batch, n, dropped, hero, false, out);
    push_counts(out);
}

void read_host_state(cs::World& world) {
    world.dialog_running = false;
    world.dialog_partner_ok = false;
    world.engaged_ok = false;
    if (!g_active || !world.loaded()) return;
    bool running = false;
    const bool readable = game::info_is_running(running);
    game::Entity partner;
    if (readable && running) {
        world.dialog_running = true;
        std::uint8_t id[game::kPropertyIdSize];
        if (game::info_dialog_npc(partner) && partner.ptr != nullptr && game::get_entity_id(partner, id)) {
            std::memcpy(world.dialog_partner.data(), id, world.dialog_partner.size());
            world.dialog_partner_ok = true;
        }
    }
    // Story 4.3 : PNJ occupé par le héros de l'hôte hors dialogue : demande en attente, puis clôture
    // (mêmes délais que la réplique libérée de l'invité : sa routine finit ZS_Talk).
    const auto now = std::chrono::steady_clock::now();
    if (readable && running) {
        g_host_request = nullptr;  // dialogue commencé : son partenaire fait foi
        g_host_closing = nullptr;
    } else if (g_host_request != nullptr && now >= g_host_request_deadline) {
        g_host_request = nullptr;
    }
    if (readable && g_host_running && !running && g_host_partner != nullptr && g_host_request == nullptr) {
        g_host_closing = g_host_partner;
        g_host_closing_since = now;
    }
    if (g_host_closing != nullptr && g_host_request == nullptr) {
        bool camera = false;
        const bool camera_ok = game::dialog_camera_active(camera);
        const auto elapsed = now - g_host_closing_since;
        if (!(elapsed < kCloseMin || (camera_ok && camera && elapsed < kCloseMax))) g_host_closing = nullptr;
    }
    if (readable) {
        g_host_running = running;
        g_host_partner = running ? partner.ptr : nullptr;
    }
    void* engaged = g_host_request != nullptr ? g_host_request : g_host_closing;
    std::uint8_t id[game::kPropertyIdSize];
    if (engaged != nullptr && game::get_entity_id(game::Entity{engaged}, id)) {
        std::memcpy(world.engaged.data(), id, world.engaged.size());
        world.engaged_ok = true;
    }
}

void guest_frame(void* hero, bool in_session, std::vector<cs::Input>& out) {
    Captured batch[kRing];
    unsigned dropped = 0;
    const std::size_t n = drain(batch, dropped);
    TalkRequest talks[kTalkRing];
    const std::size_t nt = drain_talks(talks);
    notify_refusals();  // story 4.3
    if (!g_active || !in_session || hero == nullptr) {
        // Monde déchargé pendant un dialogue : fin signalée (la session l'ignore hors session).
        if (g_guest_partner != 0) out.push_back(cs::LocalDialogState{g_guest_partner, false});
        g_guest_running = false;
        g_guest_partner = 0;
        g_talk_request = 0;
        g_closing = 0;
        push_counts(out);  // bilan des rejeux de la frame précédente
        return;
    }
    // Story 4.2 : dialogue de l'invité à cette frame. Illisible : état précédent gardé.
    bool running = false;
    bool readable = game::info_is_running(running);
    game::Entity partner;
    if (!readable) {
        running = g_guest_running;
        if (!g_running_warned) {
            g_running_warned = true;
            log::warn("[sync] dialogues : InfoSystem::IsRunning illisible (SEH) à la frame : état du dialogue de "
                      "l'invité gardé (une fois par session)");
        }
    } else if (running && !game::info_dialog_npc(partner)) {
        readable = false;
        if (!g_npc_warned) {
            g_npc_warned = true;
            log::warn("[sync] dialogues : InfoSystem::GetNPC illisible (SEH) à la frame : état du dialogue de "
                      "l'invité gardé (une fois par session)");
        }
    }
    const bool was_running = g_guest_running;
    std::uint32_t partner_id = readable && running ? npc::replica_net_id(partner.ptr) : 0;
    // Story 4.2 : demande de dialogue du héros invité. Une réplique figée n'exécuterait jamais le
    // ZS_Talk lancé par AssessTalk : elle est libérée dès maintenant, avant IsRunning.
    const auto now = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < nt; ++i) {
        if (talks[i].asker != hero) continue;  // conversation entre PNJ : pas l'affaire de l'invité
        const std::uint32_t id = npc::replica_net_id(talks[i].npc);
        log::debug("[sync] dialogues : AssessTalk du héros invité au PNJ " + ptr_text(talks[i].npc) +
                   (id != 0 ? " (réplique " + std::to_string(id) + ")" : std::string(" (pas une réplique)")));
        if (id == 0) continue;
        if (g_talk_request != id) {
            log::info("[sync] dialogues : demande de dialogue de l'invité au PNJ " + std::to_string(id) +
                      " : réplique libérée avant le dialogue");
        }
        g_talk_request = id;
        g_talk_deadline = now + kTalkTimeout;
    }
    if (g_talk_request != 0) {
        if (readable && running) {
            g_talk_request = 0;  // dialogue commencé : son partenaire fait foi
        } else if (now >= g_talk_deadline) {
            log::info("[sync] dialogues : pas de dialogue 5 s après la demande au PNJ " +
                      std::to_string(g_talk_request) + " : réplique re-figée");
            g_talk_request = 0;
        } else {
            partner_id = g_talk_request;  // en attente du dialogue : reste libérée
        }
    }
    if (readable && was_running && !running && g_guest_partner != 0 && g_talk_request == 0) {
        g_closing = g_guest_partner;
        g_closing_since = now;
        log::debug("[sync] dialogues : fin du dialogue avec le PNJ " + std::to_string(g_closing) +
                   " : réplique laissée libre le temps de clore");
    }
    if (g_closing != 0) {
        if ((readable && running) || g_talk_request != 0) {
            g_closing = 0;  // nouveau dialogue ou nouvelle demande : ils font foi
        } else {
            bool camera = false;
            const bool camera_ok = game::dialog_camera_active(camera);
            const auto elapsed = now - g_closing_since;
            if (elapsed < kCloseMin || (camera_ok && camera && elapsed < kCloseMax)) {
                partner_id = g_closing;
            } else {
                log::debug("[sync] dialogues : PNJ " + std::to_string(g_closing) + " : dialogue clos après " +
                           std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()) +
                           " ms (caméra de dialogue " +
                           (!camera_ok ? "illisible" : camera ? "encore active, délai dépassé" : "inactive") + ")");
                g_closing = 0;
            }
        }
    }
    if (readable && running != g_guest_running) {
        char ptr[24];
        std::snprintf(ptr, sizeof ptr, "0x%llx",
                      static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(partner.ptr)));
        log::debug(std::string("[sync] dialogues : dialogue de l'invité ") + (running ? "commencé" : "fini") +
                   (!running                ? std::string()
                    : partner.ptr == nullptr ? std::string(", partenaire nul (GetNPC)")
                                             : std::string(", partenaire ") + ptr +
                                                   (partner_id != 0 ? " (réplique " + std::to_string(partner_id) + ")"
                                                                    : std::string(" (pas une réplique)"))));
    }
    if (readable) g_guest_running = running;
    npc::log_talk_states(running);  // H1, debug seulement
    const dialogue_rules::GuestTransition t = dialogue_rules::guest_transition(g_guest_partner, partner_id, readable);
    // Début (ou nouveau partenaire) avant les répliques de la frame, fin après : la première et la
    // dernière réplique tombent dans le dialogue (la session refuse le partenaire hors dialogue).
    if (t.start != 0) out.push_back(cs::LocalDialogState{t.start, true});
    process_lines(batch, n, dropped, hero, true, out);
    if (t.end != 0) out.push_back(cs::LocalDialogState{t.end, false});
    g_guest_partner = t.next;
    push_counts(out);
}

void replay(void* hero, std::uint32_t net_id, std::uint8_t output_type, const std::string& sample, void* speaker,
            const char* speaker_name) {
    const std::string what = "[sync] DialogLine (locuteur " + std::to_string(net_id) + ", type " +
                             std::to_string(output_type) + ", « " + sample + " »)";
    const char* why = nullptr;
    float distance = 0.0f;
    if (!g_active) {
        why = "dialogues inactifs";
    } else if (!proto::valid_dialog_sample(sample)) {  // déjà vérifié au décodage : défense en profondeur
        why = "échantillon invalide";
    } else {
        bool dead = false;
        game::Matrix own{}, other{};
        bool running = false;
        if (speaker == nullptr) {
            why = "entité du locuteur absente ou morte";
        } else if (!game::is_dead(game::Entity{speaker}, dead)) {
            why = "Entity::IsDead en échec";
        } else if (hero == nullptr || !game::get_pose(game::Entity{hero}, own) ||
                   !game::get_pose(game::Entity{speaker}, other)) {
            why = "pose illisible";
        } else if (!game::info_is_running(running)) {
            why = "InfoSystem::IsRunning en échec";
        } else {
            const float dx = other.m[12] - own.m[12], dy = other.m[13] - own.m[13], dz = other.m[14] - own.m[14];
            distance = std::sqrt(dx * dx + dy * dy + dz * dz);
            why = dialogue_rules::guest_decide(distance, running, !dead).reason;  // nul : rejoué
        }
    }
    if (why != nullptr) {
        ++g_ignored;
        log::debug(what + " ignorée (" + why + (distance > 0.0f ? ", " + distance_text(distance) : std::string()) +
                   ")");
        return;
    }
    const game::Entity entity{speaker};
    bool started = false;
    bool output_ok = false;
    bool facial_ok = false;
    {
        AnimFilter::ReplayGuard guard;  // le registre laisse passer l'animation des lèvres
        t_replaying = true;             // story 4.2 : le détour de StartOutput ne relève pas ce rejeu
        output_ok = game::start_output(entity, sample.c_str(), output_type, started);
        t_replaying = false;
        facial_ok = game::start_facial_ani(entity, sample.c_str());
    }
    if (!output_ok && !g_output_warned) {
        g_output_warned = true;
        log::warn(what + " : sStartOutput a échoué (SEH), voix absente (une fois par session)");
    }
    if (!facial_ok && !g_facial_warned) {
        g_facial_warned = true;
        log::warn(what + " : StartFacialAni a échoué (SEH), lèvres immobiles (une fois par session)");
    }
    if (!output_ok || !started) {
        ++g_ignored;
        if (output_ok) log::debug(what + " : sStartOutput a rendu false (PS de routine absent ?), voix absente");
        return;
    }
    ++g_played;
    log::debug(what + " rejouée sur " + speaker_name + " à " + distance_text(distance) +
               (facial_ok ? "" : " (sans lèvres)"));
}

void reset() {
    g_played = 0;
    g_ignored = 0;
    g_info_warned = false;
    g_overflow_warned = false;
    g_sample_warned = false;
    g_guid_warned = false;
    g_output_warned = false;
    g_facial_warned = false;
    g_guest_running = false;  // story 4.2
    g_guest_partner = 0;
    g_running_warned = false;
    g_npc_warned = false;
    g_talk_request = 0;
    g_closing = 0;
    g_host_request = nullptr;  // story 4.3
    g_host_closing = nullptr;
    g_host_running = false;
    g_host_partner = nullptr;
    g_refusal_message_warned = false;
    set_busy(nullptr, nullptr);
}

}  // namespace mp::sync::dialogue
