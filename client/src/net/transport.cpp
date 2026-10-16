#include "net/transport.h"

#include <algorithm>
#include <climits>
#include <new>
#include <utility>

#include "core/protocol.h"
#include "ikcp.h"

namespace mp::net {

namespace {

using protocol::Channel;
using protocol::kMaxDatagram;

constexpr int kMaxWaitMs = 5;               // attente bornée de lisibilité du socket
constexpr int kMaxReceivesPerLoop = 1024;   // la boucle reprend la main régulièrement
constexpr std::size_t kKcpHeader = 24;      // IKCP_OVERHEAD : en-tête d'un segment KCP

static_assert(kKcpMtu + 1 == static_cast<int>(kMaxDatagram), "segment KCP + canal = 1200 octets");

int kcp_output(const char* buf, int len, ikcpcb* /*kcp*/, void* user) {
    return static_cast<Transport*>(user)->on_kcp_output(buf, len);
}

// splitmix64 : générateur portable (mêmes pertes sous Linux et sous Wine pour une graine).
std::uint64_t next_random(std::uint64_t& state) {
    std::uint64_t z = (state += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

}  // namespace

// Liste unique des compteurs : déclaration atomique, remise à zéro, copie vers Stats.
#define MP_NET_COUNTERS(X)                                                                  \
    X(datagrams_sent) X(datagrams_received) X(max_datagram_sent) X(dropped_simulated)       \
    X(too_big) X(send_errors) X(recv_errors) X(rx_too_big) X(ignored_empty)                 \
    X(ignored_snapshot) X(ignored_unknown_channel) X(ignored_no_reliable)                   \
    X(ignored_wrong_peer) X(ignored_wrong_conv) X(kcp_input_errors) X(reliable_without_kcp) \
    X(reliable_send_errors) X(command_queue_full) X(event_queue_full) X(internal_errors)

struct Transport::Counters {
#define MP_DECLARE(name) std::atomic<std::uint64_t> name{0};
    MP_NET_COUNTERS(MP_DECLARE)
#undef MP_DECLARE

    void reset() {
#define MP_RESET(name) name.store(0, std::memory_order_relaxed);
        MP_NET_COUNTERS(MP_RESET)
#undef MP_RESET
    }

    Stats snapshot() const {
        Stats s;
#define MP_COPY(name) s.name = name.load(std::memory_order_relaxed);
        MP_NET_COUNTERS(MP_COPY)
#undef MP_COPY
        return s;
    }
};

#undef MP_NET_COUNTERS

namespace {
void bump(std::atomic<std::uint64_t>& c) { c.fetch_add(1, std::memory_order_relaxed); }
}  // namespace

// KCP ouvert vers un pair. Détenu et utilisé par le seul thread réseau.
struct Transport::Reliable {
    ikcpcb* kcp = nullptr;
    std::uint32_t conv = 0;
    Endpoint peer;

    ~Reliable() {
        if (kcp != nullptr) ikcp_release(kcp);
    }
};

Transport::Transport() : counters_(new Counters) {}

Transport::~Transport() { stop(); }

bool Transport::start(std::uint16_t port, const Options& options, std::string& error) {
    if (running_) {
        error = "transport déjà démarré";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(commands_mutex_);
        commands_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(events_mutex_);
        events_.clear();
    }
    counters_->reset();
    options_ = options;
    rng_state_ = options.drop_seed;
    clock_origin_ = std::chrono::steady_clock::now();
    stop_requested_.store(false);
    reliable_.reset();

    if (!socket_.open(port, error)) return false;
    local_port_.store(socket_.local_port());

    try {
        thread_ = std::thread(&Transport::run, this);
    } catch (...) {
        socket_.close();
        local_port_.store(0);
        error = "création du thread réseau impossible";
        return false;
    }
    running_ = true;
    return true;
}

void Transport::stop() {
    if (!running_) return;
    stop_requested_.store(true);
    if (thread_.joinable()) thread_.join();
    socket_.close();
    local_port_.store(0);
    running_ = false;
}

bool Transport::post(Command command) {
    try {
        std::lock_guard<std::mutex> lock(commands_mutex_);
        if (commands_.size() >= kQueueCapacity) {
            bump(counters_->command_queue_full);
            return false;
        }
        commands_.push_back(std::move(command));
        return true;
    } catch (...) {  // bad_alloc
        bump(counters_->internal_errors);
        return false;
    }
}

bool Transport::poll(Event& event) {
    std::lock_guard<std::mutex> lock(events_mutex_);
    if (events_.empty()) return false;
    event = std::move(events_.front());
    events_.pop_front();
    return true;
}

Stats Transport::stats() const { return counters_->snapshot(); }

std::size_t Transport::pending_commands() const {
    std::lock_guard<std::mutex> lock(commands_mutex_);
    return commands_.size();
}

// --- Thread réseau -------------------------------------------------------------------

void Transport::run() {
    while (!stop_requested_.load()) {
        try {
            loop_once();
        } catch (...) {  // bad_alloc : on compte et on continue, rien ne sort du thread
            bump(counters_->internal_errors);
        }
    }
    reliable_.reset();
}

std::uint32_t Transport::now_ms() const {
    const auto elapsed = std::chrono::steady_clock::now() - clock_origin_;
    return static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
}

void Transport::loop_once() {
    int wait = kMaxWaitMs;
    if (reliable_) {
        const std::uint32_t now = now_ms();
        const auto until_update = static_cast<std::int32_t>(ikcp_check(reliable_->kcp, now) - now);
        wait = std::clamp(until_update, 0, kMaxWaitMs);
    }
    if (wait > 0 && pending_commands() > 0) wait = 0;
    if (wait > 0) socket_.wait_readable(wait);

    receive_all();

    // Les commandes sont retirées puis exécutées d'un bloc, sans réception entre les deux :
    // une fois la file vue vide par la frame, tout datagramme reçu ensuite l'est après
    // l'exécution de ces commandes.
    std::deque<Command> batch;
    {
        std::lock_guard<std::mutex> lock(commands_mutex_);
        batch.swap(commands_);
    }
    for (Command& command : batch) execute(command);

    if (reliable_) {
        ikcp_update(reliable_->kcp, now_ms());
        drain_reliable();
    }
}

void Transport::receive_all() {
    std::uint8_t buf[kMaxDatagram + 1];  // un octet de plus : détecte un datagramme trop gros
    for (int i = 0; i < kMaxReceivesPerLoop; ++i) {
        std::size_t size = 0;
        Endpoint from;
        const RecvStatus status = socket_.recv_from(buf, sizeof(buf), size, from);
        if (status == RecvStatus::Empty) return;
        if (status == RecvStatus::Error) {
            bump(counters_->recv_errors);
            continue;
        }
        bump(counters_->datagrams_received);
        if (status == RecvStatus::Truncated || size > kMaxDatagram) {
            bump(counters_->rx_too_big);
            continue;
        }
        handle_datagram(from, buf, size);
    }
}

void Transport::handle_datagram(const Endpoint& from, const std::uint8_t* data, std::size_t size) {
    if (size == 0) {
        bump(counters_->ignored_empty);
        return;
    }
    switch (data[0]) {
        case static_cast<std::uint8_t>(Channel::Control):
            push_event(ControlReceived{from, Bytes(data, data + size)});
            return;
        case static_cast<std::uint8_t>(Channel::Snapshot):
            bump(counters_->ignored_snapshot);  // Epic 2
            return;
        case static_cast<std::uint8_t>(Channel::Reliable):
            break;
        default:
            bump(counters_->ignored_unknown_channel);
            return;
    }

    if (!reliable_) {
        bump(counters_->ignored_no_reliable);
        return;
    }
    if (from != reliable_->peer) {
        bump(counters_->ignored_wrong_peer);
        return;
    }
    const char* segment = reinterpret_cast<const char*>(data + 1);
    const std::size_t segment_size = size - 1;
    if (segment_size < kKcpHeader || ikcp_getconv(segment) != reliable_->conv) {
        bump(counters_->ignored_wrong_conv);
        return;
    }
    if (ikcp_input(reliable_->kcp, segment, static_cast<long>(segment_size)) < 0) {
        bump(counters_->kcp_input_errors);
    }
}

void Transport::execute(Command& command) {
    if (auto* c = std::get_if<SendControl>(&command)) {
        send_datagram(c->destination, c->bytes.data(), c->bytes.size());
    } else if (auto* o = std::get_if<OpenReliable>(&command)) {
        open_reliable(o->conv, o->peer);
    } else if (auto* r = std::get_if<SendReliable>(&command)) {
        if (!reliable_) {
            bump(counters_->reliable_without_kcp);
        } else if (r->bytes.empty() || r->bytes.size() > static_cast<std::size_t>(INT_MAX) ||
                   ikcp_send(reliable_->kcp, reinterpret_cast<const char*>(r->bytes.data()),
                             static_cast<int>(r->bytes.size())) < 0) {
            bump(counters_->reliable_send_errors);
        }
    } else if (std::holds_alternative<CloseReliable>(command)) {
        close_reliable();
    }
}

void Transport::open_reliable(std::uint32_t conv, const Endpoint& peer) {
    reliable_.reset();
    auto reliable = std::make_unique<Reliable>();
    reliable->kcp = ikcp_create(conv, this);
    if (reliable->kcp == nullptr) {
        bump(counters_->internal_errors);
        return;
    }
    reliable->conv = conv;
    reliable->peer = peer;
    ikcp_setoutput(reliable->kcp, &kcp_output);
    ikcp_nodelay(reliable->kcp, 1, 10, 2, 1);
    ikcp_setmtu(reliable->kcp, kKcpMtu);
    reliable_ = std::move(reliable);
}

void Transport::close_reliable() { reliable_.reset(); }

void Transport::drain_reliable() {
    for (;;) {
        // File entrante pleine : le message reste dans KCP, dont la fenêtre de réception
        // freine l'émetteur. Rien n'est jamais perdu sur le canal fiable. Seul ce thread
        // remplit la file : une fois la place vue libre, push_event réussira.
        {
            std::lock_guard<std::mutex> lock(events_mutex_);
            if (events_.size() >= kQueueCapacity) return;
        }
        const int size = ikcp_peeksize(reliable_->kcp);
        if (size < 0) return;
        Bytes message(static_cast<std::size_t>(size));
        const int n = ikcp_recv(reliable_->kcp, reinterpret_cast<char*>(message.data()), size);
        if (n < 0) return;
        message.resize(static_cast<std::size_t>(n));
        push_event(ReliableReceived{std::move(message)});
    }
}

int Transport::on_kcp_output(const char* segment, int size) {
    if (!reliable_ || size < 0) return -1;
    std::uint8_t buf[kMaxDatagram + 1];
    if (static_cast<std::size_t>(size) + 1 > sizeof(buf)) {
        bump(counters_->too_big);
        return -1;
    }
    buf[0] = static_cast<std::uint8_t>(Channel::Reliable);
    std::copy(segment, segment + size, buf + 1);
    send_datagram(reliable_->peer, buf, static_cast<std::size_t>(size) + 1);
    return 0;
}

void Transport::send_datagram(const Endpoint& to, const std::uint8_t* data, std::size_t size) {
    if (size > kMaxDatagram) {
        bump(counters_->too_big);
        return;
    }
    // Seul ce thread écrit ce compteur : lecture puis écriture suffisent.
    if (size > counters_->max_datagram_sent.load(std::memory_order_relaxed)) {
        counters_->max_datagram_sent.store(size, std::memory_order_relaxed);
    }
    if (options_.drop_rate > 0.0) {
        const double draw = static_cast<double>(next_random(rng_state_) >> 11) * (1.0 / 9007199254740992.0);
        if (draw < options_.drop_rate) {
            bump(counters_->dropped_simulated);
            return;
        }
    }
    if (socket_.send_to(to, data, size)) {
        bump(counters_->datagrams_sent);
    } else {
        bump(counters_->send_errors);
    }
}

void Transport::push_event(Event event) {
    std::lock_guard<std::mutex> lock(events_mutex_);
    if (events_.size() >= kQueueCapacity) {
        bump(counters_->event_queue_full);
        return;
    }
    events_.push_back(std::move(event));
}

}  // namespace mp::net
