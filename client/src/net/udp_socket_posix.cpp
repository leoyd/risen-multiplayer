// Socket UDP — implémentation POSIX (Linux : tests en boucle locale).
#include "net/udp_socket.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace mp::net {

namespace {

sockaddr_in to_sockaddr(const Endpoint& e) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(e.ipv4);
    a.sin_port = htons(e.port);
    return a;
}

std::string errno_text(const char* what) {
    return std::string(what) + " : " + std::strerror(errno) + " (errno " + std::to_string(errno) + ")";
}

}  // namespace

bool UdpSocket::open(std::uint16_t port, std::string& error) {
    close();
    handle_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_UDP);
    if (handle_ < 0) {
        error = errno_text("socket");
        handle_ = -1;
        return false;
    }
    // Pas de SO_REUSEADDR : un port déjà lié doit être refusé.
    sockaddr_in a = to_sockaddr(Endpoint{INADDR_ANY, port});
    if (::bind(handle_, reinterpret_cast<const sockaddr*>(&a), sizeof(a)) != 0) {
        error = errno_text(("bind sur le port " + std::to_string(port)).c_str());
        close();
        return false;
    }
    sockaddr_in bound{};
    socklen_t len = sizeof(bound);
    if (::getsockname(handle_, reinterpret_cast<sockaddr*>(&bound), &len) != 0) {
        error = errno_text("getsockname");
        close();
        return false;
    }
    local_port_ = ntohs(bound.sin_port);
    return true;
}

void UdpSocket::close() {
    if (handle_ >= 0) ::close(handle_);
    handle_ = -1;
    local_port_ = 0;
}

bool UdpSocket::is_open() const { return handle_ >= 0; }

bool UdpSocket::send_to(const Endpoint& to, const std::uint8_t* data, std::size_t size) {
    if (handle_ < 0) return false;
    sockaddr_in a = to_sockaddr(to);
    const ssize_t n = ::sendto(handle_, data, size, 0, reinterpret_cast<const sockaddr*>(&a), sizeof(a));
    return n == static_cast<ssize_t>(size);
}

RecvStatus UdpSocket::recv_from(std::uint8_t* buf, std::size_t capacity, std::size_t& size, Endpoint& from) {
    size = 0;
    if (handle_ < 0) return RecvStatus::Error;
    sockaddr_in a{};
    socklen_t len = sizeof(a);
    // MSG_TRUNC : rend la taille réelle du datagramme, même s'il dépasse le tampon.
    const ssize_t n = ::recvfrom(handle_, buf, capacity, MSG_TRUNC, reinterpret_cast<sockaddr*>(&a), &len);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return RecvStatus::Empty;
        return RecvStatus::Error;
    }
    from.ipv4 = ntohl(a.sin_addr.s_addr);
    from.port = ntohs(a.sin_port);
    if (static_cast<std::size_t>(n) > capacity) {
        size = capacity;
        return RecvStatus::Truncated;
    }
    size = static_cast<std::size_t>(n);
    return RecvStatus::Ok;
}

bool UdpSocket::wait_readable(int timeout_ms) {
    if (handle_ < 0) return false;
    pollfd p{};
    p.fd = handle_;
    p.events = POLLIN;
    return ::poll(&p, 1, timeout_ms < 0 ? 0 : timeout_ms) > 0 && (p.revents & POLLIN) != 0;
}

}  // namespace mp::net
