// Hôte de session sous Linux, sans le jeu (stories 1.4 et 1.5) : core/session + net::Transport,
// avec un monde simulé chargé. Même boucle que le pilote Windows (sync/session) : lire le
// monde, vider poll(), tick, exécuter les commandes, écrire les logs.
//
// Usage : session_host --port N [--max-ms T] [--ends E]
// Sort après la E-ième fin de session (défaut 1), une fois le maintien terminé (Ended →
// Inactive), ou après T ms (défaut 30000). Log sur stdout ; la dernière ligne est
// « état final : <état> ». Code de sortie : 0, ou 1 sur erreur d'usage.
//
// Signaux (story 1.5) : SIGUSR1 = fin de monde (rechargement : le joueur change d'adresse
// et revit) ; SIGUSR2 = mort du héros de l'hôte.
#include "core/session.h"
#include "net/transport.h"

#include <chrono>
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

extern "C" void on_sigusr1(int) { g_reload_requests = g_reload_requests + 1; }
extern "C" void on_sigusr2(int) { g_death_requested = 1; }

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
    for (int i = 1; i + 1 < argc; i += 2) {
        if (std::strcmp(argv[i], "--port") == 0) port = std::strtol(argv[i + 1], nullptr, 10);
        else if (std::strcmp(argv[i], "--max-ms") == 0) max_ms = std::strtol(argv[i + 1], nullptr, 10);
        else if (std::strcmp(argv[i], "--ends") == 0) max_ends = std::strtol(argv[i + 1], nullptr, 10);
    }
    if (port <= 0 || port > 65535 || max_ms <= 0 || max_ends <= 0 || argc % 2 == 0) {
        std::fprintf(stderr, "usage : session_host --port N [--max-ms T] [--ends E]\n");
        return 1;
    }
    std::signal(SIGUSR1, on_sigusr1);
    std::signal(SIGUSR2, on_sigusr2);

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

    std::vector<Input> inputs;
    long ends = 0;  // transitions Ended → Inactive (fin du maintien)
    std::sig_atomic_t reloads_seen = 0;
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
        mp::net::Event ev;
        while (transport.poll(ev)) {
            if (auto* c = std::get_if<mp::net::ControlReceived>(&ev)) {
                inputs.push_back(ControlIn{c->source, std::move(c->bytes)});
            } else {
                inputs.push_back(ReliableIn{std::move(std::get<mp::net::ReliableReceived>(ev).bytes)});
            }
        }
        std::vector<Output> pending;
        pending.push_back(session.tick(now_ms(), world, inputs));
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
