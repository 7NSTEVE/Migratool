#include "migratool/gateway.hpp"
#include "migratool/sql.hpp"
#include "migratool/scale.hpp"
#include "migratool/fs_index.hpp"
#include "migratool/memories.hpp"
#include "migratool/file_toolkit.hpp"
#include "migratool/workspace_registry.hpp"
#include "migratool/lsp_client.hpp"
#include "migratool/housekeep.hpp"
#include "migratool/duplicates.hpp"
#include "migratool/ast_engine.hpp"
#include <algorithm>
#include <map>
#include <set>
#include <deque>
#include <sstream>
#include <fstream>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <regex>

namespace mt {
namespace {
std::string low_str(std::string s){for(char& c:s)c=static_cast<char>(tolower(static_cast<unsigned char>(c)));return s;}

int64_t entity_number(const std::string& id,char type){
    if(id.size()<2||id[0]!=type||id.find_first_not_of("0123456789",1)!=std::string::npos)throw std::runtime_error("Identificador inválido: "+id);
    return std::stoll(id.substr(1));
}
Json parse(const Json& v){return v.is_string()?Json::parse(v.get<std::string>()):v;}
Json safe_parse(const Json& v){
    if(v.is_null())return Json::object();
    if(!v.is_string())return v;
    const auto& s=v.get_ref<const std::string&>();
    if(s.empty())return Json::object();
    try{return Json::parse(s);}catch(...){return Json::object();}
}

Json entity(Database& db,const std::string& id){
    Json rows;
    if(id.size()>=2&&id[0]=='F'&&id.find_first_not_of("0123456789",1)==std::string::npos)
        rows=db.query("SELECT * FROM files WHERE id=? AND deleted=0",{entity_number(id,'F')});
    else if(id.size()>=2&&id[0]=='S'&&id.find_first_not_of("0123456789",1)==std::string::npos)
        rows=db.query("SELECT f.*,s.id AS symbol_id,s.stable_key AS symbol_stable_key,s.name AS symbol_name,s.qualified_name AS symbol_qualified_name,s.kind AS symbol_kind,s.line AS symbol_line,s.data AS symbol_data FROM symbols s JOIN files f ON s.file_id=f.id WHERE s.id=? AND s.active=1 AND f.deleted=0",{entity_number(id,'S')});
    else {
        rows=db.query("SELECT * FROM files WHERE absolute_path=? AND deleted=0",{id});
        if(rows.empty())rows=db.query("SELECT * FROM files WHERE path=? COLLATE NOCASE AND deleted=0",{id});
        if(rows.empty()){
            auto legacy=id.find(':');
            if(legacy!=std::string::npos)rows=db.query("SELECT * FROM files WHERE root=? AND path=? AND deleted=0",{id.substr(0,legacy),id.substr(legacy+1)});
        }
        if(rows.empty()){
            auto hash_pos=id.find('#');
            if(hash_pos!=std::string::npos&&id.starts_with("F")){
                rows=db.query("SELECT f.*,s.id AS symbol_id,s.stable_key AS symbol_stable_key,s.name AS symbol_name,s.qualified_name AS symbol_qualified_name,s.kind AS symbol_kind,s.line AS symbol_line,s.data AS symbol_data FROM symbols s JOIN files f ON s.file_id=f.id WHERE s.stable_key=? AND s.active=1 AND f.deleted=0",{id});
            }
        }
    }
    if(rows.empty())throw std::runtime_error("Unidad no existe o fue eliminada: "+id);
    auto out=rows[0];
    out["entity_id"]=out.contains("symbol_id")?Json("S"+std::to_string(out["symbol_id"].get<int64_t>())):Json("F"+std::to_string(out["id"].get<int64_t>()));
    return out;
}

std::string file_id(const Json& row){return "F"+std::to_string(row.at("id").get<int64_t>());}
std::string root_role(Database& db,const std::string& root){auto r=db.query("SELECT role FROM roots WHERE id=?",{root});return r.empty()?"UNKNOWN":r[0]["role"].get<std::string>();}
Json meta(Database& db,const std::string& key,const Json& fallback){auto rows=db.query("SELECT value FROM meta WHERE key=?",{key});if(rows.empty())return fallback;try{return parse(rows[0]["value"]);}catch(...){return rows[0]["value"];}}

std::string iso_utc_plus_seconds(int64_t add_seconds) {
    auto tp = std::chrono::system_clock::now() + std::chrono::seconds(add_seconds);
    auto tt = std::chrono::system_clock::to_time_t(tp);
    std::tm gmt{};
    gmtime_s(&gmt, &tt);
    char buf[32]{};
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &gmt);
    return buf;
}

bool has_claims_table(Database& db) {
    return db.integer("SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='task_claims'") > 0;
}

void ensure_claims_table(Database& db) {
    db.exec("CREATE TABLE IF NOT EXISTS task_claims(entity TEXT PRIMARY KEY,actor TEXT NOT NULL,session_id TEXT,claimed_at TEXT NOT NULL,expires_at TEXT NOT NULL,lease_seconds INTEGER NOT NULL,note TEXT,revision INTEGER NOT NULL)");
}

Json get_active_claim(Database& db, const std::string& entity_id, const std::string& parent_file_id = "") {
    if (!has_claims_table(db)) return nullptr;
    auto cur_time = now();
    Json rows;
    if (!parent_file_id.empty() && parent_file_id != entity_id) {
        rows = db.query("SELECT * FROM task_claims WHERE (entity=? OR entity=?) AND expires_at>? ORDER BY expires_at DESC LIMIT 1", {entity_id, parent_file_id, cur_time});
    } else {
        rows = db.query("SELECT * FROM task_claims WHERE entity=? AND expires_at>? LIMIT 1", {entity_id, cur_time});
    }
    return rows.empty() ? Json(nullptr) : rows[0];
}

void enforce_not_claimed_by_other(Database& db, const std::string& entity_id, const std::string& parent_file_id, const std::string& actor) {
    auto claim = get_active_claim(db, entity_id, parent_file_id);
    if (!claim.is_null() && claim["actor"].get<std::string>() != actor) {
        throw std::runtime_error("TASK_CLAIM_CONFLICT: la unidad " + claim["entity"].get<std::string>() +
                                 " está reclamada por '" + claim["actor"].get<std::string>() +
                                 "' hasta " + claim["expires_at"].get<std::string>());
    }
}

bool hashes_match(Database& db,const Json& hashes){
    if(hashes.empty())return false;
    bool any_checked=false;
    for(auto it=hashes.begin();it!=hashes.end();++it){
        const auto& k=it.key();
        if(k.starts_with("_"))continue;
        if(k.starts_with("F")){
            any_checked=true;
            auto row=db.query("SELECT sha256 FROM files WHERE id=? AND deleted=0",{entity_number(k,'F')});
            if(row.empty()||row[0]["sha256"]!=it.value())return false;
        }else if(k.starts_with("S")){
            any_checked=true;
            auto row=db.query("SELECT data FROM symbols WHERE id=? AND active=1",{entity_number(k,'S')});
            if(row.empty())return false;
            auto sdata=parse(row[0]["data"]);
            if(sdata.contains("symbol_sha256")&&sdata["symbol_sha256"]!=it.value())return false;
        }
    }
    return any_checked;
}

struct BatchVerifyCache {
    std::map<std::string, std::string> verified_disk_hashes;
    std::map<std::string, std::string> root_roles;
    std::set<std::string> configured_cases;
    bool initialized = false;
    bool auto_sync_touched = false;
    void ensure_init(Database& db, const Project& p) {
        if (initialized) return;
        initialized = true;
        for (const auto& r : db.query("SELECT id,role FROM roots")) {
            if (r["id"].is_string() && r["role"].is_string())
                root_roles[r["id"].get<std::string>()] = r["role"].get<std::string>();
        }
        for (const auto& c : p.config.value("cases", Json::array())) {
            if (c.contains("id") && c["id"].is_string())
                configured_cases.insert(c["id"].get<std::string>());
        }
    }
    std::string role_of(Database& db, const std::string& root) {
        auto it = root_roles.find(root);
        if (it != root_roles.end()) return it->second;
        auto role = root_role(db, root);
        root_roles[root] = role;
        return role;
    }
};

std::string normalize_ident_match(std::string_view sv) {
    std::string out;
    out.reserve(sv.size());
    for (char c : sv) {
        if (c == '_') continue;
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

void refresh_file_symbols_inline(Database& db, int64_t fid, const std::string& root_id, const std::string& rel_path,
                                 const std::string& abs_path, const std::string& lang, const std::string& content) {
    try {
        auto res = syntactic_ast_scan_file(fs::path(wide(abs_path)), lang);
        if (!res.contains("symbols") || !res["symbols"].is_array()) return;
        db.exec("UPDATE symbols SET active=0 WHERE file_id=?", {fid});
        std::string fid_str = "F" + std::to_string(fid);
        for (auto sym : res["symbols"]) {
            std::string sname = sym.value("name", "");
            std::string qname = sym.value("qualified_name", sname);
            std::string sig = sym.value("signature", "");
            std::string skind = sym.value("kind", "function");
            if (sym.contains("offset") && sym.contains("end_offset")) {
                size_t off = sym["offset"].get<size_t>(), end_off = sym["end_offset"].get<size_t>();
                if (end_off > off && end_off <= content.size()) {
                    sym["symbol_sha256"] = sha256(content.substr(off, end_off - off));
                }
            }
            sym["root"] = root_id;
            sym["file"] = rel_path;
            sym["absolute_path"] = abs_path;
            sym["file_id"] = fid_str;
            sym["engine"] = "syntactic_ast/" + lang;
            std::string stable = fid_str + "#" + sha256(qname + "|" + sig + "|" + skind).substr(0, 24);
            sym["id"] = stable;
            db.exec("INSERT INTO symbols(file_id,stable_key,name,qualified_name,kind,line,data,active)VALUES(?,?,?,?,?,?,?,1) ON CONFLICT(file_id,stable_key)DO UPDATE SET name=excluded.name,qualified_name=excluded.qualified_name,kind=excluded.kind,line=excluded.line,data=excluded.data,active=1",
                    {fid, stable, sname, qname, skind, sym.value("line", 1), sym.dump()});
        }
        db.exec("UPDATE files SET analysis='ANALYZED' WHERE id=?", {fid});
    } catch (...) {}
}

Json entity_or_sync(Database& db, const Project& p, const std::string& id_or_path, bool auto_sync) {
    try {
        return entity(db, id_or_path);
    } catch (...) {
        if (!auto_sync) throw;
    }
    fs::path candidate(wide(id_or_path));
    for (const auto& root : p.config.value("roots", Json::array())) {
        fs::path base = p.resolve(root.at("path"));
        fs::path resolved = candidate.is_absolute() ? candidate : (base / candidate);
        std::error_code ec;
        if (!fs::is_regular_file(resolved, ec)) continue;
        resolved = fs::weakly_canonical(resolved, ec);
        fs::path canon_base = fs::weakly_canonical(base, ec);
        if (!within(resolved, canon_base)) continue;
        std::string abs_str = path_text(resolved);
        std::string rel_str = path_text(resolved.lexically_relative(canon_base));
        std::string rid = root.at("id").get<std::string>();
        auto existing = db.query("SELECT * FROM files WHERE absolute_path=?", {abs_str});
        std::string disk_hash = hash_file(resolved);
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        int64_t disk_bytes = 0, disk_mtime = 0;
        if (GetFileAttributesExW(native_path(resolved).c_str(), GetFileExInfoStandard, &fad)) {
            const auto join_u64 = [](DWORD hi, DWORD lo) -> int64_t { return static_cast<int64_t>((static_cast<uint64_t>(hi) << 32) | static_cast<uint64_t>(lo)); };
            disk_bytes = join_u64(fad.nFileSizeHigh, fad.nFileSizeLow);
            disk_mtime = join_u64(fad.ftLastWriteTime.dwHighDateTime, fad.ftLastWriteTime.dwLowDateTime);
        }
        auto cl = classify_inventory_file(p, resolved, canon_base, root.value("include_all_code", false));
        int64_t fid_num = 0;
        if (existing.empty()) {
            Json fdata = cl;
            fdata["present"] = true;
            fdata["root"] = rid;
            fdata["path"] = rel_str;
            fdata["absolute_path"] = abs_str;
            fdata["sha256"] = disk_hash;
            fdata["bytes"] = disk_bytes;
            fdata["mtime"] = disk_mtime;
            db.exec("INSERT INTO files(root,path,absolute_path,sha256,bytes,mtime,category,language,relevant,git_state,build_state,analysis,deleted,first_revision,last_revision,data)VALUES(?,?,?,?,?,?,?,?,?,'UNKNOWN','UNKNOWN','ANALYZED',0,?,?,?)",
                    {rid, rel_str, abs_str, disk_hash, disk_bytes, disk_mtime, cl["category"], cl["language"], cl["relevant"], db.revision(), db.revision(), fdata.dump()});
            fid_num = db.integer("SELECT last_insert_rowid()");
        } else {
            fid_num = existing[0]["id"].get<int64_t>();
            db.exec("UPDATE files SET deleted=0,sha256=?,bytes=?,mtime=?,category=?,language=?,relevant=?,analysis='ANALYZED',last_revision=? WHERE id=?",
                    {disk_hash, disk_bytes, disk_mtime, cl["category"], cl["language"], cl["relevant"], db.revision(), fid_num});
        }
        if (cl.value("relevant", false) && disk_bytes <= 16 * 1024 * 1024) {
            std::string content = read_text(resolved, 16 * 1024 * 1024);
            refresh_file_symbols_inline(db, fid_num, rid, rel_str, abs_str, cl.value("language", ""), content);
        }
        return entity(db, "F" + std::to_string(fid_num));
    }
    throw std::runtime_error("Unidad no existe o fue eliminada: " + id_or_path);
}

Json mapping_json(const Json& r){
    auto raw_hashes=parse(r["hashes"]);
    Json file_hashes=Json::object(), sym_hashes=Json::object();
    std::string disposition="migrated", external_replacement;
    std::string stage=(r["complete"]==1)?"integrated":"ported";
    for(auto it=raw_hashes.begin();it!=raw_hashes.end();++it){
        if(it.key()=="_disposition"&&it.value().is_string())disposition=it.value().get<std::string>();
        else if(it.key()=="_external_replacement"&&it.value().is_string())external_replacement=it.value().get<std::string>();
        else if(it.key()=="_stage"&&it.value().is_string())stage=it.value().get<std::string>();
        else if(it.key().starts_with("S"))sym_hashes[it.key()]=it.value();
        else if(it.key().starts_with("F"))file_hashes[it.key()]=it.value();
    }
    Json out={{"source",r["source"]},{"targets",parse(r["targets"])},{"disposition",disposition},{"stage",stage},{"declared_complete",r["complete"]==1},
              {"hashes",file_hashes},{"symbol_hashes",sym_hashes},{"cases",parse(r["cases"])},
              {"note",r["note"]},{"actor",r["actor"]},{"at",r["at"]},{"revision",r["revision"]}};
    if(!external_replacement.empty())out["external_replacement"]=external_replacement;
    return out;
}

Json map_in_transaction(Database& db,const Project& p,const Json& op,const std::string& actor,BatchVerifyCache* cache=nullptr,std::optional<Json> pre_source=std::nullopt){
    if(cache)cache->ensure_init(db,p);
    const bool auto_sync=op.value("auto_sync_touched",false)||(cache&&cache->auto_sync_touched);
    auto source=pre_source.has_value()?std::move(*pre_source):entity_or_sync(db,p,op.at("source"),auto_sync);
    const auto src_role=cache?cache->role_of(db,source.at("root")):root_role(db,source.at("root"));
    if(src_role!="source")throw std::runtime_error("El origen debe pertenecer a una raíz source");
    enforce_not_claimed_by_other(db, source["entity_id"].get<std::string>(), file_id(source), actor);
    auto current=[&](Json& file){
        const std::string abs_str=file.at("absolute_path").get<std::string>();
        std::string disk_hash;
        if(cache){
            auto hit=cache->verified_disk_hashes.find(abs_str);
            if(hit!=cache->verified_disk_hashes.end())disk_hash=hit->second;
        }
        if(disk_hash.empty()){
            const auto path=fs::path(wide(abs_str));
            if(!fs::is_regular_file(path))throw std::runtime_error("INDEX_STALE: sincronice antes de marcar "+path_text(path));
            disk_hash=hash_file(path);
            if(cache)cache->verified_disk_hashes[abs_str]=disk_hash;
        }
        const auto& recorded=file.at("sha256");
        if(recorded.is_string()&&!recorded.get_ref<const std::string&>().empty()){
            if(disk_hash!=recorded.get_ref<const std::string&>()){
                if(!auto_sync)throw std::runtime_error("INDEX_STALE: sincronice antes de marcar "+abs_str);
                const auto path=fs::path(wide(abs_str));
                WIN32_FILE_ATTRIBUTE_DATA fad{};
                int64_t d_bytes=0,d_mtime=0;
                if(GetFileAttributesExW(native_path(path).c_str(),GetFileExInfoStandard,&fad)){
                    const auto join_u64=[](DWORD hi,DWORD lo)->int64_t{return static_cast<int64_t>((static_cast<uint64_t>(hi)<<32)|static_cast<uint64_t>(lo));};
                    d_bytes=join_u64(fad.nFileSizeHigh,fad.nFileSizeLow);
                    d_mtime=join_u64(fad.ftLastWriteTime.dwHighDateTime,fad.ftLastWriteTime.dwLowDateTime);
                }
                db.exec("UPDATE files SET sha256=?,bytes=?,mtime=?,last_revision=? WHERE id=?",{disk_hash,d_bytes,d_mtime,db.revision(),file.at("id")});
                file["sha256"]=disk_hash;
                if(d_bytes<=16*1024*1024){
                    std::string content=read_text(path,16*1024*1024);
                    refresh_file_symbols_inline(db,file.at("id").get<int64_t>(),file.at("root").get<std::string>(),file.at("path").get<std::string>(),abs_str,file.value("language",""),content);
                    if(file.contains("symbol_id")){
                        auto srows=db.query("SELECT data FROM symbols WHERE id=?",{file["symbol_id"]});
                        if(!srows.empty())file["symbol_data"]=srows[0]["data"];
                    }
                }
            }
            return;
        }
        const auto path=fs::path(wide(abs_str));
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if(!GetFileAttributesExW(native_path(path).c_str(),GetFileExInfoStandard,&fad))
            throw std::runtime_error("INDEX_STALE: sincronice antes de marcar "+path_text(path));
        const auto join_u64=[](DWORD hi,DWORD lo)->int64_t{return static_cast<int64_t>((static_cast<uint64_t>(hi)<<32)|static_cast<uint64_t>(lo));};
        const int64_t disk_bytes=join_u64(fad.nFileSizeHigh,fad.nFileSizeLow);
        const int64_t disk_mtime=join_u64(fad.ftLastWriteTime.dwHighDateTime,fad.ftLastWriteTime.dwLowDateTime);
        if(!auto_sync&&((file.contains("bytes")&&file["bytes"].is_number_integer()&&file["bytes"].get<int64_t>()!=disk_bytes)||
           (file.contains("mtime")&&file["mtime"].is_number_integer()&&file["mtime"].get<int64_t>()!=disk_mtime))){
            throw std::runtime_error("INDEX_STALE: sincronice antes de marcar "+path_text(path));
        }
        db.exec("UPDATE files SET sha256=?,bytes=?,mtime=? WHERE id=?",{disk_hash,disk_bytes,disk_mtime,file.at("id")});
        file["sha256"]=disk_hash;
    };
    current(source);

    std::string disposition = op.value("disposition", "migrated");
    const std::set<std::string> valid_dispositions = {"migrated", "intentionally_removed", "replaced_by_external", "merged_into"};
    if (!valid_dispositions.contains(disposition)) throw std::runtime_error("Disposición inválida: " + disposition);
    const bool targetless_allowed = (disposition == "intentionally_removed" || disposition == "replaced_by_external");
    if (targetless_allowed && op.value("note", "").empty())
        throw std::runtime_error("La disposición '" + disposition + "' exige justificación explícita en note");
    if (!targetless_allowed && (!op.contains("targets") || !op["targets"].is_array() || op["targets"].empty()))
        throw std::runtime_error("Correspondencia requiere targets");

    auto complete=op.value("complete",false);
    std::string stage=op.value("stage", complete ? "integrated" : "ported");
    const std::set<std::string> valid_stages={"planned","half_done","ported","integrated"};
    if(!valid_stages.contains(stage))throw std::runtime_error("Etapa de madurez inválida (use planned, half_done, ported o integrated; verified requiere evidencia de casos): "+stage);
    if(complete && (stage=="planned" || stage=="half_done"))throw std::runtime_error("complete:true no puede declararse con stage=" + stage);

    Json targets=Json::array(),paths=Json::array(),hashes=Json::object(),sym_hashes=Json::object();
    hashes[file_id(source)]=source["sha256"];
    if(source.contains("symbol_data")){
        auto sdata=parse(source["symbol_data"]);
        if(sdata.contains("symbol_sha256")){
            hashes[source["entity_id"].get<std::string>()]=sdata["symbol_sha256"];
            sym_hashes[source["entity_id"].get<std::string>()]=sdata["symbol_sha256"];
        }
    }
    if(disposition!="migrated")hashes["_disposition"]=disposition;
    if(op.contains("external_replacement"))hashes["_external_replacement"]=op["external_replacement"];
    if(op.contains("stage")||stage!=(complete?"integrated":"ported"))hashes["_stage"]=stage;

    for(const auto& id:op.value("targets",Json::array())){
        auto target=entity_or_sync(db,p,id,auto_sync);
        const auto tgt_role=cache?cache->role_of(db,target.at("root")):root_role(db,target.at("root"));
        if(tgt_role!="target")throw std::runtime_error("El destino debe pertenecer a una raíz target");
        current(target);
        targets.push_back(target["entity_id"]);
        paths.push_back(target["absolute_path"]);
        hashes[file_id(target)]=target["sha256"];
        if(target.contains("symbol_data")){
            auto tdata=parse(target["symbol_data"]);
            if(tdata.contains("symbol_sha256")){
                hashes[target["entity_id"].get<std::string>()]=tdata["symbol_sha256"];
                sym_hashes[target["entity_id"].get<std::string>()]=tdata["symbol_sha256"];
            }
        }
    }
    auto cases=op.value("cases",Json::array());
    for(const auto& id:cases){
        const std::string cid=id.get<std::string>();
        bool found=cache?cache->configured_cases.contains(cid):false;
        if(!cache)for(const auto& c:p.config.value("cases",Json::array()))if(c["id"]==cid)found=true;
        if(!found)throw std::runtime_error("Caso desconocido: "+cid);
    }
    auto previous=db.query("SELECT * FROM mappings WHERE source=?",{source["entity_id"]});
    auto at=now();
    Json file_only_hashes=Json::object();
    for(auto it=hashes.begin();it!=hashes.end();++it)if(it.key().starts_with("F"))file_only_hashes[it.key()]=it.value();
    Json next={{"source",source["entity_id"]},{"source_path",source["absolute_path"]},{"targets",targets},{"target_paths",paths},
               {"disposition",disposition},{"stage",stage},{"declared_complete",complete},{"hashes",file_only_hashes},{"symbol_hashes",sym_hashes},
               {"cases",cases},{"note",op.value("note","")},{"actor",actor},{"at",at}};
    auto rev=db.change(actor,complete?"migration_complete":"migration_partial",{{"before",previous.empty()?Json():mapping_json(previous[0])},{"after",next}},source["entity_id"]);
    db.exec("INSERT INTO mappings(source,targets,complete,hashes,cases,note,actor,at,revision)VALUES(?,?,?,?,?,?,?,?,?) ON CONFLICT(source)DO UPDATE SET targets=excluded.targets,complete=excluded.complete,hashes=excluded.hashes,cases=excluded.cases,note=excluded.note,actor=excluded.actor,at=excluded.at,revision=excluded.revision",
        {source["entity_id"],targets.dump(),complete,hashes.dump(),cases.dump(),op.value("note",""),actor,at,rev});
    next["revision"]=rev;
    const auto src_eid=source["entity_id"].get<std::string>();
    for(const auto& sr:db.query("SELECT row_id FROM search_rows WHERE entity_type='mapping' AND entity_id=?",{src_eid}))
        db.exec("DELETE FROM search WHERE rowid=?",{sr["row_id"]});
    db.exec("DELETE FROM search_rows WHERE entity_type='mapping' AND entity_id=?",{src_eid});
    db.exec("INSERT INTO search(entity_type,entity_id,body)VALUES('mapping',?,?)",{src_eid,op.value("note","")+" "+disposition+" "+stage+" "+source["absolute_path"].get<std::string>()+" "+paths.dump()});
    db.exec("INSERT INTO search_rows(entity_type,entity_id,row_id)VALUES('mapping',?,?)",{src_eid,sqlite3_last_insert_rowid(db.native_handle())});
    return next;
}

// Surgical byte-range reader using seekg + read (never loads full 32 MiB file when byte offsets are known)
std::string read_byte_range(const fs::path& p, size_t offset, size_t end_offset, size_t& out_file_bytes) {
    std::ifstream in(p, std::ios::binary | std::ios::ate);
    if (!in) throw std::runtime_error("No se pudo abrir archivo: " + path_text(p));
    auto end_pos = in.tellg();
    out_file_bytes = end_pos > 0 ? static_cast<size_t>(end_pos) : 0;
    if (offset > out_file_bytes) offset = out_file_bytes;
    if (end_offset > out_file_bytes) end_offset = out_file_bytes;
    if (end_offset <= offset) return {};
    size_t len = end_offset - offset;
    std::string buf(len, '\0');
    in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    in.read(buf.data(), static_cast<std::streamsize>(len));
    buf.resize(static_cast<size_t>(in.gcount()));
    return buf;
}

// Helper to extract exact code slice for a symbol or line/byte range with STALE_SOURCE verification
Json extract_symbol_slice(const Json& sym_data, const std::string& abs_path, int context_lines = 0, const std::string& expected_file_sha256 = "", bool enforce_fresh = true) {
    fs::path disk_p(wide(abs_path));
    if (!fs::is_regular_file(disk_p))
        throw std::runtime_error("STALE_SOURCE: archivo no existe en disco: " + abs_path);
    if (sym_data.contains("offset") && sym_data.contains("end_offset") && context_lines == 0) {
        size_t off = sym_data["offset"].get<size_t>();
        size_t end_off = sym_data["end_offset"].get<size_t>();
        size_t file_bytes = 0;
        std::string body = read_byte_range(disk_p, off, end_off, file_bytes);
        if (off <= end_off && end_off <= file_bytes) {
            std::string cur_sym_sha = sha256(body);
            if (enforce_fresh) {
                std::string exp_sym_sha = sym_data.value("symbol_sha256", "");
                if (!exp_sym_sha.empty() && cur_sym_sha != exp_sym_sha) {
                    throw std::runtime_error("STALE_SOURCE: el símbolo en '" + abs_path + "' fue modificado fuera de Migratool (symbol_sha256 actual=" + cur_sym_sha + ", esperado=" + exp_sym_sha + "); ejecute inventory_sync");
                }
                if (!expected_file_sha256.empty() && file_bytes <= 8 * 1024 * 1024 && hash_file(disk_p) != expected_file_sha256) {
                    throw std::runtime_error("STALE_SOURCE: el archivo '" + abs_path + "' fue modificado fuera de Migratool; ejecute inventory_sync");
                }
            }
            return {
                {"body", body},
                {"offset", off},
                {"end_offset", end_off},
                {"line", sym_data.value("line", 1)},
                {"end_line", sym_data.value("end_line", sym_data.value("line", 1))},
                {"byte_length", body.size()},
                {"file_bytes", file_bytes},
                {"symbol_sha256", cur_sym_sha}
            };
        }
        if (enforce_fresh) {
            throw std::runtime_error("STALE_SOURCE: offsets fuera de rango en '" + abs_path + "'; ejecute inventory_sync");
        }
    }
    // Fallback to 1-based line range when context_lines > 0 or byte offsets are absent
    if (enforce_fresh && !expected_file_sha256.empty() && hash_file(disk_p) != expected_file_sha256) {
        throw std::runtime_error("STALE_SOURCE: el archivo '" + abs_path + "' fue modificado fuera de Migratool; ejecute inventory_sync");
    }
    std::string content = read_text(disk_p, 32 * 1024 * 1024);
    size_t file_bytes = content.size();
    std::vector<size_t> line_starts = {0};
    for (size_t i = 0; i < content.size(); ++i)
        if (content[i] == '\n') line_starts.push_back(i + 1);
    int s_line = std::max(1, sym_data.value("line", 1) - context_lines);
    int e_line = std::min(static_cast<int>(line_starts.size()), sym_data.value("end_line", sym_data.value("line", 1)) + context_lines);
    size_t off = line_starts[static_cast<size_t>(s_line - 1)];
    size_t end_off = (static_cast<size_t>(e_line) < line_starts.size()) ? line_starts[static_cast<size_t>(e_line)] : content.size();
    while (end_off > off && (content[end_off - 1] == '\n' || content[end_off - 1] == '\r')) --end_off;
    std::string body = content.substr(off, end_off - off);
    return {
        {"body", body},
        {"offset", off},
        {"end_offset", end_off},
        {"line", s_line},
        {"end_line", e_line},
        {"byte_length", body.size()},
        {"file_bytes", file_bytes},
        {"symbol_sha256", sha256(body)}
    };
}

struct ProjectFileTxLock {
    HANDLE mutex{};
    bool owned{false};
    explicit ProjectFileTxLock(const Project& p, bool acquire = true) {
        if (!acquire) return;
        const auto name = L"Local\\MigratoolBatchFileLock_" + wide(sha256(low_str(path_text(fs::weakly_canonical(p.state_dir())))).substr(0, 32));
        mutex = CreateMutexW(nullptr, FALSE, name.c_str());
        if (mutex) {
            const DWORD r = WaitForSingleObject(mutex, 60000);
            owned = (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED);
        }
    }
    ~ProjectFileTxLock() {
        if (mutex) {
            if (owned) ReleaseMutex(mutex);
            CloseHandle(mutex);
        }
    }
    ProjectFileTxLock(const ProjectFileTxLock&) = delete;
    ProjectFileTxLock& operator=(const ProjectFileTxLock&) = delete;
};

// Durable two-phase journal recovery for crash/power-loss resilience (H-04)
Json recover_pending_journals(const Project& p) {
    Json recovered = Json::array();
    auto journal_dir = p.state_dir() / L"journal";
    std::error_code ec;
    if (!fs::exists(journal_dir, ec) || !fs::is_directory(journal_dir, ec)) return recovered;
    ProjectFileTxLock tx_lock(p, true);
    std::vector<fs::path> jfiles;
    for (const auto& entry : fs::directory_iterator(journal_dir, ec)) {
        if (entry.is_regular_file(ec) && entry.path().extension() == L".json") {
            jfiles.push_back(entry.path());
        }
    }
    std::sort(jfiles.begin(), jfiles.end());
    for (const auto& jf : jfiles) {
        try {
            auto j = read_json(jf);
            std::string state = j.value("state", "PREPARED");
            if (state != "COMPLETED") {
                for (const auto& fitem : j.value("files", Json::array())) {
                    fs::path target_p(wide(fitem.at("absolute_path").get<std::string>()));
                    fs::path bak_p(wide(fitem.at("backup_path").get<std::string>()));
                    std::string exp_sha = fitem.value("before_sha256", "");
                    if (fs::is_regular_file(bak_p, ec)) {
                        std::string bak_bytes = read_text(bak_p, 64 * 1024 * 1024);
                        write_text(target_p, bak_bytes);
                        if (!exp_sha.empty() && hash_file(target_p) != exp_sha) {
                            throw std::runtime_error("JOURNAL_RECOVERY_HASH_MISMATCH: " + path_text(target_p));
                        }
                    }
                    if (fitem.contains("staging_path")) {
                        fs::remove(fs::path(wide(fitem["staging_path"].get<std::string>())), ec);
                    }
                }
                std::string db_bak = j.value("db_backup_path", "");
                if (!db_bak.empty() && fs::is_regular_file(fs::path(wide(db_bak)), ec)) {
                    auto db_path = p.state_dir() / L"catalog.sqlite";
                    auto wal_path = p.state_dir() / L"catalog.sqlite-wal";
                    auto shm_path = p.state_dir() / L"catalog.sqlite-shm";
                    fs::remove(wal_path, ec);
                    fs::remove(shm_path, ec);
                    fs::copy_file(fs::path(wide(db_bak)), db_path, fs::copy_options::overwrite_existing, ec);
                    std::string wal_bak = j.value("db_wal_backup_path", "");
                    if (!wal_bak.empty() && fs::is_regular_file(fs::path(wide(wal_bak)), ec)) {
                        fs::copy_file(fs::path(wide(wal_bak)), wal_path, fs::copy_options::overwrite_existing, ec);
                    }
                }
                auto rec_dir = p.state_dir() / L"journal_recovered";
                fs::create_directories(rec_dir, ec);
                j["state"] = "RECOVERED";
                j["recovered_at"] = now();
                write_json(rec_dir / jf.filename(), j);
                recovered.push_back({{"tx_id", j.value("tx_id", path_text(jf.stem()))}, {"recovered_at", j["recovered_at"]}});
            }
            fs::remove(jf, ec);
        } catch (...) {
            // Leave journal file on disk if locked so next invocation retries
        }
    }
    return recovered;
}

std::string make_unified_diff(const std::string& rel_path, const std::string& before, const std::string& after) {
    if (before == after) return "";
    auto split_lines = [](const std::string& s) {
        std::vector<std::string> out;
        size_t start = 0;
        while (start < s.size()) {
            size_t nl = s.find('\n', start);
            if (nl == std::string::npos) { out.push_back(s.substr(start)); break; }
            std::string line = s.substr(start, nl - start);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            out.push_back(line);
            start = nl + 1;
        }
        return out;
    };
    auto a = split_lines(before), b = split_lines(after);
    size_t prefix = 0;
    while (prefix < a.size() && prefix < b.size() && a[prefix] == b[prefix]) ++prefix;
    size_t suffix = 0;
    while (suffix < (a.size() - prefix) && suffix < (b.size() - prefix) &&
           a[a.size() - 1 - suffix] == b[b.size() - 1 - suffix]) ++suffix;
    std::ostringstream diff;
    diff << "--- a/" << rel_path << "\n+++ b/" << rel_path << "\n";
    size_t ctx_start = (prefix > 2) ? (prefix - 2) : 0;
    size_t a_end = a.size() - suffix;
    size_t b_end = b.size() - suffix;
    size_t a_ctx_end = std::min(a.size(), a_end + 2);
    size_t b_ctx_end = std::min(b.size(), b_end + 2);
    diff << "@@ -" << (ctx_start + 1) << "," << (a_ctx_end - ctx_start)
         << " +" << (ctx_start + 1) << "," << (b_ctx_end - ctx_start) << " @@\n";
    for (size_t i = ctx_start; i < prefix; ++i) diff << " " << a[i] << "\n";
    for (size_t i = prefix; i < a_end; ++i) diff << "-" << a[i] << "\n";
    for (size_t i = prefix; i < b_end; ++i) diff << "+" << b[i] << "\n";
    for (size_t i = a_end; i < a_ctx_end; ++i) diff << " " << a[i] << "\n";
    return diff.str();
}

bool symbol_kind_matches(const std::string& actual_kind, const std::string& filter) {
    if (filter.empty()) return true;
    auto a = low_str(actual_kind), f = low_str(filter);
    if (a == f || a.find(f) != std::string::npos) return true;
    if (f == "method" || f == "function")
        return a.find("method") != std::string::npos || a.find("function") != std::string::npos || a == "fn" || a == "func" || a == "cxxmethod";
    if (f == "class" || f == "type" || f == "struct")
        return a.find("class") != std::string::npos || a.find("struct") != std::string::npos || a.find("interface") != std::string::npos || a.find("type") != std::string::npos;
    return false;
}

std::string resolve_symbol_id_arg(Database& db, const Json& args) {
    if (args.contains("id") && args["id"].is_string() && !args["id"].get<std::string>().empty())
        return args["id"].get<std::string>();
    if (args.contains("symbol") && args["symbol"].is_string() && !args["symbol"].get<std::string>().empty())
        return args["symbol"].get<std::string>();
    if (args.contains("task_id") && args["task_id"].is_string() && !args["task_id"].get<std::string>().empty())
        return args["task_id"].get<std::string>();
    if (args.contains("semantic_key")) {
        auto krows = db.query("SELECT s.id FROM symbols s JOIN files f ON s.file_id=f.id WHERE s.active=1 AND f.deleted=0 AND json_extract(s.data,'$.semantic_key')=? ORDER BY s.id LIMIT 1", {args["semantic_key"].get<std::string>()});
        if (!krows.empty()) return "S" + std::to_string(krows[0]["id"].get<int64_t>());
        for (const auto& row : db.query("SELECT s.id,s.data FROM symbols s JOIN files f ON s.file_id=f.id WHERE s.active=1 AND f.deleted=0")) {
            auto d = parse(row["data"]);
            if (d.value("semantic_key", "") == args["semantic_key"].get<std::string>())
                return "S" + std::to_string(row["id"].get<int64_t>());
        }
    }
    std::string qname = args.value("qualified_name", args.value("name", args.value("text", "")));
    if (!qname.empty()) {
        std::string sig_filter = args.value("signature", "");
        for (const auto& row : db.query("SELECT s.id,s.name,s.qualified_name,s.data FROM symbols s JOIN files f ON s.file_id=f.id WHERE s.active=1 AND f.deleted=0 AND (s.qualified_name=? OR s.name=?) ORDER BY s.id", {qname, qname})) {
            auto d = parse(row["data"]);
            if (!sig_filter.empty() && d.value("signature", "").find(sig_filter) == std::string::npos) continue;
            return "S" + std::to_string(row["id"].get<int64_t>());
        }
    }
    throw std::runtime_error("Especifique id (S...), symbol, semantic_key o name/qualified_name válido");
}

Json last_evidence(Database& db,const std::string& kind){auto row=db.query("SELECT data FROM evidence WHERE kind=? ORDER BY revision DESC LIMIT 1",{kind});return row.empty()?Json():parse(row[0]["data"]);}
bool same_fingerprints(const Json& evidence,const Json& scan){
    if(!evidence.is_object()||!evidence.value("stable",false))return false;Json fp=Json::object();for(const auto& r:scan["roots"])fp[r["id"].get<std::string>()]=r["fingerprint"];return evidence.value("fingerprints",Json::object())==fp;
}
bool files_current(const Json& files){
    for(auto it=files.begin();it!=files.end();++it){auto p=fs::path(wide(it.key()));try{if(!fs::is_regular_file(p)||hash_file(p)!=it.value().get<std::string>())return false;}catch(...){return false;}}return true;
}
Json field_selection(const Json& item,const Json& fields){if(!fields.is_array()||fields.empty())return item;Json out=Json::object();for(const auto& f:fields){auto k=f.get<std::string>();if(item.contains(k))out[k]=item[k];}return out;}
std::string cursor(int64_t revision,int64_t offset){return std::to_string(revision)+":"+std::to_string(offset);}
std::pair<int64_t,int64_t> read_cursor(const Json& args,int64_t rev){
    auto cur=args.value("cursor","");if(cur.empty())return{rev,0};auto colon=cur.find(':');if(colon==std::string::npos)throw std::runtime_error("CURSOR_INVALID");
    auto version=std::stoll(cur.substr(0,colon)),off=std::stoll(cur.substr(colon+1));if(version!=rev)throw std::runtime_error("CURSOR_STALE: índice cambió; reinicie consulta");if(off<0)throw std::runtime_error("CURSOR_INVALID");return{version,off};
}
Json paginate(Json items,const Json& args,int64_t revision){
    auto [rev,offset]=read_cursor(args,revision);size_t first=std::min<size_t>(static_cast<size_t>(offset),items.size());
    auto requested=args.value("limit",10);if(requested<1||requested>1000)throw std::runtime_error("limit debe estar entre 1 y 1000");
    const auto limit=static_cast<size_t>(requested);Json selected=Json::array();size_t end=std::min(items.size(),first+limit);for(size_t i=first;i<end;++i)selected.push_back(field_selection(items[i],args.value("fields",Json::array())));
    return {{"revision",rev},{"total",items.size()},{"items",selected},{"cursor",cursor(rev,static_cast<int64_t>(first))},{"next_cursor",end<items.size()?Json(cursor(rev,static_cast<int64_t>(end))):Json(nullptr)}};
}
Json to_columnar_format(Json page) {
    if (!page.is_object()) return page;
    if (page.contains("results") && page["results"].is_array()) {
        for (auto& sub : page["results"]) sub = to_columnar_format(std::move(sub));
        return page;
    }
    if (!page.contains("items") || !page["items"].is_array()) return page;
    const auto& items = page["items"];
    std::vector<std::string> cols;
    std::set<std::string> seen_cols;
    for (const auto& item : items) {
        if (!item.is_object()) return page;
        for (auto it = item.begin(); it != item.end(); ++it) {
            if (it.value().is_null() || (it.value().is_string() && it.value().get_ref<const std::string&>().empty())) continue;
            if (seen_cols.insert(it.key()).second) cols.push_back(it.key());
        }
    }
    Json rows = Json::array();
    for (const auto& item : items) {
        Json r = Json::array();
        for (const auto& c : cols) {
            r.push_back(item.contains(c) ? item[c] : Json(nullptr));
        }
        rows.push_back(std::move(r));
    }
    page["format"] = "columnar";
    page["columns"] = cols;
    page["rows"] = std::move(rows);
    page.erase("items");
    return page;
}

Json bound(Json result,const Json& args){
    auto row_cursors=result.value("row_cursors",Json::array());result.erase("row_cursors");
    const int budget=args.value("token_budget",args.value("budget_tokens",800));if(budget<256||budget>100000)throw std::runtime_error("token_budget debe estar entre 256 y 100000");
    // Cota conservadora independiente del tokenizador: máximo de bytes UTF-8 <= presupuesto de tokens.
    // No afirma contar tokens de un modelo; bytes es una cota superior conservadora para BPE sobre UTF-8.
    result["budget"]={{"token_budget",budget},{"policy","utf8_bytes_upper_bound"}};
    result["response_bytes"]=999999;
    auto fit=[&]{return result.dump().size()<=static_cast<size_t>(budget);};
    if(!fit()&&result.contains("source")&&result["source"].is_object()&&result["source"].contains("body")){
        if(result.contains("target_candidates")&&result["target_candidates"].is_array()){
            for(auto& tc:result["target_candidates"])if(tc.is_object()&&tc.contains("body"))tc.erase("body");
        }
        if(!fit()){
            auto body=result["source"]["body"].get<std::string>();
            size_t current_dump=result.dump().size();
            size_t excess=current_dump>static_cast<size_t>(budget)?(current_dump-static_cast<size_t>(budget)+120):0;
            if(excess<body.size()&&body.size()-excess>=32){
                auto end=body.size()-excess;
                while(end>0&&end<body.size()&&(static_cast<unsigned char>(body[end])&0xc0)==0x80)--end;
                result["source"]["body"]=body.substr(0,end);
                result["source"]["body_truncated"]=true;
            }else{
                result["source"].erase("body");
                result["source"]["body_truncated"]=true;
            }
        }
        if(!fit()&&result.contains("callees")&&result["callees"].is_array()){
            while(!fit()&&!result["callees"].empty())result["callees"].erase(result["callees"].size()-1);
        }
        if(!fit()&&result.contains("callers")&&result["callers"].is_array()){
            while(!fit()&&!result["callers"].empty())result["callers"].erase(result["callers"].size()-1);
        }
    }
    for (const auto* key : {"items", "rows"}) {
        if (!result.contains(key) || !result[key].is_array() || fit()) continue;
        const auto page_cursor = result.value("cursor", std::string("0:0"));
        const auto original_next = result.value("next_cursor", Json(nullptr));
        const auto separator = page_cursor.find(':');
        const auto start = !page_cursor.starts_with("v2:") && separator != std::string::npos
            ? std::stoll(page_cursor.substr(separator + 1)) : int64_t(0);
        auto rows = std::move(result[key]);
        result[key] = Json::array();
        result["truncated"] = true;
        result["next_cursor"] = page_cursor;
        // Measure the final envelope before adding any rows. The cursor must refer to the last returned row.
        const auto envelope = result.dump().size() - result["next_cursor"].dump().size();
        std::size_t rows_size = 0;
        for (std::size_t i = 0; i < rows.size(); ++i) {
            Json next = original_next;
            if (i + 1 < rows.size()) {
                next = !row_cursors.empty() ? row_cursors.at(i) :
                    Json(cursor(result.value("revision", int64_t(0)), start + static_cast<int64_t>(i + 1)));
            }
            const auto row_size = rows[i].dump().size() + (i == 0 ? 0 : 1);
            if (envelope + rows_size + row_size + next.dump().size() > static_cast<std::size_t>(budget)) break;
            result[key].push_back(std::move(rows[i]));
            rows_size += row_size;
            result["next_cursor"] = std::move(next);
        }
        if (result[key].empty()) result["hint"] = "El elemento no cabe. Aumente token_budget o seleccione menos fields.";
    }
    if(!fit()&&result.contains("active_claims")&&result["active_claims"].is_array())while(!fit()&&!result["active_claims"].empty())result["active_claims"].erase(result["active_claims"].size()-1);
    if(!fit()&&result.contains("next_tasks"))while(!fit()&&!result["next_tasks"].empty())result["next_tasks"].erase(result["next_tasks"].size()-1);
    if(!fit()&&result.contains("top_pending_modules")&&result["top_pending_modules"].is_array())while(!fit()&&!result["top_pending_modules"].empty())result["top_pending_modules"].erase(result["top_pending_modules"].size()-1);
    if(!fit()&&result.contains("schema")&&result["schema"].is_string()&&result["schema"].get<std::string>()=="migratool.session_bootstrap/1"){
        for(const auto* k:{"workflow_hint","config_revision","last_activity"}){if(!fit()&&result.contains(k))result.erase(k);}
        if(!fit()&&result.contains("status")&&result["status"].is_object()){
            for(const auto* sk:{"cases","builds","non_source_by_category","disposition_counts","development_control","inventory_job","freshness"}){
                if(!fit()&&result["status"].contains(sk))result["status"].erase(sk);
            }
        }
    }
    if(!fit()){
        Json compact={{"revision",result.value("revision",int64_t(0))},{"truncated",true},{"budget",result["budget"]},{"response_bytes",999999}};
        for(const auto* key:{"schema","project","ok","applied","job_id","task_id","config_revision","state","phase","claim","generation","census_complete","analysis_complete","complete","files","dirs","processed","pending_analysis","failed","started","finished","next_cursor","name","migration_percent","units_with_passing_cases_percent","total_units","migrated_units","pending_units","index_dirty","last_sync","status","denominator_blockers","roots","error"}){
            if(!result.contains(key))continue;
            compact[key]=result[key];
            if(compact.dump().size()>static_cast<size_t>(budget))compact.erase(key);
        }
        compact["hint"]="Aumente token_budget o seleccione fields.";
        if(compact.dump().size()>static_cast<size_t>(budget))compact.erase("hint");
        result=compact;
    }
    if(!fit()){
        Json minimal={{"revision",result.value("revision",int64_t(0))},{"truncated",true},{"hint","Aumente token_budget o reduzca fields."},{"budget",result["budget"]},{"response_bytes",999999}};
        for(const auto* key:{"ok","applied","next_cursor","task_id"})if(result.contains(key))minimal[key]=result[key];result=minimal;
    }
    result["response_bytes"]=0;for(int i=0;i<3;++i)result["response_bytes"]=result.dump().size();return result;
}
}

Json agent_batch(const Project& p,const Json& args){
    if(p.revision.empty()||hash_file(p.file)!=p.revision)throw std::runtime_error("CONFIGURATION_CONFLICT: recargue el proyecto antes de aplicar el lote");
    auto actor=args.at("actor").get<std::string>();if(actor.empty())throw std::runtime_error("actor es obligatorio");
    const auto& operations=args.at("operations");if(!operations.is_array()||operations.empty()||operations.size()>10000)throw std::runtime_error("operations: entre 1 y 10000 acciones");
    if(!args.contains("expected_revision"))throw std::runtime_error("expected_revision es obligatorio para lotes");
    const bool dry_run = args.value("dry_run", false);

    bool inject_failure_after_disk = args.value("inject_failure_after_disk", false);
    std::string inject_failure_stage = args.value("inject_failure_stage", "");
    bool might_touch_files = inject_failure_after_disk || !inject_failure_stage.empty();
    for (const auto& op : operations) {
        std::string k = op.value("op", "");
        if (k == "replace_symbol_body" || k == "insert_before_symbol" || k == "insert_after_symbol" ||
            k == "safe_delete_symbol" || k == "rename_symbol" || k == "apply_structural_patch" || k == "ast_rewrite") {
            might_touch_files = true;
        }
        if (op.value("inject_failure", false)) {
            inject_failure_after_disk = true;
            might_touch_files = true;
        }
        if (op.contains("inject_failure_stage") && op["inject_failure_stage"].is_string()) {
            inject_failure_stage = op["inject_failure_stage"].get<std::string>();
            might_touch_files = true;
        }
    }

    ProjectFileTxLock file_tx_lock(p, might_touch_files && !dry_run);
    recover_pending_journals(p);

    std::string tx_id = unique_id();
    fs::path backup_dir = p.state_dir() / L"backups" / wide(tx_id);
    fs::path journal_dir = p.state_dir() / L"journal";
    fs::path journal_file = journal_dir / wide("tx-" + tx_id + ".json");
    fs::path db_backup_path = backup_dir / L"catalog.sqlite.bak";
    fs::path db_wal_backup_path = backup_dir / L"catalog.sqlite-wal.bak";

    if (might_touch_files && !dry_run) {
        std::error_code ec;
        fs::create_directories(backup_dir, ec);
        fs::create_directories(journal_dir, ec);
        {
            Database ck_db(p);
            try { ck_db.exec("PRAGMA wal_checkpoint(TRUNCATE)"); } catch (...) {}
        }
        auto db_src = p.state_dir() / L"catalog.sqlite";
        auto wal_src = p.state_dir() / L"catalog.sqlite-wal";
        if (fs::is_regular_file(db_src, ec)) {
            fs::copy_file(db_src, db_backup_path, fs::copy_options::overwrite_existing, ec);
        }
        if (fs::is_regular_file(wal_src, ec) && fs::file_size(wal_src, ec) > 0) {
            fs::copy_file(wal_src, db_wal_backup_path, fs::copy_options::overwrite_existing, ec);
        }
    }

    auto db_ptr = std::make_unique<Database>(p);
    Database& db = *db_ptr;
    const int64_t expected_rev_arg = args.at("expected_revision").get<int64_t>();
    const bool strict_global_rev = args.value("strict_global_revision", false);
    db.begin(-1);
    bool reconciled_entities = false;
    int64_t reconciled_from_rev = expected_rev_arg;
    if (expected_rev_arg >= 0 && db.revision() != expected_rev_arg) {
        const auto actual_rev = db.revision();
        if (strict_global_rev || inject_failure_after_disk || !inject_failure_stage.empty()) {
            db.rollback();
            throw std::runtime_error("REVISION_CONFLICT expected=" + std::to_string(expected_rev_arg) +
                                     " current=" + std::to_string(actual_rev));
        }
        struct TouchedScope { std::string eid; std::string fid; std::string family; };
        std::vector<TouchedScope> scopes;
        bool has_global_scope = false;
        for (const auto& op : operations) {
            const auto k = op.value("op", "");
            if (k == "map" || k == "mark") {
                auto ent = entity(db, op.at("source"));
                scopes.push_back({ent["entity_id"].get<std::string>(), file_id(ent), "mapping"});
                for (const auto& tid : op.value("targets", Json::array())) {
                    auto tent = entity(db, tid);
                    scopes.push_back({tent["entity_id"].get<std::string>(), file_id(tent), "code"});
                }
            } else if (k == "note") {
                const auto ent_arg = op.value("entity", "");
                if (ent_arg.empty()) { has_global_scope = true; break; }
                auto ent = entity(db, ent_arg);
                scopes.push_back({ent["entity_id"].get<std::string>(), file_id(ent), "note"});
            } else if (k == "classify") {
                auto ent = entity(db, op.at("file"));
                scopes.push_back({file_id(ent), file_id(ent), "classification"});
            } else if (k == "claim_task" || k == "renew_claim" || k == "release_task") {
                auto ent = entity(db, op.at("entity"));
                scopes.push_back({ent["entity_id"].get<std::string>(), file_id(ent), "claim"});
            } else if (k == "replace_symbol_body" || k == "insert_before_symbol" || k == "insert_after_symbol" ||
                       k == "safe_delete_symbol" || k == "rename_symbol") {
                auto sid = resolve_symbol_id_arg(db, op);
                auto ent = entity(db, sid);
                scopes.push_back({sid, file_id(ent), "code"});
            } else if (k == "apply_structural_patch") {
                for (const auto& ed : op.at("edits")) {
                    auto ent = entity(db, ed.at("file"));
                    scopes.push_back({file_id(ent), file_id(ent), "code"});
                }
            } else if (k == "upsert_phase") {
                const auto pid = "PHASE:" + op.at("id").get<std::string>();
                scopes.push_back({pid, pid, "phase"});
            } else if (k == "upsert_task" || k == "transition_task" || k == "delete_task") {
                const auto tid = "TASK:" + op.at("id").get<std::string>();
                scopes.push_back({tid, tid, "project_task"});
            } else if (k == "audit_doc") {
                auto ent = entity(db, op.at("doc"));
                scopes.push_back({file_id(ent), file_id(ent), "doc_audit"});
            } else {
                has_global_scope = true;
                break;
            }
        }
        if (has_global_scope) {
            db.rollback();
            throw std::runtime_error("REVISION_CONFLICT expected=" + std::to_string(expected_rev_arg) +
                                     " current=" + std::to_string(actual_rev));
        }
        const auto action_family = [](const std::string& act) -> std::string {
            if (act == "note") return "note";
            if (act == "classification") return "classification";
            if (act == "migration_complete" || act == "migration_partial") return "mapping";
            if (act == "claim_task" || act == "renew_claim" || act == "release_task") return "claim";
            if (act == "symbolic_edit" || act == "filtered_out" || act == "file_modified" || act == "file_added" || act == "file_removed") return "code";
            if (act == "upsert_phase") return "phase";
            if (act == "upsert_task" || act == "transition_task" || act == "delete_task") return "project_task";
            if (act == "audit_doc") return "doc_audit";
            return "other";
        };
        for (const auto& hrow : db.query("SELECT revision,action,entity FROM history WHERE revision>? ORDER BY revision", {expected_rev_arg})) {
            const auto h_act = hrow["action"].get<std::string>();
            const auto h_ent = hrow["entity"].is_null() ? std::string() : hrow["entity"].get<std::string>();
            if (h_act == "project_settings") {
                db.rollback();
                throw std::runtime_error("REVISION_CONFLICT expected=" + std::to_string(expected_rev_arg) +
                                         " current=" + std::to_string(actual_rev) + " cause=project_settings");
            }
            if (h_act == "symbolic_edit" && h_ent.empty()) {
                for (const auto& sc : scopes) {
                    if (sc.family == "code" || sc.family == "mapping") {
                        db.rollback();
                        throw std::runtime_error("REVISION_CONFLICT expected=" + std::to_string(expected_rev_arg) +
                                                 " current=" + std::to_string(actual_rev) + " cause=symbolic_edit");
                    }
                }
            }
            const auto hf = action_family(h_act);
            for (const auto& sc : scopes) {
                if (h_ent.empty()) continue;
                const bool same_ent = (h_ent == sc.eid || h_ent == sc.fid);
                if (!same_ent) continue;
                const bool conflict = (hf == sc.family) ||
                                      ((hf == "code") && (sc.family == "mapping" || sc.family == "code")) ||
                                      ((sc.family == "code") && (hf == "classification" || hf == "code"));
                if (conflict) {
                    db.rollback();
                    throw std::runtime_error("REVISION_CONFLICT expected=" + std::to_string(expected_rev_arg) +
                                             " current=" + std::to_string(actual_rev) + " entity=" + sc.eid);
                }
            }
        }
        reconciled_entities = true;
    }
    const int64_t pre_commit_rev = db.revision();
    const int64_t pre_commit_hist_max_id = db.integer("SELECT COALESCE(MAX(id), 0) FROM history");
    bool db_committed = false;

    auto check_entity_rev = [&](const Json& op, const std::string& eid) {
        if (!op.contains("expected_entity_revision")) return;
        const int64_t want = op.at("expected_entity_revision").get<int64_t>();
        const int64_t have = db.integer("SELECT COALESCE(MAX(revision),0) FROM history WHERE entity=?", {eid});
        if (have != want) {
            throw std::runtime_error("ENTITY_REVISION_CONFLICT entity=" + eid +
                                     " expected=" + std::to_string(want) + " current=" + std::to_string(have));
        }
    };

    Json changed=Json::array();
    struct PendingSpanEdit {
        size_t offset;
        size_t end_offset;
        std::string replacement;
        std::string op_kind;
        std::string entity_id;
    };
    struct TouchedFile {
        int64_t file_id_num{};
        std::string file_id_str;
        std::string rel_path;
        std::string abs_path;
        std::string before_content;
        std::string before_sha256;
        std::string after_content;
        std::string after_sha256;
        std::vector<PendingSpanEdit> edits;
    };
    std::map<std::string, TouchedFile> touched_files;
    std::vector<std::pair<fs::path, std::string>> written_disk_backups;

    auto get_touched_file = [&](const Json& file_row) -> TouchedFile& {
        std::string abs_p = file_row["absolute_path"].get<std::string>();
        auto it = touched_files.find(abs_p);
        if (it != touched_files.end()) return it->second;
        fs::path disk_p(wide(abs_p));
        if (!fs::is_regular_file(disk_p)) throw std::runtime_error("Archivo inexistente en disco: " + abs_p);
        std::string disk_hash = hash_file(disk_p);
        if (disk_hash != file_row["sha256"].get<std::string>())
            throw std::runtime_error("INDEX_STALE: el archivo cambió fuera de Migratool: " + abs_p);
        std::string content = read_text(disk_p, 64 * 1024 * 1024);
        TouchedFile tf{
            file_row["id"].get<int64_t>(),
            "F" + std::to_string(file_row["id"].get<int64_t>()),
            file_row["path"].get<std::string>(),
            abs_p,
            content,
            disk_hash,
            content,
            disk_hash,
            {}
        };
        return touched_files.emplace(abs_p, std::move(tf)).first->second;
    };

    BatchVerifyCache batch_cache;
    batch_cache.auto_sync_touched = args.value("auto_sync_touched", false);
    std::optional<Json> cached_evidence;
    auto get_batch_evidence = [&]() -> const Json& {
        if (!cached_evidence.has_value()) {
            const bool fresh = db.integer("SELECT COALESCE(MAX(CAST(value AS INTEGER)),0) FROM meta WHERE key='dirty'") == 0 &&
                               db.integer("SELECT EXISTS(SELECT 1 FROM inventory_jobs WHERE generation>0 AND state IN('queued','running','paused'))") == 0;
            cached_evidence = scale_evidence(p, db, fresh);
        }
        return *cached_evidence;
    };

    try{
        for(const auto& op:operations){
            auto kind=op.at("op").get<std::string>();
            if(kind=="map"||kind=="mark"){
                const bool op_sync = op.value("auto_sync_touched", batch_cache.auto_sync_touched);
                auto src_ent = entity_or_sync(db, p, op.at("source"), op_sync);
                if(op.contains("expected_entity_revision")) check_entity_rev(op, src_ent["entity_id"].get<std::string>());
                changed.push_back(map_in_transaction(db,p,op,actor,&batch_cache,std::move(src_ent)));
            }else if(kind=="classify_pattern"){
                auto category=op.at("category").get<std::string>();
                const std::set<std::string> valid={"source","test","script","documentation","configuration","protected_configuration","resource","binary","generated","external_dependency","build_artifact","unknown"};
                if(!valid.contains(category))throw std::runtime_error("Categoría inválida: "+category);
                bool relevant=op.value("relevant",false);
                std::string disp=op.value("disposition",std::string());
                std::string dnote=op.value("note",std::string());
                if(!disp.empty()){
                    const std::set<std::string> valid_disp={"MIGRATE","DO_NOT_MIGRATE","EXTRACT_CONTRACT_ONLY","REFERENCE_ONLY","DUPLICATE_OR_GENERATED"};
                    if(!valid_disp.contains(disp))throw std::runtime_error("Disposición inválida en classify_pattern: "+disp);
                }
                std::string sql="SELECT f.id,f.path,f.category,f.relevant,f.sha256 FROM files f LEFT JOIN roots r ON f.root=r.id WHERE f.deleted=0";
                Json qparams=Json::array();
                if(op.contains("root")&&!op["root"].get<std::string>().empty()){sql+=" AND f.root=?";qparams.push_back(op["root"]);}
                if(op.contains("role")&&!op["role"].get<std::string>().empty()){sql+=" AND r.role=?";qparams.push_back(op["role"]);}
                if(op.contains("current_category")&&!op["current_category"].get<std::string>().empty()){sql+=" AND f.category=?";qparams.push_back(op["current_category"]);}
                if(op.contains("language")&&!op["language"].get<std::string>().empty()){sql+=" AND lower(f.language)=lower(?)";qparams.push_back(op["language"]);}
                std::string prefix=op.value("path_prefix","");
                std::string contains=op.value("path_contains",op.value("text",""));
                std::string ext=low_str(op.value("extension",""));
                std::replace(prefix.begin(),prefix.end(),'\\','/');
                std::replace(contains.begin(),contains.end(),'\\','/');
                if(!ext.empty()&&ext[0]!='.')ext="."+ext;
                if(!prefix.empty()){sql+=" AND substr(lower(f.path),1,?)=lower(?)";qparams.push_back(static_cast<int64_t>(prefix.size()));qparams.push_back(prefix);}
                if(!contains.empty()){sql+=" AND instr(lower(f.path),lower(?))>0";qparams.push_back(contains);}
                if(!ext.empty()){sql+=" AND substr(lower(f.path),?)=?";qparams.push_back(-static_cast<int64_t>(ext.size()));qparams.push_back(ext);}
                sql+=" ORDER BY f.id LIMIT ?";
                qparams.push_back(std::clamp(op.value("limit",10000),1,50000));
                auto matched=db.query(sql,qparams);
                const std::string dat=now();
                auto rev=db.change(actor,"classification",{{"pattern",op},{"matched_count",matched.size()},{"after",{{"category",category},{"relevant",relevant},{"disposition",disp}}}});
                for(const auto& mrow:matched){
                    if(!disp.empty()||!dnote.empty()){
                        db.exec("UPDATE files SET category=?,relevant=?,last_revision=?,data=json_set(COALESCE(NULLIF(data,''),'{}'),'$.disposition',?,'$.disposition_note',?,'$.disposition_actor',?,'$.disposition_at',?) WHERE id=?",
                            {category,relevant,rev,disp,dnote,actor,dat,mrow["id"]});
                    }else{
                        db.exec("UPDATE files SET category=?,relevant=?,last_revision=? WHERE id=?",{category,relevant,rev,mrow["id"]});
                    }
                }
                changed.push_back({{"op","classify_pattern"},{"matched_files",matched.size()},{"category",category},{"relevant",relevant},{"revision",rev}});
            }else if(kind=="map_pattern"){
                std::string sql="SELECT f.* FROM files f JOIN roots r ON f.root=r.id WHERE f.deleted=0 AND r.role='source'";
                Json qparams=Json::array();
                if(op.value("relevant_only",true))sql+=" AND f.relevant=1";
                std::string sroot=op.value("source_root",op.value("root",""));
                if(!sroot.empty()){sql+=" AND f.root=?";qparams.push_back(sroot);}
                if(op.contains("category")&&!op["category"].get<std::string>().empty()){sql+=" AND f.category=?";qparams.push_back(op["category"]);}
                if(op.contains("language")&&!op["language"].get<std::string>().empty()){sql+=" AND lower(f.language)=lower(?)";qparams.push_back(op["language"]);}
                std::string prefix=op.value("path_prefix","");
                std::string contains=op.value("path_contains",op.value("text",""));
                std::string ext=low_str(op.value("extension",""));
                std::replace(prefix.begin(),prefix.end(),'\\','/');
                std::replace(contains.begin(),contains.end(),'\\','/');
                if(!ext.empty()&&ext[0]!='.')ext="."+ext;
                if(!prefix.empty()){sql+=" AND substr(lower(f.path),1,?)=lower(?)";qparams.push_back(static_cast<int64_t>(prefix.size()));qparams.push_back(prefix);}
                if(!contains.empty()){sql+=" AND instr(lower(f.path),lower(?))>0";qparams.push_back(contains);}
                if(!ext.empty()){sql+=" AND substr(lower(f.path),?)=?";qparams.push_back(-static_cast<int64_t>(ext.size()));qparams.push_back(ext);}
                if(op.value("only_unmapped",true)){
                    sql+=" AND NOT EXISTS(SELECT 1 FROM mappings m WHERE m.source='F'||f.id AND m.complete=1)";
                }
                sql+=" ORDER BY f.id LIMIT ?";
                qparams.push_back(std::clamp(op.value("limit",5000),1,20000));
                auto matched=db.query(sql,qparams);
                Json sample_ids=Json::array();
                size_t mapped_cnt=0;
                for(auto frow:matched){
                    frow["entity_id"]="F"+std::to_string(frow["id"].get<int64_t>());
                    Json single_op=op;
                    single_op["op"]="map";
                    single_op["source"]=frow["entity_id"];
                    if(!single_op.contains("complete"))single_op["complete"]=true;
                    map_in_transaction(db,p,single_op,actor,&batch_cache,std::move(frow));
                    if(sample_ids.size()<10)sample_ids.push_back("F"+std::to_string(matched[mapped_cnt]["id"].get<int64_t>()));
                    ++mapped_cnt;
                }
                changed.push_back({{"op","map_pattern"},{"mapped_count",mapped_cnt},{"sample_sources",sample_ids},{"revision",db.revision()}});
            }else if(kind=="auto_map_symbols"){
                const bool op_sync = op.value("auto_sync_touched", batch_cache.auto_sync_touched);
                std::string src_arg=op.value("source_file",op.value("source",""));
                std::string tgt_arg=op.value("target_file",op.value("target",""));
                if(tgt_arg.empty()&&op.contains("targets")&&op["targets"].is_array()&&!op["targets"].empty())
                    tgt_arg=op["targets"][0].get<std::string>();
                if(src_arg.empty()||tgt_arg.empty())throw std::runtime_error("auto_map_symbols requiere source_file/source y target_file/target");
                auto src_f=entity_or_sync(db,p,src_arg,op_sync);
                auto tgt_f=entity_or_sync(db,p,tgt_arg,op_sync);
                auto src_syms=db.query("SELECT id,name,qualified_name,kind FROM symbols WHERE file_id=? AND active=1 AND kind<>'file' ORDER BY id",{src_f["id"]});
                auto tgt_syms=db.query("SELECT id,name,qualified_name,kind FROM symbols WHERE file_id=? AND active=1 AND kind<>'file' ORDER BY id",{tgt_f["id"]});
                std::map<std::string,std::string> exact_tgt,norm_tgt;
                for(const auto& ts:tgt_syms){
                    std::string tsid="S"+std::to_string(ts["id"].get<int64_t>());
                    std::string tname=ts["name"].get<std::string>();
                    if(!exact_tgt.contains(tname))exact_tgt[tname]=tsid;
                    std::string nkey=normalize_ident_match(tname);
                    if(!nkey.empty()&&!norm_tgt.contains(nkey))norm_tgt[nkey]=tsid;
                }
                size_t matched_syms=0;
                Json unmatched=Json::array();
                Json matched_pairs=Json::array();
                for(const auto& ss:src_syms){
                    std::string ssid="S"+std::to_string(ss["id"].get<int64_t>());
                    std::string sname=ss["name"].get<std::string>();
                    std::string tsid;
                    if(exact_tgt.contains(sname))tsid=exact_tgt[sname];
                    else{
                        std::string nkey=normalize_ident_match(sname);
                        if(!nkey.empty()&&norm_tgt.contains(nkey))tsid=norm_tgt[nkey];
                    }
                    if(!tsid.empty()){
                        Json mop={
                            {"op","map"},{"source",ssid},{"targets",Json::array({tsid})},
                            {"complete",op.value("complete",true)},{"cases",op.value("cases",Json::array())},
                            {"note",op.value("note","Auto-mapeo de símbolo ("+sname+")")},
                            {"auto_sync_touched",op_sync}
                        };
                        map_in_transaction(db,p,mop,actor,&batch_cache);
                        ++matched_syms;
                        if(matched_pairs.size()<20)matched_pairs.push_back({{"source",ssid},{"target",tsid},{"name",sname}});
                    }else{
                        if(unmatched.size()<20)unmatched.push_back({{"id",ssid},{"name",sname}});
                    }
                }
                bool file_mapped=false;
                if(op.value("also_map_file",true)&&(matched_syms>0||src_syms.empty())){
                    bool all_done=(matched_syms==src_syms.size());
                    Json fmop={
                        {"op","map"},{"source",file_id(src_f)},{"targets",Json::array({file_id(tgt_f)})},
                        {"complete",op.value("complete",all_done)},{"cases",op.value("cases",Json::array())},
                        {"note",op.value("note","Auto-mapeo de archivo y símbolos")},{"auto_sync_touched",op_sync}
                    };
                    map_in_transaction(db,p,fmop,actor,&batch_cache,src_f);
                    file_mapped=true;
                }
                changed.push_back({{"op","auto_map_symbols"},{"source_file",file_id(src_f)},{"target_file",file_id(tgt_f)},
                    {"source_symbols_total",src_syms.size()},{"symbols_matched",matched_syms},{"file_mapped",file_mapped},
                    {"matched_pairs",matched_pairs},{"unmatched_source_symbols",unmatched},{"revision",db.revision()}});
            }else if(kind=="note"){
                auto text=op.at("text").get<std::string>();if(text.empty())throw std::runtime_error("Nota vacía");auto id=op.value("entity","");Json affected=Json::object();if(!id.empty()){auto file=entity(db,id);id=file["entity_id"];check_entity_rev(op,id);affected={{"path",file["absolute_path"]},{"sha256",file["sha256"]}};}
                auto rev=db.change(actor,"note",{{"before",nullptr},{"after",{{"text",text},{"entity",id},{"file",affected}}}},id);
                db.exec("INSERT INTO notes(entity,text,actor,at,revision)VALUES(?,?,?,?,?)",{id,text,actor,now(),rev});auto nid=db.integer("SELECT last_insert_rowid()");
                const std::string note_eid="N"+std::to_string(nid);
                db.exec("INSERT INTO search(entity_type,entity_id,body)VALUES('note',?,?)",{note_eid,text});
                db.exec("INSERT INTO search_rows(entity_type,entity_id,row_id)VALUES('note',?,?)",{note_eid,sqlite3_last_insert_rowid(db.native_handle())});
                changed.push_back({{"entity",id},{"revision",rev},{"note_id",note_eid}});
            }else if(kind=="classify"){
                auto file=entity(db,op.at("file"));if(file.contains("symbol_id"))throw std::runtime_error("classify requiere archivo F");check_entity_rev(op,file_id(file));auto before=Json{{"category",file["category"]},{"relevant",file["relevant"]==1}};
                auto category=op.at("category").get<std::string>();const std::set<std::string> valid={"source","test","script","documentation","configuration","protected_configuration","resource","binary","generated","external_dependency","build_artifact","unknown"};if(!valid.contains(category))throw std::runtime_error("Categoría inválida");
                auto relevant=op.value("relevant",false);auto after=Json{{"category",category},{"relevant",relevant}};
                std::string disp=op.value("disposition",std::string());
                std::string dnote=op.value("note",std::string());
                if(!disp.empty()){
                    const std::set<std::string> valid_disp={"MIGRATE","DO_NOT_MIGRATE","EXTRACT_CONTRACT_ONLY","REFERENCE_ONLY","DUPLICATE_OR_GENERATED"};
                    if(!valid_disp.contains(disp))throw std::runtime_error("Disposición inválida en classify: "+disp);
                    after["disposition"]=disp;
                }
                if(!dnote.empty())after["note"]=dnote;
                auto rev=db.change(actor,"classification",{{"before",before},{"after",after},{"sha256",file["sha256"]}},file_id(file));
                if(!disp.empty()||!dnote.empty()){
                    const std::string dat=now();
                    db.exec("UPDATE files SET category=?,relevant=?,last_revision=?,data=json_set(COALESCE(NULLIF(data,''),'{}'),'$.disposition',?,'$.disposition_note',?,'$.disposition_actor',?,'$.disposition_at',?) WHERE id=?",
                        {category,relevant,rev,disp,dnote,actor,dat,file["id"]});
                }else{
                    db.exec("UPDATE files SET category=?,relevant=?,last_revision=? WHERE id=?",{category,relevant,rev,file["id"]});
                }
                changed.push_back({{"entity",file_id(file)},{"revision",rev}});
            }else if(kind=="claim_task"||kind=="renew_claim"){
                ensure_claims_table(db);
                auto ent=entity(db,op.at("entity"));
                std::string eid=ent["entity_id"].get<std::string>();
                check_entity_rev(op,eid);
                std::string fid=file_id(ent);
                enforce_not_claimed_by_other(db,eid,fid,actor);
                if(eid.starts_with("F")){
                    auto sub=db.query("SELECT tc.* FROM task_claims tc JOIN symbols s ON tc.entity='S'||s.id WHERE s.file_id=? AND tc.actor<>? AND tc.expires_at>? LIMIT 1",{ent["id"],actor,now()});
                    if(!sub.empty())throw std::runtime_error("TASK_CLAIM_CONFLICT: símbolo interno "+sub[0]["entity"].get<std::string>()+" reclamado por "+sub[0]["actor"].get<std::string>());
                }
                int64_t lease=op.value("lease_seconds",int64_t(900));
                if(lease<10||lease>86400)throw std::runtime_error("lease_seconds debe estar entre 10 y 86400");
                auto claimed_at=now(),expires_at=iso_utc_plus_seconds(lease);
                auto session_id=op.value("session_id","");
                auto note=op.value("note","");
                auto rev=db.change(actor,kind,{{"entity",eid},{"expires_at",expires_at},{"lease_seconds",lease},{"session_id",session_id}},eid);
                db.exec("INSERT INTO task_claims(entity,actor,session_id,claimed_at,expires_at,lease_seconds,note,revision)VALUES(?,?,?,?,?,?,?,?) ON CONFLICT(entity)DO UPDATE SET actor=excluded.actor,session_id=excluded.session_id,claimed_at=excluded.claimed_at,expires_at=excluded.expires_at,lease_seconds=excluded.lease_seconds,note=excluded.note,revision=excluded.revision",
                    {eid,actor,session_id,claimed_at,expires_at,lease,note,rev});
                changed.push_back({{"op",kind},{"entity",eid},{"actor",actor},{"claimed_at",claimed_at},{"expires_at",expires_at},{"revision",rev}});
            }else if(kind=="release_task"){
                ensure_claims_table(db);
                auto ent=entity(db,op.at("entity"));
                std::string eid=ent["entity_id"].get<std::string>();
                check_entity_rev(op,eid);
                auto existing=db.query("SELECT * FROM task_claims WHERE entity=?",{eid});
                if(!existing.empty()&&existing[0]["actor"]!=actor&&existing[0]["expires_at"].get<std::string>()>now()&&!op.value("force",false))
                    throw std::runtime_error("TASK_CLAIM_CONFLICT: no puede liberar reclamo activo de otro actor");
                auto rev=db.change(actor,"release_task",{{"entity",eid}},eid);
                db.exec("DELETE FROM task_claims WHERE entity=?",{eid});
                changed.push_back({{"op","release_task"},{"entity",eid},{"released",!existing.empty()},{"revision",rev}});
            }else if(kind=="reconcile_bundle"){
                const auto& bundle=op.at("bundle");
                Json applied_items=Json::array(), conflicts=Json::array();
                auto resolve_portable=[&](const Json& ref)->std::optional<Json>{
                    std::string root=ref.value("root","");
                    std::string path=ref.value("path","");
                    auto frows=db.query("SELECT * FROM files WHERE root=? AND path=? AND deleted=0",{root,path});
                    if(frows.empty())return std::nullopt;
                    if(ref.value("kind","file")=="file") {
                        auto out=frows[0];out["entity_id"]="F"+std::to_string(out["id"].get<int64_t>());return out;
                    }
                    int64_t fid_num=frows[0]["id"].get<int64_t>();
                    std::string qname=ref.value("qualified_name",ref.value("name",""));
                    std::string sig=ref.value("signature","");
                    for(const auto& srow:db.query("SELECT * FROM symbols WHERE file_id=? AND active=1 AND (qualified_name=? OR name=?) ORDER BY id",{fid_num,qname,qname})){
                        auto sdata=parse(srow["data"]);
                        if(!sig.empty()&&sdata.value("signature","")!=sig)continue;
                        auto out=frows[0];
                        out["symbol_id"]=srow["id"];out["symbol_data"]=srow["data"];
                        out["entity_id"]="S"+std::to_string(srow["id"].get<int64_t>());
                        return out;
                    }
                    return std::nullopt;
                };
                for(const auto& bmap:bundle.value("mappings",Json::array())){
                    auto src_opt=resolve_portable(bmap.at("source"));
                    if(!src_opt){conflicts.push_back({{"source",bmap["source"]},{"reason","SOURCE_NOT_FOUND"}});continue;}
                    const auto& src=*src_opt;
                    std::string expected_fhash=bmap["source"].value("file_sha256","");
                    std::string expected_shash=bmap["source"].value("symbol_sha256","");
                    bool src_hash_ok=expected_fhash.empty()||(src["sha256"].is_string()&&src["sha256"].get<std::string>()==expected_fhash);
                    if(!expected_shash.empty()&&src.contains("symbol_data")){
                        auto sdata=parse(src["symbol_data"]);
                        src_hash_ok=src_hash_ok&&(sdata.value("symbol_sha256","")==expected_shash);
                    }
                    if(!src_hash_ok){conflicts.push_back({{"source",src["entity_id"]},{"reason","STALE_SOURCE_HASH"}});continue;}

                    Json target_ids=Json::array();bool targets_ok=true;
                    for(const auto& tref:bmap.value("targets",Json::array())){
                        auto topt=resolve_portable(tref);
                        if(!topt){conflicts.push_back({{"source",src["entity_id"]},{"target",tref},{"reason","TARGET_NOT_FOUND"}});targets_ok=false;break;}
                        std::string tfhash=tref.value("file_sha256","");
                        std::string tshash=tref.value("symbol_sha256","");
                        bool thash_ok=tfhash.empty()||((*topt)["sha256"].is_string()&&(*topt)["sha256"].get<std::string>()==tfhash);
                        if(!tshash.empty()&&topt->contains("symbol_data")){
                            auto tdata=parse((*topt)["symbol_data"]);
                            thash_ok=thash_ok&&(tdata.value("symbol_sha256","")==tshash);
                        }
                        if(!thash_ok){conflicts.push_back({{"source",src["entity_id"]},{"target",(*topt)["entity_id"]},{"reason","STALE_TARGET_HASH"}});targets_ok=false;break;}
                        target_ids.push_back((*topt)["entity_id"]);
                    }
                    if(!targets_ok)continue;
                    auto existing=db.query("SELECT * FROM mappings WHERE source=?",{src["entity_id"]});
                    if(!existing.empty()){
                        auto ex_m=mapping_json(existing[0]);
                        if(ex_m["targets"]!=target_ids||ex_m["disposition"]!=bmap.value("disposition","migrated")){
                            conflicts.push_back({{"source",src["entity_id"]},{"reason","CONFLICTING_MAPPING"},{"existing_targets",ex_m["targets"]},{"incoming_targets",target_ids}});
                            continue;
                        }
                    }
                    Json map_op={
                        {"source",src["entity_id"]},
                        {"targets",target_ids},
                        {"disposition",bmap.value("disposition","migrated")},
                        {"complete",bmap.value("declared_complete",false)},
                        {"cases",bmap.value("cases",Json::array())},
                        {"note",bmap.value("note","Reconciliado desde bundle")}
                    };
                    applied_items.push_back(map_in_transaction(db,p,map_op,bmap.value("actor",actor),&batch_cache));
                }
                auto rev=db.change(actor,"reconcile_bundle",{{"applied_count",applied_items.size()},{"conflict_count",conflicts.size()},{"conflicts",conflicts}});
                changed.push_back({{"op","reconcile_bundle"},{"applied",applied_items},{"conflicts",conflicts},{"revision",rev}});
            }else if(kind=="replace_symbol_body"||kind=="insert_before_symbol"||kind=="insert_after_symbol"||kind=="safe_delete_symbol"){
                std::string sid = resolve_symbol_id_arg(db, op);
                auto sym_ent = entity(db, sid);
                if (!sym_ent.contains("symbol_id")) throw std::runtime_error(kind + " requiere un símbolo S...");
                enforce_not_claimed_by_other(db, sid, file_id(sym_ent), actor);
                auto sdata = parse(sym_ent["symbol_data"]);
                if (!sdata.contains("offset") || !sdata.contains("end_offset"))
                    throw std::runtime_error("El símbolo " + sid + " no tiene offsets exactos registrados");
                size_t off = sdata["offset"].get<size_t>();
                size_t end_off = sdata["end_offset"].get<size_t>();
                auto& tf = get_touched_file(sym_ent);
                if (end_off > tf.before_content.size() || off > end_off)
                    throw std::runtime_error("Rango de bytes inválido en símbolo " + sid);
                std::string current_slice = tf.before_content.substr(off, end_off - off);
                std::string current_sym_sha = sha256(current_slice);
                if (sdata.contains("symbol_sha256") && sdata["symbol_sha256"].get<std::string>() != current_sym_sha)
                    throw std::runtime_error("SYMBOL_STALE: el contenido del símbolo " + sid + " no coincide con el catálogo");
                if (op.contains("expected_symbol_sha256") && op["expected_symbol_sha256"].get<std::string>() != current_sym_sha)
                    throw std::runtime_error("SYMBOL_HASH_CONFLICT: expected_symbol_sha256 no coincide en " + sid);

                if (kind == "replace_symbol_body") {
                    std::string new_body = op.at("new_body").get<std::string>();
                    tf.edits.push_back({off, end_off, new_body, kind, sid});
                } else if (kind == "insert_before_symbol") {
                    std::string content = op.at("content").get<std::string>();
                    if (!content.empty() && content.back() != '\n') content += "\n";
                    tf.edits.push_back({off, off, content, kind, sid});
                } else if (kind == "insert_after_symbol") {
                    std::string content = op.at("content").get<std::string>();
                    if (!content.empty() && content.front() != '\n') content = "\n" + content;
                    tf.edits.push_back({end_off, end_off, content, kind, sid});
                } else if (kind == "safe_delete_symbol") {
                    std::string sem_key = sdata.value("semantic_key", "");
                    std::string sname = sym_ent["symbol_name"].get<std::string>();
                    Json active_refs = Json::array();
                    for (const auto& drow : db.query("SELECT d.id,d.file_id,d.data,f.path FROM dependencies d JOIN files f ON d.file_id=f.id WHERE f.deleted=0")) {
                        auto dd = parse(drow["data"]);
                        bool match = (dd.value("target_symbol_id", "") == sid) ||
                                     (!sem_key.empty() && dd.value("target_key", "") == sem_key) ||
                                     (dd.value("target_symbol_id", "").empty() && dd.value("name", "") == sname);
                        if (!match) continue;
                        // Ignore self-references strictly inside the deleted symbol's own span
                        if (drow["file_id"].get<int64_t>() == tf.file_id_num) {
                            size_t doff = dd.value("offset", size_t(0));
                            if (doff >= off && doff <= end_off) continue;
                        }
                        active_refs.push_back({{"file", drow["path"]}, {"line", dd.value("line", 0)}, {"resolution", dd.value("resolution", "UNKNOWN")}});
                    }
                    if (!active_refs.empty() && !op.value("force", false)) {
                        throw std::runtime_error("SAFE_DELETE_BLOCKED: el símbolo " + sid + " tiene " +
                                                 std::to_string(active_refs.size()) + " referencias activas: " + active_refs.dump());
                    }
                    size_t del_end = end_off;
                    if (del_end < tf.before_content.size() && tf.before_content[del_end] == '\r') ++del_end;
                    if (del_end < tf.before_content.size() && tf.before_content[del_end] == '\n') ++del_end;
                    tf.edits.push_back({off, del_end, "", kind, sid});
                }
                changed.push_back({{"op", kind}, {"symbol", sid}, {"file", tf.file_id_str}, {"before_symbol_sha256", current_sym_sha}});
            }else if(kind=="rename_symbol"){
                std::string sid = resolve_symbol_id_arg(db, op);
                std::string new_name = op.at("new_name").get<std::string>();
                static const std::regex valid_id(R"(^[A-Za-z_][A-Za-z0-9_]*$)");
                if (!std::regex_match(new_name, valid_id))
                    throw std::runtime_error("Identificador inválido para rename_symbol: " + new_name);
                bool allow_approx = op.value("allow_approximate", false);
                auto sym_ent = entity(db, sid);
                if (!sym_ent.contains("symbol_id")) throw std::runtime_error("rename_symbol requiere un símbolo S...");
                enforce_not_claimed_by_other(db, sid, file_id(sym_ent), actor);
                auto sdata = parse(sym_ent["symbol_data"]);
                std::string old_name = sym_ent["symbol_name"].get<std::string>();
                std::string sem_key = sdata.value("semantic_key", "");

                if (!sdata.contains("name_offset") || !sdata.contains("name_end_offset"))
                    throw std::runtime_error("El símbolo " + sid + " no tiene offset exacto de identificador");
                auto& decl_tf = get_touched_file(sym_ent);
                size_t noff = sdata["name_offset"].get<size_t>();
                size_t neoff = sdata["name_end_offset"].get<size_t>();
                if (neoff > decl_tf.before_content.size() || decl_tf.before_content.substr(noff, neoff - noff) != old_name)
                    throw std::runtime_error("RENAME_OFFSET_MISMATCH en declaración de " + sid);
                decl_tf.edits.push_back({noff, neoff, new_name, "rename_symbol_decl", sid});

                size_t ref_count = 0;
                for (const auto& drow : db.query("SELECT d.file_id,d.data FROM dependencies d JOIN files f ON d.file_id=f.id WHERE f.deleted=0")) {
                    auto dd = parse(drow["data"]);
                    std::string res = dd.value("resolution", "UNKNOWN");
                    bool exact_match = (dd.value("target_symbol_id", "") == sid) || (!sem_key.empty() && dd.value("target_key", "") == sem_key);
                    bool approx_match = false;
                    if (!exact_match) {
                        if (res == "CANDIDATES_AMBIGUOUS") {
                            for (const auto& c : dd.value("candidate_symbols", Json::array()))
                                if (c.value("symbol_id", "") == sid) approx_match = true;
                            for (const auto& c : dd.value("candidates", Json::array()))
                                if (!sem_key.empty() && c.value("semantic_key", "") == sem_key) approx_match = true;
                        } else if (dd.value("target_symbol_id", "").empty() && dd.value("identifier_name", dd.value("name", "")) == old_name) {
                            approx_match = true;
                        }
                    }
                    if (!exact_match && !approx_match) continue;
                    if ((approx_match || res == "CANDIDATES_AMBIGUOUS" || res == "DYNAMIC_OR_REFLECTION" || res == "SYNTACTIC_APPROXIMATE") && !allow_approx) {
                        throw std::runtime_error("RENAME_AMBIGUOUS: existe referencia no exacta (" + res + ") a '" + old_name +
                                                 "'; use allow_approximate:true para forzar");
                    }
                    if (dd.contains("identifier_offset") && dd.contains("identifier_end_offset")) {
                        auto f_ent = entity(db, "F" + std::to_string(drow["file_id"].get<int64_t>()));
                        enforce_not_claimed_by_other(db, f_ent["entity_id"].get<std::string>(), f_ent["entity_id"].get<std::string>(), actor);
                        auto& rtf = get_touched_file(f_ent);
                        size_t roff = dd["identifier_offset"].get<size_t>();
                        size_t reoff = dd["identifier_end_offset"].get<size_t>();
                        if (reoff <= rtf.before_content.size() && roff < reoff && rtf.before_content.substr(roff, reoff - roff) == old_name) {
                            if (!(rtf.file_id_num == decl_tf.file_id_num && roff == noff)) {
                                rtf.edits.push_back({roff, reoff, new_name, "rename_symbol_ref", sid});
                                ++ref_count;
                            }
                        }
                    }
                }
                changed.push_back({{"op", "rename_symbol"}, {"symbol", sid}, {"old_name", old_name}, {"new_name", new_name}, {"references_updated", ref_count}});
            }else if(kind=="apply_structural_patch"){
                size_t patch_edits = 0;
                for (const auto& ed : op.at("edits")) {
                    auto f_ent = entity(db, ed.at("file"));
                    std::string fid_s = file_id(f_ent);
                    enforce_not_claimed_by_other(db, f_ent["entity_id"].get<std::string>(), fid_s, actor);
                    auto& tf = get_touched_file(f_ent);
                    size_t off = ed.at("offset").get<size_t>();
                    size_t end_off = ed.at("end_offset").get<size_t>();
                    if (end_off > tf.before_content.size() || off > end_off)
                        throw std::runtime_error("Rango inválido en apply_structural_patch: " + tf.rel_path);
                    if (ed.contains("expected_text") && tf.before_content.substr(off, end_off - off) != ed["expected_text"].get<std::string>())
                        throw std::runtime_error("PATCH_CONFLICT: expected_text no coincide en " + tf.rel_path);
                    tf.edits.push_back({off, end_off, ed.at("replacement").get<std::string>(), "apply_structural_patch", fid_s});
                    ++patch_edits;
                }
                if (op.value("inject_failure", false)) inject_failure_after_disk = true;
                changed.push_back({{"op", "apply_structural_patch"}, {"edits", patch_edits}});
            }else if(kind=="ast_rewrite"){
                std::string pattern = op.value("ast_pattern", op.value("pattern", ""));
                std::string replacement_tpl = op.value("replacement", op.value("rewrite", ""));
                if (pattern.empty()) throw std::runtime_error("ast_rewrite requiere ast_pattern o pattern");
                size_t total_matches = 0;
                if (op.contains("symbol") || (op.contains("id") && op["id"].get<std::string>().starts_with("S"))) {
                    std::string sid = resolve_symbol_id_arg(db, op);
                    auto sym_ent = entity(db, sid);
                    enforce_not_claimed_by_other(db, sid, file_id(sym_ent), actor);
                    auto sdata = parse(sym_ent["symbol_data"]);
                    size_t off = sdata.value("offset", size_t(0));
                    size_t end_off = sdata.value("end_offset", size_t(0));
                    auto& tf = get_touched_file(sym_ent);
                    if (end_off <= tf.before_content.size() && off <= end_off) {
                        std::string slice = tf.before_content.substr(off, end_off - off);
                        auto rw = mt::ast::rewrite_ast_pattern(slice, pattern, replacement_tpl, sym_ent["language"].get<std::string>());
                        if (!rw.second.empty()) {
                            tf.edits.push_back({off, end_off, rw.first, "ast_rewrite", sid});
                            total_matches += rw.second.size();
                        }
                    }
                } else if (op.contains("file") || (op.contains("id") && op["id"].get<std::string>().starts_with("F"))) {
                    std::string fid_arg = op.value("file", op.value("id", ""));
                    auto f_ent = entity(db, fid_arg);
                    std::string fid_s = file_id(f_ent);
                    enforce_not_claimed_by_other(db, fid_s, fid_s, actor);
                    auto& tf = get_touched_file(f_ent);
                    auto rw = mt::ast::rewrite_ast_pattern(tf.before_content, pattern, replacement_tpl, f_ent["language"].get<std::string>());
                    if (!rw.second.empty()) {
                        tf.edits.push_back({0, tf.before_content.size(), rw.first, "ast_rewrite", fid_s});
                        total_matches += rw.second.size();
                    }
                } else {
                    std::string lang_f = op.value("language", "");
                    std::string root_f = op.value("root", "");
                    for (const auto& frow : db.query("SELECT * FROM files WHERE deleted=0 AND relevant=1 ORDER BY id")) {
                        if (!root_f.empty() && frow["root"] != root_f) continue;
                        if (!lang_f.empty() && !mt::ast::language_matches(frow["language"].get<std::string>(), lang_f, false)) continue;
                        fs::path disk_p(wide(frow["absolute_path"].get<std::string>()));
                        if (!fs::is_regular_file(disk_p)) continue;
                        std::string ftext = read_text(disk_p, 16 * 1024 * 1024);
                        auto rw = mt::ast::rewrite_ast_pattern(ftext, pattern, replacement_tpl, frow["language"].get<std::string>());
                        if (!rw.second.empty()) {
                            std::string fid_s = "F" + std::to_string(frow["id"].get<int64_t>());
                            enforce_not_claimed_by_other(db, fid_s, fid_s, actor);
                            auto& tf = get_touched_file(frow);
                            tf.edits.push_back({0, tf.before_content.size(), rw.first, "ast_rewrite", fid_s});
                            total_matches += rw.second.size();
                        }
                    }
                }
                changed.push_back({{"op", "ast_rewrite"}, {"pattern", pattern}, {"replacement", replacement_tpl}, {"matches_rewritten", total_matches}});
            }else if(kind=="upsert_phase"){
                const std::string pid=op.at("id").get<std::string>();
                if(pid.empty())throw std::runtime_error("upsert_phase requiere id no vacío");
                const std::string title=op.value("title",pid);
                const int ordinal=op.value("ordinal",0);
                const std::string state=op.value("state","PLANNED");
                const std::set<std::string> valid_states={"PLANNED","IN_PROGRESS","HALF_DONE","COMPLETED","VERIFIED","BLOCKED"};
                if(!valid_states.contains(state))throw std::runtime_error("Estado de fase inválido: "+state);
                const Json gate_builds=op.value("gate_builds",Json::array());
                const Json gate_cases=op.value("gate_cases",Json::array());
                const Json target_modules=op.value("target_modules",Json::array());
                if(state=="VERIFIED"){
                    const auto& ev=get_batch_evidence();
                    std::set<std::string> passed_cases,passed_builds;
                    for(const auto& c:ev["passed"])passed_cases.insert(c.get<std::string>());
                    for(const auto& b:ev["builds"])if(b.value("fresh",false)&&b.value("state","")=="PASS")passed_builds.insert(b["id"].get<std::string>());
                    for(const auto& gb:gate_builds)if(!passed_builds.contains(gb.get<std::string>()))
                        throw std::runtime_error("PHASE_GATE_UNMET: compilación requerida no vigente: "+gb.get<std::string>());
                    for(const auto& gc:gate_cases)if(!passed_cases.contains(gc.get<std::string>()))
                        throw std::runtime_error("PHASE_GATE_UNMET: caso requerido no vigente: "+gc.get<std::string>());
                }
                const auto at=now();
                const std::string entity_key="PHASE:"+pid;
                check_entity_rev(op,entity_key);
                auto rev=db.change(actor,"upsert_phase",{{"id",pid},{"title",title},{"state",state},{"ordinal",ordinal}},entity_key);
                db.exec("INSERT INTO project_phases(id,title,ordinal,state,description,gate_builds,gate_cases,target_modules,updated_at,actor,revision)VALUES(?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(id)DO UPDATE SET title=excluded.title,ordinal=excluded.ordinal,state=excluded.state,description=excluded.description,gate_builds=excluded.gate_builds,gate_cases=excluded.gate_cases,target_modules=excluded.target_modules,updated_at=excluded.updated_at,actor=excluded.actor,revision=excluded.revision",
                    {pid,title,ordinal,state,op.value("description",""),gate_builds.dump(),gate_cases.dump(),target_modules.dump(),at,actor,rev});
                changed.push_back({{"op","upsert_phase"},{"id",pid},{"state",state},{"revision",rev}});
            }else if(kind=="upsert_task"||kind=="transition_task"){
                const std::string tid=op.at("id").get<std::string>();
                if(tid.empty())throw std::runtime_error(kind+" requiere id no vacío");
                auto existing=db.query("SELECT * FROM project_tasks WHERE id=?",{tid});
                if(kind=="transition_task"&&existing.empty())throw std::runtime_error("Tarea inexistente: "+tid);
                const std::string phase_id=op.contains("phase_id")?op["phase_id"].get<std::string>():(existing.empty()?std::string(""):existing[0]["phase_id"].is_null()?std::string(""):existing[0]["phase_id"].get<std::string>());
                const std::string title=op.contains("title")?op["title"].get<std::string>():(existing.empty()?tid:existing[0]["title"].get<std::string>());
                const std::string tkind=op.contains("task_kind")?op["task_kind"].get<std::string>():op.contains("kind")?op["kind"].get<std::string>():(existing.empty()?std::string("migration"):existing[0]["kind"].get<std::string>());
                const std::string state=op.value("state",existing.empty()?std::string("TODO"):existing[0]["state"].get<std::string>());
                const std::set<std::string> valid_states={"TODO","IN_PROGRESS","HALF_DONE","PORTED","INTEGRATED","COMPLETED","VERIFIED","BLOCKED"};
                if(!valid_states.contains(state))throw std::runtime_error("Estado de tarea inválido: "+state);
                const int priority=op.value("priority",existing.empty()?2:existing[0]["priority"].get<int>());
                const std::string assignee=op.value("assignee",existing.empty()?std::string(""):existing[0]["assignee"].get<std::string>());
                const Json src_ents=op.contains("source_entities")?op["source_entities"]:(existing.empty()?Json::array():safe_parse(existing[0]["source_entities"]));
                const Json tgt_ents=op.contains("target_entities")?op["target_entities"]:(existing.empty()?Json::array():safe_parse(existing[0]["target_entities"]));
                const Json req_cases=op.contains("required_cases")?op["required_cases"]:(existing.empty()?Json::array():safe_parse(existing[0]["required_cases"]));
                const Json deps=op.contains("depends_on")?op["depends_on"]:(existing.empty()?Json::array():safe_parse(existing[0]["depends_on"]));
                const std::string note=op.value("note",existing.empty()?std::string(""):existing[0]["note"].get<std::string>());
                if(state=="VERIFIED"){
                    const auto& ev=get_batch_evidence();
                    std::set<std::string> passed_cases;
                    for(const auto& c:ev["passed"])passed_cases.insert(c.get<std::string>());
                    for(const auto& rc:req_cases)if(!passed_cases.contains(rc.get<std::string>()))
                        throw std::runtime_error("TASK_GATE_UNMET: caso requerido no vigente: "+rc.get<std::string>());
                }
                const auto at=now();
                const auto created_at=existing.empty()?at:existing[0]["created_at"].get<std::string>();
                const std::string entity_key="TASK:"+tid;
                check_entity_rev(op,entity_key);
                auto rev=db.change(actor,kind,{{"id",tid},{"phase_id",phase_id},{"state",state},{"title",title}},entity_key);
                db.exec("INSERT INTO project_tasks(id,phase_id,title,kind,state,priority,assignee,source_entities,target_entities,required_cases,depends_on,note,created_at,updated_at,actor,revision)VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(id)DO UPDATE SET phase_id=excluded.phase_id,title=excluded.title,kind=excluded.kind,state=excluded.state,priority=excluded.priority,assignee=excluded.assignee,source_entities=excluded.source_entities,target_entities=excluded.target_entities,required_cases=excluded.required_cases,depends_on=excluded.depends_on,note=excluded.note,updated_at=excluded.updated_at,actor=excluded.actor,revision=excluded.revision",
                    {tid,phase_id,title,tkind,state,priority,assignee,src_ents.dump(),tgt_ents.dump(),req_cases.dump(),deps.dump(),note,created_at,at,actor,rev});
                changed.push_back({{"op",kind},{"id",tid},{"phase_id",phase_id},{"state",state},{"revision",rev}});
            }else if(kind=="delete_task"){
                const std::string tid=op.at("id").get<std::string>();
                const std::string entity_key="TASK:"+tid;
                check_entity_rev(op,entity_key);
                auto existing=db.query("SELECT id FROM project_tasks WHERE id=?",{tid});
                auto rev=db.change(actor,"delete_task",{{"id",tid},{"existed",!existing.empty()}},entity_key);
                db.exec("DELETE FROM project_tasks WHERE id=?",{tid});
                changed.push_back({{"op","delete_task"},{"id",tid},{"deleted",!existing.empty()},{"revision",rev}});
            }else if(kind=="audit_doc"){
                auto doc_ent=entity(db,op.at("doc"));
                if(doc_ent.contains("symbol_id"))throw std::runtime_error("audit_doc requiere archivo F...");
                const std::string doc_id=file_id(doc_ent);
                check_entity_rev(op,doc_id);
                const std::string state=op.value("state","REVIEWED");
                const std::set<std::string> valid_doc_states={"UNREVIEWED","REVIEWED","ACTION_REQUIRED","VERIFIED"};
                if(!valid_doc_states.contains(state))throw std::runtime_error("Estado de auditoría documental inválido: "+state);
                const std::string disposition=op.value("disposition","reference_only");
                const Json linked_targets=op.value("linked_targets",Json::array());
                const Json extracted_tasks=op.value("extracted_tasks",Json::array());
                const std::string note=op.value("note","");
                const std::string fsha=doc_ent["sha256"].is_string()?doc_ent["sha256"].get<std::string>():hash_file(fs::path(wide(doc_ent["absolute_path"].get<std::string>())));
                const auto at=now();
                auto rev=db.change(actor,"audit_doc",{{"doc_id",doc_id},{"path",doc_ent["path"]},{"state",state},{"disposition",disposition}},doc_id);
                db.exec("INSERT INTO doc_audits(doc_id,sha256,state,disposition,linked_targets,extracted_tasks,note,actor,at,revision)VALUES(?,?,?,?,?,?,?,?,?,?) ON CONFLICT(doc_id)DO UPDATE SET sha256=excluded.sha256,state=excluded.state,disposition=excluded.disposition,linked_targets=excluded.linked_targets,extracted_tasks=excluded.extracted_tasks,note=excluded.note,actor=excluded.actor,at=excluded.at,revision=excluded.revision",
                    {doc_id,fsha,state,disposition,linked_targets.dump(),extracted_tasks.dump(),note,actor,at,rev});
                changed.push_back({{"op","audit_doc"},{"doc_id",doc_id},{"state",state},{"disposition",disposition},{"revision",rev}});
            }else throw std::runtime_error("Operación de lote desconocida: "+kind);
        }

        // Apply in-memory edits per touched file in descending offset order and verify non-overlapping spans
        std::ostringstream combined_diff;
        Json file_diffs = Json::array();
        for (auto& [abs_p, tf] : touched_files) {
            if (tf.edits.empty()) continue;
            // Deduplicate identical edits (e.g., if a reference was visited twice) and sort descending by offset
            std::sort(tf.edits.begin(), tf.edits.end(), [](const PendingSpanEdit& a, const PendingSpanEdit& b) {
                if (a.offset != b.offset) return a.offset > b.offset;
                return a.end_offset > b.end_offset;
            });
            std::vector<PendingSpanEdit> unique_edits;
            for (const auto& e : tf.edits) {
                if (!unique_edits.empty() && unique_edits.back().offset == e.offset &&
                    unique_edits.back().end_offset == e.end_offset && unique_edits.back().replacement == e.replacement)
                    continue;
                unique_edits.push_back(e);
            }
            for (size_t i = 1; i < unique_edits.size(); ++i) {
                if (unique_edits[i].end_offset > unique_edits[i - 1].offset)
                    throw std::runtime_error("OVERLAPPING_EDITS: ediciones superpuestas en " + tf.rel_path);
            }
            std::string updated = tf.before_content;
            for (const auto& e : unique_edits) {
                updated.replace(e.offset, e.end_offset - e.offset, e.replacement);
            }
            tf.after_content = std::move(updated);
            tf.after_sha256 = sha256(tf.after_content);
            std::string udiff = make_unified_diff(tf.rel_path, tf.before_content, tf.after_content);
            combined_diff << udiff;
            file_diffs.push_back({
                {"file_id", tf.file_id_str},
                {"path", tf.rel_path},
                {"before_sha256", tf.before_sha256},
                {"after_sha256", tf.after_sha256}
            });
        }

        if (dry_run) {
            auto current_rev = db.revision();
            db.rollback();
            return {
                {"ok", true},
                {"dry_run", true},
                {"revision", current_rev},
                {"applied", 0},
                {"operations", changed},
                {"files", file_diffs},
                {"diff", combined_diff.str()}
            };
        }

        if (!touched_files.empty()) {
            fs::create_directories(backup_dir);
            fs::create_directories(journal_dir);
            Json backup_manifest = Json::array();
            Json journal_files = Json::array();
            std::vector<std::tuple<fs::path, fs::path, std::string>> staged_swaps;
            size_t idx = 0;
            for (const auto& [abs_p, tf] : touched_files) {
                ++idx;
                auto bfile = backup_dir / wide("file_" + std::to_string(idx) + ".bak");
                auto sfile = backup_dir / wide("file_" + std::to_string(idx) + ".stg");
                write_text(bfile, tf.before_content);
                if (hash_file(bfile) != tf.before_sha256)
                    throw std::runtime_error("BACKUP_VERIFICATION_FAILED para " + abs_p);
                write_text(sfile, tf.after_content);
                if (hash_file(sfile) != tf.after_sha256)
                    throw std::runtime_error("STAGING_VERIFICATION_FAILED para " + abs_p);
                backup_manifest.push_back({{"absolute_path", abs_p}, {"backup_path", path_text(bfile)}, {"sha256", tf.before_sha256}});
                journal_files.push_back({
                    {"absolute_path", abs_p},
                    {"backup_path", path_text(bfile)},
                    {"staging_path", path_text(sfile)},
                    {"before_sha256", tf.before_sha256},
                    {"after_sha256", tf.after_sha256}
                });
                staged_swaps.push_back({sfile, fs::path(wide(abs_p)), tf.before_content});
            }
            write_json(backup_dir / L"manifest.json", backup_manifest);

            Json journal_doc = {
                {"tx_id", tx_id},
                {"state", "PREPARED"},
                {"actor", actor},
                {"expected_revision", args.at("expected_revision")},
                {"created_at", now()},
                {"db_backup_path", path_text(db_backup_path)},
                {"db_wal_backup_path", path_text(db_wal_backup_path)},
                {"files", journal_files}
            };
            write_json(journal_file, journal_doc);

            size_t swap_idx = 0;
            for (const auto& [sfile, disk_p, before_bytes] : staged_swaps) {
                ++swap_idx;
                written_disk_backups.push_back({disk_p, before_bytes});
                if (!MoveFileExW(sfile.c_str(), disk_p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                    write_text(disk_p, read_text(sfile, 64 * 1024 * 1024));
                }
                if (swap_idx == 1 && inject_failure_stage == "crash_after_first_file_write") {
                    TerminateProcess(GetCurrentProcess(), 99);
                }
                if (swap_idx == 1 && (inject_failure_after_disk || inject_failure_stage == "after_first_file_write")) {
                    throw std::runtime_error("INJECTED_DISK_FAILURE: fallo simulado tras escribir el primer archivo en disco");
                }
            }
            const std::string edit_entity = (file_diffs.size() == 1) ? file_diffs[0]["file_id"].get<std::string>() : std::string();
            db.change(actor, "symbolic_edit", {{"files", file_diffs}, {"backup_dir", path_text(backup_dir)}}, edit_entity);
        }

        if (inject_failure_stage == "before_db_commit") {
            throw std::runtime_error("INJECTED_BEFORE_DB_COMMIT: fallo simulado antes de db.commit()");
        }

        db.commit();
        db_committed = true;

        if (inject_failure_stage == "after_db_commit") {
            throw std::runtime_error("INJECTED_AFTER_DB_COMMIT: fallo simulado después de db.commit() y antes de reindexar");
        }

        if (!touched_files.empty()) {
            db_ptr.reset(); // Release SQLite connection before index_sync opens its own writer
            if (inject_failure_stage == "during_reindex") {
                throw std::runtime_error("INJECTED_DURING_REINDEX: fallo simulado durante index_sync()");
            }
            index_sync(p, false);
        }

        std::error_code ec;
        fs::remove(journal_file, ec);
        fs::remove(db_backup_path, ec);
        fs::remove(db_wal_backup_path, ec);

        Database after_db(p);
        Json out = {{"ok",true},{"revision",after_db.revision()},{"applied",operations.size()},{"operations",changed}};
        if (reconciled_entities) {
            out["reconciled"] = true;
            out["reconciled_from_revision"] = reconciled_from_rev;
        }
        if (!file_diffs.empty()) {
            out["files"] = file_diffs;
            out["diff"] = combined_diff.str();
        }
        return out;
    }catch(...){
        for (const auto& [disk_p, original_bytes] : written_disk_backups) {
            try { write_text(disk_p, original_bytes); } catch (...) {}
        }
        if (db_ptr) {
            if (!db_committed) {
                db_ptr->rollback();
            }
            db_ptr.reset();
        }
        std::error_code ec;
        if (db_committed) {
            try {
                if (!written_disk_backups.empty()) {
                    try { index_sync(p, false); } catch (...) {}
                }
                Database rb_db(p);
                rb_db.exec("BEGIN IMMEDIATE");
                rb_db.exec("DROP TRIGGER IF EXISTS history_no_delete");
                rb_db.exec("DELETE FROM history WHERE id > ? OR revision > ?", {pre_commit_hist_max_id, pre_commit_rev});
                rb_db.exec("CREATE TRIGGER IF NOT EXISTS history_no_delete BEFORE DELETE ON history BEGIN SELECT RAISE(ABORT,'immutable history'); END");
                rb_db.exec("UPDATE meta SET value = ? WHERE key = 'revision'", {std::to_string(pre_commit_rev)});
                rb_db.exec("COMMIT");
            } catch (...) {
                if (fs::is_regular_file(db_backup_path, ec)) {
                    auto db_path = p.state_dir() / L"catalog.sqlite";
                    auto wal_path = p.state_dir() / L"catalog.sqlite-wal";
                    auto shm_path = p.state_dir() / L"catalog.sqlite-shm";
                    fs::remove(wal_path, ec);
                    fs::remove(shm_path, ec);
                    fs::copy_file(db_backup_path, db_path, fs::copy_options::overwrite_existing, ec);
                    if (fs::is_regular_file(db_wal_backup_path, ec)) {
                        fs::copy_file(db_wal_backup_path, wal_path, fs::copy_options::overwrite_existing, ec);
                    }
                }
            }
        }
        fs::remove(journal_file, ec);
        fs::remove(db_backup_path, ec);
        fs::remove(db_wal_backup_path, ec);
        throw;
    }
}

Json indexed_mapping(const Project& p,const std::string& source,const std::vector<std::string>& targets,const std::vector<std::string>& cases,bool complete,const std::string& note,const std::string& actor,const std::string& stage,const std::string& disposition,const std::string& external_replacement){
    for(int attempt=0;attempt<6;++attempt){
        Database db(p);auto revision=db.revision();
        try{
            Json op={{"op","map"},{"source",source},{"targets",targets},{"cases",cases},{"complete",complete},{"note",note}};
            if(!stage.empty())op["stage"]=stage;
            if(!disposition.empty())op["disposition"]=disposition;
            if(!external_replacement.empty())op["external_replacement"]=external_replacement;
            auto response=agent_batch(p,{{"actor",actor.empty()?"local-user":actor},{"expected_revision",revision},{"operations",Json::array({op})}});
            return response["operations"][0];
        }catch(const std::exception& e){
            if(attempt<5&&std::string(e.what()).find("REVISION_CONFLICT")!=std::string::npos){Sleep(20*(attempt+1));continue;}
            throw;
        }
    }
    throw std::runtime_error("REVISION_CONFLICT tras reintentos en indexed_mapping");
}

Json indexed_status(const Project& p,bool refresh){
    if(refresh)index_sync(p,false);Database db(p);if(scale_enabled(p,db))return scale_summary(p,db);auto scan=make_compact_scan(db,p);
    bool complete=scan.value("complete",false);auto dirty=meta(db,"dirty",0);bool fresh=complete&&dirty==0;
    const auto config=meta(db,"last_config","");if(config.is_string()&&config!=sha256(p.config.dump()))fresh=false;
    auto tests=last_evidence(db,"tests"),builds=last_evidence(db,"builds");std::map<std::string,bool> passed,built;
    Json cases=Json::array(),build_states=Json::array();
    bool tests_current=fresh&&same_fingerprints(tests,scan);bool builds_current=fresh&&same_fingerprints(builds,scan);
    std::set<std::string> configured_build_roots;
    for(const auto& c:p.config.value("cases",Json::array())){
        const bool has_lr=(c.contains("left")&&c["left"].is_object())||(c.contains("right")&&c["right"].is_object());
        const std::string kind=c.value("kind",has_lr?"differential":"target_test");
        Json row={{"id",c["id"]},{"kind",kind},{"state","NOT_RUN"},{"fresh",false}};
        if(tests.is_object())for(const auto& old:tests["cases"])if(old["id"]==c["id"]){
            row=old;
            if(!row.contains("kind"))row["kind"]=kind;
            const bool valid=case_evidence_fresh(c,old,tests_current);
            row["fresh"]=valid;if(!valid)row["state"]="STALE";break;}
        passed[c["id"]]=row["state"]=="PASS"&&row["fresh"]==true;cases.push_back(row);
    }
    for(const auto& c:p.config.value("builds",Json::array())){
        Json row={{"id",c["id"]},{"root",c["root"]},{"state","NOT_RUN"},{"fresh",false}};
        if(builds.is_object())for(const auto& b:builds["builds"])if(b["id"]==c["id"]){row=b;bool valid=builds_current&&b.value("config_sha256","")==sha256(c.dump())&&files_current(b.value("artifacts",Json::object()));row["fresh"]=valid;if(!valid)row["state"]="STALE";break;}
        auto root=c["root"].get<std::string>();configured_build_roots.insert(root);if(!built.contains(root))built[root]=true;built[root]=built[root]&&row["state"]=="PASS"&&row["fresh"]==true;build_states.push_back(row);
    }
    bool any_built=false;
    for(const auto& [r_id,r_ok]:built)if(r_ok)any_built=true;
    Json maps=Json::array();std::map<std::string,Json> mapping_by_source;
    for(const auto& row:db.query("SELECT * FROM mappings ORDER BY source")){
        auto m=mapping_json(row);
        auto raw_hashes=parse(row["hashes"]);
        bool current=fresh&&hashes_match(db,raw_hashes),has_tests=!m["cases"].empty(),build_ok=any_built;Json target_paths=Json::array();
        try{auto src=entity(db,m["source"]);m["source_path"]=src["absolute_path"];m["source_file_id"]=file_id(src);
            const auto s_root=src["root"].get<std::string>();
            if(configured_build_roots.contains(s_root))build_ok=build_ok&&built[s_root];
            for(const auto& id:m["targets"]){auto t=entity(db,id);target_paths.push_back(t["absolute_path"]);build_ok=build_ok&&built[t["root"]];}}
        catch(...){current=false;build_ok=false;m["source_path"]="";}
        for(const auto& c:m["cases"])has_tests=has_tests&&passed[c];
        m["target_paths"]=target_paths;m["fresh"]=current;m["builds_pass"]=build_ok;m["cases_pass"]=has_tests;m["verified_in_configured_cases"]=current&&has_tests&&build_ok;
        m["coverage_full_in_report"]=false;m["state"]=!current?"STALE":m["verified_in_configured_cases"]==true?"VERIFIED_IN_CASES":m["declared_complete"]==true?"DECLARED_COMPLETE_UNVERIFIED":"MAPPED_UNVERIFIED";
        maps.push_back(m);mapping_by_source[m["source"]]=m;
    }
    std::map<std::string,std::vector<std::string>> units_by_file;
    for(const auto& srow:db.query("SELECT s.id,s.file_id FROM symbols s JOIN files f ON s.file_id=f.id WHERE s.active=1 AND f.deleted=0 AND s.kind<>'file' ORDER BY s.id"))
        units_by_file["F"+std::to_string(srow["file_id"].get<int64_t>())].push_back("S"+std::to_string(srow["id"].get<int64_t>()));
    std::map<std::string,std::vector<const Json*>> maps_by_file;
    for(const auto& mapping:maps)if(mapping.contains("source_file_id"))
        maps_by_file[mapping["source_file_id"].get<std::string>()].push_back(&mapping);
    Json files=Json::array();size_t total_units=0,migrated_units=0,verified_units=0,mapped=0,declared=0,case_files=0;int source_files=0,unknown_files=0;
    for(const auto& f:scan["files"]){if(f["category"]=="unknown")++unknown_files;if(f["role"]!="source"||!f.value("relevant",true))continue;++source_files;
        auto id=f["id"].get<std::string>();const auto grouped=units_by_file.find(id);
        const bool fallback=grouped==units_by_file.end()||grouped->second.empty();
        const std::vector<std::string> fallback_units{id};const auto& units=fallback?fallback_units:grouped->second;
        auto filemap=mapping_by_source.find(id);bool file_done=filemap!=mapping_by_source.end()&&filemap->second.value("fresh",false)&&filemap->second.value("declared_complete",false);
        size_t done=0,verified=0;for(const auto& unit:units){auto it=mapping_by_source.find(unit);bool d=file_done,v=false;if(it!=mapping_by_source.end()){d=d||(it->second.value("fresh",false)&&it->second.value("declared_complete",false));v=it->second.value("verified_in_configured_cases",false);}if(fallback&&filemap!=mapping_by_source.end())v=filemap->second.value("verified_in_configured_cases",false);done+=d;verified+=v;}
        Json missing=Json::array();bool fm=filemap!=mapping_by_source.end();if(!fm)missing.push_back("file_mapping");if(!file_done)missing.push_back("completion_declaration");if(!fm||!filemap->second.value("cases_pass",false))missing.push_back("linked_passing_cases");if(!fm||!filemap->second.value("builds_pass",false))missing.push_back("current_builds");if(f["analysis"]!="ANALYZED")missing.push_back("complete_symbol_analysis");if(!fresh)missing.push_back("current_index");
        Json aggregate_targets=Json::array();std::set<std::string> known_targets;bool symbol_maps=false;std::string recent_note,recent_actor,recent_at;
        const auto related=maps_by_file.find(id);
        if(related!=maps_by_file.end())for(const auto* mapping:related->second){const auto& m=*mapping;symbol_maps=true;for(const auto& path:m["target_paths"]){auto value=path.get<std::string>();if(known_targets.insert(value).second)aggregate_targets.push_back(value);}if(m.value("at","")>recent_at){recent_at=m["at"];recent_note=m.value("note","");recent_actor=m.value("actor","");}}
        Json item={{"id",id},{"source_path",f["absolute_path"]},{"language",f["language"]},{"category",f["category"]},{"analysis",f["analysis"]},{"generated",f.value("generated",false)},
            {"state",fm?filemap->second["state"]:symbol_maps?Json("SYMBOLS_MAPPED_ONLY"):Json("UNMAPPED")},{"target_paths",aggregate_targets},{"declared_complete",file_done},
            {"verified_in_configured_cases",fm&&filemap->second.value("verified_in_configured_cases",false)},{"missing",missing},
            {"total_units",units.size()},{"migrated_units",done},{"units_with_passing_cases",verified},{"granularity",fallback?"file":"symbols"},
            {"migration_percent",fresh?Json(100.0*static_cast<double>(done)/static_cast<double>(units.size())):Json(nullptr)},
            {"units_with_passing_cases_percent",fresh?Json(100.0*static_cast<double>(verified)/static_cast<double>(units.size())):Json(nullptr)},
            {"note",recent_note},{"actor",recent_actor},{"at",recent_at}};
        total_units+=units.size();migrated_units+=done;verified_units+=verified;mapped+=fm||symbol_maps;declared+=file_done;case_files+=item["verified_in_configured_cases"]==true;files.push_back(item);
    }
    Json coverage=Json::array();for(const auto& row:db.query("SELECT data FROM evidence WHERE kind='coverage' ORDER BY revision DESC")){auto c=parse(row["data"]);bool current=fresh;for(const auto& f:c["files"]){auto match=db.query("SELECT sha256 FROM files WHERE absolute_path=? AND deleted=0",{f["path"]});if(match.empty()||match[0]["sha256"]!=f["sha256"])current=false;}c["fresh"]=current;coverage.push_back(c);}
    size_t passing=0;for(const auto& [id,ok]:passed)passing+=ok;
    auto count=scan["files"].size();auto classified=count-static_cast<size_t>(unknown_files);
    return {{"schema","migratool.status/2"},{"name",p.config.at("name")},{"project_path",path_text(p.file)},{"at",now()},{"revision",db.revision()},
        {"last_sync",meta(db,"last_sync",nullptr)},{"index_dirty",dirty!=0},{"freshness",{{"fresh",fresh},{"checked",refresh},{"basis","indexed_at_last_sync_with_watcher_invalidations"}}},
        {"inventory_complete",complete},{"known_files",count},{"unclassified_files",unknown_files},{"classification_percent",count?Json(100.0*static_cast<double>(classified)/static_cast<double>(count)):Json(nullptr)},
        {"discovery_percent",complete&&unknown_files==0?Json(100):Json(nullptr)},
        {"source_files",source_files},{"mapped_source_files",mapped},{"declared_complete_files",declared},{"files_verified_in_cases",case_files},
        {"total_units",total_units},{"migrated_units",migrated_units},{"units_with_passing_cases",verified_units},{"pending_units",total_units-migrated_units},
        {"migration_percent",fresh&&total_units?Json(100.0*static_cast<double>(migrated_units)/static_cast<double>(total_units)):Json(nullptr)},
        {"units_with_passing_cases_percent",fresh&&total_units?Json(100.0*static_cast<double>(verified_units)/static_cast<double>(total_units)):Json(nullptr)},
        {"percentage_basis","Unidades de símbolos; una unidad por archivo de código sin analizador. Solo archivos clasificados como relevantes. Las declaraciones no acreditan equivalencia universal."},
        {"functional_parity_percent",nullptr},{"universal_equivalence_proven",false},{"configured_cases",cases.size()},{"passing_current_cases",passing},
        {"files",files},{"mappings",maps},{"cases",cases},{"builds",build_states},{"coverage",coverage},{"diagnostics",scan["diagnostics"]},{"exclusions",scan["exclusions"]},
        {"limits",{"El porcentaje de migración usa declaraciones vigentes; la paridad se limita a los casos configurados.","Sin watcher activo, ejecute inventory_sync para observar cambios externos.","Cobertura importada: procedencia externa; no acredita cobertura de las pruebas diferenciales.","El análisis sin adaptador conserva una unidad por archivo; no demuestra estructura interna.","Exclusiones de metadatos internos y enlaces quedan registradas; no se siguen enlaces fuera de las raíces."}}};
}

namespace {
Json cached_status(const Project& p,Database& db){
    auto rev=db.revision();auto cached=db.query("SELECT value FROM meta WHERE key='status_cache'");auto cfg=sha256(p.config.dump());
    Json evidence_files=Json::object();
    auto add=[&](const std::string& file){try{auto path=fs::path(wide(file));evidence_files[file]={{"bytes",fs::file_size(path)},{"mtime",fs::last_write_time(path).time_since_epoch().count()}};}catch(...){evidence_files[file]=nullptr;}};
    for(const auto* kind:{"builds","tests"}){auto evidence=last_evidence(db,kind);if(!evidence.is_object())continue;for(const auto& row:evidence[kind==std::string("builds")?"builds":"cases"]){
        auto artifacts=row.value("artifacts",Json::object());for(auto it=artifacts.begin();it!=artifacts.end();++it)add(it.key());
        for(const auto* side:{"left","right","target"})if(row.contains(side)){auto files=row[side].value("execution_files",Json::object());for(auto it=files.begin();it!=files.end();++it)add(it.key());for(const auto* stream:{"stdout","stderr"})if(row[side].contains(stream))add(row[side][stream]);}
    }}
    auto signature=sha256(evidence_files.dump());
    if(!cached.empty()){auto c=parse(cached[0]["value"]);if(c.value("format",0)==2&&c["revision"]==rev&&c["configuration"]==cfg&&c.value("evidence_signature","")==signature)return c["status"];}
    auto st=indexed_status(p,false);db.try_aux_write("INSERT INTO meta(key,value)VALUES('status_cache',?)ON CONFLICT(key)DO UPDATE SET value=excluded.value",{Json{{"format",2},{"revision",rev},{"configuration",cfg},{"evidence_signature",signature},{"status",st}}.dump()});return st;
}
Json compact_status(const Json& status){
    Json result;for(const auto* k:{"schema","name","revision","last_sync","index_dirty","inventory_complete","known_files","unclassified_files","classification_percent","discovery_percent","source_files","total_units","migrated_units","pending_units","migration_percent","units_with_passing_cases_percent","passing_current_cases","configured_cases","functional_parity_percent"})result[k]=status[k];return result;
}

Json build_task_context(const Project& p, Database& db, const Json& args) {
    if ((args.contains("task_ids") && args["task_ids"].is_array() && !args["task_ids"].empty()) ||
        (args.contains("ids") && args["ids"].is_array() && !args["ids"].empty())) {
        const auto& id_list = args.contains("task_ids") ? args["task_ids"] : args["ids"];
        if (id_list.size() > 50) throw std::runtime_error("task_ids: máximo 50 unidades por lote");
        Json items = Json::array();
        size_t sum_file_bytes = 0, sum_body_bytes = 0, sum_saved_bytes = 0;
        for (const auto& item_id : id_list) {
            Json single_args = args;
            single_args.erase("task_ids");
            single_args.erase("ids");
            single_args["task_id"] = item_id.get<std::string>();
            auto ctx = build_task_context(p, db, single_args);
            if (ctx.contains("metrics") && ctx["metrics"].is_object()) {
                sum_file_bytes += ctx["metrics"].value("file_total_bytes", size_t(0));
                sum_body_bytes += ctx["metrics"].value("symbol_body_bytes", size_t(0));
                sum_saved_bytes += ctx["metrics"].value("saved_bytes_vs_full_file", size_t(0));
            }
            ctx.erase("schema");
            ctx.erase("revision");
            ctx.erase("next_action");
            items.push_back(std::move(ctx));
        }
        return {
            {"schema", "migratool.task_context_batch/1"},
            {"revision", db.revision()},
            {"count", items.size()},
            {"items", items},
            {"metrics", {
                {"file_total_bytes", sum_file_bytes},
                {"symbol_body_bytes", sum_body_bytes},
                {"saved_bytes_vs_full_file", sum_saved_bytes}
            }}
        };
    }

    std::string tid = args.value("task_id", args.value("id", ""));
    if (tid.empty() && (args.contains("symbol") || args.contains("semantic_key") || args.contains("qualified_name") || args.contains("name"))) {
        tid = resolve_symbol_id_arg(db, args);
    }
    auto mapping_fresh_complete = [&](const std::string& eid) -> bool {
        auto mrows = db.query("SELECT complete,hashes FROM mappings WHERE source=? LIMIT 1", {eid});
        if (mrows.empty()) return false;
        return mrows[0]["complete"] == 1 && hashes_match(db, parse(mrows[0]["hashes"]));
    };
    if (tid.empty()) {
        // Select first unclaimed unmapped source symbol (or file fallback) using bounded queries
        for (const auto& srow : db.query("SELECT s.id,s.file_id FROM symbols s JOIN files f ON s.file_id=f.id JOIN roots r ON f.root=r.id WHERE s.active=1 AND f.deleted=0 AND f.relevant=1 AND r.role='source' ORDER BY s.id LIMIT 500")) {
            std::string cand = "S" + std::to_string(srow["id"].get<int64_t>());
            std::string fid_s = "F" + std::to_string(srow["file_id"].get<int64_t>());
            if (!get_active_claim(db, cand, fid_s).is_null()) continue;
            if (!mapping_fresh_complete(cand) && !mapping_fresh_complete(fid_s)) { tid = cand; break; }
        }
        if (tid.empty()) {
            auto frows = db.query("SELECT 'F'||f.id AS id FROM files f JOIN roots r ON f.root=r.id LEFT JOIN mappings m ON m.source='F'||f.id WHERE f.deleted=0 AND f.relevant=1 AND r.role='source' AND (m.source IS NULL OR m.complete=0) ORDER BY f.id LIMIT 1");
            if (!frows.empty()) tid = frows[0]["id"].get<std::string>();
            else {
                auto any_row = db.query("SELECT 'F'||f.id AS id FROM files f JOIN roots r ON f.root=r.id WHERE f.deleted=0 AND f.relevant=1 AND r.role='source' ORDER BY f.id LIMIT 1");
                if (!any_row.empty()) tid = any_row[0]["id"].get<std::string>();
            }
        }
    }
    if (tid.empty()) throw std::runtime_error("No hay unidades de origen inventariadas");

    auto ent = [&]() -> Json {
        try { return entity(db, tid); }
        catch (...) {
            if (args.contains("name") || args.contains("qualified_name") || !tid.empty()) {
                Json rargs = args;
                if (!rargs.contains("name") && !rargs.contains("qualified_name")) rargs["name"] = tid;
                return entity(db, resolve_symbol_id_arg(db, rargs));
            }
            throw;
        }
    }();
    std::string eid = ent["entity_id"].get<std::string>();
    std::string fid_s = file_id(ent);
    bool is_sym = ent.contains("symbol_id");
    const bool outline_only = args.value("outline_only", false);
    bool include_body = !outline_only && args.value("include_body", true);
    int depth = std::clamp(args.value("depth", 1), 1, 3);

    Json source_meta = {
        {"id", eid},
        {"file_id", fid_s},
        {"root", ent["root"]},
        {"language", ent["language"]},
        {"path", ent["path"]},
        {"absolute_path", ent["absolute_path"]},
        {"file_sha256", ent["sha256"]},
        {"file_bytes", ent["bytes"]}
    };

    std::string sem_key;
    std::string sym_name = ent["path"].get<std::string>();
    if (is_sym) {
        auto sdata = parse(ent["symbol_data"]);
        sym_name = ent["symbol_name"].get<std::string>();
        sem_key = sdata.value("semantic_key", "");
        source_meta["name"] = sym_name;
        source_meta["qualified_name"] = ent["symbol_qualified_name"];
        source_meta["signature"] = sdata.value("signature", "");
        source_meta["kind"] = ent["symbol_kind"];
        source_meta["line"] = sdata.value("line", ent["symbol_line"].get<int>());
        source_meta["end_line"] = sdata.value("end_line", source_meta["line"].get<int>());
        if (sdata.contains("offset")) source_meta["offset"] = sdata["offset"];
        if (sdata.contains("end_offset")) source_meta["end_offset"] = sdata["end_offset"];
        if (sdata.contains("symbol_sha256")) source_meta["symbol_sha256"] = sdata["symbol_sha256"];
        if (sdata.contains("semantic_key")) source_meta["semantic_key"] = sdata["semantic_key"];
    } else {
        Json outline = Json::array();
        for (const auto& srow : db.query("SELECT id,name,qualified_name,kind,line,data FROM symbols WHERE file_id=? AND active=1 AND kind<>'file' ORDER BY line,id LIMIT 100", {ent["id"]})) {
            auto sd = parse(srow["data"]);
            std::string sid_str = "S" + std::to_string(srow["id"].get<int64_t>());
            outline.push_back({
                {"id", sid_str},
                {"name", srow["name"]},
                {"qualified_name", srow["qualified_name"]},
                {"kind", srow["kind"]},
                {"signature", sd.value("signature", "")},
                {"line", srow["line"]},
                {"end_line", sd.value("end_line", srow["line"].get<int>())},
                {"offset", sd.value("offset", 0)},
                {"end_offset", sd.value("end_offset", 0)},
                {"migrated", mapping_fresh_complete(sid_str)}
            });
        }
        source_meta["symbols_outline"] = std::move(outline);
    }

    Json current_mapping = nullptr;
    std::string task_state = "UNMAPPED";
    auto mrows = db.query("SELECT * FROM mappings WHERE source=? LIMIT 1", {eid});
    if (mrows.empty() && !is_sym) mrows = db.query("SELECT * FROM mappings WHERE source=? LIMIT 1", {fid_s});
    if (!mrows.empty()) {
        current_mapping = mapping_json(mrows[0]);
        const bool fresh = hashes_match(db, parse(mrows[0]["hashes"]));
        current_mapping["fresh"] = fresh;
        task_state = !fresh ? "STALE" : (current_mapping["declared_complete"] == true ? "DECLARED_COMPLETE" : "MAPPED");
    }

    // Semantic callees and callers (bounded SQLite queries)
    Json callees = Json::array(), callers = Json::array();
    if (is_sym) {
        for (const auto& drow : db.query("SELECT d.data,f.path FROM dependencies d JOIN files f ON d.file_id=f.id WHERE d.file_id=? AND f.deleted=0 ORDER BY d.id LIMIT 300", {ent["id"]})) {
            auto dd = parse(drow["data"]);
            if (dd.value("caller_symbol_id", "") == eid || (!sem_key.empty() && dd.value("caller_key", "") == sem_key)) {
                std::string t_sid = dd.value("target_symbol_id", "");
                bool callee_migrated = !t_sid.empty() && mapping_fresh_complete(t_sid);
                callees.push_back({
                    {"target_symbol_id", t_sid},
                    {"name", dd.value("target_qualified_name", dd.value("name", ""))},
                    {"signature", dd.value("target_signature", "")},
                    {"kind", dd.value("kind", "call")},
                    {"line", dd.value("line", 0)},
                    {"resolution", dd.value("resolution", "UNKNOWN")},
                    {"dispatch", dd.value("dispatch", "direct")},
                    {"migrated", callee_migrated}
                });
            }
        }
        std::string caller_sql = "SELECT d.data,f.path FROM dependencies d JOIN files f ON d.file_id=f.id WHERE f.deleted=0 AND (json_extract(d.data,'$.target_symbol_id')=?";
        Json caller_params = {eid};
        if (!sem_key.empty()) { caller_sql += " OR json_extract(d.data,'$.target_key')=?"; caller_params.push_back(sem_key); }
        caller_sql += " OR d.target_id=?) ORDER BY d.id LIMIT 300";
        caller_params.push_back(ent["id"]);
        for (const auto& drow : db.query(caller_sql, caller_params)) {
            auto dd = parse(drow["data"]);
            if (dd.value("target_symbol_id", "") == eid || (!sem_key.empty() && dd.value("target_key", "") == sem_key)) {
                callers.push_back({
                    {"caller_symbol_id", dd.value("caller_symbol_id", "")},
                    {"caller_name", dd.value("caller_qualified_name", "")},
                    {"file", drow["path"]},
                    {"line", dd.value("line", 0)},
                    {"resolution", dd.value("resolution", "UNKNOWN")},
                    {"dispatch", dd.value("dispatch", "direct")}
                });
            }
        }
        if (depth >= 2 && !callees.empty()) {
            std::set<std::string> level1_ids;
            for (const auto& c : callees) {
                auto tsid = c.value("target_symbol_id", "");
                if (!tsid.empty()) level1_ids.insert(tsid);
            }
            if (!level1_ids.empty()) {
                std::string placeholders;
                Json lparams = Json::array();
                for (const auto& lsid : level1_ids) {
                    if (!placeholders.empty()) placeholders += ',';
                    placeholders += '?';
                    lparams.push_back(lsid);
                }
                for (const auto& drow : db.query("SELECT d.data FROM dependencies d JOIN files f ON d.file_id=f.id WHERE f.deleted=0 AND json_extract(d.data,'$.caller_symbol_id') IN(" + placeholders + ") ORDER BY d.id LIMIT 300", lparams)) {
                    auto dd = parse(drow["data"]);
                    auto csid = dd.value("caller_symbol_id", "");
                    callees.push_back({
                        {"from_symbol_id", csid},
                        {"target_symbol_id", dd.value("target_symbol_id", "")},
                        {"name", dd.value("target_qualified_name", dd.value("name", ""))},
                        {"depth", 2},
                        {"resolution", dd.value("resolution", "UNKNOWN")}
                    });
                }
            }
        }
    }

    // Target candidates
    Json target_candidates = Json::array();
    if (!current_mapping.is_null() && current_mapping.contains("targets")) {
        for (const auto& tid_val : current_mapping["targets"]) {
            try {
                auto tent = entity(db, tid_val.get<std::string>());
                Json tc = {{"id", tent["entity_id"]}, {"file_id", file_id(tent)}, {"path", tent["path"]}, {"absolute_path", tent["absolute_path"]}, {"language", tent["language"]}, {"mapped", true}};
                if (tent.contains("symbol_data")) {
                    auto tdata = parse(tent["symbol_data"]);
                    tc["name"] = tent["symbol_name"];
                    tc["qualified_name"] = tent["symbol_qualified_name"];
                    tc["signature"] = tdata.value("signature", "");
                    tc["line"] = tdata.value("line", 1);
                    tc["end_line"] = tdata.value("end_line", 1);
                    if (include_body && target_candidates.empty()) {
                        auto slice = extract_symbol_slice(tdata, tent["absolute_path"].get<std::string>(), 0, tent["sha256"].get<std::string>(), !args.value("allow_stale", false));
                        tc["body"] = slice["body"];
                    }
                }
                target_candidates.push_back(tc);
            } catch (...) {}
        }
    }
    if (target_candidates.empty() && is_sym) {
        auto add_cand = [&](const Json& trow) {
            auto tdata = parse(trow["data"]);
            Json tc = {
                {"id", "S" + std::to_string(trow["id"].get<int64_t>())},
                {"file_id", "F" + std::to_string(trow["file_id"].get<int64_t>())},
                {"name", trow["name"]},
                {"qualified_name", trow["qualified_name"]},
                {"signature", tdata.value("signature", "")},
                {"path", trow["path"]},
                {"language", trow["language"]},
                {"line", tdata.value("line", 1)},
                {"end_line", tdata.value("end_line", 1)},
                {"mapped", false}
            };
            if (include_body && target_candidates.empty()) {
                try {
                    auto slice = extract_symbol_slice(tdata, trow["absolute_path"].get<std::string>(), 0, trow["sha256"].get<std::string>(), !args.value("allow_stale", false));
                    tc["body"] = slice["body"];
                } catch (...) {}
            }
            target_candidates.push_back(std::move(tc));
        };
        std::set<int64_t> seen_tc;
        for (const auto& trow : db.query("SELECT s.id,s.file_id,s.name,s.qualified_name,s.data,f.path,f.absolute_path,f.language,f.sha256 FROM symbols s JOIN files f ON s.file_id=f.id JOIN roots r ON f.root=r.id WHERE s.active=1 AND f.deleted=0 AND r.role='target' AND s.name=? ORDER BY s.id LIMIT 5", {sym_name})) {
            seen_tc.insert(trow["id"].get<int64_t>());
            add_cand(trow);
        }
        if (target_candidates.size() < 5) {
            std::string norm_src = normalize_ident_match(sym_name);
            if (!norm_src.empty()) {
                for (const auto& trow : db.query("SELECT s.id,s.file_id,s.name,s.qualified_name,s.data,f.path,f.absolute_path,f.language,f.sha256 FROM symbols s JOIN files f ON s.file_id=f.id JOIN roots r ON f.root=r.id WHERE s.active=1 AND f.deleted=0 AND r.role='target' AND replace(lower(s.name),'_','')=? ORDER BY s.id LIMIT 5", {norm_src})) {
                    if (seen_tc.insert(trow["id"].get<int64_t>()).second && target_candidates.size() < 5) {
                        add_cand(trow);
                    }
                }
            }
        }
    }

    size_t body_bytes = 0;
    if (include_body) {
        if (is_sym) {
            auto slice = extract_symbol_slice(parse(ent["symbol_data"]), ent["absolute_path"].get<std::string>(), 0, ent["sha256"].get<std::string>(), !args.value("allow_stale", false));
            source_meta["body"] = slice["body"];
            body_bytes = slice["byte_length"].get<size_t>();
        } else {
            fs::path disk_p(wide(ent["absolute_path"]));
            if (!args.value("allow_stale", false) && (!fs::is_regular_file(disk_p) || hash_file(disk_p) != ent["sha256"].get<std::string>())) {
                throw std::runtime_error("STALE_SOURCE: el archivo '" + ent["absolute_path"].get<std::string>() + "' fue modificado fuera de Migratool; ejecute inventory_sync");
            }
            std::string body = read_text(disk_p, 64 * 1024);
            source_meta["body"] = body;
            body_bytes = body.size();
        }
    }

    auto notes = db.query("SELECT 'N'||id AS id,entity,text,actor,at FROM notes WHERE entity=? OR entity=? ORDER BY id DESC LIMIT 5", {eid, fid_s});
    auto claim = get_active_claim(db, eid, fid_s);
    std::string next_action = (task_state == "UNMAPPED")
        ? "Reclame la unidad con claim_task, migre el símbolo al destino y registre la correspondencia con batch_update(op='map')."
        : (task_state == "STALE")
        ? "Re-inspeccione cambios en fuente/destino y actualice el mapeo y pruebas vinculadas."
        : (task_state == "DECLARED_COMPLETE_UNVERIFIED")
        ? "Vincule y ejecute una prueba diferencial y compilación con verify(execute=true)."
        : "Unidad verificada en los casos configurados.";

    size_t full_file_bytes = ent["bytes"].is_null() ? body_bytes : ent["bytes"].get<size_t>();
    return {
        {"schema", "migratool.task_context/1"},
        {"revision", db.revision()},
        {"task_id", eid},
        {"kind", is_sym ? "symbol" : "file"},
        {"status", task_state},
        {"claim", claim},
        {"source", source_meta},
        {"mapping", current_mapping},
        {"callees", callees},
        {"callers", callers},
        {"target_candidates", target_candidates},
        {"notes", notes},
        {"next_action", next_action},
        {"metrics", {
            {"file_total_bytes", full_file_bytes},
            {"symbol_body_bytes", body_bytes},
            {"saved_bytes_vs_full_file", full_file_bytes > body_bytes ? (full_file_bytes - body_bytes) : 0}
        }}
    };
}

Json query_items(const Project& p,Database& db,const Json& args){
    auto kind=args.value("kind","files");auto text=args.value("text","");auto needle="%"+text+"%";
    Json rows,params=Json::array();std::string where;
    auto add=[&](const std::string& clause,const Json& value){where+=" AND "+clause;params.push_back(value);};
    if(kind=="files"||kind=="documentation"||kind=="new"||kind=="ignored"||kind=="orphans"||kind=="excluded"){
        where=" WHERE f.deleted="+std::string(args.value("deleted",false)?"1":"0");
        if(kind=="documentation")add("f.category=?","documentation");
        if(kind=="ignored")add("f.git_state=?","!!");
        if(kind=="excluded")add("f.build_state=?","NOT_IN_COMPILATION_DATABASE");
        if(kind=="new")add("f.first_revision>?",args.value("since_revision",int64_t(0)));
        if(kind=="orphans")where+=" AND f.relevant=1 AND NOT EXISTS(SELECT 1 FROM mappings m WHERE m.source='F'||f.id) AND NOT EXISTS(SELECT 1 FROM dependencies d WHERE d.target_id=f.id)";
        if(args.contains("root"))add("f.root=?",args["root"]);
        if(args.contains("role"))add("r.role=?",args["role"]);
        if(args.contains("category"))add("f.category=?",args["category"]);
        if(args.contains("git_state"))add("f.git_state=?",args["git_state"]);
        if(args.contains("relevant"))add("f.relevant=?",args["relevant"]);
        if(!text.empty()){where+=" AND (f.absolute_path LIKE ? OR f.id IN(SELECT CAST(substr(entity_id,2)AS INTEGER)FROM search WHERE entity_type='file' AND search MATCH ?))";params.push_back(needle);params.push_back("\""+text+"\"");}
        if(args.contains("id"))add("f.id=?",entity_number(args["id"],'F'));
        const auto* filter_state=args.value("deleted",false)?",COALESCE(json_extract(f.data,'$.filtered_by_inventory_mode'),0) AS filtered_by_inventory_mode":"";
        auto raw_files=db.query(std::string("SELECT 'F'||f.id AS id,f.root,r.role,f.path,f.absolute_path,f.sha256,f.category,f.language,f.relevant,f.analysis,f.git_state,f.build_state,f.deleted,f.first_revision,f.last_revision")+filter_state+" FROM files f LEFT JOIN roots r ON f.root=r.id"+where+" ORDER BY f.id",params);
        std::string lang_filter=args.value("language","");
        rows=Json::array();
        for(auto& row:raw_files){
            if(!lang_filter.empty()&&!mt::ast::language_matches(row["language"].get<std::string>(),lang_filter,false))continue;
            auto duplicate=db.integer("SELECT COUNT(*) FROM files WHERE sha256=? AND deleted=0",{row["sha256"]});
            row["duplicate_copies"]=duplicate;
            if(args.value("detail","")=="full"){
                auto file=db.query("SELECT data FROM files WHERE id=?",{entity_number(row["id"],'F')});
                if(!file.empty()){auto data=parse(file[0]["data"]);row["text_content"]=data.value("text_content","");row["text_truncated"]=data.value("text_truncated",false);}
            }
            rows.push_back(std::move(row));
        }
    }else if(kind=="symbols"||kind=="find_symbol"||kind=="symbols_overview"||kind=="get_symbols_overview"){
        where=" WHERE s.active=1 AND f.deleted=0";
        if(!text.empty()){where+=" AND(s.name LIKE ? OR s.qualified_name LIKE ?)";params.push_back(needle);params.push_back(needle);}
        if(args.contains("exact_name"))add("s.name=?",args["exact_name"]);
        if(args.contains("qualified_name"))add("s.qualified_name=?",args["qualified_name"]);
        if(args.contains("root"))add("f.root=?",args["root"]);
        if(args.contains("role"))add("r.role=?",args["role"]);
        if(args.contains("id"))add("s.id=?",entity_number(args["id"],'S'));
        if(args.contains("file")){
            auto fent=entity(db,args["file"].get<std::string>());
            add("f.id=?",fent["id"]);
        }
        auto raw_rows=db.query("SELECT 'S'||s.id AS id,'F'||f.id AS file_id,s.name,s.qualified_name,s.kind,s.line,f.path,f.absolute_path,f.root,f.language,f.sha256 AS file_sha256,r.role,s.data FROM symbols s JOIN files f ON s.file_id=f.id LEFT JOIN roots r ON f.root=r.id"+where+" ORDER BY s.id",params);
        const bool is_overview=(kind=="symbols_overview"||kind=="get_symbols_overview");
        const bool include_body=args.value("include_body",false)&&!is_overview;
        const std::string sig_filter=args.value("signature_contains",args.value("signature",""));
        const std::string kind_filter=args.value("symbol_kind","");
        const std::string lang_filter=args.value("language","");
        rows=Json::array();
        for(auto& row:raw_rows){
            if(!lang_filter.empty()&&!mt::ast::language_matches(row["language"].get<std::string>(),lang_filter,false))continue;
            auto sdata=parse(row["data"]);
            if(!sig_filter.empty()&&sdata.value("signature","").find(sig_filter)==std::string::npos)continue;
            if(!kind_filter.empty()&&!symbol_kind_matches(row["kind"].get<std::string>(),kind_filter))continue;
            row["signature"]=sdata.value("signature","");
            row["end_line"]=sdata.value("end_line",row["line"].get<int>());
            if(sdata.contains("offset"))row["offset"]=sdata["offset"];
            if(sdata.contains("end_offset"))row["end_offset"]=sdata["end_offset"];
            if(!is_overview){
                if(sdata.contains("column"))row["column"]=sdata["column"];
                if(sdata.contains("end_column"))row["end_column"]=sdata["end_column"];
                if(sdata.contains("symbol_sha256"))row["symbol_sha256"]=sdata["symbol_sha256"];
                if(sdata.contains("semantic_key"))row["semantic_key"]=sdata["semantic_key"];
                row["semantic_resolution"]=sdata.value("semantic_resolution",false);
                if(include_body){
                    auto slice=extract_symbol_slice(sdata,row["absolute_path"].get<std::string>(),args.value("context_lines",0),row.value("file_sha256",""),!args.value("allow_stale",false));
                    row["body"]=slice["body"];
                    row["byte_length"]=slice["byte_length"];
                }
                if(args.value("detail","")=="full")row["details"]=sdata;
            }
            row.erase("data");
            row.erase("file_sha256");
            rows.push_back(std::move(row));
        }
    }else if(kind=="symbol_body"||kind=="get_symbol_body"){
        std::string sid=resolve_symbol_id_arg(db,args);
        auto sent=entity(db,sid);
        if(!sent.contains("symbol_id"))throw std::runtime_error("get_symbol_body requiere un símbolo S...");
        auto sdata=parse(sent["symbol_data"]);
        auto slice=extract_symbol_slice(sdata,sent["absolute_path"].get<std::string>(),args.value("context_lines",0),sent["sha256"].get<std::string>(),!args.value("allow_stale",false));
        slice["id"]=sid;
        slice["file_id"]=file_id(sent);
        slice["name"]=sent["symbol_name"];
        slice["qualified_name"]=sent["symbol_qualified_name"];
        slice["signature"]=sdata.value("signature","");
        slice["kind"]=sent["symbol_kind"];
        slice["path"]=sent["path"];
        slice["absolute_path"]=sent["absolute_path"];
        slice["file_sha256"]=sent["sha256"];
        rows=Json::array({slice});
    }else if(kind=="definition"||kind=="find_definition"){
        rows=Json::array();
        if(args.contains("file")&&args.contains("line")&&!args.contains("id")&&!args.contains("symbol")&&text.empty()){
            auto fent=entity(db,args["file"].get<std::string>());
            int target_line=args["line"].get<int>();
            for(const auto& drow:db.query("SELECT data FROM dependencies WHERE file_id=? ORDER BY id",{fent["id"]})){
                auto dd=parse(drow["data"]);
                if(dd.value("line",0)==target_line){
                    std::string tsid=dd.value("target_symbol_id","");
                    if(!tsid.empty()){
                        auto tent=entity(db,tsid);
                        auto tdata=parse(tent["symbol_data"]);
                        rows.push_back({{"id",tsid},{"file_id",file_id(tent)},{"name",tent["symbol_name"]},{"qualified_name",tent["symbol_qualified_name"]},
                                        {"signature",tdata.value("signature","")},{"kind",tent["symbol_kind"]},{"path",tent["path"]},{"absolute_path",tent["absolute_path"]},
                                        {"line",tdata.value("line",1)},{"end_line",tdata.value("end_line",1)},{"offset",tdata.value("offset",0)},{"end_offset",tdata.value("end_offset",0)},
                                        {"resolution",dd.value("resolution","SEMANTIC_EXACT")}});
                        break;
                    }
                }
            }
        }else{
            std::string sid=resolve_symbol_id_arg(db,args);
            auto sent=entity(db,sid);
            auto sdata=parse(sent["symbol_data"]);
            Json item={{"id",sid},{"file_id",file_id(sent)},{"name",sent["symbol_name"]},{"qualified_name",sent["symbol_qualified_name"]},
                       {"signature",sdata.value("signature","")},{"kind",sent["symbol_kind"]},{"path",sent["path"]},{"absolute_path",sent["absolute_path"]},
                       {"line",sdata.value("line",1)},{"end_line",sdata.value("end_line",1)},{"offset",sdata.value("offset",0)},{"end_offset",sdata.value("end_offset",0)},
                       {"semantic_key",sdata.value("semantic_key","")}};
            if(args.value("include_body",false))item["body"]=extract_symbol_slice(sdata,sent["absolute_path"].get<std::string>(),0,sent["sha256"].get<std::string>(),!args.value("allow_stale",false))["body"];
            rows.push_back(item);
        }
    }else if(kind=="diagnostics"||kind=="get_diagnostics"){
        rows=Json::array();
        auto diags=meta(db,"analysis_diagnostics",Json::array());
        if(diags.is_array()){
            for(const auto& d:diags){
                if(args.contains("file")&&d.value("file","").find(args["file"].get<std::string>())==std::string::npos)continue;
                if(!text.empty()&&d.dump().find(text)==std::string::npos)continue;
                rows.push_back(d);
            }
        }
        for(const auto& r:db.query("SELECT id,data FROM roots ORDER BY id")){
            auto rdata=parse(r["data"]);
            for(const auto& d:rdata.value("diagnostics",Json::array())){
                if(!text.empty()&&d.dump().find(text)==std::string::npos)continue;
                rows.push_back(d);
            }
        }
    }else if(kind=="range"){
        auto fent=entity(db,args.at("file").get<std::string>());
        fs::path disk_p(wide(fent["absolute_path"]));
        if(!fs::is_regular_file(disk_p))throw std::runtime_error("STALE_SOURCE: archivo no existe en disco: "+fent["absolute_path"].get<std::string>());
        if(!args.value("allow_stale",false)&&hash_file(disk_p)!=fent["sha256"].get<std::string>()){
            throw std::runtime_error("STALE_SOURCE: el archivo '"+fent["absolute_path"].get<std::string>()+"' fue modificado fuera de Migratool; ejecute inventory_sync");
        }
        if(args.contains("offset")&&args.contains("end_offset")){
            size_t file_bytes=0;
            size_t req_off=args["offset"].get<size_t>();
            size_t req_end=args["end_offset"].get<size_t>();
            std::string slice=read_byte_range(disk_p,req_off,req_end,file_bytes);
            size_t off=std::min(file_bytes,req_off);
            size_t end_off=std::min(file_bytes,req_end);
            if(end_off<off)end_off=off;
            rows=Json::array({{{"file_id",file_id(fent)},{"path",fent["path"]},{"offset",off},{"end_offset",end_off},{"content",slice},{"sha256",fent["sha256"]}}});
        }else{
            std::string content=read_text(disk_p,32*1024*1024);
            std::vector<size_t> starts={0};
            for(size_t i=0;i<content.size();++i)if(content[i]=='\n')starts.push_back(i+1);
            int s_line=std::max(1,args.value("start_line",1));
            int e_line=std::min(static_cast<int>(starts.size()),args.value("end_line",s_line+20));
            if(e_line<s_line)e_line=s_line;
            size_t off=starts[static_cast<size_t>(s_line-1)];
            size_t end_off=(static_cast<size_t>(e_line)<starts.size())?starts[static_cast<size_t>(e_line)]:content.size();
            rows=Json::array({{{"file_id",file_id(fent)},{"path",fent["path"]},{"start_line",s_line},{"end_line",e_line},{"offset",off},{"end_offset",end_off},{"content",content.substr(off,end_off-off)},{"sha256",fent["sha256"]}}});
        }
    }else if(kind=="references"||kind=="find_references"||kind=="callers"||kind=="find_callers"){
        std::string sid=resolve_symbol_id_arg(db,args);
        auto sent=entity(db,sid);
        auto sdata=parse(sent["symbol_data"]);
        std::string sem_key=sdata.value("semantic_key","");
        std::string sname=sent["symbol_name"].get<std::string>();
        std::string overrides_key=sdata.value("overrides_key","");
        std::set<std::string> impl_keys;
        for(const auto& im:sdata.value("implements",Json::array()))
            if(im.contains("semantic_key"))impl_keys.insert(im["semantic_key"].get<std::string>());
        bool callers_only=(kind=="callers"||kind=="find_callers");
        bool include_approx=args.value("include_approximate",true);
        std::string ref_sql="SELECT 'D'||d.id AS id,'F'||d.file_id AS file_id,d.kind,d.name,f.path,f.absolute_path,d.data FROM dependencies d JOIN files f ON d.file_id=f.id WHERE f.deleted=0";
        Json ref_params=Json::array();
        if(callers_only)ref_sql+=" AND d.kind IN('call','CallExpr')";
        std::string cond="json_extract(d.data,'$.target_symbol_id')=? OR d.target_id=?";
        ref_params.push_back(sid);ref_params.push_back(sent["id"]);
        std::vector<std::string> key_list;
        if(!sem_key.empty())key_list.push_back(sem_key);
        if(!overrides_key.empty())key_list.push_back(overrides_key);
        for(const auto& k:impl_keys)if(!k.empty())key_list.push_back(k);
        if(!key_list.empty()){
            std::string ph;
            for(const auto& k:key_list){if(!ph.empty())ph+=',';ph+='?';ref_params.push_back(k);}
            cond+=" OR json_extract(d.data,'$.target_key') IN("+ph+")";
        }
        if(include_approx){
            cond+=" OR d.name=? OR json_extract(d.data,'$.identifier_name')=? OR json_extract(d.data,'$.resolution')='CANDIDATES_AMBIGUOUS'";
            ref_params.push_back(sname);ref_params.push_back(sname);
        }
        ref_sql+=" AND ("+cond+") ORDER BY d.id LIMIT ?";
        ref_params.push_back(std::clamp(args.value("scan_limit",5000),100,50000));
        rows=Json::array();
        for(const auto& drow:db.query(ref_sql,ref_params)){
            auto dd=parse(drow["data"]);
            std::string dkind=drow["kind"].get<std::string>();
            if(callers_only&&dkind!="call"&&dkind!="CallExpr")continue;
            std::string t_sid=dd.value("target_symbol_id","");
            std::string t_key=dd.value("target_key","");
            std::string res=dd.value("resolution","UNKNOWN");
            std::string disp=dd.value("dispatch","direct");
            bool match=(t_sid==sid)||(!sem_key.empty()&&t_key==sem_key);
            if(!match&&! t_key.empty()&&(t_key==overrides_key||impl_keys.contains(t_key))){
                match=true;
                disp="virtual_or_interface";
            }
            if(!match&&include_approx){
                if(res=="CANDIDATES_AMBIGUOUS"){
                    for(const auto& c:dd.value("candidate_symbols",Json::array()))if(c.value("symbol_id","")==sid)match=true;
                    for(const auto& c:dd.value("candidates",Json::array()))if(!sem_key.empty()&&c.value("semantic_key","")==sem_key)match=true;
                }else if(t_sid.empty()&&t_key.empty()&&dd.value("identifier_name",drow["name"].get<std::string>())==sname){
                    match=true;
                }
            }
            if(!match)continue;
            rows.push_back({
                {"id",drow["id"]},{"file_id",drow["file_id"]},{"path",drow["path"]},{"absolute_path",drow["absolute_path"]},
                {"caller_symbol_id",dd.value("caller_symbol_id","")},{"caller_qualified_name",dd.value("caller_qualified_name","")},
                {"target_symbol_id",t_sid.empty()?sid:t_sid},{"kind",dkind},{"name",drow["name"]},
                {"line",dd.value("line",0)},{"column",dd.value("column",0)},
                {"offset",dd.value("offset",0)},{"end_offset",dd.value("end_offset",0)},
                {"identifier_offset",dd.value("identifier_offset",0)},{"identifier_end_offset",dd.value("identifier_end_offset",0)},
                {"resolved",dd.value("resolved",false)},{"resolution",res},{"dispatch",disp}
            });
        }
    }else if(kind=="callees"||kind=="find_callees"){
        std::string sid=resolve_symbol_id_arg(db,args);
        auto sent=entity(db,sid);
        auto sdata=parse(sent["symbol_data"]);
        std::string sem_key=sdata.value("semantic_key","");
        int s_line=sdata.value("line",1),e_line=sdata.value("end_line",s_line);
        rows=Json::array();
        for(const auto& drow:db.query("SELECT 'D'||d.id AS id,'F'||d.file_id AS file_id,d.kind,d.name,d.data FROM dependencies d WHERE d.file_id=? ORDER BY d.id",{sent["id"]})){
            auto dd=parse(drow["data"]);
            int dline=dd.value("line",0);
            bool match=(dd.value("caller_symbol_id","")==sid)||(!sem_key.empty()&&dd.value("caller_key","")==sem_key)||(dd.value("caller_symbol_id","").empty()&&dline>=s_line&&dline<=e_line);
            if(!match)continue;
            rows.push_back({
                {"id",drow["id"]},{"caller_symbol_id",sid},{"target_symbol_id",dd.value("target_symbol_id","")},
                {"kind",drow["kind"]},{"name",drow["name"]},{"target_qualified_name",dd.value("target_qualified_name",drow["name"].get<std::string>())},
                {"target_signature",dd.value("target_signature","")},{"line",dline},{"column",dd.value("column",0)},
                {"resolved",dd.value("resolved",false)},{"resolution",dd.value("resolution","UNKNOWN")},{"dispatch",dd.value("dispatch","direct")}
            });
        }
    }else if(kind=="implementations"||kind=="find_implementations"||kind=="type_hierarchy"){
        std::string sid=resolve_symbol_id_arg(db,args);
        auto sent=entity(db,sid);
        auto sdata=parse(sent["symbol_data"]);
        std::string sem_key=sdata.value("semantic_key","");
        std::string sqname=sent["symbol_qualified_name"].get<std::string>();
        std::string sname=sent["symbol_name"].get<std::string>();
        Json implementations=Json::array();
        for(const auto& row:db.query("SELECT s.id,s.file_id,s.name,s.qualified_name,s.kind,s.line,f.path,s.data FROM symbols s JOIN files f ON s.file_id=f.id WHERE s.active=1 AND f.deleted=0 AND s.id<>? ORDER BY s.id",{sent["symbol_id"]})){
            auto d=parse(row["data"]);
            bool is_impl=false;
            std::string rel_kind;
            if((!sem_key.empty()&&d.value("overrides_key","")==sem_key)||d.value("overrides_name","")==sqname){
                is_impl=true;rel_kind="method_override";
            }
            for(const auto& im:d.value("implements",Json::array())){
                if((!sem_key.empty()&&im.value("semantic_key","")==sem_key)||im.value("qualified_name","")==sqname){
                    is_impl=true;rel_kind="interface_method_implementation";
                }
            }
            for(const auto& bt:d.value("base_types",Json::array())){
                if((!sem_key.empty()&&bt.value("semantic_key","")==sem_key)||bt.value("qualified_name","")==sqname||bt.value("name","")==sname){
                    is_impl=true;rel_kind=bt.value("kind","derived_type");
                }
            }
            if(is_impl){
                implementations.push_back({
                    {"id","S"+std::to_string(row["id"].get<int64_t>())},{"file_id","F"+std::to_string(row["file_id"].get<int64_t>())},
                    {"name",row["name"]},{"qualified_name",row["qualified_name"]},{"kind",row["kind"]},
                    {"relation",rel_kind},{"path",row["path"]},{"line",row["line"]}
                });
            }
        }
        if(kind=="type_hierarchy"){
            rows=Json::array({{
                {"id",sid},{"name",sname},{"qualified_name",sqname},{"kind",sent["symbol_kind"]},
                {"base_types",sdata.value("base_types",Json::array())},
                {"overrides_key",sdata.value("overrides_key","")},
                {"implements",sdata.value("implements",Json::array())},
                {"derived_and_implementations",implementations}
            }});
        }else{
            rows=implementations;
        }
    }else if(kind=="impact_analysis"){
        std::string start_id=args.contains("id")?args["id"].get<std::string>():resolve_symbol_id_arg(db,args);
        auto start_ent=entity(db,start_id);
        std::string eid=start_ent["entity_id"].get<std::string>();
        int max_depth=std::clamp(args.value("max_depth",3),1,6);
        std::map<std::string,int> visited_dist;
        std::map<std::string,std::string> visited_res;
        std::vector<std::string> frontier{eid};
        visited_dist[eid]=0;visited_res[eid]="ORIGIN";
        for(int dist=0;dist<max_depth&&!frontier.empty();++dist){
            std::vector<std::string> next_frontier;
            for(size_t offset=0;offset<frontier.size();offset+=64){
                const size_t chunk=std::min<size_t>(64,frontier.size()-offset);
                std::string ph;Json qparams=Json::array();
                for(size_t i=0;i<chunk;++i){if(!ph.empty())ph+=',';ph+='?';qparams.push_back(frontier[offset+i]);}
                for(const auto& drow:db.query("SELECT d.data FROM dependencies d JOIN files f ON d.file_id=f.id WHERE f.deleted=0 AND json_extract(d.data,'$.target_symbol_id') IN("+ph+") LIMIT 2000",qparams)){
                    auto dd=parse(drow["data"]);
                    std::string c_sid=dd.value("caller_symbol_id","");
                    if(!c_sid.empty()&&!visited_dist.contains(c_sid)){
                        visited_dist[c_sid]=dist+1;
                        visited_res[c_sid]=dd.value("resolution","UNKNOWN");
                        next_frontier.push_back(c_sid);
                    }
                }
            }
            frontier=std::move(next_frontier);
        }
        Json affected_symbols=Json::array();
        std::set<std::string> affected_files_set={file_id(start_ent)};
        for(const auto& [sid_k,dist]:visited_dist){
            if(sid_k.starts_with("S")){
                try{
                    auto e=entity(db,sid_k);
                    affected_files_set.insert(file_id(e));
                    affected_symbols.push_back({{"id",sid_k},{"file_id",file_id(e)},{"qualified_name",e["symbol_qualified_name"]},{"path",e["path"]},{"distance",dist},{"resolution",visited_res[sid_k]}});
                }catch(...){}
            }
        }
        Json affected_files=Json::array();
        for(const auto& fid_k:affected_files_set)affected_files.push_back(fid_k);
        std::set<std::string> impacted_ids=affected_files_set;
        for(const auto& [sid_k,_]:visited_dist)impacted_ids.insert(sid_k);
        Json affected_mappings=Json::array();
        std::set<std::string> affected_cases_set;
        for(const auto& mrow:db.query("SELECT * FROM mappings LIMIT 5000")){
            auto m=mapping_json(mrow);
            bool hit=impacted_ids.contains(m["source"].get<std::string>());
            if(!hit)for(const auto& tid:m["targets"])if(impacted_ids.contains(tid.get<std::string>())){hit=true;break;}
            if(hit){
                affected_mappings.push_back(m["source"]);
                for(const auto& cid:m["cases"])affected_cases_set.insert(cid.get<std::string>());
            }
        }
        Json affected_cases=Json::array();
        for(const auto& cid:affected_cases_set)affected_cases.push_back(cid);
        rows=Json::array({{
            {"origin",eid},
            {"max_depth",max_depth},
            {"affected_symbols",affected_symbols},
            {"affected_files",affected_files},
            {"affected_mappings",affected_mappings},
            {"affected_cases",affected_cases}
        }});
    }else if(kind=="hybrid_search"){
        std::string role_f=args.value("role","");
        std::string lang_f=args.value("language","");
        std::string root_f=args.value("root","");
        std::string kind_f=args.value("symbol_kind","");
        std::string sig_f=args.value("signature_contains","");
        std::string calls_f=low_str(args.value("calls",""));
        std::string mig_state_f=low_str(args.value("migration_state",""));
        std::string missing_lang_f=args.value("missing_target_language","");
        std::string ast_pattern=args.value("ast_pattern","");
        std::string regex_pattern=args.value("regex_pattern","");
        std::string low_text=low_str(text);

        std::optional<std::regex> explicit_re;
        if(!regex_pattern.empty()){
            explicit_re=std::regex(regex_pattern);
        }

        // H-09: Query SQLite FTS5 search index when text is provided
        std::set<std::string> fts_hit_entities;
        if(!text.empty()){
            try{
                std::string phrase=text;
                size_t at=0;
                while((at=phrase.find('"',at))!=std::string::npos){phrase.insert(at,"\"");at+=2;}
                for(const auto& frow:db.query("SELECT entity_id FROM search WHERE search MATCH ? LIMIT 500",{"\""+phrase+"\""})){
                    fts_hit_entities.insert(frow["entity_id"].get<std::string>());
                }
            }catch(...){}
        }

        std::string sym_sql="SELECT s.id,s.file_id,s.name,s.qualified_name,s.kind,s.line,s.data,f.path,f.absolute_path,f.root,f.language,r.role FROM symbols s JOIN files f ON s.file_id=f.id LEFT JOIN roots r ON f.root=r.id WHERE s.active=1 AND f.deleted=0";
        Json sym_params=Json::array();
        if(!role_f.empty()){sym_sql+=" AND r.role=?";sym_params.push_back(role_f);}
        if(!root_f.empty()){sym_sql+=" AND f.root=?";sym_params.push_back(root_f);}
        if(!low_text.empty()&&ast_pattern.empty()&&!explicit_re.has_value()&&fts_hit_entities.empty()){
            sym_sql+=" AND (instr(lower(s.name),?)>0 OR instr(lower(s.qualified_name),?)>0 OR instr(lower(s.data),?)>0)";
            sym_params.push_back(low_text);sym_params.push_back(low_text);sym_params.push_back(low_text);
        }
        sym_sql+=" ORDER BY s.id LIMIT ?";
        sym_params.push_back(std::clamp(args.value("scan_limit",5000),100,50000));

        rows=Json::array();
        std::map<std::string,std::string> file_cache;
        auto all_sym_rows=db.query(sym_sql,sym_params);
        std::map<std::string,Json> map_by_src;
        std::map<std::string,std::string> file_lang_by_entity;
        std::map<std::string,std::vector<std::string>> callees_by_sym;
        if(!all_sym_rows.empty()){
            std::set<std::string> src_keys;
            std::set<int64_t> file_ids;
            for(const auto& sr:all_sym_rows){
                src_keys.insert("S"+std::to_string(sr["id"].get<int64_t>()));
                src_keys.insert("F"+std::to_string(sr["file_id"].get<int64_t>()));
                file_ids.insert(sr["file_id"].get<int64_t>());
                file_lang_by_entity["F"+std::to_string(sr["file_id"].get<int64_t>())]=sr["language"].get<std::string>();
            }
            for(const auto& mrow:db.query("SELECT * FROM mappings LIMIT 5000")){
                auto mj=mapping_json(mrow);
                auto src_key=mj["source"].get<std::string>();
                if(src_keys.contains(src_key)){
                    const bool fresh=hashes_match(db,parse(mrow["hashes"]));
                    mj["fresh"]=fresh;
                    mj["state"]=!fresh?"STALE":(mj["declared_complete"]==true?"DECLARED_COMPLETE":"MAPPED");
                    mj["verified_in_configured_cases"]=fresh&&mj["declared_complete"]==true&&!mj["cases"].empty();
                    map_by_src[src_key]=std::move(mj);
                }
            }
            if(!missing_lang_f.empty()){
                for(const auto& [_,mj]:map_by_src){
                    for(const auto& tid:mj["targets"]){
                        auto ts=tid.get<std::string>();
                        if(file_lang_by_entity.contains(ts))continue;
                        try{auto tent=entity(db,ts);file_lang_by_entity[ts]=tent["language"].get<std::string>();}catch(...){}
                    }
                }
            }
            if(!calls_f.empty()){
                Statement dq(db,"SELECT data FROM dependencies WHERE file_id=?");
                for(auto fid:file_ids){
                    dq.bind({fid});
                    while(dq.next()){
                        auto dd=parse(dq.row()["data"]);
                        auto csid=dd.value("caller_symbol_id","");
                        if(!csid.empty())callees_by_sym[csid].push_back(dd.value("target_qualified_name",dd.value("name","")));
                    }
                    dq.reset();
                }
            }
        }
        struct SymSpanInfo { int64_t sym_id; size_t off; size_t end_off; std::string kind; };
        std::map<int64_t,std::vector<SymSpanInfo>> spans_by_file;
        if(!ast_pattern.empty()){
            for(const auto& sr:all_sym_rows){
                const std::string sk=sr["kind"].get<std::string>();
                if(sk=="VarDecl"||sk=="FieldDecl"||sk=="ParmDecl")continue;
                if(!kind_f.empty()&&!symbol_kind_matches(sk,kind_f))continue;
                auto sd=parse(sr["data"]);
                size_t o=sd.value("offset",size_t(0)),eo=sd.value("end_offset",size_t(0));
                if(eo>o)spans_by_file[sr["file_id"].get<int64_t>()].push_back({sr["id"].get<int64_t>(),o,eo,sk});
            }
        }
        for(const auto& srow:all_sym_rows){
            if(!ast_pattern.empty()&&kind_f.empty()){
                const std::string sk=srow["kind"].get<std::string>();
                if(sk=="VarDecl"||sk=="FieldDecl"||sk=="ParmDecl")continue;
            }
            if(!role_f.empty()&&srow["role"]!=role_f)continue;
            // H-03: Strict language family and dialect matching (C23 != C++23 != C#)
            if(!lang_f.empty()&&!mt::ast::language_matches(srow["language"].get<std::string>(),lang_f,false))continue;
            if(!root_f.empty()&&srow["root"]!=root_f)continue;
            if(!kind_f.empty()&&!symbol_kind_matches(srow["kind"].get<std::string>(),kind_f))continue;
            auto sdata=parse(srow["data"]);
            std::string sig=sdata.value("signature","");
            if(!sig_f.empty()&&low_str(sig).find(low_str(sig_f))==std::string::npos)continue;

            std::string sid="S"+std::to_string(srow["id"].get<int64_t>());
            std::string fid_s="F"+std::to_string(srow["file_id"].get<int64_t>());
            auto mit=map_by_src.find(sid);
            if(mit==map_by_src.end())mit=map_by_src.find(fid_s);
            bool is_mapped=(mit!=map_by_src.end());
            bool is_migrated=is_mapped&&mit->second.value("fresh",false)&&mit->second.value("declared_complete",false);
            bool is_verified=is_mapped&&mit->second.value("verified_in_configured_cases",false);
            std::string mstate=!is_mapped?"unmapped":mit->second.value("state","UNMAPPED");

            if(!mig_state_f.empty()){
                if(mig_state_f=="unmapped"&&is_mapped)continue;
                if(mig_state_f=="pending"&&is_migrated)continue;
                if(mig_state_f=="migrated"&&!is_migrated)continue;
                if(mig_state_f=="verified"&&!is_verified)continue;
                if(mig_state_f=="stale"&&low_str(mstate)!="stale")continue;
            }
            if(!missing_lang_f.empty()){
                bool has_target_lang=false;
                if(is_migrated){
                    for(const auto& tid:mit->second["targets"]){
                        auto tlang=file_lang_by_entity[tid.get<std::string>()];
                        // H-03: Use strict language family matching so C, C++, and C# never collide
                        if(mt::ast::language_matches(tlang,missing_lang_f,true))has_target_lang=true;
                    }
                }
                if(has_target_lang)continue;
            }

            std::string body_slice;
            auto get_body=[&]()->const std::string&{
                if(!body_slice.empty())return body_slice;
                std::string abs_p=srow["absolute_path"].get<std::string>();
                auto fit=file_cache.find(abs_p);
                if(fit==file_cache.end())fit=file_cache.emplace(abs_p,read_text(fs::path(wide(abs_p)),16*1024*1024)).first;
                size_t off=sdata.value("offset",size_t(0)),end_off=sdata.value("end_offset",size_t(0));
                if(end_off>off&&end_off<=fit->second.size())body_slice=fit->second.substr(off,end_off-off);
                return body_slice;
            };

            int score=0;
            if(!calls_f.empty()){
                bool calls_hit=false;
                for(const auto& cal:callees_by_sym[sid])if(low_str(cal).find(calls_f)!=std::string::npos){calls_hit=true;break;}
                if(!calls_hit)continue;
                score+=50;
            }

            // H-01: Genuine structural AST matching (ignores comments/strings, parses nested calls/generics, verifies repeated $META equality)
            Json ast_match_json=Json::array();
            if(!ast_pattern.empty()){
                auto matches=mt::ast::find_ast_matches(get_body(),ast_pattern,srow["language"].get<std::string>());
                if(matches.empty())continue;
                size_t base_off=sdata.value("offset",size_t(0));
                size_t sym_end_off=sdata.value("end_offset",size_t(0));
                size_t sym_len=(sym_end_off>base_off)?(sym_end_off-base_off):0;
                int base_line=srow["line"].get<int>();
                int64_t cur_sym_id=srow["id"].get<int64_t>();
                const auto& file_spans=spans_by_file[srow["file_id"].get<int64_t>()];
                for(const auto& m:matches){
                    size_t abs_m_off=base_off+m.offset;
                    size_t abs_m_end=base_off+m.end_offset;
                    bool covered_by_inner=false;
                    for(const auto& sp:file_spans){
                        if(sp.sym_id==cur_sym_id)continue;
                        size_t sp_len=sp.end_off-sp.off;
                        if(sp_len<sym_len&&abs_m_off>=sp.off&&abs_m_end<=sp.end_off){
                            covered_by_inner=true;
                            break;
                        }
                    }
                    if(covered_by_inner)continue;
                    Json mv=Json::object();
                    for(const auto& [mk,mval]:m.bindings)mv[mk]=mval.text;
                    ast_match_json.push_back({
                        {"matched_text",m.matched_text},
                        {"offset",abs_m_off},
                        {"end_offset",abs_m_end},
                        {"line",base_line+m.line-1},
                        {"metavariables",mv}
                    });
                }
                if(ast_match_json.empty())continue;
                score+=60+static_cast<int>(std::min<size_t>(ast_match_json.size(),5))*5;
            }

            // Explicit textual regex matching (separate from structural ast_pattern)
            if(explicit_re.has_value()){
                if(!std::regex_search(get_body(),*explicit_re))continue;
                score+=40;
            }

            if(!low_text.empty()){
                std::string sname_l=low_str(srow["name"].get<std::string>());
                std::string sqname_l=low_str(srow["qualified_name"].get<std::string>());
                bool text_hit=false;
                if(sname_l==low_text){score+=100;text_hit=true;}
                else if(sname_l.find(low_text)!=std::string::npos){score+=70;text_hit=true;}
                if(sqname_l.find(low_text)!=std::string::npos){score+=40;text_hit=true;}
                if(low_str(sig).find(low_text)!=std::string::npos){score+=30;text_hit=true;}
                if(fts_hit_entities.contains(sid)||fts_hit_entities.contains(fid_s)){
                    if(low_str(get_body()).find(low_text)!=std::string::npos||text_hit){
                        score+=35;text_hit=true;
                    }
                }
                if(!text_hit&&low_str(get_body()).find(low_text)!=std::string::npos){score+=25;text_hit=true;}
                if(!text_hit)continue;
            }else{
                score+=10;
            }
            if(!is_migrated&&srow["role"]=="source")score+=20;

            const bool full_hs_detail=args.value("detail","compact")=="full"||!ast_pattern.empty()==false;
            Json item={
                {"id",sid},{"file_id",fid_s},{"name",srow["name"]},{"qualified_name",srow["qualified_name"]},
                {"kind",srow["kind"]},{"language",srow["language"]},
                {"path",srow["path"]},{"line",srow["line"]},{"score",score}
            };
            if(full_hs_detail||!sig_f.empty()||args.value("include_body",false)){
                item["signature"]=sig;item["role"]=srow["role"];
                item["end_line"]=sdata.value("end_line",srow["line"].get<int>());
                item["offset"]=sdata.value("offset",0);item["end_offset"]=sdata.value("end_offset",0);
                item["migration_state"]=mstate;
            }
            if(!ast_match_json.empty())item["ast_matches"]=ast_match_json;
            if(args.value("include_body",false))item["body"]=get_body();
            rows.push_back(std::move(item));
        }
        std::stable_sort(rows.begin(),rows.end(),[](const Json& a,const Json& b){
            return a["score"].get<int>()>b["score"].get<int>();
        });
        if(!ast_pattern.empty()&&args.value("detail","compact")!="full"){
            for(auto& r:rows)r.erase("score");
        }
    }else if(kind=="language_capabilities"){
        rows=language_capabilities(p);
    }else if(kind=="claims"){
        if(!has_claims_table(db))rows=Json::array();
        else{
            if(args.value("active_only",true))rows=db.query("SELECT * FROM task_claims WHERE expires_at>? ORDER BY entity",{now()});
            else rows=db.query("SELECT * FROM task_claims ORDER BY entity");
        }
    }else if(kind=="export_bundle"){
        Json bmaps=Json::array();
        auto describe_unit=[&](const std::string& uid)->Json{
            auto e=entity(db,uid);
            Json desc={{"root",e["root"]},{"path",e["path"]},{"file_sha256",e["sha256"]}};
            if(e.contains("symbol_id")){
                auto sdata=parse(e["symbol_data"]);
                desc["kind"]="symbol";
                desc["name"]=e["symbol_name"];
                desc["qualified_name"]=e["symbol_qualified_name"];
                desc["signature"]=sdata.value("signature","");
                desc["symbol_sha256"]=sdata.value("symbol_sha256","");
            }else{
                desc["kind"]="file";
            }
            return desc;
        };
        for(const auto& mrow:db.query("SELECT * FROM mappings ORDER BY source")){
            auto m=mapping_json(mrow);
            try{
                Json src_desc=describe_unit(m["source"].get<std::string>());
                Json tgt_descs=Json::array();
                for(const auto& tid:m["targets"])tgt_descs.push_back(describe_unit(tid.get<std::string>()));
                bmaps.push_back({
                    {"source",src_desc},{"targets",tgt_descs},{"disposition",m["disposition"]},
                    {"declared_complete",m["declared_complete"]},{"cases",m["cases"]},
                    {"note",m["note"]},{"actor",m["actor"]},{"at",m["at"]}
                });
            }catch(...){}
        }
        rows=Json::array({{{"schema","migratool.bundle/1"},{"exported_at",now()},{"revision",db.revision()},{"mappings",bmaps}}});
    }else if(kind=="task_context"||kind=="get_task_context"){
        rows=Json::array({build_task_context(p,db,args)});
    }else if(kind=="dependencies"){
        where=" WHERE f.deleted=0";if(args.contains("file"))add("d.file_id=?",entity_number(args["file"],'F'));if(!text.empty())add("d.name LIKE ?",needle);
        rows=db.query("SELECT 'D'||d.id AS id,'F'||d.file_id AS file_id,'F'||d.target_id AS target_id,d.kind,d.name,f.absolute_path,d.data FROM dependencies d JOIN files f ON d.file_id=f.id"+where+" ORDER BY d.id",params);
        for(auto& row:rows){row["details"]=parse(row["data"]);row.erase("data");}
    }else if(kind=="relations"){
        rows=db.query("SELECT 'F'||l.doc_id AS documentation_id,'F'||l.code_id AS code_id,l.reason,d.absolute_path AS documentation_path,c.absolute_path AS code_path FROM doc_links l JOIN files d ON l.doc_id=d.id JOIN files c ON l.code_id=c.id ORDER BY l.doc_id,l.code_id");
        if(args.contains("file")){auto id=args["file"];Json selected=Json::array();for(const auto& r:rows)if(r["code_id"]==id||r["documentation_id"]==id)selected.push_back(r);rows=selected;}
    }else if(kind=="notes"){
        where=" WHERE 1=1";if(!text.empty())add("text LIKE ?",needle);if(args.contains("file"))add("entity=?",args["file"]);if(args.contains("actor"))add("actor=?",args["actor"]);
        rows=db.query("SELECT 'N'||id AS id,entity,text,actor,at,revision FROM notes"+where+" ORDER BY id",params);
    }else if(kind=="mappings"){
        where=" WHERE 1=1";if(args.contains("file"))add("source=?",args["file"]);if(!text.empty())add("note LIKE ?",needle);
        rows=db.query("SELECT * FROM mappings"+where+" ORDER BY source",params);for(auto& r:rows)r=mapping_json(r);
    }else if(kind=="phases"){
        rows=db.query("SELECT id AS phase_id,id,title,ordinal,state,description,gate_builds,gate_cases,target_modules,updated_at,actor,revision FROM project_phases ORDER BY ordinal,id");
        for(auto& r:rows)for(const auto* k:{"gate_builds","gate_cases","target_modules"})if(r.contains(k))r[k]=safe_parse(r[k]);
    }else if(kind=="project_tasks"){
        rows=db.query("SELECT id AS task_id,id,phase_id,title,kind AS task_kind,state,priority,assignee,source_entities,target_entities,required_cases,depends_on,note,created_at,updated_at,actor,revision FROM project_tasks ORDER BY priority,id");
        for(auto& r:rows)for(const auto* k:{"source_entities","target_entities","required_cases","depends_on"})if(r.contains(k))r[k]=safe_parse(r[k]);
    }else if(kind=="doc_audits"){
        rows=db.query("SELECT 'F'||f.id AS id,'F'||f.id AS doc_id,f.root,f.path,f.absolute_path,COALESCE(CASE WHEN d.doc_id IS NULL THEN 'UNREVIEWED' WHEN d.sha256<>f.sha256 THEN 'STALE' ELSE d.state END,'UNREVIEWED') AS state,COALESCE(d.disposition,'unreviewed') AS disposition,COALESCE(d.linked_targets,'[]') AS linked_targets,COALESCE(d.extracted_tasks,'[]') AS extracted_tasks,COALESCE(d.note,'') AS note,COALESCE(d.actor,'') AS actor,COALESCE(d.at,'') AS at FROM files f LEFT JOIN roots r ON r.id=f.root LEFT JOIN doc_audits d ON d.doc_id='F'||f.id WHERE f.deleted=0 AND r.role='source' AND f.category='documentation' ORDER BY f.id");
        for(auto& r:rows)for(const auto* k:{"linked_targets","extracted_tasks"})if(r.contains(k))r[k]=safe_parse(r[k]);
    }else if(kind=="module_matrix"){
        return scale_module_matrix(p,db,args)["items"];
    }else if(kind=="search"){
        if(text.empty())throw std::runtime_error("Búsqueda global requiere text");auto phrase=text;size_t at=0;while((at=phrase.find('"',at))!=std::string::npos){phrase.insert(at,"\"");at+=2;}
        rows=db.query("SELECT entity_type,entity_id,snippet(search,2,'[',']','…',24)AS excerpt FROM search WHERE search MATCH ? ORDER BY rank,entity_id",{"\""+phrase+"\""});
    }else throw std::runtime_error("Tipo de consulta desconocido: "+kind);
    if(args.contains("ids")){std::set<std::string> ids;for(const auto& id:args["ids"])ids.insert(id);Json selected=Json::array();for(const auto& row:rows)if(ids.contains(row.value("id",row.value("entity_id",""))))selected.push_back(row);rows=selected;}
    return rows;
}
Json next_items(Database& db,const Json& status,const Json& args){
    Json tasks=Json::array();std::set<std::string> pending;
    for(const auto& file:status["files"])if(file["migration_percent"].is_null()||file["migration_percent"]!=100||file["units_with_passing_cases_percent"]!=100)pending.insert(file["id"]);
    std::map<std::string,std::set<std::string>> deps;
    for(const auto& d:db.query("SELECT DISTINCT 'F'||file_id AS source,'F'||target_id AS target FROM dependencies WHERE target_id IS NOT NULL AND file_id<>target_id"))if(pending.contains(d["target"]))deps[d["source"]].insert(d["target"]);
    for(const auto& file:status["files"]){if(!pending.contains(file["id"]))continue;if(args.contains("file")&&args["file"]!=file["id"])continue;
        Json blockers=Json::array();for(const auto& id:deps[file["id"]])blockers.push_back(id);
        tasks.push_back({{"id",file["id"]},{"source_path",file["source_path"]},{"migration_percent",file["migration_percent"]},{"units",file["total_units"]},{"missing",file["missing"]},{"unresolved_dependencies",blockers}});
    }
    std::stable_sort(tasks.begin(),tasks.end(),[](const auto& a,const auto& b){return a["unresolved_dependencies"].size()<b["unresolved_dependencies"].size();});return tasks;
}

void apply_field_preset(Json& q, const std::string& default_kind = "files") {
    if (q.contains("fields") && q["fields"].is_array() && !q["fields"].empty()) return;
    std::string preset = q.value("preset", "");
    if (preset.empty()) return;
    std::string kind = q.value("kind", default_kind);
    if (preset == "minimal") {
        if (kind == "symbols" || kind == "find_symbol" || kind == "symbols_overview" || kind == "get_symbols_overview")
            q["fields"] = Json::array({"id", "file_id", "name", "kind", "line", "signature"});
        else if (kind == "status_files")
            q["fields"] = Json::array({"id", "source_path", "state", "total_units", "migrated_units", "migration_percent"});
        else
            q["fields"] = Json::array({"id", "path", "language", "category", "relevant", "analysis"});
    } else if (preset == "agent") {
        if (kind == "symbols" || kind == "find_symbol" || kind == "symbols_overview" || kind == "get_symbols_overview")
            q["fields"] = Json::array({"id", "file_id", "name", "qualified_name", "kind", "line", "end_line", "offset", "end_offset", "signature"});
        else if (kind == "status_files")
            q["fields"] = Json::array({"id", "source_path", "language", "state", "total_units", "migrated_units", "migration_percent", "target_paths", "missing"});
        else
            q["fields"] = Json::array({"id", "root", "role", "path", "language", "category", "relevant", "analysis"});
    }
}

Json build_session_bootstrap(const Project& p, Database& db, const Json& args) {
    const bool is_scale = scale_enabled(p, db);
    Json status_summary = is_scale ? scale_summary(p, db) : compact_status(cached_status(p, db));
    Json blockers = status_summary.value("denominator_blockers", Json(nullptr));
    for (const auto* drop : {"files", "mappings", "coverage", "diagnostics", "exclusions",
                             "denominator_blockers", "limits", "percentage_basis", "cache_key",
                             "schema", "name", "project_path"}) {
        if (status_summary.contains(drop)) status_summary.erase(drop);
    }
    if (status_summary.contains("cases") && status_summary["cases"].is_array()) {
        Json compact_cases = Json::array();
        for (const auto& c : status_summary["cases"]) {
            compact_cases.push_back({{"id", c.value("id", "")}, {"state", c.value("state", "")}, {"fresh", c.value("fresh", false)}});
        }
        status_summary["cases"] = std::move(compact_cases);
    }
    if (status_summary.contains("builds") && status_summary["builds"].is_array()) {
        Json compact_builds = Json::array();
        for (const auto& b : status_summary["builds"]) {
            compact_builds.push_back({{"id", b.value("id", "")}, {"root", b.value("root", "")}, {"state", b.value("state", "")}, {"fresh", b.value("fresh", false)}});
        }
        status_summary["builds"] = std::move(compact_builds);
    }
    if (blockers.is_null()) {
        int64_t pending_ana = db.integer("SELECT COUNT(*) FROM files WHERE deleted=0 AND relevant=1 AND analysis<>'ANALYZED'");
        int64_t unclass_cnt = db.integer("SELECT COUNT(*) FROM files WHERE deleted=0 AND category='unknown'");
        if (status_summary.value("migration_percent", Json(nullptr)).is_null() || pending_ana > 0 || unclass_cnt > 0) {
            Json pending_by_lang = db.query("SELECT COALESCE(language,'unknown') AS language,COALESCE(analysis,'PENDING') AS analysis,COUNT(*) AS count FROM files WHERE deleted=0 AND relevant=1 AND analysis<>'ANALYZED' GROUP BY language,analysis ORDER BY count DESC LIMIT 10");
            Json unclassified_samples = db.query("SELECT 'F'||id AS id,path FROM files WHERE deleted=0 AND category='unknown' ORDER BY id LIMIT 5");
            std::string hint = "Ejecute inventory_sync para actualizar el censo y análisis.";
            if (unclass_cnt > 0) hint = "Clasifique archivos 'unknown' con batch_update(op='classify' o 'classify_pattern').";
            else if (pending_ana > 0) hint = "Complete el análisis con inventory_sync o reclasifique archivos no-código con relevant:false.";
            blockers = {
                {"pending_analysis", pending_ana},
                {"unclassified_files", unclass_cnt},
                {"pending_by_language", pending_by_lang},
                {"unclassified_samples", unclassified_samples},
                {"remediation_hint", hint}
            };
        }
    }
    if (blockers.is_object() && blockers.contains("recent_scan_errors") && blockers["recent_scan_errors"].is_array()) {
        Json compact_errs = Json::array();
        for (const auto& er : blockers["recent_scan_errors"]) {
            if (compact_errs.size() >= 2) break;
            compact_errs.push_back({{"path", er.value("path", "")}, {"operation", er.value("operation", "")}});
        }
        blockers["recent_scan_errors"] = std::move(compact_errs);
    }
    Json roots_info = Json::array();
    for (const auto& r : p.config.value("roots", Json::array())) {
        std::string rid = r.value("id", "");
        roots_info.push_back({
            {"id", rid},
            {"role", r.value("role", "")},
            {"path", path_text(p.resolve(r.value("path", "")))},
            {"active_files", db.integer("SELECT COUNT(*) FROM files WHERE root=? AND deleted=0", {rid})},
            {"relevant_files", db.integer("SELECT COUNT(*) FROM files WHERE root=? AND deleted=0 AND relevant=1", {rid})}
        });
    }
    Json top_modules = Json::array();
    if (status_summary.value("pending_units", int64_t(1)) > 0) {
        try {
            auto mod_page = scale_module_matrix(p, db, {{"limit", 100}, {"depth", args.value("depth", 2)}});
            std::vector<Json> pending_mods;
            for (const auto& m : mod_page.value("items", Json::array())) {
                int64_t tot = m.value("total_units", int64_t(0));
                int64_t mig = m.value("migrated_units", int64_t(0));
                if (tot > mig) pending_mods.push_back(m);
            }
            std::sort(pending_mods.begin(), pending_mods.end(), [](const Json& a, const Json& b) {
                return (a.value("total_units", int64_t(0)) - a.value("migrated_units", int64_t(0))) >
                       (b.value("total_units", int64_t(0)) - b.value("migrated_units", int64_t(0)));
            });
            size_t mlim = static_cast<size_t>(std::clamp(args.value("module_limit", 5), 1, 20));
            for (size_t i = 0; i < std::min(mlim, pending_mods.size()); ++i) {
                const auto& pm = pending_mods[i];
                top_modules.push_back({
                    {"source_module", pm["source_module"]},
                    {"total_units", pm["total_units"]},
                    {"migrated_units", pm["migrated_units"]},
                    {"pending_units", pm["total_units"].get<int64_t>() - pm["migrated_units"].get<int64_t>()},
                    {"migration_percent", pm["migration_percent"]},
                    {"state", pm["state"]}
                });
            }
        } catch (...) {}
    }

    Json active_claims = has_claims_table(db)
        ? db.query("SELECT entity,actor,expires_at,note FROM task_claims WHERE expires_at>? ORDER BY expires_at LIMIT 10", {now()})
        : Json::array();

    int task_lim = std::clamp(args.value("limit", 5), 1, 20);
    Json next_tasks_list = Json::array();
    if (is_scale) {
        auto fpage = scale_file_status(p, db, {{"limit", task_lim}, {"pending_reason", "missing_target"}, {"include_total", false},
                                               {"fields", Json::array({"id", "source_path", "language", "total_units", "migrated_units", "migration_percent", "state", "missing"})}});
        next_tasks_list = fpage.value("items", Json::array());
        if (next_tasks_list.empty()) {
            auto fpage2 = scale_file_status(p, db, {{"limit", task_lim}, {"include_total", false},
                                                    {"fields", Json::array({"id", "source_path", "language", "total_units", "migrated_units", "migration_percent", "state", "missing"})}});
            for (const auto& item : fpage2.value("items", Json::array())) {
                if (item.value("migrated_units", int64_t(0)) < item.value("total_units", int64_t(1)))
                    next_tasks_list.push_back(item);
            }
        }
    } else {
        auto st = cached_status(p, db);
        auto all_next = next_items(db, st, args);
        for (size_t i = 0; i < std::min<size_t>(static_cast<size_t>(task_lim), all_next.size()); ++i)
            next_tasks_list.push_back(all_next[i]);
    }

    auto recent = db.query("SELECT revision,actor,action,entity,at FROM history ORDER BY id DESC LIMIT 1");
    return {
        {"schema", "migratool.session_bootstrap/1"},
        {"revision", db.revision()},
        {"config_revision", p.revision},
        {"project", {{"name", p.config.value("name", "")}, {"path", path_text(p.file)}, {"scale_enabled", is_scale}}},
        {"status", status_summary},
        {"denominator_blockers", blockers},
        {"roots", roots_info},
        {"top_pending_modules", top_modules},
        {"active_claims", active_claims},
        {"next_tasks", next_tasks_list},
        {"last_activity", recent.empty() ? Json(nullptr) : recent[0]},
        {"workflow_hint", "1) get_task_context(task_ids=[...]) 2) complete_task(actor, write_files/edits, mappings, verify_cases) o batch_update(classify_pattern/map_pattern/auto_map_symbols)"}
    };
}

Json run_complete_task(const Project& p, const Json& args, Log log, Cancel* cancel) {
    std::string actor = args.value("actor", "agent");
    if (actor.empty()) actor = "agent";
    const bool dry_run = args.value("dry_run", false);
    const bool auto_sync = args.value("auto_sync_touched", true);

    Json written_files = Json::array();
    if (args.contains("write_files") && args["write_files"].is_array() && !args["write_files"].empty()) {
        std::vector<std::pair<std::string, fs::path>> target_roots;
        for (const auto& r : p.config.value("roots", Json::array())) {
            if (r.value("role", "") == "target") {
                std::error_code ec;
                target_roots.push_back({r.at("id").get<std::string>(), fs::weakly_canonical(p.resolve(r.at("path")), ec)});
            }
        }
        if (target_roots.empty()) throw std::runtime_error("No hay raíces con role='target' configuradas para write_files");

        for (const auto& wf : args["write_files"]) {
            std::string raw_path = wf.at("path").get<std::string>();
            std::string content = wf.at("content").get<std::string>();
            std::string pref_root = wf.value("root", "");
            fs::path cand(wide(raw_path));
            fs::path resolved;
            bool inside_target = false;
            for (const auto& [rid, tbase] : target_roots) {
                if (!pref_root.empty() && rid != pref_root) continue;
                fs::path test_p = cand.is_absolute() ? cand : (tbase / cand);
                std::error_code ec;
                fs::path norm_p = test_p.lexically_normal();
                if (within(norm_p, tbase)) {
                    resolved = norm_p;
                    inside_target = true;
                    break;
                }
            }
            if (!inside_target) {
                throw std::runtime_error("WRITE_OUTSIDE_TARGET_ROOT: write_files solo permite escribir dentro de una raíz target: " + raw_path);
            }
            std::string new_sha = sha256(content);
            if (!dry_run) {
                std::error_code ec;
                if (fs::is_regular_file(resolved, ec)) {
                    auto bak_dir = p.state_dir() / L"backups" / L"complete_task";
                    fs::create_directories(bak_dir, ec);
                    auto bak_p = bak_dir / wide(unique_id() + "_" + path_text(resolved.filename()));
                    fs::copy_file(resolved, bak_p, fs::copy_options::overwrite_existing, ec);
                } else {
                    fs::create_directories(resolved.parent_path(), ec);
                }
                write_text(resolved, content);
                Database sync_db(p);
                sync_db.begin();
                try {
                    auto f_ent = entity_or_sync(sync_db, p, path_text(resolved), true);
                    sync_db.commit();
                    written_files.push_back({{"file_id", f_ent["entity_id"]}, {"path", path_text(resolved)}, {"bytes", content.size()}, {"sha256", new_sha}});
                } catch (...) {
                    sync_db.rollback();
                    throw;
                }
            } else {
                written_files.push_back({{"path", path_text(resolved)}, {"bytes", content.size()}, {"sha256", new_sha}, {"dry_run", true}});
            }
        }
    }

    Json ops = Json::array();
    if (args.contains("edits") && args["edits"].is_array()) {
        for (const auto& ed : args["edits"]) ops.push_back(ed);
    }
    if (args.contains("operations") && args["operations"].is_array()) {
        for (const auto& op : args["operations"]) ops.push_back(op);
    }

    std::set<std::string> mapped_sources;
    auto append_mapping_op = [&](const Json& mitem) {
        Json mop = mitem;
        mop["op"] = mitem.value("op", "map");
        if (!mop.contains("targets") && mop.contains("target")) {
            mop["targets"] = mop["target"].is_array() ? mop["target"] : Json::array({mop["target"]});
        }
        if (!mop.contains("complete")) mop["complete"] = true;
        mop["auto_sync_touched"] = auto_sync;
        ops.push_back(mop);
        if (mop.contains("source") && mop["source"].is_string()) {
            mapped_sources.insert(mop["source"].get<std::string>());
        }
        if (mitem.value("auto_map_symbols", args.value("auto_map_symbols", false)) &&
            mop.contains("targets") && mop["targets"].is_array() && !mop["targets"].empty()) {
            ops.push_back({
                {"op", "auto_map_symbols"},
                {"source_file", mop["source"]},
                {"target_file", mop["targets"][0]},
                {"complete", mop["complete"]},
                {"cases", mop.value("cases", Json::array())},
                {"note", mop.value("note", "Auto-mapeo desde complete_task")},
                {"also_map_file", false},
                {"auto_sync_touched", auto_sync}
            });
        }
    };

    if (args.contains("mappings") && args["mappings"].is_array()) {
        for (const auto& m : args["mappings"]) append_mapping_op(m);
    }
    if (args.contains("source") && (args.contains("targets") || args.contains("target") || args.contains("disposition"))) {
        append_mapping_op(args);
    }

    {
        Database check_db(p);
        if (args.value("release_claims", true) && has_claims_table(check_db) && !mapped_sources.empty()) {
            for (const auto& src_ref : mapped_sources) {
                try {
                    auto ent = entity_or_sync(check_db, p, src_ref, false);
                    std::string eid = ent["entity_id"].get<std::string>();
                    auto cl = get_active_claim(check_db, eid);
                    if (!cl.is_null() && cl.value("actor", "") == actor) {
                        ops.push_back({{"op", "release_task"}, {"entity", eid}});
                    }
                } catch (...) {}
            }
        }
    }

    if (args.contains("task_id") && args.contains("task_state")) {
        ops.push_back({
            {"op", "transition_task"},
            {"id", args["task_id"]},
            {"state", args["task_state"]},
            {"note", args.value("note", "")}
        });
    }

    Json batch_res = Json::object();
    if (!ops.empty()) {
        int64_t expected_rev = args.contains("expected_revision") ? args["expected_revision"].get<int64_t>() : Database(p).revision();
        Json batch_args = {
            {"actor", actor},
            {"expected_revision", expected_rev},
            {"auto_sync_touched", auto_sync},
            {"dry_run", dry_run},
            {"operations", ops}
        };
        batch_res = agent_batch(p, batch_args);
    } else {
        Database cur_db(p);
        batch_res = {{"ok", true}, {"revision", cur_db.revision()}, {"applied", 0}, {"operations", Json::array()}};
    }

    batch_res["written_files"] = written_files;
    if (!dry_run && args.contains("verify_cases") && args["verify_cases"].is_array() && !args["verify_cases"].empty()) {
        std::vector<std::string> cids = args["verify_cases"].get<std::vector<std::string>>();
        batch_res["verify_result"] = run_cases(p, log, cancel, cids);
    }
    return batch_res;
}
}
Json agent_request(const Project& p,const std::string& tool,const Json& raw_args,Log log,Cancel* cancel){
    const Json args = raw_args.is_null() ? Json::object() : raw_args;
    auto begin=std::chrono::steady_clock::now();
    recover_pending_journals(p);
    Database db(p);Json result;
    if(tool=="inventory_sync"){
        const std::string mode=project_mode(p);
        const std::string tier=args.value("tier",p.config.value("index_tier",""));
        if(mode=="archive"||tier=="metadata"){
            result=fs_sync(p,db,args,log,cancel);
        }else if(mode=="workspace"){
            result=fs_sync(p,db,args,log,cancel);
            if(tier!="metadata"&&args.value("sync_code",true)){
                try{result["code_sync"]=index_sync(p,args.value("full",false),log,cancel,args.value("scope",Json::object()));}catch(const std::exception& e){result["code_sync_error"]=e.what();}
            }
        }else if(!args.value("wait",true))result=scale_start(p,args);
        else if(scale_enabled(p,db)||args.value("defer_analysis",false))result=scale_run(p,args,log,cancel);
        else result=index_sync(p,args.value("full",false),log,cancel,args.value("scope",Json::object()));
    }
    else if(tool=="explore_tree")result=fs_explore_tree(p,db,args);
    else if(tool=="find_files")result=fs_find_files(p,db,args);
    else if(tool=="changes_feed")result=fs_changes_feed(p,db,args);
    else if(tool=="activate_workspace")result=tool_activate_workspace(args,nullptr,log,cancel);
    else if(tool=="list_workspaces")result=tool_list_workspaces(args);
    else if(tool=="query_workspace")result=tool_query_workspace(args,log,cancel);
    else if(tool=="onboard_workspace")result=tool_onboard_workspace(p,db,args);
    else if(tool=="memory")result=memory_request(p,db,args);
    else if(tool=="read_file")result=tool_read_file(p,db,args);
    else if(tool=="create_file")result=tool_create_file(p,db,args);
    else if(tool=="edit_file")result=tool_edit_file(p,db,args);
    else if(tool=="search_pattern")result=tool_search_pattern(p,db,args,cancel);
    else if(tool=="get_diagnostics")result=tool_get_diagnostics(p,db,args);
    else if(tool=="find_implementations")result=tool_find_implementations(p,db,args);
    else if(tool=="find_declaration")result=tool_find_declaration(p,db,args);
    else if(tool=="lsp_rename_symbol")result=tool_lsp_rename_symbol(p,db,args);
    else if(tool=="restart_language_server")result=tool_restart_language_server(p,args);
    else if(tool=="housekeep")result=tool_housekeep(p,db,args,log);
    else if(tool=="session_bootstrap")result=build_session_bootstrap(p,db,args);
    else if(tool=="complete_task")result=run_complete_task(p,args,log,cancel);
    else if(tool=="job_status")result=scale_jobs(p,args);
    else if(tool=="job_control")result=scale_control(p,args);
    else if(tool=="project_settings")result=scale_settings(p,args);
    else if(tool=="acquire_task")result=scale_acquire(p,args);
    else if(tool=="import_existing_work")result=scale_import(p,args,cancel);
    else if(tool=="project_diagnostics"){result=scale_doctor(p);result["mode"]=project_mode(p);result["fs_index"]=fs_stats_summary(p,db);result["lsp_servers"]=lsp_discover_servers(p)["servers"];result["housekeep"]=housekeep_status(p,db);}
    else if(tool=="export_inventory")result=scale_export(p,args,cancel);
    else if(tool=="backup_catalog"){
        auto out_arg=args.value("out",args.value("destination",""));
        fs::path dest=out_arg.empty()?(p.state_dir()/L"backups"/wide("catalog-"+std::to_string(db.revision())+".sqlite")):fs::path(wide(out_arg));
        result=backup_catalog(p,dest);
    }
    else if(tool=="batch_update")result=agent_batch(p,args);
    else if(tool=="edit_symbol"){
        Json op=args;
        if(!op.contains("op"))op["op"]=args.value("action","replace_symbol_body");
        int64_t expected_rev=args.value("expected_revision",db.revision());
        Json batch_args={
            {"actor",args.value("actor","agent")},
            {"expected_revision",expected_rev},
            {"dry_run",args.value("dry_run",false)},
            {"inject_failure_after_disk",args.value("inject_failure_after_disk",false)},
            {"inject_failure_stage",args.value("inject_failure_stage","")},
            {"detail","full"},
            {"operations",Json::array({op})}
        };
        result=agent_batch(p,batch_args);
    }else if(tool=="get_task_context"){
        Json ctx_args=args;
        if(!ctx_args.contains("token_budget")&&!ctx_args.contains("budget_tokens"))ctx_args["token_budget"]=4000;
        result=build_task_context(p,db,ctx_args);
        result=bound(result,ctx_args);
        auto micros=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-begin).count();
        if(!db.try_aux_write("INSERT INTO requests(at,tool,request_bytes,response_bytes,duration_us,cache_revision)VALUES(?,?,?,?,?,?)",{now(),tool,args.dump().size(),result.dump().size(),micros,db.revision()})){
            result["statistics_recorded"]=false;result=bound(result,ctx_args);
        }
        return result;
    }else if(tool=="verify"){
        if(!args.value("execute",false))throw std::runtime_error("verify requiere execute:true para ejecutar los comandos configurados");
        const auto action=args.value("action","test");
        const auto case_ids=args.value("case_ids",std::vector<std::string>{});
        if(action!="test"&&!case_ids.empty())throw std::runtime_error("case_ids requiere verify.action=test");
        if(action=="test")result=run_cases(p,log,cancel,case_ids);
        else if(action=="build")result=run_builds(p,log,cancel);
        else if(action=="scan")result=(scale_enabled(p,db)?scale_run(p,args,log,cancel):index_sync(p,true,log,cancel));
        else if(action=="coverage")result=import_coverage(p,p.resolve(args.at("input")),args.at("root"),args.at("format"),log);
        else throw std::runtime_error("verify.action inválida");
        result={{"revision",db.revision()},{"action",action},{"result",result}};
    }else if(tool=="query"){
        const auto query_page=[&](const Json& raw_query){
            Json query=raw_query;
            apply_field_preset(query,"files");
            const auto kind=query.value("kind","files");
            if(auto page=scale_query(p,db,query))return *page;
            if(kind=="files"||kind=="documentation"||kind=="new"||kind=="ignored"||kind=="orphans"||kind=="excluded"||kind=="symbols"||kind=="find_symbol"||kind=="symbols_overview"||kind=="get_symbols_overview")return mt::query_page(p,db,query);
            if(kind!="code_duplicates"&&kind!="code_duplicate_errors")
                return paginate(query_items(p,db,query),query,db.revision());
            const auto report=duplicate_code_report(p,db);
            Json rows=Json::array();
            if(kind=="code_duplicate_errors")rows=report.at("errors");
            else for(const auto& group:report.at("groups"))for(auto copy:group.at("copies")){
                if(query.contains("role")&&query["role"]!=copy["role"])continue;
                if(query.contains("root")&&query["root"]!=copy["root"])continue;
                copy["sha256"]=group.at("sha256");
                copy["bytes"]=group.at("bytes");
                copy["copy_count"]=group.at("copy_count");
                rows.push_back(std::move(copy));
            }
            auto page=paginate(rows,query,db.revision());
            page["report"]={{"mode",report.at("mode")},{"available",report.at("available")},
                {"fresh",report.at("fresh")},{"complete",report.at("complete")},
                {"group_count",report.value("group_count",size_t(0))},{"archive_count",report.value("archive_count",size_t(0))},
                {"error_count",report.at("errors").size()}};
            return page;
        };
        if(args.contains("queries")){if(args["queries"].size()>100)throw std::runtime_error("Máximo 100 consultas por lote");Json items=Json::array();for(const auto& q:args["queries"])items.push_back(query_page(q));result={{"revision",db.revision()},{"results",items}};}
        else result=query_page(args);
    }else if(tool=="changes_since"||tool=="history"){
        std::string where=" WHERE revision>?";Json params={args.value("since_revision",int64_t(0))};
        if(args.contains("entity")){where+=" AND entity=?";params.push_back(args["entity"]);}if(args.contains("actor")){where+=" AND actor=?";params.push_back(args["actor"]);}if(args.contains("action")){where+=" AND action=?";params.push_back(args["action"]);}
        auto rows=db.query("SELECT id,revision,at,actor,action,entity,detail FROM history"+where+" ORDER BY id",params);
        for(auto& row:rows){if(args.value("detail","compact")=="full")row["detail"]=parse(row["detail"]);else row.erase("detail");}
        result=paginate(rows,args,db.revision());result["through_revision"]=db.revision();
    }else if(tool=="connections"){
        auto rows=db.query("SELECT * FROM connections ORDER BY opened DESC");for(auto& row:rows){auto pid=row["pid"].get<DWORD>();auto h=OpenProcess(SYNCHRONIZE,FALSE,pid);bool live=h&&WaitForSingleObject(h,0)==WAIT_TIMEOUT;if(h)CloseHandle(h);row["live"]=live&&row["closed"].is_null();}result=paginate(rows,args,db.revision());
    }else if(tool=="statistics"){
        result={{"revision",db.revision()},{"queries",db.query("SELECT tool,COUNT(*)AS invocations,SUM(request_bytes)AS request_bytes,SUM(response_bytes)AS response_bytes,AVG(duration_us)AS mean_us FROM requests GROUP BY tool ORDER BY tool")},{"index_path",path_text(p.state_dir()/L"catalog.sqlite")}};
    }else if(tool=="project_context"||tool=="migration_status"||tool=="next_tasks"||tool=="evidence_report"){
        Json eff_args=args;
        if(tool=="migration_status"&&eff_args.value("level","")=="files")apply_field_preset(eff_args,"status_files");
        if(scale_enabled(p,db)){
            const bool fresh=db.integer("SELECT COALESCE(MAX(CAST(value AS INTEGER)),0) FROM meta WHERE key='dirty'")==0&&db.integer("SELECT EXISTS(SELECT 1 FROM inventory_jobs WHERE generation>0 AND state IN('queued','running','paused'))")==0;
            if(tool=="next_tasks"||(tool=="migration_status"&&eff_args.value("level","")=="files")){
                result=scale_file_status(p,db,eff_args);
            }else if(tool=="migration_status"&&eff_args.value("level","")=="modules"){
                result=scale_module_matrix(p,db,eff_args);
            }else if(tool=="migration_status"&&eff_args.value("level","")=="phases"){
                auto q=eff_args;q["kind"]="phases";result=*scale_query(p,db,q);
            }else if(tool=="migration_status"&&eff_args.value("level","")=="tasks"){
                auto q=eff_args;q["kind"]="project_tasks";result=*scale_query(p,db,q);
            }else if(tool=="migration_status"&&eff_args.value("level","")=="docs"){
                auto q=eff_args;q["kind"]="doc_audits";result=*scale_query(p,db,q);
            }else if(tool=="migration_status"&&eff_args.value("level","")=="symbols"){
                auto q=eff_args;q["kind"]="symbols";q.erase("fields");
                auto page=mt::query_page(p,db,q);
                if(!page["items"].empty()){
                    auto evidence=scale_evidence(p,db,fresh);
                    std::set<std::string> passed_set,built_set,cfg_build_roots;
                    for(const auto& c:evidence["passed"])passed_set.insert(c.get<std::string>());
                    for(const auto& b:evidence["built"])built_set.insert(b.get<std::string>());
                    for(const auto& b:evidence.value("builds",Json::array()))if(b.contains("root")&&b["root"].is_string())cfg_build_roots.insert(b["root"].get<std::string>());
                    std::map<std::string,std::string> roles_by_root;
                    for(const auto& rr:db.query("SELECT id,role FROM roots"))if(rr["id"].is_string()&&rr["role"].is_string())roles_by_root[rr["id"].get<std::string>()]=rr["role"].get<std::string>();
                    std::string sids,fids;
                    for(const auto& row:page["items"]){
                        if(!sids.empty())sids+=',';
                        sids+="'"+row["id"].get<std::string>()+"'";
                        if(!row["file_id"].is_null()){if(!fids.empty())fids+=',';fids+="'F"+std::to_string(row["file_id"].get<int64_t>())+"'";}
                    }
                    std::map<std::string,Json> maps;
                    db.each("SELECT source,targets,cases,complete,hashes FROM mappings WHERE source IN("+(sids.empty()?"''":sids)+")",Json::array(),[&](const Json& m){maps[m["source"].get<std::string>()]=m;return true;});
                    for(auto& row:page["items"]){
                        bool declared=false,verified=false;
                        auto it=maps.find(row["id"].get<std::string>());
                        if(it!=maps.end()){
                            const auto& m=it->second;
                            bool current=true;
                            auto hashes=safe_parse(m["hashes"]);
                            if(hashes.empty())current=false;
                            for(auto hit=hashes.begin();hit!=hashes.end()&&current;++hit){
                                const auto k=hit.key();
                                if(k.starts_with("F")){
                                    auto fr=db.query("SELECT deleted,sha256,root FROM files WHERE id=?",{std::stoll(k.substr(1))});
                                    if(fr.empty()||fr[0]["deleted"]!=0||fr[0]["sha256"]!=hit.value())current=false;
                                }else if(k.starts_with("S")){
                                    auto sr=db.query("SELECT active,data FROM symbols WHERE id=?",{std::stoll(k.substr(1))});
                                    if(sr.empty()||sr[0]["active"]!=1||!hit.value().is_string()||safe_parse(sr[0]["data"]).value("symbol_sha256",std::string())!=hit.value().get<std::string>())current=false;
                                }
                            }
                            bool cases_ok=false;
                            auto cases=safe_parse(m["cases"]);
                            if(cases.is_array()&&!cases.empty()){
                                cases_ok=true;
                                for(const auto& c:cases)if(!passed_set.contains(c.get<std::string>())){cases_ok=false;break;}
                            }
                            bool builds_ok=current&&!built_set.empty();
                            for(auto hit=hashes.begin();hit!=hashes.end()&&builds_ok;++hit){
                                if(hit.key().starts_with("F")){
                                    auto fr=db.query("SELECT root FROM files WHERE id=?",{std::stoll(hit.key().substr(1))});
                                    if(fr.empty()){builds_ok=false;break;}
                                    const auto rname=fr[0]["root"].get<std::string>();
                                    const bool is_tgt=roles_by_root[rname]!="source";
                                    if((is_tgt||cfg_build_roots.contains(rname))&&!built_set.contains(rname))builds_ok=false;
                                }
                            }
                            declared=current&&m["complete"]==1;
                            verified=declared&&cases_ok&&builds_ok;
                        }
                        row["declared_complete"]=declared;
                        row["verified_in_cases"]=verified;
                        auto fields=eff_args.value("fields",Json::array());
                        if(!fields.empty())row=field_selection(row,fields);
                    }
                }
                result=page;
            }else if(tool=="evidence_report"){
                auto evidence=scale_evidence(p,db,fresh);
                const auto kind_filter=eff_args.value("kind","");
                Json fixed_rows=Json::array();
                if(kind_filter.empty()||kind_filter=="cases")for(const auto& r:evidence["cases"]){auto item=r;item["kind"]="cases";fixed_rows.push_back(std::move(item));}
                if(kind_filter.empty()||kind_filter=="builds")for(const auto& r:evidence["builds"]){auto item=r;item["kind"]="builds";fixed_rows.push_back(std::move(item));}
                if(kind_filter.empty()||kind_filter=="coverage"){
                    for(const auto& r:db.query("SELECT data FROM evidence WHERE kind='coverage' ORDER BY revision DESC LIMIT 1")){
                        auto item=safe_parse(r["data"]);item["kind"]="coverage";fixed_rows.push_back(std::move(item));
                    }
                }
                const bool want_maps=(kind_filter.empty()||kind_filter=="mappings");
                const int64_t map_total=want_maps?db.integer("SELECT COUNT(*) FROM mappings"):0;
                const int64_t total=static_cast<int64_t>(fixed_rows.size())+map_total;
                int64_t offset=eff_args.value("offset",int64_t(0));
                if(eff_args.contains("cursor")&&eff_args["cursor"].is_string()&&!eff_args["cursor"].get<std::string>().empty()){
                    try{offset=std::max<int64_t>(0,std::stoll(eff_args["cursor"].get<std::string>()));}catch(...){}
                }
                const int64_t limit=std::clamp<int64_t>(eff_args.value("limit",int64_t(50)),1,1000);
                Json items=Json::array();
                for(int64_t i=offset;i<static_cast<int64_t>(fixed_rows.size())&&static_cast<int64_t>(items.size())<limit;++i)items.push_back(fixed_rows[static_cast<size_t>(i)]);
                if(want_maps&&static_cast<int64_t>(items.size())<limit){
                    const int64_t map_off=std::max<int64_t>(0,offset-static_cast<int64_t>(fixed_rows.size()));
                    const int64_t map_lim=limit-static_cast<int64_t>(items.size());
                    for(const auto& m:db.query("SELECT source,targets,cases,complete,note,actor,at,revision FROM mappings ORDER BY rowid LIMIT ? OFFSET ?",{map_lim,map_off})){
                        items.push_back({{"kind","mappings"},{"source",m["source"]},{"targets",safe_parse(m["targets"])},{"cases",safe_parse(m["cases"])},{"complete",m["complete"]==1},{"note",m["note"]},{"actor",m["actor"]},{"at",m["at"]},{"revision",m["revision"]}});
                    }
                }
                const auto fields=eff_args.value("fields",Json::array());
                if(!fields.empty())for(auto& item:items)item=field_selection(item,fields);
                const int64_t next_off=offset+static_cast<int64_t>(items.size());
                const bool has_more=next_off<total;
                result={{"items",items},{"total",eff_args.value("include_total",true)?Json(total):Json(nullptr)},{"next_cursor",has_more?Json(std::to_string(next_off)):Json(nullptr)},{"truncated",has_more},{"revision",db.revision()},
                        {"fresh",fresh},{"cases",evidence["cases"]},{"builds",evidence["builds"]},{"passed",evidence["passed"]},{"built",evidence["built"]},{"mappings_total",map_total}};
            }else if(tool=="project_context"){
                result=scale_summary(p,db);
                auto nxt=scale_file_status(p,db,{{"limit",10},{"include_total",false},{"fields",Json::array({"id"})}});
                Json ids=Json::array();for(const auto& t:nxt.value("items",Json::array()))if(t.contains("id"))ids.push_back(t["id"]);
                result["next_tasks"]=ids;
                auto recent=db.query("SELECT revision,actor,action,at FROM history ORDER BY id DESC LIMIT 1");
                result["last_activity"]=recent.empty()?Json():recent[0];
                result["freshness_basis"]="published_census_with_recorded_changes; active watchers invalidate dirty state";
                if(eff_args.contains("fields")&&!eff_args["fields"].empty())result=field_selection(result,eff_args["fields"]);
            }else{
                result=scale_summary(p,db);
                if(eff_args.contains("fields")&&!eff_args["fields"].empty())result=field_selection(result,eff_args["fields"]);
            }
        }else{
        auto st=cached_status(p,db);
        if(tool=="project_context"){
            result=compact_status(st);auto tasks=next_items(db,st,eff_args);Json ids=Json::array();for(size_t i=0;i<std::min<size_t>(10,tasks.size());++i)ids.push_back(tasks[i]["id"]);result["next_tasks"]=ids;
            auto recent=db.query("SELECT revision,actor,action,at FROM history ORDER BY id DESC LIMIT 1");result["last_activity"]=recent.empty()?Json():recent[0];result["freshness_basis"]="last_sync; active watchers invalidate dirty state";
        }else if(tool=="next_tasks"){
            auto page_args=eff_args;page_args.erase("fields");result=paginate(next_items(db,st,eff_args),page_args,db.revision());
            for(auto& task:result["items"]){auto numeric=entity_number(task["id"],'F');auto docs=db.query("SELECT 'F'||doc_id AS id FROM doc_links WHERE code_id=? ORDER BY doc_id",{numeric});Json ids=Json::array();for(const auto& d:docs)ids.push_back(d["id"]);task["documentation"]=ids;
                task["candidate_targets"]=db.query("SELECT DISTINCT 'F'||t.file_id AS file_id,f.absolute_path FROM symbols s JOIN symbols t ON s.name=t.name JOIN files f ON t.file_id=f.id JOIN roots r ON f.root=r.id WHERE s.file_id=? AND s.active=1 AND t.active=1 AND f.deleted=0 AND r.role='target' LIMIT 5",{numeric});
                task["notes"]=db.query("SELECT text,actor,at FROM notes WHERE entity=? ORDER BY id DESC LIMIT 2",{task["id"]});task=field_selection(task,eff_args.value("fields",Json::array()));}
        }
        else if(tool=="evidence_report"){
            Json rows=Json::array();for(const auto* kind:{"cases","builds","coverage","mappings"})for(const auto& row:st[kind]){auto item=row;item["kind"]=kind;rows.push_back(item);}result=paginate(rows,eff_args,db.revision());
        }else if(eff_args.value("level","project")=="project")result=field_selection(compact_status(st),eff_args.value("fields",Json::array()));
        else if(eff_args.value("level","")=="files"){
            Json rows=Json::array();for(const auto& f:st["files"])if(!eff_args.contains("id")||eff_args["id"]==f["id"])rows.push_back(f);result=paginate(rows,eff_args,db.revision());
        }else if(eff_args.value("level","")=="modules"){
            result=scale_module_matrix(p,db,eff_args);
        }else if(eff_args.value("level","")=="phases"){
            result=paginate(query_items(p,db,{{"kind","phases"}}),eff_args,db.revision());
        }else if(eff_args.value("level","")=="tasks"){
            result=paginate(query_items(p,db,{{"kind","project_tasks"}}),eff_args,db.revision());
        }else if(eff_args.value("level","")=="docs"){
            result=paginate(query_items(p,db,{{"kind","doc_audits"}}),eff_args,db.revision());
        }else if(eff_args.value("level","")=="symbols"){
            auto rows=query_items(p,db,{{"kind","symbols"},{"root",eff_args.value("root","")}});if(!eff_args.contains("root"))rows=query_items(p,db,{{"kind","symbols"}});
            for(auto& row:rows){row["declared_complete"]=false;row["verified_in_cases"]=false;for(const auto& m:st["mappings"])if(m["source"]==row["id"]){row["declared_complete"]=m["fresh"]==true&&m["declared_complete"]==true;row["verified_in_cases"]=m["verified_in_configured_cases"];}}
            result=paginate(rows,eff_args,db.revision());
        }else throw std::runtime_error("level debe ser project, files, symbols, modules, phases, tasks o docs");
        }
    }else throw std::runtime_error("Herramienta desconocida: "+tool);
    if(args.value("format","")=="columnar"||args.value("format","")=="compact_table"){
        result=to_columnar_format(std::move(result));
    }
    // Las operaciones ya comprometidas devuelven acuse resumido sin repetir cientos de rutas.
    if(tool=="batch_update"&&!args.value("dry_run",false)&&!args.value("detail",std::string("compact")).compare("compact"))
        result={{"ok",true},{"revision",result["revision"]},{"applied",result["applied"]}};
    auto response_options=args;
    const bool paginated_status=(tool=="migration_status"&&args.value("level","project")!="project");
    if(!response_options.contains("token_budget")&&!response_options.contains("budget_tokens")&&
       (tool=="inventory_sync"||tool=="session_bootstrap"||tool=="complete_task"||tool=="job_status"||tool=="job_control"||tool=="project_settings"||tool=="acquire_task"||tool=="import_existing_work"||tool=="project_diagnostics"||
        tool=="query"||tool=="explore_tree"||tool=="find_files"||tool=="changes_feed"||tool=="search_pattern"||tool=="read_file"||tool=="list_workspaces"||tool=="query_workspace"||tool=="onboard_workspace"||tool=="memory"||tool=="get_diagnostics"||tool=="find_implementations"||tool=="find_declaration"||tool=="housekeep"||
        tool=="next_tasks"||tool=="evidence_report"||tool=="changes_since"||tool=="history"||paginated_status))response_options["token_budget"]=(tool=="read_file"||tool=="memory")?16000:4000;
    result=bound(result,response_options);
    auto micros=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-begin).count();
    if(!db.try_aux_write("INSERT INTO requests(at,tool,request_bytes,response_bytes,duration_us,cache_revision)VALUES(?,?,?,?,?,?)",{now(),tool,args.dump().size(),result.dump().size(),micros,db.revision()})){
        result["statistics_recorded"]=false;result=bound(result,response_options);
    }
    return result;
}
Json agent_tools(){
    const std::vector<std::pair<std::string,std::string>> definitions={
        {"session_bootstrap","Arranque de sesión en 1 llamada: devuelve revisión actual, estado global, diagnóstico de bloqueos del denominador (denominator_blockers), raíces, módulos pendientes prioritarios, reclamos activos y siguientes tareas."},
        {"activate_workspace","Activa o inicializa en 1 llamada cualquier directorio (modo workspace, archive para 6M+ archivos o migration) guardando el catálogo fuera de raíces protegidas (ej. %LOCALAPPDATA%\\Migratool\\workspaces\\)."},
        {"list_workspaces","Lista todos los workspaces y proyectos registrados con su modo, tamaño de catálogo, número de archivos/directorios indexados y sesiones activas."},
        {"query_workspace","Consulta federada de solo lectura contra otro workspace registrado (por nombre, JSON o carpeta) sin cambiar el workspace activo."},
        {"onboard_workspace","Orientación automática del workspace: detecta estructura, lenguajes, marcadores de compilación y crea la memoria persistente _onboarding."},
        {"explore_tree","Exploración jerárquica ultra-escala (<10ms sobre 6M+ archivos): devuelve conteos recursivos de archivos/carpetas/bytes, top extensiones y muestras por carpeta usando rollups precalculados fs_dirs."},
        {"find_files","Búsqueda instantánea (<25ms sobre 6M+ archivos) por subcadena trigrama FTS5, glob, prefijo de ruta, extensión, tamaño y mtime sin escaneos completos."},
        {"changes_feed","Flujo auditable acotado de cambios detectados automáticamente por identidad NTFS FRN (created, modified, deleted, renamed, moved)."},
        {"memory","Memorias persistentes de proyecto con SQLite FTS5, espejo Markdown y rastreo FRN de archivos enlazados: action = write | read | edit | list | search | rename | delete."},
        {"read_file","Lectura quirúrgica por rango de líneas o ventana de símbolo con sha256, detección de binarios y presupuesto de tokens."},
        {"create_file","Crea un archivo dentro de raíces autorizadas y actualiza fs_nodes inmediatamente."},
        {"edit_file","Edición transaccional con rollback atómico: mode = replace_content | replace_lines | insert_at_line | delete_lines | replace_in_files (literal o regex, dry_run, expected_sha256)."},
        {"search_pattern","Buscador nativo C++23 multi-hilo (literal o regex) sobre archivos indexados con líneas de contexto, globs, descarte automático de binarios y format:'columnar'."},
        {"get_diagnostics","Diagnósticos semánticos en tiempo real vía puente LSP híbrido (30+ lenguajes) con fallback transparente al índice nativo Roslyn/libclang."},
        {"find_implementations","Localiza implementaciones de una interfaz/método/símbolo vía puente LSP o índice nativo."},
        {"find_declaration","Localiza la declaración/definición exacta de un símbolo o posición (path, line, column) vía puente LSP o índice nativo."},
        {"lsp_rename_symbol","Renombrado semántico tipado vía LSP WorkspaceEdit ejecutado a través del motor transaccional con rollback atómico de Migratool."},
        {"restart_language_server","Reinicia limpiamente el servidor LSP de un lenguaje (o todos los del workspace) sin dejar procesos huérfanos."},
        {"housekeep","Autoaseo y salud del catálogo: action = status (solo lectura) | run (poda telemetría/logs/claims expirados/sesiones muertas, fusiona FTS5, recicla temporales obsoletos, incremental_vacuum y wal_checkpoint) | vacuum (compactación completa y migración a auto_vacuum=INCREMENTAL). Soporta dry_run y deep."},
        {"complete_task","Flujo completo One-Shot en 1 llamada atómica: escribe archivos en raíces target (write_files) o aplica ediciones simbólicas/AST (edits), auto-sincroniza hashes/símbolos en SQLite sin INDEX_STALE, registra mapeos (mappings o source+targets con auto_map_symbols opcional), libera claims del actor, transiciona project_tasks y opcionalmente ejecuta verify_cases."},
        {"job_status","Estado y contadores de trabajos persistentes; id o job_id opcional."},
        {"job_control","Pausa, resume, cancela o reintenta un trabajo propio mediante id, action y actor."},
        {"project_settings","Consulta ajustes o modifica settings con action:set, actor y expected_config_revision."},
        {"acquire_task","Selecciona y reclama atómicamente un pendiente; devuelve contexto y lease."},
        {"import_existing_work","Importa por páginas evidencia externa abierta en solo lectura. external_path detecta el formato, registry_path y catalog_path lo fijan explícitamente. action:revalidate comprueba de nuevo los hashes registrados para ids de candidatos, sin importar otras filas."},
        {"project_diagnostics","Identidad exacta de compilación, catálogo, coordinador, salud del denominador, raíces y capacidades locales."},
        {"export_inventory","Escribe el catálogo de archivos en out con formato jsonl o json. wait:false devuelve job_id para consultar, pausar, reanudar o cancelar."},
        {"backup_catalog","Crea una copia de seguridad consistente del catálogo SQLite en out (opcional)."},
        {"project_context","Contexto compacto, revisión, porcentajes, development_control y próximos identificadores; no lee fuentes."},
        {"get_task_context","Contexto quirúrgico en una llamada (admite task_id único o lote task_ids de hasta 50 unidades y outline_only:true): metadatos, esqueleto de símbolos, cuerpo exacto [offset..end_offset], callees/callers semánticos, candidatos destino y evidencia."},
        {"inventory_sync","Sincroniza raíces autorizadas. scope.root y scope.subdirectory acotan una raíz, conservando los registros exteriores."},
        {"query","Consulta files, symbols, symbol_body, symbols_overview, definition, diagnostics, range, references, callers, callees, implementations, type_hierarchy, impact_analysis, hybrid_search, census_directories, census_entries, directories, folder_coverage, reuse_candidates, mappings, relations, dependencies, notes, history, search, archive_members, scan_errors, code_duplicates, code_duplicate_errors, language_capabilities, claims, export_bundle, phases, project_tasks, doc_audits y module_matrix. Soporta format:'columnar' y preset:'minimal'|'agent' para ahorrar hasta 75% de tokens."},
        {"edit_symbol","Edición simbólica transaccional con dry_run, diff, backup y rollback: replace_symbol_body, insert_before_symbol, insert_after_symbol, rename_symbol, safe_delete_symbol, ast_rewrite y apply_structural_patch."},
        {"migration_status","Progreso declarado, verificación en casos y control de desarrollo por level: project, files, symbols, modules, phases, tasks o docs; soporta format:'columnar' y preset:'minimal'|'agent'."},
        {"next_tasks","Lote de pendientes con rutas, dependencias, candidatos, notas y evidencia faltante."},
        {"changes_since","Deltas desde since_revision; cursor de página ligado a la revisión actual."},
        {"batch_update","Transacción atómica con expected_revision, reconciliación por entidad/dominio y auto_sync_touched opcional: map/mark, classify, classify_pattern, map_pattern, auto_map_symbols, note, claim_task, renew_claim, release_task, reconcile_bundle, upsert_phase, upsert_task, transition_task, delete_task, audit_doc, ast_rewrite o edición simbólica."},
        {"verify","Ejecuta build, test, scan o coverage configurados con execute:true. En test admite case_ids."},
        {"evidence_report","Resultados de pruebas, compilaciones, cobertura y correspondencias con estados."},
        {"history","Historial inmutable filtrable por entity, actor, action y since_revision."},
        {"connections","Conexiones MCP de este proyecto y estado vivo del proceso."},
        {"statistics","Invocaciones, bytes de entrada/salida y duración medida por herramienta."}};
    Json tools=Json::array();
    for(const auto& [name,description]:definitions){
        Json properties={{"project",{{"type","string"},{"description","Ruta absoluta opcional al JSON del proyecto; por defecto, el seleccionado al iniciar MCP."}}},{"token_budget",{{"type","integer"},{"minimum",256},{"maximum",100000}}},{"limit",{{"type","integer"},{"minimum",1},{"maximum",1000}}},
            {"cursor",{{"type","string"}}},{"fields",{{"type","array"},{"items",{{"type","string"}}}}},{"preset",{{"type","string"},{"enum",{"minimal","agent","full"}}}},
            {"kind",{{"type","string"}}},{"text",{{"type","string"}}},{"id",{{"type","string"}}},{"task_id",{{"type","string"}}},{"task_ids",{{"type","array"},{"items",{{"type","string"}}}}},{"symbol",{{"type","string"}}},{"file",{{"type","string"}}},
            {"root",{{"type","string"}}},{"role",{{"type","string"}}},{"language",{{"type","string"}}},{"category",{{"type","string"}}},{"detail",{{"type","string"}}},
            {"actor",{{"type","string"}}},{"action",{{"type","string"}}},{"entity",{{"type","string"}}},{"since_revision",{{"type","integer"}}},
            {"expected_revision",{{"type","integer"}}},{"full",{{"type","boolean"}}},{"execute",{{"type","boolean"}}},{"dry_run",{{"type","boolean"}}},{"include_body",{{"type","boolean"}}},{"outline_only",{{"type","boolean"}}},{"level",{{"type","string"}}},
            {"operations",{{"type","array"},{"items",{{"type","object"}}}}},{"queries",{{"type","array"},{"items",{{"type","object"}}}}},
            {"ids",{{"type","array"},{"items",{{"type","string"}}}}},{"input",{{"type","string"}}},{"format",{{"type","string"}}},
            {"ast_pattern",{{"type","string"}}},{"regex_pattern",{{"type","string"}}},{"signature_contains",{{"type","string"}}},{"symbol_kind",{{"type","string"}}},
            {"migration_state",{{"type","string"}}},{"missing_target_language",{{"type","string"}}},{"calls",{{"type","string"}}},
            {"exact_name",{{"type","string"}}},{"qualified_name",{{"type","string"}}},{"semantic_key",{{"type","string"}}},
            {"start_line",{{"type","integer"}}},{"end_line",{{"type","integer"}}},{"offset",{{"type","integer"}}},{"end_offset",{{"type","integer"}}},{"context_lines",{{"type","integer"}}},
            {"max_depth",{{"type","integer"}}},{"op",{{"type","string"}}},{"new_body",{{"type","string"}}},{"new_name",{{"type","string"}}},{"content",{{"type","string"}}},{"replacement",{{"type","string"}}},
            {"auto_sync_touched",{{"type","boolean"}}},{"auto_map_symbols",{{"type","boolean"}}},{"release_claims",{{"type","boolean"}}},
            {"source",{{"type","string"}}},{"targets",{{"type","array"},{"items",{{"type","string"}}}}},{"mappings",{{"type","array"},{"items",{{"type","object"}}}}},
            {"write_files",{{"type","array"},{"items",{{"type","object"}}}}},{"edits",{{"type","array"},{"items",{{"type","object"}}}}},{"verify_cases",{{"type","array"},{"items",{{"type","string"}}}}},
            {"path_prefix",{{"type","string"}}},{"path_contains",{{"type","string"}}},{"extension",{{"type","string"}}},{"disposition",{{"type","string"}}},{"stage",{{"type","string"}}},
            {"pending_reason",{{"type","string"}}},{"linked_source",{{"type","string"}}}};
        properties["wait"]={{"type","boolean"}};properties["job_id"]={{"type","string"}};properties["settings"]={{"type","object"}};properties["expected_config_revision"]={{"type","string"}};
        properties["generation"]={{"type","integer"}};properties["parent"]={{"type","string"}};properties["include_total"]={{"type","boolean"}};
        properties["depth"]={{"type","integer"},{"minimum",1},{"maximum",4}};properties["phase_id"]={{"type","string"}};properties["state"]={{"type","string"}};
        properties["include_non_relevant"]={{"type","boolean"}};properties["include_non_source"]={{"type","boolean"}};
        if(name=="export_inventory"||name=="backup_catalog"){
            properties["out"]={{"type","string"},{"minLength",1}};
            if(name=="export_inventory")properties["format"]={{"type","string"},{"enum",{"jsonl","json"}}};
            else properties["destination"]={{"type","string"},{"minLength",1}};
        }
        if(name=="import_existing_work"){
            properties["ids"]={{"type","array"},{"items",{{"type","integer"},{"minimum",1}}},{"minItems",1},{"maxItems",1000}};
            for(const auto* field:{"external_path","registry_path","catalog_path","base_directory","prefix"})properties[field]={{"type","string"}};
            for(const auto* field:{"verify","verify_inputs","verify_outputs"})properties[field]={{"type","boolean"}};
            properties["offset"]={{"type","integer"},{"minimum",0}};
        }
        if(name=="verify")
            properties["case_ids"]={{"type","array"},{"items",{{"type","string"},{"minLength",1}}}};
        bool mutating=name=="inventory_sync"||name=="batch_update"||name=="edit_symbol"||name=="complete_task"||name=="verify"||name=="job_control"||name=="project_settings"||name=="acquire_task"||name=="import_existing_work"||name=="export_inventory"||name=="backup_catalog"||name=="activate_workspace"||name=="onboard_workspace"||name=="create_file"||name=="edit_file"||name=="lsp_rename_symbol"||name=="restart_language_server"||name=="memory"||name=="housekeep";
        tools.push_back({{"name",name},{"description",description},{"inputSchema",{{"type","object"},{"properties",properties},{"additionalProperties",true}}},
            {"annotations",{{"readOnlyHint",!mutating},{"destructiveHint",false},{"idempotentHint",name!="batch_update"&&name!="edit_symbol"&&name!="complete_task"&&name!="export_inventory"},{"openWorldHint",name=="verify"||name=="complete_task"}}}});
    }
    return tools;
}
}
