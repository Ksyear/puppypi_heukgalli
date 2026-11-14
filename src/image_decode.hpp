// JPEG·PNG 풀기(stb_image). 카메라·지도 JPEG, rosbridge 의 PNG 압축 메시지에 쓴다.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace hg {

// channels: 결과 채널 수(4 = RGBA, 3 = RGB). 크기가 4096 을 넘으면 실패
bool decode_image(const std::string& bytes, int channels, std::vector<uint8_t>& out, int& w, int& h);

// rosbridge "png" 압축(문자열을 RGB 픽셀에 담은 PNG, base64)을 원래 JSON 문자열로 되돌린다.
// 끝에 채워 넣은 '\n' 은 지운다.
bool rosbridge_png_to_text(const std::string& b64, std::string& text);

}  // namespace hg
