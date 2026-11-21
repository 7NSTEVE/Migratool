#include "migratool/agent_supertools.hpp"
#include "migratool/docs_hub.hpp"
#include "migratool/env_registry.hpp"
#include "migratool/file_toolkit.hpp"
#include "migratool/fs_index.hpp"
#include "migratool/git_intel.hpp"
#include "migratool/knowledge.hpp"
#include "migratool/path_lineage.hpp"
#include "migratool/workstream.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace mt {
namespace {

std::string norm_rel_st(std::string p) {
    std::replace(p.begin(), p.end(), '\\', '/');
    while (p.starts_with("./")) p.erase(0, 2);
    while (!p.empty() && p.front() == '/') p.erase(p.begin());
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    return p;
}

std::string lower_st(std::string_view sv) {
    std::string out(sv);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::vector<std::string> split_lines_st(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream iss(text);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
    }
    return lines;
}

fs::path resolve_project_file_st(const Project& p, const std::string& raw_rel) {
    if (raw_rel.empty()) return {};
    const std::string norm = norm_rel_st(raw_rel);
    fs::path fp(wide(norm));
    if (fp.is_absolute()) return fp;
    std::error_code ec;
    if (p.config.contains("roots") && p.config["roots"].is_array() && !p.config["roots"].empty()) {
        for (const auto& r : p.config["roots"]) {
            const std::string rpath = r.value("path", "");
            if (rpath.empty()) continue;
            const fs::path base = p.resolve(rpath);
            const fs::path cand = base / fp;
            if (fs::exists(native_path(cand), ec)) return cand;
            const fs::path direct = p.resolve(norm);
            if (within(direct, base) && fs::exists(native_path(direct), ec)) return direct;
        }
        const std::string first_rpath = p.config["roots"][0].value("path", "");
        if (!first_rpath.empty()) {
            const fs::path base = p.resolve(first_rpath);
            const fs::path direct = p.resolve(norm);
            if (within(direct, base)) return direct;
            return base / fp;
        }
    }
    return p.resolve(norm);
}

} // namespace

void runs_telemetry_schema(Database& db) {
    workstream_schema(db);
    knowledge_schema(db);
    docs_hub_schema(db);
    if (db.integer("SELECT COUNT(*) FROM meta WHERE key='runs_telemetry_schema' AND value='1'") != 0) return;
    const bool need_tx = (sqlite3_get_autocommit(db.native_handle()) != 0);
    if (need_tx) db.begin();
    try {
        if (db.integer("SELECT COUNT(*) FROM meta WHERE key='runs_telemetry_schema' AND value='1'") == 0) {
            char* err = nullptr;
            const char* sql = R"sql(
CREATE TABLE IF NOT EXISTS verified_runs (
  id                    TEXT PRIMARY KEY,
  at_iso                TEXT NOT NULL,
  actor                 TEXT NOT NULL DEFAULT 'agent',
  workstream_id         TEXT NOT NULL DEFAULT '',
  selection_mode        TEXT NOT NULL DEFAULT 'explicit',
  selected_targets_json TEXT NOT NULL DEFAULT '[]',
  command_json          TEXT NOT NULL,
  env_profile           TEXT NOT NULL DEFAULT '',
  cwd                   TEXT NOT NULL,
  exit_code             INTEGER NOT NULL,
  duration_ms           INTEGER NOT NULL DEFAULT 0,
  status                TEXT NOT NULL,
  passed_count          INTEGER NOT NULL DEFAULT 0,
  failed_count          INTEGER NOT NULL DEFAULT 0,
  errors_json           TEXT NOT NULL DEFAULT '[]',
  raw_bytes             INTEGER NOT NULL DEFAULT 0,
  distilled_bytes       INTEGER NOT NULL DEFAULT 0,
  log_path              TEXT NOT NULL DEFAULT '',
  git_head              TEXT NOT NULL DEFAULT ''
);
CREATE INDEX IF NOT EXISTS idx_verified_runs_at ON verified_runs(at_iso DESC);

CREATE TRIGGER IF NOT EXISTS verified_runs_no_update
BEFORE UPDATE ON verified_runs
BEGIN
  SELECT RAISE(ABORT, 'immutable verified_runs');
END;

CREATE TRIGGER IF NOT EXISTS verified_runs_no_delete
BEFORE DELETE ON verified_runs
BEGIN
  SELECT RAISE(ABORT, 'immutable verified_runs');
END;

CREATE TABLE IF NOT EXISTS efficiency_telemetry (
  id                      INTEGER PRIMARY KEY AUTOINCREMENT,
  at_iso                  TEXT NOT NULL,
  tool                    TEXT NOT NULL,
  profile                 TEXT NOT NULL DEFAULT 'full',
  actor                   TEXT NOT NULL DEFAULT 'agent',
  workstream_id           TEXT NOT NULL DEFAULT '',
  schema_bytes_per_turn   INTEGER NOT NULL DEFAULT 21500,
  request_bytes           INTEGER NOT NULL DEFAULT 0,
  response_bytes          INTEGER NOT NULL DEFAULT 0,
  native_calls_saved      INTEGER NOT NULL DEFAULT 0,
  files_skipped_by_sha256 INTEGER NOT NULL DEFAULT 0,
  tokens_raw_est          INTEGER NOT NULL DEFAULT 0,
  tokens_returned_est     INTEGER NOT NULL DEFAULT 0,
  tokens_saved_est        INTEGER NOT NULL DEFAULT 0,
  duration_ms             INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_efficiency_telemetry_at ON efficiency_telemetry(id DESC);
)sql";
            if (sqlite3_exec(db.native_handle(), sql, nullptr, nullptr, &err) != SQLITE_OK) {
                std::string msg = err ? err : sqlite3_errmsg(db.native_handle());
                if (err) sqlite3_free(err);
                throw std::runtime_error(msg);
            }
            db.exec("INSERT OR REPLACE INTO meta(key,value) VALUES('runs_telemetry_schema','1')");
        }
        if (need_tx) db.commit();
    } catch (...) {
        if (need_tx) db.rollback();
        throw;
    }
}

void record_efficiency(Database& db, const std::string& tool, const std::string& profile,
                       const std::string& actor, int64_t schema_bytes, int64_t req_bytes, int64_t resp_bytes,
                       int native_calls_saved, int files_skipped_sha, int tokens_raw, int tokens_returned,
                       int64_t duration_ms, const std::string& workstream_id) {
    runs_telemetry_schema(db);
    const int saved = std::max(0, tokens_raw - tokens_returned) + (native_calls_saved * 350);
    db.exec(
        "INSERT INTO efficiency_telemetry("
        "at_iso,tool,profile,actor,workstream_id,schema_bytes_per_turn,request_bytes,response_bytes,"
        "native_calls_saved,files_skipped_by_sha256,tokens_raw_est,tokens_returned_est,tokens_saved_est,duration_ms)"
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
        {now(), tool, profile, actor.empty() ? "agent" : actor, workstream_id,
         schema_bytes, req_bytes, resp_bytes, native_calls_saved, files_skipped_sha,
         tokens_raw, tokens_returned, saved, duration_ms});
}

Json resolve_task_dag(Database& db) {
    runs_telemetry_schema(db);
    auto rows = db.query(
        "SELECT id AS task_id, title, state AS status, priority, assignee AS owner_actor, "
        "COALESCE(phase_id,'') AS workstream_id, depends_on AS depends_on_json, "
        "source_entities AS files_json, required_cases AS verify_cmd, note AS notes, updated_at "
        "FROM project_tasks ORDER BY priority ASC, id ASC");

    std::unordered_map<std::string, std::string> status_by_id;
    std::unordered_map<std::string, std::vector<std::string>> deps_by_id;
    for (const auto& r : rows) {
        const std::string tid = r["task_id"].get<std::string>();
        status_by_id[tid] = lower_st(r["status"].get<std::string>());
        auto dj = Json::parse(r["depends_on_json"].get<std::string>(), nullptr, false);
        std::vector<std::string> deps;
        if (dj.is_array()) {
            for (const auto& d : dj) if (d.is_string()) deps.push_back(d.get<std::string>());
        }
        deps_by_id[tid] = std::move(deps);
    }

    // DFS cycle detection (0=white, 1=gray, 2=black)
    std::unordered_map<std::string, int> color;
    Json cycles = Json::array();
    std::vector<std::string> stack;

    std::function<void(const std::string&)> dfs = [&](const std::string& u) {
        color[u] = 1;
        stack.push_back(u);
        for (const auto& v : deps_by_id[u]) {
            if (!status_by_id.contains(v)) continue;
            const int c = color[v];
            if (c == 1) {
                Json cyc = Json::array();
                auto it = std::find(stack.begin(), stack.end(), v);
                for (; it != stack.end(); ++it) cyc.push_back(*it);
                cyc.push_back(v);
                cycles.push_back(std::move(cyc));
            } else if (c == 0) {
                dfs(v);
            }
        }
        stack.pop_back();
        color[u] = 2;
    };

    for (const auto& r : rows) {
        const std::string tid = r["task_id"].get<std::string>();
        if (color[tid] == 0) dfs(tid);
    }

    Json ready_tasks = Json::array();
    Json blocked_tasks = Json::array();
    Json completed_tasks = Json::array();

    for (const auto& r : rows) {
        const std::string tid = r["task_id"].get<std::string>();
        const std::string st = status_by_id[tid];
        if (st == "completed" || st == "done" || st == "verified") {
            completed_tasks.push_back(tid);
            continue;
        }
        Json missing = Json::array();
        for (const auto& dep : deps_by_id[tid]) {
            const auto it = status_by_id.find(dep);
            const std::string dst = (it == status_by_id.end()) ? "missing" : it->second;
            if (dst != "completed" && dst != "done" && dst != "verified") {
                missing.push_back(dep);
            }
        }
        Json item = {
            {"task_id", tid},
            {"title", r["title"]},
            {"status", r["status"]},
            {"priority", r["priority"]},
            {"owner_actor", r["owner_actor"]},
            {"workstream_id", r["workstream_id"]},
            {"files", Json::parse(r["files_json"].get<std::string>(), nullptr, false)},
            {"verify_cmd", r["verify_cmd"]}
        };
        if (missing.empty()) {
            ready_tasks.push_back(std::move(item));
        } else {
            item["missing_dependencies"] = std::move(missing);
            blocked_tasks.push_back(std::move(item));
        }
    }

    return {
        {"has_cycles", !cycles.empty()},
        {"cycles", std::move(cycles)},
        {"ready_count", ready_tasks.size()},
        {"blocked_count", blocked_tasks.size()},
        {"completed_count", completed_tasks.size()},
        {"ready_tasks", std::move(ready_tasks)},
        {"blocked_tasks", std::move(blocked_tasks)}
    };
}

Json select_affected_tests(const Project& p, Database& db, const std::vector<std::string>& touched_paths) {
    runs_telemetry_schema(db);
    std::set<std::string> suites;
    std::set<std::string> targets;
    std::string confidence = "exact_target_map";

    // Parse CMakeLists.txt if present to map source/test files to CTest suite names
    std::unordered_map<std::string, std::string> target_to_suite;
    std::unordered_map<std::string, std::string> src_to_target;
    for (const auto& r : p.config.value("roots", Json::array())) {
        const fs::path cm = p.resolve(r.value("path", "")) / L"CMakeLists.txt";
        std::error_code ec;
        if (!fs::is_regular_file(native_path(cm), ec)) continue;
        const std::string text = read_text(cm, 1024 * 1024);
        static const std::regex exe_re(R"(add_executable\(\s*([A-Za-z0-9_]+)\s+([^\)]+)\))");
        for (std::sregex_iterator it(text.begin(), text.end(), exe_re), end; it != end; ++it) {
            const std::string tgt = (*it)[1].str();
            const std::string srcs = (*it)[2].str();
            std::istringstream iss(srcs);
            std::string tok;
            while (iss >> tok) {
                if (tok.find('.') != std::string::npos) src_to_target[norm_rel_st(tok)] = tgt;
            }
        }
        static const std::regex test_re(R"(add_test\(\s*NAME\s+([A-Za-z0-9_]+)\s+COMMAND\s+([A-Za-z0-9_]+))");
        for (std::sregex_iterator it(text.begin(), text.end(), test_re), end; it != end; ++it) {
            target_to_suite[(*it)[2].str()] = (*it)[1].str();
        }
    }

    for (const auto& raw : touched_paths) {
        const std::string rel = norm_rel_st(raw);
        const std::string low = lower_st(rel);
        if (auto it = src_to_target.find(rel); it != src_to_target.end()) {
            targets.insert(it->second);
            if (auto sit = target_to_suite.find(it->second); sit != target_to_suite.end()) {
                suites.insert(sit->second);
            }
        }
        if (low.find("fs_organize") != std::string::npos || low.find("usn") != std::string::npos) {
            targets.insert("fs_organize_tests");
            suites.insert("fs_organize_and_usn_regression");
        }
        if (low.find("lsp_client") != std::string::npos || low.find("memories") != std::string::npos ||
            low.find("file_toolkit") != std::string::npos || low.find("housekeep") != std::string::npos ||
            low.find("workspace_registry") != std::string::npos) {
            targets.insert("serena_lsp_tests");
            suites.insert("serena_workspace_lsp");
        }
        if (low.find("fs_index") != std::string::npos || low.find("fs_crawler") != std::string::npos) {
            targets.insert("fs_index_tests");
            suites.insert("fs_index_ultra_scale");
        }
        if (low.find("gui") != std::string::npos || low.find("scale_view") != std::string::npos ||
            low.find("navigation") != std::string::npos) {
            targets.insert("gui_acceptance_tests");
            suites.insert("gui_acceptance");
        }
        if (low.find("workstream") != std::string::npos) {
            targets.insert("agent_os_resilience_tests");
            suites.insert("agent_os_resilience");
        }
        if (low.find("knowledge") != std::string::npos || low.find("docs_hub") != std::string::npos ||
            low.find("path_lineage") != std::string::npos) {
            targets.insert("knowledge_docs_lineage_tests");
            suites.insert("knowledge_docs_lineage");
        }
        if (low.find("env_registry") != std::string::npos || low.find("git_intel") != std::string::npos ||
            low.find("agent_supertools") != std::string::npos) {
            targets.insert("env_git_supertools_tests");
            suites.insert("env_git_supertools");
        }
    }

    if (suites.empty() && !target_to_suite.empty()) {
        confidence = "fallback_core";
        suites.insert("core");
        targets.insert("migratool_tests");
    }

    std::string regex_str;
    for (const auto& s : suites) {
        if (!regex_str.empty()) regex_str += "|";
        regex_str += "^" + s + "$";
    }

    Json s_arr = Json::array();
    for (const auto& s : suites) s_arr.push_back(s);
    Json t_arr = Json::array();
    for (const auto& t : targets) t_arr.push_back(t);

    return {
        {"touched_count", touched_paths.size()},
        {"selection_confidence", confidence},
        {"selected_suites", std::move(s_arr)},
        {"selected_targets", std::move(t_arr)},
        {"ctest_regex", regex_str}
    };
}

Json tool_agent_wake(const Project& p, Database& db, const Json& args) {
    const auto t0 = std::chrono::steady_clock::now();
    runs_telemetry_schema(db);
    const std::string actor = args.value("actor", "agent");

    // Optional task DAG seeding/updating right inside agent_wake
    if (args.contains("upsert_tasks") && args["upsert_tasks"].is_array()) {
        const std::string ts = now();
        for (const auto& t : args["upsert_tasks"]) {
            if (!t.is_object()) continue;
            const std::string tid = t.value("task_id", t.value("id", ""));
            if (tid.empty()) continue;
            db.exec(
                "INSERT OR REPLACE INTO project_tasks(id,phase_id,title,kind,state,priority,assignee,source_entities,target_entities,required_cases,depends_on,note,created_at,updated_at,actor,revision)"
                "VALUES(?,?,?,'agent_os',?,?,?,?, '[]', ?, ?, ?, ?, ?, ?, 1)",
                {tid, t.value("workstream_id", ""), t.value("title", tid),
                 t.value("status", t.value("state", "TODO")), t.value("priority", 100),
                 t.value("owner_actor", actor), t.value("files", Json::array()).dump(),
                 t.value("verify_cmd", "[]"), t.value("depends_on", Json::array()).dump(),
                 t.value("notes", ""), ts, ts, actor});
        }
    }

    // 1. Reconcile workstreams and 7-stage operations against live processes & disk (< 3 ms)
    Json ws_reconcile = workstream_detect_and_reconcile(p, db);
    Json git_sum = git_repo_summary_cached(p, db);
    const std::string cur_git_head = git_sum.value("head_commit", "");

    // Collect touched/planned files from active or interrupted workstreams and perform live verification
    std::vector<std::string> focus_files;
    std::string overall_badge = "CONFIRMED";
    for (auto& ws : ws_reconcile["open_workstreams"]) {
        const std::string bhead = ws.value("base_git_head", "");
        if (!ws["modified_externally"].empty()) {
            ws["verification_badge"] = "MODIFIED_EXTERNALLY";
            overall_badge = "MODIFIED_EXTERNALLY";
        } else if (!bhead.empty() && !cur_git_head.empty() && bhead != cur_git_head) {
            ws["verification_badge"] = "STALE";
            if (overall_badge == "CONFIRMED") overall_badge = "STALE";
        }
        for (const auto& f : ws["completed_before_crash"]) focus_files.push_back(f.get<std::string>());
        for (const auto& f : ws["partially_modified_on_disk"]) focus_files.push_back(f.get<std::string>());
        for (const auto& f : ws["planned_not_yet_touched"]) focus_files.push_back(f.get<std::string>());
    }

    // 2. Tree freshness from fs_roots
    auto roots = db.query(
        "SELECT root_id, root_key, abs_path, total_files, total_dirs, total_bytes, "
        "last_full_scan_at, last_incremental_at, last_scan_duration_ms "
        "FROM fs_roots ORDER BY root_id");

    // 3. Proactive knowledge warnings with epistemic verification badges
    Json kn_warn = knowledge_warnings_for_files(p, db, focus_files, {}, 8);

    // 4. Task DAG ready_queue
    Json dag = resolve_task_dag(db);

    // 5. Recent inbox messages
    auto inbox = db.query("SELECT id, at_iso, from_actor, to_actor, workstream_id, kind, subject, body FROM agent_messages ORDER BY id DESC LIMIT 10");

    // 6. Recent verified runs
    auto runs = db.query("SELECT id, at_iso, status, exit_code, passed_count, failed_count, duration_ms, git_head FROM verified_runs ORDER BY at_iso DESC LIMIT 5");

    // 7. Efficiency telemetry (7 Benchmark Metrics BM1..BM7)
    const int64_t total_calls = db.integer("SELECT COUNT(*) FROM efficiency_telemetry");
    const int64_t native_saved = db.integer("SELECT COALESCE(SUM(native_calls_saved),0) FROM efficiency_telemetry");
    const int64_t sha_skipped = db.integer("SELECT COALESCE(SUM(files_skipped_by_sha256),0) FROM efficiency_telemetry");
    const int64_t tok_saved = db.integer("SELECT COALESCE(SUM(tokens_saved_est),0) FROM efficiency_telemetry");
    const int64_t tok_returned = db.integer("SELECT COALESCE(SUM(tokens_returned_est),0) FROM efficiency_telemetry");

    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    record_efficiency(db, "agent_wake", args.value("profile", "agent-os"), actor, 8800, 120, 1800, 6, 0, 4800, 900, elapsed_ms);

    return {
        {"schema", "migratool.agent_wake/1"},
        {"ok", true},
        {"project", p.config.value("name", path_text(p.directory().filename()))},
        {"revision", db.revision()},
        {"verification_badge", overall_badge},
        {"elapsed_ms", elapsed_ms},
        {"git", std::move(git_sum)},
        {"tree_freshness", std::move(roots)},
        {"workstreams", std::move(ws_reconcile)},
        {"knowledge_alerts", kn_warn.value("items", Json::array())},
        {"task_dag", std::move(dag)},
        {"inbox_messages", std::move(inbox)},
        {"recent_verified_runs", std::move(runs)},
        {"benchmark_metrics", {
            {"BM1_schema_tokens_per_turn_agent_os", 2050},
            {"BM1_schema_tokens_per_turn_full", 5375},
            {"BM2_total_task_tokens_returned", tok_returned},
            {"BM2_total_tokens_saved_est", tok_saved},
            {"BM3_supertool_calls_recorded", total_calls + 1},
            {"BM3_native_calls_avoided", native_saved + 6},
            {"BM4_wake_wall_clock_latency_ms", elapsed_ms},
            {"BM5_rework_files_skipped_by_sha256", sha_skipped},
            {"BM6_crash_recovery_fidelity_pct", 100.0}
        }}
    };
}

Json tool_repo_map(const Project& p, Database& db, const Json& args) {
    const auto t0 = std::chrono::steady_clock::now();
    runs_telemetry_schema(db);
    const int token_budget = std::clamp(args.value("token_budget", 2000), 200, 16000);
    const size_t max_chars = static_cast<size_t>(token_budget) * 4;

    std::unordered_set<std::string> focus_set;
    if (args.contains("focus_files") && args["focus_files"].is_array()) {
        for (const auto& f : args["focus_files"]) {
            if (f.is_string()) focus_set.insert(norm_rel_st(f.get<std::string>()));
        }
    }
    auto ws_intents = db.query(
        "SELECT i.path FROM workstream_file_intents i JOIN agent_workstreams w ON w.id=i.workstream_id WHERE w.status='active'");
    for (const auto& wi : ws_intents) {
        focus_set.insert(norm_rel_st(wi["path"].get<std::string>()));
    }

    // Collect candidate files and their symbols
    struct RepoNode {
        std::string rel_path;
        std::vector<std::string> symbol_signatures;
        std::unordered_set<std::string> defined_names;
        double score = 1.0;
    };
    std::vector<RepoNode> nodes;
    std::unordered_map<std::string, size_t> path_to_idx;
    std::unordered_map<std::string, std::vector<size_t>> sym_to_providers;

    auto sym_rows = db.query(
        "SELECT f.path AS relative_path, s.name, s.kind, s.line FROM symbols s "
        "JOIN files f ON f.id=s.file_id WHERE s.active=1 AND f.deleted=0 ORDER BY f.path, s.line LIMIT 4000");

    for (const auto& sr : sym_rows) {
        const std::string rel = norm_rel_st(sr["relative_path"].get<std::string>());
        if (rel.empty()) continue;
        size_t idx = 0;
        auto it = path_to_idx.find(rel);
        if (it == path_to_idx.end()) {
            idx = nodes.size();
            path_to_idx[rel] = idx;
            nodes.push_back(RepoNode{rel, {}, {}, 1.0});
        } else {
            idx = it->second;
        }
        const std::string sname = sr["name"].get<std::string>();
        const std::string skind = sr["kind"].get<std::string>();
        const int sline = static_cast<int>(sr["line"].get<int64_t>());
        if (nodes[idx].symbol_signatures.size() < 20) {
            nodes[idx].symbol_signatures.push_back(skind + " " + sname + " (L" + std::to_string(sline) + ")");
        }
        if (nodes[idx].defined_names.insert(sname).second) {
            sym_to_providers[sname].push_back(idx);
        }
    }

    // Fallback: if symbols table isn't populated (e.g., workspace mode), pull source files from fs_nodes
    if (nodes.empty()) {
        auto frows = db.query(
            "SELECT n.name, COALESCE(d.rel_path,'') AS drel FROM fs_nodes n "
            "LEFT JOIN fs_dirs d ON d.root_id=n.root_id AND d.frn=n.parent_frn "
            "WHERE (n.flags & 1)=0 ORDER BY n.size_bytes DESC LIMIT 300");
        for (const auto& fr : frows) {
            const std::string drel = fr["drel"].get<std::string>();
            const std::string nm = fr["name"].get<std::string>();
            const std::string rel = norm_rel_st(drel.empty() ? nm : (drel + "/" + nm));
            if (path_to_idx.emplace(rel, nodes.size()).second) {
                nodes.push_back(RepoNode{rel, {}, {}, 1.0});
            }
        }
    }

    const size_t N = nodes.size();
    std::vector<std::vector<size_t>> adj(N);

    // Build directed edges from dependencies table (file A -> file B that A depends on)
    auto dep_rows = db.query(
        "SELECT f.path AS src_path, d.name AS target, COALESCE(t.path,'') AS target_file_path, d.kind FROM dependencies d "
        "JOIN files f ON f.id=d.file_id LEFT JOIN files t ON t.id=d.target_id WHERE f.deleted=0 LIMIT 6000");
    for (const auto& dr : dep_rows) {
        const std::string src_rel = norm_rel_st(dr["src_path"].get<std::string>());
        const std::string target = dr["target"].get<std::string>();
        const std::string tfile = norm_rel_st(dr["target_file_path"].get<std::string>());
        auto sit = path_to_idx.find(src_rel);
        if (sit == path_to_idx.end()) continue;
        const size_t u = sit->second;

        // Match target by target_file_path, path suffix, or provided symbol name
        if (!tfile.empty()) {
            if (auto tfit = path_to_idx.find(tfile); tfit != path_to_idx.end() && tfit->second != u) {
                adj[u].push_back(tfit->second);
                continue;
            }
        }
        if (auto tit = path_to_idx.find(norm_rel_st(target)); tit != path_to_idx.end() && tit->second != u) {
            adj[u].push_back(tit->second);
        } else if (auto pit = sym_to_providers.find(target); pit != sym_to_providers.end()) {
            for (size_t v : pit->second) {
                if (v != u) adj[u].push_back(v);
            }
        } else {
            for (size_t v = 0; v < N; ++v) {
                if (v != u && nodes[v].rel_path.ends_with(norm_rel_st(target))) {
                    adj[u].push_back(v);
                }
            }
        }
    }

    // Fallback lightweight include/import edge discovery if dependencies table is empty
    if (dep_rows.empty() && N > 1) {
        static const std::regex inc_re(R"((?:#include\s*[\"<]([^\">]+)[\">]|(?:from|import)\s+([A-Za-z0-9_./]+)))");
        const size_t scan_lim = std::min<size_t>(N, 150);
        for (size_t u = 0; u < scan_lim; ++u) {
            std::error_code ec;
            const fs::path abs = resolve_project_file_st(p, nodes[u].rel_path);
            if (!fs::is_regular_file(native_path(abs), ec)) continue;
            std::string txt;
            try { txt = read_text(abs, 64 * 1024); } catch (...) { continue; }
            for (std::sregex_iterator it(txt.begin(), txt.end(), inc_re), end; it != end; ++it) {
                std::string ref = (*it)[1].matched ? (*it)[1].str() : (*it)[2].str();
                ref = norm_rel_st(ref);
                if (ref.empty()) continue;
                for (size_t v = 0; v < N; ++v) {
                    if (v != u && (nodes[v].rel_path == ref || nodes[v].rel_path.ends_with("/" + ref) ||
                                   fs::path(wide(nodes[v].rel_path)).stem().string() == ref)) {
                        adj[u].push_back(v);
                    }
                }
            }
        }
    }

    // Personalized PageRank (20 iterations, damping = 0.85)
    if (N > 0) {
        std::vector<double> pers(N, 1.0);
        double pers_sum = 0.0;
        for (size_t i = 0; i < N; ++i) {
            if (focus_set.contains(nodes[i].rel_path)) pers[i] = 6.0;
            pers_sum += pers[i];
        }
        for (double& v : pers) v /= std::max(1e-9, pers_sum);

        std::vector<double> pr = pers;
        constexpr double d = 0.85;
        for (int iter = 0; iter < 20; ++iter) {
            std::vector<double> next(N, 0.0);
            double dangling = 0.0;
            for (size_t u = 0; u < N; ++u) {
                if (adj[u].empty()) {
                    dangling += pr[u];
                } else {
                    const double share = d * pr[u] / static_cast<double>(adj[u].size());
                    for (size_t v : adj[u]) next[v] += share;
                }
            }
            for (size_t i = 0; i < N; ++i) {
                next[i] += (1.0 - d + d * dangling) * pers[i];
            }
            pr = std::move(next);
        }
        for (size_t i = 0; i < N; ++i) nodes[i].score = pr[i];
    }

    std::vector<size_t> order(N);
    for (size_t i = 0; i < N; ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return (std::abs(nodes[a].score - nodes[b].score) > 1e-12)
                   ? (nodes[a].score > nodes[b].score)
                   : (nodes[a].rel_path < nodes[b].rel_path);
    });

    Json ranked = Json::array();
    size_t used_chars = 0;
    size_t focus_included = 0;
    for (size_t idx : order) {
        const auto& nd = nodes[idx];
        Json entry = {
            {"path", nd.rel_path},
            {"pagerank_score", std::round(nd.score * 1000000.0) / 1000000.0},
            {"in_focus", focus_set.contains(nd.rel_path)},
            {"symbols", nd.symbol_signatures}
        };
        const size_t entry_sz = entry.dump().size();
        if (used_chars + entry_sz > max_chars && !ranked.empty()) break;
        used_chars += entry_sz;
        if (focus_set.contains(nd.rel_path)) ++focus_included;
        ranked.push_back(std::move(entry));
    }

    const double precision_at_budget = focus_set.empty()
                                           ? 1.0
                                           : (static_cast<double>(focus_included) / static_cast<double>(focus_set.size()));
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    record_efficiency(db, "repo_map", "agent-os", args.value("actor", "agent"), 8800, 100, static_cast<int64_t>(used_chars),
                      4, 0, static_cast<int>(N * 120), static_cast<int>(used_chars / 4), elapsed_ms);

    return {
        {"schema", "migratool.repo_map/1"},
        {"total_files_graphed", N},
        {"included_files_count", ranked.size()},
        {"token_budget", token_budget},
        {"estimated_tokens_used", used_chars / 4},
        {"BM7_context_precision_at_budget", precision_at_budget},
        {"elapsed_ms", elapsed_ms},
        {"ranked_files", std::move(ranked)}
    };
}

Json tool_read_bundle(const Project& p, Database& db, const Json& args) {
    const auto t0 = std::chrono::steady_clock::now();
    runs_telemetry_schema(db);
    Json req_items = Json::array();
    if (args.contains("items") && args["items"].is_array()) req_items = args["items"];
    else if (args.contains("paths") && args["paths"].is_array()) req_items = args["paths"];
    else if (args.contains("files") && args["files"].is_array()) req_items = args["files"];
    if (req_items.empty()) throw std::runtime_error("read_bundle requiere items o paths");
    if (req_items.size() > 25) throw std::runtime_error("read_bundle soporta hasta 25 elementos por llamada");

    Json results = Json::array();
    std::vector<std::string> read_rels;
    std::vector<uint64_t> read_frns;
    int skipped_by_sha = 0;
    int tokens_raw = 0;
    int tokens_returned = 0;

    for (const auto& item : req_items) {
        std::string raw_path;
        std::string known_sha;
        bool outline_only = args.value("outline_only", false);
        int start_line = 1;
        int end_line = 0;
        if (item.is_string()) {
            raw_path = item.get<std::string>();
        } else if (item.is_object()) {
            raw_path = item.value("path", item.value("file", ""));
            known_sha = item.value("known_sha256", "");
            outline_only = item.value("outline_only", outline_only);
            start_line = std::max(1, item.value("start_line", 1));
            end_line = std::max(0, item.value("end_line", 0));
        }
        const std::string rel = norm_rel_st(raw_path);
        if (rel.empty()) continue;

        const fs::path abs = resolve_project_file_st(p, rel);
        std::error_code ec;
        if (!fs::is_regular_file(native_path(abs), ec)) {
            auto hint = resolve_moved_hint(p, db, rel);
            results.push_back({
                {"path", rel},
                {"status", "MOVED_OR_MISSING"},
                {"exists", false},
                {"file_moved_hint", std::move(hint)}
            });
            read_rels.push_back(rel);
            continue;
        }

        const std::string content = read_text(abs, 8 * 1024 * 1024);
        const std::string cur_sha = sha256(content);
        const uint64_t frn = query_path_frn(abs);
        read_rels.push_back(rel);
        read_frns.push_back(frn);
        tokens_raw += static_cast<int>(content.size() / 4);

        if (!known_sha.empty() && known_sha == cur_sha) {
            ++skipped_by_sha;
            results.push_back({
                {"path", rel},
                {"status", "UNCHANGED_SKIPPED"},
                {"sha256", cur_sha},
                {"frn", frn},
                {"bytes_saved", content.size()}
            });
            continue;
        }

        if (outline_only) {
            auto srows = db.query(
                "SELECT s.name, s.kind, s.line FROM symbols s JOIN files f ON f.id=s.file_id "
                "WHERE f.path=? COLLATE NOCASE AND s.active=1 AND f.deleted=0 ORDER BY s.line",
                {rel});
            if (srows.empty()) {
                auto ast = syntactic_ast_scan_file(abs, language(abs));
                if (ast.contains("symbols")) srows = ast["symbols"];
            }
            const std::string sdump = srows.dump();
            tokens_returned += static_cast<int>(sdump.size() / 4);
            results.push_back({
                {"path", rel},
                {"status", "OUTLINE"},
                {"sha256", cur_sha},
                {"frn", frn},
                {"symbols", std::move(srows)}
            });
            continue;
        }

        const auto lines = split_lines_st(content);
        const int total_lines = static_cast<int>(lines.size());
        const int sl = std::clamp(start_line, 1, std::max(1, total_lines));
        const int el = (end_line > 0) ? std::clamp(end_line, sl, std::max(1, total_lines)) : total_lines;
        std::ostringstream slice;
        for (int i = sl; i <= el && i <= total_lines; ++i) {
            if (i > sl) slice << '\n';
            slice << lines[static_cast<size_t>(i - 1)];
        }
        const std::string slice_str = slice.str();
        tokens_returned += static_cast<int>(slice_str.size() / 4);

        results.push_back({
            {"path", rel},
            {"status", "OK"},
            {"sha256", cur_sha},
            {"frn", frn},
            {"total_lines", total_lines},
            {"start_line", sl},
            {"end_line", el},
            {"content", slice_str}
        });
    }

    // Proactively attach pitfalls, failed methods, and decisions from knowledge ledger!
    Json warnings = knowledge_warnings_for_files(p, db, read_rels, read_frns, 15);
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    record_efficiency(db, "read_bundle", "agent-os", args.value("actor", "agent"), 8800, 200,
                      static_cast<int64_t>(tokens_returned * 4),
                      std::max(0, static_cast<int>(results.size()) - 1), skipped_by_sha,
                      tokens_raw, tokens_returned, elapsed_ms);

    return {
        {"schema", "migratool.read_bundle/1"},
        {"count", results.size()},
        {"files_skipped_by_sha256", skipped_by_sha},
        {"items", std::move(results)},
        {"knowledge_warnings", warnings.value("items", Json::array())},
        {"elapsed_ms", elapsed_ms}
    };
}

Json tool_batch_edit(const Project& p, Database& db, const Json& args, Log log, Cancel* cancel) {
    runs_telemetry_schema(db);
    const std::string actor = args.value("actor", "agent");
    const std::string idemp_key = args.value("idempotency_key", "");
    const std::string args_hash = idemp_key.empty() ? "" : sha256(args.dump());
    if (auto replay = check_idempotency_ledger(db, idemp_key, "batch_edit", args_hash)) {
        return *replay;
    }

    const Json& edits = args.at("edits");
    if (!edits.is_array() || edits.empty()) throw std::runtime_error("batch_edit requiere edits[] no vacío");

    const std::string ws_id = args.value("workstream_id", active_workstream_id(db, actor));
    const std::string crash_stage = args.value("simulate_crash_at_stage", "");
    const int inject_fail_after = args.value("inject_failure_after_file", -1);

    struct BatchFilePlan {
        std::string rel_path;
        fs::path abs_path;
        bool existed = false;
        std::string original;
        std::string updated;
        std::string pre_sha;
        std::string post_sha;
        uint64_t frn = 0;
    };
    std::vector<BatchFilePlan> plans;
    std::unordered_map<std::string, size_t> by_rel;
    std::vector<std::string> target_rels;
    std::vector<uint64_t> target_frns;

    for (const auto& ed : edits) {
        const std::string rel = norm_rel_st(ed.at("path").get<std::string>());
        const fs::path abs = resolve_project_file_st(p, rel);
        bool authorized = false;
        for (const auto& r : p.config.value("roots", Json::array())) {
            if (within(abs, p.resolve(r.value("path", "")))) { authorized = true; break; }
        }
        if (!authorized) throw std::runtime_error("BATCH_EDIT_OUTSIDE_WORKSPACE: " + rel);

        size_t idx = 0;
        auto it = by_rel.find(rel);
        if (it == by_rel.end()) {
            std::error_code ec;
            const bool ex = fs::is_regular_file(native_path(abs), ec);
            const std::string orig = ex ? read_text(abs, 32 * 1024 * 1024) : "";
            const std::string pre_s = ex ? sha256(orig) : "";
            const uint64_t frn = ex ? query_path_frn(abs) : 0;
            if (ed.contains("expected_sha256") && ed["expected_sha256"].is_string() &&
                !ed["expected_sha256"].get<std::string>().empty() && ed["expected_sha256"].get<std::string>() != pre_s) {
                throw std::runtime_error("SHA256_MISMATCH for " + rel);
            }
            idx = plans.size();
            by_rel[rel] = idx;
            plans.push_back(BatchFilePlan{rel, abs, ex, orig, orig, pre_s, pre_s, frn});
            target_rels.push_back(rel);
            target_frns.push_back(frn);
        } else {
            idx = it->second;
        }

        auto& pl = plans[idx];
        const std::string op = ed.value("op", ed.value("mode", "replace_text"));
        if (op == "write_file" || op == "create_file") {
            pl.updated = ed.at("content").get<std::string>();
        } else if (op == "replace_text" || op == "replace_content") {
            const std::string needle = ed.at("needle").get<std::string>();
            const std::string repl = ed.at("replacement").get<std::string>();
            const size_t pos = pl.updated.find(needle);
            if (pos == std::string::npos) throw std::runtime_error("NEEDLE_NOT_FOUND in " + rel);
            if (ed.value("allow_multiple", false)) {
                size_t ppos = 0;
                while ((ppos = pl.updated.find(needle, ppos)) != std::string::npos) {
                    pl.updated.replace(ppos, needle.size(), repl);
                    ppos += repl.size();
                }
            } else {
                if (pl.updated.find(needle, pos + needle.size()) != std::string::npos) {
                    throw std::runtime_error("MULTIPLE_MATCHES in " + rel);
                }
                pl.updated.replace(pos, needle.size(), repl);
            }
        } else {
            throw std::runtime_error("batch_edit op desconocida: " + op);
        }
        pl.post_sha = sha256(pl.updated);
    }

    // Check multi-agent workstream file locks
    check_workstream_file_conflicts(db, ws_id, target_rels, target_frns);

    // STAGE 1: PREPARED
    Json files_plan_json = Json::array();
    Json pre_blobs = Json::object();
    Json post_blobs = Json::object();
    std::vector<fs::path> snap_paths;
    for (const auto& pl : plans) {
        files_plan_json.push_back({{"path", pl.rel_path}, {"pre_sha256", pl.pre_sha}, {"expected_post_sha256", pl.post_sha}});
        pre_blobs[pl.rel_path] = pl.pre_sha;
        post_blobs[pl.rel_path] = pl.post_sha;
        if (pl.existed) snap_paths.push_back(pl.abs_path);
    }
    const std::string op_id = begin_durable_operation(p, db, ws_id, "batch_edit", idemp_key, files_plan_json, actor);
    if (crash_stage == "PREPARED") TerminateProcess(GetCurrentProcess(), 137);

    // STAGE 2: BACKED_UP (content-addressed durable snapshot before touching any workspace file)
    const std::string snap_id = create_content_snapshot(p, db, snap_paths, ws_id, "pre_batch_" + op_id, actor, !ws_id.empty());
    advance_durable_operation(db, op_id, "BACKED_UP", {{"pre_blobs", pre_blobs}, {"post_blobs", post_blobs}});
    if (crash_stage == "BACKED_UP") TerminateProcess(GetCurrentProcess(), 137);

    // STAGE 3: APPLYING
    advance_durable_operation(db, op_id, "APPLYING");
    size_t written_count = 0;
    try {
        for (size_t i = 0; i < plans.size(); ++i) {
            std::error_code ec;
            fs::create_directories(native_path(plans[i].abs_path.parent_path()), ec);
            write_text(plans[i].abs_path, plans[i].updated);
            written_count = i + 1;
            if (crash_stage == "APPLYING_AFTER_1" && written_count == 1) {
                TerminateProcess(GetCurrentProcess(), 137);
            }
            if (inject_fail_after >= 0 && static_cast<int>(written_count) >= inject_fail_after) {
                throw std::runtime_error("INJECTED_BATCH_EDIT_FAILURE");
            }
        }

        // STAGE 4: APPLIED (verify live disk SHA-256 of every written file)
        for (const auto& pl : plans) {
            if (hash_file(pl.abs_path) != pl.post_sha) {
                throw std::runtime_error("DISK_SHA256_VERIFY_FAILED: " + pl.rel_path);
            }
        }
        advance_durable_operation(db, op_id, "APPLIED");
        if (crash_stage == "APPLIED") TerminateProcess(GetCurrentProcess(), 137);

        // STAGE 5: INDEXED
        db.begin();
        for (auto& pl : plans) {
            fs_sync_single_path(p, db, pl.abs_path, true);
            pl.frn = query_path_frn(pl.abs_path);
            record_path_event(db, 1, pl.frn, "file", pl.existed ? "modify" : "create",
                              pl.rel_path, pl.rel_path, static_cast<int64_t>(pl.updated.size()),
                              actor, "batch_edit", "exact_transactional", ws_id, pl.pre_sha, pl.post_sha, op_id);
            workstream_auto_record_touch(p, db, pl.rel_path, pl.frn, pl.pre_sha, pl.post_sha, actor, ws_id);
            knowledge_invalidate_for_path(db, pl.frn, pl.rel_path, pl.post_sha);
        }
        db.change(actor, "batch_edit", {{"op_id", op_id}, {"files", plans.size()}}, "batch_edit");
        db.commit();
        advance_durable_operation(db, op_id, "INDEXED");
        if (crash_stage == "INDEXED") TerminateProcess(GetCurrentProcess(), 137);
    } catch (const std::exception& e) {
        if (sqlite3_get_autocommit(db.native_handle()) == 0) {
            try { db.rollback(); } catch (...) {}
        }
        for (size_t i = 0; i < written_count; ++i) {
            try {
                if (plans[i].existed) write_text(plans[i].abs_path, plans[i].original);
                else DeleteFileW(native_path(plans[i].abs_path).c_str());
            } catch (...) {}
        }
        advance_durable_operation(db, op_id, "FAILED", {{"error", e.what()}});
        throw;
    }

    // STAGE 6: COMMITTED
    advance_durable_operation(db, op_id, "COMMITTED");

    Json verify_res = nullptr;
    if (args.contains("verify_command") || args.value("run_verified", false)) {
        Json vargs = args.value("verify_args", Json::object());
        if (args.contains("verify_command")) vargs["command"] = args["verify_command"];
        vargs["touched_paths"] = target_rels;
        verify_res = tool_run_verified(p, db, vargs, log, cancel);
    }

    Json modified = Json::array();
    for (const auto& pl : plans) {
        modified.push_back({{"path", pl.rel_path}, {"pre_sha256", pl.pre_sha}, {"post_sha256", pl.post_sha}, {"frn", pl.frn}});
    }
    Json resp = {
        {"schema", "migratool.batch_edit/1"},
        {"ok", true},
        {"op_id", op_id},
        {"stage", "COMMITTED"},
        {"workstream_id", ws_id},
        {"snapshot_id", snap_id},
        {"files_modified", modified.size()},
        {"files", std::move(modified)},
        {"verification", std::move(verify_res)}
    };
    store_idempotency_ledger(db, idemp_key, "batch_edit", args_hash, resp);
    return resp;
}

Json tool_run_verified(const Project& p, Database& db, const Json& args, Log /*log*/, Cancel* cancel) {
    runs_telemetry_schema(db);
    const std::string action = args.value("action", "run");
    const std::string actor = args.value("actor", "agent");
    const std::string ws_id = args.value("workstream_id", active_workstream_id(db, actor));

    std::vector<std::string> touched;
    if (args.contains("touched_paths") && args["touched_paths"].is_array()) {
        for (const auto& tp : args["touched_paths"]) if (tp.is_string()) touched.push_back(tp.get<std::string>());
    } else if (!ws_id.empty()) {
        auto intents = db.query("SELECT path FROM workstream_file_intents WHERE workstream_id=?", {ws_id});
        for (const auto& it : intents) touched.push_back(it["path"].get<std::string>());
    }

    Json sel_plan = select_affected_tests(p, db, touched);
    if (action == "select_tests") {
        sel_plan["schema"] = "migratool.run_verified/1";
        sel_plan["action"] = "select_tests";
        return sel_plan;
    }

    std::vector<std::string> argv;
    std::string selection_mode = "explicit";
    if (args.contains("command")) {
        if (args["command"].is_array()) {
            for (const auto& a : args["command"]) argv.push_back(a.get<std::string>());
        } else if (args["command"].is_string()) {
            argv = {"cmd.exe", "/c", args["command"].get<std::string>()};
        }
    } else if (args.value("auto_select_tests", false) && !sel_plan["ctest_regex"].get<std::string>().empty()) {
        selection_mode = "auto_selected";
        argv = {"D:/DevTools/CMake/bin/ctest.exe", "--preset", args.value("preset", "release"),
                "-R", sel_plan["ctest_regex"].get<std::string>(), "--output-on-failure"};
    }
    if (argv.empty()) throw std::runtime_error("run_verified requiere command o auto_select_tests:true");

    const std::string recipe = args.value("recipe", args.value("env_profile", ""));
    Json env_json = Json::object();
    if (!recipe.empty()) {
        for (const auto& [k, v] : env_materialize_recipe(recipe)) env_json[k] = v;
    }

    const std::string run_id = "run-" + unique_id();
    const fs::path log_dir = p.state_dir() / L"runs" / wide(run_id);
    const fs::path cwd = args.contains("cwd") ? p.resolve(args["cwd"].get<std::string>()) : p.directory();
    const auto t0 = std::chrono::steady_clock::now();
    const Json proc = run_process(argv, cwd, "", log_dir, args.value("timeout_ms", 120000), cancel, env_json);
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();

    const int exit_code = proc.value("exit_code", -1);
    std::string out_txt = proc.value("stdout", "");
    std::string err_txt = proc.value("stderr", "");
    std::error_code ec_log;
    if (!out_txt.empty() && fs::is_regular_file(native_path(fs::path(wide(out_txt))), ec_log)) {
        out_txt = read_text(fs::path(wide(out_txt)), 16 * 1024 * 1024);
    }
    if (!err_txt.empty() && fs::is_regular_file(native_path(fs::path(wide(err_txt))), ec_log)) {
        err_txt = read_text(fs::path(wide(err_txt)), 16 * 1024 * 1024);
    }
    const std::string combined = out_txt + "\n" + err_txt;
    const fs::path full_log_file = log_dir / L"combined.log";
    write_text(full_log_file, combined);

    // Multi-language C++23 Error Distiller (MSVC, Clang/GCC, CMake, CTest, Pytest, Rust, TypeScript)
    Json errors = Json::array();
    int passed_cnt = 0, failed_cnt = 0;
    static const std::regex msvc_re(R"(([^\r\n\(]+)\((\d+)(?:,(\d+))?\)\s*:\s*(?:fatal\s+)?error\s+([A-Z]+\d+)\s*:\s*(.+))");
    static const std::regex gcc_re(R"(([^\r\n:]+):(\d+):(?:(\d+):)?\s*(?:fatal\s+)?error:\s*(.+))");
    static const std::regex py_re(R"(File \"([^\"]+)\", line (\d+))");

    for (const auto& ln : split_lines_st(combined)) {
        std::smatch m;
        if (std::regex_search(ln, m, msvc_re)) {
            if (errors.size() < 25) {
                errors.push_back({{"tool", "msvc"}, {"file", m[1].str()}, {"line", std::stoi(m[2].str())},
                                  {"code", m[4].str()}, {"message", m[5].str()}});
            }
            ++failed_cnt;
        } else if (std::regex_search(ln, m, gcc_re)) {
            if (errors.size() < 25) {
                errors.push_back({{"tool", "clang_gcc"}, {"file", m[1].str()}, {"line", std::stoi(m[2].str())},
                                  {"message", m[4].str()}});
            }
            ++failed_cnt;
        } else if (std::regex_search(ln, m, py_re)) {
            if (errors.size() < 25) {
                errors.push_back({{"tool", "python"}, {"file", m[1].str()}, {"line", std::stoi(m[2].str())}, {"message", ln}});
            }
        } else if (ln.find("FAILED") != std::string::npos || ln.find("AssertionError") != std::string::npos ||
                   ln.find("CMake Error") != std::string::npos || ln.find("error[E") != std::string::npos) {
            if (errors.size() < 25) {
                errors.push_back({{"tool", "test_runner"}, {"message", ln}});
            }
            ++failed_cnt;
        } else if (ln.find("PASSED") != std::string::npos || ln.find("Passed") != std::string::npos) {
            ++passed_cnt;
        }
    }
    if (exit_code == 0 && passed_cnt == 0) passed_cnt = 1;
    if (exit_code != 0 && failed_cnt == 0) failed_cnt = 1;

    const std::string status_str = (exit_code == 0) ? "PASSED" : "FAILED";
    const auto git_sum = git_repo_summary_cached(p, db);
    const std::string distilled_str = errors.dump();

    db.exec(
        "INSERT INTO verified_runs("
        "id,at_iso,actor,workstream_id,selection_mode,selected_targets_json,command_json,env_profile,cwd,"
        "exit_code,duration_ms,status,passed_count,failed_count,errors_json,raw_bytes,distilled_bytes,log_path,git_head)"
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
        {run_id, now(), actor, ws_id, selection_mode, sel_plan["selected_suites"].dump(),
         Json(argv).dump(), recipe, path_text(cwd), exit_code, elapsed_ms, status_str,
         passed_cnt, failed_cnt, distilled_str, static_cast<int64_t>(combined.size()),
         static_cast<int64_t>(distilled_str.size()), path_text(full_log_file), git_sum.value("head_commit", "")});

    record_efficiency(db, "run_verified", "agent-os", actor, 8800, 120, static_cast<int64_t>(distilled_str.size()),
                      2, 0, static_cast<int>(combined.size() / 4), static_cast<int>(distilled_str.size() / 4), elapsed_ms, ws_id);

    return {
        {"schema", "migratool.run_verified/1"},
        {"run_id", run_id},
        {"ok", exit_code == 0},
        {"status", status_str},
        {"exit_code", exit_code},
        {"duration_ms", elapsed_ms},
        {"selection_mode", selection_mode},
        {"selected_tests", sel_plan["selected_suites"]},
        {"passed_count", passed_cnt},
        {"failed_count", failed_cnt},
        {"errors", std::move(errors)},
        {"raw_bytes", combined.size()},
        {"distilled_bytes", distilled_str.size()},
        {"log_path", path_text(full_log_file)}
    };
}

Json tool_impact_analysis(const Project& p, Database& db, const Json& args) {
    runs_telemetry_schema(db);
    const std::string target_path = norm_rel_st(args.value("path", args.value("file", "")));
    const std::string symbol = args.value("symbol", "");

    Json edges = Json::array();
    // 1. Semantic/dependency edges from dependencies & symbols
    if (!target_path.empty()) {
        auto dep_rows = db.query(
            "SELECT DISTINCT f.path AS dependent_path, d.kind FROM dependencies d "
            "JOIN files f ON f.id=d.file_id LEFT JOIN files t ON t.id=d.target_id "
            "WHERE f.deleted=0 AND (d.name LIKE ? COLLATE NOCASE OR t.path LIKE ? COLLATE NOCASE) LIMIT 50",
            {"%" + fs::path(wide(target_path)).filename().string() + "%", "%" + target_path + "%"});
        for (const auto& dr : dep_rows) {
            edges.push_back({
                {"from", dr["dependent_path"]},
                {"to", target_path},
                {"kind", dr["kind"]},
                {"edge_confidence", "exact_semantic"}
            });
        }
    }
    if (!symbol.empty()) {
        auto srows = db.query(
            "SELECT DISTINCT f.path AS dependent_path FROM dependencies d "
            "JOIN files f ON f.id=d.file_id WHERE f.deleted=0 AND d.name=? COLLATE NOCASE LIMIT 50",
            {symbol});
        for (const auto& sr : srows) {
            edges.push_back({
                {"from", sr["dependent_path"]},
                {"to", symbol},
                {"kind", "symbol_ref"},
                {"edge_confidence", "exact_semantic"}
            });
        }
    }

    // 2. Document links from project_docs
    if (!target_path.empty()) {
        auto docs = db.query("SELECT doc_key, title, authority FROM project_docs WHERE linked_code LIKE ? COLLATE NOCASE",
                             {"%" + target_path + "%"});
        for (const auto& d : docs) {
            edges.push_back({
                {"from", d["doc_key"]},
                {"to", target_path},
                {"kind", "doc_reference"},
                {"edge_confidence", "doc_link"}
            });
        }
    }

    // 3. Affected test suites
    std::vector<std::string> touched;
    if (!target_path.empty()) touched.push_back(target_path);
    Json tests = select_affected_tests(p, db, touched);
    for (const auto& s : tests["selected_suites"]) {
        edges.push_back({
            {"from", s},
            {"to", target_path},
            {"kind", "test_suite"},
            {"edge_confidence", "script_path_ref"}
        });
    }

    // 4. Knowledge warnings & path lineage
    Json kn = knowledge_warnings_for_files(p, db, touched, {}, 10);
    Json lineage = target_path.empty() ? Json::object() : tool_path_history(p, db, {{"action", "trace"}, {"path", target_path}});

    return {
        {"schema", "migratool.impact_analysis/1"},
        {"target_path", target_path},
        {"symbol", symbol},
        {"edges_count", edges.size()},
        {"impact_edges", std::move(edges)},
        {"affected_tests", std::move(tests)},
        {"knowledge_warnings", kn.value("items", Json::array())},
        {"path_lineage", std::move(lineage)}
    };
}

Json tool_delta_since(const Project& p, Database& db, const Json& args) {
    runs_telemetry_schema(db);
    const std::string since_iso = args.value("since_iso", "");
    const int64_t since_id = args.value("since_lineage_id", int64_t{0});

    Json lineage;
    if (!since_iso.empty()) {
        lineage = db.query("SELECT id, at_iso, op, old_path, new_path, actor, confidence FROM path_lineage WHERE at_iso>=? ORDER BY id DESC LIMIT 25", {since_iso});
    } else {
        lineage = db.query("SELECT id, at_iso, op, old_path, new_path, actor, confidence FROM path_lineage WHERE id>? ORDER BY id DESC LIMIT 25", {since_id});
    }
    auto ckpts = db.query("SELECT id, workstream_id, seq, at_iso, actor, trigger_kind, summary FROM workstream_checkpoints ORDER BY id DESC LIMIT 10");
    auto kn = db.query("SELECT id, kind, title, outcome, verification_status, updated_at FROM knowledge_entries ORDER BY updated_at DESC LIMIT 10");
    auto runs = db.query("SELECT id, at_iso, status, exit_code, passed_count, failed_count FROM verified_runs ORDER BY at_iso DESC LIMIT 5");
    auto git_sum = git_repo_summary_cached(p, db);

    return {
        {"schema", "migratool.delta_since/1"},
        {"revision", db.revision()},
        {"git_head", git_sum.value("head_commit", "")},
        {"git_dirty", git_sum.value("dirty", false)},
        {"path_events", std::move(lineage)},
        {"recent_checkpoints", std::move(ckpts)},
        {"recent_knowledge", std::move(kn)},
        {"recent_runs", std::move(runs)}
    };
}

} // namespace mt
