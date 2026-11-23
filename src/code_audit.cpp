#include "migratool/code_audit.hpp"
#include "migratool/ast_engine.hpp"
#include "migratool/clang_bridge.hpp"
#include "migratool/multiagent_coordinator.hpp"

#include <shellapi.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
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

constexpr const char* kDefaultAnalyzerVersion = "code_audit/2-libclang";

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
        || dlow == "release" || dlow == "artifacts" || dlow == "backups"
        || dlow == ".vs" || dlow == "obj" || dlow == "dist"
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

bool is_libclang_eligible_ext(const std::string& ext_low) {
    return ext_low == ".c" || ext_low == ".cc" || ext_low == ".cpp" ||
           ext_low == ".cxx" || ext_low == ".c++" ||
           ext_low == ".h" || ext_low == ".hh" ||
           ext_low == ".hpp" || ext_low == ".hxx";
}

std::vector<std::string> split_command_line_args(const std::string& line) {
    std::vector<std::string> out;
    const size_t first = line.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return out;
    const std::wstring text = wide(line.substr(first));
    int count = 0;
    const std::unique_ptr<wchar_t*, decltype(&LocalFree)> argv(
        CommandLineToArgvW(text.c_str(), &count), &LocalFree);
    if (!argv || count <= 0) return out;
    out.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        out.push_back(utf8(argv.get()[i]));
    }
    return out;
}

struct CompileDbEntry {
    std::string command_or_args;
    std::vector<std::string> raw_args;
    fs::path directory;
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
        fs::path entry_dir;
        if (!raw_dir.empty()) {
            entry_dir = fs::path(wide(raw_dir));
            if (entry_dir.is_relative()) entry_dir = p.directory() / entry_dir;
        } else {
            entry_dir = p.directory();
        }
        fs::path fp(wide(raw_file));
        if (fp.is_relative()) {
            fp = entry_dir / fp;
        }

        std::string flags;
        std::vector<std::string> parsed_args;
        if (item.contains("arguments") && item["arguments"].is_array()) {
            for (const auto& a : item["arguments"]) {
                if (a.is_string()) {
                    const std::string s = a.get<std::string>();
                    if (!flags.empty()) flags.push_back(' ');
                    flags += s;
                    parsed_args.push_back(s);
                }
            }
        } else if (item.contains("command") && item["command"].is_string()) {
            flags = item["command"].get<std::string>();
            parsed_args = split_command_line_args(flags);
        }

        const std::string low_flags = lower_ascii(flags);
        CompileDbEntry entry;
        entry.command_or_args = flags;
        entry.raw_args = std::move(parsed_args);
        entry.directory = entry_dir;
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

std::vector<std::string> build_libclang_compile_args(
    const fs::path& abs_file,
    const std::string& lang,
    const std::string& ext_low,
    const fs::path& base_dir,
    const CompileDbEntry* cc_entry,
    const Project& p) {

    const bool is_header = is_c_or_cpp_header_ext(ext_low);
    const bool is_c_lang = (lang == "C");

    std::string explicit_x;
    std::string explicit_std;
    std::vector<std::string> defines_and_undefs;
    std::vector<std::string> include_flags;
    std::unordered_set<std::string> seen_includes;

    auto add_include_dir = [&](const fs::path& raw_dir) {
        if (raw_dir.empty()) return;
        std::error_code iec;
        fs::path norm = canonical_win32_path(raw_dir);
        if (norm.empty()) {
            norm = raw_dir.is_absolute() ? raw_dir.lexically_normal() : fs::absolute(raw_dir, iec).lexically_normal();
        }
        const std::string text = path_text(norm);
        if (text.empty()) return;
        const std::string key = lower_ascii(text);
        if (seen_includes.insert(key).second) {
            include_flags.push_back("-I" + text);
        }
    };

    if (cc_entry != nullptr && !cc_entry->raw_args.empty()) {
        const fs::path cc_dir = cc_entry->directory.empty() ? base_dir : cc_entry->directory;
        const auto& raw = cc_entry->raw_args;
        size_t start_idx = 0;
        if (!raw[0].empty() && raw[0][0] != '-' && raw[0][0] != '/') {
            start_idx = 1; // skip compiler executable
        }
        for (size_t i = start_idx; i < raw.size(); ++i) {
            const std::string& a = raw[i];
            if (a.empty()) continue;
            const std::string alow = lower_ascii(a);

            // Two-token output/dependency/PCH flags to skip
            if (a == "-o" || a == "-MF" || a == "-MT" || a == "-MQ" ||
                alow == "/fo" || alow == "-fo" || alow == "/fd" || alow == "-fd" ||
                alow == "/fp" || alow == "-fp" || alow == "/fi" || alow == "-fi" ||
                a == "-include" || a == "-imacros") {
                ++i;
                continue;
            }

            // Two-token include directories
            if (a == "-I" || a == "/I" || a == "-isystem" ||
                alow == "/external:i" || alow == "-external:i") {
                if (i + 1 < raw.size()) {
                    fs::path ip(wide(raw[++i]));
                    if (ip.is_relative()) ip = cc_dir / ip;
                    add_include_dir(ip);
                }
                continue;
            }

            // Attached include directories
            if (a.size() > 2 && (a.starts_with("-I") || a.starts_with("/I"))) {
                fs::path ip(wide(a.substr(2)));
                if (ip.is_relative()) ip = cc_dir / ip;
                add_include_dir(ip);
                continue;
            }
            if (a.size() > 8 && a.starts_with("-isystem")) {
                fs::path ip(wide(a.substr(8)));
                if (ip.is_relative()) ip = cc_dir / ip;
                add_include_dir(ip);
                continue;
            }
            if (a.size() > 11 && (alow.starts_with("/external:i") || alow.starts_with("-external:i"))) {
                fs::path ip(wide(a.substr(11)));
                if (ip.is_relative()) ip = cc_dir / ip;
                add_include_dir(ip);
                continue;
            }

            // Macro definitions & undefinitions (avoid matching /DEBUG, /DLL, /utf-8)
            if (a == "-D" || a == "/D") {
                if (i + 1 < raw.size()) defines_and_undefs.push_back("-D" + raw[++i]);
                continue;
            }
            if (a.size() > 2 && (a.starts_with("-D") || a.starts_with("/D")) &&
                alow != "/debug" && alow != "/dll") {
                defines_and_undefs.push_back("-D" + a.substr(2));
                continue;
            }
            if (a == "-U" || a == "/U") {
                if (i + 1 < raw.size()) defines_and_undefs.push_back("-U" + raw[++i]);
                continue;
            }
            if (a.size() > 2 && (a.starts_with("-U") || a.starts_with("/U")) &&
                alow != "/utf-8") {
                defines_and_undefs.push_back("-U" + a.substr(2));
                continue;
            }

            // Language standard flags (GCC/Clang -std=... and MSVC /std:...)
            if (alow.starts_with("-std=")) {
                explicit_std = a;
                continue;
            }
            if (alow.starts_with("/std:") || alow.starts_with("-std:")) {
                const std::string sv = alow.substr(5);
                if (sv == "c++latest" || sv == "c++23") explicit_std = "-std=c++23";
                else if (sv == "c++20") explicit_std = "-std=c++20";
                else if (sv == "c++17") explicit_std = "-std=c++17";
                else if (sv == "c++14") explicit_std = "-std=c++14";
                else if (sv == "c23") explicit_std = "-std=c23";
                else if (sv == "c17") explicit_std = "-std=c17";
                else if (sv == "c11") explicit_std = "-std=c11";
                continue;
            }

            // Explicit language mode flags
            if (a == "-x" && i + 1 < raw.size()) {
                explicit_x = raw[++i];
                continue;
            }
            if (alow == "/tp" || alow == "-tp") {
                explicit_x = is_header ? "c++-header" : "c++";
                continue;
            }
            if (alow == "/tc" || alow == "-tc") {
                explicit_x = is_header ? "c-header" : "c";
                continue;
            }
        }
    }

    if (p.config.contains("defines") && p.config["defines"].is_array()) {
        for (const auto& def : p.config["defines"]) {
            if (def.is_string() && !def.get<std::string>().empty()) {
                defines_and_undefs.push_back("-D" + def.get<std::string>());
            }
        }
    }

    std::error_code ec;
    add_include_dir(abs_file.parent_path());
    add_include_dir(base_dir);
    if (fs::is_directory(native_path(base_dir / L"include"), ec)) {
        add_include_dir(base_dir / L"include");
    }
    if (fs::is_directory(native_path(base_dir / L"src"), ec)) {
        add_include_dir(base_dir / L"src");
    }

    std::vector<std::string> out;
    if (!explicit_x.empty()) {
        out.push_back("-x");
        out.push_back(explicit_x);
    } else if (is_header) {
        out.push_back("-x");
        out.push_back(is_c_lang ? "c-header" : "c++-header");
    }

    if (!explicit_std.empty()) {
        out.push_back(explicit_std);
    } else {
        out.push_back(is_c_lang ? "-std=c23" : "-std=c++23");
    }

    for (auto& d : defines_and_undefs) out.push_back(std::move(d));
    for (auto& inc : include_flags) out.push_back(std::move(inc));
    out.push_back("-ferror-limit=20");
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
    std::string signature;
    std::string kind = "function";
    int start_line = 1;
    int end_line = 1;
    int line_count = 1;
    int cyclomatic_complexity = 1;
    int nesting_depth = 0;
    int param_count = 0;
    std::string analysis_source = "syntactic_tokens";
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

std::string map_clang_cursor_kind(const std::string& raw_kind) {
    const std::string low = lower_ascii(raw_kind);
    if (low == "functiondecl") return "function";
    if (low == "cxxmethod") return "method";
    if (low == "constructor" || low == "cxxconstructor") return "constructor";
    if (low == "destructor" || low == "cxxdestructor") return "destructor";
    if (low == "conversionfunction" || low == "cxxconversion") return "conversion";
    if (low == "functiontemplate") return "function_template";
    return raw_kind.empty() ? std::string{"function"} : raw_kind;
}

int count_params_from_display_name(const std::string& disp) {
    const size_t close_p = disp.rfind(')');
    if (close_p == std::string::npos || close_p == 0) return 0;
    int depth = 1;
    size_t open_p = close_p;
    while (open_p > 0) {
        --open_p;
        if (disp[open_p] == ')') ++depth;
        else if (disp[open_p] == '(') {
            --depth;
            if (depth == 0) break;
        }
    }
    if (depth != 0 || open_p >= close_p) return 0;
    const std::string inner = trim_copy(disp.substr(open_p + 1, close_p - open_p - 1));
    if (inner.empty() || inner == "void") return 0;
    int params = 1;
    int paren_d = 0;
    int angle_d = 0;
    int bracket_d = 0;
    for (char ch : inner) {
        if (ch == '(') ++paren_d;
        else if (ch == ')') paren_d = (std::max)(0, paren_d - 1);
        else if (ch == '[') ++bracket_d;
        else if (ch == ']') bracket_d = (std::max)(0, bracket_d - 1);
        else if (ch == '<') ++angle_d;
        else if (ch == '>') angle_d = (std::max)(0, angle_d - 1);
        else if (ch == ',' && paren_d == 0 && angle_d == 0 && bracket_d == 0) {
            ++params;
        }
    }
    return params;
}

int compute_nesting_depth_for_line_range(const std::vector<ast::Token>& toks, int start_line, int end_line) {
    size_t open_brace = toks.size();
    for (size_t i = 0; i < toks.size(); ++i) {
        if (toks[i].line < start_line) continue;
        if (toks[i].line > end_line) break;
        if (toks[i].kind == ast::TokenKind::Punctuator && toks[i].text == "{") {
            open_brace = i;
            break;
        }
    }
    if (open_brace >= toks.size()) return 0;
    const size_t close_brace = find_matching_delim(toks, open_brace, "{", "}");
    if (close_brace >= toks.size()) return 0;
    int cur_nest = 0;
    int max_nest = 0;
    for (size_t i = open_brace + 1; i < close_brace; ++i) {
        if (toks[i].kind == ast::TokenKind::Punctuator) {
            if (toks[i].text == "{") {
                ++cur_nest;
                max_nest = (std::max)(max_nest, cur_nest);
            } else if (toks[i].text == "}") {
                cur_nest = (std::max)(0, cur_nest - 1);
            }
        }
    }
    return max_nest;
}

std::string make_unique_func_key(
    const std::string& rel_path,
    const std::string& qname,
    int start_line,
    const std::string& display_name,
    const std::string& signature,
    std::unordered_set<std::string>& used_keys) {
    std::string key = rel_path + ":" + qname + ":" + std::to_string(start_line);
    if (used_keys.insert(key).second) {
        return key;
    }
    std::string disamb = key + ":" + sha256(display_name + "|" + signature).substr(0, 8);
    int suffix = 1;
    while (!used_keys.insert(disamb).second) {
        disamb = key + ":" + std::to_string(++suffix);
    }
    return disamb;
}

void extract_test_macro_functions(
    const std::string& rel_path,
    const std::vector<ast::Token>& toks,
    const std::unordered_set<int>& skip_start_lines,
    std::unordered_set<std::string>& used_keys,
    std::vector<ExtractedFunction>& out_functions) {
    for (size_t i = 0; i + 1 < toks.size(); ++i) {
        const auto& tok = toks[i];
        if (tok.kind != ast::TokenKind::Identifier) continue;
        if (!is_test_macro_name(tok.text)) continue;
        if (toks[i + 1].text != "(") continue;

        const size_t open_paren = i + 1;
        const size_t close_paren = find_matching_delim(toks, open_paren, "(", ")");
        if (close_paren >= toks.size() || close_paren + 1 >= toks.size()) continue;
        if (toks[close_paren + 1].text != "{") continue;
        const size_t open_brace = close_paren + 1;
        const size_t close_brace = find_matching_delim(toks, open_brace, "{", "}");
        if (close_brace >= toks.size()) continue;

        if (!skip_start_lines.contains(tok.line)) {
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
            fn.signature = fn.display_name;
            fn.start_line = tok.line;
            fn.end_line = toks[close_brace].end_line;
            fn.line_count = (std::max)(1, fn.end_line - fn.start_line + 1);
            fn.param_count = count_params_in_parens(toks, open_paren, close_paren);
            compute_body_metrics(toks, open_brace, close_brace, fn.cyclomatic_complexity, fn.nesting_depth);
            fn.analysis_source = "syntactic_tokens";
            fn.func_key = make_unique_func_key(rel_path, fn.qualified_name, fn.start_line, fn.display_name, fn.signature, used_keys);
            out_functions.push_back(std::move(fn));
        }
        i = close_brace;
    }
}

void extract_python_functions(
    const std::string& rel_path,
    const std::vector<std::string>& lines,
    const std::vector<ast::Token>& toks,
    std::unordered_set<std::string>& used_keys,
    std::vector<ExtractedFunction>& out_functions) {
    auto line_indent = [](const std::string& s) -> int {
        int ind = 0;
        for (char c : s) {
            if (c == ' ') ++ind;
            else if (c == '\t') ind += 4;
            else break;
        }
        return ind;
    };
    for (size_t li = 0; li < lines.size(); ++li) {
        const std::string& raw = lines[li];
        const int def_ind = line_indent(raw);
        const std::string trimmed = trim_copy(raw);
        size_t pos = std::string::npos;
        if (trimmed.starts_with("def ")) pos = 4;
        else if (trimmed.starts_with("async def ")) pos = 10;
        if (pos == std::string::npos) continue;

        while (pos < trimmed.size() && std::isspace(static_cast<unsigned char>(trimmed[pos]))) ++pos;
        const size_t name_start = pos;
        while (pos < trimmed.size() && (std::isalnum(static_cast<unsigned char>(trimmed[pos])) || trimmed[pos] == '_')) ++pos;
        if (pos == name_start) continue;
        const std::string fname = trimmed.substr(name_start, pos - name_start);
        while (pos < trimmed.size() && std::isspace(static_cast<unsigned char>(trimmed[pos]))) ++pos;
        if (pos >= trimmed.size() || trimmed[pos] != '(') continue;

        const int start_line = static_cast<int>(li) + 1;
        int end_line = start_line;
        for (size_t lj = li + 1; lj < lines.size(); ++lj) {
            const std::string tj = trim_copy(lines[lj]);
            if (tj.empty() || tj.starts_with("#")) continue;
            if (line_indent(lines[lj]) <= def_ind) break;
            end_line = static_cast<int>(lj) + 1;
        }

        int cc = 1;
        int param_count = 0;
        const size_t open_p = trimmed.find('(', pos);
        const size_t close_p = (open_p != std::string::npos) ? trimmed.find(')', open_p + 1) : std::string::npos;
        if (open_p != std::string::npos && close_p != std::string::npos) {
            const std::string plist = trim_copy(trimmed.substr(open_p + 1, close_p - open_p - 1));
            if (!plist.empty()) {
                param_count = 1;
                for (char ch : plist) if (ch == ',') ++param_count;
            }
        }
        for (const auto& t : toks) {
            if (t.line < start_line || t.line > end_line) continue;
            if (t.kind == ast::TokenKind::Identifier || t.kind == ast::TokenKind::Keyword) {
                if (t.text == "if" || t.text == "elif" || t.text == "for" ||
                    t.text == "while" || t.text == "except" || t.text == "and" || t.text == "or") {
                    ++cc;
                }
            }
        }

        ExtractedFunction fn;
        fn.kind = (def_ind > 0) ? "method" : "function";
        fn.qualified_name = fname;
        fn.display_name = fname;
        fn.signature = trimmed;
        fn.start_line = start_line;
        fn.end_line = end_line;
        fn.line_count = (std::max)(1, end_line - start_line + 1);
        fn.param_count = param_count;
        fn.cyclomatic_complexity = cc;
        fn.nesting_depth = 1;
        fn.analysis_source = "syntactic_tokens";
        fn.func_key = make_unique_func_key(rel_path, fn.qualified_name, fn.start_line, fn.display_name, fn.signature, used_keys);
        out_functions.push_back(std::move(fn));
    }
}

void extract_token_stream_functions(
    const std::string& rel_path,
    const std::string& lang,
    const std::vector<std::string>& lines,
    const std::vector<ast::Token>& toks,
    std::unordered_set<std::string>& used_keys,
    std::vector<ExtractedFunction>& out_functions) {
    if (lang == "Python") {
        extract_python_functions(rel_path, lines, toks, used_keys, out_functions);
        return;
    }
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
            fn.signature = fn.display_name;
            fn.start_line = tok.line;
            fn.end_line = toks[close_brace].end_line;
            fn.line_count = (std::max)(1, fn.end_line - fn.start_line + 1);
            fn.param_count = count_params_in_parens(toks, open_paren, close_paren);
            compute_body_metrics(toks, open_brace, close_brace, fn.cyclomatic_complexity, fn.nesting_depth);
            fn.analysis_source = "syntactic_tokens";
            fn.func_key = make_unique_func_key(rel_path, fn.qualified_name, fn.start_line, fn.display_name, fn.signature, used_keys);

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
        fn.signature = "";
        fn.start_line = tok.line;
        fn.end_line = toks[close_brace].end_line;
        fn.line_count = (std::max)(1, fn.end_line - fn.start_line + 1);
        fn.param_count = count_params_in_parens(toks, open_paren, close_paren);
        compute_body_metrics(toks, open_brace, close_brace, fn.cyclomatic_complexity, fn.nesting_depth);
        fn.analysis_source = "syntactic_tokens";
        fn.func_key = make_unique_func_key(rel_path, fn.qualified_name, fn.start_line, fn.display_name, fn.signature, used_keys);

        out_functions.push_back(std::move(fn));
        // Advance past the function body so local lambdas/calls inside the body aren't double-counted as top-level functions
        i = close_brace;
    }
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
    const Project& p,
    const fs::path& base_dir,
    const fs::path& abs_path,
    const std::string& rel_path,
    const std::string& lang,
    const std::string& content,
    const CompileDbEntry* cc_entry,
    bool is_test_file,
    int in_compile_db,
    bool check_uncompiled,
    int complexity_threshold,
    int max_function_lines,
    std::vector<ExtractedFunction>& out_functions,
    std::vector<ExtractedFinding>& out_findings,
    std::string& out_clang_version,
    bool& out_used_libclang) {

    out_used_libclang = false;
    const std::vector<std::string> lines = split_source_lines(content);
    const std::string ext_low = lower_ascii(path_text(abs_path.extension()));
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

    // 1. Extract Functions & TEST_CASE definitions
    // AUD-MIG-P1-006: Use real libclang AST extraction for C/C++ files and honestly label
    // every function "clang_ast" vs "syntactic_tokens".
    std::unordered_set<std::string> used_func_keys;
    if (is_libclang_eligible_ext(ext_low)) {
        const std::vector<std::string> compile_args =
            build_libclang_compile_args(abs_path, lang, ext_low, base_dir, cc_entry, p);
        std::vector<ClangAstFunction> clang_fns;
        std::string clang_ver;
        std::string clang_err;
        if (extract_cpp_functions_libclang(abs_path, compile_args, clang_fns, clang_ver, clang_err)) {
            out_used_libclang = true;
            if (!clang_ver.empty()) out_clang_version = clang_ver;
            std::unordered_set<int> clang_start_lines;
            for (const auto& cf : clang_fns) {
                ExtractedFunction fn;
                fn.qualified_name = cf.qualified_name.empty() ? cf.display_name : cf.qualified_name;
                fn.display_name = cf.display_name.empty() ? fn.qualified_name : cf.display_name;
                fn.signature = cf.signature;
                fn.kind = map_clang_cursor_kind(cf.kind);
                fn.start_line = (std::max)(1, cf.start_line);
                fn.end_line = (std::max)(fn.start_line, cf.end_line);
                fn.line_count = fn.end_line - fn.start_line + 1;
                fn.cyclomatic_complexity = (std::max)(1, cf.cyclomatic_complexity);
                fn.param_count = count_params_from_display_name(fn.display_name);
                fn.nesting_depth = compute_nesting_depth_for_line_range(toks, fn.start_line, fn.end_line);
                fn.analysis_source = "clang_ast";
                fn.func_key = make_unique_func_key(rel_path, fn.qualified_name, fn.start_line, fn.display_name, fn.signature, used_func_keys);
                clang_start_lines.insert(fn.start_line);
                out_functions.push_back(std::move(fn));
            }
            // Supplement standalone TEST_CASE / TEST / SCENARIO macros not emitted by libclang
            // (always labeled "syntactic_tokens", never mislabeled "clang_ast").
            extract_test_macro_functions(rel_path, toks, clang_start_lines, used_func_keys, out_functions);
        } else {
            (void)clang_err;
        }
    }

    if (!out_used_libclang) {
        extract_token_stream_functions(rel_path, lang, lines, toks, used_func_keys, out_functions);
    }

    std::sort(out_functions.begin(), out_functions.end(), [](const ExtractedFunction& a, const ExtractedFunction& b) {
        if (a.start_line != b.start_line) return a.start_line < b.start_line;
        return a.func_key < b.func_key;
    });

    auto find_enclosing_function = [&](int line) -> std::string {
        std::string best_name;
        int best_span = (std::numeric_limits<int>::max)();
        for (const auto& fn : out_functions) {
            if (line >= fn.start_line && line <= fn.end_line) {
                const int span = fn.end_line - fn.start_line;
                if (span < best_span) {
                    best_span = span;
                    best_name = fn.qualified_name;
                }
            }
        }
        return best_name;
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
            analyzer_version TEXT NOT NULL DEFAULT 'code_audit/2-libclang',
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
            signature TEXT NOT NULL DEFAULT '',
            kind TEXT NOT NULL DEFAULT 'function',
            start_line INTEGER NOT NULL DEFAULT 1,
            end_line INTEGER NOT NULL DEFAULT 1,
            line_count INTEGER NOT NULL DEFAULT 1,
            cyclomatic_complexity INTEGER NOT NULL DEFAULT 1,
            nesting_depth INTEGER NOT NULL DEFAULT 0,
            param_count INTEGER NOT NULL DEFAULT 0,
            analysis_source TEXT NOT NULL DEFAULT 'syntactic_tokens',
            last_commit_sha TEXT NOT NULL DEFAULT '',
            last_author_agent TEXT NOT NULL DEFAULT '',
            covering_test_suite TEXT NOT NULL DEFAULT '',
            included_build_id TEXT NOT NULL DEFAULT '',
            audited_at_utc TEXT NOT NULL DEFAULT ''
        )
    )SQL");
    try {
        db.exec("ALTER TABLE code_audit_functions ADD COLUMN signature TEXT NOT NULL DEFAULT ''");
    } catch (...) {}
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

Json run_incremental_audit_impl(const Project& p, Database& db, const Json& args, Cancel* cancel,
                                bool verify_content_hash_override = false) {
    ensure_code_audit_schema(db);
    const std::string ts = now();
    const std::string analyzer_version = args.value("analyzer_version", std::string{kDefaultAnalyzerVersion});
    const bool force_full = json_bool_or_int(args, "force_full", false);
    const bool verify_content_hash = verify_content_hash_override || json_bool_or_int(args, "verify_content_hash", false);
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

    // Collect auditable files inside project (always hashing live disk content via SHA-256
    // so same-mtime + same-size edits are reliably detected when verify_content_hash is active).
    struct FileCandidate {
        std::string rel_path;
        std::string rel_key;
        fs::path abs_path;
        std::string file_sha256;
    };
    std::map<std::string, FileCandidate> all_project_files; // keyed by rel_key
    std::unordered_map<std::string, std::vector<std::string>> filename_to_rel_keys;

    std::unordered_set<std::string> extra_skip_dirs;
    if (p.config.contains("exclude_dirs") && p.config["exclude_dirs"].is_array()) {
        for (const auto& ed : p.config["exclude_dirs"]) {
            if (ed.is_string()) {
                std::string d = lower_ascii(norm_rel_path(ed.get<std::string>()));
                while (!d.empty() && d.back() == '/') d.pop_back();
                if (!d.empty()) extra_skip_dirs.insert(d);
            }
        }
    }
    auto load_gitignore_dirs = [&](const fs::path& dir_root) {
        std::error_code gec;
        const fs::path gi = dir_root / L".gitignore";
        if (!fs::is_regular_file(native_path(gi), gec)) return;
        try {
            const std::string gi_txt = read_text(gi, 512 * 1024);
            std::istringstream iss(gi_txt);
            std::string line;
            while (std::getline(iss, line)) {
                line = trim_copy(line);
                if (line.empty() || line[0] == '#' || line[0] == '!') continue;
                if (line.find('*') != std::string::npos || line.find('?') != std::string::npos) continue;
                if (line.back() != '/') continue;
                while (!line.empty() && (line.front() == '/' || line.front() == '\\')) line.erase(line.begin());
                while (!line.empty() && (line.back() == '/' || line.back() == '\\')) line.pop_back();
                if (!line.empty() && line.find('/') == std::string::npos && line.find('\\') == std::string::npos) {
                    extra_skip_dirs.insert(lower_ascii(line));
                }
            }
        } catch (...) {}
    };
    load_gitignore_dirs(base_dir);
    if (p.config.contains("roots") && p.config["roots"].is_array()) {
        for (const auto& r : p.config["roots"]) {
            if (r.is_object() && r.contains("path") && r["path"].is_string()) {
                const fs::path rp = p.resolve(r["path"].get<std::string>());
                if (!rp.empty()) load_gitignore_dirs(rp);
            }
        }
    }

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
                if (should_skip_audit_dir(dlow) || extra_skip_dirs.contains(dlow)) {
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
            lower_ascii(r.value("file_sha256", std::string{})),
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
    int64_t libclang_files_parsed = 0;
    std::string libclang_version_seen;
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
            func_key, file_path, qualified_name, display_name, signature, kind,
            start_line, end_line, line_count, cyclomatic_complexity, nesting_depth,
            param_count, analysis_source, last_commit_sha, last_author_agent,
            covering_test_suite, included_build_id, audited_at_utc
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(func_key) DO UPDATE SET
            file_path=excluded.file_path,
            qualified_name=excluded.qualified_name,
            display_name=excluded.display_name,
            signature=excluded.signature,
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

        // Check incremental cache hit: (live file_sha256, compile_flags_sha256, headers_deps_sha256, analyzer_version)
        // fc.file_sha256 was just computed live from disk bytes during collect_from_dir above (catches same-mtime/same-size edits).
        const std::string& effective_sha = fc.file_sha256;

        const auto cache_it = existing_cache.find(rel_k);
        if (!force_full && cache_it != existing_cache.end()) {
            const auto& c = cache_it->second;
            if (c.file_sha256 == effective_sha &&
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
        std::string file_clang_ver;
        bool used_libclang = false;
        analyze_file_ast_and_rules(p, base_dir, fc.abs_path, fc.rel_path, lang, content, cc_entry,
                                   is_test, in_compile_db, check_uncompiled,
                                   complexity_threshold, max_function_lines,
                                   funcs, findings, file_clang_ver, used_libclang);
        if (used_libclang) {
            ++libclang_files_parsed;
            if (libclang_version_seen.empty() && !file_clang_ver.empty()) {
                libclang_version_seen = file_clang_ver;
            }
        }

        // Preserve any user triage overrides (FALSE_POSITIVE / SUPPRESSED) for this file
        const bool need_file_tx = (sqlite3_get_autocommit(db.native_handle()) != 0);
        if (need_file_tx) db.begin();
        try {
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

            // Update functions table for this file (each function carries its own honest analysis_source)
            db.exec("DELETE FROM code_audit_functions WHERE file_path=?", {fc.rel_path});
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
                    fn.func_key, fc.rel_path, fn.qualified_name, fn.display_name, fn.signature, fn.kind,
                    fn.start_line, fn.end_line, fn.line_count, fn.cyclomatic_complexity,
                    fn.nesting_depth, fn.param_count, fn.analysis_source, head_commit,
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
                fc.rel_path, lang, effective_sha, compile_flags_sha256, headers_deps_sha256,
                analyzer_version, in_compile_db, is_test ? 1 : 0,
                static_cast<int64_t>(funcs.size()), max_cc, confirmed_cnt, pending_cnt,
                head_commit, active_agent, ts
            });
            if (need_file_tx) db.commit();
        } catch (...) {
            if (need_file_tx) {
                try { db.rollback(); } catch (...) {}
            }
            throw;
        }
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
    const int64_t clang_ast_functions = db.integer("SELECT COUNT(*) FROM code_audit_functions WHERE analysis_source='clang_ast'");
    const int64_t syntactic_tokens_functions = db.integer("SELECT COUNT(*) FROM code_audit_functions WHERE analysis_source='syntactic_tokens'");
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
        {"verify_content_hash", verify_content_hash},
        {"libclang_files_parsed", libclang_files_parsed},
        {"libclang_version", libclang_version_seen},
        {"compile_commands_found", has_compile_db},
        {"compile_commands_path", cc_path.empty() ? std::string{} : canonical_path_str(cc_path)},
        {"total_files", total_files},
        {"files_audited", files_audited},
        {"files_reused_cache", files_reused_cache},
        {"total_functions", total_functions},
        {"clang_ast_functions_count", clang_ast_functions},
        {"syntactic_tokens_functions_count", syntactic_tokens_functions},
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
    // Allow matching a bare module name (no slash, no dot) against file stem or directory segment
    if (mf_low.find('/') == std::string::npos && mf_low.find('.') == std::string::npos) {
        const std::string stem_low = lower_ascii(path_text(fs::path(wide(fp_low)).stem()));
        if (stem_low == mf_low) return true;
        if (fp_low.find("/" + mf_low + "/") != std::string::npos) return true;
    }
    return false;
}

std::vector<std::string> collect_scope_filters(const Json& args) {
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    auto add_one = [&](const std::string& raw) {
        const std::string norm = norm_rel_path(trim_copy(raw));
        if (norm.empty()) return;
        const std::string key = lower_ascii(norm);
        if (seen.insert(key).second) out.push_back(norm);
    };
    for (const char* k : {"module_or_path", "path", "module", "file_path"}) {
        if (args.contains(k) && args[k].is_string()) {
            add_one(args[k].get<std::string>());
        }
    }
    if (args.contains("files") && args["files"].is_array()) {
        for (const auto& el : args["files"]) {
            if (el.is_string()) add_one(el.get<std::string>());
        }
    }
    return out;
}

bool file_matches_any_scope(const std::string& file_path, const std::vector<std::string>& scope_filters) {
    if (scope_filters.empty()) return true;
    for (const auto& sf : scope_filters) {
        if (path_matches_module_filter(file_path, sf)) return true;
    }
    return false;
}

std::string normalize_suite_token(std::string s) {
    s = trim_copy(std::move(s));
    while (!s.empty() && s.front() == '^') s.erase(s.begin());
    while (!s.empty() && s.back() == '$') s.pop_back();
    s = trim_copy(std::move(s));
    for (char& c : s) {
        if (c == '\\') c = '/';
    }
    return lower_ascii(s);
}

void add_normalized_target_tokens(const std::string& raw, std::unordered_set<std::string>& out) {
    const std::string tok = normalize_suite_token(raw);
    if (tok.empty()) return;
    out.insert(tok);

    std::string base = tok;
    if (tok.find('/') != std::string::npos) {
        const fs::path tp(wide(tok));
        const std::string fname = lower_ascii(path_text(tp.filename()));
        const std::string stem = lower_ascii(path_text(tp.stem()));
        if (!fname.empty()) out.insert(fname);
        if (!stem.empty()) {
            out.insert(stem);
            base = stem;
        }
    } else if (tok.find('.') != std::string::npos) {
        const std::string stem = lower_ascii(path_text(fs::path(wide(tok)).stem()));
        if (!stem.empty()) {
            out.insert(stem);
            base = stem;
        }
    }

    if (base.ends_with("_tests") && base.size() > 6) {
        out.insert(base.substr(0, base.size() - 6));
    } else if (base.ends_with("_test") && base.size() > 5) {
        out.insert(base.substr(0, base.size() - 5));
    } else if (base.ends_with("-tests") && base.size() > 6) {
        out.insert(base.substr(0, base.size() - 6));
    } else if (base.ends_with("-test") && base.size() > 5) {
        out.insert(base.substr(0, base.size() - 5));
    } else if (base.starts_with("test_") && base.size() > 5) {
        out.insert(base.substr(5));
    } else if (base.starts_with("tests_") && base.size() > 6) {
        out.insert(base.substr(6));
    }
}

bool token_sets_intersect(const std::unordered_set<std::string>& a, const std::unordered_set<std::string>& b) {
    for (const auto& k : a) {
        if (b.contains(k)) return true;
    }
    return false;
}

void parse_run_targets_structurally(
    const Json& run_row,
    bool& out_is_full_ctest,
    std::unordered_set<std::string>& out_tokens,
    Json& out_covered_targets) {
    out_is_full_ctest = false;
    out_tokens.clear();
    out_covered_targets = Json::array();
    std::unordered_set<std::string> seen_display;

    auto push_display = [&](const std::string& s) {
        const std::string t = trim_copy(s);
        if (!t.empty() && seen_display.insert(lower_ascii(t)).second) {
            out_covered_targets.push_back(t);
        }
    };

    const Json sel_json = parse_json_safe(run_row.value("selected_targets_json", std::string{"[]"}), Json::array());
    const Json cmd_json = parse_json_safe(run_row.value("command_json", std::string{"[]"}), Json::array());

    std::vector<std::string> argv;
    if (cmd_json.is_array()) {
        for (const auto& el : cmd_json) {
            if (el.is_string()) argv.push_back(el.get<std::string>());
        }
    }

    // Unwrap simple single-command `cmd.exe /c "..."` only when free of shell chaining operators
    bool shell_chained = false;
    if (argv.size() >= 3) {
        const std::string exe0 = lower_ascii(path_text(fs::path(wide(argv[0])).stem()));
        const std::string flag1 = lower_ascii(argv[1]);
        if (exe0 == "cmd" && (flag1 == "/c" || flag1 == "/k")) {
            const std::string& body = argv[2];
            if (body.find("&&") != std::string::npos || body.find("||") != std::string::npos ||
                body.find(';') != std::string::npos || body.find('|') != std::string::npos ||
                body.find('>') != std::string::npos || body.find('<') != std::string::npos) {
                shell_chained = true;
            } else {
                const auto unwrapped = split_command_line_args(body);
                if (!unwrapped.empty()) argv = unwrapped;
            }
        }
    }

    bool ctest_narrowed = false;
    if (!shell_chained && !argv.empty()) {
        const std::string exe_stem = lower_ascii(path_text(fs::path(wide(argv[0])).stem()));
        if (exe_stem == "ctest") {
            auto collect_regex_parts = [&](const std::string& pat) {
                std::istringstream pss(pat);
                std::string part;
                while (std::getline(pss, part, '|')) {
                    const std::string norm_part = normalize_suite_token(part);
                    if (!norm_part.empty()) {
                        add_normalized_target_tokens(norm_part, out_tokens);
                        push_display(norm_part);
                    }
                }
            };

            for (size_t i = 1; i < argv.size(); ++i) {
                const std::string& a = argv[i];
                const std::string alow = lower_ascii(a);
                if (a == "-R" || alow == "--tests-regex") {
                    ctest_narrowed = true;
                    if (i + 1 < argv.size()) {
                        collect_regex_parts(argv[++i]);
                    }
                    continue;
                }
                if (a.size() > 2 && a.starts_with("-R")) {
                    ctest_narrowed = true;
                    collect_regex_parts(a.substr(2));
                    continue;
                }
                if (alow.starts_with("--tests-regex=")) {
                    ctest_narrowed = true;
                    collect_regex_parts(a.substr(14));
                    continue;
                }
                if (a == "-E" || alow == "--exclude-regex" || alow.starts_with("--exclude-regex=") ||
                    a == "-L" || alow == "--label-regex" || alow.starts_with("--label-regex=") ||
                    a == "-LE" || alow == "--label-exclude" || alow.starts_with("--label-exclude=") ||
                    a == "-I" || alow == "--tests-information" || alow == "--tests-from-file") {
                    ctest_narrowed = true;
                    if ((a == "-E" || alow == "--exclude-regex" || a == "-L" || alow == "--label-regex" ||
                         a == "-LE" || alow == "--label-exclude" || a == "-I" ||
                         alow == "--tests-information" || alow == "--tests-from-file") && i + 1 < argv.size()) {
                        ++i;
                    }
                    continue;
                }
            }
            if (!ctest_narrowed) {
                out_is_full_ctest = true;
                push_display("ctest(all)");
            }
        } else {
            static const std::unordered_set<std::string> kGenericRunners = {
                "cmd", "powershell", "pwsh", "python", "python3", "pytest",
                "node", "npm", "npx", "cargo", "dotnet", "go", "cmake", "echo"
            };
            if (!exe_stem.empty() && !kGenericRunners.contains(exe_stem)) {
                add_normalized_target_tokens(argv[0], out_tokens);
                push_display(path_text(fs::path(wide(argv[0])).stem()));
            }
            for (size_t i = 1; i < argv.size(); ++i) {
                const std::string& a = argv[i];
                if (a.empty()) continue;
                const std::string alow = lower_ascii(a);
                if (alow == "-k" || alow == "--filter") {
                    if (i + 1 < argv.size()) {
                        const std::string& fval = argv[++i];
                        add_normalized_target_tokens(fval, out_tokens);
                        push_display(fval);
                    }
                    continue;
                }
                if (alow == "--preset" || alow == "-c" || alow == "--config" || alow == "-j" ||
                    alow == "--jobs" || alow == "--test-dir" || alow == "-m") {
                    if (i + 1 < argv.size()) ++i;
                    continue;
                }
                if (a[0] == '-') continue;
                add_normalized_target_tokens(a, out_tokens);
                push_display(a);
            }
        }
    }

    // Include selected_targets_json entries only when command did not narrow ctest via -R/-E/-L/-I
    if (sel_json.is_array() && !ctest_narrowed) {
        for (const auto& el : sel_json) {
            if (!el.is_string()) continue;
            const std::string s = trim_copy(el.get<std::string>());
            if (s.empty()) continue;
            add_normalized_target_tokens(s, out_tokens);
            push_display(s);
        }
    }
}

Json evaluate_compilation_evidence(
    Database& db,
    const fs::path& repo_dir,
    const std::string& current_head,
    bool any_uncompiled_source,
    const std::string& latest_file_mtime_utc) {

    const std::string repo_dir_str = canonical_path_str(repo_dir);
    const std::string repo_dir_key = lower_ascii(repo_dir_str);
    const std::string current_head_low = lower_ascii(current_head);

    struct BuildEval {
        bool valid = false;
        std::string status = "NO_EVIDENCE";
        std::string build_id;
        std::string git_commit;
        std::string worktree_path;
        std::string provenance_mode;
        int64_t exit_code = -1;
        std::string build_ts;
        int64_t verified_arts = 0;
        int64_t total_arts = 0;
        std::string mismatch_reason;
    };

    BuildEval chosen;
    BuildEval best_wt_commit;
    BuildEval best_wt;
    BuildEval best_any;

    if (table_exists(db, "build_manifests")) {
        const bool has_artifacts_table = table_exists(db, "build_artifacts");
        const auto b_rows = db.query("SELECT * FROM build_manifests ORDER BY created_at_utc DESC, rowid DESC LIMIT 50");

        for (const auto& b : b_rows) {
            BuildEval ev;
            ev.valid = true;
            ev.build_id = b.value("build_id", std::string{});
            const std::string raw_wt = trim_copy(b.value("worktree_path", std::string{}));
            ev.worktree_path = raw_wt.empty() ? std::string{} : canonical_path_str(fs::path(wide(raw_wt)));
            ev.git_commit = trim_copy(b.value("head_commit", std::string{}));
            if (ev.git_commit.empty()) {
                ev.git_commit = trim_copy(b.value("git_commit", std::string{}));
            }
            ev.exit_code = b.value("exit_code", int64_t{-1});
            ev.build_ts = trim_copy(b.value("created_at_utc", std::string{}));
            if (b.contains("provenance_mode") && b["provenance_mode"].is_string()) {
                ev.provenance_mode = upper_ascii(trim_copy(b["provenance_mode"].get<std::string>()));
            }
            if (ev.provenance_mode.empty()) {
                ev.provenance_mode = "DECLARED_EXTERNAL";
            }

            // 1. Strict worktree_path binding
            const std::string wt_key = lower_ascii(ev.worktree_path);
            const bool wt_matches = (!wt_key.empty() && wt_key == repo_dir_key);
            if (!wt_matches) {
                ev.status = "EVIDENCE_WORKTREE_MISMATCH";
                ev.mismatch_reason = "build worktree '" + ev.worktree_path + "' does not match repository '" + repo_dir_str + "'";
                if (!best_any.valid) best_any = ev;
                continue;
            }

            // 2. Strict git commit binding against live HEAD
            const std::string commit_low = lower_ascii(ev.git_commit);
            const bool commit_matches = current_head_low.empty() || (!commit_low.empty() && commit_low == current_head_low);
            if (!commit_matches) {
                ev.status = "EVIDENCE_COMMIT_MISMATCH";
                ev.mismatch_reason = "build commit '" + ev.git_commit + "' does not match current HEAD '" + current_head + "'";
                if (!best_wt.valid) best_wt = ev;
                continue;
            }

            // 3. Exit code check
            if (ev.exit_code != 0) {
                ev.status = "FAILED";
                ev.mismatch_reason = "build exited with non-zero code " + std::to_string(ev.exit_code);
                if (!best_wt_commit.valid) best_wt_commit = ev;
                break; // Most recent build on this worktree+commit failed; do not mask with an older build
            }

            // 4. Verify recorded build_artifacts against live disk SHA-256
            int64_t total_arts = 0;
            int64_t verified_arts = 0;
            int64_t mismatched_arts = 0;
            if (has_artifacts_table && !ev.build_id.empty()) {
                for (const auto& ar : db.query("SELECT * FROM build_artifacts WHERE build_id=?", {ev.build_id})) {
                    ++total_arts;
                    const std::string ap_raw = trim_copy(ar.value("artifact_path", std::string{}));
                    const std::string rec_sha = lower_ascii(trim_copy(ar.value("sha256", std::string{})));
                    const bool has_av = ar.contains("artifact_verified") && !ar["artifact_verified"].is_null();
                    const bool av_ok = !has_av || json_bool_or_int(ar, "artifact_verified", false);

                    bool disk_ok = false;
                    if (!ap_raw.empty() && !rec_sha.empty()) {
                        std::error_code aec;
                        fs::path ap(wide(ap_raw));
                        if (ap.is_relative()) ap = repo_dir / ap;
                        if (fs::is_regular_file(native_path(ap), aec)) {
                            try {
                                const std::string live_sha = lower_ascii(hash_file(ap));
                                if (!live_sha.empty() && live_sha == rec_sha) {
                                    disk_ok = true;
                                }
                            } catch (...) {}
                        }
                    }
                    if (av_ok && disk_ok) ++verified_arts;
                    else ++mismatched_arts;
                }
            }
            ev.total_arts = total_arts;
            ev.verified_arts = verified_arts;

            // 5. Provenance trustworthiness check
            if (ev.provenance_mode == "OBSERVED_BUILD") {
                if (mismatched_arts > 0) {
                    ev.status = "ARTIFACT_HASH_MISMATCH";
                    ev.mismatch_reason = "one or more artifacts from observed build failed live SHA-256 verification on disk";
                }
            } else {
                // DECLARED_EXTERNAL (or unknown provenance): require >= 1 verified artifact on disk with matching SHA-256
                if (total_arts == 0 || verified_arts == 0 || mismatched_arts > 0) {
                    ev.status = "UNVERIFIED_EXTERNAL_BUILD";
                    ev.mismatch_reason = (total_arts == 0)
                        ? "DECLARED_EXTERNAL build has zero recorded/verified artifacts on disk"
                        : "DECLARED_EXTERNAL build has missing, unverified, or SHA-256-mismatched artifacts on disk";
                }
            }

            if (ev.status == "NO_EVIDENCE") {
                if (any_uncompiled_source) {
                    ev.status = "UNCOMPILED_FILE";
                    ev.mismatch_reason = "claimed source file is absent from compile_commands.json";
                } else if (!ev.build_ts.empty() && !latest_file_mtime_utc.empty() && latest_file_mtime_utc > ev.build_ts) {
                    ev.status = "STALE_BUILD";
                    ev.mismatch_reason = "source file modified on disk after build timestamp";
                } else {
                    ev.status = "VERIFIED_BUILD_PASS";
                }
            }

            if (!best_wt_commit.valid) best_wt_commit = ev;
            if (ev.status == "VERIFIED_BUILD_PASS") {
                chosen = ev;
                break;
            }
        }
    }

    if (!chosen.valid) {
        if (best_wt_commit.valid) {
            chosen = best_wt_commit;
        } else if (best_wt.valid) {
            chosen = best_wt;
        } else if (best_any.valid) {
            chosen = best_any;
        } else {
            chosen.status = "NO_EVIDENCE";
            chosen.mismatch_reason = "no build_manifests records found";
        }
    }

    return {
        {"status", chosen.status},
        {"latest_build_id", chosen.build_id},
        {"matched_build_id", chosen.build_id},
        {"matched_git_commit", chosen.git_commit},
        {"matched_worktree_path", chosen.worktree_path},
        {"current_head", current_head},
        {"provenance_mode", chosen.provenance_mode},
        {"verified_artifacts_count", chosen.verified_arts},
        {"total_artifacts_count", chosen.total_arts},
        {"mismatch_reason", chosen.mismatch_reason},
        {"exit_code", chosen.exit_code},
        {"build_timestamp_utc", chosen.build_ts},
        {"latest_file_mtime_utc", latest_file_mtime_utc},
        {"in_compile_db", !any_uncompiled_source},
        {"verified", chosen.status == "VERIFIED_BUILD_PASS"}
    };
}

Json evaluate_tests_evidence(
    Database& db,
    const std::string& current_head,
    const std::vector<std::string>& scope_filters,
    const std::string& required_suite,
    const std::unordered_set<std::string>& extra_expected_suites,
    const std::string& latest_file_mtime_utc) {

    const std::string current_head_low = lower_ascii(current_head);
    const std::string req_suite_trimmed = trim_copy(required_suite);
    const bool has_scope_constraint = !scope_filters.empty() || !req_suite_trimmed.empty();

    std::unordered_set<std::string> expected_tokens;
    if (!req_suite_trimmed.empty()) {
        add_normalized_target_tokens(req_suite_trimmed, expected_tokens);
    } else if (!scope_filters.empty()) {
        static const std::unordered_set<std::string> kGenericDirs = {
            "src", "include", "tests", "test", "lib", "libs", "app", "core", "internal", "common"
        };
        for (const auto& sf : scope_filters) {
            add_normalized_target_tokens(sf, expected_tokens);
            // Also include non-generic parent segment (e.g. modules/auth/login.cpp -> auth)
            std::istringstream ss(lower_ascii(norm_rel_path(sf)));
            std::string seg;
            while (std::getline(ss, seg, '/')) {
                if (!seg.empty() && seg.find('.') == std::string::npos && !kGenericDirs.contains(seg)) {
                    add_normalized_target_tokens(seg, expected_tokens);
                }
            }
        }
        for (const auto& ex : extra_expected_suites) {
            add_normalized_target_tokens(ex, expected_tokens);
        }
    }

    struct RunEval {
        bool valid = false;
        std::string status = "NO_EVIDENCE";
        std::string run_id;
        std::string git_head;
        std::string run_ts;
        int64_t passed_cnt = 0;
        int64_t failed_cnt = 0;
        int64_t exit_code = -1;
        bool tests_detected = false;
        Json covered_targets = Json::array();
        std::string mismatch_reason;
    };

    RunEval chosen;
    RunEval best_failed_same_commit;
    RunEval best_stale_edit;
    RunEval best_stale_commit;
    RunEval best_unrelated;

    if (table_exists(db, "verified_runs")) {
        Json r_rows = Json::array();
        try {
            r_rows = db.query("SELECT * FROM verified_runs ORDER BY at_iso DESC, rowid DESC, id DESC LIMIT 50");
        } catch (...) {
            try {
                r_rows = db.query("SELECT * FROM verified_runs ORDER BY id DESC LIMIT 50");
            } catch (...) {}
        }

        for (const auto& r : r_rows) {
            RunEval ev;
            ev.valid = true;
            ev.run_id = r.value("id", r.value("run_id", std::string{}));
            ev.git_head = trim_copy(r.value("git_head", std::string{}));
            ev.run_ts = trim_copy(r.value("at_iso", r.value("created_at_utc", std::string{})));
            ev.passed_cnt = r.value("passed_count", int64_t{0});
            ev.failed_cnt = r.value("failed_count", int64_t{0});
            ev.exit_code = r.value("exit_code", int64_t{-1});
            const std::string r_st = upper_ascii(trim_copy(r.value("status", std::string{})));

            bool tests_detected_ok = false;
            if (r.contains("tests_detected") && !r["tests_detected"].is_null()) {
                tests_detected_ok = json_bool_or_int(r, "tests_detected", false);
            } else {
                tests_detected_ok = (r_st != "NO_TESTS_DETECTED") && (ev.passed_cnt > 0 || ev.failed_cnt > 0);
            }
            ev.tests_detected = tests_detected_ok;

            const bool ok_col_ok = (!r.contains("ok") || r["ok"].is_null()) ? true : json_bool_or_int(r, "ok", false);
            const bool status_ok = (r_st.empty() || r_st == "PASSED" || r_st == "PASS" || r_st == "OK" || r_st == "VERIFIED");
            const bool run_passed = (ev.exit_code == 0 && ev.failed_cnt == 0 && tests_detected_ok &&
                                     ev.passed_cnt > 0 && ok_col_ok && status_ok);

            bool is_full_ctest = false;
            std::unordered_set<std::string> run_tokens;
            parse_run_targets_structurally(r, is_full_ctest, run_tokens, ev.covered_targets);

            const bool covers_module = !has_scope_constraint || is_full_ctest || token_sets_intersect(run_tokens, expected_tokens);
            if (!covers_module) {
                ev.status = "NO_MODULE_TEST_EVIDENCE";
                ev.mismatch_reason = "verified_runs entry covered targets " + ev.covered_targets.dump() +
                                     " which do not cover claimed module/suite";
                if (!best_unrelated.valid) best_unrelated = ev;
                continue;
            }

            // Check git_head binding against live HEAD
            const std::string run_head_low = lower_ascii(ev.git_head);
            const bool commit_matches = current_head_low.empty() || run_head_low.empty() || (run_head_low == current_head_low);
            if (!commit_matches) {
                ev.status = "STALE_COMMIT_TEST_EVIDENCE";
                ev.mismatch_reason = "test run git_head '" + ev.git_head + "' does not match current HEAD '" + current_head + "'";
                if (!best_stale_commit.valid) best_stale_commit = ev;
                continue;
            }

            if (!run_passed) {
                ev.status = "FAILED";
                if (!tests_detected_ok || ev.passed_cnt <= 0) {
                    ev.mismatch_reason = "test run detected zero passing tests (tests_detected=" +
                                         std::string(tests_detected_ok ? "true" : "false") +
                                         ", passed_count=" + std::to_string(ev.passed_cnt) + ")";
                } else {
                    ev.mismatch_reason = "test run failed (exit_code=" + std::to_string(ev.exit_code) +
                                         ", failed_count=" + std::to_string(ev.failed_cnt) +
                                         ", status=" + r_st + ")";
                }
                if (!best_failed_same_commit.valid) best_failed_same_commit = ev;
                break; // Most recent matching run for this commit+module failed
            }

            if (!ev.run_ts.empty() && !latest_file_mtime_utc.empty() && latest_file_mtime_utc > ev.run_ts) {
                ev.status = "STALE_AFTER_EDIT";
                ev.mismatch_reason = "source file modified on disk (" + latest_file_mtime_utc +
                                     ") after test run (" + ev.run_ts + ")";
                if (!best_stale_edit.valid) best_stale_edit = ev;
                break; // Older runs are also older than latest_file_mtime_utc
            }

            ev.status = "VERIFIED_TESTS_PASS";
            chosen = ev;
            break;
        }
    }

    if (!chosen.valid) {
        if (best_failed_same_commit.valid) {
            chosen = best_failed_same_commit;
        } else if (best_stale_edit.valid) {
            chosen = best_stale_edit;
        } else if (best_stale_commit.valid) {
            chosen = best_stale_commit;
        } else if (best_unrelated.valid) {
            chosen = best_unrelated;
        } else {
            chosen.status = "NO_EVIDENCE";
            chosen.mismatch_reason = "no verified_runs records found";
        }
    }

    return {
        {"status", chosen.status},
        {"latest_run_id", chosen.run_id},
        {"matched_run_id", chosen.run_id},
        {"matched_git_head", chosen.git_head},
        {"current_head", current_head},
        {"covered_targets", chosen.covered_targets},
        {"tests_detected", chosen.tests_detected},
        {"passed_count", chosen.passed_cnt},
        {"failed_count", chosen.failed_cnt},
        {"run_timestamp_utc", chosen.run_ts},
        {"mismatch_reason", chosen.mismatch_reason},
        {"verified", chosen.status == "VERIFIED_TESTS_PASS"}
    };
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
        // Ensure SQLite audit state is fresh against current disk files (with live SHA-256 verification)
        run_incremental_audit_impl(p, db, {{"action", "scan_incremental"}, {"verify_content_hash", true}}, cancel, true);

        const fs::path repo_canon = canonical_win32_path(p.directory());
        const fs::path repo_dir = repo_canon.empty() ? p.directory() : repo_canon;
        const std::string current_head = query_git_head_commit(repo_dir);

        const std::string actor = args.value("actor", std::string{"agent"});
        const std::vector<std::string> scope_filters = collect_scope_filters(args);
        const std::string module_or_path = scope_filters.empty() ? std::string{} : scope_filters[0];
        const std::string required_suite = trim_copy(args.value("required_suite", args.value("target_suite", std::string{})));
        const std::string claim_text = args.value("claim_text", std::string{});
        const int64_t claimed_functions_count = args.value("claimed_functions_count", int64_t{-1});
        const bool claimed_compiled = json_bool_or_int(args, "claimed_compiled", false);
        const bool claimed_tests_passed = json_bool_or_int(args, "claimed_tests_passed", false);
        const bool claimed_equivalent = json_bool_or_int(args, "claimed_equivalent", false);

        // 1. Functions Evidence (AST vs Claimed) + linked covering_test_suite collection
        int64_t actual_functions_count = 0;
        int64_t clang_ast_count = 0;
        int64_t syntactic_tokens_count = 0;
        int64_t max_cc = 0;
        std::unordered_set<std::string> extra_expected_suites;
        for (const auto& fn : db.query("SELECT * FROM code_audit_functions")) {
            if (!file_matches_any_scope(fn.value("file_path", std::string{}), scope_filters)) continue;
            ++actual_functions_count;
            max_cc = (std::max)(max_cc, fn.value("cyclomatic_complexity", int64_t{1}));
            const std::string src = fn.value("analysis_source", std::string{});
            if (src == "clang_ast") ++clang_ast_count;
            else ++syntactic_tokens_count;
            const std::string cov = trim_copy(fn.value("covering_test_suite", std::string{}));
            if (!cov.empty()) extra_expected_suites.insert(cov);
        }
        if (table_exists(db, "requirement_trace_links") && !scope_filters.empty()) {
            for (const auto& tr : db.query("SELECT file_path, test_suite FROM requirement_trace_links WHERE test_suite != ''")) {
                if (file_matches_any_scope(tr.value("file_path", std::string{}), scope_filters)) {
                    const std::string ts_name = trim_copy(tr.value("test_suite", std::string{}));
                    if (!ts_name.empty()) extra_expected_suites.insert(ts_name);
                }
            }
        }

        std::string analysis_src = "none";
        if (clang_ast_count > 0 && syntactic_tokens_count == 0) analysis_src = "clang_ast";
        else if (clang_ast_count > 0 && syntactic_tokens_count > 0) analysis_src = "mixed(clang_ast,syntactic_tokens)";
        else if (syntactic_tokens_count > 0) analysis_src = "syntactic_tokens";

        const bool funcs_match = (claimed_functions_count < 0) || (actual_functions_count == claimed_functions_count);
        const Json functions_evidence = {
            {"status", funcs_match ? "AST_VERIFIED" : "COUNT_MISMATCH"},
            {"analysis_source", analysis_src},
            {"clang_ast_count", clang_ast_count},
            {"syntactic_tokens_count", syntactic_tokens_count},
            {"actual_functions_count", actual_functions_count},
            {"claimed_functions_count", claimed_functions_count},
            {"matches_claim", funcs_match}
        };

        // Collect matched files and latest disk modification timestamp
        bool any_uncompiled_source = false;
        std::string latest_file_mtime_utc;
        for (const auto& frow : db.query("SELECT * FROM code_audit_files")) {
            const std::string fp = frow.value("file_path", std::string{});
            if (!file_matches_any_scope(fp, scope_filters)) continue;
            if (frow.value("is_test_file", int64_t{0}) == 0 && frow.value("in_compile_db", int64_t{1}) == 0) {
                any_uncompiled_source = true;
            }
            const std::string mtime_iso = file_mtime_utc_iso(repo_dir / fs::path(wide(fp)));
            if (!mtime_iso.empty() && mtime_iso > latest_file_mtime_utc) {
                latest_file_mtime_utc = mtime_iso;
            }
        }
        std::error_code f_ec;
        for (const auto& sf : scope_filters) {
            const fs::path cand = repo_dir / fs::path(wide(sf));
            if (fs::is_regular_file(native_path(cand), f_ec)) {
                const std::string mtime_iso = file_mtime_utc_iso(cand);
                if (!mtime_iso.empty() && mtime_iso > latest_file_mtime_utc) {
                    latest_file_mtime_utc = mtime_iso;
                }
            }
        }

        // 2. Compilation Evidence (strict worktree + git HEAD + provenance + artifact SHA-256 binding)
        const Json compilation_evidence = evaluate_compilation_evidence(
            db, repo_dir, current_head, any_uncompiled_source, latest_file_mtime_utc);
        const std::string comp_status = compilation_evidence.value("status", std::string{"NO_EVIDENCE"});

        // 3. Tests Evidence (strict exit_code + tests_detected + git_head + post-edit + structural module/suite binding)
        const Json tests_evidence = evaluate_tests_evidence(
            db, current_head, scope_filters, required_suite, extra_expected_suites, latest_file_mtime_utc);
        const std::string tests_status = tests_evidence.value("status", std::string{"NO_EVIDENCE"});

        // 4. Quality Evidence
        int64_t conf_p0 = 0;
        int64_t conf_p1 = 0;
        int64_t conf_p2 = 0;
        int64_t conf_p3 = 0;
        int64_t pend_val = 0;
        for (const auto& fnd : db.query("SELECT * FROM code_audit_findings")) {
            if (!file_matches_any_scope(fnd.value("file_path", std::string{}), scope_filters)) continue;
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

        // Compute Overall Anti-Hallucination Verdict (AUD-MIG-P1-005):
        // NEVER return "VERIFIED" unless BOTH compilation_evidence == VERIFIED_BUILD_PASS
        // AND tests_evidence == VERIFIED_TESTS_PASS without contradictions.
        const bool build_pass = (comp_status == "VERIFIED_BUILD_PASS");
        const bool tests_pass = (tests_status == "VERIFIED_TESTS_PASS");
        const bool comp_contradiction = (comp_status == "FAILED" ||
                                         comp_status == "UNCOMPILED_FILE" ||
                                         comp_status == "STALE_BUILD" ||
                                         comp_status == "EVIDENCE_COMMIT_MISMATCH" ||
                                         comp_status == "EVIDENCE_WORKTREE_MISMATCH" ||
                                         comp_status == "UNVERIFIED_EXTERNAL_BUILD" ||
                                         comp_status == "ARTIFACT_HASH_MISMATCH");
        const bool tests_contradiction = (tests_status == "FAILED" ||
                                          tests_status == "STALE_AFTER_EDIT" ||
                                          tests_status == "STALE_COMMIT_TEST_EVIDENCE" ||
                                          tests_status == "NO_MODULE_TEST_EVIDENCE");
        const bool build_claim_failed = claimed_compiled && !build_pass;
        const bool tests_claim_failed = claimed_tests_passed && !tests_pass;
        const bool func_count_failed = !funcs_match;

        std::string verdict = "UNVERIFIED_CLAIM";
        if (func_count_failed) {
            verdict = "CONTRADICTED_BY_EVIDENCE";
        } else if ((build_claim_failed && comp_contradiction) || (tests_claim_failed && tests_contradiction)) {
            verdict = "CONTRADICTED_BY_EVIDENCE";
        } else if (build_pass && tests_pass) {
            if (claimed_equivalent && equiv_status != "VERIFIED_IN_CASES") {
                verdict = "PARTIALLY_VERIFIED";
            } else {
                verdict = "VERIFIED";
            }
        } else if (build_claim_failed || tests_claim_failed) {
            verdict = "UNVERIFIED_CLAIM";
        } else if ((build_pass || tests_pass) && !comp_contradiction && !tests_contradiction) {
            verdict = "PARTIALLY_VERIFIED";
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
            {"current_head", current_head},
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
        // AUD-MIG-P1-009: Always perform a fresh incremental rescan with live SHA-256 content-hash
        // verification before evaluating gate rules unless allow_stale_cache=true is explicitly passed.
        const bool allow_stale_cache = json_bool_or_int(args, "allow_stale_cache", false);
        Json rescan_summary = nullptr;
        if (!allow_stale_cache) {
            Json scan_args = {
                {"action", "scan_incremental"},
                {"verify_content_hash", true}
            };
            if (args.contains("compile_commands")) scan_args["compile_commands"] = args["compile_commands"];
            if (args.contains("require_compile_commands")) scan_args["require_compile_commands"] = args["require_compile_commands"];
            if (args.contains("force_full")) scan_args["force_full"] = args["force_full"];
            if (args.contains("paths")) scan_args["paths"] = args["paths"];
            if (args.contains("target_paths")) scan_args["target_paths"] = args["target_paths"];
            const Json scan_res = run_incremental_audit_impl(p, db, scan_args, cancel, true);
            rescan_summary = {
                {"snapshot_id", scan_res.value("snapshot_id", std::string{})},
                {"total_files", scan_res.value("total_files", int64_t{0})},
                {"files_audited", scan_res.value("files_audited", int64_t{0})},
                {"files_reused_cache", scan_res.value("files_reused_cache", int64_t{0})},
                {"total_functions", scan_res.value("total_functions", int64_t{0})},
                {"confirmed_p0", scan_res.value("confirmed_p0", int64_t{0})},
                {"confirmed_p1", scan_res.value("confirmed_p1", int64_t{0})},
                {"verify_content_hash", true}
            };
        }

        const bool strict_mode = json_bool_or_int(args, "strict", false) || json_bool_or_int(args, "release_gate", false);
        const int64_t max_p0 = args.value("max_confirmed_p0", int64_t{0});
        const int64_t max_p1 = (args.contains("max_confirmed_p1") && !args["max_confirmed_p1"].is_null())
            ? args["max_confirmed_p1"].get<int64_t>()
            : (strict_mode ? int64_t{0} : int64_t{-1});
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
            const fs::path repo_canon = canonical_win32_path(p.directory());
            const fs::path repo_dir = repo_canon.empty() ? p.directory() : repo_canon;
            const std::string current_head = query_git_head_commit(repo_dir);
            const std::vector<std::string> scope_filters = collect_scope_filters(args);
            const std::string required_suite = trim_copy(args.value("required_suite", args.value("target_suite", std::string{})));

            std::string latest_file_mtime_utc;
            for (const auto& frow : db.query("SELECT file_path FROM code_audit_files")) {
                const std::string fp = frow.value("file_path", std::string{});
                if (!file_matches_any_scope(fp, scope_filters)) continue;
                const std::string mtime_iso = file_mtime_utc_iso(repo_dir / fs::path(wide(fp)));
                if (!mtime_iso.empty() && mtime_iso > latest_file_mtime_utc) {
                    latest_file_mtime_utc = mtime_iso;
                }
            }

            const Json te = evaluate_tests_evidence(db, current_head, scope_filters, required_suite, {}, latest_file_mtime_utc);
            const std::string t_st = te.value("status", std::string{"NO_EVIDENCE"});
            if (t_st != "VERIFIED_TESTS_PASS") {
                reasons.push_back(t_st == "NO_EVIDENCE" ? std::string{"NO_VERIFIED_TEST_RUN"} : ("VERIFIED_TESTS_BLOCKED: " + t_st));
            }
        }

        const bool passed = reasons.empty();
        return {
            {"schema", "migratool.code_audit/1"},
            {"action", "gate_check"},
            {"ok", true},
            {"gate_passed", passed},
            {"strict", strict_mode},
            {"cache_fresh", !allow_stale_cache},
            {"rescan_performed", !allow_stale_cache},
            {"rescan_summary", rescan_summary},
            {"max_confirmed_p0", max_p0},
            {"max_confirmed_p1", max_p1},
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
