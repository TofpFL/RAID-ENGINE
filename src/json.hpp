// Compact JSON DOM built for speed on very large FNF charts (millions of numbers).
// All values live in one flat array; arrays/objects reference a contiguous run of
// child indices, so parsing allocates only a handful of growing vectors.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace json {

enum class Type : uint8_t { Null, Bool, Number, String, Array, Object };

struct Value {
    Type type = Type::Null;
    bool b = false;
    union {
        double num;
        struct { uint32_t start, count; } r;  // String: bytes in pool; Array/Object: kids
    };
    Value() : num(0) {}
};

class Document {
public:
    bool parse(std::string_view text, std::string* err = nullptr);
    const Value& root() const { return vals_[root_]; }

    // Object lookup (linear; objects in charts are small). Returns nullptr if absent.
    const Value* get(const Value& obj, std::string_view key) const;
    size_t size(const Value& v) const { return (v.type == Type::Array || v.type == Type::Object) ? v.r.count : 0; }
    const Value& at(const Value& arr, size_t i) const { return vals_[kids_[arr.r.start + i]]; }
    // Object entries: key i and value i.
    std::string_view keyAt(const Value& obj, size_t i) const { return str(vals_[kids_[obj.r.start + 2 * i]]); }
    const Value& valAt(const Value& obj, size_t i) const { return vals_[kids_[obj.r.start + 2 * i + 1]]; }

    std::string_view str(const Value& v) const {
        return v.type == Type::String ? std::string_view(pool_.data() + v.r.start, v.r.count) : std::string_view();
    }
    static double num(const Value* v, double def) {
        if (!v) return def;
        if (v->type == Type::Number) return v->num;
        if (v->type == Type::Bool) return v->b ? 1.0 : 0.0;
        return def;
    }
    static bool boolean(const Value* v, bool def) {
        if (!v) return def;
        if (v->type == Type::Bool) return v->b;
        if (v->type == Type::Number) return v->num != 0.0;
        return def;
    }
    std::string_view strOr(const Value* v, std::string_view def) const {
        return (v && v->type == Type::String) ? str(*v) : def;
    }

private:
    uint32_t parseValue(int depth);
    bool parseString(uint32_t& start, uint32_t& count);
    void skipWs();
    bool fail(const char* msg);

    std::vector<Value> vals_;
    std::vector<uint32_t> kids_;
    std::vector<uint32_t> stack_;
    std::string pool_;
    const char* p_ = nullptr;
    const char* end_ = nullptr;
    const char* begin_ = nullptr;
    uint32_t root_ = 0;
    bool ok_ = true;
    std::string err_;
};

}  // namespace json
