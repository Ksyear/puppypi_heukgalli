#include "json.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace hg {

namespace {
const Json kNull;
const Json::Array kEmptyArray;
const Json::Object kEmptyObject;
}  // namespace

const Json::Array& Json::as_array() const { return is_array() ? *arr_ : kEmptyArray; }
const Json::Object& Json::as_object() const { return is_object() ? *obj_ : kEmptyObject; }

const Json& Json::operator[](const std::string& key) const {
    if (!is_object()) return kNull;
    for (const auto& kv : *obj_)
        if (kv.first == key) return kv.second;
    return kNull;
}

const Json& Json::operator[](size_t i) const {
    if (!is_array() || i >= arr_->size()) return kNull;
    return (*arr_)[i];
}

bool Json::has(const std::string& key) const {
    if (!is_object()) return false;
    for (const auto& kv : *obj_)
        if (kv.first == key) return true;
    return false;
}

size_t Json::size() const {
    if (is_array()) return arr_->size();
    if (is_object()) return obj_->size();
    return 0;
}

Json& Json::set(const std::string& key, Json value) {
    if (!is_object()) *this = Json(Object{});
    if (obj_.use_count() > 1) obj_ = std::make_shared<Object>(*obj_);
    for (auto& kv : *obj_)
        if (kv.first == key) {
            kv.second = std::move(value);
            return *this;
        }
    obj_->emplace_back(key, std::move(value));
    return *this;
}

Json& Json::push(Json value) {
    if (!is_array()) *this = Json(Array{});
    if (arr_.use_count() > 1) arr_ = std::make_shared<Array>(*arr_);
    arr_->push_back(std::move(value));
    return *this;
}

std::string json_quote(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out + "\"";
}

void Json::dump_to(std::string& out) const {
    switch (type_) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += bool_ ? "true" : "false"; break;
        case Type::Number: {
            char buf[40];
            if (!std::isfinite(num_)) {
                out += "null";  // JSON 에는 NaN/Inf 가 없다
            } else if (is_int_) {
                std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(num_));
                out += buf;
            } else {
                std::snprintf(buf, sizeof buf, "%.6g", num_);
                // 1e+06 같은 표기도 JSON 으로 유효하다. 정수처럼 보이는 실수는 ".0" 을 붙여 실수임을 유지
                if (!std::strpbrk(buf, ".eEn")) std::strcat(buf, ".0");
                out += buf;
            }
            break;
        }
        case Type::String: out += json_quote(*str_); break;
        case Type::Array: {
            out += '[';
            bool first = true;
            for (const auto& v : *arr_) {
                if (!first) out += ',';
                first = false;
                v.dump_to(out);
            }
            out += ']';
            break;
        }
        case Type::Object: {
            out += '{';
            bool first = true;
            for (const auto& kv : *obj_) {
                if (!first) out += ',';
                first = false;
                out += json_quote(kv.first);
                out += ':';
                kv.second.dump_to(out);
            }
            out += '}';
            break;
        }
    }
}

std::string Json::dump() const {
    std::string out;
    dump_to(out);
    return out;
}

class JsonParser {
public:
    explicit JsonParser(const std::string& s) : s_(s) {}

    Json parse_root() {
        Json v = parse_value(0);
        skip_ws();
        if (i_ != s_.size()) fail("뒤에 남은 문자");
        return v;
    }

private:
    const std::string& s_;
    size_t i_ = 0;

    [[noreturn]] void fail(const char* what) {
        throw std::runtime_error(std::string("JSON 오류(") + what + ") 위치 " + std::to_string(i_));
    }
    void skip_ws() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\n' || s_[i_] == '\r' || s_[i_] == '\t')) ++i_;
    }
    bool match(const char* lit) {
        size_t n = std::strlen(lit);
        if (s_.compare(i_, n, lit) == 0) {
            i_ += n;
            return true;
        }
        return false;
    }

    Json parse_value(int depth) {
        if (depth > 64) fail("너무 깊음");
        skip_ws();
        if (i_ >= s_.size()) fail("끝남");
        char c = s_[i_];
        if (c == '{') return parse_object(depth);
        if (c == '[') return parse_array(depth);
        if (c == '"') return Json(parse_string());
        if (match("true")) return Json(true);
        if (match("false")) return Json(false);
        if (match("null")) return Json();
        if (c == '-' || (c >= '0' && c <= '9')) return parse_number();
        fail("알 수 없는 값");
    }

    Json parse_number() {
        size_t start = i_;
        bool is_int = true;
        if (s_[i_] == '-') ++i_;
        while (i_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_]))) ++i_;
        if (i_ < s_.size() && s_[i_] == '.') {
            is_int = false;
            ++i_;
            while (i_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_]))) ++i_;
        }
        if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
            is_int = false;
            ++i_;
            if (i_ < s_.size() && (s_[i_] == '+' || s_[i_] == '-')) ++i_;
            while (i_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_]))) ++i_;
        }
        std::string tok = s_.substr(start, i_ - start);
        if (tok == "-" || tok.empty()) fail("숫자");
        if (is_int) {
            Json j(static_cast<int64_t>(std::strtoll(tok.c_str(), nullptr, 10)));
            return j;
        }
        return Json(std::strtod(tok.c_str(), nullptr));
    }

    static void append_utf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    uint32_t parse_hex4() {
        if (i_ + 4 > s_.size()) fail("\\u");
        uint32_t v = 0;
        for (int k = 0; k < 4; ++k) {
            char h = s_[i_++];
            v <<= 4;
            if (h >= '0' && h <= '9') v |= h - '0';
            else if (h >= 'a' && h <= 'f') v |= h - 'a' + 10;
            else if (h >= 'A' && h <= 'F') v |= h - 'A' + 10;
            else fail("\\u 16진수");
        }
        return v;
    }

    std::string parse_string() {
        ++i_;  // "
        std::string out;
        while (true) {
            if (i_ >= s_.size()) fail("문자열 끝 없음");
            char c = s_[i_++];
            if (c == '"') break;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i_ >= s_.size()) fail("이스케이프");
            char e = s_[i_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    uint32_t cp = parse_hex4();
                    if (cp >= 0xD800 && cp <= 0xDBFF && i_ + 6 <= s_.size() && s_[i_] == '\\' && s_[i_ + 1] == 'u') {
                        i_ += 2;
                        uint32_t lo = parse_hex4();
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    append_utf8(out, cp);
                    break;
                }
                default: fail("알 수 없는 이스케이프");
            }
        }
        return out;
    }

    Json parse_array(int depth) {
        ++i_;
        Json::Array arr;
        skip_ws();
        if (i_ < s_.size() && s_[i_] == ']') {
            ++i_;
            return Json(std::move(arr));
        }
        while (true) {
            arr.push_back(parse_value(depth + 1));
            skip_ws();
            if (i_ >= s_.size()) fail("배열 끝 없음");
            if (s_[i_] == ',') {
                ++i_;
                continue;
            }
            if (s_[i_] == ']') {
                ++i_;
                break;
            }
            fail("배열 구분자");
        }
        return Json(std::move(arr));
    }

    Json parse_object(int depth) {
        ++i_;
        Json::Object obj;
        skip_ws();
        if (i_ < s_.size() && s_[i_] == '}') {
            ++i_;
            return Json(std::move(obj));
        }
        while (true) {
            skip_ws();
            if (i_ >= s_.size() || s_[i_] != '"') fail("키");
            std::string key = parse_string();
            skip_ws();
            if (i_ >= s_.size() || s_[i_] != ':') fail(":");
            ++i_;
            obj.emplace_back(std::move(key), parse_value(depth + 1));
            skip_ws();
            if (i_ >= s_.size()) fail("객체 끝 없음");
            if (s_[i_] == ',') {
                ++i_;
                continue;
            }
            if (s_[i_] == '}') {
                ++i_;
                break;
            }
            fail("객체 구분자");
        }
        return Json(std::move(obj));
    }
};

Json Json::parse(const std::string& text) { return JsonParser(text).parse_root(); }

}  // namespace hg
