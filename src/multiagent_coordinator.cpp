#include "migratool/multiagent_coordinator.hpp"
#include "migratool/code_audit.hpp"
#include "migratool/housekeep.hpp"
#include "migratool/workstream.hpp"

#include <shellapi.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cwctype>
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

std::string canonical_path_str(const fs::path& p) {
    if (p.empty()) return {};
    fs::path cp = canonical_win32_path(p);
    if (cp.empty()) {
        std::error_code ec;
        cp = p.is_absolute() ? p.lexically_normal() : fs::absolute(p, ec).lexically_normal();
    }
    std::string s = path_text(cp);
    while (s.size() > 3 && s.back() == '/') {
        s.pop_back();
    }
    return s;
}

std::string normalize_path_key(const fs::path& p) {
    return lower_ascii(canonical_path_str(p));
}

std::string normalize_path_key_str(const std::string& raw) {
    if (raw.empty()) return {};
    return normalize_path_key(fs::path(wide(raw)));
}

bool table_exists(Database& db, const std::string& name) {
    return db.integer("SELECT EXISTS(SELECT 1 FROM sqlite_master WHERE type IN('table','view') AND name=?)", {name}) != 0;
}

bool pid_alive(int64_t pid) {
    if (pid <= 0) return false;
    if (static_cast<DWORD>(pid) == GetCurrentProcessId()) return true;
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!h) {
        return GetLastError() == ERROR_ACCESS_DENIED;
    }
    const bool alive = (WaitForSingleObject(h, 0) == WAIT_TIMEOUT);
    CloseHandle(h);
    return alive;
}

bool send_file_to_recycle_bin(const fs::path& p) {
    std::error_code ec;
    if (!fs::exists(native_path(p), ec)) return false;
    std::wstring abs = canonical_win32_path(p).wstring();
    if (abs.empty()) abs = fs::absolute(p, ec).wstring();
    if (abs.starts_with(L"\\\\?\\UNC\\")) abs = L"\\\\" + abs.substr(8);
    else if (abs.starts_with(L"\\\\?\\")) abs = abs.substr(4);
    for (auto& c : abs) {
        if (c == L'/') c = L'\\';
    }
    while (abs.size() > 3 && abs.back() == L'\\') {
        abs.pop_back();
    }
    abs.push_back(L'\0');
    abs.push_back(L'\0');
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = abs.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
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

struct GitExecResult {
    int exit_code = -1;
    bool timed_out = false;
    std::string output;
};

GitExecResult run_git_isolated(const fs::path& repo_dir, const std::vector<std::string>& git_args, DWORD timeout_ms = 10000) {
    GitExecResult res;
    wchar_t dis[16]{};
    if (GetEnvironmentVariableW(L"MIGRATOOL_DISABLE_GIT", dis, 16) > 0 && std::wstring_view(dis) == L"1") {
        res.output = "git disabled by MIGRATOOL_DISABLE_GIT";
        return res;
    }
    std::error_code ec;
    if (repo_dir.empty() || !fs::is_directory(native_path(repo_dir), ec)) {
        return res;
    }
    const fs::path git_exe = locate_git_exe();
    if (git_exe.empty()) return res;

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

    const fs::path clean_repo = canonical_win32_path(repo_dir);
    std::wstring cmd = quote_arg(git_exe.wstring()) + L" -C " + quote_arg((clean_repo.empty() ? repo_dir : clean_repo).wstring());
    for (const auto& a : git_args) {
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

    if (!CreateProcessW(nullptr, cmd_buf.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi)) {
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
            if (out.size() < 8 * 1024 * 1024) {
                const size_t take = std::min<size_t>(got, 8 * 1024 * 1024 - out.size());
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
        CancelIoEx(rpipe, nullptr);
    }
    if (reader.joinable()) reader.join();

    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    res.exit_code = res.timed_out ? -1 : static_cast<int>(code);
    res.output = std::move(out);
    return res;
}

bool is_valid_lifecycle_state(const std::string& s) {
    return s == "IMPLEMENTED" || s == "CONFIRMED" || s == "INTEGRATED" || s == "DELIVERED";
}

void validate_lifecycle_state_or_throw(const std::string& s) {
    if (!is_valid_lifecycle_state(s)) {
        throw std::runtime_error("INVALID_LIFECYCLE_STATE: state must be one of IMPLEMENTED, CONFIRMED, INTEGRATED, DELIVERED (got '" + s + "')");
    }
}

bool is_valid_directory_category(const std::string& c) {
    static const std::unordered_set<std::string> kValid = {
        "SOURCE", "TEST_SOURCE", "BUILD_OUTPUT", "GENERATED",
        "DEPENDENCY", "CACHE", "DOCUMENTATION", "USER_DATA", "UNKNOWN"
    };
    return kValid.contains(c);
}

bool is_valid_conservation_tier(const std::string& t) {
    static const std::unordered_set<std::string> kValid = {
        "PROTECTED", "TEMP_RETAINED", "VERIFIED_SAFE_CLEAN", "NEEDS_REVIEW"
    };
    return kValid.contains(t);
}

struct LiveWorktreeMetrics {
    bool is_git_worktree = false;
    std::string branch;
    std::string head_commit;
    int64_t uncommitted_count = 0;
    int64_t untracked_count = 0;
    int64_t ahead_count = 0;
    int64_t behind_count = 0;
    int64_t disk_bytes = 0;
};

int64_t compute_bounded_dir_bytes(const fs::path& dir, size_t max_entries = 2500, size_t* out_file_count = nullptr) {
    std::error_code ec;
    if (!fs::is_directory(native_path(dir), ec)) return 0;
    int64_t total = 0;
    size_t entries = 0;
    size_t files = 0;
    for (fs::recursive_directory_iterator it(native_path(dir), fs::directory_options::skip_permission_denied, ec), end;
         it != end && entries < max_entries; it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        ++entries;
        if (it->is_symlink(ec)) {
            if (it->is_directory(ec)) it.disable_recursion_pending();
            continue;
        }
        if (it->is_regular_file(ec)) {
            ++files;
            const auto sz = it->file_size(ec);
            if (!ec) total += static_cast<int64_t>(sz);
        }
    }
    if (out_file_count) *out_file_count = files;
    return total;
}

LiveWorktreeMetrics inspect_live_worktree(const fs::path& wt_dir,
                                          const std::string& target_branch = "main",
                                          const std::string& known_branch = "",
                                          const std::string& known_head = "",
                                          const fs::path& fallback_repo_dir = {}) {
    LiveWorktreeMetrics m;
    m.branch = known_branch;
    m.head_commit = known_head;
    std::error_code ec;
    if (!fs::is_directory(native_path(wt_dir), ec)) return m;
    m.disk_bytes = compute_bounded_dir_bytes(wt_dir, 1500);

    const auto st = run_git_isolated(wt_dir, {"status", "--porcelain=v2", "--branch"}, 15000);
    if (st.exit_code == 0) {
        m.is_git_worktree = true;
        std::istringstream iss(st.output);
        std::string line;
        while (std::getline(iss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.starts_with("# branch.oid ")) {
                const std::string oid = trim_copy(line.substr(13));
                if (oid != "(initial)" && !oid.empty()) m.head_commit = oid;
            } else if (line.starts_with("# branch.head ")) {
                const std::string b = trim_copy(line.substr(14));
                if (b != "(detached)" && !b.empty()) m.branch = b;
            } else if (line.starts_with("# branch.ab ")) {
                std::istringstream ab(line.substr(12));
                std::string a_str, b_str;
                if (ab >> a_str >> b_str) {
                    try {
                        if (a_str.starts_with("+")) m.ahead_count = std::stoll(a_str.substr(1));
                        if (b_str.starts_with("-")) m.behind_count = std::stoll(b_str.substr(1));
                    } catch (...) {}
                }
            } else if (line.starts_with("1 ") || line.starts_with("2 ") || line.starts_with("u ")) {
                ++m.uncommitted_count;
            } else if (line.starts_with("? ")) {
                ++m.untracked_count;
            }
        }
    } else if (!known_branch.empty() || !known_head.empty()) {
        m.is_git_worktree = true;
    } else {
        return m;
    }

    if (m.head_commit.empty()) {
        const auto rev = run_git_isolated(wt_dir, {"rev-parse", "HEAD"}, 10000);
        if (rev.exit_code == 0) m.head_commit = trim_copy(rev.output);
    }

    if (!target_branch.empty() && (!m.head_commit.empty() || !m.branch.empty()) && m.branch != target_branch) {
        const std::string left_ref = !m.head_commit.empty() ? m.head_commit : (!m.branch.empty() ? m.branch : "HEAD");
        auto lr = run_git_isolated(wt_dir, {"rev-list", "--left-right", "--count", left_ref + "..." + target_branch}, 15000);
        if (lr.exit_code != 0 && !fallback_repo_dir.empty()) {
            lr = run_git_isolated(fallback_repo_dir, {"rev-list", "--left-right", "--count", left_ref + "..." + target_branch}, 15000);
        }
        if (lr.exit_code == 0) {
            std::istringstream lrs(lr.output);
            int64_t ahead = 0, behind = 0;
            if (lrs >> ahead >> behind) {
                m.ahead_count = ahead;
                m.behind_count = behind;
            }
        }
    }
    return m;
}

std::string default_worktree_id(const std::string& canonical_wt_path) {
    return "wt-" + sha256(lower_ascii(canonical_wt_path)).substr(0, 12);
}

std::string detect_main_branch(const fs::path& repo_dir, const std::string& requested_target) {
    if (!requested_target.empty()) return requested_target;
    const auto br = run_git_isolated(repo_dir, {"rev-parse", "--abbrev-ref", "HEAD"}, 10000);
    if (br.exit_code == 0) {
        const std::string b = trim_copy(br.output);
        if (!b.empty() && b != "HEAD") return b;
    }
    return "main";
}

bool git_commit_exists(const fs::path& repo_dir, const std::string& ref) {
    if (ref.empty()) return false;
    const auto r = run_git_isolated(repo_dir, {"rev-parse", "--verify", "--quiet", ref + "^{commit}"}, 4000);
    return r.exit_code == 0 && !trim_copy(r.output).empty();
}

Json parse_json_safe(const std::string& raw, const Json& fallback) {
    if (raw.empty()) return fallback;
    try {
        return Json::parse(raw);
    } catch (...) {
        return fallback;
    }
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

Json format_worktree_row(const Json& row) {
    Json out = row;
    out["is_mandatory"] = json_bool_or_int(row, "is_mandatory", true);
    out["is_excluded"] = !row.value("excluded_by", std::string{}).empty();
    const int64_t pid = row.value("active_build_pid", int64_t{0});
    out["active_build_running"] = pid_alive(pid);
    return out;
}

bool is_worktree_unintegrated_mandatory(const Json& wt_row) {
    const bool mandatory = json_bool_or_int(wt_row, "is_mandatory", true);
    const std::string excluded_by = wt_row.value("excluded_by", std::string{});
    const std::string state = wt_row.value("lifecycle_state", std::string{"IMPLEMENTED"});
    if (!mandatory || !excluded_by.empty()) return false;
    return (state == "IMPLEMENTED" || state == "CONFIRMED");
}

} // namespace

void ensure_multiagent_coordinator_schema(Database& db) {
    db.exec(R"SQL(
        CREATE TABLE IF NOT EXISTS agent_worktrees_catalog (
            worktree_id TEXT PRIMARY KEY,
            project_name TEXT NOT NULL DEFAULT '',
            worktree_path TEXT NOT NULL UNIQUE,
            branch TEXT NOT NULL DEFAULT '',
            head_commit TEXT NOT NULL DEFAULT '',
            base_commit TEXT NOT NULL DEFAULT '',
            owner_agent TEXT NOT NULL DEFAULT '',
            workstream_id TEXT NOT NULL DEFAULT '',
            task_id TEXT NOT NULL DEFAULT '',
            goal TEXT NOT NULL DEFAULT '',
            lifecycle_state TEXT NOT NULL DEFAULT 'IMPLEMENTED',
            is_mandatory INTEGER NOT NULL DEFAULT 1,
            excluded_by TEXT NOT NULL DEFAULT '',
            exclusion_reason TEXT NOT NULL DEFAULT '',
            excluded_at_utc TEXT NOT NULL DEFAULT '',
            uncommitted_count INTEGER NOT NULL DEFAULT 0,
            untracked_count INTEGER NOT NULL DEFAULT 0,
            ahead_count INTEGER NOT NULL DEFAULT 0,
            behind_count INTEGER NOT NULL DEFAULT 0,
            active_build_pid INTEGER NOT NULL DEFAULT 0,
            disk_bytes INTEGER NOT NULL DEFAULT 0,
            pinned_snapshot_id TEXT NOT NULL DEFAULT '',
            notes TEXT NOT NULL DEFAULT '',
            last_modified_utc TEXT NOT NULL DEFAULT '',
            updated_at_utc TEXT NOT NULL DEFAULT ''
        )
    )SQL");

    db.exec(R"SQL(
        CREATE TABLE IF NOT EXISTS integration_queue (
            queue_id INTEGER PRIMARY KEY AUTOINCREMENT,
            worktree_id TEXT NOT NULL UNIQUE,
            worktree_path TEXT NOT NULL DEFAULT '',
            branch TEXT NOT NULL DEFAULT '',
            head_commit TEXT NOT NULL DEFAULT '',
            target_branch TEXT NOT NULL DEFAULT 'main',
            owner_agent TEXT NOT NULL DEFAULT '',
            lifecycle_state TEXT NOT NULL DEFAULT 'IMPLEMENTED',
            conflict_status TEXT NOT NULL DEFAULT 'UNVERIFIED',
            merge_tree_oid TEXT NOT NULL DEFAULT '',
            conflicting_files_json TEXT NOT NULL DEFAULT '[]',
            evaluated_at_utc TEXT NOT NULL DEFAULT '',
            integrated_at_utc TEXT NOT NULL DEFAULT '',
            integrated_commit TEXT NOT NULL DEFAULT '',
            status TEXT NOT NULL DEFAULT 'QUEUED'
        )
    )SQL");

    db.exec(R"SQL(
        CREATE TABLE IF NOT EXISTS build_manifests (
            build_id TEXT PRIMARY KEY,
            project_name TEXT NOT NULL DEFAULT '',
            worktree_path TEXT NOT NULL DEFAULT '',
            branch TEXT NOT NULL DEFAULT '',
            head_commit TEXT NOT NULL DEFAULT '',
            tree_clean INTEGER NOT NULL DEFAULT 1,
            snapshot_id TEXT NOT NULL DEFAULT '',
            requesting_agent TEXT NOT NULL DEFAULT 'agent',
            build_type TEXT NOT NULL DEFAULT 'Release',
            compiler_id TEXT NOT NULL DEFAULT 'MSVC',
            lifecycle_state TEXT NOT NULL DEFAULT 'CONFIRMED',
            integration_completeness TEXT NOT NULL DEFAULT 'COMPLETE',
            unintegrated_worktrees_json TEXT NOT NULL DEFAULT '[]',
            excluded_worktrees_json TEXT NOT NULL DEFAULT '[]',
            conservation_tier TEXT NOT NULL DEFAULT 'PROTECTED',
            total_artifact_bytes INTEGER NOT NULL DEFAULT 0,
            build_dir TEXT NOT NULL DEFAULT '',
            exit_code INTEGER NOT NULL DEFAULT 0,
            test_summary_json TEXT NOT NULL DEFAULT '{}',
            manifest_path TEXT NOT NULL DEFAULT '',
            created_at_utc TEXT NOT NULL DEFAULT ''
        )
    )SQL");

    db.exec(R"SQL(
        CREATE TABLE IF NOT EXISTS build_artifacts (
            artifact_id INTEGER PRIMARY KEY AUTOINCREMENT,
            build_id TEXT NOT NULL,
            artifact_name TEXT NOT NULL DEFAULT '',
            artifact_path TEXT NOT NULL DEFAULT '',
            sha256 TEXT NOT NULL DEFAULT '',
            size_bytes INTEGER NOT NULL DEFAULT 0,
            mtime_utc TEXT NOT NULL DEFAULT '',
            kind TEXT NOT NULL DEFAULT 'other'
        )
    )SQL");
    db.exec("CREATE INDEX IF NOT EXISTS idx_build_artifacts_sha256 ON build_artifacts(sha256)");
    db.exec("CREATE INDEX IF NOT EXISTS idx_build_artifacts_path ON build_artifacts(artifact_path)");
    db.exec("CREATE INDEX IF NOT EXISTS idx_build_artifacts_build_id ON build_artifacts(build_id)");

    db.exec(R"SQL(
        CREATE TABLE IF NOT EXISTS directory_classification_policies (
            policy_id INTEGER PRIMARY KEY AUTOINCREMENT,
            pattern TEXT NOT NULL UNIQUE,
            match_kind TEXT NOT NULL DEFAULT 'prefix',
            category TEXT NOT NULL DEFAULT 'UNKNOWN',
            default_tier TEXT NOT NULL DEFAULT 'NEEDS_REVIEW',
            is_builtin INTEGER NOT NULL DEFAULT 0,
            rationale TEXT NOT NULL DEFAULT '',
            updated_at_utc TEXT NOT NULL DEFAULT ''
        )
    )SQL");

    db.exec(R"SQL(
        CREATE TABLE IF NOT EXISTS storage_quota_policies (
            policy_key TEXT PRIMARY KEY,
            max_build_storage_bytes INTEGER NOT NULL DEFAULT 21474836480,
            keep_last_n_builds INTEGER NOT NULL DEFAULT 3,
            max_temp_age_days INTEGER NOT NULL DEFAULT 14,
            updated_at_utc TEXT NOT NULL DEFAULT ''
        )
    )SQL");
    if (db.integer("SELECT COUNT(*) FROM storage_quota_policies") == 0) {
        db.exec(
            "INSERT OR IGNORE INTO storage_quota_policies "
            "(policy_key, max_build_storage_bytes, keep_last_n_builds, max_temp_age_days, updated_at_utc) "
            "VALUES ('default', 21474836480, 3, 14, ?)",
            {now()});
    }

    db.exec(R"SQL(
        CREATE TABLE IF NOT EXISTS discovery_search_roots (
            root_path TEXT PRIMARY KEY,
            source_kind TEXT NOT NULL DEFAULT 'user',
            last_online INTEGER NOT NULL DEFAULT 1,
            last_scanned_utc TEXT NOT NULL DEFAULT ''
        )
    )SQL");

    db.exec(R"SQL(
        CREATE TABLE IF NOT EXISTS session_pendings (
            pending_id TEXT PRIMARY KEY,
            session_name TEXT NOT NULL DEFAULT '',
            owner_agent TEXT NOT NULL DEFAULT '',
            title TEXT NOT NULL DEFAULT '',
            details TEXT NOT NULL DEFAULT '',
            linked_req_id TEXT NOT NULL DEFAULT '',
            status TEXT NOT NULL DEFAULT 'OPEN',
            created_at_utc TEXT NOT NULL DEFAULT '',
            closed_at_utc TEXT NOT NULL DEFAULT ''
        )
    )SQL");

    db.exec(R"SQL(
        CREATE TABLE IF NOT EXISTS discovered_project_replicas (
            replica_id TEXT PRIMARY KEY,
            candidate_path TEXT NOT NULL UNIQUE,
            classification TEXT NOT NULL DEFAULT 'UNRELATED',
            git_root_commit TEXT NOT NULL DEFAULT '',
            git_head_commit TEXT NOT NULL DEFAULT '',
            git_branch TEXT NOT NULL DEFAULT '',
            git_remote_url TEXT NOT NULL DEFAULT '',
            shared_file_hashes_count INTEGER NOT NULL DEFAULT 0,
            unique_or_modified_files_count INTEGER NOT NULL DEFAULT 0,
            unique_files_sample_json TEXT NOT NULL DEFAULT '[]',
            disk_bytes INTEGER NOT NULL DEFAULT 0,
            discovered_at_utc TEXT NOT NULL DEFAULT '',
            merge_base_commit TEXT NOT NULL DEFAULT '',
            is_ancestor_of_main INTEGER NOT NULL DEFAULT 0
        )
    )SQL");
    for (const char* rep_col : {
        "merge_base_commit TEXT NOT NULL DEFAULT ''",
        "is_ancestor_of_main INTEGER NOT NULL DEFAULT 0"
    }) {
        try {
            db.exec(std::string("ALTER TABLE discovered_project_replicas ADD COLUMN ") + rep_col);
        } catch (...) {}
    }

    db.exec(R"SQL(
        CREATE TABLE IF NOT EXISTS product_requirements (
            req_id TEXT PRIMARY KEY,
            title TEXT NOT NULL DEFAULT '',
            category TEXT NOT NULL DEFAULT '',
            priority TEXT NOT NULL DEFAULT 'P0',
            status TEXT NOT NULL DEFAULT 'CONFIRMED',
            summary TEXT NOT NULL DEFAULT '',
            invariants_json TEXT NOT NULL DEFAULT '[]',
            implementation_files_json TEXT NOT NULL DEFAULT '[]',
            test_suites_json TEXT NOT NULL DEFAULT '[]',
            acceptance_criteria_json TEXT NOT NULL DEFAULT '[]',
            updated_at_utc TEXT NOT NULL DEFAULT ''
        )
    )SQL");

    db.exec(R"SQL(
        CREATE TABLE IF NOT EXISTS requirement_trace_links (
            req_id TEXT NOT NULL,
            file_path TEXT NOT NULL,
            file_exists INTEGER NOT NULL DEFAULT 0,
            file_sha256 TEXT NOT NULL DEFAULT '',
            test_suite TEXT NOT NULL DEFAULT '',
            git_commit TEXT NOT NULL DEFAULT '',
            build_id TEXT NOT NULL DEFAULT '',
            verified_at_utc TEXT NOT NULL DEFAULT '',
            last_test_status TEXT NOT NULL DEFAULT 'NO_TEST_EVIDENCE',
            last_test_run_id TEXT NOT NULL DEFAULT '',
            included_builds_json TEXT NOT NULL DEFAULT '[]',
            post_change_status TEXT NOT NULL DEFAULT 'NO_TEST_EVIDENCE',
            PRIMARY KEY(req_id, file_path)
        )
    )SQL");
    db.exec("CREATE INDEX IF NOT EXISTS idx_req_trace_file ON requirement_trace_links(file_path)");
    for (const char* col_def : {
        "last_test_status TEXT NOT NULL DEFAULT 'NO_TEST_EVIDENCE'",
        "last_test_run_id TEXT NOT NULL DEFAULT ''",
        "included_builds_json TEXT NOT NULL DEFAULT '[]'",
        "post_change_status TEXT NOT NULL DEFAULT 'NO_TEST_EVIDENCE'"
    }) {
        try {
            db.exec(std::string("ALTER TABLE requirement_trace_links ADD COLUMN ") + col_def);
        } catch (...) {}
    }

    if (db.integer("SELECT COUNT(*) FROM directory_classification_policies") == 0) {
        struct SeedPolicy {
            const char* pattern;
            const char* match_kind;
            const char* category;
            const char* tier;
            const char* rationale;
        };
        static const std::array<SeedPolicy, 18> kSeeds = {{
            {"src", "prefix", "SOURCE", "PROTECTED", "Código fuente principal de aplicación"},
            {"include", "prefix", "SOURCE", "PROTECTED", "Encabezados públicos e internos C/C++"},
            {"adapters", "prefix", "SOURCE", "PROTECTED", "Adaptadores semánticos y puentes de lenguaje"},
            {"resources", "prefix", "SOURCE", "PROTECTED", "Manifiestos Win32, iconos y recursos compilables"},
            {"examples", "prefix", "SOURCE", "PROTECTED", "Ejemplos ejecutables y plantillas del proyecto"},
            {"tools", "prefix", "SOURCE", "PROTECTED", "Herramientas auxiliares del proyecto fuera de src"},
            {"scripts", "prefix", "SOURCE", "PROTECTED", "Scripts de automatización y construcción"},
            {"tests", "prefix", "TEST_SOURCE", "PROTECTED", "Código fuente de pruebas unitarias, integración y regresión"},
            {"docs", "prefix", "DOCUMENTATION", "PROTECTED", "Especificaciones maestras, arquitectura y documentación"},
            {"third_party", "prefix", "DEPENDENCY", "PROTECTED", "Dependencias vendored bajo control de versiones"},
            {"vcpkg_installed", "prefix", "DEPENDENCY", "PROTECTED", "Dependencias instaladas del proyecto"},
            {"node_modules", "prefix", "DEPENDENCY", "PROTECTED", "Dependencias Node.js instaladas"},
            {".migratool", "prefix", "USER_DATA", "PROTECTED", "Catálogo SQLite, snapshots, manifiestos de build y estado persistente"},
            {"build", "prefix", "BUILD_OUTPUT", "TEMP_RETAINED", "Directorio de salida de compilación sujeto a auditoría"},
            {"out", "prefix", "BUILD_OUTPUT", "TEMP_RETAINED", "Directorio de salida de compilación sujeto a auditoría"},
            {"release", "prefix", "BUILD_OUTPUT", "TEMP_RETAINED", "Artefactos de salida de compilación sujetos a auditoría"},
            {"artifacts", "prefix", "BUILD_OUTPUT", "TEMP_RETAINED", "Artefactos generados sujetos a auditoría"},
            {"cache", "prefix", "CACHE", "VERIFIED_SAFE_CLEAN", "Caché local reproducible"},
        }};
        const std::string ts = now();
        Statement ins(db, R"SQL(
            INSERT OR IGNORE INTO directory_classification_policies
                (pattern, match_kind, category, default_tier, is_builtin, rationale, updated_at_utc)
            VALUES (?, ?, ?, ?, 1, ?, ?)
        )SQL");
        for (const auto& s : kSeeds) {
            ins.exec({s.pattern, s.match_kind, s.category, s.tier, s.rationale, ts});
        }
        ins.exec({".cache", "prefix", "CACHE", "VERIFIED_SAFE_CLEAN", "Caché oculta local reproducible", ts});
    }
}

Json tool_worktree_inventory(const Project& p, Database& db, const Json& args) {
    ensure_multiagent_coordinator_schema(db);
    const std::string action = lower_ascii(args.value("action", "scan"));
    const std::string proj_name = p.config.value("name", path_text(p.directory().filename()));
    const std::string ts = now();

    if (args.contains("lifecycle_state") && args["lifecycle_state"].is_string()) {
        validate_lifecycle_state_or_throw(upper_ascii(args["lifecycle_state"].get<std::string>()));
    }

    if (action == "register" || action == "update") {
        const std::string raw_wt_path = args.value("worktree_path", path_text(p.directory()));
        fs::path wt_fs = fs::path(wide(raw_wt_path));
        if (wt_fs.is_relative()) wt_fs = p.directory() / wt_fs;
        const std::string wt_path = canonical_path_str(wt_fs);

        Json existing = Json::object();
        const std::string req_id = args.value("worktree_id", std::string{});
        if (!req_id.empty()) {
            const auto rows = db.query("SELECT * FROM agent_worktrees_catalog WHERE worktree_id=?", {req_id});
            if (!rows.empty()) existing = rows[0];
        }
        if (existing.empty()) {
            const auto rows = db.query("SELECT * FROM agent_worktrees_catalog WHERE lower(worktree_path)=lower(?)", {wt_path});
            if (!rows.empty()) existing = rows[0];
        }

        const std::string wt_id = !req_id.empty()
            ? req_id
            : (!existing.empty() ? existing.value("worktree_id", default_worktree_id(wt_path)) : default_worktree_id(wt_path));

        const bool exclude_flag = args.value("exclude_from_canonical", false);
        const bool has_excl_args = exclude_flag
            || (args.contains("excluded_by") && !args["excluded_by"].is_null())
            || (args.contains("exclusion_reason") && !args["exclusion_reason"].is_null());

        std::string excluded_by = existing.value("excluded_by", std::string{});
        std::string exclusion_reason = existing.value("exclusion_reason", std::string{});
        std::string excluded_at_utc = existing.value("excluded_at_utc", std::string{});

        if (args.contains("exclude_from_canonical") && args["exclude_from_canonical"].is_boolean() && !exclude_flag
            && !args.contains("excluded_by") && !args.contains("exclusion_reason")) {
            excluded_by.clear();
            exclusion_reason.clear();
            excluded_at_utc.clear();
        } else if (has_excl_args) {
            excluded_by = trim_copy(args.value("excluded_by", std::string{}));
            exclusion_reason = trim_copy(args.value("exclusion_reason", std::string{}));
            if (excluded_by.empty() || exclusion_reason.empty()) {
                throw std::runtime_error(
                    "EXCLUSION_REQUIRES_ACTOR_AND_REASON: excluding a worktree requires both non-empty excluded_by and exclusion_reason");
            }
            excluded_at_utc = ts;
        }

        const std::string target_branch = detect_main_branch(p.directory(), args.value("target_branch", std::string{}));
        const LiveWorktreeMetrics live = inspect_live_worktree(wt_fs, target_branch);

        std::string lifecycle_state = existing.value("lifecycle_state", std::string{"IMPLEMENTED"});
        if (args.contains("lifecycle_state") && args["lifecycle_state"].is_string()) {
            lifecycle_state = upper_ascii(args["lifecycle_state"].get<std::string>());
            validate_lifecycle_state_or_throw(lifecycle_state);
        }

        const std::string branch = args.value("branch", !live.branch.empty() ? live.branch : existing.value("branch", std::string{"main"}));
        const std::string head_commit = args.value("head_commit", !live.head_commit.empty() ? live.head_commit : existing.value("head_commit", std::string{}));
        const std::string base_commit = args.value("base_commit", existing.value("base_commit", head_commit));
        const std::string owner_agent = args.value("owner_agent", existing.value("owner_agent", std::string{"agent"}));
        const std::string workstream_id = args.value("workstream_id", existing.value("workstream_id", std::string{}));
        const std::string task_id = args.value("task_id", existing.value("task_id", std::string{}));
        const std::string goal = args.value("goal", existing.value("goal", std::string{}));
        const int64_t is_mandatory = args.contains("is_mandatory")
            ? (args["is_mandatory"].is_boolean() ? (args["is_mandatory"].get<bool>() ? 1 : 0) : args["is_mandatory"].get<int64_t>())
            : existing.value("is_mandatory", int64_t{1});
        const int64_t uncommitted_count = args.contains("uncommitted_count")
            ? args["uncommitted_count"].get<int64_t>()
            : (live.is_git_worktree ? live.uncommitted_count : existing.value("uncommitted_count", int64_t{0}));
        const int64_t untracked_count = args.contains("untracked_count")
            ? args["untracked_count"].get<int64_t>()
            : (live.is_git_worktree ? live.untracked_count : existing.value("untracked_count", int64_t{0}));
        const int64_t ahead_count = args.contains("ahead_count")
            ? args["ahead_count"].get<int64_t>()
            : (live.is_git_worktree ? live.ahead_count : existing.value("ahead_count", int64_t{0}));
        const int64_t behind_count = args.contains("behind_count")
            ? args["behind_count"].get<int64_t>()
            : (live.is_git_worktree ? live.behind_count : existing.value("behind_count", int64_t{0}));
        const int64_t active_build_pid = args.value("active_build_pid", existing.value("active_build_pid", int64_t{0}));
        const int64_t disk_bytes = args.value("disk_bytes", live.disk_bytes > 0 ? live.disk_bytes : existing.value("disk_bytes", int64_t{0}));
        const std::string pinned_snapshot_id = args.value("pinned_snapshot_id", existing.value("pinned_snapshot_id", std::string{}));
        const std::string notes = args.value("notes", existing.value("notes", std::string{}));

        db.exec(R"SQL(
            INSERT INTO agent_worktrees_catalog (
                worktree_id, project_name, worktree_path, branch, head_commit, base_commit,
                owner_agent, workstream_id, task_id, goal, lifecycle_state, is_mandatory,
                excluded_by, exclusion_reason, excluded_at_utc, uncommitted_count, untracked_count,
                ahead_count, behind_count, active_build_pid, disk_bytes, pinned_snapshot_id,
                notes, last_modified_utc, updated_at_utc
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            ON CONFLICT(worktree_id) DO UPDATE SET
                project_name=excluded.project_name,
                worktree_path=excluded.worktree_path,
                branch=excluded.branch,
                head_commit=excluded.head_commit,
                base_commit=excluded.base_commit,
                owner_agent=excluded.owner_agent,
                workstream_id=excluded.workstream_id,
                task_id=excluded.task_id,
                goal=excluded.goal,
                lifecycle_state=excluded.lifecycle_state,
                is_mandatory=excluded.is_mandatory,
                excluded_by=excluded.excluded_by,
                exclusion_reason=excluded.exclusion_reason,
                excluded_at_utc=excluded.excluded_at_utc,
                uncommitted_count=excluded.uncommitted_count,
                untracked_count=excluded.untracked_count,
                ahead_count=excluded.ahead_count,
                behind_count=excluded.behind_count,
                active_build_pid=excluded.active_build_pid,
                disk_bytes=excluded.disk_bytes,
                pinned_snapshot_id=excluded.pinned_snapshot_id,
                notes=excluded.notes,
                last_modified_utc=excluded.last_modified_utc,
                updated_at_utc=excluded.updated_at_utc
        )SQL", {
            wt_id, proj_name, wt_path, branch, head_commit, base_commit,
            owner_agent, workstream_id, task_id, goal, lifecycle_state, is_mandatory,
            excluded_by, exclusion_reason, excluded_at_utc, uncommitted_count, untracked_count,
            ahead_count, behind_count, active_build_pid, disk_bytes, pinned_snapshot_id,
            notes, ts, ts
        });

        const auto updated = db.query("SELECT * FROM agent_worktrees_catalog WHERE worktree_id=?", {wt_id});
        const Json row = updated.empty() ? Json::object() : format_worktree_row(updated[0]);
        return {
            {"schema", "migratool.worktree_inventory/1"},
            {"ok", true},
            {"action", action},
            {"worktree_id", wt_id},
            {"worktree", row}
        };
    }

    if (action == "prune_stale_admin") {
        const auto res = run_git_isolated(p.directory(), {"worktree", "prune", "-v"}, 8000);
        return {
            {"schema", "migratool.worktree_inventory/1"},
            {"action", "prune_stale_admin"},
            {"ok", true},
            {"note", "git worktree prune limpia solo metadatos administrativos obsoletos de Git; no elimina compilaciones ni carpetas de proyecto"},
            {"git_output", res.output}
        };
    }

    if (action == "remove_worktree") {
        const std::string req_id = trim_copy(args.value("worktree_id", std::string{}));
        std::string raw_wt_path = trim_copy(args.value("worktree_path", std::string{}));
        if (req_id.empty() && raw_wt_path.empty()) {
            throw std::runtime_error("WORKTREE_REMOVE_MISSING_TARGET: provide worktree_path or worktree_id");
        }

        std::string wt_path;
        if (!raw_wt_path.empty()) {
            fs::path wt_fs(wide(raw_wt_path));
            if (wt_fs.is_relative()) wt_fs = p.directory() / wt_fs;
            wt_path = canonical_path_str(wt_fs);
        }

        Json existing = Json::object();
        if (!req_id.empty()) {
            const auto rows = db.query("SELECT * FROM agent_worktrees_catalog WHERE worktree_id=?", {req_id});
            if (!rows.empty()) existing = rows[0];
        }
        if (existing.empty() && !wt_path.empty()) {
            const auto rows = db.query("SELECT * FROM agent_worktrees_catalog WHERE lower(worktree_path)=lower(?)", {wt_path});
            if (!rows.empty()) existing = rows[0];
        }
        if (wt_path.empty() && !existing.empty()) {
            wt_path = existing.value("worktree_path", std::string{});
        }
        if (wt_path.empty()) {
            throw std::runtime_error("WORKTREE_NOT_FOUND: worktree not found in agent_worktrees_catalog");
        }

        const std::string main_key = normalize_path_key(p.directory());
        const std::string wt_key = normalize_path_key_str(wt_path);
        if (wt_key == main_key) {
            throw std::runtime_error("WORKTREE_REMOVE_REFUSED_MAIN: cannot remove the main project integration worktree");
        }

        const fs::path wt_fs(wide(wt_path));
        const std::string target_branch = detect_main_branch(p.directory(), args.value("target_branch", std::string{}));
        const LiveWorktreeMetrics live = inspect_live_worktree(wt_fs, target_branch);

        const int64_t uncommitted = live.is_git_worktree ? live.uncommitted_count : existing.value("uncommitted_count", int64_t{0});
        const int64_t untracked = live.is_git_worktree ? live.untracked_count : existing.value("untracked_count", int64_t{0});
        if (uncommitted > 0 || untracked > 0) {
            throw std::runtime_error(
                "WORKTREE_REMOVE_REFUSED_DIRTY: worktree has uncommitted (" + std::to_string(uncommitted) +
                ") or untracked (" + std::to_string(untracked) + ") files");
        }

        const int64_t active_build_pid = existing.value("active_build_pid", int64_t{0});
        if (pid_alive(active_build_pid)) {
            throw std::runtime_error(
                "WORKTREE_REMOVE_REFUSED_ACTIVE_BUILD: active build process PID " + std::to_string(active_build_pid) + " is running");
        }

        const bool is_mandatory = existing.empty() ? true : json_bool_or_int(existing, "is_mandatory", true);
        const std::string excluded_by = existing.value("excluded_by", std::string{});
        const std::string lifecycle_state = existing.value("lifecycle_state", std::string{"IMPLEMENTED"});
        if (is_mandatory && excluded_by.empty() && (lifecycle_state == "IMPLEMENTED" || lifecycle_state == "CONFIRMED")) {
            throw std::runtime_error(
                "WORKTREE_REMOVE_REFUSED_UNINTEGRATED: mandatory worktree in state '" + lifecycle_state +
                "' cannot be removed before integration or explicit exclusion");
        }

        const std::string wt_id = !existing.empty()
            ? existing.value("worktree_id", default_worktree_id(wt_path))
            : (!req_id.empty() ? req_id : default_worktree_id(wt_path));
        const bool execute = args.value("execute", false);
        if (execute) {
            run_git_isolated(p.directory(), {"worktree", "remove", wt_path}, 10000);
            std::error_code ec;
            if (fs::exists(native_path(wt_fs), ec) && !is_path_within_dir(wt_fs, p.directory(), true, false)) {
                if (!send_file_to_recycle_bin(wt_fs)) {
                    fs::remove_all(native_path(wt_fs), ec);
                }
                run_git_isolated(p.directory(), {"worktree", "prune"}, 5000);
            }
            db.exec("DELETE FROM agent_worktrees_catalog WHERE worktree_id=? OR lower(worktree_path)=lower(?)", {wt_id, wt_path});
            db.exec("DELETE FROM integration_queue WHERE worktree_id=? OR lower(worktree_path)=lower(?)", {wt_id, wt_path});
        }

        return {
            {"schema", "migratool.worktree_inventory/1"},
            {"action", "remove_worktree"},
            {"ok", true},
            {"executed", execute},
            {"worktree_id", wt_id},
            {"worktree_path", wt_path}
        };
    }

    if (action == "resume" || action == "handoff") {
        const Json scan_res = tool_worktree_inventory(p, db, {{"action", "scan"}});
        const Json active_worktrees = scan_res.value("worktrees", Json::array());
        Json unintegrated_mandatory = Json::array();
        for (const auto& wt : active_worktrees) {
            if (is_worktree_unintegrated_mandatory(wt)) {
                unintegrated_mandatory.push_back(wt);
            }
        }

        Json open_workstreams = Json::array();
        if (table_exists(db, "agent_workstreams")) {
            open_workstreams = db.query(
                "SELECT id AS workstream_id, owner_actor AS agent_id, title, goal, status, "
                "current_step AS stage, base_snapshot_id AS pinned_snapshot_id, updated_at AS updated_at_utc "
                "FROM agent_workstreams WHERE status NOT IN ('completed', 'abandoned', 'rolled_back', 'COMPLETED', 'ARCHIVED') "
                "ORDER BY updated_at DESC LIMIT 25");
        }

        Json latest_canonical_build = nullptr;
        const auto b_rows = db.query(
            "SELECT * FROM build_manifests WHERE lifecycle_state IN ('INTEGRATED', 'DELIVERED') "
            "ORDER BY created_at_utc DESC LIMIT 1");
        if (!b_rows.empty()) {
            latest_canonical_build = b_rows[0];
        }

        const std::string q = args.value("task_query", args.value("query", std::string{}));
        const Json req_res = tool_project_health(p, db, {
            {"action", "query_requirements"},
            {"query", q},
            {"limit", args.value("limit", 10)}
        });

        Json next_actions = Json::array();
        if (!unintegrated_mandatory.empty()) {
            next_actions.push_back("Integrar o verificar con integrate_ready los worktrees obligatorios en estado IMPLEMENTED o CONFIRMED antes de generar el build canónico.");
        }
        if (!open_workstreams.empty()) {
            next_actions.push_back("Reanudar el workstream activo desde su último checkpoint o snapshot P4 verificado.");
        }
        if (latest_canonical_build.is_null()) {
            next_actions.push_back("Registrar una compilación integrada con build_canonical(action='record', lifecycle_state='INTEGRATED').");
        }

        return {
            {"schema", "migratool.project_resume/1"},
            {"ok", true},
            {"action", action},
            {"handover_ready", true},
            {"project_name", proj_name},
            {"project_root", canonical_path_str(p.directory())},
            {"total_worktrees", active_worktrees.size()},
            {"unintegrated_mandatory_count", unintegrated_mandatory.size()},
            {"active_worktrees", active_worktrees},
            {"unintegrated_mandatory_worktrees", unintegrated_mandatory},
            {"open_workstreams", open_workstreams},
            {"latest_canonical_build", latest_canonical_build},
            {"relevant_requirements", req_res.value("requirements", Json::array())},
            {"recommended_next_actions", next_actions}
        };
    }

    // Default: "scan" | "list" | "inventory"
    const std::string target_branch = detect_main_branch(p.directory(), args.value("target_branch", std::string{}));
    const std::string main_dir_str = canonical_path_str(p.directory());
    const std::string main_dir_key = normalize_path_key(p.directory());

    struct DiscoveredWt {
        std::string path;
        std::string head;
        std::string branch;
    };
    std::vector<DiscoveredWt> discovered;
    std::unordered_set<std::string> seen_keys;

    const auto wt_out = run_git_isolated(p.directory(), {"worktree", "list", "--porcelain"}, 8000);
    if (wt_out.exit_code == 0 && !wt_out.output.empty()) {
        std::istringstream iss(wt_out.output);
        std::string line;
        DiscoveredWt cur;
        auto flush_cur = [&]() {
            if (!cur.path.empty()) {
                const fs::path fp(wide(cur.path));
                cur.path = canonical_path_str(fp);
                const std::string k = lower_ascii(cur.path);
                if (!k.empty() && seen_keys.insert(k).second) {
                    discovered.push_back(cur);
                }
            }
            cur = DiscoveredWt{};
        };
        while (std::getline(iss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) {
                flush_cur();
            } else if (line.starts_with("worktree ")) {
                if (!cur.path.empty()) flush_cur();
                cur.path = trim_copy(line.substr(9));
            } else if (line.starts_with("HEAD ")) {
                cur.head = trim_copy(line.substr(5));
            } else if (line.starts_with("branch ")) {
                std::string b = trim_copy(line.substr(7));
                if (b.starts_with("refs/heads/")) b = b.substr(11);
                cur.branch = b;
            }
        }
        flush_cur();
    }

    if (seen_keys.insert(main_dir_key).second) {
        discovered.push_back(DiscoveredWt{main_dir_str, "", ""});
    }

    // Also refresh any previously registered worktrees in SQLite
    const auto existing_rows = db.query("SELECT * FROM agent_worktrees_catalog ORDER BY updated_at_utc DESC");
    std::unordered_map<std::string, Json> existing_by_key;
    for (const auto& r : existing_rows) {
        const std::string p_str = r.value("worktree_path", std::string{});
        const std::string k = normalize_path_key_str(p_str);
        if (!k.empty()) {
            existing_by_key[k] = r;
            if (seen_keys.insert(k).second) {
                discovered.push_back(DiscoveredWt{p_str, r.value("head_commit", std::string{}), r.value("branch", std::string{})});
            }
        }
    }

    for (const auto& d : discovered) {
        const std::string k = normalize_path_key_str(d.path);
        const auto it = existing_by_key.find(k);
        const bool has_existing = (it != existing_by_key.end());
        const Json ex = has_existing ? it->second : Json::object();

        const fs::path wt_fs(wide(d.path));
        const LiveWorktreeMetrics live = inspect_live_worktree(wt_fs, target_branch, d.branch, d.head, p.directory());

        const std::string wt_id = has_existing ? ex.value("worktree_id", default_worktree_id(d.path)) : default_worktree_id(d.path);
        const std::string branch = !live.branch.empty() ? live.branch : (!d.branch.empty() ? d.branch : ex.value("branch", target_branch));
        const std::string head_commit = !live.head_commit.empty() ? live.head_commit : (!d.head.empty() ? d.head : ex.value("head_commit", std::string{}));
        const std::string base_commit = ex.value("base_commit", head_commit);
        const std::string owner_agent = ex.value("owner_agent", std::string{});
        const std::string workstream_id = ex.value("workstream_id", std::string{});
        const std::string task_id = ex.value("task_id", std::string{});
        const std::string goal = ex.value("goal", std::string{});

        const int64_t uncommitted_count = live.is_git_worktree ? live.uncommitted_count : ex.value("uncommitted_count", int64_t{0});
        const int64_t untracked_count = live.is_git_worktree ? live.untracked_count : ex.value("untracked_count", int64_t{0});
        const int64_t ahead_count = live.is_git_worktree ? live.ahead_count : ex.value("ahead_count", int64_t{0});
        const int64_t behind_count = live.is_git_worktree ? live.behind_count : ex.value("behind_count", int64_t{0});
        const int64_t disk_bytes = live.disk_bytes > 0 ? live.disk_bytes : ex.value("disk_bytes", int64_t{0});

        std::string lifecycle_state = has_existing
            ? ex.value("lifecycle_state", std::string{"IMPLEMENTED"})
            : ((k == main_dir_key && uncommitted_count == 0 && ahead_count == 0) ? "INTEGRATED" : "IMPLEMENTED");

        // If an auto-discovered main worktree becomes dirty with uncommitted changes (and was not explicitly mark_integrated), demote to IMPLEMENTED
        if (has_existing && k == main_dir_key && lifecycle_state == "INTEGRATED" && uncommitted_count > 0 && owner_agent.empty() && task_id.empty()) {
            const int64_t explicitly_integrated = db.integer(
                "SELECT COUNT(*) FROM integration_queue WHERE worktree_id=? AND integrated_at_utc != ''", {wt_id});
            if (explicitly_integrated == 0) {
                lifecycle_state = "IMPLEMENTED";
            }
        }

        const int64_t is_mandatory = ex.value("is_mandatory", int64_t{1});
        const std::string excluded_by = ex.value("excluded_by", std::string{});
        const std::string exclusion_reason = ex.value("exclusion_reason", std::string{});
        const std::string excluded_at_utc = ex.value("excluded_at_utc", std::string{});
        const int64_t active_build_pid = ex.value("active_build_pid", int64_t{0});
        const std::string pinned_snapshot_id = ex.value("pinned_snapshot_id", std::string{});
        const std::string notes = ex.value("notes", std::string{});

        db.exec(R"SQL(
            INSERT INTO agent_worktrees_catalog (
                worktree_id, project_name, worktree_path, branch, head_commit, base_commit,
                owner_agent, workstream_id, task_id, goal, lifecycle_state, is_mandatory,
                excluded_by, exclusion_reason, excluded_at_utc, uncommitted_count, untracked_count,
                ahead_count, behind_count, active_build_pid, disk_bytes, pinned_snapshot_id,
                notes, last_modified_utc, updated_at_utc
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            ON CONFLICT(worktree_id) DO UPDATE SET
                worktree_path=excluded.worktree_path,
                branch=excluded.branch,
                head_commit=excluded.head_commit,
                uncommitted_count=excluded.uncommitted_count,
                untracked_count=excluded.untracked_count,
                ahead_count=excluded.ahead_count,
                behind_count=excluded.behind_count,
                disk_bytes=excluded.disk_bytes,
                lifecycle_state=excluded.lifecycle_state,
                updated_at_utc=excluded.updated_at_utc
        )SQL", {
            wt_id, proj_name, d.path, branch, head_commit, base_commit,
            owner_agent, workstream_id, task_id, goal, lifecycle_state, is_mandatory,
            excluded_by, exclusion_reason, excluded_at_utc, uncommitted_count, untracked_count,
            ahead_count, behind_count, active_build_pid, disk_bytes, pinned_snapshot_id,
            notes, ts, ts
        });
    }

    const auto final_rows = db.query("SELECT * FROM agent_worktrees_catalog ORDER BY worktree_path ASC");
    Json items = Json::array();
    size_t unintegrated_mandatory_count = 0;
    size_t dirty_worktrees_count = 0;
    for (const auto& r : final_rows) {
        Json item = format_worktree_row(r);
        if (is_worktree_unintegrated_mandatory(item)) ++unintegrated_mandatory_count;
        if (item.value("uncommitted_count", int64_t{0}) > 0 || item.value("untracked_count", int64_t{0}) > 0) {
            ++dirty_worktrees_count;
        }
        items.push_back(std::move(item));
    }

    return {
        {"schema", "migratool.worktree_inventory/1"},
        {"ok", true},
        {"action", action},
        {"target_branch", target_branch},
        {"total_worktrees", items.size()},
        {"unintegrated_mandatory_count", unintegrated_mandatory_count},
        {"dirty_worktrees_count", dirty_worktrees_count},
        {"worktrees", items}
    };
}

Json tool_project_resume(const Project& p, Database& db, const Json& args) {
    Json resume_args = args.is_object() ? args : Json::object();
    resume_args["action"] = "resume";
    return tool_worktree_inventory(p, db, resume_args);
}

Json tool_integrate_ready(const Project& p, Database& db, const Json& args) {
    ensure_multiagent_coordinator_schema(db);
    const std::string action = lower_ascii(args.value("action", "evaluate"));
    const std::string ts = now();
    const std::string target_branch = detect_main_branch(p.directory(), args.value("target_branch", std::string{}));

    if (action == "merge_execute") {
        const std::string req_id = trim_copy(args.value("worktree_id", std::string{}));
        std::string req_path = trim_copy(args.value("worktree_path", std::string{}));
        const std::string req_branch = trim_copy(args.value("branch", std::string{}));
        if (!req_path.empty()) {
            fs::path fp(wide(req_path));
            if (fp.is_relative()) fp = p.directory() / fp;
            req_path = canonical_path_str(fp);
        }
        if (req_id.empty() && req_path.empty() && req_branch.empty()) {
            throw std::runtime_error("MERGE_EXECUTE_MISSING_TARGET: provide worktree_path, worktree_id, or branch");
        }

        auto find_wt_rows = [&]() -> Json {
            if (!req_id.empty()) {
                const auto r = db.query("SELECT * FROM agent_worktrees_catalog WHERE worktree_id=?", {req_id});
                if (!r.empty()) return r;
            }
            if (!req_path.empty()) {
                const auto r = db.query("SELECT * FROM agent_worktrees_catalog WHERE lower(worktree_path)=lower(?)", {req_path});
                if (!r.empty()) return r;
            }
            if (!req_branch.empty()) {
                const auto r = db.query("SELECT * FROM agent_worktrees_catalog WHERE branch=?", {req_branch});
                if (!r.empty()) return r;
            }
            return Json::array();
        };

        Json rows = find_wt_rows();
        if (rows.empty()) {
            tool_worktree_inventory(p, db, {{"action", "scan"}, {"target_branch", target_branch}});
            rows = find_wt_rows();
        }
        if (rows.empty()) {
            throw std::runtime_error("WORKTREE_NOT_FOUND: worktree not found in agent_worktrees_catalog");
        }

        const Json wt_row = rows[0];
        const std::string wt_id = wt_row.value("worktree_id", std::string{});
        const std::string wt_path = wt_row.value("worktree_path", std::string{});
        std::string branch = !req_branch.empty() ? req_branch : wt_row.value("branch", std::string{});
        std::string head_commit = wt_row.value("head_commit", std::string{});
        const std::string state = wt_row.value("lifecycle_state", std::string{"IMPLEMENTED"});
        const std::string owner_agent = wt_row.value("owner_agent", std::string{});

        if (!wt_path.empty()) {
            const LiveWorktreeMetrics wt_live = inspect_live_worktree(fs::path(wide(wt_path)), target_branch);
            if (!wt_live.head_commit.empty()) head_commit = wt_live.head_commit;
            if (branch.empty() && !wt_live.branch.empty()) branch = wt_live.branch;
        }

        const bool allow_implemented = args.value("allow_implemented", false);
        if (state != "CONFIRMED" && !(allow_implemented && state == "IMPLEMENTED")) {
            throw std::runtime_error(
                "MERGE_BLOCKED_UNCONFIRMED_WORKTREE: worktree '" + wt_id + "' is in state '" + state +
                "' (must be CONFIRMED before merge_execute)");
        }

        const std::string candidate_ref = (!branch.empty() && git_commit_exists(p.directory(), branch))
            ? branch
            : (!head_commit.empty() ? head_commit : branch);
        const auto mt_res = run_git_isolated(
            p.directory(), {"merge-tree", "--write-tree", target_branch, candidate_ref}, 10000);
        if (mt_res.exit_code != 0 || mt_res.output.find("CONFLICT") != std::string::npos) {
            throw std::runtime_error(
                "MERGE_BLOCKED_CONFLICT: non-destructive git merge-tree detected conflict merging '" +
                candidate_ref + "' into '" + target_branch + "': " + mt_res.output);
        }

        const LiveWorktreeMetrics main_live = inspect_live_worktree(p.directory(), target_branch);
        if (main_live.uncommitted_count > 0) {
            throw std::runtime_error(
                "MERGE_BLOCKED_DIRTY_TARGET_WORKTREE: target integration worktree has " +
                std::to_string(main_live.uncommitted_count) + " uncommitted changes");
        }

        if (args.value("enforce_audit_gates", false)) {
            const Json gate = tool_code_audit(p, db, {{"action", "gate_check"}});
            if (!json_bool_or_int(gate, "gate_passed", false)) {
                throw std::runtime_error("MERGE_BLOCKED_AUDIT_GATE: code_audit gate_check failed: " + gate.dump());
            }
        }

        const bool execute = args.value("execute", false);
        std::string new_head = head_commit;
        if (execute) {
            const auto merge_res = run_git_isolated(
                p.directory(), {"merge", "--no-ff", "--no-edit", candidate_ref}, 15000);
            if (merge_res.exit_code != 0) {
                run_git_isolated(p.directory(), {"merge", "--abort"}, 5000);
                throw std::runtime_error("MERGE_EXECUTION_FAILED: " + merge_res.output);
            }
            const auto rev = run_git_isolated(p.directory(), {"rev-parse", "HEAD"}, 4000);
            if (rev.exit_code == 0 && !trim_copy(rev.output).empty()) {
                new_head = trim_copy(rev.output);
            }
            db.exec(
                "UPDATE agent_worktrees_catalog SET lifecycle_state='INTEGRATED', uncommitted_count=0, ahead_count=0, updated_at_utc=? WHERE worktree_id=?",
                {ts, wt_id});
            db.exec(R"SQL(
                INSERT INTO integration_queue (
                    worktree_id, worktree_path, branch, head_commit, target_branch,
                    owner_agent, lifecycle_state, conflict_status, merge_tree_oid,
                    conflicting_files_json, evaluated_at_utc, integrated_at_utc, integrated_commit, status
                ) VALUES (?, ?, ?, ?, ?, ?, 'INTEGRATED', 'ALREADY_MERGED', '', '[]', ?, ?, ?, 'INTEGRATED')
                ON CONFLICT(worktree_id) DO UPDATE SET
                    lifecycle_state='INTEGRATED',
                    conflict_status='ALREADY_MERGED',
                    integrated_at_utc=excluded.integrated_at_utc,
                    integrated_commit=excluded.integrated_commit,
                    status='INTEGRATED',
                    evaluated_at_utc=excluded.evaluated_at_utc
            )SQL", {wt_id, wt_path, branch, head_commit, target_branch, owner_agent, ts, ts, new_head});
        }

        return {
            {"schema", "migratool.integrate_ready/1"},
            {"action", "merge_execute"},
            {"ok", true},
            {"executed", execute},
            {"worktree_id", wt_id},
            {"worktree_path", wt_path},
            {"branch", branch},
            {"target_branch", target_branch},
            {"integrated_commit", new_head},
            {"lifecycle_state", execute ? std::string("INTEGRATED") : state}
        };
    }

    if (action == "mark_integrated") {
        const std::string req_id = args.value("worktree_id", std::string{});
        std::string req_path = args.value("worktree_path", std::string{});
        if (!req_path.empty()) {
            fs::path fp(wide(req_path));
            if (fp.is_relative()) fp = p.directory() / fp;
            req_path = canonical_path_str(fp);
        }
        if (req_id.empty() && req_path.empty()) {
            throw std::runtime_error("MARK_INTEGRATED_MISSING_TARGET: provide worktree_id or worktree_path");
        }
        Json rows = !req_id.empty()
            ? db.query("SELECT * FROM agent_worktrees_catalog WHERE worktree_id=?", {req_id})
            : db.query("SELECT * FROM agent_worktrees_catalog WHERE lower(worktree_path)=lower(?)", {req_path});
        if (rows.empty() && !req_path.empty()) {
            tool_worktree_inventory(p, db, {
                {"action", "register"},
                {"worktree_path", req_path},
                {"lifecycle_state", "INTEGRATED"}
            });
            rows = db.query("SELECT * FROM agent_worktrees_catalog WHERE lower(worktree_path)=lower(?)", {req_path});
        }
        if (rows.empty()) {
            throw std::runtime_error("WORKTREE_NOT_FOUND: worktree not found in agent_worktrees_catalog");
        }
        const std::string wt_id = rows[0].value("worktree_id", std::string{});
        const std::string wt_path = rows[0].value("worktree_path", std::string{});
        const std::string branch = rows[0].value("branch", target_branch);
        const std::string head_commit = args.value("integrated_commit", rows[0].value("head_commit", std::string{}));
        const std::string owner_agent = rows[0].value("owner_agent", std::string{});

        db.exec("UPDATE agent_worktrees_catalog SET lifecycle_state='INTEGRATED', uncommitted_count=0, ahead_count=0, updated_at_utc=? WHERE worktree_id=?",
                {ts, wt_id});
        db.exec(R"SQL(
            INSERT INTO integration_queue (
                worktree_id, worktree_path, branch, head_commit, target_branch,
                owner_agent, lifecycle_state, conflict_status, merge_tree_oid,
                conflicting_files_json, evaluated_at_utc, integrated_at_utc, integrated_commit, status
            ) VALUES (?, ?, ?, ?, ?, ?, 'INTEGRATED', 'ALREADY_MERGED', '', '[]', ?, ?, ?, 'INTEGRATED')
            ON CONFLICT(worktree_id) DO UPDATE SET
                lifecycle_state='INTEGRATED',
                conflict_status='ALREADY_MERGED',
                integrated_at_utc=excluded.integrated_at_utc,
                integrated_commit=excluded.integrated_commit,
                status='INTEGRATED',
                evaluated_at_utc=excluded.evaluated_at_utc
        )SQL", {wt_id, wt_path, branch, head_commit, target_branch, owner_agent, ts, ts, head_commit});
    } else if (action == "enqueue" && (args.contains("worktree_path") || args.contains("worktree_id"))) {
        Json reg_args = args;
        reg_args["action"] = "register";
        if (!reg_args.contains("lifecycle_state")) reg_args["lifecycle_state"] = "CONFIRMED";
        tool_worktree_inventory(p, db, reg_args);
    }

    tool_worktree_inventory(p, db, {{"action", "scan"}, {"target_branch", target_branch}});

    const auto worktrees = db.query("SELECT * FROM agent_worktrees_catalog ORDER BY worktree_path ASC");
    const bool target_ref_valid = git_commit_exists(p.directory(), target_branch);

    for (const auto& wt : worktrees) {
        const std::string wt_id = wt.value("worktree_id", std::string{});
        const std::string wt_path = wt.value("worktree_path", std::string{});
        const std::string branch = wt.value("branch", std::string{});
        const std::string head_commit = wt.value("head_commit", std::string{});
        const std::string owner_agent = wt.value("owner_agent", std::string{});
        const std::string state = wt.value("lifecycle_state", std::string{"IMPLEMENTED"});
        const std::string excluded_by = wt.value("excluded_by", std::string{});
        const int64_t uncommitted = wt.value("uncommitted_count", int64_t{0});

        std::string conflict_status = "CLEAN";
        std::string merge_tree_oid;
        Json conflicting_files = Json::array();
        std::string status = "READY";

        if (!excluded_by.empty()) {
            conflict_status = "EXCLUDED";
            status = "EXCLUDED";
        } else if (state == "INTEGRATED" || state == "DELIVERED") {
            conflict_status = "ALREADY_MERGED";
            status = "INTEGRATED";
        } else {
            const std::string candidate_ref = !head_commit.empty() ? head_commit : branch;
            if (target_ref_valid && git_commit_exists(p.directory(), candidate_ref)) {
                const auto mt_res = run_git_isolated(
                    p.directory(), {"merge-tree", "--write-tree", target_branch, candidate_ref}, 10000);
                std::istringstream iss(mt_res.output);
                std::string first_line;
                if (std::getline(iss, first_line)) {
                    first_line = trim_copy(first_line);
                    if (first_line.size() >= 40 && first_line.find(' ') == std::string::npos) {
                        merge_tree_oid = first_line;
                    }
                }
                if (mt_res.exit_code != 0 || mt_res.output.find("CONFLICT") != std::string::npos) {
                    conflict_status = "CONFLICT";
                    std::istringstream ciss(mt_res.output);
                    std::string cline;
                    std::set<std::string> cfiles;
                    while (std::getline(ciss, cline)) {
                        cline = trim_copy(cline);
                        const auto pos = cline.find("Merge conflict in ");
                        if (pos != std::string::npos) {
                            cfiles.insert(trim_copy(cline.substr(pos + 18)));
                        } else if (cline.starts_with("CONFLICT ")) {
                            cfiles.insert(cline);
                        }
                    }
                    for (const auto& cf : cfiles) conflicting_files.push_back(cf);
                } else {
                    conflict_status = (uncommitted > 0) ? "DIRTY_TREE" : "CLEAN";
                }
            } else {
                conflict_status = (uncommitted > 0) ? "DIRTY_TREE" : "CLEAN";
            }

            if (conflict_status == "CONFLICT" || conflict_status == "DIRTY_TREE") {
                status = "BLOCKED_CONFLICT";
            } else if (state == "IMPLEMENTED") {
                status = "BLOCKED_UNCONFIRMED";
            } else {
                status = "READY";
            }
        }

        db.exec(R"SQL(
            INSERT INTO integration_queue (
                worktree_id, worktree_path, branch, head_commit, target_branch,
                owner_agent, lifecycle_state, conflict_status, merge_tree_oid,
                conflicting_files_json, evaluated_at_utc, status
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            ON CONFLICT(worktree_id) DO UPDATE SET
                worktree_path=excluded.worktree_path,
                branch=excluded.branch,
                head_commit=excluded.head_commit,
                target_branch=excluded.target_branch,
                owner_agent=excluded.owner_agent,
                lifecycle_state=excluded.lifecycle_state,
                conflict_status=excluded.conflict_status,
                merge_tree_oid=excluded.merge_tree_oid,
                conflicting_files_json=excluded.conflicting_files_json,
                evaluated_at_utc=excluded.evaluated_at_utc,
                status=excluded.status
        )SQL", {
            wt_id, wt_path, branch, head_commit, target_branch,
            owner_agent, state, conflict_status, merge_tree_oid,
            conflicting_files.dump(), ts, status
        });
    }

    Json unintegrated_mandatory = Json::array();
    Json excluded_worktrees = Json::array();
    size_t conflict_count = 0;

    for (const auto& wt : worktrees) {
        const Json formatted = format_worktree_row(wt);
        if (!wt.value("excluded_by", std::string{}).empty()) {
            excluded_worktrees.push_back(formatted);
        } else if (is_worktree_unintegrated_mandatory(wt)) {
            unintegrated_mandatory.push_back(formatted);
        }
    }

    const auto q_rows = db.query("SELECT * FROM integration_queue ORDER BY queue_id ASC");
    Json queue_out = Json::array();
    for (const auto& qr : q_rows) {
        Json item = qr;
        item["conflicting_files"] = parse_json_safe(qr.value("conflicting_files_json", std::string{"[]"}), Json::array());
        if (qr.value("conflict_status", std::string{}) == "CONFLICT") ++conflict_count;
        queue_out.push_back(std::move(item));
    }

    return {
        {"schema", "migratool.integrate_ready/1"},
        {"ok", true},
        {"action", action},
        {"target_branch", target_branch},
        {"canonical_build_ready", unintegrated_mandatory.empty() && conflict_count == 0},
        {"unintegrated_mandatory_count", unintegrated_mandatory.size()},
        {"conflict_count", conflict_count},
        {"unintegrated_mandatory_worktrees", unintegrated_mandatory},
        {"excluded_worktrees", excluded_worktrees},
        {"queue", queue_out}
    };
}

Json tool_build_canonical(const Project& p, Database& db, const Json& args) {
    ensure_multiagent_coordinator_schema(db);
    const std::string action = lower_ascii(args.value("action", "record"));
    const std::string proj_name = p.config.value("name", path_text(p.directory().filename()));
    const std::string ts = now();

    if (action == "trace_binary") {
        std::string sha256_q = lower_ascii(trim_copy(args.value("sha256", std::string{})));
        std::string raw_path = trim_copy(args.value("artifact_path", args.value("path", std::string{})));
        std::string norm_path;
        if (!raw_path.empty()) {
            fs::path fp(wide(raw_path));
            if (fp.is_relative()) fp = p.directory() / fp;
            norm_path = canonical_path_str(fp);
            std::error_code ec;
            if (sha256_q.empty() && fs::is_regular_file(native_path(fp), ec)) {
                sha256_q = lower_ascii(hash_file(fp));
            }
        }
        if (sha256_q.empty() && norm_path.empty()) {
            throw std::runtime_error("TRACE_BINARY_MISSING_QUERY: provide sha256 or artifact_path");
        }

        const auto latest_canon = db.query(
            "SELECT build_id FROM build_manifests WHERE lifecycle_state IN ('INTEGRATED', 'DELIVERED') "
            "ORDER BY created_at_utc DESC LIMIT 1");
        const std::string latest_canon_id = latest_canon.empty() ? std::string{} : latest_canon[0].value("build_id", std::string{});

        const auto rows = db.query(R"SQL(
            SELECT
                a.artifact_id, a.build_id, a.artifact_name, a.artifact_path, a.sha256, a.size_bytes, a.mtime_utc, a.kind,
                m.project_name, m.worktree_path, m.branch, m.head_commit, m.tree_clean, m.snapshot_id,
                m.requesting_agent, m.build_type, m.compiler_id, m.lifecycle_state, m.integration_completeness,
                m.unintegrated_worktrees_json, m.excluded_worktrees_json, m.conservation_tier,
                m.total_artifact_bytes, m.build_dir, m.exit_code, m.test_summary_json, m.manifest_path, m.created_at_utc
            FROM build_artifacts a
            JOIN build_manifests m ON a.build_id = m.build_id
            WHERE (? != '' AND lower(a.sha256) = lower(?))
               OR (? != '' AND lower(a.artifact_path) = lower(?))
            ORDER BY m.created_at_utc DESC
        )SQL", {sha256_q, sha256_q, norm_path, norm_path});

        Json matches = Json::array();
        for (const auto& r : rows) {
            Json item = r;
            item["tree_clean"] = (r.value("tree_clean", int64_t{1}) != 0);
            item["unintegrated_worktrees"] = parse_json_safe(r.value("unintegrated_worktrees_json", std::string{"[]"}), Json::array());
            item["excluded_worktrees"] = parse_json_safe(r.value("excluded_worktrees_json", std::string{"[]"}), Json::array());
            item["test_summary"] = parse_json_safe(r.value("test_summary_json", std::string{"{}"}), Json::object());
            const std::string b_id = r.value("build_id", std::string{});
            const std::string l_state = r.value("lifecycle_state", std::string{});
            const bool is_superseded = (!latest_canon_id.empty() && b_id != latest_canon_id)
                                    || (r.value("conservation_tier", std::string{}) == "VERIFIED_SAFE_CLEAN");
            const bool is_partial = (l_state != "INTEGRATED" && l_state != "DELIVERED")
                                 || (r.value("integration_completeness", std::string{}) != "COMPLETE");
            item["is_superseded"] = is_superseded;
            item["is_partial_build"] = is_partial;
            if (is_partial || is_superseded) {
                item["warning"] = is_partial
                    ? "Este binario proviene de una compilación parcial/intermedia (" + l_state + ") que no incluye todos los worktrees integrados."
                    : "Este binario corresponde a una compilación anterior reemplazada por el build canónico " + latest_canon_id + ".";
            } else {
                item["warning"] = "";
            }
            const std::string mpath = r.value("manifest_path", std::string{});
            if (!mpath.empty()) {
                std::error_code ec;
                const fs::path mp(wide(mpath));
                if (fs::is_regular_file(native_path(mp), ec)) {
                    try {
                        item["manifest"] = read_json(mp);
                    } catch (...) {}
                }
            }
            matches.push_back(std::move(item));
        }

        Json out = {
            {"schema", "migratool.build_canonical/1"},
            {"action", "trace_binary"},
            {"ok", !matches.empty()},
            {"found", !matches.empty()},
            {"queried_sha256", sha256_q},
            {"queried_path", norm_path},
            {"matches", matches}
        };
        if (!matches.empty()) {
            const auto& m0 = matches[0];
            out["build_id"] = m0.value("build_id", std::string{});
            out["head_commit"] = m0.value("head_commit", std::string{});
            out["branch"] = m0.value("branch", std::string{});
            out["worktree_path"] = m0.value("worktree_path", std::string{});
            out["requesting_agent"] = m0.value("requesting_agent", std::string{});
            out["lifecycle_state"] = m0.value("lifecycle_state", std::string{});
            out["integration_completeness"] = m0.value("integration_completeness", std::string{});
            out["build"] = m0;
            if (m0.contains("manifest")) out["manifest"] = m0["manifest"];
        }
        return out;
    }

    if (action == "list") {
        const auto rows = db.query("SELECT * FROM build_manifests ORDER BY created_at_utc DESC");
        Json builds = Json::array();
        for (const auto& r : rows) {
            Json b = r;
            b["tree_clean"] = (r.value("tree_clean", int64_t{1}) != 0);
            b["unintegrated_worktrees"] = parse_json_safe(r.value("unintegrated_worktrees_json", std::string{"[]"}), Json::array());
            b["excluded_worktrees"] = parse_json_safe(r.value("excluded_worktrees_json", std::string{"[]"}), Json::array());
            b["test_summary"] = parse_json_safe(r.value("test_summary_json", std::string{"{}"}), Json::object());
            b["artifacts"] = db.query("SELECT * FROM build_artifacts WHERE build_id=? ORDER BY artifact_id ASC",
                                      {r.value("build_id", std::string{})});
            builds.push_back(std::move(b));
        }
        return {
            {"schema", "migratool.build_canonical/1"},
            {"action", "list"},
            {"ok", true},
            {"total_builds", builds.size()},
            {"builds", builds}
        };
    }

    // Default: "record" | "verify_canonical" | "build_and_record"
    const std::string lifecycle_state = upper_ascii(args.value("lifecycle_state", "INTEGRATED"));
    validate_lifecycle_state_or_throw(lifecycle_state);

    const Json integ = tool_integrate_ready(p, db, {
        {"action", "evaluate"},
        {"target_branch", args.value("target_branch", std::string{})}
    });
    const size_t unintegrated_cnt = integ.value("unintegrated_mandatory_count", size_t{0});
    const bool require_complete = args.value("require_complete_integration", true);

    if ((lifecycle_state == "INTEGRATED" || lifecycle_state == "DELIVERED") && require_complete && unintegrated_cnt > 0) {
        throw std::runtime_error(
            "CANONICAL_BUILD_BLOCKED_UNINTEGRATED_WORKTREES: no se puede declarar un build " + lifecycle_state +
            " mientras existan worktrees obligatorios sin integrar: " + integ["unintegrated_mandatory_worktrees"].dump());
    }

    if (args.value("enforce_audit_gates", false)) {
        const Json gate = tool_code_audit(p, db, {{"action", "gate_check"}});
        if (!json_bool_or_int(gate, "gate_passed", false)) {
            throw std::runtime_error("CANONICAL_BUILD_BLOCKED_AUDIT_GATE: code_audit gate_check failed: " + gate.dump());
        }
    }

    const std::string build_id = args.value("build_id", "bld-" + unique_id().substr(0, 12));
    std::string wt_path = args.value("worktree_path", path_text(p.directory()));
    {
        fs::path fp(wide(wt_path));
        if (fp.is_relative()) fp = p.directory() / fp;
        wt_path = canonical_path_str(fp);
    }

    std::string build_dir = args.value("build_dir", std::string{});
    if (!build_dir.empty()) {
        fs::path bdp(wide(build_dir));
        if (bdp.is_relative()) bdp = p.directory() / bdp;
        build_dir = canonical_path_str(bdp);
    }

    int64_t exit_code = args.value("exit_code", int64_t{0});
    if (action == "build_and_record") {
        const int64_t self_pid = static_cast<int64_t>(GetCurrentProcessId());
        db.exec("UPDATE agent_worktrees_catalog SET active_build_pid=?, updated_at_utc=? WHERE lower(worktree_path)=lower(?)",
                {self_pid, ts, wt_path});
        struct ActiveBuildPidGuard {
            Database& db;
            std::string wt_path;
            ~ActiveBuildPidGuard() {
                try {
                    db.exec("UPDATE agent_worktrees_catalog SET active_build_pid=0, updated_at_utc=? WHERE lower(worktree_path)=lower(?)",
                            {now(), wt_path});
                } catch (...) {}
            }
        } pid_guard{db, wt_path};

        if (args.value("execute", false)) {
            std::vector<std::string> cmd_argv;
            if (args.contains("command")) {
                if (args["command"].is_array()) {
                    for (const auto& a : args["command"]) {
                        if (a.is_string()) cmd_argv.push_back(a.get<std::string>());
                    }
                } else if (args["command"].is_string() && !trim_copy(args["command"].get<std::string>()).empty()) {
                    cmd_argv = {"cmd.exe", "/c", args["command"].get<std::string>()};
                }
            }
            if (cmd_argv.empty()) {
                std::string cmake_bin = "cmake.exe";
                wchar_t cbuf[MAX_PATH]{};
                if (SearchPathW(nullptr, L"cmake.exe", nullptr, MAX_PATH, cbuf, nullptr) > 0) {
                    cmake_bin = utf8(cbuf);
                } else {
                    std::error_code cec;
                    if (fs::is_regular_file(L"D:\\DevTools\\CMake\\bin\\cmake.exe", cec)) {
                        cmake_bin = "D:/DevTools/CMake/bin/cmake.exe";
                    }
                }
                const std::string preset = trim_copy(args.value("preset", std::string{}));
                if (!preset.empty()) {
                    cmd_argv = {cmake_bin, "--build", "--preset", preset};
                } else if (!build_dir.empty()) {
                    cmd_argv = {cmake_bin, "--build", build_dir};
                }
            }
            if (cmd_argv.empty()) {
                throw std::runtime_error("CANONICAL_BUILD_MISSING_COMMAND: provide command, preset, or build_dir when execute=true");
            }
            const fs::path log_file = p.state_dir() / L"builds" / wide(build_id) / L"build_exec.log";
            const Json proc_res = run_process(
                cmd_argv, fs::path(wide(wt_path)), "", log_file, static_cast<unsigned>(args.value("timeout_ms", 300000)));
            exit_code = proc_res.value("exit_code", int64_t{-1});
            if (exit_code != 0) {
                throw std::runtime_error("CANONICAL_BUILD_COMMAND_FAILED: exit_code=" + std::to_string(exit_code));
            }
        }
    }

    const LiveWorktreeMetrics live = inspect_live_worktree(fs::path(wide(wt_path)), integ.value("target_branch", std::string{"main"}));
    const std::string branch = args.value("branch", !live.branch.empty() ? live.branch : integ.value("target_branch", std::string{"main"}));
    const std::string head_commit = args.value("head_commit", !live.head_commit.empty() ? live.head_commit : std::string{"HEAD"});
    const bool tree_clean = args.contains("tree_clean") ? args["tree_clean"].get<bool>() : (live.uncommitted_count == 0);
    std::string snapshot_id = args.value("snapshot_id", std::string{});
    const std::string requesting_agent = args.value("requesting_agent", args.value("agent_id", std::string{"agent"}));
    if (!tree_clean && snapshot_id.empty()) {
        try {
            const Json snap_res = tool_workstream(p, db, {
                {"action", "snapshot_create"},
                {"label", "dirty_build_" + build_id},
                {"actor", requesting_agent}
            });
            snapshot_id = snap_res.value("snapshot_id", std::string{});
        } catch (...) {}
        if (snapshot_id.empty()) {
            snapshot_id = "snap-dirty-" + build_id;
        }
    }
    const std::string build_type = args.value("build_type", args.value("build_config", std::string{"Release"}));
    const std::string compiler_id = args.value("compiler_id", std::string{"MSVC"});
    const Json test_summary = args.value("test_summary", Json::object());

    const std::string integration_completeness = (unintegrated_cnt == 0)
        ? (integ.value("excluded_worktrees", Json::array()).empty() ? "COMPLETE" : "COMPLETE_WITH_EXCLUSIONS")
        : "PARTIAL";

    std::string conservation_tier = (lifecycle_state == "INTEGRATED" || lifecycle_state == "DELIVERED")
        ? "PROTECTED"
        : args.value("conservation_tier", std::string{"TEMP_RETAINED"});
    if (!is_valid_conservation_tier(conservation_tier)) conservation_tier = "PROTECTED";

    Json raw_artifacts = args.value("artifacts", Json::array());
    if (raw_artifacts.empty() && !build_dir.empty()) {
        std::error_code ec;
        const fs::path bdp(wide(build_dir));
        if (fs::is_directory(native_path(bdp), ec)) {
            size_t scanned = 0;
            for (fs::recursive_directory_iterator it(native_path(bdp), fs::directory_options::skip_permission_denied, ec), end;
                 it != end && scanned < 400; it.increment(ec)) {
                if (ec) { ec.clear(); continue; }
                ++scanned;
                if (!it->is_regular_file(ec)) continue;
                const std::string ext = lower_ascii(path_text(it->path().extension()));
                if (ext == ".exe" || ext == ".dll") {
                    raw_artifacts.push_back(canonical_path_str(it->path()));
                }
            }
        }
    }

    Json artifacts_out = Json::array();
    int64_t total_artifact_bytes = 0;
    for (const auto& entry : raw_artifacts) {
        std::string a_raw_path;
        std::string a_name;
        std::string a_sha256;
        std::string a_kind;
        int64_t a_size = 0;
        if (entry.is_string()) {
            a_raw_path = entry.get<std::string>();
        } else if (entry.is_object()) {
            a_raw_path = entry.value("artifact_path", entry.value("path", std::string{}));
            a_name = entry.value("artifact_name", entry.value("name", std::string{}));
            a_sha256 = lower_ascii(entry.value("sha256", std::string{}));
            a_kind = entry.value("kind", std::string{});
            a_size = entry.value("size_bytes", int64_t{0});
        }
        if (a_raw_path.empty()) continue;

        fs::path ap(wide(a_raw_path));
        if (ap.is_relative()) ap = p.directory() / ap;
        const std::string a_norm_path = canonical_path_str(ap);
        if (a_name.empty()) a_name = path_text(ap.filename());
        if (a_kind.empty()) {
            const std::string ext = lower_ascii(path_text(ap.extension()));
            if (ext == ".exe") a_kind = "exe";
            else if (ext == ".dll") a_kind = "dll";
            else if (ext == ".lib" || ext == ".a") a_kind = "lib";
            else a_kind = "other";
        }
        std::error_code ec;
        if (fs::is_regular_file(native_path(ap), ec)) {
            a_size = static_cast<int64_t>(fs::file_size(native_path(ap), ec));
            if (a_sha256.empty()) a_sha256 = lower_ascii(hash_file(ap));
        } else if (a_sha256.empty()) {
            a_sha256 = sha256(a_norm_path + ":" + build_id);
        }
        total_artifact_bytes += a_size;
        artifacts_out.push_back({
            {"build_id", build_id},
            {"artifact_name", a_name},
            {"artifact_path", a_norm_path},
            {"sha256", a_sha256},
            {"size_bytes", a_size},
            {"mtime_utc", ts},
            {"kind", a_kind}
        });
    }

    const fs::path manifest_fs = p.state_dir() / L"builds" / wide(build_id) / L"manifest.json";
    const std::string manifest_path_str = canonical_path_str(manifest_fs);

    const Json unintegrated_wts = integ.value("unintegrated_mandatory_worktrees", Json::array());
    const Json excluded_wts = integ.value("excluded_worktrees", Json::array());

    Json manifest_doc = {
        {"schema", "migratool.build_manifest/1"},
        {"build_id", build_id},
        {"project_name", proj_name},
        {"worktree_path", wt_path},
        {"branch", branch},
        {"head_commit", head_commit},
        {"tree_clean", tree_clean},
        {"snapshot_id", snapshot_id},
        {"requesting_agent", requesting_agent},
        {"build_type", build_type},
        {"compiler_id", compiler_id},
        {"lifecycle_state", lifecycle_state},
        {"integration_completeness", integration_completeness},
        {"unintegrated_worktrees", unintegrated_wts},
        {"excluded_worktrees", excluded_wts},
        {"conservation_tier", conservation_tier},
        {"total_artifact_bytes", total_artifact_bytes},
        {"build_dir", build_dir},
        {"exit_code", exit_code},
        {"test_summary", test_summary},
        {"artifacts", artifacts_out},
        {"manifest_path", manifest_path_str},
        {"created_at_utc", ts}
    };

    write_json(manifest_fs, manifest_doc);

    if ((lifecycle_state == "INTEGRATED" || lifecycle_state == "DELIVERED") && args.value("demote_previous_builds", true)) {
        db.exec("UPDATE build_manifests SET conservation_tier='VERIFIED_SAFE_CLEAN' WHERE build_id != ?", {build_id});
    }

    db.exec(R"SQL(
        INSERT INTO build_manifests (
            build_id, project_name, worktree_path, branch, head_commit, tree_clean,
            snapshot_id, requesting_agent, build_type, compiler_id, lifecycle_state,
            integration_completeness, unintegrated_worktrees_json, excluded_worktrees_json,
            conservation_tier, total_artifact_bytes, build_dir, exit_code, test_summary_json,
            manifest_path, created_at_utc
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(build_id) DO UPDATE SET
            worktree_path=excluded.worktree_path,
            branch=excluded.branch,
            head_commit=excluded.head_commit,
            tree_clean=excluded.tree_clean,
            snapshot_id=excluded.snapshot_id,
            requesting_agent=excluded.requesting_agent,
            build_type=excluded.build_type,
            compiler_id=excluded.compiler_id,
            lifecycle_state=excluded.lifecycle_state,
            integration_completeness=excluded.integration_completeness,
            unintegrated_worktrees_json=excluded.unintegrated_worktrees_json,
            excluded_worktrees_json=excluded.excluded_worktrees_json,
            conservation_tier=excluded.conservation_tier,
            total_artifact_bytes=excluded.total_artifact_bytes,
            build_dir=excluded.build_dir,
            exit_code=excluded.exit_code,
            test_summary_json=excluded.test_summary_json,
            manifest_path=excluded.manifest_path,
            created_at_utc=excluded.created_at_utc
    )SQL", {
        build_id, proj_name, wt_path, branch, head_commit, tree_clean ? 1 : 0,
        snapshot_id, requesting_agent, build_type, compiler_id, lifecycle_state,
        integration_completeness, unintegrated_wts.dump(), excluded_wts.dump(),
        conservation_tier, total_artifact_bytes, build_dir, exit_code, test_summary.dump(),
        manifest_path_str, ts
    });

    db.exec("DELETE FROM build_artifacts WHERE build_id=?", {build_id});
    Statement ins_art(db, R"SQL(
        INSERT INTO build_artifacts (build_id, artifact_name, artifact_path, sha256, size_bytes, mtime_utc, kind)
        VALUES (?, ?, ?, ?, ?, ?, ?)
    )SQL");
    for (const auto& a : artifacts_out) {
        ins_art.exec({
            build_id,
            a.value("artifact_name", std::string{}),
            a.value("artifact_path", std::string{}),
            a.value("sha256", std::string{}),
            a.value("size_bytes", int64_t{0}),
            a.value("mtime_utc", ts),
            a.value("kind", std::string{"other"})
        });
    }

    Json resp = manifest_doc;
    resp["schema"] = "migratool.build_canonical/1";
    resp["action"] = action;
    resp["ok"] = true;
    return resp;
}

namespace {

bool is_handwritten_source_ext(const std::string& ext_low) {
    static const std::unordered_set<std::string> kExts = {
        ".cpp", ".hpp", ".h", ".c", ".cc", ".cxx", ".cs", ".rs", ".py", ".go", ".java", ".ts", ".tsx", ".js", ".jsx", ".rc"
    };
    return kExts.contains(ext_low);
}

bool is_ambiguous_folder_name(const std::string& name_low) {
    static const std::unordered_set<std::string> kNames = {
        "old", "backup", "backups", "temp", "tmp", "copy", "copies", "archive", "archives", "bak", "saved"
    };
    if (kNames.contains(name_low)) return true;
    return name_low.ends_with("-old") || name_low.ends_with("_old")
        || name_low.ends_with("-backup") || name_low.ends_with("_backup")
        || name_low.ends_with("-bak") || name_low.ends_with("_bak")
        || name_low.ends_with("-copy") || name_low.ends_with("_copy");
}

bool is_always_protected_core_dir(const std::string& name_low) {
    static const std::unordered_set<std::string> kCore = {
        "src", "include", "tests", "test", "tools", "scripts", "docs", "doc",
        "adapters", "resources", "examples", "third_party", ".git", ".migratool", ".agents", ".github"
    };
    return kCore.contains(name_low);
}

bool directory_has_handwritten_sources(const fs::path& dir, size_t max_entries = 1500) {
    std::error_code ec;
    if (!fs::is_directory(native_path(dir), ec)) return false;
    size_t visited = 0;
    for (fs::recursive_directory_iterator it(native_path(dir), fs::directory_options::skip_permission_denied, ec), end;
         it != end && visited < max_entries; it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        ++visited;
        if (it->is_directory(ec)) {
            const std::string dlow = lower_ascii(path_text(it->path().filename()));
            if (dlow == "cmakefiles" || dlow == "zip-headers" || dlow == "_deps"
                || dlow == "vcpkg_installed" || dlow == "node_modules" || dlow == ".git") {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (it->is_regular_file(ec)) {
            const std::string ext = lower_ascii(path_text(it->path().extension()));
            if (is_handwritten_source_ext(ext)) return true;
        }
    }
    return false;
}

bool pattern_matches_dir(std::string pat_low, const std::string& rel_low, const std::string& name_low) {
    for (char& c : pat_low) {
        if (c == '\\') c = '/';
    }
    while (pat_low.ends_with("/**")) pat_low.resize(pat_low.size() - 3);
    while (pat_low.ends_with("/*")) pat_low.resize(pat_low.size() - 2);
    while (pat_low.starts_with("**/")) pat_low = pat_low.substr(3);
    while (!pat_low.empty() && pat_low.back() == '/') pat_low.pop_back();
    if (pat_low.empty()) return false;

    if (pat_low.back() == '*') {
        const std::string prefix = pat_low.substr(0, pat_low.size() - 1);
        return rel_low.starts_with(prefix) || name_low.starts_with(prefix);
    }
    return rel_low == pat_low || name_low == pat_low || rel_low.starts_with(pat_low + "/");
}

} // namespace

Json tool_storage_audit(const Project& p, Database& db, const Json& args) {
    ensure_multiagent_coordinator_schema(db);
    const std::string action = lower_ascii(args.value("action", "audit"));
    const std::string ts = now();

    constexpr int64_t kGiB = 1024LL * 1024LL * 1024LL;
    auto read_quota_policy = [&]() -> Json {
        const auto qrows = db.query("SELECT * FROM storage_quota_policies WHERE policy_key='default' LIMIT 1");
        int64_t max_bytes = 20LL * kGiB;
        int64_t keep_n = 3;
        int64_t max_days = 14;
        std::string updated_utc;
        if (!qrows.empty()) {
            max_bytes = qrows[0].value("max_build_storage_bytes", max_bytes);
            keep_n = qrows[0].value("keep_last_n_builds", keep_n);
            max_days = qrows[0].value("max_temp_age_days", max_days);
            updated_utc = qrows[0].value("updated_at_utc", std::string{});
        }
        Json max_gb_val = (max_bytes % kGiB == 0)
            ? Json(max_bytes / kGiB)
            : Json(static_cast<double>(max_bytes) / static_cast<double>(kGiB));
        return {
            {"policy_key", "default"},
            {"max_build_storage_bytes", max_bytes},
            {"max_build_storage_gb", max_gb_val},
            {"keep_last_n_builds", keep_n},
            {"max_temp_age_days", max_days},
            {"updated_at_utc", updated_utc}
        };
    };

    if (action == "get_quota") {
        return {
            {"schema", "migratool.storage_audit/1"},
            {"action", "get_quota"},
            {"ok", true},
            {"quota_policy", read_quota_policy()}
        };
    }

    if (action == "set_quota") {
        const Json cur = read_quota_policy();
        int64_t max_bytes = cur.value("max_build_storage_bytes", 20LL * kGiB);
        int64_t keep_n = cur.value("keep_last_n_builds", int64_t{3});
        int64_t max_days = cur.value("max_temp_age_days", int64_t{14});

        if (args.contains("max_build_storage_bytes") && args["max_build_storage_bytes"].is_number()) {
            max_bytes = args["max_build_storage_bytes"].get<int64_t>();
        } else if (args.contains("max_build_storage_gb") && args["max_build_storage_gb"].is_number()) {
            if (args["max_build_storage_gb"].is_number_integer() || args["max_build_storage_gb"].is_number_unsigned()) {
                max_bytes = args["max_build_storage_gb"].get<int64_t>() * kGiB;
            } else {
                max_bytes = static_cast<int64_t>(args["max_build_storage_gb"].get<double>() * static_cast<double>(kGiB));
            }
        }
        if (args.contains("keep_last_n_builds") && args["keep_last_n_builds"].is_number()) {
            keep_n = args["keep_last_n_builds"].get<int64_t>();
        }
        if (args.contains("max_temp_age_days") && args["max_temp_age_days"].is_number()) {
            max_days = args["max_temp_age_days"].get<int64_t>();
        }
        if (max_bytes <= 0) {
            throw std::runtime_error("INVALID_STORAGE_QUOTA: max_build_storage_bytes must be > 0");
        }
        if (keep_n < 1) {
            throw std::runtime_error("INVALID_STORAGE_QUOTA: keep_last_n_builds must be >= 1");
        }
        if (max_days < 0) {
            throw std::runtime_error("INVALID_STORAGE_QUOTA: max_temp_age_days must be >= 0");
        }

        db.exec(R"SQL(
            INSERT INTO storage_quota_policies
                (policy_key, max_build_storage_bytes, keep_last_n_builds, max_temp_age_days, updated_at_utc)
            VALUES ('default', ?, ?, ?, ?)
            ON CONFLICT(policy_key) DO UPDATE SET
                max_build_storage_bytes=excluded.max_build_storage_bytes,
                keep_last_n_builds=excluded.keep_last_n_builds,
                max_temp_age_days=excluded.max_temp_age_days,
                updated_at_utc=excluded.updated_at_utc
        )SQL", {max_bytes, keep_n, max_days, ts});

        const Json qp = read_quota_policy();
        return {
            {"schema", "migratool.storage_audit/1"},
            {"action", "set_quota"},
            {"ok", true},
            {"quota_policy", qp}
        };
    }

    if (action == "set_policy") {
        const std::string pattern = trim_copy(args.value("pattern", std::string{}));
        const std::string match_kind = lower_ascii(args.value("match_kind", std::string{"prefix"}));
        const std::string category = upper_ascii(args.value("category", std::string{"UNKNOWN"}));
        const std::string default_tier = upper_ascii(
            args.value("default_tier", args.value("default_conservation_tier", std::string{"NEEDS_REVIEW"})));
        const std::string rationale = args.value("rationale", args.value("notes", std::string{}));

        if (pattern.empty()) {
            throw std::runtime_error("INVALID_POLICY_PATTERN: pattern cannot be empty");
        }
        if (!is_valid_directory_category(category)) {
            throw std::runtime_error("INVALID_DIRECTORY_CATEGORY: '" + category + "'");
        }
        if (!is_valid_conservation_tier(default_tier)) {
            throw std::runtime_error("INVALID_CONSERVATION_TIER: '" + default_tier + "'");
        }

        db.exec(R"SQL(
            INSERT INTO directory_classification_policies
                (pattern, match_kind, category, default_tier, is_builtin, rationale, updated_at_utc)
            VALUES (?, ?, ?, ?, 0, ?, ?)
            ON CONFLICT(pattern) DO UPDATE SET
                match_kind=excluded.match_kind,
                category=excluded.category,
                default_tier=excluded.default_tier,
                is_builtin=0,
                rationale=excluded.rationale,
                updated_at_utc=excluded.updated_at_utc
        )SQL", {pattern, match_kind, category, default_tier, rationale, ts});

        const auto policies = db.query("SELECT * FROM directory_classification_policies ORDER BY is_builtin ASC, pattern ASC");
        return {
            {"schema", "migratool.storage_audit/1"},
            {"action", "set_policy"},
            {"ok", true},
            {"pattern", pattern},
            {"category", category},
            {"default_tier", default_tier},
            {"policies", policies}
        };
    }

    if (action == "list_policies") {
        const auto policies = db.query("SELECT * FROM directory_classification_policies ORDER BY is_builtin ASC, pattern ASC");
        return {
            {"schema", "migratool.storage_audit/1"},
            {"action", "list_policies"},
            {"ok", true},
            {"policies", policies}
        };
    }

    // "audit" or "clean_verified"
    // Custom policies (is_builtin = 0) take precedence over built-in defaults
    const auto policies = db.query("SELECT * FROM directory_classification_policies ORDER BY is_builtin ASC, length(pattern) DESC");
    const auto manifests = db.query("SELECT build_id, build_dir, lifecycle_state, conservation_tier FROM build_manifests");
    const auto worktrees = db.query("SELECT worktree_path, uncommitted_count, untracked_count, active_build_pid, lifecycle_state FROM agent_worktrees_catalog");

    std::unordered_map<std::string, std::string> build_dir_tier;
    for (const auto& m : manifests) {
        const std::string bdir_key = normalize_path_key_str(m.value("build_dir", std::string{}));
        if (bdir_key.empty()) continue;
        const std::string mtier = m.value("conservation_tier", std::string{"TEMP_RETAINED"});
        const auto it = build_dir_tier.find(bdir_key);
        if (it == build_dir_tier.end() || mtier == "PROTECTED" || (mtier == "TEMP_RETAINED" && it->second != "PROTECTED")) {
            build_dir_tier[bdir_key] = mtier;
        }
    }

    std::vector<fs::path> candidate_dirs;
    std::unordered_set<std::string> seen_dirs;
    auto add_candidate_dir = [&](const fs::path& dp) {
        std::error_code ec;
        if (dp.empty() || !fs::exists(native_path(dp), ec)) return;
        const std::string k = normalize_path_key(dp);
        if (!k.empty() && seen_dirs.insert(k).second) {
            candidate_dirs.push_back(canonical_win32_path(dp));
        }
    };

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(native_path(p.directory()), ec)) {
        if (entry.is_directory(ec)) {
            add_candidate_dir(entry.path());
        }
    }
    for (const char* key : {"extra_paths", "target_paths"}) {
        if (args.contains(key) && args[key].is_array()) {
            for (const auto& item : args[key]) {
                if (item.is_string()) {
                    fs::path fp(wide(item.get<std::string>()));
                    if (fp.is_relative()) fp = p.directory() / fp;
                    add_candidate_dir(fp);
                }
            }
        }
    }

    const fs::path root_canon = canonical_win32_path(p.directory());
    Json items = Json::array();
    std::unordered_map<std::string, Json> item_by_key;
    std::map<std::string, int64_t> bytes_by_tier = {
        {"PROTECTED", 0}, {"TEMP_RETAINED", 0}, {"VERIFIED_SAFE_CLEAN", 0}, {"NEEDS_REVIEW", 0}
    };
    std::map<std::string, int64_t> count_by_tier = {
        {"PROTECTED", 0}, {"TEMP_RETAINED", 0}, {"VERIFIED_SAFE_CLEAN", 0}, {"NEEDS_REVIEW", 0}
    };
    std::map<std::string, int64_t> bytes_by_category;
    int64_t reclaimable_bytes = 0;

    for (const auto& dir_path : candidate_dirs) {
        const std::string dir_str = canonical_path_str(dir_path);
        const std::string dir_key = lower_ascii(dir_str);
        const std::string name_str = path_text(dir_path.filename());
        const std::string name_low = lower_ascii(name_str);

        std::string rel_str = name_str;
        std::error_code rel_ec;
        const fs::path rel_p = fs::relative(dir_path, root_canon, rel_ec);
        if (!rel_ec && !rel_p.empty() && !path_text(rel_p).starts_with("..")) {
            rel_str = path_text(rel_p);
        }
        const std::string rel_low = lower_ascii(rel_str);

        size_t file_count = 0;
        const int64_t dir_bytes = compute_bounded_dir_bytes(dir_path, 2000, &file_count);
        const bool has_sources = directory_has_handwritten_sources(dir_path);

        bool has_dirty_wt = false;
        bool has_active_build_pid = false;
        for (const auto& wt : worktrees) {
            const std::string wtk = normalize_path_key_str(wt.value("worktree_path", std::string{}));
            if (wtk == dir_key || wtk.starts_with(dir_key + "/")) {
                if (wt.value("uncommitted_count", int64_t{0}) > 0 || wt.value("untracked_count", int64_t{0}) > 0) {
                    has_dirty_wt = true;
                }
                if (pid_alive(wt.value("active_build_pid", int64_t{0}))) {
                    has_active_build_pid = true;
                }
            }
        }
        if (!has_dirty_wt && fs::exists(native_path(dir_path / L".git"), rel_ec)) {
            const LiveWorktreeMetrics lm = inspect_live_worktree(dir_path);
            if (lm.uncommitted_count > 0 || lm.untracked_count > 0) has_dirty_wt = true;
        }

        std::string category = "UNKNOWN";
        std::string tier = "NEEDS_REVIEW";
        std::string reason = "Prohibida limpieza ciega por nombre de carpeta; requiere revisión humana";
        bool matched_policy = false;
        bool matched_custom_policy = false;

        for (const auto& pol : policies) {
            const std::string pat = lower_ascii(pol.value("pattern", std::string{}));
            if (pattern_matches_dir(pat, rel_low, name_low)) {
                category = pol.value("category", std::string{"UNKNOWN"});
                tier = pol.value("default_tier", std::string{"NEEDS_REVIEW"});
                reason = pol.value("rationale", std::string{"Clasificado por política de proyecto"});
                matched_policy = true;
                matched_custom_policy = (pol.value("is_builtin", int64_t{1}) == 0);
                break;
            }
        }

        if (!matched_policy && (name_low.starts_with("build-") || name_low.starts_with("build_") || name_low.starts_with("cmake-build-"))) {
            if (!is_ambiguous_folder_name(name_low)) {
                category = "BUILD_OUTPUT";
                tier = "TEMP_RETAINED";
                reason = "Variante de directorio de compilación";
                matched_policy = true;
            }
        }

        // Enforce strict safety invariants (REQ-STORAGE-001)
        if (is_always_protected_core_dir(name_low)) {
            if (category == "UNKNOWN") {
                if (name_low == "tests" || name_low == "test") category = "TEST_SOURCE";
                else if (name_low == "docs" || name_low == "doc") category = "DOCUMENTATION";
                else if (name_low == ".migratool" || name_low == ".git") category = "USER_DATA";
                else category = "SOURCE";
            }
            tier = "PROTECTED";
            reason = "Directorio núcleo protegido (código fuente, pruebas, documentación o metadatos)";
        } else if (is_ambiguous_folder_name(name_low) && !matched_custom_policy) {
            category = "UNKNOWN";
            tier = has_dirty_wt ? "PROTECTED" : "NEEDS_REVIEW";
            reason = "Prohibida limpieza ciega por nombre ambiguo (old/backup/temp); requiere revisión";
        } else if (category == "SOURCE" || category == "TEST_SOURCE" || category == "DOCUMENTATION" || category == "USER_DATA") {
            tier = "PROTECTED";
        } else if (category == "BUILD_OUTPUT" || category == "CACHE" || category == "GENERATED") {
            if (has_dirty_wt || has_sources) {
                tier = "PROTECTED";
                reason = has_dirty_wt
                    ? "Contiene worktree con modificaciones sin confirmar (dirty)"
                    : "Contiene archivos de código fuente escritos a mano fuera de CMakeFiles/zip-headers";
            } else if (has_active_build_pid) {
                tier = "TEMP_RETAINED";
                reason = "Proceso de compilación propietario activo";
            } else {
                const auto bit = build_dir_tier.find(dir_key);
                if (bit != build_dir_tier.end()) {
                    if (bit->second == "PROTECTED") {
                        tier = "PROTECTED";
                        reason = "Directorio perteneciente a una compilación canónica protegida (INTEGRATED / DELIVERED)";
                    } else if (bit->second == "TEMP_RETAINED") {
                        tier = "TEMP_RETAINED";
                        reason = "Compilación reciente dentro de ventana de retención temporal";
                    } else {
                        tier = "VERIFIED_SAFE_CLEAN";
                        reason = "Compilación anterior reemplazada, 100% reproducible y sin código fuente ni procesos activos";
                    }
                } else {
                    tier = "VERIFIED_SAFE_CLEAN";
                    reason = "Artefactos generados/compilación 100% reproducibles sin código fuente ni build canónico protegido";
                }
            }
        } else if (!matched_policy) {
            category = "UNKNOWN";
            tier = has_dirty_wt ? "PROTECTED" : "NEEDS_REVIEW";
            reason = "Directorio no clasificado; requiere revisión humana o política explícita";
        }

        bytes_by_tier[tier] += dir_bytes;
        count_by_tier[tier] += 1;
        bytes_by_category[category] += dir_bytes;
        if (tier == "VERIFIED_SAFE_CLEAN") {
            reclaimable_bytes += dir_bytes;
        }

        Json item = {
            {"path", dir_str},
            {"relative_path", rel_str},
            {"name", name_str},
            {"category", category},
            {"conservation_tier", tier},
            {"auto_clean_allowed", tier == "VERIFIED_SAFE_CLEAN"},
            {"has_handwritten_sources", has_sources},
            {"has_uncommitted_changes", has_dirty_wt},
            {"size_bytes", dir_bytes},
            {"file_count", file_count},
            {"reason", reason}
        };
        item_by_key[dir_key] = item;
        items.push_back(std::move(item));
    }

    Json cleaned_paths = Json::array();
    int64_t reclaimed_now_bytes = 0;

    if (action == "clean_verified") {
        std::vector<Json> targets_to_clean;
        if (args.contains("target_paths") && args["target_paths"].is_array() && !args["target_paths"].empty()) {
            for (const auto& tp_val : args["target_paths"]) {
                if (!tp_val.is_string()) continue;
                fs::path fp(wide(tp_val.get<std::string>()));
                if (fp.is_relative()) fp = p.directory() / fp;
                const std::string k = normalize_path_key(fp);
                const auto it = item_by_key.find(k);
                if (it == item_by_key.end()) {
                    throw std::runtime_error("STORAGE_CLEAN_REFUSED_NON_SAFE_TIER: path not audited or does not exist: " + canonical_path_str(fp));
                }
                const std::string tier = it->second.value("conservation_tier", std::string{"NEEDS_REVIEW"});
                if (tier != "VERIFIED_SAFE_CLEAN") {
                    throw std::runtime_error(
                        "STORAGE_CLEAN_REFUSED_NON_SAFE_TIER: prohibido limpiar '" +
                        it->second.value("path", std::string{}) + "' con nivel de conservación " + tier);
                }
                targets_to_clean.push_back(it->second);
            }
        } else {
            for (const auto& item : items) {
                if (item.value("conservation_tier", std::string{}) == "VERIFIED_SAFE_CLEAN") {
                    targets_to_clean.push_back(item);
                }
            }
        }

        const bool execute = args.value("execute", false);
        for (const auto& t : targets_to_clean) {
            const std::string tpath = t.value("path", std::string{});
            const int64_t sz = t.value("size_bytes", int64_t{0});
            bool recycled = false;
            if (execute) {
                recycled = send_file_to_recycle_bin(fs::path(wide(tpath)));
                if (recycled) reclaimed_now_bytes += sz;
            }
            cleaned_paths.push_back({
                {"path", tpath},
                {"conservation_tier", "VERIFIED_SAFE_CLEAN"},
                {"deletion_mechanism", "WINDOWS_RECYCLE_BIN"},
                {"executed", execute},
                {"recycled", recycled},
                {"size_bytes", sz}
            });
        }
    }

    const Json quota_policy = read_quota_policy();
    const int64_t max_quota_bytes = quota_policy.value("max_build_storage_bytes", 20LL * kGiB);
    const int64_t build_and_cache_bytes = bytes_by_category["BUILD_OUTPUT"] + bytes_by_category["CACHE"];
    const bool quota_exceeded = (build_and_cache_bytes > max_quota_bytes);
    const int64_t bytes_over_quota = quota_exceeded ? (build_and_cache_bytes - max_quota_bytes) : int64_t{0};

    struct DupArtifactGroup {
        int64_t size_bytes = 0;
        std::vector<std::string> paths;
    };
    std::map<std::string, DupArtifactGroup> dup_by_sha;
    std::unordered_set<std::string> seen_artifact_paths;

    for (const auto& item : items) {
        if (item.value("category", std::string{}) != "BUILD_OUTPUT") continue;
        const fs::path bdir(wide(item.value("path", std::string{})));
        std::error_code dec;
        if (!fs::is_directory(native_path(bdir), dec)) continue;
        size_t scanned = 0;
        for (fs::recursive_directory_iterator it(native_path(bdir), fs::directory_options::skip_permission_denied, dec), end;
             it != end && scanned < 400; it.increment(dec)) {
            if (dec) { dec.clear(); continue; }
            ++scanned;
            if (!it->is_regular_file(dec)) continue;
            const std::string ext = lower_ascii(path_text(it->path().extension()));
            if (ext != ".exe" && ext != ".dll" && ext != ".lib" && ext != ".pdb" && ext != ".obj") continue;
            const auto fsz = it->file_size(dec);
            if (dec || fsz == 0 || fsz > 64 * 1024 * 1024) continue;
            const std::string fpath = canonical_path_str(it->path());
            if (!seen_artifact_paths.insert(lower_ascii(fpath)).second) continue;
            try {
                const std::string sha = lower_ascii(hash_file(it->path()));
                if (!sha.empty()) {
                    auto& grp = dup_by_sha[sha];
                    grp.size_bytes = static_cast<int64_t>(fsz);
                    grp.paths.push_back(fpath);
                }
            } catch (...) {}
        }
    }

    const auto art_rows = db.query("SELECT artifact_path, sha256, size_bytes FROM build_artifacts WHERE sha256 != ''");
    for (const auto& ar : art_rows) {
        const std::string apath = ar.value("artifact_path", std::string{});
        const std::string sha = lower_ascii(ar.value("sha256", std::string{}));
        const int64_t asz = ar.value("size_bytes", int64_t{0});
        if (apath.empty() || sha.empty()) continue;
        if (!seen_artifact_paths.insert(lower_ascii(apath)).second) continue;
        auto& grp = dup_by_sha[sha];
        if (grp.size_bytes == 0) grp.size_bytes = asz;
        grp.paths.push_back(apath);
    }

    Json duplicate_artifact_groups = Json::array();
    int64_t duplicate_artifact_reclaimable_bytes = 0;
    for (const auto& [sha, grp] : dup_by_sha) {
        if (grp.paths.size() < 2) continue;
        const int64_t reclaim = grp.size_bytes * static_cast<int64_t>(grp.paths.size() - 1);
        duplicate_artifact_reclaimable_bytes += reclaim;
        duplicate_artifact_groups.push_back({
            {"sha256", sha},
            {"size_bytes", grp.size_bytes},
            {"count", grp.paths.size()},
            {"reclaimable_bytes", reclaim},
            {"paths", grp.paths}
        });
    }

    return {
        {"schema", "migratool.storage_audit/1"},
        {"ok", true},
        {"action", action},
        {"reclaimable_bytes", reclaimable_bytes},
        {"reclaimed_bytes", reclaimed_now_bytes},
        {"quota_policy", quota_policy},
        {"quota_exceeded", quota_exceeded},
        {"bytes_over_quota", bytes_over_quota},
        {"duplicate_artifact_groups", duplicate_artifact_groups},
        {"duplicate_artifact_reclaimable_bytes", duplicate_artifact_reclaimable_bytes},
        {"counts_by_tier", count_by_tier},
        {"totals_by_tier", bytes_by_tier},
        {"totals_by_category", bytes_by_category},
        {"cleaned_items", cleaned_paths},
        {"items", items}
    };
}

namespace {

struct ProjectFingerprint {
    std::string git_root_commit;
    std::string git_head_commit;
    std::string git_branch;
    std::string git_remote_url;
    std::unordered_set<std::string> linked_worktree_keys;
    std::unordered_map<std::string, std::string> rel_to_sha256;
    std::unordered_set<std::string> all_sha256s;
};

bool should_skip_fingerprint_dir(const std::string& dlow) {
    return dlow == ".git" || dlow == ".migratool" || dlow == "vcpkg_installed"
        || dlow == "node_modules" || dlow == "cmakefiles" || dlow == "_deps"
        || dlow == ".cache" || dlow == "cache" || dlow == "out"
        || dlow == "build" || dlow.starts_with("build-") || dlow.starts_with("build_");
}

bool is_fingerprint_candidate_file(const fs::path& fp) {
    const std::string ext_low = lower_ascii(path_text(fp.extension()));
    static const std::unordered_set<std::string> kExcludedExts = {
        ".obj", ".o", ".pch", ".pdb", ".ilk", ".exp", ".exe", ".dll", ".lib", ".a", ".so",
        ".sqlite", ".sqlite-wal", ".sqlite-shm", ".db", ".zip", ".7z", ".tar", ".gz", ".png", ".ico"
    };
    return !kExcludedExts.contains(ext_low);
}

void collect_dir_source_hashes(const fs::path& root_dir,
                               std::unordered_map<std::string, std::string>& out_rel_to_sha,
                               std::unordered_set<std::string>& out_all_sha,
                               size_t max_files = 250) {
    std::error_code ec;
    const fs::path canon_root = canonical_win32_path(root_dir);
    if (canon_root.empty() || !fs::is_directory(native_path(canon_root), ec)) return;

    size_t visited = 0;
    for (fs::recursive_directory_iterator it(native_path(canon_root), fs::directory_options::skip_permission_denied, ec), end;
         it != end && out_rel_to_sha.size() < max_files && visited < 3000; it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        ++visited;
        if (it->is_directory(ec)) {
            const std::string dlow = lower_ascii(path_text(it->path().filename()));
            if (should_skip_fingerprint_dir(dlow)) {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (!it->is_regular_file(ec)) continue;
        if (!is_fingerprint_candidate_file(it->path())) continue;
        const auto fsz = it->file_size(ec);
        if (ec || fsz > 4 * 1024 * 1024) continue;

        const fs::path rel = fs::relative(canonical_win32_path(it->path()), canon_root, ec);
        if (ec || rel.empty()) continue;
        const std::string rel_key = lower_ascii(path_text(rel));
        try {
            const std::string h = lower_ascii(hash_file(it->path()));
            out_rel_to_sha[rel_key] = h;
            out_all_sha.insert(h);
        } catch (...) {}
    }
}

std::string first_line_trimmed(const std::string& s) {
    std::istringstream iss(s);
    std::string line;
    if (std::getline(iss, line)) return trim_copy(line);
    return {};
}

ProjectFingerprint compute_project_fingerprint(const fs::path& dir) {
    ProjectFingerprint fp;
    const auto root_res = run_git_isolated(dir, {"rev-list", "--max-parents=0", "HEAD"}, 5000);
    if (root_res.exit_code == 0) fp.git_root_commit = first_line_trimmed(root_res.output);

    const auto head_res = run_git_isolated(dir, {"rev-parse", "HEAD"}, 4000);
    if (head_res.exit_code == 0) fp.git_head_commit = first_line_trimmed(head_res.output);

    const auto br_res = run_git_isolated(dir, {"rev-parse", "--abbrev-ref", "HEAD"}, 4000);
    if (br_res.exit_code == 0) fp.git_branch = first_line_trimmed(br_res.output);

    const auto rem_res = run_git_isolated(dir, {"config", "--get", "remote.origin.url"}, 4000);
    if (rem_res.exit_code == 0) fp.git_remote_url = first_line_trimmed(rem_res.output);

    const auto wt_res = run_git_isolated(dir, {"worktree", "list", "--porcelain"}, 5000);
    if (wt_res.exit_code == 0) {
        std::istringstream iss(wt_res.output);
        std::string line;
        while (std::getline(iss, line)) {
            line = trim_copy(line);
            if (line.starts_with("worktree ")) {
                fp.linked_worktree_keys.insert(normalize_path_key_str(trim_copy(line.substr(9))));
            }
        }
    }

    collect_dir_source_hashes(dir, fp.rel_to_sha256, fp.all_sha256s);
    return fp;
}

} // namespace

Json tool_discover_replicas(const Project& p, Database& db, const Json& args) {
    ensure_multiagent_coordinator_schema(db);
    const std::string action = lower_ascii(args.value("action", "discover"));
    const std::string ts = now();

    if (action == "list") {
        const auto rows = db.query("SELECT * FROM discovered_project_replicas ORDER BY candidate_path ASC");
        Json items = Json::array();
        for (const auto& r : rows) {
            Json item = r;
            const std::string cpath = r.value("candidate_path", std::string{});
            item["replica_path"] = cpath;
            item["path"] = cpath;
            item["is_ancestor_of_main"] = (r.value("is_ancestor_of_main", int64_t{0}) != 0);
            item["unique_files_sample"] = parse_json_safe(r.value("unique_files_sample_json", std::string{"[]"}), Json::array());
            items.push_back(std::move(item));
        }
        return {
            {"schema", "migratool.discover_replicas/1"},
            {"action", "list"},
            {"ok", true},
            {"total_discovered", items.size()},
            {"replicas", items}
        };
    }

    const ProjectFingerprint ref = compute_project_fingerprint(p.directory());
    const std::string main_key = normalize_path_key(p.directory());

    struct CandidateItem {
        fs::path path;
        std::string display_path;
        bool is_online = true;
    };
    std::vector<CandidateItem> candidates;
    std::unordered_set<std::string> seen_keys;

    auto persist_search_root = [&](const std::string& rpath, const std::string& skind, bool online) {
        if (rpath.empty()) return;
        db.exec(R"SQL(
            INSERT INTO discovery_search_roots (root_path, source_kind, last_online, last_scanned_utc)
            VALUES (?, ?, ?, ?)
            ON CONFLICT(root_path) DO UPDATE SET
                source_kind=excluded.source_kind,
                last_online=excluded.last_online,
                last_scanned_utc=excluded.last_scanned_utc
        )SQL", {rpath, skind, online ? 1 : 0, ts});
    };

    auto add_cand = [&](const fs::path& cp, const std::string& source_kind, bool persist_root) {
        if (cp.empty()) return;
        std::error_code ec;
        const bool exists_dir = fs::exists(native_path(cp), ec) && !ec && fs::is_directory(native_path(cp), ec);
        std::string disp;
        fs::path resolved_cp = cp;
        if (exists_dir) {
            resolved_cp = canonical_win32_path(cp);
            if (resolved_cp.empty()) resolved_cp = cp.lexically_normal();
            disp = canonical_path_str(resolved_cp);
        } else {
            resolved_cp = cp.lexically_normal();
            disp = path_text(resolved_cp);
            while (disp.size() > 3 && (disp.back() == '/' || disp.back() == '\\')) {
                disp.pop_back();
            }
        }
        if (disp.empty()) return;
        if (persist_root) {
            persist_search_root(disp, source_kind, exists_dir);
        }
        const std::string k = lower_ascii(disp);
        if (seen_keys.insert(k).second) {
            candidates.push_back(CandidateItem{resolved_cp, disp, exists_dir});
        }
    };

    add_cand(p.directory(), "main_project", false);

    if (args.contains("candidate_paths") && args["candidate_paths"].is_array()) {
        for (const auto& c : args["candidate_paths"]) {
            if (c.is_string()) {
                const std::string raw_s = trim_copy(c.get<std::string>());
                if (raw_s.empty()) continue;
                fs::path fp(wide(raw_s));
                if (fp.is_relative()) fp = p.directory() / fp;
                add_cand(fp, "candidate_path", true);
            }
        }
    }
    if (args.contains("search_roots") && args["search_roots"].is_array()) {
        for (const auto& sr : args["search_roots"]) {
            if (!sr.is_string()) continue;
            const std::string raw_s = trim_copy(sr.get<std::string>());
            if (raw_s.empty()) continue;
            fs::path sr_p(wide(raw_s));
            if (sr_p.is_relative()) sr_p = p.directory() / sr_p;
            std::error_code ec;
            if (!fs::exists(native_path(sr_p), ec) || !fs::is_directory(native_path(sr_p), ec)) {
                add_cand(sr_p, "search_root", true);
                continue;
            }
            persist_search_root(canonical_path_str(sr_p), "search_root", true);
            add_cand(sr_p, "search_root", false);
            for (const auto& child : fs::directory_iterator(native_path(sr_p), ec)) {
                if (child.is_directory(ec)) {
                    add_cand(child.path(), "search_root_child", false);
                }
            }
        }
    }

    Json items = Json::array();
    for (const auto& cand_item : candidates) {
        const fs::path& cand = cand_item.path;
        const std::string& cand_str = cand_item.display_path;
        const std::string cand_key = lower_ascii(cand_str);
        const std::string rep_id = "rep-" + sha256(cand_key).substr(0, 12);

        std::string classification = "UNRELATED";
        std::string c_root;
        std::string c_head;
        std::string c_branch;
        std::string c_remote;
        std::string merge_base_commit;
        bool is_ancestor_of_main = false;
        int64_t shared_hashes = 0;
        int64_t unique_or_mod_count = 0;
        Json unique_sample = Json::array();
        int64_t disk_bytes = 0;

        if (!cand_item.is_online) {
            classification = "OFFLINE_DRIVE";
            const auto prev = db.query(
                "SELECT * FROM discovered_project_replicas WHERE replica_id=? OR lower(candidate_path)=lower(?) LIMIT 1",
                {rep_id, cand_str});
            if (!prev.empty()) {
                c_root = prev[0].value("git_root_commit", std::string{});
                c_head = prev[0].value("git_head_commit", std::string{});
                c_branch = prev[0].value("git_branch", std::string{});
                c_remote = prev[0].value("git_remote_url", std::string{});
                merge_base_commit = prev[0].value("merge_base_commit", std::string{});
                is_ancestor_of_main = (prev[0].value("is_ancestor_of_main", int64_t{0}) != 0);
                shared_hashes = prev[0].value("shared_file_hashes_count", int64_t{0});
                unique_or_mod_count = prev[0].value("unique_or_modified_files_count", int64_t{0});
                unique_sample = parse_json_safe(prev[0].value("unique_files_sample_json", std::string{"[]"}), Json::array());
                disk_bytes = prev[0].value("disk_bytes", int64_t{0});
            }
        } else {
            disk_bytes = compute_bounded_dir_bytes(cand, 1200);
            std::error_code ec;
            const fs::path dot_git = cand / L".git";
            const bool has_git_file = fs::is_regular_file(native_path(dot_git), ec);
            const bool has_git_dir = fs::is_directory(native_path(dot_git), ec);

            if (cand_key == main_key) {
                classification = "MAIN_PROJECT";
                c_root = ref.git_root_commit;
                c_head = ref.git_head_commit;
                c_branch = ref.git_branch;
                c_remote = ref.git_remote_url;
                merge_base_commit = ref.git_head_commit;
                is_ancestor_of_main = !ref.git_head_commit.empty();
                shared_hashes = static_cast<int64_t>(ref.rel_to_sha256.size());
            } else {
                if (has_git_file || has_git_dir) {
                    const auto r1 = run_git_isolated(cand, {"rev-list", "--max-parents=0", "HEAD"}, 4000);
                    if (r1.exit_code == 0) c_root = first_line_trimmed(r1.output);
                    const auto r2 = run_git_isolated(cand, {"rev-parse", "HEAD"}, 4000);
                    if (r2.exit_code == 0) c_head = first_line_trimmed(r2.output);
                    const auto r3 = run_git_isolated(cand, {"rev-parse", "--abbrev-ref", "HEAD"}, 4000);
                    if (r3.exit_code == 0) c_branch = first_line_trimmed(r3.output);
                    const auto r4 = run_git_isolated(cand, {"config", "--get", "remote.origin.url"}, 4000);
                    if (r4.exit_code == 0) c_remote = first_line_trimmed(r4.output);
                }

                std::unordered_map<std::string, std::string> cand_rel_to_sha;
                std::unordered_set<std::string> cand_all_sha;
                collect_dir_source_hashes(cand, cand_rel_to_sha, cand_all_sha);

                int64_t matching_rel_paths = 0;
                for (const auto& [rel, sha] : cand_rel_to_sha) {
                    if (ref.all_sha256s.contains(sha)) {
                        ++shared_hashes;
                    }
                    const auto mit = ref.rel_to_sha256.find(rel);
                    if (mit != ref.rel_to_sha256.end()) {
                        ++matching_rel_paths;
                        if (mit->second != sha) {
                            ++unique_or_mod_count;
                            if (unique_sample.size() < 25) {
                                unique_sample.push_back({{"relative_path", rel}, {"status", "MODIFIED"}, {"sha256", sha}});
                            }
                        }
                    } else {
                        ++unique_or_mod_count;
                        if (unique_sample.size() < 25) {
                            unique_sample.push_back({{"relative_path", rel}, {"status", "UNIQUE_NEW"}, {"sha256", sha}});
                        }
                    }
                }

                if (ref.linked_worktree_keys.contains(cand_key) || has_git_file) {
                    classification = "RELATED_WORKTREE";
                } else if ((has_git_dir || !c_root.empty()) &&
                           ((!ref.git_root_commit.empty() && c_root == ref.git_root_commit) ||
                            (!ref.git_remote_url.empty() && c_remote == ref.git_remote_url))) {
                    classification = "POSSIBLE_CLONE";
                } else if (shared_hashes >= 1 || matching_rel_paths >= 2) {
                    classification = "ORPHAN_WORK";
                } else {
                    classification = "UNRELATED";
                    unique_or_mod_count = 0;
                    unique_sample = Json::array();
                }

                if ((classification == "RELATED_WORKTREE" || classification == "POSSIBLE_CLONE") &&
                    !ref.git_head_commit.empty() && !c_head.empty()) {
                    const auto mb_res = run_git_isolated(p.directory(), {"merge-base", ref.git_head_commit, c_head}, 4000);
                    if (mb_res.exit_code == 0) {
                        merge_base_commit = first_line_trimmed(mb_res.output);
                        is_ancestor_of_main = (!merge_base_commit.empty() && lower_ascii(merge_base_commit) == lower_ascii(c_head));
                    }
                }
            }
        }

        db.exec(R"SQL(
            INSERT INTO discovered_project_replicas (
                replica_id, candidate_path, classification, git_root_commit, git_head_commit,
                git_branch, git_remote_url, shared_file_hashes_count, unique_or_modified_files_count,
                unique_files_sample_json, disk_bytes, discovered_at_utc, merge_base_commit, is_ancestor_of_main
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            ON CONFLICT(replica_id) DO UPDATE SET
                candidate_path=excluded.candidate_path,
                classification=excluded.classification,
                git_root_commit=excluded.git_root_commit,
                git_head_commit=excluded.git_head_commit,
                git_branch=excluded.git_branch,
                git_remote_url=excluded.git_remote_url,
                shared_file_hashes_count=excluded.shared_file_hashes_count,
                unique_or_modified_files_count=excluded.unique_or_modified_files_count,
                unique_files_sample_json=excluded.unique_files_sample_json,
                disk_bytes=excluded.disk_bytes,
                discovered_at_utc=excluded.discovered_at_utc,
                merge_base_commit=excluded.merge_base_commit,
                is_ancestor_of_main=excluded.is_ancestor_of_main
        )SQL", {
            rep_id, cand_str, classification, c_root, c_head,
            c_branch, c_remote, shared_hashes, unique_or_mod_count,
            unique_sample.dump(), disk_bytes, ts, merge_base_commit, is_ancestor_of_main ? 1 : 0
        });

        items.push_back({
            {"replica_id", rep_id},
            {"candidate_path", cand_str},
            {"replica_path", cand_str},
            {"path", cand_str},
            {"classification", classification},
            {"online", cand_item.is_online},
            {"git_root_commit", c_root},
            {"git_head_commit", c_head},
            {"git_branch", c_branch},
            {"git_remote_url", c_remote},
            {"merge_base_commit", merge_base_commit},
            {"is_ancestor_of_main", is_ancestor_of_main},
            {"shared_file_hashes_count", shared_hashes},
            {"unique_or_modified_files_count", unique_or_mod_count},
            {"unique_files_sample", unique_sample},
            {"disk_bytes", disk_bytes},
            {"discovered_at_utc", ts}
        });
    }

    return {
        {"schema", "migratool.discover_replicas/1"},
        {"ok", true},
        {"action", action},
        {"total_discovered", items.size()},
        {"replicas", items}
    };
}

namespace {

std::string file_mtime_utc_iso(const fs::path& fp) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(native_path(fp).c_str(), GetFileExInfoStandard, &fad)) {
        return {};
    }
    SYSTEMTIME st{};
    if (!FileTimeToSystemTime(&fad.ftLastWriteTime, &st)) {
        return {};
    }
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
                  static_cast<unsigned>(st.wYear), static_cast<unsigned>(st.wMonth), static_cast<unsigned>(st.wDay),
                  static_cast<unsigned>(st.wHour), static_cast<unsigned>(st.wMinute), static_cast<unsigned>(st.wSecond),
                  static_cast<unsigned>(st.wMilliseconds));
    return std::string(buf);
}

fs::path find_master_prd_json(const Project& p, const Json& args, fs::path* out_repo_root = nullptr) {
    std::error_code ec;
    if (args.contains("prd_path") && args["prd_path"].is_string()) {
        fs::path custom(wide(args["prd_path"].get<std::string>()));
        if (custom.is_relative()) custom = p.directory() / custom;
        if (fs::is_regular_file(native_path(custom), ec)) {
            if (out_repo_root) *out_repo_root = custom.parent_path().parent_path();
            return custom;
        }
    }

    const fs::path local_prd = p.directory() / L"docs" / L"PRODUCT_REQUIREMENTS_MASTER.json";
    if (fs::is_regular_file(native_path(local_prd), ec)) {
        if (out_repo_root) *out_repo_root = p.directory();
        return local_prd;
    }

    fs::path cur = executable_dir();
    for (int depth = 0; depth < 5 && !cur.empty(); ++depth) {
        const fs::path cand = cur / L"docs" / L"PRODUCT_REQUIREMENTS_MASTER.json";
        if (fs::is_regular_file(native_path(cand), ec)) {
            if (out_repo_root) *out_repo_root = cur;
            return cand;
        }
        const fs::path parent = cur.parent_path();
        if (parent == cur) break;
        cur = parent;
    }
    return {};
}

void sync_prd_into_sqlite(const Project& p, Database& db, const Json& args) {
    fs::path prd_repo_root = p.directory();
    const fs::path prd_file = find_master_prd_json(p, args, &prd_repo_root);
    if (prd_file.empty()) return;

    Json prd_doc;
    try {
        prd_doc = read_json(prd_file);
    } catch (...) {
        return;
    }
    if (!prd_doc.is_object() || !prd_doc.contains("requirements") || !prd_doc["requirements"].is_array()) {
        return;
    }

    const std::string ts = now();
    std::string repo_head_commit;
    const auto head_res = run_git_isolated(p.directory(), {"rev-parse", "HEAD"}, 3000);
    if (head_res.exit_code == 0) repo_head_commit = first_line_trimmed(head_res.output);

    std::string latest_build_id;
    Json included_builds = Json::array();
    const auto b_rows = db.query("SELECT build_id FROM build_manifests ORDER BY created_at_utc DESC");
    for (const auto& br : b_rows) {
        const std::string bid = br.value("build_id", std::string{});
        if (!bid.empty()) {
            if (latest_build_id.empty()) latest_build_id = bid;
            included_builds.push_back(bid);
        }
    }
    const std::string included_builds_str = included_builds.dump();

    Json vruns = Json::array();
    if (table_exists(db, "verified_runs")) {
        try {
            vruns = db.query("SELECT * FROM verified_runs ORDER BY rowid DESC LIMIT 100");
        } catch (...) {}
    }

    std::unordered_map<std::string, std::string> file_commit_cache;

    Statement ins_req(db, R"SQL(
        INSERT INTO product_requirements (
            req_id, title, category, priority, status, summary,
            invariants_json, implementation_files_json, test_suites_json,
            acceptance_criteria_json, updated_at_utc
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(req_id) DO UPDATE SET
            title=excluded.title,
            category=excluded.category,
            priority=excluded.priority,
            status=excluded.status,
            summary=excluded.summary,
            invariants_json=excluded.invariants_json,
            implementation_files_json=excluded.implementation_files_json,
            test_suites_json=excluded.test_suites_json,
            acceptance_criteria_json=excluded.acceptance_criteria_json,
            updated_at_utc=excluded.updated_at_utc
    )SQL");

    Statement ins_link(db, R"SQL(
        INSERT INTO requirement_trace_links (
            req_id, file_path, file_exists, file_sha256, test_suite, git_commit, build_id, verified_at_utc,
            last_test_status, last_test_run_id, included_builds_json, post_change_status
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(req_id, file_path) DO UPDATE SET
            file_exists=excluded.file_exists,
            file_sha256=excluded.file_sha256,
            test_suite=excluded.test_suite,
            git_commit=excluded.git_commit,
            build_id=excluded.build_id,
            verified_at_utc=excluded.verified_at_utc,
            last_test_status=excluded.last_test_status,
            last_test_run_id=excluded.last_test_run_id,
            included_builds_json=excluded.included_builds_json,
            post_change_status=excluded.post_change_status
    )SQL");

    std::error_code ec;
    for (const auto& req : prd_doc["requirements"]) {
        if (!req.is_object()) continue;
        const std::string req_id = req.value("req_id", std::string{});
        if (req_id.empty()) continue;

        const Json impl_files = req.value("implementation_files", Json::array());
        const Json test_suites = req.value("test_suites", Json::array());
        const std::string primary_suite = (!test_suites.empty() && test_suites[0].is_string())
            ? test_suites[0].get<std::string>()
            : std::string{};
        const std::string primary_suite_low = lower_ascii(primary_suite);

        Json matched_run = Json::object();
        if (!vruns.empty()) {
            for (const auto& vr : vruns) {
                if (primary_suite_low.empty() || lower_ascii(vr.dump()).find(primary_suite_low) != std::string::npos) {
                    matched_run = vr;
                    break;
                }
            }
        }

        ins_req.exec({
            req_id,
            req.value("title", std::string{}),
            req.value("category", std::string{}),
            req.value("priority", std::string{"P0"}),
            req.value("status", std::string{"CONFIRMED"}),
            req.value("summary", std::string{}),
            req.value("invariants", Json::array()).dump(),
            impl_files.dump(),
            test_suites.dump(),
            req.value("acceptance_criteria", Json::array()).dump(),
            ts
        });

        for (const auto& fval : impl_files) {
            if (!fval.is_string()) continue;
            const std::string rel_file = fval.get<std::string>();
            fs::path resolved = p.directory() / fs::path(wide(rel_file));
            const bool exists_in_proj = fs::is_regular_file(native_path(resolved), ec);
            bool exists = exists_in_proj;
            if (!exists && !prd_repo_root.empty()) {
                const fs::path alt = prd_repo_root / fs::path(wide(rel_file));
                if (fs::is_regular_file(native_path(alt), ec)) {
                    resolved = alt;
                    exists = true;
                }
            }
            std::string f_sha;
            std::string file_mtime_iso;
            if (exists) {
                const auto fsz = fs::file_size(native_path(resolved), ec);
                if (!ec && fsz < 8 * 1024 * 1024) {
                    try { f_sha = hash_file(resolved); } catch (...) {}
                }
                file_mtime_iso = file_mtime_utc_iso(resolved);
            }

            std::string file_last_commit = repo_head_commit;
            if (exists_in_proj && !repo_head_commit.empty()) {
                const auto cit = file_commit_cache.find(rel_file);
                if (cit != file_commit_cache.end()) {
                    file_last_commit = cit->second;
                } else {
                    const auto lres = run_git_isolated(p.directory(), {"log", "-n", "1", "--format=%H", "--", rel_file}, 2000);
                    const std::string fc = (lres.exit_code == 0) ? first_line_trimmed(lres.output) : std::string{};
                    file_last_commit = !fc.empty() ? fc : repo_head_commit;
                    file_commit_cache[rel_file] = file_last_commit;
                }
            }

            std::string last_test_status = "NO_TEST_EVIDENCE";
            std::string last_test_run_id;
            std::string post_change_status = "NO_TEST_EVIDENCE";

            if (!matched_run.empty()) {
                last_test_run_id = matched_run.value("run_id", matched_run.value("id", std::string{}));
                const std::string run_ts = matched_run.value(
                    "created_at", matched_run.value("at_iso", matched_run.value("created_at_utc", std::string{})));
                const int64_t passed_cnt = matched_run.value("passed_count", int64_t{0});
                bool run_ok = false;
                if (matched_run.contains("ok")) {
                    run_ok = json_bool_or_int(matched_run, "ok", false);
                } else if (matched_run.contains("status")) {
                    const std::string st = upper_ascii(matched_run.value("status", std::string{}));
                    run_ok = (st == "PASSED" || st == "PASS" || st == "OK");
                } else if (matched_run.contains("exit_code")) {
                    run_ok = (matched_run.value("exit_code", int64_t{-1}) == 0);
                }

                if (run_ok && passed_cnt > 0) {
                    last_test_status = "PASSED";
                    if (!file_mtime_iso.empty() && !run_ts.empty() && file_mtime_iso > run_ts) {
                        post_change_status = "STALE_AFTER_EDIT";
                    } else {
                        post_change_status = "VERIFIED_CURRENT";
                    }
                } else {
                    last_test_status = "FAILED";
                    post_change_status = "FAILED";
                }
            }

            ins_link.exec({
                req_id, rel_file, exists ? 1 : 0, f_sha, primary_suite, file_last_commit, latest_build_id, ts,
                last_test_status, last_test_run_id, included_builds_str, post_change_status
            });
        }
    }
}

} // namespace

Json tool_project_health(const Project& p, Database& db, const Json& args) {
    ensure_multiagent_coordinator_schema(db);
    const std::string action = lower_ascii(args.value("action", "summary"));
    const std::string ts = now();

    if (action == "pendings_upsert") {
        std::string pending_id = trim_copy(args.value("pending_id", std::string{}));
        if (pending_id.empty()) {
            pending_id = "pend-" + unique_id().substr(0, 12);
        }
        const auto ex = db.query("SELECT * FROM session_pendings WHERE pending_id=?", {pending_id});
        const std::string session_name = args.value(
            "session_name", !ex.empty() ? ex[0].value("session_name", std::string{}) : std::string{"default"});
        const std::string owner_agent = args.value(
            "owner_agent", !ex.empty() ? ex[0].value("owner_agent", std::string{}) : std::string{"agent"});
        const std::string title = args.value(
            "title", !ex.empty() ? ex[0].value("title", std::string{}) : std::string{});
        const std::string details = args.value(
            "details", !ex.empty() ? ex[0].value("details", std::string{}) : std::string{});
        const std::string linked_req_id = args.value(
            "linked_req_id", args.value("req_id", !ex.empty() ? ex[0].value("linked_req_id", std::string{}) : std::string{}));
        const std::string status = upper_ascii(args.value(
            "status", !ex.empty() ? ex[0].value("status", std::string{"OPEN"}) : std::string{"OPEN"}));
        const std::string created_at_utc = !ex.empty() ? ex[0].value("created_at_utc", ts) : ts;
        const std::string closed_at_utc = (status == "CLOSED" || status == "DONE" || status == "RESOLVED")
            ? ts
            : (!ex.empty() ? ex[0].value("closed_at_utc", std::string{}) : std::string{});

        db.exec(R"SQL(
            INSERT INTO session_pendings (
                pending_id, session_name, owner_agent, title, details,
                linked_req_id, status, created_at_utc, closed_at_utc
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
            ON CONFLICT(pending_id) DO UPDATE SET
                session_name=excluded.session_name,
                owner_agent=excluded.owner_agent,
                title=excluded.title,
                details=excluded.details,
                linked_req_id=excluded.linked_req_id,
                status=excluded.status,
                closed_at_utc=excluded.closed_at_utc
        )SQL", {
            pending_id, session_name, owner_agent, title, details,
            linked_req_id, status, created_at_utc, closed_at_utc
        });

        const auto rows = db.query("SELECT * FROM session_pendings WHERE pending_id=?", {pending_id});
        return {
            {"schema", "migratool.project_health/1"},
            {"action", "pendings_upsert"},
            {"ok", true},
            {"pending_id", pending_id},
            {"pending", rows.empty() ? Json::object() : rows[0]},
            {"master_requirements_untouched", true}
        };
    }

    if (action == "pendings_list") {
        const std::string status_f = upper_ascii(trim_copy(args.value("status", std::string{})));
        const std::string session_f = trim_copy(args.value("session_name", std::string{}));
        Json rows = Json::array();
        if (!status_f.empty() && !session_f.empty()) {
            rows = db.query("SELECT * FROM session_pendings WHERE status=? AND session_name=? ORDER BY created_at_utc DESC",
                            {status_f, session_f});
        } else if (!status_f.empty()) {
            rows = db.query("SELECT * FROM session_pendings WHERE status=? ORDER BY created_at_utc DESC", {status_f});
        } else if (!session_f.empty()) {
            rows = db.query("SELECT * FROM session_pendings WHERE session_name=? ORDER BY created_at_utc DESC", {session_f});
        } else {
            rows = db.query("SELECT * FROM session_pendings ORDER BY created_at_utc DESC");
        }
        return {
            {"schema", "migratool.project_health/1"},
            {"action", "pendings_list"},
            {"ok", true},
            {"total_pendings", rows.size()},
            {"open_count", db.integer("SELECT COUNT(*) FROM session_pendings WHERE status='OPEN'")},
            {"pendings", rows}
        };
    }

    if (action == "pendings_close") {
        const std::string pending_id = trim_copy(args.value("pending_id", std::string{}));
        if (pending_id.empty()) {
            throw std::runtime_error("PENDINGS_CLOSE_MISSING_ID: provide pending_id");
        }
        db.exec("UPDATE session_pendings SET status='CLOSED', closed_at_utc=? WHERE pending_id=?", {ts, pending_id});
        const auto rows = db.query("SELECT * FROM session_pendings WHERE pending_id=?", {pending_id});
        return {
            {"schema", "migratool.project_health/1"},
            {"action", "pendings_close"},
            {"ok", !rows.empty()},
            {"pending_id", pending_id},
            {"status", "CLOSED"},
            {"pending", rows.empty() ? Json::object() : rows[0]},
            {"master_requirements_count", db.integer("SELECT COUNT(*) FROM product_requirements")},
            {"note", "Cerrar un pendiente de sesión nunca elimina ni altera requisitos permanentes en product_requirements."}
        };
    }

    if (action == "sync_prd" || db.integer("SELECT COUNT(*) FROM product_requirements") == 0) {
        sync_prd_into_sqlite(p, db, args);
    }

    if (action == "query_requirements" || action == "trace") {
        const std::string req_id_f = lower_ascii(trim_copy(args.value("req_id", std::string{})));
        const std::string cat_f = lower_ascii(trim_copy(args.value("category", std::string{})));
        std::string file_f = lower_ascii(trim_copy(args.value("file_path", args.value("file", std::string{}))));
        for (char& c : file_f) {
            if (c == '\\') c = '/';
        }
        const std::string query_f = lower_ascii(trim_copy(args.value("query", std::string{})));
        const size_t limit = static_cast<size_t>(std::clamp<int64_t>(args.value("limit", int64_t{10}), 1, 100));

        const auto all_reqs = db.query("SELECT * FROM product_requirements ORDER BY priority ASC, req_id ASC");
        Json matched = Json::array();
        for (const auto& r : all_reqs) {
            if (matched.size() >= limit) break;
            const std::string rid = r.value("req_id", std::string{});
            const std::string cat = r.value("category", std::string{});
            const std::string title = r.value("title", std::string{});
            const std::string summary = r.value("summary", std::string{});
            const std::string impl_raw = r.value("implementation_files_json", std::string{"[]"});

            if (!req_id_f.empty() && lower_ascii(rid) != req_id_f) continue;
            if (!cat_f.empty() && lower_ascii(cat) != cat_f) continue;
            if (!file_f.empty() && lower_ascii(impl_raw).find(file_f) == std::string::npos) continue;
            if (!query_f.empty()) {
                const std::string hay = lower_ascii(rid + " " + cat + " " + title + " " + summary + " " + impl_raw);
                if (hay.find(query_f) == std::string::npos) continue;
            }

            const auto raw_links = db.query("SELECT * FROM requirement_trace_links WHERE req_id=? ORDER BY file_path ASC", {rid});
            Json trace_links = Json::array();
            for (const auto& rl : raw_links) {
                Json link = rl;
                link["included_builds"] = parse_json_safe(rl.value("included_builds_json", std::string{"[]"}), Json::array());
                trace_links.push_back(std::move(link));
            }

            Json item = {
                {"req_id", rid},
                {"title", title},
                {"category", cat},
                {"priority", r.value("priority", std::string{"P0"})},
                {"status", r.value("status", std::string{"CONFIRMED"})},
                {"summary", summary},
                {"invariants", parse_json_safe(r.value("invariants_json", std::string{"[]"}), Json::array())},
                {"implementation_files", parse_json_safe(impl_raw, Json::array())},
                {"test_suites", parse_json_safe(r.value("test_suites_json", std::string{"[]"}), Json::array())},
                {"acceptance_criteria", parse_json_safe(r.value("acceptance_criteria_json", std::string{"[]"}), Json::array())},
                {"trace_links", trace_links}
            };
            matched.push_back(std::move(item));
        }

        return {
            {"schema", "migratool.project_health/1"},
            {"action", action},
            {"ok", true},
            {"returned_count", matched.size()},
            {"requirements", matched}
        };
    }

    // Executive summary ("summary" | "health" | "sync_prd")
    const Json integ = tool_integrate_ready(p, db, {{"action", "evaluate"}});
    const Json storage = tool_storage_audit(p, db, {{"action", "audit"}});

    const size_t pending_integrations_count = integ.value("unintegrated_mandatory_count", size_t{0});
    const int64_t worktrees_with_pending_changes_count = db.integer(
        "SELECT COUNT(*) FROM agent_worktrees_catalog WHERE uncommitted_count > 0 OR ahead_count > 0");
    const int64_t old_manifest_builds = db.integer(
        "SELECT COUNT(*) FROM build_manifests WHERE conservation_tier = 'VERIFIED_SAFE_CLEAN'");
    const int64_t safe_clean_dirs = storage["counts_by_tier"].value("VERIFIED_SAFE_CLEAN", int64_t{0});
    const int64_t old_builds_count = old_manifest_builds + safe_clean_dirs;
    const int64_t reclaimable_storage_bytes = storage.value("reclaimable_bytes", int64_t{0});

    Json last_canonical_build = nullptr;
    const auto canon_rows = db.query(
        "SELECT * FROM build_manifests WHERE lifecycle_state IN ('INTEGRATED', 'DELIVERED') "
        "ORDER BY created_at_utc DESC LIMIT 1");
    if (!canon_rows.empty()) {
        last_canonical_build = canon_rows[0];
        last_canonical_build["tree_clean"] = (canon_rows[0].value("tree_clean", int64_t{1}) != 0);
    }

    const bool missing_completed_features_warning = (pending_integrations_count > 0);
    std::string warning_message;
    if (missing_completed_features_warning) {
        warning_message = "Atención: existen " + std::to_string(pending_integrations_count) +
                          " worktrees obligatorios con avances en estado IMPLEMENTED o CONFIRMED que aún no están integrados en el último build canónico.";
    } else if (last_canonical_build.is_null()) {
        warning_message = "Aún no se ha registrado ningún build canónico en estado INTEGRATED o DELIVERED.";
    } else {
        warning_message = "Integración al día: el último build canónico incluye todos los worktrees obligatorios.";
    }

    const int64_t total_reqs = db.integer("SELECT COUNT(*) FROM product_requirements");
    const int64_t total_links = db.integer("SELECT COUNT(*) FROM requirement_trace_links");
    const int64_t verified_links = db.integer("SELECT COUNT(*) FROM requirement_trace_links WHERE file_exists = 1");
    const double coverage_pct = (total_links > 0)
        ? (100.0 * static_cast<double>(verified_links) / static_cast<double>(total_links))
        : 0.0;

    const int64_t session_pendings_open_count = db.integer("SELECT COUNT(*) FROM session_pendings WHERE status='OPEN'");
    const Json session_pendings_summary = {
        {"open_count", session_pendings_open_count},
        {"closed_count", db.integer("SELECT COUNT(*) FROM session_pendings WHERE status!='OPEN'")},
        {"total_count", db.integer("SELECT COUNT(*) FROM session_pendings")}
    };

    Json code_audit_summary = nullptr;
    if (table_exists(db, "code_audit_snapshots")) {
        try {
            const auto ca_rows = db.query("SELECT * FROM code_audit_snapshots ORDER BY rowid DESC LIMIT 1");
            if (!ca_rows.empty()) {
                code_audit_summary = ca_rows[0];
            }
        } catch (...) {}
    }
    if (code_audit_summary.is_null() && table_exists(db, "code_audit_findings")) {
        try {
            code_audit_summary = {
                {"total_findings", db.integer("SELECT COUNT(*) FROM code_audit_findings")},
                {"confirmed_findings", db.integer("SELECT COUNT(*) FROM code_audit_findings WHERE triage_status='CONFIRMED'")},
                {"pending_validation_findings", db.integer("SELECT COUNT(*) FROM code_audit_findings WHERE triage_status='PENDING_VALIDATION'")}
            };
        } catch (...) {}
    }

    return {
        {"schema", "migratool.project_health/1"},
        {"action", action},
        {"ok", true},
        {"requirements_indexed", total_reqs},
        {"count", total_reqs},
        {"pending_integrations_count", pending_integrations_count},
        {"worktrees_with_pending_changes_count", worktrees_with_pending_changes_count},
        {"old_builds_count", old_builds_count},
        {"reclaimable_storage_bytes", reclaimable_storage_bytes},
        {"last_canonical_build", last_canonical_build},
        {"missing_completed_features_warning", missing_completed_features_warning},
        {"warning_message", warning_message},
        {"canonical_build_ready", integ.value("canonical_build_ready", true)},
        {"unintegrated_mandatory_worktrees", integ.value("unintegrated_mandatory_worktrees", Json::array())},
        {"session_pendings_open_count", session_pendings_open_count},
        {"session_pendings_summary", session_pendings_summary},
        {"code_audit_summary", code_audit_summary},
        {"requirements_coverage", {
            {"total_requirements", total_reqs},
            {"traced_files_count", total_links},
            {"verified_links_count", verified_links},
            {"coverage_pct", coverage_pct}
        }}
    };
}

} // namespace mt
