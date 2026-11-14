#include "discovery.hpp"

#include "net.hpp"
#include "sock.hpp"
#include "util.hpp"

#include <cctype>
#include <set>

namespace hg {

namespace {
bool is_hex32(const std::string& s) {
    if (s.size() != 32) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

struct Iface {
    uint32_t addr, mask;  // 호스트 바이트 순서
};

std::vector<Iface> ipv4_interfaces() {
    std::vector<Iface> out;
#ifdef _WIN32
    ULONG size = 16 * 1024;
    std::string buf(size, '\0');
    auto* aa = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(&buf[0]);
    ULONG rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                    nullptr, aa, &size);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buf.assign(size, '\0');
        aa = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(&buf[0]);
        rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                  nullptr, aa, &size);
    }
    if (rc != NO_ERROR) return out;
    for (auto* a = aa; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            auto* sin = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
            if (sin->sin_family != AF_INET) continue;
            uint32_t ip = ntohl(sin->sin_addr.s_addr);
            int bits = u->OnLinkPrefixLength;
            uint32_t mask = bits <= 0 ? 0 : (bits >= 32 ? 0xFFFFFFFFu : ~((1u << (32 - bits)) - 1));
            out.push_back({ip, mask});
        }
    }
#else
    ifaddrs* ifs = nullptr;
    if (getifaddrs(&ifs) != 0) return out;
    for (auto* i = ifs; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK))
            continue;
        uint32_t ip = ntohl(reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr.s_addr);
        uint32_t mask = i->ifa_netmask ? ntohl(reinterpret_cast<sockaddr_in*>(i->ifa_netmask)->sin_addr.s_addr) : 0;
        out.push_back({ip, mask});
    }
    freeifaddrs(ifs);
#endif
    return out;
}

std::string ip_text(uint32_t host_order) {
    in_addr a{};
    a.s_addr = htonl(host_order);
    char buf[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &a, buf, sizeof buf);
    return buf;
}
}  // namespace

std::string url_decode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

bool parse_discovery_reply(const std::string& msg, const std::string& nonce, FoundRobot& out) {
    auto f = split(msg, '|');
    if (f.size() != 6 || f[0] != "PUPPYPI_HERE/1" || f[1] != nonce || !is_hex32(f[2]) || f[5] != "2") return false;
    int port = std::atoi(f[4].c_str());
    if (port < 1 || port > 65535) return false;
    out.id = f[2];
    out.name.clear();
    for (unsigned char c : url_decode(f[3]))  // 제어 문자는 버리고(로봇 쪽 규칙과 같게) 32 바이트까지
        if (c >= 0x20 && c != 0x7F && out.name.size() < 32) out.name += static_cast<char>(c);
    if (out.name.empty()) out.name = "PuppyPi";
    out.control_port = static_cast<uint16_t>(port);
    return true;
}

std::vector<std::string> local_ipv4_addresses() {
    std::vector<std::string> out;
    for (const auto& i : ipv4_interfaces()) out.push_back(ip_text(i.addr));
    return out;
}

std::vector<FoundRobot> discover_robots(int timeout_ms, const std::vector<std::string>& extra, uint16_t port) {
    std::vector<FoundRobot> found;
    sock_t s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == kBadSock) return found;
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&yes), sizeof yes);
    std::string nonce = random_hex(32);
    std::string req = "PUPPYPI_DISCOVER/1|" + nonce;

    std::set<std::string> targets = {"255.255.255.255"};
    for (const auto& i : ipv4_interfaces())
        if (i.mask && i.mask != 0xFFFFFFFFu) targets.insert(ip_text((i.addr & i.mask) | ~i.mask));
    for (const auto& e : extra) targets.insert(e);
    for (const auto& t : targets) {
        sockaddr_in a{};
        if (!resolve_ipv4(t, port, a)) continue;
        sendto(s, req.data(), static_cast<int>(req.size()), 0, reinterpret_cast<sockaddr*>(&a), sizeof a);
    }

    std::set<std::string> seen;
    double deadline = now_s() + timeout_ms / 1000.0;
    std::string buf(1500, '\0');
    while (true) {
        int left = static_cast<int>((deadline - now_s()) * 1000);
        if (left <= 0) break;
        sock_pollfd p{};
        p.fd = s;
        p.events = POLLIN;
        if (sock_poll(&p, 1, left) <= 0) break;
        sockaddr_in from{};
        socklen_t flen = sizeof from;
        auto n = recvfrom(s, &buf[0], static_cast<int>(buf.size()), 0, reinterpret_cast<sockaddr*>(&from), &flen);
        if (n <= 0) continue;
        FoundRobot r;
        if (!parse_discovery_reply(std::string(buf.data(), static_cast<size_t>(n)), nonce, r)) continue;
        r.ip = addr_text(from).substr(0, addr_text(from).rfind(':'));
        if (seen.insert(r.id + "@" + r.ip).second) found.push_back(r);
    }
    sock_close(s);
    return found;
}

}  // namespace hg
