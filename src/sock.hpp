// Windows(Winsock)와 POSIX 소켓 차이를 덮는 얇은 층. 흑갈리(Windows)용이 본 대상이고, 맥·리눅스는 가짜 로봇 시험용.
#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
using sock_t = SOCKET;
const sock_t kBadSock = INVALID_SOCKET;
inline int sock_close(sock_t s) { return closesocket(s); }
inline int sock_poll(WSAPOLLFD* p, unsigned long n, int timeout_ms) { return WSAPoll(p, n, timeout_ms); }
using sock_pollfd = WSAPOLLFD;
inline int sock_err() { return WSAGetLastError(); }
inline bool sock_would_block(int e) { return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS; }
inline bool sock_interrupted(int e) { return e == WSAEINTR; }
#ifndef SHUT_RDWR
#define SHUT_RDWR SD_BOTH
#endif
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
using sock_t = int;
const sock_t kBadSock = -1;
inline int sock_close(sock_t s) { return ::close(s); }
using sock_pollfd = pollfd;
inline int sock_poll(pollfd* p, unsigned long n, int timeout_ms) { return ::poll(p, static_cast<nfds_t>(n), timeout_ms); }
inline int sock_err() { return errno; }
inline bool sock_would_block(int e) { return e == EINPROGRESS || e == EWOULDBLOCK || e == EAGAIN; }
inline bool sock_interrupted(int e) { return e == EINTR; }
#endif

#include <string>

namespace hg {

bool sock_init();  // Windows 는 WSAStartup, 나머지는 SIGPIPE 무시
void sock_set_nonblock(sock_t s, bool on);
void sock_set_recv_timeout(sock_t s, int timeout_ms);
std::string sock_err_text(int e);

}  // namespace hg
