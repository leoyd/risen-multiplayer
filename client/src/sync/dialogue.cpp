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

void host_frame(void* hero, bool in_session, std::vector<cs::Input>& out) {
    Captured batch[kRing];
    unsigned dropped = 0;
    const std::size_t n = drain(batch, dropped);
    if (!g_active || !in_session || hero == nullptr) {
        g_ignored = 0;
        return;
    }
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
        if (!line.hero) {
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
    push_counts(out);
}

void guest_frame(std::vector<cs::Input>& out) {
    Captured batch[kRing];
    unsigned dropped = 0;
    drain(batch, dropped);  // story 4.1 : l'invité n'émet rien (rejeux compris)
    push_counts(out);
}

void replay(void* hero, std::uint32_t net_id, std::uint8_t output_type, const std::string& sample) {
    const std::string what = "[sync] DialogLine (locuteur " + std::to_string(net_id) + ", type " +
                             std::to_string(output_type) + ", « " + sample + " »)";
    const char* why = nullptr;
    void* speaker = nullptr;
    float distance = 0.0f;
    if (!g_active) {
        why = "dialogues inactifs";
    } else if (!proto::valid_dialog_sample(sample)) {  // déjà vérifié au décodage : défense en profondeur
        why = "échantillon invalide";
    } else {
        speaker = net_id == proto::kHostNetId ? avatar::entity() : npc::replica_entity(net_id);
        bool dead = false;
        game::Matrix own{}, other{};
        bool running = false;
        if (speaker == nullptr) {
            why = net_id == proto::kHostNetId ? "pas d'avatar de l'hôte" : "réplique absente ou morte";
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
        output_ok = game::start_output(entity, sample.c_str(), output_type, started);
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
    log::debug(what + " rejouée sur " + (net_id == proto::kHostNetId ? std::string("l'avatar de l'hôte") : "la réplique") +
              " à " + distance_text(distance) + (facial_ok ? "" : " (sans lèvres)"));
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
}

}  // namespace mp::sync::dialogue
