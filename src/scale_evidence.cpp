#include "migratool/scale.hpp"
#include "migratool/sql.hpp"
#include "digest.hpp"
#include <fstream>
#include <map>

namespace mt {
namespace {
Json parsed(const Json& j){return j.is_string()?Json::parse(j.get<std::string>()):j;}
Json latest(Database& db,const std::string& kind){auto rows=db.query("SELECT data FROM evidence WHERE kind=? ORDER BY revision DESC LIMIT 1",{kind});return rows.empty()?Json::object():parsed(rows[0]["data"]);}
bool current_files(const Json& files){try{for(auto it=files.begin();it!=files.end();++it){auto path=fs::path(wide(it.key()));if(!fs::is_regular_file(path)||hash_file(path)!=it.value().get<std::string>())return false;}return true;}catch(...){return false;}}
bool fingerprints_match(Database& db, const Json& run) {
    if (!run.value("stable", false)) return false;
    const auto recorded = run.value("fingerprints", Json::object());
    if (!recorded.is_object()) return false;

    Json current = Json::object();
    db.each("SELECT id,data FROM roots", Json::array(), [&](const Json& root) {
        current[root["id"].get<std::string>()] = parsed(root["data"]).value("fingerprint", "");
        return true;
    });
    if (current.size() != recorded.size()) return false;

    for (const auto& [id, fingerprint] : current.items()) {
        const auto previous = recorded.find(id);
        if (previous == recorded.end() || *previous != fingerprint) return false;
    }
    return true;
}
}

void scale_fingerprints(Database& db){
    // At most 1024 leaves per bucket. Notes, claims and history do not dirty content.
    for(;;){auto dirty=db.query("SELECT root,bucket FROM dirty_buckets ORDER BY root,bucket LIMIT 1");if(dirty.empty())break;auto root=dirty[0]["root"];auto bucket=dirty[0]["bucket"];db.begin();
        try{Digest hash;db.each("SELECT id,path,sha256,bytes,mtime,relevant,category FROM files INDEXED BY fingerprint_bucket WHERE root=? AND (id/1024)=? AND deleted=0 ORDER BY id",{root,bucket},[&](const Json& row){auto line=row.dump();hash.add(line.data(),line.size());hash.add("\n",1);return true;});
            db.exec("INSERT INTO root_buckets VALUES(?,?,?) ON CONFLICT(root,bucket)DO UPDATE SET sha256=excluded.sha256",{root,bucket,hash.finish()});db.exec("DELETE FROM dirty_buckets WHERE root=? AND bucket=?",{root,bucket});db.commit();
        }catch(...){db.rollback();throw;}}
    db.begin();try{for(const auto& r:db.query("SELECT id,data FROM roots")){auto root=parsed(r["data"]);Digest hash;db.each("SELECT bucket,sha256 FROM root_buckets WHERE root=? ORDER BY bucket",{r["id"]},[&](const Json& row){auto line=row.dump();hash.add(line.data(),line.size());return true;});root["fingerprint"]=hash.finish();root["fingerprint_basis"]="sha256_of_ordered_content_metadata_buckets";db.exec("UPDATE roots SET data=? WHERE id=?",{root.dump(),r["id"]});}db.commit();}catch(...){db.rollback();throw;}
}

Json scale_evidence(const Project& p,Database& db,bool fresh){
    auto tests=latest(db,"tests"),builds=latest(db,"builds");const bool tests_current=fresh&&fingerprints_match(db,tests),builds_current=fresh&&fingerprints_match(db,builds);
    Json cases=Json::array(),build_states=Json::array(),passed=Json::array(),built=Json::array();std::map<std::string,bool> roots;
    for(const auto& cfg:p.config.value("cases",Json::array())){
        const bool has_lr=(cfg.contains("left")&&cfg["left"].is_object())||(cfg.contains("right")&&cfg["right"].is_object());
        const std::string kind=cfg.value("kind",has_lr?"differential":"target_test");
        Json state={{"id",cfg["id"]},{"kind",kind},{"state","NOT_RUN"},{"fresh",false}};
        for(const auto& previous:tests.value("cases",Json::array()))if(previous["id"]==cfg["id"]){
            state=previous;
            if(!state.contains("kind"))state["kind"]=kind;
            const bool valid=case_evidence_fresh(cfg,previous,tests_current);
            state["fresh"]=valid;if(!valid)state["state"]="STALE";break;}
        if(state["fresh"]==true&&state["state"]=="PASS")passed.push_back(cfg["id"]);cases.push_back(state);}
    for(const auto& cfg:p.config.value("builds",Json::array())){Json state={{"id",cfg["id"]},{"root",cfg["root"]},{"state","NOT_RUN"},{"fresh",false}};
        for(const auto& previous:builds.value("builds",Json::array()))if(previous["id"]==cfg["id"]){state=previous;const bool valid=builds_current&&previous.value("config_sha256","")==sha256(cfg.dump())&&current_files(previous.value("artifacts",Json::object()));state["fresh"]=valid;if(!valid)state["state"]="STALE";break;}
        const auto root=cfg["root"].get<std::string>();if(!roots.contains(root))roots[root]=true;roots[root]=roots[root]&&state["fresh"]==true&&state["state"]=="PASS";build_states.push_back(state);}
    for(const auto& [root,valid]:roots)if(valid)built.push_back(root);
    return {{"cases",cases},{"builds",build_states},{"passed",passed},{"built",built}};
}

Json scale_progress_sql(const Json& evidence){
    // Materialize only mapping validity inside SQLite. No catalogue-size JSON arrays.
    Json configured_roots=Json::array();
    std::map<std::string,bool> seen_cfg_roots;
    for(const auto& b:evidence.value("builds",Json::array())){
        if(b.contains("root")&&b["root"].is_string()){
            auto r=b["root"].get<std::string>();
            if(!seen_cfg_roots[r]){seen_cfg_roots[r]=true;configured_roots.push_back(r);}
        }
    }
    auto result=Json{{"params",Json::array({evidence["passed"].dump(),evidence["built"].dump(),configured_roots.dump()})},{"sql",R"sql(WITH passed AS(SELECT value AS id FROM json_each(?)),built AS(SELECT value AS root FROM json_each(?)),cfg_build_roots AS(SELECT value AS root FROM json_each(?)),
 checked AS MATERIALIZED(SELECT m.*,CASE WHEN substr(m.source,1,1)='F' THEN CAST(substr(m.source,2) AS INTEGER) ELSE (SELECT file_id FROM symbols WHERE id=CAST(substr(m.source,2) AS INTEGER) AND active=1) END AS source_file_id,
 EXISTS(SELECT 1 FROM json_each(m.hashes) h WHERE substr(h.key,1,1)='F') AND NOT EXISTS(SELECT 1 FROM json_each(m.hashes) h
 LEFT JOIN files hf ON substr(h.key,1,1)='F' AND hf.id=CAST(substr(h.key,2)AS INTEGER)
 LEFT JOIN symbols hs ON substr(h.key,1,1)='S' AND hs.id=CAST(substr(h.key,2)AS INTEGER)
 WHERE (substr(h.key,1,1)='F' AND (hf.id IS NULL OR hf.deleted<>0 OR hf.sha256 IS NOT h.value)) OR (substr(h.key,1,1)='S' AND (hs.id IS NULL OR hs.active<>1 OR json_extract(hs.data,'$.symbol_sha256') IS NOT h.value))) AS current,
 json_array_length(m.cases)>0 AND NOT EXISTS(SELECT 1 FROM json_each(m.cases) c WHERE NOT EXISTS(SELECT 1 FROM passed WHERE id=c.value)) AS cases_pass,
 EXISTS(SELECT 1 FROM built) AND NOT EXISTS(SELECT 1 FROM json_each(m.hashes) h JOIN files f ON substr(h.key,1,1)='F' AND f.id=CAST(substr(h.key,2)AS INTEGER) LEFT JOIN roots fr ON fr.id=f.root WHERE (COALESCE(fr.role,'target')='target' OR EXISTS(SELECT 1 FROM cfg_build_roots cr WHERE cr.root=f.root)) AND NOT EXISTS(SELECT 1 FROM built b WHERE b.root=f.root)) AS builds_pass
 FROM mappings m),valid_maps AS MATERIALIZED(SELECT *,current AND cases_pass AND builds_pass AS verified FROM checked),
 symbol_progress AS(SELECT u.file_id,u.units,COALESCE(sm.migrated,0) AS migrated,COALESCE(sm.verified,0) AS verified FROM file_units u LEFT JOIN(SELECT source_file_id AS file_id,SUM(CASE WHEN current AND complete THEN 1 ELSE 0 END) AS migrated,SUM(CASE WHEN verified THEN 1 ELSE 0 END) AS verified FROM valid_maps WHERE substr(source,1,1)='S' GROUP BY source_file_id) sm ON sm.file_id=u.file_id)
 )sql"}};
    if(evidence.contains("file_ids")){std::string ids;for(const auto& id:evidence["file_ids"]){if(!ids.empty())ids+=',';ids+=std::to_string(id.get<int64_t>());}if(ids.empty())ids="-1";
        auto sql=result["sql"].get<std::string>();auto at=sql.find("FROM mappings m)");sql.replace(at,std::string("FROM mappings m)").size(),"FROM mappings m WHERE m.source IN(SELECT 'F'||id FROM files WHERE id IN("+ids+")) OR m.source IN(SELECT 'S'||id FROM symbols WHERE file_id IN("+ids+")))");
        at=sql.find("FROM file_units u");sql.replace(at,std::string("FROM file_units u").size(),"FROM (SELECT file_id,units FROM file_units WHERE file_id IN("+ids+")) u");result["sql"]=sql;}
    return result;
}
}
