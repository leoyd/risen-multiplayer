// Socket UDP IPv4 non bloquant minimal. Seul code de net/ qui dépend de la plateforme
// (udp_socket_posix.cpp sous Linux, udp_socket_win.cpp sous Windows).
//
// Aucune exception, aucun log : chaque opération retourne un statut, et l'ouverture
// un message d'erreur. Un socket n'est utilisé que par un seul thread à la fois.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace mp::net {

// Adresse IPv4 + port, en ordre hôte (127.0.0.1 = 0x7f000001).
struct Endpoint {
    std::uint32_t ipv4 = 0;
    std::uint16_t port = 0;

    friend bool operator==(const Endpoint& a, const Endpoint& b) {
        return a.ipv4 == b.ipv4 && a.port == b.port;
    }
    friend bool operator!=(const Endpoint& a, const Endpoint& b) { return !(a == b); }
};

// Lit une adresse IPv4 en notation pointée stricte (« 192.168.1.20 » : 4 nombres décimaux
// de 0 à 255, sans espace ni zéro de tête superflu). Pas de résolution de nom (story 1.4).
// Retourne false sans toucher à `out` si le texte est invalide.
inline bool parse_ipv4(const std::string& text, std::uint32_t& out) {
    std::uint32_t value = 0;
    std::size_t i = 0;
    for (int part = 0; part < 4; ++part) {
        if (part > 0) {
            if (i >= text.size() || text[i] != '.') return false;
            ++i;
        }
        const std::size_t start = i;
        std::uint32_t n = 0;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9' && i - start < 3) {
            n = n * 10 + static_cast<std::uint32_t>(text[i] - '0');
            ++i;
        }
        const std::size_t digits = i - start;
        if (digits == 0 || n > 255 || (digits > 1 && text[start] == '0')) return false;
        value = (value << 8) | n;
    }
    if (i != text.size()) return false;
    out = value;
    return true;
}

// « a.b.c.d:port », pour les messages et les tests.
inline std::string to_string(const Endpoint& e) {
    std::string s;
    for (int shift = 24; shift >= 0; shift -= 8) {
        s += std::to_string((e.ipv4 >> shift) & 0xffu);
        s += shift > 0 ? '.' : ':';
    }
    s += std::to_string(e.port);
    return s;
}

enum class RecvStatus {
    Ok,         // un datagramme a été lu
    Empty,      // rien à lire (le socket aurait bloqué)
    Truncated,  // datagramme plus grand que le tampon : lu tronqué, à jeter
    Error,      // erreur passagère (ex. WSAECONNRESET résiduel) : le datagramme est perdu
};

class UdpSocket {
public:
    UdpSocket() = default;
    ~UdpSocket() { close(); }
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    // Ouvre le socket, le rend non bloquant et le lie à 0.0.0.0:`port` (0 = port éphémère).
    // En cas d'échec, `error` reçoit la raison et le socket reste fermé.
    bool open(std::uint16_t port, std::string& error);
    void close();
    bool is_open() const;

    // Port effectivement lié (utile avec le port 0), 0 si fermé.
    std::uint16_t local_port() const { return local_port_; }

    // Envoie un datagramme complet. false si l'envoi a échoué (y compris tampon plein).
    bool send_to(const Endpoint& to, const std::uint8_t* data, std::size_t size);

    // Lit au plus un datagramme dans `buf` (non bloquant). `size` reçoit sa taille.
    RecvStatus recv_from(std::uint8_t* buf, std::size_t capacity, std::size_t& size, Endpoint& from);

    // Attend au plus `timeout_ms` que le socket soit lisible. true s'il l'est.
    bool wait_readable(int timeout_ms);

private:
#if defined(_WIN32)
    std::uintptr_t handle_ = ~static_cast<std::uintptr_t>(0);  // SOCKET (INVALID_SOCKET)
    bool wsa_started_ = false;
#else
    int handle_ = -1;
#endif
    std::uint16_t local_port_ = 0;
};

}  // namespace mp::net
