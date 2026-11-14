#include "image_decode.hpp"

#include "util.hpp"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_MAX_DIMENSIONS 4096  // 받은 이미지가 터무니없이 크면 풀지 않는다(메모리 보호)
#define STBI_NO_STDIO
#include "../third_party/stb_image.h"

namespace hg {

bool decode_image(const std::string& bytes, int channels, std::vector<uint8_t>& out, int& w, int& h) {
    if (bytes.size() < 8 || bytes.size() > 16 * 1024 * 1024) return false;
    int n = 0;
    unsigned char* px = stbi_load_from_memory(reinterpret_cast<const unsigned char*>(bytes.data()), static_cast<int>(bytes.size()),
                                              &w, &h, &n, channels);
    if (!px) return false;
    out.assign(px, px + static_cast<size_t>(w) * h * channels);
    stbi_image_free(px);
    return true;
}

bool rosbridge_png_to_text(const std::string& b64, std::string& text) {
    std::string png;
    if (!base64_decode(b64, png)) return false;
    std::vector<uint8_t> rgb;
    int w = 0, h = 0;
    if (!decode_image(png, 3, rgb, w, h)) return false;
    text.assign(reinterpret_cast<const char*>(rgb.data()), rgb.size());
    while (!text.empty() && (text.back() == '\n' || text.back() == '\0')) text.pop_back();
    return true;
}

}  // namespace hg
