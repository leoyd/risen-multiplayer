// Socket UDP — implémentation Winsock (DLL du mod, net_tests.exe sous Wine).
#include "net/udp_socket.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>  // SIO_UDP_CONNRESET (macro seule, pas de lien avec mswsock.lib)

namespace mp::net {

namespace {

sockaddr_in to_sockaddr(const Endpoint& e) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(e.ipv4);
    a.sin_port = htons(e.port);
    return a;
}

std::string wsa_text(const char* what, int code) {
    return std::string(what) + " : erreur Winsock " + std::to_string(code);
}

SOCKET as_socket(std::uintptr_t h) { return static_cast<SOCKET>(h); }

}  // namespace

bool UdpSocket::open(std::uint16_t port, std::string& error) {
    close();
    // WSAStartup/WSACleanup sont comptés par Winsock : un appel par socket ouvert.
    WSADATA wsa{};
    const int rc = ::WSAStartup(MAKEWORD(2, 2), &wsa);
    if (rc != 0) {
        error = wsa_text("WSAStartup", rc);
        return false;
    }
    wsa_started_ = true;

    const SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        error = wsa_text("socket", ::WSAGetLastError());
        close();
        return false;
    }
    handle_ = static_cast<std::uintptr_t>(s);

    // Sans cela, un ICMP « port injoignable » reçu après un envoi fait échouer le
    // recvfrom suivant (WSAECONNRESET) sur un socket UDP non connecté.
    BOOL connreset = FALSE;
    DWORD returned = 0;
    ::WSAIoctl(s, SIO_UDP_CONNRESET, &connreset, sizeof(connreset), nullptr, 0, &returned, nullptr,
               nullptr);

    u_long nonblocking = 1;
    if (::ioctlsocket(s, FIONBIO, &nonblocking) != 0) {
        error = wsa_text("ioctlsocket(FIONBIO)", ::WSAGetLastError());
        close();
        return false;
    }
    // Pas de SO_REUSEADDR : un port déjà lié doit être refusé.
    sockaddr_in a = to_sockaddr(Endpoint{INADDR_ANY, port});
    if (::bind(s, reinterpret_cast<const sockaddr*>(&a), sizeof(a)) != 0) {
        error = wsa_text(("bind sur le port " + std::to_string(port)).c_str(), ::WSAGetLastError());
        close();
        return false;
    }
    sockaddr_in bound{};
    int len = sizeof(bound);
    if (::getsockname(s, reinterpret_cast<sockaddr*>(&bound), &len) != 0) {
        error = wsa_text("getsockname", ::WSAGetLastError());
        close();
        return false;
    }
    local_port_ = ntohs(bound.sin_port);
    return true;
}

void UdpSocket::close() {
    if (is_open()) ::closesocket(as_socket(handle_));
    handle_ = static_cast<std::uintptr_t>(INVALID_SOCKET);
    local_port_ = 0;
    if (wsa_started_) {
        ::WSACleanup();
        wsa_started_ = false;
    }
}

bool UdpSocket::is_open() const { return as_socket(handle_) != INVALID_SOCKET; }

bool UdpSocket::send_to(const Endpoint& to, const std::uint8_t* data, std::size_t size) {
    if (!is_open()) return false;
    sockaddr_in a = to_sockaddr(to);
    const int n = ::sendto(as_socket(handle_), reinterpret_cast<const char*>(data), static_cast<int>(size), 0,
                           reinterpret_cast<const sockaddr*>(&a), sizeof(a));
    return n == static_cast<int>(size);
}

RecvStatus UdpSocket::recv_from(std::uint8_t* buf, std::size_t capacity, std::size_t& size, Endpoint& from) {
    size = 0;
    if (!is_open()) return RecvStatus::Error;
    sockaddr_in a{};
    int len = sizeof(a);
    const int n = ::recvfrom(as_socket(handle_), reinterpret_cast<char*>(buf), static_cast<int>(capacity), 0,
                             reinterpret_cast<sockaddr*>(&a), &len);
    if (n == SOCKET_ERROR) {
        const int code = ::WSAGetLastError();
        if (code == WSAEWOULDBLOCK) return RecvStatus::Empty;
        if (code == WSAEMSGSIZE) {  // datagramme plus grand que le tampon : tronqué
            from.ipv4 = ntohl(a.sin_addr.s_addr);
            from.port = ntohs(a.sin_port);
            size = capacity;
            return RecvStatus::Truncated;
        }
        return RecvStatus::Error;
    }
    from.ipv4 = ntohl(a.sin_addr.s_addr);
    from.port = ntohs(a.sin_port);
    size = static_cast<std::size_t>(n);
    return RecvStatus::Ok;
}

bool UdpSocket::wait_readable(int timeout_ms) {
    if (!is_open()) return false;
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(as_socket(handle_), &readable);
    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = (timeout_ms < 0 ? 0 : timeout_ms) * 1000;
    return ::select(0, &readable, nullptr, nullptr, &tv) > 0;
}

}  // namespace mp::net
