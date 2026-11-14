#include "textures.hpp"

#include "util.hpp"

#include <d3d11.h>

#include <chrono>
#include <cstring>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include "stb_image.h"

namespace hg {

void JpegDecoder::start() {
    if (run_) return;
    run_ = true;
    th_ = std::thread([this] { loop(); });
}

void JpegDecoder::stop() {
    run_ = false;
    if (th_.joinable()) th_.join();
}

void JpegDecoder::loop() {
    std::string jpeg;
    double min_gap = 1.0 / max_fps_, last = 0;
    while (run_) {
        double rx = 0;
        uint64_t seq = 0;
        if (now_s() - last >= min_gap && src_(jpeg, rx, seq) && seq != done_seq_) {
            double t0 = now_s();
            int w = 0, h = 0, n = 0;
            unsigned char* px = stbi_load_from_memory(reinterpret_cast<const unsigned char*>(jpeg.data()),
                                                      static_cast<int>(jpeg.size()), &w, &h, &n, 4);
            done_seq_ = seq;
            last = t0;
            if (px) {
                std::lock_guard<std::mutex> lk(mu_);
                rgba_.assign(px, px + static_cast<size_t>(w) * h * 4);
                w_ = w;
                h_ = h;
                rx_ = rx;
                seq_ = seq;
                fresh_ = true;
                stbi_image_free(px);
            }
            decode_ms_ = (now_s() - t0) * 1000.0;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
    }
}

bool JpegDecoder::take(std::vector<uint8_t>& rgba, int& w, int& h, double& rx, uint64_t& seq) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!fresh_) return false;
    rgba.swap(rgba_);
    w = w_;
    h = h_;
    rx = rx_;
    seq = seq_;
    fresh_ = false;
    return true;
}

bool Texture::update(ID3D11Device* dev, ID3D11DeviceContext* ctx, const uint8_t* rgba, int w, int h) {
    if (w <= 0 || h <= 0) return false;
    if (!tex_ || w != w_ || h != h_) {
        release();
        D3D11_TEXTURE2D_DESC d{};
        d.Width = static_cast<UINT>(w);
        d.Height = static_cast<UINT>(h);
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DYNAMIC;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(dev->CreateTexture2D(&d, nullptr, &tex_))) return false;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = d.Format;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        if (FAILED(dev->CreateShaderResourceView(tex_, &sd, &srv_))) {
            release();
            return false;
        }
        w_ = w;
        h_ = h;
    }
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(tex_, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
    for (int y = 0; y < h; ++y)
        std::memcpy(static_cast<uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch, rgba + static_cast<size_t>(y) * w * 4,
                    static_cast<size_t>(w) * 4);
    ctx->Unmap(tex_, 0);
    return true;
}

void Texture::release() {
    if (srv_) srv_->Release();
    if (tex_) tex_->Release();
    srv_ = nullptr;
    tex_ = nullptr;
    w_ = h_ = 0;
}

}  // namespace hg
