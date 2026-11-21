#include "migratool/core.hpp"
#include "migratool/gateway.hpp"
#include "migratool/scale.hpp"
#include "migratool/agent_supertools.hpp"
#include "migratool/docs_hub.hpp"
#include "migratool/git_intel.hpp"
#include "migratool/knowledge.hpp"
#include "migratool/path_lineage.hpp"
#include "migratool/workstream.hpp"
#include <sqlite3.h>
#include <objbase.h>
#include <xmllite.h>
#include <shlwapi.h>
#include <algorithm>
#include <fstream>
#include <set>
#include <map>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <cstdlib>

namespace mt {
namespace {
std::string expand(std::string s, const Project& p, const fs::path& work={}, const std::string& root="") {
    const std::vector<std::pair<std::string,std::string>> values={{"{project}",path_text(p.directory())},{"{work}",path_text(work)},{"{root}",root},{"{bin}",path_text(executable_dir())}};
    for(const auto& [from,to]:values) { size_t at=0; while((at=s.find(from,at))!=std::string::npos) { s.replace(at,from.size(),to); at+=to.size(); } }
    return s;
}
Json fingerprints(const Json& s) {
    Json out=Json::object(); for(const auto& r:s["roots"]) out[r["id"].get<std::string>()]=r.value("fingerprint",""); return out;
}
Json audit_inventory_gate(const Project& p,Log log,Cancel* cancel) {
    bool use_scale=false;
    {
        Database db(p);
        use_scale=scale_enabled(p,db);
    }
    if(!use_scale) return inventory(p,log,cancel);
    for(int attempt=0;attempt<2;++attempt){
        scale_run(p,Json{{"wait",true}},log,cancel);
        Database check_db(p);
        auto check_sum=scale_summary(p,check_db);
        if(check_sum.value("inventory_complete",false)&&!check_sum.value("index_dirty",true))break;
    }
    Database db(p);
    auto summary=scale_summary(p,db);
    const bool complete=summary.value("inventory_complete",false)&&!summary.value("index_dirty",true);
    Json roots=Json::array();
    for(const auto& row:db.query("SELECT id,data FROM roots ORDER BY id")) {
        auto parsed=row["data"].is_string()?Json::parse(row["data"].get<std::string>()):row["data"];
        roots.push_back({{"id",row["id"]},{"fingerprint",parsed.value("fingerprint","")}});
    }
    return {{"complete",complete},{"roots",roots}};
}
const Json& root_config(const Project& p, const std::string& id) {
    for(const auto& r:p.config["roots"]) if(r["id"]==id) return r;
    throw std::runtime_error("Raíz desconocida: "+id);
}
std::vector<std::string> command(const Json& c, const Project& p, const fs::path& work={}) {
    auto root=p.resolve(root_config(p,c.at("root"))["path"]);
    auto args=c.at("argv").get<std::vector<std::string>>();
    for(auto& a:args) a=expand(a,p,work,path_text(root));
    return args;
}
Json execution_files(const std::vector<std::string>& args, const fs::path& cwd) {
    Json out=Json::object();
    for(size_t i=0;i<args.size();++i) {
        auto path=fs::path(wide(args[i])); if(!path.is_absolute()) path=cwd/path;
        if(i==0 && !fs::is_regular_file(path)) {
            std::wstring found(32768,L'\0'); auto n=SearchPathW(nullptr,wide(args[i]).c_str(),L".exe",static_cast<DWORD>(found.size()),found.data(),nullptr);
            if(n && n<found.size()) { found.resize(n); path=fs::path(found); }
        }
        if(fs::is_regular_file(path)) out[path_text(fs::weakly_canonical(path))]=hash_file(path);
    }
    return out;
}
bool process_ok(const Json& r, int expected=0) {
    return !r.value("timed_out",false) && !r.value("cancelled",false) && !r.value("output_limit",false) && r.value("exit_code",DWORD(-1))==static_cast<DWORD>(expected);
}
bool files_fresh(const Json& files) {
    for(auto it=files.begin();it!=files.end();++it) {
        auto path=fs::path(wide(it.key())); if(!fs::is_regular_file(path)||hash_file(path)!=it.value().get<std::string>()) return false;
    }
    return true;
}
Json maybe(const fs::path& path, const Json& fallback) { return fs::exists(path)?read_json(path):fallback; }
Json observed_files(const fs::path& path) {
    if(!fs::exists(path)) return nullptr;
    Json out=Json::object();
    auto add=[&](const fs::path& f,const std::string& rel) {
        if(GetFileAttributesW(f.c_str())&FILE_ATTRIBUTE_REPARSE_POINT) throw std::runtime_error("Observador no permite enlaces: "+path_text(f));
        out[rel]={{"bytes",fs::file_size(f)},{"sha256",hash_file(f)}};
    };
    if(fs::is_regular_file(path)) add(path,".");
    else for(const auto& e:fs::recursive_directory_iterator(path)) {
        if(GetFileAttributesW(e.path().c_str())&FILE_ATTRIBUTE_REPARSE_POINT) throw std::runtime_error("Observador no permite enlaces");
        if(e.is_regular_file()) add(e.path(),path_text(e.path().lexically_relative(path)));
        else if(e.is_directory()) out[path_text(e.path().lexically_relative(path))+"/"]="directory";
    }
    return out;
}
std::string crlf(std::string s) { size_t at=0; while((at=s.find("\r\n",at))!=std::string::npos) s.replace(at,2,"\n"); return s; }
std::string html(const std::string& s) {
    std::string out; for(char c:s) switch(c) { case '&':out+="&amp;";break;case '<':out+="&lt;";break;case '>':out+="&gt;";break;case '"':out+="&quot;";break;case '\'':out+="&#39;";break;default:out+=c;} return out;
}
std::string percentage_text(const Json& value){if(value.is_null())return "sin verificar";std::ostringstream out;out<<std::fixed<<std::setprecision(1)<<value.get<double>()<<" %";return out.str();}
std::string windows_path(const fs::path& p) { return utf8(fs::canonical(p).wstring()); }
const Json* find_file(const Json& s,const std::string& id) {
    for(const auto& f:s["files"]) if(f["id"]==id) return &f; return nullptr;
}
const Json* find_symbol(const Json& s,const std::string& id) {
    for(const auto& sym:s["symbols"]) if(sym["id"]==id) return &sym; return nullptr;
}
std::string metric(const Json& v,const std::string& name) {
    if(!v.contains(name) || v[name].is_null()) return "sin datos";
    return std::to_string(v[name].get<int>());
}
}
Json sqlite_snapshot(const fs::path& path) {
    sqlite3* db{};
    if(sqlite3_open_v2(path_text(path).c_str(),&db,SQLITE_OPEN_READONLY,nullptr)!=SQLITE_OK) {
        auto msg=db?std::string(sqlite3_errmsg(db)):"SQLite no disponible"; if(db) sqlite3_close(db); throw std::runtime_error(msg);
    }
    struct Close { sqlite3* db; ~Close(){sqlite3_close(db);} } close{db};
    sqlite3_busy_timeout(db,3000);
    if(sqlite3_exec(db,"BEGIN",nullptr,nullptr,nullptr)!=SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db));
    Json out={{"schema",Json::array()},{"tables",Json::object()}};
    auto rows=[&](const std::string& sql) {
        sqlite3_stmt* stmt{}; if(sqlite3_prepare_v2(db,sql.c_str(),-1,&stmt,nullptr)!=SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db));
        struct Final { sqlite3_stmt* s; ~Final(){sqlite3_finalize(s);} } final{stmt};
        Json result=Json::array(); int rc;
        while((rc=sqlite3_step(stmt))==SQLITE_ROW) {
            Json row=Json::array();
            for(int i=0;i<sqlite3_column_count(stmt);++i) {
                switch(sqlite3_column_type(stmt,i)) {
                    case SQLITE_NULL: row.push_back({{"type","null"}}); break;
                    case SQLITE_INTEGER:row.push_back({{"type","integer"},{"value",sqlite3_column_int64(stmt,i)}});break;
                    case SQLITE_FLOAT:row.push_back({{"type","real"},{"value",sqlite3_column_double(stmt,i)}});break;
                    case SQLITE_BLOB: {
                        const auto* bytes=static_cast<const unsigned char*>(sqlite3_column_blob(stmt,i));
                        std::string hex; static const char chars[]="0123456789abcdef";
                        for(int j=0;j<sqlite3_column_bytes(stmt,i);++j) {hex+=chars[bytes[j]>>4];hex+=chars[bytes[j]&15];}
                        row.push_back({{"type","blob"},{"value",hex}});break;
                    }
                    default:row.push_back({{"type","text"},{"value",std::string(reinterpret_cast<const char*>(sqlite3_column_text(stmt,i)),static_cast<size_t>(sqlite3_column_bytes(stmt,i)))}});
                }
            }
            result.push_back(row);
        }
        if(rc!=SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db));
        std::sort(result.begin(),result.end(),[](const auto& a,const auto& b){return a.dump()<b.dump();});
        return result;
    };
    out["schema"]=rows("SELECT type,name,tbl_name,sql FROM sqlite_master ORDER BY type,name");
    for(const auto& row:out["schema"]) if(row[0].value("value","")=="table") {
        auto name=row[1]["value"].get<std::string>(); std::string quoted="\"";
        for(char c:name) { quoted+=c; if(c=='"') quoted+='"'; } quoted+='"';
        out["tables"][name]=rows("SELECT * FROM "+quoted);
    }
    sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr); return out;
}
Json compare_outputs(const Json& test,const Json& l,const Json& r,const fs::path& lw,const fs::path& rw) {
    Json result={{"state","PASS"},{"differences",Json::array()},{"observations",Json::array()}};
    auto diff=[&](const std::string& field){result["state"]="DIFFERENT";result["differences"].push_back(field);};
    const int expected=test.value("expected_exit",0);
    if(!process_ok(l,expected)||!process_ok(r,expected)) {
        result["state"]="ERROR"; result["differences"].push_back("exit_code/timeout/cancel/output_limit"); return result;
    }
    auto left=read_text(fs::path(wide(l.at("stdout"))),128*1024*1024); auto right=read_text(fs::path(wide(r.at("stdout"))),128*1024*1024);
    const auto comp=test.value("comparator","bytes");
    const double abs_tol=test.value("abs_tol",test.value("numeric_tolerance",1e-6));
    const double rel_tol=test.value("rel_tol",1e-9);
    auto num_close=[&](double va,double vb){
        if(std::isnan(va)||std::isnan(vb)||std::isinf(va)||std::isinf(vb)) return false;
        return std::abs(va-vb)<=std::max(abs_tol,rel_tol*std::max(std::abs(va),std::abs(vb)));
    };
    auto json_close=[&](auto& self,const nlohmann::json& a,const nlohmann::json& b)->bool {
        if(a.is_number()&&b.is_number()) return num_close(a.get<double>(),b.get<double>());
        if(a.type()!=b.type()) return false;
        if(a.is_object()) {
            if(a.size()!=b.size()) return false;
            for(auto it=a.begin();it!=a.end();++it) {
                if(!b.contains(it.key())||!self(self,it.value(),b.at(it.key()))) return false;
            }
            return true;
        }
        if(a.is_array()) {
            if(a.size()!=b.size()) return false;
            for(size_t i=0;i<a.size();++i) if(!self(self,a[i],b[i])) return false;
            return true;
        }
        return a==b;
    };
    if(comp=="json"||comp=="json_numeric") {
        auto a=nlohmann::json::parse(left),b=nlohmann::json::parse(right);
        for(const auto& pointer:test.value("ignore_json_pointers",Json::array())) {
            const auto ptr=nlohmann::json::json_pointer(pointer.get<std::string>());
            if(!a.contains(ptr)||!b.contains(ptr)) throw std::runtime_error("Normalización apunta a campo inexistente: "+pointer.get<std::string>());
            // Reemplazar conserva posiciones de arrays; ambos valores quedan explícitamente fuera de comparación.
            a[ptr]=nullptr; b[ptr]=nullptr;
        }
        const bool use_tol=(comp=="json_numeric")||test.contains("abs_tol")||test.contains("rel_tol")||test.contains("numeric_tolerance");
        if(use_tol ? !json_close(json_close,a,b) : (a!=b)) diff("stdout_json");
        if(test.contains("expected_stdout")) {
            auto exp=nlohmann::json::parse(test["expected_stdout"].dump());
            if(use_tol ? !json_close(json_close,a,exp) : (a!=exp)) diff("expected_stdout");
        }
    } else if(comp=="numeric") {
        auto tokenize=[](const std::string& s){
            std::vector<std::string> out; std::string cur;
            for(char c:s){if(std::isspace(static_cast<unsigned char>(c))||c==','||c==';'){if(!cur.empty()){out.push_back(cur);cur.clear();}}else cur+=c;}
            if(!cur.empty())out.push_back(cur); return out;
        };
        auto parse_num=[](const std::string& tok,double& v){
            char* end=nullptr; v=std::strtod(tok.c_str(),&end);
            return end && end!=tok.c_str() && *end=='\0';
        };
        auto ta=tokenize(left),tb=tokenize(right);
        if(ta.size()!=tb.size()) diff("stdout_numeric");
        else {
            for(size_t i=0;i<ta.size();++i){
                double va=0,vb=0;
                if(parse_num(ta[i],va)&&parse_num(tb[i],vb)){if(!num_close(va,vb)){diff("stdout_numeric");break;}}
                else if(ta[i]!=tb[i]){diff("stdout_numeric");break;}
            }
        }
    } else {
        if(test.value("normalize_crlf",false)) {left=crlf(left);right=crlf(right);}
        if(left!=right) diff("stdout_bytes");
        if(test.contains("expected_stdout") && left!=test["expected_stdout"].get<std::string>()) diff("expected_stdout");
    }
    if(test.value("compare_stderr",true) && l["stderr_sha256"]!=r["stderr_sha256"]) diff("stderr_bytes");
    for(const auto& observer:test.value("observers",Json::array())) {
        auto relative=fs::path(wide(observer.at("path")));
        auto lp=lw/relative,rp=rw/relative;
        if(relative.is_absolute()||!within(lp,lw)||!within(rp,rw)) throw std::runtime_error("Observador debe quedar dentro de {work}");
        if(observer.value("expect","")=="absent") { if(fs::exists(lp)||fs::exists(rp)) diff("expected_absent:"+path_text(relative)); continue; }
        if(!fs::exists(lp)||!fs::exists(rp)) { result["state"]="ERROR";result["differences"].push_back("missing_observer:"+path_text(relative));continue; }
        auto kind=observer.value("kind","files");
        if(kind!="files"&&kind!="sqlite") throw std::runtime_error("Observador desconocido");
        auto a=kind=="sqlite"?sqlite_snapshot(lp):observed_files(lp);
        auto b=kind=="sqlite"?sqlite_snapshot(rp):observed_files(rp);
        // Los objetos no dependen del orden en que el filesystem entregue las entradas.
        if(nlohmann::json::parse(a.dump())!=nlohmann::json::parse(b.dump())) diff(kind+":"+path_text(relative));
        result["observations"].push_back({{"path",path_text(relative)},{"kind",kind},{"left",a},{"right",b}});
    }
    return result;
}
Json run_builds(const Project& p,Log log,Cancel* cancel) {
    auto before=audit_inventory_gate(p,log,cancel); if(!before.value("complete",false)) throw std::runtime_error("Inventario incompleto; no se registrará compilación válida");
    auto run=p.state_dir()/L"builds"/wide(unique_id()); fs::create_directories(run);
    Json result={{"id",run.filename().string()},{"at",now()},{"fingerprints",fingerprints(before)},{"builds",Json::array()}};
    for(const auto& b:p.config.value("builds",Json::array())) {
        if(cancel&&cancel->requested) break;
        if(log) log("Compilación: "+b.at("id").get<std::string>());
        Json receipt={{"id",b["id"]},{"root",b["root"]},{"config_sha256",sha256(b.dump())},{"state","ERROR"}};
        try {
            auto root=p.resolve(root_config(p,b.at("root"))["path"]);
            auto cwd=p.resolve(expand(b.value("cwd",path_text(root)),p,{},path_text(root)));
            auto args=command(b,p); receipt["execution_files"]=execution_files(args,cwd);
            receipt["process"]=run_process(args,cwd,"",run/wide(unique_id()),b.value("timeout_ms",120000u),cancel,b.value("env",Json::object()));
            receipt["artifacts"]=Json::object(); bool artifacts_ok=b.contains("artifacts")&&!b["artifacts"].empty();
            for(const auto& a:b.value("artifacts",Json::array())) {
                auto file=p.resolve(expand(a,p,{},path_text(root)));
                if(!fs::is_regular_file(file)) artifacts_ok=false;
                else receipt["artifacts"][path_text(file)]=hash_file(file);
            }
            if(process_ok(receipt["process"])&&artifacts_ok) receipt["state"]="PASS";
            else receipt["reason"]="Proceso falló o falta artifact explícito";
        } catch(const std::exception& e) {receipt["reason"]=e.what();}
        result["builds"].push_back(receipt);
    }
    auto after=audit_inventory_gate(p,log,cancel); result["stable"]=after.value("complete",false)&&fingerprints(before)==fingerprints(after);
    write_json(run/L"receipt.json",result); write_json(p.state_dir()/L"latest-builds.json",result);
    save_evidence(p,"builds",result);
    event(p,"builds",{{"run",path_text(run)},{"count",result["builds"].size()},{"stable",result["stable"]}});
    return result;
}
bool case_evidence_fresh(const Json& cfg,const Json& previous,bool tests_current) {
    try {
        if(!tests_current||previous.value("config_sha256","")!=sha256(cfg.dump())) return false;
        const bool has_lr=(cfg.contains("left")&&cfg["left"].is_object())||(cfg.contains("right")&&cfg["right"].is_object());
        const std::string kind=cfg.value("kind",has_lr?"differential":"target_test");
        auto proc_fresh=[&](const Json& process)->bool {
            if(!process.is_object()) return false;
            if(!files_fresh(process.value("execution_files",Json::object()))) return false;
            for(const auto* stream:{"stdout","stderr"}) {
                const std::string sha_key=std::string(stream)+"_sha256";
                if(!process.contains(stream)||!process.contains(sha_key)||!process[stream].is_string()||!process[sha_key].is_string()) return false;
                auto sp=fs::path(wide(process[stream].get<std::string>()));
                if(!fs::is_regular_file(sp)||hash_file(sp)!=process[sha_key].get<std::string>()) return false;
            }
            return true;
        };
        if(kind=="target_test") {
            if(!previous.contains("target")||!proc_fresh(previous["target"])) return false;
            if(previous.contains("artifacts")&&previous["artifacts"].is_object()&&!files_fresh(previous["artifacts"])) return false;
            return true;
        }
        for(const auto* side:{"left","right"}) {
            if(!previous.contains(side)||!proc_fresh(previous[side])) return false;
        }
        return true;
    } catch(...) {
        return false;
    }
}
namespace {
bool reusable_case_run(const Json& previous, const Json& current_fingerprints) {
    if (!previous.value("stable", false)) return false;
    const auto recorded = previous.value("fingerprints", Json::object());
    if (!recorded.is_object() || recorded.size() != current_fingerprints.size()) return false;
    for (const auto& [root, digest] : current_fingerprints.items()) {
        const auto it = recorded.find(root);
        if (it == recorded.end() || *it != digest) return false;
    }
    return true;
}
}

Json run_cases(const Project& p, Log log, Cancel* cancel, const std::vector<std::string>& case_ids) {
    const auto configured = p.config.value("cases", Json::array());
    const std::set<std::string> selected(case_ids.begin(), case_ids.end());
    for (const auto& id : selected) {
        if (std::none_of(configured.begin(), configured.end(), [&](const Json& c) { return c.at("id") == id; }))
            throw std::runtime_error("Caso no configurado: " + id);
    }
    auto before=audit_inventory_gate(p,log,cancel); if(!before.value("complete",false)) throw std::runtime_error("Inventario incompleto; revise raíces");
    auto run=p.state_dir()/L"runs"/wide(unique_id()); fs::create_directories(run);
    Json results={{"id",run.filename().string()},{"at",now()},{"fingerprints",fingerprints(before)},{"cases",Json::array()}};
    for(const auto& test:configured) {
        if(cancel&&cancel->requested) break;
        if (!selected.empty() && !selected.contains(test.at("id").get<std::string>())) continue;
        if(log) log("Caso: "+test.at("id").get<std::string>());
        const bool has_lr=(test.contains("left")&&test["left"].is_object())||(test.contains("right")&&test["right"].is_object());
        const std::string kind=test.value("kind",has_lr?"differential":"target_test");
        Json result={{"id",test["id"]},{"kind",kind},{"config_sha256",sha256(test.dump())},{"state","ERROR"}};
        auto case_dir=run/wide(unique_id()); fs::create_directories(case_dir);
        try {
            if(kind=="target_test") {
                const Json& runner=test.contains("target")?test.at("target"):test;
                auto work=case_dir/L"target"; fs::create_directories(work);
                Json fixture_hashes=Json::object();
                for(const auto& fixture:test.value("fixtures",Json::array())) {
                    auto src=p.resolve(fixture.at("source")); auto dst=work/fs::path(wide(fixture.at("destination")));
                    if(!within(dst,work)||!fs::is_regular_file(src)) throw std::runtime_error("Fixture inválido");
                    fs::create_directories(dst.parent_path()); fs::copy_file(src,dst,fs::copy_options::none);
                    fixture_hashes[path_text(src)]=hash_file(src);
                }
                auto root_path=p.resolve(root_config(p,runner.at("root"))["path"]);
                auto exec_cwd=runner.contains("cwd")?p.resolve(expand(runner["cwd"].get<std::string>(),p,work,path_text(root_path))):work;
                auto args=command(runner,p,work); auto inputs=execution_files(args,exec_cwd);
                for(auto it=fixture_hashes.begin();it!=fixture_hashes.end();++it) inputs[it.key()]=it.value();
                const auto stdin_data=test.contains("stdin")?(test["stdin"].is_string()?test["stdin"].get<std::string>():test["stdin"].dump()):"";
                auto proc=run_process(args,exec_cwd,stdin_data,case_dir/L"target-process",test.value("timeout_ms",60000u),cancel,runner.value("env",Json::object()));
                proc["execution_files"]=inputs; proc["inputs_stable"]=files_fresh(inputs);
                result["target"]=proc;
                result["differences"]=Json::array();
                result["artifacts"]=Json::object();
                const int expected=test.value("expected_exit",0);
                if(!process_ok(proc,expected)) {
                    result["state"]="ERROR";
                    result["differences"].push_back("exit_code/timeout/cancel/output_limit");
                } else {
                    std::string out_text=read_text(fs::path(wide(proc.at("stdout"))),128*1024*1024);
                    std::string err_text=read_text(fs::path(wide(proc.at("stderr"))),128*1024*1024);
                    if(test.value("normalize_crlf",false)) { out_text=crlf(out_text); err_text=crlf(err_text); }
                    bool ok=true;
                    if(test.contains("expected_stdout")) {
                        if(test.value("comparator","bytes")=="json") {
                            if(nlohmann::json::parse(out_text)!=nlohmann::json::parse(test["expected_stdout"].dump())) { ok=false; result["differences"].push_back("expected_stdout"); }
                        } else if(out_text!=test["expected_stdout"].get<std::string>()) {
                            ok=false; result["differences"].push_back("expected_stdout");
                        }
                    }
                    for(const auto& sub:test.value("expected_stdout_contains",Json::array())) {
                        const auto needle=sub.get<std::string>();
                        if(out_text.find(needle)==std::string::npos) { ok=false; result["differences"].push_back("missing_stdout:"+needle); }
                    }
                    for(const auto& sub:test.value("forbidden_stdout_contains",Json::array())) {
                        const auto needle=sub.get<std::string>();
                        if(!needle.empty()&&out_text.find(needle)!=std::string::npos) { ok=false; result["differences"].push_back("forbidden_stdout:"+needle); }
                    }
                    for(const auto& sub:test.value("expected_stderr_contains",Json::array())) {
                        const auto needle=sub.get<std::string>();
                        if(err_text.find(needle)==std::string::npos) { ok=false; result["differences"].push_back("missing_stderr:"+needle); }
                    }
                    for(const auto& req_art:test.value("required_artifacts",Json::array())) {
                        auto art_file=p.resolve(expand(req_art.get<std::string>(),p,work,path_text(root_path)));
                        if(!fs::is_regular_file(art_file)) { ok=false; result["differences"].push_back("missing_artifact:"+path_text(art_file)); }
                        else result["artifacts"][path_text(art_file)]=hash_file(art_file);
                    }
                    result["state"]=ok?"PASS":"DIFFERENT";
                }
                if(proc["inputs_stable"]==false) { result["state"]="ERROR"; result["reason"]="Ejecutable o fixture cambió durante prueba"; }
            } else {
                std::vector<fs::path> works; Json executed=Json::array();
                for(const auto* side:{"left","right"}) {
                    auto work=case_dir/wide(side); fs::create_directories(work); works.push_back(work);
                    Json fixture_hashes=Json::object();
                    for(const auto& fixture:test.value("fixtures",Json::array())) {
                        auto src=p.resolve(fixture.at("source")); auto dst=work/fs::path(wide(fixture.at("destination")));
                        if(!within(dst,work)||!fs::is_regular_file(src)) throw std::runtime_error("Fixture inválido");
                        fs::create_directories(dst.parent_path()); fs::copy_file(src,dst,fs::copy_options::none);
                        fixture_hashes[path_text(src)]=hash_file(src);
                    }
                    auto args=command(test[side],p,work); auto inputs=execution_files(args,work);
                    for(auto it=fixture_hashes.begin();it!=fixture_hashes.end();++it) inputs[it.key()]=it.value();
                    const auto stdin_data=test.contains("stdin")?(test["stdin"].is_string()?test["stdin"].get<std::string>():test["stdin"].dump()):"";
                    auto proc=run_process(args,work,stdin_data,case_dir/(wide(side)+L"-process"),test.value("timeout_ms",30000u),cancel,test[side].value("env",Json::object()));
                    proc["execution_files"]=inputs; proc["inputs_stable"]=files_fresh(inputs);
                    executed.push_back(proc); result[side]=proc;
                }
                auto compared=compare_outputs(test,executed[0],executed[1],works[0],works[1]);
                for(auto it=compared.begin();it!=compared.end();++it) result[it.key()]=it.value();
                if(executed[0]["inputs_stable"]==false||executed[1]["inputs_stable"]==false) {result["state"]="ERROR";result["reason"]="Ejecutable o fixture cambió durante prueba";}
            }
        } catch(const std::exception& e) {result["state"]="ERROR";result["reason"]=e.what();}
        write_json(case_dir/L"receipt.json",result); result["receipt_path"]=path_text(case_dir/L"receipt.json");
        results["cases"].push_back(result);
        if(log) log(test.at("id").get<std::string>()+": "+result["state"].get<std::string>());
    }
    auto after=audit_inventory_gate(p,log,cancel); results["stable"]=after.value("complete",false)&&fingerprints(before)==fingerprints(after);
    if (!selected.empty() && results["cases"].size() != selected.size()) results["stable"] = false;
    write_json(run / L"receipt.json", results);
    if (!selected.empty()) results["reused_cases"] = Json::array();
    if (selected.empty() || results["stable"] == true) {
        save_evidence(p, "tests", results, [&](Database& db, Json& stored) {
            if (!selected.empty()) {
                const auto latest = db.query("SELECT data FROM evidence WHERE kind='tests' ORDER BY revision DESC LIMIT 1");
                if (!latest.empty()) {
                    const auto previous = Json::parse(latest.front().at("data").get<std::string>());
                    if (reusable_case_run(previous, stored.at("fingerprints"))) {
                        std::map<std::string, const Json*> previous_cases;
                        for (const auto& prior : previous.at("cases"))
                            previous_cases.emplace(prior.at("id").get<std::string>(), &prior);
                        for (const auto& cfg : configured) {
                            const auto id = cfg.at("id").get<std::string>();
                            if (selected.contains(id)) continue;
                            const auto prior = previous_cases.find(id);
                            if (prior == previous_cases.end() || !case_evidence_fresh(cfg, *prior->second, true)) continue;
                            stored["cases"].push_back(*prior->second);
                            results["reused_cases"].push_back(id);
                        }
                    }
                }
                stored["reused_cases"] = results["reused_cases"];
            }
            write_json(p.state_dir() / L"latest-tests.json", stored);
        });
    }
    event(p,"differential_tests",{{"run",path_text(run)},{"count",results["cases"].size()},{"stable",results["stable"]}});
    return results;
}
Json import_coverage(const Project& p,const fs::path& path,const std::string& root_id,const std::string& format,Log log) {
    auto base=p.resolve(root_config(p,root_id)["path"]); auto snap=audit_inventory_gate(p,log,nullptr);
    if(!snap.value("complete",false)) throw std::runtime_error("Inventario incompleto");
    Json normalized=Json::array();
    auto add=[&](Json f) {
        fs::path actual(wide(f.at("path"))); if(!actual.is_absolute()) actual=base/actual; actual=fs::weakly_canonical(actual);
        if(!within(actual,base)||!fs::is_regular_file(actual)) throw std::runtime_error("Cobertura de archivo fuera de raíz o inexistente: "+path_text(actual));
        f["path"]=path_text(actual); f["sha256"]=hash_file(actual);
        f["uncovered_lines"]=Json::array();f["branch_gaps"]=Json::array();
        const auto line_hits=f.value("line_hits",Json::object());for(auto it=line_hits.begin();it!=line_hits.end();++it)if(it.value()==false)f["uncovered_lines"].push_back(std::stoi(it.key()));
        const auto branch_hits=f.value("branch_hits",Json::object());for(auto it=branch_hits.begin();it!=branch_hits.end();++it)if(it.value()["covered"]!=it.value()["count"])f["branch_gaps"].push_back({{"line",std::stoi(it.key())},{"covered",it.value()["covered"]},{"count",it.value()["count"]}});
        for(const auto& branch:f.value("llvm_branches",Json::array()))if(branch.size()>=6&&(branch[4]==0||branch[5]==0))f["branch_gaps"].push_back({{"line",branch[0]},{"true_count",branch[4]},{"false_count",branch[5]}});
        for(const auto* unit:{"lines","branches","functions"}) {
            if(!f.contains(unit)) {f[unit]=nullptr;continue;}
            if(f[unit].is_null()) continue;
            auto count=f[unit].at("count").get<int64_t>(),covered=f[unit].at("covered").get<int64_t>();
            if(count<0||covered<0||covered>count) throw std::runtime_error("Métricas de cobertura inválidas");
            f[unit]["percent"]=count?Json(100.0*static_cast<double>(covered)/static_cast<double>(count)):Json(nullptr);
        }
        normalized.push_back(f);
    };
    if(format=="llvm") {
        auto raw=read_json(path);
        if(raw.value("type","")!="llvm.coverage.json.export") throw std::runtime_error("No es llvm-cov export JSON");
        for(const auto& data:raw.at("data")) for(const auto& f:data.at("files")) {
            Json n={{"path",f.at("filename")}};
            for(const auto* unit:{"lines","branches","functions"}) if(f.at("summary").contains(unit)) {
                auto v=f["summary"][unit]; n[unit]={{"count",v.at("count")},{"covered",v.at("covered")}};
            }
            if(f.contains("segments")) n["segments"]=f["segments"];
            if(f.contains("branches"))n["llvm_branches"]=f["branches"];
            add(n);
        }
    } else if(format=="generic") {
        auto raw=read_json(path); if(raw.value("schema","")!="migratool.coverage/1") throw std::runtime_error("Esquema de cobertura inválido");
        for(const auto& f:raw.at("files")) add(f);
    } else if(format=="cobertura") {
        IStream* stream{}; IXmlReader* reader{};
        if(FAILED(SHCreateStreamOnFileEx(path.c_str(),STGM_READ|STGM_SHARE_DENY_WRITE,FILE_ATTRIBUTE_NORMAL,FALSE,nullptr,&stream))) throw std::runtime_error("No se pudo abrir XML");
        struct XMLClose{IStream*& s;IXmlReader*& r;~XMLClose(){if(r)r->Release();if(s)s->Release();}} closer{stream,reader};
        if(FAILED(CreateXmlReader(__uuidof(IXmlReader),reinterpret_cast<void**>(&reader),nullptr))||FAILED(reader->SetInput(stream))) throw std::runtime_error("No se pudo inicializar XmlLite");
        reader->SetProperty(XmlReaderProperty_DtdProcessing,DtdProcessing_Prohibit);
        auto attr=[&](const wchar_t* key) { const wchar_t* value{}; std::string out; if(reader->MoveToAttributeByName(key,nullptr)==S_OK) {reader->GetValue(&value,nullptr);out=utf8(value);reader->MoveToElement();}return out;};
        std::map<std::string,std::map<int,std::pair<bool,std::pair<int,int>>>> files;
        std::string current; XmlNodeType type; HRESULT hr;
        while((hr=reader->Read(&type))==S_OK) {
            const wchar_t* name{};reader->GetLocalName(&name,nullptr);
            if(type==XmlNodeType_Element&&wcscmp(name,L"class")==0) current=attr(L"filename");
            else if(type==XmlNodeType_EndElement&&wcscmp(name,L"class")==0) current.clear();
            else if(type==XmlNodeType_Element&&wcscmp(name,L"line")==0&&!current.empty()) {
                auto number=attr(L"number"),hits=attr(L"hits"); if(number.empty()||hits.empty()) throw std::runtime_error("Línea de Cobertura sin número/hits");
                int covered=0,total=0;auto condition=attr(L"condition-coverage"); auto start=condition.find('(');
                if(start!=std::string::npos) { auto counts=condition.substr(start+1); if(sscanf_s(counts.c_str(),"%d/%d",&covered,&total)!=2) throw std::runtime_error("condition-coverage inválido"); }
                auto& line=files[current][std::stoi(number)]; line.first=line.first||std::stoll(hits)>0;
                line.second.first=std::max(line.second.first,covered);line.second.second=std::max(line.second.second,total);
            }
        }
        if(FAILED(hr)) throw std::runtime_error("XML inválido");
        for(const auto& [name,lines]:files) {
            int covered=0,branches=0,bc=0;Json hits=Json::object(),branch_hits=Json::object();
            for(const auto& [n,v]:lines) {covered+=v.first;branches+=v.second.second;bc+=v.second.first;hits[std::to_string(n)]=v.first;if(v.second.second>0)branch_hits[std::to_string(n)]={{"count",v.second.second},{"covered",v.second.first}};}
            add({{"path",name},{"lines",{{"count",lines.size()},{"covered",covered}}},{"branches",{{"count",branches},{"covered",bc}}},{"line_hits",hits},{"branch_hits",branch_hits}});
        }
    } else throw std::runtime_error("Formato de cobertura: llvm, cobertura o generic");
    if(normalized.empty()) throw std::runtime_error("Informe de cobertura sin archivos válidos");
    auto run_id=unique_id();
    Json coverage={{"id",run_id},{"at",now()},{"root",root_id},{"input_path",path_text(fs::absolute(path))},{"input_sha256",hash_file(path)},
        {"format",format},{"fingerprints",fingerprints(snap)},{"files",normalized},
        {"provenance","IMPORTED_REPORT_NOT_INDEPENDENT_EXECUTION"}};
    write_json(p.state_dir()/L"coverage"/wide(run_id+".json"),coverage);
    write_json(p.state_dir()/wide("coverage-"+root_id+".json"),coverage);
    save_evidence(p,"coverage",coverage);
    event(p,"coverage_imported",{{"root",root_id},{"input",path_text(fs::absolute(path))},{"report_id",run_id},{"files",normalized.size()}});
    if(log) log("Cobertura importada: "+std::to_string(normalized.size())+" archivos; origen declarado por informe");
    return coverage;
}
void add_mapping(Project& p,const std::string& source,const std::vector<std::string>& targets,const std::vector<std::string>& cases,const std::string& note,bool complete,const std::string& actor) {
    indexed_mapping(p,source,targets,cases,complete,note,actor);
}
[[maybe_unused]] static void legacy_add_mapping(Project& p,const std::string& source,const std::vector<std::string>& targets,const std::vector<std::string>& cases,const std::string& note,bool complete,const std::string& actor) {
    if(targets.empty()) throw std::runtime_error("Se requiere al menos un destino");
    auto s=load_scan(p); auto src=find_symbol(s,source);
    if(!src) throw std::runtime_error("Unidad de origen no inventariada: "+source);
    auto sf=find_file(s,src->at("file_id"));
    if(!sf||sf->at("role")!="source") throw std::runtime_error("Unidad no pertenece a un origen");
    Json m={{"id","map-"+sha256(source).substr(0,20)},{"source",source},{"source_path",windows_path(fs::path(wide(sf->at("absolute_path")) ))},
        {"targets",targets},{"target_paths",Json::array()},{"cases",cases},{"reviewed",true},{"declared_complete",complete},
        {"actor",actor},{"note",note},{"at",now()},{"hashes",Json::object()}};
    m["hashes"][sf->at("absolute_path").get<std::string>()]=hash_file(fs::path(wide(sf->at("absolute_path"))));
    for(const auto& target:targets) {
        auto t=find_symbol(s,target); if(!t) throw std::runtime_error("Unidad destino no inventariada: "+target);
        auto tf=find_file(s,t->at("file_id")); if(!tf||tf->at("role")!="target") throw std::runtime_error("Unidad no pertenece a un destino");
        auto path=fs::path(wide(tf->at("absolute_path"))); m["target_paths"].push_back(windows_path(path));
        m["hashes"][path_text(path)]=hash_file(path);
    }
    for(const auto& case_id:cases) {
        bool found=false; for(const auto& t:p.config.value("cases",Json::array())) if(t["id"]==case_id) found=true;
        if(!found) throw std::runtime_error("Caso inexistente: "+case_id);
    }
    bool replaced=false;
    for(auto& existing:p.config["mappings"]) if(existing["source"]==source) { existing=m;replaced=true;break; }
    if(!replaced) p.config["mappings"].push_back(m);
    p.save(); event(p,complete?"unit_declared_complete":"mapping_reviewed",m,actor);
}
Json mark_file(Project& p,const fs::path& source,const std::vector<fs::path>& targets,bool complete,const std::string& note,const std::string& actor,const std::vector<std::string>& cases,Cancel* cancel,const std::string& stage,const std::string& disposition,const std::string& external_replacement) {
    const auto canonical_file=[](const fs::path& path){
        if(!path.is_absolute()||!fs::is_regular_file(path))throw std::runtime_error("Elija la ruta completa de un archivo existente: "+path_text(path));
        return fs::canonical(path);
    };
    const auto original=canonical_file(source);
    std::vector<fs::path> destinations;
    for(const auto& path:targets)destinations.push_back(canonical_file(path));
    const std::string eff_disp=disposition.empty()?std::string("migrated"):disposition;
    const bool targetless_allowed=(eff_disp=="intentionally_removed"||eff_disp=="replaced_by_external");
    if(targetless_allowed&&note.empty())throw std::runtime_error("La disposición '"+eff_disp+"' exige justificación explícita en note");
    if(!targetless_allowed&&destinations.empty())throw std::runtime_error("Elija al menos un archivo de destino");
    Database db(p);
    if(!scale_enabled(p,db)){
        index_sync(p,false,{},cancel);
        std::vector<std::string> paths;
        for(const auto& path:destinations)paths.push_back(path_text(path));
        return indexed_mapping(p,path_text(original),paths,cases,complete,note,actor,stage,eff_disp,external_replacement);
    }

    struct Selection {std::string root,path;};
    const auto locate=[&](const fs::path& file,std::string_view role){
        const Json* chosen=nullptr;
        fs::path base;
        for(const auto& root:p.config.at("roots")){
            if(root.at("role").get_ref<const std::string&>()!=role)continue;
            const auto candidate=fs::weakly_canonical(p.resolve(root.at("path")));
            if(candidate.native().size()>=base.native().size()&&within(file,candidate)){chosen=&root;base=candidate;}
        }
        if(!chosen)throw std::runtime_error("El archivo está fuera de las carpetas de "+std::string(role=="source"?"origen: ":"destino: ")+path_text(file));
        auto part=file.begin();
        std::advance(part,std::distance(base.begin(),base.end()));
        fs::path relative;
        for(;part!=file.end();++part)relative/=*part;
        return Selection{chosen->at("id"),path_text(relative)};
    };
    const auto origin=locate(original,"source");
    std::vector<Selection> output;
    for(const auto& path:destinations)output.push_back(locate(path,"target"));
    const auto identifier=[&](const Selection& entry){
        const auto rows=db.query("SELECT id FROM files WHERE root=? AND path=? AND deleted=0 AND sha256 IS NOT NULL",{entry.root,entry.path});
        return rows.empty()?std::string():"F"+rows.front().at("id").dump();
    };
    const auto save_mapping=[&](){
        auto origin_id=identifier(origin);
        std::vector<std::string> target_ids;
        for(const auto& item:output)target_ids.push_back(identifier(item));
        if(origin_id.empty()||std::find(target_ids.begin(),target_ids.end(),"")!=target_ids.end())throw std::runtime_error("INDEX_STALE: falta comprobar uno de los archivos");
        return indexed_mapping(p,origin_id,target_ids,cases,complete,note,actor,stage,eff_disp,external_replacement);
    };
    try{return save_mapping();}
    catch(const std::exception& error){if(std::string_view(error.what()).find("INDEX_STALE")==std::string_view::npos)throw;}

    std::map<std::string,std::set<std::string>> selected;
    selected[origin.root].insert(origin.path);
    for(const auto& item:output)selected[item.root].insert(item.path);
    for(const auto& [root,paths]:selected){
        const auto result=scale_run(p,{{"actor",actor.empty()?"local-user":actor},{"scope",{{"root",root},{"files",paths}}}}, {},cancel);
        if(result.at("state")!="completed")throw std::runtime_error("No se pudo comprobar uno de los archivos. Revise el trabajo "+result.at("id").get<std::string>());
    }
    return save_mapping();
}
[[maybe_unused]] static Json legacy_mark_file(Project& p,const fs::path& source,const std::vector<fs::path>& targets,bool complete,const std::string& note,const std::string& actor,const std::vector<std::string>& cases) {
    auto s=load_scan(p);
    auto identify=[&](const fs::path& path,const std::string& role)->std::string {
        auto absolute=fs::canonical(path); if(!fs::is_regular_file(absolute)) throw std::runtime_error("Debe marcar un archivo existente");
        for(const auto& f:s["files"]) if(f["role"]==role && fs::equivalent(absolute,fs::path(wide(f["absolute_path"])))) return f["id"];
        throw std::runtime_error("Archivo fuera del inventario con rol "+role+": "+path_text(absolute));
    };
    auto src=identify(source,"source");std::vector<std::string> target_ids;
    for(const auto& t:targets) target_ids.push_back(identify(t,"target"));
    add_mapping(p,src,target_ids,cases,note);
    Json marked;
    for(auto& m:p.config["mappings"]) if(m["source"]==src) {
        m["declared_complete"]=complete;m["actor"]=actor;m["at"]=now();marked=m;break;
    }
    p.save();event(p,complete?"file_declared_complete":"file_marked_partial",marked,actor);return marked;
}
Json status(const Project& p,bool check_fresh) {
    {
        Database db(p);
        if(scale_enabled(p,db)) {
            if(check_fresh) scale_fingerprints(db);
            return scale_summary(p,db);
        }
    }
    return indexed_status(p,check_fresh);
}
[[maybe_unused]] static Json legacy_status(const Project& p,bool check_fresh) {
    auto s=load_scan(p); auto fresh=check_fresh?freshness(p,s):Json{{"fresh",false},{"checked",false}};
    fresh["checked"]=check_fresh;
    const auto fp=fingerprints(s);
    auto builds=maybe(p.state_dir()/L"latest-builds.json",{{"builds",Json::array()}});
    auto tests=maybe(p.state_dir()/L"latest-tests.json",{{"cases",Json::array()}});
    auto now_roots=fresh.value("roots",Json::array());
    bool run_fresh=fresh.value("fresh",false)&&tests.value("stable",false)&&tests.value("fingerprints",Json::object())==fp;
    Json case_states=Json::array(); std::map<std::string,bool> passed;
    for(const auto& t:p.config.value("cases",Json::array())) {
        const bool has_lr=(t.contains("left")&&t["left"].is_object())||(t.contains("right")&&t["right"].is_object());
        const std::string kind=t.value("kind",has_lr?"differential":"target_test");
        Json st={{"id",t["id"]},{"kind",kind},{"state","NOT_RUN"},{"fresh",false}};
        for(const auto& old:tests["cases"]) if(old["id"]==t["id"]) {
            st=old;
            if(!st.contains("kind")) st["kind"]=kind;
            const bool current=case_evidence_fresh(t,old,run_fresh);
            st["fresh"]=current;if(!current) st["state"]="STALE";break;
        }
        passed[t["id"]]=st["state"]=="PASS"&&st["fresh"]==true;case_states.push_back(st);
    }
    std::map<std::string,bool> built;
    std::set<std::string> configured_build_roots;
    Json build_states=Json::array();
    for(const auto& cfg:p.config.value("builds",Json::array())) {
        Json st={{"id",cfg["id"]},{"root",cfg["root"]},{"state","NOT_RUN"},{"fresh",false}};
        for(const auto& b:builds["builds"]) if(b["id"]==cfg["id"]) {
            st=b; bool current=fresh.value("fresh",false)&&builds.value("stable",false)&&builds.value("fingerprints",Json::object())==fp&&b.value("config_sha256","")==sha256(cfg.dump());
            current=current&&files_fresh(b.value("artifacts",Json::object()));
            st["fresh"]=current;if(!current)st["state"]="STALE";break;
        }
        auto root=cfg["root"].get<std::string>();
        configured_build_roots.insert(root);
        if(!built.contains(root)) built[root]=true;
        built[root]=built[root]&&(st["state"]=="PASS"&&st["fresh"]==true);build_states.push_back(st);
    }
    bool any_built=false;
    for(const auto& [r_id,r_ok]:built) if(r_ok) any_built=true;
    Json coverage=Json::array();std::map<std::string,bool> file_coverage;
    for(const auto& root:s["roots"]) {
        auto path=p.state_dir()/wide("coverage-"+root.at("id").get<std::string>()+".json");
        if(!fs::exists(path)) continue;auto c=read_json(path);
        bool current=fresh.value("fresh",false)&&c["fingerprints"]==fp;
        auto input=fs::path(wide(c["input_path"]));current=current&&fs::is_regular_file(input)&&hash_file(input)==c["input_sha256"].get<std::string>();
        for(const auto& f:c["files"]) {
            bool full=current;auto src=fs::path(wide(f["path"]));full=full&&fs::is_regular_file(src)&&hash_file(src)==f["sha256"].get<std::string>();
            for(const auto* unit:{"lines","branches"}) {
                if(!f.contains(unit)||f[unit].is_null()) {full=false;continue;}
                auto count=f[unit]["count"].get<int64_t>();if(count!=f[unit]["covered"].get<int64_t>() || (std::string(unit)=="lines"&&count==0))full=false;
            }
            file_coverage[f["path"]]=full;
        }
        c["fresh"]=current;coverage.push_back(c);
    }
    Json mappings=Json::array();std::map<std::string,std::vector<Json>> by_source_file;
    for(const auto& map:p.config.value("mappings",Json::array())) {
        Json m=map;auto src=find_symbol(s,map.at("source"));bool current=src&&fresh.value("fresh",false)&&files_fresh(map.value("hashes",Json::object()));
        const bool src_build_req=src&&configured_build_roots.contains(src->at("root").get<std::string>());
        bool builds_ok=src&&any_built&&(!src_build_req||built[src->at("root")]);
        bool coverage_ok=src&&file_coverage[src->at("absolute_path")];
        for(const auto& id:map.at("targets")) {
            auto t=find_symbol(s,id);if(!t){current=false;builds_ok=false;coverage_ok=false;continue;}
            builds_ok=builds_ok&&built[t->at("root")];coverage_ok=coverage_ok&&file_coverage[t->at("absolute_path")];
        }
        bool tests_ok=!map.value("cases",Json::array()).empty();
        for(const auto& id:map.value("cases",Json::array())) tests_ok=tests_ok&&passed[id];
        m["fresh"]=current;m["builds_pass"]=builds_ok;m["cases_pass"]=tests_ok;m["coverage_full_in_report"]=coverage_ok;
        m["verified_in_configured_cases"]=current&&map.value("reviewed",false)&&tests_ok&&builds_ok;
        m["state"]=!current?"STALE":m["verified_in_configured_cases"]==true?"VERIFIED_IN_CASES":map.value("declared_complete",false)?"DECLARED_COMPLETE_UNVERIFIED":"MAPPED_UNVERIFIED";
        mappings.push_back(m);if(src)by_source_file[src->at("file_id")].push_back(m);
    }
    Json files=Json::array();size_t mapped_count=0,declared_count=0,verified_count=0,total_units=0,complete_units=0,case_units=0;
    for(const auto& f:s["files"]) if(f["role"]=="source") {
        Json row={{"id",f["id"]},{"source_path",f["absolute_path"]},{"language",f["language"]},{"analysis",f["analysis"]},
            {"generated",f.value("generated",false)},{"target_paths",Json::array()},{"state","UNMAPPED"},{"declared_complete",false},
            {"verified_in_configured_cases",false},{"missing",Json::array()}};
        bool file_map=false;
        for(const auto& m:by_source_file[f["id"]]) {
            if(m["source"]==f["id"]) {
                row["state"]=m["state"];row["target_paths"]=m["target_paths"];row["declared_complete"]=m.value("declared_complete",false)&&m["fresh"]==true;
                row["verified_in_configured_cases"]=m["verified_in_configured_cases"];row["note"]=m.value("note","");row["at"]=m["at"];row["actor"]=m.value("actor","");
                if(!m.value("builds_pass",false))row["missing"].push_back("current_builds");
                if(!m.value("cases_pass",false))row["missing"].push_back("linked_passing_cases");
                if(!m.value("coverage_full_in_report",false))row["missing"].push_back("complete_coverage_report");
                if(!m.value("fresh",false))row["missing"].push_back("current_file_hashes");
                file_map=true;break;
            }
        }
        if(!file_map) {row["missing"].push_back("file_mapping");if(!by_source_file[f["id"]].empty())row["state"]="SYMBOLS_MAPPED_ONLY";}
        std::vector<std::string> units;
        for(const auto& sym:s["symbols"])if(sym["file_id"]==f["id"]&&sym["kind"]!="file")units.push_back(sym["id"]);
        const bool fallback=units.empty();if(fallback)units.push_back(f["id"]);
        size_t done=0,with_cases=0;
        for(const auto& unit:units) {
            bool migrated=row.value("declared_complete",false),has_cases=false;
            for(const auto& m:by_source_file[f["id"]])if(m["source"]==unit) {
                migrated=migrated||(m.value("declared_complete",false)&&m["fresh"]==true);
                has_cases=has_cases||m.value("verified_in_configured_cases",false);
            }
            done+=migrated;with_cases+=has_cases;
        }
        row["total_units"]=units.size();row["migrated_units"]=done;row["units_with_passing_cases"]=with_cases;
        row["granularity"]=fallback?"file":"symbols";
        row["migration_percent"]=fresh.value("fresh",false)?Json(100.0*static_cast<double>(done)/static_cast<double>(units.size())):Json(nullptr);
        row["units_with_passing_cases_percent"]=fresh.value("fresh",false)?Json(100.0*static_cast<double>(with_cases)/static_cast<double>(units.size())):Json(nullptr);
        row["percentage_basis"]=fallback?"Declaración a nivel de archivo; no hay desglose de símbolos":"Unidades inventariadas declaradas completas; una marca completa de archivo incluye sus unidades";
        total_units+=units.size();complete_units+=done;case_units+=with_cases;
        if(!row.value("declared_complete",false))row["missing"].push_back("completion_declaration");
        if(f["analysis"]!="ANALYZED")row["missing"].push_back("complete_symbol_analysis");
        if(file_map)++mapped_count;if(row["declared_complete"]==true)++declared_count;if(row["verified_in_configured_cases"]==true)++verified_count;
        files.push_back(row);
    }
    size_t pass=0;for(const auto& [id,ok]:passed)if(ok)++pass;
    return {{"schema","migratool.status/1"},{"name",p.config.at("name")},{"project_path",path_text(p.file)},{"at",now()},
        {"freshness",fresh},{"inventory_complete",s["complete"]},{"source_files",files.size()},{"mapped_source_files",mapped_count},
        {"declared_complete_files",declared_count},{"files_verified_in_cases",verified_count},{"configured_cases",case_states.size()},{"passing_current_cases",pass},
        {"total_units",total_units},{"migrated_units",complete_units},{"units_with_passing_cases",case_units},
        {"migration_percent",fresh.value("fresh",false)&&total_units?Json(100.0*static_cast<double>(complete_units)/static_cast<double>(total_units)):Json(nullptr)},
        {"units_with_passing_cases_percent",fresh.value("fresh",false)&&total_units?Json(100.0*static_cast<double>(case_units)/static_cast<double>(total_units)):Json(nullptr)},
        {"percentage_basis","Unidades de símbolos; fallback de una unidad por archivo sin símbolos; ponderado por unidades. Declaraciones vigentes de agentes, no equivalencia universal."},
        {"functional_parity_percent",nullptr},{"universal_equivalence_proven",false},
        {"files",files},{"mappings",mappings},{"cases",case_states},{"builds",build_states},{"coverage",coverage},
        {"diagnostics",s["diagnostics"]},{"exclusions",s["exclusions"]},
        {"limits",{"Los resultados cubren solamente entradas y observadores configurados.","Una declaración de un agente no acredita paridad por sí sola.",
            "La cobertura importada es evidencia externa; no demuestra que los casos diferenciales la produjeron.","Las declaraciones y referencias de Roslyn son sintácticas; no hay resolución semántica completa del proyecto.",
            "El inventario excluye únicamente directorios configurados y enlaces; los archivos generados se identifican sin eliminarlos.","La paridad visual, dispositivos y protocolos requieren casos y observadores específicos."}}};
}
Json pending(const Project& p,bool check_fresh) {
    auto st=status(p,check_fresh);Json list=Json::array();
    for(const auto& f:st["files"]) if(!f["missing"].empty())list.push_back(f);
    return {{"schema","migratool.pending/1"},{"project_path",path_text(p.file)},{"at",now()},{"freshness",st["freshness"]},
            {"count",list.size()},{"files",list},{"migration_percent",st["migration_percent"]},{"total_units",st["total_units"]},{"migrated_units",st["migrated_units"]},{"functional_parity_percent",nullptr}};
}
void report(const Project& p,const fs::path& output,bool check_fresh) {
    bool use_scale=false;
    {
        Database db(p);
        use_scale=scale_enabled(p,db);
    }
    Json st,s;
    bool truncated_report=false;
    if(use_scale) {
        Database db(p);
        if(check_fresh) scale_fingerprints(db);
        st=scale_summary(p,db);
        auto fpage=scale_file_status(p,db,{{"limit",1000},{"include_total",false}});
        st["files"]=fpage.value("items",Json::array());
        st["mapped_source_files"]=db.integer("SELECT COUNT(DISTINCT CASE WHEN substr(m.source,1,1)='F' THEN m.source ELSE 'F'||s.file_id END) FROM mappings m LEFT JOIN symbols s ON s.id=CASE WHEN substr(m.source,1,1)='S' THEN CAST(substr(m.source,2) AS INTEGER) END");
        if(auto cov=scale_query(p,db,{{"kind","coverage"},{"limit",500}})) {
            st["coverage"]=Json::array({{{"fresh",st["freshness"].value("fresh",false)},{"files",cov->value("items",Json::array())}}});
        }
        st["at"]=st.value("last_sync",now());
        s={{"symbols",Json::array()},{"dependencies",Json::array()}};
        truncated_report=true;
    } else {
        st=status(p,check_fresh);
        s=load_scan(p);
    }
    {
        Database db(p);
        if(!st.contains("development_control")||!st["development_control"].is_object()) {
            const int64_t doc_total=db.integer("SELECT COUNT(*) FROM files f LEFT JOIN roots r ON r.id=f.root WHERE f.deleted=0 AND r.role='source' AND f.category='documentation'");
            const int64_t doc_reviewed=db.integer("SELECT COUNT(*) FROM doc_audits d JOIN files f ON f.id=CASE WHEN substr(d.doc_id,1,1)='F' THEN CAST(substr(d.doc_id,2) AS INTEGER) END WHERE f.deleted=0 AND d.state<>'UNREVIEWED' AND d.sha256=f.sha256");
            const int64_t doc_stale=db.integer("SELECT COUNT(*) FROM doc_audits d JOIN files f ON f.id=CASE WHEN substr(d.doc_id,1,1)='F' THEN CAST(substr(d.doc_id,2) AS INTEGER) END WHERE f.deleted=0 AND d.sha256<>f.sha256");
            const int64_t phases_total=db.integer("SELECT COUNT(*) FROM project_phases");
            const int64_t phases_done=db.integer("SELECT COUNT(*) FROM project_phases WHERE state IN('COMPLETED','VERIFIED','DONE')");
            const int64_t tasks_total=db.integer("SELECT COUNT(*) FROM project_tasks");
            const int64_t tasks_done=db.integer("SELECT COUNT(*) FROM project_tasks WHERE state IN('COMPLETED','VERIFIED','DONE')");
            const int64_t tasks_half=db.integer("SELECT COUNT(*) FROM project_tasks WHERE state='HALF_DONE'");
            const int64_t tasks_active=db.integer("SELECT COUNT(*) FROM project_tasks WHERE state='IN_PROGRESS'");
            st["development_control"]={
                {"phases_total",phases_total},{"phases_completed",phases_done},
                {"tasks_total",tasks_total},{"tasks_completed",tasks_done},{"tasks_half_done",tasks_half},{"tasks_in_progress",tasks_active},
                {"documentation_files",doc_total},{"documentation_reviewed",doc_reviewed},{"documentation_stale",doc_stale}
            };
        }
        auto ph=scale_query(p,db,{{"kind","phases"},{"limit",1000}});
        auto tk=scale_query(p,db,{{"kind","project_tasks"},{"limit",1000}});
        auto dc=scale_query(p,db,{{"kind","doc_audits"},{"limit",1000}});
        st["phases"]=ph?ph->value("items",Json::array()):Json::array();
        st["project_tasks"]=tk?tk->value("items",Json::array()):Json::array();
        st["doc_audits"]=dc?dc->value("items",Json::array()):Json::array();
        st["module_matrix"]=scale_module_matrix(p,db,{{"limit",1000}}).value("items",Json::array());
        try {
            workstream_schema(db);
            knowledge_schema(db);
            docs_hub_schema(db);
            runs_telemetry_schema(db);
            Json git_s = git_repo_summary_cached(p, db);
            auto last_scan = db.query("SELECT COALESCE(MAX(last_full_scan_at),'') AS ts FROM fs_roots");
            const int64_t ws_open = db.integer("SELECT COUNT(*) FROM agent_workstreams WHERE status IN('active','paused','handed_off','interrupted_recoverable')");
            const int64_t ws_interrupted = db.integer("SELECT COUNT(*) FROM agent_workstreams WHERE status='interrupted_recoverable'");
            const int64_t kn_total = db.integer("SELECT COUNT(*) FROM knowledge_entries");
            const int64_t docs_total = db.integer("SELECT COUNT(*) FROM project_docs");
            const int64_t tok_saved = db.integer("SELECT COALESCE(SUM(tokens_saved_est),0) FROM efficiency_telemetry");
            const int64_t calls_saved = db.integer("SELECT COALESCE(SUM(native_calls_saved),0) FROM efficiency_telemetry");
            const int64_t sha_skipped = db.integer("SELECT COALESCE(SUM(files_skipped_by_sha256),0) FROM efficiency_telemetry");
            st["agent_os"] = {
                {"git_branch", git_s.value("branch", "")},
                {"git_head", git_s.value("head_commit", "")},
                {"git_dirty", git_s.value("dirty", false)},
                {"last_full_tree_scan_at", last_scan.empty() ? "" : last_scan[0].value("ts", "")},
                {"workstreams_open", ws_open},
                {"workstreams_interrupted", ws_interrupted},
                {"knowledge_entries", kn_total},
                {"docs_hub_indexed", docs_total},
                {"BM2_tokens_saved_est", tok_saved},
                {"BM3_native_calls_saved", calls_saved},
                {"BM5_files_skipped_sha256", sha_skipped}
            };
        } catch (...) {}
    }
    const auto dc_obj=st.value("development_control",Json::object());
    std::ostringstream out;
    out<<"<!doctype html><html lang=es><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'><title>Migratool · "<<html(p.config["name"])<<"</title>"
       <<"<style>"
       <<":root{--bg:#f5f7fa;--surface:#ffffff;--surface-alt:#f8fafd;--th:#eef2f7;--text:#111827;--muted:#445265;--accent:#0056b3;--border:#a2b0c2;--divider:#cbd5e1;--ok-bg:#ecfdf3;--ok-fg:#0e6235;--warn-bg:#fef4db;--warn-fg:#8c3a0a;--badge-bg:#eef6ff;--badge-fg:#004ea2}"
       <<"@media(prefers-color-scheme:dark){:root{--bg:#12161d;--surface:#1a202a;--surface-alt:#1f2632;--th:#222a38;--text:#f1f5f9;--muted:#a5b3c6;--accent:#60a5fa;--border:#475569;--divider:#334155;--ok-bg:#132e23;--ok-fg:#6ee7b7;--warn-bg:#362611;--warn-fg:#fcd34d;--badge-bg:#172540;--badge-fg:#93c5fd}}"
       <<"@media(prefers-contrast:more){:root{--border:#334155;--divider:#64748b;--text:#000000;--muted:#1e293b}}"
       <<"*{box-sizing:border-box}body{font:15px/1.5 -apple-system,BlinkMacSystemFont,'Segoe UI',system-ui,sans-serif;background:var(--bg);color:var(--text);margin:0;font-variant-numeric:tabular-nums}"
       <<"main{max-width:1360px;margin:auto;padding:36px 28px}h1{font-size:30px;font-weight:600;letter-spacing:-0.015em;margin:6px 0}h2{font-size:20px;font-weight:600;margin:36px 0 14px;padding-bottom:8px;border-bottom:1px solid var(--divider)}small{color:var(--muted)}"
       <<".cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(190px,1fr));gap:12px;margin:16px 0}"
       <<".card{background:var(--surface);border:1px solid var(--border);border-top:3px solid var(--accent);border-radius:10px;padding:16px 18px}"
       <<".value{font-size:28px;font-weight:600;color:var(--accent);line-height:1.2;margin-bottom:4px;font-variant-numeric:tabular-nums}"
       <<"table{width:100%;border-collapse:separate;border-spacing:0;background:var(--surface);border:1px solid var(--border);border-radius:8px;overflow:hidden;font-size:13.5px;margin-bottom:16px}"
       <<"td,th{text-align:left;padding:10px 12px;border-bottom:1px solid var(--divider);vertical-align:top;overflow-wrap:anywhere}"
       <<"tr:nth-child(even) td{background:var(--surface-alt)}tr:last-child td{border-bottom:none}"
       <<"th{background:var(--th);color:var(--text);font-weight:600;white-space:nowrap;border-bottom:1px solid var(--border)}"
       <<"code,pre{font-family:'Cascadia Mono',Consolas,monospace;font-size:12.5px;white-space:pre-wrap;overflow-wrap:anywhere}"
       <<"pre{background:var(--surface);border:1px solid var(--border);border-radius:8px;padding:14px;max-height:440px;overflow:auto}"
       <<"li{margin:6px 0}.badge{display:inline-block;padding:2px 8px;background:var(--badge-bg);color:var(--badge-fg);border:1px solid var(--divider);border-radius:6px;font-weight:600;font-size:12px}"
       <<"details{margin:16px 0;background:var(--surface);border:1px solid var(--border);border-radius:8px;padding:10px 14px}summary{cursor:pointer;font-weight:600}"
       <<"input{width:100%;max-width:560px;padding:10px 14px;background:var(--surface);color:var(--text);border:1px solid var(--border);border-radius:8px;margin:8px 0 14px;font:inherit}"
       <<"input:focus{outline:2px solid var(--accent);outline-offset:1px}"
       <<".stale{background:var(--warn-bg);color:var(--warn-fg);border:1px solid var(--border);padding:14px 16px;border-radius:8px;font-weight:500}"
       <<".ok{background:var(--ok-bg);color:var(--ok-fg);border:1px solid var(--border);padding:14px 16px;border-radius:8px;font-weight:500}"
       <<"</style><main>"
       <<"<small>MIGRATOOL / AUDITORÍA DE MIGRACIÓN</small><h1>"<<html(p.config["name"])<<"</h1><p>"<<html(st.value("at",now()))<<" · "<<html(path_text(p.file))<<"</p>"
       <<"<p class="<<(st["freshness"]["fresh"]==true?"ok":"stale")<<">"<<(st["freshness"]["fresh"]==true?"Hashes de inventario vigentes":"Inventario sin comprobar, incompleto o desactualizado")<<". Paridad funcional global: sin determinar."<<(truncated_report?" Vista acotada a los primeros 1000 archivos de origen; use export_inventory para el catálogo completo.":"")<<"</p>"
       <<"<div class=cards><div class=card><div class=value>"<<percentage_text(st["migration_percent"])<<"</div>Migración declarada · "<<st["migrated_units"]<<" / "<<st["total_units"]<<" unidades</div><div class=card><div class=value>"<<percentage_text(st["units_with_passing_cases_percent"])<<"</div>Unidades con casos aprobados · "<<st["units_with_passing_cases"]<<" / "<<st["total_units"]<<"</div></div><p>"<<html(st["percentage_basis"])<<"</p><div class=cards>";
    for(const auto& [key,label]:std::vector<std::pair<std::string,std::string>>{{"source_files","Archivos de origen"},{"mapped_source_files","Con correspondencia"},{"declared_complete_files","Declarados completos"},{"passing_current_cases","Casos vigentes aprobados"}})
        out<<"<div class=card><div class=value>"<<st[key]<<"</div>"<<label<<"</div>";
    out<<"<div class=card><div class=value>"<<dc_obj.value("phases_completed",int64_t(0))<<" / "<<dc_obj.value("phases_total",int64_t(0))<<"</div>Fases completadas</div>"
       <<"<div class=card><div class=value>"<<dc_obj.value("tasks_completed",int64_t(0))<<" / "<<dc_obj.value("tasks_total",int64_t(0))<<"</div>Tareas completadas<br><small>"<<dc_obj.value("tasks_half_done",int64_t(0))<<" a medias · "<<dc_obj.value("tasks_in_progress",int64_t(0))<<" en curso</small></div>"
       <<"<div class=card><div class=value>"<<dc_obj.value("documentation_reviewed",int64_t(0))<<" / "<<dc_obj.value("documentation_files",int64_t(0))<<"</div>Documentos revisados<br><small>"<<dc_obj.value("documentation_stale",int64_t(0))<<" obsoletos</small></div>";
    if (st.contains("agent_os") && st["agent_os"].is_object()) {
        const auto& ao = st["agent_os"];
        const std::string ghead = ao.value("git_head", "");
        const std::string gshort = ghead.size() > 10 ? ghead.substr(0, 10) : (ghead.empty() ? "sin repo" : ghead);
        const std::string gbr = ao.value("git_branch", "");
        const std::string lscan = ao.value("last_full_tree_scan_at", "");
        out<<"<div class=card><div class=value>"<<html(gshort)<<"</div>Git ("<<html(gbr.empty()?"-":gbr)<<")<br><small>"<<(ao.value("git_dirty",false)?"Cambios sin commit":"Árbol limpio")<<"</small></div>"
           <<"<div class=card><div class=value>"<<ao.value("workstreams_open",int64_t(0))<<"</div>Workstreams abiertos<br><small>"<<ao.value("workstreams_interrupted",int64_t(0))<<" interrumpidos recuperables</small></div>"
           <<"<div class=card><div class=value>"<<ao.value("knowledge_entries",int64_t(0))<<" / "<<ao.value("docs_hub_indexed",int64_t(0))<<"</div>Conocimiento / Docs Hub<br><small>Último escaneo: "<<html(lscan.empty()?"sin registro":lscan)<<"</small></div>"
           <<"<div class=card><div class=value>"<<ao.value("BM2_tokens_saved_est",int64_t(0))<<"</div>Tokens ahorrados (BM2)<br><small>"<<ao.value("BM3_native_calls_saved",int64_t(0))<<" llamadas evitadas · "<<ao.value("BM5_files_skipped_sha256",int64_t(0))<<" dedup SHA-256</small></div>";
    }
    out<<"</div><h2>Archivos y pendientes</h2><input id=filter placeholder='Filtrar por ruta, estado, lenguaje o nota' aria-label='Filtrar archivos'><table id=files><thead><tr><th>Origen exacto</th><th>Destino exacto</th><th>% migrado declarado</th><th>Estado / etapa / autor</th><th>Falta / nota</th></tr></thead><tbody>";
    for(const auto& f:st["files"]) {
        const auto stage_s=f.contains("stage")&&f["stage"].is_string()?f["stage"].get<std::string>():std::string();
        out<<"<tr><td><code>"<<html(f.value("source_path",f.value("absolute_path","")))<<"</code><br>"<<html(f.value("language",""))<<" · "<<html(f.value("analysis",""))<<"</td><td>";
        for(const auto& t:f.value("target_paths",Json::array()))out<<"<code>"<<html(t)<<"</code><br>";
        out<<"</td><td>"<<percentage_text(f["migration_percent"])<<"<br>"<<f["migrated_units"]<<" / "<<f["total_units"]<<" · "<<html(f.value("granularity","file"))<<"</td><td>"<<html(f["state"])<<(stage_s.empty()?"":(" · <span class=badge>"+html(stage_s)+"</span>"))<<"<br>"<<html(f.value("actor",""))<<"<br>"<<html(f.value("at",""))<<"</td><td>"<<html(f["missing"].dump())<<"<p>"<<html(f.value("note",""))<<"</p></td></tr>";
    }
    out<<"</tbody></table><h2>Matriz de módulos</h2><table><tr><th>Módulo de origen</th><th>Archivos</th><th>Mapeados</th><th>A medias</th><th>Portados</th><th>Declarados</th><th>Verificados</th><th>Migrado %</th><th>Casos %</th><th>Destinos</th><th>Estado</th></tr>";
    for(const auto& m:st.value("module_matrix",Json::array())) {
        out<<"<tr><td><code>"<<html(m.value("source_module",""))<<"</code></td><td>"<<m.value("source_files",int64_t(0))<<"</td><td>"<<m.value("mapped_files",int64_t(0))<<"</td><td>"<<m.value("half_done_files",int64_t(0))<<"</td><td>"<<m.value("ported_files",int64_t(0))<<"</td><td>"<<m.value("declared_files",int64_t(0))<<"</td><td>"<<m.value("verified_files",int64_t(0))<<"</td><td>"<<percentage_text(m.value("migration_percent",Json(nullptr)))<<"</td><td>"<<percentage_text(m.value("units_with_passing_cases_percent",Json(nullptr)))<<"</td><td>"<<html(m.value("target_summary",""))<<"</td><td>"<<html(m.value("state",""))<<"</td></tr>";
    }
    auto str_id=[&](const Json& obj,const char* k1,const char* k2){
        if(obj.contains(k1)&&obj[k1].is_string())return obj[k1].get<std::string>();
        if(obj.contains(k2)){return obj[k2].is_string()?obj[k2].get<std::string>():obj[k2].dump();}
        return std::string();
    };
    out<<"</table><h2>Fases de desarrollo</h2><table><tr><th>Fase</th><th>Orden</th><th>Título</th><th>Estado</th><th>Módulos</th><th>Compuerta (builds / casos)</th><th>Autor / actualizado</th></tr>";
    for(const auto& ph:st.value("phases",Json::array())) {
        out<<"<tr><td><code>"<<html(str_id(ph,"phase_id","id"))<<"</code></td><td>"<<ph.value("ordinal",0)<<"</td><td>"<<html(ph.value("title",""))<<"<br><small>"<<html(ph.value("description",""))<<"</small></td><td>"<<html(ph.value("state",""))<<"</td><td>"<<html(ph.value("target_modules",Json::array()).dump())<<"</td><td>"<<html(ph.value("gate_builds",Json::array()).dump())<<" / "<<html(ph.value("gate_cases",Json::array()).dump())<<"</td><td>"<<html(ph.value("actor",""))<<"<br>"<<html(ph.value("updated_at",""))<<"</td></tr>";
    }
    out<<"</table><h2>Tareas de desarrollo y migración</h2><table><tr><th>Tarea</th><th>Fase</th><th>Título</th><th>Tipo</th><th>Estado</th><th>Prioridad</th><th>Casos requeridos</th><th>Nota / autor</th></tr>";
    for(const auto& tk:st.value("project_tasks",Json::array())) {
        const auto pid=tk.contains("phase_id")&&tk["phase_id"].is_string()?tk["phase_id"].get<std::string>():std::string();
        out<<"<tr><td><code>"<<html(str_id(tk,"task_id","id"))<<"</code></td><td>"<<html(pid)<<"</td><td>"<<html(tk.value("title",""))<<"</td><td>"<<html(tk.value("task_kind",tk.value("kind","")))<<"</td><td>"<<html(tk.value("state",""))<<"</td><td>"<<tk.value("priority",2)<<"</td><td>"<<html(tk.value("required_cases",Json::array()).dump())<<"</td><td>"<<html(tk.value("note",""))<<"<br>"<<html(tk.value("actor",""))<<"</td></tr>";
    }
    out<<"</table><h2>Auditoría documental</h2><table><tr><th>Doc ID</th><th>Raíz</th><th>Ruta</th><th>Estado</th><th>Disposición</th><th>Tareas extraídas</th><th>Nota / autor</th></tr>";
    for(const auto& dc:st.value("doc_audits",Json::array())) {
        out<<"<tr><td><code>"<<html(str_id(dc,"doc_id","id"))<<"</code></td><td>"<<html(dc.value("root",""))<<"</td><td><code>"<<html(dc.value("path",""))<<"</code></td><td>"<<html(dc.value("state",""))<<"</td><td>"<<html(dc.value("disposition",""))<<"</td><td>"<<html(dc.value("extracted_tasks",Json::array()).dump())<<"</td><td>"<<html(dc.value("note",""))<<"<br>"<<html(dc.value("actor",""))<<"</td></tr>";
    }
    out<<"</table><h2>Pruebas diferenciales</h2><table><tr><th>Caso</th><th>Estado</th><th>Diferencias / evidencia</th></tr>";
    for(const auto& t:st["cases"])out<<"<tr><td>"<<html(t["id"])<<"</td><td>"<<html(t["state"])<<"</td><td>"<<html(t.value("reason", ""))<<" "<<html(t.value("differences",Json::array()).dump())<<"<br><code>"<<html(t.value("receipt_path",""))<<"</code></td></tr>";
    out<<"</table><h2>Compilaciones</h2><table><tr><th>Identificador</th><th>Raíz</th><th>Estado</th></tr>";
    for(const auto& b:st["builds"])out<<"<tr><td>"<<html(b["id"])<<"</td><td>"<<html(b["root"])<<"</td><td>"<<html(b["state"])<<"</td></tr>";
    out<<"</table><h2>Cobertura importada</h2><table><tr><th>Archivo</th><th>Líneas cubiertas / total</th><th>Ramas cubiertas / total</th><th>Vigencia</th></tr>";
    for(const auto& c:st["coverage"])for(const auto& f:c["files"]) {
        out<<"<tr><td>"<<html(f.value("path",f.value("absolute_path","")))<<"</td><td>";
        for(const auto* u:{"lines","branches"})out<<(!f.contains(u)||f[u].is_null()?"sin datos":f[u].is_object()?f[u]["covered"].dump()+" / "+f[u]["count"].dump():f[u].dump())<<"</td><td>";
        out<<(c["fresh"]==true?"vigente":"desactualizado")<<"</td></tr>";
    }
    out<<"</table><details><summary>Inventario de símbolos y dependencias ("<<s["symbols"].size()<<" unidades, "<<s["dependencies"].size()<<" referencias)</summary><pre>"<<html(Json{{"symbols",s["symbols"]},{"dependencies",s["dependencies"]}}.dump(2))<<"</pre></details>"
       <<"<details><summary>Diagnósticos y exclusiones</summary><pre>"<<html(Json{{"diagnostics",st["diagnostics"]},{"exclusions",st["exclusions"]}}.dump(2))<<"</pre></details><h2>Registro automático</h2><pre>";
    {Database db(p);out<<html(db.query(use_scale?"SELECT revision,at,actor,action,entity,detail FROM history ORDER BY id DESC LIMIT 1000":"SELECT revision,at,actor,action,entity,detail FROM history ORDER BY id").dump(2));}
    out<<"</pre><h2>Alcance de la evidencia</h2><ul>";
    for(const auto& limit:st["limits"])out<<"<li>"<<html(limit)<<"</li>";
    out<<"</ul><script>document.getElementById('filter').addEventListener('input',e=>{const q=e.target.value.toLocaleLowerCase();for(const row of document.querySelectorAll('#files tbody tr'))row.hidden=!row.textContent.toLocaleLowerCase().includes(q)})</script></main></html>";
    write_text(output,out.str());write_json(output.parent_path()/(output.stem().wstring()+L".json"),st);
    event(p,"report_exported",{{"html",path_text(fs::absolute(output))},{"checked",check_fresh}});
}
}
