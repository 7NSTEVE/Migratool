#include "migratool/scale.hpp"
#include "migratool/sql.hpp"
#include <iostream>
#include <fstream>
#include <vector>

int wmain(int argc,wchar_t** argv)try{
    const auto execution_start=GetTickCount64();
    const auto execution_started=mt::now();
    using namespace mt;if(argc!=6)throw std::runtime_error("census_crosscheck ROOT CATALOG ROOT_ID GENERATION RECEIPT");const fs::path root=argv[1],catalog=argv[2],receipt=argv[5];const auto root_id=utf8(argv[3]);const auto generation=std::stoll(argv[4]);sqlite3* database{};
    if(sqlite3_open_v2(path_text(catalog).c_str(),&database,SQLITE_OPEN_READONLY,nullptr)!=SQLITE_OK)throw std::runtime_error("READ_ONLY_CATALOG_OPEN");struct Close{sqlite3* value;~Close(){sqlite3_close(value);}}close{database};sqlite3_busy_timeout(database,2000);
    const auto root_record=Statement(database,"SELECT path FROM roots WHERE id=?").first(Json::array({root_id}));
    std::error_code root_error;
    if(root_record.is_null()||!fs::equivalent(root,fs::path(wide(root_record["path"])),root_error))throw std::runtime_error("ROOT_PATH_MISMATCH");
    const auto published=Statement(database,"SELECT EXISTS(SELECT 1 FROM inventory_generations WHERE id=? AND state='published') AS valid").first(Json::array({generation}));
    if(published["valid"]!=1)throw std::runtime_error("GENERATION_NOT_PUBLISHED");
    // Repeated updates to a path only require the latest covering published
    // scope. Keep this index in SQLite rather than an array of catalogue paths.
    Statement(database,"PRAGMA temp_store=FILE").exec();
    Statement(database,"PRAGMA temp.cache_size=-2048").exec();
    Statement(database,"CREATE TEMP TABLE visible_scopes(root TEXT,prefix TEXT,generation INTEGER,PRIMARY KEY(root,prefix)) WITHOUT ROWID").exec();
    Statement(database,"INSERT INTO temp.visible_scopes SELECT s.root,s.prefix,MAX(s.generation) FROM generation_scopes s JOIN inventory_generations g ON g.id=s.generation WHERE g.state='published' AND s.generation<=? AND s.root=? GROUP BY s.root,s.prefix").exec({generation,root_id});
    const auto scope_count=Statement(database,"SELECT COUNT(*) AS count FROM temp.visible_scopes").first()["count"];
    const std::string visible=" AND NOT EXISTS(SELECT 1 FROM temp.visible_scopes s WHERE s.generation>c.generation AND s.root=c.root AND(s.prefix='' OR c.path=rtrim(s.prefix,'/') COLLATE NOCASE OR(c.path>=s.prefix COLLATE NOCASE AND c.path<s.prefix||char(1114111) COLLATE NOCASE)))";
    Statement covering(database,"SELECT COALESCE(MAX(generation),0) AS generation FROM temp.visible_scopes WHERE root=? AND(prefix='' OR ?=rtrim(prefix,'/') COLLATE NOCASE OR(?>=prefix COLLATE NOCASE AND ?<prefix||char(1114111) COLLATE NOCASE))");
    Statement match(database,"SELECT bytes,mtime,attrs FROM census_entries WHERE generation=? AND root=? AND path=? AND kind=?");
    const auto expected_row=Statement(database,"SELECT COUNT(*) AS total FROM census_entries c JOIN inventory_generations g ON g.id=c.generation AND g.state='published' WHERE c.generation<=? AND c.root=?"+visible).first({generation,root_id});
    const auto expected=expected_row["total"].get<int64_t>();
    const auto prepare_ms=GetTickCount64()-execution_start;
    const fs::path issues_path=receipt.wstring()+L".issues.jsonl";
    std::ofstream all_issues(native_path(issues_path),std::ios::binary|std::ios::trunc);
    if(!all_issues)throw std::runtime_error("ISSUE_LOG_OPEN");
    struct Frame{HANDLE handle=INVALID_HANDLE_VALUE;fs::path relative;WIN32_FIND_DATAW data{};bool available=false;};std::vector<Frame> stack;
    Json issues=Json::array(),size_samples=Json::array();
    int64_t files=0,dirs=0,links=0,matched=0,unindexed=0,io_errors=0,size_changes=0,mtime_changes=0,attribute_changes=0;
    const auto start=GetTickCount64();
    auto write_issue=[&](const Json& record){all_issues<<record.dump(-1,' ',false,Json::error_handler_t::replace)<<'\n';if(!all_issues)throw std::runtime_error("ISSUE_LOG_WRITE");};
    auto issue=[&](const std::string& action,const fs::path& path,DWORD code){
        if(action=="not_in_census")++unindexed;else ++io_errors;
        Json record={{"operation",action},{"path",path_text(root/path)},{"win32",code}};
        if(issues.size()<100)issues.push_back(record);
        write_issue(record);
    };
    auto open=[&](const fs::path& relative){Frame frame;frame.relative=relative;auto pattern=native_path(root/relative)/L"*";frame.handle=FindFirstFileExW(pattern.c_str(),FindExInfoBasic,&frame.data,FindExSearchNameMatch,nullptr,FIND_FIRST_EX_LARGE_FETCH);if(frame.handle==INVALID_HANDLE_VALUE){auto code=GetLastError();if(code!=ERROR_FILE_NOT_FOUND)issue("enumerate",relative,code);return;}frame.available=true;stack.push_back(std::move(frame));};open({});
    while(!stack.empty()){auto& frame=stack.back();if(!frame.available){FindClose(frame.handle);stack.pop_back();continue;}const auto entry=frame.data;const auto relative=frame.relative/entry.cFileName;frame.available=FindNextFileW(frame.handle,&frame.data)!=FALSE;if(!frame.available&&GetLastError()!=ERROR_NO_MORE_FILES)issue("next",frame.relative,GetLastError());
        if(std::wstring_view(entry.cFileName)==L"."||std::wstring_view(entry.cFileName)==L"..")continue;const bool directory=(entry.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0,link=(entry.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)!=0;const char* kind=link?(directory?"directory_link":"file_link"):(directory?"directory":"file");if(link)++links;else if(directory)++dirs;else ++files;
        const auto entry_path=path_text(relative);
        const auto scope=covering.first({root_id,entry_path,entry_path,entry_path});
        const auto expected_file=match.first({scope["generation"],root_id,entry_path,kind});
        if(!expected_file.is_null()){
            ++matched;
            if(!directory&&!link){
                const auto bytes=(static_cast<uint64_t>(entry.nFileSizeHigh)<<32)|entry.nFileSizeLow;
                const auto expected_bytes=expected_file["bytes"].get<uint64_t>();
                const auto modified=(static_cast<uint64_t>(entry.ftLastWriteTime.dwHighDateTime)<<32)|entry.ftLastWriteTime.dwLowDateTime;
                const bool different_size=expected_bytes!=bytes;
                const bool different_mtime=expected_file["mtime"].get<uint64_t>()!=modified;
                const bool different_attributes=expected_file["attrs"].get<DWORD>()!=entry.dwFileAttributes;
                size_changes+=different_size;mtime_changes+=different_mtime;attribute_changes+=different_attributes;
                if(different_size||different_mtime||different_attributes){
                    Json record={{"operation","file_metadata_changed"},{"path",path_text(root/relative)},{"size_changed",different_size},{"mtime_changed",different_mtime},{"attributes_changed",different_attributes},{"catalog_bytes",expected_bytes},{"physical_bytes",bytes},{"catalog_mtime",expected_file["mtime"]},{"physical_mtime",modified}};
                    if(size_samples.size()<100)size_samples.push_back(record);
                    write_issue(record);
                }
            }
        }else issue("not_in_census",relative,0);
        if((files+dirs+links)%100000==0)std::cerr<<files+dirs+links<<" entradas contrastadas\n";if(directory&&!link)open(relative);
    }
    all_issues.flush();if(!all_issues)throw std::runtime_error("ISSUE_LOG_FLUSH");all_issues.close();
    const bool paths_complete=unindexed==0&&io_errors==0&&matched==expected;
    const bool metadata_matches=size_changes==0&&mtime_changes==0&&attribute_changes==0;
    Json result={{"schema","migratool.census-crosscheck/3"},{"started",execution_started},{"at",now()},{"root",path_text(root)},{"catalog",path_text(catalog)},{"generation",generation},{"method","FindFirstFileExW + exact published scope and SQLite key for every entry; regular-file sizes, mtimes and attributes; no links followed or original contents read"},{"effective_scope_prefixes",scope_count},{"files",files},{"directories",dirs},{"links",links},{"matched_exact_paths",matched},{"catalog_entries",expected},{"errors",unindexed+io_errors},{"unindexed_entries",unindexed},{"io_errors",io_errors},{"size_changes",size_changes},{"mtime_changes",mtime_changes},{"attribute_changes",attribute_changes},{"issues",issues},{"size_change_examples",size_samples},{"issues_log",path_text(issues_path)},{"issues_log_sha256",hash_file(issues_path)},{"prepare_duration_ms",prepare_ms},{"enumeration_duration_ms",GetTickCount64()-start},{"duration_ms",GetTickCount64()-execution_start},{"paths_complete",paths_complete},{"file_metadata_matches",metadata_matches},{"complete",paths_complete&&metadata_matches},{"build",build_identity()}};
    write_json(receipt,result);std::cout<<result.dump(2)<<'\n';return paths_complete&&metadata_matches?0:1;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
