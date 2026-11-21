#include "migratool/scale.hpp"
#include <iostream>

namespace {
using namespace mt;
int checks=0;
void require(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
}
int wmain(int argc,wchar_t** argv)try{
    using namespace mt;
    if(argc!=2)throw std::runtime_error("Requires persistent workspace");
    const auto qa=fs::path(argv[1])/L"build"/L"qa-scale"/L"acceptance"/wide("crosscheck-"+unique_id());
    const auto source=qa/L"source",target=qa/L"target";
    fs::create_directories(source/L"empty");fs::create_directories(target);fs::create_directories(qa/L"retained");
    write_text(source/L"first.dat","A");write_text(source/L"second.dat","B");
    auto project=Project::create(qa/L"migration.json","Crosscheck history",Json::array({
        {{"id","source"},{"role","source"},{"path",path_text(source)}},
        {{"id","target"},{"role","target"},{"path",path_text(target)}}}));
    project.config["inventory_engine"]="durable";project.save();
    auto sync=[&](const Json& scope){
        const auto queued=scale_start(project,{{"actor","crosscheck-test"},{"defer_analysis",true},{"scope",scope}},false);
        scale_coordinator(project,true);
        const auto job=scale_jobs(project,{{"id",queued["job_id"]}})["items"][0];
        require(job["state"]=="completed","fixture census did not finish");return job["generation"].get<int64_t>();
    };
    auto generation=sync(Json::object());
    for(int change=0;change<40;++change){
        write_text(source/L"first.dat",std::to_string(change%10));
        generation=sync({{"root","source"},{"file","first.dat"}});
    }
    int sequence=0;
    auto verify=[&](bool complete){
        const auto output=qa/wide("verification-"+std::to_string(++sequence)+".json");
        const auto run=run_process({path_text(executable_dir()/L"census_crosscheck.exe"),path_text(source),
            path_text(project.state_dir()/L"catalog.sqlite"),"source",std::to_string(generation),path_text(output)},
            qa,"",qa/wide("process-"+std::to_string(sequence)),30000);
        require(run["exit_code"]==(complete?0:1),"verifier exit code disagrees with expected coverage");
        const auto result=read_json(output);
        require(result["complete"]==complete,"verifier coverage differs from physical fixture");
        require(result["prepare_duration_ms"].get<uint64_t>()<=result["duration_ms"].get<uint64_t>(),"preparation excluded from verification time");
        return result;
    };
    const auto initial=verify(true);
    require(initial["matched_exact_paths"]==3&&initial["effective_scope_prefixes"]==2,"repeated file scopes were not compacted without losing entries");
    Sleep(20);write_text(source/L"first.dat","X");
    const auto changed=verify(false);
    require(changed["paths_complete"]==true&&changed["size_changes"]==0&&changed["mtime_changes"]==1,"same-size content modification was not reported by mtime");
    write_text(source/L"new.dat","N");
    const auto added=verify(false);
    require(added["unindexed_entries"]==1&&added["paths_complete"]==false,"new physical path was omitted");
    generation=sync(Json::object());
    const auto renewed=verify(true);
    require(renewed["matched_exact_paths"]==4,"full generation did not supersede earlier file scopes");
    fs::rename(source/L"second.dat",qa/L"retained"/L"second.dat");
    const auto removed=verify(false);
    require(removed["paths_complete"]==false&&removed["matched_exact_paths"]==3&&removed["catalog_entries"]==4,"catalogue-only path was not detected");
    generation=sync({{"root","source"},{"file","second.dat"}});
    const auto final=verify(true);
    require(final["matched_exact_paths"]==3&&final["catalog_entries"]==3,"file-scope deletion hid unrelated entries");
    // The path is queued while it is a directory, then replaced by a file.
    fs::create_directories(source/L"replace");
    write_text(source/L"replace"/L"child.dat","child");
    generation=sync({{"root","source"},{"subdirectory","replace/"}});
    const auto folder=verify(true);
    require(folder["matched_exact_paths"]==5,"scoped census omitted its folder or child");
    const auto replacement=scale_start(project,{{"actor","crosscheck-test"},{"defer_analysis",true},
        {"scope",{{"root","source"},{"subdirectory","replace"}}}},false);
    fs::rename(source/L"replace",qa/L"retained"/L"replace");
    write_text(source/L"replace","replacement");
    scale_coordinator(project,true);
    generation=replacement["generation"];
    const auto replaced=verify(true);
    require(replaced["files"]==3&&replaced["directories"]==1,"directory-to-file replacement left stale descendants");
    Database db(project);
    require(db.integer("SELECT COUNT(*) FROM files WHERE deleted=0 AND path='replace/child.dat'")==0,"replaced directory left an active child");
    fs::rename(source/L"replace",qa/L"retained"/L"replacement.dat");
    fs::create_directories(source/L"replace");
    write_text(source/L"replace"/L"new.dat","new child");
    generation=sync({{"root","source"},{"file","replace"}});
    const auto expanded=verify(true);
    require(expanded["files"]==3&&expanded["directories"]==2,"file-to-directory replacement failed to discover its descendants");
    require(db.integer("SELECT COUNT(*) FROM files WHERE deleted=0 AND path='replace'")==0,"replaced file remained active");
    const auto long_folder=fs::path(std::wstring(124,L'x'))/std::wstring(124,L'y');
    const auto long_relative=long_folder/L"código sin extensión";
    const auto long_file=source/long_relative;
    require(long_file.wstring().size()>MAX_PATH,"long-path fixture is too short");
    write_text(long_file,"#!/usr/bin/env python\nprint(7)\n");
    require(read_text(long_file)=="#!/usr/bin/env python\nprint(7)\n","long-path atomic write and read disagree");
    generation=sync({{"root","source"},{"subdirectory",path_text(long_folder.begin()->wstring())}});
    const auto long_paths=verify(true);
    require(long_paths["matched_exact_paths"]==8&&long_paths["io_errors"]==0,"long-path scoped census missed entries");
    require(db.integer("SELECT COUNT(*) FROM files WHERE path=? AND deleted=0",{path_text(long_relative)})==1,"long-path exact identity not present");
    write_json(qa/L"receipt.json",{{"schema","migratool.crosscheck-acceptance/1"},{"passed",true},
        {"checks",checks},{"project",path_text(project.file)},{"initial",initial},{"modified",changed},{"final",final},
        {"folder",folder},{"replaced",replaced},{"expanded",expanded},{"long_paths",long_paths},{"build",build_identity()}});
    std::cout<<"PASS "<<checks<<" checks "<<path_text(qa/L"receipt.json")<<'\n';return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}
