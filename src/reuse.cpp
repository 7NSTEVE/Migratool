#include "migratool/scale.hpp"
#include "migratool/sql.hpp"
#include <algorithm>

namespace mt {
namespace {
void check_cancelled(Cancel* cancel) {
    if (cancel && cancel->requested) throw std::runtime_error("CANCELLED");
}
std::string catalog_path(const std::string& input){
    std::error_code error;
    const auto resolved=fs::canonical(fs::path(wide(input)),error);
    if(error)throw std::runtime_error("EXTERNAL_CATALOG_OPEN: "+error.message());
    return path_text(resolved);
}
std::string verify_record(Json& record, Cancel* cancel) {
    Json checks={{"performed",true},{"source_hash_matches",false},{"output_hash_matches",false},
                 {"source_expected_sha256",record["source_sha256"]},{"output_expected_sha256",record["output_sha256"]}};
    for (const auto* side : {"source", "output"}) {
        check_cancelled(cancel);
        const std::string key(side);
        const auto path=record[key+"_path"].get<std::string>();
        if (path.empty()) {checks[key+"_error"]="PATH_MISSING";continue;}
        try {
            const auto digest=hash_file(fs::path(wide(path)),cancel);
            checks[key+"_observed_sha256"]=digest;
            const auto& expected=record[key+"_sha256"];
            checks[key+"_hash_matches"]=expected.is_string() &&
                _stricmp(expected.get_ref<const std::string&>().c_str(),digest.c_str())==0;
        } catch (const std::exception& error) {
            check_cancelled(cancel);
            checks[key+"_error"]=error.what();
        }
    }
    record["checks"]=std::move(checks);
    if (record["checks"]["source_hash_matches"]!=true)
        return record["source_sha256"].is_string()?"INPUT_NOT_VERIFIED":"INPUT_PROVENANCE_HASH_MISSING";
    if (record["checks"]["output_hash_matches"]==true) return "INPUT_OUTPUT_HASH_VERIFIED";
    return record["output_path"].get<std::string>().empty()?"INPUT_HASH_VERIFIED":"OUTPUT_NOT_VERIFIED";
}
Json revalidate_records(const Project& project, const Json& args, Cancel* cancel) {
    const auto ids=args.value("ids",Json::array());
    if (!ids.is_array() || ids.empty() || ids.size()>1000)
        throw std::runtime_error("REUSE_IDS_REQUIRED_1_TO_1000");
    Database database(project);
    Statement lookup(database,"SELECT id,detail FROM reuse_evidence WHERE id=?");
    Json pending=Json::array();
    int verified_inputs=0,verified_pairs=0;
    for (const auto& id : ids) {
        check_cancelled(cancel);
        if (!id.is_number_integer() || id.get<int64_t>()<=0) throw std::runtime_error("REUSE_ID_INVALID");
        const auto stored=lookup.first(Json::array({id}));
        if (stored.is_null()) throw std::runtime_error("REUSE_EVIDENCE_NOT_FOUND");
        auto record=Json::parse(stored["detail"].get<std::string>());
        record["state"]=verify_record(record,cancel);
        if (record["checks"]["source_hash_matches"]==true) ++verified_inputs;
        if (record["state"]=="INPUT_OUTPUT_HASH_VERIFIED") ++verified_pairs;
        pending.push_back({{"id",id},{"before",stored["detail"]},{"record",std::move(record)}});
    }
    check_cancelled(cancel);
    database.begin();
    try {
        Statement save(database,"UPDATE reuse_evidence SET state=?,checked=?,detail=? WHERE id=? AND detail=?");
        const auto checked=now();
        for (const auto& item : pending) {
            check_cancelled(cancel);
            save.exec({item["record"]["state"],checked,item["record"].dump(),item["id"],item["before"]});
            if (sqlite3_changes(database.native_handle())!=1) throw std::runtime_error("REUSE_EVIDENCE_CONFLICT");
        }
        database.change(args["actor"],"existing_work_revalidated",{{"ids",ids},{"inputs_verified",verified_inputs},{"input_output_pairs_verified",verified_pairs}});
        database.commit();
    } catch (...) {database.rollback();throw;}
    return {{"revision",database.revision()},{"revalidated",pending.size()},{"inputs_verified",verified_inputs},
            {"input_output_pairs_verified",verified_pairs},{"functional_parity_proven",false},{"originals_modified",false}};
}
}
Json scale_import(const Project& p,const Json& args,Cancel* cancel){
    if(args.value("actor","").empty())throw std::runtime_error("actor requerido");
    check_cancelled(cancel);
    if(args.value("action","import")=="revalidate")return revalidate_records(p,args,cancel);
    const auto path_fields=static_cast<int>(args.contains("registry_path"))+static_cast<int>(args.contains("catalog_path"))+static_cast<int>(args.contains("external_path"));
    if(path_fields>1)throw std::runtime_error("EXTERNAL_CATALOG_AMBIGUOUS");
    bool registry=args.contains("registry_path");
    const auto supplied_path=args.value("registry_path",args.value("catalog_path",args.value("external_path",std::string())));
    if(supplied_path.empty())throw std::runtime_error("EXTERNAL_CATALOG_REQUIRED");
    const auto input=catalog_path(supplied_path);
    sqlite3* external{};if(sqlite3_open_v2(input.c_str(),&external,SQLITE_OPEN_READONLY,nullptr)!=SQLITE_OK){if(external)sqlite3_close(external);throw std::runtime_error("EXTERNAL_CATALOG_OPEN");}struct Close{sqlite3* db;~Close(){sqlite3_close(db);}}close{external};sqlite3_busy_timeout(external,2000);
    if(args.contains("external_path")){
        const auto schema=Statement(external,"SELECT SUM(name IN('jobs','attempts','outputs','source_paths')) AS registry_tables,SUM(name IN('scans','entries')) AS catalog_tables FROM sqlite_master WHERE type='table'").first();
        if(schema.value("registry_tables",Json())==4)registry=true;
        else if(schema.value("catalog_tables",Json())==2)registry=false;
        else throw std::runtime_error("EXTERNAL_SCHEMA_UNSUPPORTED: el archivo no contiene un catálogo de archivos ni un registro de recuperaciones compatible");
    }
    const auto limit=std::clamp(args.value("limit",100),1,1000);const auto offset=args.value("offset",int64_t(0));auto cursor=args.value("cursor",std::string());Json continuation=cursor.empty()?Json::object():Json::parse(cursor);
    if(!continuation.empty()&&(catalog_path(continuation.value("catalog",""))!=input||continuation.value("registry",false)!=registry))throw std::runtime_error("CURSOR_CATALOG_MISMATCH");
    if(registry&&!args.value("prefix",std::string()).empty())throw std::runtime_error("REGISTRY_PREFIX_UNSUPPORTED: el filtro de carpeta corresponde a un catálogo de archivos");
    Json params=Json::array();std::string sql;fs::path base=fs::path(wide(args.value("base_directory",path_text(fs::path(wide(input)).parent_path()))));int64_t generation=0;
    if(registry){
        sql="SELECT a.id AS origin_id,j.source_sha256,(SELECT MIN(path) FROM source_paths s WHERE s.source_sha256=j.source_sha256) AS source_path,o.path AS output_path,o.sha256 AS output_sha256,j.architecture,j.action,j.tool_sha256,j.config_sha256 FROM outputs o JOIN attempts a ON a.id=o.attempt_id JOIN jobs j ON j.id=a.job_id WHERE 1=1";
        if(continuation.contains("attempt")){sql+=" AND (a.id>? OR(a.id=? AND o.path>?))";params.push_back(continuation["attempt"]);params.push_back(continuation["attempt"]);params.push_back(continuation["output"]);}sql+=" ORDER BY a.id,o.path LIMIT ? OFFSET ?";
    }else{
        Statement scan(external,"SELECT id,root FROM scans WHERE status='completed' AND (?=0 OR id=?) ORDER BY id DESC LIMIT 1");auto requested=continuation.value("generation",args.value("generation",int64_t(0)));scan.bind({requested,requested});if(!scan.next())throw std::runtime_error("EXTERNAL_GENERATION_MISSING");auto current=scan.row();generation=current["id"];if(!args.contains("base_directory"))base=fs::path(wide(current["root"]));
        sql="SELECT path AS origin_id,sha256 AS source_sha256,path AS source_path,'' AS output_path,NULL AS output_sha256,'' AS architecture,'catalog_entry' AS action,'' AS tool_sha256,'' AS config_sha256 FROM entries WHERE scan_id=? AND kind='file'";params.push_back(generation);
        const auto prefix=args.value("prefix",continuation.value("prefix",std::string()));if(!prefix.empty()){sql+=" AND path>=? AND path<?";params.push_back(prefix);params.push_back(prefix+"\xF4\x8F\xBF\xBF");}
        if(continuation.contains("after_path")){sql+=" AND path>?";params.push_back(continuation["after_path"]);}sql+=" ORDER BY path LIMIT ? OFFSET ?";
    }
    params.push_back(limit+1);params.push_back(continuation.empty()?offset:0);Statement rows(external,sql);rows.bind(params);Json records=Json::array();while(rows.next())records.push_back(rows.row());
    const bool more=records.size()>static_cast<size_t>(limit);if(more)records.erase(records.end()-1);
    const bool verify=args.value("verify_outputs",false)||args.value("verify_inputs",false)||args.value("verify",false);int verified_inputs=0,verified_pairs=0;Json next=nullptr;
    for(auto& r:records){r["origin_output_path"]=r["output_path"];auto normalize=[&](const Json& value){if(!value.is_string()||value.get<std::string>().empty())return std::string();auto path=fs::path(wide(value));if(!path.is_absolute())path=base/path;return path_text(path.lexically_normal());};r["source_path"]=normalize(r["source_path"]);r["output_path"]=normalize(r["output_path"]);
        Json checks={{"performed",verify},{"source_hash_matches",false},{"output_hash_matches",false},{"source_expected_sha256",r["source_sha256"]},{"output_expected_sha256",r["output_sha256"]}};std::string state="CANDIDATE";
        if(verify){state=verify_record(r,cancel);checks=r["checks"];if(checks["source_hash_matches"]==true)++verified_inputs;if(state=="INPUT_OUTPUT_HASH_VERIFIED")++verified_pairs;}
        r["checks"]=checks;r["state"]=state;
    }
    Database db(p);db.begin();try{Statement save(db,"INSERT INTO reuse_evidence(source_sha256,source_path,output_path,output_sha256,origin_catalog,origin_id,architecture,action,tool_sha256,config_sha256,state,checked,detail)VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)ON CONFLICT(origin_catalog,origin_id,output_path)DO UPDATE SET source_sha256=excluded.source_sha256,source_path=excluded.source_path,output_sha256=excluded.output_sha256,tool_sha256=excluded.tool_sha256,config_sha256=excluded.config_sha256,state=excluded.state,checked=excluded.checked,detail=excluded.detail");
        for(const auto& r:records){check_cancelled(cancel);save.exec({r["source_sha256"],r["source_path"],r["output_path"],r["output_sha256"],input,r["origin_id"].is_string()?r["origin_id"]:Json(r["origin_id"].dump()),r["architecture"],r["action"],r["tool_sha256"],r["config_sha256"],r["state"],verify?Json(now()):Json(),r.dump()});}
        db.change(args["actor"],"existing_work_imported",{{"origin",input},{"generation",generation},{"rows",records.size()},{"inputs_verified",verified_inputs},{"input_output_pairs_verified",verified_pairs}});db.commit();
    }catch(...){db.rollback();throw;}
    if(more){const auto& last=records.back();Json value={{"catalog",input},{"registry",registry}};if(registry){value["attempt"]=last["origin_id"];value["output"]=last["origin_output_path"];}else{value["generation"]=generation;value["prefix"]=args.value("prefix",continuation.value("prefix",std::string()));value["after_path"]=last["origin_id"];}next=value.dump();}
    return {{"revision",db.revision()},{"imported",records.size()},{"inputs_verified",verified_inputs},{"input_output_pairs_verified",verified_pairs},{"outputs_verified",verified_pairs},{"next_cursor",next},{"next_offset",more?Json(offset+records.size()):Json()},{"generation",generation},{"functional_parity_proven",false},{"originals_modified",false}};
}
}
