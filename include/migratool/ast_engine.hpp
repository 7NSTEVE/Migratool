#pragma once
#include "migratool/core.hpp"
#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace mt::ast {

// ============================================================================
// H-03: Normalización estricta de familias y dialectos de lenguaje
// Prohibida cualquier comparación por prefijo (C23 != C++23 != C#).
// ============================================================================
struct LanguageSpec {
    std::string family;   // "C", "C++", "C#", "C/C++", "Python", "Rust", "TypeScript", "JavaScript", "Go", "Java", "Kotlin", "SQL", "WebAssembly", etc.
    std::string dialect;  // "C23", "C11", "C++23", "C++20", "C#", "TSX", "JSX", etc.
    bool is_header = false;
};

inline std::string trim_lower(std::string_view sv) {
    size_t b = 0, e = sv.size();
    while (b < e && std::isspace(static_cast<unsigned char>(sv[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(sv[e - 1]))) --e;
    std::string out(sv.substr(b, e - b));
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

inline LanguageSpec parse_language_spec(std::string_view raw) {
    std::string s = trim_lower(raw);
    if (s.empty()) return {"", "", false};

    if (s == "c#" || s == "csharp" || s == "cs" || s == "c-sharp" || s == "dotnet-csharp" ||
        s == "c#10" || s == "c#11" || s == "c#12" || s == "c#13") {
        return {"C#", "C#", false};
    }
    if (s == "c++" || s == "cpp" || s == "cxx" || s == "cc" ||
        s == "c++98" || s == "c++03" || s == "c++11" || s == "c++14" ||
        s == "c++17" || s == "c++20" || s == "c++23" || s == "c++26" ||
        s == "gnu++17" || s == "gnu++20" || s == "gnu++23") {
        std::string dial = "C++";
        if (s.find("23") != std::string::npos) dial = "C++23";
        else if (s.find("20") != std::string::npos) dial = "C++20";
        else if (s.find("17") != std::string::npos) dial = "C++17";
        else if (s.find("14") != std::string::npos) dial = "C++14";
        else if (s.find("11") != std::string::npos) dial = "C++11";
        return {"C++", dial, false};
    }
    if (s == "c" || s == "c89" || s == "c90" || s == "c99" ||
        s == "c11" || s == "c17" || s == "c18" || s == "c23" ||
        s == "iso-c" || s == "gnu99" || s == "gnu11" || s == "gnu17" || s == "gnu23") {
        std::string dial = "C";
        if (s.find("23") != std::string::npos) dial = "C23";
        else if (s.find("17") != std::string::npos || s.find("18") != std::string::npos) dial = "C17";
        else if (s.find("11") != std::string::npos) dial = "C11";
        else if (s.find("99") != std::string::npos) dial = "C99";
        else if (s.find("89") != std::string::npos || s.find("90") != std::string::npos) dial = "C89";
        return {"C", dial, false};
    }
    if (s == "c/c++" || s == "c_cpp_header") {
        return {"C/C++", "C/C++", true};
    }
    if (s == "objective-c" || s == "objc") return {"Objective-C", "Objective-C", false};
    if (s == "objective-c++" || s == "objcpp") return {"Objective-C++", "Objective-C++", false};
    if (s == "f#" || s == "fsharp" || s == "fs") return {"F#", "F#", false};
    if (s == "vb" || s == "vb.net" || s == "visualbasic") return {"VB.NET", "VB.NET", false};
    if (s == "python" || s == "py" || s == "python3") return {"Python", "Python", false};
    if (s == "rust" || s == "rs" || s == "rust2021" || s == "rust2024") return {"Rust", "Rust", false};
    if (s == "typescript" || s == "ts") return {"TypeScript", "TypeScript", false};
    if (s == "tsx") return {"TypeScript", "TSX", false};
    if (s == "javascript" || s == "js" || s == "mjs" || s == "cjs" || s == "ecmascript") return {"JavaScript", "JavaScript", false};
    if (s == "jsx") return {"JavaScript", "JSX", false};
    if (s == "typescript/javascript" || s == "js/ts") return {"TypeScript/JavaScript", "TypeScript/JavaScript", false};
    if (s == "go" || s == "golang") return {"Go", "Go", false};
    if (s == "java") return {"Java", "Java", false};
    if (s == "kotlin" || s == "kt" || s == "kts") return {"Kotlin", "Kotlin", false};
    if (s == "java/kotlin") return {"Java/Kotlin", "Java/Kotlin", false};
    if (s == "sql" || s == "sqlite" || s == "tsql" || s == "plsql" || s == "postgres" || s == "postgresql") return {"SQL", "SQL", false};
    if (s == "webassembly" || s == "wasm" || s == "wat") return {"WebAssembly", "WebAssembly", false};
    if (s == "swift") return {"Swift", "Swift", false};
    if (s == "powershell" || s == "ps1" || s == "psm1" || s == "psd1" || s == "pwsh") return {"PowerShell", "PowerShell", false};
    if (s == "shell" || s == "sh" || s == "bash" || s == "zsh") return {"Shell", "Shell", false};
    if (s == "batch" || s == "bat" || s == "cmd") return {"Batch", "Batch", false};
    if (s == "cmake") return {"CMake", "CMake", false};
    if (s == "dart") return {"Dart", "Dart", false};
    if (s == "lua") return {"Lua", "Lua", false};
    if (s == "ruby" || s == "rb") return {"Ruby", "Ruby", false};
    if (s == "php") return {"PHP", "PHP", false};
    return {s, s, false};
}

inline bool language_matches(std::string_view file_lang_raw, std::string_view query_lang_raw, bool strict_target = false) {
    auto f = parse_language_spec(file_lang_raw);
    auto q = parse_language_spec(query_lang_raw);
    if (q.family.empty()) return true;
    if (f.family.empty()) return false;
    if (f.family == q.family) return true;
    if (q.family == "TypeScript/JavaScript" && (f.family == "TypeScript" || f.family == "JavaScript")) return true;
    if (q.family == "Java/Kotlin" && (f.family == "Java" || f.family == "Kotlin")) return true;
    if (q.family == "C/C++" && (f.family == "C" || f.family == "C++" || f.family == "C/C++")) return true;
    if (!strict_target && f.is_header && (q.family == "C" || q.family == "C++")) return true;
    return false;
}

// ============================================================================
// H-01 & H-02: Lexer + Parser AST Recursivo Descendente + Matcher Estructural
// ============================================================================
enum class TokenKind {
    Identifier,
    Keyword,
    MetavarSingle,  // $VAR
    MetavarMulti,   // $$$VAR o $$$
    NumberLiteral,
    StringLiteral,
    CharLiteral,
    Punctuator,
    Operator,
    Comment,
    Newline,
    EndOfFile
};

struct Token {
    TokenKind kind = TokenKind::EndOfFile;
    std::string text;
    size_t offset = 0;
    size_t end_offset = 0;
    int line = 1;
    int column = 1;
    int end_line = 1;
    int end_column = 1;
};

struct LineIndex {
    std::vector<size_t> line_starts;
    explicit LineIndex(std::string_view text) {
        line_starts.push_back(0);
        for (size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '\n') line_starts.push_back(i + 1);
        }
    }
    std::pair<int, int> pos(size_t offset) const {
        auto it = std::upper_bound(line_starts.begin(), line_starts.end(), offset);
        size_t idx = (it == line_starts.begin()) ? 0 : static_cast<size_t>(std::distance(line_starts.begin(), it) - 1);
        return {static_cast<int>(idx + 1), static_cast<int>(offset - line_starts[idx] + 1)};
    }
};

inline bool is_ident_start(char c) {
    auto uc = static_cast<unsigned char>(c);
    return std::isalpha(uc) || c == '_' || uc >= 0x80;
}

inline bool is_ident_cont(char c) {
    auto uc = static_cast<unsigned char>(c);
    return std::isalnum(uc) || c == '_' || uc >= 0x80;
}

inline std::vector<Token> lex_source(std::string_view src, std::string_view lang_family, bool allow_metavars = false, bool keep_comments = false) {
    LineIndex lidx(src);
    std::vector<Token> tokens;
    size_t i = 0, n = src.size();
    if (n >= 3 && static_cast<unsigned char>(src[0]) == 0xEF &&
        static_cast<unsigned char>(src[1]) == 0xBB && static_cast<unsigned char>(src[2]) == 0xBF) {
        i = 3;
    }

    auto emit = [&](TokenKind k, size_t start, size_t end) {
        if (k == TokenKind::Comment && !keep_comments) return;
        auto [sl, sc] = lidx.pos(start);
        auto [el, ec] = lidx.pos(end);
        tokens.push_back(Token{k, std::string(src.substr(start, end - start)), start, end, sl, sc, el, ec});
    };

    const bool is_py = (lang_family == "Python");
    const bool is_hash_comment = (is_py || lang_family == "PowerShell" || lang_family == "Shell" ||
                                  lang_family == "CMake" || lang_family == "Ruby");
    const bool is_rs = (lang_family == "Rust");
    const bool is_cs = (lang_family == "C#");
    const bool is_cpp = (lang_family == "C++" || lang_family == "C" || lang_family == "C/C++");
    const bool is_sql = (lang_family == "SQL" || lang_family == "Lua");

    while (i < n) {
        char c = src[i];
        if (c == ' ' || c == '\t' || c == '\r') { ++i; continue; }
        if (c == '\n') { emit(TokenKind::Newline, i, i + 1); ++i; continue; }

        if (!is_hash_comment && !is_sql && c == '/' && i + 1 < n && src[i + 1] == '/') {
            size_t start = i; i += 2;
            while (i < n && src[i] != '\n') ++i;
            emit(TokenKind::Comment, start, i);
            continue;
        }
        if (is_hash_comment && c == '#') {
            size_t start = i; ++i;
            while (i < n && src[i] != '\n') ++i;
            emit(TokenKind::Comment, start, i);
            continue;
        }
        if (is_sql && c == '-' && i + 1 < n && src[i + 1] == '-') {
            size_t start = i; i += 2;
            while (i < n && src[i] != '\n') ++i;
            emit(TokenKind::Comment, start, i);
            continue;
        }
        if (!is_py && c == '/' && i + 1 < n && src[i + 1] == '*') {
            size_t start = i; i += 2;
            int depth = 1;
            while (i < n && depth > 0) {
                if (is_rs && src[i] == '/' && i + 1 < n && src[i + 1] == '*') { ++depth; i += 2; }
                else if (src[i] == '*' && i + 1 < n && src[i + 1] == '/') { --depth; i += 2; }
                else ++i;
            }
            emit(TokenKind::Comment, start, i);
            continue;
        }

        if (allow_metavars && c == '$') {
            size_t start = i;
            if (i + 2 < n && src[i + 1] == '$' && src[i + 2] == '$') {
                i += 3;
                while (i < n && is_ident_cont(src[i])) ++i;
                emit(TokenKind::MetavarMulti, start, i);
                continue;
            }
            if (i + 1 < n && (is_ident_start(src[i + 1]) || std::isdigit(static_cast<unsigned char>(src[i + 1])))) {
                ++i;
                while (i < n && is_ident_cont(src[i])) ++i;
                emit(TokenKind::MetavarSingle, start, i);
                continue;
            }
        }

        if ((c == '"' || (is_py && c == '\'')) && i + 2 < n && src[i + 1] == c && src[i + 2] == c) {
            size_t start = i; char q = c; i += 3;
            while (i < n) {
                if (src[i] == '\\' && i + 1 < n) i += 2;
                else if (i + 2 < n && src[i] == q && src[i + 1] == q && src[i + 2] == q) { i += 3; break; }
                else ++i;
            }
            emit(TokenKind::StringLiteral, start, i);
            continue;
        }

        if (is_rs && c == 'r' && i + 1 < n && (src[i + 1] == '#' || src[i + 1] == '"')) {
            size_t start = i, j = i + 1, hashes = 0;
            while (j < n && src[j] == '#') { ++hashes; ++j; }
            if (j < n && src[j] == '"') {
                ++j;
                while (j < n) {
                    if (src[j] == '"') {
                        size_t k = j + 1, h = 0;
                        while (k < n && h < hashes && src[k] == '#') { ++h; ++k; }
                        if (h == hashes) { j = k; break; }
                    }
                    ++j;
                }
                i = j;
                emit(TokenKind::StringLiteral, start, i);
                continue;
            }
        }

        if (is_rs && c == '\'' && i + 1 < n && is_ident_start(src[i + 1])) {
            size_t j = i + 2;
            while (j < n && is_ident_cont(src[j])) ++j;
            if (j >= n || src[j] != '\'') {
                emit(TokenKind::Identifier, i, j);
                i = j;
                continue;
            }
        }

        if (is_cs && ((c == '@' && i + 1 < n && src[i + 1] == '"') ||
                      ((c == '$' || c == '@') && i + 2 < n && ((src[i] == '$' && src[i + 1] == '@') || (src[i] == '@' && src[i + 1] == '$')) && src[i + 2] == '"'))) {
            size_t start = i;
            i += (src[i + 1] == '"') ? 2 : 3;
            while (i < n) {
                if (src[i] == '"' && i + 1 < n && src[i + 1] == '"') i += 2;
                else if (src[i] == '"') { ++i; break; }
                else ++i;
            }
            emit(TokenKind::StringLiteral, start, i);
            continue;
        }

        if (is_cpp && c == 'R' && i + 1 < n && src[i + 1] == '"') {
            size_t start = i, delim_start = i + 2;
            size_t open_paren = src.find('(', delim_start);
            if (open_paren != std::string_view::npos && open_paren - delim_start <= 16) {
                std::string closing = ")" + std::string(src.substr(delim_start, open_paren - delim_start)) + "\"";
                size_t close_pos = src.find(closing, open_paren + 1);
                i = (close_pos == std::string_view::npos) ? n : (close_pos + closing.size());
                emit(TokenKind::StringLiteral, start, i);
                continue;
            }
        }

        if (c == '"' || c == '\'' || c == '`') {
            size_t start = i; char q = c; ++i;
            while (i < n) {
                if (q != '`' && src[i] == '\\' && i + 1 < n) i += 2;
                else if (is_sql && q == '\'' && src[i] == '\'' && i + 1 < n && src[i + 1] == '\'') i += 2;
                else if (src[i] == q) { ++i; break; }
                else if (q != '`' && src[i] == '\n' && !is_sql) break;
                else ++i;
            }
            emit(q == '\'' && !is_py && !is_sql ? TokenKind::CharLiteral : TokenKind::StringLiteral, start, i);
            continue;
        }

        if (is_py && (c == 'f' || c == 'F' || c == 'r' || c == 'R' || c == 'b' || c == 'B')) {
            size_t j = i + 1;
            if (j < n && (src[j] == 'f' || src[j] == 'F' || src[j] == 'r' || src[j] == 'R')) ++j;
            if (j < n && (src[j] == '"' || src[j] == '\'')) {
                size_t start = i; char q = src[j];
                if (j + 2 < n && src[j + 1] == q && src[j + 2] == q) {
                    i = j + 3;
                    while (i < n) {
                        if (src[i] == '\\' && i + 1 < n) i += 2;
                        else if (i + 2 < n && src[i] == q && src[i + 1] == q && src[i + 2] == q) { i += 3; break; }
                        else ++i;
                    }
                } else {
                    i = j + 1;
                    while (i < n && src[i] != '\n') {
                        if (src[i] == '\\' && i + 1 < n) i += 2;
                        else if (src[i] == q) { ++i; break; }
                        else ++i;
                    }
                }
                emit(TokenKind::StringLiteral, start, i);
                continue;
            }
        }

        if (std::isdigit(static_cast<unsigned char>(c))) {
            size_t start = i; ++i;
            while (i < n && (std::isalnum(static_cast<unsigned char>(src[i])) || src[i] == '.' || src[i] == '_')) ++i;
            emit(TokenKind::NumberLiteral, start, i);
            continue;
        }

        if (is_ident_start(c)) {
            size_t start = i; ++i;
            while (i < n && is_ident_cont(src[i])) ++i;
            emit(TokenKind::Identifier, start, i);
            continue;
        }

        if (i + 1 < n) {
            std::string_view two = src.substr(i, 2);
            if (two == "::" || two == "->" || two == "=>" || two == "==" || two == "!=" ||
                two == "<=" || two == ">=" || two == "&&" || two == "||" || two == "?.") {
                emit(TokenKind::Operator, i, i + 2);
                i += 2;
                continue;
            }
        }

        if (c == '(' || c == ')' || c == '{' || c == '}' || c == '[' || c == ']' ||
            c == ',' || c == ';' || c == ':' || c == '.') {
            emit(TokenKind::Punctuator, i, i + 1);
            ++i;
            continue;
        }

        emit(TokenKind::Operator, i, i + 1);
        ++i;
    }

    auto [el, ec] = lidx.pos(n);
    tokens.push_back(Token{TokenKind::EndOfFile, "", n, n, el, ec, el, ec});
    return tokens;
}

enum class AstNodeKind {
    Root,
    CallExpr,       // children[0] = callee, children[1..k] = argumentos AST
    MemberExpr,     // children[0] = receptor, children[1] = miembro, subkind = "." | "::" | "->" | "?."
    GenericExpr,    // children[0] = base, children[1..k] = type args
    ParenExpr,      // ( ... )
    BracketExpr,    // [ ... ]
    BraceBlock,     // { ... }
    BinaryExpr,     // subkind = op, children[0]=lhs, children[1]=rhs
    Sequence,       // secuencia de nodos
    Identifier,
    Literal,        // StringLiteral, NumberLiteral, CharLiteral
    MetavarSingle,  // $VAR
    MetavarMulti,   // $$$VAR
    PunctOrOp
};

struct AstNode {
    AstNodeKind kind = AstNodeKind::Identifier;
    std::string text;
    std::string subkind;
    size_t offset = 0;
    size_t end_offset = 0;
    int line = 1;
    int column = 1;
    int end_line = 1;
    int end_column = 1;
    std::vector<AstNode> children;
};

class AstParser {
    const std::vector<Token>& toks_;
    size_t pos_ = 0;
    int depth_ = 0;
    int paren_nesting_ = 0;
    static constexpr int MAX_AST_DEPTH = 64;
    static constexpr size_t MAX_BINARY_CHAIN = 256;

    struct DepthGuard {
        int& d;
        explicit DepthGuard(int& depth_ref) : d(depth_ref) { ++d; }
        ~DepthGuard() { --d; }
    };

    bool at_end() const { return pos_ >= toks_.size() || toks_[pos_].kind == TokenKind::EndOfFile; }
    const Token& peek(size_t ahead = 0) const {
        size_t idx = std::min(pos_ + ahead, toks_.size() - 1);
        return toks_[idx];
    }
    const Token& consume() { return toks_[pos_++]; }
    void skip_newlines() {
        while (!at_end() && peek().kind == TokenKind::Newline) ++pos_;
    }

    bool is_generic_angle_open() const {
        if (at_end() || peek().text != "<") return false;
        size_t i = pos_ + 1;
        int depth = 1;
        size_t scanned = 0;
        while (i < toks_.size() && toks_[i].kind != TokenKind::EndOfFile && depth > 0 && scanned < 64) {
            const auto& t = toks_[i];
            if (t.kind == TokenKind::Newline) { ++i; continue; }
            if (t.text == "<") { ++depth; }
            else if (t.text == ">") {
                --depth;
                if (depth == 0) {
                    if (i + 1 < toks_.size()) {
                        const auto& next = toks_[i + 1];
                        if (next.text == "(" || next.text == "::" || next.text == "." || next.text == ")" || next.text == ",")
                            return true;
                    }
                    return false;
                }
            } else if (t.text == ";" || t.text == "{" || t.text == "}" || t.text == "(" || t.text == ")" ||
                       t.text == "&&" || t.text == "||" || t.text == "==" || t.text == "!=" || t.text == "<=" || t.text == ">=") {
                return false;
            }
            ++i;
            ++scanned;
        }
        return false;
    }

public:
    explicit AstParser(const std::vector<Token>& toks) : toks_(toks) {}

    AstNode parse_root() {
        AstNode root;
        root.kind = AstNodeKind::Root;
        if (!toks_.empty()) {
            root.offset = toks_.front().offset;
            root.line = toks_.front().line;
            root.column = toks_.front().column;
        }
        while (!at_end()) {
            skip_newlines();
            if (at_end()) break;
            size_t before = pos_;
            root.children.push_back(parse_statement_or_expr());
            if (pos_ == before && !at_end()) consume();
        }
        if (!root.children.empty()) {
            root.end_offset = root.children.back().end_offset;
            root.end_line = root.children.back().end_line;
            root.end_column = root.children.back().end_column;
        }
        return root;
    }

    AstNode parse_statement_or_expr() {
        skip_newlines();
        auto lhs = parse_postfix_or_binary();
        if (!at_end() && peek().text == ";") consume();
        return lhs;
    }

    AstNode parse_argument_expr(std::string_view close_delim = ")") {
        skip_newlines();
        std::vector<AstNode> parts;
        while (!at_end()) {
            skip_newlines();
            if (at_end()) break;
            const auto& t = peek();
            if (t.text == "," || t.text == close_delim || t.text == ")" || t.text == "]" || t.text == "}" || t.text == ";") break;
            size_t before = pos_;
            parts.push_back(parse_postfix_or_binary());
            if (pos_ == before && !at_end()) {
                auto tok = consume();
                parts.push_back(AstNode{AstNodeKind::PunctOrOp, tok.text, "", tok.offset, tok.end_offset, tok.line, tok.column, tok.end_line, tok.end_column, {}});
            }
        }
        if (parts.empty()) {
            const auto& t = peek();
            return AstNode{AstNodeKind::Sequence, "", "", t.offset, t.offset, t.line, t.column, t.line, t.column, {}};
        }
        if (parts.size() == 1) return std::move(parts[0]);
        AstNode seq;
        seq.kind = AstNodeKind::Sequence;
        seq.offset = parts.front().offset;
        seq.line = parts.front().line;
        seq.column = parts.front().column;
        seq.end_offset = parts.back().end_offset;
        seq.end_line = parts.back().end_line;
        seq.end_column = parts.back().end_column;
        seq.children = std::move(parts);
        return seq;
    }

    static int binary_precedence(std::string_view op) {
        if (op == "=" || op == "+=" || op == "-=" || op == "*=" || op == "/=" || op == "%=" || op == ":=") return 1;
        if (op == "?" || op == "=>") return 2;
        if (op == "||" || op == "or") return 3;
        if (op == "&&" || op == "and") return 4;
        if (op == "|" || op == "^" || op == "&") return 5;
        if (op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">=") return 6;
        if (op == "+" || op == "-") return 7;
        if (op == "*" || op == "/" || op == "%") return 8;
        return 6;
    }
    static bool is_right_assoc(std::string_view op) {
        return op == "=" || op == "+=" || op == "-=" || op == "*=" || op == "/=" || op == "%=" || op == ":=";
    }

    AstNode parse_postfix_or_binary(int min_prec = 1) {
        if (depth_ >= MAX_AST_DEPTH) {
            const auto& t = consume();
            return AstNode{AstNodeKind::PunctOrOp, t.text, "", t.offset, t.end_offset, t.line, t.column, t.end_line, t.end_column, {}};
        }
        DepthGuard dg(depth_);
        auto left = parse_postfix();
        size_t chain_count = 0;
        while (!at_end() && chain_count < MAX_BINARY_CHAIN) {
            size_t saved = pos_;
            bool crossed_newline = false;
            while (!at_end() && peek().kind == TokenKind::Newline) {
                crossed_newline = true;
                ++pos_;
            }
            if (at_end()) { pos_ = saved; break; }
            // Outside parentheses/brackets, a newline terminates the statement/expression
            // unless the first token on the next line is an explicit continuation operator.
            if (crossed_newline && paren_nesting_ == 0) {
                pos_ = saved;
                break;
            }
            const auto& op = peek();
            if (op.kind == TokenKind::Operator && op.text != ";" && op.text != "," &&
                op.text != ")" && op.text != "]" && op.text != "}" && op.text != "#") {
                int prec = binary_precedence(op.text);
                if (prec < min_prec) { pos_ = saved; break; }
                auto op_tok = consume();
                skip_newlines();
                if (at_end()) break;
                int next_min = is_right_assoc(op_tok.text) ? prec : (prec + 1);
                auto right = parse_postfix_or_binary(next_min);
                AstNode bin;
                bin.kind = AstNodeKind::BinaryExpr;
                bin.subkind = op_tok.text;
                bin.text = op_tok.text;
                bin.offset = left.offset;
                bin.line = left.line;
                bin.column = left.column;
                bin.end_offset = right.end_offset;
                bin.end_line = right.end_line;
                bin.end_column = right.end_column;
                bin.children.push_back(std::move(left));
                bin.children.push_back(std::move(right));
                left = std::move(bin);
                ++chain_count;
                continue;
            }
            pos_ = saved;
            break;
        }
        return left;
    }

    AstNode parse_postfix() {
        if (depth_ >= MAX_AST_DEPTH) {
            const auto& t = consume();
            return AstNode{AstNodeKind::PunctOrOp, t.text, "", t.offset, t.end_offset, t.line, t.column, t.end_line, t.end_column, {}};
        }
        DepthGuard dg(depth_);
        AstNode node = parse_primary();
        while (!at_end()) {
            size_t saved = pos_;
            bool crossed_newline = false;
            while (!at_end() && peek().kind == TokenKind::Newline) {
                crossed_newline = true;
                ++pos_;
            }
            if (at_end()) { pos_ = saved; break; }
            const auto& t = peek();
            // Do not treat `[` or `(` on a new line as postfix subscript/call of the previous line
            if (crossed_newline && (t.text == "(" || t.text == "[" || t.text == "<")) {
                pos_ = saved;
                break;
            }

            if (t.text == "." || t.text == "::" || t.text == "->" || t.text == "?.") {
                auto op_tok = consume();
                skip_newlines();
                if (!at_end() && (peek().kind == TokenKind::Identifier || peek().kind == TokenKind::MetavarSingle)) {
                    auto mem_tok = consume();
                    AstNode mem;
                    mem.kind = AstNodeKind::MemberExpr;
                    mem.text = mem_tok.text;
                    mem.subkind = op_tok.text;
                    mem.offset = node.offset;
                    mem.line = node.line;
                    mem.column = node.column;
                    mem.end_offset = mem_tok.end_offset;
                    mem.end_line = mem_tok.end_line;
                    mem.end_column = mem_tok.end_column;
                    mem.children.push_back(std::move(node));
                    AstNode rhs{mem_tok.kind == TokenKind::MetavarSingle ? AstNodeKind::MetavarSingle : AstNodeKind::Identifier,
                                mem_tok.text, "", mem_tok.offset, mem_tok.end_offset, mem_tok.line, mem_tok.column, mem_tok.end_line, mem_tok.end_column, {}};
                    mem.children.push_back(std::move(rhs));
                    node = std::move(mem);
                    continue;
                }
                pos_ = saved;
                break;
            }

            if ((node.kind == AstNodeKind::Identifier || node.kind == AstNodeKind::MemberExpr) && is_generic_angle_open()) {
                consume(); // '<'
                AstNode gen;
                gen.kind = AstNodeKind::GenericExpr;
                gen.text = node.text;
                gen.offset = node.offset;
                gen.line = node.line;
                gen.column = node.column;
                gen.children.push_back(std::move(node));
                int depth = 1;
                while (!at_end() && depth > 0) {
                    size_t before_g = pos_;
                    if (peek().text == "<") { ++depth; consume(); }
                    else if (peek().text == ">") {
                        --depth;
                        auto close_tok = consume();
                        if (depth == 0) {
                            gen.end_offset = close_tok.end_offset;
                            gen.end_line = close_tok.end_line;
                            gen.end_column = close_tok.end_column;
                        }
                    } else if (peek().text == ",") {
                        consume();
                    } else if (peek().kind == TokenKind::Newline) {
                        consume();
                    } else {
                        gen.children.push_back(parse_postfix());
                    }
                    if (pos_ == before_g && !at_end()) consume();
                }
                node = std::move(gen);
                continue;
            }

            if (t.text == "(" && (node.kind == AstNodeKind::Identifier || node.kind == AstNodeKind::MemberExpr ||
                                  node.kind == AstNodeKind::GenericExpr || node.kind == AstNodeKind::MetavarSingle ||
                                  node.kind == AstNodeKind::CallExpr)) {
                // Detect declaration headers (def foo(...), function foo(...), fn foo(...), class Foo(...), etc.)
                bool is_decl_header = false;
                {
                    // Find token immediately preceding `node.offset` on same statement
                    size_t node_tok = pos_;
                    while (node_tok > 0 && toks_[node_tok - 1].offset >= node.offset) --node_tok;
                    size_t prev_tok = node_tok;
                    while (prev_tok > 0 && toks_[prev_tok - 1].kind == TokenKind::Newline) --prev_tok;
                    if (prev_tok > 0) {
                        const auto& pt = toks_[prev_tok - 1];
                        if (pt.line == node.line &&
                            (pt.text == "def" || pt.text == "function" || pt.text == "fn" || pt.text == "func" ||
                             pt.text == "class" || pt.text == "struct" || pt.text == "interface" ||
                             pt.text == "sub" || pt.text == "proc" || pt.text == "method")) {
                            is_decl_header = true;
                        }
                    }
                }
                consume(); // '('
                ++paren_nesting_;
                AstNode call;
                call.kind = AstNodeKind::CallExpr;
                call.text = node.text;
                if (is_decl_header) call.subkind = "decl";
                call.offset = node.offset;
                call.line = node.line;
                call.column = node.column;
                call.children.push_back(std::move(node));

                skip_newlines();
                while (!at_end() && peek().text != ")") {
                    size_t before_arg = pos_;
                    if (peek().kind == TokenKind::MetavarMulti) {
                        auto mv = consume();
                        call.children.push_back(AstNode{AstNodeKind::MetavarMulti, mv.text, "", mv.offset, mv.end_offset, mv.line, mv.column, mv.end_line, mv.end_column, {}});
                    } else {
                        call.children.push_back(parse_argument_expr(")"));
                    }
                    skip_newlines();
                    if (!at_end() && peek().text == ",") {
                        consume();
                        skip_newlines();
                    } else if (!at_end() && peek().text != ")") {
                        if (pos_ == before_arg) {
                            auto stray = consume();
                            call.children.push_back(AstNode{AstNodeKind::PunctOrOp, stray.text, "", stray.offset, stray.end_offset, stray.line, stray.column, stray.end_line, stray.end_column, {}});
                        }
                        skip_newlines();
                    } else {
                        break;
                    }
                }
                --paren_nesting_;
                if (!at_end() && peek().text == ")") {
                    auto close_tok = consume();
                    call.end_offset = close_tok.end_offset;
                    call.end_line = close_tok.end_line;
                    call.end_column = close_tok.end_column;
                    // Also detect C/C++/C#/Java/JS method definitions followed by '{' (excluding control keywords)
                    if (!is_decl_header && call.children[0].kind == AstNodeKind::Identifier) {
                        const auto& cn = call.children[0].text;
                        const bool is_ctrl = (cn == "if" || cn == "while" || cn == "for" || cn == "foreach" ||
                                              cn == "switch" || cn == "catch" || cn == "using" || cn == "lock" ||
                                              cn == "with" || cn == "synchronized" || cn == "match");
                        if (!is_ctrl) {
                            size_t la = pos_;
                            int skip_guard = 0;
                            while (la < toks_.size() && skip_guard < 8) {
                                const auto& lt = toks_[la];
                                if (lt.kind == TokenKind::Newline) { ++la; continue; }
                                if (lt.text == "const" || lt.text == "override" || lt.text == "noexcept" ||
                                    lt.text == "final" || lt.text == "volatile") { ++la; ++skip_guard; continue; }
                                if (lt.text == "{") {
                                    // Verify something preceded callee on the same line (return type / modifier) or member def
                                    size_t nt = pos_;
                                    while (nt > 0 && toks_[nt - 1].offset >= call.offset) --nt;
                                    if (nt > 0 && toks_[nt - 1].line == call.line &&
                                        (toks_[nt - 1].kind == TokenKind::Identifier || toks_[nt - 1].text == "*" ||
                                         toks_[nt - 1].text == "&" || toks_[nt - 1].text == ">" || toks_[nt - 1].text == "]")) {
                                        call.subkind = "decl";
                                    }
                                }
                                break;
                            }
                        }
                    }
                } else {
                    call.end_offset = peek().end_offset;
                    call.end_line = peek().end_line;
                    call.end_column = peek().end_column;
                }
                node = std::move(call);
                continue;
            }

            if (t.text == "[") {
                consume();
                ++paren_nesting_;
                AstNode idx_node;
                idx_node.kind = AstNodeKind::BracketExpr;
                idx_node.offset = node.offset;
                idx_node.line = node.line;
                idx_node.column = node.column;
                idx_node.children.push_back(std::move(node));
                skip_newlines();
                while (!at_end() && peek().text != "]") {
                    size_t before_idx = pos_;
                    idx_node.children.push_back(parse_argument_expr("]"));
                    if (!at_end() && peek().text == ",") consume();
                    else if (!at_end() && peek().text != "]" && pos_ == before_idx) {
                        auto stray = consume();
                        idx_node.children.push_back(AstNode{AstNodeKind::PunctOrOp, stray.text, "", stray.offset, stray.end_offset, stray.line, stray.column, stray.end_line, stray.end_column, {}});
                    }
                    skip_newlines();
                }
                --paren_nesting_;
                if (!at_end() && peek().text == "]") {
                    auto cl = consume();
                    idx_node.end_offset = cl.end_offset;
                    idx_node.end_line = cl.end_line;
                    idx_node.end_column = cl.end_column;
                }
                node = std::move(idx_node);
                continue;
            }

            pos_ = saved;
            break;
        }
        return node;
    }

    AstNode parse_primary() {
        skip_newlines();
        if (at_end()) {
            const auto& t = peek();
            return AstNode{AstNodeKind::Identifier, "", "", t.offset, t.end_offset, t.line, t.column, t.end_line, t.end_column, {}};
        }
        const auto& t = consume();
        if (t.kind == TokenKind::MetavarSingle) {
            return AstNode{AstNodeKind::MetavarSingle, t.text, "", t.offset, t.end_offset, t.line, t.column, t.end_line, t.end_column, {}};
        }
        if (t.kind == TokenKind::MetavarMulti) {
            return AstNode{AstNodeKind::MetavarMulti, t.text, "", t.offset, t.end_offset, t.line, t.column, t.end_line, t.end_column, {}};
        }
        if (t.kind == TokenKind::StringLiteral || t.kind == TokenKind::CharLiteral || t.kind == TokenKind::NumberLiteral) {
            std::string sk = (t.kind == TokenKind::StringLiteral) ? "string" : (t.kind == TokenKind::CharLiteral ? "char" : "number");
            return AstNode{AstNodeKind::Literal, t.text, sk, t.offset, t.end_offset, t.line, t.column, t.end_line, t.end_column, {}};
        }
        if (t.kind == TokenKind::Identifier || t.kind == TokenKind::Keyword) {
            return AstNode{AstNodeKind::Identifier, t.text, "", t.offset, t.end_offset, t.line, t.column, t.end_line, t.end_column, {}};
        }
        if (t.text == "(" && depth_ < MAX_AST_DEPTH) {
            ++paren_nesting_;
            AstNode p;
            p.kind = AstNodeKind::ParenExpr;
            p.offset = t.offset;
            p.line = t.line;
            p.column = t.column;
            skip_newlines();
            while (!at_end() && peek().text != ")") {
                size_t before_p = pos_;
                p.children.push_back(parse_argument_expr(")"));
                if (!at_end() && peek().text == ",") consume();
                else if (!at_end() && peek().text != ")" && pos_ == before_p) {
                    auto stray = consume();
                    p.children.push_back(AstNode{AstNodeKind::PunctOrOp, stray.text, "", stray.offset, stray.end_offset, stray.line, stray.column, stray.end_line, stray.end_column, {}});
                }
                skip_newlines();
            }
            --paren_nesting_;
            if (!at_end() && peek().text == ")") {
                auto cl = consume();
                p.end_offset = cl.end_offset;
                p.end_line = cl.end_line;
                p.end_column = cl.end_column;
            } else {
                p.end_offset = peek().end_offset;
            }
            return p;
        }
        if (t.text == "[" && depth_ < MAX_AST_DEPTH) {
            ++paren_nesting_;
            AstNode b;
            b.kind = AstNodeKind::BracketExpr;
            b.offset = t.offset;
            b.line = t.line;
            b.column = t.column;
            skip_newlines();
            while (!at_end() && peek().text != "]") {
                size_t before_b = pos_;
                b.children.push_back(parse_argument_expr("]"));
                if (!at_end() && peek().text == ",") consume();
                else if (!at_end() && peek().text != "]" && pos_ == before_b) {
                    auto stray = consume();
                    b.children.push_back(AstNode{AstNodeKind::PunctOrOp, stray.text, "", stray.offset, stray.end_offset, stray.line, stray.column, stray.end_line, stray.end_column, {}});
                }
                skip_newlines();
            }
            --paren_nesting_;
            if (!at_end() && peek().text == "]") {
                auto cl = consume();
                b.end_offset = cl.end_offset;
                b.end_line = cl.end_line;
                b.end_column = cl.end_column;
            } else {
                b.end_offset = peek().end_offset;
            }
            return b;
        }
        if (t.text == "{" && depth_ < MAX_AST_DEPTH) {
            AstNode b;
            b.kind = AstNodeKind::BraceBlock;
            b.offset = t.offset;
            b.line = t.line;
            b.column = t.column;
            skip_newlines();
            while (!at_end() && peek().text != "}") {
                size_t before_b = pos_;
                b.children.push_back(parse_statement_or_expr());
                if (pos_ == before_b && !at_end() && peek().text != "}") consume();
                skip_newlines();
            }
            if (!at_end() && peek().text == "}") {
                auto cl = consume();
                b.end_offset = cl.end_offset;
                b.end_line = cl.end_line;
                b.end_column = cl.end_column;
            } else {
                b.end_offset = peek().end_offset;
            }
            return b;
        }
        return AstNode{AstNodeKind::PunctOrOp, t.text, "", t.offset, t.end_offset, t.line, t.column, t.end_line, t.end_column, {}};
    }
};

struct AstBinding {
    std::string text;
    size_t offset = 0;
    size_t end_offset = 0;
    int line = 1;
    int column = 1;
    const AstNode* node_ptr = nullptr;
};

struct AstMatchResult {
    size_t offset = 0;
    size_t end_offset = 0;
    int line = 1;
    int column = 1;
    int end_line = 1;
    int end_column = 1;
    std::string matched_text;
    std::map<std::string, AstBinding> bindings;
};

// Comparación estructural canónica de dos subárboles AST (independiente de comentarios y espacios)
inline bool ast_nodes_equivalent(const AstNode& a, const AstNode& b) {
    if (a.kind != b.kind) return false;
    if (a.kind == AstNodeKind::Identifier || a.kind == AstNodeKind::Literal || a.kind == AstNodeKind::PunctOrOp) {
        return a.text == b.text;
    }
    if (a.subkind != b.subkind) return false;
    if (a.children.size() != b.children.size()) return false;
    for (size_t i = 0; i < a.children.size(); ++i) {
        if (!ast_nodes_equivalent(a.children[i], b.children[i])) return false;
    }
    return true;
}

inline bool match_ast_node(const AstNode& pattern, const AstNode& candidate, std::string_view src_text,
                           std::map<std::string, AstBinding>& bindings);

inline bool match_ast_node_list(const std::vector<AstNode>& pat_list, size_t pi,
                                const std::vector<AstNode>& cand_list, size_t ci,
                                std::string_view src_text,
                                std::map<std::string, AstBinding>& bindings) {
    if (pi == pat_list.size() && ci == cand_list.size()) return true;
    if (pi == pat_list.size()) return false;

    if (pat_list[pi].kind == AstNodeKind::MetavarMulti) {
        std::string var_name = pat_list[pi].text;
        // Probar todas las longitudes posibles de captura variádica $$$VAR
        for (size_t k = ci; k <= cand_list.size(); ++k) {
            auto saved = bindings;
            AstBinding b;
            if (k > ci) {
                b.offset = cand_list[ci].offset;
                b.end_offset = cand_list[k - 1].end_offset;
                b.line = cand_list[ci].line;
                b.column = cand_list[ci].column;
                b.text = std::string(src_text.substr(b.offset, b.end_offset - b.offset));
            }
            saved[var_name] = b;
            if (match_ast_node_list(pat_list, pi + 1, cand_list, k, src_text, saved)) {
                bindings = std::move(saved);
                return true;
            }
        }
        return false;
    }

    if (ci >= cand_list.size()) return false;
    auto saved = bindings;
    if (!match_ast_node(pat_list[pi], cand_list[ci], src_text, saved)) return false;
    if (!match_ast_node_list(pat_list, pi + 1, cand_list, ci + 1, src_text, saved)) return false;
    bindings = std::move(saved);
    return true;
}

inline bool match_callee(const AstNode& pat_callee, const AstNode& cand_callee, std::string_view src_text,
                         std::map<std::string, AstBinding>& bindings) {
    if (match_ast_node(pat_callee, cand_callee, src_text, bindings)) return true;
    // Si el patrón especifica un nombre simple `ExecuteNonQuery($SQL)` y el candidato es
    // `SqliteConnection.ExecuteNonQuery(...)` o `db->ExecuteNonQuery<int>(...)`:
    if (cand_callee.kind == AstNodeKind::GenericExpr && !cand_callee.children.empty() &&
        pat_callee.kind != AstNodeKind::GenericExpr) {
        if (match_callee(pat_callee, cand_callee.children[0], src_text, bindings)) return true;
    }
    if (pat_callee.kind == AstNodeKind::Identifier && cand_callee.kind == AstNodeKind::MemberExpr &&
        cand_callee.children.size() == 2) {
        return match_ast_node(pat_callee, cand_callee.children[1], src_text, bindings);
    }
    return false;
}

inline bool match_ast_node(const AstNode& pattern, const AstNode& candidate, std::string_view src_text,
                           std::map<std::string, AstBinding>& bindings) {
    if (pattern.kind == AstNodeKind::MetavarSingle) {
        std::string var_name = pattern.text;
        auto it = bindings.find(var_name);
        if (it != bindings.end()) {
            return it->second.node_ptr && ast_nodes_equivalent(*it->second.node_ptr, candidate);
        }
        AstBinding b;
        b.offset = candidate.offset;
        b.end_offset = candidate.end_offset;
        b.line = candidate.line;
        b.column = candidate.column;
        b.text = std::string(src_text.substr(b.offset, b.end_offset - b.offset));
        b.node_ptr = &candidate;
        bindings[var_name] = std::move(b);
        return true;
    }

    if (pattern.kind == AstNodeKind::CallExpr && candidate.kind == AstNodeKind::CallExpr) {
        if (pattern.subkind != "decl" && candidate.subkind == "decl") return false;
        if (pattern.subkind == "decl" && candidate.subkind != "decl") return false;
        if (pattern.children.empty() || candidate.children.empty()) return false;
        auto saved = bindings;
        if (!match_callee(pattern.children[0], candidate.children[0], src_text, saved)) return false;
        if (!match_ast_node_list(pattern.children, 1, candidate.children, 1, src_text, saved)) return false;
        bindings = std::move(saved);
        return true;
    }

    if (pattern.kind != candidate.kind) return false;

    if (pattern.kind == AstNodeKind::Identifier || pattern.kind == AstNodeKind::Literal || pattern.kind == AstNodeKind::PunctOrOp) {
        return pattern.text == candidate.text;
    }

    if (pattern.kind == AstNodeKind::MemberExpr || pattern.kind == AstNodeKind::BinaryExpr) {
        if (!pattern.subkind.empty() && pattern.subkind != candidate.subkind) return false;
    }

    auto saved = bindings;
    if (!match_ast_node_list(pattern.children, 0, candidate.children, 0, src_text, saved)) return false;
    bindings = std::move(saved);
    return true;
}

inline void collect_ast_matches(const AstNode& pat_node, const AstNode& current, std::string_view src_text,
                                std::vector<AstMatchResult>& out) {
    std::map<std::string, AstBinding> bindings;
    if (match_ast_node(pat_node, current, src_text, bindings)) {
        AstMatchResult m;
        m.offset = current.offset;
        m.end_offset = current.end_offset;
        m.line = current.line;
        m.column = current.column;
        m.end_line = current.end_line;
        m.end_column = current.end_column;
        m.matched_text = std::string(src_text.substr(m.offset, m.end_offset - m.offset));
        for (auto& kv : bindings) kv.second.node_ptr = nullptr;
        m.bindings = std::move(bindings);
        out.push_back(std::move(m));
    }
    for (const auto& child : current.children) {
        collect_ast_matches(pat_node, child, src_text, out);
    }
}

inline std::vector<AstMatchResult> find_ast_matches(std::string_view src_text, std::string_view pattern_text, std::string_view lang_family = "C#") {
    auto pat_toks = lex_source(pattern_text, lang_family, /*allow_metavars=*/true, /*keep_comments=*/false);
    AstParser pat_parser(pat_toks);
    auto pat_root = pat_parser.parse_root();
    if (pat_root.children.empty()) return {};
    const AstNode& pat_node = (pat_root.children.size() == 1) ? pat_root.children[0] : pat_root;

    auto src_toks = lex_source(src_text, lang_family, /*allow_metavars=*/false, /*keep_comments=*/false);
    AstParser src_parser(src_toks);
    auto src_root = src_parser.parse_root();

    std::vector<AstMatchResult> matches;
    collect_ast_matches(pat_node, src_root, src_text, matches);
    return matches;
}

// Reescritura estructural AST preservando comentarios y formato fuera del nodo coincidente
inline std::pair<std::string, std::vector<AstMatchResult>> rewrite_ast_pattern(
    std::string_view src_text, std::string_view pattern_text, std::string_view rewrite_template, std::string_view lang_family = "C#") {
    auto matches = find_ast_matches(src_text, pattern_text, lang_family);
    if (matches.empty()) return {std::string(src_text), {}};

    // Filtrar coincidencias solapadas (preferir nodo exterior) y aplicar de derecha a izquierda
    std::sort(matches.begin(), matches.end(), [](const AstMatchResult& a, const AstMatchResult& b) {
        if (a.offset != b.offset) return a.offset < b.offset;
        return a.end_offset > b.end_offset;
    });
    std::vector<AstMatchResult> non_overlapping;
    size_t last_end = 0;
    for (const auto& m : matches) {
        if (non_overlapping.empty() || m.offset >= last_end) {
            non_overlapping.push_back(m);
            last_end = m.end_offset;
        }
    }

    std::string out(src_text);
    for (auto it = non_overlapping.rbegin(); it != non_overlapping.rend(); ++it) {
        std::string expanded(rewrite_template);
        // Reemplazar metavariables de mayor a menor longitud de nombre ($$$VAR antes de $VAR)
        std::vector<std::pair<std::string, std::string>> vars;
        for (const auto& [k, v] : it->bindings) vars.push_back({k, v.text});
        std::sort(vars.begin(), vars.end(), [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
        for (const auto& [var_name, bound_val] : vars) {
            size_t pos = 0;
            while ((pos = expanded.find(var_name, pos)) != std::string::npos) {
                expanded.replace(pos, var_name.size(), bound_val);
                pos += bound_val.size();
            }
        }
        out.replace(it->offset, it->end_offset - it->offset, expanded);
    }
    return {std::move(out), std::move(non_overlapping)};
}

} // namespace mt::ast
