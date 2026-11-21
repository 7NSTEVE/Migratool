#include "migratool/scale.hpp"
#include "migratool/sql.hpp"
#include <algorithm>
#include <cctype>
#include <iostream>
#include <thread>

namespace {
using namespace mt;
int checks=0;
void require(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
template<class Function> void rejects(Function action,const std::string& expected){
    try{action();}catch(const std::exception& error){require(std::string(error.what()).find(expected)!=std::string::npos,"unexpected rejection");return;}
    throw std::runtime_error("operation was accepted unexpectedly");
}
}
int wmain(int argc,wchar_t** argv)try{
    using namespace mt;
    if(argc!=2)throw std::runtime_error("Requires persistent workspace");
    const auto qa=fs::path(argv[1])/L"build"/L"qa-scale"/L"acceptance"/wide("reuse-"+unique_id());
    const auto source=qa/L"source",target=qa/L"target",external_path=qa/L"existing-work.sqlite";
    fs::create_directories(source);fs::create_directories(target);
    const auto input=source/L"original.py",output=target/L"result.cpp";
    write_text(input,"def source():\n    return 7\n");write_text(output,"int result(){return 7;}\n");
    auto project=Project::create(qa/L"migration.json","Reuse acceptance",Json::array({
        {{"id","source"},{"role","source"},{"path",path_text(source)}},
        {{"id","target"},{"role","target"},{"path",path_text(target)}}}));
    project.config["inventory_engine"]="durable";project.save();
    sqlite3* external{};
    if(sqlite3_open(path_text(external_path).c_str(),&external)!=SQLITE_OK)throw std::runtime_error("fixture registry open");
    try{
        const char* schema="CREATE TABLE jobs(id INTEGER PRIMARY KEY,source_sha256 TEXT,architecture TEXT,action TEXT,tool_sha256 TEXT,config_sha256 TEXT);"
            "CREATE TABLE attempts(id INTEGER PRIMARY KEY,job_id INTEGER);"
            "CREATE TABLE source_paths(source_sha256 TEXT,path TEXT);"
            "CREATE TABLE outputs(attempt_id INTEGER,path TEXT,sha256 TEXT);";
        if(sqlite3_exec(external,schema,nullptr,nullptr,nullptr)!=SQLITE_OK)throw std::runtime_error("fixture registry schema");
        auto input_hash=hash_file(input);std::transform(input_hash.begin(),input_hash.end(),input_hash.begin(),[](unsigned char c){return static_cast<char>(std::toupper(c));});
        Statement(external,"INSERT INTO jobs VALUES(1,?,'x64','recover','tool','config')").exec({input_hash});
        Statement(external,"INSERT INTO source_paths VALUES(?,?)").exec({input_hash,path_text(input)});
        for(int id=1;id<=2;++id){
            Statement(external,"INSERT INTO attempts VALUES(?,1)").exec({id});
            Statement(external,"INSERT INTO outputs VALUES(?,?,?)").exec({id,path_text(output),hash_file(output)});
        }
        sqlite3_close(external);external=nullptr;
    }catch(...){sqlite3_close(external);throw;}
    const auto registry_hash=hash_file(external_path);
    Json request={{"external_path",path_text(external_path)},{"actor","reuse-test"},{"verify",true},{"limit",1}};
    const auto first=agent_request(project,"import_existing_work",request);
    require(first["imported"]==1&&first["input_output_pairs_verified"]==1,"exact hash pair not verified");
    require(first["next_cursor"].is_string(),"first page lost continuation");
    index_record_event(project,"note",{{"text","Concurrent annotation between import pages"}},"other-agent");
    request["cursor"]=first["next_cursor"];
    const auto second=agent_request(project,"import_existing_work",request);
    require(second["imported"]==1&&second["input_output_pairs_verified"]==1,"second page lost exact result");
    require(second["next_cursor"].is_null(),"last full page falsely reports more work");
    auto ambiguous=request;ambiguous["registry_path"]=path_text(external_path);
    rejects([&]{scale_import(project,ambiguous);},"EXTERNAL_CATALOG_AMBIGUOUS");
    const auto descriptions=agent_tools();
    const auto tool=std::find_if(descriptions.begin(),descriptions.end(),[](const Json& entry){return entry["name"]=="import_existing_work";});
    require(tool!=descriptions.end()&&(*tool)["inputSchema"]["properties"]["external_path"]["type"]=="string"&&(*tool)["inputSchema"]["properties"]["ids"]["items"]["type"]=="integer","MCP does not describe automatic import and evidence IDs");
    Database db(project);
    auto windows_path=external_path;windows_path.make_preferred();
    auto alternate=request;alternate.erase("cursor");alternate["external_path"]=utf8(windows_path.wstring());
    require(scale_import(project,alternate)["imported"]==1&&db.integer("SELECT COUNT(*) FROM reuse_evidence")==2,"path separators duplicated imported provenance");
    alternate["external_path"]=path_text(external_path.parent_path()/L"."/external_path.filename());
    require(scale_import(project,alternate)["imported"]==1&&db.integer("SELECT COUNT(*) FROM reuse_evidence")==2,"relative path components duplicated imported provenance");
    const auto id=db.integer("SELECT MIN(id) FROM reuse_evidence");
    write_text(output,"int result(){return 8;}\n");
    Json revalidate={{"action","revalidate"},{"ids",Json::array({id})},{"actor","reuse-test"}};
    const auto stale=agent_request(project,"import_existing_work",revalidate);
    require(stale["revalidated"]==1&&stale["input_output_pairs_verified"]==0,"changed output was accepted");
    require(db.query("SELECT state FROM reuse_evidence WHERE id=?",{id})[0]["state"]=="OUTPUT_NOT_VERIFIED","changed output reason missing");
    write_text(output,"int result(){return 7;}\n");
    const auto restored=agent_request(project,"import_existing_work",revalidate);
    require(restored["input_output_pairs_verified"]==1,"restored bytes did not revalidate");
    require(db.integer("SELECT COUNT(*) FROM reuse_evidence")==2,"revalidation created duplicate provenance");
    require(db.integer("SELECT COUNT(*) FROM history WHERE action='existing_work_revalidated'")==2,"revalidation history missing");
    const auto revision=db.revision();Cancel cancelled;cancelled.requested=true;
    rejects([&]{scale_import(project,revalidate,&cancelled);},"CANCELLED");
    require(db.revision()==revision,"cancelled revalidation modified history");
    auto invalid=revalidate;invalid["ids"]=Json::array({-1});
    rejects([&]{scale_import(project,invalid);},"REUSE_ID_INVALID");
    require(hash_file(external_path)==registry_hash,"external registry was modified");
    require(hash_file(input,nullptr)==hash_file(input),"streaming verification hash mismatch");
    HANDLE writing=CreateFileW(output.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
    require(writing!=INVALID_HANDLE_VALUE,"fixture writer did not open");
    try{const auto busy=scale_import(project,revalidate);require(busy["input_output_pairs_verified"]==0,"writable output accepted as stable evidence");}
    catch(...){CloseHandle(writing);throw;}
    CloseHandle(writing);
    const auto large=qa/L"cancellation-input.bin";
    HANDLE large_file=CreateFileW(large.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,0,nullptr);
    if(large_file==INVALID_HANDLE_VALUE)throw std::runtime_error("cancellation fixture open");
    LARGE_INTEGER length{};length.QuadPart=128LL*1024*1024;
    const bool sized=SetFilePointerEx(large_file,length,nullptr,FILE_BEGIN)&&SetEndOfFile(large_file);CloseHandle(large_file);
    require(sized,"cancellation fixture size");
    Cancel during_hash;
    std::jthread interrupter([&]{Sleep(5);during_hash.requested=true;});
    const auto cancel_started=GetTickCount64();
    rejects([&]{hash_file(large,&during_hash);},"CANCELLED");
    interrupter.join();
    require(GetTickCount64()-cancel_started<5000,"streaming verification did not cancel within five seconds");
    HANDLE after_cancel=CreateFileW(large.c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);
    require(after_cancel!=INVALID_HANDLE_VALUE,"cancellation leaked the verification handle");
    CloseHandle(after_cancel);
    write_json(qa/L"receipt.json",{{"schema","migratool.reuse-acceptance/2"},{"checks",checks},
        {"project",path_text(project.file)},{"registry_sha256",registry_hash},{"restored_pair",restored},
        {"changed_pair",stale},{"build",build_identity()},{"passed",true}});
    std::cout<<"PASS "<<checks<<" checks "<<path_text(qa/L"receipt.json")<<'\n';return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}
