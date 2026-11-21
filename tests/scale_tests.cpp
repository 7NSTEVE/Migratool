#include "migratool/scale.hpp"
#include "migratool/sql.hpp"
#include <atomic>
#include <iostream>
#include <thread>
#include <psapi.h>
#include <cmath>
#include <chrono>
#include <set>

namespace {
using namespace mt;
int checks=0;
void require(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
Json wait_job(const Project& p,const std::string& id,unsigned timeout=120000){auto start=GetTickCount64();for(;;){auto status=scale_jobs(p,{{"id",id}})["items"][0];auto state=status["state"].get<std::string>();if(state=="completed"||state=="completed_with_errors"||state=="failed"||state=="cancelled")return status;
    if(GetTickCount64()-start>timeout)throw std::runtime_error("JOB_TIMEOUT");Sleep(100);}}
void wait_quiet(const Project& p){Database db(p);const auto start=GetTickCount64();auto quiet=start;while(GetTickCount64()-start<15000){if(db.integer("SELECT EXISTS(SELECT 1 FROM inventory_jobs WHERE state IN('queued','running'))")||db.integer("SELECT COALESCE(MAX(CAST(value AS INTEGER)),0) FROM meta WHERE key='dirty'"))quiet=GetTickCount64();else if(GetTickCount64()-quiet>=500)return;Sleep(50);}throw std::runtime_error("WATCHER_DID_NOT_SETTLE");}
void verify_scoped_coordinator(const fs::path& qa){
    const auto source=qa/L"source", target=qa/L"target";
    fs::create_directories(source);
    fs::create_directories(target);
    write_text(source/L"requested.c","int requested(void) { return 1; }\n");
    write_text(target/L"unrelated.c","int unrelated(void) { return 2; }\n");
    auto project=Project::create(qa/L"migration.json","Explicit scope",Json::array({
        {{"id","source"},{"role","source"},{"path",path_text(source)}},
        {{"id","target"},{"role","target"},{"path",path_text(target)}}}));
    project.config["inventory_engine"]="durable";
    project.config["analyze_cpp"]=false;
    project.save();
    Database db(project);
    db.exec("INSERT INTO meta VALUES('dirty','1') ON CONFLICT(key) DO UPDATE SET value='1'");

    const auto scoped=scale_run(project,{{"actor","scope-test"},{"defer_analysis",true},
        {"scope",{{"root","source"},{"file","requested.c"}}}});
    require(scoped["state"]=="completed"&&scoped["files"]==1,"scoped sync did not publish its file");
    Sleep(1100);
    require(db.integer("SELECT COUNT(*) FROM watchers")==0,"explicit scope launched a project watcher");
    require(db.integer("SELECT COUNT(*) FROM inventory_jobs")==1,"explicit scope launched another inventory job");
    require(db.integer("SELECT COUNT(*) FROM files WHERE root='target'")==0,"explicit scope published an unrelated root");

    const auto full=scale_start(project,{{"actor","scope-test"},{"defer_analysis",true}});
    const auto finished=wait_job(project,full["job_id"]);
    require(finished["state"]=="completed"&&finished["files"]==2,"full sync after explicit scope did not cover both roots");
    const auto started=GetTickCount64();
    auto watched_roots=db.integer("SELECT COUNT(DISTINCT root) FROM watchers");
    while(watched_roots<2&&GetTickCount64()-started<2000){
        Sleep(50);
        watched_roots=db.integer("SELECT COUNT(DISTINCT root) FROM watchers");
    }
    require(watched_roots==2,"full sync did not activate both root watchers");
}
int benchmark_project(const fs::path& file,const fs::path& output){
    const auto project=Project::load(file);Database database(project);
    Json arguments={{"kind","files"},{"limit",200},{"include_total",false},{"fields",{"id","root","path","absolute_path"}}};
    Json samples=Json::array();std::vector<int64_t> times;std::set<std::string> identifiers;
    for(int page=0;page<30;++page){
        const auto started=std::chrono::steady_clock::now();const auto result=query_page(project,database,arguments);
        const auto duration=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-started).count();
        const auto& items=result.at("items");
        require(items.size()==200,"benchmark page did not contain 200 indexed rows");
        for(const auto& row:items){
            require(fs::path(wide(row.at("absolute_path"))).is_absolute(),"benchmark path is not absolute");
            require(identifiers.insert(row.at("id")).second,"benchmark repeated a row across cursor pages");
        }
        samples.push_back({{"page",page+1},{"duration_us",duration},{"response_bytes",result.dump().size()},
            {"first_id",items.front()["id"]},{"last_id",items.back()["id"]}});times.push_back(duration);
        require(result.at("next_cursor").is_string(),"benchmark cursor ended before 6000 rows");arguments["cursor"]=result["next_cursor"];
    }
    const auto known=database.integer("SELECT id FROM files WHERE deleted=0 AND relevant=1 AND category<>'protected_configuration' ORDER BY id LIMIT 1");
    require(known>0,"benchmark has no known source file");
    const auto context_started=std::chrono::steady_clock::now();
    const auto context=scale_context(project,database,{{"id","F"+std::to_string(known)},{"include_body",false}});
    const auto context_us=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-context_started).count();
    require(context.at("source").at("id")=="F"+std::to_string(known)&&!context.at("source").contains("body"),"known-file context is incorrect or includes unrequested contents");
    std::sort(times.begin(),times.end());
    PROCESS_MEMORY_COUNTERS_EX memory{};GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),sizeof(memory));
    Json receipt={{"schema","migratool.query-benchmark/1"},{"project",path_text(project.file)},{"at",now()},
        {"file_rows",database.integer("SELECT COUNT(*) FROM files WHERE deleted=0")},{"pages",samples},
        {"unique_rows_checked",identifiers.size()},{"page_p95_us",times[28]},{"first_known_file_context_us",context_us},
        {"page_target_met",times[28]<=500000},{"context_target_met",context_us<=1000000},
        {"method","30 consecutive indexed pages of 200 rows, followed by known-file metadata context; no original contents read"},
        {"cache_state","OS cache uncontrolled after census and physical crosscheck; one catalog connection"},
        {"peak_working_set_bytes",memory.PeakWorkingSetSize},{"build",build_identity()}};
    write_json(output,receipt);std::cout<<"Indexed page p95 "<<times[28]<<" us; context "<<context_us<<" us.\n";
    return 0;
}
}
int wmain(int argc,wchar_t** argv)try{
    using namespace mt;
    if(argc==4&&std::wstring_view(argv[1])==L"--benchmark-project")return benchmark_project(fs::path(argv[2]),fs::path(argv[3]));
    using namespace mt;if(argc<2)throw std::runtime_error("Requiere raíz persistente");size_t files=10000,dirs=1000;if(argc>2)files=std::stoull(argv[2]);if(argc>3)dirs=std::stoull(argv[3]);
    if(files==0||dirs==0)throw std::runtime_error("El fixture necesita archivos y carpetas");
    const size_t semantic_files=std::min(files,files>=1000000?size_t(2048):files>=100000?size_t(512):size_t(64));
    const auto qa=fs::path(argv[1])/L"build"/L"qa-scale"/L"acceptance"/wide(std::to_string(files)+"-"+unique_id());const auto source=qa/L"source",target=qa/L"target";fs::create_directories(source);fs::create_directories(target);
    if(files<=10000)verify_scoped_coordinator(qa/L"explicit-scope");
    const auto generation_start=GetTickCount64();std::atomic_size_t next{0};std::atomic_int failures{0};std::vector<std::jthread> generators;
    for(int worker=0;worker<8;++worker)generators.emplace_back([&]{for(;;){const auto d=next.fetch_add(1);if(d>=dirs)break;const auto folder=source/wide("d"+std::to_string(d));if(!CreateDirectoryW(folder.c_str(),nullptr)&&GetLastError()!=ERROR_ALREADY_EXISTS){++failures;continue;}
        for(size_t f=d;f<files;f+=dirs){
            const auto suffix=std::to_string(f);const bool semantic=f<semantic_files;
            const auto path=folder/wide(semantic?"module"+suffix+".py":"item"+suffix+".dat");
            const std::string body=semantic?"def normalize_"+suffix+"(value):\n    return value + 1\n\ndef transform_"+suffix+"(value):\n    return normalize_"+suffix+"(value) * 2\n":"data";
            HANDLE handle=CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
            if(handle==INVALID_HANDLE_VALUE){++failures;continue;}
            DWORD written{};if(!WriteFile(handle,body.data(),static_cast<DWORD>(body.size()),&written,nullptr)||written!=body.size())++failures;
            CloseHandle(handle);
        }
    }});
    generators.clear();require(failures==0,"fixture creation failed");const auto generation_ms=GetTickCount64()-generation_start;
    fs::create_directories(source/L"empty");fs::create_directories(source/L".git");write_text(source/L".git"/L"hook.py","def hidden():\n    return 7\n");
    write_text(source/L"main.py","def migrated(value):\n    return value + 1\n");write_text(target/L"main.py","def migrated(value):\n    return value + 1\n");
    // Inventory insertion order differs from the ID order used in build and test receipts.
    auto project=Project::create(qa/L"migration.json","Scale acceptance",Json::array({{{"id","target"},{"role","target"},{"path",path_text(target)}},{{"id","source"},{"role","source"},{"path",path_text(source)}}}));project.config["inventory_engine"]="durable";project.config["analyze_cpp"]=false;project.save();
    auto job=scale_start(project,{{"actor","scale-test"}},false);auto id=job["job_id"].get<std::string>();
    scale_control(project,{{"id",id},{"action","pause"},{"actor","scale-test"}});require(scale_jobs(project,{{"id",id}})["items"][0]["state"]=="paused","queued pause did not persist");
    const auto start=GetTickCount64();scale_control(project,{{"id",id},{"action","resume"},{"actor","scale-test"}});auto finished=wait_job(project,id,3600000);
    require(finished["state"]=="completed",finished.dump().c_str());require(finished["files"].get<size_t>()==files+3,"census file count mismatch");require(finished["dirs"].get<size_t>()==dirs+2,"census directory count mismatch");
    Database db(project);require(db.integer("SELECT COUNT(*) FROM files WHERE deleted=0")==static_cast<int64_t>(files+3),"active files mismatch");require(db.integer("SELECT COUNT(*) FROM census_entries WHERE kind='directory' AND name='empty'")==1,"empty directory missing");require(db.integer("SELECT COUNT(*) FROM census_entries WHERE path='.git/hook.py'")==1,"hidden code missing");
    if(files<=10000){
        auto second_census=scale_start(project,{{"actor","scale-reconcile"},{"defer_analysis",true}});
        const auto second_id=second_census["job_id"].get<std::string>();
        std::atomic_int annotations=0;std::string writer_error;
        std::jthread writer([&](std::stop_token stop){
            try{
                Database state(project);
                while(!stop.stop_requested()){
                    if(state.integer("SELECT EXISTS(SELECT 1 FROM inventory_jobs WHERE id=? AND state='running')",{second_id})){
                        index_record_event(project,"note",{{"text","Note during metadata reconciliation"}},"concurrent-writer");++annotations;
                    }
                    Sleep(15);
                }
            }catch(const std::exception& error){writer_error=error.what();}
        });
        const auto reconciled=wait_job(project,second_id);
        writer.request_stop();writer.join();
        require(writer_error.empty()&&annotations>0,"concurrent writer did not run during reconciliation");
        require(reconciled["state"]=="completed"&&reconciled["files"].get<size_t>()==files+3,"second census lost progress under concurrent writes");
    }
    const auto verified_symbols=db.integer("SELECT COUNT(*) FROM symbols s JOIN files f ON f.id=s.file_id WHERE s.active=1 AND f.deleted=0 AND f.path LIKE 'd%/module%.py' AND (s.name LIKE 'normalize_%' OR s.name LIKE 'transform_%')");
    const auto verified_dependencies=db.integer("SELECT COUNT(*) FROM dependencies d JOIN files f ON f.id=d.file_id WHERE f.deleted=0 AND f.path LIKE 'd%/module%.py' AND d.name LIKE 'normalize_%' AND d.kind='call'");
    require(verified_symbols==static_cast<int64_t>(2*semantic_files),"expected Python symbols missing at scale");
    if(files<=10000){
        struct ResetAuthorizer{sqlite3* value;~ResetAuthorizer(){sqlite3_set_authorizer(value,nullptr,nullptr);}} reset{db.native_handle()};
        sqlite3_set_authorizer(db.native_handle(),[](void*,int operation,const char* table,const char* column,const char*,const char*)->int{
            if(operation!=SQLITE_READ||!table||!column)return SQLITE_OK;
            if(std::string_view(table)=="symbols"&&std::string_view(column)=="data")return SQLITE_DENY;
            if(std::string_view(table)=="files")for(const auto* unused:{"data","sha256","category","language","analysis"})if(std::string_view(column)==unused)return SQLITE_DENY;
            return SQLITE_OK;
        },nullptr);
        const auto thin_files=query_page(project,db,{{"kind","files"},{"limit",2},{"include_total",false},{"fields",{"id","path"}}});
        require(thin_files["items"].size()==2&&thin_files["items"][0].size()==2,"file projection read unrequested columns or returned private cursor fields");
        const auto thin_symbols=query_page(project,db,{{"kind","symbols"},{"limit",2},{"include_total",false},{"fields",{"id","name"}}});
        require(thin_symbols["items"].size()==2&&thin_symbols["items"][0].size()==2,"symbol projection read unrequested metadata");
    }
    require(verified_dependencies==static_cast<int64_t>(semantic_files),"expected Python call dependencies missing at scale");
    for(const size_t sample:{size_t(0),semantic_files/2,semantic_files-1}){
        const auto exact=path_text(source/wide("d"+std::to_string(sample%dirs)+"/module"+std::to_string(sample)+".py"));
        require(db.integer("SELECT COUNT(*) FROM symbols s JOIN files f ON f.id=s.file_id WHERE f.absolute_path=? AND s.name=? AND s.active=1",{exact,"transform_"+std::to_string(sample)})==1,"incorrect exact path or symbol");
        require(db.integer("SELECT COUNT(*) FROM dependencies d JOIN files f ON f.id=d.file_id WHERE f.absolute_path=? AND d.name=? AND d.kind='call'",{exact,"normalize_"+std::to_string(sample)})==1,"incorrect exact call dependency");
    }
    auto page=query_page(project,db,{{"kind","files"},{"limit",200},{"include_total",false}});require(page["items"].size()==200,"page size mismatch");auto cursor=page["next_cursor"];require(cursor.is_string()&&cursor.get<std::string>().starts_with("v2:"),"keyset cursor missing");
    index_record_event(project,"note",{{"text","Concurrent annotation"}},"scale-test");auto continued=query_page(project,db,{{"kind","files"},{"limit",200},{"include_total",false},{"cursor",cursor}});require(continued["items"][0]["id"]!=page["items"][0]["id"],"note invalidated or repeated page");
    std::vector<uint64_t> times;for(int sample=0;sample<30;++sample){auto tick=GetTickCount64();auto result=query_page(project,db,{{"kind","files"},{"limit",200},{"include_total",false}});times.push_back(GetTickCount64()-tick);require(result["items"].size()==200,"query result lost records");}std::sort(times.begin(),times.end());
    auto settings=scale_settings(project,Json::object());auto changed=scale_settings(project,{{"action","set"},{"actor","scale-test"},{"expected_config_revision",settings["config_revision"]},{"settings",{{"code_duplicate_mode","files"}}}});require(changed["code_duplicate_mode"]=="files","settings API failed");project=Project::load(project.file);
    auto acquired=scale_acquire(project,{{"actor","agent-A"},{"include_body",false}});require(acquired.contains("claim"),"task acquisition missing lease");auto second=scale_acquire(project,{{"actor","agent-B"},{"include_body",false}});require(acquired["task_id"]!=second["task_id"],"agents acquired identical task");
    fs::create_directories(source/L"delta");write_text(source/L"delta"/L"new.py","def created_later():\n    return 12\n");
    auto update=scale_start(project,{{"actor","scale-test"},{"scope",{{"root","source"},{"subdirectory","delta"}}}});auto delta=wait_job(project,update["job_id"]);require(delta["state"]=="completed","scoped delta failed");require(delta["files"]==1,"scope traversed unrelated folders");require(db.integer("SELECT COUNT(*) FROM files WHERE deleted=0")==static_cast<int64_t>(files+4),"scope deleted unrelated file");
    PROCESS_MEMORY_COUNTERS_EX memory{};GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),sizeof(memory));
    require(query_page(project,db,{{"kind","files"},{"limit",1}})["total"].get<size_t>()==files+4,"scoped generation hid unrelated records");
    if(files<=10000){
        write_text(source/L"functions.py","def one(x):\n    return x+1\ndef two(x):\n    return x+2\ndef three(x):\n    return x+3\n");
        auto multiple=scale_start(project,{{"actor","scale-test"},{"scope",{{"root","source"},{"files",{"functions.py","delta/new.py"}}}}});auto scoped=wait_job(project,multiple["job_id"]);require(scoped["state"]=="completed","multiple file scope failed");require(scoped["files"]==2,"multiple file scope traversed other files");
        auto source_id=db.integer("SELECT id FROM files WHERE absolute_path=?",{path_text(source/L"functions.py")});auto target_id=db.integer("SELECT id FROM files WHERE absolute_path=?",{path_text(target/L"main.py")});
        require(db.integer("SELECT units FROM file_units WHERE file_id=?",{source_id})==3,"Python function units mismatch");auto symbol=db.integer("SELECT MIN(id) FROM symbols WHERE file_id=? AND active=1 AND kind<>'file'",{source_id});
        auto worker=path_text(executable_dir()/L"test_worker.exe");project.config["builds"]=Json::array({{{"id","source-build"},{"root","source"},{"argv",{worker,"args","built"}},{"artifacts",{worker}}},{{"id","target-build"},{"root","target"},{"argv",{worker,"args","built"}},{"artifacts",{worker}}}});
        project.config["cases"]=Json::array({{{"id","same-output"},{"comparator","bytes"},{"left",{{"root","source"},{"argv",{worker,"args","same"}}}},{"right",{{"root","target"},{"argv",{worker,"args","same"}}}}}});project.save();
        wait_quiet(project);
        auto built=run_builds(project);require(built["stable"]==true&&built["builds"][0]["state"]=="PASS","durable build fingerprint unstable");
        wait_quiet(project);
        auto cases=run_cases(project);require(cases["stable"]==true&&cases["cases"][0]["state"]=="PASS","durable cases fingerprint unstable");
        wait_quiet(project);agent_batch(project,{{"actor","scale-contract"},{"expected_revision",db.revision()},{"operations",Json::array({{{"op","map"},{"source","S"+std::to_string(symbol)},{"targets",{"F"+std::to_string(target_id)}},{"complete",true},{"cases",{"same-output"}}}})}});
        auto progress=scale_file_status(project,db,{{"id","F"+std::to_string(source_id)},{"limit",1},{"include_total",false}})["items"][0];require(progress["migrated_units"]==1,"symbol declaration missing from file progress");require(progress["units_with_passing_cases"]==1,"current symbol evidence missing from file progress");require(std::abs(progress["migration_percent"].get<double>()-100.0/3)<0.001,"partial file percentage incorrect");
        auto before=scale_snapshot(project,db);auto summary_before=scale_summary(project,db);index_record_event(project,"note",{{"text","Does not change source fingerprints"}},"scale-contract");auto after=scale_snapshot(project,db);require(before["roots"]==after["roots"],"annotation invalidated content fingerprints");require(scale_summary(project,db)["cache_key"]==summary_before["cache_key"],"annotation invalidated progress aggregate cache");
        write_text(fs::path(wide(cases["cases"][0]["left"]["stdout"])),"tampered output");auto stale=scale_file_status(project,db,{{"id","F"+std::to_string(source_id)},{"limit",1},{"include_total",false}})["items"][0];require(stale["units_with_passing_cases"]==0,"tampered differential evidence accepted");
        const auto jobs_before=db.integer("SELECT COUNT(*) FROM inventory_jobs");
        const auto origin=path_text(source/L"functions.py"),destination=path_text(target/L"main.py");
        const auto cli=path_text(executable_dir()/L"migratool-cli-0.3.exe");
        const auto receipt=run_process({cli,"mark","--project",path_text(project.file),"--source",origin,"--target",destination,"--complete","--actor","scale-contract","--note","Exact path test"},qa,"",qa/L"mark-receipt",30000);
        require(receipt["exit_code"]==0,"CLI mark failed");
        const auto marked=read_json(fs::path(wide(receipt["stdout"])))["declaration"];
        require(marked["source_path"]==origin&&marked["target_paths"]==Json::array({destination}),"CLI mark did not record both exact paths");
        require(db.integer("SELECT COUNT(*) FROM inventory_jobs")==jobs_before,"marking indexed files launched a new traversal");
        const auto mappings=scale_query(project,db,{{"kind","mappings"},{"limit",10}}).value()["items"];
        require(std::any_of(mappings.begin(),mappings.end(),[&](const Json& row){return row["source_path"]==origin&&row["target_paths"]==Json::array({destination});}),"mapping query lost source or destination paths");

        write_text(target/L"main.py","def migrated(value):\n    return value + 2\n");
        auto target_change=scale_start(project,{{"actor","scale-test"},{"scope",{{"root","target"},{"file","main.py"}}}});
        require(wait_job(project,target_change["job_id"])["state"]=="completed","target change was not indexed");
        wait_quiet(project);
        const auto reacquired=scale_acquire(project,{{"id","F"+std::to_string(source_id)},{"actor","scale-recheck"},{"include_body",false}});
        require(reacquired["claim"]["actor"]=="scale-recheck","a changed destination did not reopen its source task");

        const auto short_sync=agent_request(project,"inventory_sync",{{"actor","scale-test"},{"defer_analysis",true},{"scope",{{"root","source"},{"file","functions.py"}}},{"token_budget",256}});
        require(short_sync.dump().size()<=256&&short_sync["job_id"].is_string(),"short sync result lost the durable job ID");
        const auto deferred=agent_request(project,"job_status",{{"id",short_sync["job_id"]}})["items"][0];
        require(deferred["phase"]=="analysis_deferred"&&deferred["census_complete"]==1&&deferred["analysis_complete"]==false,"deferred analysis reported false completeness");
    }
    Json receipt={{"schema","migratool.scale-acceptance/1"},{"checks",checks},{"files",files},{"directories",dirs},{"fixture_ms",generation_ms},{"inventory_ms",GetTickCount64()-start},{"semantic_fixture_files",semantic_files},{"verified_fixture_symbols",verified_symbols},{"verified_fixture_call_dependencies",verified_dependencies},{"page_p95_ms",times[28]},{"test_peak_working_set",memory.PeakWorkingSetSize},{"build",build_identity()},{"project",path_text(project.file)},{"first_job",finished},{"scoped_job",delta}};
    write_json(qa/L"receipt.json",receipt);std::cout<<receipt.dump(2)<<'\n';return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}
