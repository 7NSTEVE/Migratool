#include "migratool/scale.hpp"
#include <fstream>
#include <iostream>
#include <set>

namespace {
using namespace mt;
void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}

Json job_status(const Project& project,const std::string& id){
    return scale_jobs(project,{{"id",id}}).at("items").at(0);
}

Json wait_state(const Project& project,const std::string& id){
    const auto started=GetTickCount64();
    while(GetTickCount64()-started<30000){
        auto job=job_status(project,id);
        if(job["state"]!="queued"&&job["state"]!="running")return job;
        Sleep(30);
    }
    throw std::runtime_error("EXPORT_TEST_TIMEOUT");
}

void control(const Project& project,const std::string& id,const std::string& action){
    scale_control(project,{{"id",id},{"action",action},{"actor","export-test"}});
}
}

int wmain(int argc,wchar_t** argv)try{
    using namespace mt;
    if(argc!=2)throw std::runtime_error("Requires persistent workspace");
    const auto qa=fs::path(argv[1])/L"build"/L"qa-scale"/L"acceptance"/wide("export-"+unique_id());
    const auto source=qa/L"source",target=qa/L"target";
    fs::create_directories(source);fs::create_directories(target);
    auto project=Project::create(qa/L"migration.json","Durable export",Json::array({
        {{"id","source"},{"role","source"},{"path",path_text(source)}},
        {{"id","target"},{"role","target"},{"path",path_text(target)}}}));
    Database db(project);
    // SQLite fixture isolates export recovery from filesystem discovery.
    db.begin();
    db.exec("INSERT INTO roots(id,role,path,complete,data) VALUES('source','source',?,1,'{}')",{path_text(source)});
    db.exec(R"sql(WITH RECURSIVE numbers(n) AS(SELECT 1 UNION ALL SELECT n+1 FROM numbers WHERE n<2507)
        INSERT INTO files(root,path,absolute_path,category,language,relevant,bytes,mtime,analysis,data,first_revision,last_revision)
        SELECT 'source','item'||n||'.dat',?||'/item'||n||'.dat','unknown','unknown',0,n,0,'NOT_APPLICABLE','{}',0,0 FROM numbers)sql",{path_text(source)});
    db.commit();

    // Hold the real coordinator mutex so the child cannot race controlled interruptions.
    const auto catalogue=fs::weakly_canonical(project.state_dir());
    auto identity=path_text(catalogue);
    for(auto& c:identity)c=static_cast<char>(tolower(static_cast<unsigned char>(c)));
    const auto mutex_name=L"Local\\MigratoolCoordinator_"+wide(sha256(identity).substr(0,32));
    const auto guard=CreateMutexW(nullptr,TRUE,mutex_name.c_str());
    require(guard&&GetLastError()!=ERROR_ALREADY_EXISTS,"could not hold coordinator in fixture");
    struct Unlock{HANDLE handle;~Unlock(){ReleaseMutex(handle);CloseHandle(handle);}}unlock{guard};

    const auto file=qa/L"catalogue.jsonl";
    const auto queued=scale_export(project,{{"out",path_text(file)},{"wait",false},{"actor","export-test"}});
    const auto id=queued["job_id"].get<std::string>();
    require(job_status(project,id)["state"]=="queued"&&!scale_enabled(project,db),"export activated the durable scan engine");
    control(project,id,"pause");
    require(job_status(project,id)["state"]=="paused","queued export pause was not durable");
    control(project,id,"resume");
    const auto raw_job=[&]{return db.query("SELECT * FROM inventory_jobs WHERE id=?",{id}).at(0);};
    struct Interrupted{};
    try{
        run_inventory_export(db,raw_job(),[&]{
            if(db.integer("SELECT files FROM inventory_jobs WHERE id=?",{id})>=1000)throw Interrupted{};
        });
        throw std::runtime_error("export never reached a committed batch");
    }catch(const Interrupted&){}
    const auto partial=job_status(project,id);
    require(partial["files"]==1000&&!fs::exists(file),"partial export published a final file");
    const auto stage=fs::path(wide(partial.at("scope").at("stage")));
    {
        std::ofstream uncommitted(native_path(stage),std::ios::binary|std::ios::app);
        uncommitted<<"uncommitted bytes from an interrupted batch\n";
        require(static_cast<bool>(uncommitted),"could not simulate uncommitted file tail");
    }
    index_record_event(project,"note",{{"text","An unrelated annotation"}},"parallel-agent");
    run_inventory_export(db,raw_job(),[]{});
    const auto completed=job_status(project,id);
    require(completed["state"]=="completed"&&completed["result"]["sha256"]==hash_file(file),"resumed export hash does not match published bytes");
    require(completed["result"]["files"]==2507&&!fs::exists(stage),"resumed export count or final rename failed");
    std::ifstream input(file,std::ios::binary);
    std::set<std::string> ids;
    std::string line;
    while(std::getline(input,line)){
        const auto row=Json::parse(line);
        require(ids.insert(row.at("id")).second,"export duplicated a row after interruption");
        const auto name="item"+row.at("id").get<std::string>().substr(1)+".dat";
        require(row["path"]==name&&row["absolute_path"]==path_text(source/wide(name)),"export changed a file identity or exact path");
    }
    require(input.eof()&&ids.size()==2507,"export lost rows after interruption");

    const auto changed_file=qa/L"changed.json";
    const auto changed_id=scale_export(project,{{"out",path_text(changed_file)},{"format","json"},{"wait",false}})["job_id"].get<std::string>();
    const auto changed_job=[&]{return db.query("SELECT * FROM inventory_jobs WHERE id=?",{changed_id}).at(0);};
    try{
        run_inventory_export(db,changed_job(),[&]{
            if(db.integer("SELECT files FROM inventory_jobs WHERE id=?",{changed_id})>=1000)throw Interrupted{};
        });
        throw std::runtime_error("JSON export never reached a committed batch");
    }catch(const Interrupted&){}
    db.exec("UPDATE files SET bytes=9999 WHERE id=1");
    bool conflicted=false;
    try{run_inventory_export(db,changed_job(),[]{});}catch(const std::exception& error){
        conflicted=std::string(error.what()).starts_with("EXPORT_CATALOG_CHANGED");
        db.exec("UPDATE inventory_jobs SET state='failed',error=? WHERE id=?",{error.what(),changed_id});
    }
    require(conflicted&&!fs::exists(changed_file),"changed catalogue published a mixed result");
    control(project,changed_id,"retry");
    run_inventory_export(db,changed_job(),[]{});
    const auto retried=job_status(project,changed_id);
    const auto rows=read_json(changed_file).at("files");
    require(rows.size()==2507&&rows.at(0)["bytes"]==9999&&retried["result"]["sha256"]==hash_file(changed_file),"JSON retry failed to publish one current catalogue");

    const auto cancelled_file=qa/L"cancelled.jsonl";
    const auto cancelled_id=scale_export(project,{{"out",path_text(cancelled_file)},{"wait",false}})["job_id"].get<std::string>();
    control(project,cancelled_id,"cancel");
    require(wait_state(project,cancelled_id)["state"]=="cancelled"&&!fs::exists(cancelled_file),"queued cancellation produced an output");
    bool protected_destination=false;
    try{scale_export(project,{{"out",path_text(file)},{"wait",false}});}catch(const std::exception& error){protected_destination=std::string(error.what())=="EXPORT_DESTINATION_EXISTS";}
    require(protected_destination&&hash_file(file)==completed["result"]["sha256"].get<std::string>(),"existing destination was not protected");
    write_json(qa/L"receipt.json",{{"passed",true},{"confirmed_before_interruption",partial["files"]},{"reconciled_uncommitted_tail",true},
        {"conflict_prevented_publication",conflicted},{"completed",completed},{"retried",retried},{"build",build_identity()}});
    std::cout<<"PASS export checkpoint and restart "<<path_text(qa/L"receipt.json")<<'\n';return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}
