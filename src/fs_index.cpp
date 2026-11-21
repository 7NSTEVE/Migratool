#include "migratool/fs_index.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cwctype>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <winioctl.h>

namespace mt {
namespace {

std::string to_lower_ascii(std::string_view sv) {
    std::string out(sv);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string normalize_rel_path(std::string p) {
    std::replace(p.begin(), p.end(), '\\', '/');
    while (!p.empty() && p.front() == '/') p.erase(p.begin());
    while (!p.empty() && p.back() == '/') p.pop_back();
    return p;
}

std::string extract_ext_lower(std::string_view name) {
    auto dot = name.rfind('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 >= name.size()) return {};
    if (name.size() - dot > 24) return {};
    return to_lower_ascii(name.substr(dot));
}

int64_t filetime_to_unix_ns(LARGE_INTEGER ft) {
    constexpr int64_t EPOCH_DIFF_100NS = 116444736000000000LL;
    if (ft.QuadPart <= EPOCH_DIFF_100NS) return 0;
    return (ft.QuadPart - EPOCH_DIFF_100NS) * 100LL;
}

int64_t filetime_to_unix_ns(FILETIME ft) {
    LARGE_INTEGER li{};
    li.LowPart = ft.dwLowDateTime;
    li.HighPart = ft.dwHighDateTime;
    return filetime_to_unix_ns(li);
}

std::string fts5_escape_phrase(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 4);
    out.push_back('"');
    for (char c : text) {
        if (c == '"') out.push_back('"');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

struct ScopedTx {
    Database& db;
    bool owned = false;
    explicit ScopedTx(Database& d) : db(d) {
        if (sqlite3_get_autocommit(db.native_handle()) != 0) {
            db.begin();
            owned = true;
        }
    }
    void commit() {
        if (owned) {
            db.commit();
            owned = false;
        }
    }
    ~ScopedTx() {
        if (owned) db.rollback();
    }
};

struct RawStmt {
    sqlite3_stmt* s = nullptr;
    RawStmt(sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK) {
            throw std::runtime_error(std::string("SQLite prepare failed: ") + sqlite3_errmsg(db) + " | " + sql);
        }
    }
    ~RawStmt() {
        if (s) sqlite3_finalize(s);
    }
    RawStmt(const RawStmt&) = delete;
    void reset() {
        sqlite3_reset(s);
        sqlite3_clear_bindings(s);
    }
};

void exec_raw(sqlite3* db, const char* sql) {
    char* err = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::string msg = err ? err : sqlite3_errmsg(db);
        if (err) sqlite3_free(err);
        throw std::runtime_error("SQLite exec failed: " + msg);
    }
}

std::string kind_to_string(int k) {
    switch (k) {
        case 0: return "file";
        case 1: return "dir";
        case 2: return "symlink_file";
        case 3: return "symlink_dir";
        case 4: return "junction";
        default: return "file";
    }
}

int64_t get_or_create_ext_id(Database& db, const std::string& ext_lower) {
    if (ext_lower.empty()) return 0;
    auto rows = db.query("SELECT ext_id FROM fs_extensions WHERE ext=?", {ext_lower});
    if (!rows.empty()) return rows[0]["ext_id"].get<int64_t>();
    db.exec("INSERT OR IGNORE INTO fs_extensions(ext) VALUES(?)", {ext_lower});
    rows = db.query("SELECT ext_id FROM fs_extensions WHERE ext=?", {ext_lower});
    return rows.empty() ? 0 : rows[0]["ext_id"].get<int64_t>();
}

void prune_change_log_if_needed(Database& db) {
    // Bounded ring table: keep at most 100,000 change records (never an unbounded 1.5 GB log)
    static thread_local uint32_t counter = 0;
    if ((++counter & 1023u) != 0u) return;
    const int64_t max_seq = db.integer("SELECT COALESCE(MAX(seq),0) FROM fs_change_log");
    if (max_seq > 100000) {
        db.exec("DELETE FROM fs_change_log WHERE seq <= ?", {max_seq - 100000});
    }
}

void log_fs_change(Database& db, int64_t root_id, uint64_t frn, const std::string& op,
                   int kind, const std::string& old_path, const std::string& new_path, int64_t size_bytes) {
    db.exec("INSERT INTO fs_change_log(at_ns,root_id,frn,op,kind,old_path,new_path,size_bytes) VALUES(?,?,?,?,?,?,?,?)",
            {now_ns(), root_id, static_cast<int64_t>(frn), op, kind, old_path, new_path, size_bytes});
    prune_change_log_if_needed(db);
}

void update_ancestors_delta(Database& db, int64_t root_id, uint64_t start_parent_frn,
                            int64_t d_files, int64_t d_dirs, int64_t d_bytes) {
    if (d_files == 0 && d_dirs == 0 && d_bytes == 0) return;
    uint64_t cur = start_parent_frn;
    std::unordered_set<uint64_t> visited;
    int hops = 0;
    for (int hop = 0; hop < 256 && cur != 0; ++hop) {
        if (!visited.insert(cur).second) break;
        auto rows = db.query("SELECT parent_frn FROM fs_dirs WHERE root_id=? AND frn=?",
                             {root_id, static_cast<int64_t>(cur)});
        if (rows.empty()) break;
        db.exec("UPDATE fs_dirs SET total_files=MAX(0,total_files+?), total_dirs=MAX(0,total_dirs+?), "
                "total_bytes=MAX(0,total_bytes+?) WHERE root_id=? AND frn=?",
                {d_files, d_dirs, d_bytes, root_id, static_cast<int64_t>(cur)});
        ++hops;
        cur = static_cast<uint64_t>(rows[0]["parent_frn"].get<int64_t>());
    }
    if (hops > 0) {
        db.exec("UPDATE fs_roots SET total_files=MAX(0,total_files+?), total_dirs=MAX(0,total_dirs+?), "
                "total_bytes=MAX(0,total_bytes+?), last_sync_ns=? WHERE root_id=?",
                {d_files, d_dirs, d_bytes, now_ns(), root_id});
    }
}

std::string join_rel(const std::string& parent_rel, const std::string& name) {
    if (parent_rel.empty()) return name;
    return parent_rel + "/" + name;
}

std::string lookup_dir_rel(Database& db, int64_t root_id, uint64_t dir_frn, int* out_depth = nullptr) {
    auto rows = db.query("SELECT rel_path, depth FROM fs_dirs WHERE root_id=? AND frn=?",
                         {root_id, static_cast<int64_t>(dir_frn)});
    if (rows.empty()) {
        if (out_depth) *out_depth = 0;
        return {};
    }
    if (out_depth) *out_depth = rows[0]["depth"].get<int>();
    return rows[0]["rel_path"].get<std::string>();
}

void create_fs_secondary_indexes(sqlite3* raw) {
    exec_raw(raw,
        "CREATE INDEX IF NOT EXISTS idx_fs_nodes_parent ON fs_nodes(root_id,parent_frn,kind,name);"
        "CREATE INDEX IF NOT EXISTS idx_fs_nodes_ext ON fs_nodes(root_id,ext_id) WHERE (flags & 1)=0;"
        "CREATE INDEX IF NOT EXISTS idx_fs_nodes_mtime ON fs_nodes(root_id,mtime_ns) WHERE (flags & 1)=0;"
        "CREATE INDEX IF NOT EXISTS idx_fs_dirs_path ON fs_dirs(root_id,rel_path);"
        "CREATE INDEX IF NOT EXISTS idx_fs_dirs_parent ON fs_dirs(root_id,parent_frn,total_bytes DESC,total_files DESC);"
        "CREATE INDEX IF NOT EXISTS idx_fs_change_root ON fs_change_log(root_id,seq);");
}

bool match_glob_impl(std::string_view pat, std::string_view txt) {
    size_t pi = 0, ti = 0;
    size_t star_p = std::string_view::npos, star_t = 0;
    bool double_star = false;
    while (ti < txt.size()) {
        if (pi + 1 < pat.size() && pat[pi] == '*' && pat[pi + 1] == '*') {
            while (pi < pat.size() && pat[pi] == '*') ++pi;
            if (pi < pat.size() && pat[pi] == '/') ++pi;
            star_p = pi;
            star_t = ti;
            double_star = true;
            continue;
        }
        if (pi < pat.size() && pat[pi] == '*') {
            star_p = ++pi;
            star_t = ti;
            double_star = false;
            continue;
        }
        if (pi < pat.size() && (pat[pi] == '?' ||
            std::tolower(static_cast<unsigned char>(pat[pi])) == std::tolower(static_cast<unsigned char>(txt[ti])))) {
            if (pat[pi] == '?' && txt[ti] == '/') {
                // '?' does not match path separator
            } else {
                ++pi;
                ++ti;
                continue;
            }
        }
        if (star_p != std::string_view::npos) {
            if (!double_star && txt[star_t] == '/') return false;
            ti = ++star_t;
            pi = star_p;
            continue;
        }
        return false;
    }
    while (pi < pat.size() && (pat[pi] == '*' || pat[pi] == '/')) {
        if (pat[pi] == '/' && (pi == 0 || pat[pi - 1] != '*')) break;
        ++pi;
    }
    return pi == pat.size();
}

} // namespace

int64_t now_ns() {
    auto dur = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(dur).count();
}

bool is_link_reparse_tag(DWORD tag) {
    return tag == IO_REPARSE_TAG_SYMLINK || tag == IO_REPARSE_TAG_MOUNT_POINT;
}

FsNodeKind classify_win32_node(uint32_t attrs, DWORD reparse_tag) {
    const bool is_dir = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    const bool is_rep = (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    if (is_rep && is_link_reparse_tag(reparse_tag)) {
        if (reparse_tag == IO_REPARSE_TAG_MOUNT_POINT) return FsNodeKind::Junction;
        return is_dir ? FsNodeKind::SymlinkDir : FsNodeKind::SymlinkFile;
    }
    return is_dir ? FsNodeKind::Directory : FsNodeKind::File;
}

bool fs_glob_match(std::string_view pattern, std::string_view text) {
    std::string p(pattern), t(text);
    std::replace(p.begin(), p.end(), '\\', '/');
    std::replace(t.begin(), t.end(), '\\', '/');
    return match_glob_impl(p, t);
}

std::string project_mode(const Project& p) {
    if (p.config.contains("mode") && p.config["mode"].is_string()) {
        auto m = to_lower_ascii(p.config["mode"].get<std::string>());
        if (m == "archive" || m == "workspace" || m == "migration") return m;
    }
    bool has_source = false, has_target = false, has_archive = false, has_workspace = false;
    if (p.config.contains("roots") && p.config["roots"].is_array()) {
        for (const auto& r : p.config["roots"]) {
            auto role = to_lower_ascii(r.value("role", ""));
            if (role == "source") has_source = true;
            else if (role == "target") has_target = true;
            else if (role == "archive") has_archive = true;
            else if (role == "workspace") has_workspace = true;
        }
    }
    if (has_archive) return "archive";
    if (has_workspace && !has_target) return "workspace";
    if (has_source && has_target) return "migration";
    return has_source ? "migration" : "workspace";
}

bool is_watch_ignored_path(const std::wstring& rel_or_abs_lower, const Project& p) {
    std::wstring norm = rel_or_abs_lower;
    for (auto& c : norm) {
        if (c == L'/') c = L'\\';
        else c = static_cast<wchar_t>(towlower(c));
    }
    // Never watch Migratool's own state_dir or .migratool directories
    auto state_norm = p.state_dir().lexically_normal().wstring();
    for (auto& c : state_norm) {
        if (c == L'/') c = L'\\';
        else c = static_cast<wchar_t>(towlower(c));
    }
    if (!state_norm.empty() && (norm == state_norm || norm.starts_with(state_norm + L"\\"))) return true;
    auto has_segment = [&](const std::wstring& seg) {
        return norm == seg || norm.starts_with(seg + L"\\") ||
               norm.ends_with(L"\\" + seg) || norm.find(L"\\" + seg + L"\\") != std::wstring::npos;
    };
    if (has_segment(L".migratool")) return true;

    // Anti-feedback-loop: exclude SQLite journals/WAL/SHM, active VM disks, and temporary swap files
    static constexpr std::array<std::wstring_view, 11> hot_suffixes = {
        L"-wal", L"-shm", L"-journal", L".sqlite-wal", L".sqlite-shm", L".sqlite-journal",
        L".avhdx", L".vhdx", L".vmdk", L".tmp", L".crdownload"
    };
    for (auto sfx : hot_suffixes) {
        if (norm.size() >= sfx.size() && std::wstring_view(norm).ends_with(sfx)) return true;
    }
    auto slash = norm.rfind(L'\\');
    std::wstring_view base = (slash == std::wstring::npos) ? std::wstring_view(norm) : std::wstring_view(norm).substr(slash + 1);
    if (base.starts_with(L"~$") || base == L"catalogo.sqlite" || base == L"eventos_rutas.jsonl" ||
        base == L"vigilancia.jsonl" || base.find(L".new-") != std::wstring_view::npos) {
        return true;
    }

    // User-configured watch_ignore_globs
    if (p.config.contains("watch_ignore_globs") && p.config["watch_ignore_globs"].is_array()) {
        std::string utf8_path = utf8(norm);
        std::replace(utf8_path.begin(), utf8_path.end(), '\\', '/');
        for (const auto& g : p.config["watch_ignore_globs"]) {
            if (g.is_string() && fs_glob_match(g.get<std::string>(), utf8_path)) return true;
        }
    }
    return false;
}

uint64_t query_path_frn(const fs::path& path, uint64_t* out_parent_frn,
                        int64_t* out_size, int64_t* out_mtime_ns,
                        uint32_t* out_attrs, DWORD* out_reparse_tag) {
    auto npath = native_path(path);
    HANDLE h = CreateFileW(npath.c_str(), FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;
    struct CloseH { HANDLE v; ~CloseH() { if (v != INVALID_HANDLE_VALUE) CloseHandle(v); } } guard{h};

    BY_HANDLE_FILE_INFORMATION bhfi{};
    uint64_t frn = 0;
    if (GetFileInformationByHandle(h, &bhfi)) {
        frn = (static_cast<uint64_t>(bhfi.nFileIndexHigh) << 32) | bhfi.nFileIndexLow;
        if (out_size) *out_size = (static_cast<int64_t>(bhfi.nFileSizeHigh) << 32) | bhfi.nFileSizeLow;
        if (out_mtime_ns) *out_mtime_ns = filetime_to_unix_ns(bhfi.ftLastWriteTime);
        if (out_attrs) *out_attrs = bhfi.dwFileAttributes;
    }
    if (out_reparse_tag) {
        *out_reparse_tag = 0;
        if (bhfi.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            FILE_ATTRIBUTE_TAG_INFO tag_info{};
            if (GetFileInformationByHandleEx(h, FileAttributeTagInfo, &tag_info, sizeof(tag_info))) {
                *out_reparse_tag = tag_info.ReparseTag;
            }
        }
    }
    if (out_parent_frn) {
        *out_parent_frn = 0;
        auto parent = path.parent_path();
        if (!parent.empty() && parent != path) {
            *out_parent_frn = query_path_frn(parent);
        }
    }
    return frn;
}

void fs_schema(Database& db) {
    if (db.integer("SELECT COUNT(*) FROM meta WHERE key='fs_index_schema' AND value='2'") != 0) return;
    ScopedTx tx(db);
    if (db.integer("SELECT COUNT(*) FROM meta WHERE key='fs_index_schema' AND value='2'") != 0) {
        tx.commit();
        return;
    }
    sqlite3* raw = db.native_handle();
    const bool had_v1 = db.integer("SELECT COUNT(*) FROM meta WHERE key='fs_index_schema' AND value='1'") != 0;
    if (!had_v1) {
        exec_raw(raw, R"sql(
CREATE TABLE IF NOT EXISTS fs_roots (
  root_id         INTEGER PRIMARY KEY AUTOINCREMENT,
  root_key        TEXT NOT NULL UNIQUE,
  abs_path        TEXT NOT NULL,
  role            TEXT NOT NULL DEFAULT 'workspace',
  mode            TEXT NOT NULL DEFAULT 'workspace',
  volume_guid     TEXT NOT NULL DEFAULT '',
  volume_serial   INTEGER NOT NULL DEFAULT 0,
  usn_journal_id  INTEGER NOT NULL DEFAULT 0,
  next_usn        INTEGER NOT NULL DEFAULT 0,
  root_frn        INTEGER NOT NULL DEFAULT 0,
  engine          TEXT NOT NULL DEFAULT 'tier_b',
  complete        INTEGER NOT NULL DEFAULT 0,
  last_sync_ns    INTEGER NOT NULL DEFAULT 0,
  total_files     INTEGER NOT NULL DEFAULT 0,
  total_dirs      INTEGER NOT NULL DEFAULT 0,
  total_links     INTEGER NOT NULL DEFAULT 0,
  total_bytes     INTEGER NOT NULL DEFAULT 0,
  scan_errors     INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS fs_extensions (
  ext_id  INTEGER PRIMARY KEY AUTOINCREMENT,
  ext     TEXT NOT NULL UNIQUE COLLATE NOCASE
);
INSERT OR IGNORE INTO fs_extensions(ext_id, ext) VALUES(0, '');
CREATE TABLE IF NOT EXISTS fs_nodes (
  node_id     INTEGER PRIMARY KEY AUTOINCREMENT,
  root_id     INTEGER NOT NULL,
  frn         INTEGER NOT NULL,
  parent_frn  INTEGER NOT NULL,
  name        TEXT NOT NULL COLLATE NOCASE,
  ext_id      INTEGER NOT NULL DEFAULT 0,
  kind        INTEGER NOT NULL,
  size_bytes  INTEGER NOT NULL DEFAULT 0,
  mtime_ns    INTEGER NOT NULL DEFAULT 0,
  attrs       INTEGER NOT NULL DEFAULT 0,
  gen_seen    INTEGER NOT NULL DEFAULT 1,
  flags       INTEGER NOT NULL DEFAULT 0,
  link_target TEXT NOT NULL DEFAULT '',
  UNIQUE(root_id, frn)
);
CREATE TABLE IF NOT EXISTS fs_dirs (
  root_id      INTEGER NOT NULL,
  frn          INTEGER NOT NULL,
  parent_frn   INTEGER NOT NULL,
  rel_path     TEXT NOT NULL COLLATE NOCASE,
  depth        INTEGER NOT NULL DEFAULT 0,
  dir_mtime_ns INTEGER NOT NULL DEFAULT 0,
  direct_files INTEGER NOT NULL DEFAULT 0,
  direct_dirs  INTEGER NOT NULL DEFAULT 0,
  total_files  INTEGER NOT NULL DEFAULT 0,
  total_dirs   INTEGER NOT NULL DEFAULT 0,
  total_bytes  INTEGER NOT NULL DEFAULT 0,
  top_exts     TEXT NOT NULL DEFAULT '{}',
  PRIMARY KEY(root_id, frn)
) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS fs_change_log (
  seq         INTEGER PRIMARY KEY AUTOINCREMENT,
  at_ns       INTEGER NOT NULL,
  root_id     INTEGER NOT NULL,
  frn         INTEGER NOT NULL,
  op          TEXT NOT NULL,
  kind        INTEGER NOT NULL DEFAULT 0,
  old_path    TEXT NOT NULL DEFAULT '',
  new_path    TEXT NOT NULL DEFAULT '',
  size_bytes  INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS fs_errors (
  id         INTEGER PRIMARY KEY AUTOINCREMENT,
  root_id    INTEGER NOT NULL,
  at_ns      INTEGER NOT NULL,
  rel_path   TEXT NOT NULL,
  operation  TEXT NOT NULL,
  win32_code INTEGER NOT NULL DEFAULT 0,
  message    TEXT NOT NULL DEFAULT ''
);
)sql");
        create_fs_secondary_indexes(raw);

        std::string tok = "trigram";
        try {
            exec_raw(raw,
                "CREATE VIRTUAL TABLE IF NOT EXISTS fs_name_fts USING fts5(name, content='fs_nodes', content_rowid='node_id', tokenize='trigram');"
                "CREATE VIRTUAL TABLE IF NOT EXISTS fs_dir_fts USING fts5(root_id UNINDEXED, frn UNINDEXED, rel_path, tokenize='trigram');");
        } catch (...) {
            tok = "unicode61";
            exec_raw(raw,
                "CREATE VIRTUAL TABLE IF NOT EXISTS fs_name_fts USING fts5(name, content='fs_nodes', content_rowid='node_id', tokenize='unicode61');"
                "CREATE VIRTUAL TABLE IF NOT EXISTS fs_dir_fts USING fts5(root_id UNINDEXED, frn UNINDEXED, rel_path, tokenize='unicode61');");
        }
        db.exec("INSERT OR REPLACE INTO meta(key,value) VALUES('fs_fts_tokenizer',?)", {tok});
    }
    exec_raw(raw, R"sql(
CREATE TABLE IF NOT EXISTS fs_hashes (
  root_id        INTEGER NOT NULL,
  frn            INTEGER NOT NULL,
  size_bytes     INTEGER NOT NULL,
  mtime_ns       INTEGER NOT NULL,
  partial_sha256 TEXT NOT NULL DEFAULT '',
  full_sha256    TEXT NOT NULL DEFAULT '',
  computed_ns    INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY(root_id, frn)
) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS idx_fs_hashes_full ON fs_hashes(full_sha256) WHERE full_sha256 <> '';
CREATE INDEX IF NOT EXISTS idx_fs_nodes_size ON fs_nodes(root_id, size_bytes) WHERE kind=0 AND (flags & 1)=0;
)sql");
    db.exec("INSERT OR REPLACE INTO meta(key,value) VALUES('fs_index_schema','2')");
    tx.commit();
}

int64_t fs_ensure_root(const Project& p, Database& db, const Json& root_cfg) {
    fs_schema(db);
    const std::string key = root_cfg.at("id").get<std::string>();
    const std::string abs_path = path_text(p.resolve(root_cfg.at("path").get<std::string>()));
    const std::string role = root_cfg.value("role", "workspace");
    const std::string mode = project_mode(p);
    auto rows = db.query("SELECT root_id, abs_path, role, mode FROM fs_roots WHERE root_key=?", {key});
    if (!rows.empty()) {
        int64_t rid = rows[0]["root_id"].get<int64_t>();
        if (rows[0]["abs_path"] != abs_path || rows[0]["role"] != role || rows[0]["mode"] != mode) {
            db.exec("UPDATE fs_roots SET abs_path=?, role=?, mode=? WHERE root_id=?", {abs_path, role, mode, rid});
        }
        return rid;
    }
    db.exec("INSERT INTO fs_roots(root_key,abs_path,role,mode) VALUES(?,?,?,?)", {key, abs_path, role, mode});
    return db.integer("SELECT root_id FROM fs_roots WHERE root_key=?", {key});
}

Json fs_bulk_replace_root(Database& db, int64_t root_id, uint64_t root_frn, int64_t root_mtime_ns,
                          const std::vector<FsBulkEntry>& entries,
                          const std::vector<Json>& scan_errors,
                          bool build_trigram_fts,
                          Cancel* cancel) {
    fs_schema(db);
    auto t0 = std::chrono::steady_clock::now();
    sqlite3* raw = db.native_handle();

    // Boost SQLite cache during bulk load (restored afterwards)
    exec_raw(raw, "PRAGMA cache_size=-262144; PRAGMA mmap_size=1073741824; PRAGMA temp_store=MEMORY;");
    struct RestoreCache {
        sqlite3* r;
        ~RestoreCache() {
            sqlite3_exec(r, "PRAGMA cache_size=-65536; PRAGMA mmap_size=268435456;", nullptr, nullptr, nullptr);
        }
    } restore_cache{raw};

    // 1. Build extension dictionary in memory
    std::unordered_map<std::string, int32_t> ext_map;
    std::vector<std::string> ext_names;
    {
        auto existing = db.query("SELECT ext_id, ext FROM fs_extensions ORDER BY ext_id");
        for (const auto& row : existing) {
            int32_t id = static_cast<int32_t>(row["ext_id"].get<int64_t>());
            std::string e = to_lower_ascii(row["ext"].get<std::string>());
            ext_map[e] = id;
            if (static_cast<size_t>(id) >= ext_names.size()) ext_names.resize(static_cast<size_t>(id) + 1);
            ext_names[static_cast<size_t>(id)] = e;
        }
    }
    if (ext_names.empty()) {
        ext_names.push_back("");
        ext_map[""] = 0;
    }
    std::vector<std::pair<int32_t, std::string>> new_exts;
    auto intern_ext = [&](const std::string& e_raw) -> int32_t {
        if (e_raw.empty()) return 0;
        auto it = ext_map.find(e_raw);
        if (it != ext_map.end()) return it->second;
        int32_t id = static_cast<int32_t>(ext_names.size());
        ext_map.emplace(e_raw, id);
        ext_names.push_back(e_raw);
        new_exts.emplace_back(id, e_raw);
        return id;
    };

    // 2. Build directory hierarchy & bottom-up rollups in O(N) memory (~80 bytes/dir)
    struct DirBuild {
        uint64_t frn = 0;
        uint64_t parent_frn = 0;
        std::string name;
        std::string rel_path;
        int depth = -1;
        int64_t dir_mtime_ns = 0;
        int64_t direct_files = 0;
        int64_t direct_dirs = 0;
        int64_t total_files = 0;
        int64_t total_dirs = 0;
        int64_t total_bytes = 0;
        std::vector<std::pair<int32_t, int64_t>> ext_counts; // kept compact (top 16)
    };

    std::unordered_map<uint64_t, DirBuild> dirs;
    dirs.reserve(std::max<size_t>(entries.size() / 12, 256));
    {
        DirBuild root_d;
        root_d.frn = root_frn;
        root_d.parent_frn = 0;
        root_d.rel_path = "";
        root_d.depth = 0;
        root_d.dir_mtime_ns = root_mtime_ns;
        dirs.emplace(root_frn, std::move(root_d));
    }

    int64_t total_files = 0, total_dirs = 0, total_links = 0, total_bytes = 0;
    std::vector<int32_t> entry_ext_ids;
    entry_ext_ids.resize(entries.size(), 0);

    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        if (e.kind == static_cast<int>(FsNodeKind::Directory)) {
            if (e.frn == root_frn) continue;
            auto& d = dirs[e.frn];
            d.frn = e.frn;
            d.parent_frn = e.parent_frn;
            d.name = e.name;
            d.dir_mtime_ns = e.mtime_ns;
            ++total_dirs;
        } else {
            std::string ext = e.ext.empty() ? extract_ext_lower(e.name) : to_lower_ascii(e.ext);
            entry_ext_ids[i] = intern_ext(ext);
            if (e.kind == static_cast<int>(FsNodeKind::File)) {
                ++total_files;
                total_bytes += e.size_bytes;
            } else {
                ++total_links;
            }
        }
    }

    auto add_ext_count = [](std::vector<std::pair<int32_t, int64_t>>& vec, int32_t eid, int64_t cnt) {
        if (eid == 0 || cnt <= 0) return;
        for (auto& [k, v] : vec) {
            if (k == eid) { v += cnt; return; }
        }
        if (vec.size() < 24) {
            vec.emplace_back(eid, cnt);
        } else {
            auto min_it = std::min_element(vec.begin(), vec.end(),
                                           [](const auto& a, const auto& b) { return a.second < b.second; });
            if (min_it != vec.end() && cnt > min_it->second) *min_it = {eid, cnt};
        }
    };

    // Resolve rel_path and depth for all directories (memoized with cycle guard)
    std::function<void(uint64_t, int)> resolve_dir = [&](uint64_t frn, int guard_depth) {
        auto it = dirs.find(frn);
        if (it == dirs.end() || it->second.depth >= 0) return;
        if (guard_depth > 256) {
            it->second.depth = 1;
            it->second.rel_path = it->second.name;
            return;
        }
        uint64_t pfrn = it->second.parent_frn;
        auto pit = dirs.find(pfrn);
        if (pit == dirs.end() || pfrn == frn) {
            it->second.depth = 1;
            it->second.rel_path = it->second.name;
            return;
        }
        if (pit->second.depth < 0) resolve_dir(pfrn, guard_depth + 1);
        it->second.depth = pit->second.depth + 1;
        it->second.rel_path = join_rel(pit->second.rel_path, it->second.name);
    };
    for (auto& [frn, _] : dirs) resolve_dir(frn, 0);

    // Accumulate direct file/dir stats per directory
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        if (e.frn == root_frn) continue;
        auto pit = dirs.find(e.parent_frn);
        if (pit == dirs.end()) pit = dirs.find(root_frn);
        if (pit == dirs.end()) continue;
        if (e.kind == static_cast<int>(FsNodeKind::Directory)) {
            ++pit->second.direct_dirs;
        } else {
            ++pit->second.direct_files;
            pit->second.total_files += 1;
            pit->second.total_bytes += e.size_bytes;
            if (entry_ext_ids[i] != 0) add_ext_count(pit->second.ext_counts, entry_ext_ids[i], 1);
        }
    }

    // Bottom-up rollup sorted by depth descending
    std::vector<DirBuild*> ordered_dirs;
    ordered_dirs.reserve(dirs.size());
    for (auto& [_, d] : dirs) ordered_dirs.push_back(&d);
    std::sort(ordered_dirs.begin(), ordered_dirs.end(),
              [](const DirBuild* a, const DirBuild* b) { return a->depth > b->depth; });

    for (DirBuild* d : ordered_dirs) {
        if (d->frn == root_frn || d->parent_frn == 0) continue;
        auto pit = dirs.find(d->parent_frn);
        if (pit == dirs.end() || pit->first == d->frn) continue;
        pit->second.total_files += d->total_files;
        pit->second.total_dirs += d->total_dirs + 1;
        pit->second.total_bytes += d->total_bytes;
        for (const auto& [eid, cnt] : d->ext_counts) {
            add_ext_count(pit->second.ext_counts, eid, cnt);
        }
    }

    // 3. Persist new extensions and wipe old rows for this root
    const bool other_roots_have_nodes = db.integer("SELECT EXISTS(SELECT 1 FROM fs_nodes WHERE root_id<>?)", {root_id}) != 0;
    {
        db.begin();
        try {
            if (!new_exts.empty()) {
                RawStmt ins_ext(raw, "INSERT OR IGNORE INTO fs_extensions(ext_id,ext) VALUES(?,?)");
                for (const auto& [eid, ename] : new_exts) {
                    ins_ext.reset();
                    sqlite3_bind_int64(ins_ext.s, 1, eid);
                    sqlite3_bind_text(ins_ext.s, 2, ename.c_str(), static_cast<int>(ename.size()), SQLITE_STATIC);
                    sqlite3_step(ins_ext.s);
                }
            }
            if (!other_roots_have_nodes) {
                // Drop secondary indexes for maximum sequential bulk-insert throughput
                exec_raw(raw,
                    "DROP INDEX IF EXISTS idx_fs_nodes_parent;"
                    "DROP INDEX IF EXISTS idx_fs_nodes_ext;"
                    "DROP INDEX IF EXISTS idx_fs_nodes_mtime;"
                    "DROP INDEX IF EXISTS idx_fs_dirs_path;"
                    "DROP INDEX IF EXISTS idx_fs_dirs_parent;"
                    "DELETE FROM fs_nodes;"
                    "DELETE FROM fs_dirs;"
                    "DELETE FROM fs_dir_fts;");
            } else {
                db.exec("DELETE FROM fs_nodes WHERE root_id=?", {root_id});
                db.exec("DELETE FROM fs_dirs WHERE root_id=?", {root_id});
                db.exec("DELETE FROM fs_dir_fts WHERE root_id=?", {root_id});
            }
            db.exec("DELETE FROM fs_errors WHERE root_id=?", {root_id});
            db.commit();
        } catch (...) {
            db.rollback();
            throw;
        }
    }

    // 4. Bulk-insert fs_nodes in 50,000-row transactions via reused raw sqlite3_stmt
    constexpr size_t BATCH_SIZE = 50000;
    {
        RawStmt ins_node(raw,
            "INSERT OR REPLACE INTO fs_nodes(root_id,frn,parent_frn,name,ext_id,kind,size_bytes,mtime_ns,attrs,gen_seen,flags,link_target) "
            "VALUES(?,?,?,?,?,?,?,?,?,1,0,?)");
        for (size_t off = 0; off < entries.size(); off += BATCH_SIZE) {
            if (cancel && cancel->requested) throw std::runtime_error("CANCELLED");
            const size_t end = std::min(entries.size(), off + BATCH_SIZE);
            db.begin();
            try {
                for (size_t i = off; i < end; ++i) {
                    const auto& e = entries[i];
                    if (e.frn == root_frn) continue;
                    ins_node.reset();
                    sqlite3_bind_int64(ins_node.s, 1, root_id);
                    sqlite3_bind_int64(ins_node.s, 2, static_cast<int64_t>(e.frn));
                    sqlite3_bind_int64(ins_node.s, 3, static_cast<int64_t>(e.parent_frn));
                    sqlite3_bind_text(ins_node.s, 4, e.name.c_str(), static_cast<int>(e.name.size()), SQLITE_STATIC);
                    sqlite3_bind_int64(ins_node.s, 5, entry_ext_ids[i]);
                    sqlite3_bind_int(ins_node.s, 6, e.kind);
                    sqlite3_bind_int64(ins_node.s, 7, e.size_bytes);
                    sqlite3_bind_int64(ins_node.s, 8, e.mtime_ns);
                    sqlite3_bind_int64(ins_node.s, 9, static_cast<int64_t>(e.attrs));
                    sqlite3_bind_text(ins_node.s, 10, e.link_target.c_str(), static_cast<int>(e.link_target.size()), SQLITE_STATIC);
                    int rc = sqlite3_step(ins_node.s);
                    if (rc != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(raw));
                }
                db.commit();
            } catch (...) {
                db.rollback();
                throw;
            }
        }
    }

    // 5. Bulk-insert fs_dirs + fs_dir_fts in 50,000-row transactions
    {
        RawStmt ins_dir(raw,
            "INSERT OR REPLACE INTO fs_dirs(root_id,frn,parent_frn,rel_path,depth,dir_mtime_ns,direct_files,direct_dirs,total_files,total_dirs,total_bytes,top_exts) "
            "VALUES(?,?,?,?,?,?,?,?,?,?,?,?)");
        RawStmt ins_dfts(raw, "INSERT INTO fs_dir_fts(root_id,frn,rel_path) VALUES(?,?,?)");

        for (size_t off = 0; off < ordered_dirs.size(); off += BATCH_SIZE) {
            if (cancel && cancel->requested) throw std::runtime_error("CANCELLED");
            const size_t end = std::min(ordered_dirs.size(), off + BATCH_SIZE);
            db.begin();
            try {
                for (size_t i = off; i < end; ++i) {
                    auto* d = ordered_dirs[i];
                    std::sort(d->ext_counts.begin(), d->ext_counts.end(),
                              [](const auto& a, const auto& b) { return a.second > b.second; });
                    Json top = Json::object();
                    for (size_t k = 0; k < std::min<size_t>(8, d->ext_counts.size()); ++k) {
                        int32_t eid = d->ext_counts[k].first;
                        if (eid > 0 && static_cast<size_t>(eid) < ext_names.size()) {
                            top[ext_names[static_cast<size_t>(eid)]] = d->ext_counts[k].second;
                        }
                    }
                    std::string top_str = top.dump();
                    ins_dir.reset();
                    sqlite3_bind_int64(ins_dir.s, 1, root_id);
                    sqlite3_bind_int64(ins_dir.s, 2, static_cast<int64_t>(d->frn));
                    sqlite3_bind_int64(ins_dir.s, 3, static_cast<int64_t>(d->parent_frn));
                    sqlite3_bind_text(ins_dir.s, 4, d->rel_path.c_str(), static_cast<int>(d->rel_path.size()), SQLITE_STATIC);
                    sqlite3_bind_int(ins_dir.s, 5, d->depth);
                    sqlite3_bind_int64(ins_dir.s, 6, d->dir_mtime_ns);
                    sqlite3_bind_int64(ins_dir.s, 7, d->direct_files);
                    sqlite3_bind_int64(ins_dir.s, 8, d->direct_dirs);
                    sqlite3_bind_int64(ins_dir.s, 9, d->total_files);
                    sqlite3_bind_int64(ins_dir.s, 10, d->total_dirs);
                    sqlite3_bind_int64(ins_dir.s, 11, d->total_bytes);
                    sqlite3_bind_text(ins_dir.s, 12, top_str.c_str(), static_cast<int>(top_str.size()), SQLITE_TRANSIENT);
                    if (sqlite3_step(ins_dir.s) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(raw));

                    if (!d->rel_path.empty()) {
                        ins_dfts.reset();
                        sqlite3_bind_int64(ins_dfts.s, 1, root_id);
                        sqlite3_bind_int64(ins_dfts.s, 2, static_cast<int64_t>(d->frn));
                        sqlite3_bind_text(ins_dfts.s, 3, d->rel_path.c_str(), static_cast<int>(d->rel_path.size()), SQLITE_STATIC);
                        if (sqlite3_step(ins_dfts.s) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(raw));
                    }
                }
                db.commit();
            } catch (...) {
                db.rollback();
                throw;
            }
        }
    }

    auto t_insert = std::chrono::steady_clock::now();

    // 6. Recreate secondary indexes + rebuild external-content FTS5 + persist errors & root stats
    {
        db.begin();
        try {
            if (!scan_errors.empty()) {
                RawStmt ins_err(raw, "INSERT INTO fs_errors(root_id,at_ns,rel_path,operation,win32_code,message) VALUES(?,?,?,?,?,?)");
                const int64_t ts = now_ns();
                for (const auto& er : scan_errors) {
                    ins_err.reset();
                    std::string rp = er.value("path", "");
                    std::string op = er.value("operation", "enumerate");
                    std::string msg = er.value("message", "");
                    sqlite3_bind_int64(ins_err.s, 1, root_id);
                    sqlite3_bind_int64(ins_err.s, 2, ts);
                    sqlite3_bind_text(ins_err.s, 3, rp.c_str(), static_cast<int>(rp.size()), SQLITE_TRANSIENT);
                    sqlite3_bind_text(ins_err.s, 4, op.c_str(), static_cast<int>(op.size()), SQLITE_TRANSIENT);
                    sqlite3_bind_int64(ins_err.s, 5, er.value("code", int64_t(0)));
                    sqlite3_bind_text(ins_err.s, 6, msg.c_str(), static_cast<int>(msg.size()), SQLITE_TRANSIENT);
                    sqlite3_step(ins_err.s);
                }
            }
            create_fs_secondary_indexes(raw);
            if (build_trigram_fts) {
                exec_raw(raw, "INSERT INTO fs_name_fts(fs_name_fts) VALUES('rebuild');");
            }
            db.exec("UPDATE fs_roots SET root_frn=?, complete=1, last_sync_ns=?, total_files=?, total_dirs=?, "
                    "total_links=?, total_bytes=?, scan_errors=? WHERE root_id=?",
                    {static_cast<int64_t>(root_frn), now_ns(), total_files, total_dirs, total_links,
                     total_bytes, static_cast<int64_t>(scan_errors.size()), root_id});
            db.commit();
        } catch (...) {
            db.rollback();
            throw;
        }
    }

    auto t_end = std::chrono::steady_clock::now();
    return {
        {"root_id", root_id},
        {"files", total_files},
        {"dirs", total_dirs},
        {"links", total_links},
        {"total_entries", total_files + total_dirs + total_links},
        {"total_bytes", total_bytes},
        {"scan_errors", scan_errors.size()},
        {"insert_ms", std::chrono::duration_cast<std::chrono::milliseconds>(t_insert - t0).count()},
        {"index_fts_ms", std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_insert).count()},
        {"total_ms", std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t0).count()}
    };
}

void fs_apply_upsert(Database& db, int64_t root_id, uint64_t frn, uint64_t parent_frn,
                     const std::string& name, int kind, int64_t size_bytes, int64_t mtime_ns,
                     uint32_t attrs, const std::string& link_target, bool log_change) {
    if (frn == 0 || name.empty()) return;
    ScopedTx tx(db);

    const std::string ext = (kind == static_cast<int>(FsNodeKind::Directory)) ? "" : extract_ext_lower(name);
    const int64_t ext_id = get_or_create_ext_id(db, ext);

    auto existing = db.query("SELECT node_id, parent_frn, name, kind, size_bytes, flags FROM fs_nodes WHERE root_id=? AND frn=?",
                             {root_id, static_cast<int64_t>(frn)});
    if (!existing.empty()) {
        const auto& row = existing[0];
        const int64_t node_id = row["node_id"].get<int64_t>();
        const uint64_t old_parent = static_cast<uint64_t>(row["parent_frn"].get<int64_t>());
        const std::string old_name = row["name"].get<std::string>();
        const int old_kind = row["kind"].get<int>();
        const int64_t old_size = row["size_bytes"].get<int64_t>();
        const int64_t old_flags = row["flags"].get<int64_t>();
        const bool was_deleted = (old_flags & 1) != 0;

        if (!was_deleted && (old_parent != parent_frn || old_name != name)) {
            fs_apply_rename_or_move(db, root_id, frn, parent_frn, name, mtime_ns, log_change);
        }

        if (was_deleted || old_size != size_bytes || old_kind != kind) {
            if (was_deleted) {
                db.exec("INSERT INTO fs_name_fts(rowid, name) VALUES(?,?)", {node_id, name});
            }
            db.exec("UPDATE fs_nodes SET parent_frn=?, name=?, ext_id=?, kind=?, size_bytes=?, mtime_ns=?, attrs=?, flags=0, link_target=? WHERE node_id=?",
                    {static_cast<int64_t>(parent_frn), name, ext_id, kind, size_bytes, mtime_ns, static_cast<int64_t>(attrs), link_target, node_id});
            if (kind == static_cast<int>(FsNodeKind::File)) {
                int64_t d_files = was_deleted ? 1 : 0;
                int64_t d_bytes = was_deleted ? size_bytes : (size_bytes - old_size);
                if (was_deleted) {
                    db.exec("UPDATE fs_dirs SET direct_files=direct_files+1 WHERE root_id=? AND frn=?",
                            {root_id, static_cast<int64_t>(parent_frn)});
                }
                update_ancestors_delta(db, root_id, parent_frn, d_files, 0, d_bytes);
            }
            if (log_change) {
                std::string rel = join_rel(lookup_dir_rel(db, root_id, parent_frn), name);
                log_fs_change(db, root_id, frn, was_deleted ? "created" : "modified", kind, "", rel, size_bytes);
            }
        } else {
            db.exec("UPDATE fs_nodes SET mtime_ns=?, attrs=?, link_target=? WHERE node_id=?",
                    {mtime_ns, static_cast<int64_t>(attrs), link_target, node_id});
        }
        tx.commit();
        return;
    }

    // Evict any superseded FRN previously occupying the exact same (parent_frn, name)
    // (happens routinely on Windows atomic temp-file-replace saves like MoveFileExW(REPLACE_EXISTING))
    bool replaced_existing = false;
    for (const auto& dup : db.query(
             "SELECT frn FROM fs_nodes WHERE root_id=? AND parent_frn=? AND name=? COLLATE NOCASE AND frn<>? AND (flags & 1)=0",
             {root_id, static_cast<int64_t>(parent_frn), name, static_cast<int64_t>(frn)})) {
        fs_apply_delete(db, root_id, static_cast<uint64_t>(dup["frn"].get<int64_t>()), false);
        replaced_existing = true;
    }

    // Brand new node
    db.exec("INSERT INTO fs_nodes(root_id,frn,parent_frn,name,ext_id,kind,size_bytes,mtime_ns,attrs,gen_seen,flags,link_target) "
            "VALUES(?,?,?,?,?,?,?,?,?,1,0,?)",
            {root_id, static_cast<int64_t>(frn), static_cast<int64_t>(parent_frn), name, ext_id, kind,
             size_bytes, mtime_ns, static_cast<int64_t>(attrs), link_target});
    const int64_t node_id = sqlite3_last_insert_rowid(db.native_handle());
    db.exec("INSERT INTO fs_name_fts(rowid, name) VALUES(?,?)", {node_id, name});

    int pdepth = 0;
    const std::string parent_rel = lookup_dir_rel(db, root_id, parent_frn, &pdepth);
    const std::string rel = join_rel(parent_rel, name);

    if (kind == static_cast<int>(FsNodeKind::Directory)) {
        db.exec("INSERT OR REPLACE INTO fs_dirs(root_id,frn,parent_frn,rel_path,depth,dir_mtime_ns,direct_files,direct_dirs,total_files,total_dirs,total_bytes,top_exts) "
                "VALUES(?,?,?,?,?,?,0,0,0,0,0,'{}')",
                {root_id, static_cast<int64_t>(frn), static_cast<int64_t>(parent_frn), rel, pdepth + 1, mtime_ns});
        db.exec("INSERT INTO fs_dir_fts(root_id,frn,rel_path) VALUES(?,?,?)",
                {root_id, static_cast<int64_t>(frn), rel});
        db.exec("UPDATE fs_dirs SET direct_dirs=direct_dirs+1 WHERE root_id=? AND frn=?",
                {root_id, static_cast<int64_t>(parent_frn)});
        update_ancestors_delta(db, root_id, parent_frn, 0, 1, 0);
    } else if (kind == static_cast<int>(FsNodeKind::File)) {
        db.exec("UPDATE fs_dirs SET direct_files=direct_files+1 WHERE root_id=? AND frn=?",
                {root_id, static_cast<int64_t>(parent_frn)});
        update_ancestors_delta(db, root_id, parent_frn, 1, 0, size_bytes);
    } else {
        db.exec("UPDATE fs_roots SET total_links=total_links+1, last_sync_ns=? WHERE root_id=?", {now_ns(), root_id});
    }

    if (log_change) {
        log_fs_change(db, root_id, frn, replaced_existing ? "modified" : "created", kind, "", rel, size_bytes);
    }
    tx.commit();
}

bool fs_apply_rename_or_move(Database& db, int64_t root_id, uint64_t frn,
                             uint64_t new_parent_frn, const std::string& new_name,
                             int64_t new_mtime_ns, bool log_change) {
    if (frn == 0 || new_name.empty()) return false;
    ScopedTx tx(db);

    auto rows = db.query("SELECT node_id, parent_frn, name, kind, size_bytes, flags FROM fs_nodes WHERE root_id=? AND frn=?",
                         {root_id, static_cast<int64_t>(frn)});
    if (rows.empty() || (rows[0]["flags"].get<int64_t>() & 1) != 0) {
        tx.commit();
        return false;
    }

    const int64_t node_id = rows[0]["node_id"].get<int64_t>();
    const uint64_t old_parent_frn = static_cast<uint64_t>(rows[0]["parent_frn"].get<int64_t>());
    const std::string old_name = rows[0]["name"].get<std::string>();
    const int kind = rows[0]["kind"].get<int>();
    const int64_t size_bytes = rows[0]["size_bytes"].get<int64_t>();

    if (old_parent_frn == new_parent_frn && old_name == new_name) {
        tx.commit();
        return true;
    }

    int old_pdepth = 0, new_pdepth = 0;
    const std::string old_parent_rel = lookup_dir_rel(db, root_id, old_parent_frn, &old_pdepth);
    const std::string new_parent_rel = lookup_dir_rel(db, root_id, new_parent_frn, &new_pdepth);
    const std::string old_rel = join_rel(old_parent_rel, old_name);
    const std::string new_rel = join_rel(new_parent_rel, new_name);

    const std::string new_ext = (kind == static_cast<int>(FsNodeKind::Directory)) ? "" : extract_ext_lower(new_name);
    const int64_t new_ext_id = get_or_create_ext_id(db, new_ext);

    // If destination (new_parent_frn, new_name) was occupied by another FRN (overwrite rename), evict it first
    for (const auto& dup : db.query(
             "SELECT frn FROM fs_nodes WHERE root_id=? AND parent_frn=? AND name=? COLLATE NOCASE AND frn<>? AND (flags & 1)=0",
             {root_id, static_cast<int64_t>(new_parent_frn), new_name, static_cast<int64_t>(frn)})) {
        fs_apply_delete(db, root_id, static_cast<uint64_t>(dup["frn"].get<int64_t>()), false);
    }

    // 1. Update ONE row in fs_nodes + external-content fs_name_fts (O(1) even if dir has 500,000 child files!)
    db.exec("INSERT INTO fs_name_fts(fs_name_fts, rowid, name) VALUES('delete',?,?)", {node_id, old_name});
    db.exec("UPDATE fs_nodes SET parent_frn=?, name=?, ext_id=?, mtime_ns=CASE WHEN ?>0 THEN ? ELSE mtime_ns END WHERE node_id=?",
            {static_cast<int64_t>(new_parent_frn), new_name, new_ext_id, new_mtime_ns, new_mtime_ns, node_id});
    db.exec("INSERT INTO fs_name_fts(rowid, name) VALUES(?,?)", {node_id, new_name});

    // 2. If directory, prefix-swap ONLY fs_dirs (zero touches to child files in fs_nodes!)
    if (kind == static_cast<int>(FsNodeKind::Directory)) {
        auto drow = db.query("SELECT rel_path, depth, total_files, total_dirs, total_bytes FROM fs_dirs WHERE root_id=? AND frn=?",
                             {root_id, static_cast<int64_t>(frn)});
        if (!drow.empty()) {
            const std::string prev_rel = drow[0]["rel_path"].get<std::string>();
            const int prev_depth = drow[0]["depth"].get<int>();
            const int64_t d_files = drow[0]["total_files"].get<int64_t>();
            const int64_t d_dirs = drow[0]["total_dirs"].get<int64_t>();
            const int64_t d_bytes = drow[0]["total_bytes"].get<int64_t>();
            const int new_depth = new_pdepth + 1;
            const int depth_delta = new_depth - prev_depth;

            const std::string old_pfx = prev_rel + "/";
            const std::string old_hi = prev_rel + "0"; // ASCII '/' (47) + 1 == '0' (48)
            const std::string new_pfx = new_rel + "/";

            // Remove old paths from fs_dir_fts
            db.exec("DELETE FROM fs_dir_fts WHERE root_id=? AND (frn=? OR (rel_path>=? AND rel_path<?))",
                    {root_id, static_cast<int64_t>(frn), old_pfx, old_hi});

            // Update moved dir itself
            db.exec("UPDATE fs_dirs SET parent_frn=?, rel_path=?, depth=?, dir_mtime_ns=CASE WHEN ?>0 THEN ? ELSE dir_mtime_ns END WHERE root_id=? AND frn=?",
                    {static_cast<int64_t>(new_parent_frn), new_rel, new_depth, new_mtime_ns, new_mtime_ns, root_id, static_cast<int64_t>(frn)});

            // Update descendant subdirs in one B-tree range SQL statement
            if (!prev_rel.empty()) {
                const int64_t cut_pos = static_cast<int64_t>(old_pfx.size()) + 1;
                db.exec("UPDATE fs_dirs SET rel_path=? || substr(rel_path, ?), depth=depth+? WHERE root_id=? AND rel_path>=? AND rel_path<?",
                        {new_pfx, cut_pos, depth_delta, root_id, old_pfx, old_hi});
            }

            // Re-index moved dir + descendant subdirs into fs_dir_fts
            const std::string new_hi = new_rel + "0";
            db.exec("INSERT INTO fs_dir_fts(root_id,frn,rel_path) SELECT root_id,frn,rel_path FROM fs_dirs WHERE root_id=? AND (frn=? OR (rel_path>=? AND rel_path<?))",
                    {root_id, static_cast<int64_t>(frn), new_pfx, new_hi});

            // Propagate rollup deltas only if parent changed
            if (old_parent_frn != new_parent_frn) {
                db.exec("UPDATE fs_dirs SET direct_dirs=MAX(0,direct_dirs-1) WHERE root_id=? AND frn=?",
                        {root_id, static_cast<int64_t>(old_parent_frn)});
                db.exec("UPDATE fs_dirs SET direct_dirs=direct_dirs+1 WHERE root_id=? AND frn=?",
                        {root_id, static_cast<int64_t>(new_parent_frn)});
                update_ancestors_delta(db, root_id, old_parent_frn, -d_files, -(d_dirs + 1), -d_bytes);
                update_ancestors_delta(db, root_id, new_parent_frn, +d_files, +(d_dirs + 1), +d_bytes);
            }
        }
    } else if (kind == static_cast<int>(FsNodeKind::File) && old_parent_frn != new_parent_frn) {
        db.exec("UPDATE fs_dirs SET direct_files=MAX(0,direct_files-1) WHERE root_id=? AND frn=?",
                {root_id, static_cast<int64_t>(old_parent_frn)});
        db.exec("UPDATE fs_dirs SET direct_files=direct_files+1 WHERE root_id=? AND frn=?",
                {root_id, static_cast<int64_t>(new_parent_frn)});
        update_ancestors_delta(db, root_id, old_parent_frn, -1, 0, -size_bytes);
        update_ancestors_delta(db, root_id, new_parent_frn, +1, 0, +size_bytes);
    }

    if (log_change) {
        const std::string op = (old_parent_frn == new_parent_frn) ? "renamed" : "moved";
        log_fs_change(db, root_id, frn, op, kind, old_rel, new_rel, size_bytes);
    }
    tx.commit();
    return true;
}

bool fs_apply_delete(Database& db, int64_t root_id, uint64_t frn, bool log_change) {
    if (frn == 0) return false;
    ScopedTx tx(db);

    auto rows = db.query("SELECT node_id, parent_frn, name, kind, size_bytes, flags FROM fs_nodes WHERE root_id=? AND frn=?",
                         {root_id, static_cast<int64_t>(frn)});
    if (rows.empty() || (rows[0]["flags"].get<int64_t>() & 1) != 0) {
        tx.commit();
        return false;
    }

    const int64_t node_id = rows[0]["node_id"].get<int64_t>();
    const uint64_t parent_frn = static_cast<uint64_t>(rows[0]["parent_frn"].get<int64_t>());
    const std::string name = rows[0]["name"].get<std::string>();
    const int kind = rows[0]["kind"].get<int>();
    const int64_t size_bytes = rows[0]["size_bytes"].get<int64_t>();
    const std::string rel = join_rel(lookup_dir_rel(db, root_id, parent_frn), name);

    if (kind == static_cast<int>(FsNodeKind::Directory)) {
        // Collect ALL descendant directories recursively by parent_frn (immune to any rel_path mismatch)
        auto subdirs = db.query(
            "WITH RECURSIVE sub(frn) AS ("
            "  VALUES(?)"
            "  UNION ALL"
            "  SELECT n.frn FROM fs_nodes n JOIN sub s ON n.parent_frn=s.frn "
            "  WHERE n.root_id=? AND n.kind=1 AND (n.flags & 1)=0"
            ") SELECT frn FROM sub",
            {static_cast<int64_t>(frn), root_id});

        int64_t actual_files = 0, actual_dirs = 0, actual_links = 0, actual_bytes = 0;
        for (const auto& sd : subdirs) {
            const int64_t sd_frn = sd["frn"].get<int64_t>();
            auto children = db.query(
                "SELECT node_id, frn, name, kind, size_bytes FROM fs_nodes "
                "WHERE root_id=? AND parent_frn=? AND (flags & 1)=0",
                {root_id, sd_frn});
            for (const auto& ch : children) {
                db.exec("INSERT INTO fs_name_fts(fs_name_fts, rowid, name) VALUES('delete',?,?)",
                        {ch["node_id"], ch["name"]});
                const int ck = ch["kind"].get<int>();
                if (ck == static_cast<int>(FsNodeKind::File)) {
                    ++actual_files;
                    actual_bytes += ch["size_bytes"].get<int64_t>();
                } else if (ck == static_cast<int>(FsNodeKind::Directory)) {
                    ++actual_dirs;
                } else {
                    ++actual_links;
                }
            }
            db.exec("DELETE FROM fs_hashes WHERE root_id=? AND frn IN (SELECT frn FROM fs_nodes WHERE root_id=? AND parent_frn=?)",
                    {root_id, root_id, sd_frn});
            db.exec("DELETE FROM fs_nodes WHERE root_id=? AND parent_frn=?", {root_id, sd_frn});
            db.exec("DELETE FROM fs_hashes WHERE root_id=? AND frn=?", {root_id, sd_frn});
            db.exec("DELETE FROM fs_dir_fts WHERE root_id=? AND frn=?", {root_id, sd_frn});
            db.exec("DELETE FROM fs_dirs WHERE root_id=? AND frn=?", {root_id, sd_frn});
        }
        db.exec("INSERT INTO fs_name_fts(fs_name_fts, rowid, name) VALUES('delete',?,?)", {node_id, name});
        db.exec("DELETE FROM fs_nodes WHERE node_id=?", {node_id});
        db.exec("UPDATE fs_dirs SET direct_dirs=MAX(0,direct_dirs-1) WHERE root_id=? AND frn=?",
                {root_id, static_cast<int64_t>(parent_frn)});
        update_ancestors_delta(db, root_id, parent_frn, -actual_files, -(actual_dirs + 1), -actual_bytes);
        if (actual_links > 0) {
            db.exec("UPDATE fs_roots SET total_links=MAX(0,total_links-?), last_sync_ns=? WHERE root_id=?",
                    {actual_links, now_ns(), root_id});
        }
    } else {
        db.exec("INSERT INTO fs_name_fts(fs_name_fts, rowid, name) VALUES('delete',?,?)", {node_id, name});
        db.exec("DELETE FROM fs_nodes WHERE node_id=?", {node_id});
        db.exec("DELETE FROM fs_hashes WHERE root_id=? AND frn=?", {root_id, static_cast<int64_t>(frn)});
        if (kind == static_cast<int>(FsNodeKind::File)) {
            db.exec("UPDATE fs_dirs SET direct_files=MAX(0,direct_files-1) WHERE root_id=? AND frn=?",
                    {root_id, static_cast<int64_t>(parent_frn)});
            update_ancestors_delta(db, root_id, parent_frn, -1, 0, -size_bytes);
        } else {
            db.exec("UPDATE fs_roots SET total_links=MAX(0,total_links-1), last_sync_ns=? WHERE root_id=?", {now_ns(), root_id});
        }
    }

    if (log_change) {
        log_fs_change(db, root_id, frn, "deleted", kind, rel, "", size_bytes);
    }
    tx.commit();
    return true;
}

Json fs_rebuild_dir_rollups(Database& db, int64_t root_id) {
    fs_schema(db);
    auto t0 = std::chrono::steady_clock::now();
    ScopedTx tx(db);

    auto rrows = db.query("SELECT root_frn FROM fs_roots WHERE root_id=?", {root_id});
    if (rrows.empty()) {
        tx.commit();
        return {{"rebuilt", false}, {"reason", "ROOT_NOT_FOUND"}};
    }
    const uint64_t root_frn = static_cast<uint64_t>(rrows[0]["root_frn"].get<int64_t>());

    // Load extension names
    std::vector<std::string> ext_names;
    db.each("SELECT ext_id, ext FROM fs_extensions ORDER BY ext_id", Json::array(), [&](const Json& r) {
        size_t id = static_cast<size_t>(std::max<int64_t>(0, r["ext_id"].get<int64_t>()));
        if (id >= ext_names.size()) ext_names.resize(id + 1);
        ext_names[id] = r["ext"].get<std::string>();
        return true;
    });

    struct DirAcc {
        uint64_t frn = 0;
        uint64_t parent_frn = 0;
        std::string name;
        std::string rel_path;
        int depth = -1;
        int64_t mtime_ns = 0;
        int64_t direct_files = 0;
        int64_t direct_dirs = 0;
        int64_t total_files = 0;
        int64_t total_dirs = 0;
        int64_t total_bytes = 0;
        std::vector<std::pair<int32_t, int64_t>> ext_counts;
    };

    std::unordered_map<uint64_t, DirAcc> dirs;
    // Preserve root_dir mtime
    int64_t root_mtime = 0;
    auto rd_row = db.query("SELECT dir_mtime_ns FROM fs_dirs WHERE root_id=? AND frn=?",
                           {root_id, static_cast<int64_t>(root_frn)});
    if (!rd_row.empty()) root_mtime = rd_row[0]["dir_mtime_ns"].get<int64_t>();
    {
        DirAcc root_d;
        root_d.frn = root_frn;
        root_d.parent_frn = 0;
        root_d.rel_path = "";
        root_d.depth = 0;
        root_d.mtime_ns = root_mtime;
        dirs.emplace(root_frn, std::move(root_d));
    }

    // Pass 1: Load all live directory nodes from fs_nodes
    db.each("SELECT frn, parent_frn, name, mtime_ns FROM fs_nodes WHERE root_id=? AND kind=1 AND (flags & 1)=0",
            {root_id}, [&](const Json& r) {
                uint64_t f = static_cast<uint64_t>(r["frn"].get<int64_t>());
                if (f == root_frn) return true;
                DirAcc d;
                d.frn = f;
                d.parent_frn = static_cast<uint64_t>(r["parent_frn"].get<int64_t>());
                d.name = r["name"].get<std::string>();
                d.mtime_ns = r["mtime_ns"].get<int64_t>();
                dirs.emplace(f, std::move(d));
                return true;
            });

    // Resolve rel_path & depth for all reachable dirs from root_frn; drop any disconnected orphan dirs
    std::function<int(uint64_t, int)> resolve_dir = [&](uint64_t f, int guard) -> int {
        auto it = dirs.find(f);
        if (it == dirs.end()) return -2;
        if (it->second.depth >= 0) return it->second.depth;
        if (it->second.depth == -2 || guard > 256 || it->second.parent_frn == 0 || it->second.parent_frn == f) {
            it->second.depth = -2;
            return -2;
        }
        it->second.depth = -2; // cycle guard
        int pd = resolve_dir(it->second.parent_frn, guard + 1);
        if (pd < 0) return -2;
        auto pit = dirs.find(it->second.parent_frn);
        it->second.rel_path = pit->second.rel_path.empty() ? it->second.name : (pit->second.rel_path + "/" + it->second.name);
        it->second.depth = pd + 1;
        return it->second.depth;
    };
    std::vector<uint64_t> orphan_dir_frns;
    for (auto& [f, _] : dirs) {
        if (resolve_dir(f, 0) < 0) orphan_dir_frns.push_back(f);
    }
    for (uint64_t of : orphan_dir_frns) dirs.erase(of);

    // Count direct_dirs
    for (const auto& [f, d] : dirs) {
        if (f == root_frn) continue;
        auto pit = dirs.find(d.parent_frn);
        if (pit != dirs.end()) ++pit->second.direct_dirs;
    }

    auto add_ext_count = [](std::vector<std::pair<int32_t, int64_t>>& vec, int32_t eid, int64_t cnt) {
        if (eid <= 0 || cnt <= 0) return;
        for (auto& [k, v] : vec) {
            if (k == eid) { v += cnt; return; }
        }
        if (vec.size() < 24) vec.emplace_back(eid, cnt);
        else {
            auto min_it = std::min_element(vec.begin(), vec.end(),
                                           [](const auto& a, const auto& b) { return a.second < b.second; });
            if (min_it != vec.end() && cnt > min_it->second) *min_it = {eid, cnt};
        }
    };

    // Pass 2: Accumulate all live non-dir nodes into their exact parent directory; collect orphan nodes to purge
    int64_t total_files = 0, total_links = 0, total_bytes = 0, orphans_purged = 0;
    std::vector<std::pair<int64_t, std::string>> orphan_nodes;
    db.each("SELECT node_id, frn, parent_frn, name, ext_id, kind, size_bytes FROM fs_nodes "
            "WHERE root_id=? AND kind<>1 AND (flags & 1)=0",
            {root_id}, [&](const Json& r) {
                uint64_t pf = static_cast<uint64_t>(r["parent_frn"].get<int64_t>());
                auto pit = dirs.find(pf);
                if (pit == dirs.end()) {
                    orphan_nodes.emplace_back(r["node_id"].get<int64_t>(), r["name"].get<std::string>());
                    return true;
                }
                const int k = r["kind"].get<int>();
                const int64_t sz = r["size_bytes"].get<int64_t>();
                const int32_t eid = static_cast<int32_t>(r["ext_id"].get<int64_t>());
                if (k == static_cast<int>(FsNodeKind::File)) {
                    ++total_files;
                    total_bytes += sz;
                    ++pit->second.direct_files;
                    ++pit->second.total_files;
                    pit->second.total_bytes += sz;
                    add_ext_count(pit->second.ext_counts, eid, 1);
                } else {
                    ++total_links;
                }
                return true;
            });

    // Purge any orphaned directory or file nodes so fs_nodes, fs_name_fts, fs_dirs & fs_roots are 100% consistent
    for (uint64_t of : orphan_dir_frns) {
        auto o_rows = db.query("SELECT node_id, name FROM fs_nodes WHERE root_id=? AND frn=?",
                               {root_id, static_cast<int64_t>(of)});
        for (const auto& r : o_rows) orphan_nodes.emplace_back(r["node_id"].get<int64_t>(), r["name"].get<std::string>());
    }
    for (const auto& [nid, nm] : orphan_nodes) {
        db.exec("INSERT INTO fs_name_fts(fs_name_fts, rowid, name) VALUES('delete',?,?)", {nid, nm});
        db.exec("DELETE FROM fs_nodes WHERE node_id=?", {nid});
        ++orphans_purged;
    }

    // Bottom-up rollups ordered strictly by depth descending
    std::vector<DirAcc*> ordered;
    ordered.reserve(dirs.size());
    for (auto& [_, d] : dirs) ordered.push_back(&d);
    std::sort(ordered.begin(), ordered.end(), [](const DirAcc* a, const DirAcc* b) { return a->depth > b->depth; });
    for (DirAcc* d : ordered) {
        if (d->frn == root_frn || d->parent_frn == 0) continue;
        auto pit = dirs.find(d->parent_frn);
        if (pit == dirs.end() || pit->first == d->frn) continue;
        pit->second.total_files += d->total_files;
        pit->second.total_dirs += d->total_dirs + 1;
        pit->second.total_bytes += d->total_bytes;
        for (const auto& [eid, cnt] : d->ext_counts) add_ext_count(pit->second.ext_counts, eid, cnt);
    }

    // Persist rebuilt fs_dirs + fs_dir_fts + fs_roots atomically
    sqlite3* raw = db.native_handle();
    db.exec("DELETE FROM fs_dirs WHERE root_id=?", {root_id});
    db.exec("DELETE FROM fs_dir_fts WHERE root_id=?", {root_id});
    RawStmt ins_dir(raw,
        "INSERT INTO fs_dirs(root_id,frn,parent_frn,rel_path,depth,dir_mtime_ns,direct_files,direct_dirs,total_files,total_dirs,total_bytes,top_exts) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?)");
    RawStmt ins_dfts(raw, "INSERT INTO fs_dir_fts(root_id,frn,rel_path) VALUES(?,?,?)");

    for (DirAcc* d : ordered) {
        std::sort(d->ext_counts.begin(), d->ext_counts.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        Json top = Json::object();
        for (size_t k = 0; k < std::min<size_t>(8, d->ext_counts.size()); ++k) {
            size_t eid = static_cast<size_t>(d->ext_counts[k].first);
            if (eid > 0 && eid < ext_names.size()) top[ext_names[eid]] = d->ext_counts[k].second;
        }
        std::string top_s = top.dump();
        ins_dir.reset();
        sqlite3_bind_int64(ins_dir.s, 1, root_id);
        sqlite3_bind_int64(ins_dir.s, 2, static_cast<int64_t>(d->frn));
        sqlite3_bind_int64(ins_dir.s, 3, static_cast<int64_t>(d->parent_frn));
        sqlite3_bind_text(ins_dir.s, 4, d->rel_path.c_str(), static_cast<int>(d->rel_path.size()), SQLITE_STATIC);
        sqlite3_bind_int(ins_dir.s, 5, d->depth);
        sqlite3_bind_int64(ins_dir.s, 6, d->mtime_ns);
        sqlite3_bind_int64(ins_dir.s, 7, d->direct_files);
        sqlite3_bind_int64(ins_dir.s, 8, d->direct_dirs);
        sqlite3_bind_int64(ins_dir.s, 9, d->total_files);
        sqlite3_bind_int64(ins_dir.s, 10, d->total_dirs);
        sqlite3_bind_int64(ins_dir.s, 11, d->total_bytes);
        sqlite3_bind_text(ins_dir.s, 12, top_s.c_str(), static_cast<int>(top_s.size()), SQLITE_TRANSIENT);
        if (sqlite3_step(ins_dir.s) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(raw));

        if (!d->rel_path.empty()) {
            ins_dfts.reset();
            sqlite3_bind_int64(ins_dfts.s, 1, root_id);
            sqlite3_bind_int64(ins_dfts.s, 2, static_cast<int64_t>(d->frn));
            sqlite3_bind_text(ins_dfts.s, 3, d->rel_path.c_str(), static_cast<int>(d->rel_path.size()), SQLITE_STATIC);
            if (sqlite3_step(ins_dfts.s) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(raw));
        }
    }

    const int64_t total_dirs = static_cast<int64_t>(dirs.size()) > 0 ? static_cast<int64_t>(dirs.size() - 1) : 0;
    db.exec("UPDATE fs_roots SET total_files=?, total_dirs=?, total_links=?, total_bytes=?, last_sync_ns=? WHERE root_id=?",
            {total_files, total_dirs, total_links, total_bytes, now_ns(), root_id});
    tx.commit();

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    return {
        {"rebuilt", true},
        {"total_files", total_files},
        {"total_dirs", total_dirs},
        {"total_links", total_links},
        {"total_bytes", total_bytes},
        {"orphans_purged", orphans_purged},
        {"elapsed_ms", ms}
    };
}

bool fs_sync_single_path(const Project& p, Database& db, const fs::path& abs_path, bool log_change) {
    fs_schema(db);
    if (!p.config.contains("roots") || !p.config["roots"].is_array()) return false;
    for (const auto& rcfg : p.config["roots"]) {
        auto rbase = p.resolve(rcfg.at("path").get<std::string>());
        if (!within(abs_path, rbase)) continue;
        int64_t root_id = fs_ensure_root(p, db, rcfg);
        auto rel_p = abs_path.lexically_relative(rbase);
        std::string rel = normalize_rel_path(path_text(rel_p));
        if (rel.empty() || rel == ".") return false;

        uint64_t parent_frn = 0;
        int64_t size_bytes = 0, mtime_ns = 0;
        uint32_t attrs = 0;
        DWORD reparse_tag = 0;
        uint64_t frn = query_path_frn(abs_path, &parent_frn, &size_bytes, &mtime_ns, &attrs, &reparse_tag);
        if (frn != 0) {
            // Ensure ancestor directories exist in fs_dirs
            std::vector<fs::path> chain;
            for (auto cur = abs_path.parent_path(); within(cur, rbase) && cur != rbase; cur = cur.parent_path()) {
                chain.push_back(cur);
            }
            std::reverse(chain.begin(), chain.end());
            for (const auto& d : chain) {
                uint64_t d_pfrn = 0;
                int64_t d_sz = 0, d_mt = 0;
                uint32_t d_at = 0;
                uint64_t d_frn = query_path_frn(d, &d_pfrn, &d_sz, &d_mt, &d_at);
                if (d_frn != 0 && db.integer("SELECT EXISTS(SELECT 1 FROM fs_dirs WHERE root_id=? AND frn=?)",
                                             {root_id, static_cast<int64_t>(d_frn)}) == 0) {
                    fs_apply_upsert(db, root_id, d_frn, d_pfrn, utf8(d.filename().wstring()),
                                    static_cast<int>(FsNodeKind::Directory), 0, d_mt, d_at, "", false);
                }
            }
            const FsNodeKind kenum = classify_win32_node(attrs, reparse_tag);
            const int kind = static_cast<int>(kenum);
            if (kenum == FsNodeKind::Directory) size_bytes = 0;
            fs_apply_upsert(db, root_id, frn, parent_frn, utf8(abs_path.filename().wstring()),
                            kind, size_bytes, mtime_ns, attrs, "", log_change);
            return true;
        } else {
            // Path no longer exists on disk -> look up by (parent_rel, name) and delete
            auto slash = rel.rfind('/');
            std::string p_rel = (slash == std::string::npos) ? "" : rel.substr(0, slash);
            std::string bname = (slash == std::string::npos) ? rel : rel.substr(slash + 1);
            auto drow = db.query("SELECT frn FROM fs_dirs WHERE root_id=? AND rel_path=?", {root_id, p_rel});
            if (!drow.empty()) {
                auto nrow = db.query("SELECT frn FROM fs_nodes WHERE root_id=? AND parent_frn=? AND name=? AND (flags & 1)=0 LIMIT 1",
                                     {root_id, drow[0]["frn"], bname});
                if (!nrow.empty()) {
                    return fs_apply_delete(db, root_id, static_cast<uint64_t>(nrow[0]["frn"].get<int64_t>()), log_change);
                }
            }
            return false;
        }
    }
    return false;
}

Json fs_stats_summary(const Project& p, Database& db) {
    fs_schema(db);
    auto roots = db.query("SELECT root_id,root_key,abs_path,role,mode,engine,complete,last_sync_ns,total_files,total_dirs,total_links,total_bytes,scan_errors,usn_journal_id,next_usn FROM fs_roots ORDER BY root_id");
    int64_t files = 0, dirs = 0, links = 0, bytes = 0, errs = 0;
    bool all_complete = !roots.empty();
    for (const auto& r : roots) {
        files += r["total_files"].get<int64_t>();
        dirs += r["total_dirs"].get<int64_t>();
        links += r["total_links"].get<int64_t>();
        bytes += r["total_bytes"].get<int64_t>();
        errs += r["scan_errors"].get<int64_t>();
        if (r["complete"].get<int64_t>() == 0) all_complete = false;
    }
    std::error_code ec;
    int64_t db_bytes = 0;
    auto db_path = p.state_dir() / L"catalog.sqlite";
    if (fs::exists(db_path, ec)) db_bytes = static_cast<int64_t>(fs::file_size(db_path, ec));
    return {
        {"schema", "migratool.fs_stats/1"},
        {"mode", project_mode(p)},
        {"complete", all_complete},
        {"total_files", files},
        {"total_dirs", dirs},
        {"total_links", links},
        {"total_entries", files + dirs + links},
        {"total_bytes", bytes},
        {"scan_errors", errs},
        {"catalog_bytes", db_bytes},
        {"fts_tokenizer", db.query("SELECT value FROM meta WHERE key='fs_fts_tokenizer'").empty() ? "unknown" :
                          db.query("SELECT value FROM meta WHERE key='fs_fts_tokenizer'")[0]["value"]},
        {"roots", roots}
    };
}

Json fs_explore_tree(const Project& p, Database& db, const Json& args) {
    fs_schema(db);
    auto t0 = std::chrono::steady_clock::now();
    const std::string req_root = args.value("root", "");
    const std::string req_path = normalize_rel_path(args.value("path", args.value("path_prefix", "")));
    const int max_depth = std::clamp(args.value("depth", 2), 1, 4);
    const int limit = std::clamp(args.value("limit", 50), 1, 500);
    const std::string sort_by = to_lower_ascii(args.value("sort", "bytes"));
    const bool include_samples = args.value("include_samples", true);
    const int sample_limit = std::clamp(args.value("sample_limit", 5), 0, 50);
    const bool flat = args.value("flat", false) ||
                      args.value("format", "") == "columnar" ||
                      args.value("format", "") == "compact_table";
    const int ext_limit = std::clamp(args.value("extensions_limit", 8), 0, 24);

    std::string order_sql = "total_bytes DESC, total_files DESC";
    if (sort_by == "files") order_sql = "total_files DESC, total_bytes DESC";
    else if (sort_by == "dirs") order_sql = "total_dirs DESC, total_files DESC";
    else if (sort_by == "name") order_sql = "rel_path ASC";
    else if (sort_by == "mtime") order_sql = "dir_mtime_ns DESC";

    Json roots = req_root.empty()
        ? db.query("SELECT root_id,root_key,abs_path,role,mode,total_files,total_dirs,total_links,total_bytes,complete FROM fs_roots ORDER BY root_id")
        : db.query("SELECT root_id,root_key,abs_path,role,mode,total_files,total_dirs,total_links,total_bytes,complete FROM fs_roots WHERE root_key=?", {req_root});

    if (roots.empty()) {
        return {
            {"schema", "migratool.explore_tree/1"},
            {"indexed", false},
            {"hint", "Ejecute inventory_sync (o activate_workspace) para indexar las raíces en el motor ultra-escala."},
            {"items", Json::array()}
        };
    }

    std::function<Json(int64_t, const std::string&, const Json&, int)> expand_dir =
        [&](int64_t root_id, const std::string& root_key, const Json& drow, int cur_depth) -> Json {
            const int64_t frn = drow["frn"].get<int64_t>();
            const std::string rel = drow["rel_path"].get<std::string>();
            Json node = {
                {"root", root_key},
                {"path", rel.empty() ? "." : rel},
                {"depth", drow["depth"]},
                {"direct_files", drow["direct_files"]},
                {"direct_dirs", drow["direct_dirs"]},
                {"total_files", drow["total_files"]},
                {"total_dirs", drow["total_dirs"]},
                {"total_bytes", drow["total_bytes"]}
            };
            if (ext_limit > 0) {
                auto parsed_ext = Json::parse(drow["top_exts"].get<std::string>(), nullptr, false);
                if (parsed_ext.is_object()) {
                    if (static_cast<int>(parsed_ext.size()) > ext_limit) {
                        Json trimmed = Json::object();
                        int k = 0;
                        for (auto it = parsed_ext.begin(); it != parsed_ext.end() && k < ext_limit; ++it, ++k)
                            trimmed[it.key()] = it.value();
                        node["top_extensions"] = std::move(trimmed);
                    } else {
                        node["top_extensions"] = std::move(parsed_ext);
                    }
                }
            }
            if (cur_depth < max_depth) {
                auto children = db.query(
                    "SELECT frn,rel_path,depth,dir_mtime_ns,direct_files,direct_dirs,total_files,total_dirs,total_bytes,top_exts "
                    "FROM fs_dirs WHERE root_id=? AND parent_frn=? AND frn<>? ORDER BY " + order_sql + " LIMIT ?",
                    {root_id, frn, frn, limit});
                Json sub = Json::array();
                for (const auto& ch : children) {
                    sub.push_back(expand_dir(root_id, root_key, ch, cur_depth + 1));
                }
                node["subdirs"] = std::move(sub);
            }
            if (include_samples && sample_limit > 0 && cur_depth <= 1) {
                auto files = db.query(
                    "SELECT name, kind, size_bytes, mtime_ns FROM fs_nodes INDEXED BY idx_fs_nodes_parent "
                    "WHERE root_id=? AND parent_frn=? AND kind=0 AND (flags & 1)=0 ORDER BY name ASC LIMIT ?",
                    {root_id, frn, sample_limit});
                Json smps = Json::array();
                for (const auto& f : files) {
                    smps.push_back({{"name", f["name"]}, {"kind", kind_to_string(f["kind"].get<int>())}, {"bytes", f["size_bytes"]}});
                }
                if (!smps.empty()) node["sample_files"] = std::move(smps);
            }
            return node;
        };

    Json items = Json::array();
    int64_t grand_files = 0, grand_dirs = 0, grand_bytes = 0;
    for (const auto& r : roots) {
        int64_t rid = r["root_id"].get<int64_t>();
        std::string rkey = r["root_key"].get<std::string>();
        auto target = db.query(
            "SELECT frn,rel_path,depth,dir_mtime_ns,direct_files,direct_dirs,total_files,total_dirs,total_bytes,top_exts "
            "FROM fs_dirs WHERE root_id=? AND rel_path=?",
            {rid, req_path});
        if (target.empty()) continue;
        grand_files += target[0]["total_files"].get<int64_t>();
        grand_dirs += target[0]["total_dirs"].get<int64_t>();
        grand_bytes += target[0]["total_bytes"].get<int64_t>();
        if (flat) {
            const int base_depth = target[0]["depth"].get<int>();
            const int hi_depth = base_depth + max_depth;
            Json flat_rows;
            if (req_path.empty()) {
                flat_rows = db.query(
                    "SELECT rel_path,depth,direct_files,direct_dirs,total_files,total_dirs,total_bytes "
                    "FROM fs_dirs WHERE root_id=? AND depth>=? AND depth<=? ORDER BY " + order_sql + " LIMIT ?",
                    {rid, base_depth, hi_depth, limit});
            } else {
                flat_rows = db.query(
                    "SELECT rel_path,depth,direct_files,direct_dirs,total_files,total_dirs,total_bytes "
                    "FROM fs_dirs WHERE root_id=? AND (rel_path=? OR (rel_path>=? AND rel_path<?)) AND depth<=? "
                    "ORDER BY " + order_sql + " LIMIT ?",
                    {rid, req_path, req_path + "/", req_path + "0", hi_depth, limit});
            }
            for (const auto& fr : flat_rows) {
                std::string rel = fr["rel_path"].get<std::string>();
                items.push_back({
                    {"root", rkey},
                    {"path", rel.empty() ? "." : rel},
                    {"depth", fr["depth"]},
                    {"direct_files", fr["direct_files"]},
                    {"direct_dirs", fr["direct_dirs"]},
                    {"total_files", fr["total_files"]},
                    {"total_dirs", fr["total_dirs"]},
                    {"total_bytes", fr["total_bytes"]}
                });
            }
        } else {
            items.push_back(expand_dir(rid, rkey, target[0], 0));
        }
    }

    const size_t returned_cnt = items.size();
    auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
    return {
        {"schema", "migratool.explore_tree/1"},
        {"indexed", true},
        {"mode", project_mode(p)},
        {"path", req_path.empty() ? "." : req_path},
        {"depth", max_depth},
        {"flat", flat},
        {"returned", returned_cnt},
        {"summary", {{"total_files", grand_files}, {"total_dirs", grand_dirs}, {"total_bytes", grand_bytes}}},
        {"elapsed_us", elapsed_us},
        {"items", std::move(items)}
    };
}

Json fs_find_files(const Project& p, Database& db, const Json& args) {
    (void)p;
    fs_schema(db);
    auto t0 = std::chrono::steady_clock::now();

    const std::string query_text = args.value("query", args.value("text", ""));
    const std::string glob_pat = args.value("glob", "");
    const std::string path_prefix = normalize_rel_path(args.value("path_prefix", ""));
    const std::string req_root = args.value("root", "");
    const std::string kind_filter = to_lower_ascii(args.value("kind", "all"));
    const int64_t min_bytes = args.value("min_bytes", int64_t(-1));
    const int64_t max_bytes = args.value("max_bytes", int64_t(-1));
    const int64_t mod_after = args.value("modified_after_ns", int64_t(0));
    const int64_t mod_before = args.value("modified_before_ns", int64_t(0));
    const int limit = std::clamp(args.value("limit", 100), 1, 1000);
    int64_t after_node_id = 0;
    if (args.contains("cursor") && args["cursor"].is_string() && !args["cursor"].get<std::string>().empty()) {
        try { after_node_id = std::stoll(args["cursor"].get<std::string>()); } catch (...) {}
    }

    // Collect extension filter(s)
    std::vector<std::string> ext_list;
    if (args.contains("ext")) {
        if (args["ext"].is_string() && !args["ext"].get<std::string>().empty()) ext_list.push_back(args["ext"].get<std::string>());
        else if (args["ext"].is_array()) {
            for (const auto& e : args["ext"]) if (e.is_string() && !e.get<std::string>().empty()) ext_list.push_back(e.get<std::string>());
        }
    }
    if (args.contains("extension") && args["extension"].is_string() && !args["extension"].get<std::string>().empty()) {
        ext_list.push_back(args["extension"].get<std::string>());
    }
    // Extract pure extension shortcut from glob like "*.dll" or "**/*.py"
    if (ext_list.empty() && !glob_pat.empty()) {
        auto g = glob_pat;
        if (g.starts_with("**/")) g = g.substr(3);
        if (g.starts_with("*.") && g.find('*', 1) == std::string::npos && g.find('?', 1) == std::string::npos && g.find('/', 1) == std::string::npos) {
            ext_list.push_back(g.substr(1));
        }
    }
    std::vector<int64_t> ext_ids;
    for (auto e : ext_list) {
        if (!e.empty() && e.front() != '.') e.insert(e.begin(), '.');
        auto rows = db.query("SELECT ext_id FROM fs_extensions WHERE ext=?", {to_lower_ascii(e)});
        if (!rows.empty()) ext_ids.push_back(rows[0]["ext_id"].get<int64_t>());
    }
    if (!ext_list.empty() && ext_ids.empty()) {
        // Requested extension does not exist in catalog at all -> instant 0-row return!
        return {
            {"schema", "migratool.find_files/1"},
            {"items", Json::array()},
            {"returned", 0},
            {"has_more", false},
            {"next_cursor", nullptr},
            {"elapsed_us", std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count()}
        };
    }

    // Build root lookup map
    std::unordered_map<int64_t, std::pair<std::string, std::string>> root_info;
    int64_t filter_root_id = -1;
    for (const auto& r : db.query("SELECT root_id, root_key, abs_path FROM fs_roots")) {
        int64_t rid = r["root_id"].get<int64_t>();
        std::string rkey = r["root_key"].get<std::string>();
        root_info[rid] = {rkey, r["abs_path"].get<std::string>()};
        if (!req_root.empty() && rkey == req_root) filter_root_id = rid;
    }
    if (!req_root.empty() && filter_root_id < 0) {
        return {{"schema", "migratool.find_files/1"}, {"items", Json::array()}, {"returned", 0}, {"has_more", false}, {"next_cursor", nullptr}};
    }

    // Fast path: searching ONLY directories
    const std::string tok_row = db.query("SELECT value FROM meta WHERE key='fs_fts_tokenizer'").empty()
        ? "unicode61" : db.query("SELECT value FROM meta WHERE key='fs_fts_tokenizer'")[0]["value"].get<std::string>();
    const bool has_trigram = (tok_row == "trigram");

    if (kind_filter == "dir" || kind_filter == "directory") {
        std::string sql = "SELECT d.root_id, d.frn, d.rel_path, d.depth, d.dir_mtime_ns, d.total_files, d.total_dirs, d.total_bytes FROM fs_dirs d";
        std::string where = " WHERE d.rel_path<>''";
        Json params = Json::array();
        if (!query_text.empty() && query_text.size() >= 3 && has_trigram) {
            sql += " JOIN fs_dir_fts f ON f.root_id=d.root_id AND f.frn=d.frn";
            where += " AND fs_dir_fts MATCH ?";
            params.push_back(fts5_escape_phrase(query_text));
        } else if (!query_text.empty()) {
            where += " AND instr(lower(d.rel_path), lower(?))>0";
            params.push_back(query_text);
        }
        if (filter_root_id >= 0) { where += " AND d.root_id=?"; params.push_back(filter_root_id); }
        if (!path_prefix.empty()) {
            where += " AND (d.rel_path=? OR (d.rel_path>=? AND d.rel_path<?))";
            params.push_back(path_prefix);
            params.push_back(path_prefix + "/");
            params.push_back(path_prefix + "0");
        }
        int64_t offset = after_node_id;
        params.push_back(limit + 1);
        params.push_back(offset);
        auto rows = db.query(sql + where + " ORDER BY d.total_bytes DESC LIMIT ? OFFSET ?", params);
        const bool more = static_cast<int>(rows.size()) > limit;
        if (more) rows.erase(rows.end() - 1);
        Json items = Json::array();
        for (const auto& row : rows) {
            const auto& [rkey, rabs] = root_info[row["root_id"].get<int64_t>()];
            std::string rel = row["rel_path"].get<std::string>();
            if (!glob_pat.empty() && !fs_glob_match(glob_pat, rel)) continue;
            items.push_back({
                {"root", rkey},
                {"path", rel},
                {"absolute_path", rabs + "/" + rel},
                {"kind", "dir"},
                {"total_files", row["total_files"]},
                {"total_dirs", row["total_dirs"]},
                {"bytes", row["total_bytes"]},
                {"mtime_ns", row["dir_mtime_ns"]},
                {"frn", row["frn"]}
            });
        }
        const size_t dir_returned = items.size();
        return {
            {"schema", "migratool.find_files/1"},
            {"strategy", (has_trigram && query_text.size() >= 3) ? "fs_dir_fts_trigram" : "fs_dirs_btree"},
            {"items", std::move(items)},
            {"returned", dir_returned},
            {"has_more", more},
            {"next_cursor", more ? Json(std::to_string(offset + limit)) : Json(nullptr)},
            {"elapsed_us", std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count()}
        };
    }

    // Main file/entry search planner
    std::string strategy = "scan";
    std::string sql =
        "SELECT n.node_id, n.root_id, n.frn, n.parent_frn, n.name, n.kind, n.size_bytes, n.mtime_ns, n.link_target, d.rel_path AS parent_rel "
        "FROM fs_nodes n ";
    std::string where = " WHERE (n.flags & 1)=0";
    Json params = Json::array();

    // Extract longest literal token (>=3 chars) from glob if query_text is empty
    std::string fts_needle = query_text;
    if (fts_needle.empty() && !glob_pat.empty() && has_trigram && ext_ids.empty()) {
        std::string best, cur;
        for (char c : glob_pat) {
            if (c == '*' || c == '?' || c == '/' || c == '\\' || c == '[' || c == ']') {
                if (cur.size() > best.size()) best = cur;
                cur.clear();
            } else {
                cur.push_back(c);
            }
        }
        if (cur.size() > best.size()) best = cur;
        if (best.size() >= 3) fts_needle = best;
    }

    // If query contains '/', split into dir hint + name hint!
    std::string name_needle = fts_needle;
    std::string dir_needle;
    if (auto sl = fts_needle.rfind('/'); sl != std::string::npos) {
        dir_needle = fts_needle.substr(0, sl);
        name_needle = fts_needle.substr(sl + 1);
    }

    bool used_name_fts = false;
    if (has_trigram && name_needle.size() >= 3) {
        sql += "JOIN fs_name_fts f ON f.rowid=n.node_id ";
        sql += "LEFT JOIN fs_dirs d ON d.root_id=n.root_id AND d.frn=n.parent_frn ";
        where += " AND fs_name_fts MATCH ?";
        params.push_back(fts5_escape_phrase(name_needle));
        strategy = "fs_name_fts_trigram";
        used_name_fts = true;
    } else if (!path_prefix.empty() && ext_ids.empty()) {
        sql += "JOIN fs_dirs d ON d.root_id=n.root_id AND d.frn=n.parent_frn ";
        where += " AND (d.rel_path=? OR (d.rel_path>=? AND d.rel_path<?))";
        params.push_back(path_prefix);
        params.push_back(path_prefix + "/");
        params.push_back(path_prefix + "0");
        strategy = "fs_dirs_prefix_join";
    } else {
        sql += "LEFT JOIN fs_dirs d ON d.root_id=n.root_id AND d.frn=n.parent_frn ";
        if (!ext_ids.empty()) strategy = "idx_fs_nodes_ext";
        else if (mod_after > 0 || mod_before > 0) strategy = "idx_fs_nodes_mtime";
    }

    if (filter_root_id >= 0) {
        where += " AND n.root_id=?";
        params.push_back(filter_root_id);
    }
    if (!ext_ids.empty()) {
        where += " AND n.ext_id IN (";
        for (size_t i = 0; i < ext_ids.size(); ++i) {
            if (i) where += ',';
            where += '?';
            params.push_back(ext_ids[i]);
        }
        where += ")";
    }
    if (!path_prefix.empty() && used_name_fts) {
        where += " AND (d.rel_path=? OR (d.rel_path>=? AND d.rel_path<?))";
        params.push_back(path_prefix);
        params.push_back(path_prefix + "/");
        params.push_back(path_prefix + "0");
    }
    if (!dir_needle.empty()) {
        where += " AND instr(lower(COALESCE(d.rel_path,'')), lower(?))>0";
        params.push_back(dir_needle);
    }
    if (!query_text.empty() && !used_name_fts && dir_needle.empty()) {
        where += " AND instr(lower(n.name), lower(?))>0";
        params.push_back(query_text);
    }
    if (kind_filter == "file") where += " AND n.kind=0";
    else if (kind_filter == "link" || kind_filter == "symlink") where += " AND n.kind IN(2,3,4)";
    if (min_bytes >= 0) { where += " AND n.size_bytes>=?"; params.push_back(min_bytes); }
    if (max_bytes >= 0) { where += " AND n.size_bytes<=?"; params.push_back(max_bytes); }
    if (mod_after > 0) { where += " AND n.mtime_ns>=?"; params.push_back(mod_after); }
    if (mod_before > 0) { where += " AND n.mtime_ns<=?"; params.push_back(mod_before); }
    if (after_node_id > 0) { where += " AND n.node_id>?"; params.push_back(after_node_id); }

    // Fetch slightly more if post-filtering by glob, bounded cleanly
    const int fetch_limit = glob_pat.empty() ? (limit + 1) : std::min(limit * 8 + 16, 4000);
    params.push_back(fetch_limit);

    const bool include_abs = args.value("include_absolute_path", false) || args.value("detail", "") == "full";
    Json roots_map = Json::object();
    for (const auto& [rid_k, info_v] : root_info) roots_map[info_v.first] = info_v.second;

    const std::string order_clause = (strategy == "idx_fs_nodes_mtime")
        ? " ORDER BY n.mtime_ns DESC, n.node_id ASC LIMIT ?"
        : (used_name_fts ? " LIMIT ?" : " ORDER BY n.node_id ASC LIMIT ?");
    Json items = Json::array();
    int64_t last_seen_id = after_node_id;
    bool has_more = false;

    db.each(sql + where + order_clause, params, [&](const Json& row) {
        const int64_t nid = row["node_id"].get<int64_t>();
        const int64_t rid = row["root_id"].get<int64_t>();
        const auto& [rkey, rabs] = root_info[rid];
        const std::string prel = row["parent_rel"].is_null() ? std::string() : row["parent_rel"].get<std::string>();
        const std::string name = row["name"].get<std::string>();
        const std::string rel = join_rel(prel, name);
        if (!glob_pat.empty()) {
            const bool match_target = (glob_pat.find('/') != std::string::npos || glob_pat.find('\\') != std::string::npos)
                ? fs_glob_match(glob_pat, rel)
                : fs_glob_match(glob_pat, name);
            if (!match_target) return true;
        }
        if (static_cast<int>(items.size()) >= limit) {
            has_more = true;
            return false;
        }
        last_seen_id = nid;
        Json item = {
            {"root", rkey},
            {"path", rel},
            {"kind", kind_to_string(row["kind"].get<int>())},
            {"bytes", row["size_bytes"]},
            {"mtime_ns", row["mtime_ns"]},
            {"frn", row["frn"]}
        };
        if (include_abs) item["absolute_path"] = rabs + "/" + rel;
        if (row["link_target"].is_string() && !row["link_target"].get<std::string>().empty()) {
            item["link_target"] = row["link_target"];
        }
        items.push_back(std::move(item));
        return true;
    });

    const size_t returned_cnt = items.size();
    auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
    return {
        {"schema", "migratool.find_files/1"},
        {"strategy", strategy},
        {"roots", std::move(roots_map)},
        {"items", std::move(items)},
        {"returned", returned_cnt},
        {"has_more", has_more},
        {"next_cursor", has_more ? Json(std::to_string(last_seen_id)) : Json(nullptr)},
        {"elapsed_us", elapsed_us}
    };
}

Json fs_changes_feed(const Project& p, Database& db, const Json& args) {
    fs_schema(db);
    (void)p;
    const int64_t since_seq = args.value("since_seq", int64_t(0));
    const int64_t since_ns = args.value("since_ns", int64_t(0));
    const std::string req_root = args.value("root", "");
    const std::string path_prefix = normalize_rel_path(args.value("path_prefix", ""));
    const int limit = std::clamp(args.value("limit", 100), 1, 1000);
    const bool descending = args.value("order", std::string("asc")) == "desc";

    std::string sql =
        "SELECT c.seq, c.at_ns, r.root_key AS root, c.frn, c.op, c.kind, c.old_path, c.new_path, c.size_bytes "
        "FROM fs_change_log c JOIN fs_roots r ON r.root_id=c.root_id WHERE c.seq>?";
    Json params = {since_seq};
    if (since_ns > 0) { sql += " AND c.at_ns>=?"; params.push_back(since_ns); }
    if (!req_root.empty()) { sql += " AND r.root_key=?"; params.push_back(req_root); }
    if (args.contains("ops") && args["ops"].is_array() && !args["ops"].empty()) {
        sql += " AND c.op IN (";
        for (size_t i = 0; i < args["ops"].size(); ++i) {
            if (i) sql += ',';
            sql += '?';
            params.push_back(args["ops"][i]);
        }
        sql += ")";
    }
    if (!path_prefix.empty()) {
        sql += " AND ((c.new_path=? OR (c.new_path>=? AND c.new_path<?)) OR (c.old_path=? OR (c.old_path>=? AND c.old_path<?)))";
        params.push_back(path_prefix); params.push_back(path_prefix + "/"); params.push_back(path_prefix + "0");
        params.push_back(path_prefix); params.push_back(path_prefix + "/"); params.push_back(path_prefix + "0");
    }
    sql += descending ? " ORDER BY c.seq DESC LIMIT ?" : " ORDER BY c.seq ASC LIMIT ?";
    params.push_back(limit + 1);

    auto rows = db.query(sql, params);
    const bool more = static_cast<int>(rows.size()) > limit;
    if (more) rows.erase(rows.end() - 1);
    for (auto& r : rows) r["kind"] = kind_to_string(r["kind"].get<int>());
    const int64_t latest_seq = db.integer("SELECT COALESCE(MAX(seq),0) FROM fs_change_log");
    return {
        {"schema", "migratool.changes_feed/1"},
        {"latest_seq", latest_seq},
        {"returned", rows.size()},
        {"has_more", more},
        {"next_since_seq", (!rows.empty() && !descending) ? rows.back()["seq"] : Json(latest_seq)},
        {"items", std::move(rows)}
    };
}

} // namespace mt
