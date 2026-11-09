// HScript interpreter core: lexer, parser and evaluator. The game bridge is
// in hscript_bridge.cpp.
#include "hscript.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "common.hpp"
#include "psych.hpp"

namespace gh::hx {

// ---------------------------------------------------------------- values --

Value Value::array(std::vector<Value> v) {
    Value r;
    r.t = T::Array;
    r.arr = std::make_shared<std::vector<Value>>(std::move(v));
    return r;
}
Value Value::object(ValueMap m) {
    Value r;
    r.t = T::Object;
    r.map = std::make_shared<ValueMap>(std::move(m));
    return r;
}
Value Value::mapOf(ValueMap m) {
    Value r = object(std::move(m));
    r.t = T::Map;
    return r;
}
Value Value::native(std::string name, std::function<Value(std::vector<Value>&)> f) {
    Value r;
    r.t = T::Func;
    r.fn = std::make_shared<Func>();
    r.fn->name = std::move(name);
    r.fn->native = std::move(f);
    return r;
}
Value Value::hostOf(std::shared_ptr<Host> h) {
    Value r;
    if (!h) return r;
    r.t = T::Host;
    r.host = std::move(h);
    return r;
}

bool Value::truthy() const {
    switch (t) {
        case T::Null: return false;
        case T::Bool: return b;
        case T::Num: return n != 0 && !std::isnan(n);
        case T::Str: return true;
        default: return true;
    }
}

double Value::num() const {
    switch (t) {
        case T::Num: return n;
        case T::Bool: return b ? 1 : 0;
        case T::Str: {
            char* end = nullptr;
            const double d = std::strtod(s.c_str(), &end);
            return end == s.c_str() ? std::nan("") : d;
        }
        default: return 0;
    }
}

static std::string numStr(double d) {
    if (std::isnan(d)) return "NaN";
    if (std::isinf(d)) return d > 0 ? "Infinity" : "-Infinity";
    if (d == std::floor(d) && std::fabs(d) < 1e15) {
        char b[32];
        std::snprintf(b, sizeof b, "%lld", (long long)d);
        return b;
    }
    char b[40];
    std::snprintf(b, sizeof b, "%.15g", d);
    return b;
}

std::string Value::str() const {
    switch (t) {
        case T::Null: return "null";
        case T::Bool: return b ? "true" : "false";
        case T::Num: return numStr(n);
        case T::Str: return s;
        case T::Array: {
            std::string r = "[";
            for (size_t i = 0; i < arr->size(); ++i) r += (i ? "," : "") + (*arr)[i].str();
            return r + "]";
        }
        case T::Object:
        case T::Map: {
            std::string r = "{";
            bool first = true;
            for (auto& kv : *map) {
                r += (first ? "" : ", ") + kv.first + (t == T::Map ? " => " : ": ") + kv.second.str();
                first = false;
            }
            return r + "}";
        }
        case T::Func: return "<function>";
        case T::Host: return host ? "[" + host->kind + (host->path.empty() ? "" : " " + host->path) + "]" : "null";
    }
    return "";
}

Value Interp::fromScene(const scene::Value& v) {
    if (const bool* b = std::get_if<bool>(&v)) return Value(*b);
    if (const double* d = std::get_if<double>(&v)) return Value(*d);
    if (const std::string* s = std::get_if<std::string>(&v)) return Value(*s);
    return Value();
}

scene::Value Interp::toScene(const Value& v) {
    switch (v.t) {
        case T::Bool: return v.b;
        case T::Num: return v.n;
        case T::Str: return v.s;
        default: return std::monostate{};
    }
}

// ----------------------------------------------------------------- lexer --

namespace {

struct ScriptError : std::runtime_error {
    using std::runtime_error::runtime_error;
};
struct ThrownValue {
    Value v;
};

struct Tok {
    enum K { Id, Num, Str, Op, End } k = End;
    std::string s;
    double n = 0;
    int line = 1;
    bool single = false;  // single-quoted string (interpolated)
};

const char* kOps[] = {">>>=", "...", ">>>", "<<=", ">>=", "==", "!=", "<=", ">=", "&&", "||", "+=", "-=", "*=", "/=",
                      "%=",   "&=",  "|=",  "^=",  "++",  "--", "=>", "->", "<<", ">>", "?.", "??"};

std::vector<Tok> lex(const std::string& src) {
    std::vector<Tok> out;
    size_t i = 0;
    int line = 1;
    const size_t n = src.size();
    auto err = [&](const std::string& m) { throw ScriptError("line " + std::to_string(line) + ": " + m); };
    while (i < n) {
        const char c = src[i];
        if (c == '\n') { ++line; ++i; continue; }
        if (std::isspace((unsigned char)c)) { ++i; continue; }
        if (c == '/' && i + 1 < n && src[i + 1] == '/') {
            while (i < n && src[i] != '\n') ++i;
            continue;
        }
        if (c == '/' && i + 1 < n && src[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(src[i] == '*' && src[i + 1] == '/')) {
                if (src[i] == '\n') ++line;
                ++i;
            }
            i += 2;
            continue;
        }
        Tok t;
        t.line = line;
        if (std::isalpha((unsigned char)c) || c == '_' || (unsigned char)c >= 0x80) {
            size_t j = i;
            while (j < n && (std::isalnum((unsigned char)src[j]) || src[j] == '_' || (unsigned char)src[j] >= 0x80)) ++j;
            t.k = Tok::Id;
            t.s = src.substr(i, j - i);
            i = j;
        } else if (std::isdigit((unsigned char)c) || (c == '.' && i + 1 < n && std::isdigit((unsigned char)src[i + 1]))) {
            size_t j = i;
            if (c == '0' && j + 1 < n && (src[j + 1] == 'x' || src[j + 1] == 'X')) {
                j += 2;
                while (j < n && std::isxdigit((unsigned char)src[j])) ++j;
                t.n = double(std::strtoull(src.substr(i + 2, j - i - 2).c_str(), nullptr, 16));
            } else {
                while (j < n && std::isdigit((unsigned char)src[j])) ++j;
                if (j < n && src[j] == '.' && !(j + 1 < n && src[j + 1] == '.')) {
                    ++j;
                    while (j < n && std::isdigit((unsigned char)src[j])) ++j;
                }
                if (j < n && (src[j] == 'e' || src[j] == 'E')) {
                    ++j;
                    if (j < n && (src[j] == '+' || src[j] == '-')) ++j;
                    while (j < n && std::isdigit((unsigned char)src[j])) ++j;
                }
                t.n = std::strtod(src.substr(i, j - i).c_str(), nullptr);
            }
            t.k = Tok::Num;
            i = j;
        } else if (c == '"' || c == '\'') {
            t.k = Tok::Str;
            t.single = c == '\'';
            size_t j = i + 1;
            std::string s;
            while (j < n && src[j] != c) {
                if (src[j] == '\n') ++line;
                if (src[j] == '\\' && j + 1 < n) {
                    const char e = src[j + 1];
                    switch (e) {
                        case 'n': s += '\n'; break;
                        case 't': s += '\t'; break;
                        case 'r': s += '\r'; break;
                        case '0': s += '\0'; break;
                        case '$': s += t.single ? "$$" : "$"; break;  // \$ stays literal
                        default: s += e;
                    }
                    j += 2;
                    continue;
                }
                s += src[j++];
            }
            if (j >= n) err("unterminated string");
            t.s = s;
            i = j + 1;
        } else {
            t.k = Tok::Op;
            bool found = false;
            for (const char* op : kOps) {
                const size_t len = std::strlen(op);
                if (src.compare(i, len, op) == 0) {
                    t.s = op;
                    i += len;
                    found = true;
                    break;
                }
            }
            if (!found) {
                if (std::strchr("+-*/%=<>!&|^~?:.,;()[]{}@#", c) == nullptr) err(std::string("unexpected character '") + c + "'");
                t.s = std::string(1, c);
                ++i;
            }
        }
        out.push_back(std::move(t));
    }
    Tok end;
    end.line = line;
    out.push_back(end);
    return out;
}

}  // namespace

// ---------------------------------------------------------------- syntax --

enum class NK : uint8_t {
    Num, Str, Interp, Bool, Null, Ident, Field, Index, Call, New, Unary, Postfix, Binary, Assign, Ternary,
    ArrayLit, MapLit, ObjLit, Func, Var, Block, If, While, DoWhile, For, ForKV, Range, Break, Continue,
    Return, Switch, Case, Try, Throw, Nop
};

struct Node {
    NK k = NK::Nop;
    int line = 0;
    double num = 0;
    std::string str, op;
    std::vector<std::unique_ptr<Node>> kids;
    std::vector<std::string> names;
    bool flag = false;
};
using NodeP = std::unique_ptr<Node>;

struct Program {
    NodeP root;
};

namespace {

class Parser {
public:
    explicit Parser(std::vector<Tok> toks) : t_(std::move(toks)) {}

    NodeP program() {
        auto b = make(NK::Block);
        while (!at(Tok::End)) {
            if (isOp(";")) { ++p_; continue; }
            b->kids.push_back(statement());
        }
        return b;
    }

private:
    std::vector<Tok> t_;
    size_t p_ = 0;

    const Tok& cur() const { return t_[p_]; }
    const Tok& peekTok(size_t k = 1) const { return t_[std::min(p_ + k, t_.size() - 1)]; }
    bool at(Tok::K k) const { return cur().k == k; }
    bool isOp(const char* s) const { return cur().k == Tok::Op && cur().s == s; }
    bool isId(const char* s) const { return cur().k == Tok::Id && cur().s == s; }
    [[noreturn]] void fail(const std::string& m) const {
        throw ScriptError("line " + std::to_string(cur().line) + ": " + m +
                          (cur().k == Tok::End ? " (end of code)" : " near '" + cur().s + "'"));
    }
    void expect(const char* s) {
        if (!isOp(s)) fail(std::string("expected '") + s + "'");
        ++p_;
    }
    bool accept(const char* s) {
        if (isOp(s)) { ++p_; return true; }
        return false;
    }
    std::string ident() {
        if (!at(Tok::Id)) fail("expected a name");
        return t_[p_++].s;
    }
    NodeP make(NK k) {
        auto n = std::make_unique<Node>();
        n->k = k;
        n->line = cur().line;
        return n;
    }

    // Skips a type annotation: Int, flixel.FlxSprite, Map<String, Int>, Int->Void, {x:Int}.
    void skipType() {
        if (isOp("{")) {
            int depth = 0;
            do {
                if (isOp("{")) ++depth;
                else if (isOp("}")) --depth;
                ++p_;
            } while (depth > 0 && !at(Tok::End));
        } else if (isOp("(")) {
            int depth = 0;
            do {
                if (isOp("(")) ++depth;
                else if (isOp(")")) --depth;
                ++p_;
            } while (depth > 0 && !at(Tok::End));
        } else {
            accept("?");
            ident();
            while (isOp(".") && peekTok().k == Tok::Id) { p_ += 2; }
            if (isOp("<")) {
                int depth = 0;
                do {
                    if (isOp("<")) ++depth;
                    else if (isOp(">")) --depth;
                    else if (isOp(">>")) depth -= 2;
                    else if (isOp(">>>")) depth -= 3;
                    ++p_;
                } while (depth > 0 && !at(Tok::End));
            }
        }
        if (isOp("->")) {
            ++p_;
            skipType();
        }
    }

    NodeP block() {
        auto b = make(NK::Block);
        expect("{");
        while (!isOp("}")) {
            if (at(Tok::End)) fail("missing '}'");
            if (isOp(";")) { ++p_; continue; }
            b->kids.push_back(statement());
        }
        ++p_;
        return b;
    }

    void endStatement() {
        if (accept(";")) return;
        // Semicolons are optional after a block and at the end of a block.
        if (isOp("}") || at(Tok::End) || (p_ > 0 && t_[p_ - 1].k == Tok::Op && t_[p_ - 1].s == "}")) return;
        fail("expected ';'");
    }

    NodeP funcRest(const std::string& name) {
        auto f = make(NK::Func);
        f->str = name;
        expect("(");
        std::vector<NodeP> defaults;
        while (!isOp(")")) {
            accept("?");
            f->names.push_back(ident());
            if (accept(":")) skipType();
            if (accept("=")) defaults.push_back(expr());
            else defaults.push_back(make(NK::Nop));
            if (!accept(",")) break;
        }
        expect(")");
        if (accept(":")) skipType();
        if (isOp("{")) {
            f->kids.push_back(block());
        } else {
            f->kids.push_back(expr());  // function(x) return x; or expression body
            f->flag = true;
        }
        for (auto& d : defaults) f->kids.push_back(std::move(d));
        return f;
    }

    NodeP statement() {
        if (isOp("{")) return block();
        if (at(Tok::Id)) {
            const std::string& w = cur().s;
            if (w == "var" || w == "final") {
                ++p_;
                auto v = make(NK::Var);
                do {
                    v->names.push_back(ident());
                    if (accept(":")) skipType();
                    if (accept("=")) v->kids.push_back(expr());
                    else v->kids.push_back(make(NK::Nop));
                } while (accept(","));
                endStatement();
                return v;
            }
            if (w == "function" && peekTok().k == Tok::Id) {
                ++p_;
                const std::string name = ident();
                auto f = funcRest(name);
                auto v = make(NK::Var);
                v->names.push_back(name);
                v->kids.push_back(std::move(f));
                v->flag = true;  // function declaration
                return v;
            }
            if (w == "if") {
                ++p_;
                auto n = make(NK::If);
                expect("(");
                n->kids.push_back(expr());
                expect(")");
                n->kids.push_back(statement());
                accept(";");
                if (isId("else")) {
                    ++p_;
                    n->kids.push_back(statement());
                }
                return n;
            }
            if (w == "while") {
                ++p_;
                auto n = make(NK::While);
                expect("(");
                n->kids.push_back(expr());
                expect(")");
                n->kids.push_back(statement());
                return n;
            }
            if (w == "do") {
                ++p_;
                auto n = make(NK::DoWhile);
                n->kids.push_back(statement());
                if (!isId("while")) fail("expected 'while'");
                ++p_;
                expect("(");
                n->kids.push_back(expr());
                expect(")");
                endStatement();
                return n;
            }
            if (w == "for") {
                ++p_;
                expect("(");
                const std::string a = ident();
                NodeP n;
                if (accept("=>")) {
                    n = make(NK::ForKV);
                    n->names = {a, ident()};
                } else {
                    n = make(NK::For);
                    n->names = {a};
                }
                if (!isId("in")) fail("expected 'in'");
                ++p_;
                NodeP it = expr();
                if (accept("...")) {
                    auto r = make(NK::Range);
                    r->kids.push_back(std::move(it));
                    r->kids.push_back(expr());
                    it = std::move(r);
                }
                n->kids.push_back(std::move(it));
                expect(")");
                n->kids.push_back(statement());
                return n;
            }
            if (w == "break" || w == "continue") {
                ++p_;
                auto n = make(w == "break" ? NK::Break : NK::Continue);
                endStatement();
                return n;
            }
            if (w == "return") {
                ++p_;
                auto n = make(NK::Return);
                if (!isOp(";") && !isOp("}")) n->kids.push_back(expr());
                endStatement();
                return n;
            }
            if (w == "throw") {
                ++p_;
                auto n = make(NK::Throw);
                n->kids.push_back(expr());
                endStatement();
                return n;
            }
            if (w == "import" || w == "using" || w == "package") {
                ++p_;
                while (!isOp(";") && !at(Tok::End)) ++p_;
                accept(";");
                return make(NK::Nop);
            }
            if (w == "try") {
                ++p_;
                auto n = make(NK::Try);
                n->kids.push_back(statement());
                if (isId("catch")) {
                    ++p_;
                    expect("(");
                    n->names.push_back(ident());
                    if (accept(":")) skipType();
                    expect(")");
                    n->kids.push_back(statement());
                } else {
                    n->kids.push_back(make(NK::Nop));
                    n->names.push_back("_");
                }
                return n;
            }
            if (w == "switch") return switchExpr();
        }
        auto e = expr();
        endStatement();
        return e;
    }

    NodeP switchExpr() {
        ++p_;
        auto n = make(NK::Switch);
        const bool paren = accept("(");
        n->kids.push_back(expr());
        if (paren) expect(")");
        expect("{");
        while (!isOp("}")) {
            auto c = make(NK::Case);
            if (isId("case")) {
                ++p_;
                do {
                    if (isId("_")) { ++p_; c->flag = true; }
                    else c->kids.push_back(expr());
                } while (accept(",") || accept("|"));
            } else if (isId("default")) {
                ++p_;
                c->flag = true;
            } else {
                fail("expected 'case'");
            }
            expect(":");
            auto body = make(NK::Block);
            while (!isId("case") && !isId("default") && !isOp("}")) {
                if (at(Tok::End)) fail("missing '}'");
                if (isOp(";")) { ++p_; continue; }
                body->kids.push_back(statement());
            }
            c->kids.push_back(std::move(body));
            n->kids.push_back(std::move(c));
        }
        ++p_;
        return n;
    }

    // ------------------------------------------------------- expressions --
    NodeP expr() { return assign(); }

    NodeP assign() {
        auto left = ternary();
        static const char* ops[] = {"=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>=", ">>>="};
        for (const char* op : ops)
            if (isOp(op)) {
                ++p_;
                auto n = make(NK::Assign);
                n->op = op;
                n->kids.push_back(std::move(left));
                n->kids.push_back(assign());
                return n;
            }
        return left;
    }

    NodeP ternary() {
        auto c = binary(0);
        if (isOp("?")) {
            ++p_;
            auto n = make(NK::Ternary);
            n->kids.push_back(std::move(c));
            n->kids.push_back(assign());
            expect(":");
            n->kids.push_back(assign());
            return n;
        }
        return c;
    }

    static int prec(const std::string& op) {
        if (op == "??") return 1;
        if (op == "||") return 2;
        if (op == "&&") return 3;
        if (op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">=") return 4;
        if (op == "|" || op == "&" || op == "^") return 5;
        if (op == "<<" || op == ">>" || op == ">>>") return 6;
        if (op == "+" || op == "-") return 7;
        if (op == "*" || op == "/") return 8;
        if (op == "%") return 9;
        return -1;
    }

    NodeP binary(int minPrec) {
        auto left = unary();
        for (;;) {
            if (cur().k == Tok::Id && cur().s == "is") {  // x is Type
                ++p_;
                skipType();
                auto n = make(NK::Bool);
                n->flag = true;
                left = std::move(n);
                continue;
            }
            if (cur().k != Tok::Op) break;
            const int pr = prec(cur().s);
            if (pr < 0 || pr < minPrec) break;
            const std::string op = cur().s;
            ++p_;
            auto right = binary(pr + 1);
            auto n = make(NK::Binary);
            n->op = op;
            n->kids.push_back(std::move(left));
            n->kids.push_back(std::move(right));
            left = std::move(n);
        }
        return left;
    }

    NodeP unary() {
        if (isOp("-") || isOp("!") || isOp("~") || isOp("+") || isOp("++") || isOp("--")) {
            auto n = make(NK::Unary);
            n->op = cur().s;
            ++p_;
            n->kids.push_back(unary());
            return n;
        }
        return postfix(primary());
    }

    NodeP postfix(NodeP e) {
        for (;;) {
            if (isOp(".") || isOp("?.")) {
                const bool safe = cur().s == "?.";
                ++p_;
                auto n = make(NK::Field);
                n->kids.push_back(std::move(e));
                n->str = ident();
                n->flag = safe;
                e = std::move(n);
            } else if (isOp("[")) {
                ++p_;
                auto n = make(NK::Index);
                n->kids.push_back(std::move(e));
                n->kids.push_back(expr());
                expect("]");
                e = std::move(n);
            } else if (isOp("(")) {
                ++p_;
                auto n = make(NK::Call);
                n->kids.push_back(std::move(e));
                while (!isOp(")")) {
                    n->kids.push_back(expr());
                    if (!accept(",")) break;
                }
                expect(")");
                e = std::move(n);
            } else if (isOp("++") || isOp("--")) {
                auto n = make(NK::Postfix);
                n->op = cur().s;
                ++p_;
                n->kids.push_back(std::move(e));
                e = std::move(n);
            } else {
                return e;
            }
        }
    }

    // Is the '(' at p_ the start of an arrow function: (a, b) -> ...
    bool arrowAhead() const {
        size_t q = p_ + 1;
        int depth = 1;
        while (q < t_.size() && depth > 0) {
            if (t_[q].k == Tok::Op) {
                if (t_[q].s == "(") ++depth;
                else if (t_[q].s == ")") --depth;
            }
            if (t_[q].k == Tok::End) return false;
            ++q;
        }
        return q < t_.size() && t_[q].k == Tok::Op && t_[q].s == "->";
    }

    NodeP stringNode(const Tok& tk) {
        if (!tk.single || tk.s.find('$') == std::string::npos) {
            auto n = make(NK::Str);
            n->str = tk.s;
            // "$$" stands for a literal '$' in single quotes.
            if (tk.single)
                for (size_t q; (q = n->str.find("$$")) != std::string::npos;) n->str.replace(q, 2, "$");
            return n;
        }
        auto n = make(NK::Interp);
        std::string lit;
        const std::string& s = tk.s;
        auto flushLit = [&]() {
            if (lit.empty()) return;
            auto l = make(NK::Str);
            l->str = lit;
            n->kids.push_back(std::move(l));
            lit.clear();
        };
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] != '$' || i + 1 >= s.size()) { lit += s[i]; continue; }
            if (s[i + 1] == '$') { lit += '$'; ++i; continue; }
            if (s[i + 1] == '{') {
                int depth = 1;
                size_t j = i + 2;
                while (j < s.size() && depth > 0) {
                    if (s[j] == '{') ++depth;
                    else if (s[j] == '}') --depth;
                    if (depth > 0) ++j;
                }
                flushLit();
                Parser sub(lex(s.substr(i + 2, j - i - 2)));
                n->kids.push_back(sub.expr());
                i = j;
                continue;
            }
            if (std::isalpha((unsigned char)s[i + 1]) || s[i + 1] == '_') {
                size_t j = i + 1;
                while (j < s.size() && (std::isalnum((unsigned char)s[j]) || s[j] == '_')) ++j;
                flushLit();
                auto id = make(NK::Ident);
                id->str = s.substr(i + 1, j - i - 1);
                n->kids.push_back(std::move(id));
                i = j - 1;
                continue;
            }
            lit += '$';
        }
        flushLit();
        return n;
    }

    NodeP primary() {
        const Tok& tk = cur();
        if (tk.k == Tok::Num) {
            auto n = make(NK::Num);
            n->num = tk.n;
            ++p_;
            return n;
        }
        if (tk.k == Tok::Str) {
            ++p_;
            return stringNode(tk);
        }
        if (tk.k == Tok::Id) {
            const std::string w = tk.s;
            if (w == "true" || w == "false") {
                auto n = make(NK::Bool);
                n->flag = w == "true";
                ++p_;
                return n;
            }
            if (w == "null") { ++p_; return make(NK::Null); }
            if (w == "function") {
                ++p_;
                std::string name;
                if (at(Tok::Id)) name = ident();
                return funcRest(name);
            }
            if (w == "new") {
                ++p_;
                auto n = make(NK::New);
                n->str = ident();
                while (isOp(".") && peekTok().k == Tok::Id) {
                    ++p_;
                    n->str = ident();  // keep the class name only
                }
                if (isOp("<")) {
                    int depth = 0;
                    do {
                        if (isOp("<")) ++depth;
                        else if (isOp(">")) --depth;
                        else if (isOp(">>")) depth -= 2;
                        ++p_;
                    } while (depth > 0 && !at(Tok::End));
                }
                expect("(");
                while (!isOp(")")) {
                    n->kids.push_back(expr());
                    if (!accept(",")) break;
                }
                expect(")");
                return n;
            }
            if (w == "cast") {
                ++p_;
                if (accept("(")) {
                    auto e = expr();
                    if (accept(",")) skipType();
                    expect(")");
                    return e;
                }
                return unary();
            }
            if (w == "untyped") { ++p_; return unary(); }
            if (w == "if") {  // if as an expression
                ++p_;
                auto n = make(NK::If);
                n->flag = true;
                expect("(");
                n->kids.push_back(expr());
                expect(")");
                n->kids.push_back(expr());
                if (isId("else")) {
                    ++p_;
                    n->kids.push_back(expr());
                }
                return n;
            }
            if (w == "switch") return switchExpr();
            // a -> expr
            if (peekTok().k == Tok::Op && peekTok().s == "->") {
                auto f = make(NK::Func);
                f->names.push_back(ident());
                ++p_;
                f->kids.push_back(expr());
                f->flag = true;
                f->kids.push_back(make(NK::Nop));
                return f;
            }
            auto n = make(NK::Ident);
            n->str = w;
            ++p_;
            return n;
        }
        if (tk.k == Tok::Op) {
            if (tk.s == "(") {
                if (arrowAhead()) {
                    auto f = make(NK::Func);
                    ++p_;
                    std::vector<NodeP> defaults;
                    while (!isOp(")")) {
                        accept("?");
                        f->names.push_back(ident());
                        if (accept(":")) skipType();
                        defaults.push_back(make(NK::Nop));
                        if (!accept(",")) break;
                    }
                    expect(")");
                    expect("->");
                    if (isOp("{")) f->kids.push_back(block());
                    else { f->kids.push_back(expr()); f->flag = true; }
                    for (auto& d : defaults) f->kids.push_back(std::move(d));
                    return f;
                }
                ++p_;
                auto e = expr();
                if (accept(":")) skipType();  // (x : Type)
                expect(")");
                return e;
            }
            if (tk.s == "[") {
                ++p_;
                auto arr = make(NK::ArrayLit);
                bool isMap = false;
                while (!isOp("]")) {
                    arr->kids.push_back(expr());
                    if (accept("=>")) {
                        isMap = true;
                        arr->kids.push_back(expr());
                    }
                    if (!accept(",")) break;
                }
                expect("]");
                if (isMap) arr->k = NK::MapLit;
                return arr;
            }
            if (tk.s == "{") {
                // Object literal {a: 1} (or {} empty), else a block expression.
                const Tok& a = peekTok(1);
                const Tok& b = peekTok(2);
                const bool obj = (a.k == Tok::Op && a.s == "}") ||
                                 ((a.k == Tok::Id || a.k == Tok::Str) && b.k == Tok::Op && b.s == ":");
                if (!obj) return block();
                ++p_;
                auto o = make(NK::ObjLit);
                while (!isOp("}")) {
                    if (at(Tok::Str)) o->names.push_back(t_[p_++].s);
                    else o->names.push_back(ident());
                    expect(":");
                    o->kids.push_back(expr());
                    if (!accept(",")) break;
                }
                expect("}");
                return o;
            }
        }
        fail("unexpected token");
    }
};

}  // namespace

// ------------------------------------------------------------ evaluation --

struct Env {
    std::map<std::string, Value> vars;
    std::shared_ptr<Env> parent;
    Value* find(const std::string& n) {
        for (Env* e = this; e; e = e->parent.get()) {
            auto it = e->vars.find(n);
            if (it != e->vars.end()) return &it->second;
        }
        return nullptr;
    }
};

namespace {

enum class Flow { Normal, Break, Continue, Return };

int32_t toI32(double d) {
    if (!std::isfinite(d)) return 0;
    return int32_t(int64_t(d));
}

bool equalValues(const Value& a, const Value& b) {
    if (a.t == T::Null || b.t == T::Null) return a.t == b.t;
    if (a.t == T::Num && b.t == T::Num) return a.n == b.n;
    if ((a.t == T::Num || a.t == T::Bool) && (b.t == T::Num || b.t == T::Bool)) return a.num() == b.num();
    if (a.t != b.t) return false;
    switch (a.t) {
        case T::Bool: return a.b == b.b;
        case T::Str: return a.s == b.s;
        case T::Array: return a.arr == b.arr;
        case T::Object:
        case T::Map: return a.map == b.map;
        case T::Func: return a.fn == b.fn;
        case T::Host:
            if (a.host == b.host) return true;
            if (!a.host || !b.host) return false;
            if (a.host->sprite || b.host->sprite) return a.host->sprite == b.host->sprite;
            if (a.host->shader || b.host->shader) return a.host->shader == b.host->shader;
            return a.host->kind == b.host->kind && a.host->path == b.host->path && a.host->index == b.host->index;
        default: return false;
    }
}

}  // namespace

struct Evaluator {
    Interp& ip;
    std::shared_ptr<Program> prog;
    Value ret;
    Value last;
    long steps = 0;
    double start = 0;

    void tick() {
        if ((++steps & 1023) == 0 && nowSec() - start > ip.budget) throw ScriptError("script took too long, stopped");
    }

    [[noreturn]] void fail(const Node& n, const std::string& m) {
        throw ScriptError("line " + std::to_string(n.line) + ": " + m);
    }

    Value lookup(const Node& n, std::shared_ptr<Env>& env) {
        if (Value* v = env->find(n.str)) return *v;
        bool found = false;
        Value g = globalLookup(ip, n.str, found);
        if (found) return g;
        fail(n, "unknown variable " + n.str);
    }

    Value field(const Node& n, const Value& obj, const std::string& name, bool safe) {
        switch (obj.t) {
            case T::Null:
                if (safe) return Value();
                fail(n, "cannot read '" + name + "' of null");
            case T::Host: return hostGet(ip, *obj.host, name);
            case T::Str:
                if (name == "length") return Value(double(obj.s.size()));
                break;
            case T::Array:
                if (name == "length") return Value(double(obj.arr->size()));
                break;
            case T::Object:
            case T::Map: {
                auto it = obj.map->find(name);
                if (it != obj.map->end()) return it->second;
                return Value();
            }
            default: break;
        }
        return Value();
    }

    void setField(const Node& n, const Value& obj, const std::string& name, const Value& v) {
        switch (obj.t) {
            case T::Host: hostSet(ip, *obj.host, name, v); return;
            case T::Object:
            case T::Map: (*obj.map)[name] = v; return;
            case T::Null: fail(n, "cannot set '" + name + "' of null");
            default: fail(n, "cannot set '" + name + "' here");
        }
    }

    Value index(const Node& n, const Value& obj, const Value& idx) {
        switch (obj.t) {
            case T::Array: {
                const double d = idx.num();
                if (d < 0 || d >= double(obj.arr->size())) return Value();
                return (*obj.arr)[size_t(d)];
            }
            case T::Map:
            case T::Object: {
                auto it = obj.map->find(idx.str());
                return it == obj.map->end() ? Value() : it->second;
            }
            case T::Str: {
                const double d = idx.num();
                if (d < 0 || d >= double(obj.s.size())) return Value();
                return Value(std::string(1, obj.s[size_t(d)]));
            }
            case T::Host: return hostIndex(ip, *obj.host, idx);
            case T::Null: fail(n, "cannot index null");
            default: return Value();
        }
    }

    void setIndex(const Node& n, const Value& obj, const Value& idx, const Value& v) {
        if (obj.t == T::Array) {
            const double d = idx.num();
            if (d < 0 || !std::isfinite(d) || d > 1e7) fail(n, "bad array index");
            if (size_t(d) >= obj.arr->size()) obj.arr->resize(size_t(d) + 1);
            (*obj.arr)[size_t(d)] = v;
        } else if (obj.t == T::Map || obj.t == T::Object) {
            (*obj.map)[idx.str()] = v;
        } else if (obj.t == T::Host) {
            hostSet(ip, *obj.host, idx.str(), v);
        } else {
            fail(n, "cannot index this value");
        }
    }

    Value arith(const Node& n, const std::string& op, const Value& a, const Value& b) {
        if (op == "+") {
            if (a.t == T::Str || b.t == T::Str) return Value(a.str() + b.str());
            return Value(a.num() + b.num());
        }
        if (op == "-") return Value(a.num() - b.num());
        if (op == "*") return Value(a.num() * b.num());
        if (op == "/") return Value(a.num() / b.num());
        if (op == "%") return Value(std::fmod(a.num(), b.num()));
        if (op == "==") return Value(equalValues(a, b));
        if (op == "!=") return Value(!equalValues(a, b));
        if (op == "<" || op == ">" || op == "<=" || op == ">=") {
            if (a.t == T::Str && b.t == T::Str) {
                const int c = a.s.compare(b.s);
                return Value(op == "<" ? c < 0 : op == ">" ? c > 0 : op == "<=" ? c <= 0 : c >= 0);
            }
            const double x = a.num(), y = b.num();
            return Value(op == "<" ? x < y : op == ">" ? x > y : op == "<=" ? x <= y : x >= y);
        }
        const int32_t x = toI32(a.num()), y = toI32(b.num());
        if (op == "&") return Value(double(x & y));
        if (op == "|") return Value(double(x | y));
        if (op == "^") return Value(double(x ^ y));
        if (op == "<<") return Value(double(int32_t(uint32_t(x) << (y & 31))));
        if (op == ">>") return Value(double(x >> (y & 31)));
        if (op == ">>>") return Value(double(uint32_t(x) >> (y & 31)));
        fail(n, "unknown operator " + op);
    }

    // Reads and writes an assignable expression (identifier, field, index).
    Value getTarget(const Node& t, std::shared_ptr<Env>& env) { return eval(t, env); }
    void setTarget(const Node& t, std::shared_ptr<Env>& env, const Value& v) {
        if (t.k == NK::Ident) {
            if (Value* slot = env->find(t.str)) { *slot = v; return; }
            ip.setGlobal(t.str, v);  // hscript: unknown names become globals
            return;
        }
        if (t.k == NK::Field) {
            Value obj = eval(*t.kids[0], env);
            setField(t, obj, t.str, v);
            return;
        }
        if (t.k == NK::Index) {
            Value obj = eval(*t.kids[0], env);
            Value idx = eval(*t.kids[1], env);
            setIndex(t, obj, idx, v);
            return;
        }
        fail(t, "cannot assign to this");
    }

    Value makeFunc(const Node& n, std::shared_ptr<Env>& env) {
        Value v;
        v.t = T::Func;
        v.fn = std::make_shared<Func>();
        v.fn->name = n.str;
        v.fn->params = n.names;
        v.fn->body = n.kids[0].get();
        v.fn->exprBody = n.flag;
        for (size_t i = 1; i < n.kids.size(); ++i) v.fn->defaults.push_back(n.kids[i]->k == NK::Nop ? nullptr : n.kids[i].get());
        v.fn->env = env;
        v.fn->prog = prog;
        return v;
    }

    Value callFunc(const Value& f, std::vector<Value>& args) {
        if (f.t != T::Func || !f.fn) throw ScriptError("calling something that is not a function");
        const Func& fn = *f.fn;
        if (fn.native) return fn.native(args);
        auto env = std::make_shared<Env>();
        env->parent = fn.env;
        for (size_t i = 0; i < fn.params.size(); ++i) {
            if (i < args.size()) env->vars[fn.params[i]] = args[i];
            else if (i < fn.defaults.size() && fn.defaults[i]) env->vars[fn.params[i]] = eval(*fn.defaults[i], env);
            else env->vars[fn.params[i]] = Value();
        }
        if (fn.exprBody) return eval(*fn.body, env);
        Value saved = ret;
        ret = Value();
        const Flow fl = exec(*fn.body, env);
        Value out = fl == Flow::Return ? ret : Value();
        ret = saved;
        return out;
    }

    Value builtinMethod(const Node& n, const Value& obj, const std::string& m, std::vector<Value>& args) {
        auto arg = [&](size_t i) { return i < args.size() ? args[i] : Value(); };
        if (obj.t == T::Str) {
            const std::string& s = obj.s;
            if (m == "toUpperCase") { std::string r = s; for (char& c : r) c = char(std::toupper((unsigned char)c)); return Value(r); }
            if (m == "toLowerCase") return Value(toLower(s));
            if (m == "charAt") { const double d = arg(0).num(); return d >= 0 && d < double(s.size()) ? Value(std::string(1, s[size_t(d)])) : Value(""); }
            if (m == "charCodeAt") { const double d = arg(0).num(); return d >= 0 && d < double(s.size()) ? Value(double((unsigned char)s[size_t(d)])) : Value(); }
            if (m == "indexOf") { const size_t p = s.find(arg(0).str(), size_t(std::max(0.0, arg(1).num()))); return Value(p == std::string::npos ? -1.0 : double(p)); }
            if (m == "lastIndexOf") { const size_t p = s.rfind(arg(0).str()); return Value(p == std::string::npos ? -1.0 : double(p)); }
            if (m == "split") {
                std::vector<Value> parts;
                const std::string sep = arg(0).str();
                if (sep.empty()) { for (char c : s) parts.push_back(Value(std::string(1, c))); return Value::array(parts); }
                size_t p = 0;
                for (;;) {
                    const size_t e = s.find(sep, p);
                    parts.push_back(Value(s.substr(p, e == std::string::npos ? std::string::npos : e - p)));
                    if (e == std::string::npos) break;
                    p = e + sep.size();
                }
                return Value::array(parts);
            }
            if (m == "substr") {
                long long pos = (long long)arg(0).num();
                if (pos < 0) pos = std::max(0LL, (long long)s.size() + pos);
                if (pos >= (long long)s.size()) return Value("");
                const double len = args.size() > 1 ? arg(1).num() : double(s.size());
                return Value(s.substr(size_t(pos), size_t(std::max(0.0, len))));
            }
            if (m == "substring") {
                size_t a = size_t(std::clamp(arg(0).num(), 0.0, double(s.size())));
                size_t b = args.size() > 1 ? size_t(std::clamp(arg(1).num(), 0.0, double(s.size()))) : s.size();
                if (a > b) std::swap(a, b);
                return Value(s.substr(a, b - a));
            }
            if (m == "toString") return obj;
            fail(n, "unknown string method " + m);
        }
        if (obj.t == T::Array) {
            auto& a = *obj.arr;
            if (m == "push") { a.push_back(arg(0)); return Value(double(a.size())); }
            if (m == "pop") { if (a.empty()) return Value(); Value v = a.back(); a.pop_back(); return v; }
            if (m == "shift") { if (a.empty()) return Value(); Value v = a.front(); a.erase(a.begin()); return v; }
            if (m == "unshift") { a.insert(a.begin(), arg(0)); return Value(); }
            if (m == "insert") { const size_t p = size_t(std::clamp(arg(0).num(), 0.0, double(a.size()))); a.insert(a.begin() + std::ptrdiff_t(p), arg(1)); return Value(); }
            if (m == "remove") {
                for (size_t i = 0; i < a.size(); ++i)
                    if (equalValues(a[i], arg(0))) { a.erase(a.begin() + std::ptrdiff_t(i)); return Value(true); }
                return Value(false);
            }
            if (m == "indexOf" || m == "contains" || m == "lastIndexOf") {
                double found = -1;
                for (size_t i = 0; i < a.size(); ++i)
                    if (equalValues(a[i], arg(0))) { found = double(i); if (m != "lastIndexOf") break; }
                return m == "contains" ? Value(found >= 0) : Value(found);
            }
            if (m == "join") { std::string r; for (size_t i = 0; i < a.size(); ++i) r += (i ? arg(0).str() : "") + a[i].str(); return Value(r); }
            if (m == "concat") { std::vector<Value> r = a; if (arg(0).t == T::Array) r.insert(r.end(), arg(0).arr->begin(), arg(0).arr->end()); return Value::array(r); }
            if (m == "copy") return Value::array(a);
            if (m == "reverse") { std::reverse(a.begin(), a.end()); return Value(); }
            if (m == "slice") {
                long long b = (long long)arg(0).num(), e = args.size() > 1 ? (long long)arg(1).num() : (long long)a.size();
                if (b < 0) b += (long long)a.size();
                if (e < 0) e += (long long)a.size();
                b = std::clamp(b, 0LL, (long long)a.size());
                e = std::clamp(e, b, (long long)a.size());
                return Value::array(std::vector<Value>(a.begin() + b, a.begin() + e));
            }
            if (m == "splice") {
                long long b = (long long)arg(0).num(), len = (long long)arg(1).num();
                if (b < 0) b += (long long)a.size();
                b = std::clamp(b, 0LL, (long long)a.size());
                len = std::clamp(len, 0LL, (long long)a.size() - b);
                std::vector<Value> out(a.begin() + b, a.begin() + b + len);
                a.erase(a.begin() + b, a.begin() + b + len);
                return Value::array(out);
            }
            if (m == "map" || m == "filter") {
                std::vector<Value> out;
                const std::vector<Value> snap = a;
                for (const Value& v : snap) {
                    std::vector<Value> ca{v};
                    Value r = callFunc(arg(0), ca);
                    if (m == "map") out.push_back(r);
                    else if (r.truthy()) out.push_back(v);
                }
                return Value::array(out);
            }
            if (m == "sort") {
                Value f = arg(0);
                std::stable_sort(a.begin(), a.end(), [&](const Value& x, const Value& y) {
                    std::vector<Value> ca{x, y};
                    return callFunc(f, ca).num() < 0;
                });
                return Value();
            }
            if (m == "iterator") return obj;
            if (m == "resize") { a.resize(size_t(std::clamp(arg(0).num(), 0.0, 1e7))); return Value(); }
            if (m == "toString") return Value(obj.str());
            fail(n, "unknown array method " + m);
        }
        if (obj.t == T::Map || obj.t == T::Object) {
            auto& mp = *obj.map;
            if (obj.t == T::Object) {
                auto it = mp.find(m);
                if (it != mp.end()) return callFunc(it->second, args);
            }
            if (m == "get") { auto it = mp.find(arg(0).str()); return it == mp.end() ? Value() : it->second; }
            if (m == "set") { mp[arg(0).str()] = arg(1); return Value(); }
            if (m == "exists") return Value(mp.count(arg(0).str()) > 0);
            if (m == "remove") return Value(mp.erase(arg(0).str()) > 0);
            if (m == "keys") { std::vector<Value> k; for (auto& kv : mp) k.push_back(Value(kv.first)); return Value::array(k); }
            if (m == "iterator") { std::vector<Value> v; for (auto& kv : mp) v.push_back(kv.second); return Value::array(v); }
            if (m == "clear") { mp.clear(); return Value(); }
            if (m == "copy") return obj.t == T::Map ? Value::mapOf(mp) : Value::object(mp);
            if (m == "toString") return Value(obj.str());
            fail(n, "unknown method " + m);
        }
        if (obj.t == T::Host) return hostCall(ip, *obj.host, m, args);
        if (obj.t == T::Null) fail(n, "cannot call " + m + " on null");
        fail(n, "cannot call " + m + " here");
    }

    Value eval(const Node& n, std::shared_ptr<Env>& env) {
        switch (n.k) {
            case NK::Num: return Value(n.num);
            case NK::Str: return Value(n.str);
            case NK::Bool: return Value(n.flag);
            case NK::Null:
            case NK::Nop: return Value();
            case NK::Interp: {
                std::string s;
                for (const NodeP& k : n.kids) s += eval(*k, env).str();
                return Value(s);
            }
            case NK::Ident: return lookup(n, env);
            case NK::Field: {
                Value o = eval(*n.kids[0], env);
                return field(n, o, n.str, n.flag);
            }
            case NK::Index: {
                Value o = eval(*n.kids[0], env);
                Value i = eval(*n.kids[1], env);
                return index(n, o, i);
            }
            case NK::Call: {
                tick();
                const Node& callee = *n.kids[0];
                std::vector<Value> args;
                if (callee.k == NK::Field) {
                    Value obj = eval(*callee.kids[0], env);
                    if (obj.t == T::Null && callee.flag) return Value();
                    for (size_t i = 1; i < n.kids.size(); ++i) args.push_back(eval(*n.kids[i], env));
                    return builtinMethod(callee, obj, callee.str, args);
                }
                Value f = eval(callee, env);
                for (size_t i = 1; i < n.kids.size(); ++i) args.push_back(eval(*n.kids[i], env));
                if (f.t != T::Func) fail(n, (callee.k == NK::Ident ? callee.str : std::string("value")) + " is not a function");
                return callFunc(f, args);
            }
            case NK::New: {
                std::vector<Value> args;
                for (const NodeP& k : n.kids) args.push_back(eval(*k, env));
                return construct(ip, n.str, args);
            }
            case NK::Unary: {
                const std::string& op = n.op;
                if (op == "++" || op == "--") {
                    Value v = Value(getTarget(*n.kids[0], env).num() + (op == "++" ? 1 : -1));
                    setTarget(*n.kids[0], env, v);
                    return v;
                }
                Value v = eval(*n.kids[0], env);
                if (op == "-") return Value(-v.num());
                if (op == "+") return Value(v.num());
                if (op == "!") return Value(!v.truthy());
                return Value(double(~toI32(v.num())));
            }
            case NK::Postfix: {
                Value old = getTarget(*n.kids[0], env);
                setTarget(*n.kids[0], env, Value(old.num() + (n.op == "++" ? 1 : -1)));
                return Value(old.num());
            }
            case NK::Binary: {
                if (n.op == "&&") {
                    if (!eval(*n.kids[0], env).truthy()) return Value(false);
                    return Value(eval(*n.kids[1], env).truthy());
                }
                if (n.op == "||") {
                    if (eval(*n.kids[0], env).truthy()) return Value(true);
                    return Value(eval(*n.kids[1], env).truthy());
                }
                if (n.op == "??") {
                    Value a = eval(*n.kids[0], env);
                    return a.isNull() ? eval(*n.kids[1], env) : a;
                }
                Value a = eval(*n.kids[0], env);
                Value b = eval(*n.kids[1], env);
                return arith(n, n.op, a, b);
            }
            case NK::Assign: {
                Value v;
                if (n.op == "=") {
                    v = eval(*n.kids[1], env);
                } else {
                    Value cur = getTarget(*n.kids[0], env);
                    Value rhs = eval(*n.kids[1], env);
                    v = arith(n, n.op.substr(0, n.op.size() - 1), cur, rhs);
                }
                setTarget(*n.kids[0], env, v);
                return v;
            }
            case NK::Ternary: return eval(*n.kids[eval(*n.kids[0], env).truthy() ? 1 : 2], env);
            case NK::ArrayLit: {
                std::vector<Value> a;
                for (const NodeP& k : n.kids) a.push_back(eval(*k, env));
                return Value::array(a);
            }
            case NK::MapLit: {
                ValueMap m;
                for (size_t i = 0; i + 1 < n.kids.size(); i += 2) m[eval(*n.kids[i], env).str()] = eval(*n.kids[i + 1], env);
                return Value::mapOf(m);
            }
            case NK::ObjLit: {
                ValueMap m;
                for (size_t i = 0; i < n.names.size(); ++i) m[n.names[i]] = eval(*n.kids[i], env);
                return Value::object(m);
            }
            case NK::Func: {
                Value f = makeFunc(n, env);
                if (!n.str.empty()) env->vars[n.str] = f;
                return f;
            }
            case NK::Block: {
                auto inner = std::make_shared<Env>();
                inner->parent = env;
                Value lastV;
                for (const NodeP& k : n.kids) {
                    const Flow fl = exec(*k, inner);
                    if (fl != Flow::Normal) throw ScriptError("line " + std::to_string(k->line) + ": break/return inside an expression");
                    lastV = last;
                }
                return lastV;
            }
            case NK::If: {
                if (eval(*n.kids[0], env).truthy()) return eval(*n.kids[1], env);
                return n.kids.size() > 2 ? eval(*n.kids[2], env) : Value();
            }
            case NK::Switch: {
                Value v;
                switchStmt(n, env, &v);
                return v;
            }
            default: {
                const Flow fl = exec(n, env);
                (void)fl;
                return last;
            }
        }
    }

    Flow switchStmt(const Node& n, std::shared_ptr<Env>& env, Value* valueOut) {
        Value subject = eval(*n.kids[0], env);
        const Node* chosen = nullptr;
        const Node* deflt = nullptr;
        for (size_t i = 1; i < n.kids.size() && !chosen; ++i) {
            const Node& c = *n.kids[i];
            if (c.flag && c.kids.size() == 1) { deflt = &c; continue; }
            for (size_t j = 0; j + 1 < c.kids.size(); ++j)
                if (equalValues(subject, eval(*c.kids[j], env))) { chosen = &c; break; }
            if (!chosen && c.flag) deflt = &c;
        }
        if (!chosen) chosen = deflt;
        if (!chosen) return Flow::Normal;
        const Flow fl = exec(*chosen->kids.back(), env);
        if (valueOut) *valueOut = last;
        return fl;
    }

    Flow loopBody(const Node& body, std::shared_ptr<Env>& env, bool& stop) {
        tick();
        const Flow fl = exec(body, env);
        if (fl == Flow::Break) { stop = true; return Flow::Normal; }
        if (fl == Flow::Return) { stop = true; return Flow::Return; }
        return Flow::Normal;
    }

    Flow exec(const Node& n, std::shared_ptr<Env>& env) {
        switch (n.k) {
            case NK::Block: {
                auto inner = std::make_shared<Env>();
                inner->parent = env;
                // Function declarations are visible to the whole block.
                for (const NodeP& k : n.kids)
                    if (k->k == NK::Var && k->flag) inner->vars[k->names[0]] = makeFunc(*k->kids[0], inner);
                for (const NodeP& k : n.kids) {
                    const Flow fl = exec(*k, inner);
                    if (fl != Flow::Normal) return fl;
                }
                return Flow::Normal;
            }
            case NK::Var: {
                if (n.flag) {  // function declaration (already hoisted in blocks)
                    if (!env->vars.count(n.names[0])) env->vars[n.names[0]] = makeFunc(*n.kids[0], env);
                    last = Value();
                    return Flow::Normal;
                }
                for (size_t i = 0; i < n.names.size(); ++i) env->vars[n.names[i]] = eval(*n.kids[i], env);
                last = Value();
                return Flow::Normal;
            }
            case NK::If: {
                if (eval(*n.kids[0], env).truthy()) return exec(*n.kids[1], env);
                if (n.kids.size() > 2) return exec(*n.kids[2], env);
                return Flow::Normal;
            }
            case NK::While: {
                bool stop = false;
                while (!stop && eval(*n.kids[0], env).truthy()) {
                    const Flow fl = loopBody(*n.kids[1], env, stop);
                    if (fl == Flow::Return) return fl;
                }
                return Flow::Normal;
            }
            case NK::DoWhile: {
                bool stop = false;
                do {
                    const Flow fl = loopBody(*n.kids[0], env, stop);
                    if (fl == Flow::Return) return fl;
                } while (!stop && eval(*n.kids[1], env).truthy());
                return Flow::Normal;
            }
            case NK::For:
            case NK::ForKV: {
                auto loopEnv = std::make_shared<Env>();
                loopEnv->parent = env;
                bool stop = false;
                const Node& it = *n.kids[0];
                if (it.k == NK::Range) {
                    const double a = eval(*it.kids[0], env).num(), b = eval(*it.kids[1], env).num();
                    for (double i = a; i < b && !stop; i += 1) {
                        loopEnv->vars[n.names[0]] = Value(i);
                        if (loopBody(*n.kids[1], loopEnv, stop) == Flow::Return) return Flow::Return;
                    }
                    return Flow::Normal;
                }
                Value coll = eval(it, env);
                std::vector<std::pair<Value, Value>> items;  // key, value
                if (coll.t == T::Array) {
                    for (size_t i = 0; i < coll.arr->size(); ++i) items.push_back({Value(double(i)), (*coll.arr)[i]});
                } else if (coll.t == T::Map || coll.t == T::Object) {
                    for (auto& kv : *coll.map) items.push_back({Value(kv.first), kv.second});
                } else if (coll.t == T::Host) {
                    const double len = hostGet(ip, *coll.host, "length").num();
                    for (double i = 0; i < len; i += 1) items.push_back({Value(i), hostIndex(ip, *coll.host, Value(i))});
                } else if (coll.t == T::Str) {
                    for (size_t i = 0; i < coll.s.size(); ++i) items.push_back({Value(double(i)), Value(std::string(1, coll.s[i]))});
                } else if (!coll.isNull()) {
                    fail(n, "cannot iterate over this value");
                }
                for (auto& kv : items) {
                    if (stop) break;
                    if (n.k == NK::ForKV) {
                        loopEnv->vars[n.names[0]] = kv.first;
                        loopEnv->vars[n.names[1]] = kv.second;
                    } else {
                        loopEnv->vars[n.names[0]] = kv.second;
                    }
                    if (loopBody(*n.kids[1], loopEnv, stop) == Flow::Return) return Flow::Return;
                }
                return Flow::Normal;
            }
            case NK::Break: return Flow::Break;
            case NK::Continue: return Flow::Continue;
            case NK::Return:
                ret = n.kids.empty() ? Value() : eval(*n.kids[0], env);
                return Flow::Return;
            case NK::Switch: return switchStmt(n, env, nullptr);
            case NK::Throw: throw ThrownValue{eval(*n.kids[0], env)};
            case NK::Try: {
                try {
                    return exec(*n.kids[0], env);
                } catch (ThrownValue& t) {
                    auto ce = std::make_shared<Env>();
                    ce->parent = env;
                    ce->vars[n.names[0]] = t.v;
                    return exec(*n.kids[1], ce);
                } catch (ScriptError& e) {
                    if (std::string(e.what()).find("took too long") != std::string::npos) throw;
                    auto ce = std::make_shared<Env>();
                    ce->parent = env;
                    ce->vars[n.names[0]] = Value(std::string(e.what()));
                    return exec(*n.kids[1], ce);
                }
            }
            case NK::Nop: return Flow::Normal;
            default:
                last = eval(n, env);
                return Flow::Normal;
        }
    }
};

// ---------------------------------------------------------------- Interp --

Interp::Interp(scene::Scene& s, ScriptGame& g, ScriptHost* h, std::shared_ptr<Shared> sh)
    : sc(s), game(g), host(h), shared(std::move(sh)), globals_(std::make_shared<Env>()) {
    if (shared) shared->interps.push_back(this);
}

Interp::~Interp() {
    if (shared) shared->forget(this);
}

void Interp::setGlobal(const std::string& name, Value v) { globals_->vars[name] = std::move(v); }

Value Interp::getGlobal(const std::string& name) const {
    auto it = globals_->vars.find(name);
    return it == globals_->vars.end() ? Value() : it->second;
}

bool Interp::hasFunction(const std::string& fn) const {
    auto it = globals_->vars.find(fn);
    return it != globals_->vars.end() && it->second.t == T::Func;
}

bool Interp::run(const std::string& code, const std::string& name, Value& result, std::string& err) {
    result = Value();
    try {
        auto prog = std::make_shared<Program>();
        Parser p(lex(code));
        prog->root = p.program();
        programs_.push_back(prog);
        Evaluator ev{*this, prog, {}, {}, 0, nowSec()};
        // Top-level declarations go into the globals so later calls see them.
        const Node& root = *prog->root;
        for (const NodeP& k : root.kids)
            if (k->k == NK::Var && k->flag) globals_->vars[k->names[0]] = ev.makeFunc(*k->kids[0], globals_);
        for (const NodeP& k : root.kids) {
            const Flow fl = ev.exec(*k, globals_);
            if (fl == Flow::Return) { result = ev.ret; return true; }
            result = ev.last;
        }
        return true;
    } catch (ScriptError& e) {
        err = (name.empty() ? "" : name + ": ") + e.what();
    } catch (ThrownValue& t) {
        err = (name.empty() ? "" : name + ": ") + "uncaught throw: " + t.v.str();
    } catch (std::exception& e) {
        err = (name.empty() ? "" : name + ": ") + e.what();
    }
    return false;
}

Value Interp::callValue(const Value& f, std::vector<Value>& args) {
    auto prog = programs_.empty() ? std::make_shared<Program>() : programs_.back();
    Evaluator ev{*this, prog, {}, {}, 0, nowSec()};
    return ev.callFunc(f, args);
}

bool Interp::call(const std::string& fn, std::vector<Value> args, Value& result, std::string& err, bool* found) {
    result = Value();
    auto it = globals_->vars.find(fn);
    if (it == globals_->vars.end() || it->second.t != T::Func) {
        if (found) *found = false;
        return true;
    }
    if (found) *found = true;
    try {
        Value f = it->second;
        result = callValue(f, args);
        return true;
    } catch (ScriptError& e) {
        err = scriptName + " (" + fn + "): " + e.what();
    } catch (ThrownValue& t) {
        err = scriptName + " (" + fn + "): uncaught throw: " + t.v.str();
    } catch (std::exception& e) {
        err = scriptName + " (" + fn + "): " + e.what();
    }
    return false;
}

// ---------------------------------------------------------------- Shared --

void Shared::forget(Interp* ip) {
    interps.erase(std::remove(interps.begin(), interps.end(), ip), interps.end());
    for (auto it = callbacks.begin(); it != callbacks.end();)
        it = it->second.owner == ip ? callbacks.erase(it) : std::next(it);
    numTweens.erase(std::remove_if(numTweens.begin(), numTweens.end(), [&](const NumTween& t) { return t.owner == ip; }),
                    numTweens.end());
}

void Shared::update(double dt) {
    // Callbacks may add tweens: iterate over a snapshot of indexes.
    for (size_t i = 0; i < numTweens.size();) {
        NumTween& tw = numTweens[i];
        if (tw.delay > 0) { tw.delay -= dt; ++i; continue; }
        tw.t += dt;
        const double k = psych::ease(tw.ease, tw.dur > 0 ? std::min(1.0, tw.t / tw.dur) : 1.0);
        const double v = tw.from + (tw.to - tw.from) * k;
        Interp* owner = tw.owner;
        Value upd = tw.update, done = tw.complete;
        const bool finished = tw.t >= tw.dur;
        if (finished) numTweens.erase(numTweens.begin() + std::ptrdiff_t(i));
        else ++i;
        std::string err;
        if (owner && upd.t == T::Func) {
            std::vector<Value> a{Value(v)};
            try { owner->callValue(upd, a); } catch (std::exception& e) { err = e.what(); } catch (ThrownValue& t) { err = t.v.str(); }
        }
        if (owner && finished && done.t == T::Func) {
            std::vector<Value> a{Value()};
            try { owner->callValue(done, a); } catch (std::exception& e) { err = e.what(); } catch (ThrownValue& t) { err = t.v.str(); }
        }
        if (!err.empty() && report) report((owner ? owner->scriptName + ": " : std::string()) + err);
    }
}

bool Shared::complete(const std::string& tag, int loopsLeft) {
    auto it = callbacks.find(tag);
    if (it == callbacks.end()) return false;
    Callback cb = it->second;
    if (loopsLeft <= 0) callbacks.erase(it);
    if (cb.owner && cb.fn.t == T::Func) {
        std::vector<Value> a{cb.arg};
        std::string err;
        try { cb.owner->callValue(cb.fn, a); } catch (std::exception& e) { err = e.what(); } catch (ThrownValue& t) { err = t.v.str(); }
        if (!err.empty() && report) report(cb.owner->scriptName + ": " + err);
    }
    return true;
}

}  // namespace gh::hx
