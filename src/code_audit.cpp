#include "migratool/code_audit.hpp"
#include "migratool/ast_engine.hpp"
#include "migratool/multiagent_coordinator.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mt {
namespace {

struct HandleGuard {
    HANDLE h = INVALID_HANDLE_VALUE;
    explicit HandleGuard(HANDLE v = INVALID_HANDLE_VALUE) : h(v) {}
    ~HandleGuard() {
        if (h != INVALID_HANDLE_VALUE && h != nullptr) {
            CloseHandle(h);
        }
    }
    HandleGuard(const HandleGuard&) = delete;
    HandleGuard& operator=(const HandleGuard&) = delete;
};

std::string trim_copy(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) {
        s.pop_back();
    }
    size_t i = 0;
    while (i < s.size() && (s[i] == '\r' || s[i] == '\n' || s[i] == ' ' || s[i] == '\t')) {
        ++i;
    }
    return s.substr(i);
}

std::string lower_ascii(std::string_view sv) {
    std::string out(sv);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

std::string upper_ascii(std::string_view sv) {
    std::string out(sv);
    for (char& c : out) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return out;
}

std::string norm_rel_path(std::string s) {
    for (char& c : s) {
        if (c == '\\') c = '/';
    }
    while (s.starts_with("./")) {
        s.erase(0, 2);
    }
    while (!s.empty() && s.front() == '/') {
        s.erase(s.begin());
    }
    while (s.size() > 1 && s.back() == '/') {
        s.pop_back();
    }
    return s;
}

std::string canonical_path_str(const fs::path& p) {
    if (p.empty()) return {};
    fs::path cp = canonical_win32_path(p);
    if (cp.empty()) {
        std::error_code ec;
        cp = p.is_absolute() ? p.lexically_normal() : fs::absolute(p, ec).lexically_normal();
    }
    std::string s = path_text(cp);
    for (char& c : s) {
        if (c == '\\') c = '/';
    }
    while (s.size() > 3 && s.back() == '/') {
        s.pop_back();
    }
    return s;
}

bool table_exists(Database& db, const std::string& name) {
    return db.integer("SELECT EXISTS(SELECT 1 FROM sqlite_master WHERE type IN('table','view') AND name=?)", {name}) != 0;
}

bool json_bool_or_int(const Json& obj, const char* key, bool default_val) {
    if (!obj.is_object() || !obj.contains(key) || obj[key].is_null()) return default_val;
    const auto& v = obj[key];
    if (v.is_boolean()) return v.get<bool>();
    if (v.is_number_integer() || v.is_number_unsigned()) return v.get<int64_t>() != 0;
    if (v.is_string()) {
        const std::string s = lower_ascii(v.get<std::string>());
        return s == "true" || s == "1" || s == "yes";
    }
    return default_val;
}

Json parse_json_safe(const std::string& raw, const Json& fallback) {
    if (raw.empty()) return fallback;
    try {
        return Json::parse(raw);
    } catch (...) {
        return fallback;
    }
}

std::string file_mtime_utc_iso(const fs::path& p) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(native_path(p).c_str(), GetFileExInfoStandard, &fad)) {
        return {};
    }
    SYSTEMTIME st_utc{};
    if (!FileTimeToSystemTime(&fad.ftLastWriteTime, &st_utc)) {
        return {};
    }
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
                  static_cast<unsigned>(st_utc.wYear),
                  static_cast<unsigned>(st_utc.wMonth),
                  static_cast<unsigned>(st_utc.wDay),
                  static_cast<unsigned>(st_utc.wHour),
                  static_cast<unsigned>(st_utc.wMinute),
                  static_cast<unsigned>(st_utc.wSecond),
                  static_cast<unsigned>(st_utc.wMilliseconds));
    return std::string(buf);
}

struct ProcResult {
    int exit_code = -1;
    bool timed_out = false;
    std::string output;
};

ProcResult run_isolated_exe(const fs::path& exe_path, const fs::path& cwd,
                            const std::vector<std::string>& args, DWORD timeout_ms = 10000) {
    ProcResult res;
    std::error_code ec;
    if (exe_path.empty() || !fs::is_regular_file(native_path(exe_path), ec)) {
        return res;
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE rpipe = nullptr;
    HANDLE wpipe = nullptr;
    if (!CreatePipe(&rpipe, &wpipe, &sa, 0)) return res;
    HandleGuard rg(rpipe), wg(wpipe);
    SetHandleInformation(rpipe, HANDLE_FLAG_INHERIT, 0);

    HandleGuard nul_in(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));

    HandleGuard job(CreateJobObjectW(nullptr, nullptr));
    if (job.h != INVALID_HANDLE_VALUE && job.h != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job.h, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    }

    std::wstring cmd = quote_arg(exe_path.wstring());
    for (const auto& a : args) {
        cmd += L" " + quote_arg(wide(a));
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wpipe;
    si.hStdError = wpipe;
    si.hStdInput = (nul_in.h != INVALID_HANDLE_VALUE) ? nul_in.h : nullptr;

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmd_buf(cmd.begin(), cmd.end());
    cmd_buf.push_back(L'\0');

    const fs::path clean_cwd = canonical_win32_path(cwd);
    const std::wstring cwd_w = (clean_cwd.empty() ? cwd : clean_cwd).wstring();

    if (!CreateProcessW(nullptr, cmd_buf.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                        cwd_w.empty() ? nullptr : cwd_w.c_str(), &si, &pi)) {
        return res;
    }
    CloseHandle(wpipe);
    wg.h = INVALID_HANDLE_VALUE;

    HandleGuard ph(pi.hProcess), th(pi.hThread);
    if (job.h != INVALID_HANDLE_VALUE && job.h != nullptr) {
        AssignProcessToJobObject(job.h, pi.hProcess);
    }
    ResumeThread(pi.hThread);

    std::string out;
    std::thread reader([&]() {
        std::array<char, 8192> buf{};
        DWORD got = 0;
        while (ReadFile(rpipe, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr) && got > 0) {
            if (out.size() < 4 * 1024 * 1024) {
                const size_t take = (std::min)(static_cast<size_t>(got), 4 * 1024 * 1024 - out.size());
                out.append(buf.data(), take);
            }
        }
    });

    if (WaitForSingleObject(pi.hProcess, timeout_ms) == WAIT_TIMEOUT) {
        res.timed_out = true;
        if (job.h != INVALID_HANDLE_VALUE && job.h != nullptr) {
            TerminateJobObject(job.h, 1);
        } else {
            TerminateProcess(pi.hProcess, 1);
        }
        WaitForSingleObject(pi.hProcess, 2000);
    }
    if (reader.joinable()) reader.join();

    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    res.exit_code = res.timed_out ? -1 : static_cast<int>(code);
    res.output = std::move(out);
    return res;
}

fs::path locate_git_exe() {
    wchar_t buf[MAX_PATH]{};
    DWORD n = SearchPathW(nullptr, L"git.exe", nullptr, MAX_PATH, buf, nullptr);
    if (n > 0 && n < MAX_PATH) {
        return fs::path(buf);
    }
    const std::array<const wchar_t*, 3> fallbacks = {
        L"D:\\DevTools\\Tools\\Git\\cmd\\git.exe",
        L"C:\\Program Files\\Git\\cmd\\git.exe",
        L"C:\\Program Files (x86)\\Git\\cmd\\git.exe"
    };
    std::error_code ec;
    for (const auto* cand : fallbacks) {
        if (fs::is_regular_file(cand, ec)) return fs::path(cand);
    }
    return {};
}

fs::path locate_clang_tidy_exe() {
    wchar_t buf[MAX_PATH]{};
    DWORD n = SearchPathW(nullptr, L"clang-tidy.exe", nullptr, MAX_PATH, buf, nullptr);
    if (n > 0 && n < MAX_PATH) {
        return fs::path(buf);
    }
    const std::array<const wchar_t*, 4> fallbacks = {
        L"D:\\DevTools\\LLVM\\bin\\clang-tidy.exe",
        L"D:\\DevTools\\msys64\\ucrt64\\bin\\clang-tidy.exe",
        L"C:\\Program Files\\LLVM\\bin\\clang-tidy.exe",
        L"C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Tools\\Llvm\\x64\\bin\\clang-tidy.exe"
    };
    std::error_code ec;
    for (const auto* cand : fallbacks) {
        if (fs::is_regular_file(cand, ec)) return fs::path(cand);
    }
    return {};
}

std::string query_git_head_commit(const fs::path& repo_dir) {
    wchar_t dis[16]{};
    if (GetEnvironmentVariableW(L"MIGRATOOL_DISABLE_GIT", dis, 16) > 0 && std::wstring_view(dis) == L"1") {
        return {};
    }
    const fs::path git_exe = locate_git_exe();
    if (git_exe.empty()) return {};
    const auto r = run_isolated_exe(git_exe, repo_dir, {"rev-parse", "HEAD"}, 3000);
    if (r.exit_code == 0) {
        return trim_copy(r.output);
    }
    return {};
}

bool should_skip_audit_dir(const std::string& dlow) {
    return dlow == ".git" || dlow == ".migratool" || dlow == "vcpkg_installed"
        || dlow == "node_modules" || dlow == "third_party" || dlow == "cmakefiles"
        || dlow == "_deps" || dlow == ".cache" || dlow == "cache" || dlow == "out"
        || dlow == "release" || dlow == "artifacts"
        || dlow == "build" || dlow.starts_with("build-") || dlow.starts_with("build_");
}

bool is_auditable_code_extension(const std::string& ext_low) {
    static const std::unordered_set<std::string> kCodeExts = {
        ".c", ".cpp", ".cc", ".cxx", ".c++",
        ".h", ".hpp", ".hh", ".hxx", ".inl", ".ipp",
        ".py", ".rs", ".cs", ".ts", ".tsx", ".js", ".jsx", ".go", ".java", ".kt"
    };
    return kCodeExts.contains(ext_low);
}

bool is_c_or_cpp_header_ext(const std::string& ext_low) {
    return ext_low == ".h" || ext_low == ".hpp" || ext_low == ".hh" || ext_low == ".hxx" || ext_low == ".inl" || ext_low == ".ipp";
}

bool is_c_or_cpp_source_ext(const std::string& ext_low) {
    return ext_low == ".c" || ext_low == ".cpp" || ext_low == ".cc" || ext_low == ".cxx" || ext_low == ".c++";
}

bool is_c_or_cpp_ext(const std::string& ext_low) {
    return is_c_or_cpp_source_ext(ext_low) || is_c_or_cpp_header_ext(ext_low);
}

struct CompileDbEntry {
    std::string command_or_args;
    std::string flags_sha256;
    bool forces_cpp = false;
    bool forces_c = false;
};

fs::path discover_compile_commands_path(const Project& p, const Json& args) {
    std::error_code ec;
    if (args.contains("compile_commands") && args["compile_commands"].is_string()) {
        const std::string raw = trim_copy(args["compile_commands"].get<std::string>());
        if (!raw.empty()) {
            fs::path cp(wide(raw));
            if (cp.is_relative()) cp = p.directory() / cp;
            if (fs::is_regular_file(native_path(cp), ec)) return cp;
        }
    }

    const std::array<fs::path, 3> candidates = {
        p.directory() / L"compile_commands.json",
        p.directory() / L"build" / L"compile_commands.json",
        p.directory() / L"build" / L"delivery" / L"compile_commands.json"
    };
    for (const auto& c : candidates) {
        if (fs::is_regular_file(native_path(c), ec)) return c;
    }

    for (const auto& r : p.config.value("roots", Json::array())) {
        if (r.is_object() && r.contains("compile_commands") && r["compile_commands"].is_string()) {
            fs::path cp(wide(r["compile_commands"].get<std::string>()));
            if (cp.is_relative()) cp = p.directory() / cp;
            if (fs::is_regular_file(native_path(cp), ec)) return cp;
        }
    }
    return {};
}

std::unordered_map<std::string, CompileDbEntry> load_compile_commands_map(const Project& p, const fs::path& cc_path) {
    std::unordered_map<std::string, CompileDbEntry> out;
    if (cc_path.empty()) return out;

    Json doc;
    try {
        doc = read_json(cc_path);
    } catch (...) {
        return out;
    }
    if (!doc.is_array()) return out;

    const fs::path proj_canon = canonical_win32_path(p.directory());
    std::error_code ec;

    for (const auto& item : doc) {
        if (!item.is_object()) continue;
        const std::string raw_file = item.value("file", std::string{});
        if (raw_file.empty()) continue;

        const std::string raw_dir = item.value("directory", std::string{});
        fs::path fp(wide(raw_file));
        if (fp.is_relative()) {
            if (!raw_dir.empty()) {
                fs::path dp(wide(raw_dir));
                if (dp.is_relative()) dp = p.directory() / dp;
                fp = dp / fp;
            } else {
                fp = p.directory() / fp;
            }
        }

        std::string flags;
        if (item.contains("command") && item["command"].is_string()) {
            flags = item["command"].get<std::string>();
        } else if (item.contains("arguments") && item["arguments"].is_array()) {
            for (const auto& a : item["arguments"]) {
                if (a.is_string()) {
                    if (!flags.empty()) flags.push_back(' ');
                    flags += a.get<std::string>();
                }
            }
        }

        const std::string low_flags = lower_ascii(flags);
        CompileDbEntry entry;
        entry.command_or_args = flags;
        entry.flags_sha256 = sha256(flags);
        entry.forces_cpp = (low_flags.find("-x c++") != std::string::npos ||
                            low_flags.find("/tp") != std::string::npos ||
                            low_flags.find("-tp") != std::string::npos ||
                            low_flags.find("c++23") != std::string::npos ||
                            low_flags.find("c++20") != std::string::npos ||
                            low_flags.find("c++17") != std::string::npos);
        entry.forces_c = (low_flags.find("-x c ") != std::string::npos ||
                          low_flags.find("/tc") != std::string::npos);

        // Register by normalized relative path and by canonical path
        std::string rel_norm = norm_rel_path(raw_file);
        const fs::path fp_canon = canonical_win32_path(fp);
        if (!fp_canon.empty() && !proj_canon.empty()) {
            const fs::path rel = fs::relative(fp_canon, proj_canon, ec);
            if (!ec && !rel.empty() && !path_text(rel).starts_with("..")) {
                rel_norm = norm_rel_path(path_text(rel));
            }
        }
        out[lower_ascii(rel_norm)] = entry;
        out[lower_ascii(norm_rel_path(raw_file))] = entry;
    }
    return out;
}

std::string classify_file_language(const fs::path& fp, const std::string& content, const CompileDbEntry* cc_entry) {
    const std::string ext_low = lower_ascii(path_text(fp.extension()));
    if (ext_low == ".c") {
        if (cc_entry && cc_entry->forces_cpp && !cc_entry->forces_c) {
            return "C++23";
        }
        return "C";
    }
    if (ext_low == ".cpp" || ext_low == ".cc" || ext_low == ".cxx" || ext_low == ".c++" ||
        ext_low == ".hpp" || ext_low == ".hh" || ext_low == ".hxx" || ext_low == ".inl" || ext_low == ".ipp") {
        return "C++23";
    }
    if (ext_low == ".h") {
        if (cc_entry && cc_entry->forces_c && !cc_entry->forces_cpp) {
            return "C";
        }
        if (cc_entry && cc_entry->forces_cpp) {
            return "C++23";
        }
        if (content.find("namespace ") != std::string::npos ||
            content.find("class ") != std::string::npos ||
            content.find("template<") != std::string::npos ||
            content.find("template <") != std::string::npos ||
            content.find("constexpr ") != std::string::npos ||
            content.find("nullptr") != std::string::npos ||
            content.find("std::") != std::string::npos) {
            return "C++23";
        }
        return "C";
    }
    if (ext_low == ".py") return "Python";
    if (ext_low == ".rs") return "Rust";
    if (ext_low == ".cs") return "C#";
    if (ext_low == ".ts" || ext_low == ".tsx") return "TypeScript";
    if (ext_low == ".js" || ext_low == ".jsx") return "JavaScript";
    if (ext_low == ".go") return "Go";
    if (ext_low == ".java") return "Java";
    if (ext_low == ".kt") return "Kotlin";
    return language(fp);
}

bool determine_is_test_file(const std::string& rel_path, const std::vector<std::pair<std::string, std::string>>& test_policies) {
    const std::string rel_low = lower_ascii(norm_rel_path(rel_path));
    for (const auto& [pat_low, match_kind] : test_policies) {
        if (pat_low.empty()) continue;
        if (match_kind == "prefix") {
            if (rel_low == pat_low || rel_low.starts_with(pat_low + "/")) return true;
        } else if (match_kind == "exact") {
            if (rel_low == pat_low) return true;
        } else {
            if (rel_low.find(pat_low) != std::string::npos) return true;
        }
    }
    if (rel_low.starts_with("tests/") || rel_low.starts_with("test/") ||
        rel_low.find("/tests/") != std::string::npos || rel_low.find("/test/") != std::string::npos) {
        return true;
    }
    const std::string fname = lower_ascii(path_text(fs::path(wide(rel_path)).filename()));
    return fname.find("_test.") != std::string::npos ||
           fname.find("_tests.") != std::string::npos ||
           fname.starts_with("test_");
}

std::vector<std::string> extract_include_Targets(const std::string& content) {
    std::vector<std::string> includes;
    std::istringstream iss(content);
    std::string line;
    while (std::getline(iss, line)) {
        size_t i = 0;
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        if (i >= line.size() || line[i] != '#') continue;
        ++i;
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        if (line.compare(i, 7, "include") != 0) continue;
        i += 7;
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        if (i >= line.size()) continue;
        const char open_ch = line[i];
        if (open_ch != '"' && open_ch != '<') continue;
        const char close_ch = (open_ch == '"') ? '"' : '>';
        const size_t close_pos = line.find(close_ch, i + 1);
        if (close_pos == std::string::npos || close_pos <= i + 1) continue;
        std::string target = norm_rel_path(line.substr(i + 1, close_pos - (i + 1)));
        if (!target.empty()) {
            includes.push_back(std::move(target));
        }
    }
    return includes;
}

struct ExtractedFunction {
    std::string func_key;
    std::string qualified_name;
    std::string display_name;
    std::string kind = "function";
    int start_line = 1;
    int end_line = 1;
    int line_count = 1;
    int cyclomatic_complexity = 1;
    int nesting_depth = 0;
    int param_count = 0;
};

struct ExtractedFinding {
    std::string finding_id;
    int line = 1;
    int col = 1;
    std::string function_name;
    std::string category;
    std::string rule_id;
    std::string severity;
    std::string triage_status;
    std::string analyzer_tool = "ast_audit";
    std::string message;
    std::string evidence_excerpt;
};

bool is_test_macro_name(std::string_view s) {
    return s == "TEST_CASE" || s == "TEST" || s == "TEST_F" || s == "TEST_P"
        || s == "BOOST_AUTO_TEST_CASE" || s == "SCENARIO";
}

bool is_control_or_nonfunc_keyword(std::string_view s) {
    static const std::unordered_set<std::string_view> kNonFuncs = {
        "if", "for", "while", "switch", "catch", "return", "sizeof", "alignof",
        "decltype", "static_assert", "noexcept", "requires", "__attribute__",
        "__declspec", "typeof", "defined", "elif", "except", "foreach",
        "synchronized", "lock", "using", "namespace", "class", "struct", "enum"
    };
    return kNonFuncs.contains(s);
}

std::string unquote_literal(std::string_view raw) {
    if (raw.size() >= 2 && (raw.front() == '"' || raw.front() == '\'' || raw.front() == '`') && raw.back() == raw.front()) {
        return std::string(raw.substr(1, raw.size() - 2));
    }
    if (raw.starts_with("R\"")) {
        const size_t open_p = raw.find('(');
        const size_t close_p = raw.rfind(')');
        if (open_p != std::string_view::npos && close_p != std::string_view::npos && close_p > open_p) {
            return std::string(raw.substr(open_p + 1, close_p - open_p - 1));
        }
    }
    return std::string(raw);
}

size_t find_matching_delim(const std::vector<ast::Token>& toks, size_t open_idx,
                           std::string_view open_tok, std::string_view close_tok) {
    if (open_idx >= toks.size() || toks[open_idx].text != open_tok) return toks.size();
    int depth = 1;
    for (size_t i = open_idx + 1; i < toks.size(); ++i) {
        if (toks[i].kind == ast::TokenKind::EndOfFile) break;
        if (toks[i].text == open_tok) {
            ++depth;
        } else if (toks[i].text == close_tok) {
            --depth;
            if (depth == 0) return i;
        }
    }
    return toks.size();
}

void compute_body_metrics(const std::vector<ast::Token>& toks, size_t open_brace, size_t close_brace,
                          int& out_cc, int& out_nesting) {
    int cc = 1;
    int cur_nest = 0;
    int max_nest = 0;
    for (size_t i = open_brace + 1; i < close_brace && i < toks.size(); ++i) {
        const auto& t = toks[i];
        if (t.kind == ast::TokenKind::Identifier) {
            if (t.text == "if" || t.text == "for" || t.text == "while" ||
                t.text == "case" || t.text == "catch" || t.text == "elif" ||
                t.text == "except" || t.text == "foreach") {
                ++cc;
            }
        } else if (t.kind == ast::TokenKind::Operator) {
            if (t.text == "?" || t.text == "&&" || t.text == "||") {
                ++cc;
            }
        } else if (t.kind == ast::TokenKind::Punctuator) {
            if (t.text == "{") {
                ++cur_nest;
                max_nest = (std::max)(max_nest, cur_nest);
            } else if (t.text == "}") {
                cur_nest = (std::max)(0, cur_nest - 1);
            }
        }
    }
    out_cc = cc;
    out_nesting = max_nest;
}

int count_params_in_parens(const std::vector<ast::Token>& toks, size_t open_paren, size_t close_paren) {
    if (close_paren <= open_paren + 1) return 0;
    if (close_paren == open_paren + 2 && toks[open_paren + 1].text == "void") return 0;
    int params = 1;
    int paren_d = 0;
    int angle_d = 0;
    int bracket_d = 0;
    for (size_t i = open_paren + 1; i < close_paren; ++i) {
        const auto& t = toks[i].text;
        if (t == "(") ++paren_d;
        else if (t == ")") paren_d = (std::max)(0, paren_d - 1);
        else if (t == "[") ++bracket_d;
        else if (t == "]") bracket_d = (std::max)(0, bracket_d - 1);
        else if (t == "<") ++angle_d;
        else if (t == ">") angle_d = (std::max)(0, angle_d - 1);
        else if (t == "," && paren_d == 0 && angle_d == 0 && bracket_d == 0) {
            ++params;
        }
    }
    return params;
}

std::string get_line_excerpt(const std::vector<std::string>& lines, int line_1based) {
    if (line_1based < 1 || static_cast<size_t>(line_1based) > lines.size()) return {};
    std::string s = trim_copy(lines[static_cast<size_t>(line_1based - 1)]);
    if (s.size() > 140) s.resize(140);
    return s;
}

std::vector<std::string> split_source_lines(const std::string& content) {
    std::vector<std::string> out;
    std::istringstream iss(content);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out.push_back(std::move(line));
    }
    return out;
}

bool looks_like_secret_literal(const std::string& unquoted, const std::string& prev_ident_low) {
    if (unquoted.size() >= 16) {
        if (unquoted.starts_with("sk-") ||
            unquoted.starts_with("ghp_") ||
            unquoted.starts_with("github_pat_") ||
            unquoted.starts_with("AKIA") ||
            unquoted.starts_with("AIza") ||
            unquoted.starts_with("xoxb-") ||
            unquoted.starts_with("xoxp-")) {
            return true;
        }
    }
    if (unquoted.size() >= 8 &&
        (prev_ident_low == "password" || prev_ident_low == "passwd" ||
         prev_ident_low == "secret" || prev_ident_low == "api_key" ||
         prev_ident_low == "apikey" || prev_ident_low == "access_token" ||
         prev_ident_low == "private_key" || prev_ident_low == "auth_token")) {
        if (unquoted != "REDACTED" && unquoted != "***" && !unquoted.starts_with("$")) {
            return true;
        }
    }
    return false;
}

bool looks_like_hardcoded_host_path(const std::string& unquoted) {
    if (unquoted.size() < 4) return false;
    if (std::isalpha(static_cast<unsigned char>(unquoted[0])) && unquoted[1] == ':' &&
        (unquoted[2] == '\\' || unquoted[2] == '/')) {
        return true;
    }
    const std::string low = lower_ascii(unquoted);
    return low.starts_with("/users/") || low.starts_with("/home/");
}

std::string make_finding_id(const std::string& rel_path, const std::string& rule_id, int line, const std::string& func_name) {
    return "fnd-" + sha256(rel_path + "|" + rule_id + "|" + std::to_string(line) + "|" + func_name).substr(0, 16);
}

void analyze_file_ast_and_rules(
    const std::string& rel_path,
    const std::string& lang,
    const std::string& content,
    bool is_test_file,
    int in_compile_db,
    bool check_uncompiled,
    int complexity_threshold,
    int max_function_lines,
    std::vector<ExtractedFunction>& out_functions,
    std::vector<ExtractedFinding>& out_findings) {

    const std::vector<std::string> lines = split_source_lines(content);
    const bool is_cpp = (lang.starts_with("C++"));
    const bool is_c = (lang == "C");
    const std::string lex_family = (is_cpp || is_c) ? "C++" : lang;

    const std::vector<ast::Token> raw_tokens = ast::lex_source(content, lex_family, false, false);
    std::vector<ast::Token> toks;
    toks.reserve(raw_tokens.size());
    for (const auto& t : raw_tokens) {
        if (t.kind == ast::TokenKind::Newline || t.kind == ast::TokenKind::EndOfFile) continue;
        toks.push_back(t);
    }

    // 1. Extract Functions & TEST_CASE definitions from AST token stream
    for (size_t i = 0; i + 1 < toks.size(); ++i) {
        const auto& tok = toks[i];
        if (tok.kind != ast::TokenKind::Identifier) continue;
        if (toks[i + 1].text != "(") continue;

        // Case A: Test Macros (TEST_CASE, TEST, TEST_F, TEST_P, BOOST_AUTO_TEST_CASE, SCENARIO)
        if (is_test_macro_name(tok.text)) {
            const size_t open_paren = i + 1;
            const size_t close_paren = find_matching_delim(toks, open_paren, "(", ")");
            if (close_paren >= toks.size() || close_paren + 1 >= toks.size()) continue;
            if (toks[close_paren + 1].text != "{") continue;
            const size_t open_brace = close_paren + 1;
            const size_t close_brace = find_matching_delim(toks, open_brace, "{", "}");
            if (close_brace >= toks.size()) continue;

            std::string arg_summary;
            std::string first_name;
            for (size_t k = open_paren + 1; k < close_paren; ++k) {
                if (first_name.empty() && (toks[k].kind == ast::TokenKind::StringLiteral || toks[k].kind == ast::TokenKind::Identifier)) {
                    first_name = unquote_literal(toks[k].text);
                }
                if (!arg_summary.empty() && toks[k].text != ",") arg_summary.push_back(' ');
                arg_summary += toks[k].text;
            }
            if (first_name.empty()) first_name = "anonymous_test";

            ExtractedFunction fn;
            fn.kind = "test_case";
            fn.qualified_name = tok.text + "::" + first_name;
            fn.display_name = tok.text + "(" + arg_summary + ")";
            fn.start_line = tok.line;
            fn.end_line = toks[close_brace].end_line;
            fn.line_count = (std::max)(1, fn.end_line - fn.start_line + 1);
            fn.param_count = count_params_in_parens(toks, open_paren, close_paren);
            compute_body_metrics(toks, open_brace, close_brace, fn.cyclomatic_complexity, fn.nesting_depth);
            fn.func_key = rel_path + ":" + fn.qualified_name + ":" + std::to_string(fn.start_line);

            out_functions.push_back(std::move(fn));
            i = close_brace;
            continue;
        }

        // Case B: Function / Method definition
        if (is_control_or_nonfunc_keyword(tok.text)) continue;

        // Avoid treating function calls after '=', '.', '->', 'return' as declarations
        if (i > 0) {
            const std::string& prev_t = toks[i - 1].text;
            if (prev_t == "." || prev_t == "->" || prev_t == "=" || prev_t == "return" || prev_t == ",") {
                continue;
            }
        }

        const size_t open_paren = i + 1;
        const size_t close_paren = find_matching_delim(toks, open_paren, "(", ")");
        if (close_paren >= toks.size()) continue;

        // Advance past valid trailing qualifiers between ')' and '{'
        size_t la = close_paren + 1;
        while (la < toks.size()) {
            const std::string& lt = toks[la].text;
            if (lt == "const" || lt == "volatile" || lt == "override" || lt == "final" || lt == "&" || lt == "&&") {
                ++la;
            } else if (lt == "noexcept") {
                ++la;
                if (la < toks.size() && toks[la].text == "(") {
                    const size_t end_ne = find_matching_delim(toks, la, "(", ")");
                    la = (end_ne < toks.size()) ? (end_ne + 1) : toks.size();
                }
            } else if (lt == "->") {
                // Trailing return type
                ++la;
                while (la < toks.size() && toks[la].text != "{" && toks[la].text != ";") {
                    ++la;
                }
                break;
            } else {
                break;
            }
        }

        if (la >= toks.size() || toks[la].text != "{") continue;

        // Verify there is a return type, qualifier, or class scope preceding the function name
        bool has_decl_prefix = false;
        if (i > 0) {
            const auto& pt = toks[i - 1];
            if (pt.kind == ast::TokenKind::Identifier || pt.text == "*" || pt.text == "&" ||
                pt.text == "&&" || pt.text == ">" || pt.text == "::" || pt.text == "~") {
                has_decl_prefix = true;
            }
        } else if (lang == "JavaScript" || lang == "TypeScript") {
            has_decl_prefix = true;
        }
        if (!has_decl_prefix) continue;

        const size_t open_brace = la;
        const size_t close_brace = find_matching_delim(toks, open_brace, "{", "}");
        if (close_brace >= toks.size()) continue;

        // Build qualified name if preceded by Class:: or ns::
        std::string qname = tok.text;
        size_t q_idx = i;
        while (q_idx >= 2 && toks[q_idx - 1].text == "::" && toks[q_idx - 2].kind == ast::TokenKind::Identifier) {
            qname = toks[q_idx - 2].text + "::" + qname;
            q_idx -= 2;
        }

        ExtractedFunction fn;
        fn.kind = (qname.find("::") != std::string::npos) ? "method" : "function";
        fn.qualified_name = qname;
        fn.display_name = tok.text;
        fn.start_line = tok.line;
        fn.end_line = toks[close_brace].end_line;
        fn.line_count = (std::max)(1, fn.end_line - fn.start_line + 1);
        fn.param_count = count_params_in_parens(toks, open_paren, close_paren);
        compute_body_metrics(toks, open_brace, close_brace, fn.cyclomatic_complexity, fn.nesting_depth);
        fn.func_key = rel_path + ":" + fn.qualified_name + ":" + std::to_string(fn.start_line);

        out_functions.push_back(std::move(fn));
        // Advance past the function body so local lambdas/calls inside the body aren't double-counted as top-level functions
        i = close_brace;
    }

    auto find_enclosing_function = [&](int line) -> std::string {
        for (const auto& fn : out_functions) {
            if (line >= fn.start_line && line <= fn.end_line) {
                return fn.qualified_name;
            }
        }
        return {};
    };

    // 2. Complexity Findings (PENDING_VALIDATION review alert, not confirmed defect by itself)
    for (const auto& fn : out_functions) {
        if (fn.cyclomatic_complexity >= complexity_threshold || fn.line_count >= max_function_lines) {
            ExtractedFinding f;
            f.line = fn.start_line;
            f.col = 1;
            f.function_name = fn.qualified_name;
            f.category = "complexity";
            f.rule_id = "AST-COMPLEXITY-REVIEW";
            f.severity = "P2";
            f.triage_status = "PENDING_VALIDATION";
            f.analyzer_tool = "ast_audit";
            f.message = "Complejidad ciclomática AST elevada (" + std::to_string(fn.cyclomatic_complexity) +
                        ") o longitud (" + std::to_string(fn.line_count) +
                        " líneas); revisar responsabilidades (no implica defecto por sí solo)";
            f.evidence_excerpt = get_line_excerpt(lines, fn.start_line);
            f.finding_id = make_finding_id(rel_path, f.rule_id, f.line, f.function_name);
            out_findings.push_back(std::move(f));
        }
    }

    // 3. Language-Specific & Test-Aware Security / Modernization Rules over AST Tokens
    for (size_t i = 0; i < toks.size(); ++i) {
        const auto& t = toks[i];

        // 3a. Strict C vs C++23 Separation: NULL -> nullptr ONLY in C++23 (NEVER in C!)
        if (is_cpp && t.kind == ast::TokenKind::Identifier && t.text == "NULL") {
            // Skip preprocessor #define NULL / #ifndef NULL / #ifdef NULL
            bool is_macro_def = false;
            if (i >= 2 && toks[i - 2].text == "#" &&
                (toks[i - 1].text == "define" || toks[i - 1].text == "ifndef" ||
                 toks[i - 1].text == "ifdef" || toks[i - 1].text == "undef")) {
                is_macro_def = true;
            }
            if (!is_macro_def) {
                ExtractedFinding f;
                f.line = t.line;
                f.col = t.column;
                f.function_name = find_enclosing_function(t.line);
                f.category = "modernization";
                f.rule_id = "CPP23-MODERNIZE-NULLPTR";
                f.severity = "P3";
                f.triage_status = is_test_file ? "PENDING_VALIDATION" : "CONFIRMED";
                f.analyzer_tool = "ast_audit";
                f.message = "Uso de macro NULL en C++23; reemplazar por nullptr con seguridad de tipos";
                f.evidence_excerpt = get_line_excerpt(lines, t.line);
                f.finding_id = make_finding_id(rel_path, f.rule_id, f.line, f.function_name);
                out_findings.push_back(std::move(f));
            }
        }

        if (is_cpp && t.kind == ast::TokenKind::Identifier && t.text == "auto_ptr") {
            ExtractedFinding f;
            f.line = t.line;
            f.col = t.column;
            f.function_name = find_enclosing_function(t.line);
            f.category = "modernization";
            f.rule_id = "CPP23-MODERNIZE-AUTOPTR";
            f.severity = "P2";
            f.triage_status = is_test_file ? "PENDING_VALIDATION" : "CONFIRMED";
            f.analyzer_tool = "ast_audit";
            f.message = "std::auto_ptr eliminado en C++17/C++23; usar std::unique_ptr";
            f.evidence_excerpt = get_line_excerpt(lines, t.line);
            f.finding_id = make_finding_id(rel_path, f.rule_id, f.line, f.function_name);
            out_findings.push_back(std::move(f));
        }

        // 3b. Unsafe CRT functions (gets, strcpy, strcat, sprintf)
        if ((is_c || is_cpp) && t.kind == ast::TokenKind::Identifier &&
            (t.text == "gets" || t.text == "strcpy" || t.text == "strcat" || t.text == "sprintf") &&
            i + 1 < toks.size() && toks[i + 1].text == "(") {
            ExtractedFinding f;
            f.line = t.line;
            f.col = t.column;
            f.function_name = find_enclosing_function(t.line);
            f.category = "security";
            f.rule_id = "SEC-UNSAFE-CRT";
            if (is_test_file) {
                f.severity = "P3";
                f.triage_status = "PENDING_VALIDATION";
                f.message = "Patrón detectado en archivo de pruebas (posible dato de ensayo); pendiente de validación, no vulnerabilidad confirmada";
            } else {
                f.severity = (t.text == "gets") ? "P0" : "P1";
                f.triage_status = "CONFIRMED";
                f.message = "Uso de función CRT insegura '" + t.text + "' sin verificación de límites en código de producción";
            }
            f.analyzer_tool = "ast_audit";
            f.evidence_excerpt = get_line_excerpt(lines, t.line);
            f.finding_id = make_finding_id(rel_path, f.rule_id, f.line, f.function_name);
            out_findings.push_back(std::move(f));
        }

        // 3c. Test-Aware Security Triage on String Literals (Secrets / API Keys & Hardcoded Host Paths)
        if (t.kind == ast::TokenKind::StringLiteral) {
            // Skip #include "..." lines
            const std::string line_ex = get_line_excerpt(lines, t.line);
            if (line_ex.starts_with("#include")) continue;

            const std::string unq = unquote_literal(t.text);
            std::string prev_ident_low;
            if (i >= 2 && (toks[i - 1].text == "=" || toks[i - 1].text == ":" || toks[i - 1].text == "(") &&
                toks[i - 2].kind == ast::TokenKind::Identifier) {
                prev_ident_low = lower_ascii(toks[i - 2].text);
            }

            if (looks_like_secret_literal(unq, prev_ident_low)) {
                ExtractedFinding f;
                f.line = t.line;
                f.col = t.column;
                f.function_name = find_enclosing_function(t.line);
                f.category = "security";
                f.rule_id = "SEC-HARDCODED-SECRET";
                if (is_test_file) {
                    f.severity = "P3";
                    f.triage_status = "PENDING_VALIDATION";
                    f.message = "Patrón detectado en archivo de pruebas (posible dato de ensayo); pendiente de validación, no vulnerabilidad confirmada";
                } else {
                    f.severity = "P1";
                    f.triage_status = "CONFIRMED";
                    f.message = "Posible secreto o clave API embebida en código de producción";
                }
                f.analyzer_tool = "ast_audit";
                f.evidence_excerpt = line_ex;
                f.finding_id = make_finding_id(rel_path, f.rule_id, f.line, f.function_name);
                out_findings.push_back(std::move(f));
            } else if (looks_like_hardcoded_host_path(unq)) {
                ExtractedFinding f;
                f.line = t.line;
                f.col = t.column;
                f.function_name = find_enclosing_function(t.line);
                f.category = "security";
                f.rule_id = "SEC-HARDCODED-PATH";
                if (is_test_file) {
                    f.severity = "P3";
                    f.triage_status = "PENDING_VALIDATION";
                    f.message = "Patrón detectado en archivo de pruebas (posible dato de ensayo); pendiente de validación, no vulnerabilidad confirmada";
                } else {
                    f.severity = "P1";
                    f.triage_status = "CONFIRMED";
                    f.message = "Ruta absoluta de host local embebida en código de producción";
                }
                f.analyzer_tool = "ast_audit";
                f.evidence_excerpt = line_ex;
                f.finding_id = make_finding_id(rel_path, f.rule_id, f.line, f.function_name);
                out_findings.push_back(std::move(f));
            }
        }
    }

    // 4. Uncompiled source file finding
    if (check_uncompiled && !is_test_file && in_compile_db == 0) {
        const std::string ext_low = lower_ascii(path_text(fs::path(wide(rel_path)).extension()));
        if (is_c_or_cpp_source_ext(ext_low)) {
            ExtractedFinding f;
            f.line = 1;
            f.col = 1;
            f.function_name = "";
            f.category = "uncompiled";
            f.rule_id = "BUILD-UNCOMPILED-SOURCE";
            f.severity = "P2";
            f.triage_status = "CONFIRMED";
            f.analyzer_tool = "ast_audit";
            f.message = "Archivo de implementación C/C++ ausente de compile_commands.json (fuera del build activo)";
            f.evidence_excerpt = rel_path;
            f.finding_id = make_finding_id(rel_path, f.rule_id, 1, "");
            out_findings.push_back(std::move(f));
        }
    }
}

} // namespace

void ensure_code_audit_schema(Database& db) {
    ensure_multiagent_coordinator_schema(db);

    db.exec(R"SQL(
        CREATE TABLE IF NOT EXISTS code_audit_files (
            file_path TEXT PRIMARY KEY,
            language TEXT NOT NULL DEFAULT '',
            file_sha256 TEXT NOT NULL DEFAULT '',
            compile_flags_sha256 TEXT NOT NULL DEFAULT '',
            headers_deps_sha256 TEXT NOT NULL DEFAULT '',
            analyzer_version TEXT NOT NULL DEFAULT 'v3.1-ast',
            in_compile_db INTEGER NOT NULL DEFAULT 1,
            is_test_file INTEGER NOT NULL DEFAULT 0,
            function_count INTEGER NOT NULL DEFAULT 0,
            max_complexity INTEGER NOT NULL DEFAULT 0,
            confirmed_findings_count INTEGER NOT NULL DEFAULT 0,
            pending_findings_count INTEGER NOT NULL DEFAULT 0,
            last_commit_sha TEXT NOT NULL DEFAULT '',
            last_author TEXT NOT NULL DEFAULT '',
            audited_at_utc TEXT NOT NULL DEFAULT ''
        )
    )SQL");

    db.exec(R"SQL(
        CREATE TABLE IF NOT EXISTS code_audit_header_deps (
            tu_path TEXT NOT NULL,
            header_path TEXT NOT NULL,
            header_sha256 TEXT NOT NULL DEFAULT '',
            PRIMARY KEY(tu_path, header_path)
        )
    )SQL");
    db.exec("CREATE INDEX IF NOT EXISTS idx_code_audit_header_deps_hdr ON code_audit_header_deps(header_path)");

    db.exec(R"SQL(
        CREATE TABLE IF NOT EXISTS code_audit_functions (
            func_key TEXT PRIMARY KEY,
            file_path TEXT NOT NULL DEFAULT '',
            qualified_name TEXT NOT NULL DEFAULT '',
            display_name TEXT NOT NULL DEFAULT '',
            kind TEXT NOT NULL DEFAULT 'function',
            start_line INTEGER NOT NULL DEFAULT 1,
            end_line INTEGER NOT NULL DEFAULT 1,
            line_count INTEGER NOT NULL DEFAULT 1,
            cyclomatic_complexity INTEGER NOT NULL DEFAULT 1,
            nesting_depth INTEGER NOT NULL DEFAULT 0,
            param_count INTEGER NOT NULL DEFAULT 0,
            analysis_source TEXT NOT NULL DEFAULT 'syntactic_ast',
            last_commit_sha TEXT NOT NULL DEFAULT '',
            last_author_agent TEXT NOT NULL DEFAULT '',
            covering_test_suite TEXT NOT NULL DEFAULT '',
            included_build_id TEXT NOT NULL DEFAULT '',
            audited_at_utc TEXT NOT NULL DEFAULT ''
        )
    )SQL");
    db.exec("CREATE INDEX IF NOT EXISTS idx_code_audit_functions_file ON code_audit_functions(file_path)");

    db.exec(R"SQL(
        CREATE TABLE IF NOT EXISTS code_audit_findings (
            finding_id TEXT PRIMARY KEY,
            file_path TEXT NOT NULL DEFAULT '',
            line INTEGER NOT NULL DEFAULT 1,
            col INTEGER NOT NULL DEFAULT 1,
            function_name TEXT NOT NULL DEFAULT '',
            category TEXT NOT NULL DEFAULT 'quality',
            rule_id TEXT NOT NULL DEFAULT '',
            severity TEXT NOT NULL DEFAULT 'P2',
            triage_status TEXT NOT NULL DEFAULT 'CONFIRMED',
            analyzer_tool TEXT NOT NULL DEFAULT 'ast_audit',
            is_test_file INTEGER NOT NULL DEFAULT 0,
            message TEXT NOT NULL DEFAULT '',
            evidence_excerpt TEXT NOT NULL DEFAULT '',
            introduced_commit TEXT NOT NULL DEFAULT '',
            owner_agent TEXT NOT NULL DEFAULT '',
            suppression_reason TEXT NOT NULL DEFAULT '',
            audited_at_utc TEXT NOT NULL DEFAULT ''
        )
    )SQL");
    db.exec("CREATE INDEX IF NOT EXISTS idx_code_audit_findings_file ON code_audit_findings(file_path)");
    db.exec("CREATE INDEX IF NOT EXISTS idx_code_audit_findings_triage ON code_audit_findings(triage_status)");
    db.exec("CREATE INDEX IF NOT EXISTS idx_code_audit_findings_sev ON code_audit_findings(severity)");

    db.exec(R"SQL(
        CREATE TABLE IF NOT EXISTS code_audit_snapshots (
            snapshot_id TEXT PRIMARY KEY,
            git_commit TEXT NOT NULL DEFAULT '',
            files_audited INTEGER NOT NULL DEFAULT 0,
            files_reused_cache INTEGER NOT NULL DEFAULT 0,
            total_functions INTEGER NOT NULL DEFAULT 0,
            confirmed_p0 INTEGER NOT NULL DEFAULT 0,
            confirmed_p1 INTEGER NOT NULL DEFAULT 0,
            pending_validation INTEGER NOT NULL DEFAULT 0,
            max_complexity INTEGER NOT NULL DEFAULT 0,
            uncompiled_files_count INTEGER NOT NULL DEFAULT 0,
            created_at_utc TEXT NOT NULL DEFAULT ''
        )
    )SQL");

    db.exec(R"SQL(
        CREATE TABLE IF NOT EXISTS agent_claim_verifications (
            claim_id TEXT PRIMARY KEY,
            actor TEXT NOT NULL DEFAULT '',
            module_or_path TEXT NOT NULL DEFAULT '',
            claim_text TEXT NOT NULL DEFAULT '',
            verdict TEXT NOT NULL DEFAULT 'UNVERIFIED_CLAIM',
            functions_evidence_json TEXT NOT NULL DEFAULT '{}',
            compilation_evidence_json TEXT NOT NULL DEFAULT '{}',
            tests_evidence_json TEXT NOT NULL DEFAULT '{}',
            quality_evidence_json TEXT NOT NULL DEFAULT '{}',
            equivalence_evidence_json TEXT NOT NULL DEFAULT '{}',
            created_at_utc TEXT NOT NULL DEFAULT ''
        )
    )SQL");
}

namespace {

Json run_incremental_audit_impl(const Project& p, Database& db, const Json& args, Cancel* cancel) {
    ensure_code_audit_schema(db);
    const std::string ts = now();
    const std::string analyzer_version = args.value("analyzer_version", std::string{"v3.1-ast"});
    const bool force_full = json_bool_or_int(args, "force_full", false);
    const bool require_compile_commands = json_bool_or_int(args, "require_compile_commands", false);
    const bool include_clang_tidy = json_bool_or_int(args, "include_clang_tidy", false);
    const int complexity_threshold = static_cast<int>(args.value("complexity_threshold", int64_t{20}));
    const int max_function_lines = static_cast<int>(args.value("max_function_lines", int64_t{120}));

    const fs::path proj_dir = canonical_win32_path(p.directory());
    const fs::path base_dir = proj_dir.empty() ? p.directory() : proj_dir;
    const std::string head_commit = query_git_head_commit(base_dir);

    std::string active_agent = args.value("actor", args.value("owner_agent", std::string{}));
    if (active_agent.empty() && table_exists(db, "agent_worktrees_catalog")) {
        const auto wt_rows = db.query("SELECT owner_agent FROM agent_worktrees_catalog WHERE owner_agent != '' ORDER BY updated_at_utc DESC LIMIT 1");
        if (!wt_rows.empty()) active_agent = wt_rows[0].value("owner_agent", std::string{});
    }
    if (active_agent.empty()) active_agent = "agent";

    std::string latest_build_id;
    if (table_exists(db, "build_manifests")) {
        const auto b_rows = db.query("SELECT build_id FROM build_manifests WHERE exit_code = 0 ORDER BY created_at_utc DESC LIMIT 1");
        if (!b_rows.empty()) latest_build_id = b_rows[0].value("build_id", std::string{});
    }

    // Load test directory policies from directory_classification_policies
    std::vector<std::pair<std::string, std::string>> test_policies;
    if (table_exists(db, "directory_classification_policies")) {
        for (const auto& r : db.query("SELECT pattern, match_kind FROM directory_classification_policies WHERE category = 'TEST_SOURCE'")) {
            test_policies.emplace_back(lower_ascii(norm_rel_path(r.value("pattern", std::string{}))),
                                       lower_ascii(r.value("match_kind", std::string{"prefix"})));
        }
    }

    // Load requirement_trace_links test_suite map
    std::unordered_map<std::string, std::string> trace_test_suite_by_file;
    if (table_exists(db, "requirement_trace_links")) {
        for (const auto& r : db.query("SELECT file_path, test_suite FROM requirement_trace_links WHERE test_suite != ''")) {
            trace_test_suite_by_file[lower_ascii(norm_rel_path(r.value("file_path", std::string{})))] =
                r.value("test_suite", std::string{});
        }
    }

    // Discover compile_commands.json
    const fs::path cc_path = discover_compile_commands_path(p, args);
    const bool has_compile_db = !cc_path.empty();
    const auto compile_db_map = load_compile_commands_map(p, cc_path);

    // Collect auditable files inside project
    struct FileCandidate {
        std::string rel_path;
        std::string rel_key;
        fs::path abs_path;
        std::string file_sha256;
    };
    std::map<std::string, FileCandidate> all_project_files; // keyed by rel_key
    std::unordered_map<std::string, std::vector<std::string>> filename_to_rel_keys;

    std::error_code ec;
    auto collect_from_dir = [&](const fs::path& root_p) {
        if (!fs::is_directory(native_path(root_p), ec)) return;
        for (fs::recursive_directory_iterator it(native_path(root_p), fs::directory_options::skip_permission_denied, ec), end;
             it != end; it.increment(ec)) {
            if (cancel && cancel->requested) throw std::runtime_error("CANCELLED");
            if (ec) { ec.clear(); continue; }
            if (it->is_symlink(ec)) {
                if (it->is_directory(ec)) it.disable_recursion_pending();
                continue;
            }
            if (it->is_directory(ec)) {
                const std::string dlow = lower_ascii(path_text(it->path().filename()));
                if (should_skip_audit_dir(dlow)) {
                    it.disable_recursion_pending();
                }
                continue;
            }
            if (!it->is_regular_file(ec)) continue;
            const std::string ext_low = lower_ascii(path_text(it->path().extension()));
            if (!is_auditable_code_extension(ext_low)) continue;

            const auto fsz = it->file_size(ec);
            if (ec || fsz > 16 * 1024 * 1024) continue;

            const fs::path abs_p = canonical_win32_path(it->path());
            fs::path rel_fs = fs::relative(abs_p.empty() ? it->path() : abs_p, base_dir, ec);
            if (ec || rel_fs.empty() || path_text(rel_fs).starts_with("..")) {
                const fs::path root_parent = canonical_win32_path(root_p.parent_path());
                rel_fs = fs::relative(abs_p.empty() ? it->path() : abs_p, root_parent.empty() ? root_p : root_parent, ec);
            }
            std::string rel_str = (!ec && !rel_fs.empty() && !path_text(rel_fs).starts_with(".."))
                ? norm_rel_path(path_text(rel_fs))
                : norm_rel_path(path_text(it->path()));

            const std::string rel_k = lower_ascii(rel_str);
            if (all_project_files.contains(rel_k)) continue;

            std::string f_sha;
            try {
                f_sha = lower_ascii(hash_file(it->path(), cancel));
            } catch (...) {
                continue;
            }
            all_project_files[rel_k] = FileCandidate{rel_str, rel_k, abs_p.empty() ? it->path() : abs_p, f_sha};
            const std::string fn_low = lower_ascii(path_text(it->path().filename()));
            filename_to_rel_keys[fn_low].push_back(rel_k);
        }
    };

    collect_from_dir(base_dir);
    if (p.config.contains("roots") && p.config["roots"].is_array()) {
        for (const auto& r : p.config["roots"]) {
            if (!r.is_object() || !r.contains("path") || !r["path"].is_string()) continue;
            const fs::path rp = p.resolve(r["path"].get<std::string>());
            if (!rp.empty()) collect_from_dir(rp);
        }
    }

    // Optional path filter if caller passed "paths" or "target_paths"
    std::unordered_set<std::string> requested_filter_keys;
    const Json filter_arr = (args.contains("paths") && args["paths"].is_array())
        ? args["paths"]
        : ((args.contains("target_paths") && args["target_paths"].is_array()) ? args["target_paths"] : Json::array());
    for (const auto& pval : filter_arr) {
        if (!pval.is_string()) continue;
        const std::string p_norm = lower_ascii(norm_rel_path(pval.get<std::string>()));
        if (!p_norm.empty()) requested_filter_keys.insert(p_norm);
    }

    auto matches_requested_filter = [&](const std::string& rel_k) -> bool {
        if (requested_filter_keys.empty()) return true;
        for (const auto& fk : requested_filter_keys) {
            if (rel_k == fk || rel_k.starts_with(fk + "/") || rel_k.ends_with("/" + fk)) return true;
        }
        return false;
    };

    // Build local header dependency graph across all C/C++ project files
    std::unordered_map<std::string, std::vector<std::string>> direct_header_deps;
    std::unordered_map<std::string, std::string> cached_contents;

    auto get_file_content = [&](const FileCandidate& fc) -> const std::string& {
        auto it = cached_contents.find(fc.rel_key);
        if (it != cached_contents.end()) return it->second;
        std::string txt;
        try {
            txt = read_text(fc.abs_path, 16 * 1024 * 1024);
        } catch (...) {}
        return cached_contents.emplace(fc.rel_key, std::move(txt)).first->second;
    };

    auto resolve_include_to_rel_key = [&](const std::string& from_rel, const std::string& inc_raw) -> std::string {
        const std::string inc_norm = norm_rel_path(inc_raw);
        const std::string inc_low = lower_ascii(inc_norm);

        // 1. Relative to including file's directory
        const fs::path from_parent = fs::path(wide(from_rel)).parent_path();
        if (!from_parent.empty()) {
            const std::string cand1 = lower_ascii(norm_rel_path(path_text((from_parent / fs::path(wide(inc_norm))).lexically_normal())));
            if (all_project_files.contains(cand1)) return cand1;
        }
        // 2. Direct project-relative match
        if (all_project_files.contains(inc_low)) return inc_low;
        // 3. Under include/ or src/
        const std::string cand_inc = "include/" + inc_low;
        if (all_project_files.contains(cand_inc)) return cand_inc;
        const std::string cand_src = "src/" + inc_low;
        if (all_project_files.contains(cand_src)) return cand_src;
        // 4. Suffix match or filename match if unambiguous
        for (const auto& [k, fc] : all_project_files) {
            if (k.size() > inc_low.size() && k.ends_with("/" + inc_low)) {
                return k;
            }
        }
        const std::string fname_low = lower_ascii(path_text(fs::path(wide(inc_norm)).filename()));
        const auto fn_it = filename_to_rel_keys.find(fname_low);
        if (fn_it != filename_to_rel_keys.end() && fn_it->second.size() == 1) {
            return fn_it->second[0];
        }
        return {};
    };

    for (const auto& [rel_k, fc] : all_project_files) {
        const std::string ext_low = lower_ascii(path_text(fc.abs_path.extension()));
        if (!is_c_or_cpp_ext(ext_low)) continue;
        const std::string& txt = get_file_content(fc);
        std::vector<std::string> resolved_list;
        for (const auto& inc : extract_include_Targets(txt)) {
            const std::string target_k = resolve_include_to_rel_key(fc.rel_path, inc);
            if (!target_k.empty() && target_k != rel_k) {
                resolved_list.push_back(target_k);
            }
        }
        std::sort(resolved_list.begin(), resolved_list.end());
        resolved_list.erase(std::unique(resolved_list.begin(), resolved_list.end()), resolved_list.end());
        direct_header_deps[rel_k] = std::move(resolved_list);
    }

    // Load existing code_audit_files cache from SQLite
    struct CachedFileRecord {
        std::string file_path;
        std::string file_sha256;
        std::string compile_flags_sha256;
        std::string headers_deps_sha256;
        std::string analyzer_version;
    };
    std::unordered_map<std::string, CachedFileRecord> existing_cache;
    for (const auto& r : db.query("SELECT file_path, file_sha256, compile_flags_sha256, headers_deps_sha256, analyzer_version FROM code_audit_files")) {
        const std::string fp_str = r.value("file_path", std::string{});
        existing_cache[lower_ascii(fp_str)] = CachedFileRecord{
            fp_str,
            r.value("file_sha256", std::string{}),
            r.value("compile_flags_sha256", std::string{}),
            r.value("headers_deps_sha256", std::string{}),
            r.value("analyzer_version", std::string{})
        };
    }

    // If full project scan (no path filter), remove rows for deleted files
    if (requested_filter_keys.empty()) {
        for (const auto& [cached_k, rec] : existing_cache) {
            if (!all_project_files.contains(cached_k)) {
                db.exec("DELETE FROM code_audit_files WHERE file_path=?", {rec.file_path});
                db.exec("DELETE FROM code_audit_functions WHERE file_path=?", {rec.file_path});
                db.exec("DELETE FROM code_audit_findings WHERE file_path=?", {rec.file_path});
                db.exec("DELETE FROM code_audit_header_deps WHERE tu_path=? OR header_path=?", {rec.file_path, rec.file_path});
            }
        }
    }

    int64_t files_audited = 0;
    int64_t files_reused_cache = 0;
    std::vector<fs::path> clang_tidy_candidates;

    Statement ins_header_dep(db, R"SQL(
        INSERT INTO code_audit_header_deps (tu_path, header_path, header_sha256)
        VALUES (?, ?, ?)
        ON CONFLICT(tu_path, header_path) DO UPDATE SET
            header_sha256=excluded.header_sha256
    )SQL");

    Statement ins_file(db, R"SQL(
        INSERT INTO code_audit_files (
            file_path, language, file_sha256, compile_flags_sha256, headers_deps_sha256,
            analyzer_version, in_compile_db, is_test_file, function_count, max_complexity,
            confirmed_findings_count, pending_findings_count, last_commit_sha, last_author, audited_at_utc
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(file_path) DO UPDATE SET
            language=excluded.language,
            file_sha256=excluded.file_sha256,
            compile_flags_sha256=excluded.compile_flags_sha256,
            headers_deps_sha256=excluded.headers_deps_sha256,
            analyzer_version=excluded.analyzer_version,
            in_compile_db=excluded.in_compile_db,
            is_test_file=excluded.is_test_file,
            function_count=excluded.function_count,
            max_complexity=excluded.max_complexity,
            confirmed_findings_count=excluded.confirmed_findings_count,
            pending_findings_count=excluded.pending_findings_count,
            last_commit_sha=excluded.last_commit_sha,
            last_author=excluded.last_author,
            audited_at_utc=excluded.audited_at_utc
    )SQL");

    Statement ins_func(db, R"SQL(
        INSERT INTO code_audit_functions (
            func_key, file_path, qualified_name, display_name, kind,
            start_line, end_line, line_count, cyclomatic_complexity, nesting_depth,
            param_count, analysis_source, last_commit_sha, last_author_agent,
            covering_test_suite, included_build_id, audited_at_utc
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(func_key) DO UPDATE SET
            file_path=excluded.file_path,
            qualified_name=excluded.qualified_name,
            display_name=excluded.display_name,
            kind=excluded.kind,
            start_line=excluded.start_line,
            end_line=excluded.end_line,
            line_count=excluded.line_count,
            cyclomatic_complexity=excluded.cyclomatic_complexity,
            nesting_depth=excluded.nesting_depth,
            param_count=excluded.param_count,
            analysis_source=excluded.analysis_source,
            last_commit_sha=excluded.last_commit_sha,
            last_author_agent=excluded.last_author_agent,
            covering_test_suite=excluded.covering_test_suite,
            included_build_id=excluded.included_build_id,
            audited_at_utc=excluded.audited_at_utc
    )SQL");

    Statement ins_finding(db, R"SQL(
        INSERT INTO code_audit_findings (
            finding_id, file_path, line, col, function_name,
            category, rule_id, severity, triage_status, analyzer_tool,
            is_test_file, message, evidence_excerpt, introduced_commit,
            owner_agent, suppression_reason, audited_at_utc
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(finding_id) DO UPDATE SET
            file_path=excluded.file_path,
            line=excluded.line,
            col=excluded.col,
            function_name=excluded.function_name,
            category=excluded.category,
            rule_id=excluded.rule_id,
            severity=excluded.severity,
            triage_status=excluded.triage_status,
            analyzer_tool=excluded.analyzer_tool,
            is_test_file=excluded.is_test_file,
            message=excluded.message,
            evidence_excerpt=excluded.evidence_excerpt,
            introduced_commit=excluded.introduced_commit,
            owner_agent=excluded.owner_agent,
            suppression_reason=excluded.suppression_reason,
            audited_at_utc=excluded.audited_at_utc
    )SQL");

    for (const auto& [rel_k, fc] : all_project_files) {
        if (!matches_requested_filter(rel_k)) continue;

        // Compute transitive header set via BFS
        std::set<std::string> trans_header_keys;
        std::vector<std::string> q;
        if (auto dit = direct_header_deps.find(rel_k); dit != direct_header_deps.end()) {
            for (const auto& hk : dit->second) {
                if (trans_header_keys.insert(hk).second) q.push_back(hk);
            }
        }
        for (size_t qi = 0; qi < q.size(); ++qi) {
            const std::string cur_h = q[qi];
            if (auto dit = direct_header_deps.find(cur_h); dit != direct_header_deps.end()) {
                for (const auto& nh : dit->second) {
                    if (nh != rel_k && trans_header_keys.insert(nh).second) {
                        q.push_back(nh);
                    }
                }
            }
        }

        std::string headers_concat;
        for (const auto& hk : trans_header_keys) {
            const auto fit = all_project_files.find(hk);
            if (fit != all_project_files.end()) {
                headers_concat += fit->second.rel_path + ":" + fit->second.file_sha256 + "\n";
            }
        }
        const std::string headers_deps_sha256 = headers_concat.empty() ? std::string{} : sha256(headers_concat);

        // Compile flags lookup
        const std::string ext_low = lower_ascii(path_text(fc.abs_path.extension()));
        const CompileDbEntry* cc_entry = nullptr;
        auto cc_it = compile_db_map.find(rel_k);
        if (cc_it != compile_db_map.end()) {
            cc_entry = &cc_it->second;
        } else {
            // Try matching by suffix
            for (const auto& [ck, cval] : compile_db_map) {
                if (ck.size() > rel_k.size() && ck.ends_with("/" + rel_k)) {
                    cc_entry = &cval;
                    break;
                }
            }
        }

        int in_compile_db = 1;
        std::string compile_flags_sha256;
        if (has_compile_db) {
            if (cc_entry != nullptr) {
                in_compile_db = 1;
                compile_flags_sha256 = cc_entry->flags_sha256;
            } else if (is_c_or_cpp_header_ext(ext_low) || !is_c_or_cpp_source_ext(ext_low)) {
                in_compile_db = 1;
                compile_flags_sha256 = "";
            } else {
                in_compile_db = 0;
                compile_flags_sha256 = "UNCOMPILED";
            }
        } else {
            in_compile_db = require_compile_commands ? 0 : 1;
            compile_flags_sha256 = "";
        }

        // Check incremental cache hit: (file_sha256, compile_flags_sha256, headers_deps_sha256, analyzer_version)
        const auto cache_it = existing_cache.find(rel_k);
        if (!force_full && cache_it != existing_cache.end()) {
            const auto& c = cache_it->second;
            if (c.file_sha256 == fc.file_sha256 &&
                c.compile_flags_sha256 == compile_flags_sha256 &&
                c.headers_deps_sha256 == headers_deps_sha256 &&
                c.analyzer_version == analyzer_version) {
                ++files_reused_cache;
                continue;
            }
        }

        // Cache miss -> re-audit file
        ++files_audited;
        const std::string& content = get_file_content(fc);
        const std::string lang = classify_file_language(fc.abs_path, content, cc_entry);
        const bool is_test = determine_is_test_file(fc.rel_path, test_policies);
        const bool check_uncompiled = (has_compile_db || require_compile_commands);

        if (include_clang_tidy && has_compile_db && is_c_or_cpp_source_ext(ext_low) && clang_tidy_candidates.size() < 10) {
            clang_tidy_candidates.push_back(fc.abs_path);
        }

        std::vector<ExtractedFunction> funcs;
        std::vector<ExtractedFinding> findings;
        analyze_file_ast_and_rules(fc.rel_path, lang, content, is_test, in_compile_db,
                                   check_uncompiled, complexity_threshold, max_function_lines,
                                   funcs, findings);

        // Preserve any user triage overrides (FALSE_POSITIVE / SUPPRESSED) for this file
        std::unordered_map<std::string, std::pair<std::string, std::string>> preserved_triage;
        for (const auto& old_f : db.query("SELECT finding_id, triage_status, suppression_reason FROM code_audit_findings WHERE file_path=?", {fc.rel_path})) {
            const std::string st = old_f.value("triage_status", std::string{});
            if (st == "FALSE_POSITIVE" || st == "SUPPRESSED") {
                preserved_triage[old_f.value("finding_id", std::string{})] = {
                    st, old_f.value("suppression_reason", std::string{})
                };
            }
        }

        // Update header dependencies table for this file
        db.exec("DELETE FROM code_audit_header_deps WHERE tu_path=?", {fc.rel_path});
        for (const auto& hk : trans_header_keys) {
            const auto fit = all_project_files.find(hk);
            if (fit != all_project_files.end()) {
                ins_header_dep.exec({fc.rel_path, fit->second.rel_path, fit->second.file_sha256});
            }
        }

        // Update functions table for this file
        db.exec("DELETE FROM code_audit_functions WHERE file_path=?", {fc.rel_path});
        const std::string analysis_src = (has_compile_db && is_c_or_cpp_ext(ext_low)) ? "clang_ast" : "syntactic_ast";
        std::string cov_suite;
        if (auto sit = trace_test_suite_by_file.find(rel_k); sit != trace_test_suite_by_file.end()) {
            cov_suite = sit->second;
        } else if (is_test) {
            cov_suite = path_text(fc.abs_path.stem());
        }

        int max_cc = 0;
        for (const auto& fn : funcs) {
            max_cc = (std::max)(max_cc, fn.cyclomatic_complexity);
            ins_func.exec({
                fn.func_key, fc.rel_path, fn.qualified_name, fn.display_name, fn.kind,
                fn.start_line, fn.end_line, fn.line_count, fn.cyclomatic_complexity,
                fn.nesting_depth, fn.param_count, analysis_src, head_commit,
                active_agent, cov_suite, latest_build_id, ts
            });
        }

        // Update findings table for this file
        db.exec("DELETE FROM code_audit_findings WHERE file_path=?", {fc.rel_path});
        int confirmed_cnt = 0;
        int pending_cnt = 0;
        for (auto& f : findings) {
            std::string sup_reason;
            if (auto pit = preserved_triage.find(f.finding_id); pit != preserved_triage.end()) {
                f.triage_status = pit->second.first;
                sup_reason = pit->second.second;
            }
            if (f.triage_status == "CONFIRMED") ++confirmed_cnt;
            else if (f.triage_status == "PENDING_VALIDATION") ++pending_cnt;

            ins_finding.exec({
                f.finding_id, fc.rel_path, f.line, f.col, f.function_name,
                f.category, f.rule_id, f.severity, f.triage_status, f.analyzer_tool,
                is_test ? 1 : 0, f.message, f.evidence_excerpt, head_commit,
                active_agent, sup_reason, ts
            });
        }

        ins_file.exec({
            fc.rel_path, lang, fc.file_sha256, compile_flags_sha256, headers_deps_sha256,
            analyzer_version, in_compile_db, is_test ? 1 : 0,
            static_cast<int64_t>(funcs.size()), max_cc, confirmed_cnt, pending_cnt,
            head_commit, active_agent, ts
        });
    }

    // Optional clang-tidy execution in a Win32 Job Object
    if (include_clang_tidy && has_compile_db && !clang_tidy_candidates.empty()) {
        const fs::path ct_exe = locate_clang_tidy_exe();
        if (!ct_exe.empty()) {
            std::vector<std::string> ct_args = {"-p", path_text(cc_path.parent_path()), "--quiet"};
            for (const auto& cp : clang_tidy_candidates) {
                ct_args.push_back(path_text(cp));
            }
            const auto ct_res = run_isolated_exe(ct_exe, base_dir, ct_args, 15000);
            if (!ct_res.output.empty()) {
                std::istringstream iss(ct_res.output);
                std::string line;
                while (std::getline(iss, line)) {
                    line = trim_copy(line);
                    const size_t wpos = line.find(": warning: ");
                    if (wpos == std::string::npos) continue;
                    const std::string loc_part = line.substr(0, wpos);
                    const std::string msg_part = line.substr(wpos + 11);
                    const size_t c2 = loc_part.rfind(':');
                    if (c2 == std::string::npos || c2 == 0) continue;
                    const size_t c1 = loc_part.rfind(':', c2 - 1);
                    if (c1 == std::string::npos || c1 == 0) continue;

                    int ln = 1;
                    int col = 1;
                    try {
                        ln = std::stoi(loc_part.substr(c1 + 1, c2 - c1 - 1));
                        col = std::stoi(loc_part.substr(c2 + 1));
                    } catch (...) {
                        continue;
                    }
                    fs::path raw_fp(wide(loc_part.substr(0, c1)));
                    if (raw_fp.is_relative()) raw_fp = base_dir / raw_fp;
                    const fs::path rel_fs = fs::relative(canonical_win32_path(raw_fp), base_dir, ec);
                    const std::string rel_str = (!ec && !rel_fs.empty())
                        ? norm_rel_path(path_text(rel_fs))
                        : norm_rel_path(loc_part.substr(0, c1));

                    std::string rule_id = "clang-tidy";
                    if (msg_part.ends_with("]")) {
                        const size_t ob = msg_part.rfind('[');
                        if (ob != std::string::npos) {
                            rule_id = msg_part.substr(ob + 1, msg_part.size() - ob - 2);
                        }
                    }
                    const bool is_test = determine_is_test_file(rel_str, test_policies);
                    const std::string fid = make_finding_id(rel_str, rule_id, ln, "clang_tidy");
                    ins_finding.exec({
                        fid, rel_str, ln, col, "", "clang_tidy", rule_id, "P2",
                        is_test ? "PENDING_VALIDATION" : "CONFIRMED", "clang_tidy",
                        is_test ? 1 : 0, msg_part, "", head_commit, active_agent, "", ts
                    });
                }
            }
        }
    }

    const int64_t total_files = db.integer("SELECT COUNT(*) FROM code_audit_files");
    const int64_t total_functions = db.integer("SELECT COUNT(*) FROM code_audit_functions");
    const int64_t confirmed_p0 = db.integer("SELECT COUNT(*) FROM code_audit_findings WHERE triage_status='CONFIRMED' AND severity='P0'");
    const int64_t confirmed_p1 = db.integer("SELECT COUNT(*) FROM code_audit_findings WHERE triage_status='CONFIRMED' AND severity='P1'");
    const int64_t confirmed_p2 = db.integer("SELECT COUNT(*) FROM code_audit_findings WHERE triage_status='CONFIRMED' AND severity='P2'");
    const int64_t confirmed_p3 = db.integer("SELECT COUNT(*) FROM code_audit_findings WHERE triage_status='CONFIRMED' AND severity='P3'");
    const int64_t confirmed_total = db.integer("SELECT COUNT(*) FROM code_audit_findings WHERE triage_status='CONFIRMED'");
    const int64_t pending_total = db.integer("SELECT COUNT(*) FROM code_audit_findings WHERE triage_status='PENDING_VALIDATION'");
    const int64_t uncompiled_cnt = db.integer("SELECT COUNT(*) FROM code_audit_files WHERE in_compile_db=0 AND is_test_file=0");
    const int64_t max_complexity = db.integer("SELECT COALESCE(MAX(cyclomatic_complexity), 0) FROM code_audit_functions");

    const std::string snapshot_id = "aud-" + unique_id().substr(0, 12);
    db.exec(R"SQL(
        INSERT INTO code_audit_snapshots (
            snapshot_id, git_commit, files_audited, files_reused_cache, total_functions,
            confirmed_p0, confirmed_p1, pending_validation, max_complexity,
            uncompiled_files_count, created_at_utc
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    )SQL", {
        snapshot_id, head_commit, files_audited, files_reused_cache, total_functions,
        confirmed_p0, confirmed_p1, pending_total, max_complexity, uncompiled_cnt, ts
    });

    return {
        {"schema", "migratool.code_audit/1"},
        {"action", lower_ascii(args.value("action", std::string{"scan_incremental"}))},
        {"ok", true},
        {"snapshot_id", snapshot_id},
        {"git_commit", head_commit},
        {"analyzer_version", analyzer_version},
        {"compile_commands_found", has_compile_db},
        {"compile_commands_path", cc_path.empty() ? std::string{} : canonical_path_str(cc_path)},
        {"total_files", total_files},
        {"files_audited", files_audited},
        {"files_reused_cache", files_reused_cache},
        {"total_functions", total_functions},
        {"confirmed_findings_count", confirmed_total},
        {"confirmed_p0", confirmed_p0},
        {"confirmed_p1", confirmed_p1},
        {"confirmed_p2", confirmed_p2},
        {"confirmed_p3", confirmed_p3},
        {"pending_validation_count", pending_total},
        {"uncompiled_files_count", uncompiled_cnt},
        {"max_complexity", max_complexity},
        {"files", db.query("SELECT * FROM code_audit_files ORDER BY file_path ASC")},
        {"findings", db.query("SELECT * FROM code_audit_findings ORDER BY severity ASC, file_path ASC, line ASC")},
        {"functions", db.query("SELECT * FROM code_audit_functions ORDER BY file_path ASC, start_line ASC")}
    };
}

bool path_matches_module_filter(const std::string& file_path, const std::string& mod_filter) {
    if (mod_filter.empty()) return true;
    const std::string fp_low = lower_ascii(norm_rel_path(file_path));
    const std::string mf_low = lower_ascii(norm_rel_path(mod_filter));
    if (fp_low == mf_low) return true;
    if (fp_low.starts_with(mf_low + "/")) return true;
    if (fp_low.size() > mf_low.size() && fp_low.ends_with("/" + mf_low)) return true;
    return false;
}

} // namespace

Json tool_code_audit(const Project& p, Database& db, const Json& args, Cancel* cancel) {
    ensure_code_audit_schema(db);
    const std::string action = lower_ascii(args.value("action", std::string{"scan_incremental"}));
    const std::string ts = now();

    if (action == "scan_incremental" || action == "audit" || action == "scan") {
        return run_incremental_audit_impl(p, db, args, cancel);
    }

    if (action == "functions") {
        if (db.integer("SELECT COUNT(*) FROM code_audit_files") == 0) {
            run_incremental_audit_impl(p, db, {{"action", "scan_incremental"}}, cancel);
        }
        const std::string file_f = args.value("file_path", args.value("file", std::string{}));
        const int64_t min_cc = args.value("min_complexity", int64_t{0});
        const std::string query_f = lower_ascii(trim_copy(args.value("query", std::string{})));

        const auto all_funcs = db.query("SELECT * FROM code_audit_functions ORDER BY cyclomatic_complexity DESC, line_count DESC, start_line ASC");
        Json items = Json::array();
        for (const auto& r : all_funcs) {
            const std::string fp = r.value("file_path", std::string{});
            if (!path_matches_module_filter(fp, file_f)) continue;
            if (r.value("cyclomatic_complexity", int64_t{1}) < min_cc) continue;
            if (!query_f.empty()) {
                const std::string hay = lower_ascii(fp + " " + r.value("qualified_name", std::string{}) + " " + r.value("display_name", std::string{}));
                if (hay.find(query_f) == std::string::npos) continue;
            }
            items.push_back(r);
        }
        return {
            {"schema", "migratool.code_audit/1"},
            {"action", "functions"},
            {"ok", true},
            {"total_functions", items.size()},
            {"functions", items}
        };
    }

    if (action == "findings") {
        if (db.integer("SELECT COUNT(*) FROM code_audit_files") == 0) {
            run_incremental_audit_impl(p, db, {{"action", "scan_incremental"}}, cancel);
        }
        const std::string file_f = args.value("file_path", args.value("file", std::string{}));
        const std::string cat_f = lower_ascii(trim_copy(args.value("category", std::string{})));
        const std::string sev_f = upper_ascii(trim_copy(args.value("severity", std::string{})));
        const std::string tri_f = upper_ascii(trim_copy(args.value("triage_status", std::string{})));
        const std::string rule_f = lower_ascii(trim_copy(args.value("rule_id", std::string{})));

        const auto all_findings = db.query("SELECT * FROM code_audit_findings ORDER BY severity ASC, file_path ASC, line ASC");
        Json items = Json::array();
        for (const auto& r : all_findings) {
            const std::string fp = r.value("file_path", std::string{});
            if (!path_matches_module_filter(fp, file_f)) continue;
            if (!cat_f.empty() && lower_ascii(r.value("category", std::string{})) != cat_f) continue;
            if (!sev_f.empty() && upper_ascii(r.value("severity", std::string{})) != sev_f) continue;
            if (!tri_f.empty() && upper_ascii(r.value("triage_status", std::string{})) != tri_f) continue;
            if (!rule_f.empty() && lower_ascii(r.value("rule_id", std::string{})) != rule_f) continue;
            items.push_back(r);
        }
        return {
            {"schema", "migratool.code_audit/1"},
            {"action", "findings"},
            {"ok", true},
            {"total_findings", items.size()},
            {"findings", items}
        };
    }

    if (action == "triage") {
        const std::string finding_id = trim_copy(args.value("finding_id", std::string{}));
        if (finding_id.empty()) {
            throw std::runtime_error("CODE_AUDIT_TRIAGE_MISSING_ID: finding_id is required");
        }
        const std::string new_status = upper_ascii(trim_copy(args.value("triage_status", args.value("status", std::string{"CONFIRMED"}))));
        if (new_status != "CONFIRMED" && new_status != "PENDING_VALIDATION" &&
            new_status != "FALSE_POSITIVE" && new_status != "SUPPRESSED") {
            throw std::runtime_error("INVALID_TRIAGE_STATUS: must be CONFIRMED, PENDING_VALIDATION, FALSE_POSITIVE, or SUPPRESSED");
        }
        const std::string reason = args.value("suppression_reason", args.value("reason", std::string{}));

        const auto rows = db.query("SELECT * FROM code_audit_findings WHERE finding_id=?", {finding_id});
        if (rows.empty()) {
            throw std::runtime_error("CODE_AUDIT_FINDING_NOT_FOUND: " + finding_id);
        }
        const std::string fp = rows[0].value("file_path", std::string{});

        db.exec("UPDATE code_audit_findings SET triage_status=?, suppression_reason=?, audited_at_utc=? WHERE finding_id=?",
                {new_status, reason, ts, finding_id});

        const int64_t conf_cnt = db.integer("SELECT COUNT(*) FROM code_audit_findings WHERE file_path=? AND triage_status='CONFIRMED'", {fp});
        const int64_t pend_cnt = db.integer("SELECT COUNT(*) FROM code_audit_findings WHERE file_path=? AND triage_status='PENDING_VALIDATION'", {fp});
        db.exec("UPDATE code_audit_files SET confirmed_findings_count=?, pending_findings_count=? WHERE file_path=?",
                {conf_cnt, pend_cnt, fp});

        const auto updated = db.query("SELECT * FROM code_audit_findings WHERE finding_id=?", {finding_id});
        return {
            {"schema", "migratool.code_audit/1"},
            {"action", "triage"},
            {"ok", true},
            {"finding_id", finding_id},
            {"triage_status", new_status},
            {"finding", updated.empty() ? Json(nullptr) : updated[0]}
        };
    }

    if (action == "verify_claim") {
        // Ensure SQLite audit state is fresh against current disk files
        run_incremental_audit_impl(p, db, {{"action", "scan_incremental"}}, cancel);

        const std::string actor = args.value("actor", std::string{"agent"});
        const std::string module_or_path = norm_rel_path(args.value("module_or_path", args.value("path", std::string{})));
        const std::string claim_text = args.value("claim_text", std::string{});
        const int64_t claimed_functions_count = args.value("claimed_functions_count", int64_t{-1});
        const bool claimed_compiled = json_bool_or_int(args, "claimed_compiled", false);
        const bool claimed_tests_passed = json_bool_or_int(args, "claimed_tests_passed", false);
        const bool claimed_equivalent = json_bool_or_int(args, "claimed_equivalent", false);

        // 1. Functions Evidence (AST vs Claimed)
        int64_t actual_functions_count = 0;
        int64_t max_cc = 0;
        std::string analysis_src = "AST_VERIFIED";
        for (const auto& fn : db.query("SELECT * FROM code_audit_functions")) {
            if (!path_matches_module_filter(fn.value("file_path", std::string{}), module_or_path)) continue;
            ++actual_functions_count;
            max_cc = (std::max)(max_cc, fn.value("cyclomatic_complexity", int64_t{1}));
        }
        const bool funcs_match = (claimed_functions_count < 0) || (actual_functions_count == claimed_functions_count);
        const Json functions_evidence = {
            {"status", funcs_match ? "AST_VERIFIED" : "COUNT_MISMATCH"},
            {"analysis_source", analysis_src},
            {"actual_functions_count", actual_functions_count},
            {"claimed_functions_count", claimed_functions_count},
            {"matches_claim", funcs_match}
        };

        // Collect matched files and latest disk modification timestamp
        bool any_uncompiled_source = false;
        std::string latest_file_mtime_utc;
        for (const auto& frow : db.query("SELECT * FROM code_audit_files")) {
            const std::string fp = frow.value("file_path", std::string{});
            if (!path_matches_module_filter(fp, module_or_path)) continue;
            if (frow.value("is_test_file", int64_t{0}) == 0 && frow.value("in_compile_db", int64_t{1}) == 0) {
                any_uncompiled_source = true;
            }
            const std::string mtime_iso = file_mtime_utc_iso(p.directory() / fs::path(wide(fp)));
            if (!mtime_iso.empty() && mtime_iso > latest_file_mtime_utc) {
                latest_file_mtime_utc = mtime_iso;
            }
        }

        // 2. Compilation Evidence (build_manifests & compile_commands.json coverage)
        std::string comp_status = "NO_EVIDENCE";
        std::string latest_build_id;
        int64_t latest_build_exit = -1;
        std::string latest_build_ts;
        if (table_exists(db, "build_manifests")) {
            const auto b_rows = db.query("SELECT * FROM build_manifests ORDER BY created_at_utc DESC, rowid DESC LIMIT 1");
            if (!b_rows.empty()) {
                latest_build_id = b_rows[0].value("build_id", std::string{});
                latest_build_exit = b_rows[0].value("exit_code", int64_t{-1});
                latest_build_ts = b_rows[0].value("created_at_utc", std::string{});
                if (latest_build_exit != 0) {
                    comp_status = "FAILED";
                } else if (any_uncompiled_source) {
                    comp_status = "UNCOMPILED_FILE";
                } else if (!latest_build_ts.empty() && !latest_file_mtime_utc.empty() && latest_file_mtime_utc > latest_build_ts) {
                    comp_status = "STALE_BUILD";
                } else {
                    comp_status = "VERIFIED_BUILD_PASS";
                }
            }
        }
        const Json compilation_evidence = {
            {"status", comp_status},
            {"latest_build_id", latest_build_id},
            {"exit_code", latest_build_exit},
            {"build_timestamp_utc", latest_build_ts},
            {"latest_file_mtime_utc", latest_file_mtime_utc},
            {"in_compile_db", !any_uncompiled_source},
            {"verified", comp_status == "VERIFIED_BUILD_PASS"}
        };

        // 3. Tests Evidence (verified_runs)
        std::string tests_status = "NO_EVIDENCE";
        std::string latest_run_id;
        int64_t latest_passed_cnt = 0;
        int64_t latest_failed_cnt = 0;
        std::string latest_run_ts;
        if (table_exists(db, "verified_runs")) {
            const auto r_rows = db.query("SELECT * FROM verified_runs ORDER BY rowid DESC LIMIT 1");
            if (!r_rows.empty()) {
                const auto& r = r_rows[0];
                latest_run_id = r.value("id", r.value("run_id", std::string{}));
                latest_passed_cnt = r.value("passed_count", int64_t{0});
                latest_failed_cnt = r.value("failed_count", int64_t{0});
                latest_run_ts = r.value("at_iso", r.value("created_at_utc", std::string{}));
                const int64_t r_exit = r.value("exit_code", int64_t{0});
                const std::string r_st = upper_ascii(r.value("status", std::string{}));

                bool run_ok = false;
                if (r.contains("ok") && !r["ok"].is_null()) {
                    const bool ok_flag = r["ok"].is_boolean() ? r["ok"].get<bool>() : (r.value("ok", int64_t{0}) != 0);
                    run_ok = ok_flag && (latest_passed_cnt > 0);
                } else {
                    run_ok = (r_exit == 0 && latest_failed_cnt == 0 && latest_passed_cnt > 0 &&
                              (r_st.empty() || r_st == "PASSED" || r_st == "PASS" || r_st == "OK" || r_st == "VERIFIED"));
                }

                if (!run_ok) {
                    tests_status = "FAILED";
                } else if (!latest_run_ts.empty() && !latest_file_mtime_utc.empty() && latest_file_mtime_utc > latest_run_ts) {
                    tests_status = "STALE_AFTER_EDIT";
                } else {
                    tests_status = "VERIFIED_TESTS_PASS";
                }
            }
        }
        const Json tests_evidence = {
            {"status", tests_status},
            {"latest_run_id", latest_run_id},
            {"passed_count", latest_passed_cnt},
            {"failed_count", latest_failed_cnt},
            {"run_timestamp_utc", latest_run_ts},
            {"verified", tests_status == "VERIFIED_TESTS_PASS"}
        };

        // 4. Quality Evidence
        int64_t conf_p0 = 0;
        int64_t conf_p1 = 0;
        int64_t conf_p2 = 0;
        int64_t conf_p3 = 0;
        int64_t pend_val = 0;
        for (const auto& fnd : db.query("SELECT * FROM code_audit_findings")) {
            if (!path_matches_module_filter(fnd.value("file_path", std::string{}), module_or_path)) continue;
            const std::string tri = fnd.value("triage_status", std::string{});
            const std::string sev = fnd.value("severity", std::string{});
            if (tri == "CONFIRMED") {
                if (sev == "P0") ++conf_p0;
                else if (sev == "P1") ++conf_p1;
                else if (sev == "P2") ++conf_p2;
                else ++conf_p3;
            } else if (tri == "PENDING_VALIDATION") {
                ++pend_val;
            }
        }
        const Json quality_evidence = {
            {"status", (conf_p0 == 0 && conf_p1 == 0) ? "CLEAN" : "HAS_CONFIRMED_FINDINGS"},
            {"max_complexity", max_cc},
            {"confirmed_p0", conf_p0},
            {"confirmed_p1", conf_p1},
            {"confirmed_p2", conf_p2},
            {"confirmed_p3", conf_p3},
            {"pending_validation", pend_val}
        };

        // 5. Functional Equivalence Evidence (NEVER conflated with compilation or unit tests!)
        std::string equiv_status = "NOT_PROVEN";
        int64_t verified_diff_cases = 0;
        if (table_exists(db, "mappings") && table_exists(db, "evidence")) {
            const auto ev_rows = db.query("SELECT data FROM evidence WHERE kind='tests' ORDER BY revision DESC LIMIT 1");
            std::unordered_set<std::string> passed_diff_cases;
            if (!ev_rows.empty()) {
                const Json ev_data = parse_json_safe(ev_rows[0].value("data", std::string{"{}"}), Json::object());
                if (ev_data.contains("cases") && ev_data["cases"].is_array()) {
                    for (const auto& c : ev_data["cases"]) {
                        if (c.is_object() && c.value("state", std::string{}) == "PASS" &&
                            c.value("kind", std::string{"differential"}) == "differential") {
                            passed_diff_cases.insert(c.value("id", std::string{}));
                        }
                    }
                }
            }
            if (!passed_diff_cases.empty()) {
                for (const auto& mrow : db.query("SELECT * FROM mappings WHERE complete=1")) {
                    const Json mcases = parse_json_safe(mrow.value("cases", std::string{"[]"}), Json::array());
                    if (mcases.empty()) continue;
                    bool all_pass = true;
                    for (const auto& cid : mcases) {
                        if (!cid.is_string() || !passed_diff_cases.contains(cid.get<std::string>())) {
                            all_pass = false;
                            break;
                        }
                    }
                    if (all_pass) {
                        ++verified_diff_cases;
                    }
                }
            }
            if (verified_diff_cases > 0) {
                equiv_status = "VERIFIED_IN_CASES";
            }
        }
        const Json functional_equivalence_evidence = {
            {"status", equiv_status},
            {"verified_cases_count", verified_diff_cases},
            {"proven", equiv_status == "VERIFIED_IN_CASES"},
            {"note", (equiv_status == "VERIFIED_IN_CASES")
                ? "Equivalencia funcional acreditada en casos diferenciales configurados"
                : "La compilación exitosa y las pruebas unitarias no demuestran equivalencia funcional (NOT_PROVEN)"}
        };

        // Compute Overall Anti-Hallucination Verdict
        std::string verdict = "UNVERIFIED_CLAIM";
        const bool build_claim_failed = claimed_compiled && (comp_status != "VERIFIED_BUILD_PASS");
        const bool tests_claim_failed = claimed_tests_passed && (tests_status != "VERIFIED_TESTS_PASS");
        const bool func_count_failed = !funcs_match;

        if (build_claim_failed || tests_claim_failed || func_count_failed) {
            if (comp_status == "FAILED" || comp_status == "UNCOMPILED_FILE" || comp_status == "STALE_BUILD" ||
                tests_status == "FAILED" || tests_status == "STALE_AFTER_EDIT" || func_count_failed) {
                verdict = "CONTRADICTED_BY_EVIDENCE";
            } else {
                verdict = "UNVERIFIED_CLAIM";
            }
        } else if (claimed_equivalent && equiv_status != "VERIFIED_IN_CASES") {
            verdict = "PARTIALLY_VERIFIED";
        } else if (comp_status == "VERIFIED_BUILD_PASS" && tests_status == "VERIFIED_TESTS_PASS") {
            verdict = "VERIFIED";
        } else {
            verdict = "UNVERIFIED_CLAIM";
        }

        const std::string claim_id = "clm-" + unique_id().substr(0, 12);
        db.exec(R"SQL(
            INSERT INTO agent_claim_verifications (
                claim_id, actor, module_or_path, claim_text, verdict,
                functions_evidence_json, compilation_evidence_json, tests_evidence_json,
                quality_evidence_json, equivalence_evidence_json, created_at_utc
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        )SQL", {
            claim_id, actor, module_or_path, claim_text, verdict,
            functions_evidence.dump(), compilation_evidence.dump(), tests_evidence.dump(),
            quality_evidence.dump(), functional_equivalence_evidence.dump(), ts
        });

        return {
            {"schema", "migratool.code_audit/1"},
            {"action", "verify_claim"},
            {"ok", true},
            {"claim_id", claim_id},
            {"actor", actor},
            {"module_or_path", module_or_path},
            {"claim_text", claim_text},
            {"verdict", verdict},
            {"functions_evidence", functions_evidence},
            {"compilation_evidence", compilation_evidence},
            {"tests_evidence", tests_evidence},
            {"quality_evidence", quality_evidence},
            {"functional_equivalence_evidence", functional_equivalence_evidence},
            {"equivalence_evidence", functional_equivalence_evidence}
        };
    }

    if (action == "regressions") {
        if (db.integer("SELECT COUNT(*) FROM code_audit_snapshots") == 0) {
            run_incremental_audit_impl(p, db, {{"action", "scan_incremental"}}, cancel);
        }
        const auto snaps = db.query("SELECT * FROM code_audit_snapshots ORDER BY created_at_utc DESC, rowid DESC LIMIT 2");
        const Json latest = snaps.empty() ? Json::object() : snaps[0];
        const Json prev = (snaps.size() >= 2) ? snaps[1] : latest;

        const int64_t p0_delta = latest.value("confirmed_p0", int64_t{0}) - prev.value("confirmed_p0", int64_t{0});
        const int64_t p1_delta = latest.value("confirmed_p1", int64_t{0}) - prev.value("confirmed_p1", int64_t{0});
        const int64_t cc_delta = latest.value("max_complexity", int64_t{0}) - prev.value("max_complexity", int64_t{0});
        const int64_t uncomp_delta = latest.value("uncompiled_files_count", int64_t{0}) - prev.value("uncompiled_files_count", int64_t{0});

        const auto confirmed_findings = db.query(
            "SELECT * FROM code_audit_findings WHERE triage_status='CONFIRMED' ORDER BY severity ASC, file_path ASC, line ASC");
        const auto complex_functions = db.query(
            "SELECT * FROM code_audit_functions ORDER BY cyclomatic_complexity DESC, line_count DESC LIMIT 20");

        const bool has_regressions = (p0_delta > 0 || p1_delta > 0 || uncomp_delta > 0 || !confirmed_findings.empty());
        return {
            {"schema", "migratool.code_audit/1"},
            {"action", "regressions"},
            {"ok", true},
            {"has_regressions", has_regressions},
            {"latest_snapshot", latest},
            {"previous_snapshot", prev},
            {"deltas", {
                {"confirmed_p0_delta", p0_delta},
                {"confirmed_p1_delta", p1_delta},
                {"max_complexity_delta", cc_delta},
                {"uncompiled_files_delta", uncomp_delta}
            }},
            {"confirmed_findings", confirmed_findings},
            {"high_complexity_functions", complex_functions}
        };
    }

    if (action == "gate_check") {
        if (db.integer("SELECT COUNT(*) FROM code_audit_files") == 0) {
            run_incremental_audit_impl(p, db, {{"action", "scan_incremental"}}, cancel);
        }
        const int64_t max_p0 = args.value("max_confirmed_p0", int64_t{0});
        const int64_t max_p1 = args.value("max_confirmed_p1", int64_t{-1});
        const bool req_compiled = json_bool_or_int(args, "require_compiled_in_db", false);
        const bool req_tests = json_bool_or_int(args, "require_verified_tests", false);

        const int64_t confirmed_p0 = db.integer("SELECT COUNT(*) FROM code_audit_findings WHERE triage_status='CONFIRMED' AND severity='P0'");
        const int64_t confirmed_p1 = db.integer("SELECT COUNT(*) FROM code_audit_findings WHERE triage_status='CONFIRMED' AND severity='P1'");
        const int64_t pending_val = db.integer("SELECT COUNT(*) FROM code_audit_findings WHERE triage_status='PENDING_VALIDATION'");
        const int64_t uncompiled_cnt = db.integer("SELECT COUNT(*) FROM code_audit_files WHERE in_compile_db=0 AND is_test_file=0");

        Json reasons = Json::array();
        if (confirmed_p0 > max_p0) {
            reasons.push_back("CONFIRMED_P0_FINDINGS_EXCEEDED: " + std::to_string(confirmed_p0) + " > " + std::to_string(max_p0));
        }
        if (max_p1 >= 0 && confirmed_p1 > max_p1) {
            reasons.push_back("CONFIRMED_P1_FINDINGS_EXCEEDED: " + std::to_string(confirmed_p1) + " > " + std::to_string(max_p1));
        }
        if (req_compiled && uncompiled_cnt > 0) {
            reasons.push_back("UNCOMPILED_SOURCE_FILES: " + std::to_string(uncompiled_cnt));
        }
        if (req_tests) {
            bool has_passing_run = false;
            if (table_exists(db, "verified_runs")) {
                const auto r_rows = db.query("SELECT * FROM verified_runs ORDER BY rowid DESC LIMIT 1");
                if (!r_rows.empty()) {
                    const auto& r = r_rows[0];
                    const int64_t p_cnt = r.value("passed_count", int64_t{0});
                    const int64_t f_cnt = r.value("failed_count", int64_t{0});
                    const int64_t ex = r.value("exit_code", int64_t{0});
                    if (r.contains("ok") && !r["ok"].is_null()) {
                        const bool ok_f = r["ok"].is_boolean() ? r["ok"].get<bool>() : (r.value("ok", int64_t{0}) != 0);
                        has_passing_run = ok_f && p_cnt > 0;
                    } else {
                        has_passing_run = (ex == 0 && f_cnt == 0 && p_cnt > 0);
                    }
                }
            }
            if (!has_passing_run) {
                reasons.push_back("NO_VERIFIED_TEST_RUN");
            }
        }

        const bool passed = reasons.empty();
        return {
            {"schema", "migratool.code_audit/1"},
            {"action", "gate_check"},
            {"ok", true},
            {"gate_passed", passed},
            {"confirmed_p0", confirmed_p0},
            {"confirmed_p1", confirmed_p1},
            {"pending_validation_count", pending_val},
            {"uncompiled_files_count", uncompiled_cnt},
            {"blocking_reasons", reasons}
        };
    }

    throw std::runtime_error("CODE_AUDIT_UNKNOWN_ACTION: " + action);
}

} // namespace mt
