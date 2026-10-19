// Transport UDP + KCP (story 1.2). Un socket UDP, octet 0 = canal (protocol/PROTOCOL.md §2) :
// - canal 0 : segments KCP (`conv` = identifiant de session), KCP 2.1.1 vendorisé ;
// - canal 1 : snapshots (story 2.3) — datagrammes transmis tels quels, échangés avec le pair du KCP ;
// - canal 2 : contrôle (Hello/Welcome/Reject), datagrammes transmis tels quels.
//
// Threads : le thread réseau possède seul le socket et le KCP (qui n'est pas thread-safe).
// La frame ne partage avec lui que deux files sous verrou (commandes sortantes, événements
// entrants, 4096 éléments au plus chacune), un drapeau d'arrêt et des compteurs atomiques.
// Une file pleine jette l'élément et le compte : la frame n'attend jamais.
//
// Le transport ne connaît que des octets : le codec (core/protocol.h) et la session (sync/)
// restent hors de net/. Pas d'appel au jeu ni au log ici ; aucune exception ne sort de net/.
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "net/udp_socket.h"

namespace mp::net {

inline constexpr std::size_t kQueueCapacity = 4096;  // éléments par file
inline constexpr int kKcpMtu = 1199;                 // + 1 octet de canal = 1200

using Bytes = std::vector<std::uint8_t>;

// --- Commandes (frame → thread réseau) ---------------------------------------------

// Datagramme envoyé tel quel ; le codec y a déjà mis l'octet de canal 2.
struct SendControl {
    Endpoint destination;
    Bytes bytes;
};
// Crée le KCP vers `peer` (après le Welcome, AD-5). Remplace un KCP déjà ouvert.
struct OpenReliable {
    std::uint32_t conv = 0;
    Endpoint peer;
};
// Un message fiable `[MsgType][charge]`, confié à ikcp_send.
struct SendReliable {
    Bytes bytes;
};
// Détruit le KCP (les segments non acquittés sont perdus).
struct CloseReliable {};

// Snapshot : datagramme complet (octet de canal 1 compris), envoyé au pair du KCP.
struct SendSnapshot {
    Bytes bytes;
};

using Command = std::variant<SendControl, OpenReliable, SendReliable, CloseReliable, SendSnapshot>;

// --- Événements (thread réseau → frame) --------------------------------------------

// Datagramme de canal 2, octets complets (octet de canal compris).
struct ControlReceived {
    Endpoint source;
    Bytes bytes;
};
// Message rendu par ikcp_recv : `[MsgType][charge]`.
struct ReliableReceived {
    Bytes bytes;
};

// Datagramme de canal 1 venu du pair du KCP, octets complets (octet de canal compris).
struct SnapshotReceived {
    Bytes bytes;
};

using Event = std::variant<ControlReceived, ReliableReceived, SnapshotReceived>;

// --- Options et compteurs ----------------------------------------------------------

struct Options {
    // Test seulement : probabilité de jeter chaque datagramme à l'envoi (0 = aucune perte).
    double drop_rate = 0.0;
    std::uint32_t drop_seed = 12345;  // graine fixe : pertes reproductibles
};

struct Stats {
    std::uint64_t datagrams_sent = 0;      // remis au socket
    std::uint64_t datagrams_received = 0;  // lus sur le socket (tous canaux)
    std::uint64_t max_datagram_sent = 0;   // plus grande taille d'envoi tentée (octets)
    std::uint64_t dropped_simulated = 0;   // jetés par drop_rate
    std::uint64_t too_big = 0;             // envois de plus de 1200 octets refusés
    std::uint64_t send_errors = 0;         // échecs de sendto
    std::uint64_t recv_errors = 0;         // échecs de recvfrom (datagramme perdu)
    std::uint64_t rx_too_big = 0;          // datagrammes reçus de plus de 1200 octets (jetés)
    std::uint64_t ignored_empty = 0;       // datagramme vide
    std::uint64_t ignored_snapshot = 0;    // canal 1 sans KCP ouvert ou venant d'une autre adresse
    std::uint64_t snapshots_sent = 0;      // SendSnapshot remis au socket
    std::uint64_t snapshots_received = 0;  // canal 1 venant du pair, rendus à la frame
    std::uint64_t snapshot_without_peer = 0;  // SendSnapshot sans KCP ouvert
    std::uint64_t ignored_unknown_channel = 0;
    std::uint64_t ignored_no_reliable = 0;  // canal 0 sans KCP ouvert
    std::uint64_t ignored_wrong_peer = 0;   // canal 0 venant d'une autre adresse que le pair
    std::uint64_t ignored_wrong_conv = 0;   // canal 0 portant un autre conv (ou trop court)
    std::uint64_t kcp_input_errors = 0;     // segment refusé par ikcp_input
    std::uint64_t reliable_without_kcp = 0;  // SendReliable sans KCP ouvert
    std::uint64_t reliable_send_errors = 0;  // ikcp_send refusé (message vide ou trop gros)
    std::uint64_t command_queue_full = 0;    // commandes jetées (file sortante pleine)
    std::uint64_t event_queue_full = 0;      // événements de contrôle jetés (file entrante pleine) ;
                                             // les messages fiables restent dans KCP
    std::uint64_t internal_errors = 0;       // allocation impossible (commande ou KCP perdus)
};

class Transport {
public:
    Transport();
    ~Transport();  // appelle stop()
    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;

    // Lie le socket sur 0.0.0.0:`port` (0 = port éphémère) et lance le thread réseau.
    // Vide les deux files (les commandes postées transport arrêté sont jetées sans être
    // comptées) et remet les compteurs à zéro. En cas d'échec (déjà démarré,
    // port occupé, socket, création du thread), retourne false avec `error` renseigné,
    // et aucun thread n'est lancé.
    bool start(std::uint16_t port, const Options& options, std::string& error);
    bool start(std::uint16_t port, std::string& error) { return start(port, Options{}, error); }

    // Drapeau d'arrêt, join, puis socket fermé ; le port peut être relié aussitôt.
    // Sans effet si le transport n'est pas démarré. Les événements reçus restent lisibles.
    void stop();

    bool running() const { return running_; }
    std::uint16_t local_port() const { return local_port_.load(); }

    // Dépose une commande pour le thread réseau. Acceptée aussi transport arrêté, mais
    // start() vide la file : seules comptent les commandes postées après start().
    // false si la file est pleine : la commande est jetée et comptée.
    bool post(Command command);

    // Retire le plus ancien événement reçu. false si la file est vide.
    bool poll(Event& event);

    // Copie instantanée des compteurs.
    Stats stats() const;

    // Commandes en attente dans la file sortante (diagnostic, tests).
    std::size_t pending_commands() const;

    // Rappel de sortie de KCP (usage interne, appelé depuis le thread réseau).
    int on_kcp_output(const char* segment, int size);

private:
    struct Counters;
    struct Reliable;

    void run();
    void loop_once();
    void receive_all();
    void handle_datagram(const Endpoint& from, const std::uint8_t* data, std::size_t size);
    void execute(Command& command);
    void open_reliable(std::uint32_t conv, const Endpoint& peer);
    void close_reliable();
    void drain_reliable();
    void send_datagram(const Endpoint& to, const std::uint8_t* data, std::size_t size);
    void push_event(Event event);
    std::uint32_t now_ms() const;

    // Partagé avec la frame.
    mutable std::mutex commands_mutex_;
    std::deque<Command> commands_;
    mutable std::mutex events_mutex_;
    std::deque<Event> events_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<std::uint16_t> local_port_{0};
    std::unique_ptr<Counters> counters_;  // atomiques

    // Côté frame uniquement.
    bool running_ = false;
    std::thread thread_;

    // Thread réseau uniquement (entre start et stop).
    UdpSocket socket_;
    Options options_;
    std::unique_ptr<Reliable> reliable_;  // KCP ouvert, ou nul
    std::uint64_t rng_state_ = 0;
    std::chrono::steady_clock::time_point clock_origin_;
};

}  // namespace mp::net
