// 카메라·지도 JPEG 를 별도 스레드에서 풀고(stb_image), 화면 스레드에서 D3D11 텍스처로 올린다.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
struct ID3D11ShaderResourceView;

namespace hg {

// source() 가 (jpeg, 받은 시각, 번호)를 주면 새 번호일 때만 푼다
class JpegDecoder {
public:
    using Source = std::function<bool(std::string& jpeg, double& rx, uint64_t& seq)>;
    explicit JpegDecoder(Source src, int max_fps = 60) : src_(std::move(src)), max_fps_(max_fps) {}
    ~JpegDecoder() { stop(); }
    void start();
    void stop();
    // 새로 풀린 이미지가 있으면 RGBA 를 넘겨받는다
    bool take(std::vector<uint8_t>& rgba, int& w, int& h, double& rx, uint64_t& seq);
    double decode_ms() const { return decode_ms_; }

private:
    void loop();
    Source src_;
    int max_fps_;
    std::thread th_;
    std::atomic<bool> run_{false};
    std::mutex mu_;
    std::vector<uint8_t> rgba_;
    int w_ = 0, h_ = 0;
    double rx_ = 0;
    uint64_t seq_ = 0, done_seq_ = 0;
    bool fresh_ = false;
    std::atomic<double> decode_ms_{0};
};

class Texture {
public:
    ~Texture() { release(); }
    bool update(ID3D11Device* dev, ID3D11DeviceContext* ctx, const uint8_t* rgba, int w, int h);
    void release();
    ID3D11ShaderResourceView* srv() const { return srv_; }
    int width() const { return w_; }
    int height() const { return h_; }
    bool valid() const { return srv_ != nullptr; }
    double rx_time = 0;  // 이 이미지를 받은 시각
    uint64_t seq = 0;

private:
    ID3D11Texture2D* tex_ = nullptr;
    ID3D11ShaderResourceView* srv_ = nullptr;
    int w_ = 0, h_ = 0;
};

}  // namespace hg
