#include "migratool/workstream.hpp"
#include "migratool/fs_index.hpp"
#include "migratool/git_intel.hpp"
#include "migratool/path_lineage.hpp"
#include <shellapi.h>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace mt {
namespace {

std::string norm_rel_ws(std::string p) {
    std::replace(p.begin(), p.end(), '\\', '/');
    while (p.starts_with("./")) p.erase(0, 2);
    while (!p.empty() && p.front() == '/') p.erase(p.begin());
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    return p;
}

int64_t query_process_create_time(DWORD pid) {
    if (pid == 0) return 0;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return 0;
    FILETIME ft_create{}, ft_exit{}, ft_kernel{}, ft_user{};
    int64_t out = 0;
    if (GetProcessTimes(h, &ft_create, &ft_exit, &ft_kernel, &ft_user)) {
        out = static_cast<int64_t>((static_cast<uint64_t>(ft_create.dwHighDateTime) << 32) | ft_create.dwLowDateTime);
    }
    CloseHandle(h);
    return out;
}

bool is_owner_process_alive(int64_t pid, int64_t expected_create_time) {
    if (pid <= 0) return false;
    if (static_cast<DWORD>(pid) == GetCurrentProcessId()) return true;
    HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!h) {
        return GetLastError() == ERROR_ACCESS_DENIED;
    }
    const bool running = (WaitForSingleObject(h, 0) == WAIT_TIMEOUT);
    if (!running) {
        CloseHandle(h);
        return false;
    }
    if (expected_create_time > 0) {
        FILETIME ft_create{}, ft_exit{}, ft_kernel{}, ft_user{};
        if (GetProcessTimes(h, &ft_create, &ft_exit, &ft_kernel, &ft_user)) {
            const int64_t actual = static_cast<int64_t>((static_cast<uint64_t>(ft_create.dwHighDateTime) << 32) | ft_create.dwLowDateTime);
            if (actual != 0 && actual != expected_create_time) {
                CloseHandle(h);
                return false;
            }
        }
    }
    CloseHandle(h);
    return true;
}

std::string iso_after_seconds(int seconds) {
    const auto tp = std::chrono::system_clock::now() + std::chrono::seconds(seconds);
    const std::time_t tt = std::chrono::system_clock::to_time_t(tp);
    std::tm gmt{};
    gmtime_s(&gmt, &tt);
    std::ostringstream oss;
    oss << std::put_time(&gmt, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

fs::path blob_dir_for_project(const Project& p) {
    return p.state_dir() / L"snapshots" / L"blobs";
}

fs::path blob_path_for_sha(const Project& p, const std::string& sha) {
    return blob_dir_for_project(p) / wide(sha);
}

void write_durable_blob(const Project& p, const std::string& sha, const std::string& content) {
    if (sha.empty()) return;
    const fs::path bp = blob_path_for_sha(p, sha);
    std::error_code ec;
    if (fs::is_regular_file(native_path(bp), ec) && fs::file_size(native_path(bp), ec) == content.size()) {
        return;
    }
    fs::create_directories(native_path(bp.parent_path()), ec);
    const fs::path tmp = bp.parent_path() / (wide(sha) + L".tmp-" + wide(unique_id()));
    {
        HANDLE h = CreateFileW(native_path(tmp).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            throw std::runtime_error("No se pudo crear blob durable: " + path_text(bp));
        }
        const char* ptr = content.data();
        size_t rem = content.size();
        while (rem > 0) {
            DWORD chunk = static_cast<DWORD>(std::min<size_t>(rem, 1024 * 1024));
            DWORD written = 0;
            if (!WriteFile(h, ptr, chunk, &written, nullptr)) {
                CloseHandle(h);
                fs::remove(native_path(tmp), ec);
                throw std::runtime_error("Error escribiendo blob durable: " + path_text(bp));
            }
            ptr += written;
            rem -= written;
        }
        FlushFileBuffers(h);
        CloseHandle(h);
    }
    if (!MoveFileExW(native_path(tmp).c_str(), native_path(bp).c_str(), MOVEFILE_REPLACE_EXISTING)) {
        fs::remove(native_path(tmp), ec);
        if (!fs::is_regular_file(native_path(bp), ec)) {
            throw std::runtime_error("No se pudo activar blob durable: " + path_text(bp));
        }
    }
}

bool send_file_to_recycle_bin(const fs::path& p) {
    std::error_code ec;
    if (!fs::exists(native_path(p), ec)) return true;
    std::wstring abs = fs::absolute(p).wstring();
    if (abs.starts_with(L"\\\\?\\")) abs = abs.substr(4);
    abs.push_back(L'\0');
    abs.push_back(L'\0');
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = abs.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
}

fs::path resolve_project_file_ws(const Project& p, const std::string& raw_rel) {
    if (raw_rel.empty()) return {};
    const std::string norm = norm_rel_ws(raw_rel);
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

std::string to_relative_project_path(const Project& p, const fs::path& abs_or_rel) {
    std::error_code ec;
    const fs::path abs = abs_or_rel.is_absolute() ? abs_or_rel : resolve_project_file_ws(p, path_text(abs_or_rel));
    for (const auto& r : p.config.value("roots", Json::array())) {
        const fs::path root_p = p.resolve(r.value("path", ""));
        if (within(abs, root_p)) {
            auto rel = fs::relative(abs, root_p, ec);
            if (!ec && !rel.empty() && rel != ".") return norm_rel_ws(path_text(rel));
        }
    }
    if (within(abs, p.directory())) {
        auto rel = fs::relative(abs, p.directory(), ec);
        if (!ec && !rel.empty() && rel != ".") return norm_rel_ws(path_text(rel));
    }
    return norm_rel_ws(path_text(abs_or_rel));
}

struct SyncFullGuard {
    sqlite3* h = nullptr;
    explicit SyncFullGuard(Database& db) : h(db.native_handle()) {
        sqlite3_exec(h, "PRAGMA synchronous=FULL;", nullptr, nullptr, nullptr);
    }
    ~SyncFullGuard() {
        sqlite3_exec(h, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
    }
};

} // namespace

void workstream_schema(Database& db) {
    path_lineage_schema(db);
    if (db.integer("SELECT COUNT(*) FROM meta WHERE key='workstream_schema' AND value='1'") != 0) return;
    const bool need_tx = (sqlite3_get_autocommit(db.native_handle()) != 0);
    if (need_tx) db.begin();
    try {
        if (db.integer("SELECT COUNT(*) FROM meta WHERE key='workstream_schema' AND value='1'") == 0) {
            char* err = nullptr;
            const char* sql = R"sql(
CREATE TABLE IF NOT EXISTS agent_workstreams (
  id                  TEXT PRIMARY KEY,
  title               TEXT NOT NULL,
  goal                TEXT NOT NULL,
  status              TEXT NOT NULL DEFAULT 'active',
  owner_actor         TEXT NOT NULL,
  owner_session_id    TEXT NOT NULL DEFAULT '',
  owner_pid           INTEGER NOT NULL DEFAULT 0,
  owner_proc_created  INTEGER NOT NULL DEFAULT 0,
  lease_expires_at    TEXT NOT NULL DEFAULT '',
  heartbeat_at        TEXT NOT NULL DEFAULT '',
  created_at          TEXT NOT NULL,
  updated_at          TEXT NOT NULL,
  closed_at           TEXT NOT NULL DEFAULT '',
  base_git_head       TEXT NOT NULL DEFAULT '',
  base_git_branch     TEXT NOT NULL DEFAULT '',
  base_snapshot_id    TEXT NOT NULL DEFAULT '',
  current_step        TEXT NOT NULL DEFAULT '',
  completed_steps     TEXT NOT NULL DEFAULT '[]',
  next_steps          TEXT NOT NULL DEFAULT '[]',
  open_blockers       TEXT NOT NULL DEFAULT '[]',
  assumptions         TEXT NOT NULL DEFAULT '[]',
  resume_instructions TEXT NOT NULL DEFAULT '',
  linked_task_ids     TEXT NOT NULL DEFAULT '[]',
  revision            INTEGER NOT NULL DEFAULT 1
);
CREATE INDEX IF NOT EXISTS idx_agent_workstreams_status ON agent_workstreams(status, updated_at DESC);

CREATE TABLE IF NOT EXISTS workstream_operations (
  op_id           TEXT PRIMARY KEY,
  idempotency_key TEXT NOT NULL DEFAULT '',
  workstream_id   TEXT NOT NULL DEFAULT '',
  tool_name       TEXT NOT NULL,
  stage           TEXT NOT NULL DEFAULT 'PREPARED',
  actor           TEXT NOT NULL DEFAULT 'agent',
  pid             INTEGER NOT NULL DEFAULT 0,
  proc_created    INTEGER NOT NULL DEFAULT 0,
  started_at      TEXT NOT NULL,
  updated_at      TEXT NOT NULL,
  files_json      TEXT NOT NULL DEFAULT '[]',
  pre_blobs_json  TEXT NOT NULL DEFAULT '{}',
  post_blobs_json TEXT NOT NULL DEFAULT '{}',
  error_text      TEXT NOT NULL DEFAULT ''
);
CREATE INDEX IF NOT EXISTS idx_workstream_ops_stage ON workstream_operations(stage, updated_at DESC);
CREATE INDEX IF NOT EXISTS idx_workstream_ops_ws ON workstream_operations(workstream_id, started_at DESC);

CREATE TABLE IF NOT EXISTS workstream_file_intents (
  workstream_id TEXT NOT NULL,
  path          TEXT NOT NULL COLLATE NOCASE,
  frn           INTEGER NOT NULL DEFAULT 0,
  role          TEXT NOT NULL DEFAULT 'target',
  lock_mode     TEXT NOT NULL DEFAULT 'exclusive',
  status        TEXT NOT NULL DEFAULT 'planned',
  intent        TEXT NOT NULL DEFAULT '',
  pre_sha256    TEXT NOT NULL DEFAULT '',
  post_sha256   TEXT NOT NULL DEFAULT '',
  last_edit_at  TEXT NOT NULL DEFAULT '',
  actor         TEXT NOT NULL DEFAULT 'agent',
  PRIMARY KEY(workstream_id, path)
);
CREATE INDEX IF NOT EXISTS idx_ws_file_intents_path ON workstream_file_intents(path COLLATE NOCASE, status);
CREATE INDEX IF NOT EXISTS idx_ws_file_intents_frn  ON workstream_file_intents(frn, status);

CREATE TABLE IF NOT EXISTS workstream_checkpoints (
  id            INTEGER PRIMARY KEY AUTOINCREMENT,
  workstream_id TEXT NOT NULL,
  seq           INTEGER NOT NULL,
  at_iso        TEXT NOT NULL,
  actor         TEXT NOT NULL,
  trigger_kind  TEXT NOT NULL DEFAULT 'manual',
  summary       TEXT NOT NULL,
  git_head      TEXT NOT NULL DEFAULT '',
  snapshot_id   TEXT NOT NULL DEFAULT '',
  state_json    TEXT NOT NULL DEFAULT '{}',
  revision      INTEGER NOT NULL DEFAULT 1
);
CREATE INDEX IF NOT EXISTS idx_ws_checkpoints_ws ON workstream_checkpoints(workstream_id, seq DESC);

CREATE TRIGGER IF NOT EXISTS workstream_checkpoints_no_update
BEFORE UPDATE ON workstream_checkpoints
BEGIN
  SELECT RAISE(ABORT, 'immutable workstream_checkpoints');
END;

CREATE TRIGGER IF NOT EXISTS workstream_checkpoints_no_delete
BEFORE DELETE ON workstream_checkpoints
BEGIN
  SELECT RAISE(ABORT, 'immutable workstream_checkpoints');
END;

CREATE TABLE IF NOT EXISTS workstream_snapshots (
  snapshot_id     TEXT PRIMARY KEY,
  workstream_id   TEXT NOT NULL DEFAULT '',
  label           TEXT NOT NULL DEFAULT '',
  at_iso          TEXT NOT NULL,
  actor           TEXT NOT NULL DEFAULT 'agent',
  pinned          INTEGER NOT NULL DEFAULT 1,
  retention_class TEXT NOT NULL DEFAULT 'active_guard',
  total_bytes     INTEGER NOT NULL DEFAULT 0,
  files_json      TEXT NOT NULL DEFAULT '[]'
);
CREATE INDEX IF NOT EXISTS idx_ws_snapshots_ws ON workstream_snapshots(workstream_id, at_iso DESC);

CREATE TABLE IF NOT EXISTS snapshot_blob_refs (
  sha256      TEXT NOT NULL,
  snapshot_id TEXT NOT NULL,
  size_bytes  INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY(sha256, snapshot_id)
);
CREATE INDEX IF NOT EXISTS idx_snapshot_blob_refs_sha ON snapshot_blob_refs(sha256);

CREATE TABLE IF NOT EXISTS idempotency_ledger (
  idempotency_key TEXT PRIMARY KEY,
  tool            TEXT NOT NULL,
  args_sha256     TEXT NOT NULL,
  status          TEXT NOT NULL DEFAULT 'COMMITTED',
  response_json   TEXT NOT NULL,
  created_at      TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS agent_messages (
  id            INTEGER PRIMARY KEY AUTOINCREMENT,
  at_iso        TEXT NOT NULL,
  from_actor    TEXT NOT NULL,
  to_actor      TEXT NOT NULL DEFAULT 'all',
  workstream_id TEXT NOT NULL DEFAULT '',
  kind          TEXT NOT NULL DEFAULT 'handoff_note',
  subject       TEXT NOT NULL DEFAULT '',
  body          TEXT NOT NULL,
  acked_by      TEXT NOT NULL DEFAULT ''
);
CREATE INDEX IF NOT EXISTS idx_agent_messages_to ON agent_messages(to_actor, id DESC);
)sql";
            if (sqlite3_exec(db.native_handle(), sql, nullptr, nullptr, &err) != SQLITE_OK) {
                std::string msg = err ? err : sqlite3_errmsg(db.native_handle());
                if (err) sqlite3_free(err);
                throw std::runtime_error(msg);
            }
            db.exec("INSERT OR REPLACE INTO meta(key,value) VALUES('workstream_schema','1')");
        }
        if (need_tx) db.commit();
    } catch (...) {
        if (need_tx) db.rollback();
        throw;
    }
}

std::string active_workstream_id(Database& db, const std::string& actor) {
    workstream_schema(db);
    if (!actor.empty()) {
        auto r = db.query("SELECT id FROM agent_workstreams WHERE status='active' AND owner_actor=? ORDER BY updated_at DESC LIMIT 1", {actor});
        if (!r.empty()) return r[0]["id"].get<std::string>();
    }
    auto r = db.query("SELECT id FROM agent_workstreams WHERE status='active' ORDER BY updated_at DESC LIMIT 1");
    if (!r.empty()) return r[0]["id"].get<std::string>();
    return "";
}

std::string create_content_snapshot(const Project& p, Database& db, const std::vector<fs::path>& abs_paths,
                                    const std::string& workstream_id, const std::string& label,
                                    const std::string& actor, bool pinned) {
    workstream_schema(db);
    const std::string snap_id = "snap-" + unique_id();
    const std::string ts = now();
    Json files_arr = Json::array();
    int64_t total_bytes = 0;

    std::vector<std::pair<std::string, int64_t>> blob_refs;
    for (const auto& raw_p : abs_paths) {
        const fs::path abs = raw_p.is_absolute() ? raw_p : resolve_project_file_ws(p, path_text(raw_p));
        const std::string rel = to_relative_project_path(p, abs);
        std::error_code ec;
        const bool exists = fs::is_regular_file(native_path(abs), ec);
        std::string sha;
        int64_t sz = 0;
        uint64_t frn = 0;
        if (exists) {
            const std::string content = read_text(abs);
            sha = sha256(content);
            sz = static_cast<int64_t>(content.size());
            frn = query_path_frn(abs);
            write_durable_blob(p, sha, content);
            total_bytes += sz;
            blob_refs.emplace_back(sha, sz);
        }
        files_arr.push_back({
            {"path", rel},
            {"existed", exists},
            {"sha256", sha},
            {"size_bytes", sz},
            {"frn", frn}
        });
    }

    const std::string ret_class = pinned ? "active_guard" : "ephemeral";
    db.exec(
        "INSERT INTO workstream_snapshots(snapshot_id,workstream_id,label,at_iso,actor,pinned,retention_class,total_bytes,files_json)"
        "VALUES(?,?,?,?,?,?,?,?,?)",
        {snap_id, workstream_id, label, ts, actor.empty() ? "agent" : actor,
         pinned ? 1 : 0, ret_class, total_bytes, files_arr.dump()});

    for (const auto& [sha, sz] : blob_refs) {
        db.exec("INSERT OR IGNORE INTO snapshot_blob_refs(sha256,snapshot_id,size_bytes) VALUES(?,?,?)",
                {sha, snap_id, sz});
    }
    return snap_id;
}

Json restore_content_snapshot(const Project& p, Database& db, const std::string& snapshot_id,
                              const std::string& actor, bool dry_run,
                              const std::string& conflict_policy) {
    workstream_schema(db);
    auto rows = db.query("SELECT workstream_id, label, at_iso, files_json FROM workstream_snapshots WHERE snapshot_id=?", {snapshot_id});
    if (rows.empty()) throw std::runtime_error("SNAPSHOT_NOT_FOUND: " + snapshot_id);

    const std::string ws_id = rows[0]["workstream_id"].get<std::string>();
    const Json files = Json::parse(rows[0]["files_json"].get<std::string>(), nullptr, false);
    if (!files.is_array()) throw std::runtime_error("SNAPSHOT_CORRUPT: " + snapshot_id);

    // Build known workstream hashes (pre_sha256 and post_sha256) to detect post-snapshot external edits
    std::unordered_map<std::string, std::pair<std::string, std::string>> ws_hashes;
    if (!ws_id.empty()) {
        auto intents = db.query("SELECT path, pre_sha256, post_sha256 FROM workstream_file_intents WHERE workstream_id=?", {ws_id});
        for (const auto& it : intents) {
            ws_hashes[norm_rel_ws(it["path"].get<std::string>())] = {
                it["pre_sha256"].get<std::string>(),
                it["post_sha256"].get<std::string>()
            };
        }
    }

    Json conflicts = Json::array();
    for (const auto& f : files) {
        const std::string rel = norm_rel_ws(f.value("path", ""));
        const std::string snap_sha = f.value("sha256", "");
        const bool snap_existed = f.value("existed", true);
        const fs::path abs = resolve_project_file_ws(p, rel);
        std::error_code ec;
        const bool disk_exists = fs::is_regular_file(native_path(abs), ec);
        const std::string disk_sha = disk_exists ? hash_file(abs) : "";

        if (disk_sha == snap_sha && disk_exists == snap_existed) continue;

        auto it = ws_hashes.find(rel);
        if (it != ws_hashes.end()) {
            const std::string& pre_s = it->second.first;
            const std::string& post_s = it->second.second;
            if (!disk_sha.empty() && disk_sha != snap_sha && disk_sha != pre_s && disk_sha != post_s) {
                conflicts.push_back({
                    {"path", rel},
                    {"snapshot_sha256", snap_sha},
                    {"workstream_post_sha256", post_s},
                    {"current_disk_sha256", disk_sha},
                    {"reason", "EXTERNAL_MODIFICATION_DETECTED"}
                });
            }
        }
    }

    if (!conflicts.empty() && conflict_policy == "abort") {
        throw std::runtime_error("SNAPSHOT_RESTORE_CONFLICT: se detectaron modificaciones externas en " +
                                 std::to_string(conflicts.size()) + " archivo(s): " + conflicts.dump());
    }

    std::unordered_set<std::string> conflicted_paths;
    for (const auto& c : conflicts) conflicted_paths.insert(c["path"].get<std::string>());

    std::string pre_restore_backup_id;
    if (!dry_run && !conflicts.empty() && conflict_policy == "backup_and_force") {
        std::vector<fs::path> backup_targets;
        for (const auto& c : conflicts) backup_targets.push_back(resolve_project_file_ws(p, c["path"].get<std::string>()));
        pre_restore_backup_id = create_content_snapshot(p, db, backup_targets, ws_id, "pre_force_restore_" + snapshot_id, actor, true);
    }

    Json restored = Json::array();
    Json skipped_external = Json::array();
    for (const auto& f : files) {
        const std::string rel = norm_rel_ws(f.value("path", ""));
        if (conflicted_paths.contains(rel) && conflict_policy == "skip_conflicted") {
            skipped_external.push_back(rel);
            continue;
        }
        const bool existed = f.value("existed", true);
        const std::string sha = f.value("sha256", "");
        const fs::path abs = resolve_project_file_ws(p, rel);
        std::error_code ec;
        if (!existed) {
            if (!dry_run && fs::exists(native_path(abs), ec)) {
                send_file_to_recycle_bin(abs);
                fs_sync_single_path(p, db, abs, true);
                record_path_event(db, 1, f.value("frn", uint64_t{0}), "file", "restore_delete", rel, "", 0, actor, "workstream_snapshot", "exact_transactional", ws_id);
            }
            restored.push_back({{"path", rel}, {"action", "deleted_created_file"}});
        } else {
            const fs::path bp = blob_path_for_sha(p, sha);
            if (!fs::is_regular_file(native_path(bp), ec)) {
                throw std::runtime_error("SNAPSHOT_BLOB_MISSING: " + sha + " para " + rel);
            }
            if (!dry_run) {
                const std::string old_sha = fs::is_regular_file(native_path(abs), ec) ? hash_file(abs) : "";
                const std::string content = read_text(bp);
                write_text(abs, content);
                fs_sync_single_path(p, db, abs, true);
                record_path_event(db, 1, query_path_frn(abs), "file", "restore", rel, rel,
                                  static_cast<int64_t>(content.size()), actor, "workstream_snapshot",
                                  "exact_transactional", ws_id, old_sha, sha, "snapshot:" + snapshot_id);
            }
            restored.push_back({{"path", rel}, {"action", "restored_content"}, {"sha256", sha}});
        }
    }

    return {
        {"ok", true},
        {"dry_run", dry_run},
        {"snapshot_id", snapshot_id},
        {"workstream_id", ws_id},
        {"conflict_policy", conflict_policy},
        {"pre_restore_backup_id", pre_restore_backup_id},
        {"restored_count", restored.size()},
        {"restored", std::move(restored)},
        {"skipped_external_count", skipped_external.size()},
        {"skipped_external", std::move(skipped_external)},
        {"conflicts", std::move(conflicts)}
    };
}

std::string begin_durable_operation(const Project& p, Database& db, const std::string& workstream_id,
                                    const std::string& tool_name, const std::string& idempotency_key,
                                    const Json& files_plan, const std::string& actor) {
    workstream_schema(db);
    (void)p;
    const std::string op_id = "op-" + unique_id();
    const DWORD pid = GetCurrentProcessId();
    const int64_t created = query_process_create_time(pid);
    const std::string ts = now();
    SyncFullGuard sfg(db);
    db.exec(
        "INSERT INTO workstream_operations(op_id,idempotency_key,workstream_id,tool_name,stage,actor,pid,proc_created,started_at,updated_at,files_json)"
        "VALUES(?,?,?,?,?,?,?,?,?,?,?)",
        {op_id, idempotency_key, workstream_id, tool_name, "PREPARED",
         actor.empty() ? "agent" : actor, static_cast<int64_t>(pid), created, ts, ts, files_plan.dump()});
    return op_id;
}

void advance_durable_operation(Database& db, const std::string& op_id, const std::string& stage,
                               const Json& extra) {
    workstream_schema(db);
    const bool sync_full = (stage == "BACKED_UP" || stage == "COMMITTED" || stage == "RECOVERY" || stage == "FAILED");
    std::unique_ptr<SyncFullGuard> guard;
    if (sync_full) guard = std::make_unique<SyncFullGuard>(db);

    const std::string ts = now();
    if (extra.contains("pre_blobs") || extra.contains("post_blobs") || extra.contains("error")) {
        db.exec(
            "UPDATE workstream_operations SET stage=?, updated_at=?, "
            "pre_blobs_json=CASE WHEN ? != '' THEN ? ELSE pre_blobs_json END, "
            "post_blobs_json=CASE WHEN ? != '' THEN ? ELSE post_blobs_json END, "
            "error_text=CASE WHEN ? != '' THEN ? ELSE error_text END "
            "WHERE op_id=?",
            {stage, ts,
             extra.contains("pre_blobs") ? extra["pre_blobs"].dump() : "",
             extra.contains("pre_blobs") ? extra["pre_blobs"].dump() : "",
             extra.contains("post_blobs") ? extra["post_blobs"].dump() : "",
             extra.contains("post_blobs") ? extra["post_blobs"].dump() : "",
             extra.value("error", ""), extra.value("error", ""),
             op_id});
    } else {
        db.exec("UPDATE workstream_operations SET stage=?, updated_at=? WHERE op_id=?", {stage, ts, op_id});
    }
}

std::optional<Json> check_idempotency_ledger(Database& db, const std::string& idempotency_key,
                                             const std::string& tool_name, const std::string& args_sha256) {
    if (idempotency_key.empty()) return std::nullopt;
    workstream_schema(db);
    auto rows = db.query("SELECT tool, args_sha256, response_json FROM idempotency_ledger WHERE idempotency_key=?", {idempotency_key});
    if (rows.empty()) return std::nullopt;
    if (rows[0]["tool"].get<std::string>() != tool_name ||
        (!args_sha256.empty() && rows[0]["args_sha256"].get<std::string>() != args_sha256)) {
        throw std::runtime_error("IDEMPOTENCY_CONFLICT: la clave '" + idempotency_key + "' ya fue usada con argumentos distintos");
    }
    auto resp = Json::parse(rows[0]["response_json"].get<std::string>(), nullptr, false);
    if (resp.is_object()) {
        resp["idempotent_replay"] = true;
        resp["idempotency_key"] = idempotency_key;
    }
    return resp;
}

void store_idempotency_ledger(Database& db, const std::string& idempotency_key,
                              const std::string& tool_name, const std::string& args_sha256,
                              const Json& response) {
    if (idempotency_key.empty()) return;
    workstream_schema(db);
    db.exec(
        "INSERT OR REPLACE INTO idempotency_ledger(idempotency_key,tool,args_sha256,status,response_json,created_at)"
        "VALUES(?,?,?,'COMMITTED',?,?)",
        {idempotency_key, tool_name, args_sha256, response.dump(), now()});
}

void check_workstream_file_conflicts(Database& db, const std::string& current_ws_id,
                                     const std::vector<std::string>& rel_paths,
                                     const std::vector<uint64_t>& frns) {
    workstream_schema(db);
    const std::string cur_iso = now();
    for (size_t i = 0; i < rel_paths.size(); ++i) {
        const std::string norm_p = norm_rel_ws(rel_paths[i]);
        const uint64_t frn = (i < frns.size()) ? frns[i] : 0;
        auto rows = db.query(
            "SELECT i.workstream_id, i.path, i.actor, w.owner_actor, w.lease_expires_at, w.owner_pid, w.owner_proc_created "
            "FROM workstream_file_intents i "
            "JOIN agent_workstreams w ON w.id = i.workstream_id "
            "WHERE w.status = 'active' AND i.workstream_id != ? AND i.lock_mode = 'exclusive' "
            "AND (i.path = ? COLLATE NOCASE OR (? != 0 AND i.frn = ?))",
            {current_ws_id, norm_p, static_cast<int64_t>(frn), static_cast<int64_t>(frn)});
        for (const auto& r : rows) {
            const std::string exp = r.value("lease_expires_at", "");
            const int64_t pid = r.value("owner_pid", int64_t{0});
            const int64_t pcr = r.value("owner_proc_created", int64_t{0});
            const bool lease_valid = (exp.empty() || exp > cur_iso) && is_owner_process_alive(pid, pcr);
            if (lease_valid) {
                throw std::runtime_error("WORKSTREAM_FILE_CONFLICT: el archivo '" + norm_p +
                                         "' está bloqueado por el workstream activo '" +
                                         r["workstream_id"].get<std::string>() + "' (owner=" +
                                         r["owner_actor"].get<std::string>() + ")");
            }
        }
    }
}

void workstream_auto_record_touch(const Project& p, Database& db, const std::string& rel_path, uint64_t frn,
                                  const std::string& pre_sha, const std::string& post_sha,
                                  const std::string& actor, const std::string& explicit_ws_id) {
    workstream_schema(db);
    const std::string ws_id = explicit_ws_id.empty() ? active_workstream_id(db, actor) : explicit_ws_id;
    if (ws_id.empty()) return;

    const std::string norm_p = norm_rel_ws(rel_path);
    const std::string ts = now();
    if (frn == 0) {
        std::error_code ec;
        const fs::path abs = resolve_project_file_ws(p, norm_p);
        if (fs::exists(native_path(abs), ec)) frn = query_path_frn(abs);
    }

    auto existing = db.query("SELECT pre_sha256, status FROM workstream_file_intents WHERE workstream_id=? AND path=? COLLATE NOCASE",
                             {ws_id, norm_p});
    if (existing.empty()) {
        db.exec(
            "INSERT INTO workstream_file_intents(workstream_id,path,frn,role,lock_mode,status,intent,pre_sha256,post_sha256,last_edit_at,actor)"
            "VALUES(?,?,?,'touched','exclusive','modified','auto_recorded_touch',?,?,?,?)",
            {ws_id, norm_p, static_cast<int64_t>(frn), pre_sha, post_sha, ts, actor.empty() ? "agent" : actor});
    } else {
        const std::string orig_pre = existing[0]["pre_sha256"].get<std::string>().empty()
                                         ? pre_sha
                                         : existing[0]["pre_sha256"].get<std::string>();
        db.exec(
            "UPDATE workstream_file_intents SET frn=CASE WHEN ?!=0 THEN ? ELSE frn END, "
            "status='modified', pre_sha256=?, post_sha256=?, last_edit_at=?, actor=? "
            "WHERE workstream_id=? AND path=? COLLATE NOCASE",
            {static_cast<int64_t>(frn), static_cast<int64_t>(frn), orig_pre, post_sha, ts,
             actor.empty() ? "agent" : actor, ws_id, norm_p});
    }
    db.exec("UPDATE agent_workstreams SET updated_at=?, heartbeat_at=?, revision=revision+1 WHERE id=?",
            {ts, ts, ws_id});
}

Json workstream_detect_and_reconcile(const Project& p, Database& db) {
    workstream_schema(db);
    const std::string ts = now();
    Json recovered_workstreams = Json::array();
    Json recovered_operations = Json::array();

    // 1. Check active workstreams whose owner process died unexpectedly (power cut / crash / TerminateProcess)
    auto active_rows = db.query(
        "SELECT id, title, goal, owner_actor, owner_pid, owner_proc_created, lease_expires_at, base_snapshot_id "
        "FROM agent_workstreams WHERE status='active'");
    for (const auto& r : active_rows) {
        const std::string ws_id = r["id"].get<std::string>();
        const int64_t pid = r["owner_pid"].get<int64_t>();
        const int64_t pcr = r["owner_proc_created"].get<int64_t>();
        if (pid > 0 && !is_owner_process_alive(pid, pcr)) {
            SyncFullGuard sfg(db);
            db.exec("UPDATE agent_workstreams SET status='interrupted_recoverable', updated_at=?, revision=revision+1 WHERE id=?",
                    {ts, ws_id});
            const int64_t next_seq = db.integer("SELECT COALESCE(MAX(seq),0)+1 FROM workstream_checkpoints WHERE workstream_id=?", {ws_id});
            const auto git_sum = git_repo_summary_cached(p, db);
            db.exec(
                "INSERT INTO workstream_checkpoints(workstream_id,seq,at_iso,actor,trigger_kind,summary,git_head,snapshot_id,state_json,revision)"
                "VALUES(?,?,?,?,?,?,?,?,?,1)",
                {ws_id, next_seq, ts, "watchdog", "crash_detected",
                 "PID propietario " + std::to_string(pid) + " terminó inesperadamente; workstream marcado como interrupted_recoverable",
                 git_sum.value("head_commit", ""), r.value("base_snapshot_id", ""),
                 Json{{"dead_pid", pid}}.dump()});
            recovered_workstreams.push_back(ws_id);
        }
    }

    // 2. Reconcile any incomplete 7-stage operations (< COMMITTED) whose PID is no longer alive
    auto open_ops = db.query(
        "SELECT op_id, workstream_id, tool_name, stage, actor, pid, proc_created, files_json, pre_blobs_json, post_blobs_json "
        "FROM workstream_operations WHERE stage NOT IN('COMMITTED','FAILED','RECOVERY')");
    for (const auto& op : open_ops) {
        const int64_t pid = op["pid"].get<int64_t>();
        const int64_t pcr = op["proc_created"].get<int64_t>();
        if (pid > 0 && is_owner_process_alive(pid, pcr)) continue;

        const std::string op_id = op["op_id"].get<std::string>();
        const std::string stage = op["stage"].get<std::string>();
        const std::string ws_id = op["workstream_id"].get<std::string>();
        const Json files = Json::parse(op["files_json"].get<std::string>(), nullptr, false);
        std::string resolution = "aborted_pre_apply";

        if (stage == "APPLIED" || stage == "INDEXED") {
            // Disk writes finished; ensure index is synchronized and promote to COMMITTED
            if (files.is_array()) {
                for (const auto& f : files) {
                    const std::string rel = f.is_string() ? f.get<std::string>() : f.value("path", "");
                    if (!rel.empty()) fs_sync_single_path(p, db, resolve_project_file_ws(p, rel), true);
                }
            }
            advance_durable_operation(db, op_id, "COMMITTED");
            resolution = "completed_post_apply_indexing";
        } else {
            advance_durable_operation(db, op_id, "RECOVERY", {{"error", "Interrumpido en etapa " + stage}});
            resolution = "marked_for_workstream_recovery_from_" + stage;
        }
        recovered_operations.push_back({
            {"op_id", op_id},
            {"workstream_id", ws_id},
            {"interrupted_stage", stage},
            {"resolution", resolution}
        });
    }

    // 3. Classify files for all non-terminal workstreams against live disk state
    auto non_term = db.query(
        "SELECT id, title, goal, status, owner_actor, owner_pid, base_git_head, base_snapshot_id, current_step, next_steps, resume_instructions, updated_at "
        "FROM agent_workstreams WHERE status IN('active','paused','handed_off','interrupted_recoverable') ORDER BY updated_at DESC");

    Json summaries = Json::array();
    for (const auto& ws : non_term) {
        const std::string ws_id = ws["id"].get<std::string>();
        auto intents = db.query(
            "SELECT path, frn, role, status, intent, pre_sha256, post_sha256, last_edit_at, actor "
            "FROM workstream_file_intents WHERE workstream_id=? ORDER BY path",
            {ws_id});

        Json completed_before_crash = Json::array();
        Json partially_modified_on_disk = Json::array();
        Json planned_not_yet_touched = Json::array();
        Json modified_externally = Json::array();

        for (const auto& it : intents) {
            const std::string rel = it["path"].get<std::string>();
            const std::string st = it["status"].get<std::string>();
            const std::string pre_s = it["pre_sha256"].get<std::string>();
            const std::string post_s = it["post_sha256"].get<std::string>();
            const fs::path abs = resolve_project_file_ws(p, rel);
            std::error_code ec;
            const bool exists = fs::is_regular_file(native_path(abs), ec);
            const std::string disk_s = exists ? hash_file(abs) : "";

            if (!post_s.empty() && disk_s == post_s) {
                completed_before_crash.push_back(rel);
            } else if ((st == "planned" || post_s.empty()) && (pre_s.empty() || disk_s == pre_s)) {
                planned_not_yet_touched.push_back(rel);
            } else if (ws["status"].get<std::string>() == "interrupted_recoverable" && post_s.empty() && !pre_s.empty() && disk_s != pre_s) {
                partially_modified_on_disk.push_back(rel);
            } else if (!disk_s.empty() && disk_s != pre_s && disk_s != post_s) {
                modified_externally.push_back({
                    {"path", rel},
                    {"pre_sha256", pre_s},
                    {"post_sha256", post_s},
                    {"disk_sha256", disk_s}
                });
                partially_modified_on_disk.push_back(rel);
            } else {
                planned_not_yet_touched.push_back(rel);
            }
        }

        std::string badge = "CONFIRMED";
        if (!modified_externally.empty()) badge = "MODIFIED_EXTERNALLY";
        else if (ws["status"].get<std::string>() == "interrupted_recoverable") badge = "INTERRUPTED_RECOVERABLE";

        summaries.push_back({
            {"workstream_id", ws_id},
            {"title", ws["title"]},
            {"goal", ws["goal"]},
            {"status", ws["status"]},
            {"owner_actor", ws["owner_actor"]},
            {"base_git_head", ws["base_git_head"]},
            {"base_snapshot_id", ws["base_snapshot_id"]},
            {"current_step", ws["current_step"]},
            {"resume_instructions", ws["resume_instructions"]},
            {"updated_at", ws["updated_at"]},
            {"verification_badge", badge},
            {"completed_before_crash", std::move(completed_before_crash)},
            {"partially_modified_on_disk", std::move(partially_modified_on_disk)},
            {"planned_not_yet_touched", std::move(planned_not_yet_touched)},
            {"modified_externally", std::move(modified_externally)}
        });
    }

    return {
        {"recovered_workstreams", std::move(recovered_workstreams)},
        {"recovered_operations", std::move(recovered_operations)},
        {"open_workstreams", std::move(summaries)}
    };
}

Json workstream_prune_ephemeral_snapshots(const Project& p, Database& db,
                                          int max_ephemeral_keep,
                                          double max_ephemeral_days,
                                          bool dry_run) {
    workstream_schema(db);
    (void)max_ephemeral_days;
    // P4 hard guarantee: NEVER prune a snapshot if pinned=1 OR if its workstream is non-terminal
    auto ProtectedCount = db.integer(
        "SELECT COUNT(*) FROM workstream_snapshots s "
        "LEFT JOIN agent_workstreams w ON w.id = s.workstream_id "
        "WHERE s.pinned != 0 OR w.status IN('active','paused','handed_off','interrupted_recoverable')");

    auto eph_rows = db.query(
        "SELECT s.snapshot_id, s.workstream_id, s.at_iso, s.total_bytes "
        "FROM workstream_snapshots s "
        "LEFT JOIN agent_workstreams w ON w.id = s.workstream_id "
        "WHERE s.pinned = 0 AND (s.retention_class = 'ephemeral' OR s.workstream_id = '' OR w.status IN('completed','rolled_back','abandoned') OR w.id IS NULL) "
        "ORDER BY s.at_iso DESC, s.rowid DESC");

    Json pruned_snapshots = Json::array();
    Json recycled_blobs = Json::array();
    int64_t bytes_freed = 0;

    for (size_t i = static_cast<size_t>(std::max(0, max_ephemeral_keep)); i < eph_rows.size(); ++i) {
        const std::string sid = eph_rows[i]["snapshot_id"].get<std::string>();
        pruned_snapshots.push_back(sid);
        if (!dry_run) {
            db.exec("DELETE FROM snapshot_blob_refs WHERE snapshot_id=?", {sid});
            db.exec("DELETE FROM workstream_snapshots WHERE snapshot_id=?", {sid});
        }
    }

    // Garbage-collect unreferenced blobs (ref_count == 0 in snapshot_blob_refs) to the Windows Recycle Bin
    const fs::path bdir = blob_dir_for_project(p);
    std::error_code ec;
    if (fs::is_directory(native_path(bdir), ec)) {
        for (const auto& entry : fs::directory_iterator(native_path(bdir), ec)) {
            if (!entry.is_regular_file(ec)) continue;
            const std::string sha = utf8(entry.path().filename().wstring());
            if (sha.find(".tmp-") != std::string::npos) continue;
            const int64_t refs = db.integer("SELECT COUNT(*) FROM snapshot_blob_refs WHERE sha256=?", {sha});
            if (refs == 0) {
                const int64_t sz = static_cast<int64_t>(entry.file_size(ec));
                recycled_blobs.push_back(sha);
                bytes_freed += sz;
                if (!dry_run) {
                    send_file_to_recycle_bin(entry.path());
                }
            }
        }
    }

    return {
        {"dry_run", dry_run},
        {"protected_pinned_snapshots", ProtectedCount},
        {"pruned_snapshots_count", pruned_snapshots.size()},
        {"pruned_snapshots", std::move(pruned_snapshots)},
        {"recycled_blobs_count", recycled_blobs.size()},
        {"recycled_blobs", std::move(recycled_blobs)},
        {"bytes_freed", bytes_freed}
    };
}

Json tool_workstream(const Project& p, Database& db, const Json& args) {
    workstream_schema(db);
    const std::string action = args.value("action", args.value("op", "status"));
    const std::string actor = args.value("actor", "agent");
    const std::string idemp_key = args.value("idempotency_key", "");
    const std::string args_hash = idemp_key.empty() ? "" : sha256(args.dump());

    if (auto replay = check_idempotency_ledger(db, idemp_key, "workstream:" + action, args_hash)) {
        return *replay;
    }

    if (action == "start") {
        const std::string title = args.value("title", args.value("goal", "Workstream"));
        const std::string goal = args.value("goal", title);
        const std::string ws_id = args.value("workstream_id", "ws-" + unique_id());
        const int lease_sec = std::clamp(args.value("lease_seconds", 3600), 60, 86400 * 7);
        const bool simulate_dead_pid = args.value("simulate_dead_pid", false);
        const int64_t owner_pid = simulate_dead_pid ? int64_t{999999} : static_cast<int64_t>(GetCurrentProcessId());
        const int64_t proc_created = simulate_dead_pid ? int64_t{12345} : query_process_create_time(static_cast<DWORD>(owner_pid));

        std::vector<std::string> planned_rels;
        std::vector<fs::path> planned_abs;
        std::vector<uint64_t> planned_frns;
        if (args.contains("planned_files") && args["planned_files"].is_array()) {
            for (const auto& pf : args["planned_files"]) {
                const std::string rel = norm_rel_ws(pf.is_string() ? pf.get<std::string>() : pf.value("path", ""));
                if (rel.empty()) continue;
                const fs::path abs = resolve_project_file_ws(p, rel);
                planned_rels.push_back(rel);
                planned_abs.push_back(abs);
                std::error_code ec;
                planned_frns.push_back(fs::exists(native_path(abs), ec) ? query_path_frn(abs) : 0);
            }
        }

        // Multi-agent file lock conflict check
        check_workstream_file_conflicts(db, ws_id, planned_rels, planned_frns);

        const auto git_sum = git_repo_summary_cached(p, db);
        const std::string base_snap = planned_abs.empty()
                                          ? ""
                                          : create_content_snapshot(p, db, planned_abs, ws_id, "base_" + ws_id, actor, true);
        const std::string ts = now();
        const std::string exp = iso_after_seconds(lease_sec);

        SyncFullGuard sfg(db);
        db.exec(
            "INSERT OR REPLACE INTO agent_workstreams("
            "id,title,goal,status,owner_actor,owner_session_id,owner_pid,owner_proc_created,"
            "lease_expires_at,heartbeat_at,created_at,updated_at,base_git_head,base_git_branch,base_snapshot_id,"
            "current_step,next_steps,resume_instructions,revision)"
            "VALUES(?,?,?,'active',?,?,?,?,?,?,?,?,?,?,?,?,?,?,1)",
            {ws_id, title, goal, actor, args.value("session_id", ""), owner_pid, proc_created,
             exp, ts, ts, ts, git_sum.value("head_commit", ""), git_sum.value("branch", ""), base_snap,
             args.value("current_step", "iniciado"),
             args.value("next_steps", Json::array()).dump(),
             args.value("resume_instructions", "")});

        for (size_t i = 0; i < planned_rels.size(); ++i) {
            std::error_code ec;
            const std::string pre_sha = fs::is_regular_file(native_path(planned_abs[i]), ec) ? hash_file(planned_abs[i]) : "";
            db.exec(
                "INSERT OR REPLACE INTO workstream_file_intents(workstream_id,path,frn,role,lock_mode,status,intent,pre_sha256,post_sha256,last_edit_at,actor)"
                "VALUES(?,?,?,'planned',?,'planned',?,?, '', ?, ?)",
                {ws_id, planned_rels[i], static_cast<int64_t>(planned_frns[i]),
                 args.value("lock_mode", "exclusive"), args.value("intent", goal), pre_sha, ts, actor});
        }

        db.exec(
            "INSERT INTO workstream_checkpoints(workstream_id,seq,at_iso,actor,trigger_kind,summary,git_head,snapshot_id,state_json,revision)"
            "VALUES(?,1,?,?,'start',?,?,?,?,1)",
            {ws_id, ts, actor, "Workstream iniciado: " + title, git_sum.value("head_commit", ""), base_snap,
             Json{{"planned_files", planned_rels}}.dump()});

        Json resp = {
            {"schema", "migratool.workstream/1"},
            {"action", "start"},
            {"ok", true},
            {"workstream_id", ws_id},
            {"status", "active"},
            {"base_snapshot_id", base_snap},
            {"base_git_head", git_sum.value("head_commit", "")},
            {"planned_files_count", planned_rels.size()},
            {"lease_expires_at", exp}
        };
        store_idempotency_ledger(db, idemp_key, "workstream:start", args_hash, resp);
        return resp;
    }

    if (action == "checkpoint") {
        const std::string ws_id = args.value("workstream_id", active_workstream_id(db, actor));
        if (ws_id.empty()) throw std::runtime_error("workstream(checkpoint) requiere workstream_id activo");
        const std::string summary = args.value("summary", "Checkpoint durable");
        const std::string ts = now();
        const auto git_sum = git_repo_summary_cached(p, db);

        std::vector<fs::path> snap_targets;
        auto intents = db.query("SELECT path FROM workstream_file_intents WHERE workstream_id=?", {ws_id});
        for (const auto& it : intents) snap_targets.push_back(resolve_project_file_ws(p, it["path"].get<std::string>()));
        const std::string snap_id = (!snap_targets.empty() && args.value("create_snapshot", true))
                                        ? create_content_snapshot(p, db, snap_targets, ws_id, "ckpt_" + ws_id, actor, true)
                                        : "";

        SyncFullGuard sfg(db);
        const int64_t seq = db.integer("SELECT COALESCE(MAX(seq),0)+1 FROM workstream_checkpoints WHERE workstream_id=?", {ws_id});
        db.exec(
            "UPDATE agent_workstreams SET updated_at=?, heartbeat_at=?, "
            "current_step=CASE WHEN ? != '' THEN ? ELSE current_step END, "
            "resume_instructions=CASE WHEN ? != '' THEN ? ELSE resume_instructions END, "
            "revision=revision+1 WHERE id=?",
            {ts, ts, args.value("current_step", ""), args.value("current_step", ""),
             args.value("resume_instructions", ""), args.value("resume_instructions", ""), ws_id});

        db.exec(
            "INSERT INTO workstream_checkpoints(workstream_id,seq,at_iso,actor,trigger_kind,summary,git_head,snapshot_id,state_json,revision)"
            "VALUES(?,?,?,?,?,?,?,?,?,?)",
            {ws_id, seq, ts, actor, args.value("trigger_kind", "manual"), summary,
             git_sum.value("head_commit", ""), snap_id, args.value("state", Json::object()).dump(), seq});

        Json resp = {
            {"schema", "migratool.workstream/1"},
            {"action", "checkpoint"},
            {"ok", true},
            {"workstream_id", ws_id},
            {"seq", seq},
            {"snapshot_id", snap_id},
            {"git_head", git_sum.value("head_commit", "")}
        };
        store_idempotency_ledger(db, idemp_key, "workstream:checkpoint", args_hash, resp);
        return resp;
    }

    if (action == "pause" || action == "handoff") {
        const std::string ws_id = args.value("workstream_id", active_workstream_id(db, actor));
        if (ws_id.empty()) throw std::runtime_error("workstream(" + action + ") requiere workstream_id");
        const std::string new_st = (action == "handoff") ? "handed_off" : "paused";
        const std::string ts = now();
        const std::string summary = args.value("summary", args.value("resume_instructions", "Sesión en " + new_st));
        SyncFullGuard sfg(db);
        db.exec("UPDATE agent_workstreams SET status=?, resume_instructions=?, updated_at=?, revision=revision+1 WHERE id=?",
                {new_st, summary, ts, ws_id});
        const int64_t seq = db.integer("SELECT COALESCE(MAX(seq),0)+1 FROM workstream_checkpoints WHERE workstream_id=?", {ws_id});
        db.exec("INSERT INTO workstream_checkpoints(workstream_id,seq,at_iso,actor,trigger_kind,summary,state_json,revision) VALUES(?,?,?,?,?,?,?,?)",
                {ws_id, seq, ts, actor, action, summary, args.dump(), seq});
        if (action == "handoff") {
            db.exec("INSERT INTO agent_messages(at_iso,from_actor,to_actor,workstream_id,kind,subject,body) VALUES(?,?,?,?,?,?,?)",
                    {ts, actor, args.value("to_actor", "all"), ws_id, "handoff", "Handoff " + ws_id, summary});
        }
        return {{"schema", "migratool.workstream/1"}, {"action", action}, {"ok", true}, {"workstream_id", ws_id}, {"status", new_st}};
    }

    if (action == "takeover" || action == "resume") {
        workstream_detect_and_reconcile(p, db);
        std::string ws_id = args.value("workstream_id", "");
        if (ws_id.empty()) {
            auto r = db.query("SELECT id FROM agent_workstreams WHERE status IN('interrupted_recoverable','handed_off','paused') ORDER BY updated_at DESC LIMIT 1");
            if (!r.empty()) ws_id = r[0]["id"].get<std::string>();
        }
        if (ws_id.empty()) throw std::runtime_error("No hay workstream interrumpido/pausado para " + action);

        // Check for external modifications made after the workstream's last recorded edit
        auto intents = db.query("SELECT path, status, pre_sha256, post_sha256 FROM workstream_file_intents WHERE workstream_id=?", {ws_id});
        Json external_conflicts = Json::array();
        for (const auto& it : intents) {
            const std::string rel = it["path"].get<std::string>();
            const std::string pre_s = it["pre_sha256"].get<std::string>();
            const std::string post_s = it["post_sha256"].get<std::string>();
            const fs::path abs = resolve_project_file_ws(p, rel);
            std::error_code ec;
            const std::string disk_s = fs::is_regular_file(native_path(abs), ec) ? hash_file(abs) : "";
            if (!disk_s.empty() && disk_s != pre_s && (!post_s.empty() && disk_s != post_s)) {
                external_conflicts.push_back({
                    {"path", rel},
                    {"pre_sha256", pre_s},
                    {"post_sha256", post_s},
                    {"disk_sha256", disk_s}
                });
            }
        }

        const std::string resolution = args.value("resolution", "");
        if (!external_conflicts.empty() && resolution.empty()) {
            throw std::runtime_error("CONFLICT_EXTERNAL_EDIT: se detectaron ediciones externas en " +
                                     std::to_string(external_conflicts.size()) +
                                     " archivo(s); especifique resolution='accept_disk'|'restore_snapshot'|'split_workstream': " +
                                     external_conflicts.dump());
        }

        if (!external_conflicts.empty() && resolution == "accept_disk") {
            for (const auto& c : external_conflicts) {
                db.exec("UPDATE workstream_file_intents SET post_sha256=?, last_edit_at=? WHERE workstream_id=? AND path=? COLLATE NOCASE",
                        {c["disk_sha256"].get<std::string>(), now(), ws_id, c["path"].get<std::string>()});
            }
        } else if (!external_conflicts.empty() && resolution == "restore_snapshot") {
            auto ws_r = db.query("SELECT base_snapshot_id FROM agent_workstreams WHERE id=?", {ws_id});
            if (!ws_r.empty() && !ws_r[0]["base_snapshot_id"].get<std::string>().empty()) {
                restore_content_snapshot(p, db, ws_r[0]["base_snapshot_id"].get<std::string>(), actor, false, "backup_and_force");
            }
        }

        const DWORD pid = GetCurrentProcessId();
        const int64_t pcr = query_process_create_time(pid);
        const std::string ts = now();
        const std::string exp = iso_after_seconds(args.value("lease_seconds", 3600));
        SyncFullGuard sfg(db);
        db.exec(
            "UPDATE agent_workstreams SET status='active', owner_actor=?, owner_pid=?, owner_proc_created=?, "
            "lease_expires_at=?, heartbeat_at=?, updated_at=?, revision=revision+1 WHERE id=?",
            {actor, static_cast<int64_t>(pid), pcr, exp, ts, ts, ws_id});
        const int64_t seq = db.integer("SELECT COALESCE(MAX(seq),0)+1 FROM workstream_checkpoints WHERE workstream_id=?", {ws_id});
        db.exec(
            "INSERT INTO workstream_checkpoints(workstream_id,seq,at_iso,actor,trigger_kind,summary,state_json,revision)"
            "VALUES(?,?,?,?,?,?,?,?)",
            {ws_id, seq, ts, actor, action, "Workstream retomado por " + actor,
             Json{{"resolution", resolution}, {"conflicts_resolved", external_conflicts.size()}}.dump(), seq});

        return {
            {"schema", "migratool.workstream/1"},
            {"action", action},
            {"ok", true},
            {"workstream_id", ws_id},
            {"status", "active"},
            {"new_owner_actor", actor},
            {"resolution_applied", resolution},
            {"external_conflicts_count", external_conflicts.size()}
        };
    }

    if (action == "rollback") {
        const std::string ws_id = args.value("workstream_id", active_workstream_id(db, actor));
        if (ws_id.empty()) throw std::runtime_error("workstream(rollback) requiere workstream_id");
        auto ws_rows = db.query("SELECT base_snapshot_id FROM agent_workstreams WHERE id=?", {ws_id});
        if (ws_rows.empty()) throw std::runtime_error("WORKSTREAM_NOT_FOUND: " + ws_id);

        const std::string snap_id = args.value("snapshot_id", ws_rows[0]["base_snapshot_id"].get<std::string>());
        const bool preserve_external = args.value("preserve_external_edits", true);
        const std::string policy = preserve_external ? "skip_conflicted" : args.value("conflict_policy", "backup_and_force");
        Json restore_res = Json::object();
        if (!snap_id.empty()) {
            restore_res = restore_content_snapshot(p, db, snap_id, actor, args.value("dry_run", false), policy);
        }

        if (!args.value("dry_run", false)) {
            const std::string ts = now();
            SyncFullGuard sfg(db);
            db.exec("UPDATE agent_workstreams SET status='rolled_back', closed_at=?, updated_at=?, revision=revision+1 WHERE id=?",
                    {ts, ts, ws_id});
            // Unpin non-manual snapshots once workstream reaches terminal rolled_back state
            db.exec("UPDATE workstream_snapshots SET pinned=0, retention_class='ephemeral' WHERE workstream_id=? AND retention_class='active_guard'",
                    {ws_id});
        }
        return {
            {"schema", "migratool.workstream/1"},
            {"action", "rollback"},
            {"ok", true},
            {"workstream_id", ws_id},
            {"status", "rolled_back"},
            {"restore", std::move(restore_res)}
        };
    }

    if (action == "close") {
        const std::string ws_id = args.value("workstream_id", active_workstream_id(db, actor));
        if (ws_id.empty()) throw std::runtime_error("workstream(close) requiere workstream_id");
        const std::string final_status = args.value("status", "completed");
        const std::string ts = now();
        SyncFullGuard sfg(db);
        db.exec("UPDATE agent_workstreams SET status=?, closed_at=?, updated_at=?, revision=revision+1 WHERE id=?",
                {final_status, ts, ts, ws_id});
        if (final_status == "completed" || final_status == "abandoned" || final_status == "rolled_back") {
            db.exec("UPDATE workstream_snapshots SET pinned=0, retention_class='ephemeral' WHERE workstream_id=? AND retention_class='active_guard'",
                    {ws_id});
        }
        return {{"schema", "migratool.workstream/1"}, {"action", "close"}, {"ok", true}, {"workstream_id", ws_id}, {"status", final_status}};
    }

    if (action == "snapshot_create") {
        const std::string ws_id = args.value("workstream_id", active_workstream_id(db, actor));
        std::vector<fs::path> targets;
        if (args.contains("files") && args["files"].is_array()) {
            for (const auto& f : args["files"]) targets.push_back(resolve_project_file_ws(p, f.get<std::string>()));
        } else if (!ws_id.empty()) {
            auto intents = db.query("SELECT path FROM workstream_file_intents WHERE workstream_id=?", {ws_id});
            for (const auto& it : intents) targets.push_back(resolve_project_file_ws(p, it["path"].get<std::string>()));
        }
        const bool pinned = args.value("pinned", !ws_id.empty());
        const std::string sid = create_content_snapshot(p, db, targets, ws_id, args.value("label", "manual"), actor, pinned);
        return {{"schema", "migratool.workstream/1"}, {"action", "snapshot_create"}, {"ok", true}, {"snapshot_id", sid}, {"pinned", pinned}};
    }

    if (action == "snapshot_restore") {
        const std::string sid = args.at("snapshot_id").get<std::string>();
        auto res = restore_content_snapshot(p, db, sid, actor, args.value("dry_run", false), args.value("conflict_policy", "abort"));
        res["schema"] = "migratool.workstream/1";
        res["action"] = "snapshot_restore";
        return res;
    }

    if (action == "snapshot_diff") {
        const std::string sid = args.at("snapshot_id").get<std::string>();
        auto rows = db.query("SELECT files_json FROM workstream_snapshots WHERE snapshot_id=?", {sid});
        if (rows.empty()) throw std::runtime_error("SNAPSHOT_NOT_FOUND: " + sid);
        const Json files = Json::parse(rows[0]["files_json"].get<std::string>(), nullptr, false);
        Json diffs = Json::array();
        for (const auto& f : files) {
            const std::string rel = f.value("path", "");
            const std::string snap_sha = f.value("sha256", "");
            const fs::path abs = resolve_project_file_ws(p, rel);
            std::error_code ec;
            const bool exists = fs::is_regular_file(native_path(abs), ec);
            const std::string disk_sha = exists ? hash_file(abs) : "";
            if (disk_sha != snap_sha) {
                diffs.push_back({{"path", rel}, {"snapshot_sha256", snap_sha}, {"disk_sha256", disk_sha}, {"exists_on_disk", exists}});
            }
        }
        return {{"schema", "migratool.workstream/1"}, {"action", "snapshot_diff"}, {"snapshot_id", sid}, {"changed_count", diffs.size()}, {"changed", std::move(diffs)}};
    }

    if (action == "message") {
        const std::string body = args.at("body").get<std::string>();
        const std::string ws_id = args.value("workstream_id", active_workstream_id(db, actor));
        db.exec("INSERT INTO agent_messages(at_iso,from_actor,to_actor,workstream_id,kind,subject,body) VALUES(?,?,?,?,?,?,?)",
                {now(), actor, args.value("to_actor", "all"), ws_id, args.value("kind", "note"), args.value("subject", ""), body});
        return {{"schema", "migratool.workstream/1"}, {"action", "message"}, {"ok", true}};
    }

    if (action == "inbox") {
        auto msgs = db.query("SELECT id, at_iso, from_actor, to_actor, workstream_id, kind, subject, body, acked_by FROM agent_messages ORDER BY id DESC LIMIT 50");
        return {{"schema", "migratool.workstream/1"}, {"action", "inbox"}, {"count", msgs.size()}, {"messages", std::move(msgs)}};
    }

    if (action == "list") {
        workstream_detect_and_reconcile(p, db);
        auto rows = db.query("SELECT id, title, goal, status, owner_actor, owner_pid, base_git_head, base_snapshot_id, current_step, updated_at, revision FROM agent_workstreams ORDER BY updated_at DESC LIMIT 50");
        return {{"schema", "migratool.workstream/1"}, {"action", "list"}, {"count", rows.size()}, {"items", std::move(rows)}};
    }

    // Default: status (runs live reconciliation and returns detailed state)
    auto rec = workstream_detect_and_reconcile(p, db);
    rec["schema"] = "migratool.workstream/1";
    rec["action"] = "status";
    return rec;
}

} // namespace mt
