#include "migratool/gateway.hpp"
#include "migratool/scale.hpp"
#include <stdexcept>
#include <algorithm>
#include <cctype>
#include <mutex>
#include <unordered_map>

namespace mt {
namespace {
constexpr int write_timeout_ms = 60000;

struct CachedDbMeta {
    std::wstring mutex_name;
    uint64_t creation_ft = 0;
};
std::mutex db_meta_mu;
std::unordered_map<std::wstring, CachedDbMeta> db_meta_cache;

std::string low_db_path(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

struct SqlError : std::runtime_error {
    int code;
    int extended_code;
    SqlError(int rc, sqlite3* db, const std::string& extra = "")
        : std::runtime_error(std::string(db ? sqlite3_errmsg(db) : "SQLite error") +
                             " [sqlite_rc=" + std::to_string(rc) +
                             ", ext_rc=" + std::to_string(db ? sqlite3_extended_errcode(db) : rc) +
                             (extra.empty() ? "" : (", " + extra)) + "]"),
          code(rc),
          extended_code(db ? sqlite3_extended_errcode(db) : rc) {}
};

bool is_busy_or_locked(int rc) {
    const int primary = rc & 0xFF;
    return primary == SQLITE_BUSY || primary == SQLITE_LOCKED;
}

sqlite3_stmt* prepare(sqlite3* db, const std::string& sql, const Json& params) {
    const auto values = params.is_array() ? params : Json::array({params});
    sqlite3_stmt* stmt{};
    if (auto rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr); rc != SQLITE_OK)
        throw SqlError(rc, db);
    if (static_cast<size_t>(sqlite3_bind_parameter_count(stmt)) != values.size()) {
        sqlite3_finalize(stmt);
        throw std::runtime_error("Número de parámetros SQL inválido");
    }
    size_t total_bound_bytes = 0;
    for (size_t i = 0; i < values.size(); ++i) {
        const auto& p = values[i];
        int rc = SQLITE_OK;
        size_t param_bytes = 0;
        if (p.is_null()) {
            rc = sqlite3_bind_null(stmt, static_cast<int>(i) + 1);
        } else if (p.is_number_integer() || p.is_number_unsigned() || p.is_boolean()) {
            rc = sqlite3_bind_int64(stmt, static_cast<int>(i) + 1,
                                    p.is_boolean() ? static_cast<int>(p.get<bool>()) : p.get<int64_t>());
        } else if (p.is_number_float()) {
            rc = sqlite3_bind_double(stmt, static_cast<int>(i) + 1, p.get<double>());
        } else {
            auto value = p.is_string() ? p.get<std::string>() : p.dump();
            param_bytes = value.size();
            total_bound_bytes += param_bytes;
            if (param_bytes > static_cast<size_t>(sqlite3_limit(db, SQLITE_LIMIT_LENGTH, -1))) {
                sqlite3_finalize(stmt);
                throw SqlError(SQLITE_TOOBIG, db,
                               "param_index=" + std::to_string(i + 1) +
                               ", param_bytes=" + std::to_string(param_bytes) +
                               ", total_bound_bytes=" + std::to_string(total_bound_bytes));
            }
            rc = sqlite3_bind_text(stmt, static_cast<int>(i) + 1, value.c_str(),
                                   static_cast<int>(value.size()), SQLITE_TRANSIENT);
        }
        if (rc != SQLITE_OK) {
            sqlite3_finalize(stmt);
            throw SqlError(rc, db,
                           "param_index=" + std::to_string(i + 1) +
                           ", param_bytes=" + std::to_string(param_bytes) +
                           ", total_bound_bytes=" + std::to_string(total_bound_bytes));
        }
    }
    return stmt;
}
} // namespace

void Database::acquire_write_lock() {
    if (write_mutex_) return;
    HANDLE h = CreateMutexW(nullptr, FALSE, mutex_name_.c_str());
    if (!h) throw std::runtime_error("No se pudo crear mutex coordinador de escritura SQLite");
    DWORD wait_rc = WaitForSingleObject(h, static_cast<DWORD>(write_timeout_ms));
    if (wait_rc != WAIT_OBJECT_0 && wait_rc != WAIT_ABANDONED) {
        CloseHandle(h);
        throw std::runtime_error("SQLITE_WRITE_COORDINATOR_TIMEOUT: cola de escritura ocupada tras " +
                                 std::to_string(write_timeout_ms) + " ms");
    }
    write_mutex_ = h;
}

void Database::release_write_lock() noexcept {
    if (write_mutex_) {
        ReleaseMutex(write_mutex_);
        CloseHandle(write_mutex_);
        write_mutex_ = nullptr;
    }
}

Database::Database(const Project& p) {
    auto file = p.state_dir() / L"catalog.sqlite";
    const auto raw_key = file.lexically_normal().wstring();
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    const bool file_existed = GetFileAttributesExW(native_path(file).c_str(), GetFileExInfoStandard, &fad) != FALSE &&
                              (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
                              (fad.nFileSizeHigh != 0 || fad.nFileSizeLow != 0);
    const uint64_t ctime = file_existed ? ((static_cast<uint64_t>(fad.ftCreationTime.dwHighDateTime) << 32) | fad.ftCreationTime.dwLowDateTime) : 0;
    bool schema_cached = false;
    if (file_existed) {
        std::lock_guard lk(db_meta_mu);
        if (auto it = db_meta_cache.find(raw_key); it != db_meta_cache.end() && it->second.creation_ft == ctime) {
            mutex_name_ = it->second.mutex_name;
            schema_cached = true;
        }
    }
    if (mutex_name_.empty()) {
        fs::create_directories(p.state_dir());
        auto canonical_key = low_db_path(path_text(fs::weakly_canonical(file)));
        mutex_name_ = L"Local\\MigratoolWriteCoord_" + wide(sha256(canonical_key).substr(0, 32));
    }
    if (sqlite3_open_v2(path_text(file).c_str(), &db_,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
        auto msg = db_ ? std::string(sqlite3_errmsg(db_)) : "SQLite no disponible";
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error(msg);
    }
    sqlite3_busy_timeout(db_, write_timeout_ms);
    const char* schema = R"sql(
PRAGMA page_size=16384;
PRAGMA auto_vacuum=INCREMENTAL;
PRAGMA journal_mode=WAL;
BEGIN IMMEDIATE;
CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY,value TEXT NOT NULL);
INSERT OR IGNORE INTO meta VALUES('revision','0');
INSERT OR IGNORE INTO meta VALUES('schema','1');
CREATE TABLE IF NOT EXISTS roots(id TEXT PRIMARY KEY,role TEXT,path TEXT,complete INTEGER,data TEXT);
CREATE TABLE IF NOT EXISTS files(id INTEGER PRIMARY KEY AUTOINCREMENT,root TEXT NOT NULL,path TEXT NOT NULL,absolute_path TEXT NOT NULL UNIQUE COLLATE NOCASE,
 winid TEXT,sha256 TEXT,bytes INTEGER,mtime INTEGER,category TEXT,language TEXT,relevant INTEGER,git_state TEXT,build_state,
 analysis TEXT,deleted INTEGER NOT NULL DEFAULT 0,first_revision INTEGER,last_revision INTEGER,data TEXT);
CREATE INDEX IF NOT EXISTS file_identity ON files(winid);
CREATE INDEX IF NOT EXISTS file_root ON files(root,deleted,relevant);
CREATE INDEX IF NOT EXISTS file_hash ON files(sha256);
CREATE TABLE IF NOT EXISTS symbols(id INTEGER PRIMARY KEY AUTOINCREMENT,file_id INTEGER REFERENCES files(id),stable_key TEXT,
 name TEXT,qualified_name TEXT,kind TEXT,line INTEGER,data TEXT,active INTEGER NOT NULL DEFAULT 1,UNIQUE(file_id,stable_key));
CREATE TABLE IF NOT EXISTS dependencies(id INTEGER PRIMARY KEY,file_id INTEGER REFERENCES files(id),target_id INTEGER,kind TEXT,name TEXT,data TEXT);
CREATE TABLE IF NOT EXISTS doc_links(doc_id INTEGER REFERENCES files(id),code_id INTEGER REFERENCES files(id),reason TEXT,PRIMARY KEY(doc_id,code_id));
CREATE TABLE IF NOT EXISTS mappings(source TEXT PRIMARY KEY,targets TEXT NOT NULL,complete INTEGER NOT NULL,hashes TEXT NOT NULL,
 cases TEXT NOT NULL,note TEXT,actor TEXT,at TEXT,revision INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS notes(id INTEGER PRIMARY KEY AUTOINCREMENT,entity TEXT,text TEXT,actor TEXT,at TEXT,revision INTEGER);
CREATE TABLE IF NOT EXISTS history(id INTEGER PRIMARY KEY AUTOINCREMENT,revision INTEGER NOT NULL,at TEXT NOT NULL,actor TEXT NOT NULL,action TEXT NOT NULL,entity TEXT,detail TEXT NOT NULL);
CREATE TRIGGER IF NOT EXISTS history_no_update BEFORE UPDATE ON history BEGIN SELECT RAISE(ABORT,'immutable history'); END;
CREATE TRIGGER IF NOT EXISTS history_no_delete BEFORE DELETE ON history BEGIN SELECT RAISE(ABORT,'immutable history'); END;
CREATE TABLE IF NOT EXISTS evidence(id TEXT PRIMARY KEY,kind TEXT,data TEXT,revision INTEGER,at TEXT);
CREATE VIRTUAL TABLE IF NOT EXISTS search USING fts5(entity_type UNINDEXED,entity_id UNINDEXED,body,tokenize='unicode61 remove_diacritics 2');
CREATE TABLE IF NOT EXISTS requests(id INTEGER PRIMARY KEY AUTOINCREMENT,at TEXT,tool TEXT,request_bytes INTEGER,response_bytes INTEGER,duration_us INTEGER,cache_revision INTEGER);
CREATE TABLE IF NOT EXISTS connections(id TEXT PRIMARY KEY,pid INTEGER,client TEXT,opened TEXT,closed TEXT);
CREATE TABLE IF NOT EXISTS watchers(id TEXT PRIMARY KEY,pid INTEGER,root TEXT,opened TEXT,last_event TEXT,closed TEXT,error TEXT);
CREATE TABLE IF NOT EXISTS task_claims(entity TEXT PRIMARY KEY,actor TEXT NOT NULL,session_id TEXT,claimed_at TEXT NOT NULL,expires_at TEXT NOT NULL,lease_seconds INTEGER NOT NULL,note TEXT,revision INTEGER NOT NULL);
PRAGMA user_version=1;
COMMIT;
)sql";
    try {
        exec("PRAGMA foreign_keys=ON");
        exec("PRAGMA synchronous=NORMAL");
        exec("PRAGMA cache_size=-65536");
        exec("PRAGMA temp_store=MEMORY");
        exec("PRAGMA mmap_size=268435456");
        exec("PRAGMA wal_autocheckpoint=8000");
        exec("PRAGMA journal_size_limit=67108864");
        const auto version = integer("PRAGMA user_version");
        if (version > 6) throw std::runtime_error("Versión de catálogo más nueva que esta aplicación");
        if (schema_cached && version == 6 &&
            integer("SELECT COUNT(*) FROM meta WHERE key='dev_control_schema' AND value='2'") != 0) return;
        if (version == 0) {
            acquire_write_lock();
            struct UnlockInit { Database* self; ~UnlockInit() { self->release_write_lock(); } } unlock{this};
            if (integer("PRAGMA user_version") == 0) {
                auto rc = sqlite3_exec(db_, schema, nullptr, nullptr, nullptr);
                if (rc != SQLITE_OK) throw SqlError(rc, db_);
            }
        }
        if (integer("PRAGMA user_version") < 6) {
            acquire_write_lock();
            if (integer("PRAGMA user_version") < 6) {
                if (version >= 1) {
                    const auto dest = p.state_dir()/wide("schema-v"+std::to_string(version)+"-"+unique_id()+".sqlite");
                    sqlite3* backup_db{};
                    if(sqlite3_open_v2(path_text(dest).c_str(),&backup_db,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE,nullptr)!=SQLITE_OK)
                        throw std::runtime_error("No se pudo crear respaldo de esquema");
                    auto copy=sqlite3_backup_init(backup_db,"main",db_,"main");
                    const int copied=copy?sqlite3_backup_step(copy,-1):SQLITE_ERROR;
                    if(copy)sqlite3_backup_finish(copy);
                    sqlite3_close(backup_db);
                    if(copied!=SQLITE_DONE)throw std::runtime_error("Respaldo SQLite incompleto; esquema conservado");
                }
                scale_schema(*this);
            }
            release_write_lock();
        }
        const bool missing_claims = integer("SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='task_claims'") == 0;
        const bool missing_dev = integer("SELECT COUNT(*) FROM meta WHERE key='dev_control_schema' AND value='2'") == 0 ||
            integer("SELECT COUNT(*) FROM sqlite_master WHERE type='trigger' AND name IN('progress_phase_change','progress_phase_update','progress_task_change','progress_task_update','progress_task_delete','progress_doc_change','progress_doc_update')") < 7;
        if (missing_claims || missing_dev) {
            acquire_write_lock();
            struct UnlockPatch { Database* self; ~UnlockPatch() { self->release_write_lock(); } } unlock_patch{this};
            const char* dev_patch = R"sql(
BEGIN IMMEDIATE;
CREATE TABLE IF NOT EXISTS task_claims(entity TEXT PRIMARY KEY,actor TEXT NOT NULL,session_id TEXT,claimed_at TEXT NOT NULL,expires_at TEXT NOT NULL,lease_seconds INTEGER NOT NULL,note TEXT,revision INTEGER NOT NULL);
CREATE INDEX IF NOT EXISTS task_claims_expires ON task_claims(expires_at);
CREATE INDEX IF NOT EXISTS file_rel_path ON files(path COLLATE NOCASE,deleted);
CREATE TABLE IF NOT EXISTS search_rows(entity_type TEXT NOT NULL,entity_id TEXT NOT NULL,row_id INTEGER NOT NULL,PRIMARY KEY(entity_type,entity_id,row_id)) WITHOUT ROWID;
INSERT OR IGNORE INTO search_rows(entity_type,entity_id,row_id) SELECT entity_type,entity_id,rowid FROM search WHERE entity_type IN('mapping','note');
CREATE TABLE IF NOT EXISTS project_phases(id TEXT PRIMARY KEY,title TEXT NOT NULL,ordinal INTEGER NOT NULL DEFAULT 0,state TEXT NOT NULL DEFAULT 'PLANNED',description TEXT NOT NULL DEFAULT '',gate_builds TEXT NOT NULL DEFAULT '[]',gate_cases TEXT NOT NULL DEFAULT '[]',target_modules TEXT NOT NULL DEFAULT '[]',updated_at TEXT NOT NULL,actor TEXT NOT NULL,revision INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS project_tasks(id TEXT PRIMARY KEY,phase_id TEXT,title TEXT NOT NULL,kind TEXT NOT NULL DEFAULT 'migration',state TEXT NOT NULL DEFAULT 'TODO',priority INTEGER NOT NULL DEFAULT 2,assignee TEXT NOT NULL DEFAULT '',source_entities TEXT NOT NULL DEFAULT '[]',target_entities TEXT NOT NULL DEFAULT '[]',required_cases TEXT NOT NULL DEFAULT '[]',depends_on TEXT NOT NULL DEFAULT '[]',note TEXT NOT NULL DEFAULT '',created_at TEXT NOT NULL,updated_at TEXT NOT NULL,actor TEXT NOT NULL,revision INTEGER NOT NULL);
CREATE INDEX IF NOT EXISTS project_tasks_phase ON project_tasks(phase_id,state,priority);
CREATE TABLE IF NOT EXISTS doc_audits(doc_id TEXT PRIMARY KEY,sha256 TEXT NOT NULL,state TEXT NOT NULL DEFAULT 'UNREVIEWED',disposition TEXT NOT NULL DEFAULT 'reference_only',linked_targets TEXT NOT NULL DEFAULT '[]',extracted_tasks TEXT NOT NULL DEFAULT '[]',note TEXT NOT NULL DEFAULT '',actor TEXT NOT NULL,at TEXT NOT NULL,revision INTEGER NOT NULL);
CREATE TRIGGER IF NOT EXISTS progress_phase_change AFTER INSERT ON project_phases BEGIN UPDATE meta SET value=CAST(value AS INTEGER)+1 WHERE key='progress_epoch'; END;
CREATE TRIGGER IF NOT EXISTS progress_phase_update AFTER UPDATE ON project_phases BEGIN UPDATE meta SET value=CAST(value AS INTEGER)+1 WHERE key='progress_epoch'; END;
CREATE TRIGGER IF NOT EXISTS progress_task_change AFTER INSERT ON project_tasks BEGIN UPDATE meta SET value=CAST(value AS INTEGER)+1 WHERE key='progress_epoch'; END;
CREATE TRIGGER IF NOT EXISTS progress_task_update AFTER UPDATE ON project_tasks BEGIN UPDATE meta SET value=CAST(value AS INTEGER)+1 WHERE key='progress_epoch'; END;
CREATE TRIGGER IF NOT EXISTS progress_task_delete AFTER DELETE ON project_tasks BEGIN UPDATE meta SET value=CAST(value AS INTEGER)+1 WHERE key='progress_epoch'; END;
CREATE TRIGGER IF NOT EXISTS progress_doc_change AFTER INSERT ON doc_audits BEGIN UPDATE meta SET value=CAST(value AS INTEGER)+1 WHERE key='progress_epoch'; END;
CREATE TRIGGER IF NOT EXISTS progress_doc_update AFTER UPDATE ON doc_audits BEGIN UPDATE meta SET value=CAST(value AS INTEGER)+1 WHERE key='progress_epoch'; END;
DELETE FROM meta WHERE key='scale_summary_cache';
INSERT OR REPLACE INTO meta(key,value) VALUES('dev_control_schema','2');
COMMIT;
)sql";
            if (integer("SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='task_claims'") == 0 ||
                integer("SELECT COUNT(*) FROM meta WHERE key='dev_control_schema' AND value='2'") == 0 ||
                integer("SELECT COUNT(*) FROM sqlite_master WHERE type='trigger' AND name IN('progress_phase_change','progress_phase_update','progress_task_change','progress_task_update','progress_task_delete','progress_doc_change','progress_doc_update')") < 7) {
                auto rc = sqlite3_exec(db_, dev_patch, nullptr, nullptr, nullptr);
                if (rc != SQLITE_OK) {
                    sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
                    throw SqlError(rc, db_);
                }
            }
        }
        WIN32_FILE_ATTRIBUTE_DATA post_fad{};
        if (GetFileAttributesExW(native_path(file).c_str(), GetFileExInfoStandard, &post_fad) != FALSE) {
            const uint64_t post_ctime = (static_cast<uint64_t>(post_fad.ftCreationTime.dwHighDateTime) << 32) | post_fad.ftCreationTime.dwLowDateTime;
            std::lock_guard lk(db_meta_mu);
            db_meta_cache[raw_key] = CachedDbMeta{mutex_name_, post_ctime};
        }
    } catch (...) {
        release_write_lock();
        sqlite3_close(db_);
        db_ = nullptr;
        throw;
    }
}

Database::~Database() {
    if (in_tx_ && db_) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        in_tx_ = false;
    }
    release_write_lock();
    if (db_) sqlite3_close(db_);
}

void Database::exec(const std::string& sql, const Json& params) {
    const auto start = GetTickCount64();
    unsigned backoff_ms = 25;
    for (int attempt = 0;; ++attempt) {
        try {
            std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(prepare(db_, sql, params), sqlite3_finalize);
            auto rc = sqlite3_step(stmt.get());
            if (rc != SQLITE_DONE && rc != SQLITE_ROW) throw SqlError(rc, db_);
            return;
        } catch (const SqlError& e) {
            if (!in_aux_write_ && is_busy_or_locked(e.code) &&
                (GetTickCount64() - start) < static_cast<ULONGLONG>(write_timeout_ms) && attempt < 16) {
                Sleep(backoff_ms);
                backoff_ms = std::min<unsigned>(backoff_ms * 2, 1000u);
                continue;
            }
            throw;
        }
    }
}

bool Database::try_aux_write(const std::string& sql, const Json& params) {
    struct RestoreTimeout {
        sqlite3* db;
        bool& aux;
        ~RestoreTimeout() {
            aux = false;
            sqlite3_busy_timeout(db, write_timeout_ms);
        }
    } restore{db_, in_aux_write_};
    in_aux_write_ = true;
    sqlite3_busy_timeout(db_, 0);
    try {
        exec(sql, params);
        return true;
    } catch (const SqlError& e) {
        if (is_busy_or_locked(e.code)) return false;
        throw;
    }
}

Json Database::query(const std::string& sql, const Json& params) const {
    Json rows=Json::array();
    each(sql,params,[&](const Json& row){rows.push_back(row);return true;});
    return rows;
}
void Database::each(const std::string& sql,const Json& params,const std::function<bool(const Json&)>& visit) const {
    auto stmt = prepare(db_, sql, params);
    struct Finalize { sqlite3_stmt* value; ~Finalize(){sqlite3_finalize(value);} } cleanup{stmt};
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        Json row = Json::object();
        for (int i = 0; i < sqlite3_column_count(stmt); ++i) {
            auto name = sqlite3_column_name(stmt, i);
            switch (sqlite3_column_type(stmt, i)) {
                case SQLITE_NULL: row[name] = nullptr; break;
                case SQLITE_INTEGER: row[name] = sqlite3_column_int64(stmt, i); break;
                case SQLITE_FLOAT: row[name] = sqlite3_column_double(stmt, i); break;
                default:
                    row[name] = std::string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, i)),
                                            static_cast<size_t>(sqlite3_column_bytes(stmt, i)));
            }
        }
        if(!visit(row))return;
    }
    if (rc != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db_));
}

int64_t Database::integer(const std::string& sql, const Json& params) const {
    auto r = query(sql, params);
    return r.empty() || r[0].empty() ? 0 : r[0].begin().value().get<int64_t>();
}

int64_t Database::revision() const {
    return integer("SELECT CAST(value AS INTEGER) FROM meta WHERE key='revision'");
}

int64_t Database::change(const std::string& actor, const std::string& action, const Json& detail, const std::string& entity) {
    auto rev = revision() + 1;
    exec("UPDATE meta SET value=? WHERE key='revision'", {std::to_string(rev)});
    exec("INSERT INTO history(revision,at,actor,action,entity,detail)VALUES(?,?,?,?,?,?)",
         {rev, now(), actor, action, entity, detail.dump()});
    return rev;
}

void Database::begin(int64_t expected) {
    acquire_write_lock();
    try {
        exec("BEGIN IMMEDIATE");
        in_tx_ = true;
    } catch (...) {
        release_write_lock();
        throw;
    }
    if (expected >= 0 && revision() != expected) {
        auto actual = revision();
        rollback();
        throw std::runtime_error("REVISION_CONFLICT expected=" + std::to_string(expected) +
                                 " current=" + std::to_string(actual));
    }
}

void Database::commit() {
    try {
        exec("COMMIT");
        in_tx_ = false;
        release_write_lock();
    } catch (...) {
        rollback();
        throw;
    }
}

void Database::rollback() noexcept {
    if (db_) sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
    in_tx_ = false;
    release_write_lock();
}

Json backup_catalog(const Project& p, const fs::path& destination) {
    if (destination.empty()) throw std::runtime_error("Ruta de respaldo vacía");
    fs::path abs_dest = fs::absolute(destination);
    if (!abs_dest.parent_path().empty()) fs::create_directories(abs_dest.parent_path());
    fs::path staged = fs::path(abs_dest.wstring() + L".tmp-" + wide(unique_id()));
    std::error_code ec;
    fs::remove(staged, ec);

    std::string method = "VACUUM_INTO";
    {
        Database db(p);
        try {
            db.exec("VACUUM INTO ?", {path_text(staged)});
        } catch (...) {
            fs::remove(staged, ec);
            method = "SQLITE_BACKUP_API";
            sqlite3* dst_db{};
            if (sqlite3_open_v2(path_text(staged).c_str(), &dst_db,
                                SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
                if (dst_db) sqlite3_close(dst_db);
                throw std::runtime_error("No se pudo abrir archivo destino para respaldo SQLite");
            }
            sqlite3* src_db{};
            auto src_path = p.state_dir() / L"catalog.sqlite";
            if (sqlite3_open_v2(path_text(src_path).c_str(), &src_db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
                if (src_db) sqlite3_close(src_db);
                sqlite3_close(dst_db);
                fs::remove(staged, ec);
                throw std::runtime_error("No se pudo abrir catálogo origen para respaldo SQLite");
            }
            sqlite3_busy_timeout(src_db, write_timeout_ms);
            sqlite3_backup* bkp = sqlite3_backup_init(dst_db, "main", src_db, "main");
            if (!bkp) {
                sqlite3_close(src_db);
                sqlite3_close(dst_db);
                fs::remove(staged, ec);
                throw std::runtime_error("sqlite3_backup_init falló");
            }
            int brc = sqlite3_backup_step(bkp, -1);
            sqlite3_backup_finish(bkp);
            sqlite3_close(src_db);
            sqlite3_close(dst_db);
            if (brc != SQLITE_DONE) {
                fs::remove(staged, ec);
                throw std::runtime_error("sqlite3_backup_step falló con código " + std::to_string(brc));
            }
        }
    }

    // Verify integrity and counts on the staged backup before replacing destination
    sqlite3* check_db{};
    if (sqlite3_open_v2(path_text(staged).c_str(), &check_db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        if (check_db) sqlite3_close(check_db);
        fs::remove(staged, ec);
        throw std::runtime_error("No se pudo abrir el respaldo generado para verificación");
    }
    struct CloseCheck { sqlite3* d; ~CloseCheck() { if (d) sqlite3_close(d); } } close_check{check_db};
    auto scalar_text = [&](const char* q) -> std::string {
        sqlite3_stmt* st{};
        if (sqlite3_prepare_v2(check_db, q, -1, &st, nullptr) != SQLITE_OK) return "";
        std::string out;
        if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0))
            out = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
        sqlite3_finalize(st);
        return out;
    };
    auto scalar_int = [&](const char* q) -> int64_t {
        sqlite3_stmt* st{};
        if (sqlite3_prepare_v2(check_db, q, -1, &st, nullptr) != SQLITE_OK) return 0;
        int64_t out = 0;
        if (sqlite3_step(st) == SQLITE_ROW) out = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
        return out;
    };

    std::string qc = scalar_text("PRAGMA quick_check");
    int64_t rev = scalar_int("SELECT CAST(value AS INTEGER) FROM meta WHERE key='revision'");
    int64_t mappings_cnt = scalar_int("SELECT COUNT(*) FROM mappings");
    int64_t notes_cnt = scalar_int("SELECT COUNT(*) FROM notes");
    int64_t files_cnt = scalar_int("SELECT COUNT(*) FROM files WHERE deleted=0");
    int64_t symbols_cnt = scalar_int("SELECT COUNT(*) FROM symbols WHERE active=1");
    sqlite3_close(check_db);
    close_check.d = nullptr;

    if (qc != "ok") {
        fs::remove(staged, ec);
        throw std::runtime_error("BACKUP_INTEGRITY_FAILED: PRAGMA quick_check=" + qc);
    }

    if (!MoveFileExW(staged.c_str(), abs_dest.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        fs::remove(staged, ec);
        throw std::runtime_error("No se pudo mover respaldo verificado a " + path_text(abs_dest));
    }

    const auto bytes = fs::file_size(abs_dest);
    return {
        {"ok", true},
        {"method", method},
        {"backup_path", path_text(abs_dest)},
        {"bytes", bytes},
        {"quick_check", qc},
        {"revision", rev},
        {"mappings_count", mappings_cnt},
        {"notes_count", notes_cnt},
        {"files_count", files_cnt},
        {"symbols_count", symbols_cnt},
        {"at", now()}
    };
}
} // namespace mt
