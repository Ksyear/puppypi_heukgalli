// 로봇 찾기(UDP 5002): PUPPYPI_DISCOVER/1|<nonce 32hex> → PUPPYPI_HERE/1|<nonce>|<id 32hex>|<이름 URL 인코딩>|<조종 포트>|2
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace hg {

struct FoundRobot {
    std::string ip;
    std::string name;
    std::string id;
    uint16_t control_port = 5001;
};

// 응답 한 개를 검사·해석. nonce 가 다르거나 형식이 틀리면 false
bool parse_discovery_reply(const std::string& msg, const std::string& nonce, FoundRobot& out);
std::string url_decode(const std::string& s);

// 모든 IPv4 인터페이스의 브로드캐스트 주소 + 255.255.255.255 + extra(유니캐스트 주소)로 보내고 timeout 동안 모은다.
std::vector<FoundRobot> discover_robots(int timeout_ms, const std::vector<std::string>& extra = {}, uint16_t port = 5002);

// 이 기기의 IPv4 주소(루프백 제외) — 로봇과 같은 서브넷인지 안내할 때 쓴다
std::vector<std::string> local_ipv4_addresses();

}  // namespace hg
