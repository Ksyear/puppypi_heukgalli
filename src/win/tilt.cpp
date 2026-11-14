#include "tilt.hpp"

#include <windows.h>
#include <initguid.h>
#include <propkeydef.h>
#include <sensors.h>
#include <sensorsapi.h>

#include <chrono>
#include <cmath>

namespace hg {

void TiltSensor::start(void* hwnd) {
    if (run_) return;
    run_ = true;
    th_ = std::thread([this, hwnd] { loop(hwnd); });
}

void TiltSensor::stop() {
    run_ = false;
    if (th_.joinable()) th_.join();
    available_ = false;
}

std::string TiltSensor::status() const {
    std::lock_guard<std::mutex> lk(mu_);
    return status_;
}

bool TiltSensor::gravity(float& x, float& y, float& z) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (!have_) return false;
    x = gx_;
    y = gy_;
    z = gz_;
    return true;
}

static double pv_double(const PROPVARIANT& pv) {
    switch (pv.vt) {
        case VT_R8: return pv.dblVal;
        case VT_R4: return pv.fltVal;
        default: return 0.0;
    }
}

void TiltSensor::loop(void* hwnd) {
    auto set_status = [this](const std::string& s) {
        std::lock_guard<std::mutex> lk(mu_);
        status_ = s;
    };
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ISensorManager* mgr = nullptr;
    ISensorCollection* coll = nullptr;
    ISensor* sensor = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_SensorManager, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mgr));
    if (SUCCEEDED(hr)) hr = mgr->GetSensorsByType(SENSOR_TYPE_ACCELEROMETER_3D, &coll);
    ULONG count = 0;
    if (SUCCEEDED(hr)) hr = coll->GetCount(&count);
    if (SUCCEEDED(hr) && count > 0) hr = coll->GetAt(0, &sensor);
    if (FAILED(hr) || !sensor) {
        set_status("가속도계 없음");
    } else {
        SensorState st = SENSOR_STATE_ERROR;
        sensor->GetState(&st);
        if (st == SENSOR_STATE_ACCESS_DENIED) {
            mgr->RequestPermissions(static_cast<HWND>(hwnd), coll, FALSE);
            sensor->GetState(&st);
        }
        set_status(st == SENSOR_STATE_READY ? "사용 가능" : "센서 상태 " + std::to_string(static_cast<int>(st)));
        while (run_) {
            ISensorDataReport* rep = nullptr;
            if (SUCCEEDED(sensor->GetData(&rep)) && rep) {
                PROPVARIANT px, py, pz;
                PropVariantInit(&px);
                PropVariantInit(&py);
                PropVariantInit(&pz);
                bool ok = SUCCEEDED(rep->GetSensorValue(SENSOR_DATA_TYPE_ACCELERATION_X_G, &px)) &&
                          SUCCEEDED(rep->GetSensorValue(SENSOR_DATA_TYPE_ACCELERATION_Y_G, &py)) &&
                          SUCCEEDED(rep->GetSensorValue(SENSOR_DATA_TYPE_ACCELERATION_Z_G, &pz));
                if (ok) {
                    std::lock_guard<std::mutex> lk(mu_);
                    gx_ = static_cast<float>(pv_double(px));
                    gy_ = static_cast<float>(pv_double(py));
                    gz_ = static_cast<float>(pv_double(pz));
                    have_ = true;
                    status_ = "사용 가능";
                    available_ = true;
                }
                PropVariantClear(&px);
                PropVariantClear(&py);
                PropVariantClear(&pz);
                rep->Release();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
        }
    }
    if (sensor) sensor->Release();
    if (coll) coll->Release();
    if (mgr) mgr->Release();
    CoUninitialize();
}

void TiltDrive::calibrate(float x, float y, float z) {
    bx = x;
    by = y;
    bz = z;
    calibrated = true;
}

// 기기 좌표에서 기준 중력과 현재 중력의 차이를 두 축 각도로 본다(앞뒤 = x-z 평면은 쓰지 않고 y·x 성분 사용).
void TiltDrive::map(float x, float y, float z, double& forward, double& turn) const {
    forward = turn = 0;
    if (!calibrated) return;
    const double deg = 180.0 / 3.141592653589793;
    double pitch0 = std::atan2(by, std::sqrt(bx * bx + bz * bz)) * deg;
    double roll0 = std::atan2(bx, std::sqrt(by * by + bz * bz)) * deg;
    double pitch = std::atan2(y, std::sqrt(x * x + z * z)) * deg;
    double roll = std::atan2(x, std::sqrt(y * y + z * z)) * deg;
    auto shape = [](double d) {
        double a = std::fabs(d);
        if (a < 5.0) return 0.0;
        double v = std::fmin(1.0, (a - 5.0) / 20.0);
        return d < 0 ? -v : v;
    };
    forward = shape(pitch - pitch0) * (invert_forward ? -1 : 1);
    turn = shape(roll - roll0) * (invert_turn ? 1 : -1);  // 오른쪽으로 기울이면 우회전(turn 음수)
}

}  // namespace hg
