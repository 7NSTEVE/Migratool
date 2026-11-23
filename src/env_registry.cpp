#include "migratool/env_registry.hpp"
#include "migratool/fs_index.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace mt {
namespace {

std::string lower_env(std::string_view sv) {
    std::string out(sv);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string trim_env(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
    return s.substr(i);
}

std::string get_win_env(const wchar_t* name) {
    DWORD len = GetEnvironmentVariableW(name, nullptr, 0);
    if (!len) return {};
    std::wstring buf(len, L'\0');
    DWORD got = GetEnvironmentVariableW(name, buf.data(), len);
    if (!got || got >= len) return {};
    buf.resize(got);
    return utf8(buf);
}

// Standalone RAII SQLite wrapper for %LOCALAPPDATA%\Migratool\env_registry.sqlite (works WITH or WITHOUT Project!)
class EnvDb {
    sqlite3* db_ = nullptr;
public:
    explicit EnvDb(const fs::path& db_path) {
        std::error_code ec;
        fs::create_directories(native_path(db_path.parent_path()), ec);
        if (sqlite3_open_v2(path_text(db_path).c_str(), &db_,
                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
            std::string msg = db_ ? sqlite3_errmsg(db_) : "sqlite3_open_v2 failed";
            if (db_) sqlite3_close(db_);
            throw std::runtime_error("EnvDb open failed: " + msg);
        }
        exec_raw("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA busy_timeout=5000;");
        if (integer("SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='host_tools'") == 0) {
            ensure_schema();
        }
    }
    ~EnvDb() {
        if (db_) sqlite3_close(db_);
    }
    EnvDb(const EnvDb&) = delete;

    void exec_raw(const char* sql) {
        char* err = nullptr;
        if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
            std::string msg = err ? err : sqlite3_errmsg(db_);
            if (err) sqlite3_free(err);
            throw std::runtime_error("EnvDb SQL error: " + msg);
        }
    }

    void exec(const std::string& sql, const Json& params = Json::array()) {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
            throw std::runtime_error("EnvDb prepare failed: " + std::string(sqlite3_errmsg(db_)));
        }
        struct Guard { sqlite3_stmt* s; ~Guard() { sqlite3_finalize(s); } } g{st};
        for (size_t i = 0; i < params.size(); ++i) {
            const int idx = static_cast<int>(i + 1);
            const auto& v = params[i];
            if (v.is_null()) sqlite3_bind_null(st, idx);
            else if (v.is_boolean()) sqlite3_bind_int64(st, idx, v.get<bool>() ? 1 : 0);
            else if (v.is_number_integer() || v.is_number_unsigned()) sqlite3_bind_int64(st, idx, v.get<int64_t>());
            else if (v.is_number_float()) sqlite3_bind_double(st, idx, v.get<double>());
            else {
                const std::string s = v.is_string() ? v.get<std::string>() : v.dump();
                sqlite3_bind_text(st, idx, s.c_str(), static_cast<int>(s.size()), SQLITE_TRANSIENT);
            }
        }
        int rc = sqlite3_step(st);
        if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
            throw std::runtime_error("EnvDb step failed: " + std::string(sqlite3_errmsg(db_)));
        }
    }

    Json query(const std::string& sql, const Json& params = Json::array()) const {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
            throw std::runtime_error("EnvDb query prepare failed: " + std::string(sqlite3_errmsg(db_)));
        }
        struct Guard { sqlite3_stmt* s; ~Guard() { sqlite3_finalize(s); } } g{st};
        for (size_t i = 0; i < params.size(); ++i) {
            const int idx = static_cast<int>(i + 1);
            const auto& v = params[i];
            if (v.is_null()) sqlite3_bind_null(st, idx);
            else if (v.is_boolean()) sqlite3_bind_int64(st, idx, v.get<bool>() ? 1 : 0);
            else if (v.is_number_integer() || v.is_number_unsigned()) sqlite3_bind_int64(st, idx, v.get<int64_t>());
            else if (v.is_number_float()) sqlite3_bind_double(st, idx, v.get<double>());
            else {
                const std::string s = v.is_string() ? v.get<std::string>() : v.dump();
                sqlite3_bind_text(st, idx, s.c_str(), static_cast<int>(s.size()), SQLITE_TRANSIENT);
            }
        }
        Json rows = Json::array();
        const int cols = sqlite3_column_count(st);
        while (sqlite3_step(st) == SQLITE_ROW) {
            Json r = Json::object();
            for (int c = 0; c < cols; ++c) {
                const char* cname = sqlite3_column_name(st, c);
                const std::string k = cname ? cname : ("col_" + std::to_string(c));
                switch (sqlite3_column_type(st, c)) {
                    case SQLITE_INTEGER: r[k] = sqlite3_column_int64(st, c); break;
                    case SQLITE_FLOAT:   r[k] = sqlite3_column_double(st, c); break;
                    case SQLITE_TEXT: {
                        const auto* txt = reinterpret_cast<const char*>(sqlite3_column_text(st, c));
                        const int len = sqlite3_column_bytes(st, c);
                        r[k] = txt ? std::string(txt, static_cast<size_t>(len)) : std::string();
                        break;
                    }
                    default: r[k] = nullptr; break;
                }
            }
            rows.push_back(std::move(r));
        }
        return rows;
    }

    int64_t integer(const std::string& sql, const Json& params = Json::array()) const {
        auto r = query(sql, params);
        if (r.empty() || r[0].empty()) return 0;
        const auto& first = r[0].begin().value();
        return first.is_number() ? first.get<int64_t>() : 0;
    }

private:
    void ensure_schema() {
        exec_raw(R"sql(
CREATE TABLE IF NOT EXISTS host_tool_roots (
  id           INTEGER PRIMARY KEY AUTOINCREMENT,
  root_path    TEXT NOT NULL UNIQUE COLLATE NOCASE,
  alias_target TEXT NOT NULL DEFAULT '',
  kind         TEXT NOT NULL DEFAULT 'devtools',
  enabled      INTEGER NOT NULL DEFAULT 1,
  last_scan_at TEXT NOT NULL DEFAULT '',
  last_scan_ms INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS host_tools (
  tool_key        TEXT PRIMARY KEY COLLATE NOCASE,
  name            TEXT NOT NULL,
  category        TEXT NOT NULL DEFAULT 'tool',
  command         TEXT NOT NULL COLLATE NOCASE,
  version         TEXT NOT NULL DEFAULT '',
  exe_path        TEXT NOT NULL,
  real_path       TEXT NOT NULL DEFAULT '',
  root_dir        TEXT NOT NULL DEFAULT '',
  on_path         INTEGER NOT NULL DEFAULT 0,
  path_resolved   TEXT NOT NULL DEFAULT '',
  health_status   TEXT NOT NULL DEFAULT 'healthy',
  diagnostic_code TEXT NOT NULL DEFAULT 'OK',
  env_recipe      TEXT NOT NULL DEFAULT '',
  tags            TEXT NOT NULL DEFAULT '[]',
  verified_at     TEXT NOT NULL,
  exe_mtime_ns    INTEGER NOT NULL DEFAULT 0,
  pinned          INTEGER NOT NULL DEFAULT 0,
  notes           TEXT NOT NULL DEFAULT ''
);
CREATE INDEX IF NOT EXISTS idx_host_tools_cmd ON host_tools(command COLLATE NOCASE, pinned DESC);

CREATE TABLE IF NOT EXISTS project_env_profiles (
  project_key    TEXT PRIMARY KEY COLLATE NOCASE,
  project_path   TEXT NOT NULL,
  detected_langs TEXT NOT NULL DEFAULT '[]',
  bound_tools    TEXT NOT NULL DEFAULT '{}',
  build_presets  TEXT NOT NULL DEFAULT '[]',
  test_presets   TEXT NOT NULL DEFAULT '[]',
  updated_at     TEXT NOT NULL
);
)sql");
    }
};

struct KnownToolSpec {
    const char* tool_key;
    const char* name;
    const char* category;
    const char* command;
    const char* rel_devtools_hint;
    const char* version_arg;
    const char* recipe;
};

static const std::vector<KnownToolSpec> kKnownTools = {
    {"cmake",    "CMake",               "build",    "cmake",    "CMake/bin/cmake.exe",                       "--version", ""},
    {"ctest",    "CTest",               "test",     "ctest",    "CMake/bin/ctest.exe",                       "--version", ""},
    {"git",      "Git",                 "vcs",      "git",      "Tools/Git/cmd/git.exe",                     "--version", ""},
    {"python311","Python 3.11",         "language", "python",   "Python311/python.exe",                      "--version", "python311"},
    {"mathenv",  "Python MathEnv",      "language", "python",   "MathEnv/Scripts/python.exe",                "--version", "python_mathenv"},
    {"pip",      "pip",                 "package",  "pip",      "Python311/Scripts/pip.exe",                 "--version", "python311"},
    {"node",     "Node.js",             "language", "node",     "node/node.exe",                             "--version", "node"},
    {"npm",      "npm",                 "package",  "npm",      "node/npm.cmd",                              "--version", "node"},
    {"go",       "Go",                  "language", "go",       "Go/bin/go.exe",                             "version",   "go"},
    {"rustc",    "Rust (rustc)",        "language", "rustc",    "Rust/.cargo/bin/rustc.exe",                 "--version", "rust"},
    {"cargo",    "Cargo",               "build",    "cargo",    "Rust/.cargo/bin/cargo.exe",                 "--version", "rust"},
    {"java21",   "Java (JDK 21)",       "language", "java",     "jdk/bin/java.exe",                          "-version",  "jdk21"},
    {"javac21",  "javac (JDK 21)",      "compiler", "javac",    "jdk/bin/javac.exe",                         "-version",  "jdk21"},
    {"java17",   "Java 17",             "language", "java",     "JDK17/bin/java.exe",                        "-version",  "jdk17"},
    {"kotlinc",  "Kotlin (kotlinc)",    "compiler", "kotlinc",  "Kotlin/bin/kotlinc.bat",                    "-version",  "jdk21"},
    {"gradle",   "Gradle",              "build",    "gradle",   "gradle/bin/gradle.bat",                     "--version", "jdk21"},
    {"gcc",      "GCC (MSYS2 UCRT64)",  "compiler", "gcc",      "msys64/ucrt64/bin/gcc.exe",                 "--version", "msys2_ucrt64"},
    {"bash",     "MSYS2 bash",          "shell",    "bash",     "msys64/usr/bin/bash.exe",                   "--version", "msys2_ucrt64"},
    {"flutter",  "Flutter SDK",         "sdk",      "flutter",  "flutter/bin/flutter.bat",                   "--version", "flutter"},
    {"dart",     "Dart",                "language", "dart",     "flutter/bin/dart.bat",                      "--version", "flutter"},
    {"qmake",    "Qt 6 (qmake)",        "sdk",      "qmake",    "Qt/6.8.3/msvc2022_64/bin/qmake.exe",        "--version", "qt6_msvc"},
    {"vcpkg",    "vcpkg",               "package",  "vcpkg",    "vcpkg/vcpkg.exe",                           "version",   "msvc_x64"},
    {"nasm",     "NASM",                "compiler", "nasm",     "NASM/nasm.exe",                             "-v",        ""},
    {"ruby",     "Ruby",                "language", "ruby",     "Ruby/bin/ruby.exe",                         "--version", ""},
    {"adb",      "adb (Android SDK)",   "mobile",   "adb",      "AndroidSDK/platform-tools/adb.exe",         "version",   "android"},
    {"cl",       "MSVC (cl.exe)",       "compiler", "cl",       "VSBuildTools/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/cl.exe", "", "msvc_x64"},
    {"pdflatex", "LaTeX (pdflatex)",    "tex",      "pdflatex", "E:/texlive/2024/bin/windows/pdflatex.exe",  "--version", "texlive"},
    {"dotnet",   ".NET SDK",            "sdk",      "dotnet",   "C:/Program Files/dotnet/dotnet.exe",        "--version", ""}
};

std::string search_on_path_fast(const std::string& cmd_name) {
    wchar_t buf[32768]{};
    const std::wstring wcmd = wide(cmd_name);
    if (SearchPathW(nullptr, wcmd.c_str(), L".exe", 32768, buf, nullptr) > 0) {
        return path_text(fs::path(buf));
    }
    if (SearchPathW(nullptr, wcmd.c_str(), L".cmd", 32768, buf, nullptr) > 0) {
        return path_text(fs::path(buf));
    }
    if (SearchPathW(nullptr, wcmd.c_str(), L".bat", 32768, buf, nullptr) > 0) {
        return path_text(fs::path(buf));
    }
    return "";
}

std::string probe_version_fast(const fs::path& exe, const std::string& arg) {
    if (arg.empty()) return "installed";
    std::string ext = lower_env(utf8(exe.extension().wstring()));
    if (ext == ".bat" || ext == ".cmd") return "script-wrapper";

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE rpipe = nullptr, wpipe = nullptr;
    if (!CreatePipe(&rpipe, &wpipe, &sa, 0)) return "installed";
    struct HClose { HANDLE h; ~HClose() { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); } } rg{rpipe}, wg{wpipe};
    SetHandleInformation(rpipe, HANDLE_FLAG_INHERIT, 0);

    std::wstring cmd = quote_arg(exe.wstring()) + L" " + wide(arg);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wpipe;
    si.hStdError = wpipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmd_buf(cmd.begin(), cmd.end());
    cmd_buf.push_back(L'\0');
    if (!CreateProcessW(nullptr, cmd_buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi)) {
        return "installed";
    }
    CloseHandle(wpipe);
    wg.h = INVALID_HANDLE_VALUE;
    HClose ph{pi.hProcess}, th{pi.hThread};

    if (WaitForSingleObject(pi.hProcess, 1500) == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        return "installed";
    }
    std::array<char, 1024> buf{};
    DWORD got = 0;
    std::string out;
    if (ReadFile(rpipe, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr) && got > 0) {
        out.assign(buf.data(), got);
    }
    std::istringstream iss(out);
    std::string first_line;
    while (std::getline(iss, first_line)) {
        first_line = trim_env(first_line);
        if (!first_line.empty()) break;
    }
    if (first_line.size() > 100) first_line.resize(100);
    return first_line.empty() ? "installed" : first_line;
}

std::string normalize_devtools_alias(std::string p) {
    std::replace(p.begin(), p.end(), '\\', '/');
    const std::string long_prefix = "D:/Lenguajes de Programación y Herramientas de Desarrollo/";
    if (p.starts_with(long_prefix)) {
        std::error_code ec;
        if (fs::exists(L"D:\\DevTools", ec)) {
            return "D:/DevTools/" + p.substr(long_prefix.size());
        }
    }
    return p;
}

int64_t exe_mtime_ns_fast(const fs::path& p) {
    uint64_t pfrn = 0;
    int64_t sz = 0, mt = 0;
    uint32_t at = 0;
    query_path_frn(p, &pfrn, &sz, &mt, &at, nullptr);
    return mt;
}

Json run_host_scan(EnvDb& edb, const Json& args) {
    const auto t0 = std::chrono::steady_clock::now();
    const bool probe_versions = args.value("probe_versions", false);
    const std::string ts = now();

    std::vector<fs::path> scan_roots;
    if (args.contains("roots") && args["roots"].is_array()) {
        for (const auto& r : args["roots"]) {
            if (r.is_string()) scan_roots.emplace_back(wide(r.get<std::string>()));
        }
    }
    if (scan_roots.empty()) {
        auto db_roots = edb.query("SELECT root_path FROM host_tool_roots WHERE enabled=1 ORDER BY id");
        for (const auto& r : db_roots) scan_roots.emplace_back(wide(r["root_path"].get<std::string>()));
    }
    if (scan_roots.empty()) {
        std::error_code ec;
        wchar_t env_root[32768]{};
        const DWORD env_len = GetEnvironmentVariableW(L"MIGRATOOL_DEVTOOLS_ROOT", env_root, 32768);
        if (env_len > 0 && env_len < 32768 && fs::exists(fs::path(env_root), ec)) {
            scan_roots.emplace_back(env_root);
        } else if (fs::exists(L"D:\\DevTools", ec)) {
            scan_roots.emplace_back(L"D:\\DevTools");
        } else if (fs::exists(L"D:\\Lenguajes de Programación y Herramientas de Desarrollo", ec)) {
            scan_roots.emplace_back(L"D:\\Lenguajes de Programación y Herramientas de Desarrollo");
        }
        if (fs::exists(L"E:\\texlive", ec)) scan_roots.emplace_back(L"E:\\texlive");
    }

    int64_t discovered = 0;
    int64_t shadows = 0;
    int64_t duplicates = 0;

    // 1. Register roots in host_tool_roots
    for (const auto& sr : scan_roots) {
        std::error_code ec;
        const bool exists = fs::exists(native_path(sr), ec);
        std::string alias_t;
        if (exists) {
            auto wc = fs::weakly_canonical(sr, ec);
            if (!ec) alias_t = path_text(wc);
        }
        edb.exec(
            "INSERT OR REPLACE INTO host_tool_roots(root_path,alias_target,kind,enabled,last_scan_at,last_scan_ms)"
            "VALUES(?,?, 'devtools', 1, ?, 0)",
            {path_text(sr), alias_t, ts});
    }

    // Determine main DevTools root (either D:\DevTools or first custom root passed in tests)
    const fs::path primary_root = scan_roots.empty() ? fs::path(L"D:\\DevTools") : scan_roots.front();

    // 2. Scan known tools + custom executables in roots
    std::unordered_map<std::string, int> cmd_counts;
    struct DiscoveredTool {
        std::string tool_key;
        std::string name;
        std::string category;
        std::string command;
        fs::path exe_path;
        std::string root_dir;
        std::string version_arg;
        std::string recipe;
        bool pinned = false;
    };
    std::vector<DiscoveredTool> found_tools;
    std::set<std::string> seen_exe_low;

    for (const auto& kt : kKnownTools) {
        fs::path hint(wide(kt.rel_devtools_hint));
        fs::path cand = hint.is_absolute() ? hint : (primary_root / hint);
        std::error_code ec;
        if (fs::is_regular_file(native_path(cand), ec)) {
            const std::string clean_exe = normalize_devtools_alias(path_text(cand));
            if (seen_exe_low.insert(lower_env(clean_exe)).second) {
                found_tools.push_back(DiscoveredTool{
                    kt.tool_key, kt.name, kt.category, kt.command,
                    fs::path(wide(clean_exe)), path_text(primary_root),
                    kt.version_arg, kt.recipe, clean_exe.starts_with("D:/DevTools")
                });
                cmd_counts[lower_env(kt.command)]++;
            }
        }
    }

    // Also scan up to 2 levels deep inside any custom root for *.exe / *.bat / *.cmd (crucial for synthetic tree tests & custom SDKs)
    for (const auto& sr : scan_roots) {
        std::error_code ec;
        if (!fs::is_directory(native_path(sr), ec)) continue;
        for (fs::recursive_directory_iterator it(native_path(sr), fs::directory_options::skip_permission_denied, ec), end;
             it != end && !ec; it.increment(ec)) {
            if (it.depth() > 3) {
                it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file(ec)) continue;
            const std::string ext = lower_env(utf8(it->path().extension().wstring()));
            if (ext != ".exe" && ext != ".cmd" && ext != ".bat") continue;
            const std::string stem = lower_env(utf8(it->path().stem().wstring()));
            if (stem.starts_with("unins") || stem.starts_with("vc_redist")) continue;

            const std::string clean_exe = normalize_devtools_alias(path_text(it->path()));
            if (!seen_exe_low.insert(lower_env(clean_exe)).second) continue;

            std::string key = stem;
            if (cmd_counts[stem] > 0) {
                key = stem + "_" + lower_env(utf8(it->path().parent_path().filename().wstring()));
            }
            found_tools.push_back(DiscoveredTool{
                key, stem, "tool", stem,
                fs::path(wide(clean_exe)), normalize_devtools_alias(path_text(sr)),
                "--version", "", clean_exe.starts_with("D:/DevTools")
            });
            cmd_counts[stem]++;
            if (found_tools.size() >= 180) break;
        }
    }

    // Optional simulated PATH map for deterministic unit testing of PATH_SHADOWED
    std::unordered_map<std::string, std::string> simulated_path_map;
    if (args.contains("simulated_path_map") && args["simulated_path_map"].is_object()) {
        for (auto it = args["simulated_path_map"].begin(); it != args["simulated_path_map"].end(); ++it) {
            if (it.value().is_string()) {
                simulated_path_map[lower_env(it.key())] = normalize_devtools_alias(it.value().get<std::string>());
            }
        }
    }

    edb.exec_raw("BEGIN IMMEDIATE;");
    try {
        for (const auto& dt : found_tools) {
            std::error_code ec;
            const bool exists = fs::is_regular_file(native_path(dt.exe_path), ec);
            const int64_t mt = exists ? exe_mtime_ns_fast(dt.exe_path) : 0;
            std::string real_p = path_text(dt.exe_path);
            if (exists) {
                auto wc = fs::weakly_canonical(dt.exe_path, ec);
                if (!ec) real_p = path_text(wc);
            }

            // Check cached version if mtime hasn't changed
            std::string ver = "installed";
            int pinned = dt.pinned ? 1 : 0;
            auto prev = edb.query("SELECT version, exe_mtime_ns, pinned FROM host_tools WHERE tool_key=?", {dt.tool_key});
            if (!prev.empty()) {
                pinned = static_cast<int>(prev[0]["pinned"].get<int64_t>());
                if (prev[0]["exe_mtime_ns"].get<int64_t>() == mt && !prev[0]["version"].get<std::string>().empty()) {
                    ver = prev[0]["version"].get<std::string>();
                } else if (probe_versions) {
                    ver = probe_version_fast(dt.exe_path, dt.version_arg);
                }
            } else if (probe_versions) {
                ver = probe_version_fast(dt.exe_path, dt.version_arg);
            }

            std::string path_res;
            const std::string cmd_low = lower_env(dt.command);
            if (auto sit = simulated_path_map.find(cmd_low); sit != simulated_path_map.end()) {
                path_res = sit->second;
            } else {
                path_res = normalize_devtools_alias(search_on_path_fast(dt.command));
            }

            const std::string exe_norm = normalize_devtools_alias(path_text(dt.exe_path));
            const bool on_path = !path_res.empty() && (lower_env(path_res) == lower_env(exe_norm) || lower_env(path_res) == lower_env(real_p));

            std::string health = "healthy";
            std::string diag = "OK";
            std::string notes;
            if (!exists) {
                health = "error";
                diag = "BROKEN_JUNCTION";
                notes = "Ejecutable o junction destino no existe";
            } else if (!path_res.empty() && !on_path) {
                health = "warning";
                diag = "PATH_SHADOWED";
                notes = "PATH resuelve '" + dt.command + "' a '" + path_res + "' en lugar de '" + exe_norm + "'";
                ++shadows;
            } else if (cmd_counts[cmd_low] > 1) {
                diag = "DUPLICATE_VERSION";
                notes = "Múltiples instalaciones de '" + dt.command + "' detectadas";
                ++duplicates;
            }

            edb.exec(
                "INSERT OR REPLACE INTO host_tools("
                "tool_key,name,category,command,version,exe_path,real_path,root_dir,on_path,path_resolved,"
                "health_status,diagnostic_code,env_recipe,tags,verified_at,exe_mtime_ns,pinned,notes)"
                "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                {dt.tool_key, dt.name, dt.category, dt.command, ver, exe_norm, real_p, dt.root_dir,
                 on_path ? 1 : 0, path_res, health, diag, dt.recipe, Json::array({dt.category}).dump(),
                 ts, mt, pinned, notes});
            ++discovered;
        }

        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        for (const auto& sr : scan_roots) {
            edb.exec("UPDATE host_tool_roots SET last_scan_at=?, last_scan_ms=? WHERE root_path=?",
                     {ts, elapsed_ms, path_text(sr)});
        }
        edb.exec_raw("COMMIT;");
    } catch (...) {
        try { edb.exec_raw("ROLLBACK;"); } catch (...) {}
        throw;
    }

    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();

    return {
        {"schema", "migratool.env_tools/1"},
        {"action", "scan"},
        {"ok", true},
        {"tools_discovered", discovered},
        {"path_shadows_detected", shadows},
        {"duplicate_versions_detected", duplicates},
        {"elapsed_ms", elapsed_ms}
    };
}

} // namespace

fs::path env_registry_db_path() {
    if (auto ov = get_win_env(L"MIGRATOOL_ENV_REGISTRY_PATH"); !ov.empty()) {
        return fs::absolute(fs::path(wide(ov)));
    }
    if (auto la = get_win_env(L"LOCALAPPDATA"); !la.empty()) {
        return fs::path(wide(la)) / L"Migratool" / L"env_registry.sqlite";
    }
    return fs::temp_directory_path() / L"Migratool" / L"env_registry.sqlite";
}

std::mutex& env_db_mutex() {
    static std::mutex m;
    return m;
}

EnvDb& shared_env_db() {
    static std::wstring cur_path_w;
    static std::unique_ptr<EnvDb> inst;
    const fs::path target = env_registry_db_path();
    if (!inst || cur_path_w != target.wstring()) {
        inst.reset();
        cur_path_w = target.wstring();
        inst = std::make_unique<EnvDb>(target);
    }
    return *inst;
}

Json env_lookup_tool(const std::string& query_or_key, bool verify_live) {
    std::lock_guard<std::mutex> lk(env_db_mutex());
    EnvDb& edb = shared_env_db();
    if (edb.integer("SELECT COUNT(*) FROM host_tools") == 0) {
        run_host_scan(edb, {{"probe_versions", false}});
    }
    const std::string q = trim_env(query_or_key);
    auto rows = edb.query(
        "SELECT tool_key, name, category, command, version, exe_path, real_path, root_dir, on_path, path_resolved, "
        "health_status, diagnostic_code, env_recipe, pinned, notes "
        "FROM host_tools WHERE tool_key=? COLLATE NOCASE OR command=? COLLATE NOCASE OR name LIKE ? COLLATE NOCASE "
        "ORDER BY pinned DESC, (exe_path LIKE 'D:/DevTools/%') DESC, tool_key ASC LIMIT 10",
        {q, q, "%" + q + "%"});

    if (verify_live && !rows.empty()) {
        for (auto& r : rows) {
            std::error_code ec;
            const fs::path p(wide(r["exe_path"].get<std::string>()));
            r["exists_live"] = fs::is_regular_file(native_path(p), ec);
        }
    }
    return {
        {"schema", "migratool.env_tools/1"},
        {"action", "query"},
        {"query", q},
        {"found", !rows.empty()},
        {"preferred", rows.empty() ? Json(nullptr) : rows[0]},
        {"matches", std::move(rows)}
    };
}

std::map<std::string, std::string> env_materialize_recipe(const std::string& recipe_name) {
    const std::string r = lower_env(trim_env(recipe_name));
    const std::string cur_path = get_win_env(L"PATH");
    std::map<std::string, std::string> env;

    if (r == "msvc_x64" || r == "vcvars64") {
        env["PATH"] = "D:\\DevTools\\VSBuildTools\\VC\\Tools\\MSVC\\14.44.35207\\bin\\Hostx64\\x64;"
                      "D:\\DevTools\\CMake\\bin;D:\\DevTools\\vcpkg;" + cur_path;
        env["VCPKG_ROOT"] = "D:\\DevTools\\vcpkg";
        env["CC"] = "D:/DevTools/VSBuildTools/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/cl.exe";
        env["CXX"] = "D:/DevTools/VSBuildTools/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/cl.exe";
    } else if (r == "msys2_ucrt64" || r == "gcc") {
        env["PATH"] = "D:\\DevTools\\msys64\\ucrt64\\bin;D:\\DevTools\\msys64\\usr\\bin;" + cur_path;
        env["MSYS2_ROOT"] = "D:\\DevTools\\msys64";
    } else if (r == "jdk21" || r == "java21") {
        env["JAVA_HOME"] = "D:\\DevTools\\jdk";
        env["PATH"] = "D:\\DevTools\\jdk\\bin;D:\\DevTools\\gradle\\bin;D:\\DevTools\\Kotlin\\bin;" + cur_path;
    } else if (r == "jdk17" || r == "java17") {
        env["JAVA_HOME"] = "D:\\DevTools\\JDK17";
        env["PATH"] = "D:\\DevTools\\JDK17\\bin;" + cur_path;
    } else if (r == "python_mathenv" || r == "mathenv") {
        env["VIRTUAL_ENV"] = "D:\\DevTools\\MathEnv";
        env["PATH"] = "D:\\DevTools\\MathEnv\\Scripts;" + cur_path;
    } else if (r == "python311" || r == "python") {
        env["PATH"] = "D:\\DevTools\\Python311;D:\\DevTools\\Python311\\Scripts;" + cur_path;
    } else if (r == "rust" || r == "cargo") {
        env["CARGO_HOME"] = "D:\\DevTools\\Rust\\.cargo";
        env["RUSTUP_HOME"] = "D:\\DevTools\\Rust\\.rustup";
        env["PATH"] = "D:\\DevTools\\Rust\\.cargo\\bin;" + cur_path;
    } else if (r == "go") {
        env["GOROOT"] = "D:\\DevTools\\Go";
        env["PATH"] = "D:\\DevTools\\Go\\bin;" + cur_path;
    } else if (r == "node") {
        env["PATH"] = "D:\\DevTools\\node;" + cur_path;
    } else if (r == "flutter") {
        env["PATH"] = "D:\\DevTools\\flutter\\bin;D:\\DevTools\\AndroidSDK\\platform-tools;" + cur_path;
    }
    return env;
}

Json env_resolve_project_profile(const Project& p) {
    std::lock_guard<std::mutex> lk(env_db_mutex());
    EnvDb& edb = shared_env_db();
    if (edb.integer("SELECT COUNT(*) FROM host_tools") == 0) {
        run_host_scan(edb, {{"probe_versions", false}});
    }
    Json langs = Json::array();
    Json bound = Json::object();
    std::error_code ec;
    for (const auto& r : p.config.value("roots", Json::array())) {
        const fs::path rb = p.resolve(r.value("path", ""));
        if (fs::is_regular_file(native_path(rb / L"CMakeLists.txt"), ec)) {
            langs.push_back("cpp");
            bound["cmake"] = "D:/DevTools/CMake/bin/cmake.exe";
            bound["ctest"] = "D:/DevTools/CMake/bin/ctest.exe";
            bound["recipe"] = "msvc_x64";
        }
        if (fs::is_regular_file(native_path(rb / L"package.json"), ec)) {
            langs.push_back("javascript");
            bound["node"] = "D:/DevTools/node/node.exe";
        }
        if (fs::is_regular_file(native_path(rb / L"pyproject.toml"), ec) ||
            fs::is_regular_file(native_path(rb / L"requirements.txt"), ec)) {
            langs.push_back("python");
            bound["python"] = "D:/DevTools/Python311/python.exe";
        }
        if (fs::is_regular_file(native_path(rb / L"Cargo.toml"), ec)) {
            langs.push_back("rust");
            bound["cargo"] = "D:/DevTools/Rust/.cargo/bin/cargo.exe";
        }
        if (fs::is_regular_file(native_path(rb / L"go.mod"), ec)) {
            langs.push_back("go");
            bound["go"] = "D:/DevTools/Go/bin/go.exe";
        }
    }
    const std::string pkey = path_text(p.file);
    edb.exec(
        "INSERT OR REPLACE INTO project_env_profiles(project_key,project_path,detected_langs,bound_tools,updated_at)"
        "VALUES(?,?,?,?,?)",
        {pkey, path_text(p.directory()), langs.dump(), bound.dump(), now()});
    return {
        {"project_key", pkey},
        {"detected_langs", std::move(langs)},
        {"bound_tools", std::move(bound)}
    };
}

Json tool_env_tools(const std::optional<Project>& p, const Json& args) {
    const std::string action = args.value("action", args.value("op", "summary"));

    if (action == "query") {
        const std::string q = args.value("query", args.value("tool", args.value("command", "")));
        if (q.empty()) throw std::runtime_error("env_tools(query) requiere query o tool");
        return env_lookup_tool(q, args.value("verify_live", false));
    }

    if (action == "bind_project") {
        if (!p.has_value()) throw std::runtime_error("env_tools(bind_project) requiere un proyecto activo");
        auto prof = env_resolve_project_profile(*p);
        prof["schema"] = "migratool.env_tools/1";
        prof["action"] = "bind_project";
        return prof;
    }

    std::lock_guard<std::mutex> lk(env_db_mutex());
    EnvDb& edb = shared_env_db();

    if (action == "scan") {
        return run_host_scan(edb, args);
    }

    if (edb.integer("SELECT COUNT(*) FROM host_tools") == 0) {
        run_host_scan(edb, {{"probe_versions", false}});
    }

    if (action == "register_root") {
        const std::string rp = args.at("root_path").get<std::string>();
        edb.exec("INSERT OR REPLACE INTO host_tool_roots(root_path,kind,enabled,last_scan_at) VALUES(?,?,1,?)",
                 {rp, args.value("kind", "custom"), now()});
        if (args.value("scan_now", true)) {
            return run_host_scan(edb, {{"roots", Json::array({rp})}, {"probe_versions", args.value("probe_versions", false)}});
        }
        return {{"schema", "migratool.env_tools/1"}, {"action", "register_root"}, {"ok", true}, {"root_path", rp}};
    }

    if (action == "pin_tool") {
        const std::string tk = args.at("tool_key").get<std::string>();
        const bool pin = args.value("pinned", true);
        edb.exec("UPDATE host_tools SET pinned=? WHERE tool_key=? COLLATE NOCASE", {pin ? 1 : 0, tk});
        return {{"schema", "migratool.env_tools/1"}, {"action", "pin_tool"}, {"ok", true}, {"tool_key", tk}, {"pinned", pin}};
    }

    if (action == "doctor") {
        auto issues = edb.query(
            "SELECT tool_key, name, command, version, exe_path, path_resolved, health_status, diagnostic_code, notes "
            "FROM host_tools WHERE diagnostic_code != 'OK' ORDER BY diagnostic_code, tool_key");
        return {
            {"schema", "migratool.env_tools/1"},
            {"action", "doctor"},
            {"issues_count", issues.size()},
            {"issues", std::move(issues)}
        };
    }

    if (action == "export_md") {
        auto tools = edb.query(
            "SELECT tool_key, name, category, command, version, exe_path, diagnostic_code, env_recipe, notes "
            "FROM host_tools ORDER BY category, tool_key");
        std::ostringstream md;
        md << "# Inventario de Lenguajes y Herramientas del Host (Migratool v3.0)\n\n";
        md << "| Clave | Nombre | Comando | Ruta Preferida (`D:\\DevTools`) | Diagnóstico | Receta |\n";
        md << "|---|---|---|---|---|---|\n";
        for (const auto& t : tools) {
            md << "| `" << t["tool_key"].get<std::string>() << "` | " << t["name"].get<std::string>()
               << " | `" << t["command"].get<std::string>() << "` | `" << t["exe_path"].get<std::string>()
               << "` | `" << t["diagnostic_code"].get<std::string>() << "` | `" << t["env_recipe"].get<std::string>() << "` |\n";
        }
        return {
            {"schema", "migratool.env_tools/1"},
            {"action", "export_md"},
            {"count", tools.size()},
            {"markdown", md.str()}
        };
    }

    // Default: summary (including recipe materialization if recipe is passed)
    auto tools = edb.query(
        "SELECT tool_key, name, category, command, version, exe_path, real_path, root_dir, on_path, path_resolved, "
        "health_status, diagnostic_code, env_recipe, pinned, notes "
        "FROM host_tools ORDER BY pinned DESC, category ASC, tool_key ASC");
    auto roots = edb.query("SELECT root_path, alias_target, kind, enabled, last_scan_at, last_scan_ms FROM host_tool_roots ORDER BY id");
    const int64_t shadow_cnt = edb.integer("SELECT COUNT(*) FROM host_tools WHERE diagnostic_code='PATH_SHADOWED'");
    const int64_t dup_cnt = edb.integer("SELECT COUNT(*) FROM host_tools WHERE diagnostic_code='DUPLICATE_VERSION'");

    Json recipe_env = Json::object();
    if (args.contains("recipe") && args["recipe"].is_string()) {
        for (const auto& [k, v] : env_materialize_recipe(args["recipe"].get<std::string>())) {
            recipe_env[k] = v;
        }
    }

    return {
        {"schema", "migratool.env_tools/1"},
        {"action", "summary"},
        {"db_path", path_text(env_registry_db_path())},
        {"tools_count", tools.size()},
        {"path_shadows_count", shadow_cnt},
        {"duplicate_versions_count", dup_cnt},
        {"roots", std::move(roots)},
        {"tools", std::move(tools)},
        {"recipe_env", std::move(recipe_env)}
    };
}

} // namespace mt
