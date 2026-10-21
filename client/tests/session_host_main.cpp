// Hôte de session sous Linux, sans le jeu (stories 1.4 et 1.5) : core/session + net::Transport,
// avec un monde simulé chargé. Même boucle que le pilote Windows (sync/session) : lire le
// monde, vider poll(), tick, exécuter les commandes, écrire les logs.
//
// Usage : session_host --port N [--max-ms T] [--ends E] [--weapon NOM] [--armor NOM] [--npcs N]
// Sort après la E-ième fin de session (défaut 1), une fois le maintien terminé (Ended →
// Inactive), ou après T ms (défaut 30000). Log sur stdout ; la dernière ligne est
// « état final : <état> ». Code de sortie : 0, ou 1 sur erreur d'usage.
//
// Signaux (story 1.5) : SIGUSR1 = fin de monde (rechargement : le joueur change d'adresse
// et revit) ; SIGUSR2 = mort du héros de l'hôte.
//
// Arme (story 3.2) : --weapon NOM met cette arme dans la main du héros de l'hôte dès le départ
// (LocalWeapon) ; SIGHUP la range ou la ressort.
// Armure (story 3.2c) : --armor NOM la met sur le héros de l'hôte dès le départ (LocalArmor) ;
// SIGHUP la retire ou la remet (en même temps que l'arme).
// PNJ (story 3.3) : --npcs N simule N PNJ autour du héros de l'hôte (GUID propre à chacun), le
// PNJ i à 5 m + 50 cm × i, PV qui baissent avec le temps. Après l'entrée en session : de 1,5 à
// 2,5 s, les PNJ impairs partent à 60 m et plus (sortie du rayon, place pour ceux qui
// attendaient au-delà du 53ᵉ), puis reviennent (même NetId) ; le PNJ 0 meurt à 2 s. Ils sont
// lus, comme dans la DLL, seulement quand Session::wants_npcs le demande.
#include "core/session.h"
#include "net/transport.h"

#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

#ifndef MP_MOD_VERSION
#error "MP_MOD_VERSION doit être défini"
#endif

using namespace mp::core::session;

namespace {

using Clock = std::chrono::steady_clock;

volatile std::sig_atomic_t g_reload_requests = 0;
volatile std::sig_atomic_t g_death_requested = 0;
volatile std::sig_atomic_t g_weapon_toggles = 0;

extern "C" void on_sigusr1(int) { g_reload_requests = g_reload_requests + 1; }
extern "C" void on_sigusr2(int) { g_death_requested = 1; }
extern "C" void on_sighup(int) { g_weapon_toggles = g_weapon_toggles + 1; }

void print(const char* level, const std::string& text) {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm tm{};
    localtime_r(&t, &tm);
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "%H:%M:%S", &tm);
    std::printf("[%s.%03d] %-5s %s\n", stamp, static_cast<int>(ms), level, text.c_str());
    std::fflush(stdout);
}

const char* level_name(Level l) {
    switch (l) {
        case Level::Debug: return "DEBUG";
        case Level::Info: return "INFO";
        case Level::Warn: return "WARN";
        case Level::Error: return "ERROR";
    }
    return "?";
}

}  // namespace

int main(int argc, char** argv) {
    long port = -1;
    long max_ms = 30000;
    long max_ends = 1;
    std::string weapon;
    std::string armor;
    long npc_count = 0;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (std::strcmp(argv[i], "--npcs") == 0) npc_count = std::strtol(argv[i + 1], nullptr, 10);
        if (std::strcmp(argv[i], "--weapon") == 0) weapon = argv[i + 1];
        if (std::strcmp(argv[i], "--armor") == 0) armor = argv[i + 1];
        if (std::strcmp(argv[i], "--port") == 0) port = std::strtol(argv[i + 1], nullptr, 10);
        else if (std::strcmp(argv[i], "--max-ms") == 0) max_ms = std::strtol(argv[i + 1], nullptr, 10);
        else if (std::strcmp(argv[i], "--ends") == 0) max_ends = std::strtol(argv[i + 1], nullptr, 10);
    }
    if (port <= 0 || port > 65535 || max_ms <= 0 || max_ends <= 0 || npc_count < 0 || npc_count > 255 ||
        argc % 2 == 0) {
        std::fprintf(stderr,
                     "usage : session_host --port N [--max-ms T] [--ends E] [--weapon NOM] [--armor NOM] [--npcs N]\n");
        return 1;
    }
    std::signal(SIGUSR1, on_sigusr1);
    std::signal(SIGUSR2, on_sigusr2);
    std::signal(SIGHUP, on_sighup);

    Settings settings;
    settings.role = mp::Role::Host;
    settings.port = static_cast<std::uint16_t>(port);
    settings.nickname = "HoteLinux";
    settings.mod_version = MP_MOD_VERSION;
    // Empreintes de Risen Steam x64 (janvier 2023), celles du faux invité.
    settings.fingerprint = {{{0x63c9aff6, 0x01417000},
                             {0x63ca945d, 0x00d3a000},
                             {0x63c9b014, 0x004d1000},
                             {0x63c569b6, 0x0025a000}}};
    settings.seed = static_cast<std::uint64_t>(Clock::now().time_since_epoch().count());
    Session session(settings);
    mp::net::Transport transport;

    World world;
    world.player = 0x1000;
    world.running = true;
    world.pose_ok = true;
    world.x = 1.0f;
    world.y = 2.0f;
    world.z = 3.0f;
    world.yaw = 0.5f;

    const auto origin = Clock::now();
    auto now_ms = [&] {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - origin).count());
    };

    // Story 3.3 : PNJ simulés (pose et PV en fonction du temps depuis l'entrée en session).
    std::uint64_t session_start = 0;
    bool in_session = false;
    auto fill_npcs = [&](std::uint64_t now) {
        world.npcs.clear();
        world.npcs_ok = true;
        const double t = static_cast<double>(now - session_start) / 1000.0;  // s
        constexpr double kPi = 3.14159265358979323846;
        for (long i = 0; i < npc_count; ++i) {
            NpcState n;
            for (std::size_t b = 0; b < n.guid.size(); ++b) n.guid[b] = static_cast<std::uint8_t>(0xa0 + b);
            n.guid[0] = static_cast<std::uint8_t>(i);
            const double angle = 2.0 * kPi * static_cast<double>(i) / static_cast<double>(npc_count);
            double radius = 500.0 + 50.0 * static_cast<double>(i);  // 5 m à 5 + 0,5 × N m
            if (i % 2 == 1 && t >= 1.5 && t < 2.5) radius += 6000.0;  // sortie du rayon, puis retour
            radius += 20.0 * std::sin(4.0 * t + angle);                // petit mouvement
            n.pose.x = world.x + static_cast<float>(radius * std::sin(angle));
            n.pose.y = world.y;
            n.pose.z = world.z + static_cast<float>(radius * std::cos(angle));
            n.pose.yaw = static_cast<float>(angle);
            n.hp = 100 + static_cast<int>(i) - static_cast<int>(t * 4.0);
            n.alive = !(i == 0 && t > 2.0);
            world.npcs.push_back(n);
        }
    };

    std::vector<Input> inputs;
    int avatars = 0;  // avatars présents (story 2.2) : ne doit jamais dépasser 1
    // Story 2.3 : plus grand saut entre deux MoveAvatar successifs (une téléportation visible).
    struct Track {
        bool have = false;
        mp::core::Pose last;
        double max_jump = 0.0, travelled = 0.0;
        long moves = 0;
    } track;
    auto report = [&](const char* when) {
        if (track.moves == 0) return;
        char buf[160];
        std::snprintf(buf, sizeof buf, "[test] avatar (%s) : %ld déplacements, parcouru %.0f cm, saut max %.1f cm",
                      when, track.moves, track.travelled, track.max_jump);
        print("INFO", buf);
    };
    long ends = 0;  // transitions Ended → Inactive (fin du maintien)
    std::sig_atomic_t reloads_seen = 0;
    std::sig_atomic_t toggles_seen = 0;
    bool weapon_out = !weapon.empty();
    bool weapon_pushed = false;
    bool armor_worn = !armor.empty();
    bool armor_pushed = false;
    while (now_ms() < static_cast<std::uint64_t>(max_ms)) {
        if (g_reload_requests != reloads_seen) {
            reloads_seen = g_reload_requests;
            world.player += 0x1000;  // nouveau monde : autre héros, vivant
            world.dead = false;
            print("INFO", "[test] fin de monde simulée (rechargement)");
        }
        if (g_death_requested != 0) {
            g_death_requested = 0;
            world.dead = true;
            print("INFO", "[test] mort du héros de l'hôte simulée");
        }
        inputs.clear();
        if (g_weapon_toggles != toggles_seen) {
            toggles_seen = g_weapon_toggles;
            if (!weapon.empty()) {
                weapon_out = !weapon_out;
                weapon_pushed = false;
                print("INFO", std::string("[test] arme de l'hôte ") + (weapon_out ? "sortie" : "rangée"));
            }
            if (!armor.empty()) {
                armor_worn = !armor_worn;
                armor_pushed = false;
                print("INFO", std::string("[test] armure de l'hôte ") + (armor_worn ? "remise" : "retirée"));
            }
        }
        if (!weapon.empty() && !weapon_pushed) {
            weapon_pushed = true;
            inputs.push_back(LocalWeapon{weapon_out ? weapon : std::string()});
        }
        if (!armor.empty() && !armor_pushed) {
            armor_pushed = true;
            inputs.push_back(LocalArmor{armor_worn ? armor : std::string()});
        }
        mp::net::Event ev;
        while (transport.poll(ev)) {
            if (auto* c = std::get_if<mp::net::ControlReceived>(&ev)) {
                inputs.push_back(ControlIn{c->source, std::move(c->bytes)});
            } else if (auto* r = std::get_if<mp::net::ReliableReceived>(&ev)) {
                inputs.push_back(ReliableIn{std::move(r->bytes)});
            } else {
                inputs.push_back(SnapshotIn{std::move(std::get<mp::net::SnapshotReceived>(ev).bytes)});
            }
        }
        const std::uint64_t now = now_ms();
        if (session.state() == State::InSession && !in_session) session_start = now;
        in_session = session.state() == State::InSession;
        world.npcs.clear();
        world.npcs_ok = false;
        if (npc_count > 0 && session.wants_npcs(now)) fill_npcs(now);
        std::vector<Output> pending;
        pending.push_back(session.tick(now, world, inputs));
        for (std::size_t p = 0; p < pending.size(); ++p) {
            for (const auto& line : pending[p].logs) {
                print(level_name(line.level), line.text);
                if (line.text.find("[sync] Ended → Inactive") != std::string::npos) ++ends;
            }
            for (auto& cmd : pending[p].commands) {
                if (auto* s = std::get_if<Start>(&cmd)) {
                    std::string error;
                    if (!transport.start(s->port, error)) {
                        pending.push_back(session.fail(now_ms(), "réseau : " + error));
                        break;
                    }
                    print("INFO", "[sync] transport démarré sur le port UDP " + std::to_string(transport.local_port()));
                } else if (auto* s = std::get_if<Send>(&cmd)) {
                    transport.post(mp::net::SendControl{s->destination, std::move(s->bytes)});
                } else if (auto* o = std::get_if<OpenReliable>(&cmd)) {
                    transport.post(mp::net::OpenReliable{o->conv, o->peer});
                } else if (auto* r = std::get_if<SendReliable>(&cmd)) {
                    transport.post(mp::net::SendReliable{std::move(r->bytes)});
                } else if (std::holds_alternative<CloseReliable>(cmd)) {
                    transport.post(mp::net::CloseReliable{});
                } else if (std::holds_alternative<Stop>(cmd)) {
                    transport.stop();
                } else if (const auto* t = std::get_if<Teleport>(&cmd)) {
                    print("INFO", "[test] Teleport (" + std::to_string(t->pose.x) + ", " + std::to_string(t->pose.y) +
                                      ", " + std::to_string(t->pose.z) + ")");
                } else if (auto* sn = std::get_if<SendSnapshot>(&cmd)) {
                    transport.post(mp::net::SendSnapshot{std::move(sn->bytes)});
                } else if (const auto* ma = std::get_if<MoveAvatar>(&cmd)) {
                    if (track.have) {
                        const double dx = ma->pose.x - track.last.x, dy = ma->pose.y - track.last.y,
                                     dz = ma->pose.z - track.last.z;
                        const double d = std::sqrt(dx * dx + dy * dy + dz * dz);
                        track.travelled += d;
                        if (d > track.max_jump) track.max_jump = d;
                    }
                    track.have = true;
                    track.last = ma->pose;
                    ++track.moves;
                } else if (const auto* pm = std::get_if<PlayAvatarMotion>(&cmd)) {
                    print("INFO", "[test] PlayAvatarMotion NetId " + std::to_string(pm->net_id) + " « " + pm->name +
                                      " » type " + std::to_string(pm->motion.type) + " ms " +
                                      std::to_string(pm->motion.ms));
                } else if (const auto* pp = std::get_if<PlayAvatarParam>(&cmd)) {
                    print("INFO", "[test] PlayAvatarParam NetId " + std::to_string(pp->net_id) + " type " +
                                      std::to_string(pp->param.type) + " nature " + std::to_string(pp->param.kind) +
                                      " valeur " + std::to_string(pp->param.value));
                } else if (const auto* aw = std::get_if<AvatarWeapon>(&cmd)) {
                    print("INFO", "[test] AvatarWeapon NetId " + std::to_string(aw->net_id) + " « " + aw->name + " »");
                } else if (const auto* aa = std::get_if<AvatarArmor>(&cmd)) {
                    print("INFO", "[test] AvatarArmor NetId " + std::to_string(aa->net_id) + " « " + aa->name + " »");
                } else if (const auto* ea = std::get_if<EquipAvatar>(&cmd)) {
                    std::string names;
                    for (const auto& n : ea->names) names += (names.empty() ? "" : ",") + n;
                    print("INFO", "[test] EquipAvatar NetId " + std::to_string(ea->net_id) + " « " + names + " »");
                } else if (const auto* ad = std::get_if<AvatarDeath>(&cmd)) {
                    print("INFO", "[test] AvatarDeath NetId " + std::to_string(ad->net_id));
                } else if (const auto* sa = std::get_if<SpawnAvatar>(&cmd)) {
                    track = Track{};
                    track.have = true;  // le premier pas (Spawn → premier MoveAvatar) compte aussi
                    track.last = sa->pose;
                    ++avatars;
                    print(avatars > 1 ? "ERROR" : "INFO", "[test] SpawnAvatar NetId " + std::to_string(sa->net_id) +
                                                              " : avatars présents=" + std::to_string(avatars));
                } else if (const auto* da = std::get_if<DespawnAvatar>(&cmd)) {
                    report("retrait");
                    track = Track{};
                    --avatars;
                    print(avatars < 0 ? "ERROR" : "INFO", "[test] DespawnAvatar NetId " + std::to_string(da->net_id) +
                                                              " : avatars présents=" + std::to_string(avatars));
                }
            }
        }
        if (ends >= max_ends) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    transport.stop();
    print("INFO", std::string("état final : ") + state_name(session.state()));
    return 0;
}
