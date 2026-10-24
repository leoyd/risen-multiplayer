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

#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

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

void host_frame(void* hero, bool in_session, std::vector<cs::Input>& out) {
    Captured batch[kRing];
    unsigned dropped = 0;
    const std::size_t n = drain(batch, dropped);
    if (!g_active || !in_session || hero == nullptr) {
        g_ignored = 0;
        return;
    }
    process_lines(batch, n, dropped, hero, false, out);
    push_counts(out);
}

void read_host_state(cs::World& world) {
    world.dialog_running = false;
    world.dialog_partner_ok = false;
    if (!g_active || !world.loaded()) return;
    bool running = false;
    if (!game::info_is_running(running) || !running) return;
    world.dialog_running = true;
    game::Entity partner;
    std::uint8_t id[game::kPropertyIdSize];
    if (!game::info_dialog_npc(partner) || partner.ptr == nullptr || !game::get_entity_id(partner, id)) return;
    std::memcpy(world.dialog_partner.data(), id, world.dialog_partner.size());
    world.dialog_partner_ok = true;
}

void guest_frame(void* hero, bool in_session, std::vector<cs::Input>& out) {
    Captured batch[kRing];
    unsigned dropped = 0;
    const std::size_t n = drain(batch, dropped);
    if (!g_active || !in_session || hero == nullptr) {
        // Monde déchargé pendant un dialogue : fin signalée (la session l'ignore hors session).
        if (g_guest_partner != 0) out.push_back(cs::LocalDialogState{g_guest_partner, false});
        g_guest_running = false;
        g_guest_partner = 0;
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
    const std::uint32_t partner_id = readable && running ? npc::replica_net_id(partner.ptr) : 0;
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
}

}  // namespace mp::sync::dialogue
