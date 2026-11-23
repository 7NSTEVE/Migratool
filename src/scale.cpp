#include "migratool/scale.hpp"
#include "migratool/fs_index.hpp"
#include "migratool/sql.hpp"
#include "migratool/duplicates.hpp"
#include "migratool/analysis_context.hpp"
#include "digest.hpp"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <unordered_set>
#include <psapi.h>
#include <winioctl.h>

namespace mt {
namespace {
std::string lower(std::string s){std::transform(s.begin(),s.end(),s.begin(),[](unsigned char c){return static_cast<char>(tolower(c));});return s;}
std::string self_path(){std::wstring value(32768,L'\0');auto n=GetModuleFileNameW(nullptr,value.data(),static_cast<DWORD>(value.size()));value.resize(n);return utf8(value);}
std::string worker_path(){for(const auto* name:{L"migratool-cli-0.3.exe",L"migratool-cli.exe"}){auto p=executable_dir()/name;if(fs::exists(p))return path_text(p);}throw std::runtime_error("COORDINATOR_EXECUTABLE_MISSING");}
std::wstring coordinator_name(const Project& p){return L"Local\\MigratoolCoordinator_"+wide(sha256(lower(path_text(fs::weakly_canonical(p.state_dir())))).substr(0,32));}
struct Handle {HANDLE h{};~Handle(){if(h&&h!=INVALID_HANDLE_VALUE)CloseHandle(h);}operator HANDLE()const{return h;}};
Json safe_parse(const Json& j){return j.is_string()?Json::parse(j.get<std::string>()):j;}
Json resource_profile(const Project& p){auto r=p.config.value("resources",Json::object());return {{"memory_mib",r.value("memory_mib",4096)},{"workers",r.value("workers",8)},{"analyzers",r.value("analyzers",2)}};}

struct SharedLedgerSlot {
    DWORD pid{};
    DWORD requested_mib{};
    DWORD allocated_mib{};
    DWORD workers{};
    DWORD analyzers{};
    char key[32]{};
};
struct SharedLedgerPage {
    DWORD version{1};
    SharedLedgerSlot slots[64]{};
};
class SharedBudgetLedger {
    Handle mutex_;
    Handle map_;
    SharedLedgerPage* page_{};
    std::string key_;
    static bool alive(DWORD pid) {
        if (!pid) return false;
        if (pid == GetCurrentProcessId()) return true;
        HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
        if (!p) return GetLastError() == ERROR_ACCESS_DENIED;
        DWORD code = 0;
        const bool ok = GetExitCodeProcess(p, &code) && code == STILL_ACTIVE;
        CloseHandle(p);
        return ok;
    }
    void rebalance() {
        if (!page_) return;
        uint64_t total_req = 0;
        int active = 0;
        for (auto& s : page_->slots) {
            if (s.pid && !alive(s.pid)) s = SharedLedgerSlot{};
            if (s.pid) { ++active; total_req += std::max<DWORD>(128, s.requested_mib); }
        }
        if (active == 0) return;
        const uint64_t cap = 4096;
        uint64_t assigned_sum = 0;
        if (total_req <= cap) {
            for (auto& s : page_->slots) if (s.pid) { s.allocated_mib = std::max<DWORD>(128, s.requested_mib); assigned_sum += s.allocated_mib; }
        } else if (static_cast<uint64_t>(active) * 128 <= cap) {
            const uint64_t extra_cap = cap - static_cast<uint64_t>(active) * 128;
            const uint64_t extra_req = total_req - static_cast<uint64_t>(active) * 128;
            for (auto& s : page_->slots) {
                if (!s.pid) continue;
                const uint64_t req = std::max<DWORD>(128, s.requested_mib);
                const uint64_t share_extra = extra_req ? (extra_cap * (req - 128)) / extra_req : 0;
                s.allocated_mib = static_cast<DWORD>(std::min<uint64_t>(req, 128 + share_extra));
                assigned_sum += s.allocated_mib;
            }
            for (auto& s : page_->slots) {
                if (!s.pid) continue;
                while (assigned_sum > cap && s.allocated_mib > 128) { --s.allocated_mib; --assigned_sum; }
            }
        } else {
            for (auto& s : page_->slots) {
                if (!s.pid) continue;
                const uint64_t req = std::max<DWORD>(128, s.requested_mib);
                uint64_t share = std::max<uint64_t>(1, (cap * req) / total_req);
                if (share > req) share = req;
                s.allocated_mib = static_cast<DWORD>(share);
                assigned_sum += s.allocated_mib;
            }
            for (auto& s : page_->slots) {
                if (!s.pid) continue;
                while (assigned_sum > cap && s.allocated_mib > 1) { --s.allocated_mib; --assigned_sum; }
            }
        }
    }
    Json read_locked() const {
        if (!page_) return {{"budget_mib",4096},{"active_projects",0},{"total_allocated_mib",0},{"project_allocated_mib",0}};
        int active = 0;
        DWORD total_alloc = 0, proj_alloc = 0;
        for (const auto& s : page_->slots) {
            if (!s.pid || !alive(s.pid)) continue;
            ++active;
            total_alloc += s.allocated_mib;
            if (key_ == s.key) proj_alloc = s.allocated_mib;
        }
        return {{"budget_mib",4096},{"active_projects",active},{"total_allocated_mib",total_alloc},{"project_allocated_mib",proj_alloc}};
    }
    static bool acquire_mutex(HANDLE h, DWORD timeout_ms = 5000) {
        if (!h) return false;
        const DWORD rc = WaitForSingleObject(h, timeout_ms);
        return rc == WAIT_OBJECT_0 || rc == WAIT_ABANDONED;
    }
public:
    explicit SharedBudgetLedger(const Project& p)
        : key_(sha256(lower(path_text(fs::weakly_canonical(p.state_dir())))).substr(0, 31)) {
        mutex_.h = CreateMutexW(nullptr, FALSE, L"Local\\MigratoolSharedLedgerMutex");
        map_.h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(SharedLedgerPage), L"Local\\MigratoolSharedLedgerData");
        if (map_.h) {
            page_ = static_cast<SharedLedgerPage*>(MapViewOfFile(map_.h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedLedgerPage)));
            // AUD-037: Acquire interprocess mutex before initializing shared memory!
            if (page_ && acquire_mutex(mutex_.h)) {
                if (page_->version != 1) {
                    std::memset(page_, 0, sizeof(SharedLedgerPage));
                    page_->version = 1;
                }
                ReleaseMutex(mutex_.h);
            }
        }
    }
    ~SharedBudgetLedger() { release(); if (page_) UnmapViewOfFile(page_); }
    Json sync(DWORD requested_mib, DWORD workers, DWORD analyzers) {
        if (!page_ || !mutex_.h || !acquire_mutex(mutex_.h)) {
            return {{"budget_mib",4096},{"active_projects",1},{"total_allocated_mib",requested_mib},{"project_allocated_mib",requested_mib},{"effective_workers",workers},{"saturated",false}};
        }
        struct Unlocker { HANDLE h; ~Unlocker(){ if(h) ReleaseMutex(h); } } unlock{mutex_.h};
        const DWORD me = GetCurrentProcessId();
        int mine = -1, empty = -1;
        for (int i = 0; i < 64; ++i) {
            auto& s = page_->slots[i];
            if (s.pid && !alive(s.pid)) s = SharedLedgerSlot{};
            if (s.pid == me && key_ == s.key) { mine = i; break; }
            if (!s.pid && empty < 0) empty = i;
        }
        int idx = mine >= 0 ? mine : empty;
        if (idx < 0) {
            rebalance();
            auto out = read_locked();
            out["effective_workers"] = 1;
            out["saturated"] = true;
            return out;
        }
        auto& slot = page_->slots[idx];
        slot.pid = me;
        slot.requested_mib = std::clamp<DWORD>(requested_mib, 128, 4096);
        slot.workers = std::clamp<DWORD>(workers, 1, 8);
        slot.analyzers = std::clamp<DWORD>(analyzers, 1, 2);
        std::memset(slot.key, 0, sizeof(slot.key));
        std::memcpy(slot.key, key_.data(), std::min(key_.size(), sizeof(slot.key) - 1));
        rebalance();
        auto out = read_locked();
        const int active = std::max(1, out["active_projects"].get<int>());
        out["effective_workers"] = std::max<DWORD>(1, std::min<DWORD>(slot.workers, 8 / static_cast<DWORD>(active)));
        out["saturated"] = slot.allocated_mib < 128;
        return out;
    }
    Json status() const {
        if (!page_ || !mutex_.h || !acquire_mutex(mutex_.h)) {
            return {{"budget_mib",4096},{"active_projects",0},{"total_allocated_mib",0},{"project_allocated_mib",0}};
        }
        auto out = read_locked();
        ReleaseMutex(mutex_.h);
        return out;
    }
    void release() {
        if (!page_ || !mutex_.h || !acquire_mutex(mutex_.h)) return;
        const DWORD me = GetCurrentProcessId();
        for (auto& s : page_->slots) if (s.pid == me && key_ == s.key) s = SharedLedgerSlot{};
        rebalance();
        ReleaseMutex(mutex_.h);
    }
};

struct GlobalSlot {
    HANDLE handle{};
    GlobalSlot(const wchar_t* category,int count,const std::function<void()>& control){for(;;){for(int i=0;i<count;++i){auto name=std::wstring(L"Local\\Migratool_")+category+L"_"+std::to_wstring(i);auto h=CreateMutexW(nullptr,FALSE,name.c_str());if(!h)throw std::runtime_error("RESOURCE_SLOT_CREATE");auto result=WaitForSingleObject(h,0);if(result==WAIT_OBJECT_0||result==WAIT_ABANDONED){handle=h;return;}CloseHandle(h);}control();Sleep(50);}}
    ~GlobalSlot(){if(handle){ReleaseMutex(handle);CloseHandle(handle);}}
};
bool terminal(const std::string& s){return s=="completed"||s=="completed_with_errors"||s=="cancelled"||s=="failed";}
std::string scalar(Database& db,const std::string& sql,const Json& params=Json::array()){
    auto r=db.query(sql,params);return r.empty()||r[0].begin().value().is_null()?"":r[0].begin().value().get<std::string>();
}
Json preflight_warnings(const Project& p){
    Json warnings=Json::array();
    std::set<std::string> excluded;
    for(const auto& ex:p.config.value("exclude_dirs",Json::array()))if(ex.is_string())excluded.insert(lower(ex.get<std::string>()));
    for(size_t i=0;i<p.config["roots"].size();++i){
        const auto& r=p.config["roots"][i];
        const auto path=p.resolve(r["path"]);
        const auto attrs=GetFileAttributesW(native_path(path).c_str());
        if(attrs==INVALID_FILE_ATTRIBUTES)warnings.push_back({{"code","MISSING_ROOT_PATH"},{"root",r["id"]},{"path",path_text(path)}});
        else if(attrs&FILE_ATTRIBUTE_REPARSE_POINT)warnings.push_back({{"code","REPARSE_POINT_ROOT"},{"root",r["id"]},{"path",path_text(path)}});
        if(excluded.contains(lower(path_text(path.filename()))))warnings.push_back({{"code","ROOT_MATCHES_EXCLUDE_DIRS"},{"root",r["id"]},{"path",path_text(path)}});
        for(size_t j=0;j<i;++j){
            auto a=path,b=p.resolve(p.config["roots"][j]["path"]);
            if(within(a,b)||within(b,a))warnings.push_back({{"code","OVERLAPPING_ROOTS"},{"roots",{p.config["roots"][j]["id"],r["id"]}},{"paths",{path_text(b),path_text(a)}}});
        }
    }
    return warnings;
}
Json settings_view(const Project& p,Database& db){return {{"schema","migratool.settings/1"},{"config_revision",p.revision},{"revision",db.revision()},
    {"name",p.config.value("name","")},{"roots",p.config["roots"]},{"inventory_mode",p.config.value("inventory_mode","universal")},{"inventory_engine",p.config.value("inventory_engine","auto")},
    {"code_duplicate_mode",p.config.value("code_duplicate_mode","off")},{"nested_zip_depth",p.config.value("nested_zip_depth",2)},
    {"code_extensions",p.config.value("code_extensions",Json::array())},{"code_filenames",p.config.value("code_filenames",Json::array())},
    {"exclude_dirs",p.config.value("exclude_dirs",Json::array())},{"analyze_cpp",p.config.value("analyze_cpp",true)},
    {"analyzers",p.config.value("analyzers",Json::array())},{"defines",p.config.value("defines",Json::array())},{"clang_args",p.config.value("clang_args",Json::array())},
    {"builds",p.config.value("builds",Json::array())},{"cases",p.config.value("cases",Json::array())},
    {"resources",resource_profile(p)},{"preflight",preflight_warnings(p)}};}
void require_actor(const Json& args){if(args.value("actor","").empty())throw std::runtime_error("actor requerido");}
bool missing_path(DWORD error){return error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND;}
void check_scope_ancestors(const fs::path& base,const fs::path& relative){
    auto path=base;
    for(const auto& component:relative.parent_path()){
        path/=component;
        const auto attributes=GetFileAttributesW(native_path(path).c_str());
        if(attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("SCOPE_REPARSE_POINT: "+path_text(path));
    }
}
Json job_record(Json row){
    row["scope"]=safe_parse(row["scope"]);row["resources"]=safe_parse(row["resources"]);
    const bool exporting=row["scope"].value("kind",std::string())=="export_inventory";
    row["kind"]=exporting?"export_inventory":"inventory";
    row["job_complete"]=row["state"]=="completed";
    const auto incomplete=row["resources"].value("incomplete_analysis",Json());
    row["analysis_completion_known"]=!incomplete.is_null();
    row["analysis_complete"]=row["state"]=="completed"&&row["phase"]=="finished"&&row.value("pending_analysis",0)==0&&incomplete==0;
    row["complete"]=exporting?row["job_complete"]:row["analysis_complete"];
    if(exporting){
        row["census_complete"]=nullptr;
        row["result"]=row["resources"].value("export_result",Json());
        if(row["resources"].contains("export_checkpoint")){
            const auto& cp=row["resources"]["export_checkpoint"];
            row["catalog_generation"]=cp.value("generation",int64_t(0));
            row["export_total"]=cp.value("total",int64_t(0));
        }
    }
    const auto elapsed=row.value("elapsed_ms",int64_t(0));
    const auto state=row.value("state",std::string());
    const auto phase=row.value("phase",std::string());
    double rate=0.0;
    if(elapsed>=1000){
        if(phase=="census")rate=1000.0*row.value("files",0.0)/elapsed;
        else if(phase=="analysis")rate=1000.0*row.value("processed",0.0)/elapsed;
        else if(phase=="export")rate=1000.0*row.value("files",0.0)/elapsed;
    }
    row["observed_files_per_second"]=rate>0.0?Json(rate):Json();
    Json eta=nullptr;
    if(state=="running"&&elapsed>=3000&&rate>0.0){
        if(phase=="analysis"){
            const auto pending=row.value("pending_analysis",int64_t(0));
            const auto done=row.value("processed",int64_t(0));
            if(done>0&&pending>0)eta=static_cast<int64_t>(std::ceil(static_cast<double>(pending)/rate));
        }else if(phase=="export"&&row["resources"].contains("export_checkpoint")){
            const auto& cp=row["resources"]["export_checkpoint"];
            const auto total=cp.value("total",int64_t(0));
            const auto done=row.value("files",int64_t(0));
            if(total>done&&done>0)eta=static_cast<int64_t>(std::ceil(static_cast<double>(total-done)/rate));
        }
    }
    row["eta_seconds"]=eta;
    return row;
}
void add_error(Database& db,const std::string& job,const std::string& root,const std::string& path,const std::string& op,int code,const std::string& message){
    db.exec("INSERT INTO scan_errors(job_id,root,path,operation,code,message,at)VALUES(?,?,?,?,?,?,?)",{job,root,path,op,code,message,now()});
    db.exec("UPDATE inventory_jobs SET failed=failed+1,error=?,updated=? WHERE id=?",{message,now(),job});
}
}

void scale_schema(Database& db){
    const char* sql=R"sql(
BEGIN IMMEDIATE;
CREATE INDEX IF NOT EXISTS file_root_path ON files(root,path COLLATE NOCASE,deleted);
CREATE INDEX IF NOT EXISTS file_language ON files(deleted,language,id);
CREATE INDEX IF NOT EXISTS symbol_name ON symbols(name,active,file_id);
CREATE INDEX IF NOT EXISTS symbol_qualified ON symbols(qualified_name,active,file_id);
CREATE INDEX IF NOT EXISTS symbol_semantic ON symbols(json_extract(data,'$.semantic_key')) WHERE active=1;
CREATE INDEX IF NOT EXISTS dependency_target ON dependencies(target_id,file_id);
CREATE INDEX IF NOT EXISTS dependency_name ON dependencies(name,file_id);
CREATE TABLE IF NOT EXISTS search_rows(entity_type TEXT NOT NULL,entity_id TEXT NOT NULL,row_id INTEGER NOT NULL,PRIMARY KEY(entity_type,entity_id,row_id)) WITHOUT ROWID;
INSERT OR IGNORE INTO search_rows SELECT entity_type,entity_id,rowid FROM search;
CREATE INDEX IF NOT EXISTS history_entity ON history(entity,action,revision);
CREATE TABLE IF NOT EXISTS hash_copies(sha256 TEXT PRIMARY KEY,copies INTEGER NOT NULL);
INSERT OR IGNORE INTO hash_copies SELECT sha256,COUNT(*) FROM files WHERE deleted=0 AND sha256 IS NOT NULL GROUP BY sha256;
CREATE TRIGGER IF NOT EXISTS hashes_insert AFTER INSERT ON files WHEN new.deleted=0 AND new.sha256 IS NOT NULL BEGIN
 INSERT INTO hash_copies VALUES(new.sha256,1) ON CONFLICT(sha256)DO UPDATE SET copies=copies+1; END;
CREATE TRIGGER IF NOT EXISTS hashes_delete AFTER DELETE ON files WHEN old.deleted=0 AND old.sha256 IS NOT NULL BEGIN
 UPDATE hash_copies SET copies=copies-1 WHERE sha256=old.sha256; END;
CREATE TRIGGER IF NOT EXISTS hashes_update AFTER UPDATE OF sha256,deleted ON files BEGIN
 UPDATE hash_copies SET copies=copies-1 WHERE sha256=old.sha256 AND old.deleted=0;
 INSERT INTO hash_copies SELECT new.sha256,1 WHERE new.deleted=0 AND new.sha256 IS NOT NULL
 ON CONFLICT(sha256)DO UPDATE SET copies=copies+1; END;
CREATE TABLE IF NOT EXISTS inventory_jobs(id TEXT PRIMARY KEY,state TEXT NOT NULL,phase TEXT NOT NULL,scope TEXT NOT NULL,
 config_sha256 TEXT NOT NULL,created TEXT NOT NULL,updated TEXT NOT NULL,started TEXT,finished TEXT,pid INTEGER,
 control TEXT NOT NULL DEFAULT '',error TEXT NOT NULL DEFAULT '',files INTEGER NOT NULL DEFAULT 0,dirs INTEGER NOT NULL DEFAULT 0,
 links INTEGER NOT NULL DEFAULT 0,bytes INTEGER NOT NULL DEFAULT 0,processed INTEGER NOT NULL DEFAULT 0,failed INTEGER NOT NULL DEFAULT 0,
 generation INTEGER NOT NULL DEFAULT 0,resources TEXT NOT NULL,from_revision INTEGER NOT NULL,census_complete INTEGER NOT NULL DEFAULT 0);
CREATE INDEX IF NOT EXISTS jobs_state ON inventory_jobs(state,created);
CREATE TABLE IF NOT EXISTS inventory_generations(id INTEGER PRIMARY KEY AUTOINCREMENT,job_id TEXT UNIQUE NOT NULL,
 state TEXT NOT NULL,created TEXT NOT NULL,published TEXT);
CREATE TABLE IF NOT EXISTS generation_scopes(generation INTEGER NOT NULL,root TEXT NOT NULL,prefix TEXT NOT NULL,PRIMARY KEY(generation,root,prefix)) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS scope_history ON generation_scopes(root,generation,prefix);
CREATE INDEX IF NOT EXISTS roslyn_source_paths ON files(root,path COLLATE NOCASE) WHERE deleted=0 AND substr(lower(path),-3)='.cs';
CREATE INDEX IF NOT EXISTS roslyn_project_paths ON files(root,path COLLATE NOCASE) WHERE substr(lower(path),-7)='.csproj' AND (deleted=0 OR COALESCE(json_extract(data,'$.present'),0)=1);
INSERT OR IGNORE INTO generation_scopes SELECT j.generation,r.id,CASE WHEN json_extract(j.scope,'$.file') IS NOT NULL THEN json_extract(j.scope,'$.file')||'/' WHEN COALESCE(json_extract(j.scope,'$.subdirectory'),'.')='.' THEN '' ELSE json_extract(j.scope,'$.subdirectory')||'/' END FROM inventory_jobs j CROSS JOIN roots r WHERE j.generation>0 AND (j.scope='{}' OR json_extract(j.scope,'$.root')=r.id);
CREATE TABLE IF NOT EXISTS inventory_dirs(job_id TEXT NOT NULL,root TEXT NOT NULL,path TEXT NOT NULL,absolute_path TEXT NOT NULL,
 state TEXT NOT NULL DEFAULT 'queued',error TEXT NOT NULL DEFAULT '',PRIMARY KEY(job_id,root,path)) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS dirs_queue ON inventory_dirs(job_id,state,root,path);
CREATE TABLE IF NOT EXISTS census_entries(id INTEGER PRIMARY KEY,generation INTEGER NOT NULL,root TEXT NOT NULL,path TEXT NOT NULL,
 parent TEXT NOT NULL,name TEXT NOT NULL,absolute_path TEXT NOT NULL,kind TEXT NOT NULL,bytes INTEGER NOT NULL,mtime INTEGER NOT NULL,
 attrs INTEGER NOT NULL,link_target TEXT,file_id INTEGER,UNIQUE(generation,root,path));
CREATE INDEX IF NOT EXISTS census_parent ON census_entries(generation,root,parent,id);
CREATE INDEX IF NOT EXISTS census_file ON census_entries(generation,file_id);
CREATE INDEX IF NOT EXISTS census_file_history ON census_entries(file_id,generation);
CREATE TABLE IF NOT EXISTS directory_ids(root TEXT NOT NULL,volume_serial INTEGER NOT NULL,frn INTEGER NOT NULL,path TEXT NOT NULL,PRIMARY KEY(root,volume_serial,frn)) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS scan_errors(id INTEGER PRIMARY KEY,job_id TEXT NOT NULL,root TEXT NOT NULL,path TEXT NOT NULL,
 operation TEXT NOT NULL,code INTEGER NOT NULL,message TEXT NOT NULL,at TEXT NOT NULL);
CREATE INDEX IF NOT EXISTS scan_errors_job ON scan_errors(job_id,id);
CREATE TABLE IF NOT EXISTS inventory_tasks(job_id TEXT NOT NULL,file_id INTEGER NOT NULL,state TEXT NOT NULL DEFAULT 'queued',
 phase TEXT NOT NULL DEFAULT 'content',error TEXT NOT NULL DEFAULT '',PRIMARY KEY(job_id,file_id)) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS tasks_queue ON inventory_tasks(job_id,state,file_id);
CREATE TABLE IF NOT EXISTS archive_members(id INTEGER PRIMARY KEY,archive_id INTEGER NOT NULL,entry_index INTEGER NOT NULL,
 entry_path TEXT NOT NULL,bytes INTEGER,sha256 TEXT,state TEXT NOT NULL,error TEXT NOT NULL DEFAULT '',UNIQUE(archive_id,entry_index));
CREATE INDEX IF NOT EXISTS member_hash ON archive_members(sha256,bytes);
CREATE TABLE IF NOT EXISTS code_hash_counts(sha256 TEXT NOT NULL,bytes INTEGER NOT NULL,file_copies INTEGER NOT NULL DEFAULT 0,zip_copies INTEGER NOT NULL DEFAULT 0,PRIMARY KEY(sha256,bytes)) WITHOUT ROWID;
INSERT OR IGNORE INTO code_hash_counts SELECT sha256,bytes,COUNT(*),0 FROM files WHERE deleted=0 AND sha256 IS NOT NULL AND COALESCE(json_extract(data,'$.source_code'),0)=1 GROUP BY sha256,bytes;
INSERT INTO code_hash_counts SELECT sha256,bytes,0,COUNT(*) FROM archive_members WHERE state='hashed' AND NOT EXISTS(SELECT 1 FROM meta WHERE key='code_hash_schema') GROUP BY sha256,bytes ON CONFLICT(sha256,bytes)DO UPDATE SET zip_copies=excluded.zip_copies;
INSERT OR IGNORE INTO meta VALUES('code_hash_schema','1');
CREATE TRIGGER IF NOT EXISTS code_hash_insert AFTER INSERT ON files WHEN new.deleted=0 AND new.sha256 IS NOT NULL AND COALESCE(json_extract(new.data,'$.source_code'),0)=1 BEGIN
 INSERT INTO code_hash_counts VALUES(new.sha256,new.bytes,1,0)ON CONFLICT(sha256,bytes)DO UPDATE SET file_copies=file_copies+1; END;
CREATE TRIGGER IF NOT EXISTS code_hash_delete AFTER DELETE ON files WHEN old.deleted=0 AND old.sha256 IS NOT NULL AND COALESCE(json_extract(old.data,'$.source_code'),0)=1 BEGIN
 UPDATE code_hash_counts SET file_copies=file_copies-1 WHERE sha256=old.sha256 AND bytes=old.bytes; END;
CREATE TRIGGER IF NOT EXISTS code_hash_update AFTER UPDATE OF sha256,bytes,deleted,data ON files WHEN old.sha256 IS NOT new.sha256 OR old.bytes IS NOT new.bytes OR old.deleted IS NOT new.deleted OR COALESCE(json_extract(old.data,'$.source_code'),0) IS NOT COALESCE(json_extract(new.data,'$.source_code'),0) BEGIN
 UPDATE code_hash_counts SET file_copies=file_copies-1 WHERE sha256=old.sha256 AND bytes=old.bytes AND old.deleted=0 AND COALESCE(json_extract(old.data,'$.source_code'),0)=1;
 INSERT INTO code_hash_counts SELECT new.sha256,new.bytes,1,0 WHERE new.deleted=0 AND new.sha256 IS NOT NULL AND COALESCE(json_extract(new.data,'$.source_code'),0)=1 ON CONFLICT(sha256,bytes)DO UPDATE SET file_copies=file_copies+1; END;
CREATE TRIGGER IF NOT EXISTS zip_hash_insert AFTER INSERT ON archive_members WHEN new.state='hashed' BEGIN
 INSERT INTO code_hash_counts VALUES(new.sha256,new.bytes,0,1)ON CONFLICT(sha256,bytes)DO UPDATE SET zip_copies=zip_copies+1; END;
CREATE TRIGGER IF NOT EXISTS zip_hash_delete AFTER DELETE ON archive_members WHEN old.state='hashed' BEGIN
 UPDATE code_hash_counts SET zip_copies=zip_copies-1 WHERE sha256=old.sha256 AND bytes=old.bytes; END;
CREATE TRIGGER IF NOT EXISTS zip_hash_update AFTER UPDATE ON archive_members BEGIN
 UPDATE code_hash_counts SET zip_copies=zip_copies-1 WHERE sha256=old.sha256 AND bytes=old.bytes AND old.state='hashed';
 INSERT INTO code_hash_counts SELECT new.sha256,new.bytes,0,1 WHERE new.state='hashed' ON CONFLICT(sha256,bytes)DO UPDATE SET zip_copies=zip_copies+1; END;
CREATE TRIGGER IF NOT EXISTS zip_parent_stale AFTER UPDATE OF sha256,deleted,data ON files WHEN (new.sha256 IS NULL AND old.sha256 IS NOT NULL) OR (COALESCE(json_extract(old.data,'$.present'),1)=1 AND COALESCE(json_extract(new.data,'$.present'),1)=0) BEGIN
 UPDATE archive_members SET state='stale',error='ARCHIVE_CHANGED_OR_MISSING' WHERE archive_id=new.id AND state='hashed'; END;
CREATE TABLE IF NOT EXISTS reuse_evidence(id INTEGER PRIMARY KEY,source_sha256 TEXT,source_path TEXT,output_path TEXT,output_sha256 TEXT,
 origin_catalog TEXT NOT NULL,origin_id TEXT NOT NULL,architecture TEXT,action TEXT,tool_sha256 TEXT,config_sha256 TEXT,
 state TEXT NOT NULL,checked TEXT,detail TEXT NOT NULL,UNIQUE(origin_catalog,origin_id,output_path));
CREATE INDEX IF NOT EXISTS reuse_source ON reuse_evidence(source_sha256,id);
CREATE TABLE IF NOT EXISTS analysis_cache(cache_key TEXT PRIMARY KEY,file_id INTEGER NOT NULL,sha256 TEXT NOT NULL,tool TEXT NOT NULL,at TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS dirty_paths(id INTEGER PRIMARY KEY,root TEXT NOT NULL,path TEXT NOT NULL,action INTEGER NOT NULL,at TEXT NOT NULL,
 UNIQUE(root,path));
CREATE TABLE IF NOT EXISTS file_units(file_id INTEGER PRIMARY KEY,units INTEGER NOT NULL);
CREATE INDEX IF NOT EXISTS fingerprint_bucket ON files(root,(id/1024),id);
CREATE TABLE IF NOT EXISTS root_buckets(root TEXT NOT NULL,bucket INTEGER NOT NULL,sha256 TEXT NOT NULL,PRIMARY KEY(root,bucket)) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS dirty_buckets(root TEXT NOT NULL,bucket INTEGER NOT NULL,PRIMARY KEY(root,bucket)) WITHOUT ROWID;
INSERT OR IGNORE INTO dirty_buckets SELECT root,id/1024 FROM files WHERE NOT EXISTS(SELECT 1 FROM meta WHERE key='bucket_schema');
INSERT OR IGNORE INTO meta VALUES('bucket_schema','1');
INSERT OR IGNORE INTO meta VALUES('progress_epoch','0');
INSERT OR IGNORE INTO meta VALUES('visit_sequence','0');
CREATE TRIGGER IF NOT EXISTS fingerprint_insert AFTER INSERT ON files BEGIN
 INSERT OR IGNORE INTO dirty_buckets VALUES(new.root,new.id/1024);
 UPDATE meta SET value=CAST(value AS INTEGER)+1 WHERE key='progress_epoch'; END;
CREATE TRIGGER IF NOT EXISTS fingerprint_delete AFTER DELETE ON files BEGIN
 INSERT OR IGNORE INTO dirty_buckets VALUES(old.root,old.id/1024);
 UPDATE meta SET value=CAST(value AS INTEGER)+1 WHERE key='progress_epoch'; END;
CREATE TRIGGER IF NOT EXISTS fingerprint_update AFTER UPDATE OF root,path,sha256,bytes,mtime,deleted,relevant,analysis,category ON files
 WHEN old.root IS NOT new.root OR old.path IS NOT new.path OR old.sha256 IS NOT new.sha256 OR old.bytes IS NOT new.bytes OR old.mtime IS NOT new.mtime OR old.deleted IS NOT new.deleted OR old.relevant IS NOT new.relevant OR old.analysis IS NOT new.analysis OR old.category IS NOT new.category BEGIN
 INSERT OR IGNORE INTO dirty_buckets VALUES(old.root,old.id/1024);
 INSERT OR IGNORE INTO dirty_buckets VALUES(new.root,new.id/1024);
 UPDATE meta SET value=CAST(value AS INTEGER)+1 WHERE key='progress_epoch'; END;
CREATE TRIGGER IF NOT EXISTS progress_mapping_insert AFTER INSERT ON mappings BEGIN UPDATE meta SET value=CAST(value AS INTEGER)+1 WHERE key='progress_epoch'; END;
CREATE TRIGGER IF NOT EXISTS progress_mapping_update AFTER UPDATE ON mappings BEGIN UPDATE meta SET value=CAST(value AS INTEGER)+1 WHERE key='progress_epoch'; END;
CREATE TRIGGER IF NOT EXISTS progress_mapping_delete AFTER DELETE ON mappings BEGIN UPDATE meta SET value=CAST(value AS INTEGER)+1 WHERE key='progress_epoch'; END;
CREATE TRIGGER IF NOT EXISTS progress_evidence_insert AFTER INSERT ON evidence BEGIN UPDATE meta SET value=CAST(value AS INTEGER)+1 WHERE key='progress_epoch'; END;
CREATE TRIGGER IF NOT EXISTS progress_evidence_update AFTER UPDATE ON evidence BEGIN UPDATE meta SET value=CAST(value AS INTEGER)+1 WHERE key='progress_epoch'; END;
CREATE TRIGGER IF NOT EXISTS progress_symbol_insert AFTER INSERT ON symbols BEGIN UPDATE meta SET value=CAST(value AS INTEGER)+1 WHERE key='progress_epoch'; END;
CREATE TRIGGER IF NOT EXISTS progress_symbol_update AFTER UPDATE ON symbols BEGIN UPDATE meta SET value=CAST(value AS INTEGER)+1 WHERE key='progress_epoch'; END;
CREATE TRIGGER IF NOT EXISTS progress_symbol_delete AFTER DELETE ON symbols BEGIN UPDATE meta SET value=CAST(value AS INTEGER)+1 WHERE key='progress_epoch'; END;
INSERT OR IGNORE INTO file_units SELECT file_id,COUNT(*) FROM symbols WHERE active=1 AND kind<>'file' GROUP BY file_id;
CREATE TRIGGER IF NOT EXISTS units_insert AFTER INSERT ON symbols WHEN new.active=1 AND new.kind<>'file' BEGIN
 INSERT INTO file_units VALUES(new.file_id,1)ON CONFLICT(file_id)DO UPDATE SET units=units+1; END;
CREATE TRIGGER IF NOT EXISTS units_delete AFTER DELETE ON symbols WHEN old.active=1 AND old.kind<>'file' BEGIN
 UPDATE file_units SET units=units-1 WHERE file_id=old.file_id; END;
CREATE TRIGGER IF NOT EXISTS units_update AFTER UPDATE ON symbols BEGIN
 UPDATE file_units SET units=units-1 WHERE file_id=old.file_id AND old.active=1 AND old.kind<>'file';
 INSERT INTO file_units SELECT new.file_id,1 WHERE new.active=1 AND new.kind<>'file' ON CONFLICT(file_id)DO UPDATE SET units=units+1; END;
)sql";
    const char* sql_tail=R"sql(
ALTER TABLE files ADD COLUMN seen_generation INTEGER NOT NULL DEFAULT 0;
ALTER TABLE inventory_tasks ADD COLUMN attempts INTEGER NOT NULL DEFAULT 0;
ALTER TABLE census_entries ADD COLUMN visit INTEGER NOT NULL DEFAULT 0;
CREATE TABLE IF NOT EXISTS task_claims(entity TEXT PRIMARY KEY,actor TEXT NOT NULL,session_id TEXT,claimed_at TEXT NOT NULL,expires_at TEXT NOT NULL,lease_seconds INTEGER NOT NULL,note TEXT,revision INTEGER NOT NULL);
CREATE INDEX IF NOT EXISTS task_claims_expires ON task_claims(expires_at);
CREATE INDEX IF NOT EXISTS file_rel_path ON files(path COLLATE NOCASE,deleted);
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
INSERT OR IGNORE INTO search_rows(entity_type,entity_id,row_id) SELECT entity_type,entity_id,rowid FROM search WHERE entity_type IN('mapping','note');
INSERT OR REPLACE INTO meta VALUES('dev_control_schema','2');
INSERT OR REPLACE INTO meta VALUES('scale_schema','1');
PRAGMA user_version=6;
COMMIT;
)sql";
    std::string script=std::string(sql)+sql_tail;
    for(const auto* statement:{"ALTER TABLE inventory_tasks ADD COLUMN attempts INTEGER NOT NULL DEFAULT 0;","ALTER TABLE census_entries ADD COLUMN visit INTEGER NOT NULL DEFAULT 0;"}){const bool tasks=std::string(statement).find("inventory_tasks")!=std::string::npos;auto count=db.integer(tasks?"SELECT COUNT(*) FROM pragma_table_info('inventory_tasks') WHERE name='attempts'":"SELECT COUNT(*) FROM pragma_table_info('census_entries') WHERE name='visit'");if(count){auto pos=script.find(statement);script.erase(pos,std::string(statement).size());}}
    if(db.integer("SELECT COUNT(*) FROM pragma_table_info('files') WHERE name='seen_generation'")!=0){auto pos=script.find("ALTER TABLE files ADD COLUMN seen_generation INTEGER NOT NULL DEFAULT 0;");if(pos!=std::string::npos)script.erase(pos,std::string("ALTER TABLE files ADD COLUMN seen_generation INTEGER NOT NULL DEFAULT 0;").size());}
    char* error{};if(sqlite3_exec(db.native_handle(),script.c_str(),nullptr,nullptr,&error)!=SQLITE_OK){std::string msg=error?error:"scale schema";sqlite3_free(error);sqlite3_exec(db.native_handle(),"ROLLBACK",nullptr,nullptr,nullptr);throw std::runtime_error(msg);}
}

Json build_identity(){
    static const Json value=[](){Json out={{"version",MIGRATOOL_VERSION},{"build_id",MIGRATOOL_BUILD_ID},{"source_sha256",MIGRATOOL_SOURCE_SHA256},
        {"executable",self_path()},{"catalog_schema",6}};try{out["binary_sha256"]=hash_file(fs::path(wide(self_path())));}catch(...){out["binary_sha256"]=nullptr;}return out;}();return value;
}

bool scale_enabled(const Project& p,Database& db){
    if(p.config.value("inventory_engine","")=="durable")return true;
    return db.integer("SELECT EXISTS(SELECT 1 FROM inventory_jobs WHERE generation>0)")!=0||db.integer("SELECT EXISTS(SELECT 1 FROM files WHERE id>10000)")!=0;
}

Json scale_settings(const Project& original,const Json& args){
    auto p=Project::load(original.file);Database db(p);auto action=args.value("action","get");if(action=="get")return settings_view(p,db);
    if(action!="set")throw std::runtime_error("settings.action debe ser get o set");require_actor(args);
    if(!args.contains("expected_config_revision")||args["expected_config_revision"]!=p.revision)throw std::runtime_error("CONFIG_REVISION_CONFLICT");
    const auto patch=args.at("settings");if(!patch.is_object())throw std::runtime_error("settings debe ser objeto");
    const std::set<std::string> allowed={"name","roots","inventory_mode","inventory_engine","code_duplicate_mode","nested_zip_depth","code_extensions","code_filenames","exclude_dirs","builds","cases","resources","analyze_cpp","analyzers","defines","clang_args"};
    for(auto it=patch.begin();it!=patch.end();++it){if(!allowed.contains(it.key()))throw std::runtime_error("Ajuste no admitido: "+it.key());p.config[it.key()]=it.value();}
    auto resource=resource_profile(p);const auto memory=resource["memory_mib"].get<int>(),workers=resource["workers"].get<int>(),heavy=resource["analyzers"].get<int>();
    if(memory<128||memory>4096||workers<1||workers>8||heavy<1||heavy>2||heavy>workers)throw std::runtime_error("RESOURCE_RANGE: memoria 128..4096 MiB, trabajadores 1..8, analizadores 1..2");
    if(p.config.value("inventory_engine","auto")!="auto"&&p.config["inventory_engine"]!="durable")throw std::runtime_error("inventory_engine debe ser auto o durable");
    const auto nested_raw=p.config.value("nested_zip_depth",Json(2));
    if(!nested_raw.is_number_integer())throw std::runtime_error("NESTED_ZIP_DEPTH_INVALID");
    const int nested_depth=nested_raw.get<int>();
    if(nested_depth<0||nested_depth>4)throw std::runtime_error("NESTED_ZIP_DEPTH_RANGE: 0..4");
    validate(p.config);const Json warnings=preflight_warnings(p);
    auto journal=p.state_dir()/L"settings-pending.json";write_json(journal,{{"actor",args["actor"]},{"before",original.revision},{"after",sha256(p.config.dump())}});
    p.save();db.begin();try{db.change(args["actor"],"project_settings",{{"configuration",p.revision},{"settings",patch}});db.exec("INSERT INTO meta VALUES('dirty','1') ON CONFLICT(key)DO UPDATE SET value='1'");db.commit();}catch(...){db.rollback();throw;}
    std::error_code ec;fs::remove(journal,ec);if(scale_enabled(p,db))scale_spawn(p);auto out=settings_view(p,db);out["warnings"]=warnings;return out;
}

namespace {
void spawn_coordinator(const Project& p,bool watch_changes){
    std::wstring command=quote_arg(wide(worker_path()))+L" __coordinator --project "+quote_arg(p.file.wstring());
    if(!watch_changes)command+=L" --queue-only";
    STARTUPINFOW startup{sizeof(startup)};PROCESS_INFORMATION process{};
    if(!CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW|DETACHED_PROCESS,nullptr,p.directory().c_str(),&startup,&process))
        throw std::runtime_error("COORDINATOR_START: Win32 "+std::to_string(GetLastError()));
    CloseHandle(process.hThread);CloseHandle(process.hProcess);
}
}

void scale_spawn(const Project& p){spawn_coordinator(p,true);}

Json scale_start(const Project& p,const Json& args,bool launch){
    Database db(p);auto scope=args.value("scope",Json::object());if(!scope.is_object())throw std::runtime_error("scope debe ser objeto");
    if(db.integer("SELECT EXISTS(SELECT 1 FROM inventory_jobs WHERE generation>0)")==0)db.exec("INSERT OR IGNORE INTO search_rows SELECT entity_type,entity_id,rowid FROM search");
    const bool file_scope=scope.contains("file")||scope.contains("files");Json paths=scope.value("files",Json::array());
    if(scope.contains("files")&&(!paths.is_array()||paths.empty()||paths.size()>10000||scope.contains("file")||scope.contains("subdirectory")))throw std::runtime_error("SCOPE_FILES_RANGE");
    if(!scope.contains("files"))paths.push_back(scope.value("file",scope.value("subdirectory",".")));
    if(!scope.empty()){
        const auto root=scope.value("root","");bool found=false;for(const auto& r:p.config["roots"])if(r["id"]==root){found=true;for(auto& path:paths){
                auto sub=fs::path(wide(path)).lexically_normal();
                if(sub.is_absolute()||sub.has_root_path())throw std::runtime_error("SCOPE_INVALID");
                for(const auto& part:sub)if(part==L".."||part.native().find(L':')!=std::wstring::npos)throw std::runtime_error("SCOPE_INVALID");
                if(!sub.empty()&&!sub.has_filename())sub=sub.parent_path();
                if(sub.empty())sub=L".";
                if(file_scope&&sub==L".")throw std::runtime_error("SCOPE_FILE_REQUIRED");
                check_scope_ancestors(p.resolve(r["path"]),sub);
                path=path_text(sub);
            }}
        if(!found)throw std::runtime_error("SCOPE_ROOT_UNKNOWN");if(scope.contains("files"))scope["files"]=paths;else if(file_scope)scope["file"]=paths[0];else scope["subdirectory"]=paths[0];
    }
    const auto id=unique_id(),created=now();auto resources=resource_profile(p);resources["full"]=args.value("full",false);
    bool inherited_defer=false;
    if(args.value("actor",std::string())=="watcher"){
        inherited_defer=db.integer("SELECT COALESCE((SELECT j.phase='analysis_deferred' FROM inventory_generations g JOIN inventory_jobs j ON j.id=g.job_id WHERE g.state='published' ORDER BY g.id DESC LIMIT 1),0)")!=0;
    }
    resources["defer_analysis"]=args.value("defer_analysis",inherited_defer);db.begin();
    try{db.exec("INSERT INTO inventory_jobs(id,state,phase,scope,config_sha256,created,updated,resources,from_revision)VALUES(?,'queued','census',?,?,?,?,?,?)",{id,scope.dump(),sha256(p.config.dump()),created,created,resources.dump(),db.revision()});
        db.exec("INSERT INTO inventory_generations(job_id,state,created)VALUES(?,'staging',?)",{id,created});const auto gen=db.integer("SELECT last_insert_rowid()");db.exec("UPDATE inventory_jobs SET generation=? WHERE id=?",{gen,id});
        for(const auto& root:p.config["roots"]){if(!scope.empty()&&root["id"]!=scope["root"])continue;for(const auto& sub:paths){auto rel=sub=="."?"":sub.get<std::string>();
            db.exec("INSERT OR IGNORE INTO generation_scopes(generation,root,prefix)VALUES(?,?,?)",{gen,root["id"],rel.empty()?rel:rel+"/"});
            db.exec("INSERT OR IGNORE INTO inventory_dirs(job_id,root,path,absolute_path)VALUES(?,?,?,?)",{id,root["id"],rel,path_text(p.resolve(root["path"])/wide(rel))});
        }}
        // Queue creation and acknowledgement share the same transaction. A crash cannot lose a notification.
        if(scope.empty()&&!args.contains("event_receipts"))db.exec("DELETE FROM dirty_paths");
        else for(const auto& receipt:args.value("event_receipts",Json::array()))db.exec("DELETE FROM dirty_paths WHERE id=? AND at=?",{receipt["id"],receipt["at"]});
        db.change(args.value("actor","inventory"),"inventory_job_queued",{{"job_id",id},{"scope",scope}});db.commit();
    }catch(...){db.rollback();throw;}
    if(launch)spawn_coordinator(p,scope.empty());return {{"job_id",id},{"generation",db.integer("SELECT generation FROM inventory_jobs WHERE id=?",{id})},{"state","queued"},{"complete",false},{"revision",db.revision()}};
}
namespace {
Json read_jobs(Database& db,const Json& args){
    const auto id=args.value("id",args.value("job_id",""));
    const std::string select=R"sql(SELECT j.*,
 (SELECT COUNT(*) FROM inventory_dirs d WHERE d.job_id=j.id AND d.state IN('queued','running','failed')) AS pending_directories,
 (SELECT COUNT(*) FROM inventory_tasks t WHERE t.job_id=j.id AND t.state IN('queued','running','hashed')) AS pending_analysis,
 COALESCE(CAST(ROUND((julianday(COALESCE(j.finished,j.updated))-julianday(j.started))*86400000) AS INTEGER),0) AS elapsed_ms
 FROM inventory_jobs j)sql";
    auto rows=id.empty()?db.query(select+" ORDER BY j.created DESC,j.id DESC LIMIT ?",{std::clamp(args.value("limit",20),1,200)}):db.query(select+" WHERE j.id=?",{id});
    for(auto& row:rows)row=job_record(std::move(row));
    return {{"revision",db.revision()},{"items",rows},{"total",rows.size()}};
}
}

Json scale_jobs(const Project& p,const Json& args){
    Database db(p);
    return read_jobs(db,args);
}
Json scale_control(const Project& p,const Json& args){require_actor(args);Database db(p);auto id=args.value("id",args.value("job_id","")),action=args.value("action","");
    if(action!="pause"&&action!="resume"&&action!="cancel"&&action!="retry")throw std::runtime_error("JOB_ACTION_INVALID");
    bool watch_changes=true;
    db.begin();try{auto rows=db.query("SELECT state,phase,error,generation,scope FROM inventory_jobs WHERE id=?",{id});if(rows.empty())throw std::runtime_error("JOB_NOT_FOUND");const auto state=rows[0]["state"].get<std::string>();
        if(action=="resume"||action=="retry"){
            watch_changes=rows[0]["generation"]==0||safe_parse(rows[0]["scope"]).empty();
            if(state=="completed"&&rows[0]["phase"]!="analysis_deferred")throw std::runtime_error("JOB_ALREADY_FINISHED");
            if(state=="running")throw std::runtime_error("JOB_STILL_RUNNING");
            const bool begin_deferred_analysis=action=="resume"&&rows[0]["phase"]=="analysis_deferred";
            if(rows[0]["generation"]==0&&rows[0]["error"].get<std::string>().starts_with("EXPORT_CATALOG_CHANGED")){
                db.exec("UPDATE inventory_jobs SET phase='export',files=0,bytes=0,resources=json_remove(resources,'$.export_checkpoint') WHERE id=?",{id});
            }
            db.exec("UPDATE inventory_jobs SET state='queued',control='',error='',failed=0,updated=?,finished=NULL WHERE id=?",{now(),id});
            if(begin_deferred_analysis)db.exec("UPDATE inventory_jobs SET phase='analysis',resources=json_set(resources,'$.defer_analysis',json('false')) WHERE id=?",{id});
            db.exec("UPDATE inventory_tasks SET state='queued' WHERE job_id=? AND state IN('running','failed')",{id});
            db.exec("UPDATE inventory_jobs SET phase='census' WHERE id=? AND EXISTS(SELECT 1 FROM inventory_dirs WHERE job_id=? AND state='failed')",{id,id});
            db.exec("UPDATE inventory_dirs SET state='queued' WHERE job_id=? AND state='failed'",{id});
        }
        else if(!terminal(state))db.exec("UPDATE inventory_jobs SET control=?,state=CASE WHEN state IN('queued','paused') THEN ? ELSE state END,updated=? WHERE id=?",{action,action=="pause"?"paused":"cancelled",now(),id});
        db.change(args["actor"],"inventory_job_"+action,{{"job_id",id}});db.commit();
    }catch(...){db.rollback();throw;}
    if(action=="resume"||action=="retry")spawn_coordinator(p,watch_changes);return scale_jobs(p,{{"id",id}});
}

namespace {
struct Stopped {};
void check_control(Database& db,const std::string& id){
    auto control=scalar(db,"SELECT control FROM inventory_jobs WHERE id=?",{id});
    if(control=="pause"||control=="cancel")throw Stopped{};
}
std::string job_hash(Database& db,const std::string& id,const fs::path& path){std::ifstream file(native_path(path),std::ios::binary);if(!file)throw std::runtime_error("HASH_OPEN");Digest digest;std::array<char,256*1024> buffer{};unsigned chunks=0;while(file){file.read(buffer.data(),buffer.size());const auto bytes=file.gcount();if(bytes>0)digest.add(buffer.data(),static_cast<size_t>(bytes));if(++chunks%4==0)check_control(db,id);}if(!file.eof())throw std::runtime_error("HASH_READ");check_control(db,id);return digest.finish();}
uint64_t joined(DWORD high,DWORD low){return (static_cast<uint64_t>(high)<<32)|low;}
struct Entry {std::wstring name;uint64_t bytes{},modified{},created{},file_id{};DWORD attrs{};};
std::optional<Entry> stat_entry(const fs::path& path){
    WIN32_FILE_ATTRIBUTE_DATA data{};
    const auto physical=native_path(path);
    if(!GetFileAttributesExW(physical.c_str(),GetFileExInfoStandard,&data)){
        const auto code=GetLastError();
        if(missing_path(code))return std::nullopt;
        throw std::system_error(static_cast<int>(code),std::system_category(),"read entry metadata");
    }
    Handle handle{CreateFileW(physical.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr)};
    BY_HANDLE_FILE_INFORMATION identity{};
    uint64_t file_id=0;
    if(handle.h!=INVALID_HANDLE_VALUE&&GetFileInformationByHandle(handle,&identity))file_id=joined(identity.nFileIndexHigh,identity.nFileIndexLow);
    return Entry{path.filename().wstring(),joined(data.nFileSizeHigh,data.nFileSizeLow),
        joined(data.ftLastWriteTime.dwHighDateTime,data.ftLastWriteTime.dwLowDateTime),
        joined(data.ftCreationTime.dwHighDateTime,data.ftCreationTime.dwLowDateTime),file_id,data.dwFileAttributes};
}
void enumerate_directory(const fs::path& path,const std::function<void(const Entry&)>& visit){
    Handle dir{CreateFileW(native_path(path).c_str(),FILE_LIST_DIRECTORY,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr)};
    if(dir.h==INVALID_HANDLE_VALUE)throw std::system_error(static_cast<int>(GetLastError()),std::system_category(),"open directory");
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if(!GetFileInformationByHandleEx(dir,FileAttributeTagInfo,&attributes,sizeof(attributes)))
        throw std::system_error(static_cast<int>(GetLastError()),std::system_category(),"read directory attributes");
    if(!(attributes.FileAttributes&FILE_ATTRIBUTE_DIRECTORY)||(attributes.FileAttributes&FILE_ATTRIBUTE_REPARSE_POINT))
        throw std::runtime_error("DIRECTORY_CHANGED: "+path_text(path));
    alignas(FILE_ID_BOTH_DIR_INFO) std::array<unsigned char,64*1024> buffer{};bool first=true;
    for(;;){
        if(!GetFileInformationByHandleEx(dir,first?FileIdBothDirectoryRestartInfo:FileIdBothDirectoryInfo,buffer.data(),static_cast<DWORD>(buffer.size()))){
            auto error=GetLastError();if(error==ERROR_NO_MORE_FILES)return;
            if(first&&(error==ERROR_INVALID_PARAMETER||error==ERROR_NOT_SUPPORTED||error==ERROR_INVALID_FUNCTION))break;
            throw std::system_error(static_cast<int>(error),std::system_category(),"enumerate directory");
        }
        first=false;auto* info=reinterpret_cast<FILE_ID_BOTH_DIR_INFO*>(buffer.data());
        for(;;){std::wstring name(info->FileName,info->FileNameLength/sizeof(wchar_t));if(name!=L"."&&name!=L"..")visit({name,static_cast<uint64_t>(info->EndOfFile.QuadPart),static_cast<uint64_t>(info->LastWriteTime.QuadPart),static_cast<uint64_t>(info->CreationTime.QuadPart),static_cast<uint64_t>(info->FileId.QuadPart),info->FileAttributes});if(!info->NextEntryOffset)break;info=reinterpret_cast<FILE_ID_BOTH_DIR_INFO*>(reinterpret_cast<unsigned char*>(info)+info->NextEntryOffset);}
    }
    WIN32_FIND_DATAW data{};auto pattern=native_path(path)/L"*";HANDLE found=FindFirstFileExW(pattern.c_str(),FindExInfoBasic,&data,FindExSearchNameMatch,nullptr,FIND_FIRST_EX_LARGE_FETCH);
    if(found==INVALID_HANDLE_VALUE){auto error=GetLastError();if(error==ERROR_FILE_NOT_FOUND)return;throw std::system_error(static_cast<int>(error),std::system_category(),"find directory");}
    struct FindCloser{HANDLE h;~FindCloser(){FindClose(h);}} closer{found};
    do{std::wstring name(data.cFileName);if(name!=L"."&&name!=L"..")visit({name,joined(data.nFileSizeHigh,data.nFileSizeLow),joined(data.ftLastWriteTime.dwHighDateTime,data.ftLastWriteTime.dwLowDateTime),joined(data.ftCreationTime.dwHighDateTime,data.ftCreationTime.dwLowDateTime),0,data.dwFileAttributes});}while(FindNextFileW(found,&data));
    if(GetLastError()!=ERROR_NO_MORE_FILES)throw std::system_error(static_cast<int>(GetLastError()),std::system_category(),"find next");
}
bool is_build_config_path(const fs::path& path){
    const auto ext=lower(path_text(path.extension()));
    if(ext==".csproj"||ext==".props"||ext==".targets"||ext==".sln"||ext==".slnx")return true;
    const auto name=lower(path_text(path.filename()));
    return name=="directory.build.props"||name=="directory.build.targets"||name=="compile_commands.json"||name=="cmakelists.txt";
}
Json analyzer_tool_hashes(const Project& p){
    Json out={{"binary",build_identity().value("binary_sha256",build_identity().value("source_sha256",Json()))}};
    std::error_code ec;
    // AUD-036: Hash executable_dir()/libclang.dll first (matching analyzers.cpp load order!) before compile-time MIGRATOOL_CLANG_DLL
    fs::path clang_dll = executable_dir() / L"libclang.dll";
    if (!fs::exists(native_path(clang_dll), ec)) clang_dll = fs::path(MIGRATOOL_CLANG_DLL);
    if (fs::exists(native_path(clang_dll), ec)) out["clang"] = hash_file(clang_dll);
    else out["clang"] = "";
    const auto helper_dir=(executable_dir()/L"adapters/roslyn");
    Json roslyn_parts=Json::array();
    for(const auto* rel_name:{"RoslynAdapter.dll","RoslynAdapter.deps.json","RoslynAdapter.runtimeconfig.json"}){
        const auto fp=helper_dir/wide(rel_name);
        if(fs::exists(native_path(fp),ec))roslyn_parts.push_back(Json::array({rel_name,hash_file(fp)}));
    }
    out["roslyn"]=roslyn_parts.empty()?std::string():sha256(roslyn_parts.dump());
    Json custom=Json::object();
    const auto analyzers=p.config.value("analyzers",Json::array());
    auto hash_tokens=[&](const Json& list,Json& info){
        if(!list.is_array())return;
        for(const auto& item:list){
            if(!item.is_string())continue;
            auto cand=p.resolve(item.get<std::string>());
            if(fs::exists(cand,ec)&&fs::is_regular_file(cand,ec))info[item.get<std::string>()]=hash_file(cand);
        }
    };
    Json by_ext=Json::object();
    if(analyzers.is_array()){
        for(size_t idx=0;idx<analyzers.size();++idx){
            const auto& item=analyzers[idx];
            if(!item.is_object())continue;
            const auto id=item.value("id",std::string("idx_")+std::to_string(idx));
            Json info={{"spec",item}};
            if(item.contains("argv"))hash_tokens(item["argv"],info);
            else if(item.contains("command"))hash_tokens(item["command"],info);
            const auto digest=sha256(info.dump());
            custom[id]=digest;
            if(item.contains("extensions")&&item["extensions"].is_array()){
                for(const auto& ex:item["extensions"]){
                    if(!ex.is_string())continue;
                    const auto low_ext=lower(ex.get<std::string>());
                    if(!low_ext.empty()){
                        if(!by_ext.contains(low_ext))by_ext[low_ext]=Json::array();
                        by_ext[low_ext].push_back(Json::array({id,digest}));
                    }
                }
            }
        }
    }else if(analyzers.is_object()){
        for(auto it=analyzers.begin();it!=analyzers.end();++it){
            Json info={{"spec",it.value()}};
            hash_tokens(it.value(),info);
            custom[it.key()]=sha256(info.dump());
        }
    }
    out["custom"]=custom;
    out["custom_exts"]=by_ext;
    return out;
}
Json lang_config_hashes(const Project& p){
    std::error_code ec;
    Json cc_ext=Json::array(),cs_ancestors=Json::array();
    for(const auto& r:p.config.value("roots",Json::array())){
        if(r.contains("compile_commands")&&r["compile_commands"].is_string()){
            const auto rel=r["compile_commands"].get<std::string>();
            const auto cp=p.resolve(rel);
            cc_ext.push_back(Json::array({r.value("id",""),rel,(fs::exists(cp,ec)&&fs::is_regular_file(cp,ec))?hash_file(cp):std::string()}));
        }
        auto cur=p.resolve(r["path"]).parent_path();
        for(int depth=0;depth<8&&!cur.empty();++depth){
            for(const auto* fname:{"Directory.Build.props","Directory.Build.targets"}){
                const auto f=cur/wide(fname);
                if(fs::exists(f,ec)&&fs::is_regular_file(f,ec))cs_ancestors.push_back(Json::array({path_text(f),hash_file(f)}));
            }
            if(cur==cur.parent_path())break;
            cur=cur.parent_path();
        }
    }
    return {{"cpp",sha256(Json{{"analyze_cpp",p.config.value("analyze_cpp",true)},{"defines",p.config.value("defines",Json::array())},{"clang_args",p.config.value("clang_args",Json::array())},{"compile_commands",cc_ext}}.dump())},
            {"cs",sha256(Json{{"defines",p.config.value("defines",Json::array())},{"ancestor_props",cs_ancestors}}.dump())},
            {"inventory_mode",p.config.value("inventory_mode","universal")}};
}
void queue_transitive_dependents(Database& db,const std::string& jid,const std::vector<int64_t>& seeds,bool queue_seeds){
    if(seeds.empty())return;
    Statement add(db,"INSERT OR IGNORE INTO inventory_tasks(job_id,file_id,phase)VALUES(?,?,'content')");
    Statement dep_q(db,"SELECT DISTINCT d.file_id FROM dependencies d JOIN files f ON f.id=d.file_id WHERE d.target_id=? AND f.deleted=0");
    std::unordered_set<int64_t> seen;
    std::vector<int64_t> queue;
    for(auto s:seeds){
        if(s<=0)continue;
        if(queue_seeds){
            if(db.integer("SELECT EXISTS(SELECT 1 FROM files WHERE id=? AND deleted=0)",{s}))add.exec({jid,s});
        }
        if(seen.insert(s).second)queue.push_back(s);
    }
    for(size_t i=0;i<queue.size();++i){
        dep_q.bind({queue[i]});
        while(dep_q.next()){
            const auto fid=dep_q.row()["file_id"].get<int64_t>();
            if(seen.insert(fid).second){add.exec({jid,fid});queue.push_back(fid);}
        }
        dep_q.reset();
    }
}
void queue_build_config_dependents(Database& db,const std::string& jid,const std::string& root,const std::string& rel_path,const fs::path& abs){
    const auto ext=lower(path_text(abs.extension())),fname=lower(path_text(abs.filename()));
    const auto folder=fs::path(wide(rel_path)).parent_path();
    const std::string prefix=(folder.empty()||folder==L".")?std::string():path_text(folder)+"/";
    const std::string abs_s=path_text(abs);
    std::string prefix_end=prefix.empty()?std::string():prefix+"\xF4\x8F\xBF\xBF";
    std::vector<int64_t> seeds;
    if(fname=="compile_commands.json"||fname=="cmakelists.txt"){
        for(const auto& r:db.query("SELECT id FROM files WHERE deleted=0 AND relevant=1 AND language IN('C','C++') AND ((root=? AND (?='' OR (path>=? COLLATE NOCASE AND path<? COLLATE NOCASE))) OR lower(COALESCE(json_extract(data,'$.compile_commands_path'),''))=lower(?))",{root,prefix,prefix,prefix_end,abs_s}))
            seeds.push_back(r["id"].get<int64_t>());
    }else if(ext==".csproj"||ext==".props"||ext==".targets"||ext==".sln"||ext==".slnx"){
        for(const auto& r:db.query("SELECT id FROM files WHERE deleted=0 AND relevant=1 AND language='C#' AND ((root=? AND (?='' OR (path>=? COLLATE NOCASE AND path<? COLLATE NOCASE))) OR instr(lower(COALESCE(json_extract(data,'$.project_config_files'),'')),lower(?))>0)",{root,prefix,prefix,prefix_end,abs_s}))
            seeds.push_back(r["id"].get<int64_t>());
    }
    queue_transitive_dependents(db,jid,seeds,true);
}
void queue_unresolved_includers(Database& db,const std::string& jid,const std::string& rel_path,const fs::path& abs){
    std::string norm=rel_path;std::replace(norm.begin(),norm.end(),'\\','/');
    std::vector<std::string> candidates;
    const auto fname=path_text(abs.filename());
    if(!fname.empty())candidates.push_back(fname);
    if(!norm.empty()&&norm!=fname)candidates.push_back(norm);
    size_t pos=norm.find('/');
    for(int hops=0;pos!=std::string::npos&&hops<5;++hops,pos=norm.find('/',pos+1)){
        auto tail=norm.substr(pos+1);
        if(!tail.empty()&&std::find(candidates.begin(),candidates.end(),tail)==candidates.end())candidates.push_back(tail);
    }
    Statement q(db,"SELECT DISTINCT file_id FROM dependencies INDEXED BY dependency_name WHERE name=? AND target_id IS NULL");
    std::vector<int64_t> seeds;
    for(const auto& cand:candidates){
        q.bind({cand});
        while(q.next())seeds.push_back(q.row()["file_id"].get<int64_t>());
        q.reset();
    }
    queue_transitive_dependents(db,jid,seeds,true);
}
void census(const Project& p,Database& db,const Json& job){
    const auto jid=job["id"].get<std::string>();const auto gen=job["generation"].get<int64_t>();
    Statement add_entry(db,"INSERT OR IGNORE INTO census_entries(generation,root,path,parent,name,absolute_path,kind,bytes,mtime,attrs,link_target,file_id,visit)VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)");
    Statement revisit_entry(db,"UPDATE census_entries SET kind=?,bytes=?,mtime=?,attrs=?,link_target=?,file_id=?,visit=? WHERE generation=? AND root=? AND path=?");
    Statement add_dir(db,"INSERT OR IGNORE INTO inventory_dirs(job_id,root,path,absolute_path)VALUES(?,?,?,?)");
    Statement lookup(db,"SELECT id,winid,bytes,mtime,deleted,sha256,data,relevant,analysis,category,language FROM files WHERE absolute_path=?");
    Statement by_identity(db,"SELECT id,absolute_path FROM files INDEXED BY file_identity WHERE winid=? LIMIT 2");
    Statement insert_file(db,"INSERT INTO files(root,path,absolute_path,winid,bytes,mtime,category,language,relevant,git_state,build_state,analysis,first_revision,last_revision,data,seen_generation)VALUES(?,?,?,?,?,?,?,?,?,'UNKNOWN','UNKNOWN','PENDING',?,?,?,?)");
    Statement update_file(db,"UPDATE files SET root=?,path=?,absolute_path=?,winid=?,bytes=?,mtime=?,deleted=0,seen_generation=?,sha256=CASE WHEN bytes=? AND mtime=? THEN sha256 ELSE NULL END,analysis=CASE WHEN bytes=? AND mtime=? THEN analysis ELSE 'PENDING' END,last_revision=? WHERE id=?");
    Statement seen_file(db,"UPDATE files SET seen_generation=? WHERE id=?");
    Statement add_task(db,"INSERT OR IGNORE INTO inventory_tasks(job_id,file_id,phase)VALUES(?,?,?)");
    std::map<std::string,Json> roots;std::map<std::string,fs::path> root_paths;std::map<std::string,DWORD> serials;
    for(const auto& root:p.config["roots"]){const auto rid=root["id"].get<std::string>();roots[rid]=root;root_paths[rid]=p.resolve(root["path"]);DWORD serial{};std::wstring volume(32768,L'\0');if(GetVolumePathNameW(root_paths[rid].c_str(),volume.data(),static_cast<DWORD>(volume.size())))GetVolumeInformationW(volume.c_str(),nullptr,0,&serial,nullptr,nullptr,nullptr,0);serials[rid]=serial;Handle handle{CreateFileW(root_paths[rid].c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr)};BY_HANDLE_FILE_INFORMATION identity{};if(handle.h!=INVALID_HANDLE_VALUE&&GetFileInformationByHandle(handle,&identity))db.exec("INSERT INTO directory_ids VALUES(?,?,?,?)ON CONFLICT(root,volume_serial,frn)DO UPDATE SET path=excluded.path",{rid,serial,static_cast<int64_t>(joined(identity.nFileIndexHigh,identity.nFileIndexLow)),""});}
    const auto scope=safe_parse(job["scope"]),resources=safe_parse(job["resources"]);
    std::set<std::string> seed_paths;
    if(scope.contains("files"))for(const auto& path:scope["files"])seed_paths.insert(path.get<std::string>());
    else if(scope.contains("file"))seed_paths.insert(scope["file"].get<std::string>());
    else if(scope.contains("subdirectory")&&scope["subdirectory"]!=".")seed_paths.insert(scope["subdirectory"].get<std::string>());
    const bool source_only=p.config.value("inventory_mode","universal")=="source_only";
    std::set<std::string> excluded_dirs;
    for(const auto& x:p.config.value("exclude_dirs",Json::array()))if(x.is_string())excluded_dirs.insert(lower(x.get<std::string>()));
    {
        const auto cur_tools=analyzer_tool_hashes(p),cur_cfg=lang_config_hashes(p);
        const auto prev_tools_s=scalar(db,"SELECT value FROM meta WHERE key='last_tool_hashes'");
        const auto prev_cfg_s=scalar(db,"SELECT value FROM meta WHERE key='last_lang_config'");
        if(!prev_tools_s.empty()&&!prev_cfg_s.empty()){
            const auto prev_tools=safe_parse(prev_tools_s),prev_cfg=safe_parse(prev_cfg_s);
            const bool bin_changed=cur_tools.value("binary",Json())!=prev_tools.value("binary",Json())||cur_cfg.value("inventory_mode","")!=prev_cfg.value("inventory_mode","");
            const bool cpp_changed=bin_changed||cur_tools.value("clang","")!=prev_tools.value("clang","")||cur_cfg.value("cpp","")!=prev_cfg.value("cpp","");
            const bool cs_changed=bin_changed||cur_tools.value("roslyn","")!=prev_tools.value("roslyn","")||cur_cfg.value("cs","")!=prev_cfg.value("cs","");
            std::string scope_sql;
            Json scope_params=Json::array();
            if(!scope.empty()){
                scope_sql=" AND root=?";
                scope_params.push_back(scope["root"]);
                if(!seed_paths.empty()){
                    scope_sql+=" AND (";
                    bool first_seed=true;
                    for(const auto& seed:seed_paths){
                        if(!first_seed)scope_sql+=" OR ";
                        first_seed=false;
                        const auto pfx=seed+"/";
                        scope_sql+="(path=? COLLATE NOCASE OR (path>=? COLLATE NOCASE AND path<? COLLATE NOCASE))";
                        scope_params.push_back(seed);
                        scope_params.push_back(pfx);
                        scope_params.push_back(pfx+"\xF4\x8F\xBF\xBF");
                    }
                    scope_sql+=")";
                }
            }
            auto append_scope=[&](Json base_params){
                for(const auto& sp:scope_params)base_params.push_back(sp);
                return base_params;
            };
            db.begin();
            try{
                if(bin_changed)db.exec("INSERT OR IGNORE INTO inventory_tasks(job_id,file_id,phase)SELECT ?,id,'content' FROM files WHERE deleted=0 AND relevant=1"+scope_sql,append_scope({jid}));
                else{
                    if(cpp_changed)db.exec("INSERT OR IGNORE INTO inventory_tasks(job_id,file_id,phase)SELECT ?,id,'content' FROM files WHERE deleted=0 AND relevant=1 AND language IN('C','C++')"+scope_sql,append_scope({jid}));
                    if(cs_changed)db.exec("INSERT OR IGNORE INTO inventory_tasks(job_id,file_id,phase)SELECT ?,id,'content' FROM files WHERE deleted=0 AND relevant=1 AND language='C#'"+scope_sql,append_scope({jid}));
                    const auto prev_custom=prev_tools.value("custom",Json::object()),cur_custom=cur_tools.value("custom",Json::object());
                    std::set<std::string> changed_langs;
                    for(auto it=cur_custom.begin();it!=cur_custom.end();++it)if(prev_custom.value(it.key(),Json())!=it.value())changed_langs.insert(it.key());
                    for(auto it=prev_custom.begin();it!=prev_custom.end();++it)if(!cur_custom.contains(it.key()))changed_langs.insert(it.key());
                    for(const auto& lang_name:changed_langs)db.exec("INSERT OR IGNORE INTO inventory_tasks(job_id,file_id,phase)SELECT ?,id,'content' FROM files WHERE deleted=0 AND relevant=1 AND language=?"+scope_sql,append_scope({jid,lang_name}));
                    const auto prev_exts=prev_tools.value("custom_exts",Json::object()),cur_exts=cur_tools.value("custom_exts",Json::object());
                    std::set<std::string> changed_exts;
                    for(auto it=cur_exts.begin();it!=cur_exts.end();++it)if(prev_exts.value(it.key(),Json())!=it.value())changed_exts.insert(it.key());
                    for(auto it=prev_exts.begin();it!=prev_exts.end();++it)if(!cur_exts.contains(it.key()))changed_exts.insert(it.key());
                    for(const auto& ext_name:changed_exts)if(!ext_name.empty())db.exec("INSERT OR IGNORE INTO inventory_tasks(job_id,file_id,phase)SELECT ?,id,'content' FROM files WHERE deleted=0 AND relevant=1 AND substr(lower(path),?)=?"+scope_sql,append_scope({jid,-static_cast<int64_t>(ext_name.size()),ext_name}));
                }
                db.commit();
            }catch(...){db.rollback();throw;}
        }
    }
    uint64_t count=0;int64_t added_files=0,added_dirs=0,added_links=0,added_bytes=0;
    auto flush=[&](){
        // A SELECT left at SQLITE_ROW can keep its WAL snapshot after COMMIT.
        lookup.reset();by_identity.reset();
        db.exec("UPDATE inventory_jobs SET files=files+?,dirs=dirs+?,links=links+?,bytes=bytes+?,updated=? WHERE id=?",{added_files,added_dirs,added_links,added_bytes,now(),jid});
        db.commit();added_files=added_dirs=added_links=added_bytes=0;check_control(db,jid);db.begin();
    };
    db.begin();auto batch_started=GetTickCount64();
    for(;;){check_control(db,jid);auto next=db.query("SELECT root,path,absolute_path FROM inventory_dirs INDEXED BY dirs_queue WHERE job_id=? AND state='queued' ORDER BY root,path LIMIT 1",{jid});if(next.empty())break;
        const auto node=next[0];const auto root=node["root"].get<std::string>();const auto rel=node["path"].get<std::string>();const auto base=root_paths.at(root);const auto location=fs::path(wide(node["absolute_path"]));const auto serial=serials.at(root);
        db.exec("UPDATE meta SET value=CAST(value AS INTEGER)+1 WHERE key='visit_sequence'");const auto visit=db.integer("SELECT CAST(value AS INTEGER) FROM meta WHERE key='visit_sequence'");
        try{auto visitor=[&](const Entry& e,const fs::path& folder,const std::string& parent_rel,bool queue_directory){
            const auto absolute=folder/e.name;const auto path=path_text(absolute.lexically_relative(base));const auto abs=path_text(absolute);const bool reparse=(e.attrs&FILE_ATTRIBUTE_REPARSE_POINT)!=0,isdir=(e.attrs&FILE_ATTRIBUTE_DIRECTORY)!=0;
            const auto kind=reparse?(isdir?"directory_link":"file_link"):(isdir?"directory":"file");Json target=nullptr,file_id=nullptr;
            if(reparse){std::error_code ec;auto link=fs::read_symlink(native_path(absolute),ec);if(!ec)target=path_text(link);else target="UNRESOLVED: "+ec.message();}
            if(!isdir&&!reparse){
                auto old=lookup.first({abs});int64_t id=old.is_null()?0:old["id"].get<int64_t>();
                auto identity=e.file_id?std::to_string(serial)+":"+std::to_string(e.file_id)+":"+std::to_string(e.created):std::string();
                if(!id&&!identity.empty()){
                    by_identity.bind({identity});
                    if(by_identity.next()){
                        const auto previous=by_identity.row();
                        if(!by_identity.next()){
                            const auto former=native_path(fs::path(wide(previous["absolute_path"])));
                            if(GetFileAttributesW(former.c_str())==INVALID_FILE_ATTRIBUTES&&missing_path(GetLastError()))id=previous["id"];
                        }
                    }
                    by_identity.reset();
                }
                // Name-only classification avoids opening binary/data files during the census.
                auto cl=classify_inventory_name(p,absolute,base,roots.at(root).value("include_all_code",false));
                bool in_excluded=false;
                for(const auto& part:fs::path(wide(path)).parent_path())if(excluded_dirs.contains(lower(utf8(part.wstring())))){in_excluded=true;break;}
                if(in_excluded)cl["in_excluded_dir"]=true;
                const auto old_data=old.is_null()?Json::object():safe_parse(old["data"]);
                const bool filtered=source_only&&!cl.value("source_code",false);
                if(filtered)cl["exclusion_reason"]="filtered_by_inventory_mode";
                else if(cl.value("blocked_code",false))cl["exclusion_reason"]="protected_configuration";
                else if(in_excluded)cl["exclusion_reason"]="exclude_dirs";
                const bool was_filtered=old_data.value("filtered_by_inventory_mode",false);
                const bool forced=((scope.contains("file")||scope.contains("files"))&&seed_paths.contains(path))||resources.value("full",false);
                const bool changed=old.is_null()||(old["deleted"]==1&&!was_filtered)||old["bytes"]!=e.bytes||old["mtime"]!=e.modified||forced||(was_filtered&&!filtered);
                if(!id){insert_file.exec({root,path,abs,identity,e.bytes,e.modified,cl["category"],cl["language"],cl["relevant"],db.revision(),db.revision(),cl.dump(),gen});id=sqlite3_last_insert_rowid(db.native_handle());}
                else if(changed)update_file.exec({root,path,abs,identity,e.bytes,e.modified,gen,e.bytes,e.modified,e.bytes,e.modified,db.revision(),id});
                else seen_file.exec({gen,id});
                file_id=id;
                if(filtered){
                    if(!old.is_null()&&!was_filtered&&old["deleted"]==0)db.change("inventory","filtered_out",{{"id","F"+std::to_string(id)},{"path",abs}},"F"+std::to_string(id));
                    db.exec("UPDATE files SET deleted=1,data=json_set(data,'$.filtered_by_inventory_mode',json('true'),'$.present',json('true'),'$.text_content','') WHERE id=?",{id});
                    for(const auto& row:db.query("SELECT row_id FROM search_rows WHERE entity_type='file' AND entity_id=?",{"F"+std::to_string(id)}))db.exec("DELETE FROM search WHERE rowid=? AND entity_type='file' AND entity_id=?",{row["row_id"],"F"+std::to_string(id)});
                }else if(was_filtered)db.exec("UPDATE files SET deleted=0,data=json_set(data,'$.filtered_by_inventory_mode',json('false'),'$.present',json('true')) WHERE id=?",{id});
                const auto category=old.is_null()?cl["category"]:old["category"];
                const bool code=old.is_null()?cl.value("source_code",false):(old["relevant"]==1||cl.value("source_code",false));
                const bool is_cfg=is_build_config_path(absolute);
                if(!filtered&&is_cfg&&(changed||old.is_null()||old["sha256"].is_null()))add_task.exec({jid,id,"content"});
                else if(filtered&&is_cfg&&(changed||old.is_null()||old["sha256"].is_null())){try{db.exec("UPDATE files SET sha256=? WHERE id=?",{hash_file(native_path(absolute)),id});}catch(...){}}
                if((code||(!source_only&&category=="documentation")||(category=="unknown"&&absolute.extension().empty()))&&(changed||old.is_null()||old["sha256"].is_null()||old["analysis"]!="ANALYZED"))add_task.exec({jid,id,"content"});
                if(changed){
                    if(!old.is_null())queue_transitive_dependents(db,jid,{id},false);
                    if(is_cfg)queue_build_config_dependents(db,jid,root,path,absolute);
                    else queue_unresolved_includers(db,jid,path,absolute);
                }
                if(lower(path_text(absolute.extension()))==".zip"&&p.config.value("code_duplicate_mode","off")=="files_and_zip")add_task.exec({jid,id,"archive"});
            }
            add_entry.exec({gen,root,path,parent_rel=="."?"":parent_rel,utf8(e.name),abs,kind,e.bytes,e.modified,e.attrs,target,file_id,visit});
            if(sqlite3_changes(db.native_handle())){if(reparse)++added_links;else if(isdir)++added_dirs;else{++added_files;added_bytes+=e.bytes;}}
            else revisit_entry.exec({kind,e.bytes,e.modified,e.attrs,target,file_id,visit,gen,root,path});
            if(isdir&&e.file_id)db.exec("INSERT INTO directory_ids VALUES(?,?,?,?)ON CONFLICT(root,volume_serial,frn)DO UPDATE SET path=excluded.path",{root,serial,static_cast<int64_t>(e.file_id),path});
            if(queue_directory&&isdir&&!reparse)add_dir.exec({jid,root,path,abs});
            if(++count%500==0||GetTickCount64()-batch_started>=100){flush();batch_started=GetTickCount64();}
        };
            if(seed_paths.contains(rel))check_scope_ancestors(base,fs::path(wide(rel)));
            const auto attributes=GetFileAttributesW(native_path(location).c_str());
            const auto attribute_error=GetLastError();
            if(attributes==INVALID_FILE_ATTRIBUTES&&(!missing_path(attribute_error)||rel.empty()||
                GetFileAttributesW(native_path(base).c_str())==INVALID_FILE_ATTRIBUTES))
                throw std::system_error(static_cast<int>(attribute_error),std::system_category(),"read directory metadata");
            const bool directory=attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_DIRECTORY)&&!(attributes&FILE_ATTRIBUTE_REPARSE_POINT);
            if(!rel.empty()&&(seed_paths.contains(rel)||!directory)){
                const auto entry=stat_entry(location);
                if(entry)visitor(*entry,location.parent_path(),path_text(fs::path(wide(rel)).parent_path()),false);
                else {
                    db.exec("UPDATE files SET seen_generation=0 WHERE seen_generation=? AND absolute_path=?",{gen,path_text(location)});
                    db.exec("DELETE FROM census_entries WHERE generation=? AND root=? AND path=?",{gen,root,rel});
                }
            }
            if(directory)enumerate_directory(location,[&](const Entry& entry){visitor(entry,location,rel,true);});
            const auto obsolete=directory?std::string("parent=? AND visit<>?"):std::string("path>=? AND path<?");
            const auto prefix=rel+"/";
            const Json obsolete_params=directory?Json::array({gen,root,rel,visit}):Json::array({gen,root,prefix,prefix+"\xF4\x8F\xBF\xBF"});
            auto seen_params=obsolete_params;seen_params.push_back(gen);
            db.exec("UPDATE files SET seen_generation=0 WHERE id IN(SELECT file_id FROM census_entries WHERE generation=? AND root=? AND "+obsolete+
                ") AND NOT EXISTS(SELECT 1 FROM census_entries current WHERE current.file_id=files.id AND current.generation=? AND current.absolute_path=files.absolute_path)",seen_params);
            db.exec("DELETE FROM census_entries WHERE generation=? AND root=? AND "+obsolete,obsolete_params);
            if(!directory&&!rel.empty())db.exec("UPDATE inventory_dirs SET state='done' WHERE job_id=? AND root=? AND path>=? AND path<? AND state='queued'",{jid,root,prefix,prefix+"\xF4\x8F\xBF\xBF"});
            db.exec("UPDATE inventory_dirs SET state='done',error='' WHERE job_id=? AND root=? AND path=?",{jid,root,rel});if(GetTickCount64()-batch_started>=100){flush();batch_started=GetTickCount64();}
        }catch(const Stopped&){db.rollback();throw;}catch(const std::exception& error){lookup.reset();by_identity.reset();db.rollback();db.begin();const auto* system=dynamic_cast<const std::system_error*>(&error);add_error(db,jid,root,path_text(location),"enumerate",system?system->code().value():0,error.what());db.exec("UPDATE inventory_dirs SET state='failed',error=? WHERE job_id=? AND root=? AND path=?",{error.what(),jid,root,rel});db.commit();added_files=added_dirs=added_links=added_bytes=0;db.begin();batch_started=GetTickCount64();}
    }
    flush();db.rollback();
    db.begin();try{
        const bool complete=db.integer("SELECT COUNT(*) FROM inventory_dirs WHERE job_id=? AND state<>'done'",{jid})==0;
        auto totals=db.query("SELECT COALESCE(SUM(kind='file'),0) AS files,COALESCE(SUM(kind='directory'),0) AS dirs,COALESCE(SUM(kind IN('file_link','directory_link')),0) AS links,COALESCE(SUM(CASE WHEN kind='file' THEN bytes ELSE 0 END),0) AS bytes FROM census_entries WHERE generation=?",{gen})[0];
        db.exec("UPDATE inventory_jobs SET files=?,dirs=?,links=?,bytes=? WHERE id=?",{totals["files"],totals["dirs"],totals["links"],totals["bytes"],jid});
        db.exec("UPDATE inventory_jobs SET census_complete=?,phase='analysis',updated=? WHERE id=?",{complete,now(),jid});
        db.exec("UPDATE inventory_generations SET state=?,published=? WHERE id=?",{complete?"published":"partial",now(),gen});
        if(complete){
            for(const auto& [rid,root]:roots){if(!scope.empty()&&scope["root"]!=rid)continue;
                std::string where="root=? AND seen_generation<>? AND (deleted=0 OR COALESCE(json_extract(data,'$.present'),0)=1)";Json params={rid,gen};
                const auto mark_deleted=[&](const std::string& filter,const Json& values){
                    auto doomed=db.query("SELECT id,root,path,absolute_path FROM files WHERE "+filter,values);
                    std::vector<int64_t> deleted_ids;deleted_ids.reserve(doomed.size());
                    for(const auto& d:doomed)deleted_ids.push_back(d["id"].get<int64_t>());
                    db.exec("UPDATE files SET deleted=1,data=json_set(data,'$.present',json('false')) WHERE "+filter,values);
                    queue_transitive_dependents(db,jid,deleted_ids,false);
                    for(const auto& d:doomed){
                        auto abs=fs::path(wide(d["absolute_path"]));
                        if(is_build_config_path(abs))queue_build_config_dependents(db,jid,d["root"],d["path"],abs);
                    }
                };
                if(seed_paths.empty())mark_deleted(where,params);
                else for(const auto& seed:seed_paths){
                    auto exact=params;exact.push_back(seed);
                    mark_deleted(where+" AND path=? COLLATE NOCASE",exact);
                    auto descendants=params;const auto prefix=seed+"/";
                    descendants.push_back(prefix);descendants.push_back(prefix+"\xF4\x8F\xBF\xBF");
                    mark_deleted(where+" AND path>=? COLLATE NOCASE AND path<? COLLATE NOCASE",descendants);
                }
                if(scope.empty()){
                    Json rr={{"id",rid},{"role",root["role"]},{"path",path_text(p.resolve(root["path"]))},{"complete",true},{"diagnostics",Json::array()},{"exclusions",Json::array()}};
                    db.exec("INSERT INTO roots(id,role,path,complete,data)VALUES(?,?,?,1,?) ON CONFLICT(id)DO UPDATE SET role=excluded.role,path=excluded.path,complete=1,data=excluded.data",{rid,root["role"],rr["path"],rr.dump()});
                }
            }
            db.exec("INSERT INTO meta VALUES('published_generation',?)ON CONFLICT(key)DO UPDATE SET value=excluded.value",{std::to_string(gen)});
            if(scope.empty()){
                const int retain=std::max(1,p.config.value("retain_census_generations",3));
                auto old_full=db.query(
                    "SELECT g.id FROM inventory_generations g "
                    "WHERE g.state='published' AND EXISTS(SELECT 1 FROM generation_scopes s WHERE s.generation=g.id AND s.prefix='') "
                    "ORDER BY g.id DESC LIMIT -1 OFFSET ?",{retain});
                if(!old_full.empty()){
                    const int64_t cutoff=old_full[0]["id"].get<int64_t>();
                    db.exec("DELETE FROM census_entries WHERE generation<=?",{cutoff});
                }
            }
        }
        db.change("inventory","census_published",{{"job_id",jid},{"generation",gen},{"complete",complete}});db.commit();
    }catch(...){db.rollback();throw;}
}

void store_analysis(Database& db,const Json& analyzed){
    Statement deactivate(db,"UPDATE symbols SET active=0 WHERE file_id=?");
    Statement clear_dependencies(db,"DELETE FROM dependencies WHERE file_id=?");
    Statement sym(db,"INSERT INTO symbols(file_id,stable_key,name,qualified_name,kind,line,data,active)VALUES(?,?,?,?,?,?,?,1) ON CONFLICT(file_id,stable_key)DO UPDATE SET name=excluded.name,qualified_name=excluded.qualified_name,kind=excluded.kind,line=excluded.line,data=excluded.data,active=1");
    for(const auto& f:analyzed["files"]){auto id=std::stoll(f["id"].get<std::string>().substr(1));
        db.exec("DELETE FROM search WHERE rowid IN(SELECT x.row_id FROM search_rows x JOIN symbols s ON x.entity_type='symbol' AND x.entity_id='S'||s.id WHERE s.file_id=?)",{id});db.exec("DELETE FROM search_rows WHERE entity_type='symbol' AND entity_id IN(SELECT 'S'||id FROM symbols WHERE file_id=?)",{id});deactivate.exec({id});clear_dependencies.exec({id});
        auto rows=db.query("SELECT data FROM files WHERE id=?",{id});
        Json fdata=rows.empty()?Json::object():safe_parse(rows[0]["data"]);
        fdata["analysis"]=f.value("analysis","PARTIAL");
        for(const auto* k:{"analysis_config","compile_flags_sha256","compile_commands_path","project_config_files","context_config_sha256","context_files_sha256","analysis_error"}){
            if(f.contains(k))fdata[k]=f[k];else fdata.erase(k);
        }
        db.exec("UPDATE files SET analysis=?,data=? WHERE id=?",{f.value("analysis","PARTIAL"),fdata.dump(),id});}
    std::map<std::string,std::string> real_ids;
    for(const auto& s:analyzed["symbols"]){auto id=std::stoll(s["file_id"].get<std::string>().substr(1));sym.exec({id,s["id"],s["name"],s.value("qualified_name",s["name"].get<std::string>()),s["kind"],s.value("line",1),s.dump()});auto key="S"+std::to_string(db.integer("SELECT id FROM symbols WHERE file_id=? AND stable_key=?",{id,s["id"]}));real_ids[s["id"]]=key;
        db.exec("INSERT INTO search(entity_type,entity_id,body)VALUES('symbol',?,?)",{key,s.value("qualified_name",s["name"].get<std::string>())+" "+s.value("signature",std::string())});db.exec("INSERT INTO search_rows VALUES('symbol',?,?)",{key,sqlite3_last_insert_rowid(db.native_handle())});}
    for(auto dep:analyzed["dependencies"]){auto id=std::stoll(dep["file_id"].get<std::string>().substr(1));Json target=nullptr;
        auto caller=dep.value("caller_symbol_id",std::string());if(real_ids.contains(caller))dep["caller_symbol_id"]=real_ids[caller];
        const auto resolution = dep.value("resolution", std::string());
        if(resolution!="DYNAMIC_OR_REFLECTION"&&resolution!="CANDIDATES_AMBIGUOUS"&&resolution!="XAML_DECLARED"){
            const auto key=dep.value("target_key","");Json matches;
            if(!key.empty())matches=db.query("SELECT id,file_id FROM symbols WHERE active=1 AND json_extract(data,'$.semantic_key')=? LIMIT 2",{key});
            if(matches.size()==1){target=matches[0]["file_id"];dep["target_symbol_id"]="S"+std::to_string(matches[0]["id"].get<int64_t>());dep["resolution"]="SEMANTIC_EXACT";dep["resolved"]=true;}
            else {matches=db.query("SELECT MIN(id) AS id,file_id FROM symbols WHERE active=1 AND (name=? OR qualified_name=?) GROUP BY file_id LIMIT 2",{dep.value("name",""),dep.value("target_qualified_name",dep.value("name",""))});if(matches.size()==1){target=matches[0]["file_id"];dep["resolution"]="SYNTACTIC_UNIQUE";dep["resolved"]=true;}}
        }
        if(target.is_null()&&(dep.value("kind","")=="include"||dep.value("kind","")=="import")){
            const auto tpath=dep.value("target_path",std::string());
            if(!tpath.empty()){
                const auto exact=db.query("SELECT id FROM files WHERE absolute_path=? AND deleted=0 LIMIT 1",{path_text(fs::path(wide(tpath)).lexically_normal())});
                if(!exact.empty()){target=exact[0]["id"];dep["resolution"]="EXACT_INCLUDE_PATH";dep["resolved"]=true;}
            }
            if(target.is_null()){
                auto name=dep.value("name","");auto source=db.query("SELECT absolute_path,root FROM files WHERE id=?",{id});
                if(!source.empty()){
                    auto local=fs::path(wide(source[0]["absolute_path"])).parent_path()/wide(name);
                    auto match=db.query("SELECT id FROM files WHERE absolute_path=? AND deleted=0 LIMIT 1",{path_text(local.lexically_normal())});
                    if(match.empty())match=db.query("SELECT id FROM files WHERE root=? AND path=? AND deleted=0 LIMIT 2",{source[0]["root"],name});
                    if(match.empty()){
                        std::string norm=name;std::replace(norm.begin(),norm.end(),'\\','/');
                        if(!norm.empty())match=db.query("SELECT id FROM files WHERE root=? AND deleted=0 AND (path=? COLLATE NOCASE OR path LIKE '%/'||? COLLATE NOCASE) LIMIT 2",{source[0]["root"],norm,norm});
                    }
                    if(match.size()==1){target=match[0]["id"];dep["resolution"]="EXACT_INCLUDE_PATH";dep["resolved"]=true;}
                }
            }
        }
        db.exec("INSERT INTO dependencies(file_id,target_id,kind,name,data)VALUES(?,?,?,?,?)",{id,target,dep.value("kind","reference"),dep.value("name",""),dep.dump()});
    }
}
void content(const Project& p,Database& db,const Json& job){
    const auto jid=job["id"].get<std::string>();
    const auto config_hash=sha256(p.config.dump());
    const int max_zip_depth=std::clamp(p.config.value("nested_zip_depth",2),0,4);
    const auto zip_cfg=sha256(Json::array({config_hash,max_zip_depth}).dump());
    for(;;){check_control(db,jid);auto tasks=db.query("SELECT t.file_id,t.phase,t.attempts,f.* FROM inventory_tasks t JOIN files f ON t.file_id=f.id WHERE t.job_id=? AND t.state='queued' ORDER BY t.file_id LIMIT 16",{jid});if(tasks.empty())break;
        for(auto row:tasks){check_control(db,jid);const auto id=row["file_id"].get<int64_t>();const auto path=fs::path(wide(row["absolute_path"]));
            try{
                if(row["deleted"]==1){db.exec("UPDATE inventory_tasks SET state='done' WHERE job_id=? AND file_id=?",{jid,id});continue;}
                if(row["phase"]=="archive"){
                    const bool forced=safe_parse(job["resources"]).value("full",false)||safe_parse(job["scope"]).contains("file")||safe_parse(job["scope"]).contains("files");
                    const auto archive_data=safe_parse(row["data"]);
                    if(!forced&&!row["sha256"].is_null()&&archive_data.value("zip_scan_sha256",Json())==row["sha256"]&&archive_data.value("zip_scan_config","")==zip_cfg&&archive_data.value("zip_scan_state","")=="complete"){
                        db.exec("UPDATE inventory_tasks SET state='done' WHERE job_id=? AND file_id=?",{jid,id});
                        db.exec("UPDATE inventory_jobs SET processed=processed+1,updated=? WHERE id=?",{now(),jid});continue;
                    }
                    const auto original_digest=job_hash(db,jid,path);
                    const bool can_resume=!forced&&archive_data.value("zip_scan_sha256",Json())==original_digest&&archive_data.value("zip_scan_config","")==zip_cfg&&(archive_data.value("zip_scan_state","")=="paused"||archive_data.value("zip_scan_state","")=="partial");
                    int64_t start_index=0,start_nested_index=-10;
                    bool archive_errors=false;
                    if(can_resume){
                        start_index=std::max<int64_t>(archive_data.value("zip_next_index",int64_t(0)),db.integer("SELECT COALESCE(MAX(CASE WHEN entry_index>=0 THEN entry_index END),-1)+1 FROM archive_members WHERE archive_id=?",{id}));
                        start_nested_index=std::min<int64_t>(archive_data.value("zip_next_nested_index",int64_t(-10)),db.integer("SELECT COALESCE(MIN(CASE WHEN entry_index<=-10 THEN entry_index END),-9)-1 FROM archive_members WHERE archive_id=?",{id}));
                        archive_errors=db.integer("SELECT EXISTS(SELECT 1 FROM archive_members WHERE archive_id=? AND state<>'hashed')",{id})!=0;
                    }else{
                        db.exec("DELETE FROM archive_members WHERE archive_id=?",{id});
                    }
                    Json entries=Json::array();
                    int64_t next_index=start_index,next_nested_index=start_nested_index;
                    uint64_t progress_ticks=0;
                    auto flush=[&](const std::string& scan_state){
                        if(entries.empty()&&scan_state=="paused"&&next_index==start_index)return;
                        db.begin();try{
                            if(!entries.empty()){
                                Statement save(db,"INSERT INTO archive_members(archive_id,entry_index,entry_path,bytes,sha256,state,error)VALUES(?,?,?,?,?,?,?) ON CONFLICT(archive_id,entry_index)DO UPDATE SET entry_path=excluded.entry_path,bytes=excluded.bytes,sha256=excluded.sha256,state=excluded.state,error=excluded.error");
                                for(const auto& e:entries)save.exec({id,e.value("entry_index",int64_t(-1)),e.value("entry_path",std::string()),e.value("bytes",uint64_t(0)),e.value("sha256",std::string()),e.contains("error")?"failed":"hashed",e.value("error",std::string())});
                            }
                            db.exec("UPDATE files SET sha256=?,data=json_set(data,'$.zip_scan_sha256',?,'$.zip_scan_config',?,'$.zip_scan_state',?,'$.zip_next_index',?,'$.zip_next_nested_index',?) WHERE id=?",
                                {original_digest,original_digest,zip_cfg,scan_state,next_index,next_nested_index,id});
                            db.commit();entries=Json::array();
                        }catch(...){db.rollback();throw;}
                    };
                    Cancel archive_cancel;std::jthread monitor([&](std::stop_token stop){try{Database reader(p);while(!stop.stop_requested()){auto value=scalar(reader,"SELECT control FROM inventory_jobs WHERE id=?",{jid});if(value=="pause"||value=="cancel"){archive_cancel.requested=true;break;}for(int i=0;i<5&&!stop.stop_requested();++i)Sleep(20);}}catch(...){archive_cancel.requested=true;}});
                    std::string archive_error;
                    ZipScanOptions zip_options;
                    zip_options.start_index=start_index;
                    zip_options.start_nested_index=start_nested_index;
                    zip_options.max_depth=max_zip_depth;
                    zip_options.on_progress=[&](int64_t ni,int64_t nni){
                        next_index=ni;next_nested_index=nni;
                        if(entries.size()>=128||(++progress_ticks%256==0&&!entries.empty()))flush("paused");
                        if(progress_ticks%128==0)check_control(db,jid);
                    };
                    try{
                        zip_code_entries(path,[&](const fs::path& member,std::optional<std::string_view> prefix){return probe_inventory_code(p,member,prefix);},&archive_cancel,
                            [&](const Json& item,bool error){archive_errors|=error;entries.push_back(item);},zip_options);
                    }catch(const Stopped&){
                        monitor.request_stop();monitor.join();
                        flush("paused");
                        throw;
                    }catch(const std::exception& error){archive_error=error.what();}
                    monitor.request_stop();monitor.join();
                    if(archive_cancel.requested||scalar(db,"SELECT control FROM inventory_jobs WHERE id=?",{jid})=="pause"||scalar(db,"SELECT control FROM inventory_jobs WHERE id=?",{jid})=="cancel"){
                        flush("paused");
                        check_control(db,jid);
                    }
                    if(job_hash(db,jid,path)!=original_digest){
                        db.exec("DELETE FROM archive_members WHERE archive_id=?",{id});
                        throw std::runtime_error("INPUT_CHANGED");
                    }
                    if(!archive_error.empty()){
                        archive_errors=true;
                        entries.push_back({{"entry_index",-1},{"entry_path",""},{"error",archive_error}});
                    }
                    flush(archive_errors?"partial":"complete");
                    db.begin();try{
                        db.exec("UPDATE inventory_tasks SET state=?,error=? WHERE job_id=? AND file_id=?",{archive_errors?"failed":"done",archive_errors?"ARCHIVE_PARTIAL":"",jid,id});
                        db.exec("UPDATE inventory_jobs SET processed=processed+1,updated=? WHERE id=?",{now(),jid});
                        if(archive_errors)add_error(db,jid,row["root"],row["absolute_path"],"archive",0,archive_error.empty()?"ARCHIVE_PARTIAL":archive_error);
                        db.commit();
                    }catch(...){db.rollback();throw;}
                    continue;
                }
                Json root;for(const auto& r:p.config["roots"])if(r["id"]==row["root"])root=r;
                if(root.is_null())throw std::runtime_error("ROOT_REMOVED");
                WIN32_FILE_ATTRIBUTE_DATA before{},after{};if(!GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&before))throw std::runtime_error("INPUT_MISSING");
                auto cl=classify_inventory_name(p,path,p.resolve(root["path"]),root.value("include_all_code",false));
                const auto cl_full=classify_inventory_file(p,path,p.resolve(root["path"]),root.value("include_all_code",false));
                const auto digest=job_hash(db,jid,path);
                if(!GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&after)||joined(before.ftLastWriteTime.dwHighDateTime,before.ftLastWriteTime.dwLowDateTime)!=joined(after.ftLastWriteTime.dwHighDateTime,after.ftLastWriteTime.dwLowDateTime)||joined(before.nFileSizeHigh,before.nFileSizeLow)!=joined(after.nFileSizeHigh,after.nFileSizeLow))throw std::runtime_error("INPUT_CHANGED");
                Json data=Json::object();
                db.begin();try{
                    auto fresh_rows=db.query("SELECT sha256,data FROM files WHERE id=?",{id});
                    const Json old_sha=fresh_rows.empty()?Json():fresh_rows[0]["sha256"];
                    data=fresh_rows.empty()?safe_parse(row["data"]):safe_parse(fresh_rows[0]["data"]);
                    for(auto it=cl_full.begin();it!=cl_full.end();++it)data[it.key()]=it.value();
                    data["present"]=true;
                    data["id"]="F"+std::to_string(id);data["root"]=row["root"];data["path"]=row["path"];data["absolute_path"]=row["absolute_path"];data["sha256"]=digest;data["bytes"]=joined(after.nFileSizeHigh,after.nFileSizeLow);data["mtime"]=joined(after.ftLastWriteTime.dwHighDateTime,after.ftLastWriteTime.dwLowDateTime);
                    // Manual classifications are authoritative over automatic detection.
                    auto manual=db.query("SELECT detail FROM history WHERE entity=? AND action='classification' ORDER BY revision DESC LIMIT 1",{data["id"]});
                    if(!manual.empty()&&!cl_full.value("blocked_code",false)){auto m=safe_parse(manual[0]["detail"]);auto over=m.value("after",Json::object());for(const auto* k:{"category","language","relevant"})if(over.contains(k))data[k]=over[k];if(over.value("relevant",false))data["source_code"]=true;}
                    const bool filtered=p.config.value("inventory_mode","universal")=="source_only"&&!data.value("source_code",false);data["filtered_by_inventory_mode"]=filtered;
                    if(!data.value("relevant",false)&&data["category"]!="resource"&&data["category"]!="documentation"&&!is_build_config_path(path))data["analysis_note"]="NO_DEEP_ANALYZER";
                    db.exec("UPDATE files SET sha256=?,bytes=?,mtime=?,category=?,language=?,relevant=?,data=?,deleted=? WHERE id=?",{digest,data["bytes"],data["mtime"],data["category"],data["language"],data["relevant"],data.dump(),filtered?1:0,id});
                    for(const auto& previous:db.query("SELECT row_id FROM search_rows WHERE entity_type='file' AND entity_id=?",{data["id"]}))db.exec("DELETE FROM search WHERE rowid=? AND entity_type='file' AND entity_id=?",{previous["row_id"],data["id"]});
                    db.exec("DELETE FROM search_rows WHERE entity_type='file' AND entity_id=?",{data["id"]});
                    db.exec("INSERT INTO search(entity_type,entity_id,body)VALUES('file',?,?)",{data["id"],row["absolute_path"].get<std::string>()+" "+cl_full.value("text_content","")});
                    db.exec("INSERT INTO search_rows VALUES('file',?,?)",{data["id"],sqlite3_last_insert_rowid(db.native_handle())});
                    if(!old_sha.is_null()&&old_sha!=digest)queue_transitive_dependents(db,jid,{id},false);
                    db.exec("UPDATE inventory_tasks SET state='hashed' WHERE job_id=? AND file_id=?",{jid,id});db.commit();
                }catch(...){db.rollback();throw;}
                if(!data.value("relevant",false)&&data["category"]!="resource"){db.exec("UPDATE files SET analysis='NOT_APPLICABLE' WHERE id=?",{id});db.exec("UPDATE inventory_tasks SET state='done' WHERE job_id=? AND file_id=?",{jid,id});db.exec("UPDATE inventory_jobs SET processed=processed+1,updated=? WHERE id=?",{now(),jid});}
            }catch(const Stopped&){throw;}
            catch(const std::exception& error){const bool retry=std::string(error.what())=="INPUT_CHANGED"&&row["attempts"].get<int>()<3;db.exec("UPDATE inventory_tasks SET state=?,attempts=attempts+1,error=? WHERE job_id=? AND file_id=?",{retry?"queued":"failed",error.what(),jid,id});if(!retry)add_error(db,jid,row["root"],row["absolute_path"],"content",0,error.what());}
        }
    }
    scale_fingerprints(db);
    const auto tools=analyzer_tool_hashes(p),lcfg=lang_config_hashes(p);
    const int analyzer_slots=std::clamp(resource_profile(p)["analyzers"].get<int>(),1,2);
    const auto all_cc_cfg=db.query("SELECT root,path,COALESCE(sha256,'') AS sha256 FROM files WHERE deleted=0 AND (lower(path)='compile_commands.json' OR lower(path) LIKE '%/compile_commands.json' OR lower(path)='cmakelists.txt' OR lower(path) LIKE '%/cmakelists.txt') ORDER BY root,path");
    const auto all_cs_cfg=db.query("SELECT root,path,COALESCE(sha256,'') AS sha256 FROM files WHERE deleted=0 AND (lower(path) LIKE '%.csproj' OR lower(path) LIKE '%.props' OR lower(path) LIKE '%.targets' OR lower(path) LIKE '%.sln' OR lower(path) LIKE '%.slnx') ORDER BY root,path");
    Statement cs_members_q(db,"SELECT path FROM files INDEXED BY roslyn_source_paths WHERE root=? AND deleted=0 AND substr(lower(path),-3)='.cs' ORDER BY path");
    Statement dep_q(db,"SELECT DISTINCT d.target_id,COALESCE(f.sha256,'') AS sha256,COALESCE(f.deleted,1) AS deleted FROM dependencies d LEFT JOIN files f ON f.id=d.target_id WHERE d.file_id=? AND d.target_id IS NOT NULL");
    auto unit_tool_hash=[&](const std::string& lang,const std::string& rel_path){
        const auto low_lang=lower(lang);
        const auto ext=lower(path_text(fs::path(wide(rel_path)).extension()));
        Json parts=Json::array({tools["binary"]});
        if(low_lang=="c"||low_lang=="c++")parts.push_back(tools["clang"]);
        else if(low_lang=="c#")parts.push_back(tools["roslyn"]);
        if(tools["custom"].contains(lang))parts.push_back(tools["custom"][lang]);
        if(!ext.empty()&&tools.contains("custom_exts")&&tools["custom_exts"].contains(ext))parts.push_back(tools["custom_exts"][ext]);
        return sha256(parts.dump());
    };
    auto unit_config_hash=[&](const std::string& root,const std::string& rel_path,const std::string& lang,const Json&){
        const auto low_lang=lower(lang);
        if(low_lang=="c"||low_lang=="c++"){
            Json cc=Json::array();
            for(const auto& r:all_cc_cfg)if(r["root"]==root)cc.push_back(Json::array({r["path"],r["sha256"]}));
            return sha256(Json::array({lcfg["cpp"],cc}).dump());
        }
        if(low_lang=="c#"){
            Json cfgs=Json::array();
            std::string rel_dir=path_text(fs::path(wide(rel_path)).parent_path());
            if(rel_dir==".")rel_dir.clear();
            const auto low_rel_dir=lower(rel_dir);
            std::string best_proj_dir;
            for(const auto& r:all_cs_cfg){
                const auto cpath=r["path"].get<std::string>();
                cfgs.push_back(Json::array({r["root"],cpath,r["sha256"]}));
                if(r["root"]==root){
                    auto cdir=path_text(fs::path(wide(cpath)).parent_path());if(cdir==".")cdir.clear();
                    const auto low_cdir=lower(cdir);
                    if((low_cdir.empty()||low_rel_dir==low_cdir||(low_rel_dir.size()>low_cdir.size()&&low_rel_dir.starts_with(low_cdir+"/")))&&lower(cpath).ends_with(".csproj")&&cdir.size()>=best_proj_dir.size())best_proj_dir=low_cdir;
                }
            }
            Json members=Json::array();
            cs_members_q.bind({root});
            while(cs_members_q.next()){
                const auto mp=cs_members_q.row()["path"].get<std::string>();
                if(best_proj_dir.empty()||lower(mp).starts_with(best_proj_dir+"/"))members.push_back(mp);
            }
            cs_members_q.reset();
            return sha256(Json::array({lcfg["cs"],cfgs,members}).dump());
        }
        const auto ext=lower(path_text(fs::path(wide(rel_path)).extension()));
        return sha256(Json::array({lcfg["inventory_mode"],tools["custom"].value(lang,Json()),(!ext.empty()&&tools.contains("custom_exts"))?tools["custom_exts"].value(ext,Json()):Json()}).dump());
    };
    auto deps_closure_hash=[&](int64_t fid){
        std::unordered_set<int64_t> seen{fid};
        std::vector<int64_t> frontier{fid};
        std::vector<std::tuple<int64_t,int64_t,std::string>> closure;
        for(size_t i=0;i<frontier.size()&&i<4096;++i){
            dep_q.bind({frontier[i]});
            while(dep_q.next()){
                const auto& r=dep_q.row();
                const auto tid=r["target_id"].get<int64_t>();
                if(seen.insert(tid).second){
                    closure.emplace_back(tid,r["deleted"].get<int64_t>(),r["sha256"].get<std::string>());
                    frontier.push_back(tid);
                }
            }
            dep_q.reset();
        }
        std::sort(closure.begin(),closure.end());
        Json arr=Json::array();
        for(const auto& [tid,del,hash]:closure)arr.push_back(Json::array({tid,del,hash}));
        return sha256(arr.dump());
    };
    for(;;){check_control(db,jid);auto tasks=db.query("SELECT t.file_id,t.attempts,f.* FROM inventory_tasks t JOIN files f ON f.id=t.file_id WHERE t.job_id=? AND t.state='hashed' ORDER BY t.file_id LIMIT 16",{jid});if(tasks.empty())break;
        Json input={{"files",Json::array()},{"symbols",Json::array()},{"dependencies",Json::array()},{"diagnostics",Json::array()},{"roots",p.config["roots"]},{"indexed_context",true}};
        std::map<int64_t,std::pair<std::string,std::string>> batch_hashes;
        for(const auto& row:tasks){const auto id=row["file_id"].get<int64_t>();auto data=safe_parse(row["data"]);
            const auto prev_cache_key=data.value("analysis_cache_key",std::string());
            const auto lang=row["language"].get<std::string>();
            const auto rel_path=row["path"].get<std::string>();
            const auto t_hash=unit_tool_hash(lang,rel_path);
            const auto c_hash=unit_config_hash(row["root"].get<std::string>(),rel_path,lang,data);
            const auto d_hash=deps_closure_hash(id);
            data["analysis_tool_sha256"]=t_hash;
            data["analysis_cache_key"]=sha256(Json::array({data["id"],row["sha256"],t_hash,c_hash,d_hash}).dump());
            batch_hashes[id]={t_hash,c_hash};
            const bool cached=row["analysis"]=="ANALYZED"&&prev_cache_key==data["analysis_cache_key"].get<std::string>()&&db.integer("SELECT EXISTS(SELECT 1 FROM analysis_cache WHERE cache_key=? AND file_id=? AND sha256=?)",{data["analysis_cache_key"],id,row["sha256"]});
            if(cached){db.exec("UPDATE inventory_tasks SET state='done' WHERE job_id=? AND file_id=?",{jid,id});db.exec("UPDATE inventory_jobs SET processed=processed+1,resources=json_set(resources,'$.analysis_cache_hits',COALESCE(json_extract(resources,'$.analysis_cache_hits'),0)+1) WHERE id=?",{jid});continue;}
            if(row["bytes"].get<uint64_t>()>32ULL*1024*1024){db.exec("UPDATE files SET analysis='PARTIAL' WHERE id=?",{id});db.exec("UPDATE inventory_tasks SET state='failed',error='RESOURCE_FILE_LIMIT' WHERE job_id=? AND file_id=?",{jid,id});add_error(db,jid,row["root"],row["absolute_path"],"analysis",0,"RESOURCE_FILE_LIMIT: 32 MiB");continue;}
            input["files"].push_back(data);db.exec("UPDATE inventory_tasks SET state='running' WHERE job_id=? AND file_id=?",{jid,id});
        }
        if(input["files"].empty())continue;
        // AUD-017: Always wrap work directory paths with native_path() for MAX_PATH (>260 char) resilience!
        const auto work=p.state_dir()/L"work"/wide(jid)/wide(unique_id());fs::create_directories(native_path(work));
        struct WorkCleanup { fs::path dir; ~WorkCleanup(){ std::error_code ec; fs::remove_all(native_path(dir), ec); } } work_guard{work};
        auto in=work/L"request.json",out=work/L"analysis.json";write_json(in,input);
        Cancel cancel;std::jthread observer([&](std::stop_token stop){try{Database reader(p);while(!stop.stop_requested()){auto control=scalar(reader,"SELECT control FROM inventory_jobs WHERE id=?",{jid});if(control=="pause"||control=="cancel"){cancel.requested=true;break;}for(int i=0;i<5&&!stop.stop_requested();++i)Sleep(20);}}catch(...){cancel.requested=true;}});
        Json analyzed;std::string failure;
        try{
            GlobalSlot slot(L"Analyzer",analyzer_slots,[&]{check_control(db,jid);});
            const auto proc=run_process({worker_path(),"__analyze","--project",path_text(p.file),"--input",path_text(in),"--out",path_text(out)},p.directory(),"",work/L"process",600000,&cancel);
            if(proc["exit_code"]!=0||!fs::exists(native_path(out))||fs::file_size(native_path(out))>64*1024*1024)throw std::runtime_error("ANALYZER_FAILED_OR_LIMITED");
            analyzed=read_json(out);
        }catch(const std::exception& error){failure=error.what();}
        observer.request_stop();observer.join();
        if(cancel.requested){db.exec("UPDATE inventory_tasks SET state='queued' WHERE job_id=? AND state='running'",{jid});throw Stopped{};}
        std::set<std::string> valid_ids;
        if(failure.empty())for(const auto& f:input["files"]){try{if(job_hash(db,jid,fs::path(wide(f["absolute_path"])))==f["sha256"].get<std::string>())valid_ids.insert(f["id"]);}catch(const std::exception&){}}
        if(failure.empty()){for(const auto* kind:{"files","symbols","dependencies"}){auto& items=analyzed[kind];items.erase(std::remove_if(items.begin(),items.end(),[&](const Json& item){return !valid_ids.contains(item[kind==std::string("files")?"id":"file_id"].get<std::string>());}),items.end());}}
        std::map<std::string,std::string> analysis_errors;
        if(failure.empty())for(const auto& file:analyzed["files"])if(file.contains("analysis_error"))analysis_errors[file["id"]]=file["analysis_error"];
        db.begin();try{
            if(failure.empty())store_analysis(db,analyzed);
            for(const auto& f:input["files"]){const auto entity=f["id"].get<std::string>();auto id=std::stoll(entity.substr(1));const bool valid=failure.empty()&&valid_ids.contains(entity);
                if(valid&&analysis_errors.contains(entity)){
                    const auto& error=analysis_errors.at(entity);
                    db.exec("UPDATE files SET analysis='PARTIAL' WHERE id=?",{id});
                    db.exec("UPDATE inventory_tasks SET state='failed',error=? WHERE job_id=? AND file_id=?",{error,jid,id});
                    add_error(db,jid,f["root"],f["absolute_path"],"analysis",0,error);
                }
                else if(valid){const auto& [t_hash,c_hash]=batch_hashes.at(id);
                    const auto final_d_hash=deps_closure_hash(id);
                    const auto final_cache_key=sha256(Json::array({entity,f["sha256"],t_hash,c_hash,final_d_hash}).dump());
                    db.exec("UPDATE inventory_tasks SET state='done',error='' WHERE job_id=? AND file_id=?",{jid,id});
                    db.exec("INSERT INTO analysis_cache(cache_key,file_id,sha256,tool,at)VALUES(?,?,?,?,?)ON CONFLICT(cache_key)DO UPDATE SET at=excluded.at",{final_cache_key,id,f["sha256"],t_hash,now()});
                    db.exec("UPDATE files SET data=json_set(data,'$.analysis_cache_key',?,'$.analysis_tool_sha256',?) WHERE id=?",{final_cache_key,t_hash,id});}
                else {auto attempts=db.integer("SELECT attempts FROM inventory_tasks WHERE job_id=? AND file_id=?",{jid,id});const bool retry=failure.empty()&&attempts<3;db.exec("UPDATE inventory_tasks SET state=?,attempts=attempts+1,error=? WHERE job_id=? AND file_id=?",{retry?"queued":"failed",failure.empty()?"INPUT_CHANGED":failure,jid,id});db.exec("UPDATE files SET analysis='PARTIAL' WHERE id=?",{id});if(!retry)add_error(db,jid,f["root"],f["absolute_path"],"analysis",0,failure.empty()?"INPUT_CHANGED":failure);}
                if(valid||!failure.empty())db.exec("UPDATE inventory_jobs SET processed=processed+1,updated=? WHERE id=?",{now(),jid});
            }
            db.change("inventory","analysis_batch",{{"job_id",jid},{"files",input["files"].size()}});db.commit();
        }catch(...){db.rollback();throw;}
    }
    if(safe_parse(scalar(db,"SELECT scope FROM inventory_jobs WHERE id=?",{jid})).empty())
        db.exec("INSERT INTO meta VALUES('last_tool_hashes',?),('last_lang_config',?) ON CONFLICT(key)DO UPDATE SET value=excluded.value",{tools.dump(),lcfg.dump()});
    if(db.integer("SELECT EXISTS(SELECT 1 FROM inventory_tasks WHERE job_id=? AND state='queued')",{jid}))throw std::runtime_error("INPUT_CHANGED_REQUEUED");
}
}

int scale_analyzer(const Project& p,const fs::path& input,const fs::path& output){
    auto batch=read_json(input);Cancel cancel;auto analyzed=analyze(p,std::move(batch),{},&cancel);
    for(auto& symbol:analyzed["symbols"]){if(!symbol.contains("offset")||!symbol.contains("end_offset"))continue;const auto start=symbol["offset"].get<uint64_t>(),end=symbol["end_offset"].get<uint64_t>();if(end<start)continue;
        std::ifstream file(fs::path(wide(symbol["absolute_path"])),std::ios::binary);file.seekg(static_cast<std::streamoff>(start));Digest hash;std::array<char,65536> bytes{};auto left=end-start;
        while(left&&file){auto n=static_cast<std::streamsize>(std::min<uint64_t>(bytes.size(),left));file.read(bytes.data(),n);const auto got=file.gcount();if(got<=0)break;hash.add(bytes.data(),static_cast<size_t>(got));left-=static_cast<uint64_t>(got);}if(!left)symbol["symbol_sha256"]=hash.finish();
    }
    write_json(output,analyzed);return 0;
}

int scale_coordinator(const Project& original,bool once){
    return scale_coordinator(original,once,true);
}

int scale_coordinator(const Project& original,bool once,bool watch_changes){
    Handle leader{CreateMutexW(nullptr,FALSE,coordinator_name(original).c_str())};const auto acquired=WaitForSingleObject(leader,0);if(acquired!=WAIT_OBJECT_0&&acquired!=WAIT_ABANDONED)return 0;
    struct LeaderUnlock{HANDLE h;~LeaderUnlock(){ReleaseMutex(h);}} unlock{leader.h};
    const auto join_budget=[](HANDLE job,SIZE_T bytes,std::string& error){
        if(!job){error="CreateJobObject: "+std::to_string(GetLastError());return false;}
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_JOB_MEMORY;
        limits.JobMemoryLimit=bytes;
        if(!SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof(limits))){error="SetInformationJobObject: "+std::to_string(GetLastError());return false;}
        BOOL assigned=FALSE;
        if(IsProcessInJob(GetCurrentProcess(),job,&assigned)&&assigned)return true;
        if(!AssignProcessToJobObject(job,GetCurrentProcess())){error="AssignProcessToJobObject: "+std::to_string(GetLastError());return false;}
        return true;
    };
    std::string shared_error,project_error;
    Handle budget{CreateJobObjectW(nullptr,L"Local\\MigratoolSharedMemoryBudget")};
    const bool enforced=join_budget(budget.h,4ULL*1024*1024*1024,shared_error);
    const auto profile=resource_profile(original);
    SharedBudgetLedger ledger(original);
    const auto initial_ledger=ledger.sync(static_cast<DWORD>(profile["memory_mib"].get<int>()),static_cast<DWORD>(profile["workers"].get<int>()),static_cast<DWORD>(profile["analyzers"].get<int>()));
    Handle project_budget{CreateJobObjectW(nullptr,nullptr)};
    bool project_enforced=join_budget(project_budget.h,static_cast<SIZE_T>(std::max<DWORD>(128,initial_ledger["project_allocated_mib"].get<DWORD>()))*1024*1024,project_error);
    Database db(original);db.exec("INSERT INTO meta(key,value)VALUES('coordinator_memory_enforced',?)ON CONFLICT(key)DO UPDATE SET value=excluded.value",{enforced?"1":"0"});
    db.exec("INSERT INTO meta(key,value)VALUES('project_memory_enforced',?)ON CONFLICT(key)DO UPDATE SET value=excluded.value",{project_enforced?"1":"0"});
    db.exec("INSERT INTO meta VALUES('coordinator_memory_error',?),('project_memory_error',?),('shared_memory_status',?) ON CONFLICT(key) DO UPDATE SET value=excluded.value",{shared_error,project_error,initial_ledger.dump()});
    db.exec("UPDATE inventory_jobs SET state='queued',pid=NULL WHERE state='running'");db.exec("UPDATE inventory_tasks SET state='queued' WHERE state='running'");
    for(;;){
        auto watcher=!once&&watch_changes?std::make_unique<IndexWatcher>(original,900000,Log{},false):nullptr;
        auto idle=GetTickCount64();
        for(;;){auto rows=db.query("SELECT * FROM inventory_jobs WHERE state='queued' AND control='' ORDER BY created LIMIT 1");if(rows.empty()){if(once||GetTickCount64()-idle>10000)break;Sleep(100);continue;}idle=GetTickCount64();const auto job=rows[0];const auto id=job["id"].get<std::string>();
            try{
                auto p=Project::load(original.file);
                const auto scope=safe_parse(job["scope"]);
                const bool exporting=job["generation"]==0&&scope.value("kind",std::string())=="export_inventory";
                if(!exporting&&sha256(p.config.dump())!=job["config_sha256"].get<std::string>())throw std::runtime_error("CONFIG_CHANGED: retry with a new inventory job");
                // A full request may reuse the coordinator created for an explicit scope.
                if(!once&&!watcher&&!exporting&&scope.empty()){
                    watch_changes=true;
                    watcher=std::make_unique<IndexWatcher>(p,900000,Log{},false);
                }
                const auto job_profile=resource_profile(p);
                const auto active_ledger=ledger.sync(static_cast<DWORD>(job_profile["memory_mib"].get<int>()),static_cast<DWORD>(job_profile["workers"].get<int>()),static_cast<DWORD>(job_profile["analyzers"].get<int>()));
                const DWORD alloc_mib=active_ledger["project_allocated_mib"].get<DWORD>();
                project_enforced=join_budget(project_budget.h,static_cast<SIZE_T>(std::max<DWORD>(128,alloc_mib))*1024*1024,project_error);
                db.exec("INSERT INTO meta(key,value)VALUES('project_memory_enforced',?)ON CONFLICT(key)DO UPDATE SET value=excluded.value",{project_enforced?"1":"0"});
                db.exec("INSERT INTO meta(key,value)VALUES('shared_memory_status',?)ON CONFLICT(key)DO UPDATE SET value=excluded.value",{active_ledger.dump()});
                if(active_ledger.value("saturated",false))throw std::runtime_error("RESOURCE_MEMORY_LIMIT: 4096 MiB aggregate budget exhausted");
                PROCESS_MEMORY_COUNTERS_EX pmc{};pmc.cb=sizeof(pmc);
                if(GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),sizeof(pmc))&&pmc.PrivateUsage>static_cast<SIZE_T>(alloc_mib)*1024*1024){
                    throw std::runtime_error("RESOURCE_MEMORY_LIMIT: "+std::to_string(pmc.PrivateUsage/(1024*1024))+" MiB exceeds "+std::to_string(alloc_mib)+" MiB");
                }
                GlobalSlot worker(L"Worker",std::clamp(active_ledger["effective_workers"].get<int>(),1,8),[&]{check_control(db,id);});
                db.exec("UPDATE inventory_jobs SET state='running',pid=?,started=COALESCE(started,?),updated=?,resources=json_set(resources,'$.shared_budget_mib',4096,'$.project_allocated_mib',?,'$.shared_active_projects',?) WHERE id=? AND state='queued' AND control=''",{GetCurrentProcessId(),now(),now(),alloc_mib,active_ledger["active_projects"],id});
                if(sqlite3_changes(db.native_handle())!=1)continue;
                if(exporting){run_inventory_export(db,job,[&]{check_control(db,id);});idle=GetTickCount64();continue;}
                if(job["phase"]=="census")census(p,db,job);check_control(db,id);const bool deferred=safe_parse(job["resources"]).value("defer_analysis",false);if(!deferred)content(p,db,job);
                scale_fingerprints(db);
                db.begin();try{
                    const bool errors=db.integer("SELECT failed FROM inventory_jobs WHERE id=?",{id})>0;
                    const auto incomplete=deferred?Json():Json(db.integer("SELECT COUNT(*) FROM inventory_tasks t JOIN files f ON f.id=t.file_id WHERE t.job_id=? AND f.deleted=0 AND f.relevant=1 AND f.analysis<>'ANALYZED'",{id}));
                    db.exec("UPDATE inventory_jobs SET state=?,phase=?,finished=?,updated=?,pid=NULL,resources=json_set(resources,'$.incomplete_analysis',?) WHERE id=?",{errors?"completed_with_errors":"completed",deferred?"analysis_deferred":"finished",now(),now(),incomplete,id});
                    JOBOBJECT_EXTENDED_LIMIT_INFORMATION memory{};HANDLE peak_job=enforced?budget.h:(project_enforced?project_budget.h:nullptr);if(peak_job&&QueryInformationJobObject(peak_job,JobObjectExtendedLimitInformation,&memory,sizeof(memory),nullptr))db.exec("UPDATE inventory_jobs SET resources=json_set(resources,'$.peak_job_bytes',?,'$.memory_limit_enforced',1) WHERE id=?",{static_cast<uint64_t>(memory.PeakJobMemoryUsed),id});
                    db.exec("INSERT INTO meta VALUES('last_sync',?)ON CONFLICT(key)DO UPDATE SET value=excluded.value",{now()});db.exec("INSERT INTO meta VALUES('last_config',?)ON CONFLICT(key)DO UPDATE SET value=excluded.value",{sha256(p.config.dump())});
                    const bool new_events=db.integer("SELECT EXISTS(SELECT 1 FROM dirty_paths)")!=0;
                    db.exec("INSERT INTO meta VALUES('dirty',?)ON CONFLICT(key)DO UPDATE SET value=excluded.value",{errors||new_events?"1":"0"});db.change("inventory","inventory_job_finished",{{"job_id",id},{"errors",errors}});db.commit();}catch(...){db.rollback();throw;}
            }catch(const Stopped&){db.rollback();const auto control=scalar(db,"SELECT control FROM inventory_jobs WHERE id=?",{id});db.exec("UPDATE inventory_jobs SET state=?,pid=NULL,updated=? WHERE id=?",{control=="cancel"?"cancelled":"paused",now(),id});}
            catch(const std::exception& error){db.rollback();if(std::string(error.what())=="INPUT_CHANGED_REQUEUED")db.exec("UPDATE inventory_jobs SET state='queued',phase='analysis',error=?,pid=NULL,updated=? WHERE id=?",{error.what(),now(),id});else{if(std::string(error.what()).starts_with("RESOURCE_"))add_error(db,id,"",path_text(original.file),"coordinator",0,error.what());db.exec("UPDATE inventory_jobs SET state='failed',error=?,pid=NULL,finished=?,updated=? WHERE id=?",{error.what(),now(),now(),id});}}
            idle=GetTickCount64();
        }
        watcher.reset();
        if(once||db.integer("SELECT EXISTS(SELECT 1 FROM inventory_jobs WHERE state='queued' AND control='')")==0)break;
    }
    return 0;
}

Json scale_run(const Project& p,const Json& args,Log log,Cancel* cancel){
    auto started=scale_start(p,args,true);const auto id=started["job_id"].get<std::string>();
    Database db(p);
    const Json request={{"id",id}};
    auto last_poke=GetTickCount64(),last_logged=last_poke;
    std::string last_progress;
    for(;;){auto rows=read_jobs(db,request);const auto& job=rows["items"][0];auto state=job["state"].get<std::string>();
        if(log){
            auto progress="Inventario "+state+" · "+job["files"].dump()+" archivos · "+job["dirs"].dump()+" directorios";
            const auto tick=GetTickCount64();
            if(progress!=last_progress||tick-last_logged>=5000){log(progress);last_progress=std::move(progress);last_logged=tick;}
        }
        if(terminal(state)||state=="paused"){auto result=job;result["job_id"]=id;result["schema"]="migratool.sync/2";result["revision"]=db.revision();result["changes"]=Json::array();result["errors"]=db.query("SELECT * FROM scan_errors WHERE job_id=? ORDER BY id LIMIT 100",{id});result["symbols"]=db.integer("SELECT COUNT(*) FROM symbols WHERE active=1");return result;}
        if(cancel&&cancel->requested){scale_control(p,{{"id",id},{"action","cancel"},{"actor","caller"}});}
        else if(state=="queued"&&GetTickCount64()-last_poke>=1500){try{spawn_coordinator(p,job["scope"].empty());}catch(...){}last_poke=GetTickCount64();}
        Sleep(100);
    }
}

Json scale_summary(const Project& p,Database& db){
    const auto rev=db.revision();
    if(project_mode(p)=="archive"||(db.integer("SELECT EXISTS(SELECT 1 FROM sqlite_master WHERE type='table' AND name='fs_roots')")!=0&&db.integer("SELECT COUNT(*) FROM files")==0&&db.integer("SELECT COUNT(*) FROM fs_roots WHERE complete=1")>0)){
        auto fs_st=fs_stats_summary(p,db);
        return {{"schema","migratool.status/2"},{"mode",project_mode(p)},{"name",p.config.value("name","")},{"project_path",path_text(p.file)},{"revision",rev},
            {"inventory_complete",fs_st["complete"]},{"index_dirty",false},{"known_files",fs_st["total_files"]},{"physical_files",fs_st["total_files"]},
            {"total_dirs",fs_st["total_dirs"]},{"total_links",fs_st["total_links"]},{"total_entries",fs_st["total_entries"]},{"total_bytes",fs_st["total_bytes"]},
            {"scan_errors",fs_st["scan_errors"]},{"catalog_bytes",fs_st["catalog_bytes"]},{"fs_index",fs_st},
            {"hint","Modo ultra-escala activo. Use explore_tree, find_files y changes_feed para navegar millones de entradas en <25ms."}};
    }
    const auto jobs=db.query("SELECT * FROM inventory_jobs WHERE generation>0 ORDER BY created DESC LIMIT 1");
    bool complete=db.integer("SELECT EXISTS(SELECT 1 FROM inventory_generations WHERE state='published')")!=0;
    for(const auto& root:p.config["roots"])complete=complete&&db.integer("SELECT EXISTS(SELECT 1 FROM roots WHERE id=? AND path=? AND complete=1)",{root["id"],path_text(p.resolve(root["path"]))})!=0;
    const bool most_recent_ok=!jobs.empty()&&jobs[0]["state"]=="completed";
    const bool fresh=complete&&most_recent_ok&&db.integer("SELECT COALESCE(MAX(CAST(value AS INTEGER)),0) FROM meta WHERE key='dirty'")==0&&db.integer("SELECT EXISTS(SELECT 1 FROM inventory_jobs WHERE generation>0 AND state IN('queued','running','paused'))")==0;
    auto evidence=scale_evidence(p,db,fresh);auto epoch=db.integer("SELECT COALESCE(MAX(CAST(value AS INTEGER)),0) FROM meta WHERE key='progress_epoch'");
    const auto key=sha256(Json::array({"dev_control_v2",MIGRATOOL_BUILD_ID,epoch,complete,fresh,p.revision,evidence,jobs.empty()?Json():Json::array({jobs[0]["id"],jobs[0]["state"],jobs[0]["phase"]})}).dump());
    auto cached=db.query("SELECT value FROM meta WHERE key='scale_summary_cache'");if(!cached.empty()){auto value=safe_parse(cached[0]["value"]);if(value.value("cache_key","")==key){value["revision"]=rev;return value;}}
    auto statement=scale_progress_sql(evidence);
    auto counts=db.query(statement["sql"].get<std::string>()+R"sql(SELECT COUNT(*) AS known_files,
 COALESCE(SUM(f.category='unknown'),0) AS unclassified_files,
 COALESCE(SUM(CASE WHEN r.role='source' AND f.relevant=1 THEN 1 ELSE 0 END),0) AS source_files,
 COALESCE(SUM(CASE WHEN f.relevant=0 THEN 1 ELSE 0 END),0) AS non_source_files,
 COALESCE(SUM(CASE WHEN r.role='source' AND f.relevant=1 THEN MAX(COALESCE(u.units,0),1) ELSE 0 END),0) AS total_units,
 COALESCE(SUM(CASE WHEN r.role='source' AND f.relevant=1 THEN CASE WHEN m.current AND m.complete THEN MAX(COALESCE(u.units,0),1) ELSE COALESCE(u.migrated,0) END ELSE 0 END),0) AS migrated_units,
 COALESCE(SUM(CASE WHEN r.role='source' AND f.relevant=1 THEN CASE WHEN m.verified THEN MAX(COALESCE(u.units,0),1) ELSE COALESCE(u.verified,0) END ELSE 0 END),0) AS verified_units,
 COALESCE(SUM(CASE WHEN r.role='source' AND f.relevant=1 AND m.current AND m.complete THEN 1 ELSE 0 END),0) AS declared_files,
 COALESCE(SUM(CASE WHEN r.role='source' AND f.relevant=1 AND m.verified THEN 1 ELSE 0 END),0) AS verified_files,
 COALESCE(SUM(f.relevant=1 AND f.analysis<>'ANALYZED'),0) AS pending_analysis
 FROM files f LEFT JOIN roots r ON r.id=f.root LEFT JOIN symbol_progress u ON u.file_id=f.id LEFT JOIN valid_maps m ON m.source='F'||f.id WHERE f.deleted=0)sql",statement["params"])[0];
    auto units=counts["total_units"].get<int64_t>(),migrated=counts["migrated_units"].get<int64_t>(),verified=counts["verified_units"].get<int64_t>();const bool denominator=complete&&counts["pending_analysis"]==0&&counts["unclassified_files"]==0;
    Json non_source_cats=Json::object();
    for(const auto& cr:db.query("SELECT COALESCE(category,'unknown') AS cat,COUNT(*) AS cnt FROM files WHERE deleted=0 AND relevant=0 GROUP BY category"))
        non_source_cats[cr["cat"].get<std::string>()]=cr["cnt"];
    Json disp_counts=Json::object();
    for(const auto& dr:db.query("SELECT COALESCE(NULLIF(json_extract(data,'$.disposition'),''),'unassigned') AS disp,COUNT(*) AS cnt FROM files WHERE deleted=0 AND relevant=0 GROUP BY disp"))
        disp_counts[dr["disp"].get<std::string>()]=dr["cnt"];
    for(const auto& mr:db.query("SELECT COALESCE(json_extract(hashes,'$._disposition'),'migrated') AS disp,COUNT(*) AS cnt FROM mappings GROUP BY disp")){
        const auto dk=mr["disp"].get<std::string>();
        disp_counts[dk]=disp_counts.value(dk,int64_t(0))+mr["cnt"].get<int64_t>();
    }
    Json result={{"schema","migratool.status/2"},{"name",p.config.value("name","")},{"project_path",path_text(p.file)},{"revision",rev},{"cache_key",key},{"inventory_complete",complete},{"index_dirty",!fresh},
        {"known_files",counts["known_files"]},{"physical_files",db.integer("SELECT COUNT(*) FROM files WHERE deleted=0 OR COALESCE(json_extract(data,'$.present'),0)=1")},{"source_files",counts["source_files"]},{"non_source_files",counts["non_source_files"]},{"non_source_by_category",non_source_cats},{"disposition_counts",disp_counts},{"unclassified_files",counts["unclassified_files"]},
        {"total_units",units},{"migrated_units",migrated},{"pending_units",units-migrated},{"analysis_pending",counts["pending_analysis"]},
        {"migration_percent",fresh&&denominator&&units?Json(100.0*migrated/units):Json(nullptr)},{"known_subset_migration_percent",units?Json(100.0*migrated/units):Json(nullptr)},
        {"discovery_percent",complete?Json(100):Json(nullptr)},{"classification_percent",counts["known_files"]!=0?Json(100.0*(counts["known_files"].get<double>()-counts["unclassified_files"].get<double>())/counts["known_files"].get<double>()):Json(nullptr)},
        {"units_with_passing_cases",verified},{"units_with_passing_cases_percent",fresh&&denominator&&units?Json(100.0*verified/units):Json(nullptr)},{"functional_parity_percent",nullptr},{"universal_equivalence_proven",false},
        {"configured_cases",evidence["cases"].size()},{"passing_current_cases",evidence["passed"].size()},{"declared_complete_files",counts["declared_files"]},{"files_verified_in_cases",counts["verified_files"]},
        {"last_sync",scalar(db,"SELECT value FROM meta WHERE key='last_sync'")},{"freshness",{{"checked",true},{"fresh",fresh},{"basis","published_census_with_recorded_changes"}}},
        {"percentage_basis","Símbolos activos; una unidad por archivo sin desglose. Declaración vigente por archivo o símbolo. Los casos requieren hashes, compilaciones y salidas vigentes."},
        {"files",Json::array()},{"mappings",Json::array()},{"cases",evidence["cases"]},{"builds",evidence["builds"]},{"coverage",Json::array()},{"diagnostics",Json::array()},{"exclusions",Json::array()},
        {"limits",{"Censo, análisis, declaración de migración y comportamiento verificado son evidencias distintas.","El porcentaje definitivo es desconocido mientras el denominador esté incompleto.","Sin coordinador activo, sincronice antes de usar evidencia."}}};
    const int64_t doc_total=db.integer("SELECT COUNT(*) FROM files f LEFT JOIN roots r ON r.id=f.root WHERE f.deleted=0 AND r.role='source' AND f.category='documentation'");
    const int64_t doc_reviewed=db.integer("SELECT COUNT(*) FROM doc_audits d JOIN files f ON f.id=CASE WHEN substr(d.doc_id,1,1)='F' THEN CAST(substr(d.doc_id,2) AS INTEGER) END WHERE f.deleted=0 AND d.state<>'UNREVIEWED' AND d.sha256=f.sha256");
    const int64_t doc_stale=db.integer("SELECT COUNT(*) FROM doc_audits d JOIN files f ON f.id=CASE WHEN substr(d.doc_id,1,1)='F' THEN CAST(substr(d.doc_id,2) AS INTEGER) END WHERE f.deleted=0 AND d.sha256<>f.sha256");
    const int64_t phases_total=db.integer("SELECT COUNT(*) FROM project_phases");
    const int64_t phases_done=db.integer("SELECT COUNT(*) FROM project_phases WHERE state IN('COMPLETED','VERIFIED','DONE')");
    const int64_t tasks_total=db.integer("SELECT COUNT(*) FROM project_tasks");
    const int64_t tasks_done=db.integer("SELECT COUNT(*) FROM project_tasks WHERE state IN('COMPLETED','VERIFIED','DONE')");
    const int64_t tasks_half=db.integer("SELECT COUNT(*) FROM project_tasks WHERE state='HALF_DONE'");
    const int64_t tasks_active=db.integer("SELECT COUNT(*) FROM project_tasks WHERE state='IN_PROGRESS'");
    result["development_control"]={
        {"phases_total",phases_total},{"phases_completed",phases_done},
        {"tasks_total",tasks_total},{"tasks_completed",tasks_done},{"tasks_half_done",tasks_half},{"tasks_in_progress",tasks_active},
        {"documentation_files",doc_total},{"documentation_reviewed",doc_reviewed},{"documentation_stale",doc_stale}
    };
    if(!jobs.empty())result["inventory_job"]={{"id",jobs[0]["id"]},{"state",jobs[0]["state"]},{"files",jobs[0]["files"]},{"directories",jobs[0]["dirs"]},{"links",jobs[0]["links"]},{"failed",jobs[0]["failed"]}};
    if(!fresh||!denominator||units==0){
        Json pending_by_lang=db.query("SELECT COALESCE(language,'unknown') AS language,COALESCE(analysis,'PENDING') AS analysis,COUNT(*) AS count FROM files WHERE deleted=0 AND relevant=1 AND analysis<>'ANALYZED' GROUP BY language,analysis ORDER BY count DESC LIMIT 10");
        Json unclassified_samples=db.query("SELECT 'F'||id AS id,path FROM files WHERE deleted=0 AND category='unknown' ORDER BY id LIMIT 5");
        Json recent_errors=db.query("SELECT job_id,path,operation,message FROM scan_errors ORDER BY id DESC LIMIT 5");
        std::string hint="Ejecute inventory_sync para actualizar el censo y análisis.";
        if(counts["unclassified_files"].get<int64_t>()>0)hint="Clasifique archivos 'unknown' con batch_update(op='classify' o 'classify_pattern').";
        else if(counts["pending_analysis"].get<int64_t>()>0)hint="Complete el análisis con inventory_sync o reclasifique archivos no-código con relevant:false.";
        result["denominator_blockers"]={
            {"fresh",fresh},{"inventory_complete",complete},{"denominator_ready",denominator},
            {"pending_analysis",counts["pending_analysis"]},{"unclassified_files",counts["unclassified_files"]},
            {"pending_by_language",pending_by_lang},{"unclassified_samples",unclassified_samples},
            {"recent_scan_errors",recent_errors},{"remediation_hint",hint}
        };
    }
    db.try_aux_write("INSERT INTO meta VALUES('scale_summary_cache',?)ON CONFLICT(key)DO UPDATE SET value=excluded.value",{result.dump()});return result;
}
Json scale_file_status(const Project& p,Database& db,const Json& args){
    auto q=args;q["kind"]="files";q["role"]=args.value("role","source");
    if(args.value("pending_reason",std::string())=="non_relevant")q["relevant"]=false;
    else if(!args.value("include_non_relevant",false)&&!args.contains("relevant"))q["relevant"]=true;
    q.erase("fields");auto page=query_page(p,db,q);if(page["items"].empty())return page;
    const bool fresh=db.integer("SELECT COALESCE(MAX(CAST(value AS INTEGER)),0) FROM meta WHERE key='dirty'")==0&&db.integer("SELECT EXISTS(SELECT 1 FROM inventory_jobs WHERE generation>0 AND state IN('queued','running','paused'))")==0;
    auto evidence=scale_evidence(p,db,fresh);evidence["file_ids"]=Json::array();std::string ids;for(const auto& f:page["items"]){auto n=std::stoll(f["id"].get<std::string>().substr(1));evidence["file_ids"].push_back(n);if(!ids.empty())ids+=',';ids+=std::to_string(n);}
    auto statement=scale_progress_sql(evidence);std::map<int64_t,Json> states;
    db.each(statement["sql"].get<std::string>()+R"sql(SELECT f.id,MAX(COALESCE(u.units,0),1) AS units,
 CASE WHEN m.current AND m.complete THEN MAX(COALESCE(u.units,0),1) ELSE COALESCE(u.migrated,0) END AS migrated,
 CASE WHEN m.verified THEN MAX(COALESCE(u.units,0),1) ELSE COALESCE(u.verified,0) END AS verified,
 COALESCE(m.current,0) AS mapping_current,COALESCE(m.complete AND m.current,0) AS declared,
 COALESCE(m.cases_pass,0) AS cases_pass,COALESCE(m.builds_pass,0) AS builds_pass,
 COALESCE(json_extract(m.hashes,'$._stage'),'') AS raw_stage,
 COALESCE(NULLIF(json_extract(m.hashes,'$._disposition'),''),NULLIF(json_extract(f.data,'$.disposition'),''),CASE WHEN f.relevant=0 THEN 'unassigned' ELSE 'migrated' END) AS raw_disposition,
 m.source AS mapping_source,m.note,m.actor,m.at,m.targets,m.cases
 FROM files f LEFT JOIN symbol_progress u ON u.file_id=f.id LEFT JOIN valid_maps m ON m.source='F'||f.id WHERE f.id IN()sql"+ids+")",statement["params"],[&](const Json& row){states[row["id"]]=row;return true;});
    std::map<int64_t,Json> paths,claims;std::map<int64_t,std::set<std::string>> seen;
    db.each("SELECT CASE WHEN substr(m.source,1,1)='F' THEN CAST(substr(m.source,2)AS INTEGER) ELSE s.file_id END AS source_file_id,t.value AS target,COALESCE(tf.absolute_path,tsf.absolute_path) AS path FROM mappings m LEFT JOIN symbols s ON s.id=CASE WHEN substr(m.source,1,1)='S' THEN CAST(substr(m.source,2) AS INTEGER) END JOIN json_each(m.targets)t LEFT JOIN files tf ON tf.id=CASE WHEN substr(t.value,1,1)='F' THEN CAST(substr(t.value,2) AS INTEGER) END LEFT JOIN symbols ts ON ts.id=CASE WHEN substr(t.value,1,1)='S' THEN CAST(substr(t.value,2) AS INTEGER) END LEFT JOIN files tsf ON tsf.id=ts.file_id WHERE m.source IN(SELECT 'F'||id FROM files WHERE id IN("+ids+")) OR m.source IN(SELECT 'S'||id FROM symbols WHERE file_id IN("+ids+"))",Json::array(),[&](const Json& row){auto id=row["source_file_id"].get<int64_t>();if(!row["path"].is_null()&&seen[id].insert(row["path"]).second){if(!paths.contains(id))paths[id]=Json::array();paths[id].push_back(row["path"]);}return true;});
    db.each("SELECT entity,actor,expires_at FROM task_claims WHERE entity IN(SELECT 'F'||id FROM files WHERE id IN("+ids+")) AND expires_at>?",{now()},[&](const Json& row){claims[std::stoll(row["entity"].get<std::string>().substr(1))]=row;return true;});
    const bool full_detail=args.value("detail","compact")=="full";
    const auto fields=args.value("fields",Json::array());
    for(auto& f:page["items"]){const auto n=std::stoll(f["id"].get<std::string>().substr(1));const auto& st=states.at(n);auto units=st["units"].get<int64_t>(),done=st["migrated"].get<int64_t>(),verified=st["verified"].get<int64_t>();const bool mapped=!st["mapping_source"].is_null();
        Json missing=Json::array();if(!mapped&&!paths.contains(n))missing.push_back("MISSING_TARGET");if(mapped&&st["mapping_current"]==0)missing.push_back("STALE_EVIDENCE");if(f["analysis"]!="ANALYZED")missing.push_back("ANALYSIS_PARTIAL");if(done<units)missing.push_back("COMPLETION_DECLARATION");if(st["cases_pass"]==0)missing.push_back("LINKED_PASSING_CASES");if(st["builds_pass"]==0)missing.push_back("CURRENT_BUILDS");if(claims.contains(n))missing.push_back("TASK_CLAIMED");
        std::string raw_stage=st["raw_stage"].is_string()?st["raw_stage"].get<std::string>():"";
        std::string eff_stage=(verified==units&&fresh)?"verified":(mapped&&st["mapping_current"]==0)?"stale":!raw_stage.empty()?raw_stage:(done==units?"integrated":paths.contains(n)?"ported":"unmapped");
        f["source_path"]=f["absolute_path"];f["target_paths"]=paths.contains(n)?paths[n]:Json::array();f["targets"]=mapped?safe_parse(st["targets"]):Json::array();f["total_units"]=units;f["migrated_units"]=done;f["units_with_passing_cases"]=verified;
        f["migration_percent"]=fresh?Json(100.0*done/units):Json(nullptr);f["known_subset_migration_percent"]=100.0*done/units;f["units_with_passing_cases_percent"]=fresh?Json(100.0*verified/units):Json(nullptr);f["missing"]=missing;
        f["state"]=verified==units&&fresh?"VERIFIED_IN_CASES":mapped&&st["mapping_current"]==0?"STALE":done==units?"DECLARED_COMPLETE_UNVERIFIED":paths.contains(n)?"MAPPED_UNVERIFIED":"UNMAPPED";
        f["stage"]=eff_stage;
        f["disposition"]=st["raw_disposition"].is_string()?st["raw_disposition"].get<std::string>():std::string("migrated");
        f["declared_complete"]=st["declared"]==1;f["verified_in_configured_cases"]=verified==units&&fresh;for(const auto* k:{"note","actor","at"})f[k]=st[k].is_null()?Json(""):st[k];f["claim"]=claims.contains(n)?claims[n]:Json();
        if(!full_detail&&fields.empty()){for(const auto* drop:{"deleted","first_revision","last_revision","duplicate_copies"})f.erase(drop);}
        if(!fields.empty()){Json selected=Json::object();for(const auto& k:fields)if(f.contains(k.get<std::string>()))selected[k.get<std::string>()]=f[k.get<std::string>()];f=selected;}
    }return page;
}
Json scale_module_matrix(const Project& p,Database& db,const Json& args){
    const bool fresh=db.integer("SELECT COALESCE(MAX(CAST(value AS INTEGER)),0) FROM meta WHERE key='dirty'")==0&&db.integer("SELECT EXISTS(SELECT 1 FROM inventory_jobs WHERE generation>0 AND state IN('queued','running','paused'))")==0;
    const int depth=std::clamp(args.value("depth",2),1,4);
    const std::string text_filter=lower(args.value("text",""));
    const std::string root_filter=args.value("root","");
    auto module_prefix=[&](const std::string& root,const std::string& rel_path,int segs)->std::string{
        std::string norm=rel_path;std::replace(norm.begin(),norm.end(),'\\','/');
        size_t pos=0;int count=0;
        while(count<segs){
            auto slash=norm.find('/',pos);
            if(slash==std::string::npos){return count==0?root+":(root)":root+":"+norm.substr(0,pos>0?pos-1:norm.size());}
            pos=slash+1;++count;
        }
        return root+":"+(pos>0?norm.substr(0,pos-1):"(root)");
    };
    const bool include_non_source=args.value("include_non_source",false);
    struct ModAgg {
        std::string root,module_name;
        int64_t source_files=0,non_source_files=0,total_units=0,mapped_files=0,half_done_files=0,ported_files=0,declared_files=0,verified_files=0,migrated_units=0,verified_units=0;
        std::map<std::string,int64_t> target_counts;
        std::map<std::string,int64_t> disposition_counts;
    };
    std::map<std::string,ModAgg> modules;
    std::map<int64_t,std::string> file_to_mod;
    auto evidence=scale_evidence(p,db,fresh);
    auto statement=scale_progress_sql(evidence);
    const std::string rel_filter=include_non_source?"":" AND f.relevant=1";
    db.each(statement["sql"].get<std::string>()+R"sql(SELECT f.id,f.root,f.path,f.relevant,MAX(COALESCE(u.units,0),1) AS units,
 CASE WHEN m.current AND m.complete THEN MAX(COALESCE(u.units,0),1) ELSE COALESCE(u.migrated,0) END AS migrated,
 CASE WHEN m.verified THEN MAX(COALESCE(u.units,0),1) ELSE COALESCE(u.verified,0) END AS verified,
 COALESCE(m.current,0) AS mapping_current,COALESCE(m.complete AND m.current,0) AS declared,
 COALESCE(json_extract(m.hashes,'$._stage'),'') AS raw_stage,
 COALESCE(NULLIF(json_extract(m.hashes,'$._disposition'),''),NULLIF(json_extract(f.data,'$.disposition'),''),'') AS raw_disposition,
 m.source AS mapping_source
 FROM files f JOIN roots r ON r.id=f.root LEFT JOIN symbol_progress u ON u.file_id=f.id LEFT JOIN valid_maps m ON m.source='F'||f.id WHERE f.deleted=0)sql"+rel_filter+" AND r.role='source'",
        statement["params"],[&](const Json& row){
            const auto r_id=row["root"].get<std::string>();
            if(!root_filter.empty()&&r_id!=root_filter)return true;
            const auto mod=module_prefix(r_id,row["path"].get<std::string>(),depth);
            if(!text_filter.empty()&&lower(mod).find(text_filter)==std::string::npos)return true;
            const auto fid=row["id"].get<int64_t>();
            auto& agg=modules[mod];
            agg.root=r_id;agg.module_name=mod;
            const auto disp=row["raw_disposition"].is_string()?row["raw_disposition"].get<std::string>():std::string();
            if(!disp.empty())agg.disposition_counts[disp]++;
            if(row["relevant"]!=1){
                ++agg.non_source_files;
                return true;
            }
            file_to_mod[fid]=mod;
            ++agg.source_files;
            const auto units=row["units"].get<int64_t>(),mig=row["migrated"].get<int64_t>(),ver=row["verified"].get<int64_t>();
            agg.total_units+=units;agg.migrated_units+=mig;agg.verified_units+=ver;
            const bool has_map=!row["mapping_source"].is_null()||mig>0;
            if(has_map)++agg.mapped_files;
            if(row["declared"]==1)++agg.declared_files;
            if(ver==units&&fresh)++agg.verified_files;
            const auto stg=row["raw_stage"].is_string()?row["raw_stage"].get<std::string>():"";
            if(stg=="half_done")++agg.half_done_files;
            else if(stg=="ported")++agg.ported_files;
            return true;
        });
    if(!file_to_mod.empty()){
        db.each("SELECT CASE WHEN substr(m.source,1,1)='F' THEN CAST(substr(m.source,2)AS INTEGER) ELSE s.file_id END AS source_file_id,COALESCE(tf.root,tsf.root) AS t_root,COALESCE(tf.path,tsf.path) AS t_path FROM mappings m LEFT JOIN symbols s ON s.id=CASE WHEN substr(m.source,1,1)='S' THEN CAST(substr(m.source,2) AS INTEGER) END JOIN json_each(m.targets) t LEFT JOIN files tf ON tf.id=CASE WHEN substr(t.value,1,1)='F' THEN CAST(substr(t.value,2) AS INTEGER) END LEFT JOIN symbols ts ON ts.id=CASE WHEN substr(t.value,1,1)='S' THEN CAST(substr(t.value,2) AS INTEGER) END LEFT JOIN files tsf ON tsf.id=ts.file_id WHERE COALESCE(tf.root,tsf.root) IS NOT NULL",
            Json::array(),[&](const Json& row){
                if(row["source_file_id"].is_null())return true;
                auto it=file_to_mod.find(row["source_file_id"].get<int64_t>());
                if(it==file_to_mod.end())return true;
                auto tmod=module_prefix(row["t_root"].get<std::string>(),row["t_path"].get<std::string>(),2);
                modules[it->second].target_counts[tmod]++;
                return true;
            });
    }
    Json all_rows=Json::array();
    int64_t row_idx=0;
    for(const auto& [key,agg]:modules){
        Json tlist=Json::array();
        std::vector<std::pair<std::string,int64_t>> sorted_t(agg.target_counts.begin(),agg.target_counts.end());
        std::sort(sorted_t.begin(),sorted_t.end(),[](const auto& a,const auto& b){return a.second!=b.second?a.second>b.second:a.first<b.first;});
        std::string top_targets;
        for(size_t i=0;i<sorted_t.size();++i){
            tlist.push_back({{"module",sorted_t[i].first},{"links",sorted_t[i].second}});
            if(i<3){if(!top_targets.empty())top_targets+="; ";top_targets+=sorted_t[i].first+" ("+std::to_string(sorted_t[i].second)+")";}
        }
        const double mig_pct=agg.total_units?100.0*static_cast<double>(agg.migrated_units)/static_cast<double>(agg.total_units):0.0;
        const double ver_pct=agg.total_units?100.0*static_cast<double>(agg.verified_units)/static_cast<double>(agg.total_units):0.0;
        const std::string st=(agg.source_files==0&&agg.non_source_files>0)?"NON_SOURCE":
            (agg.verified_units==agg.total_units&&agg.total_units>0&&fresh)?"VERIFIED_IN_CASES":
            (agg.migrated_units==agg.total_units&&agg.total_units>0)?"DECLARED_COMPLETE_UNVERIFIED":
            (agg.mapped_files>0||agg.migrated_units>0)?"MAPPED_UNVERIFIED":"UNMAPPED";
        Json mod_row={
            {"id",++row_idx},{"source_module",agg.module_name},{"root",agg.root},
            {"source_files",agg.source_files},{"non_source_files",agg.non_source_files},{"mapped_files",agg.mapped_files},{"half_done_files",agg.half_done_files},
            {"ported_files",agg.ported_files},{"declared_files",agg.declared_files},{"verified_files",agg.verified_files},
            {"total_units",agg.total_units},{"migrated_units",agg.migrated_units},{"verified_units",agg.verified_units},
            {"migration_percent",mig_pct},{"units_with_passing_cases_percent",ver_pct},
            {"target_summary",top_targets.empty()?"Sin destino":top_targets},{"target_modules",tlist},{"state",st}
        };
        if(!agg.disposition_counts.empty())mod_row["dispositions"]=agg.disposition_counts;
        all_rows.push_back(std::move(mod_row));
    }
    const int limit=std::clamp(args.value("limit",200),1,1000);
    int64_t offset=0;
    auto cur=args.value("cursor",std::string());
    if(!cur.empty()){try{auto colon=cur.rfind(':');offset=std::max<int64_t>(0,std::stoll(colon==std::string::npos?cur:cur.substr(colon+1)));}catch(...){}}
    Json items=Json::array();
    for(size_t i=static_cast<size_t>(offset);i<all_rows.size()&&items.size()<static_cast<size_t>(limit);++i)items.push_back(all_rows[i]);
    const bool more=(static_cast<size_t>(offset)+items.size())<all_rows.size();
    return {{"revision",db.revision()},{"fresh",fresh},{"total",all_rows.size()},{"items",items},
            {"cursor",std::to_string(db.revision())+":"+std::to_string(offset)},
            {"next_cursor",more?Json(std::to_string(db.revision())+":"+std::to_string(static_cast<size_t>(offset)+items.size())):Json(nullptr)}};
}
Json scale_snapshot(const Project& p,Database& db){
    auto summary=scale_summary(p,db);Json roots=Json::array();for(const auto& row:db.query("SELECT data FROM roots ORDER BY id"))roots.push_back(safe_parse(row["data"]));
    const bool content_complete=summary["inventory_complete"]==true&&summary["index_dirty"]==false&&db.integer("SELECT EXISTS(SELECT 1 FROM files WHERE deleted=0 AND relevant=1 AND sha256 IS NULL)")==0&&db.integer("SELECT COUNT(*) FROM dirty_buckets")==0;
    auto files=query_page(p,db,{{"kind","files"},{"limit",200},{"include_total",false}});auto symbols=query_page(p,db,{{"kind","symbols"},{"limit",200},{"include_total",false}});
    return {{"schema","migratool.scan/2"},{"at",now()},{"complete",content_complete},{"census_complete",summary["inventory_complete"]},{"snapshot_complete",false},{"normalized_in_sqlite",true},
        {"roots",roots},{"inventory_config",sha256(p.config.dump())},{"files",files["items"]},{"symbols",symbols["items"]},{"dependencies",Json::array()},{"diagnostics",Json::array()},
        {"exclusions",Json::array()},{"files_count",summary["known_files"]},{"continuation",{{"files",files["next_cursor"]},{"symbols",symbols["next_cursor"]}}},
        {"hint","Vista acotada; use query para páginas y export_inventory para exportación completa en flujo."}};
}

Json scale_export(const Project& p,const Json& args,Cancel* cancel){
    const auto output=args.at("out").get<std::string>();
    const auto format=args.value("format",std::string("jsonl"));
    if(format!="jsonl"&&format!="json")throw std::runtime_error("EXPORT_FORMAT");
    if(output.empty())throw std::runtime_error("EXPORT_DESTINATION_REQUIRED");
    // AUD-009: Confine export destination to state_dir or authorized project roots!
    const auto destination=p.resolve_confined(output,true);
    if(fs::exists(native_path(destination)))throw std::runtime_error("EXPORT_DESTINATION_EXISTS");
    if(cancel&&cancel->requested)throw std::runtime_error("EXPORT_CANCELLED");
    const auto id=unique_id(),created=now();
    const auto stage=destination.parent_path()/wide(".migratool-export-"+id+".pending");
    const Json scope={{"kind","export_inventory"},{"out",path_text(destination)},{"stage",path_text(stage)},{"format",format}};
    Database db(p);
    db.begin();
    try{
        db.exec("INSERT INTO inventory_jobs(id,state,phase,scope,config_sha256,created,updated,resources,from_revision) VALUES(?,'queued','export',?,?,?,?,?,?)",
            {id,scope.dump(),sha256(p.config.dump()),created,created,resource_profile(p).dump(),db.revision()});
        db.change(args.value("actor",std::string("export")),"inventory_export_queued",{{"job_id",id},{"path",scope["out"]},{"format",format}});
        db.commit();
    }catch(...){db.rollback();throw;}
    scale_spawn(p);
    if(!args.value("wait",true))return {{"job_id",id},{"kind","export_inventory"},{"state","queued"},{"path",scope["out"]},{"revision",db.revision()}};
    bool cancel_sent=false;
    for(;;){
        const auto job=scale_jobs(p,{{"id",id}}).at("items").at(0);
        const auto state=job.at("state").get<std::string>();
        if(state=="completed")return job.at("result");
        if(terminal(state)||state=="paused")throw std::runtime_error("EXPORT_"+state+": "+job.value("error",std::string()));
        if(cancel&&cancel->requested&&!cancel_sent){
            scale_control(p,{{"id",id},{"action","cancel"},{"actor",args.value("actor",std::string("export"))}});
            cancel_sent=true;
        }
        Sleep(100);
    }
}

Json scale_context(const Project& p,Database& db,const Json& args){
    auto id=args.value("task_id",args.value("id",std::string()));if(id.empty())id=scalar(db,"SELECT 'F'||f.id FROM files f JOIN roots r ON r.id=f.root WHERE f.deleted=0 AND f.relevant=1 AND r.role='source' ORDER BY f.id LIMIT 1");
    if(id.empty())throw std::runtime_error("NO_SOURCE_TASK");const bool symbol=id[0]=='S';auto number=std::stoll(id.substr(1));
    Json rows=symbol?db.query("SELECT 'F'||f.id AS id,f.absolute_path,f.root,f.path,f.language,f.sha256,f.category,s.data AS symbol_data FROM symbols s JOIN files f ON s.file_id=f.id WHERE s.id=? AND s.active=1",{number}):db.query("SELECT 'F'||id AS id,absolute_path,root,path,language,sha256,category FROM files WHERE id=? AND deleted=0",{number});
    if(rows.empty())throw std::runtime_error("TASK_NOT_FOUND");auto source=rows[0];if(source["category"]=="protected_configuration")throw std::runtime_error("PROTECTED_CONFIGURATION");source["entity_id"]=id;
    const auto file_id=std::stoll(source["id"].get<std::string>().substr(1));uint64_t begin=0,end=0;Json sym;
    if(symbol){sym=safe_parse(source["symbol_data"]);source.erase("symbol_data");source["symbol"]=sym;begin=sym.value("offset",uint64_t(0));end=sym.value("end_offset",uint64_t(0));}
    auto physical=fs::path(wide(source["absolute_path"]));if(classify_inventory_name(p,physical,physical.parent_path(),true).value("blocked_code",false))throw std::runtime_error("PROTECTED_CONFIGURATION");if(args.value("include_body",true)){
        auto size=fs::file_size(native_path(physical));if(!symbol)end=size;const uint64_t cap=static_cast<uint64_t>(std::clamp(args.value("max_body_bytes",16384),256,65536));
        const uint64_t expected_len=(end>=begin)?(end-begin):0;
        std::ifstream in(native_path(physical),std::ios::binary);in.seekg(static_cast<std::streamoff>(begin));std::string body(static_cast<size_t>(std::min(expected_len,cap)),'\0');in.read(body.data(),static_cast<std::streamsize>(body.size()));body.resize(static_cast<size_t>(in.gcount()));source["body"]=body;source["body_truncated"]=expected_len>body.size();source["range"]={{"offset",begin},{"end_offset",begin+body.size()}};
    }
    auto status=scale_file_status(p,db,{{"id",source["id"]},{"limit",1},{"include_total",false}});
    return {{"task_id",id},{"revision",db.revision()},{"source",source},{"status",status["items"].empty()?Json():status["items"][0]},
        {"dependencies",db.query("SELECT target_id,kind,name FROM dependencies WHERE file_id=? LIMIT 30",{file_id})},
        {"notes",db.query("SELECT text,actor,at FROM notes WHERE entity=? ORDER BY id DESC LIMIT 5",{id})}};
}

Json scale_acquire(const Project& p,const Json& args){
    require_actor(args);Database db(p);const int seconds=args.value("lease_seconds",300);
    if(seconds<15||seconds>3600)throw std::runtime_error("LEASE_RANGE");
    db.begin(args.value("expected_revision",int64_t(-1)));
    std::string id;Json claim;int64_t revision=0;
    try{
        std::string sql=R"sql(SELECT 'F'||f.id FROM files f JOIN roots r ON r.id=f.root LEFT JOIN mappings m ON m.source='F'||f.id
 WHERE f.deleted=0 AND f.relevant=1 AND r.role='source'
 AND (m.complete IS NULL OR m.complete=0 OR f.sha256 IS NULL OR json_extract(m.hashes,'$.'||char(34)||'F'||f.id||char(34)) IS NOT f.sha256
      OR EXISTS(SELECT 1 FROM json_each(m.hashes) h LEFT JOIN files target ON target.id=CAST(substr(h.key,2) AS INTEGER)
                WHERE substr(h.key,1,1)='F' AND (target.id IS NULL OR target.deleted=1 OR target.sha256 IS NOT h.value)))
 AND NOT EXISTS(SELECT 1 FROM task_claims c WHERE c.entity='F'||f.id AND c.expires_at>?))sql";
        Json params={now()};
        if(args.contains("id")){
            const auto requested=args.at("id").get<std::string>();
            if(requested.size()<2||requested[0]!='F'||!std::all_of(requested.begin()+1,requested.end(),[](char c){return c>='0'&&c<='9';}))throw std::runtime_error("TASK_ID_INVALID");
            sql+=" AND f.id=?";params.push_back(std::stoll(requested.substr(1)));
        }
        id=scalar(db,sql+" ORDER BY f.id LIMIT 1",params);
        if(id.empty())throw std::runtime_error("NO_AVAILABLE_TASK");
        const auto expires=scalar(db,"SELECT strftime('%Y-%m-%dT%H:%M:%fZ','now',?)",{"+"+std::to_string(seconds)+" seconds"});
        revision=db.change(args["actor"],"claim_task",{{"entity",id},{"expires_at",expires}},id);
        claim={{"actor",args["actor"]},{"expires_at",expires}};
        db.exec("INSERT INTO task_claims(entity,actor,claimed_at,expires_at,lease_seconds,revision)VALUES(?,?,?,?,?,?)ON CONFLICT(entity)DO UPDATE SET actor=excluded.actor,claimed_at=excluded.claimed_at,expires_at=excluded.expires_at,lease_seconds=excluded.lease_seconds,revision=excluded.revision",{id,args["actor"],now(),expires,seconds,revision});
        db.commit();
    }catch(...){db.rollback();throw;}
    auto request=args;request["id"]=id;request["task_id"]=id;
    try{auto result=scale_context(p,db,request);result["claim"]=claim;return result;}
    catch(const std::exception& error){return {{"task_id",id},{"revision",revision},{"claim",claim},{"context_error",error.what()}};}
}

Json scale_doctor(const Project& p){Database db(p);auto name=coordinator_name(p);Handle mutex{OpenMutexW(SYNCHRONIZE,FALSE,name.c_str())};bool live=false;if(mutex.h){auto wait=WaitForSingleObject(mutex,0);live=wait==WAIT_TIMEOUT;if(wait==WAIT_OBJECT_0||wait==WAIT_ABANDONED)ReleaseMutex(mutex);}
    Json roots=Json::array();for(const auto& r:p.config["roots"]){auto path=p.resolve(r["path"]);auto attrs=GetFileAttributesW(path.c_str());roots.push_back({{"id",r["id"]},{"path",path_text(path)},{"available",attrs!=INVALID_FILE_ATTRIBUTES},{"reparse",attrs!=INVALID_FILE_ATTRIBUTES&&(attrs&FILE_ATTRIBUTE_REPARSE_POINT)!=0}});}
    Json pending_by_lang=db.query("SELECT COALESCE(language,'unknown') AS language,COALESCE(analysis,'PENDING') AS analysis,COUNT(*) AS count FROM files WHERE deleted=0 AND relevant=1 AND analysis<>'ANALYZED' GROUP BY language,analysis ORDER BY count DESC LIMIT 10");
    int64_t unclassified_cnt=db.integer("SELECT COUNT(*) FROM files WHERE deleted=0 AND category='unknown'");
    int64_t pending_ana_cnt=db.integer("SELECT COUNT(*) FROM files WHERE deleted=0 AND relevant=1 AND analysis<>'ANALYZED'");
    return {{"build",build_identity()},{"project",path_text(p.file)},{"catalog",path_text(p.state_dir()/L"catalog.sqlite")},{"schema",db.integer("PRAGMA user_version")},
        {"revision",db.revision()},{"coordinator_live",live},{"memory_limit_enforced",scalar(db,"SELECT value FROM meta WHERE key='coordinator_memory_enforced'")=="1"},
        {"shared_memory",SharedBudgetLedger(p).status()},{"preflight",preflight_warnings(p)},
        {"denominator_health",{{"pending_analysis",pending_ana_cnt},{"unclassified_files",unclassified_cnt},{"pending_by_language",pending_by_lang}}},
        {"roots",roots},{"resources",resource_profile(p)},{"language_capabilities",language_capabilities(p)}};
}

std::optional<Json> scale_query(const Project& p,Database& db,const Json& args){
    const auto kind=args.value("kind","");
    if(kind=="module_matrix")return scale_module_matrix(p,db,args);
    std::string from,where=" WHERE 1=1",order="id";Json params=Json::array();
    std::string cur=args.value("cursor","");
    std::optional<int64_t> cursor_gen;
    if(!cur.empty()&&cur.starts_with("v2:")){
        const auto first=cur.find(':',3);
        const auto second=(first!=std::string::npos)?cur.find(':',first+1):std::string::npos;
        if(second!=std::string::npos){
            const auto gen_str=cur.substr(3,first-3);
            if(!gen_str.empty()&&std::all_of(gen_str.begin(),gen_str.end(),[](unsigned char c){return std::isdigit(c)!=0;}))
                cursor_gen=std::stoll(gen_str);
        }
    }
    std::optional<int64_t> pinned_gen;
    if(kind=="directories"||kind=="census_entries"||kind=="folder_coverage"){
        int64_t gen=args.contains("generation")?args["generation"].get<int64_t>():(cursor_gen.has_value()?*cursor_gen:db.integer("SELECT COALESCE(MAX(CAST(value AS INTEGER)),0) FROM meta WHERE key='published_generation'"));
        if(!gen)gen=db.integer("SELECT COALESCE(MAX(generation),0) FROM inventory_jobs");
        if(cursor_gen.has_value()&&gen!=*cursor_gen)throw std::runtime_error("CURSOR_FILTER_MISMATCH");
        pinned_gen=gen;
        from="census_entries";
        const bool published=db.integer("SELECT EXISTS(SELECT 1 FROM inventory_generations WHERE id=? AND state='published')",{gen})!=0;
        if(published){where+=" AND generation<=? AND EXISTS(SELECT 1 FROM inventory_generations g WHERE g.id=census_entries.generation AND g.state='published') AND NOT EXISTS(SELECT 1 FROM generation_scopes s JOIN inventory_generations g ON g.id=s.generation AND g.state='published' WHERE s.generation>census_entries.generation AND s.generation<=? AND s.root=census_entries.root AND (s.prefix='' OR census_entries.path=rtrim(s.prefix,'/') COLLATE NOCASE OR (census_entries.path>=s.prefix COLLATE NOCASE AND census_entries.path<s.prefix||char(1114111) COLLATE NOCASE)))";params.push_back(gen);params.push_back(gen);}
        else {where+=" AND generation=?";params.push_back(gen);}
        if(kind=="directories")where+=" AND kind IN('directory','directory_link')";
        if(args.contains("root")){where+=" AND root=?";params.push_back(args["root"]);}if(args.contains("parent")){where+=" AND parent=?";params.push_back(args["parent"]);}
    }else if(kind=="scan_errors"){
        from="scan_errors";
        if(args.contains("job_id")){where+=" AND job_id=?";params.push_back(args["job_id"]);}
        if(args.contains("root")){where+=" AND root=?";params.push_back(args["root"]);}
    }
    else if(kind=="history"||kind=="changes"){
        from="history";if(args.contains("entity")){where+=" AND entity=?";params.push_back(args["entity"]);}if(args.contains("actor")){where+=" AND actor=?";params.push_back(args["actor"]);}if(args.contains("since_revision")){where+=" AND revision>?";params.push_back(args["since_revision"]);}
    }
    else if(kind=="coverage"&&scale_enabled(p,db)){
        from="(SELECT ROW_NUMBER()OVER(ORDER BY e.revision,CAST(j.key AS INTEGER)) AS id,e.id AS evidence_id,e.at,json_extract(j.value,'$.path') AS absolute_path,json_extract(j.value,'$.sha256') AS sha256,json_extract(j.value,'$.lines.count') AS lines,json_extract(j.value,'$.lines.covered') AS lines_covered,json_extract(j.value,'$.branches.count') AS branches,json_extract(j.value,'$.branches.covered') AS branches_covered FROM evidence e JOIN json_each(e.data,'$.files') j WHERE e.kind='coverage')";
    }
    else if(kind=="archive_members"){
        from="(SELECT m.*,f.absolute_path,f.root FROM archive_members m JOIN files f ON f.id=m.archive_id WHERE COALESCE(json_extract(f.data,'$.present'),1)=1)";
        if(args.contains("archive_id")){where+=" AND archive_id=?";params.push_back(args["archive_id"]);}
        if(args.contains("state")){where+=" AND state=?";params.push_back(args["state"]);}
    }
    else if(kind=="reuse_candidates"){from="reuse_evidence";if(args.contains("sha256")){where+=" AND (source_sha256=? OR output_sha256=?)";params.push_back(args["sha256"]);params.push_back(args["sha256"]);}}
    else if(kind=="mappings"&&scale_enabled(p,db)){
        from=R"sql((SELECT m.rowid AS id,m.source,COALESCE(f.absolute_path,sf.absolute_path,'') AS source_path,
 m.targets AS targets_json,m.complete AS declared_complete,m.cases AS cases_json,m.actor,m.at,m.note,m.revision,
 COALESCE(json_extract(m.hashes,'$._disposition'),'migrated') AS disposition,
 COALESCE(json_extract(m.hashes,'$._stage'),CASE WHEN m.complete=1 THEN 'integrated' ELSE 'ported' END) AS stage,
 COALESCE(json_extract(m.hashes,'$._external_replacement'),'') AS external_replacement,
 (SELECT json_group_array(destination) FROM (SELECT COALESCE(target.absolute_path,symbol_file.absolute_path) AS destination
    FROM json_each(m.targets) entry
    LEFT JOIN files target ON substr(entry.value,1,1)='F' AND target.id=CAST(substr(entry.value,2) AS INTEGER)
    LEFT JOIN symbols target_symbol ON substr(entry.value,1,1)='S' AND target_symbol.id=CAST(substr(entry.value,2) AS INTEGER)
    LEFT JOIN files symbol_file ON symbol_file.id=target_symbol.file_id
    ORDER BY CAST(entry.key AS INTEGER)) WHERE destination IS NOT NULL) AS target_paths_json
 FROM mappings m LEFT JOIN files f ON substr(m.source,1,1)='F' AND f.id=CAST(substr(m.source,2) AS INTEGER)
 LEFT JOIN symbols s ON substr(m.source,1,1)='S' AND s.id=CAST(substr(m.source,2) AS INTEGER) LEFT JOIN files sf ON sf.id=s.file_id))sql";
    }
    else if(kind=="relations"&&scale_enabled(p,db)){
        from="(SELECT d.rowid AS id,'F'||d.doc_id AS source,'F'||d.code_id AS target,f.absolute_path AS source_path,t.absolute_path AS target_path,d.reason FROM doc_links d JOIN files f ON f.id=d.doc_id JOIN files t ON t.id=d.code_id WHERE f.deleted=0 AND t.deleted=0)";
    }
    else if(kind=="dependencies"&&scale_enabled(p,db)){
        from="(SELECT d.id,'F'||d.file_id AS source,'F'||d.target_id AS target,f.root,f.path,f.absolute_path,d.kind,d.name,COALESCE(json_extract(d.data,'$.resolution'),'UNRESOLVED_EXTERNAL') AS resolution,COALESCE(json_extract(d.data,'$.caller_symbol_id'),'') AS caller_symbol_id,COALESCE(json_extract(d.data,'$.target_symbol_id'),'') AS target_symbol_id FROM dependencies d JOIN files f ON f.id=d.file_id WHERE f.deleted=0)";
        if(args.contains("file")){auto file=args["file"].get<std::string>();where+=" AND (source=? OR path=? OR absolute_path=?)";params.push_back(file);params.push_back(file);params.push_back(file);}
        if(args.contains("root")){where+=" AND root=?";params.push_back(args["root"]);}
    }
    else if(kind=="search"&&scale_enabled(p,db)){
        const auto term=args.value("text",std::string());if(term.empty())throw std::runtime_error("SEARCH_TEXT_REQUIRED");std::string phrase="\"";for(char c:term){phrase+=c;if(c=='\"')phrase+=c;}phrase+='\"';
        from="(SELECT rowid AS id,entity_type,entity_id,snippet(search,2,'[',']','…',24) AS excerpt FROM search WHERE search MATCH ?)";params.push_back(phrase);
    }
    else if((kind=="code_duplicates"||kind=="code_duplicate_errors")&&scale_enabled(p,db)){
        if(kind=="code_duplicate_errors")from="(SELECT m.id,f.root,r.role,f.absolute_path,m.entry_path,m.entry_index,m.error FROM archive_members m JOIN files f ON f.id=m.archive_id LEFT JOIN roots r ON r.id=f.root WHERE m.state<>'hashed' AND COALESCE(json_extract(f.data,'$.present'),1)=1)";
        else {std::string copies="SELECT f.id*2 AS id,'file' AS kind,'F'||f.id AS file_id,f.root,r.role,f.absolute_path,NULL AS entry_path,NULL AS entry_index,f.sha256,f.bytes FROM files f LEFT JOIN roots r ON r.id=f.root WHERE f.deleted=0 AND f.sha256 IS NOT NULL AND COALESCE(json_extract(f.data,'$.source_code'),0)=1";
            const bool zip=p.config.value("code_duplicate_mode","off")=="files_and_zip";
            if(zip)copies+=" UNION ALL SELECT m.id*2+1 AS id,'zip' AS kind,'F'||f.id AS file_id,f.root,r.role,f.absolute_path,m.entry_path,m.entry_index,m.sha256,m.bytes FROM archive_members m JOIN files f ON f.id=m.archive_id LEFT JOIN roots r ON r.id=f.root WHERE m.state='hashed' AND COALESCE(json_extract(f.data,'$.present'),1)=1 AND f.sha256 IS NOT NULL";
            const auto count=zip?std::string("h.file_copies+h.zip_copies"):std::string("h.file_copies");
            from="(SELECT c.*,"+count+" AS copy_count FROM("+copies+")c JOIN code_hash_counts h ON h.sha256=c.sha256 AND h.bytes=c.bytes WHERE "+count+">1)";}
        if(args.contains("root")){where+=" AND root=?";params.push_back(args["root"]);}if(args.contains("role")){where+=" AND role=?";params.push_back(args["role"]);}
        const auto mode=p.config.value("code_duplicate_mode","off");
        if(mode=="off"||(kind=="code_duplicate_errors"&&mode!="files_and_zip"))where+=" AND 0";
        else if(mode=="files"&&kind=="code_duplicates")where+=" AND kind='file'";
    }
    else if(kind=="phases"){
        from="(SELECT rowid AS id,id AS phase_id,title,ordinal,state,description,gate_builds,gate_cases,target_modules,updated_at,actor,revision FROM project_phases)";
        if(args.contains("state")){where+=" AND state=?";params.push_back(args["state"]);}
    }
    else if(kind=="project_tasks"){
        from="(SELECT rowid AS id,id AS task_id,phase_id,title,kind AS task_kind,state,priority,assignee,source_entities,target_entities,required_cases,depends_on,note,created_at,updated_at,actor,revision FROM project_tasks)";
        if(args.contains("phase_id")){where+=" AND phase_id=?";params.push_back(args["phase_id"]);}
        if(args.contains("state")){where+=" AND state=?";params.push_back(args["state"]);}
    }
    else if(kind=="doc_audits"){
        from="(SELECT f.id AS id,'F'||f.id AS doc_id,f.root,f.path,f.absolute_path,COALESCE(CASE WHEN d.doc_id IS NULL THEN 'UNREVIEWED' WHEN d.sha256<>f.sha256 THEN 'STALE' ELSE d.state END,'UNREVIEWED') AS state,COALESCE(d.disposition,'unreviewed') AS disposition,COALESCE(d.linked_targets,'[]') AS linked_targets,COALESCE(d.extracted_tasks,'[]') AS extracted_tasks,COALESCE(d.note,'') AS note,COALESCE(d.actor,'') AS actor,COALESCE(d.at,'') AS at FROM files f LEFT JOIN roots r ON r.id=f.root LEFT JOIN doc_audits d ON d.doc_id='F'||f.id WHERE f.deleted=0 AND r.role='source' AND f.category='documentation')";
        if(args.contains("state")){where+=" AND state=?";params.push_back(args["state"]);}
        if(args.contains("root")){where+=" AND root=?";params.push_back(args["root"]);}
    }
    else return std::nullopt;
    if(args.contains("text")&&kind!="search"){const auto text=args["text"].get<std::string>();if(!text.empty()){
        if(kind=="census_entries"||kind=="directories"||kind=="folder_coverage"){where+=" AND instr(lower(path),lower(?))>0";params.push_back(text);}
        else if(kind=="phases"){where+=" AND (instr(lower(title),lower(?))>0 OR instr(lower(description),lower(?))>0 OR instr(lower(phase_id),lower(?))>0)";params.push_back(text);params.push_back(text);params.push_back(text);}
        else if(kind=="project_tasks"){where+=" AND (instr(lower(title),lower(?))>0 OR instr(lower(note),lower(?))>0 OR instr(lower(task_id),lower(?))>0)";params.push_back(text);params.push_back(text);params.push_back(text);}
        else if(kind=="doc_audits"){where+=" AND (instr(lower(path),lower(?))>0 OR instr(lower(note),lower(?))>0)";params.push_back(text);params.push_back(text);}
        else if(kind=="reuse_candidates"){where+=" AND (instr(lower(COALESCE(source_path,'')),lower(?))>0 OR instr(lower(COALESCE(output_path,'')),lower(?))>0)";params.push_back(text);params.push_back(text);}
        else if(kind=="archive_members"){where+=" AND (instr(lower(absolute_path),lower(?))>0 OR instr(lower(entry_path),lower(?))>0)";params.push_back(text);params.push_back(text);}
        else if(kind=="scan_errors"){where+=" AND (instr(lower(path),lower(?))>0 OR instr(lower(message),lower(?))>0)";params.push_back(text);params.push_back(text);}
        else if(kind=="dependencies"){where+=" AND instr(lower(name),lower(?))>0";params.push_back(text);}
        else if(kind=="relations"){
            // AUD-011: relations subquery has source_path, target_path, reason (no 'name' column!)
            where+=" AND (instr(lower(source_path),lower(?))>0 OR instr(lower(target_path),lower(?))>0 OR instr(lower(reason),lower(?))>0)";
            params.push_back(text);params.push_back(text);params.push_back(text);
        }
    }}
    int64_t last=args.value("after_id",int64_t(0));const auto fingerprint=sha256(from+where+params.dump()).substr(0,16);
    const std::string cursor_head=pinned_gen.has_value()?("v2:"+std::to_string(*pinned_gen)+":"+fingerprint+":"):("v2:"+fingerprint+":");
    if(!cur.empty()){if(!cur.starts_with(cursor_head))throw std::runtime_error("CURSOR_FILTER_MISMATCH");last=std::stoll(cur.substr(cursor_head.size()));}
    const int limit=std::clamp(args.value("limit",100),1,1000);auto total=args.value("include_total",false)?Json(db.integer("SELECT COUNT(*) FROM "+from+where,params)):Json(nullptr);
    where+=" AND id>?";params.push_back(last);params.push_back(limit+1);auto rows=db.query("SELECT * FROM "+from+where+" ORDER BY "+order+" LIMIT ?",params);const bool more=rows.size()>static_cast<size_t>(limit);if(more)rows.erase(rows.end()-1);
    const bool enrich_coverage=(kind=="folder_coverage")||((kind=="census_entries"||kind=="directories")&&args.value("with_coverage",false));
    Json coverage_summary;
    if(enrich_coverage){
        std::map<int64_t,std::string> gen_times;
        for(const auto& g:db.query("SELECT id,COALESCE(published,created) AS at FROM inventory_generations"))gen_times[g["id"].get<int64_t>()]=g["at"].is_null()?std::string():g["at"].get<std::string>();
        std::set<std::string> excluded_dirs;
        for(const auto& x:p.config.value("exclude_dirs",Json::array()))if(x.is_string())excluded_dirs.insert(lower(x.get<std::string>()));
        const auto latest_job=scalar(db,"SELECT id FROM inventory_jobs WHERE generation>0 ORDER BY created DESC LIMIT 1");
        const bool any_pending_dirs=!latest_job.empty()&&db.integer("SELECT EXISTS(SELECT 1 FROM inventory_dirs WHERE job_id=? AND state<>'done')",{latest_job})!=0;
        const bool any_errors=db.integer("SELECT EXISTS(SELECT 1 FROM scan_errors)")!=0;
        Statement child_count_q(db,"SELECT COUNT(*) AS n FROM census_entries INDEXED BY census_parent WHERE generation=? AND root=? AND parent=?");
        Statement file_meta_q(db,"SELECT analysis,category,relevant,deleted,data FROM files WHERE id=?");
        Statement pending_dir_q(db,"SELECT COUNT(*) AS n FROM inventory_dirs WHERE job_id=? AND root=? AND state<>'done' AND (path=? OR (path>=? AND path<?))");
        Statement error_dir_q(db,"SELECT COUNT(*) AS n FROM scan_errors WHERE root=? AND (path=? COLLATE NOCASE OR (path>=? COLLATE NOCASE AND path<? COLLATE NOCASE))");
        Statement error_file_q(db,"SELECT COUNT(*) AS n FROM scan_errors WHERE root=? AND path=? COLLATE NOCASE");
        int64_t sum_discovered=0,sum_pending=0,sum_errors=0,sum_excluded=0;
        for(auto& row:rows){
            const auto r_kind=row.value("kind",std::string());
            const auto r_root=row.value("root",std::string());
            const auto r_path=row.value("path",std::string());
            const auto r_abs=row.value("absolute_path",std::string());
            const auto r_gen=row.value("generation",int64_t(0));
            const auto r_attrs=row.value("attrs",int64_t(0));
            row["last_scanned"]=gen_times.contains(r_gen)?Json(gen_times[r_gen]):Json("");
            bool path_excluded=false;
            for(const auto& part:fs::path(wide(r_path)))if(excluded_dirs.contains(lower(utf8(part.wstring())))){path_excluded=true;break;}
            int64_t disc=0,pend=0,errs=0,excl=0;
            std::string cause="CUBIERTA";
            if(r_kind=="directory"||r_kind=="directory_link"){
                child_count_q.bind({r_gen,r_root,r_path});
                if(child_count_q.next())disc=child_count_q.row()["n"].get<int64_t>();
                child_count_q.reset();
                const auto pfx=r_path.empty()?std::string():r_path+"/";
                if(any_pending_dirs){
                    pending_dir_q.bind({latest_job,r_root,r_path,pfx,pfx+"\xF4\x8F\xBF\xBF"});
                    if(pending_dir_q.next())pend=pending_dir_q.row()["n"].get<int64_t>();
                    pending_dir_q.reset();
                }
                if(any_errors){
                    const auto apfx=r_abs+"/";
                    const auto apfx_bs=r_abs+"\\";
                    error_dir_q.bind({r_root,r_abs,apfx,apfx+"\xF4\x8F\xBF\xBF"});
                    if(error_dir_q.next())errs=error_dir_q.row()["n"].get<int64_t>();
                    error_dir_q.reset();
                    error_dir_q.bind({r_root,r_abs,apfx_bs,apfx_bs+"\xF4\x8F\xBF\xBF"});
                    if(error_dir_q.next())errs+=error_dir_q.row()["n"].get<int64_t>();
                    error_dir_q.reset();
                }
                if(path_excluded)excl=disc>0?disc:1;
                if(errs>0)cause="ERROR_DE_ACCESO";
                else if(pend>0)cause="CARPETA_PENDIENTE";
                else if(r_kind=="directory_link")cause="ENLACE_NO_SEGUIDO";
                else if(path_excluded)cause="EXCLUIDA_POR_FILTRO";
                else if(r_attrs&FILE_ATTRIBUTE_HIDDEN)cause="CARPETA_OCULTA";
                else if(disc==0)cause="CARPETA_VACIA";
            }else{
                disc=1;
                std::string analysis_st="PENDING",cat="unknown",excl_reason,ana_note;
                bool relv=false,fdel=false,has_err=false;
                if(!row["file_id"].is_null()){
                    file_meta_q.bind(Json::array({row["file_id"]}));
                    if(file_meta_q.next()){
                        const auto& fr=file_meta_q.row();
                        analysis_st=fr["analysis"].is_null()?"PENDING":fr["analysis"].get<std::string>();
                        cat=fr["category"].is_null()?"unknown":fr["category"].get<std::string>();
                        relv=fr["relevant"]==1;fdel=fr["deleted"]==1;
                        auto fd=safe_parse(fr["data"]);
                        excl_reason=fd.value("exclusion_reason","");
                        ana_note=fd.value("analysis_note","");
                        has_err=fd.contains("analysis_error");
                    }
                    file_meta_q.reset();
                }
                if(any_errors){
                    error_file_q.bind({r_root,r_abs});
                    if(error_file_q.next())errs=error_file_q.row()["n"].get<int64_t>();
                    error_file_q.reset();
                }
                if(has_err||analysis_st=="PARTIAL")++errs;
                if(!excl_reason.empty()||path_excluded||fdel)excl=1;
                if(errs>0)cause="ERROR_DE_ACCESO";
                else if(r_kind=="file_link")cause="ENLACE_NO_SEGUIDO";
                else if(!excl_reason.empty())cause=excl_reason;
                else if(path_excluded)cause="EXCLUIDA_POR_FILTRO";
                else if(analysis_st=="PARTIAL")cause="ANALISIS_PARCIAL";
                else if(relv&&analysis_st!="ANALYZED")cause="ANALISIS_PENDIENTE";
                else if(cat=="unknown"&&fs::path(wide(r_abs)).extension().empty())cause="SIN_EXT_DESCONOCIDO";
                else if(ana_note=="NO_DEEP_ANALYZER"||analysis_st=="UNSUPPORTED")cause="SIN_ANALIZADOR";
                else if(r_attrs&FILE_ATTRIBUTE_HIDDEN)cause="ARCHIVO_OCULTO";
            }
            row["discovered_entries"]=disc;
            row["pending_directories"]=pend;
            row["errors_count"]=errs;
            row["excluded_entries"]=excl;
            row["coverage_cause"]=cause;
            sum_discovered+=disc;sum_pending+=pend;sum_errors+=errs;sum_excluded+=excl;
        }
        coverage_summary={{"discovered_entries",sum_discovered},{"pending_directories",sum_pending},{"errors_count",sum_errors},{"excluded_entries",sum_excluded},{"page_entries",rows.size()}};
    }
    Json row_cursors=Json::array();for(auto& row:rows){row_cursors.push_back(cursor_head+std::to_string(row["id"].get<int64_t>()));
        if((kind=="history"||kind=="changes")&&row.contains("detail"))row["detail"]=safe_parse(row["detail"]);
        if(kind=="mappings"){
            row["targets"]=safe_parse(row["targets_json"]);row["cases"]=safe_parse(row["cases_json"]);row["target_paths"]=safe_parse(row["target_paths_json"]);
            row.erase("targets_json");row.erase("cases_json");row.erase("target_paths_json");
        }
        else if(kind=="phases"){for(const auto* k:{"gate_builds","gate_cases","target_modules"})if(row.contains(k))row[k]=safe_parse(row[k]);}
        else if(kind=="project_tasks"){for(const auto* k:{"source_entities","target_entities","required_cases","depends_on"})if(row.contains(k))row[k]=safe_parse(row[k]);}
        else if(kind=="doc_audits"){for(const auto* k:{"linked_targets","extracted_tasks"})if(row.contains(k))row[k]=safe_parse(row[k]);}
        auto fields=args.value("fields",Json::array());if(!fields.empty()){Json selected=Json::object();for(const auto& field:fields)if(row.contains(field.get<std::string>()))selected[field.get<std::string>()]=row[field.get<std::string>()];row=selected;}}
    Json result={{"revision",db.revision()},{"total",total},{"items",rows},{"cursor",cur.empty()?cursor_head+"0":cur},
        {"row_cursors",row_cursors},{"next_cursor",more&&!row_cursors.empty()?row_cursors.back():Json(nullptr)}};
    if(enrich_coverage)result["coverage_summary"]=coverage_summary;
    if(kind=="code_duplicates"||kind=="code_duplicate_errors"){
        const auto mode=p.config.value("code_duplicate_mode","off");const auto config=sha256(p.config.dump());
        const bool all_roots_checked=db.integer("SELECT EXISTS(SELECT 1 FROM inventory_jobs WHERE scope='{}' AND config_sha256=? AND census_complete=1 AND state IN('completed','completed_with_errors') AND phase='finished')",{config})!=0;
        const auto archive_errors=mode=="files_and_zip"?db.integer("SELECT COUNT(*) FROM archive_members m JOIN files f ON f.id=m.archive_id WHERE m.state<>'hashed' AND COALESCE(json_extract(f.data,'$.present'),1)=1"):0;
        const bool pending_tasks=db.integer("SELECT EXISTS(SELECT 1 FROM inventory_tasks t JOIN inventory_jobs j ON j.id=t.job_id WHERE j.generation=(SELECT MAX(generation) FROM inventory_jobs) AND t.state<>'done')")!=0;
        const bool fresh=db.integer("SELECT COALESCE(MAX(CAST(value AS INTEGER)),0) FROM meta WHERE key='dirty'")==0&&db.integer("SELECT EXISTS(SELECT 1 FROM inventory_jobs WHERE generation>0 AND state IN('queued','running','paused'))")==0;
        const bool hashed=db.integer("SELECT EXISTS(SELECT 1 FROM files WHERE deleted=0 AND COALESCE(json_extract(data,'$.source_code'),0)=1 AND sha256 IS NULL)")==0;
        result["report"]={{"mode",mode},{"available",mode!="off"&&all_roots_checked},{"fresh",fresh},{"complete",mode!="off"&&all_roots_checked&&fresh&&!pending_tasks&&hashed&&archive_errors==0},{"error_count",archive_errors}};
    }
    return result;
}
}
