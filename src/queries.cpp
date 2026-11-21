#include "migratool/scale.hpp"
#include <algorithm>
#include <cctype>
#include <map>
#include <set>

namespace mt {
namespace {
std::string lower(std::string s){std::transform(s.begin(),s.end(),s.begin(),[](unsigned char c){return static_cast<char>(tolower(c));});return s;}
std::string normalized_language(std::string s){
    std::transform(s.begin(),s.end(),s.begin(),[](unsigned char c){return static_cast<char>(tolower(c));});
    if(s=="cpp"||s=="cxx"||s=="c++"||s=="c++23")return "C++";if(s=="csharp"||s=="cs"||s=="c#")return "C#";
    if(s=="c23"||s=="c17"||s=="c11"||s=="c")return "C";
    if(s=="python"||s=="py")return "Python";if(s=="js"||s=="javascript")return "JavaScript";
    if(s=="ts"||s=="typescript")return "TypeScript";return s;
}
std::set<std::string> file_columns(const std::string& sql){
    std::set<std::string> columns={"id","root"};
    size_t position=0;
    while((position=sql.find("f.",position))!=std::string::npos){
        if(position>0&&(std::isalnum(static_cast<unsigned char>(sql[position-1]))||sql[position-1]=='_')){position+=2;continue;}
        const auto begin=position+2;auto end=begin;
        while(end<sql.size()&&(std::isalnum(static_cast<unsigned char>(sql[end]))||sql[end]=='_'))++end;
        if(end>begin)columns.insert(sql.substr(begin,end-begin));
        position=end;
    }
    return columns;
}
}
Json query_page(const Project& project,Database& db,const Json& q){
    const auto kind=q.value("kind","files");const bool symbols=kind=="symbols"||kind=="find_symbol"||kind=="symbols_overview"||kind=="get_symbols_overview";
    std::string where=" WHERE f.deleted=?";Json params={q.value("deleted",false)?1:0};
    auto add=[&](const std::string& sql,const Json& value){where+=" AND "+sql;params.push_back(value);};
    const auto revision=db.revision();int64_t offset=0;auto cur=q.value("cursor","");
    const bool keyset=q.value("pagination",std::string())=="keyset"||cur.starts_with("v2:")||(scale_enabled(project,db)&&!q.value("deleted",false));
    int64_t generation=0,last_id=0;std::string cursor_prefix;
    if(keyset){auto filters=q;for(const auto* key:{"cursor","limit","fields","token_budget","budget_tokens","include_total"})filters.erase(key);const auto fingerprint=sha256(filters.dump()).substr(0,16);
        generation=q.value("generation",db.integer("SELECT COALESCE(MAX(CAST(value AS INTEGER)),0) FROM meta WHERE key='published_generation'"));
        if(!cur.empty()){const auto first=cur.find(':',3),second=cur.find(':',first+1);if(!cur.starts_with("v2:")||first==std::string::npos||second==std::string::npos)throw std::runtime_error("CURSOR_INVALID");generation=std::stoll(cur.substr(3,first-3));if(cur.substr(first+1,second-first-1)!=fingerprint)throw std::runtime_error("CURSOR_FILTER_MISMATCH");last_id=std::stoll(cur.substr(second+1));}
        if(generation&&db.integer("SELECT EXISTS(SELECT 1 FROM inventory_generations WHERE id=? AND state='published')",{generation})==0)throw std::runtime_error("CURSOR_GENERATION_EXPIRED");
        cursor_prefix="v2:"+std::to_string(generation)+":"+fingerprint+":";
    }
    else if(!cur.empty()){
        const auto at=cur.find(':');if(at==std::string::npos)throw std::runtime_error("CURSOR_INVALID");
        if(std::stoll(cur.substr(0,at))!=revision)throw std::runtime_error("CURSOR_STALE: índice cambió; reinicie consulta");
        offset=std::stoll(cur.substr(at+1));if(offset<0)throw std::runtime_error("CURSOR_INVALID");
    }
    const int limit=q.value("limit",10);if(limit<1||limit>1000)throw std::runtime_error("limit debe estar entre 1 y 1000");
    if(symbols)where+=" AND s.active=1";
    if(kind=="documentation")add("f.category=?","documentation");
    if(kind=="ignored")add("f.git_state=?","!!");
    if(kind=="excluded")add("f.build_state=?","NOT_IN_COMPILATION_DATABASE");
    if(kind=="new")add("f.first_revision>?",q.value("since_revision",int64_t(0)));
    if(kind=="orphans")where+=" AND f.relevant=1 AND NOT EXISTS(SELECT 1 FROM mappings m WHERE m.source='F'||f.id) AND NOT EXISTS(SELECT 1 FROM dependencies d WHERE d.target_id=f.id)";
    for(const auto* k:{"root","category","git_state","relevant"})if(q.contains(k))add("f."+std::string(k)+"=?",q[k]);
    if(q.contains("role"))add("r.role=?",q["role"]);
    if(q.contains("linked_source")){auto source=q["linked_source"].get<std::string>();if(source.empty()||source[0]!='F')throw std::runtime_error("LINK_SOURCE_FILE_REQUIRED");where+=" AND f.id IN(SELECT COALESCE(tf.id,ts.file_id) FROM(SELECT targets FROM mappings WHERE source=? UNION ALL SELECT m.targets FROM symbols s JOIN mappings m ON m.source='S'||s.id WHERE s.file_id=? AND s.active=1)m JOIN json_each(m.targets)t LEFT JOIN files tf ON tf.id=CASE WHEN substr(t.value,1,1)='F' THEN CAST(substr(t.value,2) AS INTEGER) END LEFT JOIN symbols ts ON ts.id=CASE WHEN substr(t.value,1,1)='S' THEN CAST(substr(t.value,2) AS INTEGER) END)";params.push_back(source);params.push_back(std::stoll(source.substr(1)));}
    const auto pending=q.value("pending_reason",std::string());
    if(pending=="missing_target")where+=" AND NOT EXISTS(SELECT 1 FROM mappings m WHERE m.source='F'||f.id) AND NOT EXISTS(SELECT 1 FROM symbols s JOIN mappings m ON m.source='S'||s.id WHERE s.file_id=f.id AND s.active=1)";
    else if(pending=="analysis_partial")where+=" AND f.analysis<>'ANALYZED'";
    else if(pending=="tests_missing")where+=" AND NOT EXISTS(SELECT 1 FROM mappings m WHERE m.source='F'||f.id AND json_array_length(m.cases)>0)";
    else if(pending=="claimed")where+=" AND EXISTS(SELECT 1 FROM task_claims c WHERE c.entity='F'||f.id AND c.expires_at>strftime('%Y-%m-%dT%H:%M:%fZ','now'))";
    else if(pending=="stale")where+=" AND EXISTS(SELECT 1 FROM mappings m JOIN json_each(m.hashes) h LEFT JOIN files hf ON hf.id=CASE WHEN substr(h.key,1,1)='F' THEN CAST(substr(h.key,2) AS INTEGER) END WHERE m.source='F'||f.id AND substr(h.key,1,1)='F' AND(hf.id IS NULL OR hf.deleted<>0 OR hf.sha256 IS NOT h.value))";
    else if(pending=="declared_complete")where+=" AND EXISTS(SELECT 1 FROM mappings m WHERE m.source='F'||f.id AND m.complete=1 AND json_extract(m.hashes,'$.'||char(34)||'F'||f.id||char(34))=f.sha256)";
    else if(pending=="non_relevant")where+=" AND f.relevant=0";
    else if(pending=="with_disposition")where+=" AND (EXISTS(SELECT 1 FROM mappings m WHERE m.source='F'||f.id AND json_extract(m.hashes,'$._disposition') IS NOT NULL) OR json_extract(f.data,'$.disposition') IS NOT NULL)";
    if(project.config.value("inventory_mode","universal")=="source_only"&&!q.value("deleted",false))where+=" AND COALESCE(json_extract(f.data,'$.source_code'),0)=1";
    if(q.contains("language")){const auto lang=normalized_language(q["language"]);add("lower(f.language)=lower(?)",lang);}
    if(q.contains("id")){auto id=q["id"].get<std::string>();if(id.size()<2)throw std::runtime_error("ID_INVALID");add(symbols?"s.id=?":"f.id=?",std::stoll(id.substr(1)));}
    if(symbols){
        for(const auto* k:{"file","file_id"})if(q.contains(k)){auto id=q[k].get<std::string>();if(!id.empty()&&id[0]=='F')add("f.id=?",std::stoll(id.substr(1)));else {where+=" AND (f.absolute_path=? OR f.path=? OR f.path LIKE ?)";params.push_back(id);params.push_back(id);params.push_back("%/"+id);}}
        if(q.contains("exact_name"))add("s.name=?",q["exact_name"]);
        if(q.contains("qualified_name"))add("s.qualified_name=?",q["qualified_name"]);
        if(q.contains("signature"))add("COALESCE(json_extract(s.data,'$.signature'),'')=?",q["signature"]);
        if(q.contains("symbol_kind")){auto symbol_kind=lower(q["symbol_kind"].get<std::string>());if(symbol_kind=="method"||symbol_kind=="function")where+=" AND (lower(s.kind) LIKE '%method%' OR lower(s.kind) LIKE '%function%' OR lower(s.kind) IN('fn','func','cxxmethod'))";else if(symbol_kind=="class"||symbol_kind=="type"||symbol_kind=="struct")where+=" AND (lower(s.kind) LIKE '%class%' OR lower(s.kind) LIKE '%struct%' OR lower(s.kind) LIKE '%interface%' OR lower(s.kind) LIKE '%type%')";else add("instr(lower(s.kind),lower(?))>0",symbol_kind);}
        if(q.contains("signature_contains"))add("instr(COALESCE(json_extract(s.data,'$.signature'),''),?)>0",q["signature_contains"]);
    }
    auto text=q.value("text",q.value("name",std::string()));if(!text.empty()){
        if(symbols){where+=" AND (instr(lower(s.name),lower(?))>0 OR instr(lower(s.qualified_name),lower(?))>0 OR instr(lower(f.absolute_path),lower(?))>0)";params.push_back(text);params.push_back(text);params.push_back(text);}
        else {where+=" AND (instr(lower(f.absolute_path),lower(?))>0 OR f.id IN(SELECT CAST(substr(entity_id,2)AS INTEGER) FROM search WHERE entity_type='file' AND search MATCH ?))";params.push_back(text);std::string escaped="\"";for(char c:text){escaped+=c;if(c=='\"')escaped+=c;}escaped+='\"';params.push_back(escaped);}
    }
    std::string columns=symbols?"'S'||s.id AS id,'F'||f.id AS file_id,f.root,f.path,f.absolute_path,f.language,s.name,s.qualified_name,s.kind,s.line,s.data":
        "'F'||f.id AS id,f.root,r.role,f.path,f.absolute_path,f.sha256,f.category,f.language,f.relevant,f.analysis,f.git_state,f.build_state,f.deleted,f.first_revision,f.last_revision";
    auto fields=q.value("fields",Json::array());bool copies=!symbols&&(fields.empty()||std::find(fields.begin(),fields.end(),Json("duplicate_copies"))!=fields.end());
    if(copies)columns+=",COALESCE((SELECT copies FROM hash_copies h WHERE h.sha256=f.sha256),0) AS duplicate_copies";
    if(!symbols&&q.value("detail","")=="full")columns+=",f.data";
    if(!symbols&&q.value("deleted",false))columns+=",COALESCE(json_extract(f.data,'$.filtered_by_inventory_mode'),0) AS filtered_by_inventory_mode";
    std::vector<std::string> optional_symbol_fields;
    if(!fields.empty()){
        std::map<std::string,std::string> available={{"root","f.root"},{"path","f.path"},{"absolute_path","f.absolute_path"},{"language","f.language"}};
        if(symbols){
            available["id"]="'S'||s.id";available["file_id"]="'F'||f.id";
            for(const auto* field:{"name","qualified_name","kind","line"})available[field]="s."+std::string(field);
            available["signature"]="CASE WHEN json_type(s.data,'$.signature') IS NULL THEN '' ELSE json_extract(s.data,'$.signature') END";
            for(const auto* field:{"end_line","column","end_column","offset","end_offset","symbol_sha256","engine"}){
                available[field]="json_extract(s.data,'$."+std::string(field)+"')";
                if(std::find(fields.begin(),fields.end(),Json(field))!=fields.end())optional_symbol_fields.emplace_back(field);
            }
        }else{
            available["id"]="'F'||f.id";available["role"]="r.role";
            for(const auto* field:{"sha256","category","relevant","analysis","git_state","build_state","deleted","first_revision","last_revision"})available[field]="f."+std::string(field);
            available["duplicate_copies"]="COALESCE((SELECT copies FROM hash_copies h WHERE h.sha256=f.sha256),0)";
            if(q.value("detail",std::string())=="full"){
                available["text_content"]="COALESCE(json_extract(f.data,'$.text_content'),'')";
                available["text_truncated"]="COALESCE(json_extract(f.data,'$.text_truncated'),0)";
            }
            if(q.value("deleted",false))available["filtered_by_inventory_mode"]="COALESCE(json_extract(f.data,'$.filtered_by_inventory_mode'),0)";
        }
        columns=(symbols?"s.id":"f.id")+std::string(" AS __cursor_key");
        for(const auto& requested:fields){
            const auto field=requested.get<std::string>();const auto found=available.find(field);
            if(found!=available.end())columns+=","+found->second+" AS "+field;
        }
        for(const auto& field:optional_symbol_fields)columns+=",json_type(s.data,'$."+field+"') IS NOT NULL AS __present_"+field;
    }
    std::string file_source="files f";
    if(keyset&&generation&&!symbols){
        const auto gen=std::to_string(generation);
        std::string snapshot_columns,legacy_columns;
        for(const auto& field:file_columns(columns+where)){
            if(!snapshot_columns.empty()){snapshot_columns+=',';legacy_columns+=',';}
            if(field=="deleted"){snapshot_columns+="0 AS deleted";legacy_columns+="0 AS deleted";}
            else if(field=="root"||field=="path"||field=="absolute_path"){snapshot_columns+="c."+field;legacy_columns+="f."+field;}
            else{snapshot_columns+="f."+field;legacy_columns+="f."+field;}
        }
        const bool has_legacy=db.integer("SELECT EXISTS(SELECT 1 FROM files WHERE deleted=0 AND seen_generation=0)")!=0;
        std::string legacy_union;
        if(has_legacy){
            legacy_union=" UNION ALL SELECT "+legacy_columns+" FROM files f WHERE f.deleted=0 AND f.seen_generation=0 AND NOT EXISTS(SELECT 1 FROM latest_scopes s WHERE s.root=f.root AND (s.prefix='' OR f.path=rtrim(s.prefix,'/') COLLATE NOCASE OR (f.path>=s.prefix COLLATE NOCASE AND f.path<s.prefix||char(1114111) COLLATE NOCASE)))";
        }
        file_source="(WITH latest_scopes AS MATERIALIZED(SELECT s.root,s.prefix,MAX(s.generation) AS generation FROM generation_scopes s JOIN inventory_generations g ON g.id=s.generation AND g.state='published' WHERE s.generation<="+gen+" GROUP BY s.root,s.prefix) SELECT "+snapshot_columns+" FROM files f CROSS JOIN census_entries c INDEXED BY census_file_history WHERE c.file_id=f.id AND c.generation<="+gen+" AND c.kind='file' AND EXISTS(SELECT 1 FROM inventory_generations g WHERE g.id=c.generation AND g.state='published') AND NOT EXISTS(SELECT 1 FROM latest_scopes s WHERE s.generation>c.generation AND s.root=c.root AND (s.prefix='' OR c.path=rtrim(s.prefix,'/') COLLATE NOCASE OR (c.path>=s.prefix COLLATE NOCASE AND c.path<s.prefix||char(1114111) COLLATE NOCASE)))"+legacy_union+") f";
    }
    const std::string from=symbols?" FROM symbols s JOIN files f ON s.file_id=f.id LEFT JOIN roots r ON f.root=r.id":" FROM "+file_source+" LEFT JOIN roots r ON f.root=r.id";
    const Json total=q.value("include_total",true)?Json(db.integer("SELECT COUNT(*)"+from+where,params)):Json(nullptr);
    auto page_params=params;if(keyset){where+=symbols?" AND s.id>?":" AND f.id>?";page_params.push_back(last_id);}page_params.push_back(limit+1);if(!keyset)page_params.push_back(offset);
    auto rows=db.query("SELECT "+columns+from+where+(symbols?" ORDER BY s.id":" ORDER BY f.id")+(keyset?" LIMIT ?":" LIMIT ? OFFSET ?"),page_params);
    const bool more=rows.size()>static_cast<size_t>(limit);if(more)rows.erase(rows.end()-1);
    Json row_cursors=Json::array();for(auto& row:rows){
        if(keyset)row_cursors.push_back(cursor_prefix+(row.contains("__cursor_key")?std::to_string(row["__cursor_key"].get<int64_t>()):row["id"].get<std::string>().substr(1)));
        row.erase("__cursor_key");
        for(const auto& field:optional_symbol_fields){if(row["__present_"+field]==0)row.erase(field);row.erase("__present_"+field);}
        if(!symbols&&row.contains("text_truncated"))row["text_truncated"]=row["text_truncated"].get<int>()!=0;
        if(row.contains("data")){auto data=Json::parse(row["data"].get<std::string>());row.erase("data");
            if(symbols){for(const auto* k:{"signature","end_line","column","end_column","offset","end_offset","symbol_sha256","engine"})if(data.contains(k))row[k]=data[k];if(!row.contains("signature"))row["signature"]="";}
            else {row["text_content"]=data.value("text_content","");row["text_truncated"]=data.value("text_truncated",false);}
        }
        if(!fields.empty()){Json filtered=Json::object();for(const auto& field:fields)if(row.contains(field.get<std::string>()))filtered[field.get<std::string>()]=row[field.get<std::string>()];row=std::move(filtered);}
    }
    if(keyset)return {{"revision",revision},{"generation",generation},{"total",total},{"items",rows},{"cursor",cur.empty()?cursor_prefix+"0":cur},{"row_cursors",row_cursors},{"next_cursor",more&&!row_cursors.empty()?row_cursors.back():Json(nullptr)}};
    return {{"revision",revision},{"total",total},{"items",rows},{"cursor",std::to_string(revision)+":"+std::to_string(offset)},
        {"next_cursor",more?Json(std::to_string(revision)+":"+std::to_string(offset+rows.size())):Json(nullptr)}};
}
}
