#include "migratool/git_intel.hpp"
#include "migratool/env_registry.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace mt {
namespace {

struct HandleCloser {
    HANDLE h = INVALID_HANDLE_VALUE;
    explicit HandleCloser(HANDLE v = INVALID_HANDLE_VALUE) : h(v) {}
    ~HandleCloser() {
        if (h != INVALID_HANDLE_VALUE && h != nullptr) CloseHandle(h);
    }
};

struct GitCmdResult {
    int exit_code = -1;
    std::string stdout_text;
};

GitCmdResult run_git_pipe(const fs::path& repo_dir, const std::vector<std::string>& args, DWORD timeout_ms = 12000) {
    GitCmdResult res;
    wchar_t dis[16]{};
    if (GetEnvironmentVariableW(L"MIGRATOOL_DISABLE_GIT", dis, 16) > 0 && std::wstring_view(dis) == L"1") {
        res.stdout_text = "git disabled by MIGRATOOL_DISABLE_GIT";
        return res;
    }
    const fs::path git = resolve_git_executable();
    if (git.empty()) return res;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE rpipe = nullptr, wpipe = nullptr;
    if (!CreatePipe(&rpipe, &wpipe, &sa, 0)) return res;
    HandleCloser rg(rpipe), wg(wpipe);
    SetHandleInformation(rpipe, HANDLE_FLAG_INHERIT, 0);

    std::wstring cmd = quote_arg(git.wstring()) + L" -C " + quote_arg(repo_dir.wstring());
    for (const auto& a : args) cmd += L" " + quote_arg(wide(a));

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
        return res;
    }
    CloseHandle(wpipe);
    wg.h = INVALID_HANDLE_VALUE;
    HandleCloser ph(pi.hProcess), th(pi.hThread);

    std::string out;
    std::array<char, 8192> buf{};
    DWORD got = 0;
    while (ReadFile(rpipe, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr) && got > 0) {
        out.append(buf.data(), got);
        if (out.size() > 16 * 1024 * 1024) break;
    }
    if (WaitForSingleObject(pi.hProcess, timeout_ms) == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        return res;
    }
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    res.exit_code = static_cast<int>(code);
    res.stdout_text = std::move(out);
    return res;
}

std::string trim_copy(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
    return s.substr(i);
}

std::vector<std::string> split_lines_clean(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream iss(text);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) out.push_back(std::move(line));
    }
    return out;
}

int64_t path_mtime_key(const fs::path& p) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(native_path(p).c_str(), GetFileExInfoStandard, &fad)) return 0;
    const uint64_t ft = (static_cast<uint64_t>(fad.ftLastWriteTime.dwHighDateTime) << 32) | fad.ftLastWriteTime.dwLowDateTime;
    return static_cast<int64_t>(ft);
}

fs::path find_project_repo_dir(const Project& p, const fs::path& explicit_dir) {
    std::error_code ec;
    if (!explicit_dir.empty() && fs::exists(native_path(explicit_dir / L".git"), ec)) {
        return explicit_dir;
    }
    for (const auto& r : p.config.value("roots", Json::array())) {
        const fs::path rp = p.resolve(r.value("path", ""));
        if (fs::exists(native_path(rp / L".git"), ec)) return rp;
        if (rp.has_parent_path() && fs::exists(native_path(rp.parent_path() / L".git"), ec)) return rp.parent_path();
    }
    if (fs::exists(native_path(p.directory() / L".git"), ec)) return p.directory();
    if (!p.config.value("roots", Json::array()).empty()) {
        return p.resolve(p.config["roots"][0].value("path", ""));
    }
    return p.directory();
}

struct CachedGitSummary {
    int64_t head_mtime = 0;
    int64_t index_mtime = 0;
    std::chrono::steady_clock::time_point cached_at{};
    Json summary;
};

std::mutex g_git_cache_mu;
std::unordered_map<std::string, CachedGitSummary> g_git_cache;

} // namespace

fs::path resolve_git_executable() {
    static std::mutex mu;
    static fs::path cached;
    std::lock_guard<std::mutex> lk(mu);
    std::error_code ec;
    if (!cached.empty() && fs::is_regular_file(native_path(cached), ec)) return cached;

    for (const wchar_t* cand : {
             L"D:\\DevTools\\Tools\\Git\\cmd\\git.exe",
             L"C:\\Program Files\\Git\\cmd\\git.exe"}) {
        if (fs::is_regular_file(cand, ec)) {
            cached = fs::path(cand);
            return cached;
        }
    }
    wchar_t buf[32768]{};
    if (SearchPathW(nullptr, L"git.exe", nullptr, 32768, buf, nullptr) > 0) {
        cached = fs::path(buf);
        return cached;
    }
    return {};
}

Json git_repo_summary_cached(const Project& p, Database& /*db*/, const fs::path& repo_dir) {
    const fs::path repo = find_project_repo_dir(p, repo_dir);
    std::error_code ec;
    const fs::path dot_git = repo / L".git";
    if (!fs::exists(native_path(dot_git), ec)) {
        return {
            {"available", false},
            {"is_git_repo", false},
            {"repo_root", path_text(repo)},
            {"reason", "NO_DOT_GIT"}
        };
    }

    const int64_t head_mt = path_mtime_key(dot_git / L"HEAD");
    const int64_t idx_mt = path_mtime_key(dot_git / L"index");
    const std::string key = path_text(repo);
    const auto now_tp = std::chrono::steady_clock::now();

    {
        std::lock_guard<std::mutex> lk(g_git_cache_mu);
        auto it = g_git_cache.find(key);
        if (it != g_git_cache.end() && it->second.head_mtime == head_mt && it->second.index_mtime == idx_mt &&
            std::chrono::duration_cast<std::chrono::milliseconds>(now_tp - it->second.cached_at).count() < 2500) {
            Json hit = it->second.summary;
            hit["cached"] = true;
            return hit;
        }
    }

    const auto head_res = run_git_pipe(repo, {"log", "-n", "1", "--format=%H%n%h%n%cI%n%an%n%s"});
    std::string head_full, head_short, commit_iso, author, subject;
    if (head_res.exit_code == 0) {
        auto lines = split_lines_clean(head_res.stdout_text);
        if (lines.size() >= 1) head_full = lines[0];
        if (lines.size() >= 2) head_short = lines[1];
        if (lines.size() >= 3) commit_iso = lines[2];
        if (lines.size() >= 4) author = lines[3];
        if (lines.size() >= 5) subject = lines[4];
    }

    const auto br_res = run_git_pipe(repo, {"rev-parse", "--abbrev-ref", "HEAD"});
    std::string branch = (br_res.exit_code == 0) ? trim_copy(br_res.stdout_text) : "HEAD";

    const auto st_res = run_git_pipe(repo, {"status", "--porcelain", "-uall"});
    int64_t staged = 0, modified = 0, untracked = 0;
    Json changed_files = Json::array();
    if (st_res.exit_code == 0) {
        for (const auto& line : split_lines_clean(st_res.stdout_text)) {
            if (line.size() < 3) continue;
            const char x = line[0];
            const char y = line[1];
            std::string rel = trim_copy(line.substr(3));
            if (x == '?' && y == '?') {
                ++untracked;
            } else {
                if (x != ' ' && x != '?') ++staged;
                if (y != ' ' && y != '?') ++modified;
            }
            if (changed_files.size() < 40) {
                changed_files.push_back({{"status", line.substr(0, 2)}, {"path", rel}});
            }
        }
    }

    Json summary = {
        {"available", true},
        {"is_git_repo", true},
        {"cached", false},
        {"repo_root", path_text(repo)},
        {"git_exe", path_text(resolve_git_executable())},
        {"branch", branch},
        {"head_commit", head_full},
        {"head_short", head_short},
        {"last_commit_at", commit_iso},
        {"last_commit_author", author},
        {"last_commit_subject", subject},
        {"dirty", (staged + modified + untracked) > 0},
        {"staged_count", staged},
        {"modified_count", modified},
        {"untracked_count", untracked},
        {"changed_files_preview", std::move(changed_files)}
    };

    {
        std::lock_guard<std::mutex> lk(g_git_cache_mu);
        g_git_cache[key] = CachedGitSummary{head_mt, idx_mt, now_tp, summary};
    }
    return summary;
}

Json tool_git_intel(const Project& p, Database& db, const Json& args) {
    const std::string action = args.value("action", args.value("op", "summary"));
    const fs::path explicit_repo = args.contains("repo_path") ? p.resolve(args["repo_path"].get<std::string>()) : fs::path{};
    const fs::path repo = find_project_repo_dir(p, explicit_repo);
    const int token_budget = std::clamp(args.value("token_budget", 2000), 200, 32000);
    const size_t max_chars = static_cast<size_t>(token_budget) * 4;

    if (action == "summary" || action == "status") {
        Json sum = git_repo_summary_cached(p, db, repo);
        sum["schema"] = "migratool.git_intel/1";
        sum["action"] = action;
        return sum;
    }

    if (action == "repos") {
        Json repos = Json::array();
        std::error_code ec;
        for (const auto& r : p.config.value("roots", Json::array())) {
            const fs::path rp = p.resolve(r.value("path", ""));
            if (fs::exists(native_path(rp / L".git"), ec)) {
                repos.push_back(git_repo_summary_cached(p, db, rp));
            }
        }
        if (repos.empty() && fs::exists(native_path(repo / L".git"), ec)) {
            repos.push_back(git_repo_summary_cached(p, db, repo));
        }
        Json worktrees = Json::array();
        auto wt = run_git_pipe(repo, {"worktree", "list", "--porcelain"});
        if (wt.exit_code == 0) {
            Json cur = Json::object();
            for (const auto& ln : split_lines_clean(wt.stdout_text)) {
                if (ln.starts_with("worktree ")) {
                    if (!cur.empty()) worktrees.push_back(cur);
                    cur = {{"path", ln.substr(9)}};
                } else if (ln.starts_with("HEAD ")) {
                    cur["head"] = ln.substr(5);
                } else if (ln.starts_with("branch ")) {
                    cur["branch"] = ln.substr(7);
                }
            }
            if (!cur.empty()) worktrees.push_back(cur);
        }
        return {
            {"schema", "migratool.git_intel/1"},
            {"action", "repos"},
            {"count", repos.size()},
            {"repos", std::move(repos)},
            {"worktrees", std::move(worktrees)}
        };
    }

    if (action == "log") {
        const int limit = std::clamp(args.value("limit", 15), 1, 100);
        const std::string path_filter = args.value("path", "");
        std::vector<std::string> gargs = {"log", "-n", std::to_string(limit), "--format=%H|%h|%cI|%an|%s"};
        if (!path_filter.empty()) {
            gargs.push_back("--");
            gargs.push_back(path_filter);
        }
        auto res = run_git_pipe(repo, gargs);
        Json commits = Json::array();
        if (res.exit_code == 0) {
            for (const auto& ln : split_lines_clean(res.stdout_text)) {
                size_t p1 = ln.find('|');
                size_t p2 = (p1 == std::string::npos) ? std::string::npos : ln.find('|', p1 + 1);
                size_t p3 = (p2 == std::string::npos) ? std::string::npos : ln.find('|', p2 + 1);
                size_t p4 = (p3 == std::string::npos) ? std::string::npos : ln.find('|', p3 + 1);
                if (p4 != std::string::npos) {
                    commits.push_back({
                        {"commit", ln.substr(0, p1)},
                        {"short", ln.substr(p1 + 1, p2 - p1 - 1)},
                        {"at_iso", ln.substr(p2 + 1, p3 - p2 - 1)},
                        {"author", ln.substr(p3 + 1, p4 - p3 - 1)},
                        {"subject", ln.substr(p4 + 1)}
                    });
                }
            }
        }
        return {
            {"schema", "migratool.git_intel/1"},
            {"action", "log"},
            {"repo_root", path_text(repo)},
            {"count", commits.size()},
            {"commits", std::move(commits)}
        };
    }

    if (action == "diff") {
        const std::string path_filter = args.value("path", "");
        const bool staged_only = args.value("staged", false);
        std::vector<std::string> gargs = {"diff"};
        if (staged_only) gargs.push_back("--cached");
        if (!path_filter.empty()) {
            gargs.push_back("--");
            gargs.push_back(path_filter);
        }
        auto res = run_git_pipe(repo, gargs);
        std::string diff_text = res.stdout_text;
        bool truncated = false;
        if (diff_text.size() > max_chars) {
            diff_text.resize(max_chars);
            diff_text += "\n... [truncated to token_budget]";
            truncated = true;
        }
        return {
            {"schema", "migratool.git_intel/1"},
            {"action", "diff"},
            {"repo_root", path_text(repo)},
            {"truncated", truncated},
            {"diff", std::move(diff_text)}
        };
    }

    if (action == "blame") {
        const std::string path_filter = args.value("path", "");
        if (path_filter.empty()) throw std::runtime_error("git_intel(blame) requiere path");
        const int start_line = std::max(1, args.value("start_line", 1));
        const int end_line = std::max(start_line, args.value("end_line", start_line + 40));
        auto res = run_git_pipe(repo, {"blame", "-L", std::to_string(start_line) + "," + std::to_string(end_line), "--porcelain", "--", path_filter});
        std::string out = res.stdout_text;
        if (out.size() > max_chars) out.resize(max_chars);
        return {
            {"schema", "migratool.git_intel/1"},
            {"action", "blame"},
            {"path", path_filter},
            {"start_line", start_line},
            {"end_line", end_line},
            {"blame", std::move(out)}
        };
    }

    throw std::runtime_error("git_intel action inválida: " + action);
}

} // namespace mt
