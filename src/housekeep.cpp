#include "migratool/housekeep.hpp"
#include "migratool/fs_index.hpp"
#include "migratool/live.hpp"
#include "migratool/workstream.hpp"
#include <shellapi.h>
#include <algorithm>
#include <chrono>
#include <cwctype>
#include <vector>

namespace mt {
namespace {

bool table_exists(Database& db, const std::string& name) {
    return db.integer("SELECT EXISTS(SELECT 1 FROM sqlite_master WHERE type IN('table','view') AND name=?)", {name}) != 0;
}

bool pid_alive(int64_t pid) {
    if (pid <= 0) return false;
    if (static_cast<DWORD>(pid) == GetCurrentProcessId()) return true;
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!h) {
        // ERROR_ACCESS_DENIED means process exists with higher privilege
        return GetLastError() == ERROR_ACCESS_DENIED;
    }
    const bool alive = (WaitForSingleObject(h, 0) == WAIT_TIMEOUT);
    CloseHandle(h);
    return alive;
}

int64_t file_size_safe(const fs::path& p) {
    std::error_code ec;
    if (!fs::is_regular_file(native_path(p), ec)) return 0;
    return static_cast<int64_t>(fs::file_size(native_path(p), ec));
}

bool move_to_recycle_bin(const fs::path& p) {
    std::wstring abs = fs::absolute(p).wstring();
    // SHFileOperationW requires double-null-terminated standard Win32 path (without \\?\ prefix)
    if (abs.starts_with(L"\\\\?\\")) abs = abs.substr(4);
    abs.push_back(L'\0');
    abs.push_back(L'\0');
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = abs.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
}

std::wstring lower_w(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

std::string auto_vacuum_label(int64_t mode) {
    switch (mode) {
        case 0: return "NONE";
        case 1: return "FULL";
        case 2: return "INCREMENTAL";
        default: return "UNKNOWN";
    }
}

struct StaleFileInfo {
    fs::path path;
    std::string reason;
    int64_t bytes = 0;
    double age_hours = 0.0;
};

std::vector<StaleFileInfo> find_stale_state_files(const Project& p, double stale_temp_hours,
                                                  int keep_schema_backups, double backup_retention_days) {
    std::vector<StaleFileInfo> found;
    std::vector<StaleFileInfo> schema_backups;
    std::error_code ec;
    const auto sdir = p.state_dir();
    if (!fs::is_directory(native_path(sdir), ec)) return found;

    const auto now_ft = fs::file_time_type::clock::now();
    for (const auto& entry : fs::directory_iterator(native_path(sdir), ec)) {
        if (!entry.is_regular_file(ec)) continue;
        const auto fname_w = entry.path().filename().wstring();
        const auto low = lower_w(fname_w);
        // Never touch core catalog files
        if (low == L"catalog.sqlite" || low == L"catalog.sqlite-wal" || low == L"catalog.sqlite-shm" ||
            low == L"changelog.jsonl" || low == L"projects.sqlite" || low == L"projects.sqlite-wal" || low == L"projects.sqlite-shm") {
            continue;
        }
        const auto ftime = entry.last_write_time(ec);
        double age_h = 0.0;
        if (!ec && now_ft > ftime) {
            age_h = std::chrono::duration<double, std::ratio<3600>>(now_ft - ftime).count();
        }
        const int64_t bytes = static_cast<int64_t>(entry.file_size(ec));

        const bool is_temp = (low.find(L".tmp-") != std::wstring::npos) || (low.find(L".new-") != std::wstring::npos);
        const bool is_schema_bak = low.starts_with(L"schema-v") && low.ends_with(L".sqlite");

        if (is_temp && age_h >= stale_temp_hours) {
            found.push_back(StaleFileInfo{entry.path(), "stale_temp_file", bytes, age_h});
        } else if (is_schema_bak) {
            schema_backups.push_back(StaleFileInfo{entry.path(), "old_schema_backup", bytes, age_h});
        }
    }

    if (static_cast<int>(schema_backups.size()) > keep_schema_backups) {
        // Keep the newest N schema backups (smallest age_hours)
        std::sort(schema_backups.begin(), schema_backups.end(),
                  [](const StaleFileInfo& a, const StaleFileInfo& b) { return a.age_hours < b.age_hours; });
        for (size_t i = static_cast<size_t>(keep_schema_backups); i < schema_backups.size(); ++i) {
            found.push_back(std::move(schema_backups[i]));
        }
    }

    // Check old edit_symbol / complete_task rollback backups under state_dir()/backups/
    // (Never touches explicit catalog-*.sqlite / catalog-backup.sqlite backups!)
    const auto bdir = sdir / L"backups";
    const double max_bak_hours = backup_retention_days * 24.0;
    if (fs::is_directory(native_path(bdir), ec)) {
        for (const auto& entry : fs::directory_iterator(native_path(bdir), ec)) {
            const auto low = lower_w(entry.path().filename().wstring());
            if (low.starts_with(L"catalog") && low.ends_with(L".sqlite")) continue;
            if (entry.is_directory(ec)) {
                if (low == L"complete_task") {
                    for (const auto& f : fs::directory_iterator(native_path(entry.path()), ec)) {
                        if (!f.is_regular_file(ec)) continue;
                        const auto ft = f.last_write_time(ec);
                        double age_h = (!ec && now_ft > ft)
                            ? std::chrono::duration<double, std::ratio<3600>>(now_ft - ft).count() : 0.0;
                        if (age_h >= max_bak_hours) {
                            found.push_back(StaleFileInfo{f.path(), "old_edit_backup", static_cast<int64_t>(f.file_size(ec)), age_h});
                        }
                    }
                } else {
                    // Per-transaction backup folder created by edit_symbol (backups/<tx_id>/)
                    const auto ft = entry.last_write_time(ec);
                    double age_h = (!ec && now_ft > ft)
                        ? std::chrono::duration<double, std::ratio<3600>>(now_ft - ft).count() : 0.0;
                    if (age_h >= max_bak_hours) {
                        int64_t dir_bytes = 0;
                        for (const auto& f : fs::recursive_directory_iterator(native_path(entry.path()), ec)) {
                            if (f.is_regular_file(ec)) dir_bytes += static_cast<int64_t>(f.file_size(ec));
                        }
                        found.push_back(StaleFileInfo{entry.path(), "old_tx_backup_dir", dir_bytes, age_h});
                    }
                }
            }
        }
    }
    return found;
}

Json wal_checkpoint_info(Database& db, const char* mode_sql) {
    Json res = {{"mode", mode_sql}, {"busy", 0}, {"log_pages", 0}, {"checkpointed_pages", 0}};
    try {
        auto rows = db.query(std::string("PRAGMA wal_checkpoint(") + mode_sql + ")");
        if (!rows.empty()) {
            res["busy"] = rows[0].value("busy", int64_t(0));
            res["log_pages"] = rows[0].value("log", int64_t(0));
            res["checkpointed_pages"] = rows[0].value("checkpointed", int64_t(0));
        }
    } catch (const std::exception& e) {
        res["error"] = e.what();
    }
    return res;
}

} // namespace

Json housekeep_status(const Project& p, Database& db, const Json& args) {
    const double stale_temp_hours = std::clamp(args.value("stale_temp_hours", 24.0), 0.0, 8760.0);
    const int keep_schema_backups = std::clamp(args.value("keep_schema_backups", 2), 0, 20);
    const double backup_retention_days = std::clamp(args.value("backup_retention_days", 14.0), 0.0, 3650.0);

    const fs::path db_path = p.state_dir() / L"catalog.sqlite";
    const fs::path wal_path = p.state_dir() / L"catalog.sqlite-wal";
    const fs::path shm_path = p.state_dir() / L"catalog.sqlite-shm";

    const int64_t page_size = db.integer("PRAGMA page_size");
    const int64_t page_count = db.integer("PRAGMA page_count");
    const int64_t freelist_count = db.integer("PRAGMA freelist_count");
    const int64_t auto_vac = db.integer("PRAGMA auto_vacuum");

    const int64_t db_bytes = file_size_safe(db_path);
    const int64_t wal_bytes = file_size_safe(wal_path);
    const int64_t shm_bytes = file_size_safe(shm_path);
    const int64_t reclaimable_bytes = freelist_count * page_size;

    const auto now_iso = now();
    Json tables = Json::object();
    tables["requests"] = table_exists(db, "requests") ? db.integer("SELECT COUNT(*) FROM requests") : 0;
    tables["connections"] = table_exists(db, "connections") ? db.integer("SELECT COUNT(*) FROM connections") : 0;
    tables["watchers"] = table_exists(db, "watchers") ? db.integer("SELECT COUNT(*) FROM watchers") : 0;
    tables["task_claims_total"] = table_exists(db, "task_claims") ? db.integer("SELECT COUNT(*) FROM task_claims") : 0;
    tables["task_claims_expired"] = table_exists(db, "task_claims")
        ? db.integer("SELECT COUNT(*) FROM task_claims WHERE expires_at < ?", {now_iso}) : 0;
    tables["fs_change_log"] = table_exists(db, "fs_change_log") ? db.integer("SELECT COUNT(*) FROM fs_change_log") : 0;
    tables["fs_errors"] = table_exists(db, "fs_errors") ? db.integer("SELECT COUNT(*) FROM fs_errors") : 0;
    tables["fs_nodes_soft_deleted"] = table_exists(db, "fs_nodes")
        ? db.integer("SELECT COUNT(*) FROM fs_nodes WHERE (flags & 1)<>0") : 0;
    tables["inventory_jobs_finished"] = table_exists(db, "inventory_jobs")
        ? db.integer("SELECT COUNT(*) FROM inventory_jobs WHERE state NOT IN('queued','running','paused')") : 0;

    int64_t dead_connections = 0;
    if (table_exists(db, "connections")) {
        for (const auto& r : db.query("SELECT id, pid FROM connections WHERE closed IS NULL")) {
            if (!pid_alive(r.value("pid", int64_t(0)))) ++dead_connections;
        }
    }
    int64_t dead_watchers = 0;
    if (table_exists(db, "watchers")) {
        for (const auto& r : db.query("SELECT id, pid FROM watchers WHERE closed IS NULL")) {
            if (!pid_alive(r.value("pid", int64_t(0)))) ++dead_watchers;
        }
    }
    tables["dead_open_connections"] = dead_connections;
    tables["dead_open_watchers"] = dead_watchers;

    auto stale = find_stale_state_files(p, stale_temp_hours, keep_schema_backups, backup_retention_days);
    int64_t stale_bytes = 0;
    Json stale_list = Json::array();
    for (const auto& sf : stale) {
        stale_bytes += sf.bytes;
        if (stale_list.size() < 50) {
            stale_list.push_back({{"path", path_text(sf.path)}, {"reason", sf.reason}, {"bytes", sf.bytes}});
        }
    }

    auto last_hk = db.query("SELECT value FROM meta WHERE key='last_housekeep_at'");
    Json reg_status = Json::object();
    try { reg_status = housekeep_registry(args, /*dry_run=*/true); } catch (...) {}

    const bool needs_attention =
        (wal_bytes > 32 * 1024 * 1024) ||
        (reclaimable_bytes > 32 * 1024 * 1024) ||
        (tables["requests"].get<int64_t>() > 20000) ||
        (tables["fs_change_log"].get<int64_t>() > 100000) ||
        (tables["task_claims_expired"].get<int64_t>() > 0) ||
        (dead_connections > 0) ||
        (dead_watchers > 0) ||
        (!stale.empty());

    return {
        {"schema", "migratool.housekeep_status/1"},
        {"auto_housekeep_enabled", true},
        {"auto_housekeep_interval_ms", p.config.value("housekeep_interval_ms", 1800000)},
        {"last_housekeep_at", last_hk.empty() ? Json(nullptr) : last_hk[0]["value"]},
        {"needs_attention", needs_attention},
        {"catalog", {
            {"path", path_text(db_path)},
            {"bytes", db_bytes},
            {"wal_bytes", wal_bytes},
            {"shm_bytes", shm_bytes},
            {"page_size", page_size},
            {"page_count", page_count},
            {"freelist_pages", freelist_count},
            {"reclaimable_bytes", reclaimable_bytes},
            {"auto_vacuum", auto_vacuum_label(auto_vac)},
            {"auto_vacuum_code", auto_vac}
        }},
        {"tables", std::move(tables)},
        {"stale_state_files", {{"count", static_cast<int64_t>(stale.size())}, {"bytes", stale_bytes}, {"items", std::move(stale_list)}}},
        {"registry", std::move(reg_status)}
    };
}

Json housekeep_run(const Project& p, Database& db, const Json& args, bool background, Log log) {
    const auto t0 = std::chrono::steady_clock::now();
    const bool dry_run = !background && args.value("dry_run", false);
    const bool deep = !background && args.value("deep", false);
    const bool compact_full = !background && args.value("compact_full", false);
    const bool prune_exts = !background && (deep || args.value("prune_extensions", false));

    const int64_t max_change_log = std::clamp<int64_t>(args.value("max_change_log", int64_t(100000)), 1000, 5000000);
    const int64_t max_requests = std::clamp<int64_t>(args.value("max_requests", int64_t(20000)), 100, 1000000);
    const int64_t max_connections = std::clamp<int64_t>(args.value("max_connections", int64_t(1000)), 20, 100000);
    const int64_t max_watchers = std::clamp<int64_t>(args.value("max_watchers", int64_t(1000)), 20, 100000);
    const int64_t max_fs_errors = std::clamp<int64_t>(args.value("max_fs_errors", int64_t(10000)), 100, 500000);
    const int64_t keep_jobs = std::clamp<int64_t>(args.value("keep_finished_jobs", int64_t(10)), 1, 500);
    const double stale_temp_hours = std::clamp(args.value("stale_temp_hours", 24.0), 0.0, 8760.0);
    const int keep_schema_backups = std::clamp(args.value("keep_schema_backups", 2), 0, 20);
    const double backup_retention_days = std::clamp(args.value("backup_retention_days", 14.0), 0.0, 3650.0);

    const fs::path db_path = p.state_dir() / L"catalog.sqlite";
    const fs::path wal_path = p.state_dir() / L"catalog.sqlite-wal";
    const int64_t db_bytes_before = file_size_safe(db_path);
    const int64_t wal_bytes_before = file_size_safe(wal_path);
    const int64_t freelist_before = db.integer("PRAGMA freelist_count");
    const int64_t auto_vac_before = db.integer("PRAGMA auto_vacuum");

    Json pruned = {
        {"fs_change_log", 0},
        {"requests", 0},
        {"connections_closed_dead", 0},
        {"connections_pruned", 0},
        {"watchers_closed_dead", 0},
        {"watchers_pruned", 0},
        {"task_claims_expired", 0},
        {"fs_errors", 0},
        {"fs_nodes_soft_deleted", 0},
        {"fs_extensions_unused", 0},
        {"inventory_jobs_pruned", 0},
        {"census_generations_pruned", 0}
    };

    const std::string ts = now();

    auto count_excess = [&](const std::string& tbl, const std::string& pk_col, int64_t cap, const std::string& extra_where = "") -> int64_t {
        if (!table_exists(db, tbl)) return 0;
        const std::string where = extra_where.empty() ? "" : (" WHERE " + extra_where);
        const int64_t cnt = db.integer("SELECT COUNT(*) FROM " + tbl + where);
        if (cnt <= cap) return 0;
        if (dry_run) return cnt - cap;
        auto cutoff = db.query("SELECT " + pk_col + " AS cut FROM " + tbl + where + " ORDER BY " + pk_col + " DESC LIMIT 1 OFFSET ?", {cap});
        if (cutoff.empty()) return 0;
        const int64_t cut_val = cutoff[0]["cut"].get<int64_t>();
        if (extra_where.empty()) {
            db.exec("DELETE FROM " + tbl + " WHERE " + pk_col + " <= ?", {cut_val});
        } else {
            db.exec("DELETE FROM " + tbl + " WHERE (" + extra_where + ") AND " + pk_col + " <= ?", {cut_val});
        }
        const int64_t after_cnt = db.integer("SELECT COUNT(*) FROM " + tbl + where);
        return std::max<int64_t>(0, cnt - after_cnt);
    };

    // Detect dead connections/watchers before entering write tx
    std::vector<std::string> dead_conns, dead_watches;
    if (table_exists(db, "connections")) {
        for (const auto& r : db.query("SELECT id, pid FROM connections WHERE closed IS NULL")) {
            if (!pid_alive(r.value("pid", int64_t(0)))) dead_conns.push_back(r["id"].get<std::string>());
        }
    }
    if (table_exists(db, "watchers")) {
        for (const auto& r : db.query("SELECT id, pid FROM watchers WHERE closed IS NULL")) {
            if (!pid_alive(r.value("pid", int64_t(0)))) dead_watches.push_back(r["id"].get<std::string>());
        }
    }

    if (dry_run) {
        pruned["fs_change_log"] = count_excess("fs_change_log", "seq", max_change_log);
        pruned["requests"] = count_excess("requests", "id", max_requests);
        pruned["connections_closed_dead"] = static_cast<int64_t>(dead_conns.size());
        if (table_exists(db, "connections")) {
            int64_t closed_c = db.integer("SELECT COUNT(*) FROM connections WHERE closed IS NOT NULL") + static_cast<int64_t>(dead_conns.size());
            pruned["connections_pruned"] = std::max<int64_t>(0, closed_c - max_connections);
        }
        pruned["watchers_closed_dead"] = static_cast<int64_t>(dead_watches.size());
        if (table_exists(db, "watchers")) {
            int64_t closed_w = db.integer("SELECT COUNT(*) FROM watchers WHERE closed IS NOT NULL") + static_cast<int64_t>(dead_watches.size());
            pruned["watchers_pruned"] = std::max<int64_t>(0, closed_w - max_watchers);
        }
        if (table_exists(db, "task_claims")) {
            pruned["task_claims_expired"] = db.integer("SELECT COUNT(*) FROM task_claims WHERE expires_at < ?", {ts});
        }
        pruned["fs_errors"] = count_excess("fs_errors", "id", max_fs_errors);
        if (table_exists(db, "fs_nodes")) {
            pruned["fs_nodes_soft_deleted"] = db.integer("SELECT COUNT(*) FROM fs_nodes WHERE (flags & 1)<>0");
        }
        if (table_exists(db, "inventory_jobs")) {
            int64_t fin_j = db.integer("SELECT COUNT(*) FROM inventory_jobs WHERE state NOT IN('queued','running','paused')");
            pruned["inventory_jobs_pruned"] = std::max<int64_t>(0, fin_j - keep_jobs);
        }
    } else {
        db.begin();
        try {
            pruned["fs_change_log"] = count_excess("fs_change_log", "seq", max_change_log);
            pruned["requests"] = count_excess("requests", "id", max_requests);
            pruned["fs_errors"] = count_excess("fs_errors", "id", max_fs_errors);

            if (table_exists(db, "connections")) {
                for (const auto& cid : dead_conns) {
                    db.exec("UPDATE connections SET closed=? WHERE id=? AND closed IS NULL", {ts, cid});
                }
                pruned["connections_closed_dead"] = static_cast<int64_t>(dead_conns.size());
                const int64_t closed_c = db.integer("SELECT COUNT(*) FROM connections WHERE closed IS NOT NULL");
                if (closed_c > max_connections) {
                    const int64_t to_del = closed_c - max_connections;
                    db.exec("DELETE FROM connections WHERE id IN (SELECT id FROM connections WHERE closed IS NOT NULL ORDER BY opened ASC LIMIT ?)", {to_del});
                    pruned["connections_pruned"] = to_del;
                }
            }
            if (table_exists(db, "watchers")) {
                for (const auto& wid : dead_watches) {
                    db.exec("UPDATE watchers SET closed=? WHERE id=? AND closed IS NULL", {ts, wid});
                }
                pruned["watchers_closed_dead"] = static_cast<int64_t>(dead_watches.size());
                const int64_t closed_w = db.integer("SELECT COUNT(*) FROM watchers WHERE closed IS NOT NULL");
                if (closed_w > max_watchers) {
                    const int64_t to_del = closed_w - max_watchers;
                    db.exec("DELETE FROM watchers WHERE id IN (SELECT id FROM watchers WHERE closed IS NOT NULL ORDER BY opened ASC LIMIT ?)", {to_del});
                    pruned["watchers_pruned"] = to_del;
                }
            }
            if (table_exists(db, "task_claims")) {
                const int64_t exp = db.integer("SELECT COUNT(*) FROM task_claims WHERE expires_at < ?", {ts});
                if (exp > 0) {
                    db.exec("DELETE FROM task_claims WHERE expires_at < ?", {ts});
                    pruned["task_claims_expired"] = exp;
                }
            }
            if (table_exists(db, "fs_nodes")) {
                const int64_t soft = db.integer("SELECT COUNT(*) FROM fs_nodes WHERE (flags & 1)<>0");
                if (soft > 0) {
                    db.exec("DELETE FROM fs_nodes WHERE (flags & 1)<>0");
                    pruned["fs_nodes_soft_deleted"] = soft;
                }
            }
            if (prune_exts && table_exists(db, "fs_extensions") && table_exists(db, "fs_nodes")) {
                const int64_t before_ext = db.integer("SELECT COUNT(*) FROM fs_extensions");
                db.exec("DELETE FROM fs_extensions WHERE ext_id>0 AND ext_id NOT IN (SELECT DISTINCT ext_id FROM fs_nodes)");
                const int64_t after_ext = db.integer("SELECT COUNT(*) FROM fs_extensions");
                pruned["fs_extensions_unused"] = std::max<int64_t>(0, before_ext - after_ext);
            }
            if (table_exists(db, "inventory_jobs")) {
                auto old_jobs = db.query(
                    "SELECT id FROM inventory_jobs WHERE state NOT IN('queued','running','paused') "
                    "ORDER BY updated DESC LIMIT -1 OFFSET ?", {keep_jobs});
                for (const auto& j : old_jobs) {
                    const std::string jid = j["id"].get<std::string>();
                    if (table_exists(db, "inventory_tasks")) db.exec("DELETE FROM inventory_tasks WHERE job_id=?", {jid});
                    if (table_exists(db, "inventory_errors")) db.exec("DELETE FROM inventory_errors WHERE job_id=?", {jid});
                    db.exec("DELETE FROM inventory_jobs WHERE id=?", {jid});
                }
                pruned["inventory_jobs_pruned"] = static_cast<int64_t>(old_jobs.size());
            }
            if (table_exists(db, "inventory_generations") && table_exists(db, "census_entries")) {
                const bool job_active = table_exists(db, "inventory_jobs") &&
                    db.integer("SELECT COUNT(*) FROM inventory_jobs WHERE state IN('queued','running','paused')") > 0;
                if (!job_active) {
                    const int64_t max_gen = db.integer("SELECT COALESCE(MAX(id),0) FROM inventory_generations WHERE state='published'");
                    if (max_gen > 2) {
                        const int64_t old_gens = db.integer("SELECT COUNT(*) FROM inventory_generations WHERE id < ?", {max_gen - 1});
                        if (old_gens > 0) {
                            db.exec("DELETE FROM census_entries WHERE generation < ?", {max_gen - 1});
                            if (table_exists(db, "generation_scopes")) {
                                db.exec("DELETE FROM generation_scopes WHERE generation < ?", {max_gen - 1});
                            }
                            db.exec("DELETE FROM inventory_generations WHERE id < ?", {max_gen - 1});
                            pruned["census_generations_pruned"] = old_gens;
                        }
                    }
                }
            }
            db.exec("INSERT OR REPLACE INTO meta(key,value) VALUES('last_housekeep_at',?)", {ts});
            db.commit();
        } catch (...) {
            db.rollback();
            throw;
        }
    }

    // FTS5 incremental merge / optimize (outside giant tx, fast and bounded)
    Json fts_ops = Json::array();
    if (!dry_run) {
        const char* fts_sql_mode = deep ? "optimize" : "merge";
        for (const char* tbl : {"fs_name_fts", "fs_dir_fts", "memories_fts"}) {
            if (!table_exists(db, tbl)) continue;
            try {
                if (deep) {
                    db.exec(std::string("INSERT INTO ") + tbl + "(" + tbl + ") VALUES('optimize')");
                } else {
                    const int pages = background ? -8 : -16;
                    db.exec(std::string("INSERT INTO ") + tbl + "(" + tbl + ", rank) VALUES('merge', ?)", {pages});
                }
                fts_ops.push_back({{"table", tbl}, {"op", fts_sql_mode}, {"ok", true}});
            } catch (const std::exception& e) {
                fts_ops.push_back({{"table", tbl}, {"op", fts_sql_mode}, {"ok", false}, {"error", e.what()}});
            }
        }
    }

    // Sweep stale temp files & old schema backups in state_dir (sent to Recycle Bin!)
    auto stale_files = find_stale_state_files(p, stale_temp_hours, keep_schema_backups, backup_retention_days);
    int64_t swept_count = 0, swept_bytes = 0;
    Json swept_items = Json::array();
    for (const auto& sf : stale_files) {
        bool ok = true;
        if (!dry_run) {
            ok = move_to_recycle_bin(sf.path);
        }
        if (ok) {
            ++swept_count;
            swept_bytes += sf.bytes;
        }
        if (swept_items.size() < 50) {
            swept_items.push_back({{"path", path_text(sf.path)}, {"reason", sf.reason}, {"bytes", sf.bytes}, {"recycled", dry_run ? false : ok}});
        }
    }

    // SQLite page reclamation, optional full VACUUM, WAL checkpoint & PRAGMA optimize
    bool ran_incremental_vacuum = false;
    bool ran_full_vacuum = false;
    Json wal_cp = Json::object();
    if (!dry_run) {
        if (compact_full) {
            try {
                db.exec("PRAGMA auto_vacuum=INCREMENTAL");
                db.exec("VACUUM");
                ran_full_vacuum = true;
            } catch (const std::exception& e) {
                if (log) log(std::string("housekeep VACUUM: ") + e.what());
            }
        } else if (auto_vac_before == 2 && db.integer("PRAGMA freelist_count") > 0) {
            try {
                if (deep) db.exec("PRAGMA incremental_vacuum");
                else db.exec(background ? "PRAGMA incremental_vacuum(1024)" : "PRAGMA incremental_vacuum(4096)");
                ran_incremental_vacuum = true;
            } catch (...) {}
        }

        try { db.exec("PRAGMA optimize"); } catch (...) {}

        if (background) {
            if (wal_bytes_before > 16 * 1024 * 1024 || ran_incremental_vacuum) {
                wal_cp = wal_checkpoint_info(db, "PASSIVE");
            }
        } else {
            wal_cp = wal_checkpoint_info(db, "TRUNCATE");
        }
    }

    // Global registry housekeeping
    Json reg_res = Json::object();
    try { reg_res = housekeep_registry(args, dry_run); } catch (const std::exception& e) { reg_res["error"] = e.what(); }

    // P4 Workstream snapshot retention (never prunes pinned or active/interrupted workstream snapshots; GC unreferenced blobs to Recycle Bin)
    Json ws_snap_res = Json::object();
    try {
        ws_snap_res = workstream_prune_ephemeral_snapshots(p, db, args.value("keep_ephemeral_snapshots", 20), backup_retention_days, dry_run);
    } catch (const std::exception& e) {
        ws_snap_res["error"] = e.what();
    }

    const int64_t db_bytes_after = file_size_safe(db_path);
    const int64_t wal_bytes_after = file_size_safe(wal_path);
    const int64_t freelist_after = db.integer("PRAGMA freelist_count");
    const int64_t auto_vac_after = db.integer("PRAGMA auto_vacuum");
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();

    if (log && !background) {
        log("Housekeep completado en " + std::to_string(elapsed_ms) + " ms (DB: " +
            std::to_string(db_bytes_before) + " -> " + std::to_string(db_bytes_after) + " B, WAL: " +
            std::to_string(wal_bytes_before) + " -> " + std::to_string(wal_bytes_after) + " B)");
    }

    return {
        {"schema", "migratool.housekeep/1"},
        {"ok", true},
        {"dry_run", dry_run},
        {"background", background},
        {"deep", deep},
        {"compact_full", compact_full},
        {"at", ts},
        {"elapsed_ms", elapsed_ms},
        {"catalog", {
            {"bytes_before", db_bytes_before},
            {"bytes_after", db_bytes_after},
            {"wal_bytes_before", wal_bytes_before},
            {"wal_bytes_after", wal_bytes_after},
            {"reclaimed_total_bytes", std::max<int64_t>(0, (db_bytes_before + wal_bytes_before) - (db_bytes_after + wal_bytes_after))},
            {"freelist_before", freelist_before},
            {"freelist_after", freelist_after},
            {"auto_vacuum", auto_vacuum_label(auto_vac_after)},
            {"ran_incremental_vacuum", ran_incremental_vacuum},
            {"ran_full_vacuum", ran_full_vacuum},
            {"wal_checkpoint", std::move(wal_cp)}
        }},
        {"pruned", std::move(pruned)},
        {"fts_maintenance", std::move(fts_ops)},
        {"stale_state_files", {{"count", swept_count}, {"bytes", swept_bytes}, {"items", std::move(swept_items)}}},
        {"workstream_snapshots", std::move(ws_snap_res)},
        {"registry", std::move(reg_res)}
    };
}

Json tool_housekeep(const Project& p, Database& db, const Json& args, Log log) {
    std::string action = args.value("action", "status");
    std::transform(action.begin(), action.end(), action.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (action == "status" || action == "inspect" || action == "health") {
        return housekeep_status(p, db, args);
    }
    if (action == "run" || action == "clean" || action == "compact" || action == "vacuum") {
        Json run_args = args;
        if (action == "vacuum" && !run_args.contains("compact_full")) run_args["compact_full"] = true;
        return housekeep_run(p, db, run_args, /*background=*/false, log);
    }
    throw std::runtime_error("housekeep action debe ser 'status', 'run' o 'vacuum'");
}

} // namespace mt
