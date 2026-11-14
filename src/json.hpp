// 작은 JSON 값·해석기·출력기. 로봇이 주고받는 JSON(ACK, rosbridge, /api/*)만 다루면 되므로 표준 라이브러리만 쓴다.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace hg {

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };
    using Array = std::vector<Json>;
    using Object = std::vector<std::pair<std::string, Json>>;  // 입력 순서 유지

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool b) : type_(Type::Bool), bool_(b) {}
    Json(int v) : type_(Type::Number), num_(v), is_int_(true) {}
    Json(int64_t v) : type_(Type::Number), num_(static_cast<double>(v)), is_int_(true) {}
    Json(uint64_t v) : type_(Type::Number), num_(static_cast<double>(v)), is_int_(true) {}
    Json(double v) : type_(Type::Number), num_(v) {}
    Json(const char* s) : type_(Type::String), str_(std::make_shared<std::string>(s)) {}
    Json(std::string s) : type_(Type::String), str_(std::make_shared<std::string>(std::move(s))) {}
    Json(Array a) : type_(Type::Array), arr_(std::make_shared<Array>(std::move(a))) {}
    Json(Object o) : type_(Type::Object), obj_(std::make_shared<Object>(std::move(o))) {}

    static Json object() { return Json(Object{}); }
    static Json array() { return Json(Array{}); }
    static Json parse(const std::string& text);  // 실패 시 std::runtime_error

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }
    bool is_bool() const { return type_ == Type::Bool; }
    bool is_number() const { return type_ == Type::Number; }
    bool is_int() const { return type_ == Type::Number && is_int_; }
    bool is_string() const { return type_ == Type::String; }
    bool is_array() const { return type_ == Type::Array; }
    bool is_object() const { return type_ == Type::Object; }

    bool as_bool(bool def = false) const { return is_bool() ? bool_ : def; }
    double as_number(double def = 0.0) const { return is_number() ? num_ : def; }
    int64_t as_int(int64_t def = 0) const { return is_number() ? static_cast<int64_t>(num_) : def; }
    std::string as_string(const std::string& def = "") const { return is_string() ? *str_ : def; }
    const Array& as_array() const;
    const Object& as_object() const;

    // 객체 접근: 없으면 null 을 돌려준다(읽기용).
    const Json& operator[](const std::string& key) const;
    const Json& operator[](size_t i) const;
    bool has(const std::string& key) const;
    size_t size() const;

    // 쓰기용
    Json& set(const std::string& key, Json value);
    Json& push(Json value);

    std::string dump() const;
    // 화면 표시용: 문자열은 따옴표 없이, 나머지는 dump
    std::string text() const { return is_string() ? *str_ : dump(); }

private:
    Type type_ = Type::Null;
    bool bool_ = false;
    double num_ = 0.0;
    bool is_int_ = false;
    std::shared_ptr<std::string> str_;
    std::shared_ptr<Array> arr_;
    std::shared_ptr<Object> obj_;
    void dump_to(std::string& out) const;
    friend class JsonParser;
};

std::string json_quote(const std::string& s);

}  // namespace hg
