#pragma once

// Platform socket headers + small helpers shared by the public API and
// the core implementation. Keep Winsock's min/max macros off the table
// so <algorithm> still works after this include.

#include <cstddef>
#include <limits>

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>

#include <BaseTsd.h>

#if !defined(SPAZNET_SSIZE_T_DEFINED)
#define SPAZNET_SSIZE_T_DEFINED
using ssize_t = SSIZE_T;
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#else // !_WIN32

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>

#endif // _WIN32

namespace spaznet::detail {

inline auto last_socket_error() -> int {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

inline auto is_retryable_socket_error(int err) -> bool {
#ifdef _WIN32
    return err == WSAEWOULDBLOCK || err == WSAEINTR || err == WSAEINPROGRESS;
#else
    return err == EAGAIN || err == EWOULDBLOCK || err == EINTR;
#endif
}

inline auto close_socket_fd(int fd) -> void {
    if (fd < 0) {
        return;
    }
#ifdef _WIN32
    closesocket(static_cast<SOCKET>(fd));
#else
    ::close(fd);
#endif
}

inline auto set_nonblocking(int fd) -> bool {
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(static_cast<SOCKET>(fd), FIONBIO, &mode) == 0;
#else
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        return false;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
#endif
}

// send/recv length argument is `int` on Winsock and `size_t` on POSIX.
inline auto socket_send(int fd, const void* buf, std::size_t len, int flags) -> ssize_t {
#ifdef _WIN32
    if (len > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        len = static_cast<std::size_t>((std::numeric_limits<int>::max)());
    }
    return ::send(static_cast<SOCKET>(fd), static_cast<const char*>(buf), static_cast<int>(len),
                  flags);
#else
    return ::send(fd, buf, len, flags);
#endif
}

inline auto socket_recv(int fd, void* buf, std::size_t len, int flags) -> ssize_t {
#ifdef _WIN32
    if (len > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        len = static_cast<std::size_t>((std::numeric_limits<int>::max)());
    }
    return ::recv(static_cast<SOCKET>(fd), static_cast<char*>(buf), static_cast<int>(len), flags);
#else
    return ::recv(fd, buf, len, flags);
#endif
}

inline auto socket_recvfrom(int fd, void* buf, std::size_t len, int flags, sockaddr* addr,
                            socklen_t* addr_len) -> ssize_t {
#ifdef _WIN32
    if (len > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        len = static_cast<std::size_t>((std::numeric_limits<int>::max)());
    }
    return ::recvfrom(static_cast<SOCKET>(fd), static_cast<char*>(buf), static_cast<int>(len), flags,
                      addr, addr_len);
#else
    return ::recvfrom(fd, buf, len, flags, addr, addr_len);
#endif
}

inline auto setsockopt_int(int fd, int level, int optname, int value) -> int {
#ifdef _WIN32
    return ::setsockopt(static_cast<SOCKET>(fd), level, optname,
                        reinterpret_cast<const char*>(&value), sizeof(value));
#else
    return ::setsockopt(fd, level, optname, &value, sizeof(value));
#endif
}

} // namespace spaznet::detail
