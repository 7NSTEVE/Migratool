#include "migratool/path_lineage.hpp"
#include "migratool/fs_index.hpp"
#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <unordered_set>
#include <vector>

namespace mt {
namespace {

std::string norm_fwd_path(std::string p) {
    std::replace(p.begin(), p.end(), '\\', '/');
    while (p.starts_with("./")) p.erase(0, 2);
    while (!p.empty() && p.front() == '/') p.erase(p.begin());
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    return p;
}

bool has_column(Database& db, const std::string& table, const std::string& col) {
    auto rows = db.query("SELECT name FROM pragma_table_info(?)", {table});
    for (const auto& r : rows) {
        if (r.value("name", "") == col) return true;
    }
    return false;
}

} // namespace

void path_lineage_schema(Database& db) {
    fs_schema(db);
    if (db.integer("SELECT COUNT(*) FROM meta WHERE key='path_lineage_schema' AND value='1'") != 0) return;
    const bool need_tx = (sqlite3_get_autocommit(db.native_handle()) != 0);
    if (need_tx) db.begin();
    try {
        if (db.integer("SELECT COUNT(*) FROM meta WHERE key='path_lineage_schema' AND value='1'") == 0) {
            if (!has_column(db, "fs_roots", "last_full_scan_ns")) {
                db.exec("ALTER TABLE fs_roots ADD COLUMN last_full_scan_ns INTEGER NOT NULL DEFAULT 0");
            }
            if (!has_column(db, "fs_roots", "last_full_scan_at")) {
                db.exec("ALTER TABLE fs_roots ADD COLUMN last_full_scan_at TEXT NOT NULL DEFAULT ''");
            }
            if (!has_column(db, "fs_roots", "last_incremental_at")) {
                db.exec("ALTER TABLE fs_roots ADD COLUMN last_incremental_at TEXT NOT NULL DEFAULT ''");
            }
            if (!has_column(db, "fs_roots", "last_scan_duration_ms")) {
                db.exec("ALTER TABLE fs_roots ADD COLUMN last_scan_duration_ms INTEGER NOT NULL DEFAULT 0");
            }

            char* err = nullptr;
            const char* sql = R"sql(
CREATE TABLE IF NOT EXISTS fs_scan_history (
  id                INTEGER PRIMARY KEY AUTOINCREMENT,
  root_id           INTEGER NOT NULL,
  scan_kind         TEXT NOT NULL,
  engine            TEXT NOT NULL,
  started_at        TEXT NOT NULL,
  finished_at       TEXT NOT NULL,
  duration_ms       INTEGER NOT NULL DEFAULT 0,
  total_files       INTEGER NOT NULL DEFAULT 0,
  total_dirs        INTEGER NOT NULL DEFAULT 0,
  total_bytes       INTEGER NOT NULL DEFAULT 0,
  added             INTEGER NOT NULL DEFAULT 0,
  modified          INTEGER NOT NULL DEFAULT 0,
  moved             INTEGER NOT NULL DEFAULT 0,
  deleted           INTEGER NOT NULL DEFAULT 0,
  usn_discontinuity INTEGER NOT NULL DEFAULT 0,
  actor             TEXT NOT NULL DEFAULT 'system'
);
CREATE INDEX IF NOT EXISTS idx_fs_scan_history_root ON fs_scan_history(root_id, id DESC);

CREATE TABLE IF NOT EXISTS path_lineage (
  id            INTEGER PRIMARY KEY AUTOINCREMENT,
  at_ns         INTEGER NOT NULL,
  at_iso        TEXT NOT NULL,
  root_id       INTEGER NOT NULL DEFAULT 1,
  frn           INTEGER NOT NULL DEFAULT 0,
  node_kind     TEXT NOT NULL DEFAULT 'file',
  op            TEXT NOT NULL,
  old_path      TEXT NOT NULL DEFAULT '',
  new_path      TEXT NOT NULL DEFAULT '',
  old_sha256    TEXT NOT NULL DEFAULT '',
  new_sha256    TEXT NOT NULL DEFAULT '',
  size_bytes    INTEGER NOT NULL DEFAULT 0,
  actor         TEXT NOT NULL DEFAULT 'system',
  source        TEXT NOT NULL DEFAULT 'fs_sync',
  confidence    TEXT NOT NULL DEFAULT 'exact_transactional',
  workstream_id TEXT NOT NULL DEFAULT '',
  reason        TEXT NOT NULL DEFAULT ''
);
CREATE INDEX IF NOT EXISTS idx_path_lineage_frn ON path_lineage(frn, id DESC);
CREATE INDEX IF NOT EXISTS idx_path_lineage_old ON path_lineage(old_path COLLATE NOCASE, id DESC);
CREATE INDEX IF NOT EXISTS idx_path_lineage_new ON path_lineage(new_path COLLATE NOCASE, id DESC);
CREATE INDEX IF NOT EXISTS idx_path_lineage_at  ON path_lineage(at_ns DESC, id DESC);

CREATE TRIGGER IF NOT EXISTS path_lineage_no_update
BEFORE UPDATE ON path_lineage
BEGIN
  SELECT RAISE(ABORT, 'immutable path_lineage');
END;

CREATE TRIGGER IF NOT EXISTS path_lineage_no_delete
BEFORE DELETE ON path_lineage
BEGIN
  SELECT RAISE(ABORT, 'immutable path_lineage');
END;
)sql";
            if (sqlite3_exec(db.native_handle(), sql, nullptr, nullptr, &err) != SQLITE_OK) {
                std::string msg = err ? err : sqlite3_errmsg(db.native_handle());
                if (err) sqlite3_free(err);
                throw std::runtime_error(msg);
            }
            db.exec("INSERT OR REPLACE INTO meta(key,value) VALUES('path_lineage_schema','1')");
        }
        if (need_tx) db.commit();
    } catch (...) {
        if (need_tx) db.rollback();
        throw;
    }
}

void record_path_event(Database& db, int64_t root_id, uint64_t frn, const std::string& node_kind,
                       const std::string& op, const std::string& old_path, const std::string& new_path,
                       int64_t size_bytes, const std::string& actor,
                       const std::string& source, const std::string& confidence,
                       const std::string& workstream_id,
                       const std::string& old_sha, const std::string& new_sha,
                       const std::string& reason) {
    path_lineage_schema(db);
    const std::string old_norm = norm_fwd_path(old_path);
    const std::string new_norm = norm_fwd_path(new_path);
    std::string eff_actor = actor.empty() ? "system" : actor;
    if ((source == "usn_journal" || source == "fs_reconcile" || source == "watcher") &&
        (eff_actor == "system" || eff_actor.empty()) && workstream_id.empty()) {
        eff_actor = "external_unattributed";
    }
    std::string eff_conf = confidence;
    if (eff_conf.empty()) {
        if (source == "usn_journal") eff_conf = "usn_exact";
        else if (source == "fs_reconcile") eff_conf = "reconciled_inferred";
        else eff_conf = "exact_transactional";
    }
    db.exec(
        "INSERT INTO path_lineage(at_ns,at_iso,root_id,frn,node_kind,op,old_path,new_path,old_sha256,new_sha256,size_bytes,actor,source,confidence,workstream_id,reason)"
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
        {now_ns(), now(), root_id, static_cast<int64_t>(frn), node_kind, op,
         old_norm, new_norm, old_sha, new_sha, size_bytes, eff_actor, source, eff_conf, workstream_id, reason});
}

void record_usn_journal_gap(Database& db, int64_t root_id, uint64_t old_journal_id, uint64_t new_journal_id,
                            int64_t lost_from_usn, int64_t resumed_at_usn, const std::string& reason) {
    path_lineage_schema(db);
    const std::string detail = "USN gap (" + reason + "): old_jid=" + std::to_string(old_journal_id) +
                               " new_jid=" + std::to_string(new_journal_id) +
                               " lost_usn=[" + std::to_string(lost_from_usn) + ".." + std::to_string(resumed_at_usn) + "]";
    db.exec(
        "INSERT INTO path_lineage(at_ns,at_iso,root_id,frn,node_kind,op,old_path,new_path,old_sha256,new_sha256,size_bytes,actor,source,confidence,workstream_id,reason)"
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
        {now_ns(), now(), root_id, 0, "volume", "journal_gap", "", "", "", "", 0,
         "external_unattributed", "usn_journal", "journal_gap", "", detail});
}

void record_scan_telemetry(Database& db, int64_t root_id, const std::string& scan_kind,
                           const std::string& engine, const std::string& started_at,
                           int64_t duration_ms, int64_t files, int64_t dirs, int64_t bytes,
                           int64_t added, int64_t modified, int64_t moved, int64_t deleted,
                           bool usn_discontinuity, const std::string& actor) {
    path_lineage_schema(db);
    const std::string fin = now();
    const int64_t n_ns = now_ns();
    db.exec(
        "INSERT INTO fs_scan_history(root_id,scan_kind,engine,started_at,finished_at,duration_ms,total_files,total_dirs,total_bytes,added,modified,moved,deleted,usn_discontinuity,actor)"
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
        {root_id, scan_kind, engine, started_at.empty() ? fin : started_at, fin,
         duration_ms, files, dirs, bytes, added, modified, moved, deleted,
         usn_discontinuity ? 1 : 0, actor.empty() ? "system" : actor});

    if (scan_kind == "full") {
        db.exec("UPDATE fs_roots SET last_full_scan_ns=?, last_full_scan_at=?, last_incremental_at=?, last_scan_duration_ms=? WHERE root_id=?",
                {n_ns, fin, fin, duration_ms, root_id});
    } else {
        db.exec("UPDATE fs_roots SET last_incremental_at=?, last_scan_duration_ms=? WHERE root_id=?",
                {fin, duration_ms, root_id});
    }
}

Json resolve_moved_hint(const Project& p, Database& db, const std::string& missing_path) {
    path_lineage_schema(db);
    const std::string target = norm_fwd_path(missing_path);
    if (target.empty()) return Json::object();

    // 1. Direct lookup in path_lineage by old_path -> follow chain by FRN or transitive new_path
    auto rows = db.query(
        "SELECT id, at_iso, root_id, frn, node_kind, op, old_path, new_path, actor, source, confidence, workstream_id, reason "
        "FROM path_lineage WHERE old_path=? COLLATE NOCASE ORDER BY id DESC LIMIT 1",
        {target});

    if (rows.empty()) {
        // Also check suffix match if missing_path was passed with or without root prefix
        rows = db.query(
            "SELECT id, at_iso, root_id, frn, node_kind, op, old_path, new_path, actor, source, confidence, workstream_id, reason "
            "FROM path_lineage WHERE old_path LIKE ? COLLATE NOCASE ORDER BY id DESC LIMIT 1",
            {"%/" + target});
    }

    if (rows.empty()) return Json::object();

    const auto& first = rows[0];
    const uint64_t frn = static_cast<uint64_t>(first["frn"].get<int64_t>());
    std::string current_path = first["new_path"].get<std::string>();
    std::string latest_op = first["op"].get<std::string>();
    std::string latest_at = first["at_iso"].get<std::string>();
    std::string latest_actor = first["actor"].get<std::string>();
    std::string min_confidence = first["confidence"].get<std::string>();
    Json chain = Json::array();

    if (frn != 0) {
        auto chain_rows = db.query(
            "SELECT id, at_iso, op, old_path, new_path, actor, source, confidence, workstream_id "
            "FROM path_lineage WHERE frn=? ORDER BY id ASC",
            {static_cast<int64_t>(frn)});
        for (const auto& cr : chain_rows) {
            chain.push_back(cr);
            const std::string np = cr["new_path"].get<std::string>();
            if (!np.empty()) current_path = np;
            latest_op = cr["op"].get<std::string>();
            latest_at = cr["at_iso"].get<std::string>();
            latest_actor = cr["actor"].get<std::string>();
            if (cr["confidence"].get<std::string>() == "reconciled_inferred") {
                min_confidence = "reconciled_inferred";
            }
        }
    } else {
        // Follow transitive path hops A -> B -> C even if FRN was 0
        std::unordered_set<std::string> visited;
        visited.insert(target);
        std::string cursor = current_path;
        chain.push_back(first);
        for (int hop = 0; hop < 16 && !cursor.empty() && !visited.contains(cursor); ++hop) {
            visited.insert(cursor);
            auto next_r = db.query(
                "SELECT id, at_iso, op, old_path, new_path, actor, source, confidence, workstream_id "
                "FROM path_lineage WHERE old_path=? COLLATE NOCASE ORDER BY id DESC LIMIT 1",
                {cursor});
            if (next_r.empty()) break;
            chain.push_back(next_r[0]);
            const std::string np = next_r[0]["new_path"].get<std::string>();
            latest_op = next_r[0]["op"].get<std::string>();
            latest_at = next_r[0]["at_iso"].get<std::string>();
            latest_actor = next_r[0]["actor"].get<std::string>();
            if (!np.empty()) {
                current_path = np;
                cursor = np;
            } else {
                break;
            }
        }
    }

    bool exists_at_new = false;
    if (!current_path.empty()) {
        std::error_code ec;
        exists_at_new = fs::exists(native_path(p.resolve(current_path)), ec);
        if (!exists_at_new && p.config.contains("roots") && p.config["roots"].is_array()) {
            for (const auto& r : p.config["roots"]) {
                if (fs::exists(native_path(p.resolve(r.value("path", "")) / fs::path(wide(current_path))), ec)) {
                    exists_at_new = true;
                    break;
                }
            }
        }
    }

    return {
        {"found", true},
        {"requested_path", target},
        {"current_path", current_path},
        {"exists_on_disk", exists_at_new},
        {"frn", frn},
        {"latest_op", latest_op},
        {"latest_at", latest_at},
        {"latest_actor", latest_actor},
        {"confidence", min_confidence},
        {"hops", chain.size()},
        {"chain", std::move(chain)}
    };
}

Json tool_path_history(const Project& p, Database& db, const Json& args) {
    path_lineage_schema(db);
    const std::string action = args.value("action", args.value("op", "timeline"));
    const int limit = std::clamp(args.value("limit", 50), 1, 500);

    if (action == "tree_scans") {
        auto roots = db.query(
            "SELECT root_id, root_key, abs_path, total_files, total_dirs, total_bytes, "
            "last_full_scan_ns, last_full_scan_at, last_incremental_at, last_scan_duration_ms, last_sync_ns, usn_journal_id, next_usn "
            "FROM fs_roots ORDER BY root_id");
        auto hist = db.query(
            "SELECT id, root_id, scan_kind, engine, started_at, finished_at, duration_ms, "
            "total_files, total_dirs, total_bytes, added, modified, moved, deleted, usn_discontinuity, actor "
            "FROM fs_scan_history ORDER BY id DESC LIMIT ?",
            {limit});
        return {
            {"schema", "migratool.path_history/1"},
            {"action", "tree_scans"},
            {"roots", std::move(roots)},
            {"history", std::move(hist)}
        };
    }

    if (action == "resolve") {
        const std::string path = args.value("path", args.value("old_path", ""));
        if (path.empty()) throw std::runtime_error("path_history(resolve) requiere path");
        auto hint = resolve_moved_hint(p, db, path);
        return {
            {"schema", "migratool.path_history/1"},
            {"action", "resolve"},
            {"resolved", !hint.empty() && hint.value("found", false)},
            {"hint", std::move(hint)}
        };
    }

    if (action == "trace") {
        const std::string path = norm_fwd_path(args.value("path", ""));
        uint64_t frn = static_cast<uint64_t>(args.value("frn", int64_t{0}));
        if (frn == 0 && !path.empty()) {
            auto r = db.query(
                "SELECT frn FROM path_lineage WHERE (old_path=? COLLATE NOCASE OR new_path=? COLLATE NOCASE) AND frn!=0 ORDER BY id DESC LIMIT 1",
                {path, path});
            if (!r.empty()) frn = static_cast<uint64_t>(r[0]["frn"].get<int64_t>());
            if (frn == 0) {
                std::error_code ec;
                fs::path abs = p.resolve(path);
                if (!fs::exists(native_path(abs), ec) && p.config.contains("roots") && p.config["roots"].is_array()) {
                    for (const auto& rt : p.config["roots"]) {
                        const fs::path cand = p.resolve(rt.value("path", "")) / fs::path(wide(path));
                        if (fs::exists(native_path(cand), ec)) { abs = cand; break; }
                    }
                }
                if (fs::exists(native_path(abs), ec)) frn = query_path_frn(abs);
            }
        }
        Json events = Json::array();
        if (frn != 0) {
            events = db.query(
                "SELECT id, at_iso, root_id, frn, node_kind, op, old_path, new_path, old_sha256, new_sha256, size_bytes, actor, source, confidence, workstream_id, reason "
                "FROM path_lineage WHERE frn=? ORDER BY id ASC LIMIT ?",
                {static_cast<int64_t>(frn), limit});
        }
        if (events.empty() && !path.empty()) {
            auto hint = resolve_moved_hint(p, db, path);
            if (hint.contains("chain")) events = hint["chain"];
            if (events.empty()) {
                events = db.query(
                    "SELECT id, at_iso, root_id, frn, node_kind, op, old_path, new_path, old_sha256, new_sha256, size_bytes, actor, source, confidence, workstream_id, reason "
                    "FROM path_lineage WHERE old_path=? COLLATE NOCASE OR new_path=? COLLATE NOCASE ORDER BY id ASC LIMIT ?",
                    {path, path, limit});
            }
        }
        std::string current_path = path;
        std::string min_conf = "exact_transactional";
        for (const auto& ev : events) {
            const std::string np = ev.value("new_path", "");
            if (!np.empty()) current_path = np;
            if (ev.value("confidence", "") == "reconciled_inferred") min_conf = "reconciled_inferred";
        }
        return {
            {"schema", "migratool.path_history/1"},
            {"action", "trace"},
            {"query_path", path},
            {"frn", frn},
            {"current_path", current_path},
            {"lineage_confidence", min_conf},
            {"count", events.size()},
            {"events", std::move(events)}
        };
    }

    if (action == "gaps") {
        auto rows = db.query(
            "SELECT id, at_iso, root_id, op, actor, source, confidence, reason "
            "FROM path_lineage WHERE op='journal_gap' OR confidence='journal_gap' ORDER BY id DESC LIMIT ?",
            {limit});
        return {
            {"schema", "migratool.path_history/1"},
            {"action", "gaps"},
            {"count", rows.size()},
            {"items", std::move(rows)}
        };
    }

    if (action == "moved_since") {
        const std::string since_iso = args.value("since_iso", "");
        const int64_t since_id = args.value("since_id", int64_t{0});
        Json rows;
        if (!since_iso.empty()) {
            rows = db.query(
                "SELECT id, at_iso, root_id, frn, node_kind, op, old_path, new_path, actor, source, confidence, workstream_id, reason "
                "FROM path_lineage WHERE at_iso>=? AND op IN('move','rename','quarantine','restore','delete') ORDER BY id DESC LIMIT ?",
                {since_iso, limit});
        } else {
            rows = db.query(
                "SELECT id, at_iso, root_id, frn, node_kind, op, old_path, new_path, actor, source, confidence, workstream_id, reason "
                "FROM path_lineage WHERE id>? AND op IN('move','rename','quarantine','restore','delete') ORDER BY id DESC LIMIT ?",
                {since_id, limit});
        }
        return {
            {"schema", "migratool.path_history/1"},
            {"action", "moved_since"},
            {"count", rows.size()},
            {"items", std::move(rows)}
        };
    }

    // Default: timeline (supports filtering by op, actor, confidence, workstream_id, path_prefix)
    const std::string op_filter = args.value("op_filter", "");
    const std::string actor_filter = args.value("actor", "");
    const std::string conf_filter = args.value("confidence", "");
    std::string sql =
        "SELECT id, at_iso, root_id, frn, node_kind, op, old_path, new_path, old_sha256, new_sha256, size_bytes, actor, source, confidence, workstream_id, reason "
        "FROM path_lineage WHERE 1=1";
    Json params = Json::array();
    if (!op_filter.empty()) { sql += " AND op=?"; params.push_back(op_filter); }
    if (!actor_filter.empty()) { sql += " AND actor=?"; params.push_back(actor_filter); }
    if (!conf_filter.empty()) { sql += " AND confidence=?"; params.push_back(conf_filter); }
    sql += " ORDER BY id DESC LIMIT ?";
    params.push_back(limit);
    auto rows = db.query(sql, params);
    return {
        {"schema", "migratool.path_history/1"},
        {"action", "timeline"},
        {"count", rows.size()},
        {"items", std::move(rows)}
    };
}

} // namespace mt
